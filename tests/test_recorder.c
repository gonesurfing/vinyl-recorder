#include "test_util.h"
#include "../src/recorder.h"
#include "../src/common.h"
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
    snprintf(buf, sizeof(buf), "/tmp/simple_record_test_%s_%d.wav", name, (int)getpid());
    return buf;
}

TEST(lookback_under_capacity) {
    lookback_t lb;
    ASSERT(lookback_init(&lb, 8) == 0);  // 8 samples = 4 stereo frames
    int16_t in[6] = { 1, 2, 3, 4, 5, 6 };
    lookback_push(&lb, in, 6);
    ASSERT_EQ_LL(lookback_size(&lb), 6);
    int16_t out[6] = { 0 };
    lookback_dump(&lb, out);
    for (int i = 0; i < 6; i++) ASSERT_EQ_LL(out[i], in[i]);
    lookback_free(&lb);
}

TEST(lookback_at_capacity_keeps_newest) {
    lookback_t lb;
    lookback_init(&lb, 4);
    int16_t a[6] = { 1, 2, 3, 4, 5, 6 };
    lookback_push(&lb, a, 6);
    ASSERT_EQ_LL(lookback_size(&lb), 4);
    int16_t out[4] = { 0 };
    lookback_dump(&lb, out);
    int16_t expected[4] = { 3, 4, 5, 6 };
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(out[i], expected[i]);
    lookback_free(&lb);
}

TEST(lookback_multiple_pushes_wrap) {
    lookback_t lb;
    lookback_init(&lb, 4);
    int16_t a[3] = { 1, 2, 3 };
    int16_t b[3] = { 4, 5, 6 };
    lookback_push(&lb, a, 3);
    lookback_push(&lb, b, 3);
    int16_t out[4] = { 0 };
    lookback_dump(&lb, out);
    int16_t expected[4] = { 3, 4, 5, 6 };
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(out[i], expected[i]);
    lookback_free(&lb);
}

TEST(wav_roundtrip_preserves_samples) {
    const char *p = tmp_path("roundtrip");
    unlink(p);
    wav_writer_t *w = wav_open(p, 44100, 2);
    ASSERT(w != NULL);
    int16_t in[8] = {  100, -100,  200, -200,  300, -300,  400, -400 };
    ASSERT_EQ_LL(wav_write(w, in, 4), 4);
    ASSERT_EQ_LL(wav_close(w), 0);

    struct stat sb;
    ASSERT(stat(p, &sb) == 0);
    ASSERT(sb.st_size > 44);  // header + data

    // Read back via libsndfile through the public include path.
    extern int sndfile_read_int16_for_test(const char *path, int16_t *out,
                                            int max_samples, int *rate, int *ch);
    int16_t out[8] = { 0 };
    int rate = 0, ch = 0;
    int n = sndfile_read_int16_for_test(p, out, 8, &rate, &ch);
    ASSERT_EQ_LL(n, 8);
    ASSERT_EQ_LL(rate, 44100);
    ASSERT_EQ_LL(ch, 2);
    for (int i = 0; i < 8; i++) ASSERT_EQ_LL(out[i], in[i]);
    unlink(p);
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
    RUN(wav_roundtrip_preserves_samples);
    RUN(flac_poll_reports_done_for_finished_process);
}
