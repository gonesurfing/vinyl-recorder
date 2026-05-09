#include "audio.h"
#include "level.h"
#include <alsa/asoundlib.h>
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

static int configure_pcm(snd_pcm_t *pcm, unsigned int *rate,
                         snd_pcm_uframes_t *period_frames,
                         unsigned int *periods_out) {
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    int rc;

    if ((rc = snd_pcm_hw_params_any(pcm, hw)) < 0) return rc;
    if ((rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return rc;
    if ((rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0) return rc;
    if ((rc = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS)) < 0) return rc;

    unsigned int rr = *rate;
    if ((rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rr, NULL)) < 0) return rc;
    *rate = rr;

    snd_pcm_uframes_t pf = PERIOD_FRAMES;
    if ((rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &pf, NULL)) < 0) return rc;

    unsigned int pn = PERIODS;
    if ((rc = snd_pcm_hw_params_set_periods_near(pcm, hw, &pn, NULL)) < 0) return rc;

    if ((rc = snd_pcm_hw_params(pcm, hw)) < 0) return rc;

    *period_frames = pf;
    *periods_out = pn;
    return 0;
}

void *audio_thread_main(void *arg) {
    audio_args_t *a = (audio_args_t *)arg;
    snd_pcm_t *pcm = NULL;
    int rc = snd_pcm_open(&pcm, a->device, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        fprintf(stderr, "snd_pcm_open(%s): %s\n", a->device, snd_strerror(rc));
        atomic_store(&a->st->quit, 1);
        return NULL;
    }

    unsigned int rate = a->rate, periods = 0;
    snd_pcm_uframes_t period_frames = 0;
    rc = configure_pcm(pcm, &rate, &period_frames, &periods);
    if (rc < 0) {
        fprintf(stderr, "configure_pcm: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        atomic_store(&a->st->quit, 1);
        return NULL;
    }
    fprintf(stderr, "ALSA: %s @ %u Hz, period=%lu frames, periods=%u\n",
            a->device, rate, (unsigned long)period_frames, periods);
    atomic_store(&a->st->negotiated_rate, rate);

    int16_t *block = (int16_t *)calloc(period_frames * CHANNELS, sizeof(int16_t));
    if (!block) {
        snd_pcm_close(pcm);
        atomic_store(&a->st->quit, 1);
        return NULL;
    }

    snd_pcm_prepare(pcm);

    while (!atomic_load(&a->st->quit)) {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, block, period_frames);
        if (got == -EPIPE) {
            atomic_fetch_add(&a->st->xrun_count, 1);
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

        // Publish per-block stats for the meter.
        float pl, rl, pr, rr;
        block_stats_s16_stereo(block, (size_t)got, &pl, &rl, &pr, &rr);
        atomic_store(&a->st->peak_left,    (int)pl);
        atomic_store(&a->st->peak_right,   (int)pr);
        atomic_store(&a->st->rms_left_q15, (int)rl);
        atomic_store(&a->st->rms_right_q15,(int)rr);
        atomic_fetch_add(&a->st->block_seq, 1);

        // Push to ring. If full, oldest data was meant to be drained by
        // the recorder; on the rare "ring full" we drop the new block and
        // count it as an xrun so the user sees it.
        size_t want = (size_t)got * CHANNELS;
        size_t wrote = ring_write(a->ring, block, want);
        if (wrote < want) atomic_fetch_add(&a->st->xrun_count, 1);
    }

    free(block);
    snd_pcm_close(pcm);
    return NULL;
}
