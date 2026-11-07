# OldCast

Stream PC games to an **Old Nintendo 3DS** at 30 fps, and play them with the 3DS buttons.

The Old 3DS (2011) has no hardware video decoder, and its 268 MHz ARM11 cannot decode
H.264 in real time, which is why Moonlight-based clients struggle on it. MPEG-2 is far
cheaper to decode: at 400x240 the Old 3DS decodes a gameplay frame in about 15 ms. This
project pairs a small MPEG-2 receiver for the 3DS with a PC side built on ffmpeg and a
virtual Xbox 360 controller.

**Status:** working on a local network. Measured on an Old 3DS over 2.4 GHz Wi-Fi: 29-30 fps
shown with gameplay, 106 ms from the PC clock to the 3DS (measured automatically over ~2,200
frames, of which 81 ms happen on the PC), and a 7 ms round trip for button packets. Next
steps: audio, play from outside the home network, a proper user interface. See
[docs/design.md](docs/design.md) for the measurements and the reasoning behind each choice.

## How it works

```
PC                                                   Old 3DS
screen ─ ddagrab ─ scale 400x240 ─ MPEG-2 2 Mbps ─┐
                                                   │ UDP 5004, MPEG-TS
                                                   └──► demux ─► libavcodec ─► Y2R ─► GPU ─► top screen
virtual Xbox 360 pad ◄─ gamepad.py ◄─ UDP 5005 ◄────── buttons, Circle Pad, touch (125 Hz)
```

- `client/`: the 3DS application (C, libctru, citro2d, libavcodec).
- `pc/`: PowerShell scripts around ffmpeg and a Python virtual gamepad (ViGEm).

## Requirements

**3DS:** any model with custom firmware (Luma3DS) and the Homebrew Launcher.

**PC (Windows):**
- [ffmpeg](https://ffmpeg.org/) 7.x on the `PATH` (uses `ddagrab`, Windows 8 or later).
- [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver. Sunshine installs it too.
- Python 3.10+ with `vgamepad`.
- To build the client: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group.

## Build

From the devkitPro MSYS2 shell:

```bash
cd client
./scripts/build-ffmpeg.sh   # once: minimal libavcodec with the MPEG-2 decoder
./scripts/build.sh          # produces client/oldcast.3dsx
```

PC side:

```bash
python -m venv pc/.venv
pc/.venv/Scripts/python.exe -m pip install -r pc/requirements.txt
```

## Play

1. On the 3DS, open the Homebrew Launcher and press **Y** (NetLoader).
2. On the PC, with the game in the foreground on the main monitor:

```bash
powershell -ExecutionPolicy Bypass -File pc\play.ps1 -Ip 192.168.0.149
```

`play.ps1` sends the client with `3dslink`, starts the virtual gamepad and streams the
screen. Hold **SELECT+START** for one second on the 3DS to stop. Alternatively, copy
`oldcast.3dsx` to `/3ds/` on the SD card, start it from the Homebrew Launcher, press
**A**, and run `play.ps1 -NoLink`.

### Controls

| 3DS | Xbox 360 |
| --- | --- |
| Circle Pad | left stick |
| touch screen | right stick (the point you touch is the center) |
| A, B, X, Y | by position: bottom (B) is A, right (A) is B, left (Y) is X, top (X) is Y |
| L, R | LT, RT |
| D-pad, START | D-pad, START |
| SELECT (tap) | BACK |
| SELECT + L / R | LB / RB |
| SELECT + B / A | L3 / R3 |
| SELECT + START (1 s) | stop streaming |

The mapping lives in the tables at the top of [pc/gamepad.py](pc/gamepad.py).
`--radius` changes the touch-stick sensitivity (default: 60 pixels for full deflection).

## Measure

- `pc\run-test.ps1 -Ip <3ds-ip> -Mode stream -Seconds 40` streams a test pattern and saves
  the client's per-second metrics to `results\`.
- `pc\run-test.ps1 -Ip <3ds-ip> -Mode bench` decodes `/3ds/oldcast/bench.ts` from the SD
  card without the network (create it with `pc\make-bench.ps1`), to check decoding speed alone.
- `pc\run-test.ps1 -Ip <3ds-ip> -Latency` measures end-to-end latency automatically: it shows
  `pc\latency-clock.html` full screen, and the 3DS reads the clock from the bars at the top of
  each frame. Add `-AB` to compare the client's two display configurations in one session.
- `pc\tools\pc_latency.py` measures the PC's share alone (capture, encoding, sending).

Metrics are also written to `/3ds/oldcast/metrics.csv` and shown on the bottom screen.

## Tests

```bash
gcc -std=c11 -Wall -Iclient/source client/test/test_demux.c client/source/ts_demux.c -o test_demux
gcc -std=c11 -Wall -Iclient/source client/test/test_probe.c client/source/latency_probe.c -o test_probe
pc/.venv/Scripts/python.exe pc/tests/test_gamepad.py   # with pc/gamepad.py --radius 40 running
```

## Credits

- [FFmpeg_for_3DS](https://github.com/Core-2-Extreme/FFmpeg_for_3DS) by Core-2-Extreme, the
  FFmpeg build used for decoding (LGPL).
- [PotatoStream](https://github.com/PainDe0Mie/PotatoStream), whose study of the Old 3DS
  showed where Moonlight hits its limits. No code is shared with it.
- devkitPro, libctru, citro2d and citro3d.
- [vgamepad](https://github.com/yannbouteiller/vgamepad) and ViGEm for the virtual controller.

Developed with the help of [Claude Code](https://claude.com/claude-code).
