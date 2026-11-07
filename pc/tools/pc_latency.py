"""Measures the PC side of the latency: screen capture, encoding and sending, without a 3DS.

Streams the main monitor to 127.0.0.1 with the same ffmpeg settings as stream.ps1 and
receives it the way the 3DS does: every frame is closed as soon as its last datagram
arrives (using the PES length) and decoded at once with libavcodec in low-delay mode
(through PyAV). The barcode drawn by latency-clock.html is then read from each frame.
What remains in the result is capture + encoding + sending, plus about 1-2 ms of local
decoding.

latency-clock.html must be full screen on the main monitor while this runs, for example:
    msedge --kiosk file:///.../pc/latency-clock.html --edge-kiosk-type=fullscreen

    pc\\.venv\\Scripts\\python.exe pc\\tools\\pc_latency.py --seconds 15
    pc\\.venv\\Scripts\\python.exe pc\\tools\\pc_latency.py --extra=-threads --extra=1

Requires PyAV (pip install av).
"""
import argparse
import socket
import statistics
import subprocess
import sys
import threading
import time

import av
import av.codec.context

WIDTH, HEIGHT = 400, 240
PORT = 5600
TS_PACKET = 188
VIDEO_PID = 0x100  # ffmpeg's mpegts muxer puts the first stream on PID 0x100
BARS = 24
PROBE_ROW = int(HEIGHT * 7.5 / 100)  # same geometry as client/source/latency_probe.c


def read_probe(luma, stride):
    """Python port of probe_read(): returns the 16-bit code or None."""
    bits = []
    for i in range(BARS):
        cx = (2 * i + 1) * WIDTH // (2 * BARS)
        total = 0
        for y in range(PROBE_ROW - 2, PROBE_ROW + 2):
            row = y * stride
            total += sum(luma[row + cx - 2:row + cx + 2])
        bits.append(total > 128 * 16)
    if not bits[0] or bits[1] or bits[-2] or not bits[-1]:
        return None
    data = 0
    for b in bits[2:18]:
        data = (data << 1) | b
    check = 0
    for b in bits[18:22]:
        check = (check << 1) | b
    expected = ((data & 15) + ((data >> 4) & 15) + ((data >> 8) & 15) + ((data >> 12) & 15)) & 15
    return data if check == expected else None


class FrameAssembler:
    """Minimal PES reassembly for one PID, closing frames by PES length like the 3DS."""

    def __init__(self, on_frame):
        self.on_frame = on_frame
        self.buf = None
        self.remaining = 0

    def feed(self, datagram):
        for i in range(0, len(datagram) - TS_PACKET + 1, TS_PACKET):
            p = datagram[i:i + TS_PACKET]
            if p[0] != 0x47 or (((p[1] & 0x1F) << 8) | p[2]) != VIDEO_PID:
                continue
            afc = (p[3] >> 4) & 3
            if not afc & 1:
                continue
            offset = 4 + (1 + p[4] if afc & 2 else 0)
            payload = p[offset:]
            if p[1] & 0x40:
                pes_len = (payload[4] << 8) | payload[5]
                header = 9 + payload[8]
                self.buf = bytearray(payload[header:])
                self.remaining = pes_len - 3 - payload[8] - len(self.buf)
            elif self.buf is not None:
                take = payload[:self.remaining]
                self.buf += take
                self.remaining -= len(take)
            if self.buf is not None and self.remaining <= 0:
                self.on_frame(bytes(self.buf))
                self.buf = None


def sender_command(extra, filter_complex, scale_flags):
    capture = "ddagrab=output_idx=0:framerate=30,hwdownload,format=bgra"
    scale = "scale=400:240" + (f":flags={scale_flags}" if scale_flags else "") + ",format=yuv420p"
    if filter_complex:
        # ddagrab as a source filter inside the encoder's filtergraph: no lavfi demuxer.
        source = ["-filter_complex", f"{capture},{scale}"]
    else:
        source = ["-f", "lavfi", "-i", capture, "-vf", scale]
    return (["ffmpeg", "-hide_banner", "-loglevel", "error"] + source +
            ["-c:v", "mpeg2video", "-b:v", "2M", "-minrate", "2M", "-maxrate", "2M", "-bufsize", "500k",
             "-bf", "0", "-g", "15", "-muxdelay", "0", "-muxpreload", "0", "-flush_packets", "1",
             "-omit_video_pes_length", "0"]
            + extra + ["-f", "mpegts", f"udp://127.0.0.1:{PORT}?pkt_size=1316"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--extra", action="append", default=[],
                        help="one extra ffmpeg sender argument; repeat for several")
    parser.add_argument("--filter-complex", action="store_true",
                        help="capture with ddagrab inside -filter_complex instead of the lavfi device")
    parser.add_argument("--scale-flags", default="", help="swscale flags, e.g. fast_bilinear")
    args = parser.parse_args()

    decoder = av.CodecContext.create("mpeg2video", "r")
    decoder.flags |= av.codec.context.Flags.low_delay
    latencies = []
    counts = {"frames": 0}

    def on_frame(data):
        for picture in decoder.decode(av.Packet(data)):
            counts["frames"] += 1
            plane = picture.planes[0]
            code = read_probe(bytes(plane), plane.line_size)
            if code is not None:
                latency = (int(time.time() * 1000) - code) & 0xFFFF
                if latency < 3000:
                    latencies.append(latency)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind(("127.0.0.1", PORT))
    sock.settimeout(0.5)
    assembler = FrameAssembler(on_frame)

    sender = subprocess.Popen(sender_command(args.extra, args.filter_complex, args.scale_flags))
    deadline = time.monotonic() + args.seconds
    threading.Timer(args.seconds, sender.terminate).start()
    while time.monotonic() < deadline + 1:
        try:
            assembler.feed(sock.recv(2048))
        except socket.timeout:
            if sender.poll() is not None:
                break
    sender.wait()

    label = " ".join(args.extra + (["filter_complex"] if args.filter_complex else [])
                     + ([f"scale={args.scale_flags}"] if args.scale_flags else [])) or "(defaults)"
    if not latencies:
        sys.exit(f"{label}: {counts['frames']} frames, no barcode. Is latency-clock.html full screen?")
    latencies.sort()
    print(f"{label}: {len(latencies)} of {counts['frames']} frames, avg {statistics.mean(latencies):.0f} ms, "
          f"min {latencies[0]}, median {latencies[len(latencies) // 2]}, "
          f"p90 {latencies[int(len(latencies) * 0.9)]}, max {latencies[-1]}")


if __name__ == "__main__":
    main()
