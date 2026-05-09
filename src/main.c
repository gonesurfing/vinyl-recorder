#include "common.h"
#include "ring.h"
#include "audio.h"
#include "recorder.h"
#include "ui.h"

#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
        "  -L, --list-devices    list available capture devices and exit\n"
        "  -P, --probe           capture for 3s without UI; print levels (diagnostic)\n"
        "  -v, --version         print version and exit\n"
        "  -h, --help            this help\n",
        APP_NAME, DEFAULT_RATE);
}

// Diagnostic mode: spin up the audio thread and poll meter atomics for a few
// seconds, printing per-tick to stderr. Bypasses ncurses entirely.
static int run_probe(audio_args_t *aargs) {
    pthread_t audio_thr;
    if (pthread_create(&audio_thr, NULL, audio_thread_main, aargs) != 0) {
        fprintf(stderr, "probe: pthread_create failed\n");
        return 1;
    }

    fprintf(stderr,
            "probe: capturing for 3s — make some noise (clap, speak, drop the needle).\n");
    fprintf(stderr,
            "  t(ms)   block_seq    peak_l    peak_r    rms_l    rms_r   xruns\n");

    struct timespec slp = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    for (int i = 0; i < 15 && !atomic_load(&g_state.quit); i++) {
        nanosleep(&slp, NULL);
        int pl = atomic_load(&g_state.peak_left);
        int pr = atomic_load(&g_state.peak_right);
        int rl = atomic_load(&g_state.rms_left_q15);
        int rr = atomic_load(&g_state.rms_right_q15);
        unsigned long seq = (unsigned long)atomic_load(&g_state.block_seq);
        int xr = atomic_load(&g_state.xrun_count);
        fprintf(stderr, "  %5d   %9lu  %8d  %8d  %7d  %7d  %6d\n",
                (i + 1) * 200, seq, pl, pr, rl, rr, xr);
    }

    atomic_store(&g_state.quit, 1);
    pthread_join(audio_thr, NULL);
    return 0;
}

int main(int argc, char **argv) {
    app_config_t cfg = {
        .device     = DEFAULT_DEVICE,
        .rate       = DEFAULT_RATE,
        .output_dir = default_output_dir(),
    };

    int probe = 0;
    static const struct option longopts[] = {
        { "device",       required_argument, 0, 'd' },
        { "rate",         required_argument, 0, 'r' },
        { "output",       required_argument, 0, 'o' },
        { "list-devices", no_argument,       0, 'L' },
        { "probe",        no_argument,       0, 'P' },
        { "version",      no_argument,       0, 'v' },
        { "help",         no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:r:o:LPvh", longopts, NULL)) != -1) {
        switch (c) {
            case 'd': cfg.device = optarg; break;
            case 'r': cfg.rate = (unsigned int)atoi(optarg); break;
            case 'o': cfg.output_dir = optarg; break;
            case 'L': audio_list_devices(); return 0;
            case 'P': probe = 1; break;
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

    cfg.rate = audio_pick_rate(cfg.device, cfg.rate);

    // Ring buffer sized for ~2 seconds of audio at the negotiated rate, rounded
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

    if (probe) {
        int rc = run_probe(&aargs);
        ring_free(&ring);
        return rc;
    }

    if (flac_check_available() != 0) {
        fprintf(stderr,
                "error: `flac` not found on PATH. Install it (e.g. "
                "`sudo apt install flac` on Debian/Raspberry Pi OS, "
                "`brew install flac` on macOS) and try again.\n");
        ring_free(&ring);
        return 1;
    }

    recorder_args_t rargs = {
        .ring = &ring, .st = &g_state, .output_dir = cfg.output_dir, .rate = (int)cfg.rate,
    };

    // Redirect stderr to a log file so audio/recorder messages (xruns,
    // CoreAudio init, etc.) don't scribble onto the ncurses UI.
    int saved_stderr = -1;
    {
        char log_path[600];
        snprintf(log_path, sizeof(log_path), "%s/vinyl_recorder.log", cfg.output_dir);
        mkdir(cfg.output_dir, 0755);  // best-effort; recorder also creates it
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            fprintf(stderr, "logging audio/recorder messages to %s\n", log_path);
            fflush(stderr);
            saved_stderr = dup(STDERR_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
            // Line-buffer the log so messages land promptly on disk.
            setvbuf(stderr, NULL, _IOLBF, 0);
        }
    }

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

    if (saved_stderr >= 0) {
        fflush(stderr);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    return 0;
}
