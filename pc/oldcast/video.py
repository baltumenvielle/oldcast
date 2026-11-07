"""Screen capture and MPEG-2 encoding with ffmpeg, sent to the 3DS over UDP.

The encoding choices are explained in docs/design.md; in short, everything is tuned for
latency on a 268 MHz ARM11 decoder.
"""
import shutil
import socket
import subprocess
import threading


def ffmpeg_command(ip, port, bitrate_kbps=2000, monitor=0, test_pattern=False):
    if test_pattern:
        source = ["-re", "-f", "lavfi", "-i", "testsrc2=s=400x240:r=30"]
    else:
        source = ["-f", "lavfi", "-i", f"ddagrab=output_idx={monitor}:framerate=30,hwdownload,format=bgra"]
    rate = f"{bitrate_kbps}k"
    return (["ffmpeg", "-hide_banner", "-loglevel", "error"] + source +
            ["-vf", "scale=400:240,format=yuv420p",
             "-c:v", "mpeg2video", "-b:v", rate, "-minrate", rate, "-maxrate", rate, "-bufsize", "500k",
             # No B-frames (no reordering delay); a keyframe every 0.5 s to recover from losses.
             "-bf", "0", "-g", "15",
             # Every frame carries its length, so the 3DS decodes it as soon as it arrives.
             "-muxdelay", "0", "-muxpreload", "0", "-flush_packets", "1", "-omit_video_pes_length", "0",
             # A 1 MB send buffer absorbs bursts; Windows' small default can fail with WSAENOBUFS.
             "-f", "mpegts", f"udp://{ip}:{port}?pkt_size=1316&buffer_size=1048576"])


class VideoSender:
    """Runs ffmpeg for a session and relays its output to the 3DS.

    ffmpeg sends to a socket on 127.0.0.1, and a relay thread forwards every datagram with
    `send(data, address)`, which the host points at its port 5007 socket. That way video
    leaves from the same port as everything else (see host.py). The relay costs well under
    a millisecond per frame.
    """

    def __init__(self, send, report=print):
        self.send = send
        self.report = report
        self.process = None
        self.target = None
        self.relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.relay.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.relay.bind(("127.0.0.1", 0))
        threading.Thread(target=self._relay, daemon=True).start()

    def _relay(self):
        while True:
            data = self.relay.recv(2048)
            target = self.target
            if target:
                self.send(data, target)

    @staticmethod
    def available():
        return shutil.which("ffmpeg") is not None

    def start(self, target, bitrate_kbps, monitor=0, test_pattern=False):
        """Streams to `target`, the (ip, port) the 3DS sent its session request from."""
        self.stop()
        self.target = target
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        local = self.relay.getsockname()
        self.process = subprocess.Popen(ffmpeg_command(local[0], local[1], bitrate_kbps, monitor, test_pattern),
                                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.PIPE, creationflags=flags)
        self.report(f"Streaming monitor {monitor} to {target[0]}:{target[1]} at {bitrate_kbps} kbps")

    def running(self):
        return self.process is not None and self.process.poll() is None

    def error(self):
        """ffmpeg's error output if it exited on its own, else None."""
        if self.process is None or self.process.poll() is None:
            return None
        return self.process.stderr.read().decode(errors="replace").strip() or f"exit code {self.process.returncode}"

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.process = None
        self.target = None
