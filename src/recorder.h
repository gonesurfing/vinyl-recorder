#pragma once
#include "common.h"
#include "ring.h"
#include <pthread.h>

// --- Lookback buffer (testable in isolation) -----------------------------

typedef struct {
    int16_t *buf;
    size_t   cap_samples; // total int16 slots; multiple of CHANNELS
    size_t   write;       // monotonic write index (in samples)
    size_t   filled;      // total samples ever written, capped at cap_samples
} lookback_t;

int  lookback_init(lookback_t *lb, size_t cap_samples);
void lookback_free(lookback_t *lb);
void lookback_push(lookback_t *lb, const int16_t *src, size_t n);
size_t lookback_size(const lookback_t *lb);
// Copy contents in chronological order into dst. dst must hold lookback_size().
void lookback_dump(const lookback_t *lb, int16_t *dst);

// --- WAV writer (testable in isolation) ----------------------------------

typedef struct wav_writer wav_writer_t;

wav_writer_t *wav_open(const char *path, int rate, int channels);
size_t        wav_write(wav_writer_t *w, const int16_t *src, size_t frames);
int           wav_close(wav_writer_t *w);  // returns 0 on success

// --- Recorder thread -----------------------------------------------------

typedef struct {
    ring_t      *ring;
    app_state_t *st;
    const char  *output_dir;
    int          rate;
} recorder_args_t;

void *recorder_thread_main(void *arg);

#include <sys/types.h>

// Returns 0 if the `flac` encoder is on PATH and runs, -1 otherwise.
// Synchronous; intended for a one-shot startup check.
int  flac_check_available(void);

// Spawn `flac --best --silent --delete-input-file <wav_path>`. On success,
// returns 0 and writes the child pid into *out_pid. The WAV file is deleted
// by flac after a successful encode; the resulting FLAC sits next to it.
int  flac_spawn(const char *wav_path, pid_t *out_pid);
// Non-blocking check: returns 1 if the encoder finished successfully,
// -1 on error, 0 if still running. *pid is zeroed when reaped.
int  flac_poll(pid_t *pid);
