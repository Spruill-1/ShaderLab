#include "pch_engine.h"
#include "DxgiDuplicationSourceProvider.h"
#include "SourceTextureUtil.h"

namespace ShaderLab::Effects
{
    namespace
    {
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

        // Kept through a failed Open, so Reopen can try again.
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
            m_duplication = nullptr;
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

        if (!CreateOutput(dc, m_width, m_height, m_outputFormat))
        {
            m_duplication = nullptr;
            return false;
        }

        m_lastError.clear();
        return true;
    }

    void DxgiDuplicationSourceProvider::Close()
    {
        m_output.Reset();
        m_duplication = nullptr;
        m_width = 0;
        m_height = 0;
        m_outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        m_frameCount = 0;
    }

    bool DxgiDuplicationSourceProvider::CreateOutput(
        ID2D1DeviceContext5* dc,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format)
    {
        m_output = CreateSharedOutput(dc, m_d3dDevice.get(), width, height, format, L"duplication");
        if (!m_output.error.empty())
        {
            m_lastError = std::move(m_output.error);
            m_output.Reset();
            return false;
        }
        return true;
    }

    bool DxgiDuplicationSourceProvider::Reopen(ID2D1DeviceContext5* dc)
    {
        auto device = m_d3dDevice;
        return Open(dc, device.get(), m_adapterIndex, m_outputIndex, m_rawFP16);
    }

    CaptureResult DxgiDuplicationSourceProvider::CaptureNextFrame(ID2D1DeviceContext5* dc)
    {
        if (!m_duplication || !m_d3dDevice)
            return CaptureResult::Failed;
        // A failed resize leaves no output; only Reopen recreates it, so keep
        // failing rather than reporting NoFrame while the desktop is idle.
        if (!m_output.texture)
            return CaptureResult::Failed;

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        winrt::com_ptr<IDXGIResource> resource;
        HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, resource.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return CaptureResult::NoFrame;

        if (hr == DXGI_ERROR_ACCESS_LOST)
            return Reopen(dc) ? CaptureResult::NoFrame : CaptureResult::Failed;

        if (FAILED(hr) || !resource)
        {
            m_lastError = L"AcquireNextFrame failed: " + HResultMessage(hr);
            return CaptureResult::Failed;
        }

        // A pointer-only update (LastPresentTime == 0) carries no new desktop
        // image: the pointer is not composited into the surface. The first
        // frame is always taken.
        if (frameInfo.LastPresentTime.QuadPart == 0 && m_frameCount > 0)
        {
            m_duplication->ReleaseFrame();
            return CaptureResult::NoFrame;
        }

        CaptureResult result = CaptureResult::NoFrame;
        winrt::com_ptr<ID3D11Texture2D> frameTexture;
        hr = resource->QueryInterface(IID_PPV_ARGS(frameTexture.put()));
        if (SUCCEEDED(hr) && frameTexture)
        {
            D3D11_TEXTURE2D_DESC frameDesc{};
            frameTexture->GetDesc(&frameDesc);

            const DXGI_FORMAT frameFormat = frameDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? DXGI_FORMAT_R16G16B16A16_FLOAT
                : DXGI_FORMAT_B8G8R8A8_UNORM;

            bool haveOutput = m_output.texture != nullptr;
            if (frameDesc.Width != m_width || frameDesc.Height != m_height || frameFormat != m_outputFormat)
            {
                m_width = frameDesc.Width;
                m_height = frameDesc.Height;
                m_outputFormat = frameFormat;
                m_output.Reset();
                haveOutput = CreateOutput(dc, m_width, m_height, m_outputFormat);
            }

            winrt::com_ptr<ID3D11DeviceContext> context;
            m_d3dDevice->GetImmediateContext(context.put());
            if (!haveOutput)
            {
                result = CaptureResult::Failed;
            }
            else if (context)
            {
                context->CopyResource(m_output.texture.get(), frameTexture.get());
                ++m_frameCount;
                result = CaptureResult::NewFrame;
            }
        }
        else
        {
            m_lastError = L"Duplicated frame is not a texture: " + HResultMessage(hr);
            result = CaptureResult::Failed;
        }

        m_duplication->ReleaseFrame();
        return result;
    }
}
