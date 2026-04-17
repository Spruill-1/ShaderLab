#include "pch.h"
#include "CustomPixelShaderEffect.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Property bindings table (used by D2D effect registration)
    // -----------------------------------------------------------------------

    // Custom property index (only InputCount is exposed to D2D).
    // Shader bytecode and constant buffer are managed internally
    // via direct pointer access from GraphEvaluator.
    enum : UINT32
    {
        PROP_INPUT_COUNT = 0,
    };

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    HRESULT CustomPixelShaderEffect::RegisterEffect(ID2D1Factory1* factory)
    {
        if (!factory)
            return E_INVALIDARG;

        // Property XML schema for D2D.
        // Only InputCount is registered as a D2D property.
        // Shader bytecode and constant buffer are managed outside the
        // D2D property system for simplicity and performance.
        static const PCWSTR pszXml =
            L"<?xml version='1.0'?>\r\n"
            L"<Effect>\r\n"
            L"  <Property name='DisplayName' type='string' value='Custom Pixel Shader'/>\r\n"
            L"  <Property name='Author'      type='string' value='ShaderLab'/>\r\n"
            L"  <Property name='Category'    type='string' value='Custom'/>\r\n"
            L"  <Property name='Description' type='string' value='Runs a user-supplied pixel shader.'/>\r\n"
            L"  <Inputs minimum='0' maximum='8'/>\r\n"
            L"  <Property name='InputCount' type='uint32'>\r\n"
            L"    <Property name='DisplayName' type='string' value='Input Count'/>\r\n"
            L"  </Property>\r\n"
            L"</Effect>\r\n";

        // Property binding table — maps D2D property indices to get/set methods.
        const D2D1_PROPERTY_BINDING bindings[] =
        {
            D2D1_VALUE_TYPE_BINDING(
                L"InputCount",
                &CustomPixelShaderEffect::SetInputCount,
                &CustomPixelShaderEffect::GetInputCountProp),
        };

        return factory->RegisterEffectFromString(
            CLSID_CustomPixelShader,
            pszXml,
            bindings,
            ARRAYSIZE(bindings),
            &CustomPixelShaderEffect::CreateFactory);
    }

    HRESULT CustomPixelShaderEffect::UnregisterEffect(ID2D1Factory1* factory)
    {
        if (!factory)
            return E_INVALIDARG;
        return factory->UnregisterEffect(CLSID_CustomPixelShader);
    }

    // Thread-local for capturing the last-created impl pointer.
    thread_local CustomPixelShaderEffect* CustomPixelShaderEffect::s_lastCreated = nullptr;

    HRESULT __stdcall CustomPixelShaderEffect::CreateFactory(IUnknown** effect)
    {
        auto* impl = new (std::nothrow) CustomPixelShaderEffect();
        *effect = static_cast<ID2D1EffectImpl*>(impl);
        s_lastCreated = impl;
        return *effect ? S_OK : E_OUTOFMEMORY;
    }

    CustomPixelShaderEffect::CustomPixelShaderEffect() = default;

    // -----------------------------------------------------------------------
    // IUnknown
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
            return E_POINTER;

        *ppv = nullptr;

        if (riid == __uuidof(ID2D1EffectImpl))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform))
            *ppv = static_cast<ID2D1DrawTransform*>(this);
        else if (riid == __uuidof(ID2D1Transform))
            *ppv = static_cast<ID2D1Transform*>(this);
        else if (riid == __uuidof(ID2D1TransformNode))
            *ppv = static_cast<ID2D1TransformNode*>(this);
        else if (riid == __uuidof(IUnknown))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else
            return E_NOINTERFACE;

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) CustomPixelShaderEffect::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) CustomPixelShaderEffect::Release()
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return count;
    }

    // -----------------------------------------------------------------------
    // ID2D1EffectImpl
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);
        m_transformGraph.copy_from(transformGraph);

        // Set ourselves as the single transform node in the graph.
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    IFACEMETHODIMP CustomPixelShaderEffect::PrepareForRender(D2D1_CHANGE_TYPE /*changeType*/)
    {
        if (!m_drawInfo)
            return E_FAIL;

        // If the shader bytecode changed, load it into D2D.
        if (m_shaderDirty && !m_shaderBytecode.empty())
        {
            // Use per-instance GUID so multiple custom effects don't collide.
            if (m_shaderGuid == GUID{})
                CoCreateGuid(&m_shaderGuid);

            HRESULT hr = m_effectContext->LoadPixelShader(
                m_shaderGuid,
                m_shaderBytecode.data(),
                static_cast<UINT32>(m_shaderBytecode.size()));

            if (SUCCEEDED(hr))
            {
                hr = m_drawInfo->SetPixelShader(m_shaderGuid);
            }

            if (FAILED(hr))
                return hr;

            m_shaderDirty = false;
        }

        // If the constant buffer data changed, push it to the GPU.
        if (m_cbDirty && !m_constantBuffer.empty())
        {
            HRESULT hr = m_drawInfo->SetPixelShaderConstantBuffer(
                m_constantBuffer.data(),
                static_cast<UINT32>(m_constantBuffer.size()));

            if (FAILED(hr))
                return hr;

            m_cbDirty = false;
        }

        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::SetGraph(ID2D1TransformGraph* transformGraph)
    {
        // Called when the number of inputs changes.
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    // -----------------------------------------------------------------------
    // ID2D1DrawTransform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::SetDrawInfo(ID2D1DrawInfo* drawInfo)
    {
        m_drawInfo.copy_from(drawInfo);

        // If we already have bytecode loaded, set the shader immediately.
        if (!m_shaderBytecode.empty())
        {
            m_shaderDirty = true;
        }

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1Transform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        // Default behavior: output rect = union of all input rects.
        if (inputRectCount > 0 && inputRects)
        {
            m_inputRect = inputRects[0];
            *outputRect = inputRects[0];

            for (UINT32 i = 1; i < inputRectCount; ++i)
            {
                outputRect->left   = (std::min)(outputRect->left,   inputRects[i].left);
                outputRect->top    = (std::min)(outputRect->top,    inputRects[i].top);
                outputRect->right  = (std::max)(outputRect->right,  inputRects[i].right);
                outputRect->bottom = (std::max)(outputRect->bottom, inputRects[i].bottom);
            }
        }
        else
        {
            // No inputs — the shader generates content (unlikely for pixel shaders).
            *outputRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        }

        // No opaque sub-rect (conservative).
        *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        // Default 1:1 mapping — each input needs the same rect as the output.
        for (UINT32 i = 0; i < inputRectCount; ++i)
        {
            inputRects[i] = *outputRect;
        }
        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L invalidInputRect,
        D2D1_RECT_L* invalidOutputRect) const
    {
        // Any change in an input invalidates the same region of the output.
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1TransformNode
    // -----------------------------------------------------------------------

    IFACEMETHODIMP_(UINT32) CustomPixelShaderEffect::GetInputCount() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Custom property accessors
    // -----------------------------------------------------------------------

    HRESULT CustomPixelShaderEffect::SetInputCount(UINT32 count)
    {
        if (count == m_inputCount)
            return S_OK;

        m_inputCount = count;

        // Update the D2D transform graph to reflect the new input count.
        if (m_transformGraph)
        {
            return m_transformGraph->SetSingleTransformNode(
                static_cast<ID2D1DrawTransform*>(this));
        }
        return S_OK;
    }

    UINT32 CustomPixelShaderEffect::GetInputCountProp() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Host-facing API (outside D2D property system)
    // -----------------------------------------------------------------------

    HRESULT CustomPixelShaderEffect::LoadShaderBytecode(ID3DBlob* bytecode)
    {
        if (!bytecode)
            return E_INVALIDARG;
        return LoadShaderBytecode(
            static_cast<const BYTE*>(bytecode->GetBufferPointer()),
            static_cast<UINT32>(bytecode->GetBufferSize()));
    }

    HRESULT CustomPixelShaderEffect::LoadShaderBytecode(const BYTE* data, UINT32 dataSize)
    {
        if (!data || dataSize == 0)
            return E_INVALIDARG;
        m_shaderBytecode.assign(data, data + dataSize);
        m_shaderDirty = true;
        return S_OK;
    }

    void CustomPixelShaderEffect::SetConstantBufferData(const BYTE* data, UINT32 dataSize)
    {
        m_constantBuffer.assign(data, data + dataSize);
        m_cbDirty = true;
    }

    // -----------------------------------------------------------------------
    // Convenience: pack PropertyValue map into constant buffer
    // -----------------------------------------------------------------------

    void CustomPixelShaderEffect::PackConstantBuffer(
        const std::map<std::wstring, Graph::PropertyValue>& properties,
        const std::vector<ShaderVariable>& variables,
        uint32_t cbSizeBytes)
    {
        // Allocate and zero the constant buffer.
        m_constantBuffer.resize(cbSizeBytes, 0);

        for (const auto& var : variables)
        {
            // Find a matching property by name.
            auto it = properties.find(var.name);
            if (it == properties.end())
                continue;

            // Write the value at the correct offset (guard against out-of-range offsets).
            if (var.offset >= cbSizeBytes)
                continue;

            BYTE* dest = m_constantBuffer.data() + var.offset;
            uint32_t remaining = cbSizeBytes - var.offset;

            std::visit([dest, remaining, &var](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                {
                    if (remaining >= sizeof(float))
                        memcpy(dest, &v, sizeof(float));
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    if (remaining >= sizeof(int32_t))
                        memcpy(dest, &v, sizeof(int32_t));
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    if (remaining >= sizeof(uint32_t))
                        memcpy(dest, &v, sizeof(uint32_t));
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    BOOL bval = v ? TRUE : FALSE;
                    if (remaining >= sizeof(BOOL))
                        memcpy(dest, &bval, sizeof(BOOL));
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    if (remaining >= sizeof(float) * 2)
                    {
                        memcpy(dest, &v.x, sizeof(float));
                        memcpy(dest + sizeof(float), &v.y, sizeof(float));
                    }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    if (remaining >= sizeof(float) * 3)
                    {
                        memcpy(dest, &v.x, sizeof(float));
                        memcpy(dest + sizeof(float), &v.y, sizeof(float));
                        memcpy(dest + sizeof(float) * 2, &v.z, sizeof(float));
                    }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    if (remaining >= sizeof(float) * 4)
                    {
                        memcpy(dest, &v.x, sizeof(float));
                        memcpy(dest + sizeof(float), &v.y, sizeof(float));
                        memcpy(dest + sizeof(float) * 2, &v.z, sizeof(float));
                        memcpy(dest + sizeof(float) * 3, &v.w, sizeof(float));
                    }
                }
                // std::wstring: not applicable to constant buffers — skip.
            }, it->second);
        }

        m_cbDirty = true;
    }
}
