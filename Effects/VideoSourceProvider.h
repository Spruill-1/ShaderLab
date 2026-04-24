#pragma once

#include "pch.h"

namespace ShaderLab::Effects
{
    // Decodes video frames using Media Foundation Source Reader and provides
    // them as D2D1Bitmap1 images for the effect graph.
    //
    // Decode + color conversion happen on a background thread.
    // The UI thread calls UploadIfReady() to copy the latest decoded frame
    // into the D2D bitmap — this is the only call that touches D2D.
    class VideoSourceProvider
    {
    public:
        VideoSourceProvider();
        ~VideoSourceProvider();

        // Open a video file. Returns true on success.
        bool Open(const std::wstring& filePath, ID2D1DeviceContext5* dc);

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
        // Does NOT decode — the background thread handles that.
        void Tick(double deltaSeconds);

        // Upload the latest decoded frame to the D2D bitmap if one is ready.
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

    private:
        void DecodeThreadFunc();
        bool DecodeOneFrame();
        bool CreateBitmap(ID2D1DeviceContext5* dc);
        void ConvertToScRGB(const BYTE* bgra, LONG pitch, std::vector<uint16_t>& outBuffer);
        static uint16_t FloatToHalf(float f);

        // Build PQ inverse LUT for fast conversion.
        void BuildPQLUT();

        // MF objects.
        winrt::com_ptr<IMFSourceReader> m_reader;

        // D2D persistent frame bitmap (UI thread only).
        winrt::com_ptr<ID2D1Bitmap1> m_bitmap;

        // Video info.
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        uint32_t m_stride{ 0 };
        double m_frameRate{ 0.0 };
        double m_durationSeconds{ 0.0 };
        std::atomic<double> m_currentPositionSeconds{ 0.0 };
        double m_frameDuration{ 0.0 };
        bool m_isHDR{ false };

        // Playback state.
        std::atomic<bool> m_playing{ false };
        bool m_loop{ true };
        float m_speed{ 1.0f };
        double m_accumulatedTime{ 0.0 };
        std::atomic<bool> m_endOfStream{ false };
        bool m_mfInitialized{ false };
        std::wstring m_lastError;

        // Background decode thread.
        std::jthread m_decodeThread;
        std::mutex m_decodeMutex;
        std::condition_variable m_decodeCV;
        std::atomic<bool> m_frameNeeded{ false };
        std::atomic<bool> m_seekPending{ false };
        double m_seekTarget{ 0.0 };

        // Double buffer: decode thread writes to m_backBuffer,
        // UI thread reads from m_frontBuffer when m_frameReady is set.
        std::vector<uint16_t> m_backBuffer;
        std::vector<uint16_t> m_frontBuffer;
        std::mutex m_bufferMutex;
        std::atomic<bool> m_frameReady{ false };

        // PQ inverse LUT (256 entries → linear nits).
        std::array<float, 256> m_pqLUT{};
    };
}
