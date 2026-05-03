# Downloads exprtk.hpp (single-header math expression library by Arash
# Partow) into third_party\exprtk\ on first build. The header is licensed
# under the MIT license. Skipped if the file is already present.
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $TargetDir
)

$ErrorActionPreference = 'Stop'

$target = Join-Path $TargetDir 'exprtk.hpp'
if (Test-Path -LiteralPath $target) {
	exit 0
}

New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null

$url = 'https://raw.githubusercontent.com/ArashPartow/exprtk/master/exprtk.hpp'
Write-Host "Downloading exprtk.hpp from $url ..."
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing
Write-Host "Saved $target"
