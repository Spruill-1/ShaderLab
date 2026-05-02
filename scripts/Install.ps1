<#
.SYNOPSIS
	Installs ShaderLab from a loose-file MSIX layout. No cert required.

.DESCRIPTION
	Registers the AppxManifest.xml directly. Requires Windows Developer Mode
	(Settings → Privacy & security → For developers → Developer Mode).

	This is the same mechanism Visual Studio uses for F5 deploy. No signing
	certificate is needed because Windows treats registered loose-file apps
	as developer-mode sideloads.

.PARAMETER LayoutDir
	Path to the AppX layout directory (containing AppxManifest.xml).
	If omitted, looks for AppxManifest.xml next to this script.
#>
[CmdletBinding()]
param(
	[string] $LayoutDir = $PSScriptRoot
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

$manifest = Join-Path $LayoutDir 'AppxManifest.xml'
if (-not (Test-Path $manifest)) {
	Write-Host "ERROR: AppxManifest.xml not found at $manifest" -ForegroundColor Red
	exit 1
}

Write-Host "Registering ShaderLab from $manifest ..." -ForegroundColor Cyan
Add-AppxPackage -Register $manifest -ForceApplicationShutdown
Write-Host 'Installed. Launch ShaderLab from the Start menu.' -ForegroundColor Green
