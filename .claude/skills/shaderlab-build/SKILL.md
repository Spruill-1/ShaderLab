---
name: shaderlab-build
description: Build and test ShaderLab (x64 or ARM64) — correct MSBuild selection, per-project builds, the test runner, headless probes, and release/MSIX packaging with its signing gotchas. Use when asked to build, compile, run tests, package a release, or when a build fails with PCH/toolset/packaging errors.
---

# Building & testing ShaderLab

Windows only. **x64 and ARM64** both build; outputs land in
`<Platform>\<Config>\<Project>\`. Needs VS 2022 17.8+ (2026 / v18 works), the Windows
App SDK 1.8 workload, Windows SDK 10.0.26100+, and `nuget.exe` on PATH.

Resolve MSBuild rather than hardcoding a path — edition and version differ per machine:

```pwsh
$vs  = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
         -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msb = "$vs\MSBuild\Current\Bin\MSBuild.exe"
# ARM64 HOST ONLY -- the default binary above will fail, see next section:
if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { $msb = "$vs\MSBuild\Current\Bin\arm64\MSBuild.exe" }
```

## On an ARM64 host, use the ARM64 MSBuild

`MSBuild\Current\Bin\MSBuild.exe` is 32-bit and reports `PROCESSOR_ARCHITECTURE=x86`
under emulation, so toolset selection in `Microsoft.Cpp.ToolsetLocation.props` never
matches its ARM64 branch and falls through to the **32-bit** `bin\HostX86\arm64\cl.exe`.
That compiler exhausts its ~3 GB address space on the large generated translation
units and fails with:

```
C3859: Failed to create virtual memory for PCH
C1076: compiler limit: internal heap limit reached
```

`/p:PreferredToolArchitecture=x64` does **not** help — the props file declares
`TreatAsLocalProperty` and demotes it straight back to `x86`. The `arm64\` MSBuild
yields `VCToolArchitecture=NativeARM64` and builds clean.

Also required: the `Microsoft.VisualStudio.Component.UWP.VC.ARM64` component. Without
it the packaged WinUI app has no ARM64 platform and fails with *"The
BaseOutputPath/OutputPath property is not set for project 'ShaderLab.vcxproj'"* —
while the plain desktop projects build fine, so the failure looks partial and
unrelated.

The x64 CI matrix cross-compiles ARM64 from an x64 runner, so neither trap surfaces
there. The `native-arm64` job (`runs-on: windows-11-arm`) exists to catch the first
one, and fails loudly if the arm64 MSBuild is missing rather than falling through.

## Build targets

Build the narrowest project that answers the question — the full solution includes
the packaged app and is much slower.

```pwsh
$plat = 'x64'   # or 'ARM64'

# Tests only (pulls in the engine). The usual inner loop.
& $msb ShaderLabTests.vcxproj    /p:Configuration=Debug /p:Platform=$plat /m /v:m /nologo

# Headless host (engine + console host, no WinUI).
& $msb ShaderLabHeadless.vcxproj /p:Configuration=Debug /p:Platform=$plat /m /v:m /nologo

# Everything, including the MSIX-packaged app.
& $msb ShaderLab.slnx            /p:Configuration=Debug /p:Platform=$plat /m /v:m /nologo
```

First build after a fresh clone: `nuget restore ShaderLab.slnx -SolutionDirectory .`
(packages.config style, not PackageReference). Submodules `exprtk` and `miniz` must be
present — `git submodule update --init --recursive`.

The GUI locks `ShaderLab.exe` / `ShaderLabEngine.dll` in the layout and a lingering
hub locks the broker copy. Kill them before rebuilding:

```pwsh
Get-Process ShaderLab, ShaderLabMcpBroker -ErrorAction SilentlyContinue | Stop-Process -Force
```

## Tests

```pwsh
& ".\$plat\Debug\ShaderLabTests\ShaderLabTests.exe" --adapter warp
```

Ends with `ALL <n> TESTS PASSED`; exit code is the failure count. `--adapter warp`
uses the software rasterizer — no GPU dependency, and what CI runs. Covers the graph
model, evaluator, bindings, bytecode cache, `GraphUiSnapshot`, dispatcher, MCP router
+ JSON-RPC contracts, broker frame codec / crypto / peer identity, GPU-binding
matrices, and the HLSL math bench.

Other suites: `Tests\RunHeadlessSmoke.ps1`, `Tests\RunBrokerSmoke.ps1`,
`Tests\RunCliTests.ps1`, `Tests\RunMathTests.ps1`, and `Tests\RunTests.ps1` (MCP —
see the **shaderlab-run** skill).

## Headless probes

Cheapest way to answer a numeric question — no deploy, no GUI:

```pwsh
$h = ".\$plat\Debug\ShaderLabHeadless\ShaderLabHeadless.exe"

