#pragma once

// Transport-neutral MCP types (stdio-migration Step 2).
//
// Response used to live inside McpHttpServer as a nested struct, which
// welded the engine ABI (IEngineCommandSink::Dispatch, every route
// handler) to the HTTP transport header. Routes and sinks now depend on
// this header only; the transport (McpRouter's HTTP listener today, the
// stdio session client later) is an implementation detail behind it.

#include <string>
#include <string_view>
#include <cstdint>
#include <cstdio>

namespace ShaderLab::Mcp
{
    // ---- Shared JSON string utilities (stdio-migration Step 3) ------------
    //
    // There used to be THREE divergent JSON escapers (EngineMcpRoutes' full
    // one, plus two ad-hoc loops in the GUI dispatcher that missed raw
    // control characters — HLSL compiler output could produce invalid
    // JSON). This is now the single authority; everything that embeds text
    // in a JSON string literal goes through it.

    inline std::string WideToUtf8(std::wstring_view ws)
    {
        if (ws.empty()) return {};
        int len = ::WideCharToMultiByte(CP_UTF8, 0,
            ws.data(), static_cast<int>(ws.size()),
            nullptr, 0, nullptr, nullptr);
        std::string out(len, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0,
            ws.data(), static_cast<int>(ws.size()),
            out.data(), len, nullptr, nullptr);
        return out;
    }

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

        // No-reply discriminator. Over HTTP an empty 202 body and "no
        // response" are the same wire bytes, so the distinction never
        // mattered. Over stdio they are different things: a JSON-RPC
        // notification must produce ZERO bytes (an empty line is not
        // valid JSON), while an empty-body reply still produces a framed
        // message. Handlers that answer notifications return None();
        // transports check noReply before serializing anything.
        bool        noReply{ false };

        static Response None()
        {
            Response r;
            r.statusCode = 202;   // HTTP transport still sends 202 Accepted
            r.noReply = true;
            return r;
        }
    };
}
