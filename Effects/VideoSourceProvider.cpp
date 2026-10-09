#include "pch_engine.h"
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
    // HLSL compute shaders for video frame color conversion.
    // These run on the GPU, converting raw YUV/RGB data to scRGB FP16.
    // -----------------------------------------------------------------------

    static const char* s_csP010Source = R"(
// P010 (10-bit YUV 4:2:0, PQ/BT.2020) → scRGB FP16 compute shader.
// Reads Y plane (R16_UNORM) and UV plane (R16G16_UNORM), outputs float4.

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
RWTexture2D<float4> output : register(u0);

cbuffer Params : register(b0) { uint Width; uint Height; uint Pad0; uint Pad1; };

float InversePQ(float N)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float Np = pow(max(N, 0.0), 1.0 / m2);
    float num = max(Np - c1, 0.0);
    float den = c2 - c3 * Np;
    return (den <= 0.0) ? 0.0 : 10000.0 * pow(num / den, 1.0 / m1);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    float yVal = texY[id.xy];
    float2 uv = texUV[uint2(id.x / 2, id.y / 2)];

    // BT.2020 limited range (10-bit levels scaled to [0,1] by R16_UNORM).
    // Y: [64/1023, 940/1023], CbCr: [64/1023, 960/1023], mid=512/1023.
    float yScaled = (yVal * 1023.0 - 64.0) / (940.0 - 64.0);
    float cb = (uv.x * 1023.0 - 512.0) / (960.0 - 64.0);
    float cr = (uv.y * 1023.0 - 512.0) / (960.0 - 64.0);
    yScaled = max(yScaled, 0.0);

    // BT.2020 NCL YCbCr → RGB (PQ-encoded).
    float r = yScaled + 1.47460 * cr;
    float g = yScaled - 0.16455 * cb - 0.57135 * cr;
    float b = yScaled + 1.88100 * cb;
    r = saturate(r); g = saturate(g); b = saturate(b);

    // Inverse PQ → linear nits.
    float nR = InversePQ(r);
    float nG = InversePQ(g);
    float nB = InversePQ(b);

    // BT.2020 → BT.709 gamut matrix.
    float3 rgb709;
    rgb709.r =  1.6605 * nR - 0.5877 * nG - 0.0728 * nB;
    rgb709.g = -0.1246 * nR + 1.1330 * nG - 0.0084 * nB;
    rgb709.b = -0.0182 * nR - 0.1006 * nG + 1.1187 * nB;

    // Nits → scRGB (80 nits = 1.0).
    output[id.xy] = float4(rgb709 / 80.0, 1.0);
}
)";

    static const char* s_csNV12Source = R"(
// NV12 (8-bit YUV 4:2:0, sRGB/BT.709) → scRGB FP16 compute shader.

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
RWTexture2D<float4> output : register(u0);

cbuffer Params : register(b0) { uint Width; uint Height; uint Pad0; uint Pad1; };

float SRGBToLinear(float s)
{
    return (s <= 0.04045) ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    float yVal = texY[id.xy];
    float2 uv = texUV[uint2(id.x / 2, id.y / 2)];

    // BT.709 limited range (8-bit levels scaled to [0,1] by R8_UNORM).
    float yScaled = (yVal * 255.0 - 16.0) / (235.0 - 16.0);
    float cb = (uv.x * 255.0 - 128.0) / (240.0 - 16.0);
    float cr = (uv.y * 255.0 - 128.0) / (240.0 - 16.0);
    yScaled = max(yScaled, 0.0);

    // BT.709 YCbCr → RGB.
    float r = saturate(yScaled + 1.5748 * cr);
    float g = saturate(yScaled - 0.1873 * cb - 0.4681 * cr);
    float b = saturate(yScaled + 1.8556 * cb);

    // sRGB EOTF → linear.
    output[id.xy] = float4(SRGBToLinear(r), SRGBToLinear(g), SRGBToLinear(b), 1.0);
}
)";

    static const char* s_csRGB32Source = R"(
// RGB32 (BGRA8, sRGB) → scRGB FP16 compute shader.

Texture2D<float4> texRGB : register(t0);
RWTexture2D<float4> output : register(u0);

cbuffer Params : register(b0) { uint Width; uint Height; uint Pad0; uint Pad1; };

