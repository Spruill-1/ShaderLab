#pragma once

#include "pch.h"

namespace ShaderLab::Graph
{
    // Variant type for node property values.
    // Covers common D2D effect property types: scalars, vectors, colors, strings, enums.
    using PropertyValue = std::variant<
        float,
        int32_t,
        uint32_t,
        bool,
        std::wstring,
        winrt::Windows::Foundation::Numerics::float2,
        winrt::Windows::Foundation::Numerics::float3,
        winrt::Windows::Foundation::Numerics::float4
    >;

    // Human-readable tag for property type (used in JSON and UI).
    inline std::wstring PropertyValueTypeTag(const PropertyValue& value)
    {
        return std::visit([](auto&& v) -> std::wstring
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>)      return L"float";
            if constexpr (std::is_same_v<T, int32_t>)    return L"int";
            if constexpr (std::is_same_v<T, uint32_t>)   return L"uint";
            if constexpr (std::is_same_v<T, bool>)       return L"bool";
            if constexpr (std::is_same_v<T, std::wstring>) return L"string";
            if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>) return L"float2";
            if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>) return L"float3";
            if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>) return L"float4";
        }, value);
    }
}
