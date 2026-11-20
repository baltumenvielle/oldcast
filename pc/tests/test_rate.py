"""Test of the automatic video quality (oldcast/rate.py) with simulated round trips.

Run from the pc folder: python tests\\test_rate.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from oldcast.rate import RateControl, UP_WAIT_S  # noqa: E402

failures = 0


def check(name, condition, detail=""):
    global failures
    print(f"{'ok  ' if condition else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    failures += not condition


def run(rate, seconds, rtt, start, step=0.05, silent=False):
    """Feeds `rate` input packets every `step` seconds; `rtt` is a number or a function of time.
    Returns the changes made, as (time, kbps), and the time reached."""
    changes = []
    now = start
    last_input = start
    while now < start + seconds:
        if not silent:
            last_input = now
        value = rtt(now) if callable(rtt) else rtt
        kbps = rate.update(value, last_input, now)
        if kbps:
            changes.append((round(now, 2), kbps))
        now += step
    return changes, now


# A calm connection: start at 1 Mbps, climb one step at a time to the highest, then stay.
rate = RateControl(2000, 1000, 0.0)
check("starts at the requested step", rate.kbps == 1000)
changes, t = run(rate, 60, 30, 0.0)
check("climbs to the highest when calm", [k for _, k in changes] == [1500, 2000], str(changes))
check("waits before each step up", changes[0][0] >= UP_WAIT_S, str(changes))

# A queue builds: the round trip climbs well above the lowest seen.
changes, t = run(rate, 3, 800, t)
check("lowers one step under congestion", [k for _, k in changes] == [1500], str(changes))
changes, t = run(rate, 2, 800, t)
check("waits for the queue to drain before judging again", changes == [], str(changes))
changes, t = run(rate, 30, 800, t)
check("keeps lowering while congested, down to the lowest", [k for _, k in changes] == [1000, 700, 500],
      str(changes))

# A single late echo is not congestion.
rate = RateControl(2000, 2000, 0.0)
changes, t = run(rate, 10, lambda now: 600 if 5.0 <= now < 5.5 else 30, 0.0)
check("ignores a short spike", changes == [], str(changes))

# No input at all for a while also counts.
changes, t = run(rate, 3, 30, t, silent=True)
check("treats silence as congestion", [k for _, k in changes] == [1500], str(changes))

# A step up that brings congestion back makes the next attempt wait twice as long.
rate = RateControl(2000, 1000, 0.0)
changes, t = run(rate, UP_WAIT_S + 1, 30, 0.0)
check("steps up after the calm period", [k for _, k in changes] == [1500], str(changes))
changes, t = run(rate, 6, 800, t)
check("steps back down when the higher step congests", [k for _, k in changes] == [1000], str(changes))
calm_from = t
changes, t = run(rate, 2 * UP_WAIT_S + 2, 30, t)
check("waits longer before the next try", len(changes) == 1 and changes[0][0] - calm_from > 2 * UP_WAIT_S - 0.1,
      f"{changes}, calm from {calm_from:.2f}")

# The setting is the ceiling, even when it is not on the ladder.
rate = RateControl(1200, 1200, 0.0)
changes, t = run(rate, 60, 30, 0.0)
check("never goes above the setting", rate.kbps == 1200 and changes == [], str(changes))

sys.exit(1 if failures else 0)
