# Changelog

All notable changes to ShaderLab will be documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [1.3.7] - 2026-05-05

### Changed
- Moved the version + effect-library version readout from the bottom-status-bar to the title bar. Frees the right side of the status bar so the FPS / frame-timing readout has room to breathe (it had been clipping against the version text on smaller windows).

### Fixed
- First selection of a node in a freshly-loaded graph or a freshly-added node would render the preview at the wrong zoom (whatever the previous viewport happened to be). Subsequent selections were correct because by then the cached output existed. Now click defers `FitPreviewToView` until the next eval has populated `cachedOutput`, so the very first fit measures real bounds.
- Newly-added Clock and other parameter nodes did not show their on-node UI (play/pause button, seek slider) until the user clicked them. The post-eval `RebuildLayout` only fired on user-forced renders; the add-node path now sets `m_forceRender = true`, triggering the same path so the controls appear on the first frame after add.

## [1.3.6] - 2026-05-05

### Fixed
- Save / load of `.effectgraph` archives with embedded media froze the UI for tens of seconds while miniz compressed/decompressed. The ProgressBar painted once at the initial 16 ms yield then sat unchanged because the work ran synchronously on the UI thread. The save/load now runs on a threadpool thread (miniz is pure native, no WinRT marshalling concerns) and the progress callback marshals each update back through `DispatcherQueue` so the bar animates smoothly.
- Video sources bound to a paused Clock free-ran in a ~1 s loop instead of holding a static frame. Cause: `TickAndUploadVideos` advanced the decoder via `provider->Tick(deltaSec)` whenever the bound `Time` value didn't differ from the current decode position by more than one frame, then the next iteration's "small forward gap" branch let it drift further. Fixed by remembering each provider's previous bound `Time` value — if it didn't change between ticks, hold the frame at exactly that position rather than ticking.

### Internal
- Resolved compiler warnings: `C4456` (shadowed `first` local in `WriteNodeJson`), `C4189` (unused `slashPos`, `imgDipX/Y`, `isPixelTraceTab`), `C4100` (unreferenced `sender` parameter on `OnPreviewPointerMoved`).

## [1.3.5] - 2026-05-05

### Fixed
- Right-hand properties panel jittered horizontally during video playback. Cause: every 250 ms while ANY graph node was dirty (always true while a video was playing) we rebuilt the entire properties panel, re-creating Slider + NumberBox pairs whose auto widths varied frame to frame. Now the rebuild only fires when the selected node has at least one `propertyBinding` whose live value can change between ticks.
- Visual Assets (app tile, splash, badges) were broken in shipped MSIX. The `Assets/` directory contained 21 corrupted `*.scale-200.scale-N.png` files (VS regenerated them by feeding existing scale-200 PNGs back as source — none of those names match any MRT lookup pattern), plus several 0-byte placeholder files. Cleaned them out and stripped the matching entries from `ShaderLab.vcxproj` / `.filters`. The remaining originals will resolve through MRT correctly; user-supplied bear artwork still needs to be dropped over the placeholder PNGs to actually appear.

## [1.2.7] - 2026-05-04

### Added
- **`--reap-now` command-line flag**: scans `%TEMP%` for orphan `ShaderLab-*` media folders left behind by a crashed instance, deletes any whose `.heartbeat` (or directory mtime) is older than the staleness threshold, prints a per-folder summary to stderr, and exits with the count of folders removed. Optional `--reap-stale-sec=<N>` overrides the default 150 s threshold for scripted regression tests.

### Fixed
- The previous v1.2.6 fix for the embed-media checkbox used `winrt::box_value(bool)`, which returns `IInspectable` not `IReference<bool>`, and tripped the `IReference<bool>` private-constructor diagnostic. Switched to `PropertyValue::CreateBoolean(...).as<IReference<bool>>()` which is the canonical conversion.

## [1.2.6] - 2026-05-04

### Fixed
- Save As crashed (`0xC000027B` — unhandled exception in coroutine `SaveGraphAsAsync`) when the user kept the *Embed referenced media* checkbox checked. The embed-media `ContentDialog` was initializing the checkbox with `IReference<bool>(m_embedMedia)`, whose `void*` constructor reinterprets a `bool` as a WinRT interface pointer rather than boxing it. Replaced with the canonical `winrt::box_value(m_embedMedia)`.

## [1.2.5] - 2026-05-04

### Added
- **Crash-safe temp media cleanup**: each extracted `.effectgraph` media folder gets a hidden `.heartbeat` sentinel that the running app touches every 60 s. On startup, ShaderLab scans `%TEMP%` for `ShaderLab-*` folders whose heartbeat (or directory mtime, for legacy folders) is older than 150 s and offers a *“Clean up old graph media”* `ContentDialog` (`Delete` / `Keep`). Live folders never go stale, so the prompt only appears when a previous instance crashed without running its `Closed` handler.

## [1.2.4] - 2026-05-04

### Added
- **Embedded media in `.effectgraph`**: source nodes that reference an external file (image, video, ICC profile) can have those files bundled inside the saved zip under `media/`. The Save As flow shows a follow-up `ContentDialog` with an *“Embed referenced media”* checkbox (default on) whenever the live graph has at least one external file path. JSON inside the archive uses canonical `media://<name>` tokens; on load the file is extracted to a unique GUID-named subdirectory under `%TEMP%` and the live graph is rewritten to point at the extracted path. Stale extraction directories are deleted at app shutdown.
- **Save / load progress dialog**: a `ContentDialog` with a `ProgressBar` and per-file status line tracks each entry as it is read or written. The actual zip work runs on a background thread so the UI keeps refreshing during large media archives.

### Changed
- `EffectGraphFile::Save` / `Load` API now take an optional media list and progress callback; the load result is a struct that includes the extraction directory and `media://` token map.
- `Ctrl+S` now goes through the same progress / embed flow rather than the old silent overwrite -- the user sees the same UI whether they overwrite or use Save As.

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
