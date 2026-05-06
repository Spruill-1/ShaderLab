# ShaderLab — Development Context (Resume Point)

## Project Identity

**ShaderLab** is a WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D shader effects with full HDR and wide color gamut (WCG) support, with a particular focus on tone-mapping and color-correction R&D.

- **Location**: `E:\source\ShaderLab\ShaderLab.slnx`
- **Version**: **1.4.0** (app) / format version **2** (graph serialization)
- **Language**: C++/WinRT — direct COM access to `ID2D1EffectImpl`, `ID2D1DrawTransform`, `ID2D1ComputeTransform`
- **Branch / repo state**: `main`, tagged `v1.4.0`, working tree clean

> Authoritative sources of truth: `README.md` (architecture + decision log), `CHANGELOG.md` (per-version diffs), `Version.h` (numeric version), `.github/copilot-instructions.md` (AI agent rules). This file is a fast-orientation summary; it can drift — re-check the README before relying on details.

---

## Solution Layout (3 projects)

| Project | Output | Purpose |
|--------|--------|---------|
| `ShaderLabEngine.vcxproj` | `ShaderLabEngine.dll` | Pure-native engine: graph model, evaluator, tone mapper, ICC reader, video, ExprTk math, GPU reduction. Exported via `SHADERLAB_API`. |
| `ShaderLab.vcxproj` | `ShaderLab.exe` (MSIX) | WinUI 3 packaged app. `RenderEngine`, all XAML, controllers, MCP server. Depends on the engine DLL. |
| `ShaderLabTests.vcxproj` | `ShaderLabTests.exe` | Standalone console test runner (`Tests/TestRunner.cpp`). No XAML. CI uses `--adapter warp`. |

