# Build Instructions

## Prerequisites
- Visual Studio 2022 17.8+ **or** Visual Studio 2026 Insiders (with C++ Desktop and UWP workloads)
- Windows App SDK 1.8
- Windows 10 SDK (10.0.26100+)
- PowerShell 5.1+ (for the dev-cert pre-build step)
- Git (the native dependencies are submodules)

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

If you skip this, `ShaderLabEngine.vcxproj`'s `VerifySubmodules` target
fails the build with that exact command rather than emitting a wall of
missing-header errors.

## Native Dependencies (submodules)

| Path | Upstream | Pin | License | Used by |
|---|---|---|---|---|
| `third_party/exprtk` | [ArashPartow/exprtk](https://github.com/ArashPartow/exprtk) | commit `1e4a80b` | MIT | `Rendering/MathExpression.cpp` — Numeric Expression node |
| `third_party/miniz` | [richgel999/miniz](https://github.com/richgel999/miniz) | tag `3.1.2` | MIT | `Rendering/EffectGraphFile.cpp` — `.effectgraph` ZIP DEFLATE |

`third_party/miniz_export.h` is a **3-line in-tree shim**, not part of the
submodule. Upstream's `miniz.h` includes `miniz_export.h`, which their
CMake generates via `generate_export_header()`; miniz's own amalgamation
step substitutes an empty `#define MINIZ_EXPORT` instead. We do the same,
so consuming the submodule doesn't drag a CMake toolchain into an
otherwise MSBuild-only repo. miniz links statically into
`ShaderLabEngine.dll` and its symbols are never re-exported, so the empty
macro is correct.

Only three of miniz's split sources are compiled — `miniz.c`,
`miniz_tdef.c`, `miniz_tinfl.c`. `miniz_zip.c` is deliberately omitted:
`EffectGraphFile.cpp` writes the ZIP container itself and uses only raw
DEFLATE (`tdefl_compress_mem_to_heap`, `tinfl_decompress_mem_to_heap`,
`mz_free`).

## Build
1. Open `ShaderLab.slnx` in Visual Studio.
2. `scripts\EnsureDevCert.ps1` runs automatically on first build to generate
   and install the local `CN=ShaderLab` F5 dev cert.
3. NuGet packages restore automatically.
4. Build configurations:
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
5. Outputs (per arch):
   - `x64\Debug\ShaderLabEngine\ShaderLabEngine.dll`
   - `x64\Debug\ShaderLab\ShaderLab.exe`
   - `x64\Debug\ShaderLabTests\ShaderLabTests.exe`

### Updating a dependency

```pwsh
cd third_party/miniz
git fetch --tags
git checkout <new-tag>
cd ../..
git add third_party/miniz
git commit -m "Bump miniz to <new-tag>"
```

## Releases

GitHub Actions workflow `.github/workflows/release.yml` runs as a matrix (`x64`, `ARM64`). Just before MSBuild, the workflow injects the unsigned-namespace OID into the manifest's `Publisher` so that the resulting MSIX is installable via `Add-AppxPackage -AllowUnsigned`. The in-repo `Package.appxmanifest` keeps the plain `CN=ShaderLab` publisher so signed F5 deploys keep working.

## Required Libraries (linked via vcxproj)

| Library | Purpose |
|---------|---------|
| `d3d11.lib` | Direct3D 11 device and context |
| `d2d1.lib` | Direct2D rendering and effects |
| `dxgi.lib` | DXGI swap chain, HDR output queries |
| `d3dcompiler.lib` | Runtime HLSL compilation (D3DCompile) |
| `dxguid.lib` | DirectX GUIDs (IID_ID2D1Factory, etc.) |
| `windowscodecs.lib` | WIC image loading |
| `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib` | Media Foundation video source decoding |
| `mscms.lib` | ICC profile reading (Image Color Management) |
| `windowsapp.lib` | Windows Graphics Capture interop (CreateDirect3D11DeviceFromDXGIDevice) |

---


---

Back to [docs/](../README.md) • [Repo root](../../README.md)