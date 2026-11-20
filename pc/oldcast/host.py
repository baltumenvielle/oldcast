"""Session logic of the OldCast host: discovery, pairing and streaming (docs/protocol.md).

Everything goes through one UDP socket on port 5007: discovery and session messages and
input packets arrive there, and the video, the audio and the input echoes leave from it.
With a single port only one has to be opened on the router to play from outside the home
network, and every flow leaves from the address the 3DS sent to, which routers on the
3DS side expect.

Everything the user interface shows comes from Host.snapshot(), and everything it can do
goes through the public methods of Host.
"""
import json
import os
import socket
import threading
import time
from collections import deque

from . import protocol, siphash
from .audio import AudioSender
from .gamepad import Gamepad
from .rate import RateControl
from .upnp import PortMapper
from .video import VideoSender

SESSION_TIMEOUT_S = 3.0  # no input packets for this long ends the session
REMOTE_START_KBPS = 1000  # sessions from outside start here and climb if the connection allows
REMOTE_TIMEOUT_S = 15.0  # the same from outside the home network, where Wi-Fi and mobile links drop
                         # for seconds and the 3DS asks for the session again once they come back
STARTUP_GRACE_S = 6.0    # the 3DS needs a moment to start sending input after OCSA
DENY_MEMORY_S = 30.0     # a rejected 3DS is answered OCSD for this long
# ffmpeg stops when Windows takes the screen capture away (a game switching to or from full
# screen, a resolution change: DXGI_ERROR_ACCESS_LOST) and cannot capture at all while the
# secure desktop is shown (a UAC prompt, the lock screen: "access denied"). Both are
# transient, so ffmpeg is restarted, waiting longer after each failure so that a prompt of
# several seconds does not use up the attempts. Only repeated failures end the session.
RESTART_LIMIT = 8        # restarts allowed within RESTART_WINDOW_S
RESTART_WINDOW_S = 60.0
RESTART_DELAYS_S = (0.5, 1.0, 2.0, 4.0)  # wait before the 1st, 2nd, 3rd and later restarts

LOG_MAX_BYTES = 1 << 20   # host.log is renamed to host.log.1 past this size
REJECT_LOG_S = 60.0       # log ignored requests from the same address at most this often

DEFAULT_SETTINGS = {
    "name": socket.gethostname(),
    "bitrate_kbps": 2000,
    "remote_bitrate_kbps": 2000,  # highest for sessions from outside, which vary a lot
    "auto_quality": True,         # fit the bitrate to the connection (rate.py)
    "monitor": 0,
    "audio": True,
    "touch_radius": 90.0,
    "touch_curve": 1.8,
    "remote_play": True,   # open the host port on the router with UPnP
}


def config_dir() -> str:
    path = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "OldCast")
    os.makedirs(path, exist_ok=True)
    return path


class Store:
    """Small JSON file in the config folder."""

    def __init__(self, name, default):
        self.path = os.path.join(config_dir(), name)
        try:
            with open(self.path, encoding="utf-8") as f:
                self.data = {**default, **json.load(f)}
        except (OSError, ValueError):
            self.data = dict(default)
            self.save()

    def save(self):
        with open(self.path, "w", encoding="utf-8") as f:
            json.dump(self.data, f, indent=2)