This split (decision #41) keeps WinUI out of the test path and lets engine logic be exercised in isolation.

---

## Complete Feature Set (v1.4.0)

### Core
- Node-based DAG graph editor for D2D effect composition.
- 40+ wrapped built-in D2D effects (`Effects/EffectRegistry.cpp`) across 9 categories.
- 30+ ShaderLab built-in effects (`Effects/ShaderLabEffects.cpp`, ~137 KB embedded HLSL).
- Custom pixel shader effects (`ID2D1DrawTransform`).
- Custom D2D compute shader effects (`ID2D1ComputeTransform`, per-tile dispatch).
- Custom **D3D11 compute shader effects** (`D3D11ComputeRunner` + `StatisticsEffect`) — bypass D2D tiling for full-image reductions with atomics and groupshared memory.
- Live HLSL hot-reload with `D3DCompile` + `D3DReflect` auto-property discovery.
- Effect Designer modal window for authoring custom pixel / D2D-compute / D3D11-compute effects with full parameter definition.
- Graph JSON serialization with versioning (format version 2) — saved as `.effectgraph` zip files (DEFLATE via miniz) with optional **embedded media**.

### ShaderLab Built-in Effects (`Effects/ShaderLabEffects.cpp`)

Grouped by `category` + optional `subcategory` (Add Node flyout sub-grouping):

- **Analysis → Highlights**: Luminance Heatmap, Nit Map, Gamut Highlight, Luminance Highlight.
- **Analysis → Scopes**: CIE Histogram (CS), CIE Chromaticity Plot, Vectorscope, Waveform Monitor.
- **Analysis → Comparison**: Delta E Comparator (CIEDE2000), Split Comparison.
- **Analysis → Gamut Mapping**: Gamut Map (Clip / Nearest / Compress / Fit), ICtCp Gamut Map, Gamut Coverage.
- **Analysis → Tone Mapping (ICtCp suite)**: ICtCp Round-Trip Validator, ICtCp Tone Map (HDR → SDR), ICtCp Inverse Tone Map (SDR → HDR), ICtCp Saturation, ICtCp Highlight Desaturation. Bind their numeric peak/SDR-white parameters to the `Working Space` node's analysis outputs to track Display Settings or simulated profiles automatically.
- **Analysis → Statistics** (D3D11 compute, data-only): Channel Statistics, Luminance Statistics, Chromaticity Statistics. Also `Image Statistics` via `StatisticsEffect`.
- **Source / Generators**: Gamut Source, ICtCp Boundary, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern.
- **Data / Parameter nodes** (no shader, evaluator-handled): Float, Integer, Toggle, Gamut, Clock, Numeric Expression (ExprTk, A..Z inputs), Random (deterministic seed → [0,1) hash).

Every effect carries a stable `effectId` + numeric `effectVersion`; saved graphs detect upgrades and offer per-node / batch upgrade in the Properties panel.

### Property System
- `PropertyValue` variant: `float`, `int32`, `uint32`, `bool`, `wstring`, `float2`, `float3`, `float4`, `D2D1_MATRIX_5X4_F`, `vector<float>`.
- Per-component property bindings (Grasshopper-style data flow), with array (whole-vector) bindings for LUT-shaped fields.
- Enum labels for named dropdown parameters; `bool` rendered as `ToggleSwitch`.
- `_hidden` suffix convention (legacy / compatibility only after decision #51): properties ending in `_hidden` are excluded from the UI and data pins. Older saved graphs may carry stale `WsRedX_hidden` / `MonMaxNits_hidden` / `SdrWhiteNits_hidden` keys; those are inert because the shader cbuffer no longer references them and the host writers were removed. New effects must not declare `_hidden` parameters; use the `Working Space` node + bindings instead.
- `visibleWhen` conditional visibility on parameters (`"Mode == 1"`, `"Strength > 0"`, etc.).
- Visual data pins (orange diamonds) on the node graph for binding connections.

### Rendering
- **Always scRGB FP16 pipeline** (`DXGI_FORMAT_R16G16B16A16_FLOAT`, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`). DWM/ACM handles final display conversion.
- **Refresh-rate-driven render loop** (60–240 Hz) — interval re-derived from `EnumDisplaySettings(dmDisplayFrequency)` on every display change.
- Dirty-gated render loop with **dirty propagation pre-pass** (any dirty node marks its direct downstream consumers dirty before evaluation; runs again after `TickAndUploadVideos` so video updates flow through analysis-only compute nodes too).
- Built-in `ToneMapper` retained (5 modes: None, Reinhard, ACES Filmic, Hable, SDR Clamp) but defaults to **None** — users build tone mappers as graph effects (the ICtCp suite is the preferred path).
- Display profile mocking (presets + ICC file loading via `mscms.dll`).
- Monitor gamut detection from `DXGI_OUTPUT_DESC1` primaries.
- **OS-reported SDR white level** queried via `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)`, tracks the *Settings → Display → HDR → "SDR content brightness"* slider; exposed to graphs as `working_space.SdrWhiteNits`. Effects pull the value via property bindings — no per-frame host injection.
- GPU info display (hardware adapter name or "Software (WARP)").
- **DXVA2 / Media Foundation video sources** with `ID3D10Multithread::SetMultithreadProtected(TRUE)` so background-thread Lock2D from the decoder doesn't crash D3D11.
- `OutputWindow` system: each `Output` node gets its own OS window with independent SwapChainPanel, pan/zoom, save-to-file. Bidirectional sync (close window ↔ delete node).
- D2D-rendered node graph canvas with pan/zoom, bezier edges (Alt+click delete via bezier hit-test), color-coded nodes, dot grid, dark theme.

### MCP Server (37 tools)
Port 47808 (auto-increments), JSON-RPC 2.0 over Streamable HTTP (`POST /`). Implements `Content-Length` **and** `Transfer-Encoding: chunked`. Has `GET /` health-check + correct `202 Accepted` for `notifications/*`. Toolbar shows an amber activity dot when the server has handled a request in the last few seconds.

| Tool | Purpose |
|------|---------|
| `graph_add_node` | Add built-in D2D or ShaderLab effect (placed at viewport center). |
| `graph_remove_node` | Remove a node. |
| `graph_rename_node` | Rename a node. |
| `graph_connect` / `graph_disconnect` | Image-pin connections. |
| `graph_set_property` | Set a node property. |
| `graph_get_node` | Node details + analysis results. |
| `graph_save_json` / `graph_load_json` | Graph serialization round-trip. |
| `graph_clear` | Clear graph (keeps Output). |
| `graph_overview` | Compact graph summary. |
| `graph_bind_property` / `graph_unbind_property` | Data-pin bindings. |
| `effect_compile` | Compile HLSL (+ optional analysisFields). |
| `effect_get_hlsl` | Read a custom-effect node's HLSL source, parameter list, compile state, last error. ShaderLab library effects flagged `isLibraryEffect`. |
| `set_preview_node` | Choose preview target. |
| `render_capture` | Capture preview as PNG (HDR clipped to SDR). |
| `render_capture_node` | Capture any node's image as PNG without changing the preview node. Forces a render frame; `inline=true` returns MCP image content. Disambiguates 404 (missing) vs 409 (`notReady`). |
| `registry_get_effect` | Built-in effect metadata. |
| `read_analysis_output` | Typed analysis fields from compute / analysis / parameter nodes. |
| `read_pixel_trace` | Per-node pixel readback at normalized coords. |
| `read_pixel_region` | Read a small w×h grid (cap 32×32, per-axis 64) of FP32 RGBA scRGB linear-light pixels. |
| `image_stats` | GPU-accelerated per-channel min/max/mean/median/p95/sum + nonzero counts. Channels default to luminance + RGBA. |
| `list_effects` | All effects by category. |
| `get_display_info` | Display caps, active profile, pipeline, app version. |
| `list_display_profiles` | 7 built-in presets + active live/simulated profile + `isSimulated`. |
| `set_display_profile` | Apply simulated profile via `{preset \| presetIndex \| iccPath \| custom}` (mutex). Custom schema = full caps + primaries. |
| `clear_simulated_profile` | Revert to live OS-reported profile. |
| `node_logs` | Per-node timestamped info / warning / error log entries. |
| `perf_timings` | Per-node evaluation timings from the most recent frame. |
| `graph_snapshot` | PNG snapshot of the live node-graph editor view (path; `inline=true` returns MCP image content). |
| `graph_get_view` | Read editor zoom/pan/viewport/content bounds. |
| `graph_set_view` | Apply `{zoom?, panX?, panY?}` to the live editor — same effect as user input. Coord convention: `screen = zoom * canvas + pan`. |
| `graph_fit_view` | Fit view to all nodes with viewport-space `padding` (DIPs, default 40). |
| `preview_get_view` | Read preview pane zoom/pan/image bounds/zoom limits. |
| `preview_set_view` | Apply `{zoom?, panX?, panY?}` to the preview pane. zoom clamped to [0.01, 100.0] (matches wheel-zoom). |
| `preview_fit_view` | Fit preview image to viewport (auto zoom + center). |

The **Working Space** parameter node — a strict sink with no input pins — mirrors the active display profile (live or any simulated preset/ICC) into 14 typed analysis output fields (`ActiveColorMode`, `Hdr/WcgSupported`/`UserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus four CIE-xy primaries as Float2). Bind any downstream property to drive an effect from the live working space — e.g. wire a tone-mapper's peak-nits to `working_space.PeakNits` and it tracks Display Settings or simulated profile changes automatically. Updated by `MainWindow::UpdateWorkingSpaceNodes()`.

All MCP routes that touch graph/UI state marshal work to the UI thread via `MainWindow::DispatchSync`.

### Effect Designer
- Three shader types: pixel (`ps_5_0`), D2D compute (`cs_5_0`), **D3D11 compute** (`cs_5_0`, host-dispatched).
- Parameter types: float, float2, float3, float4, int, uint, bool, enum.
- Enum parameters with comma-separated label definition → ComboBox.
- Bool parameters render as `ToggleSwitch`.
- Analysis output fields with typed declarations (Float, Float2, Float3, Float4, FloatArray, Float2/3/4Array).
- HLSL auto-formatting and scaffold generation per shader type (D3D11 scaffold injects auto `Width`/`Height` cbuffer + stride-reduction template).
- "Edit in Effect Designer" opens any built-in effect for inspection / fork. `LoadDefinition` correctly restores Output Type selector + analysis-field rows.
- Add to Graph / Update in Graph buttons.

### Versioning
- `Version.h`: App **1.4.0**, Graph format version **2**, plus `LibraryVersion()` (sum of all effect versions).
- Status bar shows pipeline / display / FPS; **title bar** shows app version + library version.
- Saved graphs include `formatVersion` + `appVersion`; loading newer-format graphs shows an error dialog. Per-effect `effectId`/`effectVersion` round-trip and surface upgrade prompts.

### UI / UX
- Segoe Fluent Icons toolbar with tooltips.
- `.effectgraph` file-type association (FTA) + Ctrl+S accelerators + unsaved-changes guard + async save/load with progress dialog.
- Auto-arrange resets viewport so off-screen graphs come back into view.
- New nodes spawn at the **center of the current viewport** (graph coords, accounting for pan/zoom).
- Closing an `OutputWindow` forces a single render pass so the deleted Output node disappears immediately.

---

## D2D Custom Effect Gotchas (Hard-Won Knowledge)

These are critical lessons learned during development. Any AI agent or developer working on custom D2D effects **must** be aware of these:

1. **`uint` cbuffer params don't work in D2D pixel shaders** — values pack correctly but the shader never sees updates. Use `float` with threshold comparisons (`> 0.5`, `> 1.5`). All ShaderLab enum parameters use `float` in HLSL for this reason.
2. **HLSL compiler optimizes out cbuffer variables** not referenced on ALL code paths when `D3DCOMPILE_WARNINGS_ARE_ERRORS` is set. Read all cbuffer vars at top of `main()` before any branches.
3. **D2D custom effects need TWO evaluation passes** for newly created effects — first creates/initializes, second produces correct output. The evaluator handles this with `m_justCreated` deferring analysis readback by one frame.
4. **`RegisterWithInputCount` requires `inputCount >= 1`**. Zero-input source effects use a hidden dummy 1×1 bitmap input.
5. **`MapInputRectsToOutputRect` with `SetFixedOutputSize`** must check fixed size FIRST, before input rect.
6. **D2D `TEXCOORD` values are in pixel/scene space**, NOT normalized [0,1]. Use `GetDimensions()` and divide, or call `Source.Load(int3(uv, 0))` directly.
7. **D2D custom effect transforms must NOT pass through infinite input rects** in `MapInputRectsToOutputRect`. Store the requested output rect from `MapOutputRectToInputRects` and return it.
8. **`ForceUploadConstantBuffer()` uploads cbuffer but doesn't invalidate cached output**. Need input toggle trick (disconnect+reconnect dummy input) to force re-evaluation.
9. **Variable-input D2D custom effects** (`<Inputs minimum='0' maximum='8'/>`) require BOTH `ID2D1Effect::SetInputCount(N)` (external) AND updating the transform node's internal count. Without the external call, `SetInput()` fails with `E_INVALIDARG`.
10. **Monitor gamut from `DXGI_OUTPUT_DESC1` primaries** (`RedPrimary`, `GreenPrimary`, `BluePrimary`, `WhitePoint`). Always write primaries into the cbuffer on every evaluate (correct on first frame), only mark dirty on actual change (prevents feedback loops).
11. **D2D → D3D11 texture handoff requires `dc->Flush()`** between `DrawImage` and any D3D11 read of the underlying texture. D2D batches commands until `EndDraw()` or `Flush()` — without an explicit flush, D3D11 reads zeros. Applied in both `ComputeImageStatistics` and `DispatchUserD3D11Compute`.
12. **D3D11 compute output → D2D bitmap interop**: `CreateBitmapFromDxgiSurface` must set `bp.dpiX/dpiY = 96.0f`. Default 0 DPI causes `GetImageLocalBounds` to return zero-size bounds.
13. **D3D11 multithread protection** (`ID3D10Multithread::SetMultithreadProtected(TRUE)`) must be enabled when using DXVA2 video decode on background threads with `Lock2D` on GPU buffers.
14. **D3D11 compute cbuffers**: when HLSL declares `uint`/`int`/`bool` but the property is stored as `float`, the pack code must reflect the declared `D3D_SHADER_VARIABLE_TYPE` and `static_cast` to the right type before writing — raw `memcpy` of a float bit-pattern produces nonsense ints/uints.

---

## Build / Deploy / Launch

### Prerequisites
- Visual Studio 2022 17.8+ **or** VS 2026 Insiders (C++ Desktop + UWP workloads).
- Windows App SDK 1.8.
- Windows 10 SDK 10.0.26100+.
- PowerShell 5.1+.
- Internet on first build (for `EnsureExprTk.ps1` + `EnsureMiniz.ps1`).

### Build
```pwsh
# Via Visual Studio
Open ShaderLab.slnx → Build → Debug | x64

# Via MSBuild
msbuild ShaderLab.slnx /p:Configuration=Debug /p:Platform=x64
```

Pre-build scripts run automatically on first build:
- `scripts\EnsureDevCert.ps1` — generates / installs the local F5 dev cert (`CN=ShaderLab`).
- `scripts\EnsureExprTk.ps1` — downloads `exprtk.hpp` (MIT) into `third_party\exprtk\`.
- `scripts\EnsureMiniz.ps1` — downloads `miniz` (MIT) for `.effectgraph` zip DEFLATE.

NuGet packages restore automatically (packages.config style).

### Configurations
- `Debug | x64`, `Release | x64`, `Debug | ARM64`, `Release | ARM64`.

### Deploy (local F5)
```pwsh
Add-AppxPackage -Register "x64\Debug\ShaderLab\AppxManifest.xml"
```
**Never deploy from `AppX\`** — it accumulates stale artifacts that cause XAML 0xc000027b crashes. Always deploy from `x64\Debug\ShaderLab\AppxManifest.xml`. After building, close existing running instances (`Stop-Process`) before redeploying.

### Releases
GitHub Actions `release.yml` runs as a matrix (x64, ARM64). Just before MSBuild, the workflow injects the unsigned-namespace OID into `Package.appxmanifest`'s `Publisher` so the resulting MSIX is installable via `Add-AppxPackage -AllowUnsigned`. The in-repo manifest stays plain `CN=ShaderLab` so signed F5 deploys keep working. End-user `Install.ps1` detects host arch, installs bundled VCLibs / WindowsAppRuntime dependency MSIXes, then ShaderLab.

### Linked Libraries
`d3d11.lib`, `d2d1.lib`, `dxgi.lib`, `d3dcompiler.lib`, `dxguid.lib`, `windowscodecs.lib`, `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`, `mscms.lib`.

### CI
`.github/workflows/ci.yml` builds Debug+Release x64 and runs `ShaderLabTests.exe --adapter warp`. Tests include graph DAG / topo sort / cycle detection, JSON round-trip, all ShaderLab effects compile-and-evaluate (analysis + source + tone-mapping), property bindings propagation, Numeric Expression input/output round-trip, Clock node, and three-node chain integration.

---

## Project Structure

```
ShaderLab\
├── ShaderLab.slnx                  # Solution
├── ShaderLab.vcxproj               # WinUI 3 packaged app (MSIX)
├── ShaderLabEngine.vcxproj         # Engine DLL (shared by app + tests)
├── ShaderLabTests.vcxproj          # Console test runner
├── packages.config                 # NuGet manifest
├── Package.appxmanifest            # MSIX identity (plain CN=ShaderLab)
├── app.manifest                    # DPI awareness, heap type
├── EngineExport.h                  # SHADERLAB_API import/export macro
├── Version.h                       # App 1.4.0, graph format 2
├── README.md                       # Living architecture doc + decision log
├── CHANGELOG.md                    # Version history
├── NewEffectDefaults.md            # D2D effect default-property reference
│
├── pch.h / pch.cpp                 # App PCH
├── pch_engine.h / pch_engine.cpp   # Engine + Test PCH
├── App.xaml / .h / .cpp            # WinUI 3 entry point
├── MainWindow.xaml / .h / .cpp     # Main window (~290 KB)
├── MainWindow.McpRoutes.cpp        # MCP server routes (~96 KB)
├── EffectDesignerWindow.*          # Effect Designer modal window
│
├── Tests\
│   ├── TestRunner.cpp              # Standalone test entry point
│   ├── RunTests.ps1                # Local test driver
│   └── RunCliTests.ps1             # MCP CLI round-trip tests
│
├── Graph\                          # (engine) DAG data model
│   ├── NodeType.h / PropertyValue.h
│   ├── EffectNode.h / EffectEdge.h
│   └── EffectGraph.h / .cpp        # DAG, topo sort, JSON, bindings, versioning
│
├── Rendering\                      # (engine) eval + display + tone + math
│   ├── RenderEngine.h / .cpp       # (app-only) D3D11 + D2D1 + swap chain
│   ├── GraphEvaluator.h / .cpp     # Topological eval, dirty propagation, deferred D3D11 compute
│   ├── ToneMapper.h / .cpp         # 5 tone map modes (defaults to None)
│   ├── FalseColorOverlay.h / .cpp  # False color rendering overlay
│   ├── DisplayMonitor.h / .cpp     # HDR/SDR detection, primaries, OS SDR white, jthread
│   ├── DisplayProfile.h            # Profile structs, presets
│   ├── DisplayInfo.h               # DisplayCapabilities + monitor primaries
│   ├── PipelineFormat.h            # scRGB FP16 (always)
│   ├── IccProfileParser.h / .cpp   # mscms.dll-based ICC reader
│   ├── GpuReduction.h / .cpp       # 32×32 thread-group reduction (image stats)
│   ├── D3D11ComputeRunner.h / .cpp # Generic D3D11 compute dispatcher
│   ├── EffectGraphFile.h / .cpp    # .effectgraph zip (miniz) + embedded media
│   └── MathExpression.h / .cpp     # ExprTk evaluator (PCH disabled, math-only flags)
│
├── Effects\                        # (engine) effect catalog + custom effect base
│   ├── ShaderLabEffects.h / .cpp   # 30+ built-in effects + shared color math HLSL
│   ├── EffectRegistry.h / .cpp     # 40+ wrapped D2D effect catalog
│   ├── CustomPixelShaderEffect.*   # ID2D1DrawTransform implementation
│   ├── CustomComputeShaderEffect.* # ID2D1ComputeTransform implementation
│   ├── StatisticsEffect.*          # D3D11-compute D2D effect (ID2D1StatisticsEffect)
│   ├── ShaderCompiler.h / .cpp     # D3DCompile + D3DReflect wrapper
│   ├── ImageLoader.h / .cpp        # WIC HDR/SDR image loading
│   ├── VideoSourceProvider.h / .cpp # MF video decoding → D2D bitmaps
│   ├── SourceNodeFactory.h / .cpp  # Source node creation
│   └── PropertyMetadata.h          # Effect property metadata
│
├── Controls\                       # (app) editor controllers
│   ├── NodeGraphController.*       # D2D canvas node graph editor
│   ├── ShaderEditorController.*    # Live HLSL compile controller
│   ├── PixelInspectorController.*  # GPU readback pixel inspection
│   ├── PixelTraceController.*      # Recursive pixel trace through graph
│   ├── OutputWindow.*              # Per-Output-node OS window
│   ├── LogWindow.*                 # Log viewer
│   └── NodeLog.h                   # Per-node log entry types
│
├── ShaderLab\                      # (app) MCP server (no PCH)
│   └── McpHttpServer.h / .cpp      # Winsock2 + HTTP + chunked transport
│
├── third_party\
│   └── exprtk\                     # exprtk.hpp (downloaded, gitignored)
│
├── scripts\
│   ├── EnsureDevCert.ps1
│   ├── EnsureExprTk.ps1
│   ├── EnsureMiniz.ps1
│   └── Install.ps1                 # Per-arch unsigned-MSIX installer
│
├── .github\
│   ├── workflows\
│   │   ├── ci.yml                  # Build + tests on every push / PR
│   │   └── release.yml             # x64 + ARM64 matrix, OID injection
│   └── copilot-instructions.md     # AI agent rules
└── .context\
    └── resume.md                   # This file
```

---

## Active Development Focus

The primary thesis driving recent work is **better HDR↔SDR tone mapping in BT.2100 ICtCp space**:

- I (intensity) is decoupled from Ct/Cp (chromaticity), so manipulating I alone preserves hue and saturation by construction.
- The ICtCp suite (Round-Trip Validator, Tone Map, Inverse Tone Map, Saturation, Highlight Desaturation) is the design platform for this work.
- `ReinhardCompressI` / `ReinhardExpandI` use an anchored Möbius curve with input clamping so peaks map to peaks exactly and out-of-domain inputs saturate cleanly.
- Future variants planned in this subcategory: BT.2390, hue-preserving ACES, adaptive operators.

---

## Potential Future Work

- **More tone-mapping operators** in the ICtCp subcategory (BT.2390, hue-preserving ACES, adaptive).
- **Auto-bind affordances** so SDR-white / monitor-peak hidden defaults can be wired from any matching upstream output without manual binding.
- **Effect Designer export** — emit standalone C++ header / module files for D3D11 compute effects so teams can fork them into their own codebases.
- **External binary import** — load pre-compiled D2D effect DLLs (`ID2D1EffectImpl`) and `.cso` compute binaries directly into the graph.
- **Multi-dispatch GPU reduction pyramid** for images > ~33 MP (current `GpuReduction` dispatches a single 1024-thread group).
- **Hide `Prim*` data pins from OOG-style nodes** — host-managed hidden properties should never surface as connectable orange diamonds.
