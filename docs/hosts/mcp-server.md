# MCP Server (AI Agent Integration)

ShaderLab implements the **Model Context Protocol (MCP)** JSON-RPC 2.0 for programmatic control by AI agents, carried over **stdio via the broker** (shim → hub → session over named pipes). The full protocol surface ships in the engine DLL: the route registry (`Engine/Mcp/McpRouter.{h,cpp}`), the JSON-RPC dispatcher (`McpJsonRpc.{h,cpp}` — `initialize`, `tools/*`, `resources/*`, `ping`), the declarative 39-tool catalog (`McpToolCatalog.{h,cpp}`), and **25 engine-pure routes**. Both hosts get the identical dispatcher via `RegisterJsonRpcEndpoint`; `ShaderLabHeadless --mcp-session` therefore answers `tools/call` with no GUI at all (see [Engine / Host Split](../architecture/engine-host-split.md)). A further **16 app-side routes** (view/preview/GPU tools, `/context`, `/perf`, node logs) live in `MainWindow.McpRoutes.cpp`; `tools/list` is **filtered by route presence on the answering host**, so a headless session advertises the engine subset (25 tools + the shim's 2) and a GUI session the full catalog — an advertised tool is always callable. Calling a tool whose backing route is absent still returns an `isError` "Tool not available on this host" result, which is the same predicate and is what a client sees if it calls a name it did not get from `tools/list`. Handlers receive `(path, query, body)`; the router owns the query split, so `?since=`-style parameters work identically across the tools ladder and headless scripts.

**Protocol version: `2025-06-18`.** Batch (JSON array) requests are rejected with `-32600` — 2025-06-18 removed batching from MCP, making it the first revision this server is actually conformant with. Responses are single-line JSON; notifications (absent `id`) produce no reply body (zero bytes on the wire).

> **Transport migration complete.** The embedded HTTP listener was deleted in
> migration Step 9 (decision #71, engine ABI **3**); the stdio shim + named-pipe
> broker is now the only transport, which is what makes multiple ShaderLab windows
> individually addressable. Plan and rationale:
> [mcp-stdio-migration.md](../development/mcp-stdio-migration.md).

## Connection

**Transport: stdio via the broker** (the embedded HTTP listener was removed in migration Step 9). ShaderLab copies its MCP shim to `%LOCALAPPDATA%\ShaderLab\bin\ShaderLabMcpBroker.exe` on launch and exposes each window as an MCP **session** via a singleton broker hub. Point your MCP client at that shim — the toolbar's **export button** copies a ready-to-paste config, and `Install.ps1` prints one after install:

```json
{ "mcpServers": { "shaderlab": {
  "command": "%LOCALAPPDATA%\\ShaderLab\\bin\\ShaderLabMcpBroker.exe",
  "args": ["--stdio", "--hub-aumid", "<PackageFamilyName>!Hub"] } } }
```

- **The shim** (`--stdio`) is the client's front-end: it owns `initialize` + `list_sessions` / `use_session`, activates the packaged hub on demand, correlates ids, and splices the pinned session's tools into `tools/list`. Being unpackaged, it survives ShaderLab updates.
- **The hub** is a blind relay — it routes frames on a clear `{channelId, seq}` header and never holds a key; shim↔session bodies are sealed (P-256 ECDH → HKDF → AES-256-GCM).
- **Each window** registers as a GUID-identified session; `ShaderLabHeadless --mcp-session` registers a headless one. Call `list_sessions`, `use_session <id>`, then drive the graph.
- Enable/disable per window via the MCP toolbar toggle, the `--mcp` flag, or `%LOCALAPPDATA%\ShaderLab\config.json`.

Full design + rationale: [MCP stdio migration](../development/mcp-stdio-migration.md).

## Tools (47 total)

### Graph structure

| Tool | Description |
|------|-------------|
| `graph_add_node` | Add built-in D2D or ShaderLab effect (placed at viewport center); `Video`/`Image` with `filePath` for sources. |
| `graph_remove_node` | Remove a node. |
| `graph_rename_node` | Rename a node. |
| `graph_connect` / `graph_disconnect` | Connect / disconnect image pins. |
| `graph_apply` | Bulk patch in one call: add nodes (client refs), connect edges, set bindings. Returns refToId map. |
| `graph_clear` | Clear entire graph. |
| `graph_overview` | Compact graph summary (nodes, edges, preview). Each node also carries `gpuState` and, where one exists, `gpuMs` — see [GPU cost per node](#gpu-cost-per-node). |
| `graph_get_node` | Node details incl. properties, pins, `propertyBindings`, analysis results. |
| `graph_save_json` / `graph_load_json` | Serialize / load the graph as JSON. |
| `graph_save_file` / `graph_load_file` | Save / open the graph as a file at an absolute path: a `.effectgraph` package (media embedded) or bare `.json`. See [Graph files](#graph-files). |

### Properties & bindings

| Tool | Description |
|------|-------------|
| `graph_set_property` | Set a node property (number, bool, string, or array for vectors). |
| `graph_bind_property` / `graph_unbind_property` | Bind a property to an upstream analysis field (per-component or whole-array) / remove a binding. |

### Shaders & effects

| Tool | Description |
|------|-------------|
| `effect_compile` | Compile HLSL for a custom effect node (+ optional analysisFields). |
| `effect_get_hlsl` | Read a node's HLSL source, parameters, compile state, last runtime error. |
| `list_effects` | List all effects by category (engine route `GET /effects` since migration Step 1 — headless serves it too). |
| `registry_get_effect` | Get built-in effect metadata. |

> `image_stats` was removed in migration Step 1. It had been advertised long after
> decision #63 retired its route, and longest-prefix routing turned calls into an
> HTTP-200 "success" wrapping a JSON-RPC error. Use a Statistics node +
> `read_analysis_output` instead.

### Rendering & readback

| Tool | Description |
|------|-------------|
| `set_preview_node` | Set which node is previewed. |
| `render_capture` | Capture preview as PNG (HDR clipped to SDR). |
| `render_video` | **Headless sessions only.** Render a node over a fixed timeline (Clocks stepped to start + n/fps) and encode it with an external ffmpeg: `format` hdr10 (BT.2020/PQ 10-bit, HEVC Main10, measured MaxCLL/MaxFALL + mastering SEI) or sdr (BT.709, sRGB curve, 8-bit, H.264). `ffmpegPath` or PATH; fails clearly if neither has ffmpeg. See [headless](headless.md#modes). |
| `render_capture_node` | Capture any node's resolved output as PNG; `inline=true` returns MCP image content. |
| `read_analysis_output` | Read typed analysis fields from a compute / analysis / parameter node. |
| `read_pixel_region` | FP32 RGBA region readback (scRGB linear), capped 32×32. |
| `read_pixel_trace` | Pixel trace at normalized coords (per-node values). |

### Display & environment

| Tool | Description |
|------|-------------|
| `get_display_info` | Display caps, active profile, pipeline, app version (engine route `GET /display/info` since migration Step 2 — headless serves it too). |
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
| `perf_timings` | Per-frame timings. CPU phase fields are wall-clock around an async API; `gpu*Ms` are real D3D11 timestamps. Note `fps` is `1/totalUs` (per-frame *work*), **not** achieved throughput — for that, diff `framesSampled` over a window with no MCP traffic in it. `previewRenderFps` / `previewRenderMs` are the status bar's numbers: how often the previewed node actually re-rendered (renders/s over a ~1 s window) and its last render's time from frame start to GPU done; both `null` when it has not re-rendered recently (a still graph, a paused Clock). `previewRenderCpuMs` / `previewRenderGpuMs` are that time's parts: CPU time to the last submission and the D3D11 timestamp span. `previewRenderMs` is the longer of the two: drivers that hold the start timestamp until the frame's first GPU work (Adreno) can give a span that misses the CPU time, and then the number is a lower bound. |
| `perf_gpu_timing` | Turn D3D11 timestamp sampling on or off. Costs a Flush per span, so it perturbs the frame it measures. |
| `perf_gpu_bindings` | The "Keep analysis on the GPU" checkbox: `enabled=true` serves analysis→parameter bindings GPU-side (SRV for compute consumers, 1-row analysis texture for pixel shaders) and skips the CPU readback; `false` reads every bound value back. Switching drops the effect cache on the render thread. Omit `enabled` to report the current state. |
| `perf_compute_submit` | A/B switch for compute submission: `commandList=true` (default) records on the runner's own deferred context and submits once; `false` restores the old call-by-call path on the shared immediate context, which is racy against Direct2D — for measurement only. |
| `perf_subrect_demand` | Kill switch for sub-rect input demand in custom pixel-shader transforms (default on). Off restores the legacy whole-image answer, ~20x slower past 2048 px wide. |
| `perf_render_mode` | Unthrottled (benchmark) render loop on/off: drops the worker's frame-deadline wait (monitor refresh, 60–240 Hz), evaluates every tick regardless of dirty state, presents with vsync interval 0. Expect tearing and a pegged GPU. |

Resources: `shaderlab://context`, `shaderlab://graph`, `shaderlab://registry/effects`, `shaderlab://custom-effects` via `resources/list` / `resources/read`.

## Graph files

`graph_load_file` (`POST /graph/load-file { path }`) and `graph_save_file` (`POST /graph/save-file { path, embedMedia? }`) are engine routes, so the GUI and both headless modes serve them. Paths must be absolute; the app reads and writes anything its account can reach.

- **Load** accepts a `.effectgraph` package (ZIP: `graph.json` + `media/`) or bare graph JSON, detected by the PKZIP magic rather than the extension, exactly as headless `--graph` does. Package media are extracted to `%TEMP%\ShaderLab-<id>\` and each source's `media://<name>` token is rewritten to the extracted file. The graph then replaces the current one the way File > Open does: evaluator cache released, sources prepared, and the GUI reset through `OnGraphLoaded` (preview selector, output windows, layout). The GUI's current file path and title are not changed. The response lists `nodeCount`, `nodes` (id, name, type), `mediaFiles`, `extractDir` and `errors` (per node: a source that failed to prepare or whose file is missing). A relative path, a missing or unreadable file, a broken package or invalid graph JSON is a 400, and the current graph is left alone.
- **Save** writes bare JSON when the path ends in `.json` and a package otherwise. `embedMedia` defaults to true for a package (referenced image / video / ICC files are embedded and their paths become `media://` tokens in the saved copy only) and false for JSON; `embedMedia=false` writes a package holding just `graph.json`, and `embedMedia=true` with a `.json` path is a 400. Packages go through the File > Save writer: media unchanged since the last save to the same file are reused (`mediaUnchanged`, `inPlace`), and a new file is written beside the old one and renamed into place. The folder must exist. The response carries `format`, `bytesWritten`, `fileSize`, media counts and `warnings`: a source file that does not exist (saved as its path), or, without embedding, a source that points at extracted package media in the temp folder, which is deleted when the graph is replaced.
- **Extracted media lifetime.** The extracted folder is handed to the host through `IEngineCommandSink::OnGraphMediaDirChanged` and lives until the graph is next replaced by `graph_load_file`, `graph_load_json` or `graph_clear`, or the host exits. The GUI also heartbeats it like a File > Open folder, so another instance's startup reaper leaves it alone.
- **Threading and time.** File I/O runs on the MCP listener thread; only the graph swap (load) or serialization (save) runs on the render thread, so a large package does not stall the preview or hit the 20 s render-closure budget. The remaining limit is the shim's 30 s per request (`McpTimeouts.h`). Measured headless on a 1.46 GB video package: save 6.4 s, load 4.1 s. Packages cap at 4 GB (no ZIP64); on slow storage a call past 30 s returns a timeout to the client while the operation still completes.

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
