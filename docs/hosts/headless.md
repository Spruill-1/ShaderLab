# ShaderLabHeadless (Console Host)

`ShaderLabHeadless.exe` is a console host for the engine DLL — a logged-out user can render an `.effectgraph`, sample full-accuracy FP32 pixels, or run a JSON batch script of MCP operations against a graph, all without a WinUI message pump or a swap chain.

```
ShaderLabHeadless --graph PATH --node ID --output IMAGE_PATH [options]
```

`--graph` accepts either form of the container: a **`.effectgraph` ZIP** as the GUI's Save writes it (`graph.json` plus optional `media/`), or a **bare graph JSON**. The form is detected from the PKZIP magic rather than the extension, because `.effectgraph` has historically named both. For an archive, embedded media is extracted to a temp directory, each source node's `media://<name>` token is rewritten to the extracted path, and the directory is deleted when the process exits. In `--script` and `--mcp-session` modes the same loader backs `POST /graph/load-file` (and `POST /graph/save-file` writes either form), so a script can switch graphs or round-trip a package mid-run; see [Graph files](mcp-server.md#graph-files). A load-file replaces the startup graph's extracted directory, which is deleted then rather than at exit.

## Modes

- **Image render** (default). Loads a graph, evaluates two passes, optionally pre-passes through `CLSID_D2D1HdrToneMap`, and writes an image. **The `--output` extension picks the encoder:**
  - `.jxr` / `.wdp` → **JPEG XR**, 64bpp RGBA half, lossless, written straight from the pipeline's linear scRGB with no clamp and no transfer encoding, so values above 1.0 and the negative components that express wide-gamut colour both survive.
  - anything else → **PNG**, 8-bit sRGB-encoded, clamped to [0, 1].
  - `--input-peak-nits N` (default 1000)
  - `--output-peak-nits N` (default 80 = SDR; >80 enables HDR display mode)
  - `--no-tonemap` skips the HdrToneMap pre-pass (raw scRGB → sRGB clamp)
  - `--display-profile` / `--display-sdr-white` / `--display-peak-nits` pin
    the display profile a `Working Space` node reports, instead of reading the
    live monitor — see [Pinning the display for golden images](#pinning-the-display-for-golden-images)
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
  | `gpu-bench` | (internal, needs `--gpu-timing`) re-dirty the sources and draw `nodeId` for `iterations` frames; reports GPU draw / frame / evaluate time (min, median), CPU evaluate and whole-frame time (`cpuFrameMedianMs` includes any synchronous GPU waits such as blocking readbacks), and `computeNodeMedianMs` -- each compute node's own dispatch time. Re-dirtying *every* source means a large graph measures all of it; bench one effect in a minimal graph when you want its cost alone. The GPU is shared: with the GUI or the test suite rendering at the same time, CPU evaluate and compute-node times inflate several-fold while the draw time barely moves, so bench on an otherwise idle GPU. |

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

- **Video export** (`--video PATH --node N`). Renders one node over time and encodes it with an **external ffmpeg** (`--ffmpeg <exe or folder>`, else `PATH`; if neither has it the run fails and says how to supply it). Also reachable from any headless MCP session as the `render_video` tool; the GUI does not register it, because an export holds the render thread for the whole encode.
  - **Fixed timeline.** Frame *n* sets every Clock to `start + n/fps` (`SetClocksToTime`, honouring each Clock's Speed / Loop / Start / Stop) and runs the full evaluation shape with every node dirty, so a slow frame is as correct as a fast one. `--duration` defaults to the longest Clock's span at its Speed. Video-file and live-capture sources are **not** time-stepped yet; the export warns when the graph has one.
  - **`--format hdr10`** (default): BT.2020 primaries, PQ in absolute nits (scRGB 1.0 = 80 nits), limited-range 10-bit 4:2:0, **HEVC Main10** by default (`--codec av1` for SVT-AV1), with BT.2020/PQ signalled in the bitstream and container and **mastering-display + content-light SEI**. `--mastering p3-1000` (default: P3-D65 1000 / 0.005 nits), `working-space` (the graph's Working Space node) or `none`. MaxCLL / MaxFALL are **measured** from the frames -- x265 needs them before the first frame, so that costs one render-only pass -- unless `--max-cll CLL,FALL` supplies them.
  - **`--format sdr`**: BT.709, the sRGB curve on [0, 1], limited-range 8-bit 4:2:0, H.264 by default (`--codec hevc|av1`), tagged BT.709 -- what a screen recorder does.
  - **No tone mapping.** Export the node you want. A tone mapper's output is display-referred (1.0 = SDR white), which HDR10 reads literally as 80 nits; export the HDR node for an HDR deliverable.
  - **Colour conversion is ours, not ffmpeg's**: a GPU compute pass turns the node's FP32 scRGB into finished `p010le` / `nv12` planes with **left-cosited** chroma (`[1 2 1]/4` horizontally on the even column, 2-row vertical average -- the HEVC / H.264 default siting), so ffmpeg's scaler never touches colour. `--crf`, `--preset`, `--lossless` (HEVC / H.264) pass through.
  - `--time T` on the ordinary `--output` / `--pixels` modes renders one frame of the same timeline -- how `Tests/RunVideoExport.ps1` builds its reference.

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

Because the live snapshot is frozen at launch, `POST /display/profile` is
the way to exercise a *specific* display against a graph headlessly — pass
a `custom` object with `sdrWhiteNits` and `peakNits` (the latter is
required) to simulate any SDR content white level, and
`/display/profile/clear` to revert to the launch snapshot. This is the
supported path for validating an HDR→SDR operator against the white level
it will actually meet: on Windows in HDR mode DWM composites SDR content
at the *SDR content brightness* level, not at scRGB 1.0, so SDR white in a
captured frame sits at `SdrWhiteNits / 80` — commonly ~2.5, not 1.0.

### Pinning the display for golden images

`POST /display/profile` only reaches MCP and `--script` callers. The
one-shot render path (`--output` / `--pixels`) has no session to post
into, and since the `runEval` / `RunRender` Working Space pump landed it
reads the **live** primary monitor — so a graph containing a `Working
Space` node renders differently on two machines, and differently on the
same machine after the user drags the Windows *SDR content brightness*
slider. A checked-in reference image captured that way is a test of the
tester's display settings, not of the effect.

Three flags pin it:

| Flag | Effect |
|---|---|
| `--display-profile NAME` | Replace the live profile with a fully-determined preset: `srgb-sdr`, `srgb-270`, `p3-600`, `p3-1000`, `bt2020-1000`, `bt2020-4000`, `adobergb`. An unknown name is a usage error (exit 1) — never a silent fallback to the live display, which would defeat the flag. |
| `--effects-dir DIR` | Register user effects from every `*.json` saved graph in `DIR` (repeatable). Headless never reads the GUI's `%LOCALAPPDATA%\ShaderLab\effects` folder by itself. Loaded effects are listed as `User effect:` lines and failures as `[ShaderLab] user effect not loaded:` lines, both on stderr. See [User Effects](../effects/user-effects.md). |
| `--display-sdr-white N` | Override SDR white level (nits) on top of that preset, or on top of the live profile when no preset is named. |
| `--display-peak-nits N` | Override peak luminance (nits), same layering. Also lowers `MaxFullFrameNits` if it would otherwise exceed the new peak. |

The overrides layer, so `--display-profile p3-1000 --display-sdr-white
100 --display-peak-nits 600` gives P3 primaries with 100/600 luminance
anchors. With no flag the behaviour is unchanged: track the live monitor,
which is what you want for interactive probing.

Measured on one machine (Windows SDR-content-brightness slider at 372
nits), rendering a tone mapper whose `SdrWhiteNits` is bound to a
`Working Space` node:

| run | green channel at (62,29) |
|---|---|
| `--display-sdr-white 80` | 1.000000 (clipped) |
| `--display-sdr-white 200` | 0.558341 |
| `--display-sdr-white 400` | 0.278894 (= 0.558341 x 200/400) |
| *no pin* | 0.300544 (= 0.558341 x 200/**372**) |

The last row is the hazard: the unpinned value is the slider's, so the
same command on a default 200-nit machine writes 0.558341 and the byte
comparison fails. Pin the profile for any image you intend to keep.

`Working Space` nodes are refreshed from the active profile at the top of
every evaluation (`runEval`), before dirty propagation, so a node added
mid-session picks the profile up on the next render and a profile change
reaches its binding consumers in the same pass. Prior to that pump the
engine synced only inside the `/display/profile` routes, and a node added
while a profile was already active kept its 80-nit defaults for the rest
of the session — see the `[1.9.0]` entry in `CHANGELOG.md`.

## Engine-side reuse

The MCP route registry (`RegisterEngineRoutes`) is what backs every host — the GUI window's session, the headless `--mcp-session`, and the headless `--script` mode all register the same routes. The same closures execute against the same engine state — only the sink's `Dispatch` impl differs between hosts. The GUI sink marshals to the render worker thread via `RenderThreadDispatcher::DispatchSync` (post-P7); the headless sink runs the closure inline since the script runner thread is the only consumer. The headless host overrides none of the eight UI event hooks; without a UI to keep in sync, each is a no-op. It does override `OnGraphMediaDirChanged`, handing a `/graph/load-file` package's extracted directory to the guard that deletes it when the graph is replaced or the process exits.

## Smoke coverage

`Tests/RunHeadlessSmoke.ps1` is wired into CI's `bootstrap-smoke` job and runs five checks at every commit boundary:

1. **PNG capture** — render `Tests/fixtures/test_cli_basic.json` node 1 to PNG, verify exit code + valid PNG header.
2. **FP32 pixel readback** — same fixture, `--pixels 0,0,4,4`, verify exact blob size + header bytes.
3. **Script batch** — 7-step script that adds a `Luminance Statistics` node, connects it to the source, reads its `Mean` analysis field, mutates the source's `Luminance` property from 80 to 200, re-renders, and reads `Mean` again. The ratio must be 2.5× — exercises add-node + connect + set-property + dirty propagation + ProcessDeferredCompute + analysis readback end-to-end through the standard graph-node path (no special MCP routes).
4. **JPEG XR HDR round trip** — the fixture at `Luminance = 800` nits puts every in-gamut pixel near 10.0 scRGB. Renders to `.jxr`, asserts the JPEG XR container magic and 64bpp, then decodes the centre pixel's red half and requires it above 1.0 (measured ≈ 10.34 against a 10.345 source). Pins both the encoder and the HDR-implies-no-tonemap rule: before that rule existed this test read 0.99.
5. **`.effectgraph` ZIP container** — wraps the fixture's `graph.json` in a ZIP and renders from it, so the archive path cannot regress to JSON-only.

`Tests/RunVideoExport.ps1` covers `--video` (it needs ffmpeg + ffprobe and prints SKIP without them). It builds a Clock-driven gradient, exports it **losslessly** as HDR10 and SDR, decodes every frame, and compares every luma/chroma code with a CPU conversion of the FP32 `--pixels` render at the same `--time` -- a reference written from the specs, not from the shader. Criteria: every code within 1 (float32 GPU vs double ties), luma unbiased (|mean signed error| < 0.05 code), the last frame matching its own time better than the previous one (an off-by-one-frame timeline fails this), the stream's colour tags and HDR10 SEI, and a clear error for a missing ffmpeg. Measured at introduction: HDR10 worst 1 code / 99.2% luma bit-exact / bias -0.008; SDR 100% bit-exact.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)