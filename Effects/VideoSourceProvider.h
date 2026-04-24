#pragma once

#include "pch.h"

namespace ShaderLab::Effects
{
    // Decodes video frames using Media Foundation Source Reader and provides
    // them as D2D1Bitmap1 images for the effect graph.
    //
    // Usage:
    //   1. Open(filePath, dc) — initialize decoder and create persistent bitmap
    //   2. Play() / Pause() / Seek(seconds)
    //   3. AdvanceFrame(dc, deltaSeconds) — decode next frame, update bitmap
    //   4. CurrentBitmap() — get the current frame as ID2D1Image*
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

        // Advance playback by deltaSeconds (wall clock time, scaled by speed).
        // Decodes the next frame if needed and updates the persistent bitmap.
        // Returns true if a new frame was produced.
        bool AdvanceFrame(ID2D1DeviceContext5* dc, double deltaSeconds);

        // Get the current frame bitmap (may be nullptr if no frame decoded yet).
        ID2D1Image* CurrentBitmap() const { return m_bitmap.get(); }

        // Video metadata.
        double Duration() const { return m_durationSeconds; }
        double CurrentPosition() const { return m_currentPositionSeconds; }
        uint32_t FrameWidth() const { return m_width; }
        uint32_t FrameHeight() const { return m_height; }
        double FrameRate() const { return m_frameRate; }
        bool IsHDR() const { return m_isHDR; }

    private:
        bool ReadNextFrame(ID2D1DeviceContext5* dc);
        bool CreateBitmap(ID2D1DeviceContext5* dc);

        // MF objects.
        winrt::com_ptr<IMFSourceReader> m_reader;

        // D2D persistent frame bitmap.
        winrt::com_ptr<ID2D1Bitmap1> m_bitmap;

        // Video info.
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        uint32_t m_stride{ 0 };
        double m_frameRate{ 0.0 };
        double m_durationSeconds{ 0.0 };
        double m_currentPositionSeconds{ 0.0 };
        double m_frameDuration{ 0.0 };    // seconds per frame
        bool m_isHDR{ false };

        // Playback state.
        bool m_playing{ false };
        bool m_loop{ true };
        float m_speed{ 1.0f };
        double m_accumulatedTime{ 0.0 };  // time since last frame decode
        bool m_endOfStream{ false };
        bool m_mfInitialized{ false };
    };
}
