# ShaderLab — Development Context (Resume Point)

## Project Identity

**ShaderLab** is a WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D shader effects with full HDR and wide color gamut (WCG) support.

- **Location**: `E:\source\ShaderLab\ShaderLab.slnx`
- **Version**: 1.1.0 (app) / format version 2 (graph serialization)
- **Language**: C++/WinRT — direct COM access to `ID2D1EffectImpl`, `ID2D1DrawTransform`, `ID2D1ComputeTransform`

---

## Complete Feature Set (v1.1.0)

### Core
- Node-based DAG graph editor for D2D effect composition
- 40+ built-in D2D effects across 9 categories
- 9 ShaderLab built-in effects (analysis + source generators)
- Custom pixel shader effects (`ID2D1DrawTransform`)
- Custom compute shader effects (`ID2D1ComputeTransform`)
- Live HLSL hot-reload with `D3DCompile` + `D3DReflect` auto-property discovery
- Effect Designer window for creating custom effects with full parameter definition
- Graph JSON serialization with versioning (format version 2)

### ShaderLab Built-in Effects
- **Analysis**: Luminance Heatmap, Out-of-Gamut Highlight, CIE Chromaticity Plot, Vectorscope
- **Source**: Gamut Source, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern

### Property System
- `PropertyValue` variant: `float`, `int32`, `uint32`, `bool`, `wstring`, `float2`, `float3`, `float4`, `D2D1_MATRIX_5X4_F`, `vector<float>`
- Per-component property bindings (Grasshopper-style data flow)
- Enum labels for named dropdown parameters
- Hidden properties for host-managed cbuffer values (e.g., monitor primaries)
- Visual data pins (orange diamonds) on node graph for binding connections

### Rendering
- Always scRGB FP16 pipeline (`DXGI_FORMAT_R16G16B16A16_FLOAT`)
- Dirty-gated render loop (~60 FPS via `DispatcherQueueTimer`)
- Tone mapper with 5 modes (None, Reinhard, ACES Filmic, Hable, SDR Clamp) — defaults to None
- Display profile mocking (presets + ICC file loading)
- Monitor gamut detection from `DXGI_OUTPUT_DESC1` primaries
- GPU info display (hardware adapter name or "Software (WARP)")
- D2D-rendered node graph canvas with pan/zoom, bezier edges, color-coded nodes

### MCP Server (21 tools)
Port 47808, JSON-RPC 2.0 over HTTP:
1. `graph_add_node` — Add built-in D2D or ShaderLab effect
2. `graph_remove_node` — Remove a node
3. `graph_connect` — Connect image pins
4. `graph_disconnect` — Disconnect image pins
5. `graph_set_property` — Set a node property
6. `graph_get_node` — Get node details + analysis results
7. `graph_save_json` — Serialize graph to JSON
8. `graph_load_json` — Load graph from JSON
9. `graph_clear` — Clear graph (keeps Output)
10. `effect_compile` — Compile HLSL for custom effect
11. `set_preview_node` — Set preview target
12. `render_capture` — Capture preview as PNG
13. `registry_get_effect` — Get built-in effect metadata
14. `graph_bind_property` — Bind property to analysis output
15. `graph_unbind_property` — Remove binding
16. `read_analysis_output` — Read typed analysis fields
17. `read_pixel_trace` — Pixel trace at normalized coords
18. `list_effects` — List all effects by category
19. `graph_overview` — Compact graph summary
20. `get_display_info` — Display caps, version, pipeline info
21. `graph_rename_node` — Rename a node

### Effect Designer
- Pixel shader and compute shader types
- Parameter types: float, float2, float3, float4, int, uint, bool, enum
- Enum parameters with comma-separated label definition
- Bool parameters show ToggleSwitch (not NumberBox)
- Analysis output field declaration for compute shaders
- HLSL auto-formatting and scaffold generation
- Add to graph / Update in graph buttons

### Versioning
- `Version.h`: App 1.1.0, Graph format version 2
- Status bar + title bar show version
- Saved graphs include `formatVersion` + `appVersion`
- Loading newer-format graphs shows error dialog

### UI
- Segoe Fluent Icons on toolbar buttons with tooltips
- Dot grid on graph canvas (scales with pan/zoom)
- Refined node color palette, dark theme
- Status bar: pipeline format, display mode, luminance, GPU, FPS, version

---

## D2D Custom Effect Gotchas (Hard-Won Knowledge)

These are critical lessons learned during development. Any AI agent or developer working on custom D2D effects **must** be aware of these:

