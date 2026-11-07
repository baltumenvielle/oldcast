/**
 * @file bench.h
 * @brief Offline decoding benchmark.
 *
 * Decodes DATA_DIR/bench.ts from the SD card as fast as possible, through the same
 * demuxer, decoder and display path as the live stream but without the network.
 * It tells whether the CPU can keep up with a given encoding, independently of Wi-Fi.
 * Results are shown, appended to DATA_DIR/bench.txt and sent to the PC when linked.
 */
#pragma once

#include <stdbool.h>

/** @return false if the whole app should close. */
bool run_bench(void);
