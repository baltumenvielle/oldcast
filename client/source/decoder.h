/**
 * @file decoder.h
 * @brief Single-threaded MPEG-2 decoding with libavcodec.
 */
#pragma once

#include <3ds/types.h>
#include <libavutil/frame.h>
#include <stdbool.h>
#include <stddef.h>

bool dec_init(void);
void dec_exit(void);

/**
 * Decodes one frame.
 * @param data  Frame data followed by AV_INPUT_BUFFER_PADDING_SIZE zero bytes.
 * @return The decoded picture, valid until the next call, or NULL if none came out.
 */
const AVFrame *dec_decode(const u8 *data, size_t len);
