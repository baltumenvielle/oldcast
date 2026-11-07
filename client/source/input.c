#include "input.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "siphash.h"

#define INPUT_HEARTBEAT_MS 50 // send even without changes, so the PC knows we are alive
#define EXIT_HOLD_MS 1000

#define FLAG_TOUCH 0x01
#define FLAG_PROBE 0x02 // probe_code holds a new latency probe reading

/**
 * Packet sent to the PC, little-endian. Must match PACKET_V2 in pc/oldcast/gamepad.py.
 * In a signed session it is followed by its 8-byte SipHash signature (40 bytes in total).
 * Version 1 was the same without the last four fields (24 bytes).
 */
typedef struct __attribute__((packed)) {
    char magic[4]; // "3DSI"
    u8 version;    // 2
    u8 flags;      // FLAG_*
    u16 seq;
    u32 keys;      // hidKeysHeld()
    s16 cx, cy;    // Circle Pad, about -156..156, positive Y is up
    u16 tx, ty;    // touch position, 0..319 and 0..239
    u32 t_ms;      // 3DS clock; the PC echoes it back to measure the round trip
    u16 probe_code;    // latency probe value read from a shown frame (see latency_probe.h)
    u16 probe_age_ms;  // time from showing that frame to sending this packet
    u16 rtt_ms;        // last measured round trip to the PC, 0 if unknown
    u8 probe_variant;  // A/B test configuration the probed frame was shown with
    u8 reserved;
} InputPacket;

_Static_assert(sizeof(InputPacket) == 32, "the input packet must be 32 bytes");

/** Echo sent back by the PC for every packet. */
typedef struct __attribute__((packed)) {
    char magic[4]; // "3DSE"
    u16 seq;
    u32 t_ms;      // t_ms of the packet being echoed
} EchoPacket;

volatile u32 g_input_sent;
volatile u32 g_input_rtt_ms;

static int s_sock = -1;
static volatile u32 s_target;
static bool s_signed;
static u8 s_key[16];
static volatile bool s_exit;
static volatile u32 s_keys;

static InputPacket s_pkt;
static InputPacket s_last;
static u64 s_last_send;
static u64 s_combo_start;

// Latest probe reading, written by the stream thread, read by the network thread.
static volatile bool s_probe_new;
static volatile u16 s_probe_code;
static volatile u64 s_probe_tick;
static volatile u8 s_probe_variant;

int input_open(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0)
        return -1;
    fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) | O_NONBLOCK);

    memset(&s_pkt, 0, sizeof(s_pkt));
    memcpy(s_pkt.magic, "3DSI", 4);
    s_pkt.version = 2;
    memset(&s_last, 0xFF, sizeof(s_last)); // forces the first send
    s_last_send = 0;
    s_combo_start = 0;
    s_target = 0;
    s_exit = false;
    s_keys = 0;
    s_probe_new = false;
    g_input_sent = 0;
    g_input_rtt_ms = 0;
    return s_sock;
}

void input_close(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

void input_read_echoes(void)
{
    EchoPacket echo;
    while (recv(s_sock, &echo, sizeof(echo), 0) == (ssize_t)sizeof(echo)) {
        if (memcmp(echo.magic, "3DSE", 4) == 0)
            g_input_rtt_ms = (u32)TICKS_TO_MS(svcGetSystemTick()) - echo.t_ms;
    }
}

void input_tick(void)
{
    hidScanInput();
    u32 keys = hidKeysHeld();
    s_keys = keys;
    circlePosition circle;
    hidCircleRead(&circle);
    touchPosition touch = {0, 0};
    if (keys & KEY_TOUCH)
        hidTouchRead(&touch);
    u64 now = svcGetSystemTick();

    if ((keys & (KEY_SELECT | KEY_START)) == (KEY_SELECT | KEY_START)) {
        if (!s_combo_start)
            s_combo_start = now;
        else if (TICKS_TO_MS(now - s_combo_start) >= EXIT_HOLD_MS)
            s_exit = true;
    } else {
        s_combo_start = 0;
    }

    InputPacket *pkt = &s_pkt;
    pkt->flags = (keys & KEY_TOUCH) ? FLAG_TOUCH : 0;
    pkt->keys = keys;
    pkt->cx = circle.dx;
    pkt->cy = circle.dy;
    pkt->tx = touch.px;
    pkt->ty = touch.py;

    bool changed = pkt->keys != s_last.keys || pkt->cx != s_last.cx || pkt->cy != s_last.cy ||
                   pkt->tx != s_last.tx || pkt->ty != s_last.ty || pkt->flags != (s_last.flags & FLAG_TOUCH);
    bool probe = s_probe_new;
    u32 target = s_target;
    if (!target || !(changed || probe || TICKS_TO_MS(now - s_last_send) >= INPUT_HEARTBEAT_MS))
        return;

    if (probe) {
        s_probe_new = false;
        pkt->flags |= FLAG_PROBE;
        pkt->probe_code = s_probe_code;
        pkt->probe_age_ms = (u16)TICKS_TO_MS(now - s_probe_tick);
        pkt->probe_variant = s_probe_variant;
    }
    pkt->seq++;
    pkt->t_ms = (u32)TICKS_TO_MS(now);
    pkt->rtt_ms = (u16)g_input_rtt_ms;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(INPUT_PORT);
    to.sin_addr.s_addr = target;
    u8 out[sizeof(InputPacket) + 8];
    size_t size = sizeof(InputPacket);
    memcpy(out, pkt, size);
    if (s_signed) {
        u64 tag = siphash24(s_key, pkt, sizeof(InputPacket));
        memcpy(out + size, &tag, 8);
        size += 8;
    }
    if (sendto(s_sock, out, size, 0, (struct sockaddr *)&to, sizeof(to)) >= 0)
        g_input_sent++;
    s_last = *pkt;
    s_last_send = now;
}

void input_set_target(u32 addr)
{
    s_target = addr;
}

void input_set_key(const u8 *key)
{
    s_signed = key != NULL;
    if (key)
        memcpy(s_key, key, sizeof(s_key));
}

u32 input_keys_held(void)
{
    return s_keys;
}

bool input_exit_requested(void)
{
    return s_exit;
}

void input_report_probe(u16 code, u64 shown_tick, u8 variant)
{
    s_probe_code = code;
    s_probe_tick = shown_tick;
    s_probe_variant = variant;
    s_probe_new = true;
}
