#include "metrics_log.h"

#include <stdio.h>

#include "common.h"

#define METRICS_PATH DATA_DIR "/metrics.csv"
#define LOG_ROWS 32
#define LOG_STACK_SIZE (16 * 1024)

// Single-producer, single-consumer ring: the stream loop writes, the thread reads.
static char s_rows[LOG_ROWS][METRICS_ROW_SIZE];
static volatile u32 s_write;
static volatile u32 s_read;
static LightEvent s_event;
static volatile bool s_run;
static Thread s_thread;
static FILE *s_file;

static void write_row(const char *row)
{
    if (s_file)
        fputs(row, s_file);
    if (g_linked)
        fputs(row, stderr);
}

static void log_thread(void *arg)
{
    (void)arg;
    while (true) {
        LightEvent_WaitTimeout(&s_event, 200 * 1000000LL);
        bool wrote = false;
        while (s_read != s_write) {
            write_row(s_rows[s_read]);
            s_read = (s_read + 1) % LOG_ROWS;
            wrote = true;
        }
        if (wrote && s_file)
            fflush(s_file);
        if (!s_run && s_read == s_write)
            break;
    }
}

bool metrics_log_start(s32 prio, const char *header)
{
    ensure_data_dir();
    s_file = fopen(METRICS_PATH, "a");
    if (!s_file && !g_linked)
        return false;

    s_write = s_read = 0;
    LightEvent_Init(&s_event, RESET_ONESHOT);
    write_row(header);

    s_run = true;
    s_thread = threadCreate(log_thread, NULL, LOG_STACK_SIZE, prio, 0, false);
    if (!s_thread) {
        if (s_file)
            fclose(s_file);
        s_file = NULL;
        return false;
    }
    return true;
}

void metrics_log_push(const char *row)
{
    if (!s_thread)
        return;
    u32 next = (s_write + 1) % LOG_ROWS;
    if (next == s_read)
        return;
    snprintf(s_rows[s_write], METRICS_ROW_SIZE, "%s", row);
    s_write = next;
    LightEvent_Signal(&s_event);
}

void metrics_log_stop(void)
{
    if (!s_thread)
        return;
    s_run = false;
    LightEvent_Signal(&s_event);
    threadJoin(s_thread, U64_MAX);
    threadFree(s_thread);
    s_thread = NULL;
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
}
