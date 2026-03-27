# ShaderLab — Development Context (Resume Point)

## Project Location
`E:\source\ShaderLab\ShaderLab.slnx`

## What's Built & Building (Steps 1–18 of 18 ✅ COMPLETE)

### Step 1: README.md ✅
- 5 Mermaid diagrams: architecture, pipeline format strategy, effect graph class diagram, display monitoring sequence, topological evaluation flowchart
- 17-entry decision log
- Build instructions with library table
- Full project structure tree

### Step 2: Project Scaffold ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `ShaderLab.vcxproj` | C++/WinRT, WinUI 3, packages.config NuGet, MSIX, `/bigobj`, C++17/20 |
| `packages.config` | 16 packages: WinAppSDK 1.8.260317003, CppWinRT 2.0.250303.1, **Win2D 1.3.0**, SDK BuildTools 10.0.26100.7705, WebView2, WIL, etc. |
| `pch.h` | WinRT base, WinUI/XAML, Win2D, D3D11 (`d3d11_4.h`), D2D (`d2d1_3.h`, `d2d1effects_2.h`, `d2d1effectauthor_1.h`), DXGI (`dxgi1_6.h`), DWrite, D3DCompiler, WIC, `Microsoft.Graphics.Canvas.native.h`, STL |
| `MainWindow.xaml` | Grid layout: left=node graph placeholder (2*), right=SwapChainPanel + TabView (Properties / Shader Editor / Pixel Inspector), bottom=status bar (pipeline format, display mode, luminance, FPS) |
| `MainWindow.xaml.h/.cpp` | RenderEngine + DisplayMonitor members, InitializeRendering wires device stack, UpdateStatusBar shows live display/format info, OnPreviewSizeChanged handles resize |
| `MainWindow.idl` | WinRT runtime class definition |
| `App.xaml/.h/.cpp` | Standard WinUI 3 entry, debug `UnhandledException` handler, creates `MainWindow` |
| `Package.appxmanifest` | MSIX identity `ShaderLab`, `runFullTrust` |
| `app.manifest` | PerMonitorV2 DPI, SegmentHeap, Win10+ compat |
| `README.md` | Living architecture doc (see above) |
| `.github/copilot-instructions.md` | Rules: C++/WinRT only, README with Mermaid diagrams |

### Linked Libraries (in vcxproj `<Link>`)
`d3d11.lib`, `d2d1.lib`, `dxgi.lib`, `d3dcompiler.lib`, `dxguid.lib`, `windowscodecs.lib`

### Directory Structure (empty, ready for code)
`Shaders/`, `Controls/`, `Assets/`

### Step 3: Graph Data Model ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Graph/NodeType.h` | `NodeType` enum: Source, BuiltInEffect, PixelShader, ComputeShader, Output; string conversion helpers |
| `Graph/PropertyValue.h` | `PropertyValue` = `std::variant<float, int32_t, uint32_t, bool, wstring, float2, float3, float4>`; type tag helper |
| `Graph/EffectNode.h` | `EffectNode` struct: id, name, type, position, properties map, input/output pin descriptors, optional CLSID/shader path, cached output, dirty flag |
| `Graph/EffectEdge.h` | `EffectEdge` struct: sourceNodeId, sourcePin, destNodeId, destPin; defaulted `operator==` |
| `Graph/EffectGraph.h` | `EffectGraph` class: AddNode/RemoveNode, Connect/Disconnect, TopologicalSort, WouldCreateCycle, JSON round-trip, accessors |
| `Graph/EffectGraph.cpp` | Full implementation: Kahn's algorithm topo sort, BFS cycle detection, `Windows.Data.Json` serialization with tagged PropertyValue types |
| `pch.h` | Added `winrt/Windows.Data.Json.h` include |

### Step 4: Display Change Monitoring ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Rendering/DisplayInfo.h` | `DisplayCapabilities` struct: hdrEnabled, bitsPerColor, colorSpace, SDR white level, min/max/fullFrame luminance nits; `ModeString()`/`LuminanceString()` helpers; `DisplayChangeCallback` typedef |
| `Rendering/DisplayMonitor.h` | `DisplayMonitor` class: Initialize(HWND, IDXGIFactory7*), Shutdown, QueryCurrentCapabilities, SetCallback; hidden msg window + jthread adapter event |
| `Rendering/DisplayMonitor.cpp` | Full implementation: DXGI output query via MonitorFromWindow→EnumAdapters→IDXGIOutput6::GetDesc1, hidden HWND_MESSAGE window for WM_DISPLAYCHANGE, IDXGIFactory7::RegisterAdaptersChangedEvent on jthread, change-diffing callback dispatch |
| `pch.h` | Added `<thread>` include |

