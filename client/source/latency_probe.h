/**
 * @file latency_probe.h
 * @brief Reads the timestamp barcode drawn by pc/latency-clock.html.
 *
 * End-to-end latency is measured without a camera: the PC page draws its clock
 * (milliseconds, 16 bits) as a row of 24 black and white bars across the top of the
 * screen. After the 3DS shows a frame, it reads those bars from the decoded picture and
 * sends the value back with the button packets. Since the PC both wrote the timestamp and
 * receives it, no clock synchronization is needed.
 *
 * Bar layout, left to right: 1 0 | 16 data bits, most significant first | 4 check bits | 0 1.
 * The check bits are the sum of the four data nibbles, modulo 16. The fixed bars at both
 * ends make it unlikely that ordinary game footage is read as a valid code.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PROBE_BARS 24

/**
 * Reads the barcode from the luma (Y) plane of a decoded picture.
 * Takes a plain plane instead of an AVFrame so it can be tested on a PC.
 * @return true and the encoded value if a valid code is present.
 */
bool probe_read(const uint8_t *luma, int stride, int width, int height, uint16_t *value);
