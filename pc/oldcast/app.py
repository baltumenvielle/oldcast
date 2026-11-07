"""Starts the OldCast host and opens its window.

    pc\\.venv\\Scripts\\python.exe -m oldcast            (from the pc folder)
    pc\\.venv\\Scripts\\python.exe -m oldcast --no-window

The window is the local web interface opened in Microsoft Edge's app mode: a plain window
with the OldCast icon and no browser controls. It uses its own Edge profile, so it does not
touch the user's browser. Closing the window stops the host.

The page tells the host when its window closes. As a fallback, the host also stops when
the page has not polled for a minute. Watching the Edge process does not work: when an
OldCast window is already open, Edge hands the new one to that process and the launched
process exits at once.
"""
import argparse
import os
import shutil
import subprocess
import time

CLOSING_GRACE_S = 3.0   # after the page says it is closing; a reload polls again before this
WINDOW_GONE_S = 60.0    # no poll at all for this long: the window is gone

from . import web
from .host import Host, config_dir

EDGE_PATHS = [
    os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
    os.path.join(os.environ.get("ProgramFiles", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
]


def open_window(url):
    """Opens the interface in an app-mode window. Returns the process, or None."""
    edge = next((p for p in EDGE_PATHS if os.path.isfile(p)), None) or shutil.which("msedge")
    if not edge:
        import webbrowser
        webbrowser.open(url)
        return None
    profile = os.path.join(config_dir(), "window")
    # The page must keep polling while minimized, so Edge must not throttle its timers.
    return subprocess.Popen([edge, f"--app={url}", f"--user-data-dir={profile}", "--window-size=820,900",
                             "--no-first-run", "--no-default-browser-check",
                             "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
                             "--disable-backgrounding-occluded-windows"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-window", action="store_true", help="run without opening the interface")
    parser.add_argument("--port", type=int, default=web.WEB_PORT, help="port of the local web interface")
    args = parser.parse_args()

    host = Host()
    host.start()
    url = web.start(host, args.port)
    host.report(f"Interface at {url}")

    if not args.no_window:
        open_window(url)
    try:
        while True:
            time.sleep(0.5)
            if args.no_window or web.last_poll is None:
                continue
            now = time.monotonic()
            closed = web.closing_at is not None and web.closing_at >= web.last_poll and \
                now - web.closing_at > CLOSING_GRACE_S
            if closed or now - web.last_poll > WINDOW_GONE_S:
                break  # the user closed the window
    except KeyboardInterrupt:
        pass
    finally:
        host.close()


if __name__ == "__main__":
    main()
