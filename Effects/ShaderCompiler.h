#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/PropertyValue.h"

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
        SHADERLAB_API std::wstring ErrorMessage() const;
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
    class SHADERLAB_API ShaderCompiler
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

    // ---- Typed PropertyValue -> cbuffer pack ---------------------------------
    //
    // Packs a single PropertyValue into a constant-buffer byte slot,
    // converting via the declared HLSL type so that:
    //   * float  PropertyValue -> uint cbuffer slot: static_cast<uint32_t>
    //   * float  PropertyValue -> int  cbuffer slot: static_cast<int32_t>
    //   * float  PropertyValue -> bool cbuffer slot: BOOL(value > 0.5)
    //   * uint/int/bool PropertyValue -> matching slot: direct copy
    //   * float vector PropertyValue -> matching N-component slot: direct copy
    // and the legacy "memcpy of float bit-pattern produces nonsense uints"
    // bug class can't recur (CHANGELOG 1.3.9).
    //
    // This was previously implemented inline in three places (D3D11 compute
    // dispatch, CustomPixelShaderEffect, CustomComputeShaderEffect) and only
    // the D3D11 compute site did the typed conversion. Phase 3 unifies them.
    //
    // Returns true if any bytes were written. `dest` may not be null;
    // `remaining` is the remaining cbuffer bytes from `dest`.
    SHADERLAB_API bool PackPropertyToCBuffer(
        BYTE* dest, uint32_t remaining,
        D3D_SHADER_VARIABLE_TYPE hlslType,
        uint32_t hlslColumns,
        const Graph::PropertyValue& value);
}
