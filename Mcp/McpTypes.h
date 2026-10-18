#pragma once

// Transport-neutral MCP types: the Response every route and sink returns,
// and the JSON string helpers. Routes depend on this header, never on a
// transport.

#include <string>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <format>   // JsonFloat
#include <cmath>    // std::isfinite
#include "Engine/Utf8.h"

namespace ShaderLab::Mcp
{
    using ::ShaderLab::WideToUtf8;
    using ::ShaderLab::Utf8ToWide;

    // The one JSON string escaper: everything embedded in a JSON string
    // literal goes through it, raw control characters included.
    inline std::string JsonEscape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                }
                else
                {
                    out += c;
                }
            }
        }
        return out;
    }

    struct Response
    {
        uint16_t    statusCode{ 200 };
        std::string body;
        std::string contentType{ "application/json" };

        // No reply at all, as opposed to an empty one: a JSON-RPC
        // notification must produce zero bytes on the stream, while an
        // empty-body reply is still a framed message. Handlers that answer
        // notifications return None(); transports check noReply first.
        bool        noReply{ false };

        static Response None()
        {
            Response r;
            r.statusCode = 202;
            r.noReply = true;
            return r;
        }
    };

    // JSON has no Infinity/NaN literal, but "{:.6f}" happily emits the bare C
    // tokens nan / inf / -inf. A response carrying one still returns 200 and
    // then fails to parse -- callers see a successful-looking read and get no
    // data, which is how a whole run of plausible wrong numbers happens. Worse,
    // a non-finite value that reaches a property serialises into a saved
    // document as 1.#QNAN and makes the .effectgraph unloadable.
    //
    // Every float that goes into a JSON body must go through this.
    inline std::string JsonFloat(float v)
    {
        if (!std::isfinite(v)) return "null";
        return std::format("{:.6f}", v);
    }
    // Formats as double rather than narrowing: casting to float turned a
    // finite 1e39 into inf and therefore into null.
    inline std::string JsonFloat(double v)
    {
        if (!std::isfinite(v)) return "null";
        return std::format("{:.6f}", v);
    }
}
