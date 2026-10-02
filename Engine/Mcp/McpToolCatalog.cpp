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
        { "graph_load_file",
          R"JSON({"name":"graph_load_file","description":"Load a graph from a file on disk, replacing the current graph (as File > Open does). path must be ABSOLUTE; the app reads any path it can reach. Accepts a .effectgraph package (ZIP of graph.json plus embedded media/, which is extracted to a temp folder that lives until the graph is replaced or cleared) or bare graph JSON; the form is detected from the content, not the extension. Returns nodeCount, nodes (id, name, type), the extracted media count and per-node errors (e.g. a source file that is missing). Errors (400): relative path, missing or unreadable file, not a graph.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Absolute path to a .effectgraph package or a .json graph"}},"required":["path"]}})JSON",
          L"POST", L"/graph/load-file", M::BodyPassthrough },
        { "graph_save_file",
          R"JSON({"name":"graph_save_file","description":"Save the current graph to a file on disk. path must be ABSOLUTE and its folder must exist; the app writes any path it can reach and replaces an existing file (written beside it and renamed into place). A path ending in .json is written as bare graph JSON (source media stay as absolute paths); anything else, normally .effectgraph, is written as a package (ZIP) with the referenced image/video/ICC files embedded. embedMedia=false writes a package without media; embedMedia=true with a .json path is an error. Media unchanged since the last save to the same package are reused, not rewritten. Returns format, bytesWritten, fileSize, media counts and warnings (missing source files, or temp-extracted media saved by path).","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Absolute output path (.effectgraph or .json)"},"embedMedia":{"type":"boolean","description":"Default: true for a package, false for .json"}},"required":["path"]}})JSON",
          L"POST", L"/graph/save-file", M::BodyPassthrough },

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
          R"JSON({"name":"render_capture","description":"Capture preview as PNG. HDR values are CLIPPED to SDR, so do not judge HDR content from this image -- it shows you a tone-mapped guess, not the pipeline output. Use analysis nodes (Nit Map, Luminance Heatmap, Gamut Highlight) for what is actually above SDR white, and read_pixel_region / read_analysis_output for numbers.","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/render/capture", M::NoBody },
        { "render_video",
          R"JSON({"name":"render_video","description":"HEADLESS SESSIONS ONLY (the GUI does not register it). Render one node over time on a FIXED timeline -- frame n sets every Clock to start + n/fps -- and encode it with an external ffmpeg. format hdr10 (default): BT.2020 + PQ in absolute nits (scRGB 1.0 = 80 nits), 10-bit 4:2:0, HEVC Main10 by default, with mastering-display + content-light metadata in the bitstream; MaxCLL/MaxFALL are MEASURED from the frames (one extra render-only pass) unless maxCll+maxFall are given. format sdr: BT.709, sRGB curve on [0,1], 8-bit, H.264 by default. No tone mapping is applied: export the node you want (a tone mapper output is display-referred, 1.0 = SDR white, which HDR10 reads as 80 nits). ffmpeg comes from ffmpegPath, else PATH; if neither has it the call fails and says how to supply it. Blocks until the file is written; returns frames, size, encoder, measured MaxCLL/MaxFALL, warnings (e.g. video/capture sources are not time-stepped) and the ffmpeg command line.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"outputPath":{"type":"string","description":"Output file (.mp4, .mkv or .mov)"},"format":{"type":"string","enum":["hdr10","sdr"]},"codec":{"type":"string","enum":["hevc","av1","h264"],"description":"Default hevc for hdr10, h264 for sdr"},"fps":{"type":"number","description":"Default 60"},"start":{"type":"number","description":"Timeline start, seconds (default 0)"},"duration":{"type":"number","description":"Seconds (default: the longest Clock span at its Speed)"},"crf":{"type":"number"},"preset":{"type":"string"},"lossless":{"type":"boolean","description":"HEVC/H.264 only; for verification"},"mastering":{"type":"string","enum":["p3-1000","working-space","none"],"description":"HDR10 mastering display: P3-D65 1000/0.005 nits (default), the graph's Working Space, or no static metadata"},"maxCll":{"type":"number"},"maxFall":{"type":"number"},"ffmpegPath":{"type":"string","description":"ffmpeg.exe or its folder; default: search PATH"}},"required":["nodeId","outputPath"]}})JSON",
          L"POST", L"/render/video", M::BodyPassthrough },
        { "render_capture_node",
          R"JSON({"name":"render_capture_node","description":"Capture any node's resolved output as PNG -- full frame, aspect preserved (FORCES a render frame so dirty nodes evaluate). PNG is 8-bit SDR: anything above scRGB 1.0 (80 nits) is CLIPPED and wide-gamut negatives are lost, so this image CANNOT be used to judge HDR or wide-gamut correctness -- capture a diagnostic node instead (Nit Map / Luminance Heatmap / Gamut Highlight / CIE Chromaticity Plot / Delta E Comparator in Heatmap mode), which encode those facts into SDR-visible form, and say so when reporting what you saw. With inline=true returns the image as MCP image content (base64). maxDim fits the longer edge to that many px (default 2048); use a small value (e.g. 512) for a low-res preview = fewer tokens, or larger for full detail. 404 if node missing; 409 with notReady=true if the node is dirty / has unconnected inputs, or is waiting on a background shader compile (the response lists the compiling nodes in `compiling`; retry in a few seconds).","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"inline":{"type":"boolean"},"maxDim":{"type":"number","description":"Longer-edge cap in px, aspect preserved. Small=preview/fewer tokens, large=full detail. Default 2048."}},"required":["nodeId"]}})JSON",
          L"POST", L"/render/capture-node", M::BodyPassthrough, nullptr, /*imageInline=*/true },
        { "read_analysis_output",
          R"JSON({"name":"read_analysis_output","description":"Read typed analysis output fields from a compute/analysis node. This is the trustworthy path for HDR judgement -- values are full-range scRGB computed on GPU, unlike a captured PNG which clips to SDR. Pair with Delta E Comparator (Method=dE ITP for HDR/WCG) + Luminance Statistics for measured color difference.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}})JSON",
          L"GET", L"/analysis/{}", M::PathNumber, L"nodeId" },
        { "read_pixel_region",
          R"JSON({"name":"read_pixel_region","description":"Read a small w x h region of FP32 RGBA pixels from a node's output (scRGB linear-light). Full range and unclipped -- values above 1.0 (brighter than 80-nit SDR white) and negative components (wide-gamut chroma) are preserved, so this is the ground truth for HDR checks where a captured PNG would lie. Region is capped at 32x32 (1024 pixels) and per-axis at 64. Pixels are returned row-major as a flat float array (RGBARGBA...). 409 with notReady=true while the node or anything upstream is waiting on a background shader compile (`compiling` lists them); retry in a few seconds.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"w":{"type":"number"},"h":{"type":"number"}},"required":["nodeId","x","y","w","h"]}})JSON",
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
          R"JSON({"name":"perf_timings","description":"Get per-frame performance timings (ms). CPU phase timings (totalMs/evaluateMs/drawMs/...) are wall-clock around an ASYNCHRONOUS graphics API, so they measure command recording, not GPU execution -- do not use them to judge shader cost. The gpu*Ms fields are real GPU time from D3D11 timestamp queries, but are only sampled after perf_gpu_timing enables them (gpuEnabled tells you).","inputSchema":{"type":"object","properties":{}}})JSON",
          L"GET", L"/perf", M::NoBody },
        { "perf_gpu_bindings",
          R"JSON({"name":"perf_gpu_bindings","description":"The 'Keep analysis on the GPU' switch. enabled=true routes analysis->parameter bindings GPU-side (compute consumers via SRV, pixel-shader consumers via a 1-row analysis texture) and skips the CPU readback; false reads every bound value back to the CPU. Switching drops the effect cache. Omit enabled to just report the current state.","inputSchema":{"type":"object","properties":{"enabled":{"type":"boolean"}}}})JSON",
          L"POST", L"/perf/gpu-bindings", M::BodyPassthrough },
        { "perf_compute_submit",
          R"JSON({"name":"perf_compute_submit","description":"A/B switch for how compute dispatches reach the GPU. commandList=true (default): recorded on the runner's own deferred context and submitted as one ExecuteCommandList, so no other thread can interleave. false: issued call by call on the shared immediate context (the old path, racy against Direct2D on other threads) -- for measuring the command list's cost only.","inputSchema":{"type":"object","properties":{"commandList":{"type":"boolean"}}}})JSON",
          L"POST", L"/perf/compute-submit", M::BodyPassthrough },
        { "perf_subrect_demand",
          R"JSON({"name":"perf_subrect_demand","description":"Kill switch for sub-rect input demand in custom pixel-shader transforms. On (default) a D2D tile query is answered with that tile; off restores the older whole-image answer, which makes D2D re-render the whole upstream chain once per tile and is ~20x slower past 2048px wide. Exists to A/B the two on identical content.","inputSchema":{"type":"object","properties":{"enabled":{"type":"boolean","description":"true for sub-rect (fast), false for whole-image (legacy)"}}}})JSON",
          L"POST", L"/perf/subrect-demand", M::BodyPassthrough },
        { "perf_render_mode",
          R"JSON({"name":"perf_render_mode","description":"Turn the unthrottled (benchmark) render loop on or off. Off by default: the worker normally paces itself with a 16 ms wait (~62.5 Hz ceiling) and the UI presents with vsync interval 1, both of which cap any throughput measurement at the display's cadence rather than the pipeline's. On, the worker free-runs, evaluates every tick regardless of dirty state, and presents with interval 0 -- expect tearing and a pegged GPU. Read fps from perf_timings to see the result.","inputSchema":{"type":"object","properties":{"unthrottled":{"type":"boolean","description":"true to free-run, false to pace to the display"},"forceRedraw":{"type":"boolean","description":"true = worst case every frame: every node regenerates (D2D output caches dropped, every compute node dispatches, generators rebuild). Without it a static graph evaluates to cached outputs. Each key changes only its own setting."},"analysisRefresh":{"type":"integer","description":"How often displayed analysis readouts (canvas labels, Properties panel) are read back: 0 = selected node every 2 s (default, fastest), 1 = all nodes every 2 s, 2 = all nodes every 250 ms, 3 = all nodes every frame (a GPU readback stall per frame). Display only; bindings are unaffected."}}}})JSON",
          L"POST", L"/perf/render-mode", M::BodyPassthrough },
        { "perf_gpu_timing",
          R"JSON({"name":"perf_gpu_timing","description":"Turn GPU timestamp sampling on or off. Off by default because closing a GPU span around Direct2D work requires a Flush, which breaks D2D batching and perturbs the frame being measured -- enable it to measure, disable it to run. Results appear in perf_timings as gpu*Ms and lag by a few frames.","inputSchema":{"type":"object","properties":{"enabled":{"type":"boolean","description":"true to start sampling, false to stop"}}}})JSON",
          L"POST", L"/perf/gpu-timing", M::BodyPassthrough },
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
