// The plug-in's sample timeline (plug-in side, IO thread only).
//
// The HAL drops a device whose sample time jumps backwards, but the helper's
// USB frame numbers restart at 0 whenever the stream restarts, and the helper
// may vanish altogether. So the plug-in owns its timeline and maps it onto
// whatever clock is available:
//   USB mode:      plugin_sample = usb_frame - base  (usb_frame from the shm DLL line)
//   fallback mode: plugin_sample = fb_s0 + (host - fb_h0) / nominal ticks per frame
// Every switch (generation change, helper restart, helper loss) re-anchors the
// new mapping so the sample time at "now" is unchanged (or moves < 1 frame
// forward), keeping it continuous and monotonic.
#pragma once

#include <math.h>
#include <stdint.h>

#include "shm.h"

enum { PODX3_PCLOCK_FALLBACK = 1, PODX3_PCLOCK_USB = 2 };

// One snapshot of what the shm offers right now.
typedef struct {
    int valid;       // streaming, fresh heartbeat, consistent clock
    const void *src; // identity of the mapping (a restarted helper maps a new region)
    uint32_t gen;
    podx3_clock clk;
} podx3_clock_src;

typedef struct {
    double nominal_tpf;
    uint32_t period; // zero time stamp period in frames
    int mode;
    // USB mode (clk is a private copy so a vanished generation can still be extrapolated)
    const void *src;
    uint32_t gen;
    podx3_clock clk;
    int64_t base;
    // fallback mode
    double fb_s0, fb_h0;
    // last zero time stamp handed to the HAL
    int64_t zt_k;
    uint64_t zt_host;
    uint64_t rebases;
} podx3_pclock;

static inline double podx3_pclock_sample_at(const podx3_pclock *p, double host) {
    if (p->mode == PODX3_PCLOCK_USB)
        return (double)p->clk.anchor_frame + (host - (double)p->clk.anchor_host) / p->clk.host_ticks_per_frame -
               (double)p->base;
    return p->fb_s0 + (host - p->fb_h0) / p->nominal_tpf;
}

static inline double podx3_pclock_host_at(const podx3_pclock *p, double sample) {
    if (p->mode == PODX3_PCLOCK_USB)
        return (double)p->clk.anchor_host +
               (sample + (double)p->base - (double)p->clk.anchor_frame) * p->clk.host_ticks_per_frame;
    return p->fb_h0 + (sample - p->fb_s0) * p->nominal_tpf;
}

static inline void podx3_pclock_switch(podx3_pclock *p, const podx3_clock_src *s, uint64_t now) {
    double h = (double)now;
    double cur = podx3_pclock_sample_at(p, h);
    if (s->valid) {
        double f = (double)s->clk.anchor_frame + (h - (double)s->clk.anchor_host) / s->clk.host_ticks_per_frame;
        // floor: the new sample time at "now" is >= the old one (never backwards).
        p->base = (int64_t)floor(f - cur);
        p->clk = s->clk;
        p->src = s->src;
        p->gen = s->gen;
        p->mode = PODX3_PCLOCK_USB;
    } else {
        p->fb_s0 = cur;
        p->fb_h0 = h;
        p->mode = PODX3_PCLOCK_FALLBACK;
    }
    p->rebases++;
}

// Follow the shm: refresh the line within a generation, re-anchor on any change.
// Returns 1 if the mapping was re-anchored.
static inline int podx3_pclock_update(podx3_pclock *p, const podx3_clock_src *s, uint64_t now) {
    if (s->valid) {
        if (p->mode == PODX3_PCLOCK_USB && s->src == p->src && s->gen == p->gen) {
            p->clk = s->clk;
            return 0;
        }
        podx3_pclock_switch(p, s, now);
        return 1;
    }
    if (p->mode == PODX3_PCLOCK_USB) {
        podx3_pclock_switch(p, s, now);
        return 1;
    }
    return 0;
}

// StartIO: sample time 0 is "now".
static inline void podx3_pclock_start(podx3_pclock *p, double nominal_tpf, uint32_t period,
                                      const podx3_clock_src *s, uint64_t now) {
    *p = (podx3_pclock){.nominal_tpf = nominal_tpf, .period = period, .mode = PODX3_PCLOCK_FALLBACK,
                        .fb_h0 = (double)now};
    podx3_pclock_update(p, s, now);
    p->rebases = 0;
    double h0 = podx3_pclock_host_at(p, 0);
    p->zt_k = 0;
    p->zt_host = h0 > (double)now ? now : (uint64_t)h0;
}

// GetZeroTimeStamp: the latest period boundary that has passed. A boundary's
// host time is computed once and cached, so DLL corrections show up only in
// later periods instead of making an already reported timestamp wobble.
static inline void podx3_pclock_zero_ts(podx3_pclock *p, uint64_t now, double *sample, uint64_t *host) {
    int64_t k = (int64_t)floor(podx3_pclock_sample_at(p, (double)now) / p->period);
    if (k > p->zt_k) {
        double h = podx3_pclock_host_at(p, (double)k * p->period);
        uint64_t hu = h > (double)now ? now : (uint64_t)h;
        if (hu <= p->zt_host) hu = p->zt_host + 1;
        p->zt_k = k;
        p->zt_host = hu;
    }
    *sample = (double)p->zt_k * p->period;
    *host = p->zt_host;
}

// USB frame number of plug-in sample t (only meaningful in USB mode).
static inline int64_t podx3_pclock_frame(const podx3_pclock *p, double t) {
    return (int64_t)llround(t) + p->base;
}
