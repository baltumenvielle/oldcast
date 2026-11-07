/**
 * @file common.h
 * @brief Definitions shared by the whole 3DS client.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>

#define APP_NAME "OldCast"

/** Folder on the SD card for metrics and benchmark files. */
#define DATA_DIR "sdmc:/3ds/oldcast"

/** Time available per frame at 30 fps. */
#define FRAME_BUDGET_MS 33.3

#define TICKS_TO_MS(t) ((double)(t) / CPU_TICKS_PER_MSEC)

/** Count, sum and maximum of a duration measured in CPU ticks. */
typedef struct {
    u32 count;
    u64 total;
    u64 max;
} TimeStat;

static inline void timestat_add(TimeStat *s, u64 ticks)
{
    s->count++;
    s->total += ticks;
    if (ticks > s->max)
        s->max = ticks;
}

static inline double timestat_avg_ms(const TimeStat *s)
{
    return s->count ? TICKS_TO_MS(s->total) / s->count : 0.0;
}

static inline double timestat_max_ms(const TimeStat *s)
{
    return TICKS_TO_MS(s->max);
}

/**
 * True when the app was launched with `3dslink -s`. In that case stderr is a TCP
 * connection to the PC, and metrics are mirrored there.
 */
extern bool g_linked;

/** Creates DATA_DIR if it does not exist yet. */
void ensure_data_dir(void);

/**
 * Waits for a key press.
 * @return false if the key was START or the app is closing, true for any other key.
 */
bool wait_for_key(void);
