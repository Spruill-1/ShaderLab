#include "pch.h"
#include "StatisticsEffect.h"

// Define the GUID for ID2D1StatisticsEffect (declared in header via DEFINE_GUID).
// Must be in exactly one translation unit.
// {7A8B9C0D-2345-6789-ABCD-EF0123456789}
const GUID ShaderLab::Effects::IID_ID2D1StatisticsEffect =
{ 0x7a8b9c0d, 0x2345, 0x6789, { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 } };

namespace ShaderLab::Effects
{
    // Minimal pass-through pixel shader: outputs input unchanged.
    static const char s_passThroughPS[] = R"(
        Texture2D InputTexture : register(t0);
        SamplerState InputSampler : register(s0);
        float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_Target
        {
            return InputTexture.Sample(InputSampler, uv0.xy);
        }
    )";

    thread_local StatisticsEffect* StatisticsEffect::s_lastCreated = nullptr;

    HRESULT StatisticsEffect::RegisterEffect(ID2D1Factory1* factory)
    {
        if (!factory) return E_INVALIDARG;

        static const WCHAR xml[] =
            L"<?xml version='1.0'?>\r\n"
            L"<Effect>\r\n"
            L"  <Property name='DisplayName' type='string' value='Statistics'/>\r\n"
            L"  <Property name='Author'      type='string' value='ShaderLab'/>\r\n"
            L"  <Property name='Category'    type='string' value='Analysis'/>\r\n"
            L"  <Property name='Description' type='string' value='Pass-through with D3D11 compute statistics.'/>\r\n"
            L"  <Inputs>\r\n"
            L"    <Input name='Source'/>\r\n"
            L"  </Inputs>\r\n"
            L"</Effect>\r\n";

        return factory->RegisterEffectFromString(
            CLSID_StatisticsEffect,
            xml,
            nullptr, 0,
            &StatisticsEffect::CreateFactory);
    }

    HRESULT __stdcall StatisticsEffect::CreateFactory(IUnknown** effect)
    {
        auto* impl = new (std::nothrow) StatisticsEffect();
        if (!impl) return E_OUTOFMEMORY;
        *effect = static_cast<ID2D1EffectImpl*>(impl);
        s_lastCreated = impl;
        return S_OK;
    }

    // ---- IUnknown ----

    IFACEMETHODIMP StatisticsEffect::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;

        if (riid == __uuidof(ID2D1EffectImpl))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform))
            *ppv = static_cast<ID2D1DrawTransform*>(this);
        else if (riid == __uuidof(ID2D1Transform))
            *ppv = static_cast<ID2D1Transform*>(this);
        else if (riid == __uuidof(ID2D1TransformNode))
            *ppv = static_cast<ID2D1TransformNode*>(this);
        else if (riid == IID_ID2D1StatisticsEffect)
            *ppv = static_cast<ID2D1StatisticsEffect*>(this);
        else if (riid == __uuidof(IUnknown))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) StatisticsEffect::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) StatisticsEffect::Release()
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    // ---- ID2D1EffectImpl ----

    IFACEMETHODIMP StatisticsEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);

        // Compile and load the pass-through pixel shader.
        CoCreateGuid(&m_passThroughGuid);

        winrt::com_ptr<ID3DBlob> blob;
        winrt::com_ptr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(
            s_passThroughPS, sizeof(s_passThroughPS),
            "PassThrough", nullptr, nullptr,
            "main", "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, blob.put(), errors.put());
        if (FAILED(hr)) return hr;

        hr = effectContext->LoadPixelShader(
            m_passThroughGuid,
            static_cast<const BYTE*>(blob->GetBufferPointer()),
            static_cast<UINT32>(blob->GetBufferSize()));
        if (FAILED(hr)) return hr;

        return transformGraph->SetSingleTransformNode(this);
    }

    IFACEMETHODIMP StatisticsEffect::PrepareForRender(D2D1_CHANGE_TYPE /*changeType*/)
    {
        if (m_drawInfo)
            m_drawInfo->SetPixelShader(m_passThroughGuid);
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::SetGraph(ID2D1TransformGraph* /*transformGraph*/)
    {
        return E_NOTIMPL;
    }

    // ---- ID2D1DrawTransform ----

    IFACEMETHODIMP StatisticsEffect::SetDrawInfo(ID2D1DrawInfo* drawInfo)
    {
        m_drawInfo.copy_from(drawInfo);
        return drawInfo->SetPixelShader(m_passThroughGuid);
    }

    // ---- ID2D1Transform ----

    IFACEMETHODIMP StatisticsEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        if (inputRectCount > 0 && inputRects)
        {
            m_inputRect = inputRects[0];
            *outputRect = inputRects[0];
        }
        else
        {
            *outputRect = {};
        }
        *outputOpaqueSubRect = {};
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        if (inputRectCount > 0)
            inputRects[0] = *outputRect;
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L invalidInputRect,
        D2D1_RECT_L* invalidOutputRect) const
    {
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    IFACEMETHODIMP_(UINT32) StatisticsEffect::GetInputCount() const
    {
        return 1;
    }

    // ---- ID2D1StatisticsEffect ----

    IFACEMETHODIMP StatisticsEffect::ComputeStatistics(ID2D1DeviceContext* dc, ID2D1Image* image)
    {
        if (!dc || !image || !m_reduction.IsInitialized()) return E_FAIL;

        // Get image bounds.
        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(image, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 8192.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 8192.0f));
        if (w == 0 || h == 0) return S_OK;

        // Render to a D2D bitmap backed by a DXGI surface.
        winrt::com_ptr<ID2D1Bitmap1> gpuTarget;
        D2D1_BITMAP_PROPERTIES1 props = {};
        props.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, gpuTarget.put());
        if (FAILED(hr)) return hr;

        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());
        dc->SetTarget(gpuTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(image, D2D1::Point2F(-bounds.left, -bounds.top));
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());

        // Get the D3D11 texture.
        winrt::com_ptr<IDXGISurface> surface;
        hr = gpuTarget->GetSurface(surface.put());
        if (FAILED(hr)) return hr;

        winrt::com_ptr<ID3D11Texture2D> d3dTexture;
        hr = surface->QueryInterface(d3dTexture.put());
        if (FAILED(hr)) return hr;

        return ComputeFromTexture(d3dTexture.get());
    }

    HRESULT StatisticsEffect::ComputeFromTexture(ID3D11Texture2D* texture)
    {
        if (!texture || !m_reduction.IsInitialized()) return E_FAIL;

        m_lastStats = m_reduction.Reduce(
            m_d3dContext.get(), texture, m_channel, m_nonzeroOnly);

        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::GetStatistics(Rendering::ImageStats* stats)
    {
        if (!stats) return E_POINTER;
        *stats = m_lastStats;
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::SetChannel(UINT32 channel)
    {
        m_channel = channel;
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::SetNonzeroOnly(BOOL nonzeroOnly)
    {
        m_nonzeroOnly = (nonzeroOnly != FALSE);
        return S_OK;
    }

    void StatisticsEffect::SetD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        m_d3dDevice.copy_from(device);
        m_d3dContext.copy_from(context);
        if (device && !m_reduction.IsInitialized())
            m_reduction.Initialize(device);
    }
}
