#include "common.h"
#include "ring.h"
#include "audio.h"
#include "recorder.h"
#include "ui.h"

#include <getopt.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static app_state_t g_state;

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

static void on_sigint(int sig) {
    (void)sig;
    atomic_store(&g_state.quit, 1);
}

static const char *default_output_dir(void) {
    static char buf[512];
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    snprintf(buf, sizeof(buf), "%s/recordings", home);
    return buf;
}

static void usage(void) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -d, --device <name>   capture device (default: \"default\")\n"
        "  -r, --rate <hz>       sample rate (default: %d)\n"
        "  -o, --output <dir>    output directory (default: $HOME/recordings)\n"
        "  -v, --version         print version and exit\n"
        "  -h, --help            this help\n",
        APP_NAME, DEFAULT_RATE);
}

int main(int argc, char **argv) {
    app_config_t cfg = {
        .device     = DEFAULT_DEVICE,
        .rate       = DEFAULT_RATE,
        .output_dir = default_output_dir(),
    };

    static const struct option longopts[] = {
        { "device",  required_argument, 0, 'd' },
        { "rate",    required_argument, 0, 'r' },
        { "output",  required_argument, 0, 'o' },
        { "version", no_argument,       0, 'v' },
        { "help",    no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:r:o:vh", longopts, NULL)) != -1) {
        switch (c) {
            case 'd': cfg.device = optarg; break;
            case 'r': cfg.rate = (unsigned int)atoi(optarg); break;
            case 'o': cfg.output_dir = optarg; break;
            case 'v': printf("%s %s\n", APP_NAME, APP_VERSION); return 0;
            case 'h': usage(); return 0;
            default:  usage(); return 2;
        }
    }

    app_state_init(&g_state);

    struct sigaction sa = { .sa_handler = on_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Ring buffer sized for ~2 seconds of audio at the requested rate, rounded
    // up to a power of two by ring_init. Frame size is CHANNELS samples.
    size_t ring_capacity = (size_t)(cfg.rate * CHANNELS * 2);
    ring_t ring;
    if (ring_init(&ring, ring_capacity) != 0) {
        fprintf(stderr, "ring_init failed\n");
        return 1;
    }

    audio_args_t aargs = {
        .device = cfg.device, .rate = cfg.rate, .ring = &ring, .st = &g_state,
    };
    recorder_args_t rargs = {
        .ring = &ring, .st = &g_state, .output_dir = cfg.output_dir, .rate = (int)cfg.rate,
    };

    pthread_t audio_thr, rec_thr;
    pthread_create(&audio_thr, NULL, audio_thread_main,    &aargs);
    pthread_create(&rec_thr,   NULL, recorder_thread_main, &rargs);

    ui_args_t uargs = {
        .st = &g_state, .device = cfg.device,
        .rate = (int)cfg.rate, .output_dir = cfg.output_dir,
    };
    ui_run(&uargs);

    atomic_store(&g_state.quit, 1);
    pthread_join(audio_thr, NULL);
    pthread_join(rec_thr,   NULL);
    ring_free(&ring);
    return 0;
}
