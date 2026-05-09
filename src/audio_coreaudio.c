#include "audio.h"
#include "level.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CA_INPUT_BUS  1
#define CA_OUTPUT_BUS 0

typedef struct {
    AudioUnit        au;
    AudioBufferList *abl;        // 1 buffer, points at au_scratch
    int16_t         *au_scratch; // raw AU output: device_channels * max_frames samples
    int16_t         *ring_block; // mixed-down stereo: 2 * max_frames samples
    UInt32           max_frames;
    UInt32           device_channels; // 1, 2, or N — what the AU actually delivers
    _Atomic int      last_au_err;     // OSStatus from most recent AudioUnitRender failure
    audio_args_t    *args;
} ca_ctx_t;

static const char *os_err_str(OSStatus s, char *buf, size_t n) {
    // Try four-char-code first; OSStatus is often a packed FourCC.
    UInt32 v = (UInt32)s;
    unsigned char c[4] = {
        (unsigned char)((v >> 24) & 0xff),
        (unsigned char)((v >> 16) & 0xff),
        (unsigned char)((v >>  8) & 0xff),
        (unsigned char)( v        & 0xff),
    };
    int printable = 1;
    for (int i = 0; i < 4; i++) if (c[i] < 0x20 || c[i] > 0x7e) { printable = 0; break; }
    if (printable)
        snprintf(buf, n, "'%c%c%c%c' (%d)", c[0], c[1], c[2], c[3], (int)s);
    else
        snprintf(buf, n, "%d", (int)s);
    return buf;
}

#define CHECK(expr, label) do {                                          \
    OSStatus _rc = (expr);                                               \
    if (_rc != noErr) {                                                  \
        char _eb[32];                                                    \
        fprintf(stderr, "CoreAudio: %s failed: %s\n",                    \
                label, os_err_str(_rc, _eb, sizeof(_eb)));               \
        goto fail;                                                       \
    }                                                                    \
} while (0)

static int device_has_input(AudioDeviceID id) {
    AudioObjectPropertyAddress streams_addr = {
        kAudioDevicePropertyStreams,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain,
    };
    UInt32 stream_bytes = 0;
    if (AudioObjectGetPropertyDataSize(id, &streams_addr,
                                       0, NULL, &stream_bytes) != noErr)
        return 0;
    return stream_bytes > 0;
}

static AudioDeviceID default_input_device(void) {
    AudioDeviceID dev = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                               0, NULL, &size, &dev);
    return dev;
}

// Allocate and return the list of all AudioDeviceIDs. Caller frees.
// On failure returns NULL and sets *count to 0.
static AudioDeviceID *all_device_ids(UInt32 *count) {
    *count = 0;
    AudioObjectPropertyAddress list_addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &list_addr,
                                       0, NULL, &size) != noErr || size == 0)
        return NULL;
    UInt32 n = size / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(n, sizeof(AudioDeviceID));
    if (!ids) return NULL;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &list_addr,
                                   0, NULL, &size, ids) != noErr) {
        free(ids);
        return NULL;
    }
    *count = n;
    return ids;
}

static AudioDeviceID resolve_device(const char *name) {
    if (!name || strcmp(name, "default") == 0)
        return default_input_device();

    UInt32 count = 0;
    AudioDeviceID *ids = all_device_ids(&count);
    if (!ids) return kAudioObjectUnknown;
    AudioDeviceID dev = kAudioObjectUnknown;

    for (UInt32 i = 0; i < count; i++) {
        if (!device_has_input(ids[i])) continue;

        // Match against UID (exact) or name (substring, case-insensitive).
        CFStringRef uid = NULL, dname = NULL;
        UInt32 sz;

        AudioObjectPropertyAddress uid_addr = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        sz = sizeof(uid);
        AudioObjectGetPropertyData(ids[i], &uid_addr, 0, NULL, &sz, &uid);

        AudioObjectPropertyAddress name_addr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        sz = sizeof(dname);
        AudioObjectGetPropertyData(ids[i], &name_addr, 0, NULL, &sz, &dname);

        CFStringRef needle = CFStringCreateWithCString(NULL, name,
                                                       kCFStringEncodingUTF8);
        int hit = 0;
        if (uid && CFStringCompare(uid, needle, 0) == kCFCompareEqualTo) hit = 1;
        if (!hit && dname) {
            CFRange r = CFStringFind(dname, needle, kCFCompareCaseInsensitive);
            if (r.location != kCFNotFound) hit = 1;
        }
        if (needle) CFRelease(needle);
        if (uid)    CFRelease(uid);
        if (dname)  CFRelease(dname);

        if (hit) { dev = ids[i]; break; }
    }

    free(ids);
    return dev;
}

