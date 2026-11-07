"""Message formats of the OldCast control protocol, version 2 (docs/protocol.md).

Messages the 3DS sends once it is paired end with an 8-byte SipHash-2-4 signature made with
the key it received when pairing (see siphash.py). Unsigned messages are only accepted from
the local network, and only to pair or to receive the key.
"""
import ipaddress
import struct

CONTROL_PORT = 5007
VERSION = 2
TAG_SIZE = 8
KEY_SIZE = 16

DISCOVERY_QUERY = struct.Struct("<4sB3x8s")              # OCDQ: magic, version, client_id
DISCOVERY_ANSWER = struct.Struct("<4sBB2x8s32s")         # OCDA: magic, version, flags, host_id, name
SESSION_REQUEST = struct.Struct("<4sBB2x8sQHH4x16s")     # OCSR: magic, version, flags, client_id,
                                                         #       nonce, pin, bitrate_kbps, name
SESSION_REPLY = struct.Struct("<4sBB2x")                 # OCPN / OCSD: magic, version, reason
SESSION_ACCEPT = struct.Struct("<4sBB2x8sIH2x16s")       # OCSA: magic, version, flags, host_id,
                                                         #       public_ip, public_port, key
CLIENT_MESSAGE = struct.Struct("<4sB3x8sQ")              # OCAP / OCSE / OCKA: magic, version, client_id, nonce

# OCDA flags
FLAG_PAIRED = 0x01
FLAG_BUSY = 0x02

# OCSR flags
REQUEST_AUDIO = 0x01
REQUEST_HAS_KEY = 0x02   # the 3DS holds a key for this PC and signed the request

# OCSA flags
ACCEPT_KEY = 0x01        # `key` holds the pairing key (local network only)
ACCEPT_PUBLIC = 0x02     # `public_ip`/`public_port` tell where to reach this PC from outside

# OCSD reasons
DENIED_BUSY = 1
DENIED_PAIRING = 2
DENIED_PAIR_AT_HOME = 3  # unknown or unsigned request from outside the local network
DENIED_BAD_KEY = 4       # signature does not match: the PC forgot this 3DS (local network only)


def is_local(ip: str) -> bool:
    """True for addresses of the local network (where pairing is allowed)."""
    address = ipaddress.ip_address(ip)
    return address.is_private or address.is_loopback or address.is_link_local


def discovery_answer(host_id: bytes, name: str, flags: int) -> bytes:
    encoded = name.encode("utf-8")[:31]
    return DISCOVERY_ANSWER.pack(b"OCDA", VERSION, flags, host_id, encoded)


def session_reply(magic: bytes, reason: int = 0) -> bytes:
    return SESSION_REPLY.pack(magic, VERSION, reason)


def session_accept(host_id: bytes, key: bytes = None, public=None) -> bytes:
    flags = 0
    ip, port = 0, 0
    if key:
        flags |= ACCEPT_KEY
    if public:
        flags |= ACCEPT_PUBLIC
        ip = int.from_bytes(ipaddress.IPv4Address(public[0]).packed, "little")
        port = public[1]
    return SESSION_ACCEPT.pack(b"OCSA", VERSION, flags, host_id, ip, port, key or bytes(KEY_SIZE))


def _text(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def parse(data: bytes):
    """Decodes a message the 3DS sends to the control port.

    Returns (magic, fields, body, signature) where body is the signed part and signature the
    trailing 8 bytes (None when the message is unsigned), or (None, None, None, None).
    """
    nothing = (None, None, None, None)
    if len(data) < 5 or data[4] != VERSION:
        return nothing
    magic = data[:4]
    if magic == b"OCDQ" and len(data) == DISCOVERY_QUERY.size:
        _, _, client_id = DISCOVERY_QUERY.unpack(data)
        return magic, {"client_id": client_id}, data, None

    if magic == b"OCSR" and len(data) in (SESSION_REQUEST.size, SESSION_REQUEST.size + TAG_SIZE):
        body = data[:SESSION_REQUEST.size]
        _, _, flags, client_id, nonce, pin, bitrate, name = SESSION_REQUEST.unpack(body)
        signature = data[SESSION_REQUEST.size:] or None
        return magic, {"client_id": client_id, "nonce": nonce, "pin": pin, "bitrate_kbps": bitrate,
                       "audio": bool(flags & REQUEST_AUDIO), "has_key": bool(flags & REQUEST_HAS_KEY),
                       "name": _text(name)}, body, signature

    if magic in (b"OCAP", b"OCSE", b"OCKA") and len(data) in (CLIENT_MESSAGE.size, CLIENT_MESSAGE.size + TAG_SIZE):
        body = data[:CLIENT_MESSAGE.size]
        _, _, client_id, nonce = CLIENT_MESSAGE.unpack(body)
        return magic, {"client_id": client_id, "nonce": nonce}, body, data[CLIENT_MESSAGE.size:] or None
    return nothing
