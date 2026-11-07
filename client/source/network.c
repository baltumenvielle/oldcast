#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "frame_queue.h"
#include "input.h"

#define UDP_BUF_SIZE 2048
#define NET_STACK_SIZE (32 * 1024)
#define MAX_READS_PER_WAKEUP 16

NetStats g_net_stats;

static int s_video_sock = -1;
static int s_input_sock = -1;
static Thread s_thread;
static volatile bool s_run;
static TsDemux s_demux;

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
        // Button state goes back to whoever is sending the video.
        input_set_target(sender.sin_addr.s_addr);
        g_net_stats.packets++;
        g_net_stats.bytes += (u32)got;
        ts_demux_feed(&s_demux, buf, (size_t)got);
    }
}

static void net_thread(void *arg)
{
    (void)arg;
    const u64 period = (u64)(INPUT_PERIOD_MS * CPU_TICKS_PER_MSEC);
    u64 next_tick = svcGetSystemTick();
    struct pollfd fds[2] = {
        {.fd = s_video_sock, .events = POLLIN},
        {.fd = s_input_sock, .events = POLLIN},
    };

    while (s_run) {
        u64 now = svcGetSystemTick();
        if (now >= next_tick) {
            input_tick();
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
        fds[0].revents = fds[1].revents = 0;
        int ready = poll(fds, 2, timeout_ms);
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
    }
}

bool net_start(s32 prio, RcvBufInfo *rcvbuf)
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
    input_close();
    s_input_sock = -1;
    if (s_video_sock >= 0) {
        close(s_video_sock);
        s_video_sock = -1;
    }
}

const TsDemux *net_demux(void)
{
    return &s_demux;
}