// Fetch the device's discrete and range-based supported input sample rates.
// Returns a calloc'd array (caller frees) and writes the count to *out_n.
// On failure returns NULL with *out_n = 0.
static AudioValueRange *available_rates(AudioDeviceID dev, UInt32 *out_n) {
    *out_n = 0;
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr
        || size == 0)
        return NULL;
    UInt32 n = size / sizeof(AudioValueRange);
    AudioValueRange *r = calloc(n, sizeof(AudioValueRange));
    if (!r) return NULL;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, r) != noErr) {
        free(r);
        return NULL;
    }
    *out_n = n;
    return r;
}

static int rate_is_supported(AudioDeviceID dev, unsigned hz) {
    UInt32 n = 0;
    AudioValueRange *r = available_rates(dev, &n);
    if (!r) return 0;
    int ok = 0;
    for (UInt32 i = 0; i < n; i++) {
        if ((double)hz >= r[i].mMinimum && (double)hz <= r[i].mMaximum) {
            ok = 1;
            break;
        }
    }
    free(r);
    return ok;
}

static Float64 read_current_rate(AudioDeviceID dev) {
    Float64 rate = 0.0;
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain,
    };
    UInt32 sz = sizeof(rate);
    AudioObjectGetPropertyData(dev, &addr, 0, NULL, &sz, &rate);
    return rate;
}

unsigned audio_pick_rate(const char *device, unsigned requested) {
    AudioDeviceID dev = resolve_device(device);
    if (dev == kAudioObjectUnknown) return requested;

    Float64 current = read_current_rate(dev);
    if (current <= 0) return requested;
    if ((unsigned)current == requested) return requested;

    if (!rate_is_supported(dev, requested)) {
        fprintf(stderr,
                "CoreAudio: %u Hz not supported by device; using current %u Hz "
                "(see --list-devices for supported rates)\n",
                requested, (unsigned)current);
        return (unsigned)current;
    }

    AudioObjectPropertyAddress rate_addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain,
    };
    Float64 want = (Float64)requested;
    OSStatus rc = AudioObjectSetPropertyData(dev, &rate_addr, 0, NULL,
                                             sizeof(want), &want);
    if (rc != noErr) {
        char eb[32];
        fprintf(stderr,
                "CoreAudio: failed to set device rate to %u Hz: %s; using %u Hz\n",
                requested, os_err_str(rc, eb, sizeof(eb)), (unsigned)current);
        return (unsigned)current;
    }

    // The rate change is asynchronous; poll up to ~500 ms for it to settle.
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
    for (int i = 0; i < 50; i++) {
        nanosleep(&ts, NULL);
        current = read_current_rate(dev);
        if ((unsigned)current == requested) break;
    }
    if ((unsigned)current != requested) {
        fprintf(stderr,
                "CoreAudio: device did not switch to %u Hz; reports %u Hz\n",
                requested, (unsigned)current);
        return (unsigned)current;
    }

    fprintf(stderr, "CoreAudio: device set to %u Hz\n", requested);
    return requested;
}

static void cfstr_to_c(CFStringRef s, char *out, size_t n) {
    if (!s || !CFStringGetCString(s, out, (CFIndex)n, kCFStringEncodingUTF8))
        snprintf(out, n, "(unknown)");
}

