# Changelog

All notable changes to ShaderLab will be documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [1.2.3] - 2026-05-03

### Added
- **`.effectgraph` file format**: native graph container. Standard ZIP archive (no compression, no third-party libraries — Win32 `CreateFile` + a small in-house store-only writer in `Rendering/EffectGraphFile.cpp`) holding `graph.json` plus a reserved empty `media/` folder for future media embedding (so a shared graph will eventually open with no broken file paths).
- **File type association**: `.effectgraph` is registered in `Package.appxmanifest`. Double-clicking a file in Explorer launches ShaderLab with the graph already loaded. `App::OnLaunched` parses the command line for the file path and hands it to `MainWindow::SetPendingOpenPath` to load after rendering is initialized.
- **Save / Save As keyboard accelerators**: `Ctrl+S` saves over the last picked path (or prompts on first save). `Ctrl+Shift+S` always shows the picker. `Ctrl+O` opens. The Save Graph button advertises both shortcuts in its tooltip.
- **Unsaved-changes guard**: `MainWindow` tracks `m_unsavedChanges` and shows a `Save / Discard / Cancel` `ContentDialog` when the window is closed via `AppWindow.Closing`. Title bar shows the file name with a `*` suffix while edits are unsaved.

### Changed
- `MainWindow::SaveGraphAsync` now overwrites the previously-picked path silently; previously every save reopened the picker.
- App version string in the toolbar now derives from `Version.h` instead of a hard-coded `v1.1.0`.

### Removed
- Legacy `.json` graph save / load support. `.effectgraph` is the only graph container; existing `.json` files from pre-1.2.3 builds will no longer open. There were no released users at the time of the cutover.

### Fixed
- Toolbar version text was stuck on `v1.1.0` regardless of the actual app version.

## [1.2.2] - 2026-05-02

