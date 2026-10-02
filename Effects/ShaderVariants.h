#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectNode.h"

// Option specialisation: which parameters of an effect are compiled into
// shader variants, and which variant a node's values select. A parameter
// qualifies when it is marked `specialize`, has enumLabels and is not
// gpuBindable. The first cMaxSpecializedOptions that qualify are used, in
// declaration order. A value that is not an exact option selects the generic
// build, which reads the option from the cbuffer.
namespace ShaderLab::Effects
{
    // UTF-8 names of the specialised parameters, in option key order.
    SHADERLAB_API std::vector<std::string> SpecializedOptionNames(const Graph::CustomEffectDefinition& def);

    // The option key a node's values select (0 = generic).
    SHADERLAB_API uint64_t OptionKeyFor(const Graph::CustomEffectDefinition& def,
                                        const std::map<std::wstring, Graph::PropertyValue>& values);

    // Every fully specialised option key. Empty when nothing is specialised
    // or there are more than cMaxOptionCombinations combinations.
    inline constexpr size_t cMaxOptionCombinations = 64;
    SHADERLAB_API std::vector<uint64_t> AllOptionKeys(const Graph::CustomEffectDefinition& def);

    // Number of fully specialised combinations, 0 when nothing is
    // specialised. Stops multiplying once past a million.
    SHADERLAB_API size_t OptionCombinationCount(const Graph::CustomEffectDefinition& def);

    // (name, value) macros for a generic compile outside the evaluator:
    // every gpu-bindable parameter in cbuffer mode, every option read at
    // run time.
    SHADERLAB_API std::vector<std::pair<std::string, std::string>>
        GenericVariantMacros(const Graph::CustomEffectDefinition& def);
}
