#include "audio.h"
#include "level.h"
#include "sample.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned audio_pick_rate(const char *device, unsigned requested) {
    // ALSA negotiates rate per-stream and the plug/dmix layer handles
    // resampling when the hardware can't deliver the requested rate.
    // Trust the user's choice.
    (void)device;
    return requested;
}

void audio_list_devices(void) {
    void **hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0 || !hints) {
        fprintf(stderr, "ALSA: snd_device_name_hint failed\n");
        return;
    }

    printf("ALSA capture PCMs (use -d <name>):\n\n");
    int printed = 0;
    for (void **h = hints; *h; h++) {
        char *ioid = snd_device_name_get_hint(*h, "IOID");
        // IOID null = input+output, "Input" = capture only, "Output" = playback only.
        int is_input = (!ioid || strcmp(ioid, "Input") == 0);
        if (is_input) {
            char *name = snd_device_name_get_hint(*h, "NAME");
            char *desc = snd_device_name_get_hint(*h, "DESC");
            printf("  %s\n", name ? name : "(unnamed)");
            if (desc) {
                // DESC may contain newlines — replace with " | " for one-line output.
                for (char *p = desc; *p; p++) if (*p == '\n') *p = ' ';
                printf("      %s\n", desc);
            }
            free(name);
            free(desc);
            printed++;
        }
        free(ioid);
    }
    if (!printed) printf("  (no capture-capable PCMs found)\n");
    snd_device_name_free_hint(hints);
}

// Capture formats in order of preference for the requested depth. 24-bit
// prefers the 32-bit container (what most I2S ADCs such as the HiFiBerry
// DAC+ADC Pro expose), then S24_LE; both fall back to 16-bit (e.g. USB
// codecs that only do S16_LE). The recorder writes 16-bit WAVs when the
// device can't deliver more.
static const snd_pcm_format_t formats_24[] = {
    SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S16_LE,
};
static const snd_pcm_format_t formats_16[] = {
    SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_LE,
};

static unsigned format_bits(snd_pcm_format_t f) {
    switch (f) {
        case SND_PCM_FORMAT_S32_LE: return 32;
        case SND_PCM_FORMAT_S24_LE: return 24;
        default:                    return 16;
    }
}

static int configure_pcm(snd_pcm_t *pcm, unsigned int *rate, int want_bits,
                         snd_pcm_format_t *format_out,
                         snd_pcm_uframes_t *period_frames,
                         unsigned int *periods_out) {
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    int rc;

    if ((rc = snd_pcm_hw_params_any(pcm, hw)) < 0) return rc;
    if ((rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return rc;
    const snd_pcm_format_t *prefs = (want_bits == 24) ? formats_24 : formats_16;
    snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;
    for (size_t i = 0; i < 3; i++) {
        if (snd_pcm_hw_params_test_format(pcm, hw, prefs[i]) == 0) { fmt = prefs[i]; break; }
    }
    if (fmt == SND_PCM_FORMAT_UNKNOWN) return -EINVAL;
    if ((rc = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0) return rc;
    *format_out = fmt;
    if ((rc = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS)) < 0) return rc;

    unsigned int rr = *rate;
    if ((rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rr, NULL)) < 0) return rc;
    *rate = rr;

    snd_pcm_uframes_t pf = PERIOD_FRAMES;
    if ((rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &pf, NULL)) < 0) return rc;

    unsigned int bt = BUFFER_TIME_US;
    if ((rc = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &bt, NULL)) < 0) return rc;

    if ((rc = snd_pcm_hw_params(pcm, hw)) < 0) return rc;

    // Read back what the device actually granted.
    unsigned int pn = 0;
    snd_pcm_hw_params_get_period_size(hw, &pf, NULL);
    snd_pcm_hw_params_get_periods(hw, &pn, NULL);

    *period_frames = pf;
    *periods_out = pn;
    return 0;
}

// Ask for real-time scheduling so a burst of other work (sshd/tmux redraw on
// reattach, flac encoding, writeback) can't starve the capture loop. Needs
// CAP_SYS_NICE or an rtprio rlimit; without either we keep running at normal
// priority and say so in the log.
static void try_realtime(void) {
    struct sched_param sp = { .sched_priority = AUDIO_RT_PRIORITY };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc == 0) {
        fprintf(stderr, "ALSA: capture thread at SCHED_FIFO priority %d\n",
                AUDIO_RT_PRIORITY);
    } else {
        fprintf(stderr,
                "ALSA: could not get real-time priority (%s); capture thread "
                "runs at normal priority. See README \"Real-time priority\".\n",
                strerror(rc));
    }
}

