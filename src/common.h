#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_NAME    "simple_record"
#define APP_VERSION "0.1.0"

#define DEFAULT_DEVICE      "default"
#define DEFAULT_RATE        44100
#define CHANNELS            2
#define PERIOD_FRAMES       1024
#define PERIODS             4
#define PREROLL_SECONDS     3.0f
#define UI_REFRESH_HZ       30
#define SAMPLE_FULL_SCALE   32768.0f

typedef enum {
    REC_IDLE = 0,
    REC_RUNNING,
    REC_STOPPING,
} rec_state_t;

typedef struct {
    const char *device;
    unsigned int rate;
    const char *output_dir;
} app_config_t;

typedef struct {
    _Atomic int      quit;            // set by signal handler / UI
    _Atomic int      rec_request;     // UI -> recorder: 0 = none, 1 = start, 2 = stop, 3 = split
    _Atomic int      rec_state;       // recorder -> UI: rec_state_t
    _Atomic int      xrun_count;      // capture overruns
    _Atomic int      encoder_active;  // 0/1, recorder publishes
    _Atomic uint64_t frames_recorded; // recorder publishes; resets on each new file

    // Latest block stats from the capture thread, per channel.
    // Stored as raw int32 (peak abs sample) and float (RMS).
    _Atomic int   peak_left;
    _Atomic int   peak_right;
    _Atomic int   rms_left_q15;   // RMS * 32768, fits int
    _Atomic int   rms_right_q15;
    _Atomic uint64_t block_seq;   // increments each block
} app_state_t;

void app_state_init(app_state_t *s);
