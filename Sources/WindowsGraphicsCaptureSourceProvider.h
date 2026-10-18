#pragma once

#include "pch_engine.h"
#include "Effects/SourceTextureUtil.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace ShaderLab::Sources
{
    using Effects::CaptureResult;
    using Effects::SharedOutput;

    // Captures a window or monitor through Windows Graphics Capture.
    // CurrentBitmap() is linear scRGB: FP16 frames as is, 8-bit frames (the
    // fallback when FP16 capture is unavailable) decoded from sRGB. No
    // method throws; a failure returns false or CaptureResult::Failed with
    // LastError() set.
    class WindowsGraphicsCaptureSourceProvider
    {
    public:
        WindowsGraphicsCaptureSourceProvider() = default;
        WindowsGraphicsCaptureSourceProvider(const WindowsGraphicsCaptureSourceProvider&) = delete;
        WindowsGraphicsCaptureSourceProvider& operator=(const WindowsGraphicsCaptureSourceProvider&) = delete;
        ~WindowsGraphicsCaptureSourceProvider();

        bool Open(
            ID2D1DeviceContext5* dc,
            ID3D11Device* d3dDevice,
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item,
            bool rawFP16 = false);
        void Close();

        // Opens the item of the last Open again, on the same device.
        bool Reopen(ID2D1DeviceContext5* dc);

        bool IsOpen() const { return m_session != nullptr; }
        CaptureResult CaptureNextFrame(ID2D1DeviceContext5* dc);
        ID2D1Image* CurrentBitmap() const { return m_output.image.get(); }

        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }
        uint64_t FrameCount() const { return m_frameCount; }
        // The format asked for in Open, and the one capture delivers.
        bool RequestedFP16() const { return m_requestedFP16; }
        DXGI_FORMAT OutputFormat() const { return m_outputFormat; }
        const std::wstring& ItemName() const { return m_itemName; }
        const std::wstring& LastError() const { return m_lastError; }

    private:
        bool OpenWithFormat(ID2D1DeviceContext5* dc, DXGI_FORMAT format);
        void ReleaseCapture();
        bool CreateDevice(ID3D11Device* d3dDevice);
        bool CreateOutput(ID2D1DeviceContext5* dc, uint32_t width, uint32_t height);
        bool RecreateForSize(ID2D1DeviceContext5* dc, winrt::Windows::Graphics::SizeInt32 const& size);
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat CapturePixelFormat() const;
        void RecordWinrtError(const wchar_t* stage, winrt::hresult_error const& error);

        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };

        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        SharedOutput m_output;

        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        bool m_requestedFP16{ false };
        DXGI_FORMAT m_outputFormat{ DXGI_FORMAT_B8G8R8A8_UNORM };
        uint64_t m_frameCount{ 0 };
        std::wstring m_itemName;
        std::wstring m_lastError;
    };
}
