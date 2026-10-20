// Play the POD X3's USB input on the Mac's default output device.
//   monitor [--pair main|tone1|tone2|di] [--gain DB] [--target FRAMES] [--buffer FRAMES] [--silent]
// --silent  no playback, only the tuner tap (DI on a Unix socket, see common/tap.h)
// The POD runs on its own 48 kHz crystal and the Mac output on another, so the
// consumer reads through a linear-interpolating resampler whose ratio is nudged
// to keep the ring at the target fill level.
#include "../common/ring.h"
#include "../common/tap.h"
#include "../usb/podx3_usb.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    ring_t ring;
    int pair; // 0..3: input channels 2p+1, 2p+2
    float gain;
    double target;     // ring fill target in frames (render thread)
    double min_target; // --target; raised while the output pulls larger buffers
    // consumer (render thread) state
    float prev[2], cur[2];
    double frac, step, fill_avg, integ;
    bool primed;
    _Atomic uint64_t underruns;
    _Atomic uint32_t last_fill;
    _Atomic uint32_t last_step_ppm;
    _Atomic uint32_t min_fill, max_nframes; // reset by the reporter
    tap_t tap;
} mon_state;

static void on_in(void *user, const uint8_t *data, int len, int status, uint64_t t, int idx, int n) {
    (void)status, (void)t, (void)idx, (void)n;
    mon_state *s = user;
    if (len % PODX3_IN_FRAME_BYTES) return; // e.g. the 3-byte packet at stream start
    int frames = len / PODX3_IN_FRAME_BYTES;
    float tmp[8 * 2];
    for (int f = 0; f < frames; f++) {
        const uint8_t *fr = data + f * PODX3_IN_FRAME_BYTES + s->pair * 6;
        tmp[f * 2] = podx3_s24_get(fr) * (1.0f / 8388608.0f) * s->gain;
        tmp[f * 2 + 1] = podx3_s24_get(fr + 3) * (1.0f / 8388608.0f) * s->gain;
        tap_push(&s->tap, podx3_s24_get(data + f * PODX3_IN_FRAME_BYTES + 18) * (1.0f / 8388608.0f)); // input 7 (DI)
    }
    ring_write(&s->ring, tmp, (uint32_t)frames);
}

static OSStatus render(void *user, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 nframes, AudioBufferList *io) {
    (void)flags, (void)ts, (void)bus;
    mon_state *s = user;
    float *out = io->mBuffers[0].mData;
    uint32_t fill = ring_avail(&s->ring);
    if (fill < atomic_load(&s->min_fill)) atomic_store(&s->min_fill, fill);
    if (nframes > atomic_load(&s->max_nframes)) atomic_store(&s->max_nframes, nframes);
    // The output device decides the callback size (e.g. 512 frames on the
    // headphone jack). The ring must hold a whole callback plus USB jitter, or
    // every callback underruns.
    double want = nframes + 160.0 > s->min_target ? nframes + 160.0 : s->min_target;
    if (want != s->target) {
        s->target = want;
        s->integ = 0;
        s->primed = false; // re-prime: drops any excess at once
    }
    if (!s->primed) {
        if (fill < s->target) {
            memset(out, 0, nframes * 2 * sizeof(float));
            return noErr;
        }
        // Start exactly at the target: anything queued before the output ran
        // would otherwise be extra latency the slow drift loop has to bleed off.
        ring_skip(&s->ring, fill - (uint32_t)s->target);
        fill = (uint32_t)s->target;
        s->primed = true;
        s->fill_avg = fill;
    }
    // Drift control: PI on the smoothed fill error, clamped to ±0.5 %. The
    // integral settles at the real POD/Mac clock ratio, the P term holds the fill.
    s->fill_avg += (fill - s->fill_avg) * 0.05;
    double e = (s->fill_avg - s->target) / s->target;
    s->integ += e * 5e-7;
    if (s->integ > 0.001) s->integ = 0.001; // crystals differ by far less than 1000 ppm
    if (s->integ < -0.001) s->integ = -0.001;
    double adj = e * 0.001 + s->integ;
    if (adj > 0.005) adj = 0.005;
    if (adj < -0.005) adj = -0.005;
    s->step = 1.0 + adj;
    atomic_store(&s->last_fill, fill);
    atomic_store(&s->last_step_ppm, (uint32_t)(int32_t)lrint(adj * 1e6));

    for (UInt32 i = 0; i < nframes; i++) {
        while (s->frac >= 1.0) {
            float nf[2];
            if (!ring_pop(&s->ring, nf)) {
                atomic_fetch_add(&s->underruns, 1);
                s->primed = false; // re-prime to the target fill
                memset(out + i * 2, 0, (nframes - i) * 2 * sizeof(float));
                return noErr;
            }
            s->prev[0] = s->cur[0], s->prev[1] = s->cur[1];
            s->cur[0] = nf[0], s->cur[1] = nf[1];
            s->frac -= 1.0;
        }
        out[i * 2] = s->prev[0] + (s->cur[0] - s->prev[0]) * (float)s->frac;
        out[i * 2 + 1] = s->prev[1] + (s->cur[1] - s->prev[1]) * (float)s->frac;
        s->frac += s->step;
    }
    return noErr;
}

