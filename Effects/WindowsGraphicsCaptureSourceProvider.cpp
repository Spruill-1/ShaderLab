#include "pch_engine.h"
#include "WindowsGraphicsCaptureSourceProvider.h"
#include <windows.graphics.directx.direct3d11.interop.h>

namespace ShaderLab::Effects
{
	namespace
	{
		std::wstring HResultMessage(HRESULT hr)
		{
			return std::format(L"HRESULT 0x{:08X}", static_cast<uint32_t>(hr));
		}
	}

	WindowsGraphicsCaptureSourceProvider::~WindowsGraphicsCaptureSourceProvider()
	{
		Close();
	}

	bool WindowsGraphicsCaptureSourceProvider::CreateDevice(ID3D11Device* d3dDevice)
	{
		if (!d3dDevice)
			return false;

		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		HRESULT hr = d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
		if (FAILED(hr) || !dxgiDevice)
		{
			m_lastError = L"Failed to query IDXGIDevice for WGC: " + HResultMessage(hr);
			return false;
		}

		winrt::com_ptr<::IInspectable> inspectable;
		hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
		if (FAILED(hr) || !inspectable)
		{
			m_lastError = L"CreateDirect3D11DeviceFromDXGIDevice failed: " + HResultMessage(hr);
			return false;
		}

		m_device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
		return m_device != nullptr;
	}

	bool WindowsGraphicsCaptureSourceProvider::Open(
		ID2D1DeviceContext5* dc,
		ID3D11Device* d3dDevice,
		winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item,
		bool rawFP16)
	{
		Close();

		if (!dc || !d3dDevice || !item)
		{
			m_lastError = L"Missing D2D/D3D device or capture item";
			return false;
		}

		if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported())
		{
			m_lastError = L"Windows Graphics Capture is not supported on this system";
			return false;
		}

		m_d3dDevice.copy_from(d3dDevice);
		m_item = item;
		m_itemName = item.DisplayName().c_str();
		m_rawFP16 = rawFP16;
		m_outputFormat = rawFP16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;

		if (!CreateDevice(d3dDevice))
			return false;

		auto size = item.Size();
		if (size.Width <= 0 || size.Height <= 0)
		{
			m_lastError = L"Capture item has an invalid size";
			return false;
		}

		m_width = static_cast<uint32_t>(size.Width);
		m_height = static_cast<uint32_t>(size.Height);

		if (!CreateOutputTextureAndBitmap(dc, d3dDevice, m_width, m_height, m_outputFormat))
		{
			if (!rawFP16)
			{
				Close();
				return false;
			}

			m_rawFP16 = false;
			m_outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			if (!CreateOutputTextureAndBitmap(dc, d3dDevice, m_width, m_height, m_outputFormat))
			{
				Close();
				return false;
			}
		}

		try
		{
			m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
				m_device,
				CapturePixelFormat(),
				2,
				size);
		}
		catch (...)
		{
			if (!rawFP16)
				throw;
			m_rawFP16 = false;
			m_outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			m_outputTexture = nullptr;
			m_bitmap = nullptr;
			if (!CreateOutputTextureAndBitmap(dc, d3dDevice, m_width, m_height, m_outputFormat))
			{
				Close();
				return false;
			}
			m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
				m_device,
				CapturePixelFormat(),
				2,
				size);
		}
		m_session = m_framePool.CreateCaptureSession(item);
		m_session.StartCapture();

		m_lastError.clear();
		return true;
	}

	void WindowsGraphicsCaptureSourceProvider::Close()
	{
		if (m_session)
			m_session.Close();
		if (m_framePool)
			m_framePool.Close();
		m_session = nullptr;
		m_framePool = nullptr;
		m_item = nullptr;
		m_device = nullptr;
		m_bitmap = nullptr;
		m_outputTexture = nullptr;
		m_d3dDevice = nullptr;
		m_width = 0;
		m_height = 0;
		m_rawFP16 = false;
		m_outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
		m_frameCount = 0;
	}

	bool WindowsGraphicsCaptureSourceProvider::CreateOutputTextureAndBitmap(
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
			m_lastError = L"Failed to create WGC output texture: " + HResultMessage(hr);
			return false;
		}

		winrt::com_ptr<IDXGISurface> surface;
		hr = m_outputTexture->QueryInterface(IID_PPV_ARGS(surface.put()));
		if (FAILED(hr) || !surface)
		{
			m_lastError = L"Failed to query WGC output texture surface: " + HResultMessage(hr);
			return false;
		}

		D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_NONE,
			D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));

		hr = dc->CreateBitmapFromDxgiSurface(surface.get(), &bitmapProps, m_bitmap.put());
		if (FAILED(hr) || !m_bitmap)
		{
			m_lastError = L"Failed to create D2D bitmap for WGC output: " + HResultMessage(hr);
			return false;
		}

		return true;
	}

	bool WindowsGraphicsCaptureSourceProvider::RecreateForSize(
		ID2D1DeviceContext5* dc,
		winrt::Windows::Graphics::SizeInt32 const& size)
	{
		if (!m_d3dDevice || !m_framePool || size.Width <= 0 || size.Height <= 0)
			return false;

		m_width = static_cast<uint32_t>(size.Width);
		m_height = static_cast<uint32_t>(size.Height);
		m_outputTexture = nullptr;
		m_bitmap = nullptr;

		if (!CreateOutputTextureAndBitmap(dc, m_d3dDevice.get(), m_width, m_height, m_outputFormat))
			return false;

		m_framePool.Recreate(m_device, CapturePixelFormat(), 2, size);
		return true;
	}

	winrt::Windows::Graphics::DirectX::DirectXPixelFormat WindowsGraphicsCaptureSourceProvider::CapturePixelFormat() const
	{
		return m_outputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
			? winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float
			: winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
	}

	bool WindowsGraphicsCaptureSourceProvider::CaptureNextFrame(ID2D1DeviceContext5* dc)
	{
		if (!m_framePool || !m_d3dDevice || !m_outputTexture)
			return false;

		auto frame = m_framePool.TryGetNextFrame();
		if (!frame)
			return false;

		auto size = frame.ContentSize();
		if (static_cast<uint32_t>(size.Width) != m_width || static_cast<uint32_t>(size.Height) != m_height)
		{
			if (!RecreateForSize(dc, size))
				return false;
		}

		auto surface = frame.Surface();
		auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
		winrt::com_ptr<ID3D11Texture2D> frameTexture;
		HRESULT hr = access->GetInterface(IID_PPV_ARGS(frameTexture.put()));
		if (FAILED(hr) || !frameTexture)
		{
			m_lastError = L"Failed to get WGC frame texture: " + HResultMessage(hr);
			return false;
		}

		winrt::com_ptr<ID3D11DeviceContext> context;
		m_d3dDevice->GetImmediateContext(context.put());
		if (!context)
			return false;

		context->CopyResource(m_outputTexture.get(), frameTexture.get());
		++m_frameCount;
		return true;
	}
}
