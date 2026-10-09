#include "pch_engine.h"
#include "D3D11ComputeRunner.h"
#include "../Effects/Performance.h"
#include "../Effects/ShaderLabParamsHlsl.h"
#include "../Effects/ShaderCompiler.h"

namespace ShaderLab::Rendering
{
    HRESULT __stdcall D3D11ComputeRunner::QueryInterface(REFIID iid, void** out) noexcept
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) ||
            iid == Effects::IID_IEngineComputeOutput)
        {
            // Disambiguate explicitly: the runner now inherits two IUnknown
            // paths, so a plain `this` would be ambiguous and, worse, an
            // implicit pick could hand back the wrong vtable.
            *out = static_cast<Effects::IEngineComputeOutput*>(this);
            // No-op AddRef -- runner lifetime owned by GraphEvaluator's cache.
            return S_OK;
        }
        if (iid == Effects::IID_IEngineComputeTexture)
        {
            *out = static_cast<Effects::IEngineComputeTexture*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT __stdcall D3D11ComputeRunner::GetAnalysisSrv(ID3D11ShaderResourceView** out)
    {
        if (!out) return E_POINTER;
        if (!m_resultSRV) return E_NOT_VALID_STATE;
        *out = m_resultSRV.get();
        (*out)->AddRef();
        return S_OK;
    }

    void D3D11ComputeRunner::Initialize(ID3D11Device* device)
    {
        if (!device) return;
        m_device.copy_from(device);
        m_device->GetImmediateContext(m_immediate.put());

        // Record on a PRIVATE deferred context, submit as one command list.
        //
        // The immediate context is shared with Direct2D -- the render
        // thread's and the UI thread's D2D contexts both draw through it --
        // and its shader / SRV / UAV bindings are context state, not
        // per-caller state. SetMultithreadProtected serializes individual
        // calls, not sequences, so a dispatch issued as the usual five calls
        // (clear, set shader, set SRVs, set UAVs, Dispatch) could have another
        // thread's work land in the middle and run with its bindings gone:
        // ~8% of re-dispatches wrote nothing while returning S_OK. Recording
        // here and submitting with a single ExecuteCommandList makes the
        // whole dispatch one call, which multithread protection DOES make
        // atomic -- with no lock, and no knowledge that D2D exists at all.
        if (FAILED(m_device->CreateDeferredContext(0, m_deferredCtx.put())))
            m_deferredCtx = nullptr;
        m_context = m_deferredCtx ? m_deferredCtx : m_immediate;
        m_deferred = (m_context == m_deferredCtx);
    }

    void D3D11ComputeRunner::SubmitRecorded()
    {
        if (!m_deferred || !m_context || !m_immediate) return;
        winrt::com_ptr<ID3D11CommandList> list;
        // FALSE: the deferred context starts the next recording from default
        // state, which is what we want -- every dispatch binds what it uses.
        if (FAILED(m_context->FinishCommandList(FALSE, list.put())) || !list)
            return;
        // TRUE is NOT optional. With FALSE the runtime resets the immediate
        // context to defaults afterwards, and if this submit lands in the
        // middle of D2D building its own pipeline state, that reset wipes
        // D2D's half-set bindings -- the same race, pointed the other way.
        // TRUE leaves the immediate context exactly as this call found it.
        m_immediate->ExecuteCommandList(list.get(), TRUE);
    }

    bool D3D11ComputeRunner::CompileShader(const std::string& hlslSource)
    {
        m_shader = nullptr;
        m_bytecode.clear();
        m_compileError.clear();

        winrt::com_ptr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(
            hlslSource.c_str(), hlslSource.size(),
            "D3D11ComputeEffect", nullptr, Effects::ShaderLabIncludeHandler(),
            "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, blob.put(), errors.put());

        if (FAILED(hr))
        {
            if (errors)
            {
                std::string msg(static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
                m_compileError = std::wstring(msg.begin(), msg.end());
            }
            else
            {
                m_compileError = L"D3DCompile failed with unknown error.";
            }
            return false;
        }

        hr = m_device->CreateComputeShader(
            blob->GetBufferPointer(), blob->GetBufferSize(),
            nullptr, m_shader.put());
        if (FAILED(hr))
        {
            m_compileError = L"CreateComputeShader failed.";
            return false;
        }

        // Stash bytecode so callers can run D3DReflect for cbuffer layout.
        const auto* src = static_cast<const uint8_t*>(blob->GetBufferPointer());
        m_bytecode.assign(src, src + blob->GetBufferSize());
        ReflectContracts();

        return true;
    }


    // -----------------------------------------------------------------------
    // Lane 3: analysis values as a 1 x N RGBA32F texture
    // -----------------------------------------------------------------------
    //
    // Exists because Direct2D will not let a custom PIXEL shader bind an
    // arbitrary SRV: it maps a transform's INPUTS to Texture2D at t0, t1, ...
    // and nothing else. ID2D1EffectContext::CreateResourceTexture looks like
    // the way round and is not -- it takes `const BYTE*` CPU memory, so it
    // would reintroduce the GPU->CPU readback this lane exists to remove.
    //
    // A buffer cannot be CopyResource'd into a texture (the API will not
    // convert dimensions), so the copy is a one-line compute pass. It is N
    // threads over a handful of texels; the cost is the dispatch, not the work.

    namespace
    {
        // Result[] -> row 0 of a 1 x N RGBA32F texture, texel i = Result[i].
        constexpr const char* kAnalysisCopyHLSL = R"HLSL(
StructuredBuffer<float4>   Src : register(t0);
RWTexture2D<float4>        Dst : register(u0);
cbuffer C : register(b0) { uint Count; uint3 _pad; };
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= Count) return;
    Dst[int2((int)tid.x, 0)] = Src[tid.x];
}
)HLSL";
    }

    HRESULT __stdcall D3D11ComputeRunner::RequestAnalysisTexture()
    {
        if (!m_device) return E_NOT_VALID_STATE;
        m_analysisTexWanted = true;
        return S_OK;
    }

    HRESULT __stdcall D3D11ComputeRunner::GetAnalysisTexture(ID3D11Texture2D** out)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!m_analysisTex || !m_analysisTexFilled) return E_NOT_VALID_STATE;
        *out = m_analysisTex.get();
        (*out)->AddRef();
        return S_OK;
    }

    HRESULT __stdcall D3D11ComputeRunner::GetAnalysisTextureSrv(
        ID3D11ShaderResourceView** out)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!m_analysisTexSRV || !m_analysisTexFilled) return E_NOT_VALID_STATE;
        *out = m_analysisTexSRV.get();
        (*out)->AddRef();
        return S_OK;
    }

    bool D3D11ComputeRunner::EnsureAnalysisTexture()
    {
        if (!m_analysisTexWanted || !m_device || m_resultCount == 0) return false;
        if (m_analysisTex && m_analysisCopyShader) return true;

        m_analysisTex = nullptr;
        m_analysisTexUAV = nullptr;
        m_analysisTexSRV = nullptr;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_resultCount;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        // SHADER_RESOURCE so a consumer can read it; UNORDERED_ACCESS so the
        // copy pass can write it. No CPU access on purpose -- the entire point
        // is that these values never touch the CPU.
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_analysisTex.put())))
        {
            m_analysisTexWanted = false;   // do not retry every frame
            return false;
        }
        if (FAILED(m_device->CreateUnorderedAccessView(
                m_analysisTex.get(), nullptr, m_analysisTexUAV.put())) ||
            FAILED(m_device->CreateShaderResourceView(
                m_analysisTex.get(), nullptr, m_analysisTexSRV.put())))
        {
            m_analysisTex = nullptr;
            m_analysisTexWanted = false;
            return false;
        }

        if (!m_analysisCopyShader)
        {
            winrt::com_ptr<ID3DBlob> blob, errors;
            HRESULT hr = D3DCompile(
                kAnalysisCopyHLSL, std::strlen(kAnalysisCopyHLSL),
                "AnalysisCopy", nullptr, nullptr, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, blob.put(), errors.put());
            if (FAILED(hr) || !blob)
            {
                m_analysisTexWanted = false;
                return false;
            }
            if (FAILED(m_device->CreateComputeShader(
                    blob->GetBufferPointer(), blob->GetBufferSize(),
                    nullptr, m_analysisCopyShader.put())))
            {
                m_analysisTexWanted = false;
                return false;
            }
        }
        return true;
    }

    void D3D11ComputeRunner::PublishAnalysisTexture()
    {
        if (!EnsureAnalysisTexture()) return;
        if (!m_resultSRV || !m_analysisTexUAV || !m_context) return;

        // Its own tiny cbuffer: the main shader's constants have a different
        // layout, and rebinding theirs here would corrupt the next dispatch.
        struct { UINT count; UINT pad[3]; } cb{ m_resultCount, {0, 0, 0} };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(cb);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        winrt::com_ptr<ID3D11Buffer> cbuf;
        D3D11_SUBRESOURCE_DATA init{ &cb, 0, 0 };
        if (FAILED(m_device->CreateBuffer(&bd, &init, cbuf.put()))) return;

        ID3D11ShaderResourceView* srvs[] = { m_resultSRV.get() };
        ID3D11UnorderedAccessView* uavs[] = { m_analysisTexUAV.get() };
        ID3D11Buffer* cbs[] = { cbuf.get() };
        UINT counts[] = { 0 };

        m_context->CSSetShader(m_analysisCopyShader.get(), nullptr, 0);
        m_context->CSSetShaderResources(0, 1, srvs);
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, counts);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch((m_resultCount + 63) / 64, 1, 1);

        // Unbind: leaving the result SRV bound to t0 would conflict with the
        // next dispatch binding the same resource as a UAV, and D3D silently
        // drops one of the two.
        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        ID3D11UnorderedAccessView* nullUav[] = { nullptr };
        m_context->CSSetShaderResources(0, 1, nullSrv);
        m_context->CSSetUnorderedAccessViews(0, 1, nullUav, counts);
        m_context->CSSetShader(nullptr, nullptr, 0);

        m_analysisTexFilled = true;
    }

    void D3D11ComputeRunner::EnsureBuffers(uint32_t resultCount)
    {
        if (m_resultCount == resultCount && m_resultBuffer) return;
        m_resultCount = resultCount;
        // Lane 3 is sized off resultCount, so it has to go with the rest.
        // Keep m_analysisTexWanted: the consumer's request outlives a resize.
        m_analysisTex = nullptr;
        m_analysisTexUAV = nullptr;
        m_analysisTexSRV = nullptr;
        m_analysisTexFilled = false;
        m_resultBuffer = nullptr;
        for (auto& slot : m_staging) { slot.buffer = nullptr; slot.pending = false; }
        m_resultUAV = nullptr;
        m_resultSRV = nullptr;
        m_cbuffer = nullptr;

        uint32_t byteSize = (std::max)(resultCount * 16u, 16u); // 16 bytes per float4

        // Structured buffer for results. Phase 8: BindFlags also include
        // SHADER_RESOURCE so a downstream consumer effect can read the
        // analysis values directly off the GPU through an SRV (the
        // IEngineComputeOutput path) without going through Map().
        D3D11_BUFFER_DESC bufDesc{};
        bufDesc.ByteWidth = byteSize;
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufDesc.StructureByteStride = 16; // sizeof(float4)
        m_device->CreateBuffer(&bufDesc, nullptr, m_resultBuffer.put());

        // UAV.
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = resultCount;
        m_device->CreateUnorderedAccessView(m_resultBuffer.get(), &uavDesc, m_resultUAV.put());

        // SRV — same layout, read-only. Used by IEngineComputeOutput
        // consumers; downstream effects bind this directly to a t-slot
        // and Load() the analysis values without a CPU round-trip.
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = resultCount;
        m_device->CreateShaderResourceView(m_resultBuffer.get(), &srvDesc, m_resultSRV.put());

        // Staging ring for readback.
        bufDesc.Usage = D3D11_USAGE_STAGING;
        bufDesc.BindFlags = 0;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        for (auto& slot : m_staging)
            m_device->CreateBuffer(&bufDesc, nullptr, slot.buffer.put());

        // Constant buffer (256 bytes max — room for Width, Height + user params).
        bufDesc.ByteWidth = 256;
        bufDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufDesc.MiscFlags = 0;
        bufDesc.StructureByteStride = 0;
        m_device->CreateBuffer(&bufDesc, nullptr, m_cbuffer.put());
    }

    void D3D11ComputeRunner::InstallPrecompiledShader(
        const std::vector<uint8_t>& bytecode,
        winrt::com_ptr<ID3D11ComputeShader> shader)
    {
        // Install pre-compiled bytecode + the live shader handle. Used
        // by CustomComputeBridgeEffect to bypass the runtime D3DCompile
        // path when the bytecode is already produced by the host (e.g.
        // an MCP /effect/compile call). Subsequent Dispatch calls use
        // the installed shader directly.
        m_bytecode = bytecode;
        m_shader = std::move(shader);
        m_compileError.clear();
        ReflectContracts();
    }

    void D3D11ComputeRunner::ReflectContracts()
    {
        m_usesScratch = false;
        m_usesImagePass = false;
        m_accumulatorStride = 0;
        m_accumulatorNeedsClear = true;
        m_groupSize[0] = m_groupSize[1] = m_groupSize[2] = 1;
        winrt::com_ptr<ID3D11ShaderReflection> reflect;
        if (m_bytecode.empty() ||
            FAILED(D3DReflect(m_bytecode.data(), m_bytecode.size(),
                IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflect.put()))) || !reflect)
            return;

        reflect->GetThreadGroupSize(&m_groupSize[0], &m_groupSize[1], &m_groupSize[2]);

        // Multi-group reduction contract.
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (SUCCEEDED(reflect->GetResourceBindingDescByName("_SLScratch", &bd)))
            m_usesScratch = (bd.BindPoint == 2);

        // Two-pass image contract. For a structured buffer, NumSamples is the stride.
        D3D11_SHADER_INPUT_BIND_DESC passBind{};
        D3D11_SHADER_INPUT_BIND_DESC accumulatorBind{};
        if (SUCCEEDED(reflect->GetResourceBindingDescByName("_SLPassConstants", &passBind)) &&
            passBind.Type == D3D_SIT_CBUFFER && passBind.BindPoint == Effects::cImagePassConstantsSlot &&
            SUCCEEDED(reflect->GetResourceBindingDescByName("_SLPixelAccum", &accumulatorBind)) &&
            accumulatorBind.Type == D3D_SIT_UAV_RWSTRUCTURED &&
            accumulatorBind.BindPoint == Effects::cImagePassAccumulatorSlot &&
            accumulatorBind.NumSamples > 0)
        {
            m_usesImagePass = true;
            m_accumulatorStride = accumulatorBind.NumSamples;
        }
    }

    HRESULT D3D11ComputeRunner::EnsureImagePassResources(uint64_t pixelCount)
    {
        if (!m_device) return E_NOT_VALID_STATE;
        if (!m_passConstants[0])
        {
            for (UINT pass = 0; pass < 2; ++pass)
            {
                const UINT data[4] = { pass, 0, 0, 0 };
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = sizeof(data);
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                D3D11_SUBRESOURCE_DATA init{ data, 0, 0 };
                const HRESULT hr = m_device->CreateBuffer(&desc, &init, m_passConstants[pass].put());
                if (FAILED(hr))
                {
                    m_passConstants[0] = nullptr;
                    m_passConstants[1] = nullptr;
                    return hr;
                }
            }
        }

        const uint64_t bytes = pixelCount * m_accumulatorStride;
        if (m_accumulatorUAV && m_accumulatorBytes == bytes) return S_OK;
        m_accumulator = nullptr;
        m_accumulatorUAV = nullptr;
        m_accumulatorBytes = 0;
        if (pixelCount == 0 || bytes > UINT32_MAX) return E_OUTOFMEMORY;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(bytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = m_accumulatorStride;
        HRESULT hr = m_device->CreateBuffer(&desc, nullptr, m_accumulator.put());
        if (FAILED(hr)) return hr;

        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = static_cast<UINT>(pixelCount);
        hr = m_device->CreateUnorderedAccessView(m_accumulator.get(), &view, m_accumulatorUAV.put());
        if (FAILED(hr))
        {
            m_accumulator = nullptr;
            return hr;
        }
        m_accumulatorBytes = bytes;
        m_accumulatorNeedsClear = true;
        return S_OK;
    }

    bool D3D11ComputeRunner::EnsureScratch()
    {
        if (m_scratchUAV) return true;
        if (!m_device) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = Effects::kReduceScratchUints * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, m_scratch.put()))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = Effects::kReduceScratchUints;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(m_device->CreateUnorderedAccessView(m_scratch.get(), &ud, m_scratchUAV.put())))
            return false;
        ud.Buffer.NumElements = Effects::kReduceScratchClearedUints;
        if (FAILED(m_device->CreateUnorderedAccessView(m_scratch.get(), &ud, m_scratchHeadUAV.put())))
        {
            m_scratchUAV = nullptr;
            return false;
        }
        return true;
    }

    bool D3D11ComputeRunner::PollReadback(std::vector<float>& out)
    {
        if (!m_immediate || m_resultCount == 0) return false;
        // Newest completed copy wins; walk newest-first so the first success
        // ends the search, then retire everything older.
        std::array<StagingSlot*, kStagingSlots> order{};
        size_t n = 0;
        for (auto& s : m_staging) if (s.pending) order[n++] = &s;
        std::sort(order.begin(), order.begin() + n,
            [](const StagingSlot* a, const StagingSlot* b) { return a->seq > b->seq; });
        for (size_t i = 0; i < n; ++i)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = m_immediate->Map(order[i]->buffer.get(), 0, D3D11_MAP_READ,
                                          D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (SUCCEEDED(hr))
            {
                const float* data = static_cast<const float*>(mapped.pData);
                out.assign(data, data + m_resultCount * 4);
                m_immediate->Unmap(order[i]->buffer.get(), 0);
            }
            // Completed (or failed for good): this slot and every older one
            // are done -- older values are superseded by this one.
            const uint64_t seq = order[i]->seq;
            for (auto& s : m_staging)
                if (s.pending && s.seq <= seq) s.pending = false;
            return SUCCEEDED(hr);
        }
        return false;
    }

    std::vector<float> D3D11ComputeRunner::Dispatch(
        ID3D11Texture2D* inputTexture,
        const std::vector<BYTE>& cbufferData,
        uint32_t resultCount)
    {
        std::vector<ID3D11Texture2D*> inputs{ inputTexture };
        return DispatchWithImageOutput(inputs, cbufferData, resultCount, nullptr);
    }

    std::vector<float> D3D11ComputeRunner::DispatchWithImageOutput(
        const std::vector<ID3D11Texture2D*>& inputTextures,
        const std::vector<BYTE>& cbufferData,
        uint32_t resultCount,
        ID3D11Texture2D* imageOutputTexture,
        uint32_t dispatchX, uint32_t dispatchY, uint32_t dispatchZ,
        const std::vector<ID3D11ShaderResourceView*>& extraSrvs,
        const std::vector<uint32_t>& extraSrvSlots,
        Readback readbackMode)
    {
        std::vector<float> result;
        m_lastDispatchResult = E_FAIL;
        // Record on the private deferred context unless the A/B switch says
        // otherwise (see Performance::IsComputeCommandListEnabled).
        m_context = (m_deferredCtx && Performance::IsComputeCommandListEnabled())
            ? m_deferredCtx : m_immediate;
        m_deferred = (m_context == m_deferredCtx) && m_deferredCtx;
        if (!m_shader || !m_context || inputTextures.empty() || !inputTextures[0])
        {
            m_lastDispatchResult = E_NOT_VALID_STATE;
            return result;
        }

        // Auto-inject Width / Height come from input #0 (t0). Multi-input
        // shaders are expected to operate on inputs of the same size --
        // upstream is responsible for matching dimensions before binding.
        D3D11_TEXTURE2D_DESC texDesc{};
        inputTextures[0]->GetDesc(&texDesc);

        // Ensure buffers are the right size. Use 1 as the floor so
        // analysis-only shaders that emit no analysis output (rare,
        // but possible in the image-producing case) still get a valid
        // SRV bound.
        uint32_t bufferSlots = (resultCount > 0) ? resultCount : 1;
        EnsureBuffers(bufferSlots);
        if (!m_resultBuffer || !m_resultUAV || !m_cbuffer) { m_lastDispatchResult = E_OUTOFMEMORY; return result; }

        // Create one SRV per input texture, bound at t0..t(N-1) in order.
        std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> inputSrvs;
        inputSrvs.reserve(inputTextures.size());
        std::vector<ID3D11ShaderResourceView*> inputSrvPtrs;
        inputSrvPtrs.reserve(inputTextures.size());
        for (auto* tex : inputTextures)
        {
            if (!tex) { m_lastDispatchResult = E_INVALIDARG; return result; }
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = d.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            HRESULT hr = m_device->CreateShaderResourceView(tex, &srvDesc, srv.put());
            if (FAILED(hr)) { m_lastDispatchResult = hr; return result; }
            inputSrvPtrs.push_back(srv.get());
            inputSrvs.push_back(std::move(srv));
        }

        // Image-output UAV (optional).
        winrt::com_ptr<ID3D11UnorderedAccessView> imageUAV;
        D3D11_TEXTURE2D_DESC outDesc{};
        if (imageOutputTexture)
        {
            imageOutputTexture->GetDesc(&outDesc);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = outDesc.Format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            HRESULT hr = m_device->CreateUnorderedAccessView(imageOutputTexture, &uavDesc, imageUAV.put());
            if (FAILED(hr)) { m_lastDispatchResult = hr; return result; }
            // Clear the image-output UAV so previous-frame contents
            // don't leak through when the shader writes selectively.
            float clearF[4] = { 0, 0, 0, 0 };
            m_context->ClearUnorderedAccessViewFloat(imageUAV.get(), clearF);
        }

        // The two-pass image contract needs an image to write and its accumulator.
        if (m_usesImagePass)
        {
            if (!imageUAV) { m_lastDispatchResult = E_INVALIDARG; return result; }
            const HRESULT hr = EnsureImagePassResources(uint64_t{ outDesc.Width } * outDesc.Height);
            if (FAILED(hr) || !EnsureScratch())
            {
                m_lastDispatchResult = FAILED(hr) ? hr : E_OUTOFMEMORY;
                return result;
            }
        }

        // Pack cbuffer: Width, Height (8 bytes) + user data.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_context->Map(m_cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            memset(mapped.pData, 0, 256);
            auto* cb = static_cast<BYTE*>(mapped.pData);
            // Auto-inject Width, Height (from input #0).
            uint32_t dims[2] = { texDesc.Width, texDesc.Height };
            memcpy(cb, dims, 8);
            // Copy user params starting at offset 8.
            if (!cbufferData.empty())
            {
                uint32_t userSize = (std::min)(static_cast<uint32_t>(cbufferData.size()), 248u);
                memcpy(cb + 8, cbufferData.data(), userSize);
            }
            m_context->Unmap(m_cbuffer.get(), 0);
        }

        // Clear analysis result buffer.
        uint32_t clearValues[4] = { 0, 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_resultUAV.get(), clearValues);

        // Dispatch.
        m_context->CSSetShader(m_shader.get(), nullptr, 0);
        m_context->CSSetShaderResources(0, static_cast<UINT>(inputSrvPtrs.size()), inputSrvPtrs.data());

        // Phase 8: GPU-bound parameters arrive as extra SRVs. Each
        // (slot, srv) pair binds the upstream IEngineComputeOutput's
        // structured-buffer SRV at the consumer's t-slot. Bind
        // individually so non-contiguous slots (e.g. t1 and t3)
        // work without a contiguous-array constraint.
        size_t maxExtra = (std::min)(extraSrvs.size(), extraSrvSlots.size());
        for (size_t i = 0; i < maxExtra; ++i)
        {
            ID3D11ShaderResourceView* one[1] = { extraSrvs[i] };
            m_context->CSSetShaderResources(extraSrvSlots[i], 1, one);
        }

        // Multi-group reduction: scratch at u2, head zeroed, and a
        // kReduceGroups-wide dispatch unless the caller sized it.
        const bool scratch = m_usesScratch && EnsureScratch();
        if (scratch)
        {
            const UINT zero[4] = { 0, 0, 0, 0 };
            m_context->ClearUnorderedAccessViewUint(m_scratchHeadUAV.get(), zero);
            if (!m_usesImagePass && dispatchX <= 1 && dispatchY <= 1 && dispatchZ <= 1)
                dispatchX = Effects::kReduceGroups;
        }

        ID3D11UnorderedAccessView* uavs[4] = { m_resultUAV.get(), imageUAV.get(),
                                               scratch ? m_scratchUAV.get() : nullptr,
                                               m_usesImagePass ? m_accumulatorUAV.get() : nullptr };
        UINT uavCount = m_usesImagePass ? 4u : (scratch ? 3u : (imageUAV ? 2u : 1u));
        m_context->CSSetUnorderedAccessViews(0, uavCount, uavs, nullptr);
        ID3D11Buffer* cbs[] = { m_cbuffer.get() };
        m_context->CSSetConstantBuffers(0, 1, cbs);

        if (m_usesImagePass)
        {
            // Pass 0 over input 0, pass 1 over the output. D3D11 orders the
            // two dispatches, so pass 1 sees every pass-0 atomic. Pass 1
            // leaves the accumulator zeroed, so it is cleared here only when
            // nothing has run on it yet.
            if (m_accumulatorNeedsClear)
            {
                const UINT zero[4] = { 0, 0, 0, 0 };
                m_context->ClearUnorderedAccessViewUint(m_accumulatorUAV.get(), zero);
                m_accumulatorNeedsClear = false;
            }
            ID3D11Buffer* passZero[] = { m_passConstants[0].get() };
            m_context->CSSetConstantBuffers(Effects::cImagePassConstantsSlot, 1, passZero);
            m_context->Dispatch(
                (texDesc.Width + m_groupSize[0] - 1) / m_groupSize[0],
                (texDesc.Height + m_groupSize[1] - 1) / m_groupSize[1], 1);
            ID3D11Buffer* passOne[] = { m_passConstants[1].get() };
            m_context->CSSetConstantBuffers(Effects::cImagePassConstantsSlot, 1, passOne);
            m_context->Dispatch(
                (outDesc.Width + m_groupSize[0] - 1) / m_groupSize[0],
                (outDesc.Height + m_groupSize[1] - 1) / m_groupSize[1], 1);
            ID3D11Buffer* noPass[] = { nullptr };
            m_context->CSSetConstantBuffers(Effects::cImagePassConstantsSlot, 1, noPass);
        }
        else
        {
            m_context->Dispatch(
                (std::max)(dispatchX, 1u),
                (std::max)(dispatchY, 1u),
                (std::max)(dispatchZ, 1u));
        }

        // Clear shader state.
        std::vector<ID3D11ShaderResourceView*> nullSrvs(inputSrvPtrs.size(), nullptr);
        m_context->CSSetShaderResources(0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());
        for (size_t i = 0; i < maxExtra; ++i)
        {
            ID3D11ShaderResourceView* none[1] = { nullptr };
            m_context->CSSetShaderResources(extraSrvSlots[i], 1, none);
        }
        ID3D11UnorderedAccessView* nullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
        m_context->CSSetUnorderedAccessViews(0, uavCount, nullUAVs, nullptr);
        m_context->CSSetShader(nullptr, nullptr, 0);

        // Lane 3: publish Result[] into the 1 x N texture, but ONLY if some
        // consumer asked for it. This is the demand-driven half of the
        // three-lane contract -- a graph with no pixel-shader consumer never
        // creates the texture, never compiles the copy shader and never
        // dispatches it. Runs after the main dispatch's UAVs are unbound,
        // because the copy reads the result buffer as an SRV and D3D will
        // silently drop one of the two views if both are bound at once.
        if (m_analysisTexWanted && resultCount > 0)
            PublishAnalysisTexture();

        // Readback (only if caller asked for analysis values; image
        // output stays GPU-resident). Phase 8c: also gated by
        // `readbackToCpu` so the host can keep the result GPU-only when
        // no CPU consumer needs the values this frame -- the SRV at
        // `m_resultSRV` is unaffected and downstream GPU bindings still
        // see the buffer's freshly-written contents.
        const bool readback = (resultCount > 0 && readbackMode != Readback::None);
        StagingSlot* slot = nullptr;
        if (readback)
        {
            // Prefer a free slot; with all three in flight, reuse the oldest
            // (its values are superseded by this copy anyway).
            for (auto& s : m_staging)
                if (s.buffer && !s.pending) { slot = &s; break; }
            if (!slot)
            {
                for (auto& s : m_staging)
                    if (s.buffer && (!slot || s.seq < slot->seq)) slot = &s;
            }
            if (slot)
                m_context->CopyResource(slot->buffer.get(), m_resultBuffer.get());
        }

        // Everything above -- clears, bindings, dispatch, unbind, the lane-3
        // publish and the staging copy -- goes to the GPU as ONE call.
        SubmitRecorded();

        // A deferred context cannot Map for READ, so the readback happens on
        // the immediate context, after the list that fills the staging
        // buffer. One call, touching a buffer nothing else binds.
        if (slot && readbackMode == Readback::Blocking)
        {
            hr = m_immediate->Map(slot->buffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr))
            {
                const float* data = static_cast<const float*>(mapped.pData);
                result.assign(data, data + resultCount * 4);
                m_immediate->Unmap(slot->buffer.get(), 0);
            }
            // Anything still in flight is older than what was just read.
            for (auto& s : m_staging) s.pending = false;
        }
        else if (slot)
        {
            slot->pending = true;
            slot->seq = ++m_stagingSeq;
        }

        // Phase 8: bump the dispatch counter so downstream
        // IEngineComputeOutput consumers can detect freshness.
        ++m_lastEvaluatedFrame;
        m_lastDispatchResult = S_OK;

        return result;
    }
}
