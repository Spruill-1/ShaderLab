<#
.SYNOPSIS
  Generate the video fixtures used by TestVideoSourceSeekAndZeroCopy.

.DESCRIPTION
  The clips are generated rather than checked in (binary media in git is
  permanent history weight). ShaderLabTests reports the test as [SKIP] when
  they are absent, so CI -- which has no ffmpeg -- is unaffected.

  Both clips are 1 s at 30 fps with a SINGLE keyframe, which is the point:
  seeking to frame 17 must decode forward from frame 0, exercising the
  frame-accurate seek, and the stream is hardware-decodable so the zero-copy
  path runs.

    video_hdr10_hevc_30f.mp4  3840x2160 HEVC Main10 (P010 decode), BT.2020/PQ
                              tagged. 4K also makes the decode+upload timing
                              the test prints meaningful.
    video_sdr_h264_30f.mp4    320x180 H.264 High 4:2:0 with B-frames (NV12
                              decode). 180 is not a multiple of 16, so the
                              decoder pads the surface to 192 rows -- which is
                              what caught the chroma-plane offset bug.

  Do NOT use lossless x264: it produces High 4:4:4, which the Windows H.264
  decoder cannot decode (ReadSample reports end-of-stream on the first read).

.PARAMETER Ffmpeg
  ffmpeg.exe or its directory. Default: PATH, then the winget Gyan.FFmpeg
  install location.

.EXAMPLE
  pwsh -File Tests\fixtures\MakeVideoFixtures.ps1
#>
param([string]$Ffmpeg)
$ErrorActionPreference = 'Stop'

if ($Ffmpeg) {
    $ff = if (Test-Path $Ffmpeg -PathType Container) { Join-Path $Ffmpeg 'ffmpeg.exe' } else { $Ffmpeg }
} else {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    $ff = if ($cmd) { $cmd.Source } else {
        (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin\ffmpeg.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1).FullName
    }
}
if (-not $ff -or -not (Test-Path $ff)) {
    throw "ffmpeg not found. Pass -Ffmpeg <path> or install it: winget install Gyan.FFmpeg"
}

$out = $PSScriptRoot
& $ff -hide_banner -loglevel error -y `
    -f lavfi -i "testsrc2=size=3840x2160:rate=30:duration=1" `
    -c:v libx265 -pix_fmt yuv420p10le -tag:v hvc1 `
    -x265-params "keyint=300:min-keyint=300:scenecut=0:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:log-level=error" `
    -crf 22 (Join-Path $out 'video_hdr10_hevc_30f.mp4')
if ($LASTEXITCODE) { throw "ffmpeg failed (HEVC fixture)" }

& $ff -hide_banner -loglevel error -y `
    -f lavfi -i "testsrc2=size=320x180:rate=30:duration=1" `
    -c:v libx264 -profile:v high -pix_fmt yuv420p -g 300 -keyint_min 300 -sc_threshold 0 -bf 2 -crf 20 `
    (Join-Path $out 'video_sdr_h264_30f.mp4')
if ($LASTEXITCODE) { throw "ffmpeg failed (H.264 fixture)" }

Get-ChildItem $out -Filter 'video_*_30f.mp4' | Select-Object Name, Length
