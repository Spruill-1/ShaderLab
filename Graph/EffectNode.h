#pragma once

#include "pch.h"
#include "NodeType.h"
#include "PropertyValue.h"

namespace ShaderLab::Graph
{
    // Describes a single input or output pin on a node.
    struct PinDescriptor
    {
        std::wstring name;
        uint32_t     index{ 0 };
    };

    // Shader type for custom effects.
    enum class CustomShaderType
    {
        PixelShader,
        ComputeShader,
    };

    // Describes a user-defined parameter for a custom effect.
    struct ParameterDefinition
    {
        std::wstring name;
        std::wstring typeName;       // "float", "float2", "float3", "float4", "int", "uint", "bool"
        PropertyValue defaultValue;
        float minValue{ 0.0f };
        float maxValue{ 1.0f };
        float step{ 0.01f };
    };

    // Identifies the type of non-image data produced by analysis/compute effects.
    enum class AnalysisOutputType
    {
        None,
        Histogram,
        FloatBuffer,
    };

    // Complete definition of a custom effect authored in the Effect Designer.
    // Stored per-node and serialized with the graph JSON.
    struct CustomEffectDefinition
    {
        CustomShaderType shaderType{ CustomShaderType::PixelShader };
        std::wstring hlslSource;                        // Full HLSL source code.
        std::vector<std::wstring> inputNames;           // Named inputs (textures).
        std::vector<ParameterDefinition> parameters;    // Declared cbuffer parameters.

        // Compute shader settings.
        uint32_t threadGroupX{ 8 };
        uint32_t threadGroupY{ 8 };
        uint32_t threadGroupZ{ 1 };

        // Analysis output metadata (for compute effects).
        AnalysisOutputType analysisOutputType{ AnalysisOutputType::None };
        uint32_t analysisOutputSize{ 256 };

        // Runtime: compiled bytecode (not serialized — recompiled from hlslSource on load).
        std::vector<uint8_t> compiledBytecode;

        // Runtime: unique shader GUID for this instance (avoids D2D shader ID collisions).
        GUID shaderGuid{};

        bool isCompiled() const { return !compiledBytecode.empty(); }
    };

    // Stores non-image output data from analysis/compute effects.
    // Read back from the GPU after the effect graph is evaluated.
    struct AnalysisOutput
    {
        AnalysisOutputType type{ AnalysisOutputType::None };
        std::vector<float> data;         // Bin values, buffer contents, etc.
        std::wstring       label;        // Human-readable description (e.g., "Red channel histogram").
        uint32_t           channelIndex{ 0 }; // For per-channel data (R=0, G=1, B=2, A=3).
    };

    // A node in the effect graph DAG.
    // Each node represents a D2D image source, a built-in D2D effect,
    // a custom pixel/compute shader, or the final output.
    struct EffectNode
    {
        uint32_t   id{ 0 };
        std::wstring name;
        NodeType   type{ NodeType::Source };

        // Canvas position for the visual graph editor (in DIPs).
        winrt::Windows::Foundation::Numerics::float2 position{ 0.0f, 0.0f };

        // Effect-specific properties (e.g., Gaussian blur StdDeviation, Flood Color).
        std::map<std::wstring, PropertyValue> properties;

        // Pin descriptors — filled when the node type is known.
        std::vector<PinDescriptor> inputPins;
        std::vector<PinDescriptor> outputPins;

        // For built-in D2D effects: the CLSID used to create the effect.
        std::optional<GUID> effectClsid;

        // For custom shaders: path to the HLSL source file.
        std::optional<std::wstring> shaderPath;

        // For custom effects authored in the Effect Designer.
        std::optional<CustomEffectDefinition> customEffect;

        // Cached output after evaluation (non-owning, managed by the render engine).
        // Not serialized.
        ID2D1Image* cachedOutput{ nullptr };

        // Analysis/compute output data (read back after evaluation). Not serialized.
        AnalysisOutput analysisOutput;

        // Runtime error message (e.g., effect creation failure). Not serialized.
        std::wstring runtimeError;

        // Dirty flag — set when properties change, cleared after evaluation.
        bool dirty{ true };
    };
}
