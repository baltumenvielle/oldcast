# OldCast protocol

All traffic is UDP. Multi-byte integers are little-endian. Every message starts with a
4-byte magic. The 3DS starts every exchange, and the PC always answers to the address and
port the request came from. The PC uses a single port, 5007, for everything: it is the only
one to open on the home router to play from outside, and every flow the 3DS receives comes
from the address it sent to, which routers on the 3DS side require.

## Ports

| Port | Direction | Content |
| --- | --- | --- |
| 5007 (PC) | 3DS -> PC | discovery, session control and input packets (`client/source/input.c`) |
| 5004 (3DS) | PC 5007 -> 3DS | video, MPEG-TS (`docs/design.md`), and the replies to `OCSR` |
| 5006 (3DS) | PC 5007 -> 3DS | audio packets (`pc/oldcast/audio.py`) |
| input socket (3DS) | PC 5007 -> 3DS | echoes of the input packets |

## Discovery

The 3DS sends `OCDQ` to the broadcast address on port 5007, once per second while it shows
its list of PCs. Every PC running the host answers directly.

`OCDQ` (discovery query), 3DS -> broadcast:

| Field | Type | Notes |
| --- | --- | --- |
| magic | char[4] | `OCDQ` |
| version | u8 | protocol version, 2 |
| reserved | u8[3] | |
| client_id | u8[8] | random, chosen once and stored on the SD card |

`OCDA` (discovery answer), PC -> 3DS:

| Field | Type | Notes |
| --- | --- | --- |
| magic | char[4] | `OCDA` |
| version | u8 | 2 |
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
5. Input packets (also to port 5007) keep the session alive. If none arrive for 3 seconds, or the
   3DS sends `OCSE` (session end), the PC stops streaming.

`OCSR` (session request), 3DS video socket -> PC:

| Field | Type | Notes |
| --- | --- | --- |
| magic | char[4] | `OCSR` |
| version | u8 | 2 |
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
| version | u8 | 2 |
| reason | u8 | for `OCSD`: 1 busy, 2 pairing rejected |
| reserved | u8[2] | |

`OCAP` (audio punch), `OCKA` (keepalive) and `OCSE` (session end), 3DS -> PC: magic,
version, reserved[3], client_id[8], nonce u64.

During a session the 3DS sends `OCAP` from its audio socket and `OCKA` from its video socket
every 5 seconds. Video and audio only flow toward the 3DS, and routers (mobile carriers in
particular) forget a UDP mapping that sends nothing for 20-60 s; these messages keep both
open. The PC follows the source address of each one, in case the router changed it.

## Security

Version 2 of the protocol (the `version` byte of every message) adds a pairing key:

- When the user allows a 3DS, the PC creates a random 16-byte key. It hands it over in `OCSA`
  only when the request comes from the local network and is unsigned (the 3DS has no key
  yet). The 3DS stores it on the SD card with the PC's id and public address.
- From then on every message the 3DS sends (`OCSR`, `OCAP`, `OCKA`, `OCSE` and input packets)
  ends with an 8-byte SipHash-2-4 signature of the message, made with that key.
- Each signed message carries a nonce, the 3DS clock in milliseconds. The PC rejects a nonce
  it has already seen, so a recorded message cannot be replayed (for example to make the PC
  stream the screen to someone else). Session requests keep the last nonce on disk.
- From outside the local network the PC answers nothing that is not correctly signed, and
  never pairs: an unknown or unsigned request gets `OCSD` with reason 3 ("pair at home
  first"). On the local network a wrong signature gets reason 4, and the 3DS drops its key
  to pair again.
- Input packets of a signed session must be signed; unsigned ones are ignored.

Video and audio toward the 3DS are not signed: forging them would only show something on
the screen, and the 3DS accepts them only from the PC it is connected to.

`OCSA` (session accepted), PC -> 3DS, 40 bytes: magic, version, flags (bit 0 key included,
bit 1 public address included), reserved[2], host_id[8], public_ip (4 bytes, network
order), public_port u16, reserved[2], key[16]. `OCPN` and `OCSD` keep the 8-byte layout
above; `OCSD` reasons: 1 busy, 2 pairing rejected, 3 pair at home first, 4 unknown key.

`OCSR` in version 2: magic, version, flags (bit 0 audio wanted, bit 1 signed), reserved[2],
client_id[8], nonce u64, pin u16, bitrate_kbps u16, reserved[4], name[16], then the
signature when signed.
