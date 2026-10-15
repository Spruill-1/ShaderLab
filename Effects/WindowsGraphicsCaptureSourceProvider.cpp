#include "pch_engine.h"
#include "WindowsGraphicsCaptureSourceProvider.h"
#include "SourceTextureUtil.h"
#include <windows.graphics.directx.direct3d11.interop.h>

namespace ShaderLab::Effects
{
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

        m_device = inspectable.try_as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        if (!m_device)
            m_lastError = L"The WinRT Direct3D device has no IDirect3DDevice";
        return m_device != nullptr;
    }

    void WindowsGraphicsCaptureSourceProvider::RecordWinrtError(const wchar_t* stage, winrt::hresult_error const& error)
    {
        m_lastError = std::format(L"{} failed: {} ({})", stage, std::wstring_view(error.message()),
                                  HResultMessage(error.code()));
    }

    bool WindowsGraphicsCaptureSourceProvider::Open(
        ID2D1DeviceContext5* dc,
        ID3D11Device* d3dDevice,
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item,
        bool rawFP16)
    {
        // Reopen passes m_item, which Close leaves in place.
        const auto captureItem = item;
        Close();

        if (!dc || !d3dDevice || !captureItem)
        {
            m_lastError = L"Missing D2D/D3D device or capture item";
            return false;
        }

        // Kept through a failed Open, so Reopen can try again.
        m_d3dDevice.copy_from(d3dDevice);
        m_item = captureItem;
        m_requestedFP16 = rawFP16;

        try
        {
            if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported())
            {
                m_lastError = L"Windows Graphics Capture is not supported on this system";
                return false;
            }
            m_itemName = captureItem.DisplayName().c_str();
        }
        catch (winrt::hresult_error const& error)
        {
            RecordWinrtError(L"Reading the capture item", error);
            return false;
        }

        if (!CreateDevice(d3dDevice))
            return false;

        // FP16 when asked for and available; otherwise 8-bit, which the
        // shared output decodes to linear.
        if (rawFP16 && OpenWithFormat(dc, DXGI_FORMAT_R16G16B16A16_FLOAT))
            return true;
        return OpenWithFormat(dc, DXGI_FORMAT_B8G8R8A8_UNORM);
    }

    void WindowsGraphicsCaptureSourceProvider::ReleaseCapture()
    {
        try
        {
            if (m_session)
                m_session.Close();
            if (m_framePool)
                m_framePool.Close();
        }
        catch (winrt::hresult_error const&)
        {
            // Teardown: the capture objects are dropped below either way.
        }
        m_session = nullptr;
        m_framePool = nullptr;
        m_output.Reset();
    }

    bool WindowsGraphicsCaptureSourceProvider::OpenWithFormat(ID2D1DeviceContext5* dc, DXGI_FORMAT format)
    {
        ReleaseCapture();
        m_outputFormat = format;

        winrt::Windows::Graphics::SizeInt32 size{};
        try
        {
            size = m_item.Size();
        }
        catch (winrt::hresult_error const& error)
        {
            RecordWinrtError(L"Reading the capture size", error);
            return false;
        }
        if (size.Width <= 0 || size.Height <= 0)
        {
            m_lastError = L"Capture item has an invalid size";
            return false;
        }
        m_width = static_cast<uint32_t>(size.Width);
        m_height = static_cast<uint32_t>(size.Height);

        if (!CreateOutput(dc, m_width, m_height))
            return false;

        try
        {
            m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_device,
                CapturePixelFormat(),
                2,
                size);
            m_session = m_framePool.CreateCaptureSession(m_item);
            m_session.StartCapture();
        }
        catch (winrt::hresult_error const& error)
        {
            RecordWinrtError(L"Starting the capture", error);
            ReleaseCapture();
            return false;
        }

        m_lastError.clear();
        return true;
    }

    bool WindowsGraphicsCaptureSourceProvider::Reopen(ID2D1DeviceContext5* dc)
    {
        const auto item = m_item;
        auto device = m_d3dDevice;
        return Open(dc, device.get(), item, m_requestedFP16);
    }

    void WindowsGraphicsCaptureSourceProvider::Close()
    {
        ReleaseCapture();
        m_device = nullptr;
        m_width = 0;
        m_height = 0;
        m_outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        m_frameCount = 0;
    }

    bool WindowsGraphicsCaptureSourceProvider::CreateOutput(
        ID2D1DeviceContext5* dc,
        uint32_t width,
        uint32_t height)
    {
        m_output = CreateSharedOutput(dc, m_d3dDevice.get(), width, height, m_outputFormat, L"WGC");
        if (!m_output.error.empty())
        {
            m_lastError = std::move(m_output.error);
            m_output.Reset();
            return false;
        }
        return true;
    }

    bool WindowsGraphicsCaptureSourceProvider::RecreateForSize(
        ID2D1DeviceContext5* dc,
        winrt::Windows::Graphics::SizeInt32 const& size)
    {
        if (!m_d3dDevice || !m_framePool || size.Width <= 0 || size.Height <= 0)
        {
            m_lastError = L"Cannot resize the capture output";
            return false;
        }

        m_width = static_cast<uint32_t>(size.Width);
        m_height = static_cast<uint32_t>(size.Height);
        m_output.Reset();

        if (!CreateOutput(dc, m_width, m_height))
            return false;

        try
        {
            m_framePool.Recreate(m_device, CapturePixelFormat(), 2, size);
        }
        catch (winrt::hresult_error const& error)
        {
            RecordWinrtError(L"Resizing the capture", error);
            m_output.Reset();
            return false;
        }
        return true;
    }

    winrt::Windows::Graphics::DirectX::DirectXPixelFormat WindowsGraphicsCaptureSourceProvider::CapturePixelFormat() const
    {
        return m_outputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
            ? winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float
            : winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    }

    CaptureResult WindowsGraphicsCaptureSourceProvider::CaptureNextFrame(ID2D1DeviceContext5* dc)
    {
        if (!m_framePool || !m_d3dDevice)
            return CaptureResult::Failed;

        try
        {
            auto frame = m_framePool.TryGetNextFrame();
            if (!frame)
                return m_output.texture ? CaptureResult::NoFrame : CaptureResult::Failed;

            // A failed resize leaves no output; every frame retries it.
            auto size = frame.ContentSize();
            if (!m_output.texture ||
                static_cast<uint32_t>(size.Width) != m_width || static_cast<uint32_t>(size.Height) != m_height)
            {
                if (!RecreateForSize(dc, size))
                    return CaptureResult::Failed;
            }

            auto surface = frame.Surface();
            auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> frameTexture;
            HRESULT hr = access->GetInterface(IID_PPV_ARGS(frameTexture.put()));
            if (FAILED(hr) || !frameTexture)
            {
                m_lastError = L"Failed to get WGC frame texture: " + HResultMessage(hr);
                return CaptureResult::Failed;
            }

            winrt::com_ptr<ID3D11DeviceContext> context;
            m_d3dDevice->GetImmediateContext(context.put());
            if (!context)
                return CaptureResult::NoFrame;

            context->CopyResource(m_output.texture.get(), frameTexture.get());
            ++m_frameCount;
            return CaptureResult::NewFrame;
        }
        catch (winrt::hresult_error const& error)
        {
            RecordWinrtError(L"Capturing a frame", error);
            return CaptureResult::Failed;
        }
    }
}
