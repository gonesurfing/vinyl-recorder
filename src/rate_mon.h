#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Capture-rate monitor: compares frames received against a monotonic clock.
//
// A USB audio packet that the host controller loses is never resent; the
// kernel just delivers fewer frames. On a Raspberry Pi 2 or 3 that happens
// often enough (0.7 to 12 packets/s measured) to make the device look
// hundreds or thousands of ppm slow. Honest clock mismatch between a codec
// and the system is under ~100 ppm, so a large deficit means lost audio even
// though no xrun was reported.
//
// Least-squares fit of frames vs. time over a sliding window, so a few ms
// of wakeup jitter per read averages out.

#define RATE_MON_WINDOW_S     60.0   // fit over the last minute
#define RATE_MON_MIN_SPAN_S   20.0   // no estimate until this much history
#define RATE_MON_MIN_STEP_S    0.1   // keep at most one point per 100 ms
#define RATE_MON_WARN_PPM    300.0   // |ppm| above this is flagged
#define RATE_MON_CAP          1024   // >= WINDOW_S / MIN_STEP_S

typedef struct {
    double   nominal;              // negotiated sample rate
    double   t[RATE_MON_CAP];      // seconds since origin
    double   f[RATE_MON_CAP];      // frames since origin
    size_t   head, count;          // ring of points; head = oldest
    bool     have_origin;
    double   t0;
    uint64_t f0;
} rate_mon_t;

void rate_mon_init(rate_mon_t *m, unsigned nominal_rate);

// Forget all history (call after an xrun or restart: the frame count jumps).
void rate_mon_reset(rate_mon_t *m);

// One observation: at time t_sec, `frames` frames had been captured.
void rate_mon_add(rate_mon_t *m, double t_sec, uint64_t frames);

// Deviation of the delivered rate from nominal, in ppm (negative = frames
// missing). Returns false until RATE_MON_MIN_SPAN_S of history exists.
bool rate_mon_ppm(const rate_mon_t *m, double *ppm);

// True when the deviation is too large to be clock mismatch.
bool rate_mon_suspect(double ppm);
