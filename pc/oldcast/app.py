"""Starts the OldCast host and opens its window.

    pc\\.venv\\Scripts\\python.exe -m oldcast            (from the pc folder)
    pc\\.venv\\Scripts\\python.exe -m oldcast --no-window

The window is the local web interface opened in Microsoft Edge's app mode: a plain window
with the OldCast icon and no browser controls. It uses its own Edge profile, so it does not
touch the user's browser. Closing the window stops the host.
"""
import argparse
import os
import shutil
import subprocess
import time

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
    return subprocess.Popen([edge, f"--app={url}", f"--user-data-dir={profile}", "--window-size=820,900",
                             "--no-first-run", "--no-default-browser-check"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-window", action="store_true", help="run without opening the interface")
    parser.add_argument("--port", type=int, default=web.WEB_PORT, help="port of the local web interface")
    args = parser.parse_args()

    host = Host()
    host.start()
    url = web.start(host, args.port)
    host.report(f"Interface at {url}")

    window = None if args.no_window else open_window(url)
    try:
        while True:
            time.sleep(0.5)
            if window is not None and window.poll() is not None:
                break  # the user closed the window
    except KeyboardInterrupt:
        pass
    finally:
        host.stop_session("the host was closed")


if __name__ == "__main__":
    main()
