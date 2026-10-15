#pragma once

// Readers for MCP tool arguments, shared by the engine routes, the GUI's own
// routes and headless. Untyped (`{}`-schema) arguments arrive from Claude Code
// as JSON strings ("203", "true"), so each reader accepts its own JSON type
// or a string spelling it.
//
// A missing key returns nullopt and leaves `error` alone. A present key whose
// value cannot be read returns nullopt and sets `error` unless an earlier
// reader already did; routes answer 400. The Require* forms also set `error`
// for a missing key.

#include <winrt/Windows.Data.Json.h>
#include "McpTypes.h"
#include "../../Graph/PropertyWrite.h"

namespace ShaderLab::Mcp
{
    namespace McpJson = winrt::Windows::Data::Json;

    inline bool HasArg(const McpJson::JsonObject& args, std::wstring_view key)
    {
        return args && args.HasKey(key);
    }

    // Keeps the first error, so a route reports the first bad argument.
    inline void SetArgError(std::string& error, std::string message)
    {
        if (error.empty()) error = std::move(message);
    }

    // The request body as a JSON object; false when it is not one.
    inline bool ParseObjectBody(const std::string& body, McpJson::JsonObject& out)
    {
        return McpJson::JsonObject::TryParse(winrt::to_hstring(body), out);
    }

    // A whole non-negative number in 32 bits written as decimal digits and
    // nothing else, such as the id in /graph/node/{id}.
    inline std::optional<uint32_t> ParseUintText(std::wstring_view text)
    {
        if (text.empty() || text.size() > 10) return std::nullopt;
        uint64_t value = 0;
        for (wchar_t c : text)
        {
            if (c < L'0' || c > L'9') return std::nullopt;
            value = value * 10 + static_cast<uint64_t>(c - L'0');
        }
        if (value > UINT32_MAX) return std::nullopt;
        return static_cast<uint32_t>(value);
    }

    inline std::optional<bool> ReadBoolArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        if (!HasArg(args, key)) return std::nullopt;
        const auto value = args.GetNamedValue(key);
        switch (value.ValueType())
        {
        case McpJson::JsonValueType::Boolean: return value.GetBoolean();
        case McpJson::JsonValueType::Number:  return value.GetNumber() != 0.0;
        case McpJson::JsonValueType::String:
            if (auto flag = Graph::ParseBool(value.GetString())) return flag;
            break;
        default: break;
        }
        SetArgError(error, WideToUtf8(key) + " must be a boolean (true/false, 1/0, on/off, yes/no)");
        return std::nullopt;
    }

    inline std::optional<double> ReadNumberArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        if (!HasArg(args, key)) return std::nullopt;
        const auto value = args.GetNamedValue(key);
        if (value.ValueType() == McpJson::JsonValueType::Number) return value.GetNumber();
        if (value.ValueType() == McpJson::JsonValueType::String)
            if (auto number = Graph::ParseNumber(value.GetString())) return number;
        SetArgError(error, WideToUtf8(key) + " must be a number");
        return std::nullopt;
    }

    // A non-negative whole number that fits in 32 bits.
    inline std::optional<uint32_t> ReadUintArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        auto number = ReadNumberArg(args, key, error);
        if (!number) return std::nullopt;
        if (*number < 0.0 || *number > static_cast<double>(UINT32_MAX) || std::floor(*number) != *number)
        {
            SetArgError(error, WideToUtf8(key) + " must be a non-negative integer");
            return std::nullopt;
        }
        return static_cast<uint32_t>(*number);
    }

    // A whole number that fits in a signed 32-bit integer.
    inline std::optional<int32_t> ReadIntArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        auto number = ReadNumberArg(args, key, error);
        if (!number) return std::nullopt;
        if (*number < static_cast<double>(INT32_MIN) || *number > static_cast<double>(INT32_MAX) ||
            std::floor(*number) != *number)
        {
            SetArgError(error, WideToUtf8(key) + " must be an integer");
            return std::nullopt;
        }
        return static_cast<int32_t>(*number);
    }

    inline std::optional<std::wstring> ReadStringArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        if (!HasArg(args, key)) return std::nullopt;
        const auto value = args.GetNamedValue(key);
        if (value.ValueType() == McpJson::JsonValueType::String) return std::wstring(value.GetString());
        SetArgError(error, WideToUtf8(key) + " must be a string");
        return std::nullopt;
    }

    // An object member; anything else sets `error`.
    inline std::optional<McpJson::JsonObject> ReadObjectArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        if (!HasArg(args, key)) return std::nullopt;
        const auto value = args.GetNamedValue(key);
        if (value.ValueType() == McpJson::JsonValueType::Object) return value.GetObject();
        SetArgError(error, WideToUtf8(key) + " must be an object");
        return std::nullopt;
    }

    // An array member; anything else sets `error`.
    inline std::optional<McpJson::JsonArray> ReadArrayArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        if (!HasArg(args, key)) return std::nullopt;
        const auto value = args.GetNamedValue(key);
        if (value.ValueType() == McpJson::JsonValueType::Array) return value.GetArray();
        SetArgError(error, WideToUtf8(key) + " must be an array");
        return std::nullopt;
    }

    template <class T>
    std::optional<T> RequireArg(std::optional<T> value, const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        if (!value && !HasArg(args, key)) SetArgError(error, WideToUtf8(key) + " is required");
        return value;
    }

    inline std::optional<uint32_t> RequireUintArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        return RequireArg(ReadUintArg(args, key, error), args, key, error);
    }

    inline std::optional<int32_t> RequireIntArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        return RequireArg(ReadIntArg(args, key, error), args, key, error);
    }

    inline std::optional<double> RequireNumberArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        return RequireArg(ReadNumberArg(args, key, error), args, key, error);
    }

    inline std::optional<std::wstring> RequireStringArg(const McpJson::JsonObject& args, std::wstring_view key, std::string& error)
    {
        return RequireArg(ReadStringArg(args, key, error), args, key, error);
    }

    // A property value for Graph::WriteProperty. Null, objects and arrays
    // holding anything but numbers are refused.
    inline std::optional<Graph::UntypedValue> ToUntypedValue(const McpJson::IJsonValue& value, std::string& error)
    {
        switch (value.ValueType())
        {
        case McpJson::JsonValueType::Number:  return Graph::UntypedValue{ value.GetNumber() };
        case McpJson::JsonValueType::Boolean: return Graph::UntypedValue{ value.GetBoolean() };
        case McpJson::JsonValueType::String:  return Graph::UntypedValue{ std::wstring(value.GetString()) };
        case McpJson::JsonValueType::Array:
        {
            std::vector<double> numbers;
            for (const auto& element : value.GetArray())
            {
                if (element.ValueType() != McpJson::JsonValueType::Number)
                {
                    SetArgError(error, "array values must be numbers");
                    return std::nullopt;
                }
                numbers.push_back(element.GetNumber());
            }
            return Graph::UntypedValue{ std::move(numbers) };
        }
        default:
            SetArgError(error, "Unsupported value type");
            return std::nullopt;
        }
    }
}
