// Shared memory between the USB helper (podx3d) and the HAL plug-in.
//
// The helper owns the region (created with mach_vm_allocate and handed out as an
// xpc_shmem object). Both rings are indexed by absolute USB frame number F
// (frames since the helper's USB stream started), so no read/write pointers
// are exchanged - only "how far has the producer got".
//
//   in_ring:  helper writes frame F after the IN packet carrying it completes,
//             then publishes in_write_frame = F + 1.
//   out_ring: plug-in writes frame F (WriteMix); the helper reads frame F when
//             it fills the OUT packet that is sent at roughly the time of F,
//             then clears it so stale audio is never replayed.
//
// The clock (anchor_frame, anchor_host, host_ticks_per_frame) is the helper's
// DLL-smoothed mapping from USB frame number to mach_absolute_time. The POD is
// the clock master; the plug-in derives its zero timestamps from this.
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#define PODX3_SHM_MAGIC 0x50583341u // 'PX3A'
#define PODX3_SHM_VERSION 1
#define PODX3_SHM_RING_FRAMES 8192 // power of two, ~170 ms
#define PODX3_SHM_IN_CH 8
#define PODX3_SHM_OUT_CH 2
#define PODX3_MACH_SERVICE "com.pkalamarski.podx3d"
#define PODX3_SHM_PAGE 16384 // arm64 page; also a multiple of the 4 KiB x86 page

typedef struct {
    uint32_t magic, version;
    uint32_t ring_frames, in_channels, out_channels, sample_rate;

    // Incremented every time the USB stream (re)starts; frame numbers restart at 0.
    _Atomic uint32_t generation;
    _Atomic uint32_t streaming; // 1 while the USB stream runs

    // Clock, guarded by a seqlock (odd = being written).
    _Atomic uint32_t clock_seq;
    uint64_t anchor_frame;
    uint64_t anchor_host;
    double host_ticks_per_frame;

    _Atomic uint64_t in_write_frame;  // frames [0, in_write_frame) are in in_ring
    _Atomic uint64_t out_read_frame;  // next frame the helper will take from out_ring
    _Atomic uint64_t heartbeat_host;  // last helper update (mach_absolute_time)

    // Plug-in -> helper diagnostics
    _Atomic uint32_t plugin_io_running;
    _Atomic uint64_t plugin_in_underruns, plugin_out_late;

    float in_ring[PODX3_SHM_RING_FRAMES * PODX3_SHM_IN_CH];
    float out_ring[PODX3_SHM_RING_FRAMES * PODX3_SHM_OUT_CH];
} podx3_shm;

typedef struct {
    uint64_t anchor_frame, anchor_host;
    double host_ticks_per_frame;
} podx3_clock;

static inline void podx3_clock_publish(podx3_shm *s, const podx3_clock *c) {
    uint32_t q = atomic_load_explicit(&s->clock_seq, memory_order_relaxed);
    atomic_store_explicit(&s->clock_seq, q + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    s->anchor_frame = c->anchor_frame;
    s->anchor_host = c->anchor_host;
    s->host_ticks_per_frame = c->host_ticks_per_frame;
    atomic_store_explicit(&s->clock_seq, q + 2, memory_order_release);
}

// Returns 0 on success (consistent snapshot).
static inline int podx3_clock_read(podx3_shm *s, podx3_clock *c) {
    for (int tries = 0; tries < 100; tries++) {
        uint32_t q1 = atomic_load_explicit(&s->clock_seq, memory_order_acquire);
        if (q1 & 1) continue;
        c->anchor_frame = s->anchor_frame;
        c->anchor_host = s->anchor_host;
        c->host_ticks_per_frame = s->host_ticks_per_frame;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->clock_seq, memory_order_relaxed) == q1) return q1 ? 0 : -1;
    }
    return -1;
}

// Mapping length both sides agree on (mach_vm_allocate works in whole pages).
static inline uint64_t podx3_shm_size(void) {
    return ((uint64_t)sizeof(podx3_shm) + PODX3_SHM_PAGE - 1) & ~(uint64_t)(PODX3_SHM_PAGE - 1);
}
