"""SipHash-2-4, the signature of every message the 3DS sends (see client/source/siphash.h).

Pure Python: a few microseconds per 40-byte packet, plenty for ~125 packets per second.
"""
import struct

MASK = 0xFFFFFFFFFFFFFFFF


def _rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & MASK


def siphash24(key: bytes, data: bytes) -> int:
    k0, k1 = struct.unpack("<QQ", key)
    v0 = 0x736F6D6570736575 ^ k0
    v1 = 0x646F72616E646F6D ^ k1
    v2 = 0x6C7967656E657261 ^ k0
    v3 = 0x7465646279746573 ^ k1

    def rounds(n):
        nonlocal v0, v1, v2, v3
        for _ in range(n):
            v0 = (v0 + v1) & MASK
            v1 = _rotl(v1, 13) ^ v0
            v0 = _rotl(v0, 32)
            v2 = (v2 + v3) & MASK
            v3 = _rotl(v3, 16) ^ v2
            v0 = (v0 + v3) & MASK
            v3 = _rotl(v3, 21) ^ v0
            v2 = (v2 + v1) & MASK
            v1 = _rotl(v1, 17) ^ v2
            v2 = _rotl(v2, 32)

    full = len(data) - len(data) % 8
    for i in range(0, full, 8):
        (m,) = struct.unpack_from("<Q", data, i)
        v3 ^= m
        rounds(2)
        v0 ^= m
    b = (len(data) & 0xFF) << 56
    for i, byte in enumerate(data[full:]):
        b |= byte << (8 * i)
    v3 ^= b
    rounds(2)
    v0 ^= b
    v2 ^= 0xFF
    rounds(4)
    return v0 ^ v1 ^ v2 ^ v3


def tag(key: bytes, data: bytes) -> bytes:
    """The 8-byte signature appended to a message."""
    return struct.pack("<Q", siphash24(key, data))


def verify(key: bytes, signed: bytes) -> bool:
    """True if the last 8 bytes of `signed` are the signature of the rest."""
    if len(signed) < 8:
        return False
    import hmac
    return hmac.compare_digest(tag(key, signed[:-8]), signed[-8:])
