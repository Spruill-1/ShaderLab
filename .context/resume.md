# ShaderLab — Development Context (Resume Point)

## Project Identity

**ShaderLab** is a WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D shader effects with full HDR and wide color gamut (WCG) support, with a particular focus on tone-mapping and color-correction R&D.

- **Location**: `C:\Users\david\source\ShaderLab\ShaderLab.slnx`
- **Version**: **1.7.3** released. Current branch `user/daspr/mcp_migration_httptostdio`: the **MCP HTTP → stdio + broker migration is COMPLETE** (all 9 steps). The embedded HTTP listener is deleted; the broker (shim → hub → session over named pipes, bodies sealed) is the only MCP transport. Engine ABI **3**. Only the **manual verification sweep** at the end of [docs/development/mcp-stdio-migration.md](../docs/development/mcp-stdio-migration.md) remains before full sign-off (WinUI window lifecycle, packaged install/activation, a real MCP client, in-place upgrade).
- **Graph format version**: **2** (unchanged).
- **Engine ABI version**: **3** (`SHADERLAB_ENGINE_ABI_VERSION` in `EngineExport.h`; Step 2 re-typed `McpRouter`/`Mcp::Response`, Step 9 deleted the HTTP transport).
- **Language**: C++/WinRT — direct COM access to `ID2D1EffectImpl`, `ID2D1DrawTransform`, `ID2D1ComputeTransform`. No C#.

