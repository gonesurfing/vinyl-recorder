#pragma once
#include <stddef.h>
#include <stdint.h>

// Internal sample format: signed 32-bit, left-justified. Full scale is the
// int32 range regardless of the device's bit depth, so 16-bit and 24-bit
// captures share one code path and libsndfile can write either depth from it.
typedef int32_t sample_t;

// 16-bit sample in the high half of the word (value * 65536).
void samples_s16_to_s32(const int16_t *src, sample_t *dst, size_t n);

// ALSA S24_LE: a 24-bit value in the low three bytes of a 32-bit container;
// the top byte is padding and ignored.
void samples_s24_to_s32(const int32_t *src, sample_t *dst, size_t n);
