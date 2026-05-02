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
    $MsixPath = Get-ChildItem -Path $PSScriptRoot -Filter '*.msix' -File -Recurse `
        | Sort-Object FullName | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $MsixPath -or -not (Test-Path $MsixPath)) {
    Write-Host "ERROR: No .msix file found. Pass -MsixPath '<path-to-msix>'." -ForegroundColor Red
    exit 1
}

Write-Host "Installing $MsixPath ..." -ForegroundColor Cyan

# Install dependencies first (VCLibs UWPDesktop, WindowsAppRuntime).
$depsDir = Join-Path (Split-Path $MsixPath -Parent) 'Dependencies'
if (Test-Path $depsDir) {
    $arch = $env:PROCESSOR_ARCHITECTURE.ToLower()
    if ($arch -eq 'amd64') { $arch = 'x64' }
    $archDir = Join-Path $depsDir $arch
    if (Test-Path $archDir) {
        Get-ChildItem -Path $archDir -Filter '*.appx' -File | ForEach-Object {
            Write-Host "  Dependency: $($_.Name)" -ForegroundColor DarkGray
            try { Add-AppxPackage -Path $_.FullName -ErrorAction SilentlyContinue } catch {}
        }
    }
}

# Install the main package. -AllowUnsigned needs Developer Mode.
Add-AppxPackage -Path $MsixPath -AllowUnsigned -ForceApplicationShutdown
Write-Host 'Installed. Launch ShaderLab from the Start menu.' -ForegroundColor Green