void *audio_thread_main(void *arg) {
    audio_args_t *a = (audio_args_t *)arg;
    try_realtime();
    snd_pcm_t *pcm = NULL;
    int rc = snd_pcm_open(&pcm, a->device, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        fprintf(stderr, "snd_pcm_open(%s): %s\n", a->device, snd_strerror(rc));
        atomic_store(&a->st->quit, 1);
        return NULL;
    }

    unsigned int rate = a->rate, periods = 0;
    snd_pcm_uframes_t period_frames = 0;
    snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;
    rc = configure_pcm(pcm, &rate, a->bits, &fmt, &period_frames, &periods);
    if (rc < 0) {
        fprintf(stderr, "configure_pcm: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        atomic_store(&a->st->quit, 1);
        return NULL;
    }
    fprintf(stderr, "ALSA: %s @ %u Hz, %s, period=%lu frames, periods=%u (%.0f ms buffer)\n",
            a->device, rate, snd_pcm_format_name(fmt),
            (unsigned long)period_frames, periods,
            1000.0 * (double)period_frames * periods / rate);
    // capture_bits first: the recorder reads it once it sees negotiated_rate.
    atomic_store(&a->st->capture_bits, format_bits(fmt));
    atomic_store(&a->st->negotiated_rate, rate);

    // raw: what the device delivers (2 or 4 bytes/sample). block: converted
    // to the internal left-justified format for the meters and the ring.
    size_t nsamp = (size_t)period_frames * CHANNELS;
    void     *raw   = calloc(nsamp, 4);
    sample_t *block = (sample_t *)calloc(nsamp, sizeof(sample_t));
    if (!raw || !block) {
        free(raw);
        free(block);
        snd_pcm_close(pcm);
        atomic_store(&a->st->quit, 1);
        return NULL;
    }

    snd_pcm_prepare(pcm);

    while (!atomic_load(&a->st->quit)) {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, raw, period_frames);
        if (got == -EPIPE) {
            int n = atomic_fetch_add(&a->st->xrun_count, 1) + 1;
            fprintf(stderr, "xrun #%d: ALSA capture overrun (capture thread starved)\n", n);
            snd_pcm_prepare(pcm);
            continue;
        }
        if (got == -EAGAIN) continue;
        if (got < 0) {
            int rec = snd_pcm_recover(pcm, (int)got, 1);
            if (rec < 0) {
                fprintf(stderr, "snd_pcm_readi: %s\n", snd_strerror((int)got));
                break;
            }
            continue;
        }

        size_t got_samples = (size_t)got * CHANNELS;
        switch (fmt) {
            case SND_PCM_FORMAT_S32_LE: memcpy(block, raw, got_samples * sizeof(sample_t)); break;
            case SND_PCM_FORMAT_S24_LE: samples_s24_to_s32((const int32_t *)raw, block, got_samples); break;
            default:                    samples_s16_to_s32((const int16_t *)raw, block, got_samples); break;
        }

        // Publish per-block stats for the meter.
        float pl, rl, pr, rr;
        int clips = 0;
        block_stats_stereo(block, (size_t)got, &pl, &rl, &pr, &rr, &clips);
        atomic_store(&a->st->peak_left,    (int)pl);
        atomic_store(&a->st->peak_right,   (int)pr);
        atomic_store(&a->st->rms_left_q15, (int)rl);
        atomic_store(&a->st->rms_right_q15,(int)rr);
        atomic_fetch_add(&a->st->block_seq, 1);
        if (clips > 0) atomic_fetch_add(&a->st->clip_count, clips);

        // Push to ring. If full, oldest data was meant to be drained by
        // the recorder; on the rare "ring full" we drop the new block and
        // count it as an xrun so the user sees it.
        size_t want = got_samples;
        size_t wrote = ring_write(a->ring, block, want);
        if (wrote < want) {
            int n = atomic_fetch_add(&a->st->xrun_count, 1) + 1;
            fprintf(stderr, "xrun #%d: ring full, dropped %zu frames (recorder thread stalled)\n",
                    n, (want - wrote) / CHANNELS);
        }
    }

    free(raw);
    free(block);
    snd_pcm_close(pcm);
    return NULL;
}
