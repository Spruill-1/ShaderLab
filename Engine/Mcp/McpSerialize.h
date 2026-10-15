#pragma once

// JSON vocabulary and payload helpers shared by the engine's MCP routes and
// the GUI's own routes, so both hosts answer in one vocabulary.

#include "pch_engine.h"
#include "../../EngineExport.h"
#include "../../Graph/EffectNode.h"
#include "../../Rendering/DisplayProfile.h"

#include <span>

namespace ShaderLab::Mcp
{
    // Standard base64 with '=' padding and no line breaks.
    SHADERLAB_API std::string Base64Encode(std::span<const uint8_t> bytes);

    // The name a node type is reported as: Source, BuiltInEffect,
    // PixelShader, ComputeShader or Output.
    SHADERLAB_API const char* NodeTypeName(Graph::NodeType type);

    // {"name","type","value":[...]} for one analysis field; an array field
    // also carries "count", its element count.
    SHADERLAB_API std::string AnalysisFieldToJson(const Graph::AnalysisFieldValue& field);

    // One property value: a number, bool, string or number array; a matrix
    // or curve as the placeholder strings "<matrix>" / "<curve>".
    SHADERLAB_API std::string PropertyValueToJson(const Graph::PropertyValue& value);

    // A node as GET /graph/node/{id} reports it: identity, position, media
    // path (as "shaderPath"), properties, bindings in the saved-graph shape,
    // pins, clsid, runtime error, the custom-effect definition and the last
    // analysis results.
    SHADERLAB_API std::string NodeToJson(const Graph::EffectNode& node);

    // A display profile as the /display/profiles routes report it.
    SHADERLAB_API std::string DisplayProfileToJson(const Rendering::DisplayProfile& profile);

    // Writes `png` to %TEMP%\<prefix>_<pid>_<n>.png, a name no other call in
    // any process reuses. Returns the path, or an empty path and the reason.
    struct TempFileResult
    {
        std::wstring path;
        std::string  error;
    };
    SHADERLAB_API TempFileResult WriteTempPng(std::wstring_view prefix, std::span<const uint8_t> png);
}
