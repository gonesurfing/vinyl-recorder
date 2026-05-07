#include "recorder.h"
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Lookback buffer -----------------------------------------------------

int lookback_init(lookback_t *lb, size_t cap_samples) {
    lb->buf = (int16_t *)calloc(cap_samples, sizeof(int16_t));
    if (!lb->buf) return -1;
    lb->cap_samples = cap_samples;
    lb->write = 0;
    lb->filled = 0;
    return 0;
}

void lookback_free(lookback_t *lb) {
    free(lb->buf);
    lb->buf = NULL;
    lb->cap_samples = 0;
    lb->write = lb->filled = 0;
}

void lookback_push(lookback_t *lb, const int16_t *src, size_t n) {
    if (lb->cap_samples == 0) return;
    for (size_t i = 0; i < n; i++) {
        lb->buf[lb->write % lb->cap_samples] = src[i];
        lb->write++;
    }
    lb->filled += n;
    if (lb->filled > lb->cap_samples) lb->filled = lb->cap_samples;
}

size_t lookback_size(const lookback_t *lb) {
    return lb->filled;
}

void lookback_dump(const lookback_t *lb, int16_t *dst) {
    size_t n = lb->filled;
    if (n == 0) return;
    // Oldest sample sits at (write - n) modulo cap.
    size_t start = (lb->write - n) % lb->cap_samples;
    size_t first = lb->cap_samples - start;
    if (first > n) first = n;
    memcpy(dst, &lb->buf[start], first * sizeof(int16_t));
    if (n > first) memcpy(dst + first, &lb->buf[0], (n - first) * sizeof(int16_t));
}

// --- WAV writer ----------------------------------------------------------

struct wav_writer {
    SNDFILE *sf;
    SF_INFO  info;
};

wav_writer_t *wav_open(const char *path, int rate, int channels) {
    wav_writer_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->info.samplerate = rate;
    w->info.channels   = channels;
    w->info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    w->sf = sf_open(path, SFM_WRITE, &w->info);
    if (!w->sf) {
        fprintf(stderr, "sf_open(%s): %s\n", path, sf_strerror(NULL));
        free(w);
        return NULL;
    }
    return w;
}

size_t wav_write(wav_writer_t *w, const int16_t *src, size_t frames) {
    sf_count_t n = sf_writef_short(w->sf, src, (sf_count_t)frames);
    return (size_t)(n < 0 ? 0 : n);
}

int wav_close(wav_writer_t *w) {
    if (!w) return -1;
    int rc = sf_close(w->sf);
    free(w);
    return rc == 0 ? 0 : -1;
}

// --- Test helper (not declared in recorder.h; tests link via extern) -----

int sndfile_read_int16_for_test(const char *path, int16_t *out,
                                int max_samples, int *rate, int *ch) {
    SF_INFO info; memset(&info, 0, sizeof(info));
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) return -1;
    *rate = info.samplerate;
    *ch   = info.channels;
    sf_count_t total_frames = info.frames;
    sf_count_t want_frames = max_samples / info.channels;
    if (want_frames > total_frames) want_frames = total_frames;
    sf_count_t got = sf_readf_short(sf, out, want_frames);
    sf_close(sf);
    return (int)(got * info.channels);
}

// --- Recorder thread (stub — implemented in next task) -------------------

void *recorder_thread_main(void *arg) {
    (void)arg;
    return NULL;
}
