# ShaderLabHeadless (Console Host)

`ShaderLabHeadless.exe` is a console host for the engine DLL — a logged-out user can render an `.effectgraph`, sample full-accuracy FP32 pixels, or run a JSON batch script of MCP operations against a graph, all without a WinUI message pump or a swap chain.

```
ShaderLabHeadless --graph PATH --node ID --output IMAGE_PATH [options]
```

`--graph` accepts either form of the container: a **`.effectgraph` ZIP** as the GUI's Save writes it (`graph.json` plus optional `media/`), or a **bare graph JSON**. The form is detected from the PKZIP magic rather than the extension, because `.effectgraph` has historically named both. For an archive, embedded media is extracted to a temp directory, each source node's `media://<name>` token is rewritten to the extracted path, and the directory is deleted when the process exits.

## Modes

- **Image render** (default). Loads a graph, evaluates two passes, optionally pre-passes through `CLSID_D2D1HdrToneMap`, and writes an image. **The `--output` extension picks the encoder:**
  - `.jxr` / `.wdp` → **JPEG XR**, 64bpp RGBA half, lossless, written straight from the pipeline's linear scRGB with no clamp and no transfer encoding, so values above 1.0 and the negative components that express wide-gamut colour both survive.
  - anything else → **PNG**, 8-bit sRGB-encoded, clamped to [0, 1].
  - `--input-peak-nits N` (default 1000)
  - `--output-peak-nits N` (default 80 = SDR; >80 enables HDR display mode)
  - `--no-tonemap` skips the HdrToneMap pre-pass (raw scRGB → sRGB clamp)
  - `--width N` / `--height N` (default 1024×1024)
  - `--adapter warp|default` (CI uses warp)

  > **HDR output implies `--no-tonemap`.** The default tone map targets 80 nits (SDR), so leaving it on would hand the JXR encoder an already-compressed SDR image and store it in an HDR container — the file would be HDR in name only. A `.jxr` / `.wdp` output therefore skips the tone map and says so on stdout. Naming a peak explicitly (`--input-peak-nits` / `--output-peak-nits`) overrides that, because tone mapping *into* an HDR deliverable — 4000-nit source down to a 1000-nit target — is a legitimate request.
  >
  > This matches the GUI: `OutputWindow`'s save flyout offers the same two formats and takes the same FP16-vs-8-bit branch.

- **Pixel-region readback** (`--pixels x,y,w,h`). FP32 RGBA samples from any node, no PNG / tonemap involved. Output extension drives format: `.csv` writes `x,y,r,g,b,a` rows; anything else writes packed binary (`uint32 W` + `uint32 H` header + `float[W*H*4]` row-major). Designed for MCP-driven full-accuracy color sampling and ΔE sweeps.

- **Script batch** (`--script PATH [--script-output PATH]`). Loads a graph then walks an array of MCP-style operations through the engine route registry, accumulating one `{step, method, path, status, body}` entry per operation in a JSON response document (stdout if `--script-output` is omitted). Designed for parameter sweeps where the agent wants 50+ engine queries per session without HTTP round-trip overhead each one.

  Each step is either a raw HTTP shape `{method, path, body}` or one of these shorthand `op` forms:

  | `op` | Maps to |
  |------|---------|
  | `set-property` | `POST /graph/set-property` |
  | `pixel-region` | `POST /render/pixel-region` |
  | `capture-node` | `POST /render/capture-node` |
  | `get-graph` | `GET /graph` |
  | `get-node` | `GET /graph/node/<nodeId>` |
  | `analysis` | `GET /analysis/<nodeId>` |
  | `render` | (internal) force a fresh evaluator pass — barrier between mutations and readbacks |

  Example script (insert a Luminance Statistics node, sweep upstream, read back):

  ```json
  {
    "steps": [
      { "method": "POST", "path": "/graph/add-node", "body": {"effectName":"Luminance Statistics"} },
      { "method": "POST", "path": "/graph/connect", "body": {"srcId":1,"srcPin":0,"dstId":2,"dstPin":0} },
      { "op": "render" },
      { "op": "analysis", "nodeId": 2 },
      { "op": "set-property", "nodeId": 1, "key": "Luminance", "value": 200.0 },
      { "op": "render" },
      { "op": "analysis", "nodeId": 2 }
    ]
  }
  ```

