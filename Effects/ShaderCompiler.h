#pragma once

#include "pch.h"

namespace ShaderLab::Effects
{
    // Describes a single constant buffer variable discovered via D3DReflect.
    struct ShaderVariable
    {
        std::wstring name;
        uint32_t     offset{ 0 };       // byte offset in the constant buffer
        uint32_t     size{ 0 };          // byte size
        D3D_SHADER_VARIABLE_TYPE type{ D3D_SVT_FLOAT };
        uint32_t     rows{ 0 };
        uint32_t     columns{ 0 };
    };

    // Describes a constant buffer discovered via D3DReflect.
    struct ShaderConstantBuffer
    {
        std::wstring name;
        uint32_t     sizeBytes{ 0 };
        std::vector<ShaderVariable> variables;
    };

    // Compilation result.
    struct ShaderCompileResult
    {
        winrt::com_ptr<ID3DBlob> bytecode;      // Compiled shader blob (nullptr on failure)
        winrt::com_ptr<ID3DBlob> errors;         // Error/warning messages (may be set even on success)
        bool                     succeeded{ false };

        // Human-readable error string (empty on success).
        std::wstring ErrorMessage() const;
    };

    // Reflection result.
    struct ShaderReflectionResult
    {
        std::vector<ShaderConstantBuffer> constantBuffers;
        uint32_t                          boundResources{ 0 };  // SRV/UAV count
        uint32_t                          inputCount{ 0 };      // texture inputs
    };

    // HLSL shader compilation and reflection utilities.
    // Wraps D3DCompile and D3DReflect for use by the custom effect system.
    class ShaderCompiler
    {
    public:
        // Compile an HLSL file from disk.
        // entryPoint: e.g. "main"
        // target:     e.g. "ps_5_0" or "cs_5_0"
        static ShaderCompileResult CompileFromFile(
            const std::filesystem::path& hlslPath,
            const std::string& entryPoint = "main",
            const std::string& target = "ps_5_0");

        // Compile HLSL source from a string.
        static ShaderCompileResult CompileFromString(
            const std::string& hlslSource,
            const std::string& sourceName,
            const std::string& entryPoint = "main",
            const std::string& target = "ps_5_0");

        // Reflect a compiled shader to discover constant buffers and bindings.
        static ShaderReflectionResult Reflect(ID3DBlob* bytecode);
        static ShaderReflectionResult Reflect(const std::vector<uint8_t>& bytecode);
    };
}
