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

- **`ProcessDeferredCompute` must run inside an active `BeginDraw`/`EndDraw`.** It
  calls `dc->DrawImage` internally; outside a draw session that silently no-ops and
  the compute reads black — Min/Max/Mean all 0, with no error anywhere.
- **D2D→D3D11 texture handoff needs an explicit `dc->Flush()`** between `DrawImage`
  and any D3D11 read, or D3D11 reads zeros.
- **New D2D custom effects need two evaluation passes** before output is correct.
- **`D3DCOMPILE_WARNINGS_ARE_ERRORS` optimizes out cbuffer vars** not referenced on
  *all* paths — read every cbuffer var at the top of `main()` before branching.
- **D2D `TEXCOORD` is pixel/scene space, not normalized [0,1].**
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
`m_`-prefixed, methods/types PascalCase. COM members are `winrt::com_ptr<T>`; custom
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
