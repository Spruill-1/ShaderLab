# Copilot Instructions

## Project Identity

ShaderLab is a WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D shader effects with full HDR/WCG support. The primary goals are fixing issues with the existing D2D tonemapper effect and adding capabilities to the color correction effect.

## Hard Rules

- **C++/WinRT only** — never generate C# code. Direct COM access to `ID2D1EffectImpl`, `ID2D1DrawTransform`, `ID2D1ComputeTransform` is the reason this project exists.
- **README.md is a living architecture doc** — update it with Mermaid diagrams and a decision log entry whenever a significant architectural decision is made.
- **All new `.cpp` files must `#include "pch.h"` as the first include** — precompiled header is mandatory (`pch.h` aggregates WinRT, D2D, D3D, Win2D, DXGI, WIC, and STL headers).

## Build

- Open `ShaderLab.slnx` in Visual Studio 2022 17.8+
- NuGet packages restore automatically (packages.config style, not PackageReference)
- Build target: **Debug | x64** (also supports ARM64, Release)
- No command-line build scripts exist; use MSBuild via VS or `msbuild ShaderLab.vcxproj /p:Configuration=Debug /p:Platform=x64`
- Required: Windows App SDK 1.8, Windows 10 SDK 10.0.26100+, Win2D 1.3.0
- Linked native libs: `d3d11.lib`, `d2d1.lib`, `dxgi.lib`, `d3dcompiler.lib`, `dxguid.lib`, `windowscodecs.lib`
- `/bigobj` is enabled; language standard is C++20 (VS 18+) or C++17 (VS 17)

## Architecture

```
MainWindow (WinUI 3 XAML)
  ├── RenderEngine        — D3D11 + D2D1 device stack, DXGI swap chain, BeginDraw/EndDraw/Present
  ├── DisplayMonitor      — HDR/SDR detection, WM_DISPLAYCHANGE + adapter-change jthread
  ├── GraphEvaluator      — Topological walk of EffectGraph, per-node D2D effect cache, dirty gating
  ├── ToneMapper          — D2D WhiteLevelAdjustment + HdrToneMap + ColorMatrix exposure chain
  ├── EffectGraph (DAG)   — Nodes, edges, Kahn's topo sort, JSON serialization (Windows.Data.Json)
  ├── SourceNodeFactory   — WIC image loading (HDR/SDR format split) + Flood fill sources
  ├── ShaderLabEffects    — 9 built-in effects (analysis + source) with embedded HLSL + color math
  ├── ShaderEditorCtrl    — Live HLSL compile (D3DCompile), D3DReflect auto-property generation
  ├── NodeGraphController — D2D-rendered canvas: bezier edges, color-coded nodes, pan/zoom, hit-test
  ├── PixelInspectorCtrl  — GPU readback (1×1 D2D1Bitmap1), scRGB→sRGB/PQ/luminance conversions
  ├── PixelTraceCtrl      — Recursive per-node pixel readback through effect graph
  ├── FalseColorOverlay   — False color rendering overlay
  └── EffectDesignerWindow — Modal window for creating/editing custom pixel/compute shader effects
```

Render loop: `DispatcherQueueTimer` at 16ms → `GraphEvaluator.Evaluate()` → `ToneMapper.Apply()` → `BeginDraw` → `DrawImage` → `EndDraw` → `Present`.

## Namespace Convention

All code lives under `ShaderLab::` with sub-namespaces matching directories:
- `ShaderLab::Graph` — data model (EffectGraph, EffectNode, EffectEdge, PropertyValue)
- `ShaderLab::Rendering` — device stack, swap chain, evaluator, tone mapping
- `ShaderLab::Effects` — effect registry, custom effects, shader compilation, image loading
- `ShaderLab::Controls` — UI controllers (decoupled from XAML views)
- `winrt::ShaderLab::implementation` — WinRT XAML types (App, MainWindow)

## Code Conventions

### COM & Pointer Patterns
- **Member COM objects**: Always `winrt::com_ptr<T>` with `m_` prefix (e.g., `m_d3dDevice`, `m_swapChain`)
- **API output params**: Use `.put()` for COM creation, `.get()` for passing to raw APIs, `.as<T>()` for QueryInterface
- **D2D cached outputs in graph nodes**: Raw `ID2D1Image*` (non-owning; render engine manages lifetime)
- **Custom D2D effects**: Manual `IUnknown` ref counting with `InterlockedIncrement/Decrement` on `LONG m_refCount` — these implement `ID2D1EffectImpl` directly, not via WinRT

### Error Handling
- Initialization paths: `winrt::check_hresult()` to throw on failure
- Render/runtime paths: `SUCCEEDED(hr)` / `FAILED(hr)` checks with early return (no exceptions in hot paths)
- Graph operations: Return `bool` for success/failure; `nullptr` for not-found
- Topological sort: Throws `std::logic_error` on cycle detection

