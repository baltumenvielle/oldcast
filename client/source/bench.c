#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "decoder.h"
#include "frame_queue.h"
#include "ts_demux.h"
#include "video_out.h"

#define BENCH_INPUT_PATH DATA_DIR "/bench.ts"
#define BENCH_OUTPUT_PATH DATA_DIR "/bench.txt"
#define DATAGRAM_SIZE (7 * TS_PACKET_SIZE)
#define DECODER_PADDING 64

typedef struct {
    u8 *buf;
    TimeStat decode, y2r, draw;
    u32 frames, shown, over_budget, no_output;
    bool stop;
} Bench;

static u8 *on_frame_begin(void *user, size_t *cap)
{
    Bench *b = user;
    *cap = FQ_SLOT_CAP;
    return b->buf;
}

/** Called by the demuxer for every frame: decode it and draw it right away. */
static void on_frame_end(void *user, size_t len, bool corrupt)
{
    Bench *b = user;
    (void)corrupt;
    if (b->stop || len == 0)
        return;

    memset(b->buf + len, 0, DECODER_PADDING);
    u64 t0 = svcGetSystemTick();
    const AVFrame *frame = dec_decode(b->buf, len);
    u64 t1 = svcGetSystemTick();
    timestat_add(&b->decode, t1 - t0);
    if (TICKS_TO_MS(t1 - t0) > FRAME_BUDGET_MS)
        b->over_budget++;
    b->frames++;

    if (!frame) {
        b->no_output++;
    } else if (vo_convert(frame)) {
        u64 t2 = svcGetSystemTick();
        vo_present(true);
        timestat_add(&b->y2r, t2 - t1);
        timestat_add(&b->draw, svcGetSystemTick() - t2);
        b->shown++;
    }

    hidScanInput();
    if (hidKeysDown() & (KEY_B | KEY_START))
        b->stop = true;
}

/** Reads the whole file into memory so that SD card speed does not affect the result. */
static u8 *load_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = malloc(*size);
    if (data && fread(data, 1, *size, f) != (size_t)*size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

bool run_bench(void)
{
    consoleClear();
    printf("Loading %s\n", BENCH_INPUT_PATH);

    long size = 0;
    u8 *file = load_file(BENCH_INPUT_PATH, &size);
    Bench b;
    memset(&b, 0, sizeof(b));
    b.buf = malloc(FQ_SLOT_CAP + DECODER_PADDING);
    if (!file || !b.buf) {
        printf("\nCannot read the file. Create it with\npc\\make-bench.ps1 (see README).\n\n"
               "A: back  START: quit\n");
        free(file);
        free(b.buf);
        return wait_for_key();
    }

    printf("%ld KB. Decoding (B: stop)...\n", size / 1024);

    // Feed the demuxer in datagram-sized chunks, as the network would.
    TsDemux demux;
    ts_demux_init(&demux, on_frame_begin, on_frame_end, &b);
    u64 t0 = svcGetSystemTick();
    for (long pos = 0; pos < size && !b.stop && aptMainLoop(); pos += DATAGRAM_SIZE) {
        long chunk = size - pos < DATAGRAM_SIZE ? size - pos : DATAGRAM_SIZE;
        ts_demux_feed(&demux, file + pos, (size_t)chunk);
    }
    double total_ms = TICKS_TO_MS(svcGetSystemTick() - t0);

    char text[512];
    snprintf(text, sizeof(text),
             "frames %lu  without output %lu\n"
             "decode ms avg %.2f  max %.2f\n"
             "decode > 33ms: %lu (%.1f%%)\n"
             "y2r ms    avg %.2f  max %.2f\n"
             "draw ms   avg %.2f  max %.2f\n"
             "fps decode only %.1f\n"
             "fps total %.1f (60 max, vsync)\n",
             b.frames, b.no_output, timestat_avg_ms(&b.decode), timestat_max_ms(&b.decode), b.over_budget,
             b.frames ? 100.0 * b.over_budget / b.frames : 0.0, timestat_avg_ms(&b.y2r),
             timestat_max_ms(&b.y2r), timestat_avg_ms(&b.draw), timestat_max_ms(&b.draw),
             b.decode.count ? 1000.0 / timestat_avg_ms(&b.decode) : 0.0,
             total_ms > 0 ? b.frames * 1000.0 / total_ms : 0.0);
    printf("\n%s\nA: back  START: quit\n", text);
    if (g_linked)
        fprintf(stderr, "# bench %s, %ld KB\n%s\n", BENCH_INPUT_PATH, size / 1024, text);

    ensure_data_dir();
    FILE *out = fopen(BENCH_OUTPUT_PATH, "a");
    if (out) {
        fprintf(out, "# %s, %ld KB\n%s\n", BENCH_INPUT_PATH, size / 1024, text);
        fclose(out);
    }

    free(file);
    free(b.buf);
    return wait_for_key();
}
