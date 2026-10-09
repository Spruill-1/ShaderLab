#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

#include <deque>
#include <map>

namespace ShaderLab::Effects
{
    // Decodes video frames using Media Foundation Source Reader and provides
    // them as D2D1Bitmap1 images for the effect graph.
    //
    // A background thread decodes a few frames ahead of the target time into
    // a queue. UploadIfReady() shows the newest queued frame that starts at
    // or before the target and drops older ones, so a late tick skips frames
    // instead of slowing playback. Color conversion (YCbCr→RGB→PQ→linear→
    // gamut→scRGB) runs on the GPU in a D3D11 compute shader during the upload.
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

        // Seek to a position in seconds: flush the queue and decode from the
        // preceding keyframe to the frame that covers it.
        void Seek(double seconds);

        // Show the frame whose [start, start + duration) contains `seconds`.
        // Moving forward decodes on from the current position, discarding
        // frames already in the past; it seeks only when the keyframe the
        // seek would land on is further ahead than SeekThresholdFrames().
        // Moving backward seeks. Calling it again with the same time holds
        // the frame.
        void PlayTo(double seconds);

        // Free-running playback: advance by wall-clock delta times Speed(),
        // wrapping at the end when looping, and show the frame for that time.
        void Tick(double deltaSeconds);

        // Step to the frame after the one last uploaded. At the end of the
        // stream a looping video steps back to the first frame.
        void RequestNextFrame();

        // Upload the frame for the current target to the GPU and run the
        // conversion shader. Returns true if a new frame was uploaded.
        // Call on the render thread.
        bool UploadIfReady(ID2D1DeviceContext5* dc);

        // Get the current frame bitmap (may be nullptr if no frame decoded yet).
        ID2D1Image* CurrentBitmap() const { return m_bitmap.get(); }

        // Video metadata.
        double Duration() const { return m_durationSeconds; }
        // Where playback is: the seek target until its frame is uploaded, then
        // the start of the uploaded frame.
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
        // Frames decoded into the queue and dropped unshown because a newer
        // frame was already due, and seeks performed.
        uint64_t DroppedFrames() const { return m_droppedFrames; }
        uint64_t SeekCount() const { return m_seekCount; }
        // Measured costs, 0 until measured: seconds per frame over a run of
        // back-to-back reads, seconds from SetCurrentPosition to the first
        // sample (the keyframe), and the mean distance from a seek's keyframe
        // to its target, which a seek has to decode through.
        double DecodeSecondsPerFrame() const { return m_decodeSecondsPerFrame.load(); }
        double SeekOverheadSeconds() const { return m_seekOverheadSeconds.load(); }
        double MeanSeekPrerollSeconds() const { return m_meanPrerollSeconds.load(); }
        // Frames that decode in the time a seek's overhead takes: a seek pays
        // only if its keyframe is at least this far ahead of the decode head.
        double SeekThresholdFrames() const;
        // Keyframes learned from where seeks landed.
        size_t KnownKeyframeCount();
        // Frames the decode thread keeps queued ahead of the shown one.
        static constexpr size_t DecodeAheadFrames() { return scDecodeAheadFrames; }

        // Presentation time (seconds) of the frame most recently uploaded, or
        // -1 before the first upload. Unlike CurrentPosition(), which Seek()
        // sets immediately, this says which frame the output actually holds.
        double UploadedFrameTime() const { return m_uploadedFrameTime.load(); }
        // How long that frame is shown (the sample duration, or 1/fps if the
        // file gives none); 0 before the first upload.
        double UploadedFrameDuration() const { return m_uploadedFrameDuration.load(); }
        // Presentation time of the first frame, which need not be 0 (B-frame
        // reordering delay). Earlier times show this frame.
        double FirstFrameTime() const { return m_firstFrameTime; }

        // The last target handed to Seek(), or NaN once playback has moved
        // the position since. PlayTo holds on it: the frame shown for a
        // target starts at or before it, so comparing the target with
        // CurrentPosition() alone would seek again for any time between two
        // frame starts.
        double LastSeekTarget() const { return m_lastSeekTarget.load(); }

