#include "pch.h"
#include "VideoSourceProvider.h"

namespace ShaderLab::Effects
{
    VideoSourceProvider::VideoSourceProvider() = default;

    VideoSourceProvider::~VideoSourceProvider()
    {
        Close();
    }

    bool VideoSourceProvider::Open(const std::wstring& filePath, ID2D1DeviceContext5* dc)
    {
        Close();

        // Initialize Media Foundation.
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr))
            return false;
        m_mfInitialized = true;

        // Create Source Reader.
        winrt::com_ptr<IMFAttributes> attrs;
        hr = MFCreateAttributes(attrs.put(), 1);
        if (FAILED(hr))
            return false;

        // Enable video processing (format conversion).
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        hr = MFCreateSourceReaderFromURL(filePath.c_str(), attrs.get(), m_reader.put());
        if (FAILED(hr))
            return false;

        // Get native media type to detect HDR and frame info.
        winrt::com_ptr<IMFMediaType> nativeType;
        hr = m_reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nativeType.put());
        if (FAILED(hr))
        {
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
            // Duration is in 100-nanosecond units.
            m_durationSeconds = static_cast<double>(var.uhVal.QuadPart) / 10'000'000.0;
            PropVariantClear(&var);
        }

        // Configure output format: request BGRA (8-bit) or ABGR16F (HDR).
        winrt::com_ptr<IMFMediaType> outputType;
        hr = MFCreateMediaType(outputType.put());
        if (FAILED(hr)) { Close(); return false; }

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        // Always request BGRA 8-bit — reliable across all decoders.
        // HDR content will be tone-mapped by the decoder or handled downstream.
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        m_stride = m_width * 4;

        hr = MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
        if (FAILED(hr)) { Close(); return false; }

        hr = m_reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
        if (FAILED(hr)) { Close(); return false; }

        // Select only the video stream.
        m_reader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        m_reader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        // Create the persistent D2D bitmap.
        if (!CreateBitmap(dc))
        {
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
            MFShutdown();
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
            return false;

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            m_endOfStream = true;
            return false;
        }

        if (!sample)
            return false;

        // Update position from timestamp (100ns units → seconds).
        m_currentPositionSeconds = static_cast<double>(timestamp) / 10'000'000.0;

        // Get the buffer.
        winrt::com_ptr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(buffer.put());
        if (FAILED(hr))
            return false;

        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = buffer->Lock(&data, &maxLen, &curLen);
        if (FAILED(hr))
            return false;

        // Copy frame data into the persistent D2D bitmap.
        D2D1_RECT_U destRect = { 0, 0, m_width, m_height };
        hr = m_bitmap->CopyFromMemory(&destRect, data, m_stride);

        buffer->Unlock();

        return SUCCEEDED(hr);
    }

    bool VideoSourceProvider::CreateBitmap(ID2D1DeviceContext5* dc)
    {
        if (!dc || m_width == 0 || m_height == 0)
            return false;

        D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
        bitmapProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bitmapProps.dpiX = 96.0f;
        bitmapProps.dpiY = 96.0f;
        bitmapProps.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

        D2D1_SIZE_U size = { m_width, m_height };
        HRESULT hr = dc->CreateBitmap(size, nullptr, 0, bitmapProps, m_bitmap.put());
        return SUCCEEDED(hr);
    }
}
