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

    # ---- Pixel-region readback mode (FP32 RGBA) ----------------------------
    # Validates the engine-side Rendering::ReadPixelRegion helper used by
    # MCP read_pixel_region. Output format: 8-byte header (uint32 W,
    # uint32 H, little-endian) then floats[W*H*4].
    $bin = Join-Path $env:TEMP "shaderlab_headless_pixels_$([guid]::NewGuid().ToString('N')).bin"
    Write-Host "Pixel readback: 4x4 region from node 1 -> $bin"
    & $exe --graph $fixture --node 1 --pixels 0,0,4,4 --output $bin --adapter warp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Pixel readback exited $LASTEXITCODE"
        if (Test-Path $bin) { Remove-Item $bin }
        exit $LASTEXITCODE
    }
    if (-not (Test-Path $bin)) {
        Write-Error "Pixel binary not produced at $bin"
        exit 1
    }
    $pbytes = [System.IO.File]::ReadAllBytes($bin)
    $expectedSize = 8 + 4 * 4 * 4 * 4  # header + W*H*RGBA*4-byte-floats
    if ($pbytes.Length -ne $expectedSize) {
        Remove-Item $bin
        Write-Error "Pixel blob size $($pbytes.Length) != expected $expectedSize"
        exit 1
    }
    $w = [BitConverter]::ToUInt32($pbytes, 0)
    $h = [BitConverter]::ToUInt32($pbytes, 4)
    if ($w -ne 4 -or $h -ne 4) {
        Remove-Item $bin
        Write-Error "Pixel header W=$w H=$h, expected W=4 H=4"
        exit 1
    }
    Write-Host "PASS: pixel blob $($pbytes.Length) bytes, W=$w H=$h"
    Remove-Item $bin

    exit 0
}
finally {
    Pop-Location
}
