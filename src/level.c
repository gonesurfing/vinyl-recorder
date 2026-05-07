#include "level.h"
#include "common.h"
#include <math.h>

float dbfs_from_linear(float linear) {
    if (linear <= 0.0f) return LEVEL_FLOOR_DB;
    float v = 20.0f * log10f(linear / SAMPLE_FULL_SCALE);
    if (v < LEVEL_FLOOR_DB) v = LEVEL_FLOOR_DB;
    if (v > 0.0f) v = 0.0f;
    return v;
}

void block_stats_s16_stereo(const int16_t *samples, size_t frames,
                            float *peak_l, float *rms_l,
                            float *peak_r, float *rms_r) {
    int32_t pl = 0, pr = 0;
    double sl = 0.0, sr = 0.0;
    for (size_t i = 0; i < frames; i++) {
        int32_t a = samples[i * 2 + 0];
        int32_t b = samples[i * 2 + 1];
        int32_t aa = a < 0 ? -a : a;     // safe: int32 holds 32768
        int32_t bb = b < 0 ? -b : b;
        if (aa > pl) pl = aa;
        if (bb > pr) pr = bb;
        sl += (double)a * (double)a;
        sr += (double)b * (double)b;
    }
    double n = (double)(frames > 0 ? frames : 1);
    *peak_l = (float)pl;
    *peak_r = (float)pr;
    *rms_l  = (float)sqrt(sl / n);
    *rms_r  = (float)sqrt(sr / n);
}

#define PPM_ATTACK_TC   0.010f
#define PPM_RELEASE_TC  1.700f
#define PPM_HOLD_TIME   1.500f
#define PPM_HOLD_DECAY_PER_S 0.5f  // hold falls by half per second after expiry

void ppm_init(ppm_t *p) {
    p->bar = 0.0f;
    p->hold = 0.0f;
    p->hold_age = 0.0f;
}

void ppm_update(ppm_t *p, float new_peak, float dt) {
    float tc = (new_peak > p->bar) ? PPM_ATTACK_TC : PPM_RELEASE_TC;
    float alpha = 1.0f - expf(-dt / tc);
    p->bar += alpha * (new_peak - p->bar);

    if (new_peak >= p->hold) {
        p->hold = new_peak;
        p->hold_age = 0.0f;
    } else {
        p->hold_age += dt;
        if (p->hold_age > PPM_HOLD_TIME) {
            p->hold *= expf(-dt / PPM_HOLD_DECAY_PER_S);
            if (p->hold < p->bar) p->hold = p->bar;
        }
    }
}
