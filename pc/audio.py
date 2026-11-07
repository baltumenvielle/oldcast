"""Streams what the PC is playing to the 3DS: WASAPI loopback capture, IMA ADPCM, UDP port 5006.

    pc\\.venv\\Scripts\\python.exe pc\\audio.py --ip 192.168.0.149

Every packet carries 10 ms of stereo audio (480 frames at 48 kHz), each channel encoded as
IMA ADPCM (4 bits per sample, about 400 kbps in total). Each packet also carries the decoder
state for both channels, so a lost packet only loses its own 10 ms: the next one decodes
correctly on its own. The 3DS decodes it with a few operations per sample (client/source/audio.c).

Packet layout, little-endian (must match AudioPacket in client/source/audio.c):
    char[4]  magic "3DSA"
    u8       version (1)
    u8       channels (2)
    u16      sequence number
    u32      sample rate in Hz
    u16      frames (samples per channel)
    u16      reserved
    u32      PC time in ms when the audio was captured (low 32 bits)
    per channel: s16 predictor, u8 step index, u8 reserved
    per channel: frames / 2 bytes of ADPCM, first sample in the high nibble

Requires pyaudiowpatch (pip install pyaudiowpatch). Uses the standard audioop module for the
ADPCM encoder, available up to Python 3.12.
"""
import argparse
import socket
import struct
import sys
import time
import warnings
import wave

with warnings.catch_warnings():
    warnings.simplefilter("ignore", DeprecationWarning)
    import audioop

HEADER = struct.Struct("<4sBBHIHHI")
CHANNEL_STATE = struct.Struct("<hBB")
PORT = 5006
PACKET_MS = 10


class Encoder:
    """Turns interleaved 16-bit stereo PCM into packets, keeping the ADPCM state per channel."""

    def __init__(self, rate):
        self.rate = rate
        self.seq = 0
        self.states = [(0, 0), (0, 0)]

    def packet(self, pcm, capture_ms):
        frames = len(pcm) // 4
        channels = [audioop.tomono(pcm, 2, 1, 0), audioop.tomono(pcm, 2, 0, 1)]
        header = HEADER.pack(b"3DSA", 1, 2, self.seq & 0xFFFF, self.rate, frames, 0, capture_ms & 0xFFFFFFFF)
        states = b""
        payload = b""
        for i, mono in enumerate(channels):
            predictor, index = self.states[i]
            states += CHANNEL_STATE.pack(predictor, index, 0)
            data, self.states[i] = audioop.lin2adpcm(mono, 2, self.states[i])
            payload += data
        self.seq += 1
        return header + states + payload


def decode_packet(packet):
    """Reference decoder (what the 3DS does), returns interleaved 16-bit stereo PCM."""
    _, _, channels, _, _, frames, _, _ = HEADER.unpack_from(packet)
    offset = HEADER.size
    states = []
    for _ in range(channels):
        predictor, index, _ = CHANNEL_STATE.unpack_from(packet, offset)
        states.append((predictor, index))
        offset += CHANNEL_STATE.size
    mono = []
    for state in states:
        data = packet[offset:offset + frames // 2]
        offset += frames // 2
        mono.append(audioop.adpcm2lin(data, 2, state)[0])
    left = audioop.tostereo(mono[0], 2, 1, 0)
    right = audioop.tostereo(mono[1], 2, 0, 1)
    return audioop.add(left, right, 2)


def open_loopback():
    import pyaudiowpatch as pyaudio

    audio = pyaudio.PyAudio()
    device = audio.get_default_wasapi_loopback()
    rate = int(device["defaultSampleRate"])
    frames = rate * PACKET_MS // 1000
    stream = audio.open(format=pyaudio.paInt16, channels=2, rate=rate, input=True,
                        input_device_index=device["index"], frames_per_buffer=frames)
    return audio, stream, device["name"], rate, frames


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ip", help="3DS address; omit to only capture (with --wav or --dump)")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = until Ctrl+C)")
    parser.add_argument("--wav", help="also write what the 3DS would decode to this WAV file")
    parser.add_argument("--dump", help="also write the raw packets (u16 length + packet) to this file")
    args = parser.parse_args()

    audio, stream, name, rate, frames = open_loopback()
    print(f"Capturing '{name}' at {rate} Hz, {PACKET_MS} ms packets", flush=True)
    encoder = Encoder(rate)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    wav = None
    if args.wav:
        wav = wave.open(args.wav, "wb")
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(rate)
    dump = open(args.dump, "wb") if args.dump else None

    sent = 0
    start = time.monotonic()
    next_report = start + 5
    try:
        while not args.seconds or time.monotonic() - start < args.seconds:
            # Blocks until 10 ms of audio exist. With nothing playing, WASAPI loopback
            # produces nothing at all, and the 3DS simply pauses its playback.
            pcm = stream.read(frames, exception_on_overflow=False)
            packet = encoder.packet(pcm, int(time.time() * 1000))
            if args.ip:
                sock.sendto(packet, (args.ip, args.port))
            if wav:
                wav.writeframes(decode_packet(packet))
            if dump:
                dump.write(struct.pack("<H", len(packet)) + packet)
            sent += 1
            now = time.monotonic()
            if now >= next_report:
                print(f"packets {sent / (now - next_report + 5):.0f}/s  size {len(packet)} bytes", flush=True)
                sent = 0
                next_report = now + 5
    except KeyboardInterrupt:
        pass
    finally:
        stream.stop_stream()
        stream.close()
        audio.terminate()
        if wav:
            wav.close()
        if dump:
            dump.close()


if __name__ == "__main__":
    sys.exit(main())
