<#
.SYNOPSIS
  End-to-end check of headless --video against an independent reference.

.DESCRIPTION
  Builds a small animated graph (a Gradient Generator whose start colour is
  bound to a Clock), exports it LOSSLESSLY as HDR10 (HEVC) and SDR (H.264),
  decodes every frame back with ffmpeg, and compares each luma / chroma code
  with a CPU conversion of the FP32 --pixels render at the same timeline time
  (--time k/fps). The reference is written from the specs -- BT.2020 / PQ /
  limited 10-bit and BT.709 / sRGB curve / limited 8-bit, left-cosited chroma
  -- not from the shader, so a matrix, range, siting, packing or timeline
  mistake shows up as a code mismatch.

  Also checks: the stream's colour tags and HDR10 SEI (ffprobe), that the last
  frame matches ITS OWN time better than the previous frame's (catches an
  off-by-one-frame timeline), and that a missing ffmpeg is a clear error.

  Needs ffmpeg + ffprobe (-Ffmpeg <dir or ffmpeg.exe>, else PATH); without
  them the script prints SKIP and exits 0.

.EXAMPLE
  pwsh -File Tests\RunVideoExport.ps1 -Ffmpeg 'C:\tools\ffmpeg\bin'
#>
param(
    [string]$Ffmpeg,
    [string]$Headless,
    [string]$Adapter = 'default',
    [int]$Frames = 6,
    [double]$Fps = 30
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# ---- tools ---------------------------------------------------------------
if (-not $Headless) {
    $Headless = Get-ChildItem -Path (Join-Path $repo '*\*\ShaderLabHeadless\ShaderLabHeadless.exe') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Headless -or -not (Test-Path $Headless)) { throw "ShaderLabHeadless.exe not found; pass -Headless" }

$ffExe = $null
if ($Ffmpeg) {
    $ffExe = if (Test-Path $Ffmpeg -PathType Container) { Join-Path $Ffmpeg 'ffmpeg.exe' } else { $Ffmpeg }
} else {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { $ffExe = $cmd.Source }
}
if (-not $ffExe -or -not (Test-Path $ffExe)) { Write-Host 'SKIP: ffmpeg not found (pass -Ffmpeg or put it on PATH)'; exit 0 }
$ffDir = Split-Path -Parent $ffExe
$ffprobe = Join-Path $ffDir 'ffprobe.exe'
if (-not (Test-Path $ffprobe)) { Write-Host "SKIP: ffprobe.exe not next to $ffExe"; exit 0 }

$work = Join-Path ([IO.Path]::GetTempPath()) ("shaderlab-video-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$failures = [System.Collections.Generic.List[string]]::new()
function Check([string]$name, [bool]$ok, [string]$detail) {
    Write-Host ("  [{0}] {1}  {2}" -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $detail)
    if (-not $ok) { $failures.Add($name) }
}
$common = @('--adapter', $Adapter, '--display-profile', 'srgb-sdr')

# ---- the animated probe graph --------------------------------------------
$size = 128
$script = @(
    @{ method = 'POST'; path = '/graph/clear'; body = @{} },
    @{ method = 'POST'; path = '/graph/add-node'; body = @{ effectName = 'Gradient Generator' } },
    @{ method = 'POST'; path = '/graph/add-node'; body = @{ effectName = 'Clock' } },
    @{ op = 'set-property'; nodeId = 1; key = 'GradSize'; value = [double]$size },
    @{ op = 'set-property'; nodeId = 1; key = 'EndR'; value = 12.0 },
    @{ op = 'set-property'; nodeId = 1; key = 'EndG'; value = 6.0 },
    @{ op = 'set-property'; nodeId = 1; key = 'EndB'; value = 2.0 },
    @{ op = 'set-property'; nodeId = 1; key = 'StartG'; value = 0.2 },
    @{ op = 'set-property'; nodeId = 2; key = 'StopTime'; value = 1.0 },
    @{ method = 'POST'; path = '/graph/bind-property'; body = @{ nodeId = 1; propertyName = 'StartR'; sourceNodeId = 2; sourceFieldName = 'Progress' } },
    @{ method = 'POST'; path = '/graph/bind-property'; body = @{ nodeId = 1; propertyName = 'StartB'; sourceNodeId = 2; sourceFieldName = 'Progress' } },
    @{ method = 'GET'; path = '/graph/save'; body = @{} }
)
$scriptPath = Join-Path $work 'build.json'
$script | ConvertTo-Json -Depth 6 | Set-Content $scriptPath -Encoding utf8
$seed = Join-Path $repo 'Tests\fixtures\test_cli_basic.json'
& $Headless --graph $seed --script $scriptPath --script-output (Join-Path $work 'build_out.json') @common | Out-Null
$graphPath = Join-Path $work 'probe_graph.json'
((Get-Content (Join-Path $work 'build_out.json') -Raw | ConvertFrom-Json).results[-1].body | ConvertTo-Json -Depth 30) |
    Set-Content $graphPath -Encoding utf8

# ---- reference conversion (from the specs) ---------------------------------
# BT.709 -> BT.2020 linear (BT.2087).
$M = @(@(0.627403896, 0.329283039, 0.043313065), @(0.069097289, 0.919540395, 0.011362316), @(0.016391439, 0.088013308, 0.895595253))
function PQ([double]$nits) {
    $n = [Math]::Min([Math]::Max($nits, 0.0), 10000.0) / 10000.0
    $m1 = 2610.0 / 16384; $m2 = 2523.0 / 4096 * 128; $c1 = 3424.0 / 4096; $c2 = 2413.0 / 4096 * 32; $c3 = 2392.0 / 4096 * 32
    $y = [Math]::Pow($n, $m1)
    return [Math]::Pow(($c1 + $c2 * $y) / (1 + $c3 * $y), $m2)
}
function Srgb([double]$x) {
    $x = [Math]::Min([Math]::Max($x, 0.0), 1.0)
    if ($x -le 0.0031308) { return $x * 12.92 } else { return 1.055 * [Math]::Pow($x, 1 / 2.4) - 0.055 }
}
function EncodePixel([double]$r, [double]$g, [double]$b, [bool]$hdr) {
    # NB: PowerShell variables are case-insensitive -- the encoded values must
    # not be called $R/$G/$B, or they overwrite the inputs $r/$g/$b mid-matrix.
    if ($hdr) {
        $eR = PQ (80 * ($M[0][0] * $r + $M[0][1] * $g + $M[0][2] * $b))
        $eG = PQ (80 * ($M[1][0] * $r + $M[1][1] * $g + $M[1][2] * $b))
        $eB = PQ (80 * ($M[2][0] * $r + $M[2][1] * $g + $M[2][2] * $b))
        $eY = 0.2627 * $eR + 0.6780 * $eG + 0.0593 * $eB
        return @($eY, (($eB - $eY) / 1.8814), (($eR - $eY) / 1.4746))
    }
    $eR = Srgb $r; $eG = Srgb $g; $eB = Srgb $b
    $eY = 0.2126 * $eR + 0.7152 * $eG + 0.0722 * $eB
    return @($eY, (($eB - $eY) / 1.8556), (($eR - $eY) / 1.5748))
}
function ReadPixels([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $w = [BitConverter]::ToUInt32($bytes, 0); $h = [BitConverter]::ToUInt32($bytes, 4)
    $f = [float[]]::new($w * $h * 4)
    [Buffer]::BlockCopy($bytes, 8, $f, 0, $f.Length * 4)
    return @{ W = [int]$w; H = [int]$h; F = $f }
}
function ReferenceCodes([double]$t, [bool]$hdr) {
    $bin = Join-Path $work 'ref.bin'
    & $Headless --graph $graphPath --node 1 --pixels "0,0,$size,$size" --output $bin --time $t @common | Out-Null
    $p = ReadPixels $bin
    $W = $p.W; $H = $p.H
    $Yp = [double[]]::new($W * $H); $Cb = [double[]]::new($W * $H); $Cr = [double[]]::new($W * $H)
    for ($i = 0; $i -lt $W * $H; $i++) {
        $e = EncodePixel $p.F[4 * $i] $p.F[4 * $i + 1] $p.F[4 * $i + 2] $hdr
        $Yp[$i] = $e[0]; $Cb[$i] = $e[1]; $Cr[$i] = $e[2]
    }
    $Y = [int[]]::new($W * $H); $U = [int[]]::new(($W / 2) * ($H / 2)); $V = [int[]]::new(($W / 2) * ($H / 2))
    for ($i = 0; $i -lt $W * $H; $i++) {
        $Y[$i] = if ($hdr) { [Math]::Min(1023, [Math]::Max(0, [Math]::Floor(64 + 876 * $Yp[$i] + 0.5))) }
                 else      { [Math]::Min(255,  [Math]::Max(0, [Math]::Floor(16 + 219 * $Yp[$i] + 0.5))) }
    }
    for ($j = 0; $j -lt $H / 2; $j++) {
        for ($i = 0; $i -lt $W / 2; $i++) {
            # Accumulators must not be $cb/$cr: PowerShell names are
            # case-insensitive, and those ARE the $Cb/$Cr arrays.
            $sumB = 0.0; $sumR = 0.0
            foreach ($dy in 0, 1) {
                $row = (2 * $j + $dy) * $W
                foreach ($k in @(@(-1, 0.125), @(0, 0.25), @(1, 0.125))) {
                    $x = [Math]::Min([Math]::Max(2 * $i + $k[0], 0), $W - 1)   # left-cosited [1 2 1]/4, replicate at x = -1
                    $sumB += $k[1] * $Cb[$row + $x]; $sumR += $k[1] * $Cr[$row + $x]
                }
            }
            $U[$j * ($W / 2) + $i] = if ($hdr) { [Math]::Min(1023, [Math]::Max(0, [Math]::Floor(512 + 896 * $sumB + 0.5))) } else { [Math]::Min(255, [Math]::Max(0, [Math]::Floor(128 + 224 * $sumB + 0.5))) }
            $V[$j * ($W / 2) + $i] = if ($hdr) { [Math]::Min(1023, [Math]::Max(0, [Math]::Floor(512 + 896 * $sumR + 0.5))) } else { [Math]::Min(255, [Math]::Max(0, [Math]::Floor(128 + 224 * $sumR + 0.5))) }
        }
    }
    return @{ Y = $Y; U = $U; V = $V; W = $W; H = $H }
}
function CompareCodes($dec, $ref) {
    $max = 0; $exact = 0; $signed = 0.0
    for ($i = 0; $i -lt $ref.Y.Length; $i++) {
        $s = $dec.Y[$i] - $ref.Y[$i]; $signed += $s
        $d = [Math]::Abs($s); if ($d -gt $max) { $max = $d }; if ($d -eq 0) { $exact++ }
    }
    for ($i = 0; $i -lt $ref.U.Length; $i++) {
        $max = [Math]::Max($max, [Math]::Max([Math]::Abs($dec.U[$i] - $ref.U[$i]), [Math]::Abs($dec.V[$i] - $ref.V[$i])))
    }
    return @{ Max = $max; ExactY = $exact / $ref.Y.Length; MeanSignedY = $signed / $ref.Y.Length }
}
function MeanAbsY($dec, $ref) {
    $s = 0.0; for ($i = 0; $i -lt $ref.Y.Length; $i++) { $s += [Math]::Abs($dec.Y[$i] - $ref.Y[$i]) }; return $s / $ref.Y.Length
}

# ---- export + verify --------------------------------------------------------
foreach ($fmt in 'hdr10', 'sdr') {
    $hdr = $fmt -eq 'hdr10'
    Write-Host "=== $fmt"
    $out = Join-Path $work "probe_$fmt.mp4"
    $log = & $Headless --graph $graphPath --node 1 --video $out --format $fmt --fps $Fps --duration ($Frames / $Fps) --lossless --ffmpeg $ffDir @common 2>&1
    Check "$fmt export succeeds" ($LASTEXITCODE -eq 0) (($log | Select-Object -Last 1) -join '')
    if ($LASTEXITCODE -ne 0) { continue }

    $tags = & $ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt,color_range,color_space,color_transfer,color_primaries,chroma_location,nb_frames -of json $out | ConvertFrom-Json
    $st = $tags.streams[0]
    if ($hdr) {
        Check 'HDR10 stream tags' ($st.pix_fmt -eq 'yuv420p10le' -and $st.color_primaries -eq 'bt2020' -and $st.color_transfer -eq 'smpte2084' -and $st.color_space -eq 'bt2020nc' -and $st.color_range -eq 'tv' -and $st.chroma_location -eq 'left') ($st | ConvertTo-Json -Compress)
        $side = & $ffprobe -v error -select_streams v:0 -read_intervals '%+#1' -show_frames -show_entries frame_side_data_list $out
        Check 'HDR10 mastering-display + content-light SEI' (($side -match 'Mastering display metadata').Count -gt 0 -and ($side -match 'Content light level metadata').Count -gt 0) ''
    } else {
        Check 'SDR stream tags' ($st.pix_fmt -eq 'yuv420p' -and $st.color_primaries -eq 'bt709' -and $st.color_transfer -eq 'bt709' -and $st.color_space -eq 'bt709' -and $st.color_range -eq 'tv') ($st | ConvertTo-Json -Compress)
    }
    Check "$fmt frame count" ([int]$st.nb_frames -eq $Frames) "nb_frames=$($st.nb_frames)"

    # Decode to raw planes.
    $raw = Join-Path $work "dec_$fmt.raw"
    & $ffExe -v error -y -i $out -f rawvideo -pix_fmt ($(if ($hdr) { 'p010le' } else { 'nv12' })) $raw
    $bytes = [IO.File]::ReadAllBytes($raw)
    $bpc = if ($hdr) { 2 } else { 1 }
    $frameBytes = ($size * $size + ($size / 2) * ($size / 2) * 2) * $bpc
    $decFrame = {
        param([int]$k)
        $o = $k * $frameBytes
        $get = if ($hdr) { { param($i) [BitConverter]::ToUInt16($bytes, $o + 2 * $i) -shr 6 } } else { { param($i) [int]$bytes[$o + $i] } }
        $Y = [int[]]::new($size * $size); for ($i = 0; $i -lt $Y.Length; $i++) { $Y[$i] = & $get $i }
        $n = ($size / 2) * ($size / 2)
        $U = [int[]]::new($n); $V = [int[]]::new($n)
        for ($i = 0; $i -lt $n; $i++) { $U[$i] = & $get ($size * $size + 2 * $i); $V[$i] = & $get ($size * $size + 2 * $i + 1) }
        @{ Y = $Y; U = $U; V = $V }
    }

    $worst = 0; $minExact = 1.0; $bias = 0.0
    for ($k = 0; $k -lt $Frames; $k++) {
        $c = CompareCodes (& $decFrame $k) (ReferenceCodes ($k / $Fps) $hdr)
        $worst = [Math]::Max($worst, $c.Max); $minExact = [Math]::Min($minExact, $c.ExactY)
        if ([Math]::Abs($c.MeanSignedY) -gt [Math]::Abs($bias)) { $bias = $c.MeanSignedY }
    }
    # <= 1 code: the GPU does PQ / sRGB in float32 and the reference in double,
    # so a value within float error of a rounding boundary may land either side.
    # Those ties are unbiased; a real mistake (rounding mode, range offset) is
    # not -- it shifts the MEAN. So the second criterion is the mean signed
    # error, not the fraction of exact codes (which depends on how many pixels
    # the content happens to put near a boundary: 94.5% here, 96-98% at 512^2).
    Check "$fmt codes match the reference (<= 1 code)" ($worst -le 1) ("worst {0}, min exact-luma {1:P2}" -f $worst, $minExact)
    Check "$fmt luma unbiased (|mean signed error| < 0.05 code)" ([Math]::Abs($bias) -lt 0.05) ("worst-frame mean {0:N4}" -f $bias)

    $last = & $decFrame ($Frames - 1)
    $own = MeanAbsY $last (ReferenceCodes (($Frames - 1) / $Fps) $hdr)
    $prev = MeanAbsY $last (ReferenceCodes (($Frames - 2) / $Fps) $hdr)
    Check "$fmt timeline aligned (last frame matches its own time)" ($own -lt $prev) ("mean|dY| own {0:N4} vs previous {1:N4}" -f $own, $prev)
}

# ---- missing ffmpeg is a clear error -----------------------------------------
$bad = & $Headless --graph $graphPath --node 1 --video (Join-Path $work 'x.mp4') --duration 0.1 --ffmpeg (Join-Path $work 'no-such\ffmpeg.exe') @common 2>&1
Check 'missing ffmpeg fails with instructions' ($LASTEXITCODE -ne 0 -and ($bad -join ' ') -match 'ffmpeg not found') (($bad | Select-Object -Last 1) -join '')

if ($failures.Count) { Write-Host "FAILED: $($failures -join '; ')"; exit 1 }
Write-Host 'ALL VIDEO EXPORT CHECKS PASSED'
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
exit 0
