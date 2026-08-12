# Engine / Host Split

The codebase is divided between a host-agnostic engine DLL and one or more host applications:

- **`ShaderLabEngine.dll`** owns everything that doesn't need a UI thread or a swap chain: the `EffectGraph` model + JSON serialization, the `GraphEvaluator` (per-node D2D effect cache, dirty propagation, two-pass evaluate), `SourceNodeFactory` (image / video / DXGI / WGC sources), `EffectRegistry` (40+ wrapped D2D effects + 20+ ShaderLab effects with embedded HLSL), `DisplayMonitor` + ICC parsing, the `Effects/CustomPixelShaderEffect` / `CustomComputeShaderEffect` COM classes, the generic D3D11 compute dispatch helper (`Rendering/D3D11ComputeRunner.{h,cpp}`), the `ShaderCompiler` (D3DCompile + D3DReflect), and **the entire MCP protocol surface — the `McpRouter` route registry, JSON-RPC dispatcher, declarative tool catalog, session client, plus all 25 engine-pure routes** (`Engine/Mcp/{McpRouter,McpTypes,McpJsonRpc,McpToolCatalog,McpSessionClient,EngineMcpRoutes}`) and the broker plumbing (`McpFrame` codec, `McpCrypto` ECDH/GCM stack, `McpChannel`, `McpPeerIdentity` pairing). The transport is the broker (shim → hub → session over named pipes) — the embedded HTTP listener was deleted in stdio-migration Step 9. Engine-pure helpers extracted for reuse: `Rendering/PixelReadback.{h,cpp}` (FP32 RGBA region readback), `Rendering/CaptureNode.{h,cpp}` (D2D + WIC PNG encode), `Rendering/WorkingSpaceSync.{h,cpp}` (Working Space parameter node refresh).

- **`ShaderLab.exe`** (the WinUI 3 host) keeps everything that genuinely needs WinUI: `MainWindow.xaml.{h,cpp}` (which itself is split into sibling partial TUs `MainWindow.WorkingSpace.cpp`, `MainWindow.GraphFileIo.cpp`, `MainWindow.RenderTick.cpp`, `MainWindow.McpRoutes.cpp` for the 18 app-side routes), `Controls/NodeGraphController` (canvas rendering), `Controls/OutputWindow` (per-Output OS window), `Controls/ShaderEditorController`, the Effect Designer modal window, and `RenderEngine` (D3D11 + D2D1 device stack, `SwapChainPanel` binding).

- **`ShaderLabHeadless.exe`** (see below) reuses everything from the engine DLL with no WinUI dependency.

- **`ShaderLabMcpBroker.exe`** (stdio-migration Step 5) is the odd one out: it does **not** link the engine at all. The hub must start in milliseconds and never touches a GPU, so it compiles the small transport-only `Engine/Mcp/Mcp{Frame,Crypto,PeerIdentity}` translation units directly into the exe. It runs as the MSIX package's second `<Application Id="Hub">` (hidden, full-trust) and as an unpackaged `--stdio` shim copied out of the package.

## `IEngineCommandSink` event hook architecture

When MCP routes mutate engine state, they fire through an `IEngineCommandSink` (`Engine/Mcp/EngineMcpRoutes.h`) so that:

1. The host marshals the mutation closure to whatever thread is appropriate. The GUI host routes to the **render worker thread** via `RenderThreadDispatcher::DispatchSync` (P7+) so `m_graph` stays single-writer; the headless host runs the closure inline since there is no separate consumer.
2. The host runs **event hooks** afterwards. Hooks may need to touch XAML (which is STA-only), so the GUI sink re-marshals UI work back to the UI thread via `DispatcherQueue().TryEnqueue` from inside the render-thread closure.

The eight hooks are: `OnNodeAdded`, `OnNodeRemoved`, `OnNodeChanged`, `OnGraphCleared`, `OnGraphLoaded`, `OnGraphStructureChanged`, `OnCustomEffectRecompiled`, `OnDisplayProfileChanged`. The GUI's `MainWindow::GuiEngineCommandSink` overrides each one to call the same UI methods that handle native user interactions (`AutoLayout`, `RebuildLayout`, `PopulatePreviewNodeSelector`, `PopulateAddNodeFlyout`, `UpdateStatusBar`, `MarkAllDirty`, `CloseOutputWindow`, `ResetAfterGraphLoad`). The headless host leaves each hook as the default no-op. **Result: an MCP client calling `/graph/add-node` triggers exactly the same downstream UI code path as the user clicking the toolbar.**

See [Threading Model](threading-model.md) for the full UI / render-worker split and the offscreen-blit composition path.

The 16 routes that remain in `MainWindow.McpRoutes.cpp` are intentionally app-side because they are either UI-coupled (`/graph/snapshot`, `/graph/view*`, `/preview/view*`, `/render/preview-node`, `/render/capture`, `/render/pixel-trace`, `/graph/rename-node`, `/gpu/list`, `/gpu/switch`) or host-specific (`/context`, `/perf`, `/node/<id>/logs`). `GET /` (health) and `POST /` (JSON-RPC dispatcher) are engine-registered on both hosts via `RegisterJsonRpcEndpoint` (stdio-migration Step 3).


---

Back to [docs/](../README.md) • [Repo root](../../README.md)