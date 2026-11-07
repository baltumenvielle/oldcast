# Design notes and measurements

Why the client is built the way it is, and the numbers that drove each decision.
All measurements: Old 3DS (2011), Luma3DS, 2.4 GHz Wi-Fi, PC on Ethernet to the same router.

## Why MPEG-2

The Old 3DS has no hardware video decoder (the MVD block is New 3DS only), and its ARM11
runs at 268 MHz without NEON. H.264 does not decode in real time on it, which is the wall
Moonlight-based clients hit. MPEG-2 is much simpler, and libavcodec has ARMv6 assembly for
its IDCT and motion compensation.

Offline benchmark (`bench` mode), 900 frames of Spider-Man gameplay at 400x240, 2 Mbps CBR:

| Step | Average | Maximum |
| --- | --- | --- |
| Decode (libavcodec, 1 thread) | 13.9 ms | 20.3 ms |
| Y2R conversion | 6.0 ms | 6.4 ms |
| Draw (mostly waiting for VBlank) | 10.6 ms | 16.2 ms |

Decoding alone would allow 72 fps. FFmpeg must be built **with** assembly: the ARMv6
routines are `.S` files that `--disable-asm` silently removes (`scripts/build-ffmpeg.sh`
checks for them).

## Why not a fork of PotatoStream

PotatoStream is about 10,000 lines, most of them the Moonlight session (pairing, TLS,
control stream, audio, input over GameStream) with OpenSSL, curl and expat as dependencies.
Replacing the session with a UDP receiver would leave little of it. Sunshine also cannot
encode MPEG-2. A client written from scratch is about 1,500 lines with libctru, citro2d and
libavcodec as its only dependencies.

## Latency choices

- **No B-frames** (`-bf 0`): B-frames force the decoder to hold a frame back.
- **`AV_CODEC_FLAG_LOW_DELAY`**: without it, mpeg12dec returns each frame only after
  decoding the next one.
- **PES length on video packets** (`-omit_video_pes_length 0`): ffmpeg omits it by default,
  and then the end of a frame is only known when the next frame starts, 33 ms later. With
  the length, the demuxer delivers a frame as soon as its last byte arrives. Verified on a
  real UDP stream: every frame ends exactly at the end of a datagram.
- **Short queue that jumps to keyframes**: a P-frame cannot be skipped without breaking the
  frames after it. When more than 2 frames are waiting, the decoder jumps to the newest
  I-frame in the queue; if the queue fills, it waits for the next I-frame (`-g 15`, every
  0.5 s).
- **Y2R writes straight into a GPU texture** (8x8 tiled layout) and the GPU draws it, so the
  CPU never touches pixels, not even for the rotated framebuffer of the 3DS.

## The receive buffer problem

The first Wi-Fi test lost half of the packets and nearly every frame arrived damaged. The
cause was not the radio: the SOC service gives new UDP sockets a **4416-byte** receive
buffer, three datagrams. A frame is sent as a burst of 7 datagrams (more than 15 for a
keyframe), so its tail was dropped every time. The client now requests 64 KB (128 KB fails
with ENOMEM).

| | Before | After |
| --- | --- | --- |
| Datagrams received per second | ~110 of ~220 | 211 |
| Damaged frames per second | ~28 of 30 | 0.2 |
| Frames shown per second | 0 to 11 | 29.1 |

Two other hypotheses were tested and discarded: Wi-Fi power saving (a keepalive sent by the
3DS every 20 ms changed nothing) and send bursts (pacing with ffmpeg's UDP `bitrate` option
was not needed once the buffer was fixed).

## Current numbers

| Test | Frames shown | Decode | 3DS latency* | End to end |
| --- | --- | --- | --- | --- |
| Test pattern, 40 s | 29.1 fps | 9.8 ms | 25 ms | |
| Desktop with a clock, 60 s | 29.9 fps | 6.9 ms | 23 ms | ~130 ms (filmed) |
| Spider-Man, 3 min | 29.0 fps | 15.6 ms | 57 ms | ~160 ms (estimate) |
| Spider-Man with controls, 7 min | 29.2 fps | 16.0 ms | 58 ms | |

\* From the moment a frame is fully received to the moment it is submitted to the GPU.

With game content, decode + Y2R + draw take about 32 ms of the 33 ms budget. When Wi-Fi
delivers two frames together (arrival gaps up to 166 ms were seen), the second waits for the
first, which is why the 3DS latency doubles compared with a static desktop.

## Measuring latency without a camera

`pc/latency-clock.html` draws the PC clock (`Date.now()`, 16 bits) as a row of 24 bars at the
top of the screen. The 3DS reads the bars from every frame it shows (`latency_probe.c`) and
returns the value inside the next button packet, together with how long it held it and the
last measured round trip. `pc/gamepad.py` echoes every button packet, so the 3DS knows the
round trip, and computes:

    video latency = arrival time - clock value - time held on the 3DS - round trip / 2

