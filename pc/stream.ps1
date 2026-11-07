<#
.SYNOPSIS
    Streams the PC screen (or a test pattern) to the 3DS as MPEG-2 over UDP.

.EXAMPLE
    .\stream.ps1 -Ip 192.168.0.50
.EXAMPLE
    .\stream.ps1 -Ip 192.168.0.50 -Source test -Bitrate 1500k -Seconds 30
#>
param(
    [Parameter(Mandatory = $true)][string]$Ip,
    [int]$Port = 5004,
    [string]$Bitrate = "2M",
    # desktop: monitor number -Output, captured with DXGI Desktop Duplication.
    # test:    ffmpeg's moving test pattern, useful to separate capture from network issues.
    [ValidateSet("desktop", "test")][string]$Source = "desktop",
    [int]$Output = 0,
    # Stop after this many seconds; 0 streams until interrupted.
    [int]$Seconds = 0,
    # If > 0, ffmpeg spaces datagrams out at this rate instead of sending each frame as
    # one burst. Not needed since the 3DS receive buffer fix; kept for experiments.
    [int]$PaceKbps = 0
)

if ($Source -eq "desktop") {
    $inputArgs = @("-f", "lavfi", "-i", "ddagrab=output_idx=${Output}:framerate=30,hwdownload,format=bgra")
} else {
    $inputArgs = @("-re", "-f", "lavfi", "-i", "testsrc2=s=400x240:r=30")
}

$url = "udp://${Ip}:${Port}?pkt_size=1316"
if ($PaceKbps -gt 0) { $url += "&bitrate=$($PaceKbps * 1000)" }

$durationArgs = @()
if ($Seconds -gt 0) { $durationArgs = @("-t", "$Seconds") }

# Encoding choices, all made for latency on a 268 MHz ARM11 decoder:
#   - MPEG-2 at 400x240: the Old 3DS decodes it in ~15 ms per frame (H.264 does not fit).
#   - Constant bitrate and a 500k buffer: frame sizes stay predictable.
#   - -bf 0: no B-frames, so no reordering delay. -g 15: a keyframe every 0.5 s to
#     recover from losses.
#   - -omit_video_pes_length 0: each frame carries its length, so the 3DS can decode it
#     as soon as its last byte arrives instead of waiting for the next frame (33 ms).
#   - 1316-byte datagrams: 7 TS packets, below the Ethernet/Wi-Fi MTU.
& ffmpeg -hide_banner @inputArgs @durationArgs `
    -vf "scale=400:240,format=yuv420p" `
    -c:v mpeg2video -b:v $Bitrate -minrate $Bitrate -maxrate $Bitrate -bufsize 500k -bf 0 -g 15 `
    -muxdelay 0 -muxpreload 0 -flush_packets 1 -omit_video_pes_length 0 `
    -f mpegts $url