static void print_device(AudioDeviceID devid) {
    Float64 rate = 0;
    UInt32 sz, bufsz = 0, lat = 0, safety = 0;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    sz = sizeof rate;
    AudioObjectGetPropertyData(devid, &a, 0, NULL, &sz, &rate);
    a.mSelector = kAudioDevicePropertyBufferFrameSize;
    sz = sizeof bufsz;
    AudioObjectGetPropertyData(devid, &a, 0, NULL, &sz, &bufsz);
    a.mSelector = kAudioDevicePropertyLatency;
    a.mScope = kAudioObjectPropertyScopeOutput;
    sz = sizeof lat;
    AudioObjectGetPropertyData(devid, &a, 0, NULL, &sz, &lat);
    a.mSelector = kAudioDevicePropertySafetyOffset;
    sz = sizeof safety;
    AudioObjectGetPropertyData(devid, &a, 0, NULL, &sz, &safety);
    CFStringRef name = NULL;
    a.mSelector = kAudioObjectPropertyName;
    a.mScope = kAudioObjectPropertyScopeGlobal;
    sz = sizeof name;
    AudioObjectGetPropertyData(devid, &a, 0, NULL, &sz, &name);
    char cname[128] = "?";
    if (name) CFStringGetCString(name, cname, sizeof cname, kCFStringEncodingUTF8), CFRelease(name);
    printf("output: %s, %.0f Hz, buffer %u, device latency %u + safety %u frames\n", cname, rate, bufsz, lat,
           safety);
    fflush(stdout);
}

// The default-output unit follows the user's output choice, but the buffer
// size we asked for only applied to the device active at start.
static void follow_default_output(UInt32 buffer_frames) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListenerBlock(
        kAudioObjectSystemObject, &a, dispatch_get_main_queue(),
        ^(UInt32 n, const AudioObjectPropertyAddress *addrs) {
            (void)n, (void)addrs;
            AudioDeviceID dev = 0;
            UInt32 sz = sizeof dev;
            AudioObjectPropertyAddress d = {kAudioHardwarePropertyDefaultOutputDevice,
                                            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &d, 0, NULL, &sz, &dev)) return;
            if (buffer_frames) {
                AudioObjectPropertyAddress b = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain};
                AudioObjectSetPropertyData(dev, &b, 0, NULL, sizeof buffer_frames, &buffer_frames);
            }
            print_device(dev);
        });
}

static int start_output(mon_state *s, UInt32 buffer_frames, AudioUnit *out_unit) {
    AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                      kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioUnit au;
    OSStatus e = AudioComponentInstanceNew(comp, &au);
    // Our stream is 48 kHz; the output unit converts to the device's rate.
    AudioStreamBasicDescription fmt = {
        .mSampleRate = PODX3_RATE, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 8, .mFramesPerPacket = 1, .mBytesPerFrame = 8, .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32,
    };
    if (!e) e = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
    AURenderCallbackStruct cb = {render, s};
    if (!e) e = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (!e && buffer_frames)
        AudioUnitSetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &buffer_frames,
                             sizeof buffer_frames); // best effort
    if (!e) e = AudioUnitInitialize(au);
    if (!e) e = AudioOutputUnitStart(au);
    if (e) {
        fprintf(stderr, "CoreAudio output setup failed: %d\n", (int)e);
        return -1;
    }

    AudioDeviceID devid = 0;
    UInt32 sz = sizeof devid;
    AudioUnitGetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &devid, &sz);
    print_device(devid);
    follow_default_output(buffer_frames);
    *out_unit = au;
    return 0;
}