1. **`uint` cbuffer params DON'T WORK in D2D pixel shaders** — values pack correctly but the shader never sees updates. Use `float` with threshold comparisons (`> 0.5`, `> 1.5`).
2. **HLSL compiler optimizes out cbuffer variables** not referenced on ALL code paths when `D3DCOMPILE_WARNINGS_ARE_ERRORS` is set. Read all cbuffer vars at top of `main()` before any branches.
3. **D2D custom effects need TWO evaluation passes** for newly created effects — first creates/initializes, second produces correct output.
4. **`RegisterWithInputCount` requires `inputCount >= 1`**. Zero-input source effects use a hidden dummy bitmap.
5. **`MapInputRectsToOutputRect` with `SetFixedOutputSize`** must check fixed size FIRST, before input rect.
6. **D2D `TEXCOORD` values are in pixel/scene space**, NOT normalized [0,1]. Use `GetDimensions()` and divide.
7. **D2D custom effect transforms must NOT pass through infinite input rects** in `MapInputRectsToOutputRect`.
8. **`ForceUploadConstantBuffer()` uploads cbuffer but doesn't invalidate cached output**. Need input toggle trick to force re-evaluation.
9. **Monitor gamut from `DXGI_OUTPUT_DESC1` primaries** (`RedPrimary`, `GreenPrimary`, `BluePrimary`, `WhitePoint` fields).
10. **Always write primaries into OOG properties** (ensure correct on first frame), only dirty on change (prevent feedback loops).

---

## Build / Deploy / Launch

### Prerequisites
- Visual Studio 2022 17.8+ (C++ Desktop + UWP workloads)
- Windows App SDK 1.8
- Windows 10 SDK 10.0.26100+
- Win2D 1.3.0

### Build
```
# Via Visual Studio
Open ShaderLab.slnx → Build → Debug | x64

# Via MSBuild
msbuild ShaderLab.vcxproj /p:Configuration=Debug /p:Platform=x64
```

NuGet packages restore automatically (packages.config style).

### Launch
Deploy and run via Visual Studio (F5). MSIX packaged (`runFullTrust`).

### Linked Libraries
`d3d11.lib`, `d2d1.lib`, `dxgi.lib`, `d3dcompiler.lib`, `dxguid.lib`, `windowscodecs.lib`

---

## Project Structure

```
ShaderLab\
├── ShaderLab.slnx / .vcxproj      # Solution + project
├── Version.h                        # App 1.1.0, graph format 2
├── pch.h / pch.cpp                  # Precompiled header
├── App.xaml / .h / .cpp             # WinUI 3 entry point
├── MainWindow.xaml / .h / .cpp      # Main window (~4100 lines)
├── MainWindow.McpRoutes.cpp         # MCP server routes (~1400 lines)
├── EffectDesignerWindow.*           # Effect Designer modal window
├── Graph\
│   ├── NodeType.h                   # NodeType enum
│   ├── PropertyValue.h              # PropertyValue variant
│   ├── EffectNode.h                 # EffectNode, ParameterDefinition, PropertyBinding, etc.
│   ├── EffectEdge.h                 # EffectEdge struct
│   └── EffectGraph.h / .cpp         # DAG with topo sort, JSON, versioning
├── Rendering\
│   ├── RenderEngine.h / .cpp        # D3D11 + D2D1 + DXGI swap chain
│   ├── GraphEvaluator.h / .cpp      # Topological evaluation, auto-compile, dirty gating
│   ├── ToneMapper.h / .cpp          # 5 tone map modes
│   ├── DisplayMonitor.h / .cpp      # HDR/SDR detection, gamut primaries, jthread
│   ├── DisplayProfile.h             # Profile structs, presets
│   ├── DisplayInfo.h                # DisplayCapabilities + monitor primaries
│   ├── PipelineFormat.h             # scRGB FP16 (always)
│   ├── FalseColorOverlay.h          # False color rendering
│   └── IccProfileParser.h / .cpp    # ICC v2/v4 binary parsing
├── Effects\
│   ├── ShaderLabEffects.h / .cpp    # 9 built-in effects + color math HLSL library
│   ├── EffectRegistry.h / .cpp      # 40+ built-in D2D effect catalog
│   ├── CustomPixelShaderEffect.h/.cpp   # ID2D1DrawTransform implementation
│   ├── CustomComputeShaderEffect.h/.cpp # ID2D1ComputeTransform implementation
│   ├── ShaderCompiler.h / .cpp      # D3DCompile + D3DReflect wrapper
│   ├── ImageLoader.h / .cpp         # WIC HDR/SDR image loading
│   ├── SourceNodeFactory.h / .cpp   # Source node creation
│   └── PropertyMetadata.h           # Effect property metadata
├── Controls\
│   ├── NodeGraphController.h / .cpp # D2D canvas node graph editor
│   ├── ShaderEditorController.h/.cpp # Live HLSL compile controller
│   ├── PixelInspectorController.h/.cpp # GPU readback pixel inspection
│   └── PixelTraceController.h / .cpp  # Recursive pixel trace
├── ShaderLab\
│   ├── McpHttpServer.h / .cpp       # Winsock2 TCP server, JSON-RPC
├── .github\copilot-instructions.md  # AI agent instructions
├── .context\resume.md               # This file
└── CHANGELOG.md                     # Version history
```

---

## Potential Future Work

- **Waveform Monitor**: Compute shader approach for real-time waveform display
- **Processing Effects**: Gamut Map, ACES Tonemap, Soft Clip effects
- **Histogram Visualizer**: Compute shader histogram with visual overlay
- **Hide Prim* data pins from OOG nodes**: The Out-of-Gamut Highlight effect exposes `PrimRedX`, `PrimRedY`, etc. as visible data pins — these are host-managed hidden properties and should not show as connectable pins on the node graph
