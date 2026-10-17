#include "pch_engine.h"
#include "ScriptRunner.h"
#include "GpuBench.h"
#include "HeadlessEngine.h"

#include "Engine/Utf8.h"
#include "../Mcp/McpArgs.h"
#include "../Mcp/McpRouter.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace ShaderLab::Headless
{
    namespace
    {
        namespace WDJ = winrt::Windows::Data::Json;

        // What a step asks for: a route request, which RunStep sends, or the
        // step's own answer.
        struct StepOutcome
        {
            bool         isRoute{ false };
            std::wstring method;
            std::wstring path;
            std::string  body;

            uint16_t        status{ 200 };
            WDJ::JsonObject result{ nullptr };
            std::wstring    error;          // set: the step failed with `status`
        };

        StepOutcome Route(std::wstring method, std::wstring path, std::string body = {})
        {
            StepOutcome outcome;
            outcome.isRoute = true;
            outcome.method = std::move(method);
            outcome.path = std::move(path);
            outcome.body = std::move(body);
            return outcome;
        }

        StepOutcome Answer(WDJ::JsonObject body)
        {
            StepOutcome outcome;
            outcome.result = std::move(body);
            return outcome;
        }

        StepOutcome Failure(uint16_t status, std::wstring message)
        {
            StepOutcome outcome;
            outcome.status = status;
            outcome.error = std::move(message);
            return outcome;
        }

        WDJ::JsonObject MakeStepResult(uint32_t index, uint16_t status, const WDJ::IJsonValue& body)
        {
            WDJ::JsonObject entry;
            entry.Insert(L"step", WDJ::JsonValue::CreateNumberValue(index));
            entry.Insert(L"status", WDJ::JsonValue::CreateNumberValue(status));
            entry.Insert(L"body", body);
            return entry;
        }

        WDJ::JsonObject MakeStepError(uint32_t index, uint16_t status, const std::wstring& message)
        {
            WDJ::JsonObject entry;
            entry.Insert(L"step", WDJ::JsonValue::CreateNumberValue(index));
            entry.Insert(L"status", WDJ::JsonValue::CreateNumberValue(status));
            entry.Insert(L"error", WDJ::JsonValue::CreateStringValue(message));
            return entry;
        }

        // A route step's result. The response body is kept as JSON when it
        // parses, so a consumer can navigate it, and as a string otherwise.
        WDJ::JsonObject MakeRouteResult(uint32_t index, const StepOutcome& call, const Mcp::Response& response)
        {
            WDJ::JsonObject entry;
            entry.Insert(L"step", WDJ::JsonValue::CreateNumberValue(index));
            entry.Insert(L"method", WDJ::JsonValue::CreateStringValue(call.method));
            entry.Insert(L"path", WDJ::JsonValue::CreateStringValue(call.path));
            entry.Insert(L"status", WDJ::JsonValue::CreateNumberValue(response.statusCode));
            if (response.body.empty())
            {
                entry.Insert(L"body", WDJ::JsonValue::CreateNullValue());
                return entry;
            }
            const winrt::hstring text(Utf8ToWide(response.body));
            WDJ::JsonValue parsed{ nullptr };
            if (WDJ::JsonValue::TryParse(text, parsed))
                entry.Insert(L"body", parsed);
            else
                entry.Insert(L"body", WDJ::JsonValue::CreateStringValue(text));
            return entry;
        }

        std::wstring ArgError(const std::string& error) { return Utf8ToWide(error); }

        // The step's members other than "op", as the JSON body of the route
        // a shorthand stands for.
        std::string ShorthandBody(const WDJ::JsonObject& step)
        {
            WDJ::JsonObject body;
            for (auto member : step)
                if (member.Key() != L"op") body.Insert(member.Key(), member.Value());
            return WideToUtf8(body.Stringify());
        }

        struct ShorthandContext
        {
            HeadlessEngine& engine;
            const Args&     args;
        };

        using ShorthandHandler = StepOutcome (*)(ShorthandContext&, const WDJ::JsonObject& step);

        struct Shorthand
        {
            const wchar_t*   name;
            ShorthandHandler handler;
        };

        StepOutcome NodeRoute(const WDJ::JsonObject& step, std::wstring_view prefix)
        {
            std::string error;
            auto nodeId = Mcp::RequireUintArg(step, L"nodeId", error);
            if (!nodeId) return Failure(400, ArgError(error));
            return Route(L"GET", std::wstring(prefix) + std::to_wstring(*nodeId));
        }

        StepOutcome RunGpuBenchOp(ShorthandContext& context, const WDJ::JsonObject& step)
        {
            std::string error;
            GpuBenchRequest request;
            request.nodeId = Mcp::ReadUintArg(step, L"nodeId", error).value_or(context.args.nodeId);
            request.iterations = Mcp::ReadUintArg(step, L"iterations", error).value_or(20);
            request.width = Mcp::ReadUintArg(step, L"width", error);
            request.height = Mcp::ReadUintArg(step, L"height", error);
            request.fallbackWidth = context.args.width;
            request.fallbackHeight = context.args.height;
            if (!error.empty()) return Failure(400, ArgError(error));
            return Answer(RunGpuBench(context.engine, request));
        }

        // A barrier between a batch of mutations and a readback. Readback
        // routes render a frame themselves, so it is rarely needed.
        StepOutcome RunRenderOp(ShorthandContext& context, const WDJ::JsonObject&)
        {
            context.engine.RenderFrame();
            WDJ::JsonObject body;
            body.Insert(L"ok", WDJ::JsonValue::CreateBooleanValue(true));
            return Answer(body);
        }

        const Shorthand cShorthands[] =
        {
            { L"set-property", [](ShorthandContext&, const WDJ::JsonObject& s) { return Route(L"POST", L"/graph/set-property", ShorthandBody(s)); } },
            { L"pixel-region", [](ShorthandContext&, const WDJ::JsonObject& s) { return Route(L"POST", L"/render/pixel-region", ShorthandBody(s)); } },
            { L"capture-node", [](ShorthandContext&, const WDJ::JsonObject& s) { return Route(L"POST", L"/render/capture-node", ShorthandBody(s)); } },
            { L"get-graph",    [](ShorthandContext&, const WDJ::JsonObject&)   { return Route(L"GET", L"/graph"); } },
            { L"get-node",     [](ShorthandContext&, const WDJ::JsonObject& s) { return NodeRoute(s, L"/graph/node/"); } },
            { L"analysis",     [](ShorthandContext&, const WDJ::JsonObject& s) { return NodeRoute(s, L"/analysis/"); } },
            { L"render",       &RunRenderOp },
            { L"gpu-bench",    &RunGpuBenchOp },
        };

        const Shorthand* FindShorthand(std::wstring_view name)
        {
            for (const auto& op : cShorthands)
                if (name == op.name) return &op;
            return nullptr;
        }

        // The raw shape: {"method" (default GET), "path", "body"}. A string
        // body is sent byte for byte; anything else is serialised.
        StepOutcome RawRoute(const WDJ::JsonObject& step)
        {
            std::string error;
            const auto method = Mcp::ReadStringArg(step, L"method", error).value_or(L"GET");
            const auto path = Mcp::RequireStringArg(step, L"path", error);
            if (!path) return Failure(400, ArgError(error));
            if (!error.empty()) return Failure(400, ArgError(error));
            std::string body;
            if (step.HasKey(L"body"))
            {
                const auto value = step.GetNamedValue(L"body");
                body = value.ValueType() == WDJ::JsonValueType::String
                    ? WideToUtf8(value.GetString())
                    : WideToUtf8(value.Stringify());
            }
            return Route(method, *path, std::move(body));
        }

        std::string ReadFileUtf8(const std::wstring& path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) return {};
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string s = ss.str();
            if (s.size() >= 3 &&
                static_cast<uint8_t>(s[0]) == 0xEF &&
                static_cast<uint8_t>(s[1]) == 0xBB &&
                static_cast<uint8_t>(s[2]) == 0xBF)
            {
                s.erase(0, 3);   // UTF-8 BOM
            }
            return s;
        }
    }

    ScriptRunner::ScriptRunner(HeadlessEngine& engine, McpRouter& router, const Args& args)
        : m_engine(engine), m_router(router), m_args(args)
    {
    }

    std::wstring ScriptRunner::ShorthandNames()
    {
        std::wstring names;
        for (const auto& op : cShorthands)
        {
            if (!names.empty()) names += L", ";
            names += op.name;
        }
        return names;
    }

    WDJ::JsonArray ScriptRunner::Run(const WDJ::JsonArray& steps)
    {
        WDJ::JsonArray results;
        for (uint32_t i = 0; i < steps.Size(); ++i)
            results.Append(RunStep(i, steps.GetAt(i)));
        return results;
    }

    WDJ::JsonObject ScriptRunner::RunStep(uint32_t index, const WDJ::IJsonValue& stepValue)
    {
        if (stepValue.ValueType() != WDJ::JsonValueType::Object)
            return MakeStepError(index, 400, L"Step is not an object");
        const auto step = stepValue.GetObject();

        try
        {
            StepOutcome outcome;
            if (step.HasKey(L"op"))
            {
                std::string error;
                const auto op = Mcp::ReadStringArg(step, L"op", error);
                if (!op) return MakeStepError(index, 400, ArgError(error));
                const Shorthand* shorthand = FindShorthand(*op);
                if (!shorthand) return MakeStepError(index, 400, L"Unknown op: " + *op);
                ShorthandContext context{ m_engine, m_args };
                outcome = shorthand->handler(context, step);
            }
            else if (step.HasKey(L"path"))
            {
                outcome = RawRoute(step);
            }
            else
            {
                return MakeStepError(index, 400, L"Step must have either 'op' or 'path'");
            }

            if (!outcome.error.empty())
                return MakeStepError(index, outcome.status, outcome.error);
            if (!outcome.isRoute)
                return MakeStepResult(index, outcome.status, outcome.result);
            return MakeRouteResult(index, outcome, m_router.RouteRequest(outcome.method, outcome.path, outcome.body));
        }
        catch (const winrt::hresult_error& ex)
        {
            return MakeStepError(index, 500, std::format(L"step failed (0x{:08X}): {}",
                static_cast<uint32_t>(ex.code()), std::wstring(ex.message())));
        }
        catch (const std::exception& ex)
        {
            return MakeStepError(index, 500, L"step failed: " + Utf8ToWide(ex.what()));
        }
    }

    ExitCode RunScriptFile(HeadlessEngine& engine, McpRouter& router, const Args& args)
    {
        auto scriptText = ReadFileUtf8(args.scriptPath);
        if (scriptText.empty())
        {
            std::wprintf(L"FATAL: could not read script file '%ls'\n", args.scriptPath.c_str());
            return ExitCode::InputRead;
        }

        // A top-level array of steps, or an object with a "steps" array
        // (room for top-level metadata without a schema change).
        WDJ::JsonArray steps{ nullptr };
        WDJ::JsonValue document{ nullptr };
        if (!WDJ::JsonValue::TryParse(winrt::hstring(Utf8ToWide(scriptText)), document))
        {
            std::wprintf(L"FATAL: script is not valid JSON\n");
            return ExitCode::InputParse;
        }
        if (document.ValueType() == WDJ::JsonValueType::Array)
        {
            steps = document.GetArray();
        }
        else if (document.ValueType() == WDJ::JsonValueType::Object)
        {
            auto object = document.GetObject();
            if (!object.HasKey(L"steps") || object.GetNamedValue(L"steps").ValueType() != WDJ::JsonValueType::Array)
            {
                std::wprintf(L"FATAL: script object must have a 'steps' array\n");
                return ExitCode::InputParse;
            }
            steps = object.GetNamedArray(L"steps");
        }
        else
        {
            std::wprintf(L"FATAL: script must be a JSON array or object with 'steps'\n");
            return ExitCode::InputParse;
        }

        ScriptRunner runner(engine, router, args);
        WDJ::JsonObject outDoc;
        outDoc.Insert(L"stepCount", WDJ::JsonValue::CreateNumberValue(static_cast<double>(steps.Size())));
        outDoc.Insert(L"results", runner.Run(steps));
        const std::string outUtf8 = WideToUtf8(outDoc.Stringify());

        if (args.scriptOutputPath.empty())
        {
            std::fwrite(outUtf8.data(), 1, outUtf8.size(), stdout);
            std::fputc('\n', stdout);
            return ExitCode::Ok;
        }
        std::ofstream f(args.scriptOutputPath, std::ios::binary);
        if (!f)
        {
            std::wprintf(L"FATAL: cannot open --script-output '%ls' for write\n", args.scriptOutputPath.c_str());
            return ExitCode::OutputWrite;
        }
        f.write(outUtf8.data(), static_cast<std::streamsize>(outUtf8.size()));
        std::wprintf(L"OK: ran %u step(s) -> %ls\n", steps.Size(), args.scriptOutputPath.c_str());
        return ExitCode::Ok;
    }
}
