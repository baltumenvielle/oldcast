<#
.SYNOPSIS
    Creates bench.ts for the offline benchmark, encoded exactly like stream.ps1 does.

.DESCRIPTION
    Copy the result to /3ds/oldcast/bench.ts on the SD card, then run the benchmark
    with Y on the client's menu or with run-test.ps1 -Mode bench.

.EXAMPLE
    .\make-bench.ps1 -In gameplay.mp4 -Seconds 30
#>
param(
    [Parameter(Mandatory = $true)][string]$In,
    [string]$Out = "bench.ts",
    [string]$Bitrate = "2M",
    [int]$Seconds = 30
)

& ffmpeg -hide_banner -y -i $In -t $Seconds -an `
    -vf "fps=30,scale=400:240,format=yuv420p" `
    -c:v mpeg2video -b:v $Bitrate -minrate $Bitrate -maxrate $Bitrate -bufsize 500k -bf 0 -g 15 `
    -muxdelay 0 -muxpreload 0 -flush_packets 1 -omit_video_pes_length 0 `
    -f mpegts $Out
