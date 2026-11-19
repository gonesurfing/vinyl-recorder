#include "sample.h"

// Shifts go through uint32_t: left-shifting a negative signed value is
// undefined behavior in C.

void samples_s16_to_s32(const int16_t *src, sample_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = (sample_t)((uint32_t)(int32_t)src[i] << 16);
}

void samples_s24_to_s32(const int32_t *src, sample_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = (sample_t)((uint32_t)src[i] << 8);
}
