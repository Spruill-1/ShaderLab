# MCP Server (AI Agent Integration)

ShaderLab implements the **Model Context Protocol (MCP)** JSON-RPC 2.0 for programmatic control by AI agents, carried over **stdio via the broker** (shim → hub → session over named pipes). The full protocol surface ships in the engine DLL: the route registry (`Engine/Mcp/McpRouter.{h,cpp}`), the JSON-RPC dispatcher (`McpJsonRpc.{h,cpp}` — `initialize`, `tools/*`, `resources/*`, `ping`), the declarative 47-tool catalog (`McpToolCatalog.{h,cpp}`), and **28 engine routes** in `Engine/Mcp/Routes/`, one file per concern (catalog and `/context`, graph structure and `/graph/apply`, properties and bindings, effect HLSL and compile, readback, graph files, display). Both hosts get the identical dispatcher via `RegisterJsonRpcEndpoint`; `ShaderLabHeadless --mcp-session` therefore answers `tools/call` with no GUI at all (see [Engine / Host Split](../architecture/engine-host-split.md)). A further **20 GUI routes** (canvas, preview and GPU tools, node logs, `/perf*`) live in `MainWindow.McpAppRoutes.cpp` and `Controls/PerfMcpRoutes`, each registered as a UI, render or any-thread route ([threading](../architecture/threading-model.md#mcp-routes)); `tools/list` is **filtered by route presence on the answering host**, so a headless session advertises the engine subset (25 tools + the shim's 2) and a GUI session the full catalog — an advertised tool is always callable. Calling a tool whose backing route is absent still returns an `isError` "Tool not available on this host" result, which is the same predicate and is what a client sees if it calls a name it did not get from `tools/list`. Handlers receive `(path, query, body)`; the router owns the query split, so `?since=`-style parameters work identically across the tools ladder and headless scripts.

**Protocol version: `2025-06-18`.** Batch (JSON array) requests are rejected with `-32600` — 2025-06-18 removed batching from MCP, making it the first revision this server is actually conformant with. Responses are single-line JSON; notifications (absent `id`) produce no reply body (zero bytes on the wire).

> The stdio shim + named-pipe broker is the only transport (decision #71), which is
> what makes multiple ShaderLab windows individually addressable. Design history:
> [mcp-stdio-migration.md](../development/mcp-stdio-migration.md).

## Connection

**Transport: stdio via the broker.** ShaderLab copies its MCP shim to `%LOCALAPPDATA%\ShaderLab\bin\ShaderLabMcpBroker.exe` on launch and exposes each window as an MCP **session** via a singleton broker hub. Point your MCP client at that shim — the toolbar's **export button** copies a ready-to-paste config, and `Install.ps1` prints one after install:

```json
{ "mcpServers": { "shaderlab": {
  "command": "%LOCALAPPDATA%\\ShaderLab\\bin\\ShaderLabMcpBroker.exe",
  "args": ["--stdio", "--hub-aumid", "<PackageFamilyName>!Hub"] } } }
```

- **The shim** (`--stdio`) is the client's front-end: it owns `initialize` + `list_sessions` / `use_session`, activates the packaged hub on demand, correlates ids, and splices the pinned session's tools into `tools/list`. Being unpackaged, it survives ShaderLab updates.
- **The hub** is a blind relay — it routes frames on a clear `{channelId, seq}` header and never holds a key; shim↔session bodies are sealed (P-256 ECDH → HKDF → AES-256-GCM). Each connection has its own thread, counted from accept (so idle exit never runs while a peer is still in its hello), joined when it finishes, and on exit stopped by cancelling its pipe I/O and joined.
- **In the source**, `ShaderLabMcpBroker/` is `Hub`, `Shim` and the `BrokerPipeIo` they share. The shim answers from a method table (`initialize`, `ping`, `resources/list`, `tools/list`, `tools/call`, `resources/read`) and a tool table (`list_sessions`, `use_session`); each handler returns the lines to write, so `use_session`'s reply followed by `notifications/tools/list_changed` is one return value. An exception inside a handler is logged with the method and its message before the client gets `-32603`.
- **Each window** registers as a GUID-identified session; `ShaderLabHeadless --mcp-session` registers a headless one. Call `list_sessions`, `use_session <id>`, then drive the graph.
- A window starts its session on launch. Turn it off per window with the MCP toolbar toggle, or for every launch with `"mcp": false` in `%LOCALAPPDATA%\ShaderLab\config.json` or `SHADERLAB_MCP=0` (decision #202).

Full design + rationale: [MCP stdio migration](../development/mcp-stdio-migration.md).

## Tools (50 total)

### Graph structure

| Tool | Description |
|------|-------------|
| `graph_add_node` | Add a node by name (placed at viewport center): a built-in D2D or ShaderLab effect, `Custom Pixel Shader` / `Custom Compute Shader` / `Custom D3D11 Compute Shader`, `Output` (a second one is named `Output 2`, ...), `Desktop Duplication` with `adapterIndex` / `outputIndex`, or `Video` / `Image` with `filePath`. One lookup, `Effects::CreateNodeByName`, serves this and `graph_apply`. |
| `graph_remove_node` | Remove a node. |
| `graph_duplicate_nodes` | Copy `nodeIds` with the edges between them, as copy and paste does (`Graph::DuplicateNodes`). A binding between two copied nodes reads the copy; one to a node outside the set still reads the original. A repeated id is copied once; a missing one is a 404 and adds nothing. Returns `nodeIds` (the copies, in order) and `idMap` (old → new). |
| `graph_upgrade_nodes` | Upgrade an outdated ShaderLab effect node (`nodeId`), or every one when `nodeId` is omitted, to the registry's version, keeping property values that still exist — the Properties panel's Update button and the toolbar's Update Effects. Returns `upgraded` (ids); a node already current is not an error. |
| `graph_expression_input` | Numeric Expression inputs: `action: "add"` creates the next free letter (A–Z, value 0) and returns its `name`; `action: "remove"` with `name` deletes that input, its property and any binding to it. The last input and `Expression` itself stay. |
| `graph_rename_node` | Rename a node. |
| `graph_connect` / `graph_disconnect` | Connect / disconnect image pins. |
| `graph_apply` | Bulk patch in one call: add nodes (client refs; any name `graph_add_node` takes, with `filePath` / `adapterIndex` / `outputIndex`), connect edges, set bindings. A node is named by a ref from the same patch or by its id, as a number or as digits in a string, in every position (`"from": "3.Max"` and `"to": "3"` alike). Properties are written exactly as `graph_set_property` writes them. All or nothing: the whole patch is checked before anything changes, and a failure while applying (an unknown effect, a property the node refuses, a cycle, a bad binding) removes the nodes it added and puts back the edges and bindings it changed; the host hears about the new nodes only on success. Returns `nodeIds` and the `refToId` map. |
| `graph_clear` | Clear entire graph. |
| `graph_overview` | Compact graph summary (nodes, edges, preview). Node types use the same names as `graph_get_node` (`Source`, `BuiltInEffect`, `PixelShader`, `ComputeShader`, `Output`). Each node also carries `gpuState` and, where one exists, `gpuMs` — see [GPU cost per node](#gpu-cost-per-node). |
| `graph_get_node` | Node details incl. properties, pins, `propertyBindings`, analysis results (each with `name`, `type`, `value`, and `count` for an array, as `read_analysis_output` reports them). |
| `graph_save_json` / `graph_load_json` | Serialize / load the graph as JSON. |
| `graph_save_file` / `graph_load_file` | Save / open the graph as a file at an absolute path: a `.effectgraph` package (media embedded) or bare `.json`. See [Graph files](#graph-files). |

### Properties & bindings

| Tool | Description |
|------|-------------|
| `graph_set_property` | Set a node property (number, bool, string, or array for vectors). See [Property values](#property-values). |
| `graph_bind_property` / `graph_unbind_property` | Bind a property to an upstream analysis field (per-component or whole-array) / remove a binding. |

### Shaders & effects

| Tool | Description |
|------|-------------|
| `effect_compile` | Compile HLSL for a custom effect node (+ optional analysisFields). On an authored node (no built-in id) it re-derives the node contracts from the new HLSL and the node's current properties, so an `InputMask` / `OutputW` / `OutputH` property set since creation is injected from then on; a built-in keeps its declared contracts. |
| `effect_get_hlsl` | Read a node's HLSL source, parameters, compile state, last runtime error. |
| `list_effects` | List all effects by category (engine route `GET /effects`, so headless serves it too). |
| `registry_get_effect` | Get built-in effect metadata. |

> There is no `image_stats` tool (decision #63): use a Statistics node +
> `read_analysis_output`.

### Rendering & readback

| Tool | Description |
|------|-------------|
| `set_preview_node` | Set which node is previewed. |
| `render_capture` | Capture preview as PNG (HDR clipped to SDR), the longer edge fitted to 2048 px. Each call writes its own `%TEMP%` file. |
| `render_video` | **Headless sessions only.** Render a node over a fixed timeline (Clocks stepped to start + n/fps) and encode it with an external ffmpeg: `format` hdr10 (BT.2020/PQ 10-bit, HEVC Main10, measured MaxCLL/MaxFALL + mastering SEI) or sdr (BT.709, sRGB curve, 8-bit, H.264). `ffmpegPath` or PATH; fails clearly if neither has ffmpeg. See [headless](headless.md#modes). |
| `render_capture_node` | Capture any node's resolved output as PNG; `inline=true` returns MCP image content. |
| `read_analysis_output` | Read typed analysis fields from a compute / analysis / parameter node. When the evaluator skips unneeded readbacks (the GUI), the route dirties the node and renders a frame with `RequestFreshCpuReadback`, so the values are from that frame; it changes no process-wide setting. |
| `read_pixel_region` | FP32 RGBA region readback (scRGB linear), at most 64 pixels per axis and 1024 in all; a non-finite sample reads as `null`. |
| `read_pixel_trace` | The pixel at normalized coords of the node's own image (the preview's when the node's is unbounded), read at the node and every node upstream (each once). `pixelX`/`pixelY` are the coordinates read, the image origin included; an unknown node is a 404. |

### Display & environment

| Tool | Description |
|------|-------------|
| `get_display_info` | Display caps, active profile, pipeline, app version (engine route `GET /display/info`, so headless serves it too). |
| `list_display_profiles` | Built-in presets + currently active simulated/live profile. |
| `set_display_profile` | Apply a simulated profile (preset / presetIndex / iccPath / custom spec). |
| `clear_simulated_profile` | Revert to the live OS-reported profile. |
| `list_gpus` | Enumerate DXGI adapters (active adapter, LUID, VRAM, isWarp). |
| `switch_gpu` | Switch adapter (`warp` / `default` / `adapter` by LUID or name substring). Full save–teardown–reload cycle. |

### Editor view & diagnostics (GUI host only)

| Tool | Description |
|------|-------------|
| `graph_snapshot` | PNG snapshot of the live node-graph editor view; `inline=true` returns MCP image content. |
| `graph_get_view` / `graph_set_view` / `graph_fit_view` | Read / set / fit the editor's zoom + pan. |
| `preview_get_view` / `preview_set_view` / `preview_fit_view` | Read / set / fit the preview pane's zoom + pan. |
| `node_logs` | Per-node timestamped info / warning / error log entries (`sinceSeq` for incremental reads). |
| `perf_timings` | Per-frame timings, from the copy the render worker publishes at the end of each frame (it never waits on the render thread). CPU phase fields are wall-clock around an async API; `gpu*Ms` are real D3D11 timestamps. Note `fps` is `1/totalUs` (per-frame *work*), **not** achieved throughput — for that, diff `framesSampled` over a window with no MCP traffic in it. `previewRenderFps` / `previewRenderMs` are the status bar's numbers: how often the previewed node actually re-rendered (renders/s over a ~1 s window) and its last render's time from frame start to GPU done; both `null` when it has not re-rendered recently (a still graph, a paused Clock). `previewRenderCpuMs` / `previewRenderGpuMs` are that time's parts: CPU time to the last submission and the D3D11 timestamp span. `previewRenderMs` is the longer of the two: drivers that hold the start timestamp until the frame's first GPU work (Adreno) can give a span that misses the CPU time, and then the number is a lower bound. |
| `perf_gpu_timing` | Turn D3D11 timestamp sampling on or off. Costs a Flush per span, so it perturbs the frame it measures. |
| `perf_gpu_bindings` | The "Keep analysis on the GPU" checkbox: `enabled=true` serves analysis→parameter bindings GPU-side (SRV for compute consumers, 1-row analysis texture for pixel shaders) and skips the CPU readback; `false` reads every bound value back. Switching drops the effect cache on the render thread. Omit `enabled` to report the current state. |
| `perf_compute_submit` | A/B switch for compute submission: `commandList=true` (default) records on the runner's own deferred context and submits once; `false` restores the old call-by-call path on the shared immediate context, which is racy against Direct2D — for measurement only. |
| `perf_subrect_demand` | Kill switch for sub-rect input demand in custom pixel-shader transforms (default on). Off restores the legacy whole-image answer, ~20x slower past 2048 px wide. |
| `perf_render_mode` | Unthrottled (benchmark) render loop on/off: drops the worker's frame-deadline wait (monitor refresh, 60–240 Hz), evaluates every tick regardless of dirty state, presents with vsync interval 0. Expect tearing and a pegged GPU. |

Resources: `shaderlab://context` (pipeline and shader conventions; `tools/list` is the API reference), `shaderlab://graph`, `shaderlab://registry/effects` (`GET /registry`), `shaderlab://custom-effects` via `resources/list` / `resources/read`. Both hosts serve all four.

## Graph files

`graph_load_file` (`POST /graph/load-file { path }`) and `graph_save_file` (`POST /graph/save-file { path, embedMedia? }`) are engine routes, so the GUI and both headless modes serve them. Paths must be absolute; the app reads and writes anything its account can reach.

- **Load** accepts a `.effectgraph` package (ZIP: `graph.json` + `media/`) or bare graph JSON, detected by the PKZIP magic rather than the extension, exactly as headless `--graph` does. Package media are extracted to `%TEMP%\ShaderLab-<id>\` and each source's `media://<name>` token is rewritten to the extracted file. The graph then replaces the current one the way File > Open does: evaluator cache released, sources prepared, and the GUI reset through `OnGraphLoaded` (preview selector, output windows, layout). The GUI's current file path and title are not changed. The response lists `nodeCount`, `nodes` (id, name, type), `mediaFiles`, `extractDir` and `errors` (per node: a source that failed to prepare or whose file is missing). A relative path, a missing or unreadable file, a broken package or invalid graph JSON is a 400, and the current graph is left alone.
- **Save** writes bare JSON when the path ends in `.json` and a package otherwise. `embedMedia` defaults to true for a package (referenced image / video / ICC files are embedded and their paths become `media://` tokens in the saved copy only) and false for JSON; `embedMedia=false` writes a package holding just `graph.json`, and `embedMedia=true` with a `.json` path is a 400. Packages go through the File > Save writer: media unchanged since the last save to the same file are reused (`mediaUnchanged`, `inPlace`), and a new file is written beside the old one and renamed into place. The folder must exist. The response carries `format`, `bytesWritten`, `fileSize`, media counts and `warnings`: a source file that does not exist (saved as its path), or, without embedding, a source that points at extracted package media in the temp folder, which is deleted when the graph is replaced.
- **Packages are untrusted input.** A media entry must be a flat file name under `media/` (no separators, `..`, drive or stream colons, trailing dots or spaces, or device names such as `NUL`), so nothing extracts outside the temp folder; a DEFLATE entry is inflated into exactly its declared size and refused if it would grow past it or declares more than raw DEFLATE can expand to (about 1032:1). Either failure rejects the whole package. The same loader backs File > Open and headless `--graph`.
- **Extracted media lifetime.** The extracted folder is handed to the host through `IEngineCommandSink::OnGraphMediaDirChanged` and lives until the graph is next replaced by `graph_load_file`, `graph_load_json` or `graph_clear`, or the host exits. The GUI also heartbeats it like a File > Open folder, so another instance's startup reaper leaves it alone.
- **Threading and time.** File I/O runs on the MCP listener thread; only the graph swap (load) or serialization (save) runs on the render thread, so a large package does not stall the preview or hit the 20 s render-closure budget. The remaining limit is the shim's 30 s per request (`McpTimeouts.h`). Measured headless on a 1.46 GB video package: save 6.4 s, load 4.1 s. Packages cap at 4 GB (no ZIP64); on slow storage a call past 30 s returns a timeout to the client while the operation still completes.

## Arguments and errors

Every engine route and GUI route reads its arguments with the readers in `Engine/Mcp/McpArgs.h`, which accept a value of the right JSON type or a string spelling it, since Claude Code sends untyped arguments as strings. Node ids, pins and pixel coordinates are whole numbers (`"7"` and `7` alike; `7.5` or `"seven"` is a 400 naming the field); booleans follow the rule under [Property values](#property-values). The tool-call layer reads path arguments (`graph_get_node`'s `nodeId`, `node_logs`' `sinceSeq`, ...) the same way, so a malformed one is a tool error naming the field rather than a JSON-RPC internal error.

An error body is `{"error": "<message>", "code": "<code>"}`, built by one JSON writer (`Engine/Mcp/McpJsonWriter.h`) that escapes every string and writes a non-finite number as `null`. The code tells the kinds of failure apart, since a tool result does not carry the HTTP-style status:

| code | status | meaning |
|------|--------|---------|
| `bad_argument` | 400 | an argument is missing or unreadable; nothing was changed |
| `not_found` | 404 | the node, binding or file named does not exist |
| `not_ready` | 409 | the node has not been evaluated yet; the body also has `"notReady": true`, and `"compiling": [...]` when shaders it depends on are still compiling |
| `busy` | 503 | the GUI is switching GPU adapters; retry |
| `internal` | 500 | a server fault after the arguments were accepted, with the exception's message |

A route checks its arguments before it does anything, and nothing after that is caught as a bad request, so a fault inside the engine is a 500 with its message rather than a misleading `Invalid request`.

## Property values

`graph_set_property` and `graph_apply` write through one function, `Graph::WriteProperty`. The value takes the property's type: the declared parameter's `typeName`, else the type of the value it replaces; a key with neither keeps the value's own type (a number becomes a float, a string stays a string). Clients may send any value as a string, as Claude Code sends untyped arguments:

- A number string must be a whole number (`"203"`, `" 1.5 "`); `"12abc"`, `"nan"` and `""` are a 400, and nothing is written.
- A boolean string is one of true/false, 1/0, on/off, yes/no in any case; any other word is a 400. The same rule reads every boolean tool argument, in the engine routes, the GUI's own routes and headless `render_video`.
- A vector or float-array property also takes `"[1, 2, 3]"`. An array of the wrong length, or into a scalar, is a 400.
- A number into a `uint`, `int` or `bool` property is converted (a negative `uint` is a 400); `true` into a `float` is 1.
- `isPlaying` / `IsPlaying` starts or pauses a clock; playing a non-looping clock that has reached its end rewinds it, from either tool. `shaderPath` (or `mediaPath`) sets a Source node's file; it is stored as the node's media path, not as a property, and `graph_get_node` / `GET /graph` report it as a top-level `shaderPath` on the node.

## Known Limitations

- **Compile-before-connect**: First-time compile of a compute shader node that's already connected to the render pipeline crashes D2D. Workaround: compile the shader while the node is disconnected, then wire it in. Recompiles of already-compiled nodes work fine.
- **FP16 precision**: Analysis readback values show minor quantization (e.g., 0.1 → 0.099976) due to the D2D output buffer using 16-bit float precision.
- **HLSL optimizer removes unreferenced cbuffer vars**: With `D3DCOMPILE_WARNINGS_ARE_ERRORS`, variables not referenced on ALL code paths are optimized out. Read all cbuffer vars at top of `main()` before branches.
- **ExprTk math-only subset**: Numeric Expression has the regex / IO / enhanced subsystems disabled. Expressions must produce finite scalar `float` results — no strings, no file I/O, no vector return values.



## GPU cost per node

With GPU timing on (the performance flyout's *Measure real GPU time*, or
`perf_gpu_timing`), `graph_overview` reports each node's `gpuState` and, where
there is a figure to give, `gpuMs`. **Branch on the state, not on the presence
of `gpuMs`** — a missing number means five different things:

| `gpuState` | Meaning |
|---|---|
| `measured` | `gpuMs` is a real measurement of this node: its own bracketed compute dispatch, or — for the node at the end of a Direct2D chain — the whole chain that feeds it. |
| `fused-downstream` | A D2D image node mid-chain. Its cost is real but is already counted inside a downstream chain end's figure. Direct2D evaluates a chain lazily at `DrawImage`, so separating it would mean materialising the node alone, which changes the workload being measured. |
| `cached` | Needed this frame, but clean, so it served its cached result. Zero is the correct answer, not a missing measurement. |
| `idle` | The evaluator skipped it: nothing consumes its output. Actionable — bind or wire it and it becomes measurable. |
| `cpu-only` | A parameter node (`Clock`, `Float Parameter`, …) with no image output. There is no GPU work to attribute. |

The field is absent entirely when timing is off. The same states are drawn on
the canvas by the flyout's *Show GPU time on graph nodes*.

Worked example on `HDR Test Pattern → Exposure → {Luminance Statistics, a
tone mapper}`, preview on the tone mapper, a `Clock` driving `Exposure`:
tone mapper `measured` 3.75 ms (the fused chain), statistics `measured`
1.90 ms (its own dispatch), source and Exposure `fused-downstream`, Clock
`cpu-only`. Unbinding the statistics node's `Max` from the tone mapper flips it
to `idle`.

---

Back to [docs/](../README.md) • [Repo root](../../README.md)
