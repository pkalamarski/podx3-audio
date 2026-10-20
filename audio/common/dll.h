// Delay-locked loop mapping USB frame count -> mach_absolute_time (helper side).
//
// Fed once per completed IN transfer with (frames received so far, completion
// time). Completion times jitter by ~1 ms (and occasionally stall longer), so
// the raw pairs are useless as a clock; a 2nd-order loop (Adriaensen, "Using a
// DLL to filter time") turns them into a smooth line. The line maps frames to
// *completion* time, i.e. it lags the device by the USB scheduling delay - a
// constant the safety offsets absorb, not something to correct here.
#pragma once

#include <math.h>
#include <stdint.h>

#include "shm.h"

#ifndef PODX3_DLL_BW_FAST
#define PODX3_DLL_BW_FAST 4.0 // Hz, while locking
#endif
#ifndef PODX3_DLL_BW_SLOW
#define PODX3_DLL_BW_SLOW 0.1 // Hz, settled: rate noise ~20 ppm vs ~200 at 0.5 Hz (tests/test_logic.c)
#endif
#define PODX3_DLL_MAX_PPM 1000.0 // crystals are far closer than this

typedef struct {
    double nominal_tpf;  // host ticks per frame at exactly 48 kHz
    double tpf;          // current estimate
    double anchor_host;  // smoothed host time of anchor_frame (double: sub-tick)
    uint64_t anchor_frame;
    uint64_t frames_seen; // since init, to narrow the bandwidth
    uint64_t updates, clamped, resets;
    int locked; // has at least one point
} podx3_dll;

static inline void podx3_dll_init(podx3_dll *d, double nominal_tpf) {
    *d = (podx3_dll){.nominal_tpf = nominal_tpf, .tpf = nominal_tpf};
}

// frames: total frames received after this transfer; host: its completion time.
static inline void podx3_dll_update(podx3_dll *d, uint64_t frames, uint64_t host) {
    if (!d->locked || frames <= d->anchor_frame) {
        if (d->locked && frames < d->anchor_frame) d->resets++;
        if (!d->locked || frames < d->anchor_frame) {
            d->anchor_frame = frames;
            d->anchor_host = (double)host;
            d->tpf = d->nominal_tpf;
            d->frames_seen = 0;
            d->locked = 1;
        }
        return; // same frame count again (empty transfer): nothing to learn
    }
    double n = (double)(frames - d->anchor_frame);
    double pred = d->anchor_host + n * d->tpf;
    double e = (double)host - pred;
    double ms = d->nominal_tpf * 48.0; // ticks per ms
    if (fabs(e) > 50 * ms) {
        // Lost lock (machine sleep, huge stall, frame count discontinuity): restart.
        d->resets++;
        d->locked = 0;
        podx3_dll_update(d, frames, host);
        return;
    }
    // One late completion (4-6 ms stalls happen without RT threads) must not
    // yank the clock; 2 ms covers normal jitter.
    if (e > 2 * ms) e = 2 * ms, d->clamped++;
    if (e < -2 * ms) e = -2 * ms, d->clamped++;
    // Narrow the loop gradually (halving every 0.5 s after the first 0.5 s): a
    // hard switch leaves the fast loop's rate error to decay at the slow rate.
    double secs = (double)d->frames_seen / 48000.0;
    double bw = secs < 0.5 ? PODX3_DLL_BW_FAST : PODX3_DLL_BW_FAST * exp2(-(secs - 0.5) / 0.5);
    if (bw < PODX3_DLL_BW_SLOW) bw = PODX3_DLL_BW_SLOW;
    double w = 2 * M_PI * bw * n / 48000.0; // per-update loop gain scales with the interval
    double b = sqrt(2) * w, c = w * w;
    d->anchor_host = pred + b * e;
    d->anchor_frame = frames;
    d->tpf += c * e / n;
    double lim = d->nominal_tpf * PODX3_DLL_MAX_PPM * 1e-6;
    if (d->tpf > d->nominal_tpf + lim) d->tpf = d->nominal_tpf + lim;
    if (d->tpf < d->nominal_tpf - lim) d->tpf = d->nominal_tpf - lim;
    d->frames_seen += (uint64_t)n;
    d->updates++;
}

static inline void podx3_dll_get(const podx3_dll *d, podx3_clock *c) {
    c->anchor_frame = d->anchor_frame;
    c->anchor_host = (uint64_t)llround(d->anchor_host); // <= 0.5 tick (~20 ns) rounding
    c->host_ticks_per_frame = d->tpf;
}
