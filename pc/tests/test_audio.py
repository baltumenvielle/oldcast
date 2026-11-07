"""Tests the audio packet format and the 3DS ADPCM decoder, without a 3DS or a sound card.

Encodes two seconds of synthetic stereo audio with pc/audio.py's encoder, checks the
quality of the round trip, and, when given the path of client/test/test_adpcm, checks that
the C decoder produces exactly the same samples as the Python reference.

    pc\\.venv\\Scripts\\python.exe pc\\tests\\test_audio.py [path\\to\\test_adpcm.exe]
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import warnings

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
with warnings.catch_warnings():
    warnings.simplefilter("ignore", DeprecationWarning)
    import audioop  # noqa: F401  (imported here to silence its warning once)
from audio import HEADER, Encoder, decode_packet  # noqa: E402

RATE = 48000
FRAMES = 480
failures = 0


def check(name, condition, detail=""):
    global failures
    print(("ok    " if condition else "FAIL  ") + name + (f"  ({detail})" if detail else ""))
    if not condition:
        failures += 1


def synthetic_audio(seconds):
    """Left: 440 Hz tone with a 3 kHz overtone. Right: 1 kHz tone plus noise."""
    random.seed(1)
    samples = []
    for n in range(int(RATE * seconds)):
        t = n / RATE
        left = 9000 * math.sin(2 * math.pi * 440 * t) + 3000 * math.sin(2 * math.pi * 3000 * t)
        right = 8000 * math.sin(2 * math.pi * 1000 * t) + random.uniform(-1500, 1500)
        samples += [int(left), int(right)]
    return struct.pack(f"<{len(samples)}h", *samples)


def snr_db(reference, decoded):
    ref = struct.unpack(f"<{len(reference) // 2}h", reference)
    dec = struct.unpack(f"<{len(decoded) // 2}h", decoded)
    signal = sum(r * r for r in ref)
    noise = sum((r - d) ** 2 for r, d in zip(ref, dec)) or 1
    return 10 * math.log10(signal / noise)


pcm = synthetic_audio(2.0)
encoder = Encoder(RATE)
packets = [encoder.packet(pcm[i:i + FRAMES * 4], i) for i in range(0, len(pcm), FRAMES * 4)]

check("packet size", all(len(p) == HEADER.size + 8 + FRAMES for p in packets), f"{len(packets[0])} bytes")
reference = b"".join(decode_packet(p) for p in packets)
check("round trip quality", snr_db(pcm, reference) > 25, f"SNR {snr_db(pcm, reference):.1f} dB")

# A lost packet must not damage the ones after it: each carries its own decoder state.
alone = decode_packet(packets[57])
check("packets decode independently", alone == reference[57 * FRAMES * 4:58 * FRAMES * 4])

if len(sys.argv) > 1:
    with tempfile.TemporaryDirectory() as tmp:
        dump = os.path.join(tmp, "packets.bin")
        out = os.path.join(tmp, "decoded.pcm")
        with open(dump, "wb") as f:
            for p in packets:
                f.write(struct.pack("<H", len(p)) + p)
        subprocess.run([sys.argv[1], dump, out], check=True, capture_output=True)
        with open(out, "rb") as f:
            c_output = f.read()
        check("C decoder matches the reference bit for bit", c_output == reference,
              f"{len(c_output)} vs {len(reference)} bytes")

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