### Step 5: PipelineFormat Abstraction ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Rendering/PipelineFormat.h` | `PipelineFormat` struct: dxgiFormat, colorSpace, name, bitsPerChannel, isLinear, isFloatingPoint, BytesPerPixel(); 4 inline constants (FormatScRgbFP16, FormatSrgb8, FormatHdr10, FormatLinearFP32); AllFormats[] array; RecommendedFormat(caps) helper |

### Step 6: Rendering Engine ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Rendering/RenderEngine.h` | `RenderEngine` class: Initialize(HWND, SwapChainPanel, PipelineFormat), Shutdown, Resize, SetPipelineFormat, BeginDraw/EndDraw/Present; D3D11/D2D1/DXGI accessors |
| `Rendering/RenderEngine.cpp` | Full implementation: D3D11CreateDevice→D2D1CreateFactory→ID2D1Device6→DeviceContext5, CreateSwapChainForComposition→ISwapChainPanelNative, SetColorSpace1, D2D1Bitmap1 from back buffer, Resize→ResizeBuffers, SetPipelineFormat recreates swap chain |
| `MainWindow.xaml.h` | Added RenderEngine + DisplayMonitor members, destructor, OnPreviewSizeChanged handler |
| `MainWindow.xaml.cpp` | InitializeRendering creates full device stack + swap chain, UpdateStatusBar shows live display/format data, SizeChanged wired |

### Step 7: Per-Node Graph Evaluation ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Rendering/GraphEvaluator.h` | `GraphEvaluator` class: Evaluate(EffectGraph&, ID2D1DeviceContext5*)→ID2D1Image*, ReleaseCache, InvalidateNode; per-node effect cache (nodeId→com_ptr<ID2D1Effect>) |
| `Rendering/GraphEvaluator.cpp` | Topological walk: Source pass-through, BuiltInEffect create+wire+apply, PixelShader/ComputeShader placeholder, Output pass-through; property application via GetPropertyIndex+SetValue with PropertyValue variant dispatch; edge wiring via SetInput |

### Step 8: Source Nodes ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Effects/ImageLoader.h` | `ImageLoader` class: LoadFromFile, LoadFromStream; WIC factory; HDR pixel format detection |
| `Effects/ImageLoader.cpp` | WIC decode pipeline: CreateDecoderFromFilename→GetFrame→FormatConverter (SDR→32bppPBGRA, HDR→64bppRGBAHalf)→CreateBitmapFromWicBitmap |
| `Effects/SourceNodeFactory.h` | `SourceNodeFactory` class: CreateImageSourceNode, CreateFloodSourceNode (static); PrepareSourceNode; bitmap+flood caches |
| `Effects/SourceNodeFactory.cpp` | Image sources load via ImageLoader + bitmap cache; Flood sources create CLSID_D2D1Flood + apply Color property (D2D1_FLOOD_PROP_COLOR) + flood cache |

### Step 9: Built-in D2D Effect Wrappers ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Effects/EffectRegistry.h` | `EffectDescriptor` struct (CLSID, name, category, pins, default properties) + `EffectRegistry` singleton class: `FindByName`, `FindByClsid`, `ByCategory`, `Categories`, `CreateNode`, `CreateOutputNode` |
| `Effects/EffectRegistry.cpp` | 40+ built-in D2D effects registered across 9 categories: Blur (3), Color (16), Composition (5), Transform (6), Detail (5), Lighting (6), Distort (2+Turbulence), HDR (2), Analysis (1). Uses designated initializers + SINGLE_INPUT/DUAL_INPUT macros. |

### Step 10: Custom Pixel Shaders ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Effects/ShaderCompiler.h` | `ShaderVariable`, `ShaderConstantBuffer`, `ShaderCompileResult`, `ShaderReflectionResult` structs + `ShaderCompiler` class: `CompileFromFile`, `CompileFromString`, `Reflect` |
| `Effects/ShaderCompiler.cpp` | D3DCompile with debug/release flags, D3D_COMPILE_STANDARD_FILE_INCLUDE, D3DReflect for cbuffer/SRV enumeration, MultiByteToWideChar for error messages |
| `Effects/CustomPixelShaderEffect.h` | `CustomPixelShaderEffect` class: ID2D1EffectImpl + ID2D1DrawTransform dual implementation; CLSID; RegisterEffect/UnregisterEffect; LoadShaderBytecode; PackConstantBuffer |
| `Effects/CustomPixelShaderEffect.cpp` | D2D effect registration (XML + D2D1_VALUE_TYPE_BINDING for InputCount), IUnknown ref counting, PrepareForRender (LoadPixelShader + SetPixelShaderConstantBuffer), MapInputRectsToOutputRect (union), PackConstantBuffer (PropertyValue variant → byte layout via reflection offsets) |
| `pch.h` | Added `d2d1effecthelpers.h` include |

