#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "decoder.h"
#include "frame_queue.h"
#include "input.h"
#include "latency_probe.h"
#include "metrics_log.h"
#include "network.h"
#include "video_out.h"

/** Seconds each configuration stays active in an A/B test. */
#define AB_PERIOD_S 10

/** Display settings that can be compared in an A/B test. */
typedef struct {
    const char *name;
    /**
     * When at least this many newer frames are already waiting, the frame that was
     * just decoded is not drawn: drawing it would only add delay.
     */
    u32 skip_show_depth;
    /** Wait for the next VBlank before drawing (C3D_FRAME_SYNCDRAW). */
    bool wait_vblank;
} Variant;

// Measured on 2026-11-07 (see docs/design.md): waiting for VBlank cost 8.6 ms per frame and
// 16 ms of 3DS latency, so both variants draw immediately. Skipping at 1 gave no latency
// gain and dropped 5 frames per second when Wi-Fi delivered frames in pairs, so A is the
// default and B stays available for further A/B tests.
static const Variant VARIANTS[2] = {
    {"A: no vsync, skip at 2", 2, false},
    {"B: no vsync, skip at 1", 1, false},
};

/** Variant used outside A/B tests. */
#define DEFAULT_VARIANT 0

static const char METRICS_HEADER[] =
    "# new session\n"
    "t_s,udp_pkts,kbps,ts_lost,frames_rx,corrupt,decoded,shown,not_shown,dropped,"
    "late_jumps,overflows,dec_avg,dec_max,dec_over_budget,y2r_avg,y2r_max,draw_avg,draw_max,"
    "age_avg,age_max,q_avg,q_max,gap_max,no_pes_len,rx_errno,wakeups,eagain,revents,inputs,"
    "rcvbuf_default,rcvbuf_now,rcvbuf_errno,rtt_ms,probes,late_ticks,variant\n";

/** Measurements of the current one-second window. */
typedef struct {
    TimeStat decode; /**< libavcodec time per frame */
    TimeStat y2r;    /**< YUV->RGB conversion, including waiting for it */
    TimeStat draw;   /**< GPU submit, including any wait for VBlank */
    TimeStat age;    /**< from "frame fully received" to "frame submitted to the screen" */
    u32 decoded;
    u32 shown;
    u32 not_shown;
    u32 over_budget;
    u32 y2r_failed;
    u32 probes; /**< shown frames that carried a valid latency probe */
    u32 queue_sum;
    u32 queue_samples;
    u32 queue_max;
} Window;

/** Cumulative counters as of the previous window, to print per-second deltas. */
typedef struct {
    u32 packets, bytes, lost, received, corrupt, dropped, jumps, overflows;
} Totals;

static Totals read_totals(void)
{
    const TsDemux *demux = net_demux();
    return (Totals){
        .packets = g_net_stats.packets,
        .bytes = g_net_stats.bytes,
        .lost = demux->ts_lost,
        .received = g_fq_stats.received,
        .corrupt = g_fq_stats.corrupt,
        .dropped = g_fq_stats.dropped,
        .jumps = g_fq_stats.late_jumps,
        .overflows = g_fq_stats.overflows,
    };
}

/** Decodes one queued frame and, unless newer frames are waiting, shows it. */
static void process_frame(FqSlot *slot, Window *w, u8 variant_index)
{
    const Variant *variant = &VARIANTS[variant_index];
    u32 depth = fq_depth() - 1; // not counting the frame about to be decoded
    w->queue_sum += depth;
    w->queue_samples++;
    if (depth > w->queue_max)
        w->queue_max = depth;

    u64 t_ready = slot->t_ready;
    u64 t0 = svcGetSystemTick();
    const AVFrame *frame = dec_decode(slot->data, slot->len);
    u64 t1 = svcGetSystemTick();
    fq_release();

    timestat_add(&w->decode, t1 - t0);
    if (TICKS_TO_MS(t1 - t0) > FRAME_BUDGET_MS)
        w->over_budget++;
    if (!frame)
        return;
    w->decoded++;

    if (fq_depth() >= variant->skip_show_depth) {
        w->not_shown++;
        return;
    }
    if (!vo_convert(frame)) {
        w->y2r_failed++;
        return;
    }
    u64 t2 = svcGetSystemTick();
    vo_present(variant->wait_vblank);
    u64 t3 = svcGetSystemTick();
    timestat_add(&w->y2r, t2 - t1);
    timestat_add(&w->draw, t3 - t2);
    timestat_add(&w->age, t3 - t_ready);
    w->shown++;

    u16 code;
    if (probe_read(frame->data[0], frame->linesize[0], frame->width, frame->height, &code)) {
        input_report_probe(code, t3, variant_index);
        w->probes++;
    }
}

