/**
 * @file input.h
 * @brief Input channel: sends buttons, Circle Pad and touch screen state to the PC.
 *
 * The network thread (network.h) calls input_tick() every INPUT_PERIOD_MS. Each tick
 * samples the HID service and, if something changed (or every 50 ms as a heartbeat),
 * sends a 32-byte UDP packet to the PC that streams the video, on port INPUT_PORT (the
 * host's single port, see docs/protocol.md).
 * pc/gamepad.py turns it into a virtual Xbox 360 controller.
 *
 * There is no separate input thread on purpose: libctru talks to the SOC (network)
 * service through a single session, and the kernel serves one request per session at a
 * time. A send from another thread had to wait whenever the video thread was blocked in
 * poll(), which delayed button packets by up to ~100 ms.
 *
 * The same packets carry latency measurements: the PC echoes every packet so the
 * round-trip time is known, and latency probe readings (latency_probe.h) are forwarded
 * with the next tick.
 *
 * While streaming, the network thread is the only reader of the HID service.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>

#define INPUT_PORT 5007
#define INPUT_PERIOD_MS 8

extern volatile u32 g_input_sent;
/** Last measured round trip to the PC in ms, 0 until the first echo arrives. */
extern volatile u32 g_input_rtt_ms;

/** Creates the input socket. @return its descriptor, or -1. */
int input_open(void);
void input_close(void);

/** Samples the buttons and sends a packet if needed. Call every INPUT_PERIOD_MS. */
void input_tick(void);

/** Reads the PC's echoes waiting on the input socket. Never blocks. */
void input_read_echoes(void);

/** IPv4 address (network byte order) to send to. 0 means not known yet. */
void input_set_target(u32 addr);

/** Signs every packet with this 16-byte key from now on; NULL sends unsigned packets. */
void input_set_key(const u8 *key);

/** Keys held at the last tick (hidKeysHeld), for screens shown while streaming. */
u32 input_keys_held(void);

/** True once SELECT+START have been held for a second: the user wants to stop. */
bool input_exit_requested(void);

/**
 * Forwards a latency probe reading to the PC with the next tick.
 * @param code        Value read from the frame.
 * @param shown_tick  svcGetSystemTick() when that frame was submitted to the screen.
 * @param variant     Configuration the frame was shown with, for A/B tests (0 or 1).
 */
void input_report_probe(u16 code, u64 shown_tick, u8 variant);
