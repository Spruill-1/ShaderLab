#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Effects/IEngineComputeOutput.h"
#include "../Effects/IEngineComputeTexture.h"

namespace ShaderLab::Rendering
{
    // Generic D3D11 compute shader dispatch runner for user-authored shaders.
    // Handles: shader compilation, SRV/UAV/CB creation, dispatch, and readback.
    //
    // Output uses RWStructuredBuffer<float4> matching the analysisFields ABI:
    //   - One float4 per analysis pixel (field.pixelCount())
    //   - Float → .x, Float2 → .xy, Float3 → .xyz, Float4 → all
    //   - Compatible with existing ReadCustomAnalysisOutput readback
    //
    // cbuffer contract: first 8 bytes = uint Width, uint Height (auto-injected).
    // User parameters start at offset 8.
    //
    // Phase 8: implements IEngineComputeOutput so downstream effects can
    // bind the result SRV directly into a SHADERLAB_GPU_BUFFER slot,
    // avoiding the Map() round-trip. The COM impl is no-op-refcounted
    // -- the runner's lifetime is owned by the GraphEvaluator's
    // unique_ptr cache, callers are not allowed to AddRef beyond that.
    class SHADERLAB_API D3D11ComputeRunner
        : public Effects::IEngineComputeOutput
        , public Effects::IEngineComputeTexture
    {
    public:
        D3D11ComputeRunner() = default;
        virtual ~D3D11ComputeRunner() = default;

        // Initialize with a D3D11 device (caches device + immediate context).
        void Initialize(ID3D11Device* device);

        // Compile a compute shader from HLSL source.
        // Returns true on success. Call GetCompileError() on failure.
        // On success, GetCompiledBytecode() returns the bytecode blob —
        // the caller should store it on the CustomEffectDefinition so
        // downstream cbuffer reflection (in DispatchUserD3D11Compute)
        // can pack user properties at the correct offsets.
        bool CompileShader(const std::string& hlslSource);

        // Install pre-compiled shader bytecode (Phase 8 bridge effect
        // path). Bypasses D3DCompile -- used when the host already
        // has the bytecode (e.g. after an MCP /effect/compile call).
        // The runner stashes the bytecode blob and the live
        // ID3D11ComputeShader; subsequent Dispatch calls use it like
        // they would the CompileShader output.
        void InstallPrecompiledShader(
            const std::vector<uint8_t>& bytecode,
            winrt::com_ptr<ID3D11ComputeShader> shader);

        // Get the last compile error message.
        const std::wstring& GetCompileError() const { return m_compileError; }

        // Get the compiled bytecode (empty until CompileShader succeeds).
        const std::vector<uint8_t>& GetCompiledBytecode() const { return m_bytecode; }

        // Dispatch the compiled shader on an input texture.
        // cbufferData: user constant buffer bytes (Width/Height prepended automatically).
        // resultCount: number of float4 elements in the result buffer.
        // Returns the readback data (resultCount * 4 floats).
        std::vector<float> Dispatch(
            ID3D11Texture2D* inputTexture,
            const std::vector<BYTE>& cbufferData,
            uint32_t resultCount);

        // Dispatch with an optional secondary image-output texture
        // bound at u1 as `RWTexture2D<float4>`. Used by
        // CustomComputeBridgeEffect: image-producing D3D11 compute
        // effects (CIE Histogram et al.) write their image output
        // here while still publishing analysis values to the
        // structured buffer at u0. Pass nullptr for analysis-only.
        // Same readback contract as the regular Dispatch.
        //
        // dispatchX/Y/Z: number of thread groups. Defaults to (1,1,1)
        // for analysis-only effects whose [numthreads] is sized to
        // cover the analysis output internally (e.g. numthreads(64,1,1)
        // running a per-thread loop over Width*Height pixels). Image-
        // producing per-pixel effects pass (W/tx, H/ty, 1) where tx,ty
        // are the [numthreads] dimensions.
        //
        // extraSrvs / extraSrvSlots: Phase 8 GPU-binding extension.
        // For each entry, the SRV is bound at the named t-slot before
        // dispatch. Used by the bridge to wire upstream
        // IEngineComputeOutput SRVs into a consumer's
        // SHADERLAB_GPU_BUFFER slots without a CPU readback round-trip.
        // Pass empty vectors for the no-binding case.
        //
        // readback: None leaves the values GPU-only (the SRV still sees them).
        // Blocking copies and Maps immediately -- the Map waits for the GPU to
        // finish EVERYTHING queued on the immediate context, including the
        // upstream D2D pre-render, so it drains the pipeline (2-10 ms per call
        // measured on this project's graphs). Async queues the copy into a
        // small staging ring and returns nothing; PollReadback collects the
        // values on a later frame without waiting.
        enum class Readback { None, Blocking, Async };
        std::vector<float> DispatchWithImageOutput(
            const std::vector<ID3D11Texture2D*>& inputTextures,
            const std::vector<BYTE>& cbufferData,
            uint32_t resultCount,
            ID3D11Texture2D* imageOutputTexture,
            uint32_t dispatchX = 1, uint32_t dispatchY = 1, uint32_t dispatchZ = 1,
            const std::vector<ID3D11ShaderResourceView*>& extraSrvs = {},
            const std::vector<uint32_t>& extraSrvSlots = {},
            Readback readback = Readback::Blocking);

        // Newest async readback that has completed, without waiting. Returns
        // false when nothing new has landed. Older completed copies are
        // discarded in favour of the newest.
        bool PollReadback(std::vector<float>& out);
        bool HasPendingReadback() const
        {
            for (const auto& s : m_staging) if (s.pending) return true;
            return false;
        }

        bool IsInitialized() const { return m_device != nullptr; }
        bool HasShader() const { return m_shader != nullptr; }

        // SRV onto the result structured buffer. Used by upstream
        // effects implementing IEngineComputeOutput to expose their
        // analysis output for direct GPU consumption by downstream
        // SHADERLAB_GPU_BUFFER bindings -- no Map() round-trip. Returns
        // nullptr until the first Dispatch (the SRV is created lazily
        // alongside the UAV in EnsureBuffers).
        ID3D11ShaderResourceView* GetResultSRV() const { return m_resultSRV.get(); }

        // ---- IUnknown (no-op refcount) ----------------------------------
        // The runner's lifetime is owned by the GraphEvaluator's cache;
        // AddRef / Release are no-ops returning a constant. This is
        // valid for "interior" COM objects whose lifetime is managed
        // by an external owner -- callers are required not to outlive
        // the evaluator that vends the pointer.
        HRESULT __stdcall QueryInterface(REFIID iid, void** out) noexcept override;
        ULONG   __stdcall AddRef() noexcept override { return 1; }
        ULONG   __stdcall Release() noexcept override { return 1; }

        // ---- IEngineComputeOutput ---------------------------------------
        HRESULT __stdcall GetAnalysisSrv(ID3D11ShaderResourceView** out) override;
        UINT64  __stdcall GetLastEvaluatedFrame() override { return m_lastEvaluatedFrame; }

        // ---- IEngineComputeTexture (lane 3) ------------------------------
        HRESULT __stdcall RequestAnalysisTexture() override;
        HRESULT __stdcall GetAnalysisTexture(ID3D11Texture2D** out) override;
        HRESULT __stdcall GetAnalysisTextureSrv(ID3D11ShaderResourceView** out) override;
        // True once a consumer has asked for lane 3. Read by Dispatch to
        // decide whether to run the copy pass at all.
        bool AnalysisTextureRequested() const { return m_analysisTexWanted; }

        // Called by the evaluator immediately after a successful Dispatch
        // so consumers can detect freshness via GetLastEvaluatedFrame.
        void SetLastEvaluatedFrame(uint64_t frame) { m_lastEvaluatedFrame = frame; }

    private:
        winrt::com_ptr<ID3D11Device> m_device;
        // The context every command is RECORDED on: a deferred context owned
        // by this runner alone. Nothing here is issued on the device's shared
        // immediate context -- a whole dispatch is recorded, closed into a
        // command list, and handed over with one ExecuteCommandList. See
        // SubmitRecorded().
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        // The shared immediate context. Touched for exactly two single calls:
        // ExecuteCommandList, and the CPU readback Map (which a deferred
        // context cannot do).
        winrt::com_ptr<ID3D11DeviceContext> m_immediate;
        // The runner's private deferred context (null if the device refused
        // one). m_context points at it or at m_immediate, chosen per dispatch.
        winrt::com_ptr<ID3D11DeviceContext> m_deferredCtx;
        bool m_deferred{ false };

        // Close what has been recorded on m_context into a command list and
        // execute it on the immediate context. No-op in immediate mode.
        void SubmitRecorded();
        winrt::com_ptr<ID3D11ComputeShader> m_shader;
        std::vector<uint8_t> m_bytecode;

        // Result buffer (RWStructuredBuffer<float4>) -- bound as both
        // UAV (writer) and SRV (downstream Phase 8 consumer).
        winrt::com_ptr<ID3D11Buffer> m_resultBuffer;
        // Staging ring. Blocking reads use any slot and Map at once; async
        // reads rotate through the ring so a copy can be in flight while
        // earlier ones are collected. Three covers the usual 1-2 frame GPU
        // latency; a fourth outstanding copy overwrites the oldest.
        struct StagingSlot
        {
            winrt::com_ptr<ID3D11Buffer> buffer;
            bool     pending{ false };
            uint64_t seq{ 0 };
        };
        static constexpr size_t kStagingSlots = 3;
        std::array<StagingSlot, kStagingSlots> m_staging{};
        uint64_t m_stagingSeq{ 0 };
        winrt::com_ptr<ID3D11Buffer> m_cbuffer;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_resultUAV;
        winrt::com_ptr<ID3D11ShaderResourceView>  m_resultSRV;
        uint32_t m_resultCount{ 0 };
        uint64_t m_lastEvaluatedFrame{ 0 };

        // Lane 3: Result[] copied into a 1 x N RGBA32F texture so a Direct2D
        // PIXEL shader can consume it as an effect input. Nothing here is
        // created until RequestAnalysisTexture() is called -- a graph with no
        // pixel-shader consumer pays nothing for the lane.
        bool m_analysisTexWanted{ false };
        winrt::com_ptr<ID3D11Texture2D>           m_analysisTex;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_analysisTexUAV;
        winrt::com_ptr<ID3D11ShaderResourceView>  m_analysisTexSRV;
        winrt::com_ptr<ID3D11ComputeShader>       m_analysisCopyShader;
        bool m_analysisTexFilled{ false };

        std::wstring m_compileError;

        // Multi-group reduction scratch (see ShaderLabParamsHlsl.h). Created
        // on first use by a shader that declares `_SLScratch`.
        bool m_usesScratch{ false };
        winrt::com_ptr<ID3D11Buffer>              m_scratch;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_scratchUAV;       // whole buffer
        winrt::com_ptr<ID3D11UnorderedAccessView> m_scratchHeadUAV;   // cleared head
        bool EnsureScratch();

        void EnsureBuffers(uint32_t resultCount);
        // Creates the lane-3 texture + copy shader on demand. Safe to call
        // every dispatch; returns false if the lane is unavailable.
        bool EnsureAnalysisTexture();
        // Buffer -> texture copy dispatch. Runs only when the lane is
        // wanted AND available.
        void PublishAnalysisTexture();
    };
}
