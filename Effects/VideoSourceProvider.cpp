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

        if (filePath.empty())
        {
            m_lastError = L"Empty file path";
            return false;
        }

        EnsureMFInitialized();
        m_mfInitialized = true;

        HRESULT hr;

        // Create DXGI Device Manager for hardware-accelerated decode.
        // This enables DXVA2 — the GPU decodes HEVC/H.264 directly.
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

        winrt::com_ptr<IMFAttributes> attrs;
        hr = MFCreateAttributes(attrs.put(), 3);
        if (FAILED(hr))
        {
            m_lastError = std::format(L"MFCreateAttributes failed: 0x{:08X}", static_cast<uint32_t>(hr));
            return false;
        }

        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (m_dxgiDeviceManager)
        {
            attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_dxgiDeviceManager.get());
        }

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

        // Choose output format: HDR → P010, SDR → NV12.
        // Both are YUV planar — GPU shader handles all color conversion.
        winrt::com_ptr<IMFMediaType> outputType;
        MFCreateMediaType(outputType.put());
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        if (m_isHDR)
        {
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_P010);
            MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
            m_stride = m_width * 2;
            hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
            if (SUCCEEDED(hr))
            {
                m_outputFormat = OutputFormat::P010;
            }
            else
            {
                // Fall back to NV12.
                outputType = nullptr;
                MFCreateMediaType(outputType.put());
                outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
                m_stride = m_width;
                hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.get());
                if (SUCCEEDED(hr)) m_outputFormat = OutputFormat::NV12;
            }
        }
        else
        {
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
                // Fall back to RGB32.
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

        // Allocate raw byte double buffers.
        {
            size_t ySize, uvSize = 0;
            if (m_outputFormat == OutputFormat::P010)
            {
                ySize = static_cast<size_t>(m_stride) * m_height;
                uvSize = static_cast<size_t>(m_stride) * (m_height / 2);
            }
            else if (m_outputFormat == OutputFormat::NV12)
            {
                ySize = static_cast<size_t>(m_stride) * m_height;
                uvSize = static_cast<size_t>(m_stride) * (m_height / 2);
            }
            else
            {
                ySize = static_cast<size_t>(m_stride) * m_height;
            }
            size_t totalSize = ySize + uvSize;
            m_backBuffer.resize(totalSize);
            m_frontBuffer.resize(totalSize);
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

        // Decode first frame synchronously.
        m_currentPositionSeconds = 0.0;
        m_endOfStream = false;
        if (DecodeOneFrame())
        {
            std::lock_guard lock(m_bufferMutex);
            std::swap(m_frontBuffer, m_backBuffer);
            m_lastPitch = m_stride;
            m_frameReady = true;
        }

        // Start background decode thread.
        m_decodeThread = std::jthread([this](std::stop_token) { DecodeThreadFunc(); });

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
            m_decodeThread.request_stop();
            m_decodeCV.notify_all();
            m_decodeThread.join();
        }

        m_reader = nullptr;
        m_dxgiDeviceManager = nullptr;
        m_bitmap = nullptr;
        m_csP010 = nullptr; m_csNV12 = nullptr; m_csRGB32 = nullptr;
        m_texY = nullptr; m_texUV = nullptr; m_texRGB = nullptr;
        m_texOutput = nullptr;
        m_srvY = nullptr; m_srvUV = nullptr; m_srvRGB = nullptr;
        m_uavOutput = nullptr; m_cbParams = nullptr;
        m_d3dDevice = nullptr; m_d3dContext = nullptr;
        m_width = 0; m_height = 0; m_stride = 0;
        m_frameRate = 0.0; m_durationSeconds = 0.0;
        m_currentPositionSeconds = 0.0; m_frameDuration = 0.0;
        m_playing = false; m_endOfStream = false;
        m_accumulatedTime = 0.0;
        m_frameReady = false; m_frameNeeded = false;
        m_firstFrameLogged = false;

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

    void VideoSourceProvider::RunConversionShader(ID3D11DeviceContext* ctx)
    {
        // Set shader.
        ID3D11ComputeShader* cs = nullptr;
        if (m_outputFormat == OutputFormat::P010) cs = m_csP010.get();
        else if (m_outputFormat == OutputFormat::NV12) cs = m_csNV12.get();
        else cs = m_csRGB32.get();
        if (!cs) return;

        ctx->CSSetShader(cs, nullptr, 0);

        // Bind resources.
        ID3D11Buffer* cbs[] = { m_cbParams.get() };
        ctx->CSSetConstantBuffers(0, 1, cbs);

        if (m_outputFormat == OutputFormat::RGB32)
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvRGB.get() };
            ctx->CSSetShaderResources(0, 1, srvs);
        }
        else
        {
            ID3D11ShaderResourceView* srvs[] = { m_srvY.get(), m_srvUV.get() };
            ctx->CSSetShaderResources(0, 2, srvs);
        }

        ID3D11UnorderedAccessView* uavs[] = { m_uavOutput.get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // Dispatch: 16x16 thread groups.
        UINT gx = (m_width + 15) / 16;
        UINT gy = (m_height + 15) / 16;
        ctx->Dispatch(gx, gy, 1);

        // Unbind.
        ID3D11ShaderResourceView* nullSRVs[2] = {};
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        ctx->CSSetShaderResources(0, 2, nullSRVs);
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);
    }

    // -----------------------------------------------------------------------
    // Playback controls
    // -----------------------------------------------------------------------

    void VideoSourceProvider::Play()
    {
        if (!m_reader) return;
        if (m_endOfStream && m_loop) Seek(0.0);
        m_playing = true;
        m_frameNeeded = true;
        m_decodeCV.notify_one();
    }

    void VideoSourceProvider::Pause() { m_playing = false; }

    void VideoSourceProvider::Seek(double seconds)
    {
        if (!m_reader) return;
        seconds = (std::clamp)(seconds, 0.0, m_durationSeconds);
        {
            std::lock_guard lock(m_decodeMutex);
            m_seekTarget = seconds;
            m_seekPending = true;
        }
        m_endOfStream = false;
        m_accumulatedTime = 0.0;
        m_currentPositionSeconds = seconds;
        m_frameNeeded = true;  // Force decode at the new position.
        m_decodeCV.notify_one();
    }

    void VideoSourceProvider::Tick(double deltaSeconds)
    {
        if (!m_reader || !m_playing) return;

        // Handle end-of-stream looping.
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
            }
            return;
        }

        m_accumulatedTime += deltaSeconds * m_speed;
        if (m_accumulatedTime >= m_frameDuration)
        {
            m_accumulatedTime -= m_frameDuration;
            if (m_accumulatedTime > m_frameDuration * 2)
                m_accumulatedTime = 0.0;
            m_frameNeeded = true;
            m_decodeCV.notify_one();
        }
    }

    void VideoSourceProvider::RequestNextFrame()
    {
        if (!m_reader) return;
        if (m_endOfStream)
        {
            if (m_loop) { Seek(0.0); m_playing = true; }
            return;
        }
        if (!m_frameNeeded.exchange(true))
            m_decodeCV.notify_one();
    }

    // -----------------------------------------------------------------------
    // Upload (UI thread) — GPU copy or CPU upload, then run conversion shader
    // -----------------------------------------------------------------------

    bool VideoSourceProvider::UploadIfReady(ID2D1DeviceContext5* dc)
    {
        m_uploadAttempts++;
        if (!m_frameReady || !m_d3dContext || !dc) return false;
        m_uploadSuccesses++;

        // Upload raw bytes from the front buffer to GPU textures.
        std::vector<BYTE> uploadBuf;
        LONG pitch;
        {
            std::lock_guard lock(m_bufferMutex);
            uploadBuf.swap(m_frontBuffer);
            pitch = m_lastPitch;
            m_frameReady = false;
        }

        if (m_outputFormat == OutputFormat::P010)
        {
            D3D11_BOX yBox = { 0, 0, 0, m_width, m_height, 1 };
            m_d3dContext->UpdateSubresource(m_texY.get(), 0, &yBox, uploadBuf.data(),
                static_cast<UINT>(pitch), 0);

            const BYTE* uvData = uploadBuf.data() + static_cast<ptrdiff_t>(pitch) * m_height;
            D3D11_BOX uvBox = { 0, 0, 0, m_width / 2, m_height / 2, 1 };
            m_d3dContext->UpdateSubresource(m_texUV.get(), 0, &uvBox, uvData,
                static_cast<UINT>(pitch), 0);
        }
        else if (m_outputFormat == OutputFormat::NV12)
        {
            D3D11_BOX yBox = { 0, 0, 0, m_width, m_height, 1 };
            m_d3dContext->UpdateSubresource(m_texY.get(), 0, &yBox, uploadBuf.data(),
                static_cast<UINT>(pitch), 0);

            const BYTE* uvData = uploadBuf.data() + static_cast<ptrdiff_t>(pitch) * m_height;
            D3D11_BOX uvBox = { 0, 0, 0, m_width / 2, m_height / 2, 1 };
            m_d3dContext->UpdateSubresource(m_texUV.get(), 0, &uvBox, uvData,
                static_cast<UINT>(pitch), 0);
        }
        else // RGB32
        {
            D3D11_BOX box = { 0, 0, 0, m_width, m_height, 1 };
            m_d3dContext->UpdateSubresource(m_texRGB.get(), 0, &box, uploadBuf.data(),
                static_cast<UINT>(pitch), 0);
        }

        RunConversionShader(m_d3dContext);

        // Return buffer for reuse.
        {
            std::lock_guard lock(m_bufferMutex);
            m_frontBuffer.swap(uploadBuf);
        }

        return true;
    }

    // -----------------------------------------------------------------------
    // Background decode thread — raw byte copy only, no color conversion
    // -----------------------------------------------------------------------

    void VideoSourceProvider::DecodeThreadFunc()
    {
        auto token = m_decodeThread.get_stop_token();
        while (!token.stop_requested())
        {
            {
                std::unique_lock lock(m_decodeMutex);
                m_decodeCV.wait(lock, [&] {
                    return m_frameNeeded.load() || m_seekPending.load() || token.stop_requested();
                });
            }
            if (token.stop_requested()) break;

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

            if (m_frameNeeded)
            {
                m_frameNeeded = false;
                if (DecodeOneFrame())
                {
                    m_decodeCount++;
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

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { m_endOfStream = true; return false; }
        if (!sample) return false;

        m_currentPositionSeconds = static_cast<double>(timestamp) / 10'000'000.0;

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
        if (FAILED(hr) || !buffer) return false;

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
        size_t yPlaneSize, totalSize;
        if (m_outputFormat == OutputFormat::P010)
        {
            yPlaneSize = static_cast<size_t>(absPitch) * m_height;
            totalSize = yPlaneSize + static_cast<size_t>(absPitch) * (m_height / 2);
        }
        else if (m_outputFormat == OutputFormat::NV12)
        {
            yPlaneSize = static_cast<size_t>(absPitch) * m_height;
            totalSize = yPlaneSize + static_cast<size_t>(absPitch) * (m_height / 2);
        }
        else
        {
            yPlaneSize = static_cast<size_t>(absPitch) * m_height;
            totalSize = yPlaneSize;
        }

        if (m_backBuffer.size() < totalSize)
            m_backBuffer.resize(totalSize);

        if (pitch > 0)
        {
            std::memcpy(m_backBuffer.data(), data, totalSize);
        }
        else
        {
            for (uint32_t y = 0; y < m_height; ++y)
            {
                const BYTE* srcRow = data + y * pitch;
                BYTE* dstRow = m_backBuffer.data() + y * absPitch;
                std::memcpy(dstRow, srcRow, absPitch);
            }
            if (m_outputFormat != OutputFormat::RGB32)
            {
                const BYTE* uvSrc = data + static_cast<ptrdiff_t>(m_height) * pitch;
                BYTE* uvDst = m_backBuffer.data() + yPlaneSize;
                for (uint32_t y = 0; y < m_height / 2; ++y)
                {
                    std::memcpy(uvDst + y * absPitch, uvSrc + y * pitch, absPitch);
                }
            }
        }
        m_lastPitch = absPitch;

        if (locked2D) buffer2D->Unlock2D();
        else buffer->Unlock();

        return true;
    }
}

