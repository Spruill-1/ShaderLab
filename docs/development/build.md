# Build Instructions

ShaderLab builds on [EffectGraph](https://github.com/Spruill-1/EffectGraph), its
engine, which lives in its own repository and arrives as NuGet packages
(decisions #231, #232). Building the engine itself is covered by EffectGraph's
[build instructions](https://github.com/Spruill-1/EffectGraph/blob/main/docs/development/build.md).

## Prerequisites
- Visual Studio 2022 17.8+ **or** Visual Studio 2026 Insiders (with C++ Desktop and UWP workloads)
- Windows App SDK 1.8
- Windows 10 SDK (10.0.26100+)
- PowerShell 5.1+ (for the dev-cert pre-build step)
- Git (the native dependencies are submodules)
- `nuget.exe` on PATH
- EffectGraph's packages, `EffectGraph.Engine` and `EffectGraph.Native` (below)

## Clone

The two native third-party dependencies are git submodules, so clone
recursively:

```pwsh
git clone --recurse-submodules https://github.com/<owner>/ShaderLab.git
```

On an existing clone, or after pulling a change that moves a submodule
pointer:

```pwsh
git submodule update --init --recursive
```

If you skip this, `ShaderLabSupport.vcxproj`'s `VerifySubmodules` target
fails the build with that exact command rather than emitting a wall of
missing-header errors.

## EffectGraph packages

`packages.config` lists `EffectGraph.Engine` and `EffectGraph.Native`;
`build\EffectGraphPackage.props` pins their version
(`EffectGraphPackageVersion`, currently `1.0.0`), and the two must change
together.

- **`EffectGraph.Engine`** is the static engine library (`EffectGraphEngine.lib`),
  its headers (including `pch_engine.h`) and the built-in effect headers.
  `build\EffectGraphEngine.targets` imports the restored package's targets into
  every ShaderLab project; `ShaderLabSupport` and `ShaderLabMcpBroker` take only
  the headers (`EffectGraphEngineHeadersOnly`).
- **`EffectGraph.Native`** supplies the headers and `EffectGraphCompile` that
  `TestEffectPlugin.vcxproj` builds its effect with, as a real plugin would.

Both come from nuget.org. `nuget.config` maps EffectGraph's package IDs to
nuget.org only; any other package may also come from a source your own
NuGet.Config adds, provided that config maps it (a source with no mapping is
not used):

```pwsh
nuget restore ShaderLab.slnx -SolutionDirectory .
```

The restore fills `packages\` with everything, EffectGraph's packages included.
A project built without the package fails with the restore command rather than
missing headers.

### Building against an EffectGraph checkout

To change the engine and ShaderLab together, build against the checkout's
source instead of the package:

```pwsh
& $msb ShaderLab.slnx /p:Configuration=Debug /p:Platform=x64 /p:EffectGraphSourceDir=..\EffectGraph\
```

`EffectGraphSourceDir` (with a trailing backslash) makes
`build\EffectGraphEngine.targets` reference the checkout's
`EffectGraphEngine.vcxproj` and import its `build\EffectGraphBuiltIns.targets`,
which generates the built-in effect headers. `EffectGraph.Native` is still
restored from the feed for `TestEffectPlugin`.

## Native Dependencies (submodules)

| Path | Upstream | Pin | License | Used by |
|---|---|---|---|---|
| `third_party/exprtk` | [ArashPartow/exprtk](https://github.com/ArashPartow/exprtk) | commit `1e4a80b` | MIT | `Support/ExprTkEvaluator.cpp` — Numeric Expression node |
| `third_party/miniz` | [richgel999/miniz](https://github.com/richgel999/miniz) | tag `3.1.2` | MIT | `Support/EffectGraphFile.cpp` — `.effectgraph` ZIP DEFLATE |

Both build only into `ShaderLabSupport.lib`, which ShaderLab's programs link
and EffectGraph does not (decision #224), so EffectGraph can be MIT-0 while
the ShaderLab programs carry these two MIT notices (`THIRD-PARTY-NOTICES.txt`,
also packaged into the MSIX). The `VerifySubmodules` check lives in
`ShaderLabSupport.vcxproj` with them.

`third_party/miniz_export.h` is a **3-line in-tree shim**, not part of the
submodule. Upstream's `miniz.h` includes `miniz_export.h`, which their
CMake generates via `generate_export_header()`; miniz's own amalgamation
step substitutes an empty `#define MINIZ_EXPORT` instead. We do the same,
so consuming the submodule doesn't drag a CMake toolchain into an
otherwise MSBuild-only repo. miniz links statically into the support
library and its symbols are never exported, so the empty macro is correct.

Only three of miniz's split sources are compiled — `miniz.c`,
`miniz_tdef.c`, `miniz_tinfl.c`. `miniz_zip.c` is deliberately omitted:
`EffectGraphFile.cpp` writes the ZIP container itself and uses only raw
DEFLATE (`tdefl_compress_mem_to_heap`, `tinfl_decompress_mem_to_heap`,
`mz_free`).

## Build
1. Restore (above), then open `ShaderLab.slnx` in Visual Studio. The solution
   holds `ShaderLabSupport`, `ShaderLabTests`, `ShaderLabHeadless`,
   `TestEffectPlugin`, `ShaderLabMcpBroker` and `ShaderLab`.
2. `scripts\EnsureDevCert.ps1` runs automatically on first build to generate
   and install the local `CN=ShaderLab` F5 dev cert.
3. Build configurations:
   - `Debug | x64`, `Release | x64`
   - `Debug | ARM64`, `Release | ARM64`

### Building ARM64 *on* an ARM64 host

CI cross-compiles ARM64 from an x64 runner, so neither of these surfaces there. Both
bite when building ARM64 natively on an ARM64 machine, and both fail in misleading ways:

- **Install the `Microsoft.VisualStudio.Component.UWP.VC.ARM64` component.** Without
  it, `MSBuild\Microsoft\VC\<ver>\Application Type\Windows Store\10.0\Platforms\`
  contains only `Win32` and `x64`, so the packaged WinUI app has no ARM64 platform.
  `OutDir` then falls back to a managed default and the build fails with *"The
  BaseOutputPath/OutputPath property is not set for project 'ShaderLab.vcxproj'"*. The
  plain desktop projects build fine, so the failure looks partial and unrelated.
- **Invoke the ARM64 MSBuild** — `MSBuild\Current\Bin\arm64\MSBuild.exe`, not
  `MSBuild\Current\Bin\MSBuild.exe`. The default binary is 32-bit and reports
  `PROCESSOR_ARCHITECTURE=x86` under emulation, so the toolset selection in
  `Microsoft.Cpp.ToolsetLocation.props` never matches its ARM64 branch and falls
  through to the **32-bit** `bin\HostX86\arm64\cl.exe`. That compiler exhausts its
  ~3 GB address space on the large generated translation units and fails with
  `C3859: Failed to create virtual memory for PCH` + `C1076: internal heap limit
  reached`. Passing `/p:PreferredToolArchitecture=x64` does **not** help — that props
  file declares `TreatAsLocalProperty` and demotes the value straight back to `x86`.
  Using the ARM64 MSBuild yields `VCToolArchitecture=NativeARM64` and the whole
  solution builds clean.

### Outputs (per arch)

- `x64\Debug\ShaderLabSupport\ShaderLabSupport.lib` (linked into every ShaderLab program)
- `x64\Debug\ShaderLab\ShaderLab.exe`
- `x64\Debug\ShaderLabTests\ShaderLabTests.exe` (the tests)
- `x64\Debug\ShaderLabHeadless\ShaderLabHeadless.exe`
- `x64\Debug\ShaderLabMcpBroker\ShaderLabMcpBroker.exe` (also copied into the app layout + MSIX payload as the `Hub` application)
- `x64\Debug\TestEffectPlugin\` (the plugin the discovery test loads)

### Updating a dependency

```pwsh
cd third_party/miniz
git fetch --tags
git checkout <new-tag>
cd ../..
git add third_party/miniz
git commit -m "Bump miniz to <new-tag>"
```

A new EffectGraph version: change `EffectGraphPackageVersion` in
`build\EffectGraphPackage.props` and both EffectGraph entries in
`packages.config`, then restore.

## Releases

A `v*` tag builds, signs and publishes the MSIX; see [releasing](releasing.md).

## Required Libraries

Linked through each project's `AdditionalDependencies`, except where a source file pulls its own in with `#pragma comment(lib, ...)`. The engine library comes from `EffectGraph.Engine`; a program that links it needs the system libraries below.

| Library | Purpose | Linked by |
|---------|---------|-----------|
| `d3d11.lib` | Direct3D 11 device and context | app, headless, tests |
| `d2d1.lib` | Direct2D rendering and effects | app, headless, tests |
| `dxgi.lib` | DXGI swap chain, HDR output queries | app, headless, tests |
| `d3dcompiler.lib` | Runtime HLSL compilation (D3DCompile) | app, headless, tests |
| `dxguid.lib` | DirectX GUIDs (IID_ID2D1Factory, etc.) | app, headless, tests |
| `windowscodecs.lib` | WIC image loading and PNG / JPEG XR encode | app, headless, tests |
| `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib` | Media Foundation video source decoding | app, headless, tests |
| `windowsapp.lib` | WinRT umbrella | broker; the app through the WinUI defaults |
| `bcrypt.lib` | MCP session crypto (`Mcp/McpCrypto.cpp`) | tests, and every binary compiling `McpCrypto.cpp` |
| `shell32.lib`, `CoreMessaging.lib` | Known folders; the system `DispatcherQueue` `DisplayInformation` needs | app (`App.xaml.cpp`, `MainWindow.xaml.cpp`) |

---


---

Back to [docs/](../README.md) • [Repo root](../../README.md)
