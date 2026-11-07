/**
 * @file network.h
 * @brief The network thread: video and audio reception, and the input channel.
 *
 * One thread owns every socket. It waits in poll() on the video, audio and input sockets,
 * but never longer than the next input tick (INPUT_PERIOD_MS), so buttons are sampled and
 * sent on time. Video datagrams go through the TS demuxer into the frame queue, audio
 * packets are decoded straight into DSP buffers (audio.h), and echoes from the PC update
 * the round-trip time. See input.h for why this is a single thread.
 *
 * With a SessionRequest, the thread also runs the 3DS side of the session protocol
 * (docs/protocol.md): it repeats OCSR from the video socket until the PC answers, then
 * sends OCAP from the audio socket until audio arrives, and OCSE when the stream stops.
 * Without one it just listens, which is how the measurement scripts in pc/ drive it.
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

typedef enum {
    SESSION_NONE,       /**< passive: no session request, video is pushed by the PC */
    SESSION_REQUESTING, /**< OCSR sent, no answer yet */
    SESSION_PAIRING,    /**< the PC asks the user to confirm our PIN */
    SESSION_ACTIVE,     /**< accepted: the PC is streaming */
    SESSION_DENIED,     /**< rejected: busy with another 3DS, or pairing refused */
} SessionState;

typedef struct {
    u32 host_addr;     /**< PC address, network byte order */
    u8 host_id[8];     /**< the PC's id, to forget its key if it no longer knows us */
    u16 host_port;     /**< PC port; 0 = the default, 5007 */
    bool has_key;      /**< sign every message with `key` (paired with this PC before) */
    u8 key[16];
    u16 pin;           /**< 4-digit PIN shown while pairing */
    u16 bitrate_kbps;  /**< 0 = the PC's setting */
    bool audio;
    char name[16];     /**< this console's name, shown on the PC */
} SessionRequest;

/** What the PC sent with its acceptance (OCSA), to be saved with the PC. */
typedef struct {
    u8 host_id[8];
    bool has_key;     /**< a new pairing key (only sent on the local network) */
    u8 key[16];
    u32 public_addr;  /**< where to reach the PC from outside, 0 if unknown */
    u16 public_port;
} SessionAccept;

/** Cumulative counters. Written by the network thread only. */
typedef struct {
    volatile u32 packets;
    volatile u32 bytes;
    volatile u32 wakeups;    /**< poll() calls that reported video data */
    volatile u32 eagain;     /**< recvfrom() calls that found no data after a positive poll() */
    volatile u32 revents;    /**< last revents value of the video socket */
    volatile u32 late_ticks; /**< input ticks that ran more than one period late */
    volatile u32 rerequests; /**< sessions requested again after video stopped */
    volatile s32 error;      /**< last unexpected errno, 0 if none */
} NetStats;

extern NetStats g_net_stats;

/**
 * Opens the video and input sockets and starts the network thread.
 * @param prio     Thread priority (lower value = higher priority).
 * @param rcvbuf   Filled with the video socket's receive buffer sizes.
 * @param request  Session to request from a PC, or NULL to only listen.
 * @return false if a socket could not be created or bound (errno is set).
 */
bool net_start(s32 prio, RcvBufInfo *rcvbuf, const SessionRequest *request);

SessionState net_session_state(void);

/** Returns the data of a new acceptance once, to save it. */
bool net_take_accept(SessionAccept *out);

/**
 * For SESSION_DENIED: 1 busy with another 3DS, 2 pairing refused, 3 must pair at home
 * first, 4 the PC no longer knows our key (pair again).
 */
u8 net_session_deny_reason(void);

/** True if the audio socket is open (the DSP is available and the port could be bound). */
bool net_audio_enabled(void);

/** Stops the network thread and closes the sockets. */
void net_stop(void);

/** Demuxer state, for its loss and format counters. */
const TsDemux *net_demux(void);