# Render one node. The --output extension picks the encoder: .jxr/.wdp is
# 64bpp half, HDR preserved (and implies --no-tonemap); anything else is
# 8-bit sRGB PNG. --graph takes a .effectgraph ZIP or a bare graph JSON.
& $h --graph Tests\fixtures\test_cli_basic.json --node 3 --output out.png --adapter warp
& $h --graph Tests\fixtures\test_cli_basic.json --node 3 --output out.jxr --adapter warp

# FP32 pixel readback
& $h --graph <graph> --node <id> --pixels --adapter warp

# Batch parameter sweep
& $h --graph <graph> --script sweep.json --script-output result.json --adapter warp
```

Flags: `--width/--height` (default 1024), `--adapter warp|default`,
`--input-peak-nits` / `--output-peak-nits`, `--no-tonemap`,
`--enable/--disable-gpu-bindings`, `--reap-shader-cache` / `--clear-shader-cache`,
and `--mcp-session --pipe <name>`.

## Release / MSIX packaging

```pwsh
& $msb ShaderLab.slnx /p:Configuration=Release /p:Platform=$plat `
   /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly /p:GenerateAppxPackageOnBuild=true
```

Output: `AppPackages\ShaderLab\ShaderLab_<ver>_<Platform>_Test\` (msix + `Dependencies\<arch>\`).

**Unsigned** (what `release.yml` ships): inject the unsigned-namespace OID into
`Package.appxmanifest` (`Publisher="CN=ShaderLab"` →
`…, OID.2.25.311729368913984317654407730594956997722=1"`) and add
`/p:AppxPackageSigningEnabled=false`. Restore the plain manifest afterward **from a
byte-for-byte backup, not `git checkout`** — the working tree may hold uncommitted
manifest edits.

**Signed for local install** (plain manifest, no OID): MSBuild's own signing fails
here (APPX0105/APPX0107 importing `ShaderLab_TemporaryKey.pfx`). Sign manually:

```pwsh
# <thumbprint> = your CN=ShaderLab dev cert in CurrentUser\My. Find it with:
#   Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq 'CN=ShaderLab'
signtool sign /fd SHA256 /sha1 <thumbprint> <msix>
```

`signtool` lives under `packages\Microsoft.Windows.SDK.BuildTools.*\bin\...\{arm64,x64}\`.

Signing gotchas, each of which has cost real time:

- **`0x8007000b` ("SignerSign() failed / unexpected internal error") means Publisher ≠
  cert subject** — not an ARM64 signtool bug. Usually the msix still carries the OID
  Publisher while you sign with the plain `CN=ShaderLab` cert. Check the msix's
  internal `AppxManifest.xml` Publisher first.
- **Incremental packaging can leave a stale msix.** After swapping the manifest
  (OID ↔ plain) MSBuild may not re-pack, so you sign yesterday's package. Force a
  re-pack by moving `<Platform>\Release\ShaderLab\AppxManifest.xml` *and* the stale
  `...Test\*.msix` aside, then rebuild.
- **A signed msix install needs the cert in `LocalMachine\TrustedPeople`** (admin,
  one-time `Import-Certificate`). The dev cert is only in `CurrentUser\TrustedPeople`,
  which suffices for `-Register` under Dev Mode but not for a signed install.
- **Unsigned + non-admin `Install.ps1` fails `0x80073D2B`.** ShaderLab is full-trust
  (both `App` and `Hub` are `Windows.FullTrustApplication`), and an unsigned package
  with executable activations requires an **elevated all-users** install — or a signed
  release. This is a known release-process gap, recorded in `README.md`.

Fastest path into the app to test something: `Add-AppxPackage -Register` (dev-mode, no
signature check) — see the **shaderlab-run** skill.
