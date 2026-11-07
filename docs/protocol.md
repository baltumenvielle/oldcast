# OldCast protocol

All traffic is UDP. Multi-byte integers are little-endian. Every message starts with a
4-byte magic. The 3DS starts every exchange, and the PC always answers to the address and
port the request came from. That way the same protocol works through home routers (NAT),
which is what playing from outside the home network needs.

## Ports

| Port | Direction | Content |
| --- | --- | --- |
| 5007 (PC) | 3DS -> PC | discovery and session control |
| 5004 (3DS) | PC -> 3DS | video, MPEG-TS (`docs/design.md`) |
| 5006 (3DS) | PC -> 3DS | audio packets (`pc/audio.py`) |
| 5005 (PC) | 3DS -> PC | input packets, echoed back (`client/source/input.c`) |

## Discovery

The 3DS sends `OCDQ` to the broadcast address on port 5007, once per second while it shows
its list of PCs. Every PC running the host answers directly.

`OCDQ` (discovery query), 3DS -> broadcast:

| Field | Type | Notes |
| --- | --- | --- |
| magic | char[4] | `OCDQ` |
| version | u8 | protocol version, 1 |
| reserved | u8[3] | |
| client_id | u8[8] | random, chosen once and stored on the SD card |

`OCDA` (discovery answer), PC -> 3DS:

| Field | Type | Notes |
| --- | --- | --- |
| magic | char[4] | `OCDA` |
| version | u8 | 1 |
| flags | u8 | bit 0: this 3DS is already paired; bit 1: a session is active with another 3DS |
| reserved | u8[2] | |
| host_id | u8[8] | random, chosen once and stored by the host |
| name | char[32] | PC name, UTF-8, zero-padded |

## Session

1. The 3DS sends `OCSR` (session request) from its **video** socket (port 5004) to the PC.
   The PC streams video to the address and port this request came from.
2. If the 3DS is not paired, the PC answers `OCPN` and shows the request in its window;
   the 3DS shows a 4-digit PIN (included in `OCSR`) that the user must see in the PC
   window and confirm there. Once confirmed, both sides store the pairing.
3. When paired, the PC answers `OCSA` (session accepted) and starts streaming.
4. The 3DS then sends `OCAP` (audio punch) from its **audio** socket (port 5006). The PC
   sends audio to the address and port it came from.
5. Input packets (port 5005) keep the session alive. If none arrive for 3 seconds, or the
   3DS sends `OCSE` (session end), the PC stops streaming.

`OCSR` (session request), 3DS video socket -> PC:

| Field | Type | Notes |
| --- | --- | --- |
| magic | char[4] | `OCSR` |
| version | u8 | 1 |
| reserved | u8[3] | |
| client_id | u8[8] | |
| pin | u16 | 4-digit PIN shown on the 3DS while pairing, 0 when paired |
| bitrate_kbps | u16 | requested video bitrate |
| flags | u8 | bit 0: audio wanted |
| reserved | u8[3] | |
| name | char[16] | 3DS name shown in the PC window |

`OCSA` (session accepted), `OCPN` (pairing needed) and `OCSD` (session denied, for
example busy with another 3DS), PC -> 3DS:

| Field | Type | Notes |
| --- | --- | --- |
| magic | char[4] | `OCSA`, `OCPN` or `OCSD` |
| version | u8 | 1 |
| reason | u8 | for `OCSD`: 1 busy, 2 pairing rejected |
| reserved | u8[2] | |

`OCAP` (audio punch) and `OCSE` (session end), 3DS -> PC: magic, version, reserved[3],
client_id[8].

## Security (not implemented yet)

Pairing will create a shared key, and input packets will carry a truncated HMAC so that
nobody else on the network can drive the virtual controller. Until then, the host only
accepts sessions from paired 3DS and only from the local network.
