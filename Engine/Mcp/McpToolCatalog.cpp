#include "pch_engine.h"
#include "McpToolCatalog.h"

// The listJson strings below are the tools/list entries formerly embedded
// as one giant multi-line raw literal in MainWindow.McpRoutes.cpp. Each is
// a SINGLE LINE by construction — the stdio transport frames one JSON
// message per line, so no catalog string may ever contain a newline
// (invisible over HTTP, fatal over stdio). Content is unchanged from the
// pre-catalog dispatcher except for the removed `image_stats` phantom.

namespace ShaderLab::Mcp
{
    namespace
    {
        // Shorthand so rows below stay readable.
        using M = ToolArgMode;

        const std::vector<ToolDef> kCatalog = {

        // ---- Graph structure ------------------------------------------------
        { "graph_add_node",
          R"JSON({"name":"graph_add_node","description":"Add a node. Use effectName for built-in/ShaderLab effects. For sources use effectName='Video' or 'Image' with optional filePath.","inputSchema":{"type":"object","properties":{"effectName":{"type":"string","description":"Effect name, or 'Video'/'Image' for source nodes"},"filePath":{"type":"string","description":"File path for Video/Image source nodes (optional)"}},"required":["effectName"]}})JSON",
          L"POST", L"/graph/add-node", M::BodyPassthrough },
        { "graph_remove_node",
          R"JSON({"name":"graph_remove_node","description":"Remove a node by ID","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}})JSON",
          L"POST", L"/graph/remove-node", M::BodyPassthrough },
        { "graph_rename_node",
          R"JSON({"name":"graph_rename_node","description":"Rename a node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"name":{"type":"string"}},"required":["nodeId","name"]}})JSON",
          L"POST", L"/graph/rename-node", M::BodyPassthrough },
        { "graph_connect",
          R"JSON({"name":"graph_connect","description":"Connect output pin to input pin","inputSchema":{"type":"object","properties":{"srcId":{"type":"number"},"srcPin":{"type":"number"},"dstId":{"type":"number"},"dstPin":{"type":"number"}},"required":["srcId","srcPin","dstId","dstPin"]}})JSON",
          L"POST", L"/graph/connect", M::BodyPassthrough },
        { "graph_disconnect",
          R"JSON({"name":"graph_disconnect","description":"Disconnect an edge","inputSchema":{"type":"object","properties":{"srcId":{"type":"number"},"srcPin":{"type":"number"},"dstId":{"type":"number"},"dstPin":{"type":"number"}},"required":["srcId","srcPin","dstId","dstPin"]}})JSON",
          L"POST", L"/graph/disconnect", M::BodyPassthrough },
        { "graph_apply",
          R"JSON({"name":"graph_apply","description":"Apply a graph patch in one call: add nodes (using client refs), connect edges, set property bindings. Call /graph/clear first if you need a fresh graph. Body: { nodes:[{ref,effect,filePath?,properties?}], edges:[{from,to,fromPin?,toPin?}], bindings:[{node,property,from:'ref.field'|{node,field},component?}] }. 'from' and 'to' accept either a ref string or numeric nodeId. Returns refToId map + nodeIds in add order.","inputSchema":{"type":"object","properties":{"nodes":{"type":"array"},"edges":{"type":"array"},"bindings":{"type":"array"}}}})JSON",
          L"POST", L"/graph/apply", M::BodyPassthrough },
        { "graph_clear",
          R"JSON({"name":"graph_clear","description":"Clear the graph","inputSchema":{"type":"object","properties":{}}})JSON",
          L"POST", L"/graph/clear", M::NoBody },
        { "graph_overview",
          R"JSON({"name":"graph_overview","description":"Compact graph summary: nodes (id, name, type, error), edges, preview node","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/graph/overview", M::NoBody },
        { "graph_get_node",
          R"JSON({"name":"graph_get_node","description":"Get detailed info about a node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}})JSON",
          L"GET", L"/graph/node/{}", M::PathNumber, L"nodeId" },
        { "graph_save_json",
          R"JSON({"name":"graph_save_json","description":"Serialize graph to JSON","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/graph/save", M::NoBody },
        { "graph_load_json",
          R"JSON({"name":"graph_load_json","description":"Load graph from JSON string","inputSchema":{"type":"object","properties":{"json":{"type":"string"}},"required":["json"]}})JSON",
          L"POST", L"/graph/load", M::UnwrapField, L"json" },

        // ---- Properties & bindings ------------------------------------------
        { "graph_set_property",
          R"JSON({"name":"graph_set_property","description":"Set a node property. Value can be number, bool, string, or array for vectors.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"key":{"type":"string"},"value":{}},"required":["nodeId","key","value"]}})JSON",
          L"POST", L"/graph/set-property", M::BodyPassthrough },
        { "graph_bind_property",
          R"JSON({"name":"graph_bind_property","description":"Bind a node property to an upstream analysis output field","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"propertyName":{"type":"string"},"sourceNodeId":{"type":"number"},"sourceFieldName":{"type":"string"},"sourceComponent":{"type":"number","description":"0-3 for .xyzw component (scalar dest only)"}},"required":["nodeId","propertyName","sourceNodeId","sourceFieldName"]}})JSON",
          L"POST", L"/graph/bind-property", M::BodyPassthrough },
        { "graph_unbind_property",
          R"JSON({"name":"graph_unbind_property","description":"Remove a property binding","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"propertyName":{"type":"string"}},"required":["nodeId","propertyName"]}})JSON",
          L"POST", L"/graph/unbind-property", M::BodyPassthrough },

        // ---- Shaders & effects ----------------------------------------------
        { "effect_compile",
          R"JSON({"name":"effect_compile","description":"Compile HLSL for a custom effect node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"hlsl":{"type":"string"}},"required":["nodeId","hlsl"]}})JSON",
          L"POST", L"/effect/compile", M::BodyPassthrough },
        { "effect_get_hlsl",
          R"JSON({"name":"effect_get_hlsl","description":"Read a node's custom-effect HLSL source, parameter list, compile state, and last runtime error. For non-custom nodes returns hasCustomEffect=false (200, not 404). For ShaderLab library effects, also includes isLibraryEffect=true + shaderLabEffectId/Version.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}})JSON",
          L"GET", L"/effect/hlsl/{}", M::PathNumber, L"nodeId" },
        { "list_effects",
          R"JSON({"name":"list_effects","description":"List all available effects (Built-in D2D + ShaderLab) with categories","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/effects", M::NoBody },
        { "registry_get_effect",
          R"JSON({"name":"registry_get_effect","description":"Get metadata for a built-in effect","inputSchema":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}})JSON",
          L"GET", L"/registry/effect/", M::PathString, L"name" },

        // ---- Rendering & readback -------------------------------------------
        { "set_preview_node",
          R"JSON({"name":"set_preview_node","description":"Set which node is previewed","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}})JSON",
          L"POST", L"/render/preview-node", M::BodyPassthrough },
        { "render_capture",
          R"JSON({"name":"render_capture","description":"Capture preview as PNG. Note: HDR values clipped to SDR.","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/render/capture", M::NoBody },
        { "render_capture_node",
          R"JSON({"name":"render_capture_node","description":"Capture any node's resolved output as PNG -- full frame, aspect preserved (FORCES a render frame so dirty nodes evaluate). With inline=true returns the image as MCP image content (base64). maxDim fits the longer edge to that many px (default 2048); use a small value (e.g. 512) for a low-res preview = fewer tokens, or larger for full detail. 404 if node missing; 409 with notReady=true if the node is dirty / has unconnected inputs.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"inline":{"type":"boolean"},"maxDim":{"type":"number","description":"Longer-edge cap in px, aspect preserved. Small=preview/fewer tokens, large=full detail. Default 2048."}},"required":["nodeId"]}})JSON",
          L"POST", L"/render/capture-node", M::BodyPassthrough, nullptr, /*imageInline=*/true },
        { "read_analysis_output",
          R"JSON({"name":"read_analysis_output","description":"Read typed analysis output fields from a compute/analysis node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}})JSON",
          L"GET", L"/analysis/{}", M::PathNumber, L"nodeId" },
        { "read_pixel_region",
          R"JSON({"name":"read_pixel_region","description":"Read a small w x h region of FP32 RGBA pixels from a node's output (scRGB linear-light). Region is capped at 32x32 (1024 pixels) and per-axis at 64. Pixels are returned row-major as a flat float array (RGBARGBA...).","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"w":{"type":"number"},"h":{"type":"number"}},"required":["nodeId","x","y","w","h"]}})JSON",
          L"POST", L"/render/pixel-region", M::BodyPassthrough },
        { "read_pixel_trace",
          R"JSON({"name":"read_pixel_trace","description":"Run pixel trace at normalized coordinates, returns per-node pixel values and analysis outputs","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"x":{"type":"number","description":"Normalized X (0-1)"},"y":{"type":"number","description":"Normalized Y (0-1)"}},"required":["nodeId","x","y"]}})JSON",
          L"POST", L"/render/pixel-trace", M::BodyPassthrough },

        // ---- Display & environment ------------------------------------------
        { "get_display_info",
          R"JSON({"name":"get_display_info","description":"Current display capabilities, active profile, pipeline format, app version","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/display/info", M::NoBody },
        { "list_display_profiles",
          R"JSON({"name":"list_display_profiles","description":"List all built-in display profile presets and the currently active simulated/live profile. Returns full caps (HDR, peak nits, SDR white) and CIE primaries.","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/display/profiles", M::NoBody },
        { "set_display_profile",
          R"JSON({"name":"set_display_profile","description":"Apply a simulated display profile (overrides OS-reported caps until cleared). Specify exactly ONE of: preset (factory or display name), presetIndex (0-based), iccPath (.icc/.icm file), custom (full chroma + nits spec).","inputSchema":{"type":"object","properties":{"preset":{"type":"string"},"presetIndex":{"type":"number"},"iccPath":{"type":"string"},"custom":{"type":"object","properties":{"name":{"type":"string"},"hdrEnabled":{"type":"boolean"},"sdrWhiteNits":{"type":"number"},"peakNits":{"type":"number"},"minNits":{"type":"number"},"maxFullFrameNits":{"type":"number"},"primaryRed":{"type":"array","items":{"type":"number"}},"primaryGreen":{"type":"array","items":{"type":"number"}},"primaryBlue":{"type":"array","items":{"type":"number"}},"whitePoint":{"type":"array","items":{"type":"number"}},"gamut":{"type":"string"}},"required":["peakNits"]}}}})JSON",
          L"POST", L"/display/profile", M::BodyPassthrough },
        { "clear_simulated_profile",
          R"JSON({"name":"clear_simulated_profile","description":"Revert to the live OS-reported display profile (clears any simulated/preset/ICC override).","inputSchema":{"type":"object","properties":{}}})JSON",
          L"POST", L"/display/profile/clear", M::NoBody },
        { "list_gpus",
          R"JSON({"name":"list_gpus","description":"Enumerate available GPU adapters (DXGI). Returns the active adapter and a list of all installed adapters with name, vendorId, deviceId, dedicated VRAM (MB), LUID, and isWarp flag.","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/gpu/list", M::NoBody },
        { "switch_gpu",
          R"JSON({"name":"switch_gpu","description":"Switch the active GPU adapter. Triggers a full graph-save, device-teardown, and graph-reload cycle. Use mode='warp' for the WARP software adapter, 'default' to let the driver pick, or 'adapter' with either {luid:{low,high}} or {name:'partial-match'}. Falls back to default if the requested adapter fails to initialize.","inputSchema":{"type":"object","properties":{"mode":{"type":"string","enum":["warp","default","adapter"]},"name":{"type":"string","description":"Substring match against adapter name (used when mode='adapter')"},"luid":{"type":"object","properties":{"low":{"type":"number"},"high":{"type":"number"}}}},"required":["mode"]}})JSON",
          L"POST", L"/gpu/switch", M::BodyPassthrough },

        // ---- Editor view & diagnostics (GUI-only backing routes) ------------
        { "graph_snapshot",
          R"JSON({"name":"graph_snapshot","description":"Capture a PNG snapshot of the live node-graph editor view at the current pan/zoom and panel size. With inline=true returns the image as MCP image content (base64). Without inline, returns the temp file path only.","inputSchema":{"type":"object","properties":{"inline":{"type":"boolean","description":"If true, return the PNG bytes inline as MCP image content"}}}})JSON",
          L"POST", L"/graph/snapshot", M::BodyPassthrough, nullptr, /*imageInline=*/true },
        { "graph_get_view",
          R"JSON({"name":"graph_get_view","description":"Get the node-graph view's current zoom, pan offset, viewport size, and the bounding box of all nodes (in canvas space).","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/graph/view", M::NoBody },
        { "graph_set_view",
          R"JSON({"name":"graph_set_view","description":"Pan and/or zoom the node-graph editor view. Any subset of {zoom, panX, panY} may be supplied. Changes apply immediately to the live UI. zoom is clamped to [0.1, 5.0]; pan has no clamp. Coordinate convention: screen = zoom * canvas + pan.","inputSchema":{"type":"object","properties":{"zoom":{"type":"number"},"panX":{"type":"number"},"panY":{"type":"number"}}}})JSON",
          L"POST", L"/graph/view", M::BodyPassthrough },
        { "graph_fit_view",
          R"JSON({"name":"graph_fit_view","description":"Fit the node-graph view to show all nodes with the given viewport-space padding (DIPs, default 40). No-op when the graph is empty.","inputSchema":{"type":"object","properties":{"padding":{"type":"number"}}}})JSON",
          L"POST", L"/graph/view/fit", M::BodyPassthrough },
        { "preview_get_view",
          R"JSON({"name":"preview_get_view","description":"Get the preview pane's current zoom + pan + image bounds + zoom limits.","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/preview/view", M::NoBody },
        { "preview_set_view",
          R"JSON({"name":"preview_set_view","description":"Set the preview pane's zoom and/or pan. zoom clamped to [0.01, 100.0]. Returns post-clamp values.","inputSchema":{"type":"object","properties":{"zoom":{"type":"number"},"panX":{"type":"number"},"panY":{"type":"number"}}}})JSON",
          L"POST", L"/preview/view", M::BodyPassthrough },
        { "preview_fit_view",
          R"JSON({"name":"preview_fit_view","description":"Fit the preview image to the preview viewport (auto zoom + center).","inputSchema":{"type":"object","properties":{}}})JSON",
          L"POST", L"/preview/view/fit", M::NoBody },
        { "node_logs",
          R"JSON({"name":"node_logs","description":"Get per-node log entries (timestamped info/warning/error). Use sinceSeq for incremental reads.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"sinceSeq":{"type":"number","description":"Only return entries after this sequence number"}},"required":["nodeId"]}})JSON",
          L"GET", L"/node/{}/logs?since={}", M::NodeLogs, L"nodeId" },
        { "perf_timings",
          R"JSON({"name":"perf_timings","description":"Get per-frame performance timings (ms) for render pipeline phases","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/perf", M::NoBody },
        };
    }

    const std::vector<ToolDef>& ToolCatalog()
    {
        return kCatalog;
    }

    const ToolDef* FindTool(std::string_view name)
    {
        for (const auto& t : kCatalog)
            if (name == t.name)
                return &t;
        return nullptr;
    }
}
