#pragma once

#include "pch.h"
#include "ShaderCompiler.h"
#include "../Graph/PropertyValue.h"

namespace ShaderLab::Effects
{
    // Custom D2D effect that runs a user-supplied pixel shader.
    //
    // Implements both ID2D1EffectImpl (effect lifecycle) and
    // ID2D1DrawTransform (GPU pixel shader execution).
    //
    // The effect is registered with D2D via RegisterEffect() using a
    // well-known CLSID. Each instance can load different HLSL bytecode
    // at runtime (hot-reload). Constant buffer values are populated from
    // the EffectNode property map.
    //
    // D2D custom property:
    //   Index 0: InputCount (uint32) — number of texture inputs
    //
    // Host-managed state (outside D2D property system):
    //   - Shader bytecode (set via LoadShaderBytecode)
    //   - Constant buffer data (set via PackConstantBuffer / SetConstantBufferData)
    //
    // Lifecycle:
    //   1. Host calls ID2D1DeviceContext::CreateEffect(CLSID_CustomPixelShader)
    //   2. Host obtains CustomPixelShaderEffect* via the effect cache
    //   3. Host calls LoadShaderBytecode() with compiled pixel shader
    //   4. Host calls PackConstantBuffer() to populate cbuffer from properties
    //   5. D2D calls PrepareForRender → effect pushes shader + cbuffer to GPU
    //   6. D2D renders with the pixel shader via ID2D1DrawInfo
    class CustomPixelShaderEffect
        : public ID2D1EffectImpl
        , public ID2D1DrawTransform
    {
    public:
        // CLSID for this custom effect.
        // {A1B2C3D4-1234-5678-9ABC-DEF012345678}
        static constexpr GUID CLSID_CustomPixelShader =
        { 0xa1b2c3d4, 0x1234, 0x5678, { 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78 } };

        // Register this effect with a D2D factory. Call once at startup.
        static HRESULT RegisterEffect(ID2D1Factory1* factory);

        // Register a per-definition CLSID with the exact number of inputs.
        static HRESULT RegisterWithInputCount(ID2D1Factory1* factory, REFCLSID clsid, UINT32 inputCount);

        // Unregister from the factory (optional cleanup).
        static HRESULT UnregisterEffect(ID2D1Factory1* factory);

        // D2D creation callback (invoked by CreateEffect).
        static HRESULT __stdcall CreateFactory(IUnknown** effect);

        // Last-created instance (used to capture impl pointer after CreateEffect).
        static thread_local CustomPixelShaderEffect* s_lastCreated;

        // Set before CreateEffect to initialize m_inputCount in the constructor.
        static thread_local UINT32 s_pendingInputCount;

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

        // ---- ID2D1DrawTransform ----
        IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* drawInfo) override;

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

        // Load shader from compiled bytecode blob. Updates the D2D pixel shader.
        HRESULT LoadShaderBytecode(ID3DBlob* bytecode);

        // Load shader from raw bytecode data.
        HRESULT LoadShaderBytecode(const BYTE* data, UINT32 dataSize);

        // Set per-instance shader GUID (must be set before loading bytecode).
        void SetShaderGuid(const GUID& guid) { m_shaderGuid = guid; }

        // Force-upload the constant buffer to the GPU via DrawInfo.
        // Call this from the evaluator when properties change, bypassing
        // D2D's PrepareForRender change detection.
        HRESULT ForceUploadConstantBuffer();

        // Check if shader bytecode needs initial loading.
        bool NeedsShaderLoad() const { return m_shaderBytecode.empty(); }

        // Set input count directly and rebuild the D2D transform graph.
        void SetInputCountDirect(UINT32 count)
        {
            if (count == m_inputCount) return;
            m_inputCount = count;
            if (m_transformGraph)
                m_transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
        }

        // Set raw constant buffer data (will be uploaded on next PrepareForRender).
        void SetConstantBufferData(const BYTE* data, UINT32 dataSize);

        // Pack PropertyValue map into the constant buffer using reflection info.
        void PackConstantBuffer(
            const std::map<std::wstring, Graph::PropertyValue>& properties,
            const std::vector<ShaderVariable>& variables,
            uint32_t cbSizeBytes);

    private:
        CustomPixelShaderEffect();
        ~CustomPixelShaderEffect() = default;

        LONG m_refCount{ 1 };

        winrt::com_ptr<ID2D1EffectContext> m_effectContext;
        winrt::com_ptr<ID2D1DrawInfo>      m_drawInfo;
        winrt::com_ptr<ID2D1TransformGraph> m_transformGraph;

        // Shader state.
        std::vector<BYTE> m_shaderBytecode;
        GUID m_shaderGuid{};  // Per-instance shader ID for D2D LoadPixelShader.
        std::vector<BYTE> m_constantBuffer;
        UINT32            m_inputCount{ 1 };
        D2D1_RECT_L       m_inputRect{};
        D2D1_RECT_L       m_lastRequestedRect{};
        bool              m_hasRequestedRect{ false };

        // Whether we need to re-upload the constant buffer.
        bool m_cbDirty{ false };
        // Whether we need to re-set the pixel shader.
        bool m_shaderDirty{ false };
    };
}