### Added
- **Random parameter node**: takes a single `Seed` float input and outputs a deterministic, well-mixed `Result` in `[0, 1)`. The output is a pure function of the seed (SplitMix64-style integer mixer on the float's bit pattern), so identical seeds reproduce identical values and any change to the seed (e.g. a tick from an upstream Clock or Numeric Expression) yields a fresh random number. Implemented in `Rendering/GraphEvaluator.cpp`; registered in `Effects/ShaderLabEffects.cpp` (`effectId = "Random"`).

## [1.2.1] - 2026-05-01

### Added
- ICC profile reading now goes through `mscms.dll` (`OpenColorProfileW` + `GetColorProfileElement`) instead of an in-house binary parser. Public `IccProfileParser::LoadFromFile` API and `IccProfileData` struct are unchanged. Engine link list gains `mscms.lib`.
- Render `DispatcherQueueTimer` now ticks at the active monitor's refresh rate (`EnumDisplaySettings(ENUM_CURRENT_SETTINGS).dmDisplayFrequency`), clamped to [60, 240] Hz, refreshed on every display change. 120 / 144 / 165 / 240 Hz panels and high-FPS video sources run at native cadence. Interval is set in microseconds so non-integer-ms periods (e.g. 144 Hz ≈ 6.944 ms) stay accurate.

### Fixed
- Per-node connection log entries used Unicode arrows (`←` / `→`) that rendered as garbled glyphs in the LogWindow. Replaced with plain ASCII `from` / `to` wording in both `MainWindow::OnEdgeAdded` and the `/graph/connect` MCP route.

## [1.2.0] - 2026-04-30

### Added
- **Numeric Expression node**: a single ExprTk-powered math node replaces the legacy Add / Subtract / Multiply / Divide / Min / Max nodes.
  - Dynamic input list (`A..Z`, 26-input cap) editable from the Properties panel (`➕ Add Input`, per-row `✕`).
  - Expression rendered under the node title on the canvas as `= <formula>`.
  - Single `Result` analysis output bindable to any downstream scalar property.
  - Expression and input list round-trip through graph JSON.
- **Alt+click edge delete**: Alt + Left-click on any image or data-binding edge removes it (bezier hit-test in `NodeGraphController::HitTestEdge`).
- **ARM64 release builds**: GitHub Actions release workflow now produces both `ShaderLab-<version>-x64.zip` and `ShaderLab-<version>-arm64.zip`.
- **Smarter `Install.ps1`**: detects host architecture, installs bundled dependency MSIXes (VCLibs, Windows App Runtime) before the ShaderLab MSIX, and is robust to Microsoft.* dependency packages sharing the folder.
- Pre-build script `scripts\EnsureExprTk.ps1` that downloads `exprtk.hpp` (MIT) into `third_party\exprtk\` on first build.

### Changed
- New nodes (toolbar / context menu / MCP `graph_add_node`) now spawn at the **center of the current viewport** in graph coordinates, accounting for pan / zoom.
- Closing an OutputWindow forces a single render pass so the deleted Output node disappears from the canvas immediately.
- `Package.appxmanifest` `Publisher` stays plain `CN=ShaderLab` in the repo; the release workflow injects the unsigned-namespace OID just before MSBuild so signed F5 deploys keep working.
- README and decision log updated through entry #48.
- MCP tool count is now 23 (added `node_logs`, `perf_timings`).

### Fixed
- 0xC0000005 in MSVC Release builds when first evaluating a Numeric Expression — fixed by disabling ExprTk's regex / IO / enhanced subsystems via `exprtk_disable_*` macros before `#include`-ing `exprtk.hpp` (with PCH disabled on `Rendering/MathExpression.cpp`).
- Signed F5 deploy was rejected after we tried to keep the unsigned-namespace OID in `Package.appxmanifest` — separating the OID into a release-only injection step restores both flows.

## [1.1.0] - 2026-04-23

### Added
- **ShaderLab Effects Library**: 9 built-in effects with embedded HLSL and shared color math
  - Analysis: Luminance Heatmap, Out-of-Gamut Highlight, CIE Chromaticity Plot, Vectorscope
  - Source: Gamut Source, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern
- **Effect Designer**: Dedicated window for creating custom pixel/compute shader effects
  - Parameter types: float, float2, float3, float4, int, uint, bool, enum
  - Enum parameters with named dropdown labels
  - Bool parameters with ToggleSwitch control
  - Analysis output field declaration for compute shaders
  - HLSL auto-formatting and scaffold generation
- **MCP Server**: 21-tool JSON-RPC 2.0 server for AI agent integration (port 47808)
  - Graph manipulation, property control, HLSL compilation, render capture
  - Analysis output reading, pixel trace, effect listing, display info
- **Property Binding System**: Per-component Grasshopper-style data flow between analysis outputs and effect properties
  - Visual data pins (orange diamonds) on node graph
  - Type-safe bindings with component picking (float4.x → float)
- **Versioning System**: App version 1.1.0, graph format version 2
  - Forward compatibility check on graph load
  - Version display in status bar and title bar
- **Monitor Gamut Detection**: Live primaries from DXGI_OUTPUT_DESC1
  - Updates on monitor change
  - Feeds Out-of-Gamut Highlight "Current Monitor" mode
- **GPU Info**: Status bar shows hardware adapter name or "Software (WARP)"
- **Display Profile Mocking**: Override live display with presets or ICC profiles
- **Pixel Trace**: Recursive per-node pixel readback through effect graph
- Dark theme, Segoe Fluent Icons toolbar, dot grid canvas, refined node colors

### Changed
- Pipeline always uses scRGB FP16 (no sRGB fallback on SDR displays)
- Tone mapper defaults to None (inactive) — DWM/ACM handles display conversion
- Render loop is dirty-gated (skips evaluate when no nodes changed)
- Node graph canvas is dirty-gated (skips redraw when nothing changed)
- Analysis readback only runs on dirty frames

### Fixed
- Compile-on-connected-node crash (D2D access violation from sentinel input rects)
- Out-of-Gamut Highlight initial state showing no highlighting
- Monitor primaries being zeroed by HLSL optimizer
- Custom effects duplication menu and name uniqueness enforcement

## [1.0.0] - 2026-04-20

### Added
- Initial release
- Node-based DAG graph editor for D2D effect composition
- 40+ built-in D2D effects across 9 categories
- Custom pixel shader effects (ID2D1DrawTransform)
- Custom compute shader effects (ID2D1ComputeTransform)
- Live HLSL hot-reload with D3DCompile
- WIC HDR/SDR image loading
- Pixel inspector with scRGB → sRGB/PQ/luminance conversion
- Tone mapping (5 modes)
- Display change monitoring (WM_DISPLAYCHANGE + DXGI adapter events)
- Graph JSON serialization
