# Downloads the miniz amalgamation (single-file miniz.c + miniz.h by Rich
# Geldreich) into third_party\miniz\ on first build. miniz is licensed
# under the MIT license. Skipped if the files are already present.
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $TargetDir
)

$ErrorActionPreference = 'Stop'

$cTarget = Join-Path $TargetDir 'miniz.c'
$hTarget = Join-Path $TargetDir 'miniz.h'
if ((Test-Path -LiteralPath $cTarget) -and (Test-Path -LiteralPath $hTarget)) {
	exit 0
}

New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null

# Pinned release: 3.0.2 ships the amalgamated single-file build
# (miniz.c + miniz.h) in the release zip's root directory.
$version = '3.0.2'
$url = "https://github.com/richgel999/miniz/releases/download/$version/miniz-$version.zip"
$tempZip = Join-Path ([System.IO.Path]::GetTempPath()) "miniz-$version.zip"
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) "miniz-$version-extract"

Write-Host "Downloading miniz $version from $url ..."
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $tempZip -UseBasicParsing

if (Test-Path -LiteralPath $tempDir) { Remove-Item -Recurse -Force -LiteralPath $tempDir }
Expand-Archive -LiteralPath $tempZip -DestinationPath $tempDir -Force

# The release zip layout puts miniz.c / miniz.h either at the root or
# under a versioned folder. Find them recursively to be tolerant.
$cSrc = Get-ChildItem -Path $tempDir -Recurse -Filter 'miniz.c' | Select-Object -First 1
$hSrc = Get-ChildItem -Path $tempDir -Recurse -Filter 'miniz.h' | Select-Object -First 1
if (-not $cSrc -or -not $hSrc) {
	throw "miniz.c / miniz.h not found inside $tempZip"
}

Copy-Item -LiteralPath $cSrc.FullName -Destination $cTarget -Force
Copy-Item -LiteralPath $hSrc.FullName -Destination $hTarget -Force

Remove-Item -Recurse -Force -LiteralPath $tempDir
Remove-Item -Force -LiteralPath $tempZip

Write-Host "Saved $cTarget and $hTarget"
