# CLAUDE.md

ShaderLab — a WinUI 3 / C++/WinRT desktop tool for authoring and debugging Direct2D
effects in an HDR / WCG pipeline, driven either by hand or by an AI agent over MCP.
The deep reference is [`docs/`](docs/README.md); this file is the part you need
*before* you read anything.

## Hard rules

- **C++/WinRT only. Never generate C#.** Direct COM access to `ID2D1EffectImpl`,
  `ID2D1DrawTransform`, `ID2D1ComputeTransform` is the reason this project exists.
- **Every new `.cpp` starts with `#include "pch.h"`** (engine-side: `pch_engine.h`).
  Precompiled headers are mandatory; anything else fails to build.
- **Docs are part of the change, not a follow-up.** A significant change updates the
  relevant file under `docs/`, adds a `CHANGELOG.md` entry, and — for a choice whose
  *why* isn't obvious from the code — a row in [`docs/history/decision-log.md`](docs/history/decision-log.md).
  Root `README.md` stays slim (install + build + pointer to `docs/`).
- **Don't widen `MainWindow`.** It is already ~10k LOC across six files with ~89
  member fields. New UI behavior belongs in a `Controls/` controller; new
  host-agnostic behavior belongs in the engine.

## Build, test, run

Windows only. **x64 and ARM64** are both supported; build outputs land in
`<Platform>\<Config>\<Project>\`. Locate MSBuild with `vswhere` rather than assuming
an install path — editions and versions differ per machine:

```pwsh
$vs  = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
         -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msb = "$vs\MSBuild\Current\Bin\MSBuild.exe"           # x64 host
# On an ARM64 HOST, use the arm64-native binary instead -- see the trap below:
# $msb = "$vs\MSBuild\Current\Bin\arm64\MSBuild.exe"

