#pragma once

#include "pch.h"
#include "../Effects/ShaderCompiler.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Controls
{
    // Describes a single auto-generated property derived from D3DReflect.
    // Used by the properties panel to create UI controls dynamically.
    struct AutoProperty
    {
        std::wstring name;
        Graph::PropertyValue defaultValue;

        // Shader variable metadata (for layout / tooltip).
        Effects::ShaderVariable variable;
    };

    // Compile result with additional metadata for the editor UI.
    struct EditorCompileResult
    {
        bool         succeeded{ false };
        std::wstring errorText;           // Human-readable compile errors
        uint32_t     errorLine{ 0 };      // Line number of first error (1-based, 0 = unknown)

        // Shader bytecode (valid only if succeeded).
        winrt::com_ptr<ID3DBlob> bytecode;

        // Auto-discovered properties from constant buffer reflection.
        std::vector<AutoProperty> autoProperties;

        // Reflection data (constant buffers, texture SRV count).
        Effects::ShaderReflectionResult reflection;
    };

    // Shader editor controller — bridges the HLSL text editor (TextBox) with
    // the shader compilation pipeline and effect graph.
    //
    // Responsibilities:
    //   - Compile HLSL on demand (Ctrl+Enter, save, or explicit call)
    //   - Parse D3DCompile errors and map to line numbers
    //   - Run D3DReflect on successful compilation to discover cbuffers
    //   - Generate AutoProperty list for dynamic property UI
    //   - Manage shader type detection (pixel vs compute)
    //   - Track file path for disk-based shaders (optional)
    //
    // The controller does NOT own the TextBox or D2D effect — the host
    // (MainWindow) wires these together.
    class ShaderEditorController
    {
    public:
        ShaderEditorController() = default;

        // Compile HLSL source text. Returns a result with bytecode and
        // auto-generated properties on success, or error info on failure.
        // shaderType: "ps_5_0" for pixel shaders, "cs_5_0" for compute.
        EditorCompileResult Compile(
            const std::string& hlslSource,
            const std::string& shaderType = "ps_5_0",
            const std::string& entryPoint = "main");

        // Compile from file on disk.
        EditorCompileResult CompileFromFile(
            const std::filesystem::path& hlslPath,
            const std::string& shaderType = "ps_5_0",
            const std::string& entryPoint = "main");

        // Get the last compilation result (cached).
        const EditorCompileResult& LastResult() const { return m_lastResult; }

        // Whether the last compilation succeeded.
        bool HasValidShader() const { return m_lastResult.succeeded; }

        // Get the default HLSL template for a new pixel shader.
        static std::string DefaultPixelShaderTemplate();

        // Get the default HLSL template for a new compute shader.
        static std::string DefaultComputeShaderTemplate();

    private:
        // Parse D3DCompile error output to extract line number.
        static uint32_t ParseErrorLine(const std::wstring& errorText);

        // Build auto-properties from reflection data.
        static std::vector<AutoProperty> BuildAutoProperties(
            const Effects::ShaderReflectionResult& reflection);

        // Map a D3D shader variable type + dimensions to a default PropertyValue.
        static Graph::PropertyValue DefaultValueForVariable(
            const Effects::ShaderVariable& variable);

        EditorCompileResult m_lastResult;
    };
}
