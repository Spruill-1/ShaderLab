#pragma once

// Script batch mode: a JSON list of MCP-shaped steps run against the loaded
// graph in one process, answered as one JSON document. A step is either the
// raw route shape {"method", "path", "body"} or a shorthand {"op", ...args}
// from the op table in ScriptRunner.cpp. Each step's result is
// {step, status, body} (route steps add method and path) or
// {step, status, error}; a step that fails never stops the steps after it.

#include "HeadlessArgs.h"

#include <winrt/Windows.Data.Json.h>

#include <string>

namespace ShaderLab
{
    class McpRouter;
}

namespace ShaderLab::Headless
{
    class HeadlessEngine;

    class ScriptRunner
    {
    public:
        // `router` holds the engine routes for `engine`; `args` supplies
        // gpu-bench's default node and size.
        ScriptRunner(HeadlessEngine& engine, McpRouter& router, const Args& args);

        // One result per step, in order.
        winrt::Windows::Data::Json::JsonArray Run(const winrt::Windows::Data::Json::JsonArray& steps);

        // The shorthand op names, comma-separated, for the usage text.
        static std::wstring ShorthandNames();

    private:
        winrt::Windows::Data::Json::JsonObject RunStep(uint32_t index, const winrt::Windows::Data::Json::IJsonValue& step);

        HeadlessEngine& m_engine;
        McpRouter&      m_router;
        const Args&     m_args;
    };

    // Reads --script, runs it and writes the document to --script-output
    // (stdout when unset).
    ExitCode RunScriptFile(HeadlessEngine& engine, McpRouter& router, const Args& args);
}
