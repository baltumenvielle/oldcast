"""End-to-end test of gamepad.py's mapping, without a 3DS.

Start the translator first, then run this test. It sends fake 3DS packets to
127.0.0.1:5005 and reads the virtual controller back through XInput, the same API games use.

    pc\\.venv\\Scripts\\python.exe pc\\gamepad.py --radius 40 --curve 1
    pc\\.venv\\Scripts\\python.exe pc\\tests\\test_gamepad.py

Windows only (XInput). A virtual Xbox 360 controller appears while gamepad.py runs.
"""
import ctypes
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from gamepad import touch_stick, video_latency_ms  # noqa: E402

PACKET = struct.Struct("<4sBBHIhhHHI")
PACKET_V2 = struct.Struct("<4sBBHIhhHHIHHHBB")
ECHO = struct.Struct("<4sHI")
KEY_A, KEY_B, KEY_SELECT, KEY_START = 1 << 0, 1 << 1, 1 << 2, 1 << 3
KEY_R, KEY_L, KEY_Y = 1 << 8, 1 << 9, 1 << 11

XINPUT_A, XINPUT_B, XINPUT_X = 0x1000, 0x2000, 0x4000
XINPUT_BACK, XINPUT_LB = 0x0020, 0x0100


class XInputGamepad(ctypes.Structure):
    _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte),
                ("bRightTrigger", ctypes.c_ubyte), ("sThumbLX", ctypes.c_short),
                ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short), ("sThumbRY", ctypes.c_short)]


class XInputState(ctypes.Structure):
    _fields_ = [("dwPacketNumber", ctypes.c_uint), ("Gamepad", XInputGamepad)]


xinput = ctypes.windll.xinput1_4
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq = 0
failures = 0


def connected_pads():
    pads = {}
    for index in range(4):
        state = XInputState()
        if xinput.XInputGetState(index, ctypes.byref(state)) == 0:
            pads[index] = state.Gamepad
    return pads


def send(keys=0, cx=0, cy=0, touch=None, repeat=3):
    """Sends the same state a few times, as the 3DS would, then lets it settle."""
    global seq
    tx, ty = touch or (0, 0)
    for _ in range(repeat):
        seq += 1
        sock.sendto(PACKET.pack(b"3DSI", 1, 1 if touch else 0, seq, keys, cx, cy, tx, ty, 0), ("127.0.0.1", 5005))
        time.sleep(0.01)
    time.sleep(0.05)


def check(name, condition):
    global failures
    print(("ok    " if condition else "FAIL  ") + name)
    if not condition:
        failures += 1


# Identify the virtual pad by pressing a button only it can have pressed.
send(keys=KEY_Y)
ours = [i for i, pad in connected_pads().items() if pad.wButtons == XINPUT_X]
if len(ours) != 1:
    sys.exit("Could not find the virtual controller. Is gamepad.py running?")
state = lambda: connected_pads()[ours[0]]

send(keys=KEY_B); check("3DS B -> Xbox A", state().wButtons == XINPUT_A)
send(keys=KEY_A); check("3DS A -> Xbox B", state().wButtons == XINPUT_B)
send(keys=KEY_L | KEY_R); s = state()
check("L/R -> LT/RT", s.bLeftTrigger == 255 and s.bRightTrigger == 255 and s.wButtons == 0)
send(keys=KEY_SELECT | KEY_L); s = state()
check("SELECT+L -> LB, no LT", s.wButtons == XINPUT_LB and s.bLeftTrigger == 0)
send(keys=KEY_SELECT | KEY_START); check("SELECT+START -> nothing", state().wButtons == 0)
send(keys=0)
send(keys=KEY_SELECT, repeat=2); send(keys=0, repeat=1); check("SELECT tap -> BACK", state().wButtons == XINPUT_BACK)
time.sleep(0.15); send(keys=0); check("BACK is released", state().wButtons == 0)
send(cx=145, cy=-145); s = state()
check("Circle Pad -> left stick", s.sThumbLX > 32000 and s.sThumbLY < -32000)
send(cx=10, cy=-10); s = state(); check("Circle Pad deadzone", s.sThumbLX == 0 and s.sThumbLY == 0)
send(touch=(100, 100)); s = state(); check("first touch is the stick center", s.sThumbRX == 0 and s.sThumbRY == 0)
send(touch=(140, 80)); s = state()
check("drag 40 right, 20 up -> capped at full length, same direction", 28800 < s.sThumbRX < 29800 and 14300 < s.sThumbRY < 15000)
send(touch=(120, 110)); s = state()
check("drag 20 right, 10 down -> X half, Y quarter", 15000 < s.sThumbRX < 18000 and -9000 < s.sThumbRY < -7000)
send(); check("lifting the finger centers the stick", state().sThumbRX == 0)
send(keys=KEY_B); time.sleep(0.6); check("no packets for 0.6 s -> released", state().wButtons == 0)

# Touch response curve (pure function, independent of the running translator's settings).
x, y = touch_stick(45, 0, 90, 1.8)
check("curve: half drag gives about 29%", abs(x - 0.287) < 0.01 and y == 0)
x, y = touch_stick(200, 0, 90, 1.8)
check("curve: drags past the radius are capped at 100%", abs(x - 1.0) < 1e-9)
x, y = touch_stick(30, 40, 100, 2.0)
check("curve keeps the drag direction", abs(x / y - 0.75) < 1e-9 and abs((x * x + y * y) ** 0.5 - 0.25) < 1e-9)
check("resting finger jitter is ignored", touch_stick(1, 1, 90, 1.8) == (0.0, 0.0))

# Version 2 packets are echoed back to the sender with their seq and t_ms.
sock.settimeout(1.0)
sock.sendto(PACKET_V2.pack(b"3DSI", 2, 0, 777, 0, 0, 0, 0, 0, 123456, 0, 0, 0, 0, 0), ("127.0.0.1", 5005))
try:
    magic, seq_back, t_back = ECHO.unpack(sock.recvfrom(64)[0])
    check("v2 packet is echoed", magic == b"3DSE" and seq_back == 777 and t_back == 123456)
except socket.timeout:
    check("v2 packet is echoed", False)

# Latency math: drawn 150 ms ago, held 10 ms on the 3DS, 20 ms round trip -> 130 ms.
now = 1_700_000_123_456.0
check("latency math", abs(video_latency_ms(now, (int(now) - 150) & 0xFFFF, 10, 20) - 130) < 1)
check("latency math across 16-bit wrap", abs(video_latency_ms(65_540.0, 65_500, 0, 0) - 40) < 1)
check("implausible reading rejected", video_latency_ms(now, (int(now) + 5) & 0xFFFF, 0, 0) is None)

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
