# Project Structure

```
ShaderLab/
├── ShaderLab.slnx                  # Solution file
├── ShaderLab.vcxproj               # WinUI 3 app project (MSIX packaged app; packages the Hub too)
├── ShaderLabEngine.vcxproj         # Host-agnostic engine, a static library every program links
├── ShaderLabSupport.vcxproj        # ShaderLab-only static library: ExprTk, .effectgraph files (miniz), package routes
├── EffectGraph.vcxproj             # EffectGraph.dll: the engine for outside programs, exporting only EffectGraphDll\EffectGraph.def
├── BuiltInEffects.vcxproj          # Static library of the built-in shader effects' source, for EffectGraphCompile and the tests
├── EffectGraphCompile.vcxproj      # Effect blob / header generator (out-of-tree effects; --builtins)
├── TestEffectPlugin.vcxproj        # Effect plugin the tests load (Tests/TestPlugin): built like a real one, links no engine
├── pch_host.cpp                    # ShaderLab's own /Yc source for pch_engine.h (headless, support library, broker)
├── EffectGraphTests.vcxproj        # The engine's tests (no ShaderLab code)
├── ShaderLabTests.vcxproj          # ShaderLab's tests (MCP, hosts, support library), same runner
├── ShaderLabHeadless.vcxproj       # Console host project (no WinUI dependency)
├── ShaderLabMcpBroker.vcxproj      # MCP broker (hub + stdio shim); no engine link
├── *.vcxproj.filters              # Solution Explorer folders, one per project, mirroring the directories
├── packages.config                 # NuGet package manifest
├── Package.appxmanifest            # MSIX app identity
├── app.manifest                    # DPI awareness, heap type
├── Version.h                       # ShaderLab's version (EffectGraph's is Engine/Api/EffectGraphVersion.h)
├── README.md                       # Slim repo intro: install, dev quickstart, doc index
├── CHANGELOG.md                    # Version history
├── .gitmodules                     # third_party submodule pins (exprtk, miniz)
├── .mcp.json                       # MCP client config for contributors (stdio → build-tree ShaderLabMcpBroker --stdio)
│
├── pch.h / pch.cpp                 # App PCH (WinRT, WinUI, D2D, D3D, STL)
├── pch_engine.h / pch_engine.cpp   # Engine/Test/Headless PCH (WinRT base, D2D, D3D, MF, STL)
├── App.xaml / .h / .cpp            # Application entry point
├── MainWindow.xaml / .h / .cpp     # Main window layout + initialization (~5000 lines)
├── MainWindow.WorkingSpace.cpp     # Display-profile selection + ICC loader + UpdateWorkingSpaceNodes shim
├── MainWindow.GraphFileIo.cpp      # Save/load + miniz embedded-media archive + heartbeat reaper
├── MainWindow.RenderTick.cpp       # OnRenderTick (UI blit + Present) / RenderWorkerLoop + RenderFrameToOffscreen (render worker) / dirty propagation
├── MainWindow.McpRoutes.cpp        # MCP router setup + GuiEngineCommandSink (engine routes on the render worker)
├── MainWindow.McpAppRoutes.cpp     # The GUI's own MCP routes: canvas, preview, GPU adapter (UI / render routes)
├── MainWindow.McpSession.cpp       # This window as a hub session: shim copy, hub activation, status label
├── MainWindow.idl                  # WinRT interface definition
├── EffectDesignerWindow.xaml / .h / .cpp  # Effect Designer modal window
│
├── Engine/EngineBootstrap.h / .cpp # Engine DLL: register D2D effect classes, configure the bytecode cache, ShutdownEngineServices
├── Engine/Utf8.h                   # WideToUtf8 / Utf8ToWide, the one UTF-8 conversion every project uses
├── Mcp/                            # ShaderLabSupport.lib: MCP router + engine routes (not EffectGraph; decision #225)
│   ├── McpRouter.h / .cpp          # Pure route registry (longest-prefix, query split, HasRoute)
│   ├── McpTypes.h                  # Transport-neutral Mcp::Response (+ noReply) + shared JsonEscape/WideToUtf8
│   ├── McpArgs.h                   # Typed tool-argument readers (bool / number / uint / int / string / object / array, Require*) + ToUntypedValue
│   ├── McpJsonWriter.h             # JsonWriter (escaping, commas, non-finite -> null) + {error, code} error bodies
│   ├── McpSerialize.h / .cpp       # Shared JSON vocabulary: NodeToJson, PropertyValueToJson, DisplayProfileToJson, Base64Encode, NodeTypeName, AnalysisFieldToJson, WriteTempPng
│   ├── McpJsonRpc.h / .cpp         # Engine-side JSON-RPC dispatcher: initialize / tools / resources / ping
│   ├── McpToolCatalog.h / .cpp     # Declarative 47-tool table: list JSON + route mapping + arg modes
│   ├── McpFrame.h / .cpp           # Broker wire codec: [len][channelId][seq][body], 64 MB cap
│   ├── McpCrypto.h / .cpp          # P-256 ECDH -> HKDF-SHA256 -> AES-256-GCM via BCrypt
│   ├── McpPeerIdentity.h / .cpp    # Package-family peer identity + binary-pairing policy
│   ├── McpChannel.h / .cpp         # Per-channel SecureChannel: P-256 handshake + AES-GCM seal/open
│   ├── McpSessionClient.h / .cpp   # Registers a session with the hub, serves sealed requests via the router, reports Status() (headless + GUI)
│   ├── McpTimeouts.h               # The MCP timeout ladder (render < DispatchSync < shim < client), static_assert-ordered
│   ├── EngineMcpRoutes.h / .cpp    # IEngineCommandSink + EngineContext + RegisterEngineRoutes
│   ├── Routes/                     # The 28 engine routes, one file per concern
│   │   ├── RouteSupport.h / .cpp   # Shared route helpers + each file's Register* declaration
│   │   ├── CatalogRoutes.cpp       # /registry, /effects, /context
│   │   ├── GraphRoutes.cpp         # /graph, /graph/overview, add / remove node, connect, /graph/apply, /custom-effects
│   │   ├── GraphPatch.h / .cpp     # /graph/apply's patch: parse everything first, apply all or nothing
│   │   ├── PropertyRoutes.cpp      # set-property, bind / unbind property
│   │   ├── EffectRoutes.cpp        # /effect/hlsl, /effect/compile
│   │   ├── ReadbackRoutes.cpp      # pixel-region, /analysis, image-bounds, capture-node
│   │   ├── FileRoutes.cpp          # clear, load, load-file, save-file
│   │   └── DisplayRoutes.cpp       # /display/info, profiles, profile, profile/clear
│
├── Tests/                          # EffectGraphTests + ShaderLabTests sources, smoke scripts
│   │                               # (which cases are ShaderLab's: scripts/ShaderLabOwnedFiles.txt)
│   ├── TestRunner.cpp              # main() for both runners: device bring-up, --adapter / --tag / --filter /
│   │                               # --list / --require-fixtures, SL_TEST_SETUP start-up, the registry, the summary
│   ├── ShaderLabTestSetup.cpp      # ShaderLabTests only: registers the support library (ExprTk)
│   ├── TestCommon.h                # TEST() / TEST_EQ() / TIMING_TEST() (failures print case + file:line),
│   │                               # skip counter, and the registry: SL_TEST_CASE(name, tags) defines and
│   │                               # registers a case, SL_REGISTER_TEST wraps an existing function.
│   │                               # Tags: fast gpu video timing mcp math manual last. TIMING_TEST is
│   │                               # asserted on hardware outside CI; SHADERLAB_TIMING_TESTS=1/0 overrides
│   ├── TestSupport.h / .cpp        # Shared D3D11 device, CaseContext() (an EffectGraph context per case; g_dc and
│   │                               # CaseEvaluator() are its), CreateTestContext, FindFixture,
│   │                               # SkipMissingFixture, ScopedTempDir, frame and pattern-source helpers
│   ├── ShaderTestBench.h / .cpp    # D3D11 compute test harness for HLSL math (Bench())
│   ├── FixtureManifestTests.cpp    # fixtures/manifest.json: committed fixtures present, clip SHA-256s match
│   ├── Graph/                      # Graph model, frame semantics, serialization, bindings, node contracts,
│   │                               # single-owner policies; ShaderLab: GraphUiTests (snapshot, visibility), layout
│   ├── Effects/                    # Effect catalog, statistics, shader compiler + includes,
│   │                               # bytecode cache, variants, user effects (tagged last), and the
│   │                               # ICtCp Tone Map's FP32 output vs a CPU port (GPU-bound and cbuffer)
│   ├── Rendering/                  # Compute through the evaluator, GPU bindings and readback policy,
│   │                               # evaluator helpers, pixel readback + ICC, GPU timer; ShaderLab:
│   │                               # ExpressionTests (ExprTk), HostLoopTests (pacer, dispatcher, preview view)
│   ├── Sources/                    # Image loader, source colour contract, video colour / conversion
│   ├── Video/                      # Video sources and the decoder surface pool; ShaderLab: VideoExportTests,
│   │                               # and VideoPlaybackTests.cpp's real-time harnesses (playback rate,
│   │                               # render-loop pacing, controls, seeks, past-the-end black, tick work;
│   │                               # [info] tables; SHADERLAB_VIDEO_MATRIX=full runs every clip x tick rate)
│   ├── Persistence/                # .effectgraph packages
│   ├── Mcp/                        # Router, JSON-RPC, engine routes (also driving an EffectGraph context),
│   │                               # argument readers, session client, frame / crypto / peer identity / channel
│   ├── Hosts/                      # Headless argument parsing, effect plugin discovery, the broker shim's JSON-RPC handling
│   │                               # (compiles ShaderLabHeadless/HeadlessArgs.cpp and the broker's
│   │                               # BrokerPipeIo.cpp / Shim.cpp into the test exe)
│   ├── Math/                       # HLSL math on the bench (tagged math)
│   │   ├── TransferFunctionTests.cpp  # PQ, HLG, sRGB encode/decode round-trips
│   │   ├── ColorMatrixTests.cpp       # BT.709/2020/P3 matrix round-trips
│   │   ├── MobiusReinhardTests.cpp    # ICtCp tone-map curve invariants
│   │   ├── DeltaETests.cpp            # Sharma reference pairs for CIEDE2000
│   │   └── GamutTests.cpp             # sRGB, xyY, PointInTriangle, OKLab, soft compress, dither
│   ├── RunVideoExport.ps1          # headless --video vs a spec-derived CPU reference (needs ffmpeg; SKIPs without)
│   ├── RunTests.ps1                # 40-test MCP integration suite (shim-driven; pins a running session, GUI-only tests self-skip on headless)
│   ├── RunMathTests.ps1            # Builds (vswhere) and runs both runners --tag math on WARP
│   ├── RunHeadlessSmoke.ps1        # CI smoke (PNG + FP32 pixels + script batch)
│   ├── RunBrokerSmoke.ps1          # CI smoke: hub election, shim protocol, idle exit
│   └── fixtures/                      # manifest.json (every fixture: committed or generated, clip hashes),
│                                      # test_cli_basic.json (golden graph for headless smoke),
│                                      # legacy_contracts.json (a graph saved before node contracts),
│                                      # MakeVideoFixtures.ps1 -> video_*_30f.mp4 (generated with ffmpeg, git-ignored;
│                                      # tests that need them count a skip, a failure under --require-fixtures)
│                                      # and video_index_h264_{180p24,180p30,180p60,720p60,720p60_g600}.mp4 (checked
│                                      # in, < 100 KB each; 10 bars encode each frame's index for the playback
│                                      # harness; _g600 has one keyframe in 10 s, for the seek tests)
│
├── ShaderLabHeadless/              # Console host (docs/hosts/headless.md)
│   ├── Main.cpp                    # wmain + image / --pixels / --video / --mcp-session / cache runs
│   ├── HeadlessArgs.h / .cpp       # Args, the flag table (value parsers + modes that read each flag),
│   │                               # mode selection, ExitCode, usage text
│   ├── HeadlessEngine.h / .cpp     # Device, graph, evaluator, sources, GPU timer, display monitor;
│   │                               # the IEngineCommandSink for script and session modes
│   ├── ScriptRunner.h / .cpp       # --script: shorthand op table, per-step result envelope
│   ├── GpuBench.h / .cpp           # the gpu-bench script op
│   └── ImageOutput.h / .cpp        # PNG / JPEG XR encode, --pixels file formats
│
├── ShaderLabMcpBroker/            # MCP broker binary: --hub relay + --stdio shim (no engine link;
│   │                               # compiles Mcp/Mcp{Frame,Crypto,PeerIdentity,Channel})
│   ├── Main.cpp                    # argument parsing, picks the mode
│   ├── BrokerPipeIo.h / .cpp       # log, pipe names, overlapped framed I/O, control frames, ConnectToHub
│   ├── Hub.h / .cpp                # election, user-only pipe DACL, session registry + channel relay,
│   │                               # connection threads (counted from accept, joined, stopped on exit)
│   └── Shim.h / .cpp               # JSON-RPC method and tool tables, session pinning + handshake,
│                                   # tools/list splice
│
├── Graph/                          # Engine: effect graph data model (files marked ShaderLab build into ShaderLabSupport.lib)
│   ├── NodeType.h                  # NodeType enum
│   ├── PropertyValue.h             # std::variant type for node properties
│   ├── EffectNode.h                # EffectNode struct, ParameterDefinition, AnalysisFieldDef
│   ├── EffectEdge.h                # EffectEdge struct
│   ├── GraphModel.h / .cpp        # DAG, dependencies, topological sort, dirty propagation, demand, JSON
│   ├── ClockModel.h / .cpp         # Clock time semantics: live advance, AutoDuration, fixed timeline, SeekClock / SetClockPlaying
│   ├── PropertyWrite.h / .cpp      # WriteProperty: untyped value -> the property's type (set-property, apply)
│   ├── ParameterModel.h / .cpp     # Parameter range + normalized slider value (canvas sliders)
│   ├── ParameterVisibility.h / .cpp # ShaderLab: visibleWhen conditions, IsParameterShown (panel and canvas pins)
│   ├── GraphEdits.h / .cpp         # EditNodeProperty, paste cloning, Numeric Expression inputs
│   ├── GraphLayout.h / .cpp        # ShaderLab: ComputeAutoLayout, canvas columns by dependency depth (pure, tested)
│   ├── ValueFormat.h / .cpp        # Display text for property values and analysis fields
│   ├── GraphUiSnapshot.h / .cpp    # ShaderLab: immutable per-frame value copy of nodes + edges for UI-thread reads (decision #70)
│   ├── VideoSourceStatus.h         # A video source's playback state, copied out on the render thread
│
├── Rendering/                      # Engine: rendering + analysis (files marked ShaderLab or App-only are not the engine's)
│   ├── DisplayInfo.h               # DisplayCapabilities struct
│   ├── DisplayMonitor.h / .cpp     # WinRT AdvancedColorInfoChanged event + simulated profile
│   ├── DisplayProfile.h            # DisplayProfile struct + preset factories
│   ├── IccProfileParser.h / .cpp   # mscms.dll-based ICC reader
│   ├── PipelineFormat.h            # PipelineFormat struct (scRGB FP16 always)
│   ├── RenderEngine.h / .cpp       # App-only D3D11 device + swap chain; D2D from the EffectGraph context
│   ├── HostRenderSettings.h / .cpp # App-only render-loop policy: unthrottled mode, analysis hint scope, readout presets
│   ├── FrameTimings.h              # App-only: render / UI frame timings and the board that publishes them (/perf, status bar)
│   ├── FramePacer.h                # ShaderLab: frame deadlines for the throttled render worker
│   ├── RenderThreadDispatcher.h    # ShaderLab: closure queue for UI → render-worker marshalling
│   ├── GraphEvaluator.h / .cpp     # Exported facade over Evaluator/EvaluatorCore (one pointer, stable ABI)
│   ├── Evaluator/                  # The evaluator, split by seam (topological-evaluation.md)
│   │   ├── EvaluatorCore.h / .cpp  # The pull-dirty walk, one visitor per node kind, per-node records
│   │   ├── CustomEffectNode.cpp    # Custom D2D shaders: auto-compile, host dimensions, build + cbuffer
│   │   ├── ComputeScheduler.cpp    # D3D11 compute: queue, ProcessDeferredCompute, DispatchViaBridge
│   │   ├── EffectFactory.cpp       # Effect creation, property application, input wiring
│   │   ├── AnalysisReadback.cpp    # Histogram / custom / async readback, lane-3 texture views
│   │   ├── CachePolicy.cpp         # D2D1_PROPERTY_CACHED policy, ReleaseCache, InvalidateNode
│   │   ├── VariantSelector.h / .cpp# Build choice, option precompile, baseline compile, shader memos
│   │   ├── BindingResolver.h / .cpp# Property bindings and their non-finite-safe write-back
│   │   ├── ParameterNodes.h / .cpp # Clock / Expression / Random / constant fields (no device)
│   │   ├── ComputeDispatch.h / .cpp# Compute cbuffer packing, output size, thread groups (no device)
│   │   ├── NodeRuntime.h           # Everything kept per node, in one record
│   │   └── InputPins.h             # Lane-3 pin layout
│   ├── D2DHelpers.h                # ScopedDpi96, ScopedTarget, GetSnappedPixelBounds
│   ├── FrameExecutor.h / .cpp      # The one frame order: Evaluate outside, compute + frozen sweep + draw inside
│   ├── EvaluatorOptions.h          # Per-evaluator behaviour switches (GPU bindings, readback skip, caching, ...)
│   ├── SourceRuntime.h / .cpp      # Per-frame source preparation and offline video settling
│   ├── D3D11ComputeRunner.h / .cpp # Generic D3D11 compute dispatch for user shaders
│   ├── FfmpegProcess.h / .cpp      # ShaderLab: ffmpeg child process with a drained log pipe (video export)
│   ├── PixelReadback.h / .cpp      # Engine helper: FP32 RGBA region readback
│   ├── PixelTrace.h / .cpp         # TracePixel: one pixel at a node and every node upstream; HDR10 via BT.2020
│   ├── PreviewViewState.h / .cpp   # ShaderLab: the preview's node, view, auto-fit and per-node memory (thread-safe, tested)
│   ├── CaptureNode.h / .cpp        # Engine helper: PNG of any node's output (on ImageCapture)
│   ├── ImageCapture.h / .cpp       # Engine helper: resolve a node's image, 96-DPI bounds, RenderToPixels, PNG / JXR encode
│   ├── DeviceStack.h / .cpp        # Engine helper: the D3D11 + D2D device stack every host creates (adapter choice, WARP fallback)
│   ├── GpuBindingPlan.h / .cpp     # PlanGpuBindings: which bindings a consumer's GPU lane serves (decision #141)
│   ├── WorkingSpaceSync.h / .cpp   # Engine helper: refresh Working Space parameter nodes from a DisplayProfile
│   ├── MathExpression.h / .cpp     # Numeric Expression hook: dispatches to the evaluator a host registers
│
├── Support/                        # ShaderLabSupport.lib: ShaderLab's programs only, not EffectGraph (decision #224)
│   ├── ShaderLabSupport.h / .cpp   # RegisterShaderLabSupport: every program calls it at startup
│   ├── ExprTkEvaluator.h / .cpp    # ExprTk behind the Numeric Expression hook (PCH disabled on .cpp)
│   ├── EffectGraphFile.h / .cpp    # .effectgraph zip container (miniz DEFLATE) + embedded media
│   ├── PackageRoutes.h / .cpp      # MCP /graph/load-file and /graph/save-file
│   ├── EffectPlugins.h / .cpp      # Which plugin DLLs ShaderLab loads (package Effects\ folders), and loading them
│
├── Effects/                        # Engine: built-in effect wrappers + custom effect base
│   ├── ShaderLabEffects.h / .cpp   # Effect registry; the 8 built-ins with no shader; queued headers register on first use
│   ├── BuiltInEffects.h / .cpp     # Every built-in's name and id, class ids, header names, the not-included message
│   ├── BuiltInEffectHooks.cpp      # The C++ three built-ins carry beyond their HLSL, attached by effect id
│   ├── GamutLutGeometry.h          # The gamut boundary table's size, shared by the LUT effect and shaderlab_gamut.hlsli
│   ├── ColorMath.cpp               # Shared HLSL color math library, served as shaderlab_colormath.hlsli
│   ├── ColorMathCpu.h              # CPU port of the ICtCp path, for derived-constant tables (tested against the HLSL)
│   ├── PropertyMetadata.h          # Effect property metadata for UI generation
│   ├── ImageLoader.h / .cpp        # WIC image loading to linear FP16 scRGB, with a stage + HRESULT on failure
│   ├── SourceNodeFactory.h / .cpp  # Source node creation + per-node runtime: one Prepare per kind, per-frame ticks
│   ├── SourceTextureUtil.h / .cpp  # Source colour contract (AsLinearScRgb) + shared capture output
│   ├── NodeCatalog.h / .cpp        # CreateNodeByName: the one name -> node lookup (add-node, apply)
│   ├── EffectUpgrade.h / .cpp      # NewerDescriptorFor, UpgradeNode, UpgradeOutdatedNodes (effect versioning)
│   ├── ShaderVariants.h / .cpp     # Variant policy: MakeVariantRequest, BuildVariantMacros, PrecompileBindingShapes (decision #142)
│   ├── HlslScaffold.h / .cpp       # GenerateEffectScaffold: the Effect Designer's starting HLSL (every scaffold compiles; tested)
│   ├── EffectRegistry.h / .cpp     # 40+ built-in D2D effect registrations (9 categories)
│   ├── ShaderCompiler.h / .cpp     # D3DCompile + D3DReflect wrapper
│   ├── CustomEffectBase.h / .cpp   # CRTP COM + registration base for the custom D2D effects
│   ├── CustomPixelShaderEffect.h / .cpp     # ID2D1EffectImpl + ID2D1DrawTransform for user pixel shaders
│   ├── CustomComputeShaderEffect.h / .cpp   # ID2D1EffectImpl + ID2D1ComputeTransform for user D2D compute
│   ├── CustomComputeBridgeEffect.h / .cpp   # D2D wrapper for D3D11 compute
│   ├── BytecodeCache.h / .cpp      # Compile-once bytecode store + disk cache; lazy workers, explicit Shutdown
│   ├── ShaderLabParamsHlsl.h / .cpp # Engine-embedded shaderlab_params.hlsli macro library
│   ├── Performance.h / .cpp        # Process-default EvaluatorOptions + telemetry counters
│   ├── IEngineComputeOutput.h      # COM interface for compute effects exposing GPU-resident SRVs
│   ├── DxgiDuplicationSourceProvider.h / .cpp        # Live-capture provider for DXGI Desktop Duplication
│   ├── VideoSourceProvider.h / .cpp                  # Video source: decode thread, frame queue, seek state, D2D bitmap
│   ├── VideoFrameReader.h / .cpp                     # Media Foundation reader, stream colour description, sample copy
│   ├── VideoSeekModel.h / .cpp                       # Decode/seek costs and the forward-seek decision
│   ├── VideoFrameConverter.h / .cpp                  # Decoded frame -> linear scRGB FP16 (one shader template)
│   └── WindowsGraphicsCaptureSourceProvider.h / .cpp # Live-capture provider for the WinUI graphics-capture picker (app-side)
│
├── Controls/                       # App-only: UI controllers (decoupled from XAML views)
│   ├── OutputWindow.h / .cpp           # Per-Output-node OS window (independent SwapChainPanel)
│   ├── OutputSinkRenderState.h         # What an output window shares with the worker: published SinkFrame, closed flag
│   ├── OutputSinkRenderer.h / .cpp     # Render thread: draws a node into its sink's free buffer and publishes it
│   ├── NodeGraphController.h / .cpp    # Canvas editor: visuals, selection, drags, hit-tests and the graph edits they make
│   ├── NodeLayout.h / .cpp             # NodeVisual (pins by PinKind, inline controls), ComputeNodeVisual, data edges
│   ├── NodeGraphRenderer.h / .cpp      # Canvas paint: cached brushes and geometries, one method per part of a node
│   ├── GraphCanvasInputController.h / .cpp # Canvas pointer and keyboard routing, clipboard
│   ├── PreviewController.h / .cpp      # Preview pan / zoom / double-click fit / click-to-trace over Rendering::PreviewViewState
│   ├── PixelTracePanelController.h / .cpp # The Pixel Trace tab's rows and the crosshair
│   ├── LogWindow.h / .cpp              # Per-node log overlay
│   ├── CompileProgressController.h / .cpp # "Recompiling shaders" modal + background variant status
│   ├── DialogCoordinator.h / .cpp      # One ContentDialog at a time: ShowAsync + the background modal that steps aside
│   ├── McpRouteTable.h / .cpp          # Registers the GUI's MCP routes as UI / render / any-thread routes
│   ├── PerfMcpRoutes.h / .cpp          # /perf and the /perf/* measurement switches
│   ├── PropertiesPanelController.h / .cpp # The Properties panel: sections built from the snapshot, edits dispatched
│   ├── PropertyEditors.h / .cpp        # One editor builder per PropertyValue type (toggle, combo, slider, vector, matrix, curve)
│   ├── CurveEditorDialog.h / .cpp      # Curve (LUT) editor dialog and CurveShape::WriteTo
│   ├── McpStatusIndicator.h / .cpp     # MCP activity dot + tooltip; the session thread records requests
│   └── NodeLog.h                       # NodeLog data structure
│
├── Shaders/                        # HLSL source files (user shaders)
├── Assets/                         # App icons, splash screen
├── third_party/
│   ├── exprtk/                     # submodule: ArashPartow/exprtk (MIT) — exprtk.hpp; Support only
│   ├── miniz/                      # submodule: richgel999/miniz @ 3.1.2 (MIT)
│   └── miniz_export.h              # in-tree shim: empty MINIZ_EXPORT (upstream CMake-generates it)
├── scripts/
│   ├── EnsureDevCert.ps1           # Generates + installs CN=ShaderLab dev cert for F5
│   ├── Install.ps1                 # Per-arch unsigned-MSIX installer for end users
│   ├── ShaderLabOwnedFiles.txt     # ShaderLab's files inside the engine's folders (and the repo-split path list)
│   ├── Check-EngineBoundary.ps1    # CI: fails if engine code includes a ShaderLab-owned file
│   └── Test-SplitBuild.ps1         # Builds ShaderLab's files alone against the EffectGraph.Engine package
├── .github/
│   ├── workflows/
│   │   ├── ci.yml                  # PR / push CI: build-and-test (unit + MCP suite vs headless serve) + clean-clone-smoke
│   │   └── release.yml             # Tagged-release matrix (x64 + ARM64)
│   └── copilot-instructions.md
├── <Platform>\<Config>\ShaderLabEngine\   # Engine library output (x64|ARM64 × Debug|Release)
├── <Platform>\<Config>\ShaderLabEngine.Package\ # The same built for EffectGraph.Engine (no /GL, /Z7)
├── <Platform>\<Config>\EffectGraph\       # EffectGraph.dll, EffectGraph.lib and include\ (the public header + built-in effect headers)
├── <Platform>\<Config>\EffectGraphBuiltIns\ # Generated built-in effect headers + their compile cache
├── <Platform>\<Config>\ShaderLab\         # WinUI app output — deploy from here, never from AppX\
├── <Platform>\<Config>\ShaderLabTests\    # Console test output
├── <Platform>\<Config>\ShaderLabHeadless\ # Console host output
└── packages/                       # NuGet packages (restored)
```


---

Back to [docs/](../README.md) • [Repo root](../../README.md)