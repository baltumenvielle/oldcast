/**
 * @file adpcm.h
 * @brief IMA ADPCM decoder, bit-exact with Python's audioop.adpcm2lin.
 *
 * Kept free of 3DS dependencies so it can be tested on a PC (test/test_adpcm.c).
 * Decoding costs a handful of integer operations per sample: about 1% of the Old 3DS
 * CPU for 48 kHz stereo.
 */
#pragma once

#include <stdint.h>

/**
 * Decodes `samples` 4-bit codes (two per byte, first sample in the high nibble).
 * @param predictor, index  Decoder state before the first sample.
 * @param out     Output samples, written every `stride` elements (2 for interleaved stereo).
 */
void adpcm_decode(const uint8_t *in, int samples, int16_t predictor, int index, int16_t *out, int stride);
