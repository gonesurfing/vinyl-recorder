#include "recorder.h"
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

extern char **environ;

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

// --- FLAC spawn / poll ---------------------------------------------------

int flac_check_available(void) {
    char *argv[] = { "flac", "--version", NULL };
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return -1;
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "flac", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

int flac_spawn(const char *wav_path, pid_t *out_pid) {
    char *argv[] = {
        "flac", "--best", "--silent", "--delete-input-file",
        (char *)wav_path, NULL,
    };
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "flac", NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr,
                "flac: failed to launch encoder for %s: %s "
                "(is the `flac` package installed?)\n",
                wav_path, strerror(rc));
        errno = rc;
        return -1;
    }
    *out_pid = pid;
    return 0;
}

int flac_poll(pid_t *pid) {
    if (*pid == 0) return 1;
    int status = 0;
    pid_t r = waitpid(*pid, &status, WNOHANG);
    if (r == 0) return 0;
    if (r < 0)  return -1;
    pid_t reaped = *pid;
    *pid = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 1;
    if (WIFEXITED(status)) {
        fprintf(stderr,
                "flac: encoder pid %d exited with status %d; "
                "WAV file was left behind\n",
                (int)reaped, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr,
                "flac: encoder pid %d killed by signal %d; "
                "WAV file was left behind\n",
                (int)reaped, WTERMSIG(status));
    }
    return -1;
}

// --- Recorder thread -----------------------------------------------------

static void make_filename(char *out, size_t n, const char *dir) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%s/%04d-%02d-%02d_%02d%02d%02d.wav",
             dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int ensure_dir(const char *dir) {
    struct stat sb;
    if (stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode)) return 0;
    return mkdir(dir, 0755);
}

#define SCRATCH_FRAMES (PERIOD_FRAMES * 2)

void *recorder_thread_main(void *arg) {
    recorder_args_t *a = (recorder_args_t *)arg;
    app_state_t *st = a->st;

    if (ensure_dir(a->output_dir) != 0) {
        fprintf(stderr, "cannot create output dir %s\n", a->output_dir);
        atomic_store(&st->quit, 1);
        return NULL;
    }

    lookback_t lb;
    size_t lb_samples = (size_t)(PREROLL_SECONDS * a->rate) * CHANNELS;
    if (lookback_init(&lb, lb_samples) != 0) {
        atomic_store(&st->quit, 1);
        return NULL;
    }

    int16_t *scratch = malloc(SCRATCH_FRAMES * CHANNELS * sizeof(int16_t));
    if (!scratch) { lookback_free(&lb); atomic_store(&st->quit, 1); return NULL; }

    wav_writer_t *wav = NULL;
    char wav_path[512] = { 0 };
    pid_t encoder_pid = 0;
    rec_state_t state = REC_IDLE;
    atomic_store(&st->rec_state, state);

    while (!atomic_load(&st->quit)) {
        // Reap any finished encoder.
        if (encoder_pid != 0) {
            int r = flac_poll(&encoder_pid);
            if (r != 0) atomic_store(&st->encoder_active, 0);
        }

        // Handle UI requests.
        int req = atomic_exchange(&st->rec_request, 0);
        if (req == 1 && state == REC_IDLE) {
            // start
            make_filename(wav_path, sizeof(wav_path), a->output_dir);
            wav = wav_open(wav_path, a->rate, CHANNELS);
            if (wav) {
                size_t n = lookback_size(&lb);
                if (n > 0) {
                    int16_t *dump = malloc(n * sizeof(int16_t));
                    if (dump) {
                        lookback_dump(&lb, dump);
                        wav_write(wav, dump, n / CHANNELS);
                        free(dump);
                    }
                }
                atomic_store(&st->frames_recorded, 0);
                state = REC_RUNNING;
                atomic_store(&st->rec_state, state);
            }
        } else if (req == 2 && state == REC_RUNNING) {
            // stop -> drain ring -> close -> spawn flac
            state = REC_STOPPING;
            atomic_store(&st->rec_state, state);
        } else if (req == 3 && state == REC_RUNNING) {
            // split: close current, open next
            wav_close(wav); wav = NULL;
            if (flac_spawn(wav_path, &encoder_pid) == 0)
                atomic_store(&st->encoder_active, 1);
            make_filename(wav_path, sizeof(wav_path), a->output_dir);
            wav = wav_open(wav_path, a->rate, CHANNELS);
            atomic_store(&st->frames_recorded, 0);
        }

        // Drain the ring.
        size_t got = ring_read(a->ring, scratch, SCRATCH_FRAMES * CHANNELS);
        if (got == 0) {
            // No data this tick. If stopping with file open and ring empty, finish.
            if (state == REC_STOPPING && wav != NULL) {
                wav_close(wav); wav = NULL;
                if (flac_spawn(wav_path, &encoder_pid) == 0)
                    atomic_store(&st->encoder_active, 1);
                state = REC_IDLE;
                atomic_store(&st->rec_state, state);
            }
            struct timespec ts = { 0, 2 * 1000 * 1000 }; // 2 ms
            nanosleep(&ts, NULL);
            continue;
        }

        size_t frames = got / CHANNELS;
        if (state == REC_IDLE) {
            lookback_push(&lb, scratch, got);
        } else {
            // Keep lookback fresh during recording too, so a split has pre-roll.
            lookback_push(&lb, scratch, got);
            if (wav != NULL) {
                wav_write(wav, scratch, frames);
                atomic_fetch_add(&st->frames_recorded, frames);
            }
        }
    }

    // Shutdown: close any open file, but DO NOT block on the encoder.
    if (wav != NULL) {
        wav_close(wav);
        flac_spawn(wav_path, &encoder_pid);
    }
    lookback_free(&lb);
    free(scratch);
    return NULL;
}
