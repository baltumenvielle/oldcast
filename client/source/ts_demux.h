/**
 * @file ts_demux.h
 * @brief Minimal MPEG-TS demuxer that extracts MPEG-2 video frames.
 *
 * Only what the stream needs: PAT and PMT parsing to find the video PID, PES
 * reassembly into whole frames, and loss detection with the continuity counter.
 * It has no 3DS dependencies so it can be tested on a PC (see test/test_demux.c).
 *
 * Latency note: a frame is delivered as soon as its last byte arrives, using the
 * PES packet length. The sender must therefore use `-omit_video_pes_length 0`;
 * otherwise the end of a frame is only known when the next one starts, 33 ms later.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TS_PACKET_SIZE 188

/** MPEG-2 picture_coding_type values. */
enum { PIC_UNKNOWN = 0, PIC_I = 1, PIC_P = 2, PIC_B = 3 };

/** Returns the buffer to write the next frame into, or NULL to skip the frame. */
typedef uint8_t *(*TsFrameBegin)(void *user, size_t *cap);

/**
 * Receives a complete frame.
 * @param len      Frame size; 0 if it did not fit in the buffer.
 * @param corrupt  True if packets of this frame were lost.
 */
typedef void (*TsFrameEnd)(void *user, size_t len, bool corrupt);

typedef struct {
    TsFrameBegin begin;
    TsFrameEnd end;
    void *user;

    int pmt_pid;
    int video_pid;
    int last_cc;

    uint8_t *buf;
    size_t cap;
    size_t len;
    int32_t remaining; /**< bytes left in the current PES; -1 if its length is unknown */
    bool in_frame;
    bool corrupt;
    bool overflow;

    /* Cumulative counters. */
    uint32_t ts_packets;
    uint32_t ts_lost;   /**< from continuity counter gaps; undercounts runs of 16 or more */
    uint32_t bad_sync;
    uint32_t no_length; /**< frames without PES length: each one adds a frame of latency */
    uint32_t overflows;
} TsDemux;

void ts_demux_init(TsDemux *d, TsFrameBegin begin, TsFrameEnd end, void *user);

/** Processes one datagram (a multiple of 188 bytes). */
void ts_demux_feed(TsDemux *d, const uint8_t *data, size_t size);

/** Returns PIC_I, PIC_P or PIC_B from the first picture header of a frame. */
int mpeg2_picture_type(const uint8_t *data, size_t size);