void audio_list_devices(void) {
    AudioDeviceID def = default_input_device();
    UInt32 count = 0;
    AudioDeviceID *ids = all_device_ids(&count);
    if (!ids) {
        fprintf(stderr, "CoreAudio: failed to enumerate devices\n");
        return;
    }

    printf("CoreAudio input devices (use -d <name-substring|UID>):\n\n");
    int printed = 0;
    for (UInt32 i = 0; i < count; i++) {
        if (!device_has_input(ids[i])) continue;

        CFStringRef cf_name = NULL, cf_uid = NULL;
        UInt32 sz;
        AudioObjectPropertyAddress name_addr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        sz = sizeof(cf_name);
        AudioObjectGetPropertyData(ids[i], &name_addr, 0, NULL, &sz, &cf_name);

        AudioObjectPropertyAddress uid_addr = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        sz = sizeof(cf_uid);
        AudioObjectGetPropertyData(ids[i], &uid_addr, 0, NULL, &sz, &cf_uid);

        Float64 rate = read_current_rate(ids[i]);

        char name_c[256], uid_c[256];
        cfstr_to_c(cf_name, name_c, sizeof(name_c));
        cfstr_to_c(cf_uid,  uid_c,  sizeof(uid_c));

        printf("  %s %s\n", (ids[i] == def) ? "*" : " ", name_c);
        printf("      UID:     %s\n", uid_c);
        printf("      current: %.0f Hz\n", rate);

        UInt32 nrates = 0;
        AudioValueRange *ranges = available_rates(ids[i], &nrates);
        if (ranges && nrates > 0) {
            printf("      rates:   ");
            for (UInt32 j = 0; j < nrates; j++) {
                if (ranges[j].mMinimum == ranges[j].mMaximum)
                    printf("%.0f", ranges[j].mMinimum);
                else
                    printf("%.0f-%.0f", ranges[j].mMinimum, ranges[j].mMaximum);
                if (j + 1 < nrates) printf(", ");
            }
            printf(" Hz\n");
        }
        free(ranges);

        if (cf_name) CFRelease(cf_name);
        if (cf_uid)  CFRelease(cf_uid);
        printed++;
    }
    if (!printed) printf("  (no input-capable devices found)\n");
    printf("\n  * = current system default input (selected by -d default)\n");
    free(ids);
}

// Sum input channel count across all input streams of the device.
static UInt32 device_input_channels(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr
        || size == 0)
        return 0;
    AudioBufferList *abl = malloc(size);
    if (!abl) return 0;
    UInt32 channels = 0;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, abl) == noErr) {
        for (UInt32 i = 0; i < abl->mNumberBuffers; i++)
            channels += abl->mBuffers[i].mNumberChannels;
    }
    free(abl);
    return channels;
}

static void log_device_info(AudioDeviceID dev, unsigned int requested_rate) {
    char namebuf[256] = "(unknown)";
    AudioObjectPropertyAddress name_addr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    CFStringRef cfname = NULL;
    UInt32 sz = sizeof(cfname);
    if (AudioObjectGetPropertyData(dev, &name_addr, 0, NULL, &sz, &cfname) == noErr
        && cfname) {
        CFStringGetCString(cfname, namebuf, sizeof(namebuf),
                           kCFStringEncodingUTF8);
        CFRelease(cfname);
    }

    Float64 nominal = 0.0;
    AudioObjectPropertyAddress rate_addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain,
    };
    sz = sizeof(nominal);
    AudioObjectGetPropertyData(dev, &rate_addr, 0, NULL, &sz, &nominal);

    UInt32 dev_ch = device_input_channels(dev);

    fprintf(stderr,
            "CoreAudio: \"%s\" (id=%u), nominal=%.0f Hz, requested=%u Hz, "
            "device input channels=%u (client wants %u)%s\n",
            namebuf, (unsigned)dev, nominal, requested_rate,
            (unsigned)dev_ch, (unsigned)CHANNELS,
            (nominal > 0 && (unsigned)nominal != requested_rate)
                ? " — AU will resample"
                : "");
}