### Naming
- **Member variables**: `m_` prefix (`m_graph`, `m_renderEngine`, `m_refCount`)
- **Methods**: PascalCase (`AddNode`, `PrepareForRender`, `TopologicalSort`)
- **Enums**: PascalCase values (`NodeType::BuiltInEffect`, `ToneMapMode::ACESFilmic`)
- **Structs**: PascalCase, used for plain data (`EffectNode`, `DisplayCapabilities`, `ShaderVariable`)
- **Classes**: PascalCase, used for stateful objects with methods (`EffectGraph`, `RenderEngine`, `ToneMapper`)

### File Organization
- **Header-only**: Small data structs and inline constants (`NodeType.h`, `PropertyValue.h`, `DisplayInfo.h`, `PipelineFormat.h`, `EffectEdge.h`, `EffectNode.h`)
- **Header + .cpp**: Stateful classes with complex logic
- **One class per file pair** (matching names)
- **Anonymous namespaces** for file-local helpers (e.g., JSON serialization helpers in `EffectGraph.cpp`)

### Property System
- Node properties stored as `std::map<std::wstring, PropertyValue>` where `PropertyValue = std::variant<float, int32_t, uint32_t, bool, std::wstring, float2, float3, float4, D2D1_MATRIX_5X4_F, std::vector<float>>`
- JSON serialization uses type tags (`"float"`, `"float4"`, etc.) for round-trip fidelity
- Shader constant buffers packed from PropertyValue via D3DReflect offsets (byte-level memcpy)

## Custom D2D Effect Pattern

When creating new D2D effects (the core purpose of this tool):
1. Implement `ID2D1EffectImpl` + `ID2D1DrawTransform` (pixel shader) or `ID2D1ComputeTransform` (compute shader)
2. Register with `ID2D1Factory1::RegisterEffectFromString()` using XML schema + `D2D1_VALUE_TYPE_BINDING` macros
3. Keep only `InputCount` as a D2D-exposed property; manage shader bytecode and cbuffers host-side
4. Use `ShaderCompiler` for `D3DCompile` (ps_5_0 / cs_5_0) + `D3DReflect` for cbuffer discovery
5. Register effects in `MainWindow::RegisterCustomEffects()` at startup
6. Follow `CustomPixelShaderEffect` / `CustomComputeShaderEffect` as templates

## Tone Mapping & Color Correction (Primary Development Focus)

The tone mapping system (`Rendering/ToneMapper.h/.cpp`) and color correction are where active development happens:
- Current tone map modes: None, Reinhard, ACES Filmic (Narkowicz 2015), Hable (Uncharted 2), SDR Clamp
- Uses D2D built-in effects: `CLSID_D2D1WhiteLevelAdjustment`, `CLSID_D2D1HdrToneMap`, `CLSID_D2D1ColorMatrix`
- Reference math implementations exist inline for each operator (for porting to custom shaders)
- Pipeline works in scRGB FP16 linear light (1.0 = 80 nits SDR white)
- **Goals**: Fix issues with the existing D2D tonemapper effect; extend the color correction effect with additional capabilities

## Key Technical Context

- **Pipeline is always scRGB FP16**: `DXGI_FORMAT_R16G16B16A16_FLOAT` with `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`. No pipeline format switching — DWM/ACM handles final display conversion.
- **Swap chain**: `CreateSwapChainForComposition` + `ISwapChainPanelNative` (WinUI 3 requirement). Color space set via `SetColorSpace1()`.
- **Display monitoring**: Dual path — `WM_DISPLAYCHANGE` via hidden message-only HWND + `IDXGIFactory7::RegisterAdaptersChangedEvent` on a jthread.
- **Graph serialization**: `Windows.Data.Json` (zero extra dependencies). GUID fields use `StringFromGUID2`/`CLSIDFromString`.
- **Effect registry**: Singleton with 40+ built-in D2D effects across 9 categories. Case-insensitive name lookup.
- **ShaderLab effects library**: 20+ built-in effects in `Effects/ShaderLabEffects.h/.cpp` (Analysis + Color Processing + Source/Generator + Parameter / Clock / Numeric Expression). Embedded HLSL with shared color math. Auto-compiled at first use.
- **MCP server**: 23-tool JSON-RPC 2.0 server on port 47808 for AI agent integration. Routes in `MainWindow.McpRoutes.cpp`.
- **Versioning**: `Version.h` defines app version (1.2.2) and graph format version (2). Both stored in saved graphs. Forward compatibility check on load.
- **Dirty-gated render loop**: `DispatcherQueueTimer` at 16ms (~60 FPS). Skips evaluate when no nodes changed. Analysis readback only on dirty frames.
- **Ctrl+Enter** compiles shader from the editor TextBox.

## Adding New Files

