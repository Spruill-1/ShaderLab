#pragma once

#include "pch.h"

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
    class D3D11ComputeRunner
    {
    public:
        D3D11ComputeRunner() = default;

        // Initialize with a D3D11 device (caches device + immediate context).
        void Initialize(ID3D11Device* device);

        // Compile a compute shader from HLSL source.
        // Returns true on success. Call GetCompileError() on failure.
        bool CompileShader(const std::string& hlslSource);

        // Get the last compile error message.
        const std::wstring& GetCompileError() const { return m_compileError; }

        // Dispatch the compiled shader on an input texture.
        // cbufferData: user constant buffer bytes (Width/Height prepended automatically).
        // resultCount: number of float4 elements in the result buffer.
        // Returns the readback data (resultCount * 4 floats).
        std::vector<float> Dispatch(
            ID3D11Texture2D* inputTexture,
            const std::vector<BYTE>& cbufferData,
            uint32_t resultCount);

        bool IsInitialized() const { return m_device != nullptr; }
        bool HasShader() const { return m_shader != nullptr; }

    private:
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        winrt::com_ptr<ID3D11ComputeShader> m_shader;

        // Result buffer (RWStructuredBuffer<float4>)
        winrt::com_ptr<ID3D11Buffer> m_resultBuffer;
        winrt::com_ptr<ID3D11Buffer> m_stagingBuffer;
        winrt::com_ptr<ID3D11Buffer> m_cbuffer;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_resultUAV;
        uint32_t m_resultCount{ 0 };

        std::wstring m_compileError;

        void EnsureBuffers(uint32_t resultCount);
    };
}
