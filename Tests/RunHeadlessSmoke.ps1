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

    # ---- Script batch mode (--script) -------------------------------------
    # Validates the standard MCP workflow for ad-hoc analysis: insert a
    # Luminance Statistics node, connect upstream, evaluate, read fields,
    # mutate upstream parameter, re-evaluate, read fields again. The 2.5x
    # luminance set-property invariant catches:
    #   * graph mutation routing (add-node, connect, set-property)
    #   * ProcessDeferredCompute dispatching D3D11 compute analysis nodes
    #   * dirty propagation through the evaluator
    #   * /analysis/{id} returning the freshly-populated typed fields
    #   * shorthand op -> route translation in RunScript
    #
    # Fixture has Gamut Source as node id=1 and nextId=2, so the new
    # Luminance Statistics node will be id=2 deterministically.
    $scriptText = @'
{
  "steps": [
    { "method": "POST", "path": "/graph/add-node", "body": {"effectName":"Luminance Statistics"} },
    { "method": "POST", "path": "/graph/connect", "body": {"srcId":1,"srcPin":0,"dstId":2,"dstPin":0} },
    { "op": "render" },
    { "op": "analysis", "nodeId": 2 },
    { "op": "set-property", "nodeId": 1, "key": "Luminance", "value": 200.0 },
    { "op": "render" },
    { "op": "analysis", "nodeId": 2 }
  ]
}
'@
    $scriptPath = Join-Path $env:TEMP "shaderlab_smoke_script_$([guid]::NewGuid().ToString('N')).json"
    $scriptOut  = Join-Path $env:TEMP "shaderlab_smoke_script_out_$([guid]::NewGuid().ToString('N')).json"
    Set-Content -Path $scriptPath -Value $scriptText -Encoding UTF8
    Write-Host "Script batch: 7 steps -> $scriptOut"
    & $exe --graph $fixture --script $scriptPath --script-output $scriptOut --adapter warp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Script mode exited $LASTEXITCODE"
        if (Test-Path $scriptPath) { Remove-Item $scriptPath }
        if (Test-Path $scriptOut)  { Remove-Item $scriptOut }
        exit $LASTEXITCODE
    }
    if (-not (Test-Path $scriptOut)) {
        Write-Error "Script output not produced at $scriptOut"
        exit 1
    }
    $doc = Get-Content $scriptOut -Raw | ConvertFrom-Json
    if ($doc.stepCount -ne 7 -or $doc.results.Count -ne 7) {
        Remove-Item $scriptPath, $scriptOut
        Write-Error "Script result count $($doc.results.Count) != 7"
        exit 1
    }
    foreach ($i in 0..6) {
        if ($doc.results[$i].status -ne 200) {
            Remove-Item $scriptPath, $scriptOut
            Write-Error "Step $i did not return 200 (got $($doc.results[$i].status))"
            exit 1
        }
    }
    # Pull Mean field from the Luminance Statistics analysis output before
    # and after the set-property. Doubling the source Luminance (80 -> 200)
    # should produce a 2.5x rise in the Mean nit value.
    $meanBefore = ($doc.results[3].body.fields | Where-Object name -eq 'Mean').value[0]
    $meanAfter  = ($doc.results[6].body.fields | Where-Object name -eq 'Mean').value[0]
    if ($meanBefore -le 0 -or $meanAfter -le 0) {
        Remove-Item $scriptPath, $scriptOut
        Write-Error "Mean values were zero (before=$meanBefore, after=$meanAfter)"
        exit 1
    }
    $ratio = $meanAfter / $meanBefore
    if ([math]::Abs($ratio - 2.5) -gt 0.05) {
        Remove-Item $scriptPath, $scriptOut
        Write-Error "Mean ratio $ratio not ~2.5 (set-property -> Luminance Statistics invariant broken)"
        exit 1
    }
    Write-Host "PASS: script batch ratio $('{0:N3}' -f $ratio) ~ 2.5 (graph-node analysis end-to-end)"
    Remove-Item $scriptPath, $scriptOut

    # ---- JPEG XR (HDR-preserving) output ---------------------------------
    # The fixture's Gamut Source emits Luminance nits / 80 as scRGB, so at
    # 800 nits every in-gamut pixel lands near 10.0 -- far above the [0,1]
    # PNG clamps to. A .jxr output must carry those values through: that is
    # the entire reason the encoder exists, and it also pins the rule that
    # HDR output skips the default SDR HdrToneMap.
    $hdrGraph = Join-Path $env:TEMP "shaderlab_smoke_hdr_$([guid]::NewGuid().ToString('N')).json"
    $g = Get-Content $fixture -Raw | ConvertFrom-Json
    foreach ($p in $g.nodes[0].properties) {
        if ($p.name -eq 'Luminance')  { $p.value = 800.0 }
        if ($p.name -eq 'OutputSize') { $p.value = 256.0 }
    }
    $g | ConvertTo-Json -Depth 64 | Set-Content $hdrGraph -Encoding UTF8

    $jxrOut = Join-Path $env:TEMP "shaderlab_smoke_$([guid]::NewGuid().ToString('N')).jxr"
    & $exe --graph $hdrGraph --node 1 --output $jxrOut --width 256 --height 256 --adapter warp
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $hdrGraph, $jxrOut -ErrorAction SilentlyContinue
        Write-Error "JXR render failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
    $jb = [System.IO.File]::ReadAllBytes($jxrOut)
    # JPEG XR container magic: 'II' + 0xBC 0x01.
    if ($jb.Length -lt 4 -or $jb[0] -ne 0x49 -or $jb[1] -ne 0x49 -or $jb[2] -ne 0xBC -or $jb[3] -ne 0x01) {
        Remove-Item $hdrGraph, $jxrOut -ErrorAction SilentlyContinue
        Write-Error "Output is not a JPEG XR file (magic bytes wrong)"
        exit 1
    }

    Add-Type -AssemblyName PresentationCore
    $st = [System.IO.File]::OpenRead($jxrOut)
    $fr = ([System.Windows.Media.Imaging.BitmapDecoder]::Create(
             $st, 'PreservePixelFormat', 'OnLoad')).Frames[0]
    if ($fr.Format.BitsPerPixel -ne 64) {
        $st.Close(); Remove-Item $hdrGraph, $jxrOut -ErrorAction SilentlyContinue
        Write-Error "JXR is $($fr.Format.BitsPerPixel)bpp, expected 64 (RGBA half)"
        exit 1
    }
    $raw = New-Object 'ushort[]' ($fr.PixelWidth * $fr.PixelHeight * 4)
    $fr.CopyPixels($raw, $fr.PixelWidth * 8, 0)
    $st.Close()
    # Decode the red half at the image centre (inside the gamut triangle).
    $i = (128 * $fr.PixelWidth + 128) * 4
    $hb = $raw[$i]; $e = ($hb -shr 10) -band 0x1F; $m = $hb -band 0x3FF
    $red = if ($e -eq 0) { [math]::Pow(2, -14) * ($m / 1024) }
           else          { [math]::Pow(2, $e - 15) * (1 + $m / 1024) }
    Remove-Item $hdrGraph, $jxrOut -ErrorAction SilentlyContinue
    if ($red -lt 2.0) {
        Write-Error "JXR centre red = $red; expected ~10 (HDR clamped or tone mapped away)"
        exit 1
    }
    Write-Host "PASS: JXR 64bpp half, centre red $('{0:N3}' -f $red) > 1.0 (HDR preserved)"

    # ---- .effectgraph ZIP container --------------------------------------
    # The GUI's Save writes a ZIP (graph.json + optional media/). Headless
    # must read that form, not just bare JSON.
    $zipGraph = Join-Path $env:TEMP "shaderlab_smoke_$([guid]::NewGuid().ToString('N')).effectgraph"
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zs = [System.IO.File]::Open($zipGraph, 'Create')
    $za = New-Object System.IO.Compression.ZipArchive($zs, 'Create')
    $entry = $za.CreateEntry('graph.json')
    $sw = New-Object System.IO.StreamWriter($entry.Open())
    $sw.Write((Get-Content $fixture -Raw)); $sw.Dispose()
    $za.Dispose(); $zs.Dispose()

    $zipOut = Join-Path $env:TEMP "shaderlab_smoke_$([guid]::NewGuid().ToString('N')).png"
    & $exe --graph $zipGraph --node 1 --output $zipOut --width 128 --height 128 --adapter warp
    $zipExit = $LASTEXITCODE
    $zipOk = ($zipExit -eq 0) -and (Test-Path $zipOut)
    if ($zipOk) {
        $zb = [System.IO.File]::ReadAllBytes($zipOut)
        $zipOk = $zb.Length -gt 8 -and $zb[0] -eq 0x89 -and $zb[1] -eq 0x50
    }
    Remove-Item $zipGraph, $zipOut -ErrorAction SilentlyContinue
    if (-not $zipOk) {
        Write-Error ".effectgraph ZIP load failed (exit $zipExit) or produced no valid PNG"
        exit 1
    }
    Write-Host "PASS: .effectgraph ZIP container loaded and rendered"

    exit 0
}
finally {
    Pop-Location
}
