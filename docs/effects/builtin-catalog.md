# ShaderLab Built-in Effects

ShaderLab ships with **36 built-in ShaderLab effects** implemented in `Effects/ShaderLabEffects.h/.cpp`, on top of the **40+ wrapped built-in D2D effects** in `Effects/EffectRegistry.cpp`. Each ShaderLab effect has its HLSL embedded as a string constant, compiled at first use via `ShaderCompiler` (and cached on disk under `%LOCALAPPDATA%\ShaderLab\bytecode\` so subsequent sessions reuse the bytecode), and shares a common color math library (BT.709 / BT.2020 / DCI-P3 matrices, PQ / HLG transfer functions, CIE xy conversions, ICtCp). Every effect is versioned with `effectId` and `effectVersion` so saved graphs can be upgraded in place — see [Effect Versioning System](effect-versioning.md).

The **Type** column below uses these abbreviations:

- **PS** = Pixel shader (`CustomPixelShaderEffect`, D2D `ID2D1DrawTransform`)
- **CS-Img** = D3D11 compute shader producing an FP16 image output (`CustomComputeBridgeEffect` wrapper, `hasImageOutput = true`)
- **CS-Data** = D3D11 compute shader producing only analysis fields (no image output, `dataOnly = true`)
- **Host** = Implemented host-side; no HLSL (image sources, parameter nodes)

## Analysis → Heatmaps

False-color overlays on the input image.

| Effect | Type | Description |
|--------|------|-------------|
| Luminance Heatmap | CS-Img | False-color BT.709 luminance overlay (Turbo / Inferno gradients). |
| Luminance Highlight | CS-Img | Highlights luminance bands above/below configurable thresholds. |
| Delta E Comparator | CS-Img | Two-input perceptual difference map (Heatmap or Grayscale dE). `Method` selects CIE76 / CIE94 / CIEDE2000 / **ΔE ITP (BT.2124)** — ITP is the default and the right choice for anything HDR or wide-gamut; the three Lab metrics were fit to reflective samples under SDR viewing and leave their domain above ~100 nits. Measured on a 1100 vs 1000 nit step: ITP 7.47, CIEDE2000 118.09, CIE76 253.26. One unit ≈ 1 JND in all four, so the numbers stay comparable when switching. |
| Gamut Highlight | PS | Highlights pixels outside a target gamut (sRGB / P3 / BT.2020 / current monitor). |
| Nit Map | PS | Display-referred nit visualization with configurable luminance bands. |

## Analysis → Scopes

Plot the input image's chromaticity or luminance distribution.

| Effect | Type | Description |
|--------|------|-------------|
| CIE Histogram | CS-Img | 2D histogram of pixel chromaticity on the CIE xy plane. **Multi-group (2026-09-25):** runs as 64 thread groups with per-group partials folded by the last group to finish (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| CIE Chromaticity Plot | PS | Plots image pixels on a CIE 1931 xy diagram with gamut triangle overlays. |

## Analysis → Statistics (data-only)

CPU-readable reductions over the input image. Bind their analysis fields to downstream properties via the property-binding system.

| Effect | Type | Description |
|--------|------|-------------|
| Channel Statistics | CS-Data | Per-channel R / G / B / A min / max / mean / median / P95 / nonzero%. **Multi-group (2026-09-25):** runs as 64 thread groups with per-group partials folded by the last group to finish (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| Luminance Statistics | CS-Data | BT.709 Y stats with HDR-aware extras (log-spaced histogram, AvgLog, ClippedFraction). **Multi-group (2026-09-25):** runs as 64 thread groups with per-group partials folded by the last group to finish (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| Chromaticity Statistics | CS-Data | ICtCp Ct/Cp stats (mean / max chroma, mean hue). **Multi-group (2026-09-25):** runs as 64 thread groups with per-group partials folded by the last group to finish (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| Image Info | CS-Data | Image metadata (width, height, format-derived nit assumptions). |

## Analysis → Tone Mapping (ICtCp suite)

HDR ↔ SDR operators built around BT.2100 ICtCp. The key property: I (intensity) is decoupled from Ct/Cp (chromaticity), so compressing or expanding I alone preserves hue and saturation by construction. This is the design thesis of the suite: operations that need to be carefully done in linear RGB or CIE xyY (per-channel hue shifts, gamut excursions, chromaticity drift) reduce to one-line manipulations of I in ICtCp. Each operator exposes its nit-targets as numeric parameters — wire them from `working_space.SdrWhiteNits` / `PeakNits` to track the OS slider or simulated profile automatically.

| Effect | Type | Description |
|--------|------|-------------|
| ICtCp Round-Trip Validator | PS | Diagnostic: outputs `\|scRGB→ICtCp→scRGB - in\| × Gain`. Should render black on a correct image; non-zero output indicates a bug in the ICtCp conversion. |
| ICtCp Tone Map (HDR → SDR) | CS-Img | I-channel Reinhard compression with `ToneLift` polynomial bump. Source / target peaks specified in nits; Ct/Cp pass through unchanged. |
| ICtCp Inverse Tone Map (SDR → HDR) | CS-Img | Mirror of the above; inverse Reinhard expands SDR-anchored content into the HDR peak. |
| ICtCp Saturation | PS | Per-pixel saturation gain in ICtCp (scales Ct/Cp around the I-axis). |
| ICtCp Highlight Desaturation | CS-Img | Smoothly reduces saturation above a configurable I threshold (counteracts tone-map hue shifts at clipping). |
| ICtCp Gamut Boundary LUT | CS-Img | **A generator** (no image input) that bakes `B(I, hue)` — the gamut boundary radius `CubeBoundaryRadius` finds — into a 256×128 FP32 table for a consumer's lookup input (read with `GamutLutBoundaryRadius`). It exists because on real wide-gamut HDR content (a PQ/BT.2020 capture) a gamut-compressing tone mapper's boundary searches were **81% of its cost**, and `B` depends only on `(I, hue, peak, target gamut)` — nothing in the image — so it can be built once and reused every frame until the display's SDR white moves. The generator compiles the **same** HLSL a consumer would otherwise run (`GamutBoundaryHLSL()`, one copy), so a table can only ever be a sampled copy of the exact answer. **Give it the same `SdrWhiteNits` and `TargetGamut` as its consumer** (bind both to the same Working Space node): every texel is stamped with the peak and a fingerprint of the target gamut it was built for, so a consumer can *refuse* a table built for another peak or gamut and fall back to the exact search — slower, never wrong. One table is one target: an sRGB and a P3 consumer each want their own LUT node (a rebuild is ~0.04 ms of GPU, so the graph is the cache). Rows are packed toward I(W) (`GAMUT_LUT_WARP_K` = 2), where the boundary collapses and the knee lands every bright pixel: measured against the exact search, real HDR content (bird capture) mean 0.0034 / max 2.2 ΔE ITP; a saturated BT.2020 gradient -- the worst case, nearly every pixel in that band -- mean 0.18 / max 3.3 (uniform rows: 0.42 / 6.1). Output size is fixed (hidden `OutputWidth/Height`) because the reader indexes it with the same constants. |

## Analysis → Gamut

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Coverage | CS-Img | Percentage of target gamut volume covered by input (image overlay + analysis field). **Multi-group (2026-09-25):** runs as 64 thread groups that add their bins into global counters, the last group to finish rendering the diagram (`SHADERLAB_REDUCE_SCRATCH`), not one group walking the frame on a single CU. |
| ICtCp Boundary | PS | ICtCp gamut boundary visualization for a chosen target gamut. **v5:** the six boundary polygons are built on the CPU once per parameter change (derived constants) instead of per pixel: 6.1 -> 2.8 ms at 1 Mpx. |

## Color Processing

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Map | PS | CIE xy gamut mapping: Clip / Nearest / Compress to White / Fit Gamut. Clip zeroes *negative* target-space components (a chromaticity clip; HDR values above 1 are kept). v6: Clip honours a Custom target -- through v5 it silently clipped Custom to BT.2020. |
| ICtCp Gamut Map | PS | Perceptual gamut mapping in BT.2100 ICtCp space: Nearest on Shell / Compress to Neutral / Fit to Shell. (Renamed from `Perceptual Gamut Map` in v1.3.8 — old graphs load via legacy alias.) **v13:** the target boundary (64 levels of I) and Fit to Shell's scale (256 levels) are CPU-built tables, interpolated in I, instead of being rebuilt per pixel -- Fit to Shell 1,924 -> 2.0 ms at 1 Mpx. Max 0.11 dE ITP from the exact per-pixel result. |
| Scale | CS-Img | Resample the input to a target width × height with selectable filter (bilinear / Catmull-Rom / Mitchell-Netravali / Lanczos-3 / box / Gaussian). Used to throttle preview resolution for heavy chains without losing analysis-branch fidelity. **v2:** separable -- each tile filters the source rows it needs horizontally, then vertically -- so a Lanczos-3 4x downscale takes 1.9 ms instead of 94.6. Differs from v1 by float rounding (<= 7e-6). |

## Source / Generator

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Source | PS | Swept gamut fill for a target color space. |
| Color Checker | PS | Macbeth ColorChecker pattern with accurate sRGB patches. |
| Zone Plate | PS | Sine-wave zone plate for resolution / aliasing testing. |
| Gradient Generator | PS | Configurable linear / radial gradient with HDR range. |
| HDR Test Pattern | PS | Luminance step wedge from 0 to 10,000 nits. |
| Image Source | Host (WIC) | Static image file. **Whatever WIC can decode on the machine** — the file-picker filter is built from `ImageLoader::SupportedExtensions()`, which enumerates the registered WIC decoders, so HEIF/AVIF/JPEG XL and camera RAW appear when their codecs are installed (65 extensions on a stock Windows 11 box with the HEIF extension). Decoding always produced FP16 BT.709 scRGB; only the picker was gated. A file WIC cannot decode now sets `runtimeError` instead of rendering nothing. |
| Video Source | Host (Media Foundation) | Decodes a video file frame-by-frame; advances under the animation timeline / Clock node. |
| DXGI Desktop Duplication | Host (DXGI) | Live capture of an entire monitor via `IDXGIOutputDuplication`. Submenu lists each adapter / output. Outputs raw FP16 scRGB so SDR monitors land at scRGB 1.0 ≈ 80 nits and HDR monitors preserve their full range. |
| Windows Graphics Capture | Host (Windows.Graphics.Capture) | Live capture of an arbitrary window or monitor via the standard WinUI graphics-capture picker. Same FP16 scRGB output convention as DXGI duplication. |

## Composition

| Effect | Type | Description |
|--------|------|-------------|
| Split Comparison | PS | Two-input wipe comparator with adjustable split & divider. |

## Data / Parameter Nodes

| Node | Description |
|------|-------------|
| Float Parameter | Continuous slider; teal node with inline canvas slider. |
| Integer Parameter | Discrete slider. |
| Toggle Parameter | Boolean on / off. |
| Gamut Parameter | Gamut-id selector (sRGB / DCI-P3 / BT.2020 / Custom). |
| Clock | Time source: outputs `Time` (seconds) and `Progress` (0–1 over a configurable duration). Drives the animation system. |
| Numeric Expression | Single configurable math node powered by ExprTk; user-supplied formula evaluated against dynamic float inputs `A..Z`. Replaces the older Add / Subtract / Multiply / Divide / Min / Max nodes. See [Numeric Expression Node](numeric-expression.md). |
| Random | Takes a single `Seed` float input and outputs a deterministic, well-mixed `Result` in `[0, 1)`. The output is a pure function of the seed (SplitMix64-style integer mixer on the float's bit pattern), so identical seeds always reproduce identical values and any change — e.g. a tick from an upstream Clock or Numeric Expression — yields a fresh random number. |
| Working Space | Strict sink (no input pins, no output image pin) that mirrors the active display profile from the top-bar profile selector — live OS-reported caps or whatever simulated preset / ICC the user has applied. Exposes 14 typed analysis output fields: `ActiveColorMode` (0=SDR, 1=WCG/ACM, 2=HDR), `HdrSupported`, `HdrUserEnabled`, `WcgSupported`, `WcgUserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus the four CIE-xy primaries `RedPrimary` / `GreenPrimary` / `BluePrimary` / `WhitePoint` (each Float2). Bind any downstream property to these fields via the property-binding system to drive an effect from the live working space — e.g. wire a tone-mapper's peak-nits to `working_space.PeakNits` and it will track Display Settings or simulated profile changes automatically without touching the graph. |


---

Back to [docs/](../README.md) • [Repo root](../../README.md)
