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
  ├── GraphEvaluator      — Topological walk of EffectGraph, per-node D2D effect cache
  ├── ToneMapper          — D2D WhiteLevelAdjustment + HdrToneMap + ColorMatrix exposure chain
  ├── EffectGraph (DAG)   — Nodes, edges, Kahn's topo sort, JSON serialization (Windows.Data.Json)
  ├── SourceNodeFactory   — WIC image loading (HDR/SDR format split) + Flood fill sources
  ├── ShaderEditorCtrl    — Live HLSL compile (D3DCompile), D3DReflect auto-property generation
  ├── NodeGraphController — D2D-rendered canvas: bezier edges, color-coded nodes, pan/zoom, hit-test
  └── PixelInspectorCtrl  — GPU readback (1×1 D2D1Bitmap1), scRGB→sRGB/PQ/luminance conversions
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
- Node properties stored as `std::map<std::wstring, PropertyValue>` where `PropertyValue = std::variant<float, int32_t, uint32_t, bool, std::wstring, float2, float3, float4>`
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

- **Pipeline format is configurable**: scRGB FP16 (default), sRGB 8-bit, HDR10 (PQ/BT.2020), Linear FP32. Format affects swap chain, render targets, tone mapper, and pixel inspector.
- **Swap chain**: `CreateSwapChainForComposition` + `ISwapChainPanelNative` (WinUI 3 requirement). Color space set via `SetColorSpace1()`.
- **Display monitoring**: Dual path — `WM_DISPLAYCHANGE` via hidden message-only HWND + `IDXGIFactory7::RegisterAdaptersChangedEvent` on a jthread.
- **Graph serialization**: `Windows.Data.Json` (zero extra dependencies). GUID fields use `StringFromGUID2`/`CLSIDFromString`.
- **Effect registry**: Singleton with 40+ built-in D2D effects across 9 categories. Case-insensitive name lookup.
- **Ctrl+Enter** compiles shader from the editor TextBox.

## Adding New Files

1. Create `.h` and `.cpp` in the appropriate directory (`Graph/`, `Rendering/`, `Effects/`, `Controls/`)
2. Add both to `ShaderLab.vcxproj` under the correct `<ClInclude>` / `<ClCompile>` `<ItemGroup>`
3. Add to `ShaderLab.vcxproj.filters` for VS Solution Explorer organization
4. Use the matching `ShaderLab::SubNamespace` namespace
5. `#include "pch.h"` as the first line in every `.cpp` file
