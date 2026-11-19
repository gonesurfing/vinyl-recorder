#include "test_util.h"
#include "../src/recorder.h"
#include "../src/common.h"
#include "../src/sample.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
extern char **environ;

static const char *tmp_path(const char *name) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/vinyl_recorder_test_%s_%d.wav", name, (int)getpid());
    return buf;
}

TEST(lookback_under_capacity) {
    lookback_t lb;
    ASSERT(lookback_init(&lb, 8) == 0);  // 8 samples = 4 stereo frames
    sample_t in[6] = { 1, 2, 3, 4, 5, 6 };
    lookback_push(&lb, in, 6);
    ASSERT_EQ_LL(lookback_size(&lb), 6);
    sample_t out[6] = { 0 };
    lookback_dump(&lb, out);
    for (int i = 0; i < 6; i++) ASSERT_EQ_LL(out[i], in[i]);
    lookback_free(&lb);
}

TEST(lookback_at_capacity_keeps_newest) {
    lookback_t lb;
    lookback_init(&lb, 4);
    sample_t a[6] = { 1, 2, 3, 4, 5, 6 };
    lookback_push(&lb, a, 6);
    ASSERT_EQ_LL(lookback_size(&lb), 4);
    sample_t out[4] = { 0 };
    lookback_dump(&lb, out);
    sample_t expected[4] = { 3, 4, 5, 6 };
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(out[i], expected[i]);
    lookback_free(&lb);
}

TEST(lookback_multiple_pushes_wrap) {
    lookback_t lb;
    lookback_init(&lb, 4);
    sample_t a[3] = { 1, 2, 3 };
    sample_t b[3] = { 4, 5, 6 };
    lookback_push(&lb, a, 3);
    lookback_push(&lb, b, 3);
    sample_t out[4] = { 0 };
    lookback_dump(&lb, out);
    sample_t expected[4] = { 3, 4, 5, 6 };
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(out[i], expected[i]);
    lookback_free(&lb);
}

TEST(lookback_preserves_32bit_samples) {
    lookback_t lb;
    lookback_init(&lb, 4);
    sample_t in[4] = { 0x7FFFFF00, INT32_MIN, 256, -256 };
    lookback_push(&lb, in, 4);
    sample_t out[4] = { 0 };
    lookback_dump(&lb, out);
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(out[i], in[i]);
    lookback_free(&lb);
}

extern int sndfile_read_for_test(const char *path, int32_t *out, int max_samples,
                                 int *rate, int *ch, int *bits);

TEST(wav16_roundtrip_keeps_top_16_bits) {
    const char *p = tmp_path("rt16");
    unlink(p);
    wav_writer_t *w = wav_open(p, 44100, 2, 16);
    ASSERT(w != NULL);
    // 16-bit values, left-justified, as the capture path produces them.
    int16_t v[8] = { 100, -100, 200, -200, 300, -300, 32767, -32768 };
    sample_t in[8];
    samples_s16_to_s32(v, in, 8);
    ASSERT_EQ_LL(wav_write(w, in, 4), 4);
    ASSERT_EQ_LL(wav_close(w), 0);

    int32_t out[8] = { 0 };
    int rate = 0, ch = 0, bits = 0;
    ASSERT_EQ_LL(sndfile_read_for_test(p, out, 8, &rate, &ch, &bits), 8);
    ASSERT_EQ_LL(rate, 44100);
    ASSERT_EQ_LL(ch, 2);
    ASSERT_EQ_LL(bits, 16);
    for (int i = 0; i < 8; i++) ASSERT_EQ_LL(out[i], in[i]);
    unlink(p);
}

TEST(wav24_roundtrip_preserves_24bit_detail) {
    const char *p = tmp_path("rt24");
    unlink(p);
    wav_writer_t *w = wav_open(p, 96000, 2, 24);
    ASSERT(w != NULL);
    // Values that need all 24 bits: 1 LSB, full scale, minimum.
    sample_t in[6] = { 256, -256, 0x7FFFFF00, INT32_MIN, 0x12345600, -0x12345600 };
    ASSERT_EQ_LL(wav_write(w, in, 3), 3);
    ASSERT_EQ_LL(wav_close(w), 0);

    int32_t out[6] = { 0 };
    int rate = 0, ch = 0, bits = 0;
    ASSERT_EQ_LL(sndfile_read_for_test(p, out, 6, &rate, &ch, &bits), 6);
    ASSERT_EQ_LL(rate, 96000);
    ASSERT_EQ_LL(bits, 24);
    for (int i = 0; i < 6; i++) ASSERT_EQ_LL(out[i], in[i]);
    unlink(p);
}

TEST(output_bits_honors_24_when_device_delivers_it) {
    ASSERT_EQ_LL(recorder_output_bits(24, 24), 24);
    ASSERT_EQ_LL(recorder_output_bits(24, 32), 24);
}

TEST(output_bits_falls_back_to_16_for_16bit_device) {
    // Padding a 16-bit capture to 24-bit just wastes space.
    ASSERT_EQ_LL(recorder_output_bits(24, 16), 16);
}

TEST(output_bits_16_requested_stays_16) {
    ASSERT_EQ_LL(recorder_output_bits(16, 24), 16);
    ASSERT_EQ_LL(recorder_output_bits(16, 32), 16);
}

TEST(flac_poll_reports_done_for_finished_process) {
    pid_t pid;
    char *argv[] = { "true", NULL };
    // posix_spawnp searches PATH so the test works whether `true` lives in
    // /bin (Linux) or /usr/bin (macOS).
    ASSERT(posix_spawnp(&pid, "true", NULL, NULL, argv, environ) == 0);
    // Wait for the child to actually exit before polling.
    for (int i = 0; i < 100; i++) {
        int rc = flac_poll(&pid);
        if (rc == 1) { ASSERT_EQ_LL(pid, 0); return; }
        ASSERT(rc == 0);
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    ASSERT(0 && "child never reaped");
}

void run_recorder_tests(void) {
    RUN(lookback_under_capacity);
    RUN(lookback_at_capacity_keeps_newest);
    RUN(lookback_multiple_pushes_wrap);
    RUN(lookback_preserves_32bit_samples);
    RUN(wav16_roundtrip_keeps_top_16_bits);
    RUN(wav24_roundtrip_preserves_24bit_detail);
    RUN(output_bits_honors_24_when_device_delivers_it);
    RUN(output_bits_falls_back_to_16_for_16bit_device);
    RUN(output_bits_16_requested_stays_16);
    RUN(flac_poll_reports_done_for_finished_process);
}
