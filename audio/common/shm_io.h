// Ring access on podx3_shm, shared by the helper, the plug-in and the tests.
// Frames are addressed by absolute USB frame number F; slot = F & (ring - 1).
#pragma once

#include <stdint.h>
#include <string.h>

#include "shm.h"

#define PODX3_RING_MASK ((uint64_t)PODX3_SHM_RING_FRAMES - 1)
// Frames just behind the writer's wrap point are not trusted: the helper may be
// overwriting them while the plug-in copies (a copy takes microseconds; 512
// frames are 10 ms).
#define PODX3_IN_READ_MARGIN 512

// Helper: store n interleaved 8-ch frames at F. Caller publishes in_write_frame.
static inline void podx3_in_put(podx3_shm *s, uint64_t f, const float *src, uint32_t n) {
    while (n) {
        uint64_t slot = f & PODX3_RING_MASK;
        uint32_t run = (uint32_t)(PODX3_SHM_RING_FRAMES - slot);
        if (run > n) run = n;
        memcpy(&s->in_ring[slot * PODX3_SHM_IN_CH], src, (size_t)run * PODX3_SHM_IN_CH * sizeof(float));
        src += run * PODX3_SHM_IN_CH, f += run, n -= run;
    }
}

// Plug-in: copy frames [f, f+n) into dst, zero-filling whatever is not
// (or no longer) in the ring. w = in_write_frame as loaded (acquire) before the
// copy. Returns the number of frames that came from the ring.
static inline uint32_t podx3_in_get(const podx3_shm *s, uint64_t f, uint32_t n, uint64_t w, float *dst) {
    uint64_t lo = w > PODX3_SHM_RING_FRAMES - PODX3_IN_READ_MARGIN ? w - (PODX3_SHM_RING_FRAMES - PODX3_IN_READ_MARGIN) : 0;
    uint32_t got = 0;
    for (uint32_t i = 0; i < n;) {
        uint64_t fr = f + i;
        uint32_t run;
        if (fr < lo || fr >= w) {
            // Missing: zero up to the next frame that is available (or the end).
            uint64_t next = fr < lo ? lo : f + n;
            run = (uint32_t)((next > f + n ? f + n : next) - fr);
            memset(dst + (size_t)i * PODX3_SHM_IN_CH, 0, (size_t)run * PODX3_SHM_IN_CH * sizeof(float));
        } else {
            uint64_t slot = fr & PODX3_RING_MASK;
            uint64_t end = w < f + n ? w : f + n;
            run = (uint32_t)(end - fr);
            if (run > PODX3_SHM_RING_FRAMES - slot) run = (uint32_t)(PODX3_SHM_RING_FRAMES - slot);
            memcpy(dst + (size_t)i * PODX3_SHM_IN_CH, &s->in_ring[slot * PODX3_SHM_IN_CH],
                   (size_t)run * PODX3_SHM_IN_CH * sizeof(float));
            got += run;
        }
        i += run;
    }
    return got;
}

// Plug-in: store n interleaved stereo frames at F, skipping frames the helper
// has already taken (r = out_read_frame) - writing them would only be replayed
// one ring length later. Also refuses frames more than a ring ahead of r.
// Returns the number of frames dropped as late.
static inline uint32_t podx3_out_put(podx3_shm *s, uint64_t f, const float *src, uint32_t n, uint64_t r) {
    uint32_t late = 0;
    if (f < r) {
        late = (uint32_t)(r - f < n ? r - f : n);
        f += late, src += (size_t)late * PODX3_SHM_OUT_CH, n -= late;
    }
    if (f + n > r + PODX3_SHM_RING_FRAMES) {
        uint64_t keep = f >= r + PODX3_SHM_RING_FRAMES ? 0 : r + PODX3_SHM_RING_FRAMES - f;
        n = (uint32_t)keep; // far future: the clock is off, nothing sensible to do
    }
    while (n) {
        uint64_t slot = f & PODX3_RING_MASK;
        uint32_t run = (uint32_t)(PODX3_SHM_RING_FRAMES - slot);
        if (run > n) run = n;
        memcpy(&s->out_ring[slot * PODX3_SHM_OUT_CH], src, (size_t)run * PODX3_SHM_OUT_CH * sizeof(float));
        src += run * PODX3_SHM_OUT_CH, f += run, n -= run;
    }
    return late;
}

#define PODX3_OUT_SCRUB_LAG 256

// Helper: take n stereo frames at F into dst and clear them, so audio the
// plug-in stops writing is never played twice. Also clears the slots of
// [F - lag, F - lag + n): a WriteMix racing with the take can land a few frames
// behind the read position, and those would otherwise replay a ring later.
static inline void podx3_out_take(podx3_shm *s, uint64_t f, uint32_t n, float *dst) {
    for (uint64_t g = f - PODX3_OUT_SCRUB_LAG, e = g + n; f >= PODX3_OUT_SCRUB_LAG && g < e; g++)
        s->out_ring[(g & PODX3_RING_MASK) * PODX3_SHM_OUT_CH] = s->out_ring[(g & PODX3_RING_MASK) * PODX3_SHM_OUT_CH + 1] = 0;
    while (n) {
        uint64_t slot = f & PODX3_RING_MASK;
        uint32_t run = (uint32_t)(PODX3_SHM_RING_FRAMES - slot);
        if (run > n) run = n;
        float *p = &s->out_ring[slot * PODX3_SHM_OUT_CH];
        memcpy(dst, p, (size_t)run * PODX3_SHM_OUT_CH * sizeof(float));
        memset(p, 0, (size_t)run * PODX3_SHM_OUT_CH * sizeof(float));
        dst += run * PODX3_SHM_OUT_CH, f += run, n -= run;
    }
}
