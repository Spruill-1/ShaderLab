# MCP Server (AI Agent Integration)

ShaderLab includes an embedded HTTP server implementing the **Model Context Protocol (MCP)** JSON-RPC 2.0 for programmatic control by AI agents. The full protocol surface ships in the engine DLL: the route registry + HTTP listener (`Engine/Mcp/McpRouter.{h,cpp}`), the JSON-RPC dispatcher (`McpJsonRpc.{h,cpp}` — `initialize`, `tools/*`, `resources/*`, `ping`), the declarative 39-tool catalog (`McpToolCatalog.{h,cpp}`), and **25 engine-pure routes**. Both hosts get the identical dispatcher via `RegisterJsonRpcEndpoint`; `ShaderLabHeadless --serve` therefore answers `tools/call` with no GUI at all (see [Engine / Host Split](../architecture/engine-host-split.md)). A further **16 app-side routes** (view/preview/GPU tools, `/context`, `/perf`, node logs) live in `MainWindow.McpRoutes.cpp`; calling a tool whose backing route is absent on the answering host returns an `isError` "Tool not available on this host" result. Handlers receive `(path, query, body)`; the router owns the query split, so `?since=`-style parameters work identically over HTTP, the tools ladder, and headless scripts.

**Protocol version: `2025-06-18`.** Batch (JSON array) requests are rejected with `-32600` — 2025-06-18 removed batching from MCP, making it the first revision this server is actually conformant with. Responses are single-line JSON; notifications (absent `id`) produce no reply body (zero bytes on the wire).

> **Transport migration in progress.** The HTTP transport described here is being
> replaced by a stdio shim + named-pipe broker so multiple ShaderLab windows
> become individually addressable. Plan, rationale, and status:
> [mcp-stdio-migration.md](../development/mcp-stdio-migration.md). Everything on
> this page describes the **current** (HTTP) behaviour.

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

## Tools (39 total)

### Graph structure

| Tool | Description |
|------|-------------|
| `graph_add_node` | Add built-in D2D or ShaderLab effect (placed at viewport center); `Video`/`Image` with `filePath` for sources. |
| `graph_remove_node` | Remove a node. |
| `graph_rename_node` | Rename a node. |
| `graph_connect` / `graph_disconnect` | Connect / disconnect image pins. |
| `graph_apply` | Bulk patch in one call: add nodes (client refs), connect edges, set bindings. Returns refToId map. |
| `graph_clear` | Clear entire graph. |
| `graph_overview` | Compact graph summary (nodes, edges, preview). |
| `graph_get_node` | Node details incl. properties, pins, `propertyBindings`, analysis results. |
| `graph_save_json` / `graph_load_json` | Serialize / load the graph as JSON. |

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
| `perf_timings` | Per-node evaluation timings from the most recent frame. |

Resources: `shaderlab://context`, `shaderlab://graph`, `shaderlab://registry/effects`, `shaderlab://custom-effects` via `resources/list` / `resources/read`.

## Known Limitations

- **Compile-before-connect**: First-time compile of a compute shader node that's already connected to the render pipeline crashes D2D. Workaround: compile the shader while the node is disconnected, then wire it in. Recompiles of already-compiled nodes work fine.
- **FP16 precision**: Analysis readback values show minor quantization (e.g., 0.1 → 0.099976) due to the D2D output buffer using 16-bit float precision.
- **HLSL optimizer removes unreferenced cbuffer vars**: With `D3DCOMPILE_WARNINGS_ARE_ERRORS`, variables not referenced on ALL code paths are optimized out. Read all cbuffer vars at top of `main()` before branches.
- **ExprTk math-only subset**: Numeric Expression has the regex / IO / enhanced subsystems disabled. Expressions must produce finite scalar `float` results — no strings, no file I/O, no vector return values.

---

Back to [docs/](../README.md) • [Repo root](../../README.md)