int main(int argc, char **argv) {
    static const char *pairs[4] = {"main", "tone1", "tone2", "di"};
    mon_state s = {.gain = 1, .min_target = 288, .frac = 1.0, .step = 1.0};
    UInt32 buffer_frames = 128;
    int n_xfers = 16;
    bool silent = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pair") && i + 1 < argc) {
            i++;
            for (int p = 0; p < 4; p++)
                if (!strcmp(argv[i], pairs[p])) s.pair = p;
        } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) s.gain = powf(10, (float)atof(argv[++i]) / 20);
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) s.min_target = atof(argv[++i]);
        else if (!strcmp(argv[i], "--buffer") && i + 1 < argc) buffer_frames = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--xfers") && i + 1 < argc) n_xfers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--silent")) silent = true;
    }
    s.target = s.min_target;
    ring_init(&s.ring, 2, 8192);
    if (tap_start(&s.tap, NULL)) fprintf(stderr, "warning: tuner tap unavailable\n");
    else printf("tuner tap: %s\n", s.tap.path);

    static volatile atomic_bool stop;
    podx3_install_stop_signals(&stop);
    podx3_stream_cfg cfg = {.n_xfers = n_xfers, .pkts_per_xfer = 8, .enable_in = true, .enable_out = true,
                            .in_cb = on_in, .user = &s};
    AudioUnit au = NULL;
    bool output_started = false;
    // One USB session per iteration: if the stream dies (a failed resubmit, the
    // device reset or replugged), drain, close and start over after a second.
    for (int session = 0; !atomic_load(&stop); session++) {
        if (session) {
            for (int i = 0; i < 10 && !atomic_load(&stop); i++) usleep(100000);
            if (atomic_load(&stop)) break;
            printf("restarting USB stream\n");
            fflush(stdout);
        }
        podx3_t *dev;
        if (podx3_open(&dev, false)) {
            if (!session) return 1;
            continue;
        }
        if (podx3_stream_start(dev, &cfg)) {
            podx3_close(dev);
            if (!session) return 1;
            continue;
        }
        // Before the AudioUnit exists, so only our thread and libusb's event thread are affected.
        if (podx3_make_all_threads_realtime()) fprintf(stderr, "warning: could not make the USB threads real-time\n");
        if (!output_started) {
            output_started = true;
            if (silent) {
                printf("silent: tuner tap only. Ctrl-C to stop.\n");
            } else if (start_output(&s, buffer_frames, &au)) {
                atomic_store(&stop, true);
            } else {
                printf("monitoring POD X3 %s (inputs %d+%d), ring target %.0f frames (%.1f ms). Ctrl-C to stop.\n",
                       pairs[s.pair], s.pair * 2 + 1, s.pair * 2 + 2, s.target, s.target / 48.0);
            }
            fflush(stdout);
        }
        atomic_store(&s.min_fill, UINT32_MAX);
        const podx3_stats *st = podx3_get_stats(dev);
        struct timespec tl, tn;
        clock_gettime(CLOCK_MONOTONIC, &tl);
        uint64_t last_in = st->in_frames;
        while (!atomic_load(&stop)) {
            if (podx3_poll(dev, 100) < 0) break;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true); // device-change listener (main queue)
            clock_gettime(CLOCK_MONOTONIC, &tn);
            double dt = (tn.tv_sec - tl.tv_sec) + (tn.tv_nsec - tl.tv_nsec) * 1e-9;
            if (dt >= 2.0) {
                printf("USB in %.0f frames/s | ring fill %u/%.0f (min %u), callback max %u frames, rate adj %+d ppm, "
                       "underruns %llu\n",
                       (st->in_frames - last_in) / dt, atomic_load(&s.last_fill), s.target, atomic_load(&s.min_fill),
                       atomic_load(&s.max_nframes), (int32_t)atomic_load(&s.last_step_ppm),
                       (unsigned long long)atomic_load(&s.underruns));
                fflush(stdout);
                atomic_store(&s.min_fill, UINT32_MAX);
                atomic_store(&s.max_nframes, 0);
                last_in = st->in_frames;
                tl = tn;
            }
        }
        podx3_stream_stop(dev);
        if (!atomic_load(&stop)) fprintf(stderr, "USB stream stopped unexpectedly\n");
        podx3_close(dev);
    }
    if (au) {
        AudioOutputUnitStop(au);
        AudioComponentInstanceDispose(au);
    }
    tap_stop(&s.tap);
    return 0;
}
