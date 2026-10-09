#include "pch_engine.h"
#include "CustomComputeBridgeEffect.h"

#include <d3dcompiler.h>

namespace ShaderLab::Effects
{
    // ---- Static pass-through pixel shader -------------------------------
    //
    // Compiled once on the first effect instance's PrepareForRender, and
    // cached for every subsequent instance. The shader is purely
    // decorative -- the bridge's D2D output is never consumed by
    // downstream graph consumers (they wire to `node->cachedOutput`
    // directly, exactly like the rest of the engine). It exists only
    // so the effect satisfies D2D's "one input -> one output" contract.

    GUID CustomComputeBridgeEffect::s_passThroughGuid{};
    bool CustomComputeBridgeEffect::s_passThroughLoaded = false;
    thread_local CustomComputeBridgeEffect* CustomComputeBridgeEffect::s_lastCreated = nullptr;

    namespace
    {
        // Minimal pass-through pixel shader. D2D pixel shaders take the
        // shape of a free function with the correct signature; the host
        // (D2D) sets up the input texture binding via the effect's
        // transform graph.
        constexpr const char* kPassThroughHLSL = R"HLSL(
Texture2D<float4> Source : register(t0);
SamplerState Sampler   : register(s0);

float4 main(
    float4 clipSpaceOutput : SV_POSITION,
    float4 sceneSpaceOutput : SCENE_POSITION,
    float4 texelSpaceInput0 : TEXCOORD0) : SV_Target
{
    return Source.Sample(Sampler, texelSpaceInput0.xy);
}
)HLSL";

