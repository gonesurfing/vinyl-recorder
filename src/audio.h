#pragma once
#include "common.h"
#include "ring.h"

typedef struct {
    const char  *device;
    unsigned int rate;
    ring_t      *ring;
    app_state_t *st;
} audio_args_t;

// Thread entry. Returns NULL. Sets st->quit on fatal error.
void *audio_thread_main(void *arg);

// Print available input-capable capture devices to stdout. Backend-specific.
void audio_list_devices(void);

// Negotiate the actual capture rate that will be used; may differ from
// `requested` (e.g. CoreAudio devices are locked at a single sample rate
// and AUHAL cannot resample on input). Backends that natively handle
// resampling return `requested` unchanged.
unsigned audio_pick_rate(const char *device, unsigned requested);
