#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio.h"
#include "common.h"
#include "discovery.h"
#include "frame_queue.h"
#include "input.h"

#define UDP_BUF_SIZE 2048
#define NET_STACK_SIZE (32 * 1024)
#define MAX_READS_PER_WAKEUP 16
#define REQUEST_INTERVAL_MS 500
// No video for this long during an active session: ask for the session again. If the PC
// ended it (or restarted), it answers with a new one; if it is still running, OCSA again.
#define VIDEO_SILENCE_MS 3000

typedef struct __attribute__((packed)) {
    char magic[4]; // "OCSR"
    u8 version;
    u8 reserved[3];
    u8 client_id[8];
    u16 pin;
    u16 bitrate_kbps;
    u8 flags; // bit 0: audio wanted
    u8 reserved2[3];
    char name[16];
} SessionRequestMsg;

typedef struct __attribute__((packed)) {
    char magic[4]; // "OCAP" or "OCSE"
    u8 version;
    u8 reserved[3];
    u8 client_id[8];
} ClientMsg;

_Static_assert(sizeof(SessionRequestMsg) == 40, "OCSR is 40 bytes");
_Static_assert(sizeof(ClientMsg) == 16, "OCAP/OCSE are 16 bytes");

NetStats g_net_stats;

static int s_video_sock = -1;
static int s_audio_sock = -1;
static int s_input_sock = -1;
static Thread s_thread;
static volatile bool s_run;
static TsDemux s_demux;

static bool s_has_request;
static SessionRequest s_request;
static volatile SessionState s_state;
static volatile u8 s_deny_reason;
static u64 s_next_request;
static u64 s_next_punch;
static volatile u64 s_last_video;

static void send_to_host(int sock, const void *msg, size_t size)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(CONTROL_PORT);
    to.sin_addr.s_addr = s_request.host_addr;
    sendto(sock, msg, size, 0, (struct sockaddr *)&to, sizeof(to));
}

static void send_client_msg(int sock, const char *magic)
{
    ClientMsg m;
    memset(&m, 0, sizeof(m));
    memcpy(m.magic, magic, 4);
    m.version = 1;
    memcpy(m.client_id, client_id(), 8);
    send_to_host(sock, &m, sizeof(m));
}

/**
 * OCSR goes out from the video socket on purpose: the PC sends the video back to the
 * address and port it came from, which also opens the way through a router.
 */
static void send_session_request(void)
{
    SessionRequestMsg m;
    memset(&m, 0, sizeof(m));
    memcpy(m.magic, "OCSR", 4);
    m.version = 1;
    memcpy(m.client_id, client_id(), 8);
    m.pin = s_request.pin;
    m.bitrate_kbps = s_request.bitrate_kbps;
    m.flags = s_request.audio ? 1 : 0;
    memcpy(m.name, s_request.name, sizeof(m.name));
    send_to_host(s_video_sock, &m, sizeof(m));
}

/** Handles a control reply from the PC, which arrives on the video socket. */
static void handle_reply(const u8 *data, size_t size, u32 from)
{
    if (!s_has_request || from != s_request.host_addr || size < 8 || data[4] != 1)
        return;
    if (memcmp(data, "OCSA", 4) == 0) {
        s_state = SESSION_ACTIVE;
    } else if (memcmp(data, "OCPN", 4) == 0) {
        if (s_state != SESSION_ACTIVE)
            s_state = SESSION_PAIRING;
    } else if (memcmp(data, "OCSD", 4) == 0) {
        s_deny_reason = data[5];
        s_state = SESSION_DENIED;
    }
}

/** Repeats the messages that have no answer yet. Called every input tick. */
static void session_tick(u64 now)
{
    if (!s_has_request)
        return;
    const u64 interval = (u64)(REQUEST_INTERVAL_MS * CPU_TICKS_PER_MSEC);
    if ((s_state == SESSION_REQUESTING || s_state == SESSION_PAIRING) && now >= s_next_request) {
        send_session_request();
        s_next_request = now + interval;
    }
    // Until audio flows, keep telling the PC where to send it.
    if (s_state == SESSION_ACTIVE && s_last_video && TICKS_TO_MS(now - s_last_video) > VIDEO_SILENCE_MS) {
        s_state = SESSION_REQUESTING;
        s_next_request = 0;
        s_last_video = now; // wait another period before asking again
        g_net_stats.rerequests++;
    }
    if (s_state == SESSION_ACTIVE && s_audio_sock >= 0 && s_request.audio && g_audio_stats.packets == 0 &&
        now >= s_next_punch) {
        send_client_msg(s_audio_sock, "OCAP");
        s_next_punch = now + interval;
    }
}