float SRGBToLinear(float s)
{
    return (s <= 0.04045) ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    float4 px = texRGB[id.xy]; // BGRA as float4 via B8G8R8A8_UNORM SRV
    output[id.xy] = float4(SRGBToLinear(px.r), SRGBToLinear(px.g), SRGBToLinear(px.b), 1.0);
}
)";

    // -----------------------------------------------------------------------
    // FloatToHalf (only needed for fallback; GPU path doesn't use it)
    // -----------------------------------------------------------------------

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
    // Lifecycle
    // -----------------------------------------------------------------------

    VideoSourceProvider::VideoSourceProvider() = default;

    VideoSourceProvider::~VideoSourceProvider()
    {
        Close();
    }

    bool VideoSourceProvider::Open(const std::wstring& filePath, ID2D1DeviceContext5* dc,
                                    ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext)
    {
        Close();
        m_lastError.clear();
        m_filePath = filePath;
        m_d3dDevice = d3dDevice;
        m_d3dContext = d3dContext;
        if (d3dDevice && FAILED(d3dDevice->CreateDeferredContext(0, m_convertCtx.put())))
            m_convertCtx = nullptr;   // falls back to the immediate context

        if (filePath.empty())
        {
            m_lastError = L"Empty file path";
            return false;
        }

        EnsureMFInitialized();
        m_mfInitialized = true;

        HRESULT hr;

        // Create DXGI Device Manager for hardware-accelerated decode.
        UINT resetToken = 0;
        winrt::com_ptr<IMFDXGIDeviceManager> dxgiManager;
        hr = MFCreateDXGIDeviceManager(&resetToken, dxgiManager.put());
        if (SUCCEEDED(hr))
        {
            hr = dxgiManager->ResetDevice(d3dDevice, resetToken);
            if (SUCCEEDED(hr))
            {
                m_dxgiDeviceManager = dxgiManager;
                m_resetToken = resetToken;
            }
        }

        // Try opening with DXGI device manager first (hardware decode).
        // If that fails, try without video processing (native decoder output).
        // Final fallback: with video processing (software format conversion).
        auto tryCreateReader = [&](bool useDxgi, bool useVideoProc) -> HRESULT
        {
            winrt::com_ptr<IMFAttributes> attrs;
            HRESULT hres = MFCreateAttributes(attrs.put(), 3);
            if (FAILED(hres)) return hres;

            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            if (useDxgi && m_dxgiDeviceManager)
                attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_dxgiDeviceManager.get());
            if (useVideoProc)
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            return MFCreateSourceReaderFromURL(filePath.c_str(), attrs.get(), m_reader.put());
        };

        // Attempt 1: DXGI manager (hardware decode).
        int readerAttempt = 1;
        hr = tryCreateReader(true, false);
        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[VideoSource] Attempt 1 (DXGI) failed: 0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
            // Attempt 2: Video processing (software format conversion — reliable).
            m_dxgiDeviceManager = nullptr;
            readerAttempt = 2;
            hr = tryCreateReader(false, true);
        }
        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[VideoSource] Attempt 2 (video proc) failed: 0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
            // Attempt 3: Bare reader (no acceleration, no format conversion).
            readerAttempt = 3;
            hr = tryCreateReader(false, false);
        }
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFCreateSourceReaderFromURL failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }
        OutputDebugStringW(std::format(L"[VideoSource] Reader created via attempt {}\n", readerAttempt).c_str());

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
        
        // Also check video profile — HEVC Main 10 is typically HDR.
        GUID subtype{};
        nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 profile = 0;
        nativeType->GetUINT32(MF_MT_MPEG2_PROFILE, &profile);

        m_isHDR = (transferFunc == MFVideoTransFunc_2084) ||
                  (colorPrimaries == MFVideoPrimaries_BT2020);

        // Fallback: detect HDR from filename hint or 10-bit profile.
        if (!m_isHDR && (subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265))
        {
            // HEVC Main 10 profile (2 = Main 10) is almost always HDR content.
            if (profile == 2) m_isHDR = true;
        }
        // Filename hint: "(HDR)" in the path.
        if (!m_isHDR && filePath.find(L"HDR") != std::wstring::npos)
            m_isHDR = true;

        OutputDebugStringW(std::format(
            L"[VideoSource] HDR detection: transferFunc={}, primaries={}, profile={}, isHDR={}\n",
            transferFunc, colorPrimaries, profile, m_isHDR).c_str());

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

        // Choose output format.
        // P010 preserves HDR; RGB32 is the reliable fallback (video processor
        // handles all YUV→RGB conversion, avoiding NV12 stride issues).
        winrt::com_ptr<IMFMediaType> outputType;
        MFCreateMediaType(outputType.put());
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        if (m_isHDR)
        {
            // Try P010 first (10-bit HDR).
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_P010);
            MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
            m_stride = m_width * 2;
            hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
            if (SUCCEEDED(hr))
            {
                m_outputFormat = OutputFormat::P010;
                OutputDebugStringW(L"[VideoSource] P010 format accepted\n");
            }
            else
            {
                OutputDebugStringW(std::format(L"[VideoSource] P010 rejected: 0x{:08X}, falling back to RGB32\n",
                    static_cast<uint32_t>(hr)).c_str());
                // Fall back to RGB32 — video processor handles YUV→RGB.
                outputType = nullptr;
                MFCreateMediaType(outputType.put());
                outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
                m_stride = m_width * 4;
                hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
                if (SUCCEEDED(hr)) m_outputFormat = OutputFormat::RGB32;
            }
        }
        else
        {
            // SDR: try NV12 first, fall back to RGB32.
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
            m_stride = m_width;
            hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
            if (SUCCEEDED(hr))
            {
                m_outputFormat = OutputFormat::NV12;
            }
            else
            {
                outputType = nullptr;
                MFCreateMediaType(outputType.put());
                outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
                m_stride = m_width * 4;
                hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
                if (SUCCEEDED(hr)) m_outputFormat = OutputFormat::RGB32;
            }
        }
        if (FAILED(hr))
        {
            m_lastError = std::format(L"SetCurrentMediaType failed: 0x{:08X}", static_cast<uint32_t>(hr));
            Close();
            return false;
        }

        // Re-read actual stride from the confirmed output type.
        winrt::com_ptr<IMFMediaType> actualType;
        hr = m_reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), actualType.put());
        if (SUCCEEDED(hr))
        {
            INT32 actualStride = 0;
            if (SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&actualStride))))
            {
                if (actualStride < 0) { m_stride = static_cast<uint32_t>(-actualStride); m_bottomUp = true; }
                else if (actualStride > 0) { m_stride = static_cast<uint32_t>(actualStride); m_bottomUp = false; }
            }
        }

        // Create GPU conversion resources.
        if (!CreateGPUResources(dc, d3dDevice))
        {
            m_lastError = L"Failed to create GPU conversion resources: " + m_lastError;
            Close();
            return false;
        }

        // Compile the appropriate conversion shader.
        if (!CompileConversionShader(d3dDevice))
        {
            m_lastError = L"Failed to compile conversion shader: " + m_lastError;
            Close();
            return false;
        }

        // Decode the first frame synchronously so the node has an image at once.
        {
            DecodedFrame first;
            ReadStats stats;
            if (DecodeOneFrame(first, 0, {}, stats) == DecodeResult::Frame)
            {
                first.seekFrame = true;
                m_firstFrameTime = first.time;
                std::lock_guard lock(m_stateMutex);
                m_decodeHead = first.time;
                RecordKeyframeLocked(first.time, first.time);
                m_queue.push_back(std::move(first));
            }
        }

        // Start background decode thread.
        m_decodeThread = std::jthread([this](std::stop_token token) { DecodeThreadFunc(token); });

        const wchar_t* fmtName = (m_outputFormat == OutputFormat::P010) ? L"P010"
            : (m_outputFormat == OutputFormat::NV12) ? L"NV12" : L"RGB32";
        OutputDebugStringW(std::format(L"[VideoSource] Opened: {}x{} @ {:.1f}fps, {:.1f}s, HDR={}, format={}\n",
            m_width, m_height, m_frameRate, m_durationSeconds, m_isHDR, fmtName).c_str());

        return true;
    }

    void VideoSourceProvider::Close()
    {
        if (m_decodeThread.joinable())
        {
            // Release the queued frames first: their decoder surfaces are what
            // a ReadSample blocked on a full surface pool is waiting for.
            std::vector<DecodedFrame> released;
            {
                std::lock_guard lock(m_stateMutex);
                m_decodeThread.request_stop();
                for (auto& frame : m_queue) released.push_back(std::move(frame));
                m_queue.clear();
            }
            released.clear();
            m_decodeCV.notify_all();
            m_decodeThread.join();
        }

        // Held samples pin decoder surfaces: release them before the reader.
        m_queue.clear();
        m_spareBuffers.clear();
        m_seekPending = false;
        m_seekTarget = 0.0;
        m_endOfStream = false;
        m_stepRequested = false;
        m_seekFrameDecoded = true;
        m_resumePending = false;
        m_readRetried = false;
        m_keyframeSpans.clear();
        m_minKeyframeInterval = 0.0;
        m_generation = 0;
        m_targetTime = 0.0;
        m_decodeHead = 0.0;
        m_uploadedGeneration = 0;
        m_playTime = 0.0;
        m_decodeSecondsPerFrame = 0.0;
        m_seekOverheadSeconds = 0.0;
        m_meanPrerollSeconds = 0.0;
        m_texPlanar = nullptr; m_srvPlanarY = nullptr; m_srvPlanarUV = nullptr;
        m_zeroCopyFailed = false;
        m_lastSeekTarget = std::numeric_limits<double>::quiet_NaN();
        m_reader = nullptr;
        m_dxgiDeviceManager = nullptr;
        m_bitmap = nullptr;
        m_csP010 = nullptr; m_csNV12 = nullptr; m_csRGB32 = nullptr;
        m_texY = nullptr; m_texUV = nullptr; m_texRGB = nullptr;
        m_texOutput = nullptr;
        m_srvY = nullptr; m_srvUV = nullptr; m_srvRGB = nullptr;
        m_uavOutput = nullptr; m_cbParams = nullptr;
        m_d3dDevice = nullptr; m_d3dContext = nullptr; m_convertCtx = nullptr;
        m_width = 0; m_height = 0; m_stride = 0;
        m_frameRate = 0.0; m_durationSeconds = 0.0;
        m_currentPositionSeconds = 0.0; m_frameDuration = 0.0;
        m_playing = false;
        m_firstFrameLogged = false;
        m_uploadedFrameTime = -1.0;
        m_uploadedFrameDuration = 0.0;
        m_firstFrameTime = 0.0;

        if (m_mfInitialized) { ReleaseMF(); m_mfInitialized = false; }
    }

    // -----------------------------------------------------------------------
    // GPU resource creation
    // -----------------------------------------------------------------------

    bool VideoSourceProvider::CreateGPUResources(ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice)
    {
        if (!dc || !d3dDevice || m_width == 0 || m_height == 0) return false;
        HRESULT hr;

        // Output texture: R16G16B16A16_FLOAT with UAV + shader resource for D2D sharing.
        D3D11_TEXTURE2D_DESC outDesc{};
        outDesc.Width = m_width;
        outDesc.Height = m_height;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        hr = d3dDevice->CreateTexture2D(&outDesc, nullptr, m_texOutput.put());
        if (FAILED(hr)) { m_lastError = L"CreateTexture2D output"; return false; }

        hr = d3dDevice->CreateUnorderedAccessView(m_texOutput.get(), nullptr, m_uavOutput.put());
        if (FAILED(hr)) { m_lastError = L"CreateUAV output"; return false; }

        // Create D2D bitmap from the output texture's DXGI surface.
        winrt::com_ptr<IDXGISurface> surface;
        m_texOutput.as(surface);
        D2D1_BITMAP_PROPERTIES1 bmpProps{};
        bmpProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        bmpProps.dpiX = 96.0f; bmpProps.dpiY = 96.0f;
        bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        hr = dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_bitmap.put());
        if (FAILED(hr)) { m_lastError = L"CreateBitmapFromDxgiSurface"; return false; }

        // Create separate Y/UV/RGB textures for UpdateSubresource uploads.
        if (m_outputFormat == OutputFormat::P010)
        {
            // Y plane: R16_UNORM, full resolution.
            D3D11_TEXTURE2D_DESC yDesc{};
            yDesc.Width = m_width; yDesc.Height = m_height;
            yDesc.MipLevels = 1; yDesc.ArraySize = 1;
            yDesc.Format = DXGI_FORMAT_R16_UNORM;
            yDesc.SampleDesc.Count = 1;
            yDesc.Usage = D3D11_USAGE_DEFAULT;
            yDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice->CreateTexture2D(&yDesc, nullptr, m_texY.put());
            if (FAILED(hr)) { m_lastError = L"CreateTexture2D Y (P010)"; return false; }

            hr = d3dDevice->CreateShaderResourceView(m_texY.get(), nullptr, m_srvY.put());
            if (FAILED(hr)) { m_lastError = L"CreateSRV Y"; return false; }

            // UV plane: R16G16_UNORM, half resolution.
            D3D11_TEXTURE2D_DESC uvDesc{};
            uvDesc.Width = m_width / 2; uvDesc.Height = m_height / 2;
            uvDesc.MipLevels = 1; uvDesc.ArraySize = 1;
            uvDesc.Format = DXGI_FORMAT_R16G16_UNORM;
            uvDesc.SampleDesc.Count = 1;
            uvDesc.Usage = D3D11_USAGE_DEFAULT;
            uvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice->CreateTexture2D(&uvDesc, nullptr, m_texUV.put());
            if (FAILED(hr)) { m_lastError = L"CreateTexture2D UV (P010)"; return false; }

            hr = d3dDevice->CreateShaderResourceView(m_texUV.get(), nullptr, m_srvUV.put());
            if (FAILED(hr)) { m_lastError = L"CreateSRV UV"; return false; }
        }
        else if (m_outputFormat == OutputFormat::NV12)
        {
            // Y plane: R8_UNORM, full resolution.
            D3D11_TEXTURE2D_DESC yDesc{};
            yDesc.Width = m_width; yDesc.Height = m_height;
            yDesc.MipLevels = 1; yDesc.ArraySize = 1;
            yDesc.Format = DXGI_FORMAT_R8_UNORM;
            yDesc.SampleDesc.Count = 1;
            yDesc.Usage = D3D11_USAGE_DEFAULT;
            yDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice->CreateTexture2D(&yDesc, nullptr, m_texY.put());
            if (FAILED(hr)) { m_lastError = L"CreateTexture2D Y (NV12)"; return false; }

            hr = d3dDevice->CreateShaderResourceView(m_texY.get(), nullptr, m_srvY.put());
            if (FAILED(hr)) { m_lastError = L"CreateSRV Y"; return false; }

            // UV plane: R8G8_UNORM, half resolution.
            D3D11_TEXTURE2D_DESC uvDesc{};
            uvDesc.Width = m_width / 2; uvDesc.Height = m_height / 2;
            uvDesc.MipLevels = 1; uvDesc.ArraySize = 1;
            uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
            uvDesc.SampleDesc.Count = 1;
            uvDesc.Usage = D3D11_USAGE_DEFAULT;
            uvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice->CreateTexture2D(&uvDesc, nullptr, m_texUV.put());
            if (FAILED(hr)) { m_lastError = L"CreateTexture2D UV (NV12)"; return false; }

            hr = d3dDevice->CreateShaderResourceView(m_texUV.get(), nullptr, m_srvUV.put());
            if (FAILED(hr)) { m_lastError = L"CreateSRV UV"; return false; }
        }
        else // RGB32
        {
            D3D11_TEXTURE2D_DESC rgbDesc{};
            rgbDesc.Width = m_width; rgbDesc.Height = m_height;
            rgbDesc.MipLevels = 1; rgbDesc.ArraySize = 1;
            rgbDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            rgbDesc.SampleDesc.Count = 1;
            rgbDesc.Usage = D3D11_USAGE_DEFAULT;
            rgbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice->CreateTexture2D(&rgbDesc, nullptr, m_texRGB.put());
            if (FAILED(hr)) { m_lastError = L"CreateTexture2D RGB"; return false; }

            hr = d3dDevice->CreateShaderResourceView(m_texRGB.get(), nullptr, m_srvRGB.put());
            if (FAILED(hr)) { m_lastError = L"CreateSRV RGB"; return false; }
        }

        // Constant buffer for dimensions.
        struct { uint32_t w, h, pad0, pad1; } cbData = { m_width, m_height, 0, 0 };
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(cbData);
        cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cbInit{ &cbData, 0, 0 };
        hr = d3dDevice->CreateBuffer(&cbDesc, &cbInit, m_cbParams.put());
        if (FAILED(hr)) { m_lastError = L"CreateBuffer cbParams"; return false; }

        return true;
    }

    bool VideoSourceProvider::CompileConversionShader(ID3D11Device* d3dDevice)
    {
        const char* source = nullptr;
        winrt::com_ptr<ID3D11ComputeShader>* target = nullptr;

        if (m_outputFormat == OutputFormat::P010) { source = s_csP010Source; target = &m_csP010; }
        else if (m_outputFormat == OutputFormat::NV12) { source = s_csNV12Source; target = &m_csNV12; }
        else { source = s_csRGB32Source; target = &m_csRGB32; }

        winrt::com_ptr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(source, strlen(source), "VideoConvert",
            nullptr, nullptr, "main", "cs_5_0", 0, 0, blob.put(), errors.put());
        if (FAILED(hr))
        {
            if (errors)
                m_lastError = std::wstring(L"Shader compile: ") +
                    std::wstring(reinterpret_cast<const char*>(errors->GetBufferPointer()),
                                 reinterpret_cast<const char*>(errors->GetBufferPointer()) + errors->GetBufferSize());
            else
                m_lastError = std::format(L"D3DCompile failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        hr = d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, target->put());
        if (FAILED(hr)) { m_lastError = L"CreateComputeShader"; return false; }
        return true;
    }

    bool VideoSourceProvider::EnsurePlanarTexture(ID3D11Texture2D* decoderTexture)
    {
        if (m_texPlanar) return true;
        if (!decoderTexture || !m_d3dDevice) return false;
        D3D11_TEXTURE2D_DESC sd{};
        decoderTexture->GetDesc(&sd);
        DXGI_FORMAT lumaFmt, chromaFmt;
        if (sd.Format == DXGI_FORMAT_NV12)      { lumaFmt = DXGI_FORMAT_R8_UNORM;  chromaFmt = DXGI_FORMAT_R8G8_UNORM; }
        else if (sd.Format == DXGI_FORMAT_P010) { lumaFmt = DXGI_FORMAT_R16_UNORM; chromaFmt = DXGI_FORMAT_R16G16_UNORM; }
        else return false;
        // The conversion shader was chosen for the reader's output subtype, so
        // the decoder surface has to match it.
        if ((sd.Format == DXGI_FORMAT_NV12) != (m_outputFormat == OutputFormat::NV12)) return false;

        UINT support = 0;
        if (FAILED(m_d3dDevice->CheckFormatSupport(sd.Format, &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D) ||
            !(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
            return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_width;
        td.Height = m_height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = sd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        winrt::com_ptr<ID3D11Texture2D> tex;
        if (FAILED(m_d3dDevice->CreateTexture2D(&td, nullptr, tex.put()))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        winrt::com_ptr<ID3D11ShaderResourceView> srvY, srvUV;
        vd.Format = lumaFmt;
        if (FAILED(m_d3dDevice->CreateShaderResourceView(tex.get(), &vd, srvY.put()))) return false;
        vd.Format = chromaFmt;
        if (FAILED(m_d3dDevice->CreateShaderResourceView(tex.get(), &vd, srvUV.put()))) return false;

        m_texPlanar = std::move(tex);
        m_srvPlanarY = std::move(srvY);
        m_srvPlanarUV = std::move(srvUV);
        return true;
    }

    bool VideoSourceProvider::RunConversionShader(bool planar, ID3D11Texture2D* decoderTex,
                                                  UINT decoderSubresource)
    {
        // Set shader.
        ID3D11ComputeShader* cs = nullptr;
        if (m_outputFormat == OutputFormat::P010) cs = m_csP010.get();
        else if (m_outputFormat == OutputFormat::NV12) cs = m_csNV12.get();
        else cs = m_csRGB32.get();
        if (!cs)
        {
            m_lastError = L"No conversion shader for the output format";
            return false;
        }

        ID3D11DeviceContext* context = m_convertCtx ? m_convertCtx.get() : m_d3dContext;

        // Zero-copy: copy both planes of the decoder surface into the planar
        // texture, recorded with the conversion so the two run together.
        if (decoderTex)
        {
            D3D11_BOX box = { 0, 0, 0, m_width, m_height, 1 };
            context->CopySubresourceRegion(m_texPlanar.get(), 0, 0, 0, 0,
                decoderTex, decoderSubresource, &box);
        }

        context->CSSetShader(cs, nullptr, 0);

        // Bind resources.
        ID3D11Buffer* cbs[] = { m_cbParams.get() };
        context->CSSetConstantBuffers(0, 1, cbs);

        if (m_outputFormat == OutputFormat::RGB32)
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvRGB.get() };
            context->CSSetShaderResources(0, 1, srvs);
        }
        else if (planar)
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvPlanarY.get(), m_srvPlanarUV.get() };
            context->CSSetShaderResources(0, 2, srvs);
        }
        else
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvY.get(), m_srvUV.get() };
            context->CSSetShaderResources(0, 2, srvs);
        }

        ID3D11UnorderedAccessView* uavs[] = { m_uavOutput.get() };
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // Dispatch: 16x16 thread groups.
        UINT gx = (m_width + 15) / 16;
        UINT gy = (m_height + 15) / 16;
        context->Dispatch(gx, gy, 1);

        // Unbind.
        ID3D11ShaderResourceView* nullSRVs[2] = {};
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        context->CSSetShaderResources(0, 2, nullSRVs);
        context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);

        if (m_convertCtx)
        {
            winrt::com_ptr<ID3D11CommandList> commandList;
            const HRESULT hr = m_convertCtx->FinishCommandList(FALSE, commandList.put());
            if (FAILED(hr) || !commandList)
            {
                m_lastError = std::format(L"FinishCommandList failed: 0x{:08X}", static_cast<uint32_t>(hr));
                return false;
            }
            // TRUE keeps the immediate context's state, which D2D on
            // another thread may be in the middle of setting.
            m_d3dContext->ExecuteCommandList(commandList.get(), TRUE);
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Playback controls (render thread)
    // -----------------------------------------------------------------------

    // A frame starting within this of a target counts as starting at it.
    static constexpr double scTimeTolerance = 1e-4;
    // Weight of each new measurement in the decode and seek cost averages.
    static constexpr double scCostSmoothing = 0.2;
    // A read loop this long times decode throughput. Single reads after an
    // idle wait come from the decoder's pipeline and are far cheaper.
    static constexpr int scMinTimedReads = 8;
    // Seek overhead, in seconds of video, until the costs have been measured.
    static constexpr double scDefaultSeekThresholdSeconds = 0.25;
    // Keyframe-to-target distance a seek is assumed to decode through until
    // one has been measured: half a typical 2 s keyframe interval.
    static constexpr double scDefaultPrerollSeconds = 1.0;
    static constexpr size_t scMaxKnownKeyframes = 4096;
    // Longest a catch-up decodes without delivering a frame.
    static constexpr double scCatchUpFrameSeconds = 0.1;

    static void Smooth(std::atomic<double>& average, double sample)
    {
        const double previous = average.load();
        average = previous > 0.0 ? previous + scCostSmoothing * (sample - previous) : sample;
    }

    static double SecondsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    void VideoSourceProvider::Play()
    {
        if (!m_reader) return;
        // A video that played to its end without looping starts over.
        if (!m_loop && m_durationSeconds > 0.0 && m_playTime >= m_durationSeconds - m_frameDuration)
            Seek(0.0);
        m_playing = true;
    }

    void VideoSourceProvider::Pause() { m_playing = false; }

    void VideoSourceProvider::Seek(double seconds)
    {
        if (!m_reader) return;
        seconds = m_durationSeconds > 0.0 ? (std::clamp)(seconds, 0.0, m_durationSeconds) : (std::max)(seconds, 0.0);
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            ++m_generation;
            m_seekTarget = seconds;
            m_seekPending = true;
            m_seekFrameDecoded = false;
            m_resumePending = false;
            m_readRetried = false;
            m_stepRequested = false;
            m_targetTime = seconds;
            m_decodeHead = seconds;
            for (auto& frame : m_queue)
            {
                RecycleLocked(frame);
                released.push_back(std::move(frame));
            }
            m_queue.clear();
        }
        m_decodeCV.notify_one();
        ++m_seekCount;
        m_playTime = seconds;
        m_currentPositionSeconds = seconds;
        m_lastSeekTarget = seconds;
    }

    double VideoSourceProvider::SeekThresholdFrames() const
    {
        const double frameSeconds = m_decodeSecondsPerFrame.load();
        const double overheadSeconds = m_seekOverheadSeconds.load();
        const double frames = (frameSeconds > 0.0 && overheadSeconds > 0.0)
            ? overheadSeconds / frameSeconds
            : scDefaultSeekThresholdSeconds * m_frameRate;
        return (std::max)(static_cast<double>(scDecodeAheadFrames), frames);
    }

    size_t VideoSourceProvider::KnownKeyframeCount()
    {
        std::lock_guard lock(m_stateMutex);
        return m_keyframeSpans.size();
    }

    void VideoSourceProvider::RecordKeyframeLocked(double keyframe, double target)
    {
        if (keyframe < 0.0 || target < keyframe - scTimeTolerance) return;
        m_minKeyframeInterval = (std::max)(m_minKeyframeInterval, target - keyframe);
        auto it = m_keyframeSpans.find(keyframe);
        if (it != m_keyframeSpans.end())
            it->second = (std::max)(it->second, target);
        else if (m_keyframeSpans.size() < scMaxKnownKeyframes)
            m_keyframeSpans.emplace(keyframe, target);
    }

    bool VideoSourceProvider::ForwardSeekPaysLocked(double target) const
    {
        if (m_frameDuration <= 0.0) return false;
        const double threshold = SeekThresholdFrames() * m_frameDuration;
        if (target - m_decodeHead <= threshold) return false;

        // The latest known keyframe at or before the target. A seek lands on
        // it exactly when the target is inside its known span, and at or
        // after it otherwise.
        double knownKeyframe = -1.0;
        double knownSpanEnd = -1.0;
        bool landingKnown = false;
        auto it = m_keyframeSpans.upper_bound(target + scTimeTolerance);
        if (it != m_keyframeSpans.begin())
        {
            --it;
            knownKeyframe = it->first;
            knownSpanEnd = it->second;
            landingKnown = target <= knownSpanEnd + scTimeTolerance;
        }

        // While a seek is still decoding up to its frame, only a keyframe known
        // to be further ahead justifies another; an estimate would restart it
        // on every tick.
        if (!m_seekFrameDecoded || landingKnown)
            return knownKeyframe - m_decodeHead > threshold;

        // Otherwise the seek lands on the known keyframe if the next one
        // cannot be that close yet, or the usual preroll before the target if
        // that falls past the known span, which holds no other keyframe.
        if (target < knownKeyframe + m_minKeyframeInterval)
            return knownKeyframe - m_decodeHead > threshold;
        const double meanPreroll = m_meanPrerollSeconds.load();
        const double expected = target - (meanPreroll > 0.0 ? meanPreroll : scDefaultPrerollSeconds);
        const double landing = expected > knownSpanEnd ? expected : knownKeyframe;
        return landing - m_decodeHead > threshold;
    }

    void VideoSourceProvider::PlayTo(double seconds)
    {
        if (!m_reader) return;
        seconds = (std::max)(seconds, 0.0);
        // Past the end holds the last frame, which ends at or after the duration.
        if (m_durationSeconds > 0.0)
            seconds = (std::min)(seconds, m_durationSeconds - 2.0 * scTimeTolerance);
        m_playTime = seconds;
        // A seek to this time is already delivering its frame.
        if (seconds == m_lastSeekTarget.load()) return;

        bool seek;
        {
            std::lock_guard lock(m_stateMutex);
            // The earliest time reachable without a seek is the start of the
            // frame on screen, or the pending seek's target. The first frame
            // also covers any earlier time.
            const bool shownThisGeneration = m_uploadedGeneration == m_generation.load() && m_uploadedFrameTime.load() >= 0.0;
            const double earliest = shownThisGeneration ? m_uploadedFrameTime.load() : m_seekTarget;
            const bool behind = earliest > m_firstFrameTime + scTimeTolerance && seconds < earliest - scTimeTolerance;
            seek = behind || ForwardSeekPaysLocked(seconds);
        }
        if (seek)
        {
            Seek(seconds);
            return;
        }
        SetTarget(seconds);
    }

    void VideoSourceProvider::SetTarget(double seconds)
    {
        m_lastSeekTarget = std::numeric_limits<double>::quiet_NaN();
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            m_targetTime = seconds;
            PruneQueueLocked(released);
        }
        m_decodeCV.notify_one();
    }

    void VideoSourceProvider::Tick(double deltaSeconds)
    {
        if (!m_reader || !m_playing) return;
        double seconds = m_playTime + deltaSeconds * m_speed;
        if (m_durationSeconds > 0.0 && seconds >= m_durationSeconds)
        {
            if (m_loop)
            {
                seconds = std::fmod(seconds, m_durationSeconds);
            }
            else
            {
                // Hold the last frame, which ends at or after the duration.
                seconds = m_durationSeconds - 2.0 * scTimeTolerance;
                m_playing = false;
            }
        }
        PlayTo(seconds);
    }

    void VideoSourceProvider::RequestNextFrame()
    {
        if (!m_reader) return;
        // Until a seek's frame is up there is nothing to step from.
        const double shown = m_uploadedFrameTime.load();
        if (shown < 0.0 || m_uploadedGeneration != m_generation.load()) return;
        const double next = shown + (std::max)(m_uploadedFrameDuration.load(), 2.0 * scTimeTolerance);
        m_lastSeekTarget = std::numeric_limits<double>::quiet_NaN();
        m_playTime = next;
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            m_targetTime = next;
            m_stepRequested = true;
            PruneQueueLocked(released);
        }
        m_decodeCV.notify_one();
    }

    void VideoSourceProvider::PruneQueueLocked(std::vector<DecodedFrame>& released)
    {
        const double target = m_targetTime.load();
        while (m_queue.size() >= 2 && m_queue[1].time <= target + scTimeTolerance)
        {
            RecycleLocked(m_queue.front());
            released.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
            ++m_droppedFrames;
        }
    }

    void VideoSourceProvider::RecycleLocked(DecodedFrame& frame)
    {
        if (!frame.bytes.empty() && m_spareBuffers.size() < scDecodeAheadFrames + 2)
            m_spareBuffers.push_back(std::move(frame.bytes));
        frame.bytes = {};
    }

    // -----------------------------------------------------------------------
    // Upload (render thread) — GPU copy or CPU upload, then run conversion shader
    // -----------------------------------------------------------------------

    bool VideoSourceProvider::UploadIfReady(ID2D1DeviceContext5* dc)
    {
        m_uploadAttempts++;
        if (!m_reader || !m_d3dContext || !dc) return false;

        // Take the newest queued frame due at the target; older ones are dropped.
        DecodedFrame frame;
        bool haveFrame = false;
        bool restart = false;
        std::vector<DecodedFrame> released;
        {
            std::lock_guard lock(m_stateMutex);
            const double target = m_targetTime.load();
            size_t due = 0;
            while (due < m_queue.size() &&
                   (m_queue[due].seekFrame || m_queue[due].time <= target + scTimeTolerance))
                ++due;
            if (due > 0)
            {
                for (size_t i = 0; i + 1 < due; ++i)
                {
                    RecycleLocked(m_queue[i]);
                    released.push_back(std::move(m_queue[i]));
                    ++m_droppedFrames;
                }
                frame = std::move(m_queue[due - 1]);
                m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<ptrdiff_t>(due));
                m_stepRequested = false;
                haveFrame = true;
            }
            else
            {
                restart = m_queue.empty() && m_endOfStream && m_loop && m_stepRequested;
            }
        }
        released.clear();
        if (restart)
        {
            Seek(0.0);
            return false;
        }
        if (!haveFrame) return false;
        m_decodeCV.notify_one();   // the queue has room again

        bool converted = false;
        if (frame.gpu.tex)
        {
            // Zero-copy: the decoder surface is already on this device. One
            // GPU copy into the planar texture (D3D11 copies both planes of
            // an NV12/P010 subresource together), then convert from it.
            if (!EnsurePlanarTexture(frame.gpu.tex.get()))
            {
                // Unsupported here: use the CPU path from now on, and decode
                // this position again so the frame is not simply lost.
                m_zeroCopyFailed = true;
                frame = {};
                Seek(m_targetTime.load());
                return false;
            }
            converted = RunConversionShader(/*planar*/ true, frame.gpu.tex.get(), frame.gpu.subresource);
            m_lastUploadZeroCopy = true;
        }
        else
        {
            m_lastUploadZeroCopy = false;
            const UINT pitch = static_cast<UINT>(frame.pitch);
            if (m_outputFormat == OutputFormat::P010 || m_outputFormat == OutputFormat::NV12)
            {
                D3D11_BOX yBox = { 0, 0, 0, m_width, m_height, 1 };
                m_d3dContext->UpdateSubresource(m_texY.get(), 0, &yBox, frame.bytes.data(), pitch, 0);
                const BYTE* uvData = frame.bytes.data() + static_cast<ptrdiff_t>(pitch) * m_height;
                D3D11_BOX uvBox = { 0, 0, 0, m_width / 2, m_height / 2, 1 };
                m_d3dContext->UpdateSubresource(m_texUV.get(), 0, &uvBox, uvData, pitch, 0);
            }
            else // RGB32
            {
                D3D11_BOX box = { 0, 0, 0, m_width, m_height, 1 };
                m_d3dContext->UpdateSubresource(m_texRGB.get(), 0, &box, frame.bytes.data(), pitch, 0);
            }
            // The uploads above can stay on the immediate context: each is a
            // single call, and they are ordered before the conversion.
            converted = RunConversionShader();
        }

        {
            std::lock_guard lock(m_stateMutex);
            RecycleLocked(frame);
        }
        // Releasing the sample hands its surface back to the decoder.
        frame.gpu = {};

        if (!converted) return false;
        m_uploadSuccesses++;
        m_uploadedGeneration = frame.generation;
        m_uploadedFrameDuration = frame.duration;
        m_uploadedFrameTime = frame.time;
        m_currentPositionSeconds = frame.time;
        return true;
    }

    // -----------------------------------------------------------------------
    // Background decode thread — raw byte copy only, no color conversion
    // -----------------------------------------------------------------------

    void VideoSourceProvider::DecodeThreadFunc(std::stop_token token)
    {
        for (;;)
        {
            bool seek = false;
            bool resume = false;
            double seekTarget = 0.0;
            double skipThrough = -1.0;
            uint64_t generation = 0;
            DecodedFrame frame;
            {
                std::unique_lock lock(m_stateMutex);
                m_decodeCV.wait(lock, [&] {
                    return token.stop_requested() || m_seekPending || m_resumePending ||
                           (!m_endOfStream && m_queue.size() < scDecodeAheadFrames);
                });
                if (token.stop_requested()) return;
                if (m_seekPending)
                {
                    seek = true;
                    seekTarget = m_seekTarget;
                    m_seekPending = false;
                    m_endOfStream = false;
                }
                else if (m_resumePending)
                {
                    resume = true;
                    skipThrough = m_decodeHead;
                    m_resumePending = false;
                }
                generation = m_generation.load();
                if (!m_spareBuffers.empty())
                {
                    frame.bytes = std::move(m_spareBuffers.back());
                    m_spareBuffers.pop_back();
                }
            }

            bool positioned = true;
            if (seek || resume)
            {
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = static_cast<LONGLONG>((seek ? seekTarget : skipThrough) * 10'000'000.0);
                positioned = SUCCEEDED(m_reader->SetCurrentPosition(GUID_NULL, position));
                PropVariantClear(&position);
            }
            ReadStats stats;
            const DecodeResult result = positioned
                ? DecodeOneFrame(frame, generation, token, stats, skipThrough,
                                 seek ? seekTarget : -std::numeric_limits<double>::infinity())
                : DecodeResult::Failed;
            if (seek && result == DecodeResult::Frame)
                Smooth(m_seekOverheadSeconds, stats.secondsToFirstSample);

            std::vector<DecodedFrame> released;
            std::lock_guard lock(m_stateMutex);
            if (seek && stats.firstSampleTime >= 0.0)
            {
                RecordKeyframeLocked(stats.firstSampleTime, seekTarget);
                if (stats.firstSampleTime <= seekTarget + scTimeTolerance)
                    Smooth(m_meanPrerollSeconds, (std::max)(seekTarget - stats.firstSampleTime, scTimeTolerance));
            }
            const bool current = generation == m_generation.load();
            if (seek && current && result != DecodeResult::Superseded)
                m_seekFrameDecoded = true;
            if (token.stop_requested() || !current || result != DecodeResult::Frame)
            {
                RecycleLocked(frame);
                released.push_back(std::move(frame));
                if (current && result == DecodeResult::Failed && !m_readRetried)
                {
                    // Retry a failed read once: redo the seek, or re-position
                    // at the decode head and skip what was already read.
                    m_readRetried = true;
                    if (seek)
                    {
                        m_seekPending = true;
                        m_seekFrameDecoded = false;
                    }
                    else
                    {
                        m_resumePending = true;
                    }
                }
                else if (current && (result == DecodeResult::EndOfStream || result == DecodeResult::Failed))
                {
                    // Decoding stops until the next seek.
                    m_endOfStream = true;
                }
                continue;
            }
            m_readRetried = false;
            frame.generation = generation;
            frame.seekFrame = seek;
            m_queue.push_back(std::move(frame));
            ++m_decodeCount;
            PruneQueueLocked(released);
        }
    }

    VideoSourceProvider::DecodeResult VideoSourceProvider::DecodeOneFrame(
        DecodedFrame& frame, uint64_t generation, std::stop_token token, ReadStats& stats, double skipThrough,
        double progressFrom)
    {
        if (!m_reader) return DecodeResult::Failed;

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        winrt::com_ptr<IMFSample> sample;
        HRESULT hr = S_OK;
        double sampleSeconds = 0.0;
        double sampleDuration = 0.0;
        int reads = 0;
        const auto readStart = std::chrono::steady_clock::now();
        auto firstSampleAt = readStart;

        // Read until a sample is still showing at the target. Earlier ones are
        // already in the past: after a seek they lead up from the keyframe MF
        // lands on, and in playback they are frames the target has moved past.
        for (;;)
        {
            if (token.stop_requested() || m_generation.load() != generation)
                return DecodeResult::Superseded;
            sample = nullptr;
            hr = m_reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                0, &streamIndex, &flags, &timestamp, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return DecodeResult::Failed;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return DecodeResult::EndOfStream;
            if (!sample) continue;   // a stream tick or format notice
            sampleSeconds = static_cast<double>(timestamp) / 10'000'000.0;
            if (reads++ == 0)
            {
                firstSampleAt = std::chrono::steady_clock::now();
                stats.firstSampleTime = sampleSeconds;
                stats.secondsToFirstSample = std::chrono::duration<double>(firstSampleAt - readStart).count();
            }
            if (sampleSeconds <= skipThrough + scTimeTolerance) continue;

            LONGLONG durationTicks = 0;
            sampleDuration = (SUCCEEDED(sample->GetSampleDuration(&durationTicks)) && durationTicks > 0)
                ? static_cast<double>(durationTicks) / 10'000'000.0 : m_frameDuration;
            {
                std::lock_guard lock(m_stateMutex);
                if (m_generation.load() == generation) m_decodeHead = (std::max)(m_decodeHead, sampleSeconds);
            }
            if (sampleSeconds + sampleDuration > m_targetTime.load() + 1e-6) break;
            if (sampleSeconds >= progressFrom && SecondsSince(readStart) > scCatchUpFrameSeconds) break;
        }

        // Timed from the first sample, so a seek's own latency is not counted.
        if (reads > scMinTimedReads)
            Smooth(m_decodeSecondsPerFrame, SecondsSince(firstSampleAt) / (reads - 1));
        frame.time = sampleSeconds;
        frame.duration = sampleDuration;

        // Lock buffer and copy raw bytes — GPU shader handles all color conversion.
        // With DXVA2 (DXGI device manager), the buffer may be a GPU texture;
        // Lock2D handles the GPU→CPU copy transparently.
        winrt::com_ptr<IMFMediaBuffer> buffer;
        DWORD bufCount = 0;
        sample->GetBufferCount(&bufCount);
        if (bufCount > 0)
            hr = sample->GetBufferByIndex(0, buffer.put());
        else
            hr = sample->ConvertToContiguousBuffer(buffer.put());
        if (FAILED(hr) || !buffer) return DecodeResult::Failed;

        // Hardware decode: queue the decoder texture instead of reading it
        // back (see m_texPlanar).
        if (m_zeroCopyAllowed && !m_zeroCopyFailed && m_outputFormat != OutputFormat::RGB32)
        {
            winrt::com_ptr<IMFDXGIBuffer> dxgiBuffer;
            if (buffer.try_as(dxgiBuffer))
            {
                GpuFrame gpu;
                if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(gpu.tex.put()))) &&
                    SUCCEEDED(dxgiBuffer->GetSubresourceIndex(&gpu.subresource)) && gpu.tex)
                {
                    gpu.sample = sample;
                    frame.gpu = std::move(gpu);
                    return DecodeResult::Frame;
                }
            }
        }
        frame.gpu = {};

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
        DWORD lockedLength = 0;   // bytes behind `data`, when known
        if (locked2D)
        {
            buffer2D->GetContiguousLength(&lockedLength);
        }
        else
        {
            DWORD maxLen = 0;
            hr = buffer->Lock(&data, &maxLen, &lockedLength);
            if (FAILED(hr)) return DecodeResult::Failed;
            pitch = static_cast<LONG>(m_stride);
        }

        if (!m_firstFrameLogged)
        {
            m_firstFrameLogged = true;
            winrt::com_ptr<IMFDXGIBuffer> dxgiBuf;
            buffer.try_as(dxgiBuf);
            const wchar_t* fmtName = (m_outputFormat == OutputFormat::P010) ? L"P010"
                : (m_outputFormat == OutputFormat::NV12) ? L"NV12" : L"RGB32";
            OutputDebugStringW(std::format(
                L"[VideoSource] First frame: format={}, DXVA={}, locked2D={}, pitch={}, stride={}, {}x{}\n",
                fmtName, dxgiBuf != nullptr, locked2D, pitch, m_stride, m_width, m_height).c_str());
        }

        // Raw byte copy — no color conversion. GPU shader handles everything.
        LONG absPitch = (pitch < 0) ? -pitch : pitch;
        const size_t yPlaneSize = static_cast<size_t>(absPitch) * m_height;
        const size_t totalSize = m_outputFormat == OutputFormat::RGB32
            ? yPlaneSize : yPlaneSize + static_cast<size_t>(absPitch) * (m_height / 2);

        if (frame.bytes.size() < totalSize)
            frame.bytes.resize(totalSize);

        // The chroma plane follows the ALLOCATED luma rows, not the frame's:
        // decoders round the surface height up to their block size (1080 is
        // stored as 1088). The allocated height is recoverable from the
        // buffer size: luma rows plus half as many chroma rows, one pitch each.
        uint32_t surfaceRows = m_height;
        if (m_outputFormat != OutputFormat::RGB32 && absPitch > 0 && lockedLength > 0)
        {
            const uint64_t rows = (static_cast<uint64_t>(lockedLength) * 2) /
                                  (static_cast<uint64_t>(absPitch) * 3);
            if (rows >= m_height) surfaceRows = static_cast<uint32_t>(rows);
        }

        if (pitch > 0 && surfaceRows == m_height)
        {
            std::memcpy(frame.bytes.data(), data, totalSize);
        }
        else
        {
            for (uint32_t y = 0; y < m_height; ++y)
            {
                const BYTE* srcRow = data + static_cast<ptrdiff_t>(y) * pitch;
                BYTE* dstRow = frame.bytes.data() + static_cast<size_t>(y) * absPitch;
                std::memcpy(dstRow, srcRow, absPitch);
            }
            if (m_outputFormat != OutputFormat::RGB32)
            {
                const BYTE* uvSrc = data + static_cast<ptrdiff_t>(surfaceRows) * pitch;
                BYTE* uvDst = frame.bytes.data() + yPlaneSize;
                for (uint32_t y = 0; y < m_height / 2; ++y)
                {
                    std::memcpy(uvDst + static_cast<size_t>(y) * absPitch,
                                uvSrc + static_cast<ptrdiff_t>(y) * pitch, absPitch);
                }
            }
        }
        frame.pitch = absPitch;

        if (locked2D) buffer2D->Unlock2D();
        else buffer->Unlock();

        return DecodeResult::Frame;
    }
}
