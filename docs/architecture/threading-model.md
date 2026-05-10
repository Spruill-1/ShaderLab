# Threading Model

ShaderLab uses a two-thread architecture: a **UI thread** (XAML STA) for input,
layout, and composition, and a **render worker** (`std::jthread` running MTA) for
all D3D11/D2D graph evaluation. They communicate through a closure-based
dispatcher and a double-buffered offscreen blit.

```mermaid
sequenceDiagram
    participant UI as UI thread (STA)
    participant Disp as RenderThreadDispatcher
    participant W as Render worker (MTA)
    participant Swap as SwapChainPanel back buffer
    Note over UI: User interacts<br/>(timer tick, input, MCP route)
    UI->>Disp: DispatchSync(closure)
    Disp->>W: queue
    W->>W: drain queue → run closure
    W->>W: graph eval + BeginDraw/EndDraw → offscreen[N]
    W->>Disp: signal published(idx, version)
    UI->>UI: OnRenderTick: BlitOffscreenToSwapChain
    UI->>Swap: DrawImage(offscreen[idx]) + Present1
```

## Why two threads

Graph evaluation can take 50-100 ms per frame on heavy 4K HDR chains. Doing
that on the UI dispatcher starved input event delivery — dropdown highlights,
hover, and clicks visibly stalled. Decoupling makes the UI Present cost
sub-millisecond regardless of graph eval throughput.

## Why offscreen-blit (not direct Present-from-render-thread)

`IDXGISwapChain1::Present1` on a chain bound to a XAML `SwapChainPanel` throws
`RPC_E_WRONG_THREAD` from a render-thread MTA, even with multi-threaded D2D and
D3D11 `MultithreadProtected`. The XAML composition integration path is
STA-bound. The render thread therefore writes to offscreen `ID3D11Texture2D`
buffers; the UI thread blits the latest published buffer onto the
SwapChainPanel-bound swap chain.

## Resources

| Resource | Owned by | Notes |
|---|---|---|
| `m_d3dDevice`, `m_d3dContext` | `RenderEngine` | D3D11 immediate context with `ID3D10Multithread::SetMultithreadProtected(TRUE)`. Both threads call into it. |
| Multi-threaded D2D device | `RenderEngine` | Single device, two contexts. |
| `m_d2dDeviceContext` | UI thread | Editor canvas, blit-to-swap-chain, file-save capture. |
| `m_renderD2dContext` | Render thread | `BeginDraw` → graph eval → `EndDraw` per tick. |
| `m_swapChain` | Bound to UI's `SwapChainPanel`. UI thread Presents. |
| `m_offscreenTextures[2]` + `m_offscreenRenderTargets[2]` | Render thread writes; UI thread reads via `m_offscreenSourceBitmapUi[2]` (UI-context wrappers). |
| `m_graph` | Logical single writer/reader on render thread. UI mutations route through `RenderThreadDispatcher::DispatchSync` (worker drains the queue between iterations, so a closure runs while the worker is implicitly paused). |
| `m_outputWindows` (XAML) | UI thread |
| `m_outputSinks` (`OutputSinkRenderState`) | shared_ptr<>; UI thread mutates the vector under `m_outputSinksMutex`; render thread snapshots and reads. |

## Dispatcher

`Rendering::RenderThreadDispatcher` is a closure queue with one consumer (the
worker) and many producers (UI thread, MCP server thread). `DispatchSync`
blocks until the closure has run and returns its result. Re-entrant calls
from inside the consumer thread run inline (the dispatcher detects this with
a thread-local flag).

## MCP routes

`MainWindow::GuiEngineCommandSink::Dispatch` (the engine sink the GUI host
provides to `EngineMcpRoutes`) marshals the closure to the render thread.
Mutating routes (`/graph/add-node`, `/graph/set-property`, …) and readback
routes (`/render/capture-node`, `/render/pixel-region`, `/render/pixel-trace`)
all run on the worker. Engine context (`graph`, `dc`, `d3dDevice`, etc.)
points at render-thread resources.

## Output windows (P12)

Each Output node owns an `OutputSinkRenderState` (shared between UI thread's
`OutputWindow` and render thread's `m_outputSinks` snapshot). The render
worker writes into the sink's offscreen at the source image's native size
(stateless about the panel's display size or DPI). The UI thread does the
fit-to-panel transform (`Scale(zoom)*Translation(pan)` with
`SetDpi(96 * compositionScale)`) when blitting into the window's swap chain
back buffer.

A buffer-generation handshake (`bufferGen` vs `uiObservedGen`) lets the UI
rebuild its source-bitmap wrappers when the worker recreates the offscreen
on a size change, without locking on the read path.

## Pixel trace + capture (P13)

Pixel inspection / pixel trace / per-node capture all walk `m_graph` and
read pixel values from `cachedOutput` via `DrawImage` to a 1×1 staging
texture. They're routed through the render dispatcher so `m_graph` is stable
for the duration of the readback.

## What stays on UI thread

- XAML mutations (Properties panel rebuilds, node-graph canvas, FPS counter,
  flyouts).
- Editor `m_graphSwapChain` Present (the editor canvas renders cheaply,
  sub-ms — kept on UI for simplicity).
- File pickers, dialogs.
- Settings / per-monitor profile UI.

## Adapter switch

UI thread orchestrates: stop the worker → `m_renderDispatcher.Shutdown()` →
`m_renderEngine.Shutdown()` → recreate engine on new adapter → restart
worker. MCP routes return 503 during the swap (the dispatcher is shut down).

## Two-phase shutdown

`MainWindow::~MainWindow`:
1. `m_isShuttingDown = true` (UI thread guard for in-flight callbacks)
2. Stop MCP server (no more incoming routes)
3. Stop UI render timer
4. `m_renderDispatcher.Shutdown()` (cancels pending closures)
5. Stop + join render worker
6. Release output windows, offscreen wrappers, evaluator cache, display monitor, render engine

---

Back to [docs/](../README.md) • [Repo root](../../README.md)
