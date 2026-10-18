#include "pch_engine.h"
#include "ParameterVisibility.h"

#include <cerrno>
#include <cwchar>

namespace ShaderLab::Graph
{
    namespace
    {
        // Parses the right-hand side the way std::stof does (leading
        // whitespace skipped, trailing text ignored); nullopt where it throws.
        std::optional<float> ParseNumber(const std::wstring& text)
        {
            const wchar_t* begin = text.c_str();
            wchar_t* end = nullptr;
            errno = 0;
            const float value = std::wcstof(begin, &end);
            if (end == begin || errno == ERANGE) return std::nullopt;
            return value;
        }

        std::optional<float> NumericValue(const PropertyValue& value)
        {
            if (auto* f = std::get_if<float>(&value)) return *f;
            if (auto* i = std::get_if<int32_t>(&value)) return static_cast<float>(*i);
            if (auto* u = std::get_if<uint32_t>(&value)) return static_cast<float>(*u);
            if (auto* b = std::get_if<bool>(&value)) return *b ? 1.0f : 0.0f;
            return std::nullopt;
        }
    }

    bool EvaluateVisibleWhen(
        const std::wstring& condition,
        const std::map<std::wstring, PropertyValue>& props)
    {
        if (condition.empty()) return true;

        // The first operator in this list found anywhere in the condition wins.
        struct Operator { const wchar_t* text; size_t length; };
        static constexpr Operator scOperators[] = {
            { L"==", 2 }, { L"!=", 2 }, { L"<=", 2 }, { L">=", 2 }, { L"<", 1 }, { L">", 1 }
        };
        size_t opPos = std::wstring::npos;
        size_t opLen = 0;
        for (const auto& op : scOperators)
        {
            const size_t pos = condition.find(op.text);
            if (pos != std::wstring::npos) { opPos = pos; opLen = op.length; break; }
        }
        if (opPos == std::wstring::npos) return true;

        std::wstring paramName = condition.substr(0, opPos);
        while (!paramName.empty() && paramName.back() == L' ') paramName.pop_back();
        const std::wstring op = condition.substr(opPos, opLen);
        std::wstring valueText = condition.substr(opPos + opLen);
        while (!valueText.empty() && valueText.front() == L' ') valueText.erase(valueText.begin());

        const auto rhs = ParseNumber(valueText);
        if (!rhs) return true;

        const auto it = props.find(paramName);
        if (it == props.end()) return true;
        const auto lhs = NumericValue(it->second);
        if (!lhs) return true;

        if (op == L"==") return std::abs(*lhs - *rhs) < 0.001f;
        if (op == L"!=") return std::abs(*lhs - *rhs) >= 0.001f;
        if (op == L"<")  return *lhs < *rhs;
        if (op == L"<=") return *lhs <= *rhs;
        if (op == L">")  return *lhs > *rhs;
        if (op == L">=") return *lhs >= *rhs;
        return true;
    }

    bool IsParameterShown(const EffectNode& node, const std::wstring& key)
    {
        if (!node.customEffect.has_value()) return true;
        const auto& params = node.customEffect->parameters;
        const bool declared = std::any_of(params.begin(), params.end(),
            [&key](const ParameterDefinition& p) { return p.name == key; });
        if (!declared) return false;
        for (const auto& p : params)
            if (p.name == key && !p.visibleWhen.empty())
                return EvaluateVisibleWhen(p.visibleWhen, node.properties);
        return true;
    }
}
