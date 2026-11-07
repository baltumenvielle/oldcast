"""Message formats of the OldCast control protocol (docs/protocol.md)."""
import struct

CONTROL_PORT = 5007
VERSION = 1

DISCOVERY_QUERY = struct.Struct("<4sB3x8s")             # OCDQ: magic, version, client_id
DISCOVERY_ANSWER = struct.Struct("<4sBB2x8s32s")        # OCDA: magic, version, flags, host_id, name
SESSION_REQUEST = struct.Struct("<4sB3x8sHHB3x16s")     # OCSR: magic, version, client_id, pin,
                                                        #       bitrate_kbps, flags, name
SESSION_REPLY = struct.Struct("<4sBB2x")                # OCSA / OCPN / OCSD: magic, version, reason
CLIENT_MESSAGE = struct.Struct("<4sB3x8s")              # OCAP / OCSE: magic, version, client_id

FLAG_PAIRED = 0x01
FLAG_BUSY = 0x02

REQUEST_AUDIO = 0x01

DENIED_BUSY = 1
DENIED_PAIRING = 2


def discovery_answer(host_id: bytes, name: str, flags: int) -> bytes:
    encoded = name.encode("utf-8")[:31]
    return DISCOVERY_ANSWER.pack(b"OCDA", VERSION, flags, host_id, encoded)


def session_reply(magic: bytes, reason: int = 0) -> bytes:
    return SESSION_REPLY.pack(magic, VERSION, reason)


def _text(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def parse(data: bytes):
    """Decodes any message the 3DS sends to the control port.

    Returns (magic, fields) where fields is a dict, or (None, None) for anything invalid.
    """
    if len(data) < 5 or data[4] != VERSION:
        return None, None
    magic = data[:4]
    if magic == b"OCDQ" and len(data) == DISCOVERY_QUERY.size:
        _, _, client_id = DISCOVERY_QUERY.unpack(data)
        return magic, {"client_id": client_id}
    if magic == b"OCSR" and len(data) == SESSION_REQUEST.size:
        _, _, client_id, pin, bitrate, flags, name = SESSION_REQUEST.unpack(data)
        return magic, {"client_id": client_id, "pin": pin, "bitrate_kbps": bitrate,
                       "audio": bool(flags & REQUEST_AUDIO), "name": _text(name)}
    if magic in (b"OCAP", b"OCSE") and len(data) == CLIENT_MESSAGE.size:
        _, _, client_id = CLIENT_MESSAGE.unpack(data)
        return magic, {"client_id": client_id}
    return None, None
