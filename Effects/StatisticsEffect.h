#pragma once

#include "pch.h"
#include "../Rendering/GpuReduction.h"

namespace ShaderLab::Effects
{
    // {7A8B9C0D-2345-6789-ABCD-EF0123456789}
    DEFINE_GUID(IID_ID2D1StatisticsEffect,
        0x7a8b9c0d, 0x2345, 0x6789, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89);

    // Custom COM interface for retrieving analysis results from a statistics effect.
    // Any D2D consumer can QI for this after DrawImage to get reduction results.
    MIDL_INTERFACE("7A8B9C0D-2345-6789-ABCD-EF0123456789")
    ID2D1StatisticsEffect : public IUnknown
    {
        // Compute statistics on the provided D2D image.
        // Call after dc->DrawImage(effect) with the effect's output image.
        STDMETHOD(ComputeStatistics)(ID2D1DeviceContext* dc, ID2D1Image* image) PURE;

        // Retrieve the last computed statistics.
        STDMETHOD(GetStatistics)(Rendering::ImageStats* stats) PURE;

        // Set which channel to analyze (0=lum, 1=R, 2=G, 3=B, 4=A).
        STDMETHOD(SetChannel)(UINT32 channel) PURE;

        // Set whether to exclude zero pixels from min/max/mean.
        STDMETHOD(SetNonzeroOnly)(BOOL nonzeroOnly) PURE;
    };

    // D2D effect implementation that wraps a pass-through pixel shader
    // with D3D11 compute shader statistics. Works as a standard D2D effect:
    //   dc->CreateEffect(CLSID_StatisticsEffect, ...)
    //   effect->SetInput(0, someImage)
    //   dc->DrawImage(effect)  // pass-through render
    //   auto impl = effect.as<ID2D1StatisticsEffect>()
    //   impl->ComputeStatistics(dc)
    //   ImageStats stats; impl->GetStatistics(&stats)
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

        // ---- ID2D1StatisticsEffect ----
        IFACEMETHODIMP ComputeStatistics(ID2D1DeviceContext* dc, ID2D1Image* image) override;
        IFACEMETHODIMP GetStatistics(Rendering::ImageStats* stats) override;
        IFACEMETHODIMP SetChannel(UINT32 channel) override;
        IFACEMETHODIMP SetNonzeroOnly(BOOL nonzeroOnly) override;

        // Direct texture path for evaluator (avoids re-rendering).
        HRESULT ComputeFromTexture(ID3D11Texture2D* texture);

        // Set D3D11 device (called by evaluator after effect creation).
        void SetD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context);

    private:
        LONG m_refCount{ 1 };
        winrt::com_ptr<ID2D1EffectContext> m_effectContext;
        winrt::com_ptr<ID2D1DrawInfo> m_drawInfo;
        D2D1_RECT_L m_inputRect{};

        // D3D11 compute resources.
        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
        Rendering::GpuReduction m_reduction;

        // Analysis settings.
        uint32_t m_channel{ 0 };
        bool m_nonzeroOnly{ true };
        Rendering::ImageStats m_lastStats{};

        // Pass-through pixel shader GUID.
        GUID m_passThroughGuid{};
    };
}
