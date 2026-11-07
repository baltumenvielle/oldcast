<#
.SYNOPSIS
    Runs one measured test on the 3DS without touching it beyond opening the NetLoader.

.DESCRIPTION
    Sends the client with 3dslink, starts it in the requested mode and saves the
    metrics the 3DS reports over the network to results\<mode>-<time>.csv. In stream
    mode it also streams video for the duration of the test.

    -Latency measures end-to-end video latency automatically: it opens latency-clock.html
    full screen on the main monitor (Edge kiosk mode, separate profile), streams the desktop,
    runs gamepad.py to collect the readings, and prints a summary at the end.

    -Audio also streams what the PC is playing (pc/audio.py) during a stream test.

    -AB makes the client alternate two display configurations every 10 seconds (see
    client/source/stream.c); with -Latency the summary is split by configuration.

    On the 3DS: open the Homebrew Launcher and press Y (NetLoader) first.

.EXAMPLE
    .\run-test.ps1 -Ip 192.168.0.149 -Mode stream -Seconds 40
.EXAMPLE
    .\run-test.ps1 -Ip 192.168.0.149 -Latency -Seconds 40
.EXAMPLE
    .\run-test.ps1 -Ip 192.168.0.149 -Latency -AB -Seconds 80
.EXAMPLE
    .\run-test.ps1 -Ip 192.168.0.149 -Mode bench
#>
param(
    [Parameter(Mandatory = $true)][string]$Ip,
    [ValidateSet("stream", "bench")][string]$Mode = "stream",
    [int]$Seconds = 40,
    [ValidateSet("test", "desktop")][string]$Source = "test",
    [string]$Bitrate = "2M",
    [int]$PaceKbps = 0,
    [switch]$Latency,
    [switch]$AB,
    [switch]$Audio
)

$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root "client\oldcast.3dsx"
$link = "C:\devkitPro\tools\bin\3dslink.exe"
$python = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$outDir = Join-Path $root "results"
New-Item -ItemType Directory -Force $outDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $outDir "$Mode-$stamp.csv"
$err = Join-Path $outDir "$Mode-$stamp.3dslink.txt"
$latencyLog = Join-Path $outDir "latency-$stamp.csv"

if ($Latency) {
    $Mode = "stream"
    $Source = "desktop"
}

$helpers = @()
try {
    if ($Latency) {
        $profileDir = Join-Path $env:TEMP "oldcast-latency-clock"
        $page = "file:///" + (Join-Path $PSScriptRoot "latency-clock.html").Replace("\", "/")
        $helpers += Start-Process -FilePath $edge -PassThru -ArgumentList @(
            "--kiosk", $page, "--edge-kiosk-type=fullscreen", "--user-data-dir=$profileDir",
            "--no-first-run", "--no-default-browser-check")
        $helpers += Start-Process -FilePath $python -NoNewWindow -PassThru `
            -ArgumentList @("-u", (Join-Path $PSScriptRoot "gamepad.py"), "--log", $latencyLog) `
            -RedirectStandardOutput (Join-Path $outDir "gamepad-$stamp.txt") `
            -RedirectStandardError (Join-Path $outDir "gamepad-$stamp.err.txt")
    }

    if ($Audio -and $Mode -eq "stream") {
        $helpers += Start-Process -FilePath $python -NoNewWindow -PassThru `
            -ArgumentList @("-u", (Join-Path $PSScriptRoot "audio.py"), "--ip", $Ip) `
            -RedirectStandardOutput (Join-Path $outDir "audio-$stamp.txt") `
            -RedirectStandardError (Join-Path $outDir "audio-$stamp.err.txt")
    }

    $appArgs = @("-a", $Ip, "-s", $app, $Mode)
    if ($Mode -eq "stream") {
        $appArgs += "$Seconds"
        if ($AB) { $appArgs += "ab" }
    }

    Write-Host "Sending the client to $Ip..."
    $proc = Start-Process -FilePath $link -ArgumentList $appArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError $err

    if ($Mode -eq "stream") {
        # Time for the ~720 KB upload and the client start-up.
        Start-Sleep -Seconds 8
        & (Join-Path $PSScriptRoot "stream.ps1") -Ip $Ip -Source $Source -Bitrate $Bitrate `
            -Seconds ($Seconds - 6) -PaceKbps $PaceKbps
    }

    if (-not $proc.WaitForExit(($Seconds + 120) * 1000)) {
        $proc.Kill()
        Write-Host "3dslink did not finish in time; stopped."
    }
} finally {
    foreach ($p in $helpers) {
        if ($p -and -not $p.HasExited) { & taskkill.exe /T /F /PID $p.Id | Out-Null }
    }
}

Write-Host "Results: $log"
Get-Content $log -Tail 15

if ($Latency -and (Test-Path $latencyLog)) {
    $readings = @(Import-Csv $latencyLog)
    if ($readings.Count -eq 0) {
        Write-Host "No latency readings. Was latency-clock.html visible on the captured monitor?"
    }
    foreach ($group in ($readings | Group-Object variant | Sort-Object Name)) {
        $values = @($group.Group | ForEach-Object { [double]$_.video_latency_ms } | Sort-Object)
        $rtts = @($group.Group | ForEach-Object { [double]$_.rtt_ms })
        $avg = ($values | Measure-Object -Average).Average
        $p = { param($q) $values[[math]::Min($values.Count - 1, [math]::Floor($q * $values.Count))] }
        $label = if ($AB) { "Variant $([char](65 + [int]$group.Name)): " } else { "" }
        Write-Host ("{0}video latency over {1} frames: avg {2:N0} ms, min {3:N0}, median {4:N0}, p90 {5:N0}, max {6:N0}; rtt avg {7:N0} ms" -f `
            $label, $values.Count, $avg, $values[0], (& $p 0.5), (& $p 0.9), $values[-1], ($rtts | Measure-Object -Average).Average)
    }
    Write-Host "Readings: $latencyLog"
}
