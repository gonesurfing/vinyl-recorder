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
    AudioBufferList *abl;       // 1 buffer, points at scratch
    int16_t         *scratch;
    UInt32           max_frames;
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

static AudioDeviceID resolve_device(const char *name) {
    AudioObjectID dev = kAudioObjectUnknown;

    if (!name || strcmp(name, "default") == 0) {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        UInt32 size = sizeof(dev);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                       0, NULL, &size, &dev) != noErr)
            return kAudioObjectUnknown;
        return dev;
    }

    AudioObjectPropertyAddress list_addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &list_addr,
                                       0, NULL, &size) != noErr || size == 0)
        return kAudioObjectUnknown;

    UInt32 count = size / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(count, sizeof(AudioDeviceID));
    if (!ids) return kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &list_addr,
                                   0, NULL, &size, ids) != noErr) {
        free(ids);
        return kAudioObjectUnknown;
    }

    for (UInt32 i = 0; i < count; i++) {
        // Skip devices with no input streams.
        AudioObjectPropertyAddress streams_addr = {
            kAudioDevicePropertyStreams,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain,
        };
        UInt32 stream_bytes = 0;
        if (AudioObjectGetPropertyDataSize(ids[i], &streams_addr,
                                           0, NULL, &stream_bytes) != noErr
            || stream_bytes == 0)
            continue;

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

    fprintf(stderr, "CoreAudio: \"%s\" (id=%u), nominal=%.0f Hz, requested=%u Hz%s\n",
            namebuf, (unsigned)dev, nominal, requested_rate,
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
        (UInt32)(nframes * CHANNELS * sizeof(int16_t));

    OSStatus r = AudioUnitRender(c->au, flags, ts, bus, nframes, c->abl);
    if (r != noErr) {
        atomic_fetch_add(&c->args->st->xrun_count, 1);
        return noErr;
    }

    int16_t *block = (int16_t *)c->abl->mBuffers[0].mData;

    float pl, rl, pr, rr;
    block_stats_s16_stereo(block, (size_t)nframes, &pl, &rl, &pr, &rr);
    atomic_store(&c->args->st->peak_left,    (int)pl);
    atomic_store(&c->args->st->peak_right,   (int)pr);
    atomic_store(&c->args->st->rms_left_q15, (int)rl);
    atomic_store(&c->args->st->rms_right_q15,(int)rr);
    atomic_fetch_add(&c->args->st->block_seq, 1);

    size_t want = (size_t)nframes * CHANNELS;
    size_t wrote = ring_write(c->args->ring, block, want);
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

    // Client format: S16 interleaved stereo at requested rate.
    AudioStreamBasicDescription fmt = {
        .mSampleRate       = (Float64)a->rate,
        .mFormatID         = kAudioFormatLinearPCM,
        .mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger
                           | kLinearPCMFormatFlagIsPacked,
        .mFramesPerPacket  = 1,
        .mChannelsPerFrame = CHANNELS,
        .mBitsPerChannel   = 16,
        .mBytesPerFrame    = CHANNELS * sizeof(int16_t),
        .mBytesPerPacket   = CHANNELS * sizeof(int16_t),
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

    ctx.scratch = calloc((size_t)max_frames * CHANNELS, sizeof(int16_t));
    ctx.abl = calloc(1, sizeof(AudioBufferList));
    if (!ctx.scratch || !ctx.abl) {
        fprintf(stderr, "CoreAudio: OOM allocating capture buffers\n");
        goto fail;
    }
    ctx.abl->mNumberBuffers = 1;
    ctx.abl->mBuffers[0].mNumberChannels = CHANNELS;
    ctx.abl->mBuffers[0].mDataByteSize =
        (UInt32)(max_frames * CHANNELS * sizeof(int16_t));
    ctx.abl->mBuffers[0].mData = ctx.scratch;

    AURenderCallbackStruct cb = { .inputProc = input_cb, .inputProcRefCon = &ctx };
    CHECK(AudioUnitSetProperty(ctx.au, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0,
                               &cb, sizeof(cb)),
          "SetInputCallback");

    CHECK(AudioUnitInitialize(ctx.au), "AudioUnitInitialize");
    CHECK(AudioOutputUnitStart(ctx.au), "AudioOutputUnitStart");
    started = 1;

    fprintf(stderr, "CoreAudio: capture started @ %u Hz, max_frames=%u\n",
            a->rate, (unsigned)max_frames);

    // Block until shutdown — capture happens on the CoreAudio RT thread.
    struct timespec slp = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    while (!atomic_load(&a->st->quit)) nanosleep(&slp, NULL);

fail:
    if (ctx.au) {
        if (started) AudioOutputUnitStop(ctx.au);
        AudioUnitUninitialize(ctx.au);
        AudioComponentInstanceDispose(ctx.au);
    }
    free(ctx.scratch);
    free(ctx.abl);
    if (!started) atomic_store(&a->st->quit, 1);
    return NULL;
}
