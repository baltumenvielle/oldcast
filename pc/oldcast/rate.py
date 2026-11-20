"""Automatic video quality: fits the bitrate to what the path to the 3DS can carry.

When the stream needs more than the connection delivers (a weak Wi-Fi, a slow home
connection), packets wait in a queue somewhere on the way, and everything arrives later and
later until input stops getting through. The 3DS measures the round trip of its input
packets (the PC echoes each one) and reports it in the next packet. That round trip is the
signal: it stays near its usual value while the connection keeps up, and climbs as soon as a
queue builds.

The controller lowers the bitrate one step when the round trip stays well above the lowest
one seen in the session, and raises it one step after a long enough calm period. A step up
that causes congestion soon after makes the next attempt wait twice as long. Each change
restarts ffmpeg, which costs a fraction of a second of video, so changes are kept rare.
"""

LADDER_KBPS = (500, 700, 1000, 1500, 2000, 2500, 3000)
MIN_KBPS = LADDER_KBPS[0]

CONGESTED_MS = 250      # round trip this far above the session's lowest means a queue
CONGESTED_FOR_S = 1.0   # ...for this long, so a single late echo does not count
SILENCE_S = 1.0         # no input at all for this long also counts as congestion
SETTLE_S = 4.0          # after a change, the queue needs time to drain before judging again
UP_WAIT_S = 15.0        # calm time before trying a higher bitrate
UP_WAIT_MAX_S = 120.0
UP_FAILED_S = 15.0      # congestion this soon after a step up means that step was too much


class RateControl:
    """Decides the bitrate of one session. Pure logic: the host feeds it and applies changes."""

    def __init__(self, max_kbps, start_kbps, now):
        self.max_kbps = max_kbps
        self.steps = sorted({k for k in LADDER_KBPS if k < max_kbps} | {max_kbps})
        self.kbps = max(k for k in self.steps if k <= max(start_kbps, self.steps[0]))
        self.lowest_rtt = None
        self.congested_since = None
        self.changed_at = now
        self.last_congestion = now
        self.raised_at = None
        self.up_wait = UP_WAIT_S

    def update(self, rtt_ms, last_input, now):
        """Takes the latest round trip reported by the 3DS (0 if none yet) and the time of its
        latest input packet. Returns the new bitrate when it should change, else None."""
        if rtt_ms and (self.lowest_rtt is None or rtt_ms < self.lowest_rtt):
            self.lowest_rtt = rtt_ms
        congested = ((last_input is not None and now - last_input > SILENCE_S) or
                     (self.lowest_rtt is not None and rtt_ms > self.lowest_rtt + CONGESTED_MS))
        if congested:
            self.last_congestion = now
            if self.congested_since is None:
                self.congested_since = now
        else:
            self.congested_since = None

        if now - self.changed_at < SETTLE_S:
            return None
        index = self.steps.index(self.kbps)
        if self.congested_since is not None and now - self.congested_since >= CONGESTED_FOR_S:
            if index == 0:
                return None
            if self.raised_at is not None and now - self.raised_at < UP_FAILED_S:
                self.up_wait = min(self.up_wait * 2, UP_WAIT_MAX_S)
            self.raised_at = None
            return self._change(self.steps[index - 1], now)
        if index + 1 < len(self.steps) and now - max(self.last_congestion, self.changed_at) >= self.up_wait:
            if self.raised_at is not None:
                self.up_wait = UP_WAIT_S  # the previous step up held: the next one need not wait long
            self.raised_at = now
            return self._change(self.steps[index + 1], now)
        return None

    def _change(self, kbps, now):
        self.kbps = kbps
        self.changed_at = now
        self.congested_since = None
        return kbps
