# Effect Designer

The Effect Designer is a modal window for authoring custom shader effects directly inside ShaderLab. It supports three shader types:

## Supported Types

| Type | Target | Execution | Output |
|------|--------|-----------|--------|
| **Pixel Shader** | `ps_5_0` | D2D render pipeline | Image (RGBA) |
| **D2D Compute Shader** | `cs_5_0` | D2D per-tile dispatch | Image or analysis data |
| **D3D11 Compute Shader** | `cs_5_0` | Host-side D3D11 dispatch | Analysis data only |

## Engine Headers

The engine serves three headers to every shader it compiles. None is required.

| Header | Contents |
|---|---|
| `shaderlab_colormath.hlsli` | The shared color-math library (`Effects/ColorMath.cpp`): transfer functions, gamut matrices, ICtCp, dE ITP, the target-gamut helpers. |
| `shaderlab_params.hlsli` | The parameter macros: `SHADERLAB_PARAM` / `SHADERLAB_GPU_BUFFER` / `SHADERLAB_LOAD_PARAM` for GPU-bindable parameters and `SHADERLAB_OPTION` / `SHADERLAB_OPTION_VALUE` for [option variants](option-variants.md). |
| `shaderlab_gamut.hlsli` | The target-gamut boundary search in ICtCp and the `ICtCp Gamut Boundary LUT` contract (`Effects/ShaderLabEffects.cpp`, `GetGamutHLSL()`). Includes `shaderlab_colormath.hlsli` itself. |

**Generate Scaffold** emits both includes at the top of every shader type:

```hlsl
#include "shaderlab_colormath.hlsli"   // shared color math (optional)
#include "shaderlab_params.hlsli"      // parameter macros (optional)
```

Delete either line if you do not want it. A shader that includes
`shaderlab_colormath.hlsli` compiles against the engine's current library, so a
fix there reaches it without an edit. To pin a shader to the math it was
written against, replace the include with a pasted copy of the functions it
calls; that shader then no longer changes when the library does. The params
header is needed only by shaders that use its macros.

The scaffold does not include `shaderlab_gamut.hlsli`; add it to a shader that
searches a gamut boundary or reads a boundary table. It provides:

| Name | Purpose |
|---|---|
| `InTargetCube`, `InTargetCubeRgb` | Whether an ICtCp coordinate reconstructs inside `[0, peak]` of the target's RGB cube (`TargetXf` from `MakeTargetXf`). |
| `CubeBoundaryRadius`, `CUBE_COARSE`, `CUBE_REFINE`, `CUBE_EPS` | The exact search: the largest chroma radius along a hue that stays in the cube. |
| `GAMUT_LUT_W`, `GAMUT_LUT_H` | The table size the LUT generator allocates. |
| `GamutLutRowToI`, `GamutLutIToRow`, `GAMUT_LUT_WARP_K` | The row spacing on the I axis, used by the generator and the reader. |
| `GamutLutColumn`, `GamutLutColumnFor`, `GamutLutRadiusAt`, `GamutLutBoundaryRadius` | Reading `B(I, hue)` from a table on a lookup input. `GamutLutColumnFor` is the hue half, so a search along one hue computes it once. |
| `GamutLutStampAlpha`, `GamutLutStampMatches`, `GAMUT_LUT_STAMP_OFFSET` | The stamp every texel carries (`.g` peak, `.b` row span, `.a` target fingerprint plus a width tag) and the check a reader makes before trusting a table. |

A reader that includes it compiles the same geometry and stamp logic as the
generator, so the two cannot disagree about the table's layout.

The target itself comes from the color math: `MakeTargetXf(gamut, R, G, B, W)`
returns the `TargetXf` for a [target gamut id](builtin-catalog.md#target-gamut-ids)
(0 sRGB, 1 Display P3, 2 BT.2020, 3 Custom from `R`/`G`/`B`/`W`, 4 DCI-P3), and
`ScRGBToTarget` / `TargetToScRGB` convert through it. The xy constants
`GAMUT_709_*`, `GAMUT_P3_*`, `GAMUT_2020_*`, `D65_WHITE` and `DCI_WHITE` are there
too, with `GAMUT_DCIP3_*` for DCI-P3's primaries as the D65 working space sees them
(Bradford-adapted, as `MakeTargetXf(4)` uses them).

All three headers carry include guards, so including one twice is harmless.
The color-math text is not guarded against an older pasted copy of itself:
keep either the include or the pasted functions, not both. The same holds for
the gamut header.

The same names resolve in every compile path: the evaluator, the bytecode
cache, graph and user-effect loading, the Effect Designer and MCP
`effect_compile` (`ShaderLabIncludeHandler()` in `Effects/ShaderCompiler.h`).
Any other `#include` fails.

## Option Variants

An `enum` parameter row has a **Variant per option** checkbox. Ticked, the
effect is compiled once per option with the value a compile-time constant, and
every variant is built as soon as the effect is placed, so switching options
never waits on a compile. The generated cbuffer declares the parameter with
`SHADERLAB_OPTION`. See [Option Variants](option-variants.md).

## D3D11 Compute Shader Workflow

D3D11 compute shaders run outside D2D's tiling system, enabling full-image reductions with atomics and groupshared memory. The Effect Designer generates a scaffold with the stride-based reduction pattern:

```hlsl
Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);

cbuffer Constants : register(b0)
{
    uint Width;   // Auto-injected
    uint Height;  // Auto-injected
    // User parameters start at offset 8
};

groupshared float4 gs_sum[32 * 32];

[numthreads(32, 32, 1)]
void main(uint3 GTid : SV_GroupThreadID)
{
    uint tid = GTid.y * 32 + GTid.x;
    float4 acc = float4(0, 0, 0, 0);

    // Stride over entire image
    for (uint y = GTid.y; y < Height; y += 32)
        for (uint x = GTid.x; x < Width; x += 32)
            acc += Source.Load(int3(x, y, 0));

    gs_sum[tid] = acc;
    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction
    for (uint s = 512; s > 0; s >>= 1) {
        if (tid < s) gs_sum[tid] += gs_sum[tid + s];
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0)
        Result[0] = gs_sum[0] / float(Width * Height);
}
```

**Key differences from D2D compute:**
- No `_TileOffset` — single dispatch covers the full image
- `Width`/`Height` auto-injected at cbuffer offset 0 (user params at offset 8)
- Output is `RWStructuredBuffer<float4>` (not `RWTexture2D<float4>`)
- Results map to typed analysis output fields (one `float4` per field)
- Supports atomics, full-image groupshared memory, and arbitrary reduction patterns

## Opening Built-in Effects

ShaderLab's built-in effects can be opened in the Effect Designer via the **"Edit in Effect Designer"** button in the Properties panel. The designer loads the effect's HLSL source, parameters, and analysis field definitions. Edits can be compiled and pushed back into the running graph.

## Export (Future)

The Effect Designer will support exporting D3D11 compute effects as standalone C++ header/module files. The export includes the HLSL source, input/parameter/output schema, and the dispatch contract. Developers can then customize the C++ post-processing (e.g., histogram → median computation) in their own codebase.

## Import from External Binary (Planned)

ShaderLab does not currently support importing a fully compiled effect from an external DLL or binary module. All effects are either built-in (registered at startup) or authored within the Effect Designer from HLSL source. A future release will add the ability to load pre-compiled D2D effect DLLs (implementing `ID2D1EffectImpl`) and D3D11 compute shader binaries (`.cso` files) directly into the graph, enabling teams to develop effects in external toolchains and test them inside ShaderLab without providing source code.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)