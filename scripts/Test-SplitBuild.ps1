<#
.SYNOPSIS
    Builds ShaderLab the way it will build after the repository split: only
    ShaderLab's files, in an empty folder, against a packed EffectGraph.Engine.

.DESCRIPTION
    Copies what ShaderLab keeps (everything but EffectGraph: the engine folders
    less scripts/ShaderLabOwnedFiles.txt, the engine's projects, bindings,
    packaging, the tests) into -WorkDir, links packages\ and third_party\ in,
    extracts the EffectGraph.Engine package, and builds the GUI, headless and
    broker with EffectGraphEnginePackageDir set. Then renders a fixture with
    that headless and with this repository's, and fails unless the PNGs match.
    Run packaging/Pack.ps1 and build ShaderLabHeadless first.

.PARAMETER Package
    The EffectGraph.Engine .nupkg; defaults to the newest in packaging\out.
#>
param(
    [string]$Package,
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'ShaderLabSplitBuild'),
    [string]$Platform = 'ARM64',
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot

if (-not $Package) {
    $Package = (Get-ChildItem (Join-Path $root 'packaging\out') -Filter 'EffectGraph.Engine.*.nupkg' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $Package -or -not (Test-Path $Package)) { throw 'No EffectGraph.Engine package: run packaging/Pack.ps1.' }

# ---- What ShaderLab keeps.
$engineDirs = 'Graph', 'Rendering', 'Effects', 'Engine', 'BuiltInEffects', 'EffectGraphDll', 'EffectGraphCompile'
$engineFiles = 'pch_engine.h', 'pch_engine.cpp', 'ShaderLabEngine.vcxproj', 'ShaderLabEngine.vcxproj.filters',
    'EffectGraph.vcxproj', 'EffectGraph.vcxproj.filters', 'EffectGraphCompile.vcxproj', 'EffectGraphCompile.vcxproj.filters',
    'BuiltInEffects.vcxproj', 'BuiltInEffects.vcxproj.filters', 'TestEffectPlugin.vcxproj', 'TestEffectPlugin.vcxproj.filters',
    'build/EffectGraph.targets', 'build/EffectGraphBuiltIns.targets'
$engineTrees = 'bindings/', 'packaging/', 'Tests/', 'third_party/'
$owned = Get-Content (Join-Path $PSScriptRoot 'ShaderLabOwnedFiles.txt') |
    Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { $_.Trim() -replace '\*\*', '*' }

function Test-ShaderLabFile([string]$relative)
{
    if ($owned | Where-Object { $relative -like $_ }) { return $true }
    if ($relative -in $engineFiles) { return $false }
    if ($engineTrees | Where-Object { $relative.StartsWith($_) }) { return $false }
    return $relative.Split('/')[0] -notin $engineDirs
}

if (Test-Path $WorkDir) { cmd /c rmdir /s /q "$WorkDir" | Out-Null }
New-Item -ItemType Directory -Path $WorkDir | Out-Null
$files = git -C $root ls-files -co --exclude-standard | Where-Object { Test-ShaderLabFile $_ }
foreach ($file in $files) {
    $source = Join-Path $root $file
    if (-not (Test-Path $source -PathType Leaf)) { continue }
    $target = Join-Path $WorkDir $file
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item $source $target
}
Write-Host "ShaderLab's files: $($files.Count)"
# Restored packages and the submodules, as a clone would have them.
cmd /c mklink /J "$(Join-Path $WorkDir 'packages')" "$(Join-Path $root 'packages')" | Out-Null
cmd /c mklink /J "$(Join-Path $WorkDir 'third_party')" "$(Join-Path $root 'third_party')" | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $WorkDir 'Tests') | Out-Null
Copy-Item -Recurse (Join-Path $root 'Tests\fixtures') (Join-Path $WorkDir 'Tests\fixtures')

# ---- The package, where packages.config would restore it.
$packageDir = Join-Path $WorkDir "EffectGraphPackages\$([IO.Path]::GetFileNameWithoutExtension($Package))"
Expand-Archive -Path $Package -DestinationPath $packageDir

# ---- Build.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
        -requires Microsoft.Component.MSBuild -property installationPath
$msb = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') {
    "$vs\MSBuild\Current\Bin\arm64\MSBuild.exe" } else { "$vs\MSBuild\Current\Bin\amd64\MSBuild.exe" }
foreach ($project in 'ShaderLabHeadless.vcxproj', 'ShaderLabMcpBroker.vcxproj', 'ShaderLab.vcxproj') {
    & $msb (Join-Path $WorkDir $project) /p:Configuration=$Configuration /p:Platform=$Platform `
        "/p:EffectGraphEnginePackageDir=$packageDir\" /p:IgnoreWarnIntDirInTempDetected=true /m /v:m /nologo
    if ($LASTEXITCODE -ne 0) { throw "Split build failed: $project" }
}

# ---- The same render from both headless builds.
$fixture = Join-Path $root 'Tests\fixtures\test_cli_basic.json'
$results = foreach ($exe in (Join-Path $root "$Platform\$Configuration\ShaderLabHeadless\ShaderLabHeadless.exe"),
                            (Join-Path $WorkDir "$Platform\$Configuration\ShaderLabHeadless\ShaderLabHeadless.exe")) {
    $png = [IO.Path]::Combine($WorkDir, [IO.Path]::GetRandomFileName() + '.png')
    & $exe --graph $fixture --node 1 --output $png --adapter warp 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Render failed: $exe" }
    (Get-FileHash $png -Algorithm SHA256).Hash
}
if ($results[0] -ne $results[1]) { throw "Renders differ: source $($results[0]), package $($results[1])" }
Write-Host "Split build OK: the package-built headless renders identically ($($results[0].Substring(0, 16)))"
