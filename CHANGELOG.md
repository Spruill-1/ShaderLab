# Changelog

All notable changes to ShaderLab will be documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [1.4.1] - 2026-05-05

### Added
- **`OutputMode` parameter on the `Delta E Comparator` effect** (effectVersion 2 → 3). Two modes: `0 = Heatmap` (default — Turbo colormap, prior behavior) and `1 = Grayscale dE`. Grayscale mode writes `saturate(dE / MaxDeltaE)` to all three RGB channels, which makes a downstream `Luminance Statistics` node read true mean/p95/max color-difference values directly off the GPU — no CPU pixel readback required. Enables live in-graph dE telemetry while sweeping a tone-mapper's parameters: bind / drive any input, watch the mean dE field update in the Properties panel each frame.
  - Read all uniforms unconditionally before the mode branch (mirrors gotcha #2 in CLAUDE.md so D3DCompile can't strip `OutputMode` if the agent only ever uses heatmap mode).

### Changed
- **`README.md` decision log #52** updated with a cleaner empirical result on `Colors of Journey_1002.mp4`. Earlier entry said `ToneLift ≈ 0.6` matches D2D within ~10 %; that was matching the dark/mid *luminance levels* visually. A proper CIEDE2000 fidelity-to-source measurement (using the new Grayscale dE + LumStats live-readout pipeline) shows the *color-accuracy* optimum is `ToneLift ≈ 0.30`, with mean dE 9–32 % lower than D2D HDR Tone Map across three frames spanning 0.5 → 124 nit p95 source brightness. The default of `0.0` (pure peak compression, neutral) is retained.

## [1.4.0] - 2026-05-05

### Removed
- **Per-effect "follow the live monitor / working space" plumbing.** Removed all 5 host-side per-frame writer blocks from `MainWindow.xaml.cpp` (~150 LOC) and stripped every `_hidden` cbuffer field from 12 ShaderLab effects. The `Working Space` parameter node + property bindings (decision #51) are now the only path for tracking the active display profile from a graph.
  - **Effects converted** (12, all schema-broken — bumped `effectVersion`): Gamut Highlight, Luminance Highlight, CIE Chromaticity Plot, Gamut Source, Gamut Coverage, Gamut Map, ICtCp Gamut Map, ICtCp Boundary, ICtCp Tone Map (HDR → SDR), ICtCp Inverse Tone Map (SDR → HDR), ICtCp Highlight Desaturation, Luminance Statistics. Plus a light cleanup on the `Gamut Parameter` data node (dropped its `Working Space` value-enum entry).
  - **Replacement pattern**: each affected effect's gamut/range enum gains a `Custom` entry as the last index (existing static modes — `sRGB`, `DCI-P3`, `BT.2020`, etc. — are kept for strict-mode analysis without wiring 3 binds), and the host-managed primaries are promoted to first-class bindable `Float2` parameters (`RedPrimary`, `GreenPrimary`, `BluePrimary`, plus `WhitePoint` where applicable) gated by `visibleWhen "TargetGamut == 3"`. CIE Chromaticity Plot is the exception — its monitor primaries were always implicit, so the new `Float2` params are unconditional. Effects that consumed `(Ws)SdrWhiteNits_hidden` (ICtCp Tone Map / Inverse Tone Map / Highlight Desaturation / Luminance Statistics) drop the hidden field entirely; their existing numeric peak-nit parameter (`TargetPeakNits` / `SourcePeakNits` / `ClipNits`) becomes the single source.
  - **Old enum modes dropped**: every `Current Monitor` / `Working Space` entry from `TargetGamut`, `SourceGamut`, and `TargetRange`; entire `SdrWhiteSource` and `ClipSource` switch enums.
  - **Saved-graph breakage** (accepted, no other users yet): old graphs loaded from `.effectgraph` files keep stale `_hidden` properties in their `node.properties` map, but the shader cbuffer no longer references them and they're filtered out of the Properties panel + pin list (decision #35 retained for exactly this reason). Old `Working Space` enum-mode selections clamp to an invalid index → user re-picks. No graph format version bump.

### Added
- **HDR / gamut / tone-mapping MCP tool suite (10 new, 27 → 37 total).** Round 2 of the agent-driven HDR workflow: simulate any display profile, capture any node's output, drive the preview view, run GPU stats, sample pixel grids, and inspect HLSL — all without mutating UI state.
  - `list_display_profiles` — GET `/display/profiles`. Returns 7 built-in presets (sRGB SDR, sRGB 270, P3 600, P3 1000, BT.2020 1000, BT.2020 4000, AdobeRGB) + the active live/simulated profile + `isSimulated` flag. Full caps in JSON: hdr/peak/sdrWhite/min/maxFullFrame nits, primaries (CIE xy), gamut id.
  - `set_display_profile` — POST `/display/profile`. Apply a simulated profile via exactly one of `{preset, presetIndex, iccPath, custom}`. `custom` accepts the full `{name, hdrEnabled, sdrWhiteNits, peakNits, minNits, maxFullFrameNits, primaryRed, primaryGreen, primaryBlue, whitePoint, gamut}` schema; missing fields fall back to sane defaults. Drives `MainWindow::ApplyDisplayProfile`, marks the graph dirty, forces the next render frame.
  - `clear_simulated_profile` — POST `/display/profile/clear`. Reverts to the live OS-reported profile.
  - `render_capture_node` — POST `/render/capture-node` body `{nodeId, inline?}`. Captures any node's resolved image as PNG without touching `m_previewNodeId`. Forces a render frame first so dirty nodes evaluate. With `inline=true` returns MCP-native image content (`type:"image"`, `mimeType:"image/png"`); without it, writes a unique `%TEMP%\shaderlab_node_<pid>_<seq>.png` and returns the path. Disambiguates not-found (404) vs not-ready (409 + `notReady:true, reason:"dirty"|"missingInputs"`).
  - `preview_get_view` / `preview_set_view` / `preview_fit_view` — symmetric with the graph view tools but for the preview pane. zoom is clamped to [0.01, 100.0] (matches the wheel-zoom range, much wider than the graph view's [0.1, 5.0]); pan is unclamped. `preview_set_view` returns post-clamp values.
  - `image_stats` — POST `/render/image-stats` body `{nodeId, channels?, nonzeroOnly?}`. Per-channel min/max/mean/median/p95/sum + nonzero counts, GPU-reduced on a fresh FP32 render of the target. Defaults to luminance + R + G + B + A; pass `channels:["luminance"]` to skip the others. Backed by a new `GraphEvaluator::ComputeStandaloneStats(dc, image, channels, nonzeroOnly)` so MCP doesn't have to own a separate reduction instance or mutate the graph for diagnostics.
  - `read_pixel_region` — POST `/render/pixel-region` body `{nodeId, x, y, w, h}`. Reads a small w×h region of FP32 RGBA pixels (scRGB linear-light) row-major as a flat float array. Capped at 32×32 (1024 pixels) and per-axis at 64; oversize requests fail 400 with the cap quoted so agents can chunk. Out-of-bounds rects clip to the image; empty after clip → 404.
  - `effect_get_hlsl` — GET `/effect/hlsl/{nodeId}`. Reads a custom-effect node's HLSL source, parameter list, compile state, last runtime error. For ShaderLab library effects also returns `isLibraryEffect:true` + `shaderLabEffectId`/`shaderLabEffectVersion` so the agent knows what's read-only. Non-custom nodes return `hasCustomEffect:false` (200, not 404) so the agent can probe any node.
- **Graph snapshot + view-control MCP tools** (4 new). AI agents can now request a PNG of the live node-graph editor view and steer its pan/zoom exactly the way a user would. Implementation lives in `MainWindow::CaptureGraphAsPng()`, `MainWindow::FitGraphView()`, and `NodeGraphController::ContentBounds()`. The shared `MainWindow::RenderGraphScene()` helper drives both the live render tick and the off-screen snapshot so the two never drift.
  - `graph_snapshot` — POST `/graph/snapshot`. Always writes a unique `%TEMP%\shaderlab_graph_snapshot_<pid>_<seq>.png`. With `inline=true`, the JSON-RPC response carries MCP-native `content[].type=image` + `mimeType=image/png` so the agent gets the bytes directly. Snapshot renders the current pan/zoom at the live swap-chain panel size — same dimensions the user sees.
  - `graph_get_view` — GET `/graph/view`. Returns `{zoom, panX, panY, viewportW, viewportH, contentBounds, zoomLimits}`.
  - `graph_set_view` — POST `/graph/view` body `{zoom?, panX?, panY?}` (any subset). Applies via `NodeGraphController::SetZoom`/`SetPanOffset`, which mark the canvas dirty so the next 16ms render tick shows the new view. zoom is clamped to [0.1, 5.0]; pan has no clamp. Coord convention: `screen = zoom * canvas + pan`.
  - `graph_fit_view` — POST `/graph/view/fit` body `{padding?}` (viewport DIPs, default 40). No-op on empty graph.
- **`NodeGraphController::ContentBounds()`** — returns the AABB of all node visuals in canvas (pre-pan/zoom) space. `{0,0,0,0}` when no nodes are laid out.
- **MCP activity indicator on the toolbar.** A single dot next to the MCP toggle pulses amber when the server has handled a request in the last few seconds and turns off when idle. Provides at-a-glance feedback that an external agent is actually connected and active. The amber colour was chosen because the original green was nearly invisible against the toggle's blue Checked background.
- **`Working Space` parameter node + display-profile mirroring.** New ShaderLab Parameter effect (no input pins, no output image pin) that mirrors the active display profile from the top-bar profile selector — live OS-reported caps or any simulated preset / ICC the user has applied — into 14 typed analysis output fields: `ActiveColorMode` (0=SDR, 1=WCG/ACM, 2=HDR), `HdrSupported`, `HdrUserEnabled`, `WcgSupported`, `WcgUserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus four CIE-xy primaries `RedPrimary` / `GreenPrimary` / `BluePrimary` / `WhitePoint` (each Float2). Bind any downstream property to these fields via the property-binding system — e.g. wire a tone-mapper's peak-nits to `working_space.PeakNits` and it tracks Display Settings or simulated profile changes automatically. Replaces ad-hoc per-effect "follow the working space" toggles. Updated in `MainWindow::UpdateWorkingSpaceNodes()` which runs on `ApplyDisplayProfile`, `RevertToLiveDisplay`, the display-change callback, and once per render tick (cheap node-list walk; only marks dirty when at least one field actually changed, so freshly-added nodes pick up live values immediately without hooking every AddNode site).
- **`DisplayCapabilities` extended with ACM / WCG state** (`activeColorMode`, `hdrSupported`, `hdrUserEnabled`, `wcgSupported`, `wcgUserEnabled`). Sourced from a new `DisplayMonitor::QueryAdvancedColorInfo2()` helper that calls `DisplayConfigGetDeviceInfo` with `DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2` (type 15, requires SDK 10.0.26100). Falls back to deriving `activeColorMode` from `caps.hdrEnabled` when the type-15 query is unavailable. Also trusts DisplayConfig's `bitsPerColorChannel` over the legacy DXGI heuristic, which reports 8 in many WCG configurations even when the actual scanout is 10-bit. All seven preset factories now stamp coherent ACM/WCG flags via the new `StampSimulatedColorMode()` helper so simulated profiles report a self-consistent display mode through the Working Space node.

### Changed
- **The "Regular parameter nodes" branch in `GraphEvaluator::Evaluate` now unpacks the full `PropertyValue` variant** (`bool` → 0/1 float, `int32_t` / `uint32_t` → float, `float2` / `float3` / `float4` → component[0..N]) into `AnalysisFieldValue::components`. Previously only `float` was unpacked, so any vector or boolean property on a parameter node was silently zero. Used by the Working Space node for primary chromaticities (Float2) and capability flags (Float-as-bool); also benefits any future host-managed parameter nodes that need richer types.
- **`Working Space Integration` README section rewritten** to describe the new "bind, don't hide" pattern for tracking the active display profile. Decision log gains entry #51; entry #35 (the `_hidden` suffix convention) cross-references #51 to record that the suffix filter is now legacy-compatibility only.
- **`ICtCp Tone Map (HDR → SDR)` gained a `ToneLift` parameter** (default `0.0` = identity, range `[0..1]`). Adds an anchored polynomial mid-bump on top of the existing I-channel Reinhard compression: `f(x) = x + a·x·(1−x)` evaluated in normalized `[0, peakOut]` I-space then scaled back. Designed to let the operator approximate D2D `HdrToneMap`'s "make HDR readable on SDR" dark-end lift without giving up the hue-preservation property of pure I-channel compression. Quantitatively benchmarked against D2D HDR Tone Map on the `Colors of Journey` HDR test clip: at `ToneLift ≈ 0.6`, dark-region nit counts match D2D within ~10%; at `ToneLift = 0` the effect is its prior pure-compression behavior. Effect bumped from version 9 → 10 (drops `ToneLift` initialized to default on existing graphs). Note: the docstring no longer claims "saturation is preserved by construction" — only hue is. Lifting I without rescaling Ct/Cp can desaturate slightly, especially at higher `ToneLift`.

### Fixed
- **`ICtCpToScRGB` could emit NaN/Inf when `pqLms` exceeded the PQ valid range `[0, 1]`.** The function transformed ICtCp → PQ-encoded LMS via the inverse matrix and then called `PQ_EOTF` on each component without clamping. For modified ICtCp inputs (e.g. tone-mapped I) or out-of-gamut chroma, the matrix product could push an LMS component above 1 (where `PQ_EOTF`'s denominator goes through zero and then negative around `V ≈ 1.16`) or below 0 (where `pow` of a negative is undefined). Added `pqLms = saturate(pqLms);` before the EOTF calls. In-domain values are unaffected; out-of-domain values now saturate to the nearest representable nit value rather than producing NaN that propagates into the inverse XYZ→scRGB matrix. Benefits all 8 ICtCp-using effects.
- **`DispatchSync` was UB on timeout.** The original wait-and-deref pattern closed the event after a 10-second timeout, then dereferenced `*result` without checking `WAIT_OBJECT_0` — and the in-flight lambda still held references to stack `result`/`ex`/`event` which dangled if the caller had returned. Replaced with a `shared_ptr<State>` capture so the lambda can safely complete after the caller times out, an explicit `WAIT_OBJECT_0` check that throws on timeout (caller's `catch` returns 500 cleanly), and a 30s timeout for the heavier compute/readback tools. Closed by the new round-2 routes that need it; old routes benefit too.
- **Snapshot capture no longer suppresses the live editor repaint.** `NodeGraphController::Render()` clears `m_needsRedraw` at the end. `CaptureGraphAsPng()` now snapshots the dirty flag before its render and re-sets it after, so an MCP-driven snapshot taken between frames doesn't cancel the next live render.

## [1.3.9] - 2026-05-05

### Fixed
- **D3D11 compute effects (Channel / Luminance / Chromaticity Statistics) never compiled or dispatched.** `DispatchUserD3D11Compute` started with `if (!node.customEffect->isCompiled()) return;` — but `isCompiled()` checks `compiledBytecode.empty()`, and for D3D11 compute the bytecode is only ever populated *inside* this function via the lazy `runner->CompileShader` path. Chicken-and-egg: every call early-returned, no bytecode was ever stored, the shader never ran, and analysis-output fields stayed empty forever. Removed the early-out for that case so the runner can compile on first dispatch and persist the bytecode back to `def.compiledBytecode` for the cbuffer reflection pass.
- **`D3D11ComputeRunner::CompileShader` discarded the compiled bytecode blob** after creating the `ID3D11ComputeShader` object, leaving `def.compiledBytecode` empty for D3D11 compute effects forever. Cached the blob in `m_bytecode` and exposed it via `GetCompiledBytecode()`; `DispatchUserD3D11Compute` now copies it onto `node.customEffect->compiledBytecode` after a successful compile so the cbuffer-reflection pass that packs user properties has something to reflect against.
- **D3D11 compute cbuffers received raw float bit patterns where shaders declared `uint`/`int`/`bool`.** The Properties panel stores enum-style fields (e.g. `Units`, `ClipSource`) as `float`, but the HLSL declares `cbuffer { uint Units; ... }`. Doing `memcpy(dest, &floatVal, 4)` writes `0x3F800000` (1,065,353,216) into the uint slot, so `if (Units == 1)` was never true and conditional code paths were dead. The pack code now reflects each variable's `D3D_SHADER_VARIABLE_TYPE` and converts via `static_cast<uint32_t>` / `static_cast<int32_t>` based on the declared HLSL type before writing.
- **Analysis-only compute nodes froze their output fields after the first frame.** `needsCompute` was only true on `node->dirty || fields.empty()`, and a Statistics node's own properties don't change frame-to-frame even as the upstream video does. Added a topological dirty-propagation pre-pass at the start of `Evaluate`: any node that's dirty marks all its direct downstream consumers dirty before the eval loop runs. Same pass is also done in `RenderFrame` after `TickAndUploadVideos`, so video-frame updates correctly flow through pixel-shader chains *and* analysis-only compute nodes in the same frame they're decoded. Both passes are idempotent — no-op when nothing is dirty.
- **Effect Designer "Output type" + analysis fields were blank when opening a built-in ShaderLab effect** (or any saved custom effect with typed analysis output). `LoadDefinition` populated inputs / parameters / thread-groups but never restored the `OutputTypeSelector` selection or the analysis-field rows, so opening *Channel Statistics* showed no record of its declared `Min` / `Max` / `Mean` / etc. Refactored the per-row construction in the Add-Field click handler into a reusable `AppendAnalysisFieldRow()` method, then taught `LoadDefinition` to map `def.analysisOutputType` → selector index and rebuild one row per `def.analysisFields` entry with name / type / array length restored.

## [1.3.8] - 2026-05-05

### Added
- **ICtCp tone-mapping suite (initial)**: three new effects under *Analysis → Tone Mapping*. `ICtCp Tone Map (HDR → SDR)` and `ICtCp Inverse Tone Map (SDR → HDR)` apply Reinhard compression / expansion to I in BT.2100 ICtCp space, leaving Ct/Cp untouched so hue and saturation are preserved by construction. `ICtCp Round-Trip Validator` is a diagnostic effect that outputs `|scRGB→ICtCp→scRGB - in| × Gain` — used to verify the conversion math is correct (renders black on a correct image). All three opt into `SdrWhiteNits_hidden` so future auto-bind affordances can drive Target/SourcePeakNits from the live OS SDR-white value.
- **Color-math HLSL helpers**: `NitsToI`, `IToNits`, `ReinhardCompressI`, `ReinhardExpandI` for I-channel tone mapping in PQ-encoded nit space. Live next to the existing `ScRGBToICtCp` / `ICtCpToScRGB` pair in `GetColorMathHLSL()`.
- **OS-reported SDR white level**: `DisplayMonitor` now queries `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)` so `sdrWhiteLevelNits` tracks the Windows *Settings → Display → HDR → "SDR content brightness"* slider in real time. Falls back to 80 nits when the call isn't available (older Windows builds, virtual outputs). Status bar now displays it next to the peak nit value (e.g. `Max Luminance: 1000 nits (SDR white 240)`). Simulated `DisplayProfile` presets continue to override the live value through the existing path.
- **`SdrWhiteNits_hidden` / `WsSdrWhiteNits_hidden`** hidden-default keys for effects that need scRGB-1.0-in-nits. Injected per frame from `RenderFrame` with the same change-detected pattern as `MonMaxNits_hidden` and `WsRedX_hidden`.
- **Add Node flyout sub-grouping**: `ShaderLabEffectDescriptor` gained a `subcategory` field. The Analysis category — which had grown to 16+ entries — is now folded into **Comparison**, **Gamut Mapping**, **Highlights**, **Scopes**, **Statistics**, and **Tone Mapping** sub-trees. Effects without a subcategory remain at the top level under their category.

### Changed
- **`Perceptual Gamut Map` renamed to `ICtCp Gamut Map`** (effectId + display name) for consistency with the existing `ICtCp Boundary` effect and the new ICtCp tone-mapping suite. Saved graphs that reference the old ID resolve through a legacy alias in `FindById`, so existing `.effectgraph` files still load — they'll surface the standard "Update Effect" prompt on next save. Effect version bumped 7 → 8.

### Fixed
- **MCP server now accepts `Transfer-Encoding: chunked` requests.** Visual Studio's MCP client uses chunked HTTP rather than `Content-Length`; our raw-Winsock reader was passing the chunk framing (`<hex-size>\r\n<payload>\r\n0\r\n\r\n`) straight to `JsonObject::Parse`, which threw `0x83750007 "Invalid JSON string"` on every connect. The reader now detects chunked encoding, waits for the terminating zero-length chunk, and decodes the framing into the actual JSON payload. Content-Length requests are unchanged. Same path also adds: a non-throwing `JsonObject::TryParse` wrapper that returns a JSON-RPC `-32700 Parse error` response instead of bubbling a `winrt::hresult_error`; correct `202 Accepted` (not `200 OK`) for `notifications/*` per the MCP Streamable HTTP spec; a small `GET /` health-check route so clients that probe before posting don't get a 404; and proper status-text for 202/204/400/500 in the HTTP response builder.
- **All five ICtCp pixel shaders were sampling the same edge texel for every output pixel.** They were calling `Source.Sample(Sampler, uv)` with `uv` from `TEXCOORD`, but D2D `TEXCOORD` is in pixel/scene space, not normalized [0,1]. Every output pixel was reading from texel ~256 with edge clamping, so the entire image came out as one constant color regardless of input. Switched to `Source.Load(int3(uv, 0))` to match the Gamut Highlight / scope pattern that was already correct. Affects: `ICtCp Round-Trip Validator`, `ICtCp Tone Map (HDR → SDR)`, `ICtCp Inverse Tone Map (SDR → HDR)`, `ICtCp Saturation`, `ICtCp Highlight Desaturation`. Effect versions bumped to invalidate cached bytecode.
- **`ReinhardCompressI` / `ReinhardExpandI` were unanchored.** The original form `peakOut * t / (1 + t*(1 - ratio))` had `f(peakIn) = peakOut / (2 - ratio)` instead of `peakOut`, so peaks did not map to peaks: HDR→SDR compression undershot the SDR target, SDR→HDR expansion overshot the HDR target by hundreds of percent (a 1000-nit SDR-white input produced ~14,943 nits at TargetPeakNits=1000). Replaced with the anchored Möbius `f(I) = I·peakIn·peakOut / (peakIn·peakOut + I·(peakIn − peakOut))` and its analytic inverse — same shape near zero (slope 1, "Reinhard" feel in the dark end), but `f(0) = 0`, `f(peakIn) = peakOut` exactly, with a smooth shoulder. Round-trip error stays at FP16 noise (~7e-10 per channel at Gain=100k).
- **`ReinhardExpandI` had its peakIn/peakOut convention inverted in the call site.** The doc-comment said `peakIn = SDR, peakOut = HDR`, but the algebra is the literal inverse of `ReinhardCompressI(peakIn=HDR, peakOut=SDR)`, so the call inside the Inverse Tone Map shader was running the curve in the wrong direction (input got dimmer with higher Strength). Both the doc-comment and the call site now agree: peakIn is the *uncompressed* (HDR) range, peakOut is the *compressed* (SDR) range, for both Compress and Expand.
- **Over-range inputs to the ICtCp tone-mapping helpers walked toward the Möbius asymptote.** A 10,000-nit input through an Inverse Tone Map configured for SourcePeakNits=100 produced 1.4 million nits because the helper extrapolated past its anchor onto the rising branch beyond peakIn. Both `ReinhardCompressI` and `ReinhardExpandI` now clamp their input to the valid input range before mapping, so out-of-domain inputs saturate cleanly to the target peak instead of overshooting.

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
