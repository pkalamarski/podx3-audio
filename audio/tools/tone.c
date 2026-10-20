// Step (c): play a sine on EP 0x02 (IN runs alongside, as the device requires).
//   tone [seconds] [--freq HZ] [--db DBFS] [--latency] [--xfers N] [--pkts N]
// --latency  instead of a sine, send a click every 500 ms and look for it on
//            input channels 1+2 (needs the POD to route USB playback to its
//            main output, or a physical loop cable).
#include "../usb/podx3_usb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLICK_PERIOD 24000 // frames

typedef struct {
    double phase, step, amp;
    bool latency;
    uint64_t out_pos, in_pos; // frames sent / received
    // latency measurement
    uint64_t click_at;     // out frame of last click
    bool waiting;
    double thresh;
    double sum_lat;
    int n_lat;
    int peak_in;
} tone_state;

static void on_out(void *user, uint8_t *data, int nframes) {
    tone_state *s = user;
    for (int i = 0; i < nframes; i++, s->out_pos++) {
        int32_t v;
        if (s->latency) {
            bool click = s->out_pos % CLICK_PERIOD == 0;
            if (click) s->click_at = s->out_pos, s->waiting = true;
            v = click ? (int32_t)(s->amp * 8388607) : 0;
        } else {
            v = (int32_t)(sin(s->phase) * s->amp * 8388607);
            s->phase += s->step;
            if (s->phase > 2 * M_PI) s->phase -= 2 * M_PI;
        }
        podx3_s24_put(data + i * 6, v);
        podx3_s24_put(data + i * 6 + 3, v);
    }
}

static void on_in(void *user, const uint8_t *data, int len, int status, uint64_t t, int idx, int n) {
    (void)status, (void)t, (void)idx, (void)n;
    tone_state *s = user;
    for (int f = 0; f + PODX3_IN_FRAME_BYTES <= len; f += PODX3_IN_FRAME_BYTES, s->in_pos++) {
        int v = abs(podx3_s24_get(data + f));
        if (v > s->peak_in) s->peak_in = v;
        if (s->latency && s->waiting && v > s->thresh) {
            // in_pos and out_pos count from the same stream start; the OUT
            // queue is ahead of the wire, which this measurement includes.
            double ms = ((double)s->in_pos - (double)s->click_at) * 1000.0 / PODX3_RATE;
            if (ms >= 0 && ms < 400) {
                printf("click -> input: %.2f ms\n", ms);
                s->sum_lat += ms;
                s->n_lat++;
            }
            s->waiting = false;
        }
    }
}

int main(int argc, char **argv) {
    double seconds = 5, freq = 440, db = -20;
    int xfers = 6, pkts = 8; // --xfers: OUT transfers in flight (output latency)
    tone_state s = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--freq") && i + 1 < argc) freq = atof(argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--latency")) s.latency = true;
        else if (!strcmp(argv[i], "--xfers") && i + 1 < argc) xfers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pkts") && i + 1 < argc) pkts = atoi(argv[++i]);
        else seconds = atof(argv[i]);
    }
    s.amp = pow(10, db / 20);
    s.step = 2 * M_PI * freq / PODX3_RATE;
    s.thresh = 8388607 * s.amp * 0.25;

    podx3_t *dev;
    if (podx3_open(&dev, false)) return 1;
    static volatile atomic_bool stop;
    podx3_install_stop_signals(&stop);
    podx3_stream_cfg cfg = {
        .n_xfers_out = xfers, .pkts_per_xfer = pkts, .enable_in = true, .enable_out = true,
        .in_cb = on_in, .out_cb = on_out, .user = &s,
    };
    if (s.latency) printf("latency test: click every %d ms, %.0f dBFS, %d x %d\n", CLICK_PERIOD / 48, db, xfers, pkts);
    else printf("sine %.0f Hz at %.0f dBFS for %.1f s, %d transfers x %d packets\n", freq, db, seconds, xfers, pkts);

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
    printf("OUT %.1f frames/s, IN %.1f frames/s, transfer errors %llu\n", st->out_frames / el,
           st->in_frames / el, st->xfer_errors);
    printf("input ch1 peak: %.1f dBFS\n", s.peak_in ? 20 * log10(s.peak_in / 8388608.0) : -999.0);
    if (s.latency)
        printf(s.n_lat ? "mean click latency: %.2f ms over %d clicks\n" : "no clicks detected on input 1\n",
               s.n_lat ? s.sum_lat / s.n_lat : 0, s.n_lat);
    podx3_close(dev);
    return r ? 1 : 0;
}
