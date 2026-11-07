"""End-to-end test of the host's session protocol (version 2) with a simulated 3DS.

Start the host first, then run this test from the pc folder:

    pc\\.venv\\Scripts\\python.exe -m oldcast --no-window
    pc\\.venv\\Scripts\\python.exe pc\\tests\\test_host.py

The test plays the 3DS side of docs/protocol.md on 127.0.0.1: discovery, an unsigned pairing
request approved through the web API, the key handed over in OCSA, then a signed session:
video arriving at the address the request came from, signed input keeping the session
alive, unsigned input and replayed requests being ignored, the audio address learned from
OCAP, video following a keepalive from a new port, and the end of the session. It unpairs its fake console at the end. During the test
ffmpeg captures the real screen for a few seconds and sends it only to the test's socket.
"""
import json
import os
import socket
import struct
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from oldcast import protocol, siphash  # noqa: E402

HOST = "127.0.0.1"
PORT = protocol.CONTROL_PORT
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


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((HOST, 0))
    s.settimeout(2)
    return s


def nonce():
    return int(time.time() * 1000)


def request(sock, key=None, pin=1234, fixed_nonce=None):
    flags = protocol.REQUEST_AUDIO | (protocol.REQUEST_HAS_KEY if key else 0)
    body = protocol.SESSION_REQUEST.pack(b"OCSR", 2, flags, CLIENT_ID, fixed_nonce or nonce(), pin, 1500,
                                         b"Test 3DS")
    sock.sendto(body + (siphash.tag(key, body) if key else b""), (HOST, PORT))
    try:
        return sock.recv(64)
    except socket.timeout:
        return b""


def client_message(sock, magic, key):
    body = protocol.CLIENT_MESSAGE.pack(magic, 2, CLIENT_ID, nonce())
    sock.sendto(body + siphash.tag(key, body), (HOST, PORT))


control, video, audio, inputs = udp(), udp(), udp(), udp()

control.sendto(protocol.DISCOVERY_QUERY.pack(b"OCDQ", 2, CLIENT_ID), (HOST, PORT))
answer = protocol.DISCOVERY_ANSWER.unpack(control.recv(64))
check("discovery answer", answer[0] == b"OCDA" and not answer[2] & protocol.FLAG_PAIRED, answer[4].rstrip(b"\0"))

check("unpaired request asks for pairing", request(video)[:4] == b"OCPN")
pending = api("/api/state")["pending"]
check("the PC shows the PIN", pending is not None and pending["pin"] == 1234, pending)
api("/api/pairing", {"approve": True})

reply = request(video, pin=0)
check("first paired request is accepted", reply[:4] == b"OCSA", f"{len(reply)} bytes")
_, _, flags, host_id, _, _, key = protocol.SESSION_ACCEPT.unpack(reply)
check("the key comes with it (local network)", flags & protocol.ACCEPT_KEY and key != bytes(16))
api("/api/stop", {})
time.sleep(0.3)

# From now on the simulated 3DS signs everything.
check("signed request is accepted", request(video, key)[:4] == b"OCSA")
state = api("/api/state")
check("session is signed", state["session"] is not None and state["session"]["signed"])
replayed = nonce() - 60_000
check("replayed (old nonce) request is ignored", request(control, key, fixed_nonce=replayed) == b"")
wrong = os.urandom(16)
reply = request(control, wrong)
check("wrong key is told to pair again (local network)",
      reply[:4] == b"OCSD" and reply[5] == protocol.DENIED_BAD_KEY)

video.settimeout(6)
try:
    first = video.recv(2048)
    check("video arrives at the requesting socket", first[0] == 0x47, f"{len(first)} bytes")
except socket.timeout:
    check("video arrives at the requesting socket", False, "nothing in 6 s")

client_message(audio, b"OCAP", key)
time.sleep(0.3)
check("signed OCAP is accepted", api("/api/state")["session"] is not None)

# A router may give the 3DS another public port: video follows the keepalive (OCKA).
moved = udp()
client_message(moved, b"OCKA", key)
moved.settimeout(4)
try:
    packet = moved.recv(2048)
    check("video follows OCKA to a new address", packet[0] == 0x47)
except socket.timeout:
    check("video follows OCKA to a new address", False, "nothing in 4 s")


def send_input(seq, signed):
    packet = INPUT.pack(b"3DSI", 2, 0, seq, 0, 0, 0, 0, 0, seq, 0, 0, 0, 0, 0)
    inputs.sendto(packet + (siphash.tag(key, packet) if signed else b""), (HOST, PORT))


# Unsigned input must not keep a signed session alive (nor drive the controller).
for seq in range(1, 30):
    send_input(seq, signed=False)
    time.sleep(0.05)
check("unsigned input is ignored", api("/api/state")["session"]["input_age_ms"] is None)
for seq in range(30, 170):
    send_input(seq, signed=True)
    time.sleep(0.04)
state = api("/api/state")
check("signed input keeps the session alive", state["session"] is not None and
      state["session"]["input_age_ms"] is not None and state["session"]["input_age_ms"] < 500)

client_message(control, b"OCSE", key)
time.sleep(0.5)
check("signed OCSE ends the session", api("/api/state")["session"] is None)

api("/api/unpair", {"id": CLIENT_ID.hex()})
check("fake console unpaired", all(d["id"] != CLIENT_ID.hex() for d in api("/api/state")["paired"]))

check("public addresses are not local", not protocol.is_local("190.247.1.1") and protocol.is_local("192.168.0.5"))

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
