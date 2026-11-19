#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sample.h"

#define LEVEL_FLOOR_DB -90.0f

// A clip "event" is CLIP_RUN_SAMPLES or more consecutive saturated samples
// (at or above 16-bit full scale, |sample| >= CLIP_THRESHOLD) on a single
// channel within one block. Two separate
// runs in the same block count as two events; a run that ends and restarts
// in the next block is two events. Cross-block runs are not stitched.
#define CLIP_RUN_SAMPLES 3

// 32767 in 16-bit terms, left-justified (see sample.h).
#define CLIP_THRESHOLD 0x7FFF0000LL

// Linear |sample| (0..32768) -> dBFS (clamped LEVEL_FLOOR_DB..0).
float dbfs_from_linear(float linear);

// Compute per-channel peak (|max|), RMS, and clip-event count over `frames`
// interleaved stereo samples. `samples` length must be frames * 2.
// Peak/RMS outputs are linear on the 16-bit scale (0..32768) whatever the
// capture depth, so meters and dB math are unchanged; 24-bit detail shows up
// as fractions. `clip_events` is the total clip events across both channels
// in this block (see CLIP_RUN_SAMPLES).
void block_stats_stereo(const sample_t *samples, size_t frames,
                            float *peak_l, float *rms_l,
                            float *peak_r, float *rms_r,
                            int *clip_events);

typedef struct {
    float bar;       // current PPM bar level (linear)
    float hold;      // peak-hold (linear)
    float hold_age;  // seconds since hold was set
} ppm_t;

void ppm_init(ppm_t *p);
void ppm_update(ppm_t *p, float new_peak_linear, float dt_seconds);
