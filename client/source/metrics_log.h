/**
 * @file metrics_log.h
 * @brief Background writer for metrics rows.
 *
 * Rows go to DATA_DIR/metrics.csv on the SD card and, when the app was launched by
 * `3dslink -s`, to the PC as well. Writing to the SD card can block for tens of
 * milliseconds, so it happens on a low-priority thread and never on the thread that
 * decodes video. If the writer falls behind, rows are dropped instead of frames.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>

/** Maximum length of one row, including the newline. */
#define METRICS_ROW_SIZE 256

/**
 * Starts the writer thread.
 * @param prio    Thread priority; use a value above the decoder's (= lower priority).
 * @param header  Text written first, usually the CSV header line(s).
 * @return false if there is nowhere to write (no SD file and no 3dslink connection).
 */
bool metrics_log_start(s32 prio, const char *header);

/** Queues one row (or a "# comment" line). Never blocks. */
void metrics_log_push(const char *row);

/** Flushes pending rows and stops the writer thread. */
void metrics_log_stop(void);