### Step 11: Custom Compute Shaders ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Effects/CustomComputeShaderEffect.h` | `CustomComputeShaderEffect` class: ID2D1EffectImpl + ID2D1ComputeTransform dual implementation; CLSID; RegisterEffect/UnregisterEffect; LoadShaderBytecode; PackConstantBuffer; SetThreadGroupSize |
| `Effects/CustomComputeShaderEffect.cpp` | D2D registration, IUnknown ref counting, PrepareForRender (LoadComputeShader + SetComputeShaderConstantBuffer), SetComputeInfo, CalculateThreadgroups (output / group size round-up), CheckFeatureSupport for compute capability |

### Step 12: Live Shader Editor ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Controls/ShaderEditorController.h` | `AutoProperty` struct, `EditorCompileResult` struct, `ShaderEditorController` class: Compile, CompileFromFile, DefaultPixelShaderTemplate, DefaultComputeShaderTemplate |
| `Controls/ShaderEditorController.cpp` | Compile from string/file via ShaderCompiler, D3DReflect auto-property generation (D3D_SVT_FLOAT→float/float2-4, INT→int32, UINT→uint32, BOOL→bool), error line parsing from D3DCompile output, default PS/CS HLSL templates |

### Step 13: Visual Node Graph Editor ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Controls/NodeGraphController.h` | `NodeVisual`, `ConnectionDrag`, `SelectionState` structs + `NodeGraphController` class: SetGraph, RebuildLayout, HitTestNode/Pin, drag nodes/connections, selection, Render to D2D, pan/zoom |
| `Controls/NodeGraphController.cpp` | D2D bezier edge rendering, color-coded nodes per NodeType (green=Source, blue=BuiltIn, red=PS, orange=CS, gray=Output), DWrite text format for titles, pin circle hit-test, screen↔canvas coordinate transforms |

### Step 14: Pixel Inspector ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Controls/PixelInspectorController.h` | `InspectedPixel` struct (scRGB, sRGB, PQ, luminance, position) + `PixelInspectorController` class: InspectPixel, ReInspect, tracked position |
| `Controls/PixelInspectorController.cpp` | D2D1Bitmap1 target→CPU_READ readback, DrawImage at pixel coords, LinearToSRGB (transfer function), LinearToPQ (ST.2084), BT.709 luminance (80 nit ref white) |

### Step 15: Tone Mapping ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `Rendering/ToneMapper.h` | `ToneMapMode` enum (None, Reinhard, ACESFilmic, Hable, SDRClamp) + `ToneMapper` class: Initialize, SetMode, SetExposure, SetSDRWhiteLevel, Apply |
| `Rendering/ToneMapper.cpp` | D2D WhiteLevelAdjustment + HdrToneMap + ColorMatrix exposure chain; reference math for Reinhard, ACES filmic (Narkowicz 2015), Hable (Uncharted 2) |

### Step 16: MainWindow Wiring ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `MainWindow.xaml.h` | Added all controller members (GraphEvaluator, ToneMapper, EffectGraph, SourceNodeFactory, ShaderEditorController, NodeGraphController, PixelInspectorController), DispatcherQueueTimer, custom effect registration flag |
| `MainWindow.xaml.cpp` | Full initialization: device stack → custom effect registration → node graph binding → pixel inspector init → tone mapper init → default shader template → render timer start. Ctrl+Enter shader compile. Display change callback updates tone mapper. |

### Step 17: Render Loop ✅ (Builds Debug|x64)
| File | Contents |
|------|----------|
| `MainWindow.xaml.cpp` | DispatcherQueueTimer at 16ms (~60 FPS): OnRenderTick → RenderFrame (evaluate graph → tone map → BeginDraw → DrawImage → EndDraw → Present). FPS counter updated every second via steady_clock delta. |

### Step 18: Final Build Verification ✅
All 18 steps build successfully in Debug|x64.

---

## Key Technical Decisions

