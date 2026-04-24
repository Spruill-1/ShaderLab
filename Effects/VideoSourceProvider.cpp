#include "pch.h"
#include "VideoSourceProvider.h"

namespace ShaderLab::Effects
{
    // Global MF initialization (reference counted — safe to call multiple times).
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

    VideoSourceProvider::VideoSourceProvider() = default;

    VideoSourceProvider::~VideoSourceProvider()
    {
        Close();
    }

    bool VideoSourceProvider::Open(const std::wstring& filePath, ID2D1DeviceContext5* dc)
    {
        Close();
        m_lastError.clear();

        if (filePath.empty())
        {
            m_lastError = L"Empty file path";
            return false;
        }

        // Initialize Media Foundation (global ref-counted).
        EnsureMFInitialized();
        m_mfInitialized = true;

        HRESULT hr;

        // Create Source Reader with video processing enabled.
        winrt::com_ptr<IMFAttributes> attrs;
        hr = MFCreateAttributes(attrs.put(), 2);
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFCreateAttributes failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        // Enable video processing (format conversion by the pipeline).
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        // Enable advanced video processing for HDR content.
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        hr = MFCreateSourceReaderFromURL(filePath.c_str(), attrs.get(), m_reader.put());
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFCreateSourceReaderFromURL failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }

