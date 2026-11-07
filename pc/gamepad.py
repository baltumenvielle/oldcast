"""Turns the 3DS buttons (UDP port 5005) into a virtual Xbox 360 controller (ViGEm).

    pc\\.venv\\Scripts\\python.exe pc\\gamepad.py

Requires the ViGEmBus driver (installed by Sunshine, or from its own installer) and the
vgamepad package (pip install vgamepad).

Default mapping (edit the tables below to change it):
    Circle Pad        -> left stick
    touch screen      -> right stick; where the finger lands is the stick center
    A B X Y           -> by physical position (the bottom 3DS button is Xbox A)
    L / R             -> LT / RT triggers
    SELECT tapped     -> BACK
    SELECT + L / R    -> LB / RB
    SELECT + B / A    -> L3 / R3 (stick clicks)
    SELECT + START    -> nothing: the 3DS uses it to stop streaming

It also measures latency. Every packet is echoed back so the 3DS knows the round-trip
time, and when pc/latency-clock.html is on screen the 3DS reports the timestamp it reads
from each frame it shows (see client/source/latency_probe.h). The end-to-end video latency
is then: arrival time - timestamp - time the 3DS held the reading - half the round trip.
"""
import argparse
import csv
import socket
import struct
import time

import vgamepad as vg

# Must match InputPacket in client/source/input.c. Version 1 lacks the last four fields.
PACKET_V1 = struct.Struct("<4sBBHIhhHHI")
PACKET_V2 = struct.Struct("<4sBBHIhhHHIHHHBB")
ECHO = struct.Struct("<4sHI")
MAGIC = b"3DSI"
FLAG_TOUCH = 0x01
FLAG_PROBE = 0x02
MAX_PLAUSIBLE_LATENCY_MS = 3000

# 3DS key bits, from libctru's hid.h.
KEY_A, KEY_B, KEY_SELECT, KEY_START = 1 << 0, 1 << 1, 1 << 2, 1 << 3
KEY_DRIGHT, KEY_DLEFT, KEY_DUP, KEY_DDOWN = 1 << 4, 1 << 5, 1 << 6, 1 << 7
KEY_R, KEY_L, KEY_X, KEY_Y = 1 << 8, 1 << 9, 1 << 10, 1 << 11
FACE_AND_SHOULDER_KEYS = 0xFFF

X = vg.XUSB_BUTTON

BUTTONS = {
    KEY_B: X.XUSB_GAMEPAD_A,
    KEY_A: X.XUSB_GAMEPAD_B,
    KEY_Y: X.XUSB_GAMEPAD_X,
    KEY_X: X.XUSB_GAMEPAD_Y,
    KEY_DUP: X.XUSB_GAMEPAD_DPAD_UP,
    KEY_DDOWN: X.XUSB_GAMEPAD_DPAD_DOWN,
    KEY_DLEFT: X.XUSB_GAMEPAD_DPAD_LEFT,
    KEY_DRIGHT: X.XUSB_GAMEPAD_DPAD_RIGHT,
    KEY_START: X.XUSB_GAMEPAD_START,
}

# While SELECT is held these keys change meaning; all others keep their normal mapping.
SELECT_LAYER = {
    KEY_L: X.XUSB_GAMEPAD_LEFT_SHOULDER,
    KEY_R: X.XUSB_GAMEPAD_RIGHT_SHOULDER,
    KEY_B: X.XUSB_GAMEPAD_LEFT_THUMB,
    KEY_A: X.XUSB_GAMEPAD_RIGHT_THUMB,
}

CIRCLE_MAX = 145      # the Old 3DS Circle Pad reaches about 150
CIRCLE_DEADZONE = 15
SELECT_TAP_S = 0.35   # SELECT released sooner than this, alone, sends BACK
BACK_PULSE_S = 0.08
TIMEOUT_S = 0.3       # no packets for this long: release everything, so nothing stays stuck


def axis(value, maximum, deadzone=0.0):
    """Scales value to -1..1, with a deadzone around zero."""
    if abs(value) <= deadzone:
        return 0.0
    return max(-1.0, min(1.0, value / maximum))


class Translator:
    """Applies one 3DS input state to the virtual controller."""

    def __init__(self, pad, touch_radius, absolute_touch):
        self.pad = pad
        self.radius = touch_radius
        self.absolute = absolute_touch
        self.anchor = None
        self.select_since = None
        self.select_combined = False
        self.back_until = 0.0

    def apply(self, keys, cx, cy, touching, tx, ty, now):
        pad = self.pad
        pad.reset()

        select = bool(keys & KEY_SELECT)
        if select:
            if self.select_since is None:
                self.select_since = now
                self.select_combined = False
            if keys & ~KEY_SELECT & FACE_AND_SHOULDER_KEYS:
                self.select_combined = True
        elif self.select_since is not None:
            if not self.select_combined and now - self.select_since < SELECT_TAP_S:
                self.back_until = now + BACK_PULSE_S
            self.select_since = None

        if select and keys & KEY_START:
            keys &= ~KEY_START  # reserved: the 3DS stops streaming with SELECT+START

        for bit, button in BUTTONS.items():
            if keys & bit and not (select and bit in SELECT_LAYER):
                pad.press_button(button)
        if select:
            for bit, button in SELECT_LAYER.items():
                if keys & bit:
                    pad.press_button(button)
        else:
            pad.left_trigger(255 if keys & KEY_L else 0)
            pad.right_trigger(255 if keys & KEY_R else 0)
        if now < self.back_until:
            pad.press_button(X.XUSB_GAMEPAD_BACK)

        pad.left_joystick_float(axis(cx, CIRCLE_MAX, CIRCLE_DEADZONE), axis(cy, CIRCLE_MAX, CIRCLE_DEADZONE))

        if touching:
            if self.absolute:
                center = (160, 120)
            else:
                if self.anchor is None:
                    self.anchor = (tx, ty)
                center = self.anchor
            # Screen Y grows downwards; stick Y grows upwards.
            pad.right_joystick_float(axis(tx - center[0], self.radius), axis(center[1] - ty, self.radius))
        else:
            self.anchor = None

        pad.update()

    def release(self):
        self.anchor = None
        self.select_since = None
        self.pad.reset()
        self.pad.update()


