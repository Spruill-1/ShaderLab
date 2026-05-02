#pragma once

#include "pch_engine.h"
#include "../Rendering/GpuReduction.h"

namespace ShaderLab::Effects
{
    // {7A8B9C0D-2345-6789-ABCD-EF0123456789}
    DEFINE_GUID(IID_ID2D1StatisticsEffect,
        0x7a8b9c0d, 0x2345, 0x6789, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89);

    // Custom COM interface for GPU-accelerated image statistics.
    //
    // Drop-in usage for any D2D application:
    //
    //   // Setup (once):
    //   dc->CreateEffect(CLSID_StatisticsEffect, effect.put());
    //   auto stats = effect.as<ID2D1StatisticsEffect>();
    //   stats->SetDeviceContext(dc, effect.get());  // weak cache
    //   stats->SetChannel(0);              // 0=luminance, 1=R, 2=G, 3=B, 4=A
    //   stats->SetNonzeroOnly(TRUE);
    //
    //   // Per-frame (standard D2D pipeline):
    //   effect->SetInput(0, anyUpstreamImage);
    //   dc->DrawImage(effect.get());       // pass-through: output == input
    //
    //   // Get results (lazy compute — no extra calls needed):
    //   Rendering::ImageStats result;
    //   stats->GetStatistics(&result);     // auto-dispatches D3D11 compute if stale
    //
    MIDL_INTERFACE("7A8B9C0D-2345-6789-ABCD-EF0123456789")
    ID2D1StatisticsEffect : public IUnknown
    {
        // Cache the device context and effect wrapper for lazy compute.
        // Stored as weak (non-AddRef) pointers — both must outlive the effect.
        // Call once after creation.
        STDMETHOD(SetDeviceContext)(ID2D1DeviceContext* dc, ID2D1Effect* self) PURE;

        // Retrieve statistics. If results are stale (input changed since last
        // DrawImage), automatically triggers a D3D11 compute dispatch using
        // the cached device context. Returns S_FALSE if no dc is set.
        STDMETHOD(GetStatistics)(Rendering::ImageStats* stats) PURE;

        // Set which channel to analyze (0=luminance, 1=R, 2=G, 3=B, 4=A).
        STDMETHOD(SetChannel)(UINT32 channel) PURE;

        // Set whether to exclude near-zero pixels from min/max/mean.
        STDMETHOD(SetNonzeroOnly)(BOOL nonzeroOnly) PURE;

        // Explicit compute from a provided image + dc (for callers that
        // don't want to use the cached dc pattern).
        STDMETHOD(ComputeStatistics)(ID2D1DeviceContext* dc, ID2D1Image* image) PURE;
    };

    // Self-contained D2D effect with D3D11 compute statistics.
    //
    // Works as a standard D2D effect in any existing pipeline. The pixel shader
    // is a pass-through (output == input). The D3D11 compute dispatch is fully
    // encapsulated — the effect acquires its own ID3D11Device from the DXGI
    // device backing D2D during Initialize.
    //
    // For hosts that already have the input as an ID3D11Texture2D (e.g.,
    // ShaderLab's evaluator), ComputeFromTexture() skips the D2D rendering
    // step for better performance.
    //
    class StatisticsEffect
        : public ID2D1EffectImpl
        , public ID2D1DrawTransform
        , public ID2D1StatisticsEffect
    {
    public:
        // {B3C4D5E6-3456-789A-BCDE-F01234567890}
        static constexpr GUID CLSID_StatisticsEffect =
        { 0xb3c4d5e6, 0x3456, 0x789a, { 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78, 0x90 } };

        static HRESULT RegisterEffect(ID2D1Factory1* factory);
        static HRESULT __stdcall CreateFactory(IUnknown** effect);
        static thread_local StatisticsEffect* s_lastCreated;

        // ---- IUnknown ----
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override;
        IFACEMETHODIMP_(ULONG) Release() override;

        // ---- ID2D1EffectImpl ----
        IFACEMETHODIMP Initialize(ID2D1EffectContext* effectContext,
            ID2D1TransformGraph* transformGraph) override;
        IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE changeType) override;
        IFACEMETHODIMP SetGraph(ID2D1TransformGraph* transformGraph) override;

        // ---- ID2D1DrawTransform ----
        IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* drawInfo) override;

        // ---- ID2D1Transform ----
        IFACEMETHODIMP MapInputRectsToOutputRect(
            const D2D1_RECT_L* inputRects, const D2D1_RECT_L* inputOpaqueSubRects,
            UINT32 inputRectCount, D2D1_RECT_L* outputRect,
            D2D1_RECT_L* outputOpaqueSubRect) override;
        IFACEMETHODIMP MapOutputRectToInputRects(
            const D2D1_RECT_L* outputRect, D2D1_RECT_L* inputRects,
            UINT32 inputRectCount) const override;
        IFACEMETHODIMP MapInvalidRect(
            UINT32 inputIndex, D2D1_RECT_L invalidInputRect,
            D2D1_RECT_L* invalidOutputRect) const override;
        IFACEMETHODIMP_(UINT32) GetInputCount() const override;

        // ---- ID2D1StatisticsEffect ----
        IFACEMETHODIMP SetDeviceContext(ID2D1DeviceContext* dc, ID2D1Effect* self) override;
        IFACEMETHODIMP GetStatistics(Rendering::ImageStats* stats) override;
        IFACEMETHODIMP SetChannel(UINT32 channel) override;
        IFACEMETHODIMP SetNonzeroOnly(BOOL nonzeroOnly) override;
        IFACEMETHODIMP ComputeStatistics(ID2D1DeviceContext* dc, ID2D1Image* image) override;

        // ---- Optimized path (skips D2D re-render) ----
        HRESULT ComputeFromTexture(ID3D11Texture2D* texture);

    private:
        LONG m_refCount{ 1 };
        winrt::com_ptr<ID2D1EffectContext> m_effectContext;
        winrt::com_ptr<ID2D1DrawInfo> m_drawInfo;
        D2D1_RECT_L m_inputRect{};

        // D3D11 resources — acquired from DXGI device in Initialize.
        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
        Rendering::GpuReduction m_reduction;

        // Settings + cached results.
        uint32_t m_channel{ 0 };
        bool m_nonzeroOnly{ true };
        Rendering::ImageStats m_lastStats{};
        bool m_statsValid{ false };
        GUID m_passThroughGuid{};

        // Weak-cached device context for lazy compute in GetStatistics.
        // NOT AddRef'd — consumer must ensure dc outlives the effect.
        ID2D1DeviceContext* m_weakDc{ nullptr };
        ID2D1Effect* m_weakSelf{ nullptr };  // Weak ref to our D2D effect wrapper.
    };
}
