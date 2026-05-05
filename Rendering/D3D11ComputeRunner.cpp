#include "pch_engine.h"
#include "D3D11ComputeRunner.h"

namespace ShaderLab::Rendering
{
    void D3D11ComputeRunner::Initialize(ID3D11Device* device)
    {
        if (!device) return;
        m_device.copy_from(device);
        m_device->GetImmediateContext(m_context.put());
    }

    bool D3D11ComputeRunner::CompileShader(const std::string& hlslSource)
    {
        m_shader = nullptr;
        m_bytecode.clear();
        m_compileError.clear();

        winrt::com_ptr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(
            hlslSource.c_str(), hlslSource.size(),
            "D3D11ComputeEffect", nullptr, nullptr,
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

        return true;
    }

    void D3D11ComputeRunner::EnsureBuffers(uint32_t resultCount)
    {
        if (m_resultCount == resultCount && m_resultBuffer) return;
        m_resultCount = resultCount;
        m_resultBuffer = nullptr;
        m_stagingBuffer = nullptr;
        m_resultUAV = nullptr;
        m_cbuffer = nullptr;

        uint32_t byteSize = (std::max)(resultCount * 16u, 16u); // 16 bytes per float4

        // Structured buffer for results.
        D3D11_BUFFER_DESC bufDesc{};
        bufDesc.ByteWidth = byteSize;
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
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

        // Staging buffer for readback.
        bufDesc.Usage = D3D11_USAGE_STAGING;
        bufDesc.BindFlags = 0;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        m_device->CreateBuffer(&bufDesc, nullptr, m_stagingBuffer.put());

        // Constant buffer (256 bytes max — room for Width, Height + user params).
        bufDesc.ByteWidth = 256;
        bufDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufDesc.MiscFlags = 0;
        bufDesc.StructureByteStride = 0;
        m_device->CreateBuffer(&bufDesc, nullptr, m_cbuffer.put());
    }

    std::vector<float> D3D11ComputeRunner::Dispatch(
        ID3D11Texture2D* inputTexture,
        const std::vector<BYTE>& cbufferData,
        uint32_t resultCount)
    {
        std::vector<float> result;
        if (!m_shader || !m_context || !inputTexture || resultCount == 0)
            return result;

        // Get texture dimensions.
        D3D11_TEXTURE2D_DESC texDesc{};
        inputTexture->GetDesc(&texDesc);

        // Ensure buffers are the right size.
        EnsureBuffers(resultCount);
        if (!m_resultBuffer || !m_resultUAV || !m_cbuffer) return result;

        // Create SRV for input.
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        HRESULT hr = m_device->CreateShaderResourceView(inputTexture, &srvDesc, srv.put());
        if (FAILED(hr)) return result;

        // Pack cbuffer: Width, Height (8 bytes) + user data.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = m_context->Map(m_cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            memset(mapped.pData, 0, 256);
            auto* cb = static_cast<BYTE*>(mapped.pData);
            // Auto-inject Width, Height.
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

        // Clear result buffer.
        uint32_t clearValues[4] = { 0, 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_resultUAV.get(), clearValues);

        // Dispatch.
        m_context->CSSetShader(m_shader.get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { srv.get() };
        m_context->CSSetShaderResources(0, 1, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_resultUAV.get() };
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ID3D11Buffer* cbs[] = { m_cbuffer.get() };
        m_context->CSSetConstantBuffers(0, 1, cbs);

        m_context->Dispatch(1, 1, 1);

        // Clear shader state.
        ID3D11ShaderResourceView* nullSRV[] = { nullptr };
        m_context->CSSetShaderResources(0, 1, nullSRV);
        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        m_context->CSSetShader(nullptr, nullptr, 0);

        // Readback.
        m_context->CopyResource(m_stagingBuffer.get(), m_resultBuffer.get());
        hr = m_context->Map(m_stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            const float* data = static_cast<const float*>(mapped.pData);
            result.assign(data, data + resultCount * 4);
            m_context->Unmap(m_stagingBuffer.get(), 0);
        }

        return result;
    }
}
