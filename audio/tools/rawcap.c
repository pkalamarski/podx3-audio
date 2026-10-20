// Step (a): stream raw isochronous IN packets from EP 0x86 and describe them.
//   rawcap [seconds] [--init] [--settle MS] [--xfers N] [--pkts N]
// OUT (silence) always runs alongside IN: the device must not be polled for IN alone.
// --init     run the Linux 0x67 init sequence first
#include "../usb/podx3_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <time.h>

#define BUCKETS 200 // 50 ms each

typedef struct {
    uint64_t t0;
    double tick_ms;
    int timeline[BUCKETS];
    int dumped;
    uint64_t last_xfer_t;
    int gap_hist[12]; // completion-to-completion gap, 0.5 ms buckets (last = >= 5.5 ms)
    double max_gap_ms;
    int bad_len;
    int peak[PODX3_IN_CHANNELS];
} cap_state;

static void on_in(void *user, const uint8_t *data, int len, int status, uint64_t host_time, int idx,
                  int n) {
    (void)n;
    cap_state *s = user;
    if (idx == 0) {
        if (s->last_xfer_t) {
            double g = (host_time - s->last_xfer_t) * s->tick_ms;
            int b = (int)(g / 0.5);
            s->gap_hist[b > 11 ? 11 : b]++;
            if (g > s->max_gap_ms) s->max_gap_ms = g;
        }
        s->last_xfer_t = host_time;
    }
    if (len == 0) return;
    int b = (int)((host_time - s->t0) * s->tick_ms / 50.0);
    if (b >= 0 && b < BUCKETS) s->timeline[b]++;
    if (len % PODX3_IN_FRAME_BYTES) s->bad_len++;
    if (s->dumped < 4) {
        printf("packet (idx %d, status %d, %d bytes):", idx, status, len);
        for (int i = 0; i < len; i++) printf("%s%02x", i % 24 ? " " : "\n  ", data[i]);
        printf("\n");
        s->dumped++;
    }
    for (int f = 0; f + PODX3_IN_FRAME_BYTES <= len; f += PODX3_IN_FRAME_BYTES)
        for (int c = 0; c < PODX3_IN_CHANNELS; c++) {
            int v = abs(podx3_s24_get(data + f + c * 3));
            if (v > s->peak[c]) s->peak[c] = v;
        }
}

int main(int argc, char **argv) {
    double seconds = 2;
    bool init = false;
    int xfers = 8, pkts = 8, settle_ms = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--init")) init = true;
        else if (!strcmp(argv[i], "--xfers") && i + 1 < argc) xfers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pkts") && i + 1 < argc) pkts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--settle") && i + 1 < argc) settle_ms = atoi(argv[++i]);
        else seconds = atof(argv[i]);
    }

    podx3_t *dev;
    if (podx3_open(&dev, true)) return 1;
    if (getenv("PODX3_RT") && podx3_make_all_threads_realtime()) fprintf(stderr, "could not go real-time\n");
    if (init) {
        uint32_t fw = 0;
        int r = podx3_linux_init(dev, &fw);
        printf("linux init: r=%d firmware 0x%06x\n", r, fw);
    }

    if (settle_ms) {
        struct timespec ts = {settle_ms / 1000, (settle_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
    static volatile atomic_bool stop;
    podx3_install_stop_signals(&stop);
    cap_state s = {0};
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    s.tick_ms = (double)tb.numer / tb.denom / 1e6;
    s.t0 = mach_absolute_time();
    podx3_stream_cfg cfg = {
        .n_xfers = xfers, .pkts_per_xfer = pkts, .enable_in = true, .enable_out = true,
        .in_cb = on_in, .user = &s,
    };
    printf("streaming %.1f s, IN + OUT, %d transfers x %d packets\n", seconds, xfers, pkts);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = podx3_stream_start(dev, &cfg);
    if (r == 0) {
        while (!atomic_load(&stop)) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9 >= seconds) break;
            if (podx3_poll(dev, 100) < 0) break;
        }
        r = podx3_stream_stop(dev);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;

    const podx3_stats *st = podx3_get_stats(dev);
    printf("\n--- %.2f s ---\n", el);
    printf("IN  xfers %llu pkts %llu (empty %llu, pkt errors %llu) bytes %llu frames %llu -> %.1f frames/s\n",
           st->in_xfers, st->in_pkts, st->in_pkts_empty, st->in_pkts_err, st->in_bytes, st->in_frames,
           st->in_frames / el);
    printf("OUT xfers %llu pkts %llu frames %llu -> %.1f frames/s\n", st->out_xfers, st->out_pkts,
           st->out_frames, st->out_frames / el);
    printf("transfer errors %llu (last %d)\n", st->xfer_errors, st->last_error);
    printf("IN actual_length histogram:\n");
    for (int i = 0; i < 256; i++)
        if (st->len_hist[i]) printf("  %3d bytes (%d frames%s): %llu\n", i, i / 24, i % 24 ? ", NOT /24" : "", st->len_hist[i]);
    printf("non-empty IN packets per 50 ms:");
    for (int b = 0; b < BUCKETS && b * 50 < el * 1000; b++) printf("%s%d", b % 20 ? " " : "\n  ", s.timeline[b]);
    printf("\n");
    printf("IN transfer completion gaps (ms): ");
    for (int b = 0; b < 12; b++) printf("%s%.1f:%d", b ? " " : "", b * 0.5, s.gap_hist[b]);
    printf("  max %.2f\n", s.max_gap_ms);
    printf("packets with length %% 24 != 0: %d\n", s.bad_len);
    printf("peak per channel (dBFS):");
    for (int c = 0; c < PODX3_IN_CHANNELS; c++) {
        double db = s.peak[c] ? 20 * __builtin_log10((double)s.peak[c] / 8388608.0) : -999;
        printf(" %d:%.1f", c + 1, db);
    }
    printf("\n");
    podx3_close(dev);
    return r ? 1 : 0;
}
