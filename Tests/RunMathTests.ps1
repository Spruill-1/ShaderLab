# Runs the HLSL math cases of ShaderLabTests.exe (--tag math) on WARP, so it
# works without a GPU; -Tag '' runs the whole suite (EffectGraph's own math
# tests are in its repository). Builds a runner first when it is missing. Exits with the failure
# count, so non-zero means a suite is red.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [string]$Tag = 'math'
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
Push-Location $Repo
try {
    $failures = 0
    foreach ($runner in 'ShaderLabTests') {
        $exe = Join-Path $Repo "$Platform\$Configuration\$runner\$runner.exe"
        if (-not (Test-Path $exe)) {
            Write-Host "$runner not built; building now..."
            $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installerswhere.exe"
            $vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
            if (-not $vs) { throw 'No Visual Studio with MSBuild found (vswhere).' }
            # The 32-bit MSBuild picks a 32-bit compiler on an ARM64 host, which
            # runs out of address space on the big translation units.
            $hostArm64 = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64'
            $msbuild = if ($hostArm64) { "$vs\MSBuild\Current\Binrm64\MSBuild.exe" } else { "$vs\MSBuild\Current\Bin\MSBuild.exe" }
            & $msbuild "$runner.vcxproj" /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal /m /nologo
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }

        $testArgs = @('--adapter', 'warp')
        if ($Tag) { $testArgs += @('--tag', $Tag) }
        & $exe @testArgs
        $failures += $LASTEXITCODE
    }
    exit $failures
}
finally {
    Pop-Location
}
