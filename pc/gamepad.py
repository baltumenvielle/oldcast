"""Virtual Xbox 360 controller driven by the 3DS. The implementation is pc/oldcast/gamepad.py.

    pc\\.venv\\Scripts\\python.exe pc\\gamepad.py [--radius 90] [--curve 1.8] [--log latency.csv]
"""
from oldcast.gamepad import main, touch_stick, video_latency_ms  # noqa: F401

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
