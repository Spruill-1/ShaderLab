# ShaderLab — Development Context (Resume Point)

## How to use this file

A fast orientation to the **shape** of the project: what the pieces are, why they are
split that way, and the hard-won rules that are expensive to rediscover.

**It deliberately carries no counts, versions, or test totals.** Those rot between
edits and made this the most drift-prone file in the repo — it once claimed engine ABI
1 while three other lines in it said 3. Live numbers come from:

| Question | Authority |
|---|---|
| App / graph-format version | `Version.h` |
| Engine ABI | `EngineExport.h::SHADERLAB_ENGINE_ABI_VERSION` |
| What changed, when, and why | [`CHANGELOG.md`](../CHANGELOG.md) |
| Architectural decisions + rationale | [`docs/history/decision-log.md`](../docs/history/decision-log.md) |
| Effect catalog | [`docs/effects/builtin-catalog.md`](../docs/effects/builtin-catalog.md) (a test pins the count) |
| MCP tools / routes | [`docs/hosts/mcp-server.md`](../docs/hosts/mcp-server.md) |
| Test totals | run the suite — it prints them |
| Agent working rules | [`CLAUDE.md`](../CLAUDE.md), [`.github/copilot-instructions.md`](../.github/copilot-instructions.md) |
| Everything else, in depth | [`docs/`](../docs/README.md) |

If you find yourself adding a number here, put it in one of those instead.

---

## Project Identity

**ShaderLab** is a WinUI 3 desktop application (C++/WinRT) for developing, testing, and
debugging Direct2D shader effects with full HDR and wide colour gamut (WCG) support,
with a particular focus on tone-mapping and colour-correction R&D.

- **Language**: C++/WinRT — direct COM access to `ID2D1EffectImpl`,
  `ID2D1DrawTransform`, `ID2D1ComputeTransform`. No C#.
- **Solution**: `ShaderLab.slnx` at the repo root.

---

## Solution Layout

| Project | Output | Purpose |
|--------|--------|---------|
| `ShaderLabEngine.vcxproj` | `ShaderLabEngine.dll` | Host-agnostic engine: graph model + `GraphUiSnapshot`, evaluator, `RenderThreadDispatcher`, ICC reader, video + live-capture sources, ExprTk math, D3D11 compute runner + `CustomComputeBridgeEffect` + `BytecodeCache`, `IEngineComputeOutput` COM interface, and the whole MCP protocol surface (router, JSON-RPC dispatcher, tool catalog, engine-pure routes, broker crypto). Exported via `SHADERLAB_API`. |
| `ShaderLab.vcxproj` | `ShaderLab.exe` (MSIX) | WinUI 3 packaged app. `RenderEngine` (app-only), all XAML, controllers, the render worker thread, and the UI-coupled MCP routes in `MainWindow.McpRoutes.cpp` + `GuiEngineCommandSink`. Depends on the engine DLL. |
| `ShaderLabTests.vcxproj` | `ShaderLabTests.exe` | Standalone console runner — graph/evaluator/bindings, bytecode cache, snapshot, dispatcher, MCP router + JSON-RPC contracts, broker frame codec / crypto / peer identity, GPU-binding matrices, and the HLSL math bench. No WinUI; CI runs it on `--adapter warp`. |
| `ShaderLabHeadless.vcxproj` | `ShaderLabHeadless.exe` | Console host, no WinUI: image render (PNG or JPEG XR, chosen by output extension), `.effectgraph` ZIP + embedded media, FP32 pixel readback, JSON batch script mode, MCP session mode, bytecode-cache ops. |
| `ShaderLabMcpBroker.vcxproj` | `ShaderLabMcpBroker.exe` | MCP transport. `--hub`: singleton blind relay — first-instance election, per-peer pairing, session registry, channel relay (routes on channelId only; bodies sealed end-to-end). `--stdio`: the client's front-end — owns initialize + `list_sessions`/`use_session`, pins a session, runs the initiator handshake, seals/forwards requests, splices `tools/list`. Does **not** link the engine; compiles the `Mcp*` plumbing TUs directly. Packaged as the manifest's second `<Application Id="Hub">`. |

