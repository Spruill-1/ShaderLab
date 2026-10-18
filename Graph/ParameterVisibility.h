#pragma once

#include "pch_engine.h"
#include "Graph/EffectNode.h"

// Conditional visibility of custom-effect parameters (ParameterDefinition::
// visibleWhen), read by the Properties panel and the canvas pin layout.
namespace ShaderLab::Graph
{
    // Evaluates a "ParamName op value" condition (op is ==, !=, <, <=, > or >=,
    // compared numerically; == and != within 0.001) against the node's
    // properties. True (visible) when the condition is empty, does not parse,
    // names a missing or non-numeric property, or holds.
    bool EvaluateVisibleWhen(
        const std::wstring& condition,
        const std::map<std::wstring, PropertyValue>& props);

    // Whether the panel and the canvas show property `key` of `node`. A node
    // without a custom effect shows every property; one with a custom effect
    // shows only declared parameters whose visibleWhen holds, so host-fed
    // backing values (Working Space) stay hidden.
    bool IsParameterShown(const EffectNode& node, const std::wstring& key);
}
