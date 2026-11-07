#include "latency_probe.h"

// The page draws the bars in the top 15% of the screen; scaled to 240 lines they cover
// about 36 lines. Sample the middle of that band.
#define PROBE_ROW_PERCENT 7.5
#define SAMPLE_SIZE 4
// Luma threshold. MPEG-2 uses limited range: black is about 16 and white about 235.
#define WHITE_THRESHOLD 128

static bool read_bar(const uint8_t *luma, int stride, int width, int row, int index)
{
    // Center of bar `index` when the width is split into PROBE_BARS equal bars.
    int cx = (2 * index + 1) * width / (2 * PROBE_BARS);
    int x0 = cx - SAMPLE_SIZE / 2;
    int y0 = row - SAMPLE_SIZE / 2;

    uint32_t sum = 0;
    for (int y = 0; y < SAMPLE_SIZE; y++)
        for (int x = 0; x < SAMPLE_SIZE; x++)
            sum += luma[(y0 + y) * stride + x0 + x];
    return sum > WHITE_THRESHOLD * SAMPLE_SIZE * SAMPLE_SIZE;
}

bool probe_read(const uint8_t *luma, int stride, int width, int height, uint16_t *value)
{
    if (width < 2 * PROBE_BARS * SAMPLE_SIZE || height < 32)
        return false;
    int row = (int)(height * PROBE_ROW_PERCENT / 100.0);

    bool bars[PROBE_BARS];
    for (int i = 0; i < PROBE_BARS; i++)
        bars[i] = read_bar(luma, stride, width, row, i);

    if (!bars[0] || bars[1] || bars[PROBE_BARS - 2] || !bars[PROBE_BARS - 1])
        return false;

    uint16_t data = 0;
    for (int i = 0; i < 16; i++)
        data = (uint16_t)((data << 1) | bars[2 + i]);
    unsigned check = 0;
    for (int i = 0; i < 4; i++)
        check = (check << 1) | bars[18 + i];

    unsigned expected = ((data & 0xF) + ((data >> 4) & 0xF) + ((data >> 8) & 0xF) + ((data >> 12) & 0xF)) & 0xF;
    if (check != expected)
        return false;

    *value = data;
    return true;
}
