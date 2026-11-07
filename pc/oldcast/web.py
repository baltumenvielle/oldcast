"""Local web interface of the host, served on 127.0.0.1 only.

The page (web/index.html) polls GET /api/state and sends actions as POST requests with a
JSON body. Binding to the loopback address keeps it unreachable from the network.
"""
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WEB_PORT = 47807
STATIC = os.path.join(os.path.dirname(__file__), "web")
CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".svg": "image/svg+xml", ".png": "image/png",
                 ".ico": "image/x-icon"}


def make_handler(host):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # keep the console for the host's own messages

        def _send(self, status, body, content_type="application/json"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/api/state":
                self._send(200, json.dumps(host.snapshot()).encode())
                return
            name = "index.html" if self.path in ("/", "/index.html") else self.path.lstrip("/")
            path = os.path.normpath(os.path.join(STATIC, name))
            if not path.startswith(STATIC) or not os.path.isfile(path):
                self._send(404, b"not found", "text/plain")
                return
            with open(path, "rb") as f:
                body = f.read()
            self._send(200, body, CONTENT_TYPES.get(os.path.splitext(path)[1], "application/octet-stream"))

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except ValueError:
                self._send(400, b'{"error": "bad json"}')
                return
            if self.path == "/api/pairing":
                host.answer_pairing(bool(body.get("approve")))
            elif self.path == "/api/stop":
                host.stop_session()
            elif self.path == "/api/unpair":
                host.unpair(str(body.get("id", "")))
            elif self.path == "/api/settings":
                host.update_settings(body)
            else:
                self._send(404, b'{"error": "unknown action"}')
                return
            self._send(200, json.dumps(host.snapshot()).encode())

    return Handler


def start(host, port=WEB_PORT):
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(host))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f"http://127.0.0.1:{port}/"