> Authoritative sources of truth: [`docs/`](../docs/README.md) (architecture tree + per-file references) and especially [`docs/history/decision-log.md`](../docs/history/decision-log.md) (**70 entries**; #64–67 were never written — that stretch is covered by `CHANGELOG.md` §1.6.0), `CHANGELOG.md` (per-version diffs), `Version.h` (numeric version), `.github/copilot-instructions.md` (AI agent rules, including the graph-access threading rule). This file is a fast-orientation summary; it can drift — re-check the docs tree before relying on details.

---

## Solution Layout (4 projects)

| Project | Output | Purpose |
|--------|--------|---------|
| `ShaderLabEngine.vcxproj` | `ShaderLabEngine.dll` | Host-agnostic engine: graph model + `GraphUiSnapshot`, evaluator, `RenderThreadDispatcher`, ICC reader, video + live-capture sources, ExprTk math, D3D11 compute runner + `CustomComputeBridgeEffect` + `BytecodeCache`, `IEngineComputeOutput` COM interface, MCP router (`McpRouter`, HTTP listener until migration Step 9) + JSON-RPC dispatcher + 39-tool catalog + **25 engine-pure routes**. Exported via `SHADERLAB_API`. |
| `ShaderLab.vcxproj` | `ShaderLab.exe` (MSIX) | WinUI 3 packaged app. `RenderEngine` (app-only), all XAML, controllers, the render worker thread, `MainWindow.McpRoutes.cpp` (**18 app-side routes** + JSON-RPC dispatcher + `GuiEngineCommandSink`). Depends on the engine DLL. |
| `ShaderLabTests.vcxproj` | `ShaderLabTests.exe` | Standalone console test runner — **244 tests** (graph/evaluator/bindings, BytecodeCache, GraphUiSnapshot, RenderThreadDispatcher, McpRouter + JSON-RPC dispatcher contracts, MCP frame codec + crypto + peer identity, GPU-binding + skip-readback matrices, 51-test HLSL math bench). CI uses `--adapter warp`. |
| `ShaderLabHeadless.vcxproj` | `ShaderLabHeadless.exe` | Console host, no WinUI: PNG render, FP32 pixel readback (`--pixels`), JSON batch script mode (`--script`), **MCP session mode (`--mcp-session`, registers with the broker hub)**, bytecode-cache reap/clear ops, `--enable/--disable-gpu-bindings`. |
| `ShaderLabMcpBroker.vcxproj` | `ShaderLabMcpBroker.exe` | MCP broker. `--hub`: singleton blind relay — first-instance election, per-peer pairing, session registry + channel relay (routes on channelId only; bodies sealed end-to-end). `--stdio`: the MCP client's front-end — owns initialize + list_sessions/use_session, pins a session, runs the initiator handshake, seals/forwards requests, splices tools/list. Does NOT link the engine — compiles the `McpFrame`/`McpCrypto`/`McpPeerIdentity`/`McpChannel` TUs directly. Packaged as the manifest's second `<Application Id="Hub">`. |

This split (decisions #41 + #58) keeps WinUI out of the test path, lets engine logic be exercised in isolation, and gives MCP agents a fully-functional logged-out host for parameter sweeps.

---

## Threading Model (v1.7.0, decisions #68 + #70)

All D3D11/D2D graph work runs on a dedicated **render worker `std::jthread`**; the UI thread only blits a double-buffered offscreen into the `SwapChainPanel` swap chain and `Present1`s (presenting from the worker is impossible — XAML composition is STA-bound). The worker per tick: drain `RenderThreadDispatcher` closures → working-space sync → live-capture/clock/video tick → dirty-propagation BFS → `RenderFrameToOffscreen` → publish index + `GraphUiSnapshot`. A version-gated blit keeps the UI thread from vsync-blocking when the worker publishes slower than the UI ticks.

**Graph access rule** (the full text lives in `Controls/NodeGraphController.h` and `.github/copilot-instructions.md`; getting it wrong is an access violation inside `std::map`, not a compile error):

1. **UI-thread reads → the per-frame `GraphUiSnapshot`**, never live `m_graph`.
2. **Writes (any thread) → `RenderThreadDispatcher::DispatchSync`.**
3. **Layout computation → live graph, render thread only.**

Two locks with a strict order (`m_graphMutex` → `m_visualsMutex`); never hold `m_visualsMutex` across a `DispatchSync`. See [docs/architecture/threading-model.md](../docs/architecture/threading-model.md) for the diagrams and the resource-ownership table.

---

## Complete Feature Set (v1.7.3)

### Core
- Node-based DAG graph editor for D2D effect composition.
- 40+ wrapped built-in D2D effects (`Effects/EffectRegistry.cpp`) across 9 categories.
- ShaderLab built-in effect library with embedded HLSL (`Effects/ShaderLabEffects.cpp` + `Effects/ColorMath.cpp`) — current catalog table in [docs/effects/builtin-catalog.md](../docs/effects/builtin-catalog.md).
- Custom pixel shader effects (`ID2D1DrawTransform`).
- Custom D2D compute shader effects (`ID2D1ComputeTransform`, per-tile dispatch).
- Custom **D3D11 compute shader effects** — routed through `CustomComputeBridgeEffect` (D2D wrapper) + `D3D11ComputeRunner`, which implements `IEngineComputeOutput` so downstream compute consumers can bind analysis SRVs directly (Phase 8, shipped in 1.6.0, feature flag default ON).
- **`BytecodeCache`**: compile-once bytecode store with eager GPU-binding-variant precompile and disk persistence under `%LOCALAPPDATA%\ShaderLab\bytecode\`; reaper wired to the status-bar broom button and headless CLI flags.
- Live HLSL hot-reload with `D3DCompile` + `D3DReflect` auto-property discovery.
- Effect Designer modal window for authoring custom pixel / D2D-compute / D3D11-compute effects with full parameter definition.
- Graph JSON serialization with versioning (format version 2) — saved as `.effectgraph` zip files (DEFLATE via miniz) with optional **embedded media**.

### ShaderLab Built-in Effects (`Effects/ShaderLabEffects.cpp`)

Grouped by `category` + optional `subcategory` (Add Node flyout sub-grouping):

- **Analysis → Highlights**: Luminance Heatmap, Nit Map, Gamut Highlight, Luminance Highlight.
- **Analysis → Scopes**: CIE Histogram (D3D11 compute), CIE Chromaticity Plot. (Vectorscope and Waveform Monitor were **removed in 1.6.0** — no clear use case after the compute-scatter migration.)
- **Analysis → Comparison**: Delta E Comparator (CIEDE2000, `OutputMode` Heatmap / Grayscale dE), Split Comparison.
- **Analysis → Gamut Mapping**: Gamut Map (Clip / Nearest / Compress / Fit), ICtCp Gamut Map, Gamut Coverage (single-group D3D11 compute scatter since 1.6.0).
- **Analysis → Tone Mapping (ICtCp suite)**: ICtCp Round-Trip Validator, ICtCp Tone Map (HDR → SDR; D3D11 compute since 1.6.0, `SourcePeakNits`/`TargetPeakNits` gpuBindable), ICtCp Inverse Tone Map (SDR → HDR), ICtCp Saturation, ICtCp Highlight Desaturation. Bind their numeric peak/SDR-white parameters to the `Working Space` node's analysis outputs to track Display Settings or simulated profiles automatically.
- **Analysis → Statistics** (D3D11 compute, data-only): Channel Statistics, Luminance Statistics, Chromaticity Statistics. Stats are not architecturally special (decision #63): agents use `/graph/add-node` + `/analysis/<id>`.
- **Source / Generators**: Gamut Source, ICtCp Boundary, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern.
- **Live capture sources**: DXGI Desktop Duplication (per-output enumerated), Windows Graphics Capture (WinUI picker). Per-frame ticking via `SourceNodeFactory::TickAndUploadLiveCaptures` on the render worker.
- **Data / Parameter nodes** (no shader, evaluator-handled): Float, Integer, Toggle, Gamut, Clock, Numeric Expression (ExprTk, A..Z inputs), Random (deterministic seed → [0,1) hash), **Working Space** (mirrors active display profile into 14 typed analysis fields).

Every effect carries a stable `effectId` + numeric `effectVersion`; saved graphs detect upgrades and offer per-node / batch upgrade in the Properties panel.

### Property System
- `PropertyValue` variant: `float`, `int32`, `uint32`, `bool`, `wstring`, `float2`, `float3`, `float4`, `D2D1_MATRIX_5X4_F`, `vector<float>`.
- Per-component property bindings (Grasshopper-style data flow), with array (whole-vector) bindings for LUT-shaped fields. `gpuBindable` parameters + `gpuPublish` analysis fields route upstream compute SRVs directly to D3D11 compute consumers (CPU readback skipped when no CPU consumer needs the value).
- Enum labels for named dropdown parameters; `bool` rendered as `ToggleSwitch`.
- `visibleWhen` conditional visibility on parameters (`"Mode == 1"`, `"Strength > 0"`, etc.) — including conditionally-visible **input pins**, extended on the canvas after MCP or Properties-panel changes (1.7.3 fix).
- Visual data pins (orange diamonds) on the node graph for binding connections.

### Rendering
- **Always scRGB FP16 pipeline** (`DXGI_FORMAT_R16G16B16A16_FLOAT`, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`). DWM/ACM handles final display conversion.
- **Render worker thread** (see Threading Model above); UI-thread Present cost is a sub-ms FP16 blit.
- **Refresh-rate-driven loop** (60–240 Hz) — interval re-derived from `EnumDisplaySettings(dmDisplayFrequency)` on every display change.
- Dirty-gated evaluation with dirty-propagation pre-pass; no built-in tone-mapping pass — users build tone mappers as graph effects (the ICtCp suite is the preferred path).
- Display profile mocking (presets + ICC file loading via `mscms.dll`); monitor gamut from `DXGI_OUTPUT_DESC1` primaries; **OS-reported SDR white level** via `DisplayConfigGetDeviceInfo`, exposed to graphs as `working_space.SdrWhiteNits`.
- **DXVA2 / Media Foundation video sources** with `ID3D10Multithread` protection.
- `OutputWindow` system: each `Output` node gets its own OS window (cross-thread `OutputSinkRenderState`, worker renders native-size, UI fits + presents). Bidirectional sync (close window ↔ delete node); `OnNodeAdded` auto-spawns windows for MCP/file-load Output nodes.
- D2D-rendered node graph canvas with pan/zoom, bezier edges (Alt+click delete), color-coded nodes, dot grid, dark theme; canvas paints from the `GraphUiSnapshot`.

### MCP Server (stdio via the broker; HTTP deleted in migration Step 9)
JSON-RPC 2.0 (protocol `2025-06-18`, batching rejected) over the broker — **no HTTP**. `Engine/Mcp/McpRouter.{h,cpp}` is now a pure route registry (`AddRoute`/`RouteRequest`/`HasRoute`, query split, `ActivityCallback` fired on top-level `POST /`); transport-neutral types in `McpTypes.h` (`Mcp::Response` + `noReply`); the JSON-RPC dispatcher + declarative 39-tool `McpToolCatalog` in `McpJsonRpc.{h,cpp}`. Enable/disable per window via the toolbar toggle, `--mcp` flag, or `config.json`; the toggle registers this window as a hub **session**.

- **Transport = broker** (`ShaderLabMcpBroker`): a client's unpackaged **shim** (`--stdio`, distributed to `%LOCALAPPDATA%\ShaderLab\bin\`, update-immune) activates the packaged **hub** (`--hub`, blind relay routing on `{channelId, seq}`; shim↔session bodies sealed P-256/HKDF/AES-GCM), lists **sessions** (GUID-identified per window), `use_session <id>` to pin one. `ShaderLabHeadless --mcp-session` registers a headless session.
- **25 engine-pure routes** (`EngineMcpRoutes.cpp`) via `IEngineCommandSink` — `/graph/apply`, `/effects`, `/graph/overview`, `/display/info`, etc. In the GUI, `GuiEngineCommandSink::Dispatch` marshals to the **render thread** with `ctx.dc = RenderD2DContext()`, then fires the 8 event hooks so MCP mutations look native. **16 app-side routes** (`MainWindow.McpRoutes.cpp`): UI-coupled + `/context`/`/perf`/`/node/<id>/logs`. Tools whose backing route is absent on a host return isError "Tool not available" via `HasSpecificRoute`.
- **39 tools** in `tools/list` (see [docs/hosts/mcp-server.md](../docs/hosts/mcp-server.md)). Pairing: strict PFN for sessions, role-relaxed for the shim; `DefaultPipeBaseName()` is the shared meeting point. Full design: [docs/development/mcp-stdio-migration.md](../docs/development/mcp-stdio-migration.md).

The **Working Space** parameter node — a strict sink with no input pins — mirrors the active display profile (live or simulated preset/ICC) into 14 typed analysis output fields. Bind any downstream property to drive an effect from the live working space. Updated by `Rendering::UpdateWorkingSpaceNodes` on the render worker.

### Effect Designer
- Three shader types: pixel (`ps_5_0`), D2D compute (`cs_5_0`), **D3D11 compute** (`cs_5_0`, host-dispatched).
- Parameter types: float, float2, float3, float4, int, uint, bool, enum; analysis output fields with typed declarations.
- HLSL auto-formatting and scaffold generation per shader type (D3D11 scaffold injects auto `Width`/`Height` cbuffer + stride-reduction template).
- "Edit in Effect Designer" opens any built-in effect for inspection / fork; Add to Graph / Update in Graph buttons.
- Talks to `MainWindow` only through two `std::function` callbacks — no back-pointer.

### Versioning
- `Version.h`: App **1.7.3**, Graph format version **2**, plus `LibraryVersion()` (sum of all effect versions).
- `EngineExport.h::SHADERLAB_ENGINE_ABI_VERSION` = **1** (independent of app version; mismatch between header and DLL aborts startup with a friendly message box).
- Saved graphs include `formatVersion` + `appVersion`; loading newer-format graphs shows an error dialog. Per-effect `effectId`/`effectVersion` round-trip and surface upgrade prompts.

### UI / UX
- Segoe Fluent Icons toolbar with tooltips; status bar shows pipeline / display / FPS; title bar shows app version + library version.
- `.effectgraph` file-type association + Ctrl+S accelerators + unsaved-changes guard + async save/load with progress dialog.
- Status-bar broom button runs both reapers (orphan graph media + bytecode-cache drift) and reports freed bytes.
- Auto-arrange resets viewport; new nodes spawn at the center of the current viewport.

---

## D2D Custom Effect Gotchas (Hard-Won Knowledge)

These are critical lessons learned during development. Any AI agent or developer working on custom D2D effects **must** be aware of these:

1. **Typed cbuffer pack (Phase 3+)**: `uint`, `int`, and `bool` cbuffer slots in HLSL are now packed correctly even when the corresponding `PropertyValue` is stored as `float` (the default for enum-style parameters). The `Effects::PackPropertyToCBuffer` helper reflects each cbuffer variable's `D3D_SHADER_VARIABLE_TYPE` and converts via `static_cast<uint32_t>` / `<int32_t>` / `BOOL` before writing. So you *can* declare `uint Mode` in HLSL and use clean `if (Mode == 1)` comparisons. **Pre-Phase-3 historical convention** (still works, used by all existing ShaderLab effects): declare enums as `float` in HLSL with `> 0.5` / `> 1.5` threshold comparisons.
2. **HLSL compiler optimizes out cbuffer variables** not referenced on ALL code paths when `D3DCOMPILE_WARNINGS_ARE_ERRORS` is set. Read all cbuffer vars at top of `main()` before any branches.
3. **D2D custom effects need TWO evaluation passes** for newly created effects — first creates/initializes, second produces correct output. The evaluator handles this with `m_justCreated` deferring analysis readback by one frame.
4. **`RegisterWithInputCount` requires `inputCount >= 1`**. Zero-input source effects use a hidden dummy 1×1 bitmap input.
5. **`MapInputRectsToOutputRect` with `SetFixedOutputSize`** must check fixed size FIRST, before input rect.
6. **D2D `TEXCOORD` values are in pixel/scene space**, NOT normalized [0,1]. Use `GetDimensions()` and divide, or call `Source.Load(int3(uv, 0))` directly.
7. **D2D custom effect transforms must NOT pass through infinite input rects** in `MapInputRectsToOutputRect`. Store the requested output rect from `MapOutputRectToInputRects` and return it.
8. **`ForceUploadConstantBuffer()` uploads cbuffer but doesn't invalidate cached output**. Need input toggle trick (disconnect+reconnect dummy input) to force re-evaluation.
9. **Variable-input D2D custom effects** (`<Inputs minimum='0' maximum='8'/>`) require BOTH `ID2D1Effect::SetInputCount(N)` (external) AND updating the transform node's internal count. Without the external call, `SetInput()` fails with `E_INVALIDARG`.
10. **Monitor gamut from `DXGI_OUTPUT_DESC1` primaries** (`RedPrimary`, `GreenPrimary`, `BluePrimary`, `WhitePoint`). Always write primaries into the cbuffer on every evaluate (correct on first frame), only mark dirty on actual change (prevents feedback loops).
11. **D2D → D3D11 texture handoff requires `dc->Flush()`** between `DrawImage` and any D3D11 read of the underlying texture. D2D batches commands until `EndDraw()` or `Flush()` — without an explicit flush, D3D11 reads zeros. Applied in `DispatchUserD3D11Compute`.
12. **`ProcessDeferredCompute` requires an active D2D draw session** (decision #63). It calls `dc->DrawImage` internally to pre-render the upstream chain into an FP32 bitmap, and outside `BeginDraw`/`EndDraw` that DrawImage silently no-ops — the compute reads black input and emits Min/Max/Mean = 0. The GUI's render path, the headless host's `runEval` / `RunRender`, and the test bench all wrap accordingly.
13. **D3D11 compute output → D2D bitmap interop**: `CreateBitmapFromDxgiSurface` must set `bp.dpiX/dpiY = 96.0f`. Default 0 DPI causes `GetImageLocalBounds` to return zero-size bounds.
14. **D3D11 multithread protection** (`ID3D10Multithread::SetMultithreadProtected(TRUE)`) must be enabled when using DXVA2 video decode on background threads with `Lock2D` on GPU buffers.
15. **D3D11 compute cbuffers**: when HLSL declares `uint`/`int`/`bool` but the property is stored as `float`, the pack code must reflect the declared `D3D_SHADER_VARIABLE_TYPE` and `static_cast` to the right type before writing — raw `memcpy` of a float bit-pattern produces nonsense ints/uints.

---

## Build / Deploy / Launch

### Prerequisites
- Visual Studio 2022 17.8+ **or** VS 2026 (C++ Desktop + UWP workloads).
- Windows App SDK 1.8; Windows 10 SDK 10.0.26100+; PowerShell 5.1+.
- Git — `exprtk` + `miniz` are **submodules** (decision #69); clone with `--recurse-submodules` or run `git submodule update --init --recursive`. A clone without them fails fast via the `VerifySubmodules` MSBuild target. `third_party/miniz_export.h` is an in-tree shim, not part of the submodule.

### Build
```pwsh
# Via Visual Studio: open ShaderLab.slnx → Build (Debug | x64 or Debug | ARM64)

# Via MSBuild (x64 host)
msbuild ShaderLab.slnx /p:Configuration=Debug /p:Platform=x64

# On an ARM64 host you MUST use the ARM64-native MSBuild — see the
# "Building ARM64 on an ARM64 host" section of docs/development/build.md
# for why the default 32-bit MSBuild fails with PCH out-of-memory errors.
```

`scripts\EnsureDevCert.ps1` runs automatically on first build (local `CN=ShaderLab` F5 cert). NuGet restores automatically (packages.config style).

### Deploy (local F5)
```pwsh
Add-AppxPackage -Register "<Platform>\<Config>\ShaderLab\AppxManifest.xml"
```
**Never deploy from `AppX\`** — it accumulates stale artifacts that cause XAML 0xc000027b crashes. Close running instances before redeploying.

### Verification
```pwsh
<Platform>\<Config>\ShaderLabTests\ShaderLabTests.exe --adapter warp   # 261 unit tests
pwsh -NoProfile -File .\Tests\RunBrokerSmoke.ps1 -Platform ARM64        # broker smoke 26/26
# MCP suite is shim-driven (no HTTP). Against a running GUI (shim activates the hub):
pwsh -NoProfile -File .\Tests\RunTests.ps1 -HubAumid 'ShaderLab_9v3yd384n9j18!Hub'
# or against a headless session (what CI does; GUI-only tests self-skip):
#   $env:SHADERLAB_MCP_ALLOW_UNPACKAGED='1'
#   ShaderLabMcpBroker.exe --hub --pipe P ; ShaderLabHeadless.exe --graph fixture --mcp-session --pipe P --adapter warp
#   pwsh -NoProfile -File .\Tests\RunTests.ps1 -Pipe P
.\Tests\RunHeadlessSmoke.ps1 -Configuration Debug -Platform x64        # headless smoke
```

### CI / Releases
`.github/workflows/ci.yml`: `build-and-test` (Debug+Release x64, WARP unit tests) + `clean-clone-smoke` (checks out **without** submodules, runs the documented submodule-init command explicitly, builds, tests, headless smoke). `release.yml` runs an x64 + ARM64 matrix and injects the unsigned-namespace OID into the manifest just before MSBuild; end-user `Install.ps1` installs dependency MSIXes then ShaderLab.

---

## Project Structure

The annotated per-file tree lives in [docs/development/project-structure.md](../docs/development/project-structure.md) — maintained there, not here. Orientation summary:

```
ShaderLab\            4 vcxproj at repo root; MainWindow.xaml.cpp (~5000 lines) + sibling
│                     partial TUs (WorkingSpace / GraphFileIo / RenderTick / McpRoutes)
├── Engine\Mcp\       McpRouter + McpTypes + McpJsonRpc + McpToolCatalog + McpTimeouts + 25 engine routes;
│                     broker plumbing: McpFrame + McpCrypto + McpPeerIdentity + McpChannel;
│                     McpSessionClient (registers a session with the hub; used by headless + GUI)
├── ShaderLabMcpBroker\  hub relay + stdio shim (Main.cpp); compiles the Mcp* plumbing directly
├── Graph\            EffectGraph DAG + GraphUiSnapshot (immutable per-frame UI copy)
├── Rendering\        Evaluator, RenderThreadDispatcher, display/ICC, readback, .effectgraph zip
├── Effects\          Effect catalogs, custom-effect COM classes, compute bridge, BytecodeCache
├── Controls\         Canvas editor, output windows, inspectors, log windows (app-only)
├── ShaderLabHeadless\ Console host   ├── Tests\  Runner + math bench + PS1 suites
└── third_party\      exprtk + miniz submodules + miniz_export.h shim
```

---

## Active Development Focus

**MCP transport migration: HTTP → stdio + broker — COMPLETE** (branch `user/daspr/mcp_migration_httptostdio`; decision #71, supersedes #31/#58; engine ABI **3**). Full plan + the end-of-migration manual sweep: [docs/development/mcp-stdio-migration.md](../docs/development/mcp-stdio-migration.md).

The embedded Winsock HTTP listener is deleted. The transport is now: a client's unpackaged **shim** (`ShaderLabMcpBroker --stdio`, copied to `%LOCALAPPDATA%\ShaderLab\bin\` via rename-then-write, so it survives ShaderLab updates) activates a singleton packaged **hub** (`--hub`, a blind relay routing on a clear `{channelId, seq}` frame header; shim↔session bodies sealed with ephemeral P-256 ECDH → HKDF → AES-256-GCM) which relays to per-window **sessions** (`McpSessionClient`, GUID-identified, so `use_session` pins a stable graph across hub restarts). This fixed the three original defects: multiple windows are now addressable, the unauthenticated loopback port is gone, and clients get the stdio transport they expect. The engine keeps the pure routing surface (`McpRouter::{AddRoute,RouteRequest,HasRoute}` + the `McpJsonRpc` dispatcher + the 39-tool `McpToolCatalog`); GUI tool calls still marshal to the render worker via `GuiEngineCommandSink::Dispatch` and fire the 8 event hooks. Along the way: `RenderThreadDispatcher` fails pending `DispatchSync` promises fast on shutdown/reset (no 30 s stall), the timeout ladder lives in `Engine/Mcp/McpTimeouts.h`, `SwitchAdapter` gates the sink to 503, pairing is role-aware (strict PFN for sessions, relaxed for the shim), and the dead pre-worker tick path was removed. Verified green: 261 unit tests, broker smoke 26/26, headless smoke, and the shim-driven `RunTests.ps1` at 40/40 (GUI) / 21/21 (headless) on WARP; grep for `47808`/`WSA` is clean. **Remaining before full sign-off:** the manual verification sweep (WinUI window lifecycle, packaged install/activation, a real MCP client, in-place upgrade) at the end of the migration doc.

Recently shipped context: **1.6.0** was the Phase 8 GPU-binding release (SRV routing between compute effects, bytecode cache + disk persistence, `CustomComputeBridgeEffect`, ICtCp Tone Map on compute, Vectorscope/Waveform removed); **1.7.0** was the render-worker-thread release (decision #68); **1.7.1–1.7.3** were targeted fixes (deferred-compute regression, Win2D removal, clock-controls-on-load + `visibleWhen` pin extension).

The product thesis carries through: I (intensity) is decoupled from Ct/Cp in ICtCp, so manipulating I alone preserves hue and saturation by construction; the empirical fidelity loop (`Working Space` + `Delta E Comparator` Grayscale dE + `Luminance Statistics` live readout) tunes effect parameters against measured CIEDE2000 rather than visual impression. The MCP work is what lets agents drive that loop reliably across multiple sessions.

---

## Potential Future Work

- **More tone-mapping operators** in the ICtCp subcategory (BT.2390, hue-preserving ACES, adaptive).
- **Auto-bind affordances** so SDR-white / monitor-peak hidden defaults can be wired from any matching upstream output without manual binding.
- **Effect Designer export** — emit standalone C++ header / module files for D3D11 compute effects.
- **External binary import** — load pre-compiled D2D effect DLLs (`ID2D1EffectImpl`) and `.cso` compute binaries directly into the graph.
- **Multi-dispatch GPU reduction pyramid** for images > ~33 MP (current `D3D11ComputeRunner` dispatches a single 1024-thread group).
- **Hide `Prim*` data pins from OOG-style nodes** — host-managed hidden properties should never surface as connectable orange diamonds.