        // Zero-copy upload of hardware-decoded frames (on by default). Off
        // forces the CPU Lock2D path, which exists for software decode; tests
        // use it to compare the two paths on the same frame.
        void SetZeroCopyEnabled(bool enabled) { m_zeroCopyAllowed = enabled; }
        bool LastUploadWasZeroCopy() const { return m_lastUploadZeroCopy; }
        // The FP16 scRGB output texture (for tests / readback).
        ID3D11Texture2D* OutputTexture() const { return m_texOutput.get(); }

        // Output format from MF Source Reader.
        enum class OutputFormat { RGB32, NV12, P010 };
        OutputFormat GetOutputFormat() const { return m_outputFormat; }

    private:
        // A decoded frame waiting in the queue: raw bytes (CPU path) or the
        // decoder's sample (zero-copy path).
        struct GpuFrame
        {
            winrt::com_ptr<IMFSample>       sample;
            winrt::com_ptr<ID3D11Texture2D> tex;
            UINT                            subresource{ 0 };
        };
        struct DecodedFrame
        {
            double time{ -1.0 };
            double duration{ 0.0 };
            uint64_t generation{ 0 };
            bool seekFrame{ false };    // first frame after a seek: shown whatever the target
            std::vector<BYTE> bytes;
            LONG pitch{ 0 };
            GpuFrame gpu;
        };
        enum class DecodeResult { Frame, EndOfStream, Failed, Superseded };
        struct ReadStats
        {
            double firstSampleTime{ -1.0 };      // the keyframe, after a seek
            double secondsToFirstSample{ 0.0 };
        };

        void DecodeThreadFunc(std::stop_token token);
        // Reads samples until one ends after the target time (earlier ones are
        // already in the past and are discarded uncopied) and starts after
        // `skipThrough`, then fills `frame`. A catch-up that takes longer than
        // scCatchUpFrameSeconds stops early at a sample starting at or after
        // `progressFrom`, so the picture keeps moving. Superseded if a seek or
        // Close arrives while it reads.
        DecodeResult DecodeOneFrame(DecodedFrame& frame, uint64_t generation, std::stop_token token,
                                    ReadStats& stats, double skipThrough = -1.0,
                                    double progressFrom = std::numeric_limits<double>::infinity());
        // Whether a seek to `target` lands far enough ahead of the decode head
        // to beat decoding forward: m_stateMutex held.
        bool ForwardSeekPaysLocked(double target) const;
        // A seek to `target` landed on `keyframe`: m_stateMutex held.
        void RecordKeyframeLocked(double keyframe, double target);
        // Moves the target and lets the decode thread fill toward it.
        void SetTarget(double seconds);
        // Removes queued frames a newer queued frame already replaces: m_stateMutex held.
        void PruneQueueLocked(std::vector<DecodedFrame>& released);
        // Keeps a released frame's byte buffer for reuse: m_stateMutex held.
        void RecycleLocked(DecodedFrame& frame);
        bool CreateGPUResources(ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice);
        bool CompileConversionShader(ID3D11Device* d3dDevice);
        // Copy the decoder surface (when given) and convert into the output
        // texture, submitted as one immediate-context call; see m_convertCtx.
        // False (with m_lastError set) if nothing was submitted.
        bool RunConversionShader(bool planar = false, ID3D11Texture2D* decoderTex = nullptr,
                                 UINT decoderSubresource = 0);
        bool EnsurePlanarTexture(ID3D11Texture2D* decoderTexture);
        static uint16_t FloatToHalf(float f);

        // Each queued zero-copy frame pins a decoder surface. The H.264
        // decoder lets a caller hold 9 before ReadSample blocks, and
        // MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT does not raise that, so the queue
        // plus the frame being converted stays well under it.
        static constexpr size_t scDecodeAheadFrames = 4;

        // MF objects.
        winrt::com_ptr<IMFSourceReader> m_reader;
        winrt::com_ptr<IMFDXGIDeviceManager> m_dxgiDeviceManager;
        UINT m_resetToken{ 0 };