This split (decisions #41 + #58) keeps WinUI out of the test path, lets engine logic be
exercised in isolation, and gives MCP agents a fully-functional logged-out host for
parameter sweeps.

---

## Threading Model

All D3D11/D2D graph work runs on a dedicated **render worker `std::jthread`**; the UI
thread only blits a double-buffered offscreen into the `SwapChainPanel` swap chain and
`Present1`s (presenting from the worker is impossible — XAML composition is STA-bound).
Per worker tick: drain `RenderThreadDispatcher` closures → working-space sync →
live-capture/clock/video tick → dirty-propagation BFS → `RenderFrameToOffscreen` →
publish index + `GraphUiSnapshot`. A version-gated blit keeps the UI thread from
vsync-blocking when the worker publishes slower than the UI ticks.

**Graph access rule** — getting it wrong is an access violation inside `std::map`, not a
compile error. Full text in `Controls/NodeGraphController.h`, `CLAUDE.md`, and
`.github/copilot-instructions.md`:

1. **UI-thread reads → the per-frame `GraphUiSnapshot`**, never live `m_graph`.
2. **Writes (any thread) → `RenderThreadDispatcher::DispatchSync`.**
3. **Layout computation → live graph, render thread only.**

Two locks with a strict order (`m_graphMutex` → `m_visualsMutex`); never hold
`m_visualsMutex` across a `DispatchSync`. Diagrams and the resource-ownership table:
[docs/architecture/threading-model.md](../docs/architecture/threading-model.md).

---

## Feature Shape

### Core
- Node-based DAG graph editor for D2D effect composition.
- Wrapped built-in D2D effects, grouped by category (`Effects/EffectRegistry.cpp`), plus
  the ShaderLab effect library with embedded HLSL (`Effects/ShaderLabEffects.cpp` +
  `Effects/ColorMath.cpp`).
- Three custom-effect flavours: pixel (`ID2D1DrawTransform`), D2D compute
  (`ID2D1ComputeTransform`, per-tile), and **D3D11 compute** — the last routed through
  `CustomComputeBridgeEffect` + `D3D11ComputeRunner`, which implements
  `IEngineComputeOutput` so downstream compute consumers can bind analysis SRVs directly.
- **`BytecodeCache`**: compile-once store with eager GPU-binding-variant precompile and
  disk persistence under `%LOCALAPPDATA%\ShaderLab\bytecode\`; reaper on the status-bar
  broom button and headless CLI flags.
- Live HLSL hot-reload with `D3DCompile` + `D3DReflect` auto-property discovery.
- Effect Designer modal for authoring custom effects of all three shader types.
- Graph serialization as `.effectgraph` ZIP (DEFLATE via miniz) with optional embedded
  media; `media://` tokens rewritten to extracted paths on load.

### Effect categories
Grouped by `category` + optional `subcategory`, which drives the Add Node flyout.
Members change; the structure doesn't — full table in
[builtin-catalog.md](../docs/effects/builtin-catalog.md).

- **Analysis → Highlights** — false-colour / heatmap views of where the energy is.
- **Analysis → Scopes** — CIE histogram and chromaticity plot.
- **Analysis → Comparison** — Delta E Comparator (Heatmap / Grayscale dE; prefer the
  **ΔE ITP** method for HDR/WCG), Split Comparison.
- **Analysis → Gamut Mapping** — Gamut Map, ICtCp Gamut Map, Gamut Coverage.
- **Analysis → Tone Mapping (the ICtCp suite)** — forward/inverse tone map, saturation,
  highlight desaturation, round-trip validator. Bind their nit parameters to the
  `Working Space` node to track Display Settings or a simulated profile automatically.
- **Analysis → Statistics** (compute, data-only) — channel / luminance / chromaticity.
  Not architecturally special (decision #63): agents use `/graph/add-node` + `/analysis/<id>`.
- **Source / Generators** — synthetic patterns and gamut sources.
- **Live capture sources** — DXGI Desktop Duplication (per-output) and Windows Graphics
  Capture; ticked per frame by `SourceNodeFactory::TickAndUploadLiveCaptures`.
- **Data / Parameter nodes** (no shader, evaluator-handled) — Float / Integer / Toggle /
  Gamut, Clock, Numeric Expression (ExprTk), Random, and **Working Space**.

Every effect carries a stable `effectId` + numeric `effectVersion`; saved graphs detect
upgrades and offer per-node / batch upgrade in the Properties panel.

### Property system
- `PropertyValue` variant: `float`, `int32`, `uint32`, `bool`, `wstring`, `float2/3/4`,
  `D2D1_MATRIX_5X4_F`, `vector<float>`.
- Per-component property bindings (Grasshopper-style data flow), plus whole-array
  bindings for LUT-shaped fields. `gpuBindable` parameters + `gpuPublish` analysis
  fields route upstream compute SRVs straight to D3D11 compute consumers, skipping CPU
  readback when no CPU consumer needs the value.
- Enum labels render as dropdowns; `bool` as a `ToggleSwitch`.
- `visibleWhen` conditional visibility on parameters *and* input pins.
- Visual data pins (orange diamonds) on the canvas for binding connections.

### Rendering
- **Always scRGB FP16** (`DXGI_FORMAT_R16G16B16A16_FLOAT`,
  `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`). No format switching; DWM/ACM handles the
  final display conversion.
- **No built-in tone-mapping pass** — users compose tone mappers as graph effects, with
  the ICtCp suite as the preferred path.
- Refresh-rate-driven loop, re-derived from `EnumDisplaySettings` on every display
  change. Dirty-gated evaluation with a dirty-propagation pre-pass.
- Display profile mocking (presets + ICC via `mscms.dll`); monitor gamut from
  `DXGI_OUTPUT_DESC1` primaries; OS-reported SDR white level via
  `DisplayConfigGetDeviceInfo`, exposed to graphs as `working_space.SdrWhiteNits`.
- DXVA2 / Media Foundation video sources with `ID3D10Multithread` protection.
- `OutputWindow`: each `Output` node gets its own OS window, worker renders native-size,
  UI fits + presents. Bidirectional sync (close window ↔ delete node).
- D2D-rendered canvas with pan/zoom, bezier edges, colour-coded nodes; paints from the
  `GraphUiSnapshot`.

### MCP
JSON-RPC 2.0 over **stdio via the broker** — there is no HTTP listener (deleted in
stdio-migration Step 9). Enable per window via the toolbar toggle, `--mcp`, or
`config.json`; the toggle registers that window as a hub **session**.

Transport is **shim → hub → session**: a client's unpackaged shim
(`ShaderLabMcpBroker --stdio`, copied to `%LOCALAPPDATA%\ShaderLab\bin\` by
rename-then-write so it survives app updates) activates the singleton packaged hub,
which relays blindly on a clear `{channelId, seq}` header while shim↔session bodies stay
sealed (ephemeral P-256 ECDH → HKDF → AES-256-GCM). Sessions are GUID-identified, so
`use_session` pins a stable graph across hub restarts.

Engine-pure routes live in `EngineMcpRoutes.cpp` and reach the host through
`IEngineCommandSink`; the GUI's sink marshals to the render worker and then fires event
hooks so MCP mutations look native. UI-coupled routes stay in `MainWindow.McpRoutes.cpp`;
a tool whose backing route is absent on the answering host returns an `isError`
"Tool not available". Design: [mcp-server.md](../docs/hosts/mcp-server.md) and
[mcp-stdio-migration.md](../docs/development/mcp-stdio-migration.md).

The **Working Space** node — a strict sink with no input pins — mirrors the active
display profile (live or simulated) into typed analysis output fields, so any downstream
property can be driven from the real working space. Updated by
`Rendering::UpdateWorkingSpaceNodes` on the render worker.

### Judging HDR output
An agent's vision input is 8-bit SDR, so a captured PNG of an HDR frame has already
clipped everything above scRGB 1.0 and lost wide-gamut negatives — judging a tone mapper
from a tone-mapped screenshot is circular. Prefer numbers (FP32 readback, analysis
fields), then measured difference (Delta E Comparator with **ΔE ITP**), then diagnostic
renders that encode HDR facts into SDR-visible form. Full rule in `CLAUDE.md`
§*Looking at HDR output*.

---

## D2D Custom Effect Gotchas (Hard-Won Knowledge)

Stable, expensive to rediscover, and mirrored in `.github/copilot-instructions.md` —
change one, check the other. Anyone touching custom D2D effects must know these:

1. **Typed cbuffer pack**: `uint` / `int` / `bool` cbuffer slots are packed correctly
   even when the `PropertyValue` is stored as `float` (the default for enum-style
   parameters) — `Effects::PackPropertyToCBuffer` reflects each variable's
   `D3D_SHADER_VARIABLE_TYPE` and converts before writing. So `uint Mode` with clean
   `if (Mode == 1)` works. The older convention (declare enums as `float`, compare with
   `> 0.5` / `> 1.5`) still works and is what most existing effects use.
2. **The HLSL compiler optimizes out cbuffer variables** not referenced on *all* code
   paths under `D3DCOMPILE_WARNINGS_ARE_ERRORS`. Read every cbuffer var at the top of
   `main()` before branching.
3. **New D2D custom effects need TWO evaluation passes** — the first creates and
   initializes, the second produces correct output. The evaluator handles this with
   `m_justCreated` deferring analysis readback by one frame.
4. **`RegisterWithInputCount` requires `inputCount >= 1`.** Zero-input source effects
   use a hidden dummy 1×1 bitmap input.
5. **`MapInputRectsToOutputRect` with `SetFixedOutputSize`** must check the fixed size
   FIRST, before the input rect.
6. **D2D `TEXCOORD` is pixel/scene space**, not normalized [0,1]. Use `GetDimensions()`
   and divide, or `Source.Load(int3(uv, 0))` directly.
7. **Transforms must NOT pass through infinite input rects** in
   `MapInputRectsToOutputRect` — store the requested output rect from
   `MapOutputRectToInputRects` and return that.
8. **`ForceUploadConstantBuffer()` uploads the cbuffer but does not invalidate cached
   output.** Forcing re-evaluation needs the input-toggle trick.
9. **Variable-input custom effects** (`<Inputs minimum='0' maximum='8'/>`) need BOTH
   `ID2D1Effect::SetInputCount(N)` externally AND the transform node's internal count
   updated. Without the external call `SetInput()` fails `E_INVALIDARG`.
10. **Monitor gamut comes from `DXGI_OUTPUT_DESC1` primaries.** Write them into the
    cbuffer on every evaluate (so frame one is right), but only mark dirty on actual
    change (or property writes and evaluation feed back into each other).
11. **D2D → D3D11 texture handoff requires `dc->Flush()`** between `DrawImage` and any
    D3D11 read. D2D batches until `EndDraw()` or `Flush()`; without it D3D11 reads zeros.
12. **`ProcessDeferredCompute` requires an active D2D draw session** (decision #63). It
    calls `dc->DrawImage` internally; outside `BeginDraw`/`EndDraw` that silently no-ops
    and the compute reads black, emitting Min/Max/Mean = 0 with no error anywhere.
13. **D3D11 compute output → D2D bitmap interop** must set `bp.dpiX/dpiY = 96.0f`.
    Default 0 DPI makes `GetImageLocalBounds` return zero-size bounds.
14. **`ID3D10Multithread::SetMultithreadProtected(TRUE)`** is required when DXVA2 video
    decode runs on background threads with `Lock2D` on GPU buffers.
15. **scRGB is signed on purpose.** Negative Rec.709 components are how wide-gamut colour
    is expressed. A `max(rgb, 0)` or `saturate()` at the top of a colour transform is a
    gamut clip, not a safety net — this silently sRGB-clipped the entire ICtCp suite
    once. Use the signed PQ helpers in `Effects/ColorMath.cpp`.

---

## Build / Deploy / Launch

Commands and the platform traps live in [docs/development/build.md](../docs/development/build.md),
`CLAUDE.md`, and the `.claude/skills/shaderlab-build` + `shaderlab-run` skills. The
shape:

- **Prerequisites** — Visual Studio 2022 17.8+ or VS 2026 (C++ Desktop + UWP workloads),
  Windows App SDK, a recent Windows SDK, PowerShell.
- **Submodules** — `exprtk` + `miniz` (decision #69). Clone with `--recurse-submodules`
  or run `git submodule update --init --recursive`; a clone without them fails fast via
  the `VerifySubmodules` MSBuild target. `third_party/miniz_export.h` is an in-tree shim,
  not part of the submodule.
- **ARM64 hosts need the ARM64-native MSBuild.** The default is 32-bit and picks a
  toolset that dies with misleading PCH out-of-memory errors. CI cross-compiles ARM64
  from an x64 runner, so only the `native-arm64` job covers this.
- **Deploy from the layout root, never `AppX\`** — `AppX\` accumulates stale artifacts
  and produces XAML `0xc000027b` crashes. Close running instances first; the GUI locks
  the engine DLL, and a lingering broker hub locks the broker copy.
- **Launch by shell activation**, not the exe directly — a packaged app started
  directly aborts in CRT dependency resolution.
- **Verification** — unit runner (WARP), headless smoke, broker smoke, and the
  shim-driven MCP suite. Each prints its own totals.

---

## Project Structure

The annotated per-file tree is maintained in
[docs/development/project-structure.md](../docs/development/project-structure.md), not
here. Orientation only:

```
ShaderLab\            vcxproj files at repo root; MainWindow.xaml.cpp + sibling partial
│                     TUs (WorkingSpace / GraphFileIo / RenderTick / McpRoutes)
├── Engine\Mcp\       Router + types + JSON-RPC + tool catalog + timeouts + engine routes;
│                     broker plumbing (frame codec, crypto, peer identity, channel);
│                     McpSessionClient (registers a session with the hub; headless + GUI)
├── ShaderLabMcpBroker\  hub relay + stdio shim (Main.cpp); compiles the Mcp* plumbing directly
├── Graph\            EffectGraph DAG + GraphUiSnapshot (immutable per-frame UI copy)
├── Rendering\        Evaluator, RenderThreadDispatcher, display/ICC, readback, .effectgraph zip
├── Effects\          Effect catalogs, custom-effect COM classes, compute bridge, BytecodeCache
├── Controls\         Canvas editor, output windows, inspectors, log windows (app-only)
├── ShaderLabHeadless\ Console host   ├── Tests\  Runner + math bench + PS1 suites
└── third_party\      exprtk + miniz submodules + miniz_export.h shim
```

---

## Product Thesis

I (intensity) is decoupled from Ct/Cp in ICtCp, so manipulating I alone preserves hue
and saturation *by construction*. That is why the tone-mapping work lives in ICtCp
rather than linear RGB or xyY, where the same operations turn into hue shifts and gamut
excursions.

Around it sits the **empirical fidelity loop**: `Working Space` (the real display
profile) + `Delta E Comparator` in Grayscale dE mode + `Luminance Statistics`, giving a
live measured colour-difference readout while a parameter sweeps. Effects get tuned
against measured difference, not visual impression — and for HDR/WCG that measurement
must use **ΔE ITP**, since the CIE Lab metrics leave their fitted domain above roughly
100 nits.

The MCP work exists to let agents drive that loop reliably across multiple windows.

---

## Potential Future Work

- **More tone-mapping operators** in the ICtCp subcategory (BT.2390, hue-preserving
  ACES, adaptive).
- **Auto-bind affordances** so SDR-white / monitor-peak hidden defaults can be wired
  from any matching upstream output without manual binding.
- **Effect Designer export** — emit standalone C++ header / module files for D3D11
  compute effects.
- **External binary import** — load pre-compiled D2D effect DLLs (`ID2D1EffectImpl`) and
  `.cso` compute binaries directly into the graph.
- **Multi-dispatch GPU reduction pyramid** for images beyond what a single thread group
  can reduce.
- **Hide `Prim*` data pins from OOG-style nodes** — host-managed hidden properties should
  never surface as connectable orange diamonds.
- **Content-adaptive screenshot tone mapping** — drive the knee from a statistics pass
  (fraction of frame above SDR white, percentile content peak) plus a bypass fast path
  when nothing exceeds it, so pure-SDR captures stay bit-exact.
