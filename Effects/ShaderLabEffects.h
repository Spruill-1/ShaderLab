#pragma once

#include "pch.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Effects
{
    // Describes a pre-built ShaderLab effect with embedded HLSL.
    struct ShaderLabEffectDescriptor
    {
        std::wstring name;
        std::wstring category;      // "Analysis" or "Source"
        Graph::CustomShaderType shaderType{ Graph::CustomShaderType::PixelShader };

        // Embedded HLSL source — compiled on first use.
        std::string hlslSource;

        // Named texture inputs.
        std::vector<std::wstring> inputNames;

        // Declared parameters with defaults + UI metadata.
        std::vector<Graph::ParameterDefinition> parameters;

        // Analysis output fields (for compute shaders).
        std::vector<Graph::AnalysisFieldDescriptor> analysisFields;
        Graph::AnalysisOutputType analysisOutputType{ Graph::AnalysisOutputType::None };

        // Compute shader thread group dimensions.
        uint32_t threadGroupX{ 8 };
        uint32_t threadGroupY{ 8 };
        uint32_t threadGroupZ{ 1 };
    };

    // Registry of all ShaderLab pre-built effects.
    class ShaderLabEffects
    {
    public:
        static ShaderLabEffects& Instance();

        const ShaderLabEffectDescriptor* FindByName(std::wstring_view name) const;
        const std::vector<ShaderLabEffectDescriptor>& All() const { return m_effects; }
        std::vector<const ShaderLabEffectDescriptor*> ByCategory(std::wstring_view category) const;
        std::vector<std::wstring> Categories() const;

        // Create a fully-configured EffectNode from a descriptor.
        static Graph::EffectNode CreateNode(const ShaderLabEffectDescriptor& desc);

    private:
        ShaderLabEffects();
        void RegisterAll();

        std::vector<ShaderLabEffectDescriptor> m_effects;
    };

    // Shared HLSL color math functions (prepended to all ShaderLab shaders).
    const std::string& GetColorMathHLSL();
}