static OSStatus input_cb(void *ud,
                         AudioUnitRenderActionFlags *flags,
                         const AudioTimeStamp *ts,
                         UInt32 bus,
                         UInt32 nframes,
                         AudioBufferList *ioData) {
    (void)ioData;
    ca_ctx_t *c = (ca_ctx_t *)ud;

    if (nframes > c->max_frames) {
        atomic_fetch_add(&c->args->st->xrun_count, 1);
        return noErr;
    }

    c->abl->mBuffers[0].mDataByteSize =
        (UInt32)(nframes * c->device_channels * sizeof(int16_t));

    OSStatus r = AudioUnitRender(c->au, flags, ts, bus, nframes, c->abl);
    if (r != noErr) {
        atomic_store(&c->last_au_err, (int)r);
        atomic_fetch_add(&c->args->st->xrun_count, 1);
        return noErr;
    }

    const int16_t *src = (const int16_t *)c->abl->mBuffers[0].mData;
    int16_t       *dst = c->ring_block;

    if (c->device_channels == 1) {
        // Mono → stereo: duplicate the single channel into both L and R.
        for (UInt32 i = 0; i < nframes; i++) {
            int16_t s = src[i];
            dst[2 * i + 0] = s;
            dst[2 * i + 1] = s;
        }
    } else if (c->device_channels == 2) {
        memcpy(dst, src, (size_t)nframes * 2 * sizeof(int16_t));
    } else {
        // N>2 channels: take the first two as L/R and ignore the rest.
        const UInt32 stride = c->device_channels;
        for (UInt32 i = 0; i < nframes; i++) {
            dst[2 * i + 0] = src[i * stride + 0];
            dst[2 * i + 1] = src[i * stride + 1];
        }
    }

    float pl, rl, pr, rr;
    int clips = 0;
    block_stats_s16_stereo(dst, (size_t)nframes, &pl, &rl, &pr, &rr, &clips);
    atomic_store(&c->args->st->peak_left,    (int)pl);
    atomic_store(&c->args->st->peak_right,   (int)pr);
    atomic_store(&c->args->st->rms_left_q15, (int)rl);
    atomic_store(&c->args->st->rms_right_q15,(int)rr);
    atomic_fetch_add(&c->args->st->block_seq, 1);
    if (clips > 0) atomic_fetch_add(&c->args->st->clip_count, clips);

    size_t want = (size_t)nframes * CHANNELS;
    size_t wrote = ring_write(c->args->ring, dst, want);
    if (wrote < want) atomic_fetch_add(&c->args->st->xrun_count, 1);

    return noErr;
}