        std::vector<BYTE> CompilePassThrough()
        {
            winrt::com_ptr<ID3DBlob> blob, errors;
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
            HRESULT hr = D3DCompile(
                kPassThroughHLSL, std::strlen(kPassThroughHLSL),
                "ComputeBridgePassThrough",
                nullptr, nullptr,
                "main", "ps_5_0",
                flags, 0,
                blob.put(), errors.put());
            if (FAILED(hr) || !blob)
                return {};
            std::vector<BYTE> bytecode(blob->GetBufferSize());
            std::memcpy(bytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());
            return bytecode;
        }
    }

    // ---- Registration ---------------------------------------------------

    HRESULT CustomComputeBridgeEffect::RegisterEffect(ID2D1Factory1* factory)
    {
        if (!factory) return E_INVALIDARG;

        // XML schema. One image input, no D2D properties (the bridge's
        // shader bytecode + cbuffer data is set via ICustomComputeBridge,
        // not through D2D's property system).
        static constexpr PCWSTR xml =
            L"<?xml version='1.0'?>"
            L"<Effect>"
            L"  <Property name='DisplayName' type='string' value='Custom Compute Bridge'/>"
            L"  <Property name='Author'      type='string' value='ShaderLab'/>"
            L"  <Property name='Category'    type='string' value='Compute'/>"
            L"  <Property name='Description' type='string' value='D2D wrapper around D3D11 compute shaders for analysis + image output'/>"
            L"  <Inputs>"
            L"    <Input name='Source'/>"
            L"  </Inputs>"
            L"</Effect>";

        return factory->RegisterEffectFromString(
            CLSID_CustomComputeBridge, xml, nullptr, 0, &CreateFactory);
    }

    HRESULT __stdcall CustomComputeBridgeEffect::CreateFactory(IUnknown** effect)
    {
        if (!effect) return E_POINTER;
        auto* impl = new (std::nothrow) CustomComputeBridgeEffect();
        if (!impl) return E_OUTOFMEMORY;
        s_lastCreated = impl;
        *effect = static_cast<ID2D1EffectImpl*>(impl);
        return S_OK;
    }

    // ---- IUnknown -------------------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        if      (riid == __uuidof(IUnknown))            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1EffectImpl))     *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform))  *ppv = static_cast<ID2D1DrawTransform*>(this);
        else if (riid == __uuidof(ID2D1Transform))      *ppv = static_cast<ID2D1Transform*>(this);
        else if (riid == __uuidof(ID2D1TransformNode))  *ppv = static_cast<ID2D1TransformNode*>(this);
        else if (riid == IID_ICustomComputeBridge)      *ppv = static_cast<ICustomComputeBridge*>(this);
        else if (riid == IID_IEngineComputeOutput)      *ppv = static_cast<IEngineComputeOutput*>(this);
        else if (riid == IID_IEngineComputeTexture)     *ppv = static_cast<IEngineComputeTexture*>(this);
        else return E_NOINTERFACE;

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) CustomComputeBridgeEffect::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) CustomComputeBridgeEffect::Release()
    {
        auto count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    // ---- ID2D1EffectImpl ------------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::PrepareForRender(D2D1_CHANGE_TYPE)
    {
        if (!m_drawInfo) return E_FAIL;
        return EnsurePassThroughShader();
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::SetGraph(ID2D1TransformGraph* transformGraph)
    {
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    HRESULT CustomComputeBridgeEffect::EnsurePassThroughShader()
    {
        // Lazy first-instance compile + load. Subsequent instances skip
        // the compile (D2D effect contexts share their loaded shaders
        // by GUID at the factory level).
        if (!s_passThroughLoaded)
        {
            auto bytecode = CompilePassThrough();
            if (bytecode.empty())
                return E_FAIL;
            ::CoCreateGuid(&s_passThroughGuid);
            HRESULT hr = m_effectContext->LoadPixelShader(
                s_passThroughGuid, bytecode.data(),
                static_cast<UINT32>(bytecode.size()));
            if (FAILED(hr)) return hr;
            s_passThroughLoaded = true;
        }
        return m_drawInfo->SetPixelShader(s_passThroughGuid);
    }

    // ---- ID2D1DrawTransform ---------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::SetDrawInfo(ID2D1DrawInfo* drawInfo)
    {
        m_drawInfo.copy_from(drawInfo);
        return S_OK;
    }

    // ---- ID2D1Transform -------------------------------------------------

    IFACEMETHODIMP CustomComputeBridgeEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        if (inputRectCount == 0 || !inputRects || !outputRect)
            return E_INVALIDARG;
        // Pass-through: output rect = input rect. Don't pass through
        // infinite rects (gotcha #7 in copilot-instructions).
        m_inputRect = inputRects[0];
        if (m_imageOutputW > 0 && m_imageOutputH > 0)
        {
            outputRect->left   = 0;
            outputRect->top    = 0;
            outputRect->right  = static_cast<LONG>(m_imageOutputW);
            outputRect->bottom = static_cast<LONG>(m_imageOutputH);
        }
        else
        {
            *outputRect = m_inputRect;
        }
        if (outputOpaqueSubRect)
            *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        if (inputRectCount == 0 || !outputRect || !inputRects)
            return E_INVALIDARG;
        inputRects[0] = *outputRect;
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeBridgeEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L invalidInputRect,
        D2D1_RECT_L* invalidOutputRect) const
    {
        if (!invalidOutputRect) return E_INVALIDARG;
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    // ---- ICustomComputeBridge -------------------------------------------

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::SetCompiledBytecode(
        const BYTE* bytecode, UINT32 bytecodeSize)
    {
        if (!bytecode || bytecodeSize == 0) return E_INVALIDARG;
        // Content-compare, cheaply: every DXBC blob carries a 16-byte
        // compiler checksum at offset 4. The evaluator calls this before
        // every dispatch with whatever variant the binding plan implies, so
        // it MUST be a no-op when nothing changed -- it used to mark the
        // shader dirty unconditionally, and the evaluator's post-dispatch
        // "restore baseline" made that two CreateComputeShader calls per
        // frame for every GPU-bound consumer.
        const ShaderId id = IdOf(bytecode, bytecodeSize);
        const ShaderId& current = m_bytecodeDirty ? m_pendingId : m_installedId;
        if ((m_bytecodeDirty || m_hasInstalled) && id == current)
            return S_OK;
        m_pendingBytecode.assign(bytecode, bytecode + bytecodeSize);
        m_pendingId = id;
        m_bytecodeDirty = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::Dispatch(
        ID2D1DeviceContext5* dc,
        ID2D1Image* const* inputImages,
        UINT32 inputCount,
        const BYTE* cbufferData,
        UINT32 cbufferSize,
        UINT32 analysisFloat4Count,
        UINT32 imageOutputW,
        UINT32 imageOutputH,
        std::vector<float>* outAnalysisFloats)
    {
        if (!dc || !inputImages || inputCount == 0 || !inputImages[0])
            return E_INVALIDARG;

        // Pre-render each input to its own FP32 D3D11 texture. 96 DPI
        // keeps GetImageLocalBounds in pixels.
        float oldDpiX = 0, oldDpiY = 0;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        if (m_inputBitmaps.size() < inputCount)
            m_inputBitmaps.resize(inputCount);

        // Hold a reference per slot so the textures don't get released
        // while we're building SRVs / driving the dispatch below.
        std::vector<winrt::com_ptr<ID3D11Texture2D>> inputTextures;
        inputTextures.reserve(inputCount);
        UINT32 firstW = 0, firstH = 0;

        for (UINT32 idx = 0; idx < inputCount; ++idx)
        {
            ID2D1Image* inputImage = inputImages[idx];
            if (!inputImage)
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                return E_INVALIDARG;
            }

            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(inputImage, &bounds);
            D2D1_RECT_L px{};
            if (HRESULT snapHr = SnapComputeInputRect(bounds, px); FAILED(snapHr))
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                return snapHr;
            }
            const UINT32 w = static_cast<UINT32>(px.right - px.left);
            const UINT32 h = static_cast<UINT32>(px.bottom - px.top);
            if (idx == 0) { firstW = w; firstH = h; }

            winrt::com_ptr<ID2D1Bitmap1> inputBmp;
            // Fast path: input is already an FP32-RGBA D2D bitmap of the
            // expected size (e.g. produced by GraphEvaluator's
            // PreRenderInputBitmap and shared across multiple deferred-
            // compute consumers). Skip the per-dispatch DrawImage
            // round-trip and grab the DXGI surface directly.
            bool inputIsAlreadyFp32Bitmap = false;
            {
                winrt::com_ptr<ID2D1Bitmap1> asBitmap;
                if (SUCCEEDED(inputImage->QueryInterface(asBitmap.put())) && asBitmap)
                {
                    auto fmt = asBitmap->GetPixelFormat();
                    auto sz = asBitmap->GetPixelSize();
                    if (fmt.format == DXGI_FORMAT_R32G32B32A32_FLOAT &&
                        px.left == 0 && px.top == 0 &&
                        sz.width == w && sz.height == h)
                    {
                        inputBmp = asBitmap;
                        inputIsAlreadyFp32Bitmap = true;
                    }
                }
            }

            if (!inputIsAlreadyFp32Bitmap)
            {
                auto& cache = m_inputBitmaps[idx];
                if (cache.bitmap && cache.width == w && cache.height == h)
                {
                    inputBmp = cache.bitmap;
                }
                else
                {
                    D2D1_BITMAP_PROPERTIES1 bp{};
                    bp.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
                    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
                    bp.dpiX = 96.0f;
                    bp.dpiY = 96.0f;
                    HRESULT hrAlloc = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bp, inputBmp.put());
                    if (FAILED(hrAlloc)) { dc->SetDpi(oldDpiX, oldDpiY); return hrAlloc; }
                    cache.bitmap = inputBmp;
                    cache.width  = w;
                    cache.height = h;
                }

                // Render upstream chain into our cached bitmap. Necessary
                // when inputImage is a D2D effect output (not already an
                // FP32 bitmap) -- the format-convert happens via DrawImage.
                winrt::com_ptr<ID2D1Image> prevTarget;
                dc->GetTarget(prevTarget.put());
                dc->SetTarget(inputBmp.get());
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                dc->DrawImage(inputImage, D2D1::Point2F(
                    -static_cast<float>(px.left), -static_cast<float>(px.top)));
                dc->SetTarget(prevTarget.get());
                dc->Flush();  // D2D batches DrawImage until Flush/EndDraw.
            }

            winrt::com_ptr<IDXGISurface> surface;
            HRESULT hr = inputBmp->GetSurface(surface.put());
            if (FAILED(hr)) { dc->SetDpi(oldDpiX, oldDpiY); return hr; }
            winrt::com_ptr<ID3D11Texture2D> tex;
            hr = surface->QueryInterface(tex.put());
            if (FAILED(hr)) { dc->SetDpi(oldDpiX, oldDpiY); return hr; }
            inputTextures.push_back(std::move(tex));
        }

        dc->SetDpi(oldDpiX, oldDpiY);

        // Lazily initialize the runner with the same D3D11 device as the
        // input texture (matches D2D's underlying device).
        winrt::com_ptr<ID3D11Device> device;
        inputTextures[0]->GetDevice(device.put());
        if (!m_runner.IsInitialized())
            m_runner.Initialize(device.get());

        if (m_bytecodeDirty)
        {
            // Switching between the baseline and a GPU-binding variant (a
            // binding added or removed) reuses a shader created earlier
            // rather than creating it again.
            winrt::com_ptr<ID3D11ComputeShader> shader;
            for (const auto& v : m_shaderVariants)
                if (v.id == m_pendingId) { shader = v.shader; break; }
            if (!shader)
            {
                HRESULT hr = device->CreateComputeShader(
                    m_pendingBytecode.data(),
                    m_pendingBytecode.size(),
                    nullptr, shader.put());
                if (FAILED(hr)) return hr;
                constexpr size_t kMaxVariants = 4;
                if (m_shaderVariants.size() >= kMaxVariants)
                    m_shaderVariants.erase(m_shaderVariants.begin());
                m_shaderVariants.push_back({ m_pendingId, shader });
            }
            m_runner.InstallPrecompiledShader(m_pendingBytecode, shader);
            m_installedId = m_pendingId;
            m_hasInstalled = true;
            m_bytecodeDirty = false;
        }

        if (!m_runner.HasShader())
            return E_NOT_VALID_STATE;

        // Image-output texture (when requested). Created lazily and
        // re-created on size change.
        if (imageOutputW > 0 && imageOutputH > 0)
        {
            HRESULT hr = EnsureImageOutputTexture(device.get(), dc, imageOutputW, imageOutputH);
            if (FAILED(hr)) return hr;
        }
        else if (m_imageOutputTex)
        {
            m_imageOutput = nullptr;
            m_imageOutputTex = nullptr;
            m_imageOutputW = 0;
            m_imageOutputH = 0;
        }

        // Build the cbuffer bytes the runner expects: it prepends
        // Width/Height itself, so cbufferData starts at user-param
        // offset.
        std::vector<BYTE> cbBytes;
        if (cbufferData && cbufferSize > 0)
            cbBytes.assign(cbufferData, cbufferData + cbufferSize);

        // Drive the dispatch through the runner. Phase 8c: when caller
        // passes outAnalysisFloats=nullptr it signals "no CPU consumer
        // this frame" -- skip the runner's CopyResource + Map.
        const bool readbackToCpu = (outAnalysisFloats != nullptr);
        const auto readbackMode = !readbackToCpu
            ? Rendering::D3D11ComputeRunner::Readback::None
            : (m_asyncReadback ? Rendering::D3D11ComputeRunner::Readback::Async
                               : Rendering::D3D11ComputeRunner::Readback::Blocking);

        // Build a raw-pointer array of input textures for the runner.
        std::vector<ID3D11Texture2D*> inputRaw;
        inputRaw.reserve(inputTextures.size());
        for (const auto& t : inputTextures) inputRaw.push_back(t.get());

        // The runner records the whole dispatch on its own deferred context
        // and submits it as a single ExecuteCommandList -- see
        // D3D11ComputeRunner::Initialize. No lock is needed here, and none is
        // taken: compute has no dependency on Direct2D's threading.

        auto floats = m_runner.DispatchWithImageOutput(
            inputRaw,
            cbBytes,
            analysisFloat4Count,
            m_imageOutputTex.get(),
            m_dispatchX, m_dispatchY, m_dispatchZ,
            m_gpuBindingSrvs, m_gpuBindingSlots,
            readbackMode);

        if (outAnalysisFloats)
            *outAnalysisFloats = std::move(floats);

        // Reset per-frame state so the next dispatch starts clean.
        m_asyncReadback = false;
        m_gpuBindingSrvs.clear();
        m_gpuBindingSlots.clear();
        m_dispatchX = m_dispatchY = m_dispatchZ = 1;
        if (FAILED(m_runner.LastDispatchResult()))
            return m_runner.LastDispatchResult();

        m_lastEvaluatedFrame++;
        return S_OK;
    }

    CustomComputeBridgeEffect::ShaderId CustomComputeBridgeEffect::IdOf(
        const BYTE* bytes, size_t size)
    {
        ShaderId id{};
        if (size >= 20 && bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C')
        {
            std::memcpy(id.data(), bytes + 4, id.size());
            return id;
        }
        // Not DXBC (never expected): fall back to a full FNV-1a so two
        // different blobs still compare different.
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
        std::memcpy(id.data(), &h, sizeof(h));
        const uint64_t sz = size;
        std::memcpy(id.data() + 8, &sz, sizeof(sz));
        return id;
    }

    bool CustomComputeBridgeEffect::PollAnalysisReadback(std::vector<float>& out)
    {
        return m_runner.PollReadback(out);
    }

    HRESULT CustomComputeBridgeEffect::SetGpuBinding(
        UINT32 slot, ID3D11ShaderResourceView* srv)
    {
        // Cleared automatically at end of each Dispatch; caller binds
        // fresh per frame from the evaluator's binding-resolution pass.
        m_gpuBindingSrvs.push_back(srv);
        m_gpuBindingSlots.push_back(slot);
        return S_OK;
    }

    HRESULT CustomComputeBridgeEffect::SetDispatchDims(
        UINT32 x, UINT32 y, UINT32 z)
    {
        m_dispatchX = x;
        m_dispatchY = y;
        m_dispatchZ = z;
        return S_OK;
    }

    HRESULT CustomComputeBridgeEffect::EnsureImageOutputTexture(
        ID3D11Device* device,
        ID2D1DeviceContext5* dc,
        UINT32 w, UINT32 h)
    {
        if (!device || !dc) return E_INVALIDARG;
        if (m_imageOutputTex && m_imageOutputW == w && m_imageOutputH == h)
            return S_OK;

        m_imageOutput = nullptr;
        m_imageOutputTex = nullptr;
        m_imageOutputW = w;
        m_imageOutputH = h;

        D3D11_TEXTURE2D_DESC td{};
        td.Width  = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, m_imageOutputTex.put());
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IDXGISurface> surface;
        hr = m_imageOutputTex->QueryInterface(surface.put());
        if (FAILED(hr)) return hr;

        // Wrap as ID2D1Bitmap1. DPI must be 96 -- decision-log gotcha:
        // default 0 DPI causes GetImageLocalBounds to return zeros.
        D2D1_BITMAP_PROPERTIES1 bp{};
        bp.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        bp.dpiX = 96.0f;
        bp.dpiY = 96.0f;
        return dc->CreateBitmapFromDxgiSurface(surface.get(), &bp, m_imageOutput.put());
    }

    // ---- IEngineComputeOutput -------------------------------------------

    // ---- IEngineComputeTexture (lane 3) -----------------------------------
    // Straight delegation: the runner owns the texture, the copy shader and
    // the decision not to create either until asked. Requesting the lane is
    // what makes a compute node pay for it, so the request has to reach the
    // runner rather than being cached here.

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::RequestAnalysisTexture()
    {
        return m_runner.RequestAnalysisTexture();
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::GetAnalysisTexture(
        ID3D11Texture2D** out)
    {
        return m_runner.GetAnalysisTexture(out);
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::GetAnalysisTextureSrv(
        ID3D11ShaderResourceView** out)
    {
        return m_runner.GetAnalysisTextureSrv(out);
    }

    HRESULT STDMETHODCALLTYPE CustomComputeBridgeEffect::GetAnalysisSrv(
        ID3D11ShaderResourceView** out)
    {
        if (!out) return E_POINTER;
        // Delegate to the runner. Returns E_NOT_VALID_STATE before the
        // first successful Dispatch (the runner's SRV is null until the
        // result buffer has been allocated).
        return m_runner.GetAnalysisSrv(out);
    }
}