& $msb ShaderLabTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:m /nologo
& '.\x64\Debug\ShaderLabTests\ShaderLabTests.exe' --adapter warp  # ends "ALL <n> TESTS PASSED"
```

> **Trap — building ARM64 *on* an ARM64 host needs the arm64 MSBuild.**
> `MSBuild\Current\Bin\MSBuild.exe` is 32-bit and reports
> `PROCESSOR_ARCHITECTURE=x86` under emulation, so toolset selection falls through to
> the 32-bit `HostX86\arm64\cl.exe`, which exhausts its ~3 GB address space on the big
> translation units and dies with `C3859: Failed to create virtual memory for PCH` +
> `C1076`. `/p:PreferredToolArchitecture=x64` does **not** help (the props file
> declares `TreatAsLocalProperty` and demotes it back). You also need the
> `Microsoft.VisualStudio.Component.UWP.VC.ARM64` component or the packaged app has no
> ARM64 platform. CI cross-compiles ARM64 from an x64 runner, so neither surfaces
> there — the `native-arm64` job exists to catch them. Full detail:
> [`docs/development/build.md`](docs/development/build.md).

> **Trap — the Bash tool reports the wrong architecture on ARM64 hosts.** git-bash runs
> emulated and prints `PROCESSOR_ARCHITECTURE=AMD64`. Use PowerShell to branch on arch.

Fastest inner loops, cheapest first:

| Loop | Command |
|---|---|
| Pure math / shader-bench | build + run `ShaderLabTests.exe --adapter warp` (no GPU, no UI) |
| Headless render / pixel probe | `ARM64\Debug\ShaderLabHeadless\ShaderLabHeadless.exe --graph Tests\fixtures\test_cli_basic.json --node <id> --output out.png --adapter warp` |
| Batch parameter sweep | same exe with `--script <in.json> --script-output <out.json>` |
| Full app + MCP | see the **shaderlab-run** skill — packaged deploy has real gotchas |

Prefer headless over the GUI when a question can be answered numerically: it needs no
deploy, no registration, and gives FP32 readback via `--pixels`.

## The graph-access rule (read before touching `m_graph` from UI code)

The render worker is the **single writer** of the live `EffectGraph` and writes
continuously — clock-node property inserts every tick, plus every MCP mutation.
Getting this wrong is a data race that surfaces as an access violation deep inside
`std::map`, **not** a compile error. Two such crashes shipped before the rule was
written down; both were `node->properties.find()` from the UI thread during paint.

1. **UI-thread reads → the per-frame `GraphUiSnapshot`, never `m_graph`.**
   `MainWindow::CurrentGraphSnapshot()` / `NodeGraphController::Snapshot()`. At most
   one frame stale. Hold the `shared_ptr` for the whole read.
2. **Writes (any thread) → `RenderThreadDispatcher::DispatchSync`.** Never mutate
   `m_graph` from a pointer or interaction handler.
3. **Layout computation** (`RebuildLayout` / `AutoLayout` / `ComputeNodeVisual`) →
   live `m_graph`, **render thread only** (it must see post-mutation state).
   UI callers go through `MainWindow::RunLayoutOnRenderThread`.

**Lock order: `m_graphMutex` → `m_visualsMutex`.** Never hold `m_visualsMutex` across
a `DispatchSync`; don't hold references into `m_visuals` across a dispatch (copy by
value — they dangle if the worker rebuilds layout); don't read `m_visuals` inside a
dispatched closure (it runs on the render thread). Full contract:
[`docs/architecture/threading-model.md`](docs/architecture/threading-model.md).

## Effect-authoring traps that cost real debugging time

The full list — and it is worth reading before writing any D2D effect — is in
[`.github/copilot-instructions.md`](.github/copilot-instructions.md) §*D2D Custom
Effect Gotchas* and [`docs/architecture/d2d-d3d11-hybrid-compute.md`](docs/architecture/d2d-d3d11-hybrid-compute.md).
The ones that bite most often:

- **The draw-session contract is split, and both halves matter.**
  `ProcessDeferredCompute` must run **inside** an active `BeginDraw`/`EndDraw` — it
  calls `dc->DrawImage` internally; outside a draw session that silently no-ops and
  the compute reads black (Min/Max/Mean all 0, no error anywhere). But
  `GraphEvaluator::Evaluate` must run **outside** one: the D2D Histogram and
  custom-analysis readbacks open their *own* `BeginDraw`, and nesting that fails
  with `D2DERR_WRONG_STATE` (`0x88990001`). Any post-dispatch `Evaluate` sweep that
  must happen inside the session runs under `SetDeferredComputeFrozen(true)` so it
  cannot re-queue a compute node (the Histogram defers its read to the next frame).
  `MainWindow::RenderFrameToOffscreen`, headless `runEval`, and headless `RunRender`
  all use this exact shape — copy it, don't improvise. History worth knowing:
  `Evaluate` used to pre-render compute inputs too (`PreRenderInputBitmap`, deleted
  2026-09-25), and getting the split wrong looked like a property-plumbing bug —
  correct on the first frame, then black or **stale** after the first `set-property`.
  Compute inputs are now pre-rendered only inside `ProcessDeferredCompute`, after
  any same-pass producer has dispatched (decision #95).
- **D2D→D3D11 texture handoff needs an explicit `dc->Flush()`** between `DrawImage`
  and any D3D11 read, or D3D11 reads zeros.
- **New D2D custom effects need two evaluation passes** before output is correct.
- **`D3DCOMPILE_WARNINGS_ARE_ERRORS` optimizes out cbuffer vars** not referenced on
  *all* paths — read every cbuffer var at the top of `main()` before branching.
- **A D2D pixel shader MUST declare `SCENE_POSITION`, and `TEXCOORD` is then
  normalized.** The required input signature is
  `main(float4 : SV_POSITION, float4 : SCENE_POSITION, float4 : TEXCOORD0, …)`,
  one TEXCOORD per input. Omit the middle parameter and the binding shifts: what
  you called `TEXCOORD0` receives the **scene** coordinate instead of input 0's
  texel coordinate. Those two coincide only while the content fills D2D's
  intermediate allocation, so the mistake is invisible at the usual source size
  and then displaces the image by `−(intermediate − content)/2` per axis the
  moment it isn't — measured at exactly −128 px for a 256 px source in a 512 px
  intermediate. This file previously claimed "TEXCOORD is pixel/scene space, not
  normalized"; that was a description of the bug, not of D2D. With the signature
  correct, sample with `Sample(s, uv0.xy)`, take pixel coordinates from
  `SCENE_POSITION`, and give each input its **own** TEXCOORD — a two-input effect
  reusing one coordinate for both is how "two-input effects render displaced"
  was born. **And `SCENE_POSITION` is not exact:** D2D interpolates it with up
  to 2^-12 px of error, and the error *moves when D2D re-tiles the effect* --
  which wiring an extra input can trigger. Anything chaotic in it (a dither
  hash) must `floor()` it to the integer pixel first, or the same pixel renders
  differently depending on graph topology (measured: 0.037% of pixels, 1-3 codes).
- **scRGB is signed on purpose.** Negative Rec.709 components are how wide-gamut
  color is expressed (BT.2020 green ≈ `(-0.87, 1.00, 0.06)`). A `max(rgb, 0)` or
  `saturate()` at the top of a color transform is a **gamut clip**, not a safety
  net — this exact bug silently sRGB-clipped the whole ICtCp suite. Use the signed
  PQ helpers in `Effects/ColorMath.cpp`.

**MCP-specific:** untyped (`{}`-schema) tool args arrive from Claude Code as JSON
**strings** — `"203"`, `"true"`. `EngineMcpRoutes.cpp` coerces them to the parameter's
real type in both `/graph/set-property` and `/graph/apply`. Keep that coercion when
touching those paths; without it bindings break, shaders read 0, and clocks freeze.
Diagnostic tell: `graph_get_node` printing `"203"` (quoted = string, broken) vs
`203.000000`.
Adding a tool also requires bumping the exact count asserted by `Rpc_CatalogCount`
in `Tests/TestRunner.cpp`, and `Tests/RunTests.ps1` **clears the graph**, so don't
run it against a session whose graph you still need.

## Looking at HDR output (you cannot, directly)

Your vision input is 8-bit SDR. A captured PNG of an HDR frame has clipped
everything above scRGB 1.0 (80 nits) and lost the negative components that carry
wide-gamut chroma. **Seeing** an HDR image therefore requires tone mapping it — which
in this project is usually the thing under test. Judging a tone mapper from a
tone-mapped screenshot is circular.

So, in order of preference:

1. **Numbers first.** `read_pixel_region` / headless `--pixels` (FP32, unclipped) and
   `read_analysis_output` on a Statistics node. These are ground truth.
2. **Measured difference:** `Delta E Comparator` with `Method = dE ITP (BT.2124)` →
   `Luminance Statistics` → read Mean/p95/Max. Use ITP, not the CIE Lab metrics, for
   anything HDR or wide-gamut — see the effect's catalog entry for why.
3. **Diagnostic renders when you need to *look*.** These encode HDR facts into an
   SDR-visible image, so capturing them is legitimate: `Nit Map` and
   `Luminance Heatmap` (where the energy is), `Gamut Highlight` and
   `CIE Chromaticity Plot` (what is out of gamut), `ICtCp Boundary`,
   `Delta E Comparator` in Heatmap mode (where two images differ).
4. **A raw capture of HDR content** is for composition and gross sanity only.

**Always say which one you used.** "The Nit Map shows the highlights peaking around
1200 nits" is a claim you can support; "the image looks right" after capturing an HDR
node is not — flag that you are looking through a tone map, or that a judgement is a
taste call rather than a measurement. `render_capture` / `render_capture_node` clip to
SDR and their tool descriptions say so.

For a full-range artifact, headless `--output foo.jxr` writes lossless 64bpp-half
JPEG XR with no clamp — but note **you still can't view it**; it is for archiving,
golden-image comparison, and feeding back in as an Image source.

## Layout

Four projects around one host-agnostic engine:

- `ShaderLabEngine.dll` — `Graph/` (model + `GraphUiSnapshot`), `Rendering/`
  (evaluator, `D3D11ComputeRunner`, `DisplayMonitor`, ICC, capture), `Effects/`
  (built-in effect library with embedded HLSL, registry, compiler, sources),
  `Engine/Mcp/` (router, JSON-RPC, tool catalog, engine-pure routes, broker crypto).
  `SHADERLAB_ENGINE_ABI_VERSION` in `EngineExport.h` is bumped **manually** on ABI
  breaks; a mismatch aborts host startup.
- `ShaderLab.exe` — WinUI 3 packaged app: XAML, `Controls/` controllers,
  `RenderEngine`, the render worker, app-side MCP routes.
- `ShaderLabHeadless.exe` — console host, no WinUI. PNG render, FP32 readback,
  script batch, MCP session.
- `ShaderLabMcpBroker.exe` — MCP transport. `--hub` is a blind relay (sealed bodies);
  `--stdio` is the client shim. Does **not** link the engine.
- `ShaderLabTests.exe` — standalone runner, no WinUI, WARP-capable.

Pipeline is **always scRGB FP16 linear light** (1.0 = 80 nits), no format switching,
no built-in tone-mapping pass — tone mappers are composed as graph effects (the ICtCp
suite is the preferred path) and validated empirically with `Delta E Comparator` +
`Luminance Statistics` + `Working Space`.

## Conventions

`ShaderLab::` namespaces mirror directories (`Graph`, `Rendering`, `Effects`,
`Controls`); XAML types live in `winrt::ShaderLab::implementation`. Members are
`m_`-prefixed, methods/types PascalCase, locals descriptive camelCase. Constants take
a `c` prefix, statics `s`, static consts `sc` (`cMaxOptionValues`, `scGamutLutWidth`);
older code still uses `k`, so don't mass-rename it in passing. Comments are short and
plain: what the next block does, or the one-line constraint that would surprise a
reader. Change history and measurements belong in `CHANGELOG.md` and the decision log,
not in code. COM members are `winrt::com_ptr<T>`; custom
D2D effects hand-roll `IUnknown` refcounting on a `LONG m_refCount`. Init paths use
`winrt::check_hresult`; hot paths use `SUCCEEDED`/`FAILED` with early return.

New files go in the matching directory **and** into both `ShaderLab.vcxproj` (or the
right project) and `.vcxproj.filters`.

**Don't add a bare `catch (...) {}`.** Record the failure somewhere a caller can see
it (`LastError()`, `runtimeError`, an MCP status field), or — when swallowing really is
correct (teardown, a best-effort UI indicator) — say so in a comment, so an intentional
swallow is distinguishable from an oversight. Of ~37 catch-alls in the engine
directories, the genuinely silent ones have been fixed or annotated; two cost real
debugging time before that: `DisplayMonitor::Initialize` swallowed a throw and served
SDR defaults on a 4000-nit HDR panel (a missing `Windows.System.DispatcherQueue`, not
an API bug), and `OutputWindow::SaveImageAsync` swallowed every WIC/D2D failure so
"Save image" silently did nothing.

## Verifying work

Claims about color or performance in this project are expected to be **measured**,
not asserted — that standard is visible throughout `CHANGELOG.md` and is the house
style. Before reporting a fix: run the tests, and where the change is numeric, probe
it headless (`--pixels` / `--script`) and quote the numbers.

**Validate the instrument before you believe the number.** A whole measurement
round here once produced a complete, plausible, *wrong* table — twice over, from
two independent silent faults: compute nodes serving stale pixels, and a metric
chain (`Delta E Comparator` → `Luminance Statistics`) that was actually measuring
the luminance of a heatmap, because the comparator is image-output only and has no
analysis fields. Neither was visible in the numbers. So:

- Prove the pipeline is live before sweeping it — change an input, confirm the
  output moves. A sweep that returns identical values at several settings is
  **suspect**, not a finding that "the parameter has no effect".
- Watch stderr. Nested-draw and pre-render failures now print `[ShaderLab]` lines;
  any of them voids that run's numbers.
- Prefer FP32 `pixel-region` readback plus CPU math over reading a diagnostic
  effect's rendered output, and keep the meter off the compute path being tested.

**Performance has its own instrument traps**, and they also read as results. An
ablation here concluded the tone mapper's shader math was 3% of its cost; on
representative HDR content it was **81%** — the first rig used desktop-capture and
synthetic-pattern content, where the gamut early-out fires on nearly every pixel.
Content is part of the instrument. Also:

- **Drive the node dirty every frame or you measure nothing.** D2D output caching
  serves a clean node without re-executing it, which reads as 0.00–0.13 ms rather
  than as "cached". Bind a `Clock` to any property of the node under test; the
  node's `gpuState` (`graph_overview`, or the perf flyout's on-canvas display)
  says which case you are in.
- **Preview zoom decides how much is shaded.** At zoom ≥ fit, D2D clips to the
  visible region; only below fit does it shade the full intermediate.
- **`perf_timings.fps` is `1/totalUs`** — per-frame *work*, not achieved rate
  (~35,000 on an idle throttled loop). For throughput diff `framesSampled` over a
  window with **no MCP traffic inside it**: the worker's `WaitFor` is a cv
  predicate wait, so every MCP call wakes it early and inflates the rate being
  measured — observed 264 Hz against a 62.5 Hz timeout floor.
- **Check that an ablation ablates.** Non-monotonic deltas — removing more work
  saving less — mean the variant didn't take (e.g. `skipGamut = true` on a line a
  later block reassigns). Kill the branch at the `[branch]`, not at its inputs.
- Caps: `render_capture_node` downscales to 2048 px; `read_pixel_region` is
  64×64 / 1024 px **and evaluates only the region asked for**, so neither can
  exercise D2D tiling.
- Ablating a built-in via `effect_compile` takes its source as-is: the route
  resolves `shaderlab_colormath.hlsli` and `shaderlab_params.hlsli` and defines
  the generic-variant macros (every GPU-bindable parameter in cbuffer mode).

A metric harness worth trusting self-checks first (for example, cross-validating
its own CPU ΔE ITP against the shader's) and refuses to report anything if the
instrument fails.

One more shape to watch: **statistics are permutation-invariant**. Min/Max/Mean/
Median/P95 match perfectly across a change that scrambles pixel *positions*, so a
reduction alone cannot validate anything spatial — pair it with scattered
`read_pixel_region` probes or a capture diff.
