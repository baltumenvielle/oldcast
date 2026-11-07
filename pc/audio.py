"""Streams the PC sound to the 3DS. The implementation is pc/oldcast/audio.py.

    pc\\.venv\\Scripts\\python.exe pc\\audio.py --ip 192.168.0.149
"""
import sys

from oldcast.audio import HEADER, Encoder, decode_packet, main  # noqa: F401

if __name__ == "__main__":
    sys.exit(main())
