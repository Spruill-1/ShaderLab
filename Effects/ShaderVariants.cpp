#include "pch_engine.h"
#include "ShaderVariants.h"
#include "BytecodeCache.h"

#include <cmath>

namespace ShaderLab::Effects
{
    namespace
    {
        struct Option
        {
            std::string  name;      // UTF-8
            std::wstring wideName;
            uint32_t     valueCount;
        };

        std::vector<Option> SpecializedOptions(const Graph::CustomEffectDefinition& def)
        {
            std::vector<Option> options;
            for (const auto& param : def.parameters)
            {
                if (!param.specialize || param.gpuBindable || param.enumLabels.empty()) continue;
                if (options.size() >= cMaxSpecializedOptions) break;
                Option option;
                option.wideName = param.name;
                option.name.assign(param.name.begin(), param.name.end());   // HLSL identifiers are ASCII
                option.valueCount = static_cast<uint32_t>((std::min<size_t>)(param.enumLabels.size(), cMaxOptionValues));
                options.push_back(std::move(option));
            }
            return options;
        }

        // The option index a property value names, or -1 when it names none.
        int OptionIndex(const Graph::PropertyValue& value, uint32_t valueCount)
        {
            double number = 0.0;
            if (auto* floatValue = std::get_if<float>(&value))         number = *floatValue;
            else if (auto* uintValue = std::get_if<uint32_t>(&value))  number = *uintValue;
            else if (auto* intValue = std::get_if<int32_t>(&value))    number = *intValue;
            else if (auto* boolValue = std::get_if<bool>(&value))      number = *boolValue ? 1.0 : 0.0;
            else return -1;
            const double rounded = std::round(number);
            // Only an exact option picks a variant: the shader's own cast would
            // truncate a fractional value differently. A value just below an
            // option truncates to the one before it in the generic build, so it
            // goes there too.
            if (!std::isfinite(number) || std::abs(number - rounded) > 1e-4 || number < rounded ||
                rounded < 0.0 || rounded >= valueCount) return -1;
            return static_cast<int>(rounded);
        }
    }

    std::vector<std::string> SpecializedOptionNames(const Graph::CustomEffectDefinition& def)
    {
        std::vector<std::string> names;
        for (auto& option : SpecializedOptions(def)) names.push_back(std::move(option.name));
        return names;
    }

    uint64_t OptionKeyFor(const Graph::CustomEffectDefinition& def,
                          const std::map<std::wstring, Graph::PropertyValue>& values)
    {
        const auto options = SpecializedOptions(def);
        uint64_t key = 0;
        for (size_t i = 0; i < options.size(); ++i)
        {
            auto it = values.find(options[i].wideName);
            if (it == values.end()) return 0;              // no value: generic
            const int optionIndex = OptionIndex(it->second, options[i].valueCount);
            if (optionIndex < 0) return 0;                 // not an option: generic
            key |= static_cast<uint64_t>(optionIndex + 1) << (8u * i);
        }
        return key;
    }

    size_t OptionCombinationCount(const Graph::CustomEffectDefinition& def)
    {
        const auto options = SpecializedOptions(def);
        if (options.empty()) return 0;
        size_t combinations = 1;
        for (const auto& option : options)
        {
            combinations *= option.valueCount;
            if (combinations > 1'000'000) break;
        }
        return combinations;
    }

    std::vector<uint64_t> AllOptionKeys(const Graph::CustomEffectDefinition& def)
    {
        const auto options = SpecializedOptions(def);
        std::vector<uint64_t> keys;
        const size_t total = OptionCombinationCount(def);
        if (total == 0 || total > cMaxOptionCombinations) return keys;
        keys.reserve(total);
        // Count through every combination like an odometer, one digit per option.
        std::vector<uint32_t> digits(options.size(), 0);
        for (size_t combination = 0; combination < total; ++combination)
        {
            uint64_t key = 0;
            for (size_t i = 0; i < options.size(); ++i)
                key |= static_cast<uint64_t>(digits[i] + 1) << (8u * i);
            keys.push_back(key);
            for (size_t i = 0; i < options.size(); ++i)
            {
                if (++digits[i] < options[i].valueCount) break;
                digits[i] = 0;
            }
        }
        return keys;
    }

    std::vector<std::pair<std::string, std::string>> GenericVariantMacros(const Graph::CustomEffectDefinition& def)
    {
        std::vector<std::pair<std::string, std::string>> macros;
        for (const auto& param : def.parameters)
            if (param.gpuBindable)
                macros.push_back({ "_SLPARAM_" + std::string(param.name.begin(), param.name.end()) + "_GPU", "0" });
        for (const auto& name : SpecializedOptionNames(def))
        {
            macros.push_back({ "_SLOPT_" + name + "_MODE", "0" });
            macros.push_back({ "_SLOPT_" + name, "0" });
        }
        return macros;
    }
}
