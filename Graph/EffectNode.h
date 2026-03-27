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

        // Cached output after evaluation (non-owning, managed by the render engine).
        // Not serialized.
        ID2D1Image* cachedOutput{ nullptr };

        // Dirty flag — set when properties change, cleared after evaluation.
        bool dirty{ true };
    };
}
