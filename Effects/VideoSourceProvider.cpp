#include "pch.h"
#include "VideoSourceProvider.h"

namespace ShaderLab::Effects
{
    // Global MF initialization (reference counted).
    static std::atomic<int> s_mfRefCount{ 0 };
    static void EnsureMFInitialized()
    {
        if (s_mfRefCount.fetch_add(1) == 0)
            MFStartup(MF_VERSION);
    }
    static void ReleaseMF()
    {
        if (s_mfRefCount.fetch_sub(1) == 1)
            MFShutdown();
    }

    // -----------------------------------------------------------------------
    // sRGB EOTF
    // -----------------------------------------------------------------------
    static float SRGBToLinear(float s)
    {
        if (s <= 0.04045f) return s / 12.92f;
        return std::pow((s + 0.055f) / 1.055f, 2.4f);
    }

    // BT.2020 → BT.709 color matrix.
    static void BT2020ToBT709(float r2020, float g2020, float b2020,
                               float& r709, float& g709, float& b709)
    {
        r709 =  1.6605f * r2020 - 0.5877f * g2020 - 0.0728f * b2020;
        g709 = -0.1246f * r2020 + 1.1330f * g2020 - 0.0084f * b2020;
        b709 = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    VideoSourceProvider::VideoSourceProvider() = default;

    VideoSourceProvider::~VideoSourceProvider()
    {
        Close();
    }

    void VideoSourceProvider::BuildPQLUT()
    {
        // Pre-compute inverse PQ for all 256 8-bit input values.
        // Result is linear nits (0–10000).
        constexpr float m1 = 0.1593017578125f;
        constexpr float m2 = 78.84375f;
        constexpr float c1 = 0.8359375f;
        constexpr float c2 = 18.8515625f;
        constexpr float c3 = 18.6875f;

        for (int i = 0; i < 256; ++i)
        {
            float N = i / 255.0f;
            float Np = std::pow((std::max)(N, 0.0f), 1.0f / m2);
            float num = (std::max)(Np - c1, 0.0f);
            float den = c2 - c3 * Np;
            m_pqLUT[i] = (den <= 0.0f) ? 0.0f : 10000.0f * std::pow(num / den, 1.0f / m1);
        }
    }

    uint16_t VideoSourceProvider::FloatToHalf(float f)
    {
        uint32_t fi;
        std::memcpy(&fi, &f, 4);
        uint32_t sign = (fi >> 16) & 0x8000;
        int32_t exponent = ((fi >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = fi & 0x007FFFFF;

        if (exponent <= 0)
        {
            if (exponent < -10) return static_cast<uint16_t>(sign);
            mantissa = (mantissa | 0x00800000) >> (1 - exponent);
            return static_cast<uint16_t>(sign | (mantissa >> 13));
        }
        if (exponent == 0xFF - 127 + 15)
            return static_cast<uint16_t>(sign | 0x7C00 | (mantissa ? (mantissa >> 13) : 0));
        if (exponent > 30)
            return static_cast<uint16_t>(sign | 0x7C00);
        return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
    }

    // -----------------------------------------------------------------------
    // Open / Close
    // -----------------------------------------------------------------------

    bool VideoSourceProvider::Open(const std::wstring& filePath, ID2D1DeviceContext5* dc)
    {
        Close();
        m_lastError.clear();

        if (filePath.empty())
        {
            m_lastError = L"Empty file path";
            return false;
        }

        EnsureMFInitialized();
        m_mfInitialized = true;

        HRESULT hr;

        winrt::com_ptr<IMFAttributes> attrs;
        hr = MFCreateAttributes(attrs.put(), 2);
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFCreateAttributes failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        hr = MFCreateSourceReaderFromURL(filePath.c_str(), attrs.get(), m_reader.put());
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFCreateSourceReaderFromURL failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }

        m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        // Get native media type for HDR detection.
        winrt::com_ptr<IMFMediaType> nativeType;
        hr = m_reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nativeType.put());
        if (FAILED(hr))
        {
            m_lastError = std::format(L"GetNativeMediaType failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }

        UINT32 transferFunc = 0;
        nativeType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transferFunc);
        UINT32 colorPrimaries = 0;
        nativeType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &colorPrimaries);
        m_isHDR = (transferFunc == MFVideoTransFunc_2084) || (colorPrimaries == MFVideoPrimaries_BT2020);

        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(nativeType.get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr)) { m_lastError = L"Failed to get frame size"; Close(); return false; }
        m_width = width;
        m_height = height;

        UINT32 fpsNum = 0, fpsDen = 0;
        hr = MFGetAttributeRatio(nativeType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        m_frameRate = (SUCCEEDED(hr) && fpsDen > 0) ? static_cast<double>(fpsNum) / fpsDen : 30.0;
        m_frameDuration = 1.0 / m_frameRate;

        PROPVARIANT var;
        PropVariantInit(&var);
        hr = m_reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var);
        if (SUCCEEDED(hr))
        {
            m_durationSeconds = static_cast<double>(var.uhVal.QuadPart) / 10'000'000.0;
            PropVariantClear(&var);
        }

        // Request RGB32 output.
        winrt::com_ptr<IMFMediaType> outputType;
        MFCreateMediaType(outputType.put());
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
        m_stride = m_width * 4;

        hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
        if (FAILED(hr))
        {
            outputType = nullptr;
            MFCreateMediaType(outputType.put());
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
            MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
            hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
        }
        if (FAILED(hr))
        {
            m_lastError = std::format(L"SetCurrentMediaType failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }

        // Re-read actual stride.
        winrt::com_ptr<IMFMediaType> actualType;
        hr = m_reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), actualType.put());
        if (SUCCEEDED(hr))
        {
            UINT32 actualStride = 0;
            if (SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, &actualStride)) && actualStride > 0)
                m_stride = actualStride;
        }

        // Build PQ LUT for fast HDR conversion.
        if (m_isHDR)
            BuildPQLUT();

        // Allocate double buffers.
        size_t bufSize = static_cast<size_t>(m_width) * m_height * 4;
        m_backBuffer.resize(bufSize);
        m_frontBuffer.resize(bufSize);

        // Create the D2D bitmap.
        if (!CreateBitmap(dc))
        {
            m_lastError = L"Failed to create D2D bitmap";
            Close();
            return false;
        }

        // Decode first frame synchronously so we have something to show.
        m_currentPositionSeconds = 0.0;
        m_endOfStream = false;
        if (DecodeOneFrame())
        {
            std::lock_guard lock(m_bufferMutex);
            std::swap(m_frontBuffer, m_backBuffer);
            m_frameReady = true;
        }

        // Start background decode thread.
        m_decodeThread = std::jthread([this](std::stop_token st) {
            DecodeThreadFunc();
        });

        OutputDebugStringW(std::format(L"[VideoSource] Opened: {}x{} @ {:.1f}fps, {:.1f}s, HDR={}\n",
            m_width, m_height, m_frameRate, m_durationSeconds, m_isHDR).c_str());

        return true;
    }

    void VideoSourceProvider::Close()
    {
        // Stop the decode thread first.
        if (m_decodeThread.joinable())
        {
            m_decodeThread.request_stop();
            m_decodeCV.notify_all();
            m_decodeThread.join();
        }

        m_reader = nullptr;
        m_bitmap = nullptr;
        m_width = 0;
        m_height = 0;
        m_stride = 0;
        m_frameRate = 0.0;
        m_durationSeconds = 0.0;
        m_currentPositionSeconds = 0.0;
        m_frameDuration = 0.0;
        m_playing = false;
        m_endOfStream = false;
        m_accumulatedTime = 0.0;
        m_frameReady = false;
        m_frameNeeded = false;

        if (m_mfInitialized)
        {
            ReleaseMF();
            m_mfInitialized = false;
        }
    }

    bool VideoSourceProvider::CreateBitmap(ID2D1DeviceContext5* dc)
    {
        if (!dc || m_width == 0 || m_height == 0) return false;

        D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
        bitmapProps.pixelFormat.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bitmapProps.dpiX = 96.0f;
        bitmapProps.dpiY = 96.0f;
        bitmapProps.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

        D2D1_SIZE_U size = { m_width, m_height };
        HRESULT hr = dc->CreateBitmap(size, nullptr, 0, bitmapProps, m_bitmap.put());
        return SUCCEEDED(hr);
    }

    // -----------------------------------------------------------------------
    // Playback controls
    // -----------------------------------------------------------------------

    void VideoSourceProvider::Play()
    {
        if (!m_reader) return;
        if (m_endOfStream && m_loop)
            Seek(0.0);
        m_playing = true;
        // Wake up decode thread.
        m_frameNeeded = true;
        m_decodeCV.notify_one();
    }

    void VideoSourceProvider::Pause()
    {
        m_playing = false;
    }

    void VideoSourceProvider::Seek(double seconds)
    {
        if (!m_reader) return;
        seconds = (std::clamp)(seconds, 0.0, m_durationSeconds);

        // Tell decode thread to seek.
        {
            std::lock_guard lock(m_decodeMutex);
            m_seekTarget = seconds;
            m_seekPending = true;
        }
        m_endOfStream = false;
        m_accumulatedTime = 0.0;
        m_currentPositionSeconds = seconds;
        m_decodeCV.notify_one();
    }

    // -----------------------------------------------------------------------
    // Tick (UI thread) — advance clock and request frames
    // -----------------------------------------------------------------------

    void VideoSourceProvider::Tick(double deltaSeconds)
    {
        if (!m_reader || !m_playing) return;

        m_accumulatedTime += deltaSeconds * m_speed;

        if (m_accumulatedTime >= m_frameDuration)
        {
            m_accumulatedTime -= m_frameDuration;
            // Prevent drift accumulation.
            if (m_accumulatedTime > m_frameDuration * 2)
                m_accumulatedTime = 0.0;

            // Request a new frame from the decode thread.
            m_frameNeeded = true;
            m_decodeCV.notify_one();
        }
    }

    // -----------------------------------------------------------------------
    // Upload (UI thread) — copy decoded frame to D2D bitmap
    // -----------------------------------------------------------------------

    bool VideoSourceProvider::UploadIfReady(ID2D1DeviceContext5* dc)
    {
        if (!m_frameReady || !m_bitmap || !dc) return false;

        // Swap front buffer under lock.
        std::vector<uint16_t> uploadBuf;
        {
            std::lock_guard lock(m_bufferMutex);
            uploadBuf.swap(m_frontBuffer);
            m_frameReady = false;
        }

        // CopyFromMemory is fast — just a GPU upload.
        D2D1_RECT_U destRect = { 0, 0, m_width, m_height };
        UINT32 fp16Stride = m_width * 4 * sizeof(uint16_t);
        HRESULT hr = m_bitmap->CopyFromMemory(&destRect,
            reinterpret_cast<const BYTE*>(uploadBuf.data()), fp16Stride);

        // Put the buffer back for reuse.
        {
            std::lock_guard lock(m_bufferMutex);
            m_frontBuffer.swap(uploadBuf);
        }

        // Handle end of stream with loop.
        if (m_endOfStream && m_loop && m_playing)
        {
            Seek(0.0);
            m_playing = true;
        }
        else if (m_endOfStream && !m_loop)
        {
            m_playing = false;
        }

        return SUCCEEDED(hr);
    }

    // -----------------------------------------------------------------------
    // Background decode thread
    // -----------------------------------------------------------------------

    void VideoSourceProvider::DecodeThreadFunc()
    {
        auto token = m_decodeThread.get_stop_token();

        while (!token.stop_requested())
        {
            // Wait until a frame is needed.
            {
                std::unique_lock lock(m_decodeMutex);
                m_decodeCV.wait(lock, [&] {
                    return m_frameNeeded.load() || m_seekPending.load() || token.stop_requested();
                });
            }

            if (token.stop_requested()) break;

            // Handle seek.
            if (m_seekPending)
            {
                double target;
                {
                    std::lock_guard lock(m_decodeMutex);
                    target = m_seekTarget;
                    m_seekPending = false;
                }

                PROPVARIANT var;
                PropVariantInit(&var);
                var.vt = VT_I8;
                var.hVal.QuadPart = static_cast<LONGLONG>(target * 10'000'000.0);
                m_reader->SetCurrentPosition(GUID_NULL, var);
                PropVariantClear(&var);
                m_endOfStream = false;
            }

            // Decode a frame.
            if (m_frameNeeded)
            {
                m_frameNeeded = false;

                if (DecodeOneFrame())
                {
                    // Publish to front buffer.
                    std::lock_guard lock(m_bufferMutex);
                    std::swap(m_frontBuffer, m_backBuffer);
                    m_frameReady = true;
                }
            }
        }
    }

    bool VideoSourceProvider::DecodeOneFrame()
    {
        if (!m_reader) return false;

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        winrt::com_ptr<IMFSample> sample;

        HRESULT hr = m_reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, &streamIndex, &flags, &timestamp, sample.put());

        if (FAILED(hr)) return false;

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            m_endOfStream = true;
            return false;
        }
        if (!sample) return false;

        m_currentPositionSeconds = static_cast<double>(timestamp) / 10'000'000.0;

        // Get buffer.
        winrt::com_ptr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(buffer.put());
        if (FAILED(hr)) return false;

        winrt::com_ptr<IMF2DBuffer> buffer2D;
        buffer.try_as(buffer2D);

        BYTE* data = nullptr;
        LONG pitch = 0;
        bool locked2D = false;

        if (buffer2D)
        {
            hr = buffer2D->Lock2D(&data, &pitch);
            locked2D = SUCCEEDED(hr);
        }
        if (!locked2D)
        {
            DWORD maxLen = 0, curLen = 0;
            hr = buffer->Lock(&data, &maxLen, &curLen);
            if (FAILED(hr)) return false;
            pitch = static_cast<LONG>(m_stride);
        }

        if (pitch < 0)
        {
            data = data + static_cast<LONG>(m_height - 1) * pitch;
            pitch = -pitch;
        }

        // Convert to scRGB FP16 into the back buffer.
        ConvertToScRGB(data, pitch, m_backBuffer);

        if (locked2D) buffer2D->Unlock2D();
        else buffer->Unlock();

        return true;
    }

    // -----------------------------------------------------------------------
    // Color conversion: BGRA8 → scRGB FP16 (LUT-accelerated for HDR)
    // -----------------------------------------------------------------------

    void VideoSourceProvider::ConvertToScRGB(const BYTE* bgra, LONG pitch, std::vector<uint16_t>& outBuffer)
    {
        const uint16_t halfOne = FloatToHalf(1.0f);

        if (m_isHDR)
        {
            // HDR path: LUT-based PQ inverse + BT.2020→BT.709 matrix + /80 nits.
            // LUT replaces 6 pow() calls per pixel with 3 table lookups.
            for (uint32_t y = 0; y < m_height; ++y)
            {
                const BYTE* row = bgra + y * pitch;
                uint16_t* outRow = outBuffer.data() + y * m_width * 4;

                for (uint32_t x = 0; x < m_width; ++x)
                {
                    // MF RGB32: B G R X.
                    float nitsR = m_pqLUT[row[x * 4 + 2]];
                    float nitsG = m_pqLUT[row[x * 4 + 1]];
                    float nitsB = m_pqLUT[row[x * 4 + 0]];

                    float r709, g709, b709;
                    BT2020ToBT709(nitsR, nitsG, nitsB, r709, g709, b709);

                    outRow[x * 4 + 0] = FloatToHalf(r709 / 80.0f);
                    outRow[x * 4 + 1] = FloatToHalf(g709 / 80.0f);
                    outRow[x * 4 + 2] = FloatToHalf(b709 / 80.0f);
                    outRow[x * 4 + 3] = halfOne;
                }
            }
        }
        else
        {
            // SDR path: sRGB → linear.
            // Pre-build sRGB LUT.
            static float srgbLUT[256] = {};
            static bool srgbLUTBuilt = false;
            if (!srgbLUTBuilt)
            {
                for (int i = 0; i < 256; ++i)
                    srgbLUT[i] = SRGBToLinear(i / 255.0f);
                srgbLUTBuilt = true;
            }

            for (uint32_t y = 0; y < m_height; ++y)
            {
                const BYTE* row = bgra + y * pitch;
                uint16_t* outRow = outBuffer.data() + y * m_width * 4;

                for (uint32_t x = 0; x < m_width; ++x)
                {
                    outRow[x * 4 + 0] = FloatToHalf(srgbLUT[row[x * 4 + 2]]);
                    outRow[x * 4 + 1] = FloatToHalf(srgbLUT[row[x * 4 + 1]]);
                    outRow[x * 4 + 2] = FloatToHalf(srgbLUT[row[x * 4 + 0]]);
                    outRow[x * 4 + 3] = halfOne;
                }
            }
        }
    }
}