def video_latency_ms(now_ms, code, age_ms, rtt_ms):
    """End-to-end video latency from a probe reading, or None if it is not plausible.

    code holds the low 16 bits of the page's Date.now() when the frame was drawn on the PC.
    """
    drawn_ms = now_ms - ((int(now_ms) - code) & 0xFFFF)
    latency = now_ms - drawn_ms - age_ms - rtt_ms / 2
    return latency if 0 < latency < MAX_PLAUSIBLE_LATENCY_MS else None


class LatencyStats:
    """Collects latency readings and prints a summary every few seconds."""

    def __init__(self, log_path):
        self.values = []
        self.rtts = []
        self.writer = None
        if log_path:
            self.file = open(log_path, "w", newline="")
            self.writer = csv.writer(self.file)
            self.writer.writerow(["pc_time_ms", "code", "age_ms", "rtt_ms", "variant", "video_latency_ms"])

    def add(self, now_ms, code, age_ms, rtt_ms, variant):
        latency = video_latency_ms(now_ms, code, age_ms, rtt_ms)
        if latency is None:
            return
        self.values.append(latency)
        if self.writer:
            self.writer.writerow([f"{now_ms:.1f}", code, age_ms, rtt_ms, variant, f"{latency:.1f}"])
            self.file.flush()

    def add_rtt(self, rtt_ms):
        if rtt_ms:
            self.rtts.append(rtt_ms)

    def summary(self):
        parts = []
        if self.values:
            v = sorted(self.values)
            parts.append(f"video latency avg {sum(v) / len(v):.0f} ms  min {v[0]:.0f}  "
                         f"median {v[len(v) // 2]:.0f}  max {v[-1]:.0f}  ({len(v)} frames)")
        if self.rtts:
            parts.append(f"rtt avg {sum(self.rtts) / len(self.rtts):.0f} ms  max {max(self.rtts)}")
        self.values.clear()
        self.rtts.clear()
        return "  ".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=5005)
    parser.add_argument("--radius", type=float, default=60.0,
                        help="touch drag in pixels that moves the right stick fully (default 60)")
    parser.add_argument("--absolute-touch", action="store_true",
                        help="use the center of the touch screen as the stick center")
    parser.add_argument("--log", help="CSV file for every latency reading")
    args = parser.parse_args()

    pad = vg.VX360Gamepad()
    translator = Translator(pad, args.radius, args.absolute_touch)
    latency = LatencyStats(args.log)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(TIMEOUT_S)
    print(f"Virtual controller ready. Waiting for the 3DS on UDP {args.port}...", flush=True)

    source = None
    last_seq = None
    last_packet = None
    received = lost = 0
    max_gap = 0.0
    next_report = time.monotonic() + 5
    active = False

    while True:
        try:
            data, address = sock.recvfrom(64)
        except socket.timeout:
            data = None
        now = time.monotonic()

        fields = None
        if data and len(data) == PACKET_V2.size:
            fields = PACKET_V2.unpack(data)
        elif data and len(data) == PACKET_V1.size:
            fields = PACKET_V1.unpack(data) + (0, 0, 0, 0, 0)

        if fields:
            magic, version, flags, seq, keys, cx, cy, tx, ty, t_ms, code, age_ms, rtt_ms, variant, _ = fields
            if magic == MAGIC and version in (1, 2):
                if version >= 2:
                    # Echo first, so the 3DS measures the network and not our processing.
                    sock.sendto(ECHO.pack(b"3DSE", seq, t_ms), address)
                    latency.add_rtt(rtt_ms)
                    if flags & FLAG_PROBE:
                        latency.add(time.time() * 1000, code, age_ms, rtt_ms, variant)
                if address[0] != source:
                    source = address[0]
                    print(f"3DS connected: {source}", flush=True)
                if last_seq is not None:
                    lost += (seq - last_seq - 1) & 0xFFFF
                if last_packet is not None:
                    max_gap = max(max_gap, now - last_packet)
                last_seq = seq
                last_packet = now
                received += 1
                translator.apply(keys, cx, cy, bool(flags & FLAG_TOUCH), tx, ty, now)
                active = True
        elif active and (last_packet is None or now - last_packet > TIMEOUT_S):
            translator.release()
            active = False
            print("No packets from the 3DS: controller released", flush=True)

        if now >= next_report:
            if received:
                print(f"packets {received / 5:.0f}/s  lost {lost}  max gap {max_gap * 1000:.0f} ms  "
                      f"{latency.summary()}", flush=True)
            received = lost = 0
            max_gap = 0.0
            next_report = now + 5


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
