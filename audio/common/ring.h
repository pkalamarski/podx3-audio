// Single-producer single-consumer lock-free ring of interleaved float frames.
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float *buf;
    uint32_t channels;
    uint32_t capacity; // frames, power of two
    _Atomic uint64_t wr, rd;
} ring_t;

static inline int ring_init(ring_t *r, uint32_t channels, uint32_t capacity_pow2) {
    r->channels = channels;
    r->capacity = capacity_pow2;
    r->buf = calloc((size_t)capacity_pow2 * channels, sizeof(float));
    atomic_init(&r->wr, 0);
    atomic_init(&r->rd, 0);
    return r->buf ? 0 : -1;
}

static inline uint32_t ring_avail(ring_t *r) {
    return (uint32_t)(atomic_load_explicit(&r->wr, memory_order_acquire) -
                      atomic_load_explicit(&r->rd, memory_order_relaxed));
}

// Producer: returns frames written (drops the excess when full).
static inline uint32_t ring_write(ring_t *r, const float *src, uint32_t frames) {
    uint64_t wr = atomic_load_explicit(&r->wr, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&r->rd, memory_order_acquire);
    uint32_t space = r->capacity - (uint32_t)(wr - rd);
    if (frames > space) frames = space;
    for (uint32_t i = 0; i < frames; i++)
        memcpy(&r->buf[((wr + i) & (r->capacity - 1)) * r->channels], &src[i * r->channels],
               r->channels * sizeof(float));
    atomic_store_explicit(&r->wr, wr + frames, memory_order_release);
    return frames;
}

// Consumer: pops one frame into dst; returns 0 if empty.
static inline int ring_pop(ring_t *r, float *dst) {
    uint64_t rd = atomic_load_explicit(&r->rd, memory_order_relaxed);
    if (atomic_load_explicit(&r->wr, memory_order_acquire) == rd) return 0;
    memcpy(dst, &r->buf[(rd & (r->capacity - 1)) * r->channels], r->channels * sizeof(float));
    atomic_store_explicit(&r->rd, rd + 1, memory_order_release);
    return 1;
}

// Consumer: discards up to `frames` frames.
static inline void ring_skip(ring_t *r, uint32_t frames) {
    uint32_t avail = ring_avail(r);
    if (frames > avail) frames = avail;
    atomic_fetch_add_explicit(&r->rd, frames, memory_order_release);
}