        // D2D output bitmap (shared with D3D11 output texture).
        winrt::com_ptr<ID2D1Bitmap1> m_bitmap;

        // D3D11 GPU conversion resources.
        ID3D11Device* m_d3dDevice{ nullptr };           // Non-owning, from render engine.
        ID3D11DeviceContext* m_d3dContext{ nullptr };    // Non-owning, from render engine.
        // Deferred context the conversion is recorded on, then submitted with
        // one ExecuteCommandList. The immediate context is shared with D2D on
        // other threads, and multithread protection serializes single calls,
        // not sequences of them. Null if unavailable; then the immediate
        // context is used directly.
        winrt::com_ptr<ID3D11DeviceContext> m_convertCtx;
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
        std::atomic<uint64_t> m_droppedFrames{ 0 };
        std::atomic<uint64_t> m_seekCount{ 0 };
        std::atomic<double> m_decodeSecondsPerFrame{ 0.0 };
        std::atomic<double> m_seekOverheadSeconds{ 0.0 };
        std::atomic<double> m_meanPrerollSeconds{ 0.0 };

        // Playback state (render thread).
        std::atomic<bool> m_playing{ false };
        bool m_loop{ true };
        float m_speed{ 1.0f };
        double m_playTime{ 0.0 };        // Tick's wall-clock position
        bool m_mfInitialized{ false };
        std::wstring m_lastError;
        std::wstring m_filePath;

        // Background decode thread. m_stateMutex guards the members from
        // m_queue to m_decodeHead; the atomics among them are also read
        // without it.
        std::jthread m_decodeThread;
        std::mutex m_stateMutex;
        std::condition_variable m_decodeCV;
        std::deque<DecodedFrame> m_queue;
        std::vector<std::vector<BYTE>> m_spareBuffers;
        bool m_seekPending{ false };
        double m_seekTarget{ 0.0 };
        bool m_endOfStream{ false };
        bool m_stepRequested{ false };
        // False from a seek until its frame is decoded.
        bool m_seekFrameDecoded{ true };
        // A failed read is retried once by re-seeking to the decode head and
        // skipping the frames already read.
        bool m_resumePending{ false };
        bool m_readRetried{ false };
        // Keyframe -> the furthest seek target known to land on it, so no
        // other keyframe lies between the two.
        std::map<double, double> m_keyframeSpans;
        // The longest known span: keyframes are at least this far apart.
        double m_minKeyframeInterval{ 0.0 };
        // Bumped by every seek; a frame decoded for an older one is discarded.
        std::atomic<uint64_t> m_generation{ 0 };
        std::atomic<double> m_targetTime{ 0.0 };
        // Start of the newest frame read in this generation, and never behind
        // the seek target: where forward decoding continues from.
        double m_decodeHead{ 0.0 };

        std::atomic<double> m_uploadedFrameTime{ -1.0 };
        std::atomic<double> m_uploadedFrameDuration{ 0.0 };
        uint64_t m_uploadedGeneration{ 0 };   // render thread
        double m_firstFrameTime{ 0.0 };

        // Zero-copy path. A hardware-decoded sample is already a D3D11
        // texture on the render device, so the decode thread queues the
        // sample (holding it keeps its decoder surface alive) and
        // UploadIfReady copies it GPU-side into a planar texture whose
        // planes the conversion shader reads through R8/R8G8 (NV12) or
        // R16/R16G16 (P010) views, instead of a Lock2D readback and re-upload.
        winrt::com_ptr<ID3D11Texture2D>          m_texPlanar;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvPlanarY;
        winrt::com_ptr<ID3D11ShaderResourceView> m_srvPlanarUV;
        bool m_zeroCopyAllowed{ true };
        std::atomic<bool> m_zeroCopyFailed{ false };
        bool m_lastUploadZeroCopy{ false };

        std::atomic<double> m_lastSeekTarget{ std::numeric_limits<double>::quiet_NaN() };
    };
}
