#include "test_util.h"
#include "../src/rate_mon.h"
#include <stdint.h>

// Feed a capture stream that delivers `rate * (1 + ppm/1e6)` frames per
// second of wall time, one observation every `step` seconds.
static void feed(rate_mon_t *m, double t_start, double t_end, double step,
                 double *frames, double rate, double ppm) {
    for (double t = t_start; t < t_end; t += step) {
        rate_mon_add(m, t, (uint64_t)*frames);
        *frames += rate * (1.0 + ppm * 1e-6) * step;
    }
}

TEST(no_estimate_until_enough_span) {
    rate_mon_t m;
    rate_mon_init(&m, 44100);
    double f = 0, ppm = 0;
    feed(&m, 0.0, RATE_MON_MIN_SPAN_S - 1.0, 0.1, &f, 44100, 0);
    ASSERT(!rate_mon_ppm(&m, &ppm));
    feed(&m, RATE_MON_MIN_SPAN_S - 1.0, RATE_MON_MIN_SPAN_S + 1.0, 0.1, &f, 44100, 0);
    ASSERT(rate_mon_ppm(&m, &ppm));
}

TEST(exact_rate_reads_zero_ppm) {
    rate_mon_t m;
    rate_mon_init(&m, 44100);
    double f = 0, ppm = 99;
    feed(&m, 0.0, 30.0, 0.1, &f, 44100, 0);
    ASSERT(rate_mon_ppm(&m, &ppm));
    ASSERT_NEAR(ppm, 0.0, 2.0);
}

TEST(lost_usb_packets_read_negative_ppm) {
    // Pi 2: ~0.7 lost 1 ms packets per second = 700 ppm fewer frames.
    rate_mon_t m;
    rate_mon_init(&m, 44100);
    double f = 0, ppm = 0;
    feed(&m, 0.0, 30.0, 0.1, &f, 44100, -700);
    ASSERT(rate_mon_ppm(&m, &ppm));
    ASSERT_NEAR(ppm, -700.0, 5.0);
}

TEST(tolerates_timestamp_jitter) {
    // Reads wake up a few ms late at random; the fit must average it out.
    rate_mon_t m;
    rate_mon_init(&m, 48000);
    uint32_t lcg = 12345;
    double f = 0, ppm = 0;
    for (double t = 0.0; t < 60.0; t += 0.025) {
        lcg = lcg * 1664525u + 1013904223u;
        double jitter = ((double)(lcg >> 8) / (double)(1u << 24)) * 0.010; // 0..10 ms late
        rate_mon_add(&m, t + jitter, (uint64_t)f);
        f += 48000.0 * (1.0 - 700e-6) * 0.025;
    }
    ASSERT(rate_mon_ppm(&m, &ppm));
    ASSERT_NEAR(ppm, -700.0, 50.0);
}

TEST(old_points_age_out_of_window) {
    rate_mon_t m;
    rate_mon_init(&m, 44100);
    double f = 0, ppm = 0;
    feed(&m, 0.0, 60.0, 0.1, &f, 44100, 0);
    feed(&m, 60.0, 60.0 + RATE_MON_WINDOW_S + 1.0, 0.1, &f, 44100, -3000);
    ASSERT(rate_mon_ppm(&m, &ppm));
    ASSERT_NEAR(ppm, -3000.0, 10.0);
}

TEST(reset_discards_history) {
    rate_mon_t m;
    rate_mon_init(&m, 44100);
    double f = 0, ppm = 0;
    feed(&m, 0.0, 30.0, 0.1, &f, 44100, 0);
    rate_mon_reset(&m);
    ASSERT(!rate_mon_ppm(&m, &ppm));
    // After an xrun the frame count jumps; only post-reset data counts.
    f += 44100;
    feed(&m, 30.0, 60.0, 0.1, &f, 44100, -700);
    ASSERT(rate_mon_ppm(&m, &ppm));
    ASSERT_NEAR(ppm, -700.0, 5.0);
}

TEST(dense_calls_are_decimated_not_overflowed) {
    // A tiny period (one call per ms) must not overflow the point store.
    rate_mon_t m;
    rate_mon_init(&m, 44100);
    double f = 0, ppm = 0;
    feed(&m, 0.0, 90.0, 0.001, &f, 44100, -1500);
    ASSERT(rate_mon_ppm(&m, &ppm));
    ASSERT_NEAR(ppm, -1500.0, 10.0);
}

TEST(suspect_threshold) {
    ASSERT(!rate_mon_suspect(0.0));
    ASSERT(!rate_mon_suspect(-100.0));
    ASSERT(!rate_mon_suspect(150.0));
    ASSERT(rate_mon_suspect(-700.0));
    ASSERT(rate_mon_suspect(-RATE_MON_WARN_PPM - 1.0));
    ASSERT(rate_mon_suspect(RATE_MON_WARN_PPM + 1.0));
}

void run_rate_mon_tests(void) {
    RUN(no_estimate_until_enough_span);
    RUN(exact_rate_reads_zero_ppm);
    RUN(lost_usb_packets_read_negative_ppm);
    RUN(tolerates_timestamp_jitter);
    RUN(old_points_age_out_of_window);
    RUN(reset_discards_history);
    RUN(dense_calls_are_decimated_not_overflowed);
    RUN(suspect_threshold);
}
