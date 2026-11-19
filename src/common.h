#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_NAME    "vinyl_recorder"
#define APP_VERSION "0.1.0"

#define DEFAULT_DEVICE      "default"
#define DEFAULT_RATE        44100
#define CHANNELS            2
#define PERIOD_FRAMES       1024
// Total ALSA capture buffer. Latency is irrelevant for a recorder, so make
// it deep: the capture thread can be starved this long (ssh/tmux reattach,
// SD-card writeback on a Pi) before the hardware overruns. Period size, not
// buffer size, sets how often the meters update.
#define BUFFER_TIME_US      500000
// SCHED_FIFO priority for the capture thread (Linux). Moderate on purpose:
// above every SCHED_OTHER task, below kernel housekeeping at 99.
#define AUDIO_RT_PRIORITY   60
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
    int bits;                 // requested capture/output depth: 16 or 24
    const char *output_dir;
} app_config_t;

typedef struct {
    _Atomic int      quit;            // set by signal handler / UI
    _Atomic int      rec_request;     // UI -> recorder: 0 = none, 1 = start, 2 = stop, 3 = split
    _Atomic int      rec_state;       // recorder -> UI: rec_state_t
    _Atomic int      xrun_count;      // capture overruns
    _Atomic int      clip_count;      // ADC clip events (see CLIP_RUN_SAMPLES)
    _Atomic int      encoder_active;  // 0/1, recorder publishes
    _Atomic uint64_t frames_recorded; // recorder publishes; resets on each new file
    _Atomic unsigned negotiated_rate; // audio thread publishes once device is open; 0 until then
    _Atomic unsigned capture_bits;    // audio thread: device sample depth (16/24/32), stored before negotiated_rate
    _Atomic int      output_bits;     // recorder publishes the WAV depth it writes (16/24); 0 until known
    _Atomic int      rate_ppm;        // capture rate vs system clock, ppm (negative = frames missing)
    _Atomic int      rate_ppm_valid;  // 1 once rate_ppm has enough history (ALSA only)

    // Latest block stats from the capture thread, per channel.
    // Stored as raw int32 (peak abs sample) and float (RMS).
    _Atomic int   peak_left;
    _Atomic int   peak_right;
    _Atomic int   rms_left_q15;   // RMS * 32768, fits int
    _Atomic int   rms_right_q15;
    _Atomic uint64_t block_seq;   // increments each block
} app_state_t;

void app_state_init(app_state_t *s);