static u8 *on_frame_begin(void *user, size_t *cap)
{
    (void)user;
    return fq_begin(cap);
}

static void on_frame_end(void *user, size_t len, bool corrupt)
{
    (void)user;
    fq_commit(len, corrupt);
}

/*
 * Every frame arrives as a burst of datagrams (7 for an average frame, more than 15
 * for a keyframe). The SOC service gives new sockets a 4416-byte buffer, three
 * datagrams, so the tail of almost every frame used to be dropped. Sizes are tried
 * from largest to smallest because the service rejects 128 KB with ENOMEM.
 */
static void enlarge_receive_buffer(int sock, RcvBufInfo *info)
{
    static const int sizes[] = {128 * 1024, 65536, 49152, 32768, 24576, 16384};

    info->last_errno = 0;
    socklen_t len = sizeof(info->default_size);
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &info->default_size, &len) < 0)
        info->default_size = -errno;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int size = sizes[i];
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0)
            break;
        info->last_errno = errno;
    }

    len = sizeof(info->final_size);
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &info->final_size, &len) < 0)
        info->final_size = -errno;
}

/** Reads every datagram waiting on the video socket into the demuxer. */
static void read_video(void)
{
    static u8 buf[UDP_BUF_SIZE] __attribute__((aligned(4)));
    struct sockaddr_in sender;

    g_net_stats.wakeups++;
    for (int i = 0; i < MAX_READS_PER_WAKEUP; i++) {
        socklen_t sender_len = sizeof(sender);
        ssize_t got = recvfrom(s_video_sock, buf, sizeof(buf), 0, (struct sockaddr *)&sender, &sender_len);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                g_net_stats.eagain++;
            else
                g_net_stats.error = errno;
            return;
        }
        // Control replies from the PC share this socket; video datagrams start with 0x47.
        if (got >= 4 && buf[0] == 'O' && buf[1] == 'C') {
            handle_reply(buf, (size_t)got, sender.sin_addr.s_addr);
            continue;
        }
        // Button state goes back to whoever is sending the video.
        input_set_target(sender.sin_addr.s_addr);
        s_last_video = svcGetSystemTick();
        g_net_stats.packets++;
        g_net_stats.bytes += (u32)got;
        ts_demux_feed(&s_demux, buf, (size_t)got);
    }
}

/** Hands every audio packet waiting on the audio socket to the player. */
static void read_audio(void)
{
    static u8 buf[UDP_BUF_SIZE] __attribute__((aligned(4)));
    for (int i = 0; i < MAX_READS_PER_WAKEUP; i++) {
        ssize_t got = recv(s_audio_sock, buf, sizeof(buf), 0);
        if (got <= 0)
            return;
        audio_handle_packet(buf, (size_t)got);
    }
}

/** Opens a non-blocking UDP socket bound to `port`. Returns -1 on failure. */
static int open_udp(u16 port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(sock);
        errno = saved;
        return -1;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    return sock;
}

