#pragma once

#include "pch.h"
#include "ShaderCompiler.h"
#include "../Graph/PropertyValue.h"

namespace ShaderLab::Effects
{
    // Custom D2D effect that runs a user-supplied compute shader.
    //
    // Implements both ID2D1EffectImpl (effect lifecycle) and
    // ID2D1ComputeTransform (GPU compute shader execution via UAV).
    //
    // The compute transform differs from the draw transform:
    //   - Uses a compute shader (cs_5_0) instead of a pixel shader
    //   - Writes to a UAV output buffer instead of a render target
    //   - Must specify thread group dimensions for dispatch
    //
    // D2D custom property:
    //   Index 0: InputCount (uint32) — number of texture inputs
    //
    // Host-managed state (outside D2D property system):
    //   - Shader bytecode (set via LoadShaderBytecode)
    //   - Constant buffer data (set via PackConstantBuffer / SetConstantBufferData)
    //   - Thread group size (set via SetThreadGroupSize)
    //
    // Lifecycle:
    //   1. Host calls ID2D1DeviceContext::CreateEffect(CLSID_CustomComputeShader)
    //   2. Host obtains CustomComputeShaderEffect* via the effect cache
    //   3. Host calls LoadShaderBytecode() with compiled compute shader
    //   4. Host calls PackConstantBuffer() to populate cbuffer from properties
    //   5. D2D calls PrepareForRender → effect pushes shader + cbuffer to GPU
    //   6. D2D dispatches compute shader via ID2D1ComputeInfo
    class CustomComputeShaderEffect
        : public ID2D1EffectImpl
        , public ID2D1ComputeTransform
    {
    public:
        // CLSID for this custom effect.
        // {B2C3D4E5-2345-6789-ABCD-EF0123456789}
        static constexpr GUID CLSID_CustomComputeShader =
        { 0xb2c3d4e5, 0x2345, 0x6789, { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 } };

        // Register this effect with a D2D factory. Call once at startup.
        static HRESULT RegisterEffect(ID2D1Factory1* factory);

        // Unregister from the factory (optional cleanup).
        static HRESULT UnregisterEffect(ID2D1Factory1* factory);

        // D2D creation callback (invoked by CreateEffect).
        static HRESULT __stdcall CreateFactory(IUnknown** effect);
        static thread_local CustomComputeShaderEffect* s_lastCreated;

        // ---- IUnknown ----
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override;
        IFACEMETHODIMP_(ULONG) Release() override;

        // ---- ID2D1EffectImpl ----
        IFACEMETHODIMP Initialize(
            ID2D1EffectContext* effectContext,
            ID2D1TransformGraph* transformGraph) override;
        IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE changeType) override;
        IFACEMETHODIMP SetGraph(ID2D1TransformGraph* transformGraph) override;

        // ---- ID2D1ComputeTransform ----
        IFACEMETHODIMP SetComputeInfo(ID2D1ComputeInfo* computeInfo) override;
        IFACEMETHODIMP CalculateThreadgroups(
            const D2D1_RECT_L* outputRect,
            UINT32* dimensionX,
            UINT32* dimensionY,
            UINT32* dimensionZ) override;

        // ---- ID2D1Transform ----
        IFACEMETHODIMP MapInputRectsToOutputRect(
            const D2D1_RECT_L* inputRects,
            const D2D1_RECT_L* inputOpaqueSubRects,
            UINT32 inputRectCount,
            D2D1_RECT_L* outputRect,
            D2D1_RECT_L* outputOpaqueSubRect) override;
        IFACEMETHODIMP MapOutputRectToInputRects(
            const D2D1_RECT_L* outputRect,
            D2D1_RECT_L* inputRects,
            UINT32 inputRectCount) const override;
        IFACEMETHODIMP MapInvalidRect(
            UINT32 inputIndex,
            D2D1_RECT_L invalidInputRect,
            D2D1_RECT_L* invalidOutputRect) const override;

        // ---- ID2D1TransformNode ----
        IFACEMETHODIMP_(UINT32) GetInputCount() const override;

        // ---- D2D property accessor (InputCount only) ----
        HRESULT SetInputCount(UINT32 count);
        UINT32  GetInputCountProp() const;

        // ---- Host-facing API (managed outside D2D property system) ----

        // Load shader from compiled bytecode blob. Updates the compute shader.
        HRESULT LoadShaderBytecode(ID3DBlob* bytecode);
        HRESULT LoadShaderBytecode(const BYTE* data, UINT32 dataSize);
        void SetShaderGuid(const GUID& guid) { m_shaderGuid = guid; }

        // Set input count directly (bypasses D2D property system).
        void SetInputCountDirect(UINT32 count) { m_inputCount = count; }

        void SetDesiredOutputRect(const D2D1_RECT_L& rect) { m_desiredOutputRect = rect; m_hasDesiredRect = true; }

        // Set raw constant buffer data (will be uploaded on next PrepareForRender).
        void SetConstantBufferData(const BYTE* data, UINT32 dataSize);

        // Pack PropertyValue map into the constant buffer using reflection info.
        void PackConstantBuffer(
            const std::map<std::wstring, Graph::PropertyValue>& properties,
            const std::vector<ShaderVariable>& variables,
            uint32_t cbSizeBytes);

        // Set thread group dimensions (default: 8×8×1).
        void SetThreadGroupSize(UINT32 x, UINT32 y, UINT32 z);

    private:
        CustomComputeShaderEffect();
        ~CustomComputeShaderEffect() = default;

        LONG m_refCount{ 1 };

        winrt::com_ptr<ID2D1EffectContext> m_effectContext;
        winrt::com_ptr<ID2D1ComputeInfo>   m_computeInfo;
        winrt::com_ptr<ID2D1TransformGraph> m_transformGraph;

        // Shader state.
        std::vector<BYTE> m_shaderBytecode;
        GUID m_shaderGuid{};
        std::vector<BYTE> m_constantBuffer;
        UINT32            m_inputCount{ 8 };
        D2D1_RECT_L       m_desiredOutputRect{};
        bool              m_hasDesiredRect{ false };

        // Thread group dimensions for compute dispatch.
        UINT32 m_threadGroupX{ 8 };
        UINT32 m_threadGroupY{ 8 };
        UINT32 m_threadGroupZ{ 1 };

        // Whether we need to re-upload the constant buffer.
        bool m_cbDirty{ false };
        // Whether we need to re-set the compute shader.
        bool m_shaderDirty{ false };
    };
}
