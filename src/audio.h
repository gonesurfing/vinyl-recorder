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
