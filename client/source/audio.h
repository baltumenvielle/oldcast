/**
 * @file audio.h
 * @brief Audio playback: IMA ADPCM packets from the PC played through the DSP (NDSP).
 *
 * Packets (pc/audio.py) carry 10 ms of stereo audio each, with the decoder state included,
 * so every packet decodes on its own. The network thread decodes each packet straight
 * into a DSP wave buffer and queues it.
 *
 * Wi-Fi delivers packets with gaps of up to ~150 ms, while audio must play continuously.
 * The queue therefore keeps a minimum depth: playback is paused until `target` buffers are
 * queued. If the queue runs dry while packets keep arriving (an underrun), the target grows
 * by 10 ms; after 15 s without underruns it shrinks again. Packets that would make the
 * queue more than DROP_MARGIN buffers deeper than the target are dropped, so latency does
 * not build up (for example from the small clock difference between PC and 3DS).
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#define AUDIO_PORT 5006

/** Cumulative counters, written by the network thread. */
typedef struct {
    volatile u32 packets;
    volatile u32 lost;       /**< gaps in the sequence numbers */
    volatile u32 dropped;    /**< dropped to keep latency down */
    volatile u32 underruns;  /**< queue ran dry while packets were arriving */
    volatile u32 no_buffer;  /**< no free wave buffer (should not happen) */
    volatile u32 queued_ms;  /**< audio queued in the DSP at the last check */
    volatile u32 target_ms;  /**< current minimum queue depth */
} AudioStats;

extern AudioStats g_audio_stats;

/** Starts the DSP. @return false if it is unavailable (missing sdmc:/3ds/dspfirm.cdc). */
bool audio_init(void);
void audio_exit(void);
bool audio_available(void);

/** Stops playback and clears the queue; call when a stream starts and ends. */
void audio_reset(void);

/** Handles one UDP packet from the PC. Called by the network thread. */
void audio_handle_packet(const u8 *data, size_t size);

/** Detects underruns and adapts the queue depth. Called by the network thread every tick. */
void audio_tick(void);
