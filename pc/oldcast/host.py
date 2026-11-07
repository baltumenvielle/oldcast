"""Session logic of the OldCast host: discovery, pairing and streaming (docs/protocol.md).

Everything the user interface shows comes from Host.snapshot(), and everything it can do
goes through the public methods of Host.
"""
import json
import os
import socket
import threading
import time
from collections import deque

from . import protocol
from .audio import AudioSender
from .gamepad import GamepadServer
from .video import VideoSender

SESSION_TIMEOUT_S = 3.0  # no input packets for this long ends the session
STARTUP_GRACE_S = 6.0    # the 3DS needs a moment to start sending input after OCSA
DENY_MEMORY_S = 30.0     # a rejected 3DS is answered OCSD for this long

DEFAULT_SETTINGS = {
    "name": socket.gethostname(),
    "bitrate_kbps": 2000,
    "monitor": 0,
    "audio": True,
    "touch_radius": 90.0,
    "touch_curve": 1.8,
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

        self.video = VideoSender(report=self.report)
        self.audio = AudioSender(report=self.report)
        self.gamepad = GamepadServer(radius=self.settings.data["touch_radius"],
                                     curve=self.settings.data["touch_curve"], report=self.report)
        self.gamepad.allowed_ip = ""  # no session: the controller ignores every packet

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", protocol.CONTROL_PORT))
        self.sock.settimeout(0.5)

    # --- lifecycle ---------------------------------------------------------------------

    def start(self):
        self.gamepad.start()
        threading.Thread(target=self._control_loop, daemon=True).start()
        self.report(f"Host '{self.settings.data['name']}' ready on UDP {protocol.CONTROL_PORT}")
        if not VideoSender.available():
            self.report("ffmpeg was not found: install it and add it to the PATH")

    def report(self, message):
        line = f"{time.strftime('%H:%M:%S')}  {message}"
        with self.lock:  # several threads report; keep their lines whole
            self.log.append(line)
            print(line, flush=True)

    # --- control port --------------------------------------------------------------------

    def _control_loop(self):
        while True:
            try:
                data, address = self.sock.recvfrom(2048)
            except socket.timeout:
                self._check_session()
                continue
            except OSError:
                continue
            magic, fields = protocol.parse(data)
            with self.lock:
                if magic == b"OCDQ":
                    self._answer_discovery(fields, address)
                elif magic == b"OCSR":
                    self._handle_request(fields, address)
                elif magic == b"OCAP":
                    self._handle_audio_punch(fields, address)
                elif magic == b"OCSE":
                    self._handle_end(fields)
            self._check_session()

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

    def _handle_request(self, fields, address):
        client = fields["client_id"].hex()
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

        # Paired: start (or keep) the session. The 3DS repeats OCSR until it gets OCSA.
        if not self.session:
            self._start_session(client, fields, address)
        self._reply(b"OCSA", address)

    def _start_session(self, client, fields, address):
        settings = self.settings.data
        bitrate = fields["bitrate_kbps"] or settings["bitrate_kbps"]
        self.session = {"client": client, "name": fields["name"], "video": address, "audio": None,
                        "started": time.monotonic(), "bitrate_kbps": bitrate}
        self.paired.data[client]["name"] = fields["name"]
        self.paired.save()
        self.gamepad.allowed_ip = address[0]
        self.gamepad.last_packet = None
        self.video.start(address[0], address[1], bitrate, settings["monitor"])
        if settings["audio"] and fields["audio"]:
            self.audio.start()
        self.report(f"Session started with '{fields['name']}' ({address[0]})")

    def _handle_audio_punch(self, fields, address):
        if self.session and self.session["client"] == fields["client_id"].hex():
            if self.session["audio"] != address:
                self.session["audio"] = address
                self.audio.target = address
                self.report(f"Audio to {address[0]}:{address[1]}")

    def _handle_end(self, fields):
        if self.session and self.session["client"] == fields["client_id"].hex():
            self.stop_session("the 3DS ended it")

    def _check_session(self):
        with self.lock:
            if not self.session:
                return
            now = time.monotonic()
            last = self.gamepad.last_packet
            if now - self.session["started"] > STARTUP_GRACE_S and (last is None or now - last > SESSION_TIMEOUT_S):
                self.stop_session("no input from the 3DS for 3 seconds")
            elif not self.video.running():
                error = self.video.error()
                self.stop_session(f"ffmpeg stopped: {error}" if error else "ffmpeg stopped")

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
                    "audio": self.session["audio"] is not None and self.audio.running,
                    "audio_packets": self.audio.packets,
                    "input_age_ms": None if last is None else round((now - last) * 1000),
                    "rtt_ms": self.gamepad.rtt_ms,
                }
            pending = None
            if self.pending and now - self.pending["since"] < 10:
                pending = {k: self.pending[k] for k in ("name", "pin", "address")}
            return {
                "host": {"name": self.settings.data["name"], "id": self.host_id.hex(),
                         "ffmpeg": VideoSender.available()},
                "session": session,
                "pending": pending,
                "paired": [{"id": k, **v} for k, v in self.paired.data.items()],
                "settings": self.settings.data,
                "log": list(self.log)[-40:],
            }