static void print_metrics(const Window *w, const Totals *now, const Totals *prev, double secs, u32 gap_max,
                          u32 total_shown, const RcvBufInfo *rcvbuf, const Variant *variant)
{
    struct in_addr ip = {.s_addr = gethostid()};
    const TsDemux *demux = net_demux();
    double kbps = (now->bytes - prev->bytes) * 8.0 / 1000.0 / secs;
    double q_avg = w->queue_samples ? (double)w->queue_sum / w->queue_samples : 0.0;

    // \x1b[1;1H moves the cursor home and \x1b[K clears the rest of each line, so the
    // screen is redrawn in place without flicker.
    printf("\x1b[1;1H");
    printf("%s:%d  SELECT+START 1s: quit\x1b[K\n", inet_ntoa(ip), VIDEO_PORT);
    printf("%s%s\x1b[K\n", variant->name, g_linked ? "  [PC link]" : "");
    printf("rcvbuf %d -> %d  shown %lu\x1b[K\n", rcvbuf->default_size, rcvbuf->final_size, total_shown);
    printf("buttons sent %lu  rtt %lu ms  late %lu\x1b[K\n", g_input_sent, g_input_rtt_ms,
           g_net_stats.late_ticks);
    printf("fps shown     %5.1f\x1b[K\n", w->shown / secs);
    printf("fps decoded   %5.1f\x1b[K\n", w->decoded / secs);
    printf("fps received  %5.1f\x1b[K\n\n", (now->received - prev->received) / secs);
    printf("            avg    max\x1b[K\n");
    printf("decode ms  %5.1f  %5.1f\x1b[K\n", timestat_avg_ms(&w->decode), timestat_max_ms(&w->decode));
    printf("y2r ms     %5.1f  %5.1f\x1b[K\n", timestat_avg_ms(&w->y2r), timestat_max_ms(&w->y2r));
    printf("draw ms    %5.1f  %5.1f\x1b[K\n", timestat_avg_ms(&w->draw), timestat_max_ms(&w->draw));
    printf("3DS ms     %5.1f  %5.1f\x1b[K\n", timestat_avg_ms(&w->age), timestat_max_ms(&w->age));
    printf("queue      %5.1f  %5lu\x1b[K\n\n", q_avg, w->queue_max);
    printf("decode > 33ms   %lu/s\x1b[K\n", w->over_budget);
    printf("arrival gap     %lu ms\x1b[K\n", gap_max);
    printf("UDP %lu pkt/s  %.0f kbps\x1b[K\n", (u32)((now->packets - prev->packets) / secs), kbps);
    printf("TS lost         %lu (+%lu)\x1b[K\n", now->lost, now->lost - prev->lost);
    printf("frames corrupt  %lu\x1b[K\n", now->corrupt);
    printf("frames dropped  %lu (+%lu)\x1b[K\n", now->dropped, now->dropped - prev->dropped);
    printf("jumps to I %lu  queue full %lu\x1b[K\n", now->jumps, now->overflows);
    printf("not drawn %lu  y2r errors %lu\x1b[K\n", w->not_shown, w->y2r_failed);
    printf("latency probes %lu/s\x1b[K\n", w->probes);
    if (demux->no_length)
        printf("WARNING: sender lacks -omit_video_pes_length 0\x1b[K\n");
    if (g_net_stats.error)
        printf("network errno: %ld\x1b[K\n", g_net_stats.error);
    gfxFlushBuffers();
}

