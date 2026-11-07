/**
 * @file siphash.h
 * @brief SipHash-2-4: a keyed hash used as the signature of every message the 3DS sends.
 *
 * SipHash is designed for short messages, where it is fast and secure as a MAC; signing a
 * 40-byte input packet costs a few microseconds on the Old 3DS. Reference:
 * Aumasson and Bernstein, "SipHash: a fast short-input PRF" (2012).
 * Kept free of 3DS dependencies so it can be tested on a PC (test/test_siphash.c).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** Returns the 64-bit SipHash-2-4 of `data` with a 16-byte key. */
uint64_t siphash24(const uint8_t key[16], const void *data, size_t size);
