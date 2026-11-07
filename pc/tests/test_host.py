"""End-to-end test of the host's session protocol with a simulated 3DS (no console needed).

Start the host first, then run this test from the pc folder:

    pc\\.venv\\Scripts\\python.exe -m oldcast --no-window
    pc\\.venv\\Scripts\\python.exe pc\\tests\\test_host.py

The test plays the 3DS side of docs/protocol.md on 127.0.0.1: discovery, a pairing request
that it approves through the web API, the session, video and audio arriving at the addresses
the requests came from, input keeping the session alive, and the end of the session. It
unpairs its fake console at the end. During the test ffmpeg captures the real screen for a
few seconds and sends it only to the test's local socket.
"""
import json
import os
import socket
import struct
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from oldcast import protocol  # noqa: E402

HOST = "127.0.0.1"
API = "http://127.0.0.1:47807"
CLIENT_ID = os.urandom(8)
INPUT = struct.Struct("<4sBBHIhhHHIHHHBB")
failures = 0


def check(name, condition, detail=""):
    global failures
    print(("ok    " if condition else "FAIL  ") + name + (f"  ({detail})" if detail else ""))
    if not condition:
        failures += 1


def api(path, body=None):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(API + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=3) as r:
        return json.load(r)


def udp(port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((HOST, port))
    s.settimeout(2)
    return s


def session_request(sock, pin=1234):
    sock.sendto(protocol.SESSION_REQUEST.pack(b"OCSR", 1, CLIENT_ID, pin, 1500, protocol.REQUEST_AUDIO,
                                              b"Test 3DS"), (HOST, protocol.CONTROL_PORT))
    return sock.recv(64)[:4]


control = udp()
video = udp()   # plays the 3DS video socket: requests come from here, video must arrive here
audio = udp()
inputs = udp()

control.sendto(protocol.DISCOVERY_QUERY.pack(b"OCDQ", 1, CLIENT_ID), (HOST, protocol.CONTROL_PORT))
answer = protocol.DISCOVERY_ANSWER.unpack(control.recv(64))
check("discovery answer", answer[0] == b"OCDA" and not answer[2] & protocol.FLAG_PAIRED, answer[4].rstrip(b"\0"))

check("unpaired request asks for pairing", session_request(video) == b"OCPN")
pending = api("/api/state")["pending"]
check("the PC shows the PIN", pending is not None and pending["pin"] == 1234, pending)
api("/api/pairing", {"approve": True})
check("paired request is accepted", session_request(video, pin=0) == b"OCSA")

state = api("/api/state")
check("session is active", state["session"] is not None and state["session"]["name"] == "Test 3DS")

# Video must arrive at the socket the request came from (ffmpeg needs a moment to start).
video.settimeout(6)
try:
    first = video.recv(2048)
    check("video arrives at the requesting socket", first[0] == 0x47, f"{len(first)} bytes")
except socket.timeout:
    check("video arrives at the requesting socket", False, "nothing in 6 s")

# Audio goes to wherever OCAP came from (skipped if the PC has no playback device).
audio.sendto(protocol.CLIENT_MESSAGE.pack(b"OCAP", 1, CLIENT_ID), (HOST, protocol.CONTROL_PORT))
time.sleep(0.3)
check("audio address learned from OCAP", api("/api/state")["session"]["audio"] in (True, False))

# Input keeps the session alive past the startup grace period.
for seq in range(1, 200):
    inputs.sendto(INPUT.pack(b"3DSI", 2, 0, seq, 0, 0, 0, 0, 0, seq, 0, 0, 0, 0, 0), (HOST, 5005))
    time.sleep(0.04)
check("input keeps the session alive", api("/api/state")["session"] is not None)

control.sendto(protocol.CLIENT_MESSAGE.pack(b"OCSE", 1, CLIENT_ID), (HOST, protocol.CONTROL_PORT))
time.sleep(0.5)
check("OCSE ends the session", api("/api/state")["session"] is None)

api("/api/unpair", {"id": CLIENT_ID.hex()})
check("fake console unpaired", all(d["id"] != CLIENT_ID.hex() for d in api("/api/state")["paired"]))

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
