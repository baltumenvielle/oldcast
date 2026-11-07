<#
.SYNOPSIS
    Starts a play session: virtual gamepad, 3DS client, screen and audio streaming.

.DESCRIPTION
    1. On the 3DS: open the Homebrew Launcher and press Y (NetLoader).
    2. On the PC:  .\play.ps1 -Ip 192.168.0.149
    3. To finish: hold SELECT+START for one second on the 3DS.

    With -NoLink, start the client yourself from the Homebrew Launcher (A on its menu)
    and finish the session with Enter in this window.

    Logs go to results\: gamepad-<time>.txt, audio-<time>.txt and, when using 3dslink,
    game-<time>.csv with the client's metrics. -NoAudio leaves the sound on the PC.
#>
param(
    [Parameter(Mandatory = $true)][string]$Ip,
    [string]$Bitrate = "2M",
    [int]$Output = 0,
    [switch]$NoLink,
    [switch]$NoAudio
)

$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
$link = "C:\devkitPro\tools\bin\3dslink.exe"
$app = Join-Path $root "client\oldcast.3dsx"
$outDir = Join-Path $root "results"
New-Item -ItemType Directory -Force $outDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"

$procs = @()
try {
    $gamepad = Start-Process -FilePath $python -ArgumentList @("-u", (Join-Path $PSScriptRoot "gamepad.py")) `
        -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $outDir "gamepad-$stamp.txt") `
        -RedirectStandardError (Join-Path $outDir "gamepad-$stamp.err.txt")
    $procs += $gamepad

    $linkProc = $null
    if (-not $NoLink) {
        Write-Host "Sending the client to $Ip..."
        $linkProc = Start-Process -FilePath $link -ArgumentList @("-a", $Ip, "-s", $app, "stream", "0") `
            -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $outDir "game-$stamp.csv") `
            -RedirectStandardError (Join-Path $outDir "game-$stamp.3dslink.txt")
        $procs += $linkProc
        Start-Sleep -Seconds 8
    }

    if (-not $NoAudio) {
        $procs += Start-Process -FilePath $python -NoNewWindow -PassThru `
            -ArgumentList @("-u", (Join-Path $PSScriptRoot "audio.py"), "--ip", $Ip) `
            -RedirectStandardOutput (Join-Path $outDir "audio-$stamp.txt") `
            -RedirectStandardError (Join-Path $outDir "audio-$stamp.err.txt")
    }

    $streamArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "stream.ps1"),
        "-Ip", $Ip, "-Bitrate", $Bitrate, "-Output", "$Output")
    $ffmpeg = Start-Process -FilePath "powershell.exe" -ArgumentList $streamArgs -WindowStyle Minimized -PassThru
    $procs += $ffmpeg

    Write-Host "Streaming. To stop: hold SELECT+START for 1 s on the 3DS."
    if ($linkProc) {
        $linkProc.WaitForExit()
    } else {
        Read-Host "Press Enter to stop" | Out-Null
    }
} finally {
    foreach ($p in $procs) {
        if ($p -and -not $p.HasExited) {
            # /T also ends ffmpeg, which runs as a child of the stream.ps1 PowerShell.
            & taskkill.exe /T /F /PID $p.Id | Out-Null
        }
    }
    Write-Host "Session ended. Logs in $outDir"
}
