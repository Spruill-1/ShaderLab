# ShaderLab

**HDR / WCG / SDR shader effect development & debugging tool**

A WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D shader effects with full HDR and wide color gamut support.

Its engine — the effect graph, evaluator and built-in effects — is [EffectGraph](https://github.com/Spruill-1/EffectGraph), a separate repository that ShaderLab consumes as NuGet packages.

---

## Installing a Release Build

Download the zip for your machine from the GitHub Releases page
(`ShaderLab-<version>-x64.zip` for AMD64 / Intel, `ShaderLab-<version>-arm64.zip`
for ARM64), extract it, and run `Install.ps1`. It installs the Microsoft
packages ShaderLab depends on (VCLibs, Windows App Runtime) for your
architecture, then ShaderLab. ShaderLab requires **Windows 11 22H2 (build 22621)
or later**: the display pipeline uses WinRT `AdvancedColorInfo` through desktop
interop, which first shipped there.

A **signed** release installs per user, with no Developer Mode and no
elevation. An **unsigned** one (a release made before signing was set up, or a
fork's) needs Developer Mode (*Settings → Privacy & security → For developers*)
and an elevated PowerShell: an unsigned full-trust package installs only for all
users (`0x80073D2B` otherwise). `Install.ps1` reads the package's signature and
takes the right path. How releases are built and signed:
[releasing](docs/development/releasing.md).

---

## Local Development

Clone recursively — the two native dependencies (`exprtk`, `miniz`, both MIT) are git submodules under `third_party/`:

```pwsh
git clone --recurse-submodules https://github.com/<owner>/ShaderLab.git
```

On an existing clone: `git submodule update --init --recursive`.

The engine comes from EffectGraph's packages (`EffectGraph.Engine`, `EffectGraph.Native`) on nuget.org. Restore:

```pwsh
nuget restore ShaderLab.slnx -SolutionDirectory .
```

Then open `ShaderLab.slnx` and F5 (Debug | x64, startup project = `ShaderLab`) to deploy and launch the packaged app. MSBuild auto-runs **`scripts/EnsureDevCert.ps1`** on first build to generate a self-signed `CN=ShaderLab` cert and import it into `TrustedPeople` for F5 deploy — the project ships without a code-signing certificate.

If the submodules are missing, the build stops with an actionable error rather than a cascade of missing-header failures.

See [docs/development/build.md](docs/development/build.md) for full prerequisites, configurations, building against an EffectGraph checkout, and the dependency map.

---

## Documentation

The deep technical reference lives under [docs/](docs/README.md), organized by audience.

| Area | Highlights |
|---|---|
| [Architecture](docs/README.md#architecture) | System overview, threading model (UI thread + render worker), engine/host split |
| [Effects](docs/README.md#effects) | Effect Designer, Numeric Expression node |
| [UI / UX](docs/README.md#ui--ux) | Graph editor, multi-output windows, animation, conditional parameter visibility |
| [Hosts](docs/README.md#hosts) | ShaderLabHeadless console host, MCP server for AI agent integration |
| [Development](docs/README.md#development) | Build instructions, project structure |
| [History](docs/README.md#history) | Decision log (architectural choices with rationale) |
| [EffectGraph](docs/README.md#effectgraph-engine--in-its-own-repository) | The engine's docs, in its repository: pipeline format, graph model, evaluation, hybrid compute, built-in catalog, bindings |

[`CHANGELOG.md`](CHANGELOG.md) tracks every release. The [decision log](docs/history/decision-log.md) tracks architectural choices independently of release boundaries.

---

## Project Identity

ShaderLab is a testbed for HDR / WCG / SDR pixel-effect development. The pipeline is always **scRGB FP16 linear light** (`DXGI_FORMAT_R16G16B16A16_FLOAT`, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`) — no pipeline format switching; DWM/ACM handles the final display conversion.

Core capabilities:

- **Live effect graph** with D2D + D3D11 compute custom effects, dirty-gated re-evaluation, JSON serialization (`Windows.Data.Json`).
- **Effect Designer** for authoring custom pixel & compute shaders with live HLSL compile + reflection-driven property generation.
- **Analysis viewers** (Luminance / Channel / Chromaticity Statistics, CIE Histogram + Plot, Gamut Coverage, Luminance Heatmap, etc.) — all share the same compute-bridge architecture and route their outputs as SRVs to downstream consumers when possible.
- **Tone-mapping suite** (D2D `HDR Tone Map`, ICtCp Tone Map, ICtCp Inverse Tone Map, ICtCp Gamut Map, etc.) operating in scRGB FP16 with PQ / HLG / sRGB transfer functions.
- **HDR / WCG aware** — event-driven display tracking (WinRT `AdvancedColorInfoChanged`: HDR toggles, the Windows SDR-brightness slider, monitor moves), ICC profile parsing, monitor primaries piped into Custom-gamut analysis effects via the Working Space node.
- **MCP integration** (stdio, via a broker: shim → hub → per-window sessions) + **headless host** for AI-agent and CI use; the MCP route layer lives in ShaderLab's support library so headless and GUI hosts share the route implementations.

Build: Visual Studio 2022 17.8+, Windows 10 SDK 10.0.26100+, C++/WinRT only (no C#). Runtime: Windows 11 22H2 (10.0.22621)+.

---

## License & Contact

ShaderLab is MIT ([LICENSE](LICENSE)). The EffectGraph engine and its packages are MIT-0 ([its LICENSE](https://github.com/Spruill-1/EffectGraph/blob/main/LICENSE)). Third-party code in ShaderLab's programs: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt). Issues and PRs on the GitHub repository.
