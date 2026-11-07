/**
 * @file stream.h
 * @brief Live streaming session.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>

typedef struct {
    /** Stop automatically after this many seconds; 0 runs until the user quits. */
    u32 seconds;
    /**
     * Alternate between two display configurations every AB_PERIOD_S seconds (see
     * stream.c). Each latency probe and metrics row is tagged with the active one, so
     * a single session compares both under the same network conditions.
     */
    bool ab_test;
} StreamOptions;

/**
 * Receives, decodes and shows video until the user holds SELECT+START.
 *
 * Threads on core 0, by priority:
 *   - network (highest): video reception, button sampling and sending (network.h),
 *   - this thread: decodes, converts with Y2R and draws,
 *   - metrics writer (lowest).
 * Core 1 is left to the system, which runs the SOC (network) service there.
 *
 * @return false if the whole app should close.
 */
bool run_stream(const StreamOptions *options);
