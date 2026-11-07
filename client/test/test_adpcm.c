// PC test for the IMA ADPCM decoder: decodes audio packets written by pc/audio.py --dump
// (or pc/tests/test_audio.py) and writes interleaved 16-bit stereo PCM.
//   gcc -std=c11 -Wall -I../source test_adpcm.c ../source/adpcm.c -o test_adpcm
//   ./test_adpcm packets.bin decoded.pcm
// decoded.pcm must be byte-identical to the reference decoding done with Python's audioop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adpcm.h"

#define HEADER_SIZE 20
#define STATE_SIZE 4

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s packets.bin decoded.pcm\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out) {
        perror("fopen");
        return 2;
    }

    uint8_t packet[4096];
    int16_t pcm[2 * 2048];
    unsigned packets = 0;
    uint8_t len_bytes[2];
    while (fread(len_bytes, 1, 2, in) == 2) {
        uint16_t len = u16le(len_bytes);
        if (len > sizeof(packet) || fread(packet, 1, len, in) != len) {
            fprintf(stderr, "truncated file\n");
            return 1;
        }
        if (memcmp(packet, "3DSA", 4) != 0 || packet[5] != 2) {
            fprintf(stderr, "bad packet %u\n", packets);
            return 1;
        }
        int frames = u16le(packet + 12);
        const uint8_t *data = packet + HEADER_SIZE + 2 * STATE_SIZE;
        for (int ch = 0; ch < 2; ch++) {
            const uint8_t *state = packet + HEADER_SIZE + ch * STATE_SIZE;
            adpcm_decode(data + ch * frames / 2, frames, (int16_t)u16le(state), state[2], pcm + ch, 2);
        }
        fwrite(pcm, sizeof(int16_t), 2 * (size_t)frames, out);
        packets++;
    }
    fclose(in);
    fclose(out);
    printf("decoded %u packets\n", packets);
    return 0;
}