static void net_thread(void *arg)
{
    (void)arg;
    const u64 period = (u64)(INPUT_PERIOD_MS * CPU_TICKS_PER_MSEC);
    u64 next_tick = svcGetSystemTick();
    // A negative fd is ignored by poll(), so audio simply drops out when it is disabled.
    struct pollfd fds[3] = {
        {.fd = s_video_sock, .events = POLLIN},
        {.fd = s_input_sock, .events = POLLIN},
        {.fd = s_audio_sock, .events = POLLIN},
    };

    while (s_run) {
        u64 now = svcGetSystemTick();
        if (now >= next_tick) {
            input_tick();
            audio_tick();
            session_tick(now);
            next_tick += period;
            if (next_tick <= now) {
                g_net_stats.late_ticks++;
                next_tick = now + period;
            }
        }

        // Wait for data, but never past the next input tick.
        int timeout_ms = (int)((next_tick - now) / CPU_TICKS_PER_MSEC) + 1;
        if (timeout_ms > INPUT_PERIOD_MS)
            timeout_ms = INPUT_PERIOD_MS;
        fds[0].revents = fds[1].revents = fds[2].revents = 0;
        int ready = poll(fds, 3, timeout_ms);
        if (ready < 0) {
            g_net_stats.error = errno;
            svcSleepThread(1000000LL);
            continue;
        }
        if (ready == 0)
            continue;

        // Any revents bit counts as "readable": the sockets are non-blocking, so an
        // extra read costs nothing, while filtering on POLLIN could drop data silently.
        if (fds[0].revents) {
            g_net_stats.revents = fds[0].revents;
            read_video();
        }
        if (fds[1].revents)
            input_read_echoes();
        if (fds[2].revents)
            read_audio();
    }
}

bool net_start(s32 prio, RcvBufInfo *rcvbuf, const SessionRequest *request)
{
    memset(&g_net_stats, 0, sizeof(g_net_stats));
    memset(rcvbuf, 0, sizeof(*rcvbuf));
    rcvbuf->default_size = rcvbuf->final_size = -1;

    s_video_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_video_sock < 0)
        return false;
    enlarge_receive_buffer(s_video_sock, rcvbuf);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(VIDEO_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_video_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail;
    fcntl(s_video_sock, F_SETFL, fcntl(s_video_sock, F_GETFL, 0) | O_NONBLOCK);

    s_input_sock = input_open();
    if (s_input_sock < 0)
        goto fail;

    s_has_request = request != NULL;
    s_state = SESSION_NONE;
    s_deny_reason = 0;
    if (request) {
        s_request = *request;
        s_state = SESSION_REQUESTING;
        s_next_request = s_next_punch = 0;
        s_last_video = 0;
        input_set_target(request->host_addr);
    }

    // Audio is optional: without the DSP firmware the stream works silently.
    if (audio_available()) {
        s_audio_sock = open_udp(AUDIO_PORT);
        // The default 4416-byte buffer holds only 8 audio packets: a keyframe arriving at
        // the same time kept the thread busy long enough to lose some of them.
        if (s_audio_sock >= 0) {
            RcvBufInfo audio_rcvbuf;
            enlarge_receive_buffer(s_audio_sock, &audio_rcvbuf);
        }
        audio_reset();
    }

    ts_demux_init(&s_demux, on_frame_begin, on_frame_end, NULL);
    s_run = true;
    s_thread = threadCreate(net_thread, NULL, NET_STACK_SIZE, prio, 0, false);
    if (!s_thread) {
        errno = ENOMEM;
        goto fail;
    }
    return true;

fail: {
    int saved = errno;
    if (s_audio_sock >= 0)
        close(s_audio_sock);
    s_audio_sock = -1;
    input_close();
    s_input_sock = -1;
    close(s_video_sock);
    s_video_sock = -1;
    errno = saved;
    return false;
}
}

void net_stop(void)
{
    s_run = false;
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    // Tell the PC to stop streaming; sent twice in case one is lost.
    if (s_has_request && s_video_sock >= 0) {
        send_client_msg(s_video_sock, "OCSE");
        send_client_msg(s_video_sock, "OCSE");
    }
    s_has_request = false;
    input_close();
    s_input_sock = -1;
    if (s_audio_sock >= 0) {
        close(s_audio_sock);
        s_audio_sock = -1;
    }
    audio_reset();
    if (s_video_sock >= 0) {
        close(s_video_sock);
        s_video_sock = -1;
    }
}

SessionState net_session_state(void)
{
    return s_state;
}

u8 net_session_deny_reason(void)
{
    return s_deny_reason;
}

bool net_audio_enabled(void)
{
    return s_audio_sock >= 0;
}

const TsDemux *net_demux(void)
{
    return &s_demux;
}