        // Select only the first video stream, deselect everything else.
        m_reader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        m_reader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        // Get native media type to detect HDR and frame info.
        winrt::com_ptr<IMFMediaType> nativeType;
        hr = m_reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nativeType.put());
        if (FAILED(hr))
        {
            m_lastError = std::format(L"GetNativeMediaType failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }

        // Check for HDR (PQ transfer function or BT.2020 primaries).
        UINT32 transferFunc = 0;
        nativeType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transferFunc);
        UINT32 colorPrimaries = 0;
        nativeType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &colorPrimaries);

        m_isHDR = (transferFunc == MFVideoTransFunc_2084) ||
                  (colorPrimaries == MFVideoPrimaries_BT2020);

        // Get frame size.
        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(nativeType.get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFGetAttributeSize (frame size) failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }
        m_width = width;
        m_height = height;

        // Get frame rate.
        UINT32 fpsNum = 0, fpsDen = 0;
        hr = MFGetAttributeRatio(nativeType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        if (SUCCEEDED(hr) && fpsDen > 0)
        {
            m_frameRate = static_cast<double>(fpsNum) / fpsDen;
            m_frameDuration = 1.0 / m_frameRate;
        }
        else
        {
            m_frameRate = 30.0;
            m_frameDuration = 1.0 / 30.0;
        }

        // Get duration.
        PROPVARIANT var;
        PropVariantInit(&var);
        hr = m_reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var);
        if (SUCCEEDED(hr))
        {
            m_durationSeconds = static_cast<double>(var.uhVal.QuadPart) / 10'000'000.0;
            PropVariantClear(&var);
        }

        // Configure output format.
        // Request RGB32 from MF — we'll handle PQ→scRGB conversion ourselves for HDR.
        winrt::com_ptr<IMFMediaType> outputType;
        hr = MFCreateMediaType(outputType.put());
        if (FAILED(hr)) { m_lastError = L"MFCreateMediaType failed"; Close(); return false; }

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
        m_stride = m_width * 4;

        hr = m_reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
        if (FAILED(hr))
        {
            outputType = nullptr;
            MFCreateMediaType(outputType.put());
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
            MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);

            hr = m_reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
        }
        if (FAILED(hr))
        {
            m_lastError = std::format(L"SetCurrentMediaType failed: 0x{:08X} ({}x{}, HDR={})",
                static_cast<uint32_t>(hr), m_width, m_height, m_isHDR);
            Close();
            return false;
        }

        // Re-read the actual output type to get the correct stride.
        winrt::com_ptr<IMFMediaType> actualType;
        hr = m_reader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), actualType.put());
        if (SUCCEEDED(hr))
        {
            UINT32 actualStride = 0;
            hr = actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, &actualStride);
            if (SUCCEEDED(hr) && actualStride > 0)
                m_stride = actualStride;

            // Log the actual subtype for diagnostics.
            GUID subtype{};
            actualType->GetGUID(MF_MT_SUBTYPE, &subtype);
            OutputDebugStringW(std::format(L"[VideoSource] Actual output format: stride={}, HDR={}\n",
                m_stride, m_isHDR).c_str());
        }

        // Create the persistent D2D bitmap.
        if (!CreateBitmap(dc))
        {
            m_lastError = L"Failed to create D2D bitmap";
            Close();
            return false;
        }

        // Decode the first frame so we have something to show immediately.
        m_currentPositionSeconds = 0.0;
        m_accumulatedTime = 0.0;
        m_endOfStream = false;
        ReadNextFrame(dc);

        return true;
    }

    void VideoSourceProvider::Close()
    {
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

        if (m_mfInitialized)
        {
            ReleaseMF();
            m_mfInitialized = false;
        }
    }

    void VideoSourceProvider::Play()
    {
        if (!m_reader) return;
        if (m_endOfStream && m_loop)
            Seek(0.0);
        m_playing = true;
    }

    void VideoSourceProvider::Pause()
    {
        m_playing = false;
    }

    void VideoSourceProvider::Seek(double seconds)
    {
        if (!m_reader) return;

        seconds = (std::clamp)(seconds, 0.0, m_durationSeconds);

        // MF expects 100-nanosecond units.
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10'000'000.0);

        HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, var);
        PropVariantClear(&var);

        if (SUCCEEDED(hr))
        {
            m_currentPositionSeconds = seconds;
            m_accumulatedTime = 0.0;
            m_endOfStream = false;
        }
    }

    bool VideoSourceProvider::AdvanceFrame(ID2D1DeviceContext5* dc, double deltaSeconds)
    {
        if (!m_reader || !m_playing || !dc)
            return false;

        m_accumulatedTime += deltaSeconds * m_speed;

        // Only decode a new frame if enough time has passed.
        if (m_accumulatedTime < m_frameDuration)
            return false;

        // Skip frames if we're behind (drop frames to keep up).
        while (m_accumulatedTime >= m_frameDuration)
        {
            m_accumulatedTime -= m_frameDuration;

            if (m_endOfStream)
            {
                if (m_loop)
                {
                    Seek(0.0);
                    m_playing = true;
                }
                else
                {
                    m_playing = false;
                    return false;
                }
            }

            if (!ReadNextFrame(dc))
            {
                // If ReadNextFrame failed (EOS or error), break out.
                if (m_endOfStream && m_loop)
                {
                    Seek(0.0);
                    m_playing = true;
                    ReadNextFrame(dc);
                }
                break;
            }
        }

        m_accumulatedTime = 0.0;  // reset to avoid drift
        return true;
    }

    bool VideoSourceProvider::ReadNextFrame(ID2D1DeviceContext5* dc)
    {
        if (!m_reader || !m_bitmap)
            return false;

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        winrt::com_ptr<IMFSample> sample;

        HRESULT hr = m_reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, &streamIndex, &flags, &timestamp, sample.put());

        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[VideoSource] ReadSample failed: 0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
            return false;
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            m_endOfStream = true;
            return false;
        }

        if (!sample)
            return false;

        // Update position from timestamp (100ns units → seconds).
        m_currentPositionSeconds = static_cast<double>(timestamp) / 10'000'000.0;

        // Get the buffer — prefer IMF2DBuffer for correct stride.
        winrt::com_ptr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(buffer.put());
        if (FAILED(hr))
            return false;

        // Try 2D buffer interface for proper stride handling.
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
            if (FAILED(hr))
                return false;
            pitch = static_cast<LONG>(m_stride);
        }

        // Handle negative stride (bottom-up bitmap).
        if (pitch < 0)
        {
            data = data + static_cast<LONG>(m_height - 1) * pitch;
            pitch = -pitch;
        }

        // Convert BGRA8 → scRGB FP16 and copy into the D2D bitmap.
        ConvertToScRGB(data, pitch);
        D2D1_RECT_U destRect = { 0, 0, m_width, m_height };
        UINT32 fp16Stride = m_width * 4 * sizeof(uint16_t);
        hr = m_bitmap->CopyFromMemory(&destRect,
            reinterpret_cast<const BYTE*>(m_fp16Buffer.data()), fp16Stride);

        if (locked2D)
            buffer2D->Unlock2D();
        else
            buffer->Unlock();

        return SUCCEEDED(hr);
    }

    // -----------------------------------------------------------------------
    // Color conversion: BGRA8 → scRGB FP16
    // -----------------------------------------------------------------------

    // Inverse PQ (ST.2084) EOTF: electrical → optical (0–10000 nits).
    static float InversePQ(float N)
    {
        // N is normalized [0,1] electrical signal.
        constexpr float m1 = 0.1593017578125f;
        constexpr float m2 = 78.84375f;
        constexpr float c1 = 0.8359375f;
        constexpr float c2 = 18.8515625f;
        constexpr float c3 = 18.6875f;

        float Np = std::pow((std::max)(N, 0.0f), 1.0f / m2);
        float num = (std::max)(Np - c1, 0.0f);
        float den = c2 - c3 * Np;
        if (den <= 0.0f) return 0.0f;
        // Returns linear light in [0, 10000] nits.
        return 10000.0f * std::pow(num / den, 1.0f / m1);
    }

    // sRGB EOTF: electrical → linear light.
    static float SRGBToLinear(float s)
    {
        if (s <= 0.04045f)
            return s / 12.92f;
        return std::pow((s + 0.055f) / 1.055f, 2.4f);
    }

    // BT.2020 → BT.709 (sRGB) color matrix (3×3, row-major).
    // Converts linear BT.2020 RGB to linear BT.709 RGB.
    static void BT2020ToBT709(float r2020, float g2020, float b2020,
                               float& r709, float& g709, float& b709)
    {
        r709 =  1.6605f * r2020 - 0.5877f * g2020 - 0.0728f * b2020;
        g709 = -0.1246f * r2020 + 1.1330f * g2020 - 0.0084f * b2020;
        b709 = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;
    }

    uint16_t VideoSourceProvider::FloatToHalf(float f)
    {
        // IEEE 754 float → half conversion.
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
        {
            return static_cast<uint16_t>(sign | 0x7C00 | (mantissa ? (mantissa >> 13) : 0));
        }
        if (exponent > 30)
        {
            return static_cast<uint16_t>(sign | 0x7C00);  // infinity
        }
        return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
    }

    void VideoSourceProvider::ConvertToScRGB(const BYTE* bgra, LONG pitch)
    {
        size_t pixelCount = static_cast<size_t>(m_width) * m_height;
        m_fp16Buffer.resize(pixelCount * 4);

        for (uint32_t y = 0; y < m_height; ++y)
        {
            const BYTE* row = bgra + y * pitch;
            uint16_t* outRow = m_fp16Buffer.data() + y * m_width * 4;

            for (uint32_t x = 0; x < m_width; ++x)
            {
                // MF RGB32 layout: B G R X (or A).
                float b = row[x * 4 + 0] / 255.0f;
                float g = row[x * 4 + 1] / 255.0f;
                float r = row[x * 4 + 2] / 255.0f;

                float linR, linG, linB;

                if (m_isHDR)
                {
                    // MF decoded HDR10: values are PQ-encoded BT.2020.
                    // Step 1: Inverse PQ → linear nits (0–10000).
                    float nitsR = InversePQ(r);
                    float nitsG = InversePQ(g);
                    float nitsB = InversePQ(b);

                    // Step 2: BT.2020 → BT.709 gamut conversion.
                    float r709, g709, b709;
                    BT2020ToBT709(nitsR, nitsG, nitsB, r709, g709, b709);

                    // Step 3: Nits → scRGB (1.0 = 80 nits SDR white).
                    linR = r709 / 80.0f;
                    linG = g709 / 80.0f;
                    linB = b709 / 80.0f;
                }
                else
                {
                    // SDR: sRGB gamma → linear scRGB.
                    linR = SRGBToLinear(r);
                    linG = SRGBToLinear(g);
                    linB = SRGBToLinear(b);
                }

                outRow[x * 4 + 0] = FloatToHalf(linR);
                outRow[x * 4 + 1] = FloatToHalf(linG);
                outRow[x * 4 + 2] = FloatToHalf(linB);
                outRow[x * 4 + 3] = FloatToHalf(1.0f);  // alpha
            }
        }
    }

    bool VideoSourceProvider::CreateBitmap(ID2D1DeviceContext5* dc)
    {
        if (!dc || m_width == 0 || m_height == 0)
            return false;

        D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
        // Always output scRGB FP16 — matches the pipeline format.
        // SDR content: sRGB→linear. HDR content: PQ BT.2020→scRGB linear.
        bitmapProps.pixelFormat.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bitmapProps.dpiX = 96.0f;
        bitmapProps.dpiY = 96.0f;
        bitmapProps.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

        D2D1_SIZE_U size = { m_width, m_height };
        HRESULT hr = dc->CreateBitmap(size, nullptr, 0, bitmapProps, m_bitmap.put());
        return SUCCEEDED(hr);
    }
}
