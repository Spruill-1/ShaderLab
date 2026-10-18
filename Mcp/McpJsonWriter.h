#pragma once

// Builds JSON text for MCP responses. Commas, string escaping and
// non-finite numbers are handled here, so a body built with it always
// parses. Hand-concatenated JSON is what this replaces.

#include "McpTypes.h"

#include <concepts>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderLab::Mcp
{
    class JsonWriter
    {
    public:
        JsonWriter& BeginObject() { BeforeValue(); m_out += '{'; m_first.push_back(true); return *this; }
        JsonWriter& EndObject()   { m_out += '}'; m_first.pop_back(); return *this; }
        JsonWriter& BeginArray()  { BeforeValue(); m_out += '['; m_first.push_back(true); return *this; }
        JsonWriter& EndArray()    { m_out += ']'; m_first.pop_back(); return *this; }

        // The next value is this member's.
        JsonWriter& Key(std::string_view key)
        {
            Separate();
            m_out += '"';
            m_out += JsonEscape(key);
            m_out += "\":";
            m_afterKey = true;
            return *this;
        }

        JsonWriter& String(std::string_view utf8)
        {
            BeforeValue();
            m_out += '"';
            m_out += JsonEscape(utf8);
            m_out += '"';
            return *this;
        }
        JsonWriter& String(std::wstring_view text) { return String(WideToUtf8(text)); }
        JsonWriter& String(const char* utf8) { return String(std::string_view(utf8)); }
        JsonWriter& String(const wchar_t* text) { return String(std::wstring_view(text)); }

        JsonWriter& Bool(bool value) { BeforeValue(); m_out += value ? "true" : "false"; return *this; }
        JsonWriter& Null() { BeforeValue(); m_out += "null"; return *this; }

        template <std::integral T>
            requires (!std::same_as<T, bool>)
        JsonWriter& Int(T value) { BeforeValue(); m_out += std::format("{}", value); return *this; }

        // Six decimals; null when not finite.
        JsonWriter& Float(double value) { BeforeValue(); m_out += JsonFloat(value); return *this; }

        // `decimals` places; null when not finite.
        JsonWriter& Fixed(double value, int decimals)
        {
            BeforeValue();
            m_out += std::isfinite(value) ? std::format("{:.{}f}", value, decimals) : std::string("null");
            return *this;
        }

        // The shortest text that reads back as the same float; null when not finite.
        JsonWriter& Shortest(float value)
        {
            BeforeValue();
            m_out += std::isfinite(value) ? std::format("{}", value) : std::string("null");
            return *this;
        }

        // A value that is already JSON text.
        JsonWriter& Raw(std::string_view json) { BeforeValue(); m_out += json; return *this; }

        const std::string& Text() const { return m_out; }
        std::string Take() { return std::move(m_out); }

    private:
        void Separate()
        {
            if (m_first.empty()) return;
            if (!m_first.back()) m_out += ',';
            m_first.back() = false;
        }

        void BeforeValue()
        {
            if (m_afterKey) { m_afterKey = false; return; }
            Separate();
        }

        std::string       m_out;
        std::vector<bool> m_first;   // per open container: nothing written yet
        bool              m_afterKey{ false };
    };

    // What kind of failure an error response reports, so a client can tell a
    // bad argument from a missing node, a node that is not ready yet or a
    // server fault without parsing the message.
    enum class ErrorCode
    {
        BadArgument,
        NotFound,
        NotReady,
        Busy,
        Internal,
    };

    inline const char* ErrorCodeName(ErrorCode code)
    {
        switch (code)
        {
        case ErrorCode::BadArgument: return "bad_argument";
        case ErrorCode::NotFound:    return "not_found";
        case ErrorCode::NotReady:    return "not_ready";
        case ErrorCode::Busy:        return "busy";
        case ErrorCode::Internal:    return "internal";
        }
        return "internal";
    }

    inline ErrorCode ErrorCodeForStatus(uint16_t status)
    {
        switch (status)
        {
        case 400: return ErrorCode::BadArgument;
        case 404: return ErrorCode::NotFound;
        case 409: return ErrorCode::NotReady;
        case 503: return ErrorCode::Busy;
        default:  return ErrorCode::Internal;
        }
    }

    inline Response JsonResponse(uint16_t status, std::string body)
    {
        Response response;
        response.statusCode = status;
        response.body = std::move(body);
        return response;
    }

    // Starts an error body: {"error":<message>,"code":<code>. The caller may
    // add members and must close it.
    inline JsonWriter BeginErrorBody(uint16_t status, std::string_view message)
    {
        JsonWriter json;
        json.BeginObject()
            .Key("error").String(message)
            .Key("code").String(ErrorCodeName(ErrorCodeForStatus(status)));
        return json;
    }

    // {"error":<message>,"code":<code>}; message is raw text.
    inline Response ErrorResponse(uint16_t status, std::string_view message)
    {
        return JsonResponse(status, BeginErrorBody(status, message).EndObject().Take());
    }
}
