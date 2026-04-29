#include "pch.h"
#include "StatisticsEffect.h"

// Define the IID (declared via DEFINE_GUID in header).
const GUID ShaderLab::Effects::IID_ID2D1StatisticsEffect =
{ 0x7a8b9c0d, 0x2345, 0x6789, { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 } };

namespace ShaderLab::Effects
{
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
            L"  <Property name='Description' type='string' value='Pass-through with D3D11 GPU statistics.'/>\r\n"
            L"  <Inputs><Input name='Source'/></Inputs>\r\n"
            L"</Effect>\r\n";
        return factory->RegisterEffectFromString(
            CLSID_StatisticsEffect, xml, nullptr, 0, &CreateFactory);
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
        if (riid == __uuidof(ID2D1EffectImpl))       *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform))*ppv = static_cast<ID2D1DrawTransform*>(this);
        else if (riid == __uuidof(ID2D1Transform))    *ppv = static_cast<ID2D1Transform*>(this);
        else if (riid == __uuidof(ID2D1TransformNode))*ppv = static_cast<ID2D1TransformNode*>(this);
        else if (riid == IID_ID2D1StatisticsEffect)   *ppv = static_cast<ID2D1StatisticsEffect*>(this);
        else if (riid == __uuidof(IUnknown))          *ppv = static_cast<ID2D1EffectImpl*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) StatisticsEffect::AddRef()  { return InterlockedIncrement(&m_refCount); }
    IFACEMETHODIMP_(ULONG) StatisticsEffect::Release()  { auto c = InterlockedDecrement(&m_refCount); if (!c) delete this; return c; }

    // ---- ID2D1EffectImpl ----

    IFACEMETHODIMP StatisticsEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);

        // Acquire D3D11 device from the DXGI device backing D2D.
        // ID2D1EffectContext doesn't expose this directly, so we go through
        // the D2D device. EffectContext has no GetDevice, but the D2D factory
        // that created us shares the same DXGI device. We use a helper:
        // create a tiny D2D resource to extract the device chain.
        //
        // Alternative: the host can call ComputeFromTexture() with a pre-acquired
        // D3D11 texture, bypassing the need for device acquisition here.
        //
        // For now, we defer device acquisition to ComputeStatistics() where
        // we have an ID2D1DeviceContext.

        // Compile pass-through pixel shader.
        CoCreateGuid(&m_passThroughGuid);
        winrt::com_ptr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(s_passThroughPS, sizeof(s_passThroughPS),
            "PassThrough", nullptr, nullptr, "main", "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, blob.put(), errors.put());
        if (FAILED(hr)) return hr;

        hr = effectContext->LoadPixelShader(m_passThroughGuid,
            static_cast<const BYTE*>(blob->GetBufferPointer()),
            static_cast<UINT32>(blob->GetBufferSize()));
        if (FAILED(hr)) return hr;

        return transformGraph->SetSingleTransformNode(this);
    }

    IFACEMETHODIMP StatisticsEffect::PrepareForRender(D2D1_CHANGE_TYPE)
    {
        if (m_drawInfo) m_drawInfo->SetPixelShader(m_passThroughGuid);
        m_statsValid = false;  // Input may have changed — invalidate cached stats.
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::SetGraph(ID2D1TransformGraph*) { return E_NOTIMPL; }

    // ---- ID2D1DrawTransform ----

    IFACEMETHODIMP StatisticsEffect::SetDrawInfo(ID2D1DrawInfo* drawInfo)
    {
        m_drawInfo.copy_from(drawInfo);
        return drawInfo->SetPixelShader(m_passThroughGuid);
    }

    // ---- ID2D1Transform ----

    IFACEMETHODIMP StatisticsEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects, const D2D1_RECT_L*, UINT32 inputRectCount,
        D2D1_RECT_L* outputRect, D2D1_RECT_L* outputOpaqueSubRect)
    {
        if (inputRectCount > 0 && inputRects)
        {
            m_inputRect = inputRects[0];
            *outputRect = inputRects[0];
        }
        else *outputRect = {};
        *outputOpaqueSubRect = {};
        m_statsValid = false;  // New input rect — invalidate.
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect, D2D1_RECT_L* inputRects, UINT32 inputRectCount) const
    {
        if (inputRectCount > 0) inputRects[0] = *outputRect;
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::MapInvalidRect(
        UINT32, D2D1_RECT_L invalidInputRect, D2D1_RECT_L* invalidOutputRect) const
    {
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    IFACEMETHODIMP_(UINT32) StatisticsEffect::GetInputCount() const { return 1; }

    // ---- ID2D1StatisticsEffect ----

    IFACEMETHODIMP StatisticsEffect::ComputeStatistics(ID2D1DeviceContext* dc, ID2D1Image* image)
    {
        if (!dc || !image) return E_INVALIDARG;
        if (m_statsValid) return S_OK;

        // Lazy-init D3D11 device from the D2D device context.
        if (!m_d3dDevice)
        {
            winrt::com_ptr<ID2D1Device> d2dDevice;
            dc->GetDevice(d2dDevice.put());
            if (d2dDevice)
            {
                winrt::com_ptr<ID2D1Device2> d2dDevice2;
                d2dDevice->QueryInterface(d2dDevice2.put());
                if (d2dDevice2)
                {
                    winrt::com_ptr<IDXGIDevice> dxgiDevice;
                    if (SUCCEEDED(d2dDevice2->GetDxgiDevice(dxgiDevice.put())))
                    {
                        dxgiDevice->QueryInterface(m_d3dDevice.put());
                        if (m_d3dDevice)
                        {
                            m_d3dDevice->GetImmediateContext(m_d3dContext.put());
                            m_reduction.Initialize(m_d3dDevice.get());
                        }
                    }
                }
            }
        }
        if (!m_reduction.IsInitialized()) return E_FAIL;

        // Get image bounds.
        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(image, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 8192.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 8192.0f));
        if (w == 0 || h == 0) return S_OK;

        // Render image to FP32 bitmap for D3D11 access.
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
        hr = dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        if (FAILED(hr)) return hr;

        // Get D3D11 texture from DXGI surface.
        winrt::com_ptr<IDXGISurface> surface;
        hr = gpuTarget->GetSurface(surface.put());
        if (FAILED(hr)) return hr;

        winrt::com_ptr<ID3D11Texture2D> d3dTexture;
        hr = surface->QueryInterface(d3dTexture.put());
        if (FAILED(hr)) return hr;

        // Dispatch GPU reduction.
        return ComputeFromTexture(d3dTexture.get());
    }

    IFACEMETHODIMP StatisticsEffect::GetStatistics(Rendering::ImageStats* stats)
    {
        if (!stats) return E_POINTER;
        *stats = m_lastStats;
        return m_statsValid ? S_OK : S_FALSE;
    }

    IFACEMETHODIMP StatisticsEffect::SetChannel(UINT32 channel)
    {
        if (m_channel != channel) { m_channel = channel; m_statsValid = false; }
        return S_OK;
    }

    IFACEMETHODIMP StatisticsEffect::SetNonzeroOnly(BOOL nonzeroOnly)
    {
        bool nz = (nonzeroOnly != FALSE);
        if (m_nonzeroOnly != nz) { m_nonzeroOnly = nz; m_statsValid = false; }
        return S_OK;
    }

    HRESULT StatisticsEffect::ComputeFromTexture(ID3D11Texture2D* texture)
    {
        if (!texture) return E_INVALIDARG;

        // Lazy-init from texture's device if needed.
        if (!m_d3dDevice)
        {
            texture->GetDevice(m_d3dDevice.put());
            if (m_d3dDevice)
            {
                m_d3dDevice->GetImmediateContext(m_d3dContext.put());
                m_reduction.Initialize(m_d3dDevice.get());
            }
        }

        if (!m_reduction.IsInitialized()) return E_FAIL;

        m_lastStats = m_reduction.Reduce(m_d3dContext.get(), texture, m_channel, m_nonzeroOnly);
        m_statsValid = true;
        return S_OK;
    }
}
