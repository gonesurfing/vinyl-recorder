#include "common.h"
#include <stdio.h>
#include <string.h>

void app_state_init(app_state_t *s) {
    atomic_init(&s->quit, 0);
    atomic_init(&s->rec_request, 0);
    atomic_init(&s->rec_state, REC_IDLE);
    atomic_init(&s->xrun_count, 0);
    atomic_init(&s->encoder_active, 0);
    atomic_init(&s->frames_recorded, 0);
    atomic_init(&s->peak_left, 0);
    atomic_init(&s->peak_right, 0);
    atomic_init(&s->rms_left_q15, 0);
    atomic_init(&s->rms_right_q15, 0);
    atomic_init(&s->block_seq, 0);
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("%s %s\n", APP_NAME, APP_VERSION);
        return 0;
    }
    fprintf(stderr, "%s %s — not yet implemented\n", APP_NAME, APP_VERSION);
    return 0;
}