- **MCP session** (`--mcp-session [--session-id GUID] [--session-label NAME] [--pipe BASE]`; stdio-migration Step 6). Loads a graph and registers with the broker hub as a **session**, so a shim-fronted MCP client selects it with `use_session` and drives it through the sealed relay. `initialize` / `tools/list` / `tools/call` / `resources/*` all work with no GUI; requests arrive as sealed channel frames, get routed through the router's `POST /` dispatcher, and the response is sealed back. Tools whose backing route is GUI-only (snapshot/view/gpu/perf/logs) return an `isError` "Tool not available on this host" result. `--session-id` is a persisted per-window GUID (a fresh one is generated when omitted); `--session-label` is what `list_sessions` surfaces (headless labels contain "headless", which the test suite uses to self-skip GUI-only tests). Reconnects to the hub with backoff after a drop. This is what CI's "MCP suite vs headless session" step drives, and the headless half of `RunBrokerSmoke.ps1`. (The Step 3 `--serve` HTTP mode was removed with the rest of the HTTP transport in Step 9.)

## Display capabilities without a window

The headless host has no HWND and runs no `DispatcherQueue`, so it cannot
bind a `DisplayInformation` to a window or receive
`AdvancedColorInfoChanged` events. Instead it calls
`DisplayMonitor::InitializeForPrimaryMonitor()` at startup — a one-shot
`GetForMonitor` snapshot of the primary monitor's real advanced-color
caps (HDR state, luminance, SDR white level, primaries). `get_display_info`
therefore reports genuine values, but they are frozen at launch; display
changes during a headless run are not observed. When no display is
reachable (CI runners, session 0) the snapshot fails silently and
struct-default capabilities are served (SDR, 80-nit white, sRGB).

## Engine-side reuse

The MCP route registry (`RegisterEngineRoutes`) is what backs every host — the GUI window's session, the headless `--mcp-session`, and the headless `--script` mode all register the same routes. The same closures execute against the same engine state — only the sink's `Dispatch` impl differs between hosts. The GUI sink marshals to the render worker thread via `RenderThreadDispatcher::DispatchSync` (post-P7); the headless sink runs the closure inline since the script runner thread is the only consumer. The headless host overrides none of the eight `IEngineCommandSink` event hooks; without a UI to keep in sync, every hook is a no-op.

## Smoke coverage

`Tests/RunHeadlessSmoke.ps1` is wired into CI's `bootstrap-smoke` job and runs five checks at every commit boundary:

1. **PNG capture** — render `Tests/fixtures/test_cli_basic.json` node 1 to PNG, verify exit code + valid PNG header.
2. **FP32 pixel readback** — same fixture, `--pixels 0,0,4,4`, verify exact blob size + header bytes.
3. **Script batch** — 7-step script that adds a `Luminance Statistics` node, connects it to the source, reads its `Mean` analysis field, mutates the source's `Luminance` property from 80 to 200, re-renders, and reads `Mean` again. The ratio must be 2.5× — exercises add-node + connect + set-property + dirty propagation + ProcessDeferredCompute + analysis readback end-to-end through the standard graph-node path (no special MCP routes).
4. **JPEG XR HDR round trip** — the fixture at `Luminance = 800` nits puts every in-gamut pixel near 10.0 scRGB. Renders to `.jxr`, asserts the JPEG XR container magic and 64bpp, then decodes the centre pixel's red half and requires it above 1.0 (measured ≈ 10.34 against a 10.345 source). Pins both the encoder and the HDR-implies-no-tonemap rule: before that rule existed this test read 0.99.
5. **`.effectgraph` ZIP container** — wraps the fixture's `graph.json` in a ZIP and renders from it, so the archive path cannot regress to JSON-only.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)