1. Create `.h` and `.cpp` in the appropriate directory (`Graph/`, `Rendering/`, `Effects/`, `Controls/`)
2. Add both to `ShaderLab.vcxproj` under the correct `<ClInclude>` / `<ClCompile>` `<ItemGroup>`
3. Add to `ShaderLab.vcxproj.filters` for VS Solution Explorer organization
4. Use the matching `ShaderLab::SubNamespace` namespace
5. `#include "pch.h"` as the first line in every `.cpp` file

## D2D Custom Effect Gotchas

These are critical lessons learned during development. Any AI agent generating or modifying D2D custom effect code **must** account for these:

1. **`uint` cbuffer params DON'T WORK in D2D pixel shaders** — values pack correctly in the constant buffer but the shader never sees updates. Use `float` with threshold comparisons (`> 0.5`, `> 1.5`) instead.
2. **HLSL compiler with `D3DCOMPILE_WARNINGS_ARE_ERRORS` optimizes out cbuffer variables** not referenced on ALL code paths. Read all cbuffer vars at top of `main()` before any branches.
3. **D2D custom effects need TWO evaluation passes** for newly created effects — first creates/initializes, second produces correct output. Don't expect correct output on the first frame after creation.
4. **`RegisterWithInputCount` requires `inputCount >= 1`**. Zero-input source effects must use a hidden dummy bitmap input.
5. **`MapInputRectsToOutputRect` with `SetFixedOutputSize`** must check fixed size FIRST, before examining input rect. Getting this order wrong causes incorrect output sizing.
6. **D2D `TEXCOORD` values are in pixel/scene space**, NOT normalized [0,1]. Always use `GetDimensions()` and divide to get normalized UVs.
7. **D2D custom effect transforms must NOT pass through infinite input rects** in `MapInputRectsToOutputRect`. Infinite rects cause D2D to attempt unbounded rendering.
8. **`ForceUploadConstantBuffer()` uploads cbuffer but doesn't invalidate cached output**. Need an input toggle trick (disconnect+reconnect dummy input) to force D2D to re-evaluate the effect.
9. **Monitor gamut comes from `DXGI_OUTPUT_DESC1` primaries** — use `RedPrimary`, `GreenPrimary`, `BluePrimary`, `WhitePoint` fields. These are CIE xy chromaticity coordinates.
10. **Always write primaries into OOG properties on every evaluate** (ensures correct values on first frame), but only mark dirty on actual change (prevents feedback loops between property writes and evaluation).

## ShaderLab Effects Library

The built-in effects library lives in `Effects/ShaderLabEffects.h/.cpp`:

- **Embedded HLSL**: Each effect's shader code is stored as a `const char*` string constant. No external `.hlsl` files.
- **Shared color math**: A common HLSL library (BT.709/BT.2020/P3 color matrices, PQ/HLG transfer functions, CIE XYZ↔xy conversions, luminance calculations) is prepended to each shader at compile time.
- **Auto-compile**: Effects are compiled via `ShaderCompiler` at first use (when added to graph). Compiled bytecode is cached.
- **Categories**:
  - **Analysis** (4): Luminance Heatmap (PS), Out-of-Gamut Highlight (CS), CIE Chromaticity Plot (CS), Vectorscope (CS)
  - **Source** (5): Gamut Source (PS), Color Checker (PS), Zone Plate (PS), Gradient Generator (PS), HDR Test Pattern (PS)
- **Hidden defaults**: Host-managed properties (e.g., monitor primaries for OOG Highlight) are marked `isHidden = true` in `ParameterDefinition`. They don't appear in the Properties panel but are written by the host before each evaluate.
- **Analysis outputs**: Compute shader analysis effects declare `AnalysisFieldDef` entries that describe their output fields (name, type, count). These are read back to CPU via `GraphEvaluator` and can be bound to downstream properties.

## Effect Designer

`EffectDesignerWindow.xaml/.h/.cpp` provides a dedicated modal window for creating custom effects:

- **Shader types**: Pixel shader (`ps_5_0`) and compute shader (`cs_5_0`)
- **Parameter definition**: Users define parameters with types: `float`, `float2`, `float3`, `float4`, `int`, `uint`, `bool`, `enum`
  - **Enum params**: Comma-separated label definition (e.g., `"Off,Low,Medium,High"`) generates a dropdown in the Properties panel
  - **Bool params**: Rendered as `ToggleSwitch` controls (not `NumberBox`)
- **Analysis output fields**: For compute shaders, users can declare typed output fields that will be readable via `read_analysis_output`
- **HLSL auto-formatting**: Basic indent/brace formatting applied on edit
- **Scaffold generation**: Templates generate a starting HLSL shader with the declared parameters already in the cbuffer
- **Graph integration**: "Add to Graph" creates a new node; "Update in Graph" recompiles and updates an existing node
