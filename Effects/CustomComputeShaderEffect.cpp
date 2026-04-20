#include "pch.h"
#include "CustomComputeShaderEffect.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    HRESULT CustomComputeShaderEffect::RegisterEffect(ID2D1Factory1* factory)
    {
        return RegisterWithInputCount(factory, CLSID_CustomComputeShader, 1);
    }

    HRESULT CustomComputeShaderEffect::RegisterWithInputCount(
        ID2D1Factory1* factory, REFCLSID clsid, UINT32 inputCount)
    {
        if (!factory || inputCount == 0 || inputCount > 8)
            return E_INVALIDARG;

        std::wstring xml =
            L"<?xml version='1.0'?>\r\n"
            L"<Effect>\r\n"
            L"  <Property name='DisplayName' type='string' value='Custom Compute Shader'/>\r\n"
            L"  <Property name='Author'      type='string' value='ShaderLab'/>\r\n"
            L"  <Property name='Category'    type='string' value='Custom'/>\r\n"
            L"  <Property name='Description' type='string' value='Runs a user-supplied compute shader.'/>\r\n"
            L"  <Inputs>\r\n";
        for (UINT32 i = 0; i < inputCount; ++i)
            xml += std::format(L"    <Input name='I{}'/>\r\n", i);
        xml += L"  </Inputs>\r\n"
               L"</Effect>\r\n";

        return factory->RegisterEffectFromString(
            clsid,
            xml.c_str(),
            nullptr,
            0,
            &CustomComputeShaderEffect::CreateFactory);
    }

    HRESULT CustomComputeShaderEffect::UnregisterEffect(ID2D1Factory1* factory)
    {
        if (!factory)
            return E_INVALIDARG;
        return factory->UnregisterEffect(CLSID_CustomComputeShader);
    }

    thread_local CustomComputeShaderEffect* CustomComputeShaderEffect::s_lastCreated = nullptr;

    HRESULT __stdcall CustomComputeShaderEffect::CreateFactory(IUnknown** effect)
    {
        auto* impl = new (std::nothrow) CustomComputeShaderEffect();
        *effect = static_cast<ID2D1EffectImpl*>(impl);
        s_lastCreated = impl;
        return *effect ? S_OK : E_OUTOFMEMORY;
    }

    CustomComputeShaderEffect::CustomComputeShaderEffect() = default;

    // -----------------------------------------------------------------------
    // IUnknown
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
            return E_POINTER;

        *ppv = nullptr;

        if (riid == __uuidof(ID2D1EffectImpl))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1ComputeTransform))
            *ppv = static_cast<ID2D1ComputeTransform*>(this);
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

    IFACEMETHODIMP_(ULONG) CustomComputeShaderEffect::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) CustomComputeShaderEffect::Release()
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return count;
    }

    // -----------------------------------------------------------------------
    // ID2D1EffectImpl
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);
        m_transformGraph.copy_from(transformGraph);

        // Check compute shader support (feature level 11_0+ required).
        if (!effectContext->IsShaderLoaded(CLSID_CustomComputeShader))
        {
            D2D1_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hardwareOptions{};
            HRESULT hr = effectContext->CheckFeatureSupport(
                D2D1_FEATURE_D3D10_X_HARDWARE_OPTIONS,
                &hardwareOptions,
                sizeof(hardwareOptions));

            if (FAILED(hr) || !hardwareOptions.computeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
            {
                // Compute shaders not supported on this hardware.
                return D2DERR_INSUFFICIENT_DEVICE_CAPABILITIES;
            }
        }

        return transformGraph->SetSingleTransformNode(
            static_cast<ID2D1ComputeTransform*>(this));
    }

    IFACEMETHODIMP CustomComputeShaderEffect::PrepareForRender(D2D1_CHANGE_TYPE /*changeType*/)
    {
        if (!m_computeInfo)
            return E_FAIL;

        // If the shader bytecode changed, load it into D2D.
        if (m_shaderDirty && !m_shaderBytecode.empty())
        {
            if (m_shaderGuid == GUID{})
                CoCreateGuid(&m_shaderGuid);

            HRESULT hr = m_effectContext->LoadComputeShader(
                m_shaderGuid,
                m_shaderBytecode.data(),
                static_cast<UINT32>(m_shaderBytecode.size()));

            if (SUCCEEDED(hr))
            {
                hr = m_computeInfo->SetComputeShader(m_shaderGuid);
            }

            if (FAILED(hr))
                return hr;

            m_shaderDirty = false;
        }

        // If the constant buffer data changed, push it to the GPU.
        if (m_cbDirty && !m_constantBuffer.empty())
        {
            HRESULT hr = m_computeInfo->SetComputeShaderConstantBuffer(
                m_constantBuffer.data(),
                static_cast<UINT32>(m_constantBuffer.size()));

            if (FAILED(hr))
                return hr;

            m_cbDirty = false;
        }

        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::SetGraph(ID2D1TransformGraph* transformGraph)
    {
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(
            static_cast<ID2D1ComputeTransform*>(this));
    }

    // -----------------------------------------------------------------------
    // ID2D1ComputeTransform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::SetComputeInfo(ID2D1ComputeInfo* computeInfo)
    {
        m_computeInfo.copy_from(computeInfo);

        if (!m_shaderBytecode.empty())
            m_shaderDirty = true;

        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::CalculateThreadgroups(
        const D2D1_RECT_L* outputRect,
        UINT32* dimensionX,
        UINT32* dimensionY,
        UINT32* dimensionZ)
    {
        // Calculate thread group count based on output dimensions and group size.
        UINT32 width  = static_cast<UINT32>(outputRect->right - outputRect->left);
        UINT32 height = static_cast<UINT32>(outputRect->bottom - outputRect->top);

        // Round up to cover the full output area.
        *dimensionX = (width  + m_threadGroupX - 1) / m_threadGroupX;
        *dimensionY = (height + m_threadGroupY - 1) / m_threadGroupY;
        *dimensionZ = m_threadGroupZ;

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1Transform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        if (inputRectCount > 0 && inputRects)
        {
            *outputRect = inputRects[0];
            for (UINT32 i = 1; i < inputRectCount; ++i)
            {
                outputRect->left   = (std::min)(outputRect->left,   inputRects[i].left);
                outputRect->top    = (std::min)(outputRect->top,    inputRects[i].top);
                outputRect->right  = (std::max)(outputRect->right,  inputRects[i].right);
                outputRect->bottom = (std::max)(outputRect->bottom, inputRects[i].bottom);
            }
            outputRect->left   = (std::max)(outputRect->left,   0L);
            outputRect->top    = (std::max)(outputRect->top,    0L);
            outputRect->right  = (std::min)(outputRect->right,  4096L);
            outputRect->bottom = (std::min)(outputRect->bottom, 4096L);
        }
        else
        {
            *outputRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        }

        *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        for (UINT32 i = 0; i < inputRectCount; ++i)
        {
            inputRects[i] = *outputRect;
        }
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L invalidInputRect,
        D2D1_RECT_L* invalidOutputRect) const
    {
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1TransformNode
    // -----------------------------------------------------------------------

    IFACEMETHODIMP_(UINT32) CustomComputeShaderEffect::GetInputCount() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Custom property accessors
    // -----------------------------------------------------------------------

    HRESULT CustomComputeShaderEffect::SetInputCount(UINT32 count)
    {
        if (count == m_inputCount)
            return S_OK;

        m_inputCount = count;

        if (m_transformGraph)
        {
            return m_transformGraph->SetSingleTransformNode(
                static_cast<ID2D1ComputeTransform*>(this));
        }
        return S_OK;
    }

    UINT32 CustomComputeShaderEffect::GetInputCountProp() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Host-facing API (outside D2D property system)
    // -----------------------------------------------------------------------

    HRESULT CustomComputeShaderEffect::LoadShaderBytecode(ID3DBlob* bytecode)
    {
        if (!bytecode)
            return E_INVALIDARG;
        return LoadShaderBytecode(
            static_cast<const BYTE*>(bytecode->GetBufferPointer()),
            static_cast<UINT32>(bytecode->GetBufferSize()));
    }

    HRESULT CustomComputeShaderEffect::LoadShaderBytecode(const BYTE* data, UINT32 dataSize)
    {
        if (!data || dataSize == 0)
            return E_INVALIDARG;
        m_shaderBytecode.assign(data, data + dataSize);
        m_shaderDirty = true;
        return S_OK;
    }

    void CustomComputeShaderEffect::SetConstantBufferData(const BYTE* data, UINT32 dataSize)
    {
        m_constantBuffer.assign(data, data + dataSize);
        m_cbDirty = true;
    }

    void CustomComputeShaderEffect::PackConstantBuffer(
        const std::map<std::wstring, Graph::PropertyValue>& properties,
        const std::vector<ShaderVariable>& variables,
        uint32_t cbSizeBytes)
    {
        m_constantBuffer.resize(cbSizeBytes, 0);

        for (const auto& var : variables)
        {
            auto it = properties.find(var.name);
            if (it == properties.end())
                continue;

            // Guard against out-of-range offsets.
            if (var.offset >= cbSizeBytes)
                continue;

            BYTE* dest = m_constantBuffer.data() + var.offset;
            uint32_t remaining = cbSizeBytes - var.offset;

            std::visit([dest, remaining](auto&& v)
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
            }, it->second);
        }

        m_cbDirty = true;
    }

    void CustomComputeShaderEffect::SetThreadGroupSize(UINT32 x, UINT32 y, UINT32 z)
    {
        m_threadGroupX = (std::max)(x, 1u);
        m_threadGroupY = (std::max)(y, 1u);
        m_threadGroupZ = (std::max)(z, 1u);
    }
}
