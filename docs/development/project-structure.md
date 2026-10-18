# Project Structure

The engine (graph model, evaluator, effects, the built-in effects, the
EffectGraph API, its tests and their docs) is in its own repository,
[EffectGraph](https://github.com/Spruill-1/EffectGraph); ShaderLab builds on its
`EffectGraph.Engine` package (decision #232, [build](build.md#effectgraph-packages)).
The `Graph/` and `Rendering/` folders here hold only ShaderLab's host pieces.

```
ShaderLab/
├── ShaderLab.slnx                  # Solution file
├── ShaderLab.vcxproj               # WinUI 3 app project (MSIX packaged app; packages the Hub too)
├── ShaderLabSupport.vcxproj        # ShaderLab-only static library: MCP, ExprTk, .effectgraph files (miniz), package routes, plugins, capture sources
├── TestEffectPlugin.vcxproj        # Effect plugin the tests load (Tests/TestPlugin): built against EffectGraph.Native, links no engine
├── pch_host.cpp                    # /Yc source for EffectGraph's pch_engine.h (headless, support library, broker, tests)
├── ShaderLabTests.vcxproj          # The tests (MCP, hosts, support library, host pieces)
├── ShaderLabHeadless.vcxproj       # Console host project (no WinUI dependency)
├── ShaderLabMcpBroker.vcxproj      # MCP broker (hub + stdio shim); no engine link
├── *.vcxproj.filters              # Solution Explorer folders, one per project, mirroring the directories
├── packages.config                 # NuGet package manifest (EffectGraph.Engine + EffectGraph.Native included)
├── nuget.config                    # nuget.org; source mapping pins EffectGraph's packages to it
├── build/
│   ├── EffectGraphPackage.props    # EffectGraphPackageVersion (both EffectGraph packages) + the packages folder
│   └── EffectGraphEngine.targets   # The engine: the EffectGraph.Engine package, or an EffectGraph checkout with /p:EffectGraphSourceDir
├── Package.appxmanifest            # MSIX app identity
├── app.manifest                    # DPI awareness, heap type
├── Version.h                       # ShaderLab's version (EffectGraph's is its Engine/Api/EffectGraphVersion.h)
├── README.md                       # Slim repo intro: install, dev quickstart, doc index
├── CHANGELOG.md                    # Version history
├── .gitmodules                     # third_party submodule pins (exprtk, miniz)
├── .mcp.json                       # MCP client config for contributors (stdio → build-tree ShaderLabMcpBroker --stdio)
│
├── pch.h / pch.cpp                 # App PCH (WinRT, WinUI, D2D, D3D, STL)
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
├── Tests/                          # ShaderLabTests sources, smoke scripts (EffectGraph's tests are in its repository)
│   ├── TestRunner.cpp              # main(): device bring-up, --adapter / --tag / --filter / --list /
│   │                               # --require-fixtures, SL_TEST_SETUP start-up, the registry, the summary
│   │                               # (TestRunner / TestSupport / TestCommon / ShaderTestBench are a copy of EffectGraph's runner)
│   ├── ShaderLabTestSetup.cpp      # SL_TEST_SETUP: registers the support library (ExprTk)
│   ├── TestCommon.h                # TEST() / TEST_EQ() / TIMING_TEST() (failures print case + file:line),
│   │                               # skip counter, and the registry: SL_TEST_CASE(name, tags) defines and
│   │                               # registers a case, SL_REGISTER_TEST wraps an existing function.
│   │                               # Tags: fast gpu video timing mcp math manual last. TIMING_TEST is
│   │                               # asserted on hardware outside CI; SHADERLAB_TIMING_TESTS=1/0 overrides
│   ├── TestSupport.h / .cpp        # Shared D3D11 device, CaseContext() (an EffectGraph context per case; g_dc and
│   │                               # CaseEvaluator() are its), CreateTestContext, FindFixture,
│   │                               # SkipMissingFixture, ScopedTempDir, frame and pattern-source helpers
│   ├── ShaderTestBench.h / .cpp    # D3D11 compute test harness for HLSL math (Bench())
│   ├── Graph/                      # GraphUiTests (snapshot, visibility), GraphLayoutTests
│   ├── Rendering/                  # ExpressionTests (ExprTk), HostLoopTests (pacer, dispatcher, preview view)
│   ├── Video/                      # VideoExportTests, and VideoPlaybackTests.cpp's real-time harnesses (playback rate,
│   │                               # render-loop pacing, controls, seeks, past-the-end black, tick work;
│   │                               # [info] tables; SHADERLAB_VIDEO_MATRIX=full runs every clip x tick rate)
│   ├── Persistence/                # .effectgraph packages
│   ├── Mcp/                        # Router, JSON-RPC, engine routes (also driving an EffectGraph context),
│   │                               # argument readers, session client, frame / crypto / peer identity / channel
│   ├── Hosts/                      # Headless argument parsing, effect plugin discovery, the broker shim's JSON-RPC handling
│   │                               # (compiles ShaderLabHeadless/HeadlessArgs.cpp and the broker's
│   │                               # BrokerPipeIo.cpp / Shim.cpp into the test exe)
│   ├── TestPlugin/                 # TestEffectPlugin's source and its effect (TestPluginGain.json)
│   ├── RunVideoExport.ps1          # headless --video vs a spec-derived CPU reference (needs ffmpeg; SKIPs without)
│   ├── RunTests.ps1                # 40-test MCP integration suite (shim-driven; pins a running session, GUI-only tests self-skip on headless)
│   ├── RunMathTests.ps1            # Builds (vswhere) and runs ShaderLabTests --tag math on WARP
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
├── Graph/                          # ShaderLab's host pieces in the engine's folder names (ShaderLabSupport.lib)
│   ├── ParameterVisibility.h / .cpp # visibleWhen conditions, IsParameterShown (panel and canvas pins)
│   ├── GraphLayout.h / .cpp        # ComputeAutoLayout, canvas columns by dependency depth (pure, tested)
│   ├── GraphUiSnapshot.h / .cpp    # Immutable per-frame value copy of nodes + edges for UI-thread reads (decision #70)
│
├── Rendering/                      # ShaderLab's host pieces in the engine's folder names (App-only ones in the app, the rest ShaderLabSupport.lib)
│   ├── RenderEngine.h / .cpp       # App-only D3D11 device + swap chain; D2D from the EffectGraph context
│   ├── HostRenderSettings.h / .cpp # App-only render-loop policy: unthrottled mode, analysis hint scope, readout presets
│   ├── FrameTimings.h              # App-only: render / UI frame timings and the board that publishes them (/perf, status bar)
│   ├── PreviewRenderStats.h        # The previewed node's re-render rate and GPU time (status bar)
│   ├── FramePacer.h                # Frame deadlines for the throttled render worker
│   ├── RenderThreadDispatcher.h    # Closure queue for UI → render-worker marshalling
│   ├── PreviewViewState.h / .cpp   # The preview's node, view, auto-fit and per-node memory (thread-safe, tested)
│   ├── VideoExport.h / .cpp        # Video export through ffmpeg (POST /render/video, headless --video)
│   ├── FfmpegProcess.h / .cpp      # ffmpeg child process with a drained log pipe (video export)
│
├── Sources/                        # ShaderLabSupport.lib: screen and window capture as EffectGraph live sources (decision #234)
│   ├── CaptureSources.h / .cpp     # RegisterCaptureSources: the two live source kinds + "Desktop Duplication" by name
│   ├── DxgiDuplicationSourceProvider.h / .cpp        # One monitor through IDXGIOutputDuplication
│   ├── WindowsGraphicsCaptureSourceProvider.h / .cpp # A picked window or monitor through Windows.Graphics.Capture
│
├── Support/                        # ShaderLabSupport.lib: ShaderLab's programs only, not EffectGraph (decision #224)
│   ├── ShaderLabSupport.h / .cpp   # RegisterShaderLabSupport: every program calls it at startup
│   ├── ExprTkEvaluator.h / .cpp    # ExprTk behind the Numeric Expression hook (PCH disabled on .cpp)
│   ├── EffectGraphFile.h / .cpp    # .effectgraph zip container (miniz DEFLATE) + embedded media
│   ├── PackageRoutes.h / .cpp      # MCP /graph/load-file and /graph/save-file
│   ├── EffectPlugins.h / .cpp      # Which plugin DLLs ShaderLab loads (package Effects\ folders), and loading them
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
├── Assets/                         # App icons, splash screen
├── third_party/
│   ├── exprtk/                     # submodule: ArashPartow/exprtk (MIT) — exprtk.hpp; Support only
│   ├── miniz/                      # submodule: richgel999/miniz @ 3.1.2 (MIT)
│   └── miniz_export.h              # in-tree shim: empty MINIZ_EXPORT (upstream CMake-generates it)
├── scripts/
│   ├── EnsureDevCert.ps1           # Generates + installs CN=ShaderLab dev cert for F5
│   └── Install.ps1                 # Per-arch unsigned-MSIX installer for end users
├── .github/
│   ├── workflows/
│   │   ├── ci.yml                  # PR / push CI: build-and-test (unit + MCP suite vs headless serve) + clean-clone-smoke
│   │   └── release.yml             # Tagged-release matrix (x64 + ARM64)
│   └── copilot-instructions.md
├── <Platform>\<Config>\ShaderLabSupport\ # Support library output (x64|ARM64 × Debug|Release)
├── <Platform>\<Config>\ShaderLab\         # WinUI app output — deploy from here, never from AppX\
├── <Platform>\<Config>\ShaderLabTests\    # Console test output
├── <Platform>\<Config>\ShaderLabHeadless\ # Console host output
├── <Platform>\<Config>\ShaderLabMcpBroker\ # Broker output
├── <Platform>\<Config>\TestEffectPlugin\  # Test plugin output
└── packages/                       # NuGet packages (restored), incl. EffectGraph.Engine.1.0.0 and EffectGraph.Native.1.0.0
```


---

Back to [docs/](../README.md) • [Repo root](../../README.md)