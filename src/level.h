#pragma once
#include <stddef.h>
#include <stdint.h>

#define LEVEL_FLOOR_DB -90.0f

// Linear |sample| (0..32768) -> dBFS (clamped LEVEL_FLOOR_DB..0).
float dbfs_from_linear(float linear);

// Compute per-channel peak (|max|) and RMS over `frames` interleaved S16
// stereo samples. `samples` length must be frames * 2.
// Outputs are linear (0..32768).
void block_stats_s16_stereo(const int16_t *samples, size_t frames,
                            float *peak_l, float *rms_l,
                            float *peak_r, float *rms_r);

typedef struct {
    float bar;       // current PPM bar level (linear)
    float hold;      // peak-hold (linear)
    float hold_age;  // seconds since hold was set
} ppm_t;

void ppm_init(ppm_t *p);
void ppm_update(ppm_t *p, float new_peak_linear, float dt_seconds);
