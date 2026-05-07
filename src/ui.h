#pragma once
#include "common.h"

typedef struct {
    app_state_t *st;
    const char  *device;
    int          rate;
    const char  *output_dir;
} ui_args_t;

// Initialize curses, run the render loop until st->quit is set, then tear
// down curses. Returns when the loop exits.
void ui_run(ui_args_t *args);
