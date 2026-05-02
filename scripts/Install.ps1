<#
.SYNOPSIS
    Installs ShaderLab from an unsigned MSIX package. Requires Developer Mode.

.DESCRIPTION
    Calls Add-AppxPackage with -AllowUnsigned, which lets Windows install an
    unsigned MSIX when Developer Mode is on (Windows 10 1903+ / Windows 11).
    No code-signing certificate is required.

.PARAMETER MsixPath
    Path to the .msix file. If omitted, looks for the first .msix next to this script.
#>
[CmdletBinding()]
param(
    [string] $MsixPath
)

$ErrorActionPreference = 'Stop'

# Verify Developer Mode is enabled.
$devKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
$devMode = (Get-ItemProperty -Path $devKey -Name AllowDevelopmentWithoutDevLicense -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
if ($devMode -ne 1) {
    Write-Host 'ERROR: Windows Developer Mode is not enabled.' -ForegroundColor Red
    Write-Host 'Enable it: Settings -> Privacy & security -> For developers -> Developer Mode.'
    exit 1
}

if (-not $MsixPath) {
    # Pick the ShaderLab package, not Microsoft.* dependency .msix files that
    # may also live next to this script. Exclude anything starting with
    # "Microsoft." and prefer files containing "ShaderLab".
    $candidates = Get-ChildItem -Path $PSScriptRoot -Filter '*.msix' -File `
        | Where-Object { $_.Name -notlike 'Microsoft.*' }
    $shaderLab = $candidates | Where-Object { $_.Name -like '*ShaderLab*' } | Select-Object -First 1
    if ($shaderLab) {
        $MsixPath = $shaderLab.FullName
    }
    elseif ($candidates) {
        $MsixPath = ($candidates | Sort-Object FullName | Select-Object -First 1).FullName
    }
}
if (-not $MsixPath -or -not (Test-Path $MsixPath)) {
    Write-Host "ERROR: No .msix file found. Pass -MsixPath '<path-to-msix>'." -ForegroundColor Red
    exit 1
}

Write-Host "Installing $MsixPath ..." -ForegroundColor Cyan

# Determine current architecture (PowerShell maps amd64 -> x64 for MSIX naming).
$arch = $env:PROCESSOR_ARCHITECTURE.ToLower()
if ($arch -eq 'amd64') { $arch = 'x64' }

function Install-Dependency($file) {
    # Skip wrong-architecture packages by filename heuristic
    # (VS sideload packages embed the arch in the filename, e.g.
    # Microsoft.WindowsAppRuntime.1.8_8000.1099.354.0_arm64__8wekyb3d8bbwe.msix).
    $lower = $file.Name.ToLower()
    foreach ($otherArch in @('x86','x64','arm64')) {
        if ($otherArch -ne $arch -and $lower -match "[_\.]$otherArch([_\.]|$)") {
            return  # skip
        }
    }
    Write-Host "  Dependency: $($file.Name)" -ForegroundColor DarkGray
    try { Add-AppxPackage -Path $file.FullName -ErrorAction SilentlyContinue } catch {}
}

# Top-level Microsoft.* dependency packages (some release zips put them next to Install.ps1).
$mainName = [System.IO.Path]::GetFileName($MsixPath)
Get-ChildItem -Path (Join-Path $PSScriptRoot '*') -Include '*.appx','*.msix' -File |
    Where-Object { $_.Name -ne $mainName -and $_.Name -like 'Microsoft.*' } |
    ForEach-Object { Install-Dependency $_ }

# Install dependencies bundled by VS sideload under Dependencies\<arch>\.
$depsDir = Join-Path (Split-Path $MsixPath -Parent) 'Dependencies'
if (Test-Path $depsDir) {
    $archDir = Join-Path $depsDir $arch
    if (Test-Path $archDir) {
        Get-ChildItem -Path (Join-Path $archDir '*') -Include '*.appx','*.msix' -File |
            ForEach-Object { Install-Dependency $_ }
    }
}

# Install the main package. -AllowUnsigned needs Developer Mode.
Add-AppxPackage -Path $MsixPath -AllowUnsigned -ForceApplicationShutdown
Write-Host 'Installed. Launch ShaderLab from the Start menu.' -ForegroundColor Green
