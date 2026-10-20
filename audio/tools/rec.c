// Step (b): record the POD X3's 8 USB input channels to WAV.
//   rec [seconds] [prefix]
// Writes <prefix>.wav (8 ch) and <prefix>-main.wav, -tone1.wav, -tone2.wav, -di.wav
// (stereo pairs 1+2, 3+4, 5+6, 7+8). 24-bit, 48 kHz. Silence is sent on OUT.
#include "../common/wav.h"
#include "../usb/podx3_usb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *pair_names[4] = {"main", "tone1", "tone2", "di"};

typedef struct {
    wav_writer all, pair[4];
    int peak[PODX3_IN_CHANNELS];
    uint64_t frames, bad_len, gaps;
    uint64_t last_t;
} rec_state;

static void on_in(void *user, const uint8_t *data, int len, int status, uint64_t t, int idx, int n) {
    (void)status, (void)idx, (void)n, (void)t;
    rec_state *s = user;
    if (len % PODX3_IN_FRAME_BYTES) s->bad_len++;
    int frames = len / PODX3_IN_FRAME_BYTES;
    if (!frames) {
        if (s->frames) s->gaps++; // empty packet after the stream started
        return;
    }
    wav_write(&s->all, data, (size_t)frames);
    for (int f = 0; f < frames; f++) {
        const uint8_t *fr = data + f * PODX3_IN_FRAME_BYTES;
        for (int p = 0; p < 4; p++) wav_write(&s->pair[p], fr + p * 6, 1);
        for (int c = 0; c < PODX3_IN_CHANNELS; c++) {
            int v = abs(podx3_s24_get(fr + c * 3));
            if (v > s->peak[c]) s->peak[c] = v;
        }
    }
    s->frames += (uint64_t)frames;
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 5;
    const char *prefix = argc > 2 ? argv[2] : "capture";
    rec_state s = {0};
    char path[512];
    snprintf(path, sizeof path, "%s.wav", prefix);
    if (wav_open(&s.all, path, 8, PODX3_RATE)) {
        perror(path);
        return 1;
    }
    for (int p = 0; p < 4; p++) {
        snprintf(path, sizeof path, "%s-%s.wav", prefix, pair_names[p]);
        if (wav_open(&s.pair[p], path, 2, PODX3_RATE)) {
            perror(path);
            return 1;
        }
    }

    podx3_t *dev;
    if (podx3_open(&dev, false)) return 1;
    static volatile atomic_bool stop;
    podx3_install_stop_signals(&stop);
    podx3_stream_cfg cfg = {.enable_in = true, .enable_out = true, .in_cb = on_in, .user = &s};
    printf("recording %.1f s to %s*.wav - play something\n", seconds, prefix);
    fflush(stdout);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = podx3_stream_start(dev, &cfg);
    if (r == 0) {
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (atomic_load(&stop) || (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9 >= seconds) break;
            if (podx3_poll(dev, 100) < 0) break;
        }
        r = podx3_stream_stop(dev);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    const podx3_stats *st = podx3_get_stats(dev);
    podx3_close(dev);

    wav_close(&s.all, PODX3_RATE);
    for (int p = 0; p < 4; p++) wav_close(&s.pair[p], PODX3_RATE);
    printf("%llu frames in %.2f s (%.1f frames/s), empty packets after start %llu, bad lengths %llu, "
           "transfer errors %llu\n",
           s.frames, el, s.frames / el, s.gaps, s.bad_len, st->xfer_errors);
    printf("peak dBFS:");
    for (int c = 0; c < PODX3_IN_CHANNELS; c++)
        printf(" %d:%.1f", c + 1, s.peak[c] ? 20 * log10(s.peak[c] / 8388608.0) : -999.0);
    printf("\n");
    return r || !s.frames ? 1 : 0;
}
