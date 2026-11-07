"""Writes client/assets/banner.wav: the short sound the HOME menu plays when OldCast is selected.

Two rising notes with a soft attack and decay, synthesized here so the project owns it.

    python client/scripts/make_jingle.py
"""
import math
import os
import struct
import wave

RATE = 22050
NOTES = [(659.25, 0.0, 0.55), (987.77, 0.18, 0.9)]  # E5 then B5: (frequency, start s, length s)
LENGTH_S = 1.2
VOLUME = 0.32


def envelope(t, length):
    attack = 0.01
    return min(1.0, t / attack) * math.exp(-4.5 * t / length)


def main():
    samples = []
    for n in range(int(RATE * LENGTH_S)):
        t = n / RATE
        value = 0.0
        for freq, start, length in NOTES:
            local = t - start
            if 0 <= local < length:
                # A touch of the octave makes it sound like a small bell rather than a beep.
                tone = math.sin(2 * math.pi * freq * local) + 0.3 * math.sin(4 * math.pi * freq * local)
                value += tone * envelope(local, length)
        samples.append(int(max(-1.0, min(1.0, value * VOLUME)) * 32767))

    out = os.path.join(os.path.dirname(__file__), "..", "assets", "banner.wav")
    with wave.open(out, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    print(f"wrote {os.path.normpath(out)}")


if __name__ == "__main__":
    main()