class Host:
    def __init__(self):
        self.lock = threading.RLock()
        self.log = deque(maxlen=200)
        self.settings = Store("settings.json", DEFAULT_SETTINGS)
        self.paired = Store("paired.json", {})            # client id hex -> {"name", "since"}
        identity = Store("host.json", {"host_id": os.urandom(8).hex()})
        self.host_id = bytes.fromhex(identity.data["host_id"])

        self.pending = None   # pairing request waiting for the user: dict
        self.denied = {}      # client id hex -> time of the rejection
        self.session = None   # active session: dict
        self.rate = None      # automatic quality of the active session, if enabled

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Video bursts go out through this socket; a large send buffer avoids WSAENOBUFS.
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.sock.bind(("0.0.0.0", protocol.CONTROL_PORT))
        self.sock.settimeout(0.05)

        self.video = VideoSender(self._send, report=self.report)
        self.audio = AudioSender(self._send, report=self.report)
        # Input statistics every 5 s would bury the events in the log: keep only the last line.
        self.input_stats = None
        self.gamepad = Gamepad(radius=self.settings.data["touch_radius"],
                               curve=self.settings.data["touch_curve"], report=self.report,
                               stats=lambda line: setattr(self, "input_stats", line))
        self.gamepad.allowed_ip = ""  # no session: the controller ignores every packet
        self.router = PortMapper(protocol.CONTROL_PORT, report=self.report)
        self.log_path = os.path.join(config_dir(), "host.log")
        self.rejected = {}  # address -> time it was last logged

    # --- lifecycle ---------------------------------------------------------------------

    def start(self):
        threading.Thread(target=self._control_loop, daemon=True).start()
        if self.settings.data["remote_play"]:
            self.router.start()
        self.report(f"Host '{self.settings.data['name']}' ready on UDP {protocol.CONTROL_PORT}")
        if not VideoSender.available():
            self.report("ffmpeg was not found: install it and add it to the PATH")

    def report(self, message):
        line = f"{time.strftime('%H:%M:%S')}  {message}"
        with self.lock:  # several threads report; keep their lines whole
            self.log.append(line)
            print(line, flush=True)
            self._write_log(f"{time.strftime('%Y-%m-%d')} {line}")

    def _write_log(self, line):
        """Keeps a log on disk, so problems can be looked at after the fact."""
        try:
            if os.path.exists(self.log_path) and os.path.getsize(self.log_path) > LOG_MAX_BYTES:
                os.replace(self.log_path, self.log_path + ".1")
            with open(self.log_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass

    def _log_rejected(self, address, reason):
        now = time.monotonic()
        if now - self.rejected.get(address[0], -REJECT_LOG_S) >= REJECT_LOG_S:
            self.rejected[address[0]] = now
            self.report(f"Ignored a session request from {address[0]}: {reason}")

    def _send(self, data, address):
        """Sends from the host port. Used by the video relay and the audio sender too."""
        try:
            self.sock.sendto(data, address)
        except OSError:
            pass  # a full buffer or an unreachable 3DS: the stream recovers by itself

    # --- the host port -------------------------------------------------------------------

    def _control_loop(self):
        while True:
            try:
                data, address = self.sock.recvfrom(2048)
            except (socket.timeout, OSError):
                # OSError: Windows reports an ICMP "port unreachable" from an earlier send here.
                data = None
            now = time.monotonic()
            if data and data[:4] == b"3DSI":
                packet = self._check_input(data)
                echo = self.gamepad.handle(packet, address, now) if packet else None
                if echo:
                    self._send(echo, address)  # right away, so the 3DS measures the network
                data = None
            self.gamepad.tick(now)
            if not data:
                self._check_session()
                continue
            magic, fields, body, signature = protocol.parse(data)
            with self.lock:
                if magic == b"OCDQ":
                    self._answer_discovery(fields, address)
                elif magic == b"OCSR":
                    self._handle_request(fields, address, body, signature)
                elif magic in (b"OCAP", b"OCSE", b"OCKA") and self._session_message_ok(fields, body, signature):
                    if magic == b"OCAP":
                        self._handle_audio_punch(address)
                    elif magic == b"OCKA":
                        self._follow_video_address(address)
                    else:
                        self.stop_session("the 3DS ended it")
            self._check_session()

    # --- authentication ------------------------------------------------------------------

    def _check_input(self, data):
        """Returns the input packet without its signature if it may drive the controller."""
        session = self.session
        if not session:
            return None
        if session["key"]:
            # Signed sessions only accept signed input: 32-byte packet + 8-byte signature.
            if len(data) == 40 and siphash.verify(session["key"], data):
                return data[:32]
            return None
        return data

    def _verify(self, client, body, signature, nonce, persist=True):
        """Checks a signed message from a paired 3DS.

        Returns "ok", "bad" (no key or wrong signature) or "replay" (valid signature, but a
        nonce already seen: the 3DS clock in ms only goes forward, a replay carries an old one).
        Session requests store the nonce on disk, so replays fail even after a restart; the
        messages repeated every few seconds during a session keep it in memory only.
        """
        entry = self.paired.data.get(client)
        if not entry or not entry.get("key") or signature is None:
            return "bad"
        if not siphash.verify(bytes.fromhex(entry["key"]), body + signature):
            return "bad"
        if nonce <= entry.get("nonce", 0):
            return "replay"
        entry["nonce"] = nonce
        if persist:
            self.paired.save()
        return "ok"

    def _session_message_ok(self, fields, body, signature):
        session = self.session
        if not session or session["client"] != fields["client_id"].hex():
            return False
        if session["key"]:
            return self._verify(session["client"], body, signature, fields["nonce"], persist=False) == "ok"
        return True

    def _follow_video_address(self, address):
        """A router may move the 3DS to another public port: keep sending video where it is."""
        if self.session["video"] != address:
            self.session["video"] = address
            self.video.target = address
            self.report(f"Video now to {address[0]}:{address[1]}")

    def _answer_discovery(self, fields, address):
        client = fields["client_id"].hex()
        flags = 0
        if client in self.paired.data:
            flags |= protocol.FLAG_PAIRED
        if self.session and self.session["client"] != client:
            flags |= protocol.FLAG_BUSY
        self.sock.sendto(protocol.discovery_answer(self.host_id, self.settings.data["name"], flags), address)

    def _reply(self, magic, address, reason=0):
        self.sock.sendto(protocol.session_reply(magic, reason), address)

    def _handle_request(self, fields, address, body, signature):
        client = fields["client_id"].hex()
        local = protocol.is_local(address[0])
        if signature is not None:
            verdict = self._verify(client, body, signature, fields["nonce"])
            if verdict != "ok":
                # A replay, or a bad signature from outside, gets no answer at all. A bad
                # signature at home usually means the PC forgot this 3DS: tell it to pair again.
                if verdict == "bad" and local:
                    self._reply(b"OCSD", address, protocol.DENIED_BAD_KEY)
                self._log_rejected(address, "replayed request" if verdict == "replay"
                                   else "unknown console or wrong key")
                return
        elif not local:
            self._reply(b"OCSD", address, protocol.DENIED_PAIR_AT_HOME)
            self._log_rejected(address, "unsigned request from outside (pair at home first)")
            return
        signed = signature is not None

        if self.session and self.session["client"] != client:
            self._reply(b"OCSD", address, protocol.DENIED_BUSY)
            return
        denied_at = self.denied.get(client)
        if denied_at and time.monotonic() - denied_at < DENY_MEMORY_S:
            self._reply(b"OCSD", address, protocol.DENIED_PAIRING)
            return
        if client not in self.paired.data:
            if not self.pending or self.pending["client"] != client:
                self.report(f"3DS '{fields['name']}' asks to pair, PIN {fields['pin']:04d}")
            self.pending = {"client": client, "name": fields["name"], "pin": fields["pin"],
                            "address": address[0], "since": time.monotonic()}
            self._reply(b"OCPN", address)
            return

        # Paired. An unsigned request (local network only) means the 3DS has no key yet:
        # it gets one with the answer. The 3DS repeats OCSR until it gets OCSA.
        entry = self.paired.data[client]
        if not entry.get("key"):
            entry["key"] = os.urandom(protocol.KEY_SIZE).hex()
            self.paired.save()
        if not self.session:
            self._start_session(client, fields, address, signed)
        elif self.session["video"] != address:
            self._follow_video_address(address)
        key = None if signed else bytes.fromhex(entry["key"])
        public = self.router.public if local else None
        self.sock.sendto(protocol.session_accept(self.host_id, key, public), address)

    def _start_session(self, client, fields, address, signed):
        settings = self.settings.data
        remote = not protocol.is_local(address[0])
        requested = fields["bitrate_kbps"]
        bitrate = requested or settings["remote_bitrate_kbps" if remote else "bitrate_kbps"]
        # A bitrate the 3DS asks for (tests) stays fixed; otherwise the setting is the highest.
        self.rate = None
        if settings["auto_quality"] and not requested:
            start = min(bitrate, REMOTE_START_KBPS) if remote else bitrate
            self.rate = RateControl(bitrate, start, time.monotonic())
            bitrate = self.rate.kbps
        key = bytes.fromhex(self.paired.data[client]["key"]) if signed else None
        self.session = {"client": client, "name": fields["name"], "video": address, "audio": None,
                        "started": time.monotonic(), "bitrate_kbps": bitrate, "restarts": [],
                        "key": key, "remote": remote, "retry_at": None}
        self.paired.data[client]["name"] = fields["name"]
        self.paired.save()
        self.gamepad.allowed_ip = address[0]
        self.gamepad.last_packet = None
        self.gamepad.rtt_ms = 0  # the last session's value would skew the automatic quality
        self.video.start(address, bitrate, settings["monitor"])
        if settings["audio"] and fields["audio"]:
            self.audio.start()
        where = "from outside the home network" if self.session["remote"] else "on the local network"
        security = "signed" if key else "unsigned (receiving its key)"
        self.report(f"Session started with '{fields['name']}' ({address[0]}, {where}, {security})")

    def _handle_audio_punch(self, address):
        if self.session["audio"] != address:
            self.session["audio"] = address
            self.audio.target = address
            self.report(f"Audio to {address[0]}:{address[1]}")

    def _check_session(self):
        with self.lock:
            if not self.session:
                return
            now = time.monotonic()
            last = self.gamepad.last_packet
            timeout = REMOTE_TIMEOUT_S if self.session["remote"] else SESSION_TIMEOUT_S
            if now - self.session["started"] > STARTUP_GRACE_S and (last is None or now - last > timeout):
                self.stop_session(f"no input from the 3DS for {timeout:.0f} seconds")
            elif not self.video.running():
                session = self.session
                if session["retry_at"] is None:
                    error = self.video.error() or "no error message"
                    restarts = [t for t in session["restarts"] if now - t < RESTART_WINDOW_S]
                    if len(restarts) >= RESTART_LIMIT:
                        self.stop_session(f"ffmpeg kept stopping: {error}")
                        return
                    delay = RESTART_DELAYS_S[min(len(restarts), len(RESTART_DELAYS_S) - 1)]
                    restarts.append(now)
                    session["restarts"] = restarts
                    session["retry_at"] = now + delay
                    self.report(f"ffmpeg stopped ({error.splitlines()[0]}); restarting in {delay:g} s")
                elif now >= session["retry_at"]:
                    session["retry_at"] = None
                    self.video.start(session["video"], session["bitrate_kbps"], self.settings.data["monitor"])
            elif self.rate:
                rtt = self.gamepad.rtt_ms
                kbps = self.rate.update(rtt, last, now)
                if kbps:
                    change = "lowered" if kbps < self.session["bitrate_kbps"] else "raised"
                    self.report(f"Video quality {change} to {kbps} kbps (round trip {rtt} ms)")
                    self.session["bitrate_kbps"] = kbps
                    self.video.start(self.session["video"], kbps, self.settings.data["monitor"])

    # --- actions from the user interface -------------------------------------------------

    def stop_session(self, reason="stopped from the PC"):
        with self.lock:
            if not self.session:
                return
            self.video.stop()
            self.audio.stop()
            self.audio.target = None
            self.gamepad.allowed_ip = ""
            self.report(f"Session with '{self.session['name']}' ended: {reason}")
            self.session = None

    def answer_pairing(self, approve: bool):
        with self.lock:
            if not self.pending:
                return
            client, name = self.pending["client"], self.pending["name"]
            if approve:
                self.paired.data[client] = {"name": name, "since": time.strftime("%Y-%m-%d %H:%M")}
                self.paired.save()
                self.denied.pop(client, None)
                self.report(f"Paired with '{name}'")
            else:
                self.denied[client] = time.monotonic()
                self.report(f"Pairing with '{name}' rejected")
            self.pending = None

    def unpair(self, client):
        with self.lock:
            if self.session and self.session["client"] == client:
                self.stop_session("unpaired")
            if self.paired.data.pop(client, None) is not None:
                self.paired.save()
                self.report("3DS unpaired")

    def update_settings(self, changes):
        with self.lock:
            allowed = {k: type(DEFAULT_SETTINGS[k]) for k in DEFAULT_SETTINGS}
            for key, value in changes.items():
                if key in allowed:
                    self.settings.data[key] = allowed[key](value)
            self.settings.save()
            self.gamepad.set_sensitivity(self.settings.data["touch_radius"], self.settings.data["touch_curve"])
            if "remote_play" in changes:
                if self.settings.data["remote_play"]:
                    self.router.start()
                else:
                    self.router.stop()

    def close(self):
        """Stops streaming and closes the router port."""
        self.stop_session("the host was closed")
        self.router.stop()

    # --- state for the user interface ----------------------------------------------------

    def snapshot(self):
        with self.lock:
            now = time.monotonic()
            session = None
            if self.session:
                last = self.gamepad.last_packet
                session = {
                    "name": self.session["name"],
                    "address": self.session["video"][0],
                    "seconds": round(now - self.session["started"]),
                    "bitrate_kbps": self.session["bitrate_kbps"],
                    "auto_quality": self.rate is not None,
                    "audio": self.session["audio"] is not None and self.audio.running,
                    "audio_packets": self.audio.packets,
                    "input_age_ms": None if last is None else round((now - last) * 1000),
                    "rtt_ms": self.gamepad.rtt_ms,
                    "remote": self.session["remote"],
                    "input_stats": self.input_stats,
                    "signed": self.session["key"] is not None,
                }
            pending = None
            if self.pending and now - self.pending["since"] < 10:
                pending = {k: self.pending[k] for k in ("name", "pin", "address")}
            return {
                "host": {"name": self.settings.data["name"], "id": self.host_id.hex(),
                         "ffmpeg": VideoSender.available()},
                "remote": {"state": self.router.state, "error": self.router.error,
                           "public": f"{self.router.public[0]}:{self.router.public[1]}" if self.router.public else None},
                "session": session,
                "pending": pending,
                "paired": [{"id": k, "name": v.get("name"), "since": v.get("since")}
                           for k, v in self.paired.data.items()],
                "settings": self.settings.data,
                "log": list(self.log)[-40:],
            }
