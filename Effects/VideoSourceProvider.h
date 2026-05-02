#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

namespace ShaderLab::Effects
{
    // Decodes video frames using Media Foundation Source Reader and provides
    // them as D2D1Bitmap1 images for the effect graph.
    //
    // Decode happens on a background thread (raw byte copy only).
    // Color conversion (YCbCr→RGB→PQ→linear→gamut→scRGB) runs on GPU
    // via a D3D11 compute shader during UploadIfReady().
    class SHADERLAB_API VideoSourceProvider
    {
    public:
        VideoSourceProvider();
        ~VideoSourceProvider();

        // Open a video file. Returns true on success.
        // Requires D3D11 device + context for GPU conversion shader setup.
        bool Open(const std::wstring& filePath, ID2D1DeviceContext5* dc,
                  ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);

        // Close and release all resources.
        void Close();

        bool IsOpen() const { return m_reader != nullptr; }

        // Playback controls.
        void Play();
        void Pause();
        bool IsPlaying() const { return m_playing; }
        void SetLoop(bool loop) { m_loop = loop; }
        bool IsLooping() const { return m_loop; }
        void SetSpeed(float speed) { m_speed = (std::max)(speed, 0.01f); }
        float Speed() const { return m_speed; }

        // Seek to a position in seconds.
        void Seek(double seconds);

        // Advance playback clock. Call each tick with wall-clock delta.
        void Tick(double deltaSeconds);

        // Request the next frame without the accumulator (for clock-driven mode).
        void RequestNextFrame();

        // Upload the latest decoded frame to GPU and run conversion shader.
        // Returns true if a new frame was uploaded. Call on UI thread.
        bool UploadIfReady(ID2D1DeviceContext5* dc);

        // Get the current frame bitmap (may be nullptr if no frame decoded yet).
        ID2D1Image* CurrentBitmap() const { return m_bitmap.get(); }

        // Video metadata.
        double Duration() const { return m_durationSeconds; }
        double CurrentPosition() const { return m_currentPositionSeconds; }
        uint32_t FrameWidth() const { return m_width; }
        uint32_t FrameHeight() const { return m_height; }
        double FrameRate() const { return m_frameRate; }
        bool IsHDR() const { return m_isHDR; }
        const std::wstring& LastError() const { return m_lastError; }
        const std::wstring& FilePath() const { return m_filePath; }
        uint64_t UploadAttempts() const { return m_uploadAttempts; }
        uint64_t UploadSuccesses() const { return m_uploadSuccesses; }
        uint64_t DecodeCount() const { return m_decodeCount; }

        // Output format from MF Source Reader.
        enum class OutputFormat { RGB32, NV12, P010 };
        OutputFormat GetOutputFormat() const { return m_outputFormat; }

    private:

        void DecodeThreadFunc();
        bool DecodeOneFrame();
        bool CreateGPUResources(ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice);
        bool CompileConversionShader(ID3D11Device* d3dDevice);
        void RunConversionShader(ID3D11DeviceContext* ctx);
        static uint16_t FloatToHalf(float f);

        // MF objects.
        winrt::com_ptr<IMFSourceReader> m_reader;
        winrt::com_ptr<IMFDXGIDeviceManager> m_dxgiDeviceManager;
        UINT m_resetToken{ 0 };

        // D2D output bitmap (shared with D3D11 output texture).
        winrt::com_ptr<ID2D1Bitmap1> m_bitmap;

        // D3D11 GPU conversion resources.
        ID3D11Device* m_d3dDevice{ nullptr };           // Non-owning, from render engine.
        ID3D11DeviceContext* m_d3dContext{ nullptr };    // Non-owning, from render engine.
        winrt::com_ptr<ID3D11ComputeShader> m_csP010;
        winrt::com_ptr<ID3D11ComputeShader> m_csNV12;
        winrt::com_ptr<ID3D11ComputeShader> m_csRGB32;
        winrt::com_ptr<ID3D11Texture2D> m_texY;        // Y plane input
        winrt::com_ptr<ID3D11Texture2D> m_texUV;       // UV plane input
        winrt::com_ptr<ID3D11Texture2D> m_texRGB;      // RGB32 input
        winrt::com_ptr<ID3D11Texture2D> m_texOutput;    // scRGB FP16 output
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvY;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvUV;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvRGB;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_uavOutput;
        winrt::com_ptr<ID3D11Buffer> m_cbParams;       // Constant buffer for dimensions

        // Video info.
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        uint32_t m_stride{ 0 };
        double m_frameRate{ 0.0 };
        double m_durationSeconds{ 0.0 };
        std::atomic<double> m_currentPositionSeconds{ 0.0 };
        double m_frameDuration{ 0.0 };
        bool m_isHDR{ false };
        OutputFormat m_outputFormat{ OutputFormat::RGB32 };
        bool m_bottomUp{ false };
        bool m_firstFrameLogged{ false };
        std::atomic<uint64_t> m_uploadAttempts{ 0 };
        std::atomic<uint64_t> m_uploadSuccesses{ 0 };
        std::atomic<uint64_t> m_decodeCount{ 0 };

        // Playback state.
        std::atomic<bool> m_playing{ false };
        bool m_loop{ true };
        float m_speed{ 1.0f };
        double m_accumulatedTime{ 0.0 };
        std::atomic<bool> m_endOfStream{ false };
        bool m_mfInitialized{ false };
        std::wstring m_lastError;
        std::wstring m_filePath;

        // Background decode thread.
        std::jthread m_decodeThread;
        std::mutex m_decodeMutex;
        std::condition_variable m_decodeCV;
        std::atomic<bool> m_frameNeeded{ false };
        std::atomic<bool> m_seekPending{ false };
        double m_seekTarget{ 0.0 };

        // Double buffer: decode thread writes raw bytes to m_backBuffer,
        // UI thread reads from m_frontBuffer when m_frameReady is set.
        std::vector<BYTE> m_backBuffer;
        std::vector<BYTE> m_frontBuffer;
        LONG m_lastPitch{ 0 };           // Pitch from last decoded frame.
        std::mutex m_bufferMutex;
        std::atomic<bool> m_frameReady{ false };
    };
}
