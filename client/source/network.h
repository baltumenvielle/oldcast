/**
 * @file network.h
 * @brief The network thread: video reception and the input channel.
 *
 * One thread owns every socket. It waits in poll() on the video and input sockets, but
 * never longer than the next input tick (INPUT_PERIOD_MS), so buttons are sampled and
 * sent on time. Video datagrams go through the TS demuxer into the frame queue; echoes
 * from the PC update the round-trip time. See input.h for why this is a single thread.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>

#include "ts_demux.h"

/** UDP port the client listens on for video. */
#define VIDEO_PORT 5004

/** Socket receive buffer sizes, reported for diagnostics. */
typedef struct {
    int default_size; /**< Size the SOC service gives a new socket (or -errno). */
    int final_size;   /**< Size after our request (or -errno). */
    int last_errno;   /**< errno of the last rejected size, 0 if the first one worked. */
} RcvBufInfo;

/** Cumulative counters. Written by the network thread only. */
typedef struct {
    volatile u32 packets;
    volatile u32 bytes;
    volatile u32 wakeups;    /**< poll() calls that reported video data */
    volatile u32 eagain;     /**< recvfrom() calls that found no data after a positive poll() */
    volatile u32 revents;    /**< last revents value of the video socket */
    volatile u32 late_ticks; /**< input ticks that ran more than one period late */
    volatile s32 error;      /**< last unexpected errno, 0 if none */
} NetStats;

extern NetStats g_net_stats;

/**
 * Opens the video and input sockets and starts the network thread.
 * @param prio     Thread priority (lower value = higher priority).
 * @param rcvbuf   Filled with the video socket's receive buffer sizes.
 * @return false if a socket could not be created or bound (errno is set).
 */
bool net_start(s32 prio, RcvBufInfo *rcvbuf);

/** Stops the network thread and closes the sockets. */
void net_stop(void);

/** Demuxer state, for its loss and format counters. */
const TsDemux *net_demux(void);