static void log_metrics(const Window *w, const Totals *now, const Totals *prev, double secs, double t_s,
                        u32 gap_max, const RcvBufInfo *rcvbuf, u8 variant_index)
{
    const TsDemux *demux = net_demux();
    double kbps = (now->bytes - prev->bytes) * 8.0 / 1000.0 / secs;
    double q_avg = w->queue_samples ? (double)w->queue_sum / w->queue_samples : 0.0;
    char row[METRICS_ROW_SIZE];

    snprintf(row, sizeof(row),
             "%.1f,%lu,%.0f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
             "%.2f,%.2f,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%lu,%lu,%ld,%lu,%lu,0x%lx,%lu,%d,%d,%d,"
             "%lu,%lu,%lu,%u\n",
             t_s, now->packets - prev->packets, kbps, now->lost - prev->lost, now->received - prev->received,
             now->corrupt - prev->corrupt, w->decoded, w->shown, w->not_shown, now->dropped - prev->dropped,
             now->jumps - prev->jumps, now->overflows - prev->overflows, timestat_avg_ms(&w->decode),
             timestat_max_ms(&w->decode), w->over_budget, timestat_avg_ms(&w->y2r), timestat_max_ms(&w->y2r),
             timestat_avg_ms(&w->draw), timestat_max_ms(&w->draw), timestat_avg_ms(&w->age),
             timestat_max_ms(&w->age), q_avg, w->queue_max, gap_max, demux->no_length, g_net_stats.error,
             g_net_stats.wakeups, g_net_stats.eagain, g_net_stats.revents, g_input_sent, rcvbuf->default_size,
             rcvbuf->final_size, rcvbuf->last_errno, g_input_rtt_ms, w->probes, g_net_stats.late_ticks,
             variant_index);
    metrics_log_push(row);
}

bool run_stream(const StreamOptions *options)
{
    consoleClear();

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    fq_reset();
    RcvBufInfo rcvbuf;
    // From here on the network thread is the only reader of the HID service.
    if (!net_start(prio - 1, &rcvbuf)) {
        printf("Error: cannot open the network sockets\n(errno %d).\nA: back  START: quit\n", errno);
        return wait_for_key();
    }

    metrics_log_start(prio + 1, METRICS_HEADER);
    char note[METRICS_ROW_SIZE];
    snprintf(note, sizeof(note), "# rcvbuf default %d, final %d, last errno %d; %s\n", rcvbuf.default_size,
             rcvbuf.final_size, rcvbuf.last_errno, options->ab_test ? "A/B test" : VARIANTS[DEFAULT_VARIANT].name);
    metrics_log_push(note);

    vo_clear();

    u64 t_session = svcGetSystemTick();
    u64 t_window = t_session;
    Window w;
    memset(&w, 0, sizeof(w));
    Totals prev;
    memset(&prev, 0, sizeof(prev));
    u32 total_shown = 0;
    u8 variant = DEFAULT_VARIANT;
    bool keep_app = true;

    while (true) {
        if (!aptMainLoop()) {
            keep_app = false;
            break;
        }
        if (input_exit_requested())
            break;

        FqSlot *slot = fq_acquire(10 * 1000000LL);
        if (slot)
            process_frame(slot, &w, variant);

        u64 now = svcGetSystemTick();
        double elapsed_s = TICKS_TO_MS(now - t_session) / 1000.0;
        if (options->seconds && elapsed_s >= options->seconds)
            break;
        double window_ms = TICKS_TO_MS(now - t_window);
        if (window_ms < 1000.0)
            continue;

        // Once per second: refresh the screen and log a metrics row.
        double secs = window_ms / 1000.0;
        Totals totals = read_totals();
        u32 gap_max = g_fq_stats.max_gap_ms;
        g_fq_stats.max_gap_ms = 0;
        total_shown += w.shown;

        print_metrics(&w, &totals, &prev, secs, gap_max, total_shown, &rcvbuf, &VARIANTS[variant]);
        log_metrics(&w, &totals, &prev, secs, elapsed_s, gap_max, &rcvbuf, variant);

        // Variants switch only at window boundaries, so every metrics row is pure.
        if (options->ab_test)
            variant = ((u32)elapsed_s / AB_PERIOD_S) % 2;

        t_window = now;
        memset(&w, 0, sizeof(w));
        prev = totals;
    }

    net_stop();
    metrics_log_stop();
    return keep_app;
}
