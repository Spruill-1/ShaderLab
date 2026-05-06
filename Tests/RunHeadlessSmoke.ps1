# Smoke test for ShaderLabHeadless.exe.
#
# Renders the test_cli_basic.json fixture's Gamut Source node to a PNG,
# verifies the binary exits cleanly and produces a valid PNG file, then
# cleans up. Pass = exit code 0 and PNG file exists with the PNG magic
# header (89 50 4E 47 ...). Failure = non-zero exit.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
Push-Location $Repo
try {
    $exe = Join-Path $Repo "$Platform\$Configuration\ShaderLabHeadless\ShaderLabHeadless.exe"
    $fixture = Join-Path $Repo 'Tests\fixtures\test_cli_basic.json'
    $out = Join-Path $env:TEMP "shaderlab_headless_smoke_$([guid]::NewGuid().ToString('N')).png"

    if (-not (Test-Path $exe)) {
        Write-Error "Headless exe not built: $exe"
        exit 1
    }
    if (-not (Test-Path $fixture)) {
        Write-Error "Fixture missing: $fixture"
        exit 1
    }

    Write-Host "Rendering $fixture node 1 -> $out"
    & $exe --graph $fixture --node 1 --output $out --width 128 --height 128 --adapter warp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Headless exited $LASTEXITCODE"
        if (Test-Path $out) { Remove-Item $out }
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $out)) {
        Write-Error "PNG not produced at $out"
        exit 1
    }

    $bytes = [System.IO.File]::ReadAllBytes($out)
    if ($bytes.Length -lt 8 -or
        $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or
        $bytes[2] -ne 0x4E -or $bytes[3] -ne 0x47) {
        Remove-Item $out
        Write-Error "Output file is not a valid PNG (bad magic header)"
        exit 1
    }

    Write-Host "PASS: PNG size $($bytes.Length) bytes, valid header"
    Remove-Item $out
    exit 0
}
finally {
    Pop-Location
}
