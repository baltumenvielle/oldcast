// PC test for the TS demuxer: extracts the video of a .ts file and counts its frames.
//   gcc -std=c11 -Wall -I../source test_demux.c ../source/ts_demux.c -o test_demux
//   ./test_demux input.ts output.m2v [drop_every_N_datagrams]
// Without drops, output.m2v must be byte-identical to:
//   ffmpeg -i input.ts -c copy -f mpeg2video reference.m2v
#include <stdio.h>
#include <stdlib.h>

#include "ts_demux.h"

#define DATAGRAM (7 * TS_PACKET_SIZE)

static uint8_t s_frame[1024 * 1024];
static FILE *s_out;
static unsigned s_count[4];
static unsigned s_frames, s_corrupt;

static uint8_t *on_begin(void *user, size_t *cap)
{
    (void)user;
    *cap = sizeof(s_frame);
    return s_frame;
}

static void on_end(void *user, size_t len, bool corrupt)
{
    (void)user;
    int type = mpeg2_picture_type(s_frame, len);
    s_frames++;
    s_corrupt += corrupt;
    if (type >= 0 && type < 4)
        s_count[type]++;
    fwrite(s_frame, 1, len, s_out);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s input.ts output.m2v [drop_every_N]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    s_out = fopen(argv[2], "wb");
    if (!in || !s_out) {
        perror("fopen");
        return 2;
    }
    int lose_every = argc > 3 ? atoi(argv[3]) : 0;

    TsDemux d;
    ts_demux_init(&d, on_begin, on_end, NULL);

    static uint8_t buf[DATAGRAM];
    size_t got;
    int n = 0;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        n++;
        if (lose_every && n % lose_every == 0)
            continue;
        ts_demux_feed(&d, buf, got);
    }
    fclose(in);
    fclose(s_out);

    printf("frames=%u I=%u P=%u B=%u unknown=%u corrupt=%u\n", s_frames, s_count[PIC_I], s_count[PIC_P],
           s_count[PIC_B], s_count[PIC_UNKNOWN], s_corrupt);
    printf("ts=%u lost=%u bad_sync=%u no_length=%u overflows=%u\n", d.ts_packets, d.ts_lost, d.bad_sync,
           d.no_length, d.overflows);
    return 0;
}
