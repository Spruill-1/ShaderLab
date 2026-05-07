#include "pch_engine.h"
#include "DxgiDuplicationSourceProvider.h"

namespace ShaderLab::Effects
{
	namespace
	{
		std::wstring HResultMessage(HRESULT hr)
		{
			return std::format(L"HRESULT 0x{:08X}", static_cast<uint32_t>(hr));
		}

		winrt::com_ptr<IDXGIFactory1> GetFactoryFromDevice(ID3D11Device* d3dDevice)
		{
			if (!d3dDevice)
				return nullptr;

			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
				return nullptr;

			winrt::com_ptr<IDXGIAdapter> adapter;
			if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
				return nullptr;

			winrt::com_ptr<IDXGIFactory1> factory;
			if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.put()))))
				return nullptr;

			return factory;
		}

		winrt::com_ptr<IDXGIFactory1> CreateFactory()
		{
			winrt::com_ptr<IDXGIFactory1> factory;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
				return nullptr;
			return factory;
		}
	}

	DxgiDuplicationSourceProvider::~DxgiDuplicationSourceProvider()
	{
		Close();
	}

	std::vector<DxgiOutputInfo> DxgiDuplicationSourceProvider::EnumerateOutputs(ID3D11Device* d3dDevice)
	{
		std::vector<DxgiOutputInfo> outputs;
		auto factory = d3dDevice ? GetFactoryFromDevice(d3dDevice) : CreateFactory();
		if (!factory)
			return outputs;

		for (uint32_t adapterIndex = 0;; ++adapterIndex)
		{
			winrt::com_ptr<IDXGIAdapter1> adapter;
			if (factory->EnumAdapters1(adapterIndex, adapter.put()) == DXGI_ERROR_NOT_FOUND)
				break;
			if (!adapter)
				continue;

			for (uint32_t outputIndex = 0;; ++outputIndex)
			{
				winrt::com_ptr<IDXGIOutput> output;
				HRESULT hr = adapter->EnumOutputs(outputIndex, output.put());
				if (hr == DXGI_ERROR_NOT_FOUND)
					break;
				if (FAILED(hr) || !output)
					continue;

				DXGI_OUTPUT_DESC desc{};
				if (FAILED(output->GetDesc(&desc)))
					continue;

				DxgiOutputInfo info;
				info.adapterIndex = adapterIndex;
				info.outputIndex = outputIndex;
				info.deviceName = desc.DeviceName;
				info.desktopCoordinates = desc.DesktopCoordinates;
				info.attachedToDesktop = desc.AttachedToDesktop != FALSE;
				outputs.push_back(std::move(info));
			}
		}

		return outputs;
	}

	bool DxgiDuplicationSourceProvider::Open(
		ID2D1DeviceContext5* dc,
		ID3D11Device* d3dDevice,
		uint32_t adapterIndex,
		uint32_t outputIndex,
		bool rawFP16)
	{
		Close();

		if (!dc || !d3dDevice)
		{
			m_lastError = L"Missing D2D or D3D device";
			return false;
		}

		m_adapterIndex = adapterIndex;
		m_outputIndex = outputIndex;
		m_rawFP16 = rawFP16;
		m_d3dDevice.copy_from(d3dDevice);

		auto factory = GetFactoryFromDevice(d3dDevice);
		if (!factory)
		{
			m_lastError = L"Failed to get DXGI factory";
			return false;
		}

		winrt::com_ptr<IDXGIAdapter1> adapter;
		HRESULT hr = factory->EnumAdapters1(adapterIndex, adapter.put());
		if (FAILED(hr) || !adapter)
		{
			m_lastError = L"Failed to enumerate DXGI adapter: " + HResultMessage(hr);
			return false;
		}

		winrt::com_ptr<IDXGIOutput> output;
		hr = adapter->EnumOutputs(outputIndex, output.put());
		if (FAILED(hr) || !output)
		{
			m_lastError = L"Failed to enumerate DXGI output: " + HResultMessage(hr);
			return false;
		}

		DXGI_OUTPUT_DESC outputDesc{};
		output->GetDesc(&outputDesc);
		m_outputName = outputDesc.DeviceName;

		if (rawFP16)
		{
			winrt::com_ptr<IDXGIOutput5> output5;
			hr = output->QueryInterface(IID_PPV_ARGS(output5.put()));
			if (SUCCEEDED(hr) && output5)
			{
				DXGI_FORMAT formats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM };
				hr = output5->DuplicateOutput1(
					d3dDevice,
					0,
					static_cast<UINT>(std::size(formats)),
					formats,
					m_duplication.put());
			}
		}

		if (!m_duplication)
		{
			winrt::com_ptr<IDXGIOutput1> output1;
			hr = output->QueryInterface(IID_PPV_ARGS(output1.put()));
			if (FAILED(hr) || !output1)
			{
				m_lastError = L"DXGI output does not support DuplicateOutput: " + HResultMessage(hr);
				return false;
			}

			hr = output1->DuplicateOutput(d3dDevice, m_duplication.put());
		}
		if (FAILED(hr) || !m_duplication)
		{
			m_lastError = L"DuplicateOutput failed: " + HResultMessage(hr);
			return false;
		}

		DXGI_OUTDUPL_DESC dupDesc{};
		m_duplication->GetDesc(&dupDesc);
		m_width = dupDesc.ModeDesc.Width;
		m_height = dupDesc.ModeDesc.Height;
		m_outputFormat = dupDesc.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
			? DXGI_FORMAT_R16G16B16A16_FLOAT
			: DXGI_FORMAT_B8G8R8A8_UNORM;

		if (!CreateOutputTextureAndBitmap(dc, d3dDevice, m_width, m_height, m_outputFormat))
		{
			Close();
			return false;
		}

		m_lastError.clear();
		return true;
	}

	void DxgiDuplicationSourceProvider::Close()
	{
		m_bitmap = nullptr;
		m_outputTexture = nullptr;
		m_duplication = nullptr;
		m_d3dDevice = nullptr;
		m_width = 0;
		m_height = 0;
		m_outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
		m_frameCount = 0;
	}

	bool DxgiDuplicationSourceProvider::CreateOutputTextureAndBitmap(
		ID2D1DeviceContext5* dc,
		ID3D11Device* d3dDevice,
		uint32_t width,
		uint32_t height,
		DXGI_FORMAT format)
	{
		if (!dc || !d3dDevice || width == 0 || height == 0)
			return false;

		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = width;
		texDesc.Height = height;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = format;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		HRESULT hr = d3dDevice->CreateTexture2D(&texDesc, nullptr, m_outputTexture.put());
		if (FAILED(hr) || !m_outputTexture)
		{
			m_lastError = L"Failed to create duplication output texture: " + HResultMessage(hr);
			return false;
		}

		winrt::com_ptr<IDXGISurface> surface;
		hr = m_outputTexture->QueryInterface(IID_PPV_ARGS(surface.put()));
		if (FAILED(hr) || !surface)
		{
			m_lastError = L"Failed to query output texture surface: " + HResultMessage(hr);
			return false;
		}

		D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_NONE,
			D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));

		hr = dc->CreateBitmapFromDxgiSurface(surface.get(), &bitmapProps, m_bitmap.put());
		if (FAILED(hr) || !m_bitmap)
		{
			m_lastError = L"Failed to create D2D bitmap for duplication output: " + HResultMessage(hr);
			return false;
		}

		return true;
	}

	bool DxgiDuplicationSourceProvider::Reopen(ID2D1DeviceContext5* dc)
	{
		auto device = m_d3dDevice;
		auto adapterIndex = m_adapterIndex;
		auto outputIndex = m_outputIndex;
		auto rawFP16 = m_rawFP16;
		Close();
		return Open(dc, device.get(), adapterIndex, outputIndex, rawFP16);
	}

	bool DxgiDuplicationSourceProvider::CaptureNextFrame(ID2D1DeviceContext5* dc)
	{
		if (!m_duplication || !m_d3dDevice)
			return false;

		DXGI_OUTDUPL_FRAME_INFO frameInfo{};
		winrt::com_ptr<IDXGIResource> resource;
		HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, resource.put());
		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
			return false;

		if (hr == DXGI_ERROR_ACCESS_LOST)
		{
			Reopen(dc);
			return false;
		}

		if (FAILED(hr) || !resource)
		{
			m_lastError = L"AcquireNextFrame failed: " + HResultMessage(hr);
			return false;
		}

		bool copied = false;
		winrt::com_ptr<ID3D11Texture2D> frameTexture;
		hr = resource->QueryInterface(IID_PPV_ARGS(frameTexture.put()));
		if (SUCCEEDED(hr) && frameTexture && m_outputTexture)
		{
			D3D11_TEXTURE2D_DESC frameDesc{};
			frameTexture->GetDesc(&frameDesc);

			DXGI_FORMAT frameFormat = frameDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
				? DXGI_FORMAT_R16G16B16A16_FLOAT
				: DXGI_FORMAT_B8G8R8A8_UNORM;

			if (frameDesc.Width != m_width || frameDesc.Height != m_height || frameFormat != m_outputFormat)
			{
				m_width = frameDesc.Width;
				m_height = frameDesc.Height;
				m_outputFormat = frameFormat;
				m_outputTexture = nullptr;
				m_bitmap = nullptr;
				CreateOutputTextureAndBitmap(dc, m_d3dDevice.get(), m_width, m_height, m_outputFormat);
			}

			winrt::com_ptr<ID3D11DeviceContext> context;
			m_d3dDevice->GetImmediateContext(context.put());
			if (context && m_outputTexture)
			{
				context->CopyResource(m_outputTexture.get(), frameTexture.get());
				copied = true;
				++m_frameCount;
			}
		}

		m_duplication->ReleaseFrame();
		return copied;
	}
}
