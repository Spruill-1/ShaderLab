# Fails when EffectGraph code includes a file ShaderLab owns
# (scripts/ShaderLabOwnedFiles.txt). Until the repos split, the engine and
# ShaderLab share folders, so nothing else stops the dependency coming back.
param([string]$Root = (Split-Path $PSScriptRoot))
$ErrorActionPreference = 'Stop'

$engineDirs = 'Graph', 'Rendering', 'Effects', 'Engine', 'BuiltInEffects', 'EffectGraphDll', 'EffectGraphCompile'
$patterns = Get-Content (Join-Path $PSScriptRoot 'ShaderLabOwnedFiles.txt') |
    Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { $_.Trim() }

function Test-Owned([string]$relative)
{
    foreach ($pattern in $patterns)
    {
        $like = $pattern -replace '\*\*', '*'
        if ($relative -like $like) { return $true }
    }
    return $false
}

$files = foreach ($dir in $engineDirs)
{
    Get-ChildItem (Join-Path $Root $dir) -Recurse -File -Include *.h, *.cpp -ErrorAction SilentlyContinue
}
$files += Get-ChildItem $Root -File -Filter 'pch_engine.*'

$violations = @()
foreach ($file in $files)
{
    $relative = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
    if (Test-Owned $relative) { continue }
    $lineNumber = 0
    foreach ($line in [IO.File]::ReadLines($file.FullName))
    {
        $lineNumber++
        if ($line -notmatch '^\s*#\s*include\s*"([^"]+)"') { continue }
        $spec = $Matches[1]
        foreach ($candidate in @((Join-Path $file.DirectoryName $spec), (Join-Path $Root $spec)))
        {
            $full = [IO.Path]::GetFullPath($candidate)
            if (-not (Test-Path $full -PathType Leaf)) { continue }
            $target = [IO.Path]::GetRelativePath($Root, $full).Replace('\', '/')
            if (Test-Owned $target) { $violations += "${relative}:${lineNumber}: includes $target" }
            break
        }
    }
}

if ($violations)
{
    Write-Host "Engine code includes ShaderLab-owned files:"
    $violations | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "Engine boundary OK ($($files.Count) engine files checked)"
