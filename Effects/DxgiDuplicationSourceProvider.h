#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "SourceTextureUtil.h"

namespace ShaderLab::Effects
{
    struct SHADERLAB_API DxgiOutputInfo
    {
        uint32_t adapterIndex{ 0 };
        uint32_t outputIndex{ 0 };
        std::wstring deviceName;
        RECT desktopCoordinates{};
        bool attachedToDesktop{ false };
    };

    // Captures one DXGI output with Desktop Duplication. CurrentBitmap() is
    // linear scRGB: the FP16 duplication as is, an 8-bit one decoded from sRGB.
    class SHADERLAB_API DxgiDuplicationSourceProvider
    {
    public:
        DxgiDuplicationSourceProvider() = default;
        DxgiDuplicationSourceProvider(const DxgiDuplicationSourceProvider&) = delete;
        DxgiDuplicationSourceProvider& operator=(const DxgiDuplicationSourceProvider&) = delete;
        ~DxgiDuplicationSourceProvider();

        static std::vector<DxgiOutputInfo> EnumerateOutputs(ID3D11Device* d3dDevice = nullptr);

        bool Open(
            ID2D1DeviceContext5* dc,
            ID3D11Device* d3dDevice,
            uint32_t adapterIndex,
            uint32_t outputIndex,
            bool rawFP16 = false);
        void Close();

        // Opens the same output again on the device of the last Open, after
        // the duplication was lost (mode change, secure desktop, exclusive
        // fullscreen). False with LastError() set while it cannot be opened.
        bool Reopen(ID2D1DeviceContext5* dc);

        bool IsOpen() const { return m_duplication != nullptr; }
        CaptureResult CaptureNextFrame(ID2D1DeviceContext5* dc);
        ID2D1Image* CurrentBitmap() const { return m_output.image.get(); }

        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }
        uint64_t FrameCount() const { return m_frameCount; }
        uint32_t AdapterIndex() const { return m_adapterIndex; }
        uint32_t OutputIndex() const { return m_outputIndex; }
        // The format asked for in Open, and the one the duplication delivers.
        bool RawFP16() const { return m_rawFP16; }
        DXGI_FORMAT OutputFormat() const { return m_outputFormat; }
        const std::wstring& OutputName() const { return m_outputName; }
        const std::wstring& LastError() const { return m_lastError; }

    private:
        bool CreateOutput(ID2D1DeviceContext5* dc, uint32_t width, uint32_t height, DXGI_FORMAT format);

        winrt::com_ptr<IDXGIOutputDuplication> m_duplication;
        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        SharedOutput m_output;

        uint32_t m_adapterIndex{ 0 };
        uint32_t m_outputIndex{ 0 };
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        bool m_rawFP16{ false };
        DXGI_FORMAT m_outputFormat{ DXGI_FORMAT_B8G8R8A8_UNORM };
        uint64_t m_frameCount{ 0 };
        std::wstring m_outputName;
        std::wstring m_lastError;
    };
}
