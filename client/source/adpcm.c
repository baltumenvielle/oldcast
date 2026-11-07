#include "adpcm.h"

static const int16_t STEP_SIZES[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t INDEX_CHANGES[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

void adpcm_decode(const uint8_t *in, int samples, int16_t predictor, int index, int16_t *out, int stride)
{
    int value = predictor;
    if (index < 0)
        index = 0;
    if (index > 88)
        index = 88;

    for (int i = 0; i < samples; i++) {
        int code = (i & 1) ? (in[i >> 1] & 0x0F) : (in[i >> 1] >> 4);
        int step = STEP_SIZES[index];

        int diff = step >> 3;
        if (code & 4)
            diff += step;
        if (code & 2)
            diff += step >> 1;
        if (code & 1)
            diff += step >> 2;
        value += (code & 8) ? -diff : diff;
        if (value > 32767)
            value = 32767;
        else if (value < -32768)
            value = -32768;

        index += INDEX_CHANGES[code];
        if (index < 0)
            index = 0;
        else if (index > 88)
            index = 88;

        out[i * stride] = (int16_t)value;
    }
}
