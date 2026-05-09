#include "test_util.h"
#include "../src/level.h"
#include "../src/common.h"
#include <math.h>

TEST(dbfs_full_scale_is_zero) {
    ASSERT_NEAR(dbfs_from_linear(SAMPLE_FULL_SCALE), 0.0f, 0.01f);
}

TEST(dbfs_half_scale_is_minus_six) {
    ASSERT_NEAR(dbfs_from_linear(SAMPLE_FULL_SCALE * 0.5f), -6.020f, 0.01f);
}

TEST(dbfs_silent_clamps_to_floor) {
    ASSERT_NEAR(dbfs_from_linear(0.0f), LEVEL_FLOOR_DB, 0.01f);
}

TEST(block_stats_constant_signal) {
    int16_t s[8] = { 10000, -20000,  10000, -20000,
                     10000, -20000,  10000, -20000 };
    float pl, rl, pr, rr;
    block_stats_s16_stereo(s, 4, &pl, &rl, &pr, &rr);
    ASSERT_NEAR(pl, 10000.0f, 1.0f);
    ASSERT_NEAR(pr, 20000.0f, 1.0f);
    ASSERT_NEAR(rl, 10000.0f, 1.0f);
    ASSERT_NEAR(rr, 20000.0f, 1.0f);
}

TEST(block_stats_silence) {
    int16_t s[8] = { 0 };
    float pl, rl, pr, rr;
    block_stats_s16_stereo(s, 4, &pl, &rl, &pr, &rr);
    ASSERT_NEAR(pl, 0.0f, 0.001f);
    ASSERT_NEAR(rl, 0.0f, 0.001f);
}

TEST(block_stats_handles_int16_min) {
    // INT16_MIN's absolute value is 32768 — must not overflow.
    int16_t s[2] = { -32768, -32768 };
    float pl, rl, pr, rr;
    block_stats_s16_stereo(s, 1, &pl, &rl, &pr, &rr);
    ASSERT_NEAR(pl, 32768.0f, 1.0f);
    ASSERT_NEAR(pr, 32768.0f, 1.0f);
}

TEST(ppm_attack_then_release) {
    ppm_t p; ppm_init(&p);
    // Hit with a step of 16384 (-6 dBFS). Attack TC = 10 ms; after 50 ms (5 TCs)
    // we should be very close to the input.
    float dt = 0.001f; // 1 ms steps
    for (int i = 0; i < 50; i++) ppm_update(&p, 16384.0f, dt);
    ASSERT_NEAR(p.bar, 16384.0f, 200.0f);

    // Now drop input to zero. Release is linear-in-dB at 20 dB / 1.7 s.
    // Starting at -6.0206 dBFS, after 0.5 s we expect -6.0206 - (20/1.7)*0.5
    // = -11.903 dBFS, i.e. ~8324 linear.
    for (int i = 0; i < 500; i++) ppm_update(&p, 0.0f, dt);
    float expected_db = -6.0206f - (20.0f / 1.7f) * 0.5f;
    float expected    = 32768.0f * powf(10.0f, expected_db / 20.0f);
    ASSERT_NEAR(p.bar, expected, expected * 0.02f);
}

TEST(ppm_hold_captures_peak) {
    ppm_t p; ppm_init(&p);
    ppm_update(&p, 30000.0f, 0.001f);
    ASSERT_NEAR(p.hold, 30000.0f, 1.0f);
    // Smaller signal must not lower hold immediately.
    for (int i = 0; i < 100; i++) ppm_update(&p, 5000.0f, 0.001f); // 100 ms
    ASSERT_NEAR(p.hold, 30000.0f, 1.0f);
}

void run_level_tests(void) {
    RUN(dbfs_full_scale_is_zero);
    RUN(dbfs_half_scale_is_minus_six);
    RUN(dbfs_silent_clamps_to_floor);
    RUN(block_stats_constant_signal);
    RUN(block_stats_silence);
    RUN(block_stats_handles_int16_min);
    RUN(ppm_attack_then_release);
    RUN(ppm_hold_captures_peak);
}
