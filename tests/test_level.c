#include "test_util.h"
#include "../src/level.h"
#include "../src/common.h"
#include "../src/sample.h"
#include <math.h>

// Feed 16-bit test vectors through the same conversion the capture path uses.
static void stats16(const int16_t *s16, size_t frames,
                    float *pl, float *rl, float *pr, float *rr, int *clips) {
    sample_t buf[256];
    samples_s16_to_s32(s16, buf, frames * 2);
    block_stats_stereo(buf, frames, pl, rl, pr, rr, clips);
}

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
    int clips;
    stats16(s, 4, &pl, &rl, &pr, &rr, &clips);
    ASSERT_NEAR(pl, 10000.0f, 1.0f);
    ASSERT_NEAR(pr, 20000.0f, 1.0f);
    ASSERT_NEAR(rl, 10000.0f, 1.0f);
    ASSERT_NEAR(rr, 20000.0f, 1.0f);
    ASSERT_EQ_LL(clips, 0);
}

TEST(block_stats_silence) {
    int16_t s[8] = { 0 };
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 4, &pl, &rl, &pr, &rr, &clips);
    ASSERT_NEAR(pl, 0.0f, 0.001f);
    ASSERT_NEAR(rl, 0.0f, 0.001f);
    ASSERT_EQ_LL(clips, 0);
}

TEST(block_stats_handles_int16_min) {
    // INT16_MIN's absolute value is 32768 — must not overflow.
    int16_t s[2] = { -32768, -32768 };
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 1, &pl, &rl, &pr, &rr, &clips);
    ASSERT_NEAR(pl, 32768.0f, 1.0f);
    ASSERT_NEAR(pr, 32768.0f, 1.0f);
    // Single saturated sample is below the run threshold (3) so no event.
    ASSERT_EQ_LL(clips, 0);
}

TEST(clip_below_threshold_no_event) {
    // 10 frames near full-scale but not saturated: peak = 32766 < 32767.
    int16_t s[20];
    for (int i = 0; i < 20; i++) s[i] = 32766;
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 10, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 0);
}

TEST(clip_two_consecutive_no_event) {
    // 2 saturated samples on L is below the 3-sample run threshold.
    int16_t s[8] = {
        32767, 0,
        32767, 0,
        0,     0,
        0,     0,
    };
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 4, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 0);
}

TEST(clip_three_consecutive_one_event) {
    int16_t s[8] = {
        32767, 0,
        32767, 0,
        32767, 0,
        0,     0,
    };
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 4, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 1);
}

TEST(clip_long_run_still_one_event) {
    // 50 consecutive saturated samples on R = a single sustained clip event.
    int16_t s[100] = { 0 };
    for (int i = 0; i < 50; i++) s[i * 2 + 1] = -32768;
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 50, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 1);
}

TEST(clip_two_separate_runs_two_events) {
    // Two distinct 3-sample runs on L separated by a non-saturated sample.
    int16_t s[16] = {
        32767, 0,
        32767, 0,
        32767, 0,
        0,     0,        // breaks the run
        32767, 0,
        32767, 0,
        32767, 0,
        0,     0,
    };
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 8, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 2);
}

TEST(clip_both_channels_simultaneous_two_events) {
    // 3 saturated samples on both L and R at the same time = 2 events
    // (one per channel).
    int16_t s[8] = {
        32767, -32768,
        32767, -32768,
        32767, -32768,
        0,     0,
    };
    float pl, rl, pr, rr;
    int clips;
    stats16(s, 4, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 2);
}

TEST(block_stats_24bit_resolution) {
    // One 24-bit LSB (256 in left-justified int32) is 1/256 of a 16-bit LSB.
    sample_t s[2] = { 256, -256 };
    float pl, rl, pr, rr;
    int clips;
    block_stats_stereo(s, 1, &pl, &rl, &pr, &rr, &clips);
    ASSERT_NEAR(pl, 1.0f / 256.0f, 1e-6);
    ASSERT_NEAR(pr, 1.0f / 256.0f, 1e-6);
}

TEST(block_stats_handles_int32_min) {
    sample_t s[2] = { INT32_MIN, INT32_MIN };
    float pl, rl, pr, rr;
    int clips;
    block_stats_stereo(s, 1, &pl, &rl, &pr, &rr, &clips);
    ASSERT_NEAR(pl, 32768.0f, 0.01f);
    ASSERT_NEAR(rr, 32768.0f, 0.01f);
}

TEST(clip_24bit_full_scale_is_saturated) {
    // 24-bit positive full scale (0x7FFFFF) left-justified.
    sample_t s[6] = { 0x7FFFFF00, 0, 0x7FFFFF00, 0, 0x7FFFFF00, 0 };
    float pl, rl, pr, rr;
    int clips;
    block_stats_stereo(s, 3, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 1);
}

TEST(clip_24bit_just_below_16bit_full_scale_not_saturated) {
    // Just under 16-bit 32767 even with 24-bit detail: not a clip.
    sample_t s[6] = { 0x7FFEFF00, 0, 0x7FFEFF00, 0, 0x7FFEFF00, 0 };
    float pl, rl, pr, rr;
    int clips;
    block_stats_stereo(s, 3, &pl, &rl, &pr, &rr, &clips);
    ASSERT_EQ_LL(clips, 0);
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
    RUN(clip_below_threshold_no_event);
    RUN(clip_two_consecutive_no_event);
    RUN(clip_three_consecutive_one_event);
    RUN(clip_long_run_still_one_event);
    RUN(clip_two_separate_runs_two_events);
    RUN(clip_both_channels_simultaneous_two_events);
    RUN(block_stats_24bit_resolution);
    RUN(block_stats_handles_int32_min);
    RUN(clip_24bit_full_scale_is_saturated);
    RUN(clip_24bit_just_below_16bit_full_scale_not_saturated);
    RUN(ppm_attack_then_release);
    RUN(ppm_hold_captures_peak);
}
