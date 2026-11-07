// PC test for the latency probe reader: decodes the barcode from raw luma frames.
//   gcc -std=c11 -Wall -I../source test_probe.c ../source/latency_probe.c -o test_probe
//   ./test_probe frames.y 400 240 [expected_code]
// frames.y holds 8-bit luma frames back to back, for example from:
//   ffmpeg -i stream.ts -f rawvideo -pix_fmt gray frames.y
// Exit status is 0 only if every frame decodes (to expected_code, when given).
#include <stdio.h>
#include <stdlib.h>

#include "latency_probe.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s frames.y width height [expected_code]\n", argv[0]);
        return 2;
    }
    int width = atoi(argv[2]), height = atoi(argv[3]);
    long expected = argc > 4 ? atol(argv[4]) : -1;
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror("fopen");
        return 2;
    }

    size_t frame_size = (size_t)width * height;
    uint8_t *frame = malloc(frame_size);
    unsigned frames = 0, valid = 0, wrong = 0;
    while (fread(frame, 1, frame_size, in) == frame_size) {
        frames++;
        uint16_t code;
        if (probe_read(frame, width, width, height, &code)) {
            valid++;
            if (expected >= 0 && code != expected) {
                wrong++;
                printf("frame %u: read %u, expected %ld\n", frames, code, expected);
            }
        }
    }
    fclose(in);
    free(frame);

    printf("frames=%u valid=%u wrong=%u\n", frames, valid, wrong);
    return (frames > 0 && valid == frames && wrong == 0) ? 0 : 1;
}
