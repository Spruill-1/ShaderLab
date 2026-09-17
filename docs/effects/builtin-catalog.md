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
| CIE Histogram | CS-Img | 2D histogram of pixel chromaticity on the CIE xy plane. |
| CIE Chromaticity Plot | PS | Plots image pixels on a CIE 1931 xy diagram with gamut triangle overlays. |

## Analysis → Statistics (data-only)

CPU-readable reductions over the input image. Bind their analysis fields to downstream properties via the property-binding system.

| Effect | Type | Description |
|--------|------|-------------|
| Channel Statistics | CS-Data | Per-channel R / G / B / A min / max / mean / median / P95 / nonzero%. |
| Luminance Statistics | CS-Data | BT.709 Y stats with HDR-aware extras (log-spaced histogram, AvgLog, ClippedFraction). |
| Chromaticity Statistics | CS-Data | ICtCp Ct/Cp stats (mean / max chroma, mean hue). |
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
| HDR Screenshot Tonemap (8bpc) | PS | The display-referred screenshot path fused into one pass: knee tone map → chroma correction → soft gamut compression into sRGB → white-level normalise → sRGB OETF → TPDF dither → 8-bit quantize, then **decode back to linear scRGB** so downstream analysis measures exactly the damage the 8-bit handback would do. `KneeRatio` is a fraction of the SDR white level `W`, not absolute nits, so it keeps its meaning when the OS brightness slider moves. Fused rather than chained because the chained equivalent pays three ICtCp round trips over a 4K frame where this pays one. Known trade: the knee darkens SDR white by 15–20% whenever it engages. |

## Analysis → Gamut

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Coverage | CS-Img | Percentage of target gamut volume covered by input (image overlay + analysis field). |
| ICtCp Boundary | PS | ICtCp gamut boundary visualization for a chosen target gamut. |

## Color Processing

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Map | PS | CIE xy gamut mapping: Clip / Nearest / Compress to White / Fit Gamut. |
| ICtCp Gamut Map | PS | Perceptual gamut mapping in BT.2100 ICtCp space: Nearest on Shell / Compress to Neutral / Fit to Shell. (Renamed from `Perceptual Gamut Map` in v1.3.8 — old graphs load via legacy alias.) |
| Scale | CS-Img | Resample the input to a target width × height with selectable filter (point / bilinear / box). Used to throttle preview resolution for heavy chains without losing analysis-branch fidelity. |

## Source / Generator

| Effect | Type | Description |
|--------|------|-------------|
| Gamut Source | PS | Swept gamut fill for a target color space. |
| Color Checker | PS | Macbeth ColorChecker pattern with accurate sRGB patches. |
| Zone Plate | PS | Sine-wave zone plate for resolution / aliasing testing. |
| Gradient Generator | PS | Configurable linear / radial gradient with HDR range. |
| HDR Test Pattern | PS | Luminance step wedge from 0 to 10,000 nits. |
| Image Source | Host (WIC) | Static image file (PNG / JPEG / JXR / EXR / HDR). HDR formats decode as FP16 BT.709 scRGB. |
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
