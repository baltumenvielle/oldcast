<#
.SYNOPSIS
    Regenerates the CIA artwork in client/assets from its sources.

.DESCRIPTION
    icon.png (48x48)    from pc/oldcast/web/icon.svg
    banner.png (256x128) from client/assets/banner.html
    banner.wav           from client/scripts/make_jingle.py

    Uses Microsoft Edge in headless mode to render the HTML and SVG, and ffmpeg to resize.
    The generated files are committed, so this is only needed after changing a source.
#>
$ErrorActionPreference = "Stop"
$client = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $client
$assets = Join-Path $client "assets"
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$work = Join-Path $env:TEMP "oldcast-assets"
New-Item -ItemType Directory -Force $work | Out-Null

function Render($url, $width, $height, $out) {
    # Edge reports progress on stderr, which PowerShell 5.1 would treat as an error.
    Start-Process -FilePath $edge -Wait -NoNewWindow -RedirectStandardError (Join-Path $work "edge.log") `
        -ArgumentList @("--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
            "--default-background-color=00000000", "--user-data-dir=$work\profile",
            "--window-size=$width,$height", "--screenshot=$out", $url)
}

# Icon: render large, then downscale, which gives cleaner edges than rendering at 48 px.
$iconPage = Join-Path $work "icon.html"
$svg = "file:///" + (Join-Path $root "pc\oldcast\web\icon.svg").Replace("\", "/")
Set-Content -Encoding utf8 $iconPage "<body style='margin:0'><img src='$svg' style='width:480px;height:480px;display:block'></body>"
Render ("file:///" + $iconPage.Replace("\", "/")) 480 480 (Join-Path $work "icon480.png")
& ffmpeg -hide_banner -loglevel error -y -i (Join-Path $work "icon480.png") -vf "scale=48:48:flags=lanczos" (Join-Path $assets "icon.png")

# Banner: crop in case the browser enforces a minimum window size.
Render ("file:///" + (Join-Path $assets "banner.html").Replace("\", "/")) 256 128 (Join-Path $work "banner_raw.png")
& ffmpeg -hide_banner -loglevel error -y -i (Join-Path $work "banner_raw.png") -vf "crop=256:128:0:0" (Join-Path $assets "banner.png")

& python (Join-Path $PSScriptRoot "make_jingle.py")
Get-ChildItem $assets | Select-Object Name, Length