| # | Decision | Detail |
|---|----------|--------|
| 1 | **C++/WinRT only** | Direct COM to `ID2D1EffectImpl`, `ID2D1DrawTransform`, `ID2D1ComputeTransform`. No C#. |
| 2 | **packages.config NuGet** | Standard for C++/WinRT WinUI 3; `.props`/`.targets` imports in vcxproj |
| 3 | **scRGB FP16 default pipeline** | `DXGI_FORMAT_R16G16B16A16_FLOAT` + `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` |
| 4 | **Configurable PipelineFormat** | Not hardwired; users select sRGB/HDR10/FP32 as needed |
| 5 | **Node-based DAG graph editor** | Primary UI; matches D2D effect graph model |
| 6 | **Live HLSL + D3DCompile** | Hot-reload; `D3DReflect` discovers cbuffers for auto UI |
| 7 | **Win2D interop** | Via `Microsoft.Graphics.Canvas.native.h` — `GetWrappedResource`/`CreateDrawingSession` |
| 8 | **MSIX packaged** | `AppContainerApplication=false`, `runFullTrust` for DirectX device access |
| 9 | **Clean project directory** | `E:\source\ShaderLab` — avoids manifest conflicts from prior workspace |
| 10 | **Kahn's algorithm for topo sort** | Linear O(V+E), cycle detection built in, no recursion |
| 11 | **Windows.Data.Json for serialization** | Zero extra deps; tagged type/value pairs for PropertyValue round-trip fidelity |
| 12 | **Hidden message-only window for WM_DISPLAYCHANGE** | Decouples display monitoring from XAML window; no subclassing needed |
| 13 | **jthread + event for adapter hot-plug** | IDXGIFactory7::RegisterAdaptersChangedEvent fires Win32 event; jthread with stop_token for clean shutdown |
| 14 | **Header-only PipelineFormat with inline constants** | Four formats as inline const globals; AllFormats[] for UI enumeration; RecommendedFormat(caps) ties display detection to default |
| 15 | **CreateSwapChainForComposition + ISwapChainPanelNative** | WinUI 3 SwapChainPanel needs composition swap chain; SetColorSpace1 for HDR; D2D1Bitmap1 wraps back buffer |
| 16 | **Per-node D2D effect cache in GraphEvaluator** | Effects created once, reused across frames; properties re-applied on dirty nodes; GetPropertyIndex maps string keys to D2D indices |
| 17 | **WIC HDR-aware image loading with format-split** | SDR→PBGRA 32bpp, HDR→RGBA Half 64bpp (FP16); Flood source uses CLSID_D2D1Flood; both cached per node ID |
| 18 | **Singleton EffectRegistry with categorized D2D catalog** | 40+ effects, 9 categories; EffectDescriptor stores CLSID/pins/defaults; CreateNode() factory produces EffectNodes; case-insensitive name + CLSID lookup |
| 19 | **ID2D1EffectImpl + ID2D1DrawTransform for custom pixel shaders** | Only InputCount as D2D property; shader bytecode + cbuffer managed host-side; ShaderCompiler wraps D3DCompile + D3DReflect; PackConstantBuffer maps PropertyValue to cbuffer via reflection |
| 20 | **ID2D1ComputeTransform for custom compute shaders** | Mirrors pixel shader pattern; CalculateThreadgroups divides output rect by configurable group size (default 8×8×1); CheckFeatureSupport validates hardware; reuses ShaderCompiler with cs_5_0 target |
| 21 | **ShaderEditorController for live HLSL hot-reload** | Compile-on-demand; D3DReflect auto-discovers cbuffer variables→PropertyValue defaults; error line parsing; default PS/CS templates; view-agnostic (no TextBox dependency) |
| 22 | **Canvas-based NodeGraphController with D2D rendering** | Visual node layout, bezier edges, color-coded headers per NodeType, hit-test nodes/pins, drag/connect/select, pan/zoom. DWrite text. Decoupled from XAML. |
| 23 | **GPU readback via D2D1Bitmap1 for pixel inspection** | 1×1 target bitmap → CPU_READ bitmap → float4 map. scRGB→sRGB/PQ/luminance. Tracked position persists across re-evaluations. |
| 24 | **D2D built-in effects for tone mapping** | WhiteLevelAdjustment + HdrToneMap + ColorMatrix exposure. Five modes. Reference math for Reinhard/ACES/Hable. |
| 25 | **DispatcherQueueTimer at 16ms for render loop** | ~60 FPS: evaluate → tone map → present. FPS counter. Ctrl+Enter compile. Custom effects registered at startup. |

---

## Status
**All 18 steps complete.** The project builds successfully in Debug|x64.

Open `E:\source\ShaderLab\ShaderLab.slnx` in Visual Studio to continue development.
