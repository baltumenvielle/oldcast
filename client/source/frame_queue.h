/**
 * @file frame_queue.h
 * @brief Short queue of frames between the network thread and the decoder.
 *
 * Low-latency policy: in MPEG-2 a P-frame cannot be skipped without breaking the
 * frames that follow it, so falling behind can only be recovered by jumping to a
 * keyframe. When more than FQ_LATE_DEPTH frames are waiting and one of them is an
 * I-frame, everything before that I-frame is discarded. When the queue fills up it
 * is emptied and nothing is decoded until the next I-frame arrives.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#define FQ_SLOTS 6
#define FQ_SLOT_CAP (96 * 1024)
#define FQ_LATE_DEPTH 2

typedef struct {
    u8 *data;
    size_t len;
    int type;     /**< PIC_I, PIC_P, ... */
    bool corrupt;
    u64 t_ready;  /**< tick at which the last byte of the frame arrived */
} FqSlot;

typedef struct {
    volatile u32 received;
    volatile u32 corrupt;
    volatile u32 dropped;    /**< frames discarded without decoding */
    volatile u32 late_jumps; /**< jumps to an I-frame to catch up */
    volatile u32 overflows;  /**< times the queue was full */
    volatile u32 max_gap_ms; /**< longest gap between arrivals; reset to 0 by the reader */
} FqStats;

extern FqStats g_fq_stats;

bool fq_init(void);
void fq_exit(void);
void fq_reset(void);

/* Producer side (network thread). Single producer. */
u8 *fq_begin(size_t *cap);
void fq_commit(size_t len, bool corrupt);

/* Consumer side (decoder). */

/**
 * Returns the oldest frame to decode, after applying the low-latency policy.
 * @return NULL if nothing arrived within the timeout.
 * The slot stays valid until fq_release().
 */
FqSlot *fq_acquire(s64 timeout_ns);
void fq_release(void);

/** Number of complete frames in the queue, including one being decoded. */
u32 fq_depth(void);