void *audio_thread_main(void *arg) {
    audio_args_t *a = (audio_args_t *)arg;
    ca_ctx_t ctx = { .args = a };
    int started = 0;

    AudioDeviceID dev = resolve_device(a->device);
    if (dev == kAudioObjectUnknown) {
        fprintf(stderr, "CoreAudio: no input device matching \"%s\"\n",
                a->device ? a->device : "(null)");
        goto fail;
    }
    log_device_info(dev, a->rate);

    AudioComponentDescription desc = {
        .componentType         = kAudioUnitType_Output,
        .componentSubType      = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "CoreAudio: HALOutput component not found\n");
        goto fail;
    }
    CHECK(AudioComponentInstanceNew(comp, &ctx.au), "AudioComponentInstanceNew");

    UInt32 enable = 1, disable = 0;
    CHECK(AudioUnitSetProperty(ctx.au, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, CA_INPUT_BUS,
                               &enable, sizeof(enable)),
          "EnableIO(input)");
    CHECK(AudioUnitSetProperty(ctx.au, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, CA_OUTPUT_BUS,
                               &disable, sizeof(disable)),
          "DisableIO(output)");

    CHECK(AudioUnitSetProperty(ctx.au, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0,
                               &dev, sizeof(dev)),
          "CurrentDevice");

    UInt32 dev_ch = device_input_channels(dev);
    if (dev_ch == 0) {
        fprintf(stderr, "CoreAudio: device reports 0 input channels\n");
        goto fail;
    }
    ctx.device_channels = dev_ch;

    // Client format matches the device's channel count; we mix to stereo
    // ourselves in the callback before pushing into the (stereo) ring.
    AudioStreamBasicDescription fmt = {
        .mSampleRate       = (Float64)a->rate,
        .mFormatID         = kAudioFormatLinearPCM,
        .mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger
                           | kLinearPCMFormatFlagIsPacked,
        .mFramesPerPacket  = 1,
        .mChannelsPerFrame = dev_ch,
        .mBitsPerChannel   = 16,
        .mBytesPerFrame    = dev_ch * sizeof(int16_t),
        .mBytesPerPacket   = dev_ch * sizeof(int16_t),
    };
    CHECK(AudioUnitSetProperty(ctx.au, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, CA_INPUT_BUS,
                               &fmt, sizeof(fmt)),
          "StreamFormat(client)");

    UInt32 max_frames = 0, sz = sizeof(max_frames);
    CHECK(AudioUnitGetProperty(ctx.au, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0,
                               &max_frames, &sz),
          "Get MaximumFramesPerSlice");
    if (max_frames < 4096) max_frames = 4096;
    ctx.max_frames = max_frames;

    ctx.au_scratch = calloc((size_t)max_frames * dev_ch,    sizeof(int16_t));
    ctx.ring_block = calloc((size_t)max_frames * CHANNELS,  sizeof(int16_t));
    ctx.abl        = calloc(1, sizeof(AudioBufferList));
    if (!ctx.au_scratch || !ctx.ring_block || !ctx.abl) {
        fprintf(stderr, "CoreAudio: OOM allocating capture buffers\n");
        goto fail;
    }
    ctx.abl->mNumberBuffers = 1;
    ctx.abl->mBuffers[0].mNumberChannels = dev_ch;
    ctx.abl->mBuffers[0].mDataByteSize =
        (UInt32)(max_frames * dev_ch * sizeof(int16_t));
    ctx.abl->mBuffers[0].mData = ctx.au_scratch;

    AURenderCallbackStruct cb = { .inputProc = input_cb, .inputProcRefCon = &ctx };
    CHECK(AudioUnitSetProperty(ctx.au, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0,
                               &cb, sizeof(cb)),
          "SetInputCallback");

    CHECK(AudioUnitInitialize(ctx.au), "AudioUnitInitialize");

    // Diagnostic: query the formats AU actually accepted on both scopes of bus 1.
    {
        AudioStreamBasicDescription got;
        UInt32 got_sz = sizeof(got);
        if (AudioUnitGetProperty(ctx.au, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Input, CA_INPUT_BUS,
                                 &got, &got_sz) == noErr) {
            fprintf(stderr,
                    "  bus1 input scope  (device): %.0f Hz, %u ch, %u bits, flags=0x%x\n",
                    got.mSampleRate, (unsigned)got.mChannelsPerFrame,
                    (unsigned)got.mBitsPerChannel, (unsigned)got.mFormatFlags);
        }
        got_sz = sizeof(got);
        if (AudioUnitGetProperty(ctx.au, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Output, CA_INPUT_BUS,
                                 &got, &got_sz) == noErr) {
            fprintf(stderr,
                    "  bus1 output scope (client): %.0f Hz, %u ch, %u bits, flags=0x%x\n",
                    got.mSampleRate, (unsigned)got.mChannelsPerFrame,
                    (unsigned)got.mBitsPerChannel, (unsigned)got.mFormatFlags);
        }
    }

    CHECK(AudioOutputUnitStart(ctx.au), "AudioOutputUnitStart");
    started = 1;

    fprintf(stderr,
            "CoreAudio: capture started @ %u Hz, max_frames=%u, device_channels=%u\n",
            a->rate, (unsigned)max_frames, (unsigned)dev_ch);
    atomic_store(&a->st->negotiated_rate, (unsigned)a->rate);

    // Block until shutdown — capture happens on the CoreAudio RT thread.
    // Surface the first AudioUnitRender error code (and any change after that)
    // to stderr exactly once so we don't spam.
    struct timespec slp = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    int last_logged_err = 0;
    while (!atomic_load(&a->st->quit)) {
        nanosleep(&slp, NULL);
        int err = atomic_load(&ctx.last_au_err);
        if (err != 0 && err != last_logged_err) {
            char eb[32];
            fprintf(stderr, "CoreAudio: AudioUnitRender failing with %s\n",
                    os_err_str((OSStatus)err, eb, sizeof(eb)));
            last_logged_err = err;
        }
    }

fail:
    if (ctx.au) {
        if (started) AudioOutputUnitStop(ctx.au);
        AudioUnitUninitialize(ctx.au);
        AudioComponentInstanceDispose(ctx.au);
    }
    free(ctx.au_scratch);
    free(ctx.ring_block);
    free(ctx.abl);
    if (!started) atomic_store(&a->st->quit, 1);
    return NULL;
}