Both times come from the PC clock, so no synchronization is needed. The reading ends when the
3DS submits the frame to the GPU; the LCD shows it within the next refresh (~8 ms later on
average). `pc/run-test.ps1 -Latency` runs the whole test, and `-AB` alternates two client
configurations every 10 seconds within one session so they are compared under the same Wi-Fi
conditions. Checked on the PC: the bars survive the 400x240, 2 Mbps encoding in every frame,
and 900 frames of gameplay produced no false reading.

`pc/tools/pc_latency.py` measures the PC alone: it streams to 127.0.0.1 and receives the way
the 3DS does (frame closed by PES length, decoded at once).

## The single network thread

Button packets reached the PC with gaps of up to 125 ms and a 53 ms round trip, although the
3DS sampled every 8 ms. libctru talks to the SOC service through one session, and the kernel
serves one request per session at a time: while the video thread waited in `poll()`, the
input thread's `sendto()` waited too. Now one thread owns both sockets and never waits past
the next input tick. Round trip: **53 ms -> 7 ms**.

## Latency budget (static clock page, 2 Mbps)

| Stage | Time | How it was measured |
| --- | --- | --- |
| Browser draw + Windows compositor + capture at 30 fps | ~50-65 ms | estimate: PC total minus the rest |
| ffmpeg scale, encode, send | small | no ffmpeg option tried changed the PC total |
| PC total | **81 ms** | `pc_latency.py` |
| Wi-Fi, one way | ~4 ms | half the measured round trip |
| 3DS: decode + Y2R + draw | **15 ms** | client metrics (`age`) |
| End to end, to GPU submit | **106 ms** | `run-test.ps1 -Latency`, ~2,200 frames |

Changes measured with A/B tests:

| Change | Result |
| --- | --- |
| Single network thread | button round trip 53 -> 7 ms |
| Draw without waiting for VBlank | draw 8.6 -> 0.3 ms, 3DS latency -16 ms, no frame loss (adopted) |
| Skip a frame when 1 newer one waits (instead of 2) | no gain on a static page, 5 fewer fps when Wi-Fi delivers frames in pairs (rejected) |
| ffmpeg `-threads 1` | PC +7 ms (rejected) |
| `scale` with `fast_bilinear`, ddagrab in `-filter_complex` | no measurable change |

Part of the PC share belongs to the measuring page itself (a browser takes one or two
refreshes to show what it draws); a full-screen game may skip some of those steps. The next
real step would be 60 fps streaming, which halves the capture wait, but the Old 3DS needs
14-22 ms per gameplay frame (decode + Y2R), above the 16.7 ms a 60 fps stream allows.

## Audio

**Capture:** `pc/audio.py` records what Windows is playing with WASAPI loopback, at the mix
format of the output device (48 kHz stereo here), so nothing is resampled on the PC.

**Codec: IMA ADPCM, decoded by the 3DS CPU.** The 3DS DSP can decode Nintendo's DSP-ADPCM in
hardware, but encoding it well means searching the best predictor for every 14-sample block,
too slow in Python for real time. IMA ADPCM is encoded by Python's `audioop` and decoded with
a few integer operations per sample: about 1% of the Old 3DS CPU for 48 kHz stereo. PotatoStream
decodes Opus, which competes with the video for the same CPU. Quality: 32.6 dB SNR on a
synthetic test signal (`pc/tests/test_audio.py`).

**Packets:** 10 ms each (508 bytes, 100 per second, about 410 kbps). Every packet carries the
decoder state of both channels, so a lost packet only loses its own 10 ms. The C decoder is
checked to be bit-exact with the Python reference.

**Playback:** the network thread decodes each packet straight into one of 24 DSP wave buffers
and queues it on an NDSP channel (polyphase resampling to the DSP's 32.7 kHz).

**Jitter:** Wi-Fi delivers packets with gaps of 50 to 250 ms, but audio must not stop. Playback
pauses until a minimum queue depth is reached, 40 ms at first. An underrun while packets keep
arriving raises that minimum by 10 ms; 15 s without underruns lowers it again. A packet that
would make the queue more than 40 ms deeper than the minimum is dropped instead, so the delay
does not build up. The audio socket gets the same 64 KB receive buffer as the video: with
the default 8-packet buffer, the first keyframe of a stream caused losses.

5-minute game session:

| | |
| --- | --- |
| Audio packets | 29,614, none lost |
| Underruns | 8 (around Wi-Fi gaps of up to 258 ms) |
| Dropped to limit delay | 76 (0.25 per second; each one is a faint 10 ms click) |
| Average queue | 49 ms |

Video spends about 25 ms inside the 3DS and audio about 50 ms in its queue, so sound trails
the picture by roughly 25 ms, below what most people notice; the player did not notice any
offset. Possible improvement: instead of dropping whole packets, play slightly faster when the
queue is deep, which would remove the clicks.
