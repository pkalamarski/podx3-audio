// Unit tests for the hardware-independent pieces shared by podx3d and the
// plug-in: DLL, ring index math, plug-in timeline re-basing, RT thread lookup.
#include "../common/dll.h"
#include "../common/pclock.h"
#include "../common/shm_io.h"
#include "../helper/rt.h"

#include <mach/mach_vm.h>
#include <pthread.h>
#include <xpc/xpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

// Deterministic PRNG (xorshift64) so results are reproducible run to run.
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}
static double urand(void) { return (double)rnd() / 4294967296.0; }

// ---------------------------------------------------------------------------
// DLL: 5/6/7-frame packets every 125 us of device time, 8 per transfer;
// completion = end of the last packet + 3 ms +- 1 ms (+2.3 ms spikes 1 % of the time).

static void test_dll(double ppm) {
    const double tpf_nom = 500.0, ms = tpf_nom * 48;
    const double tpf_true = tpf_nom * (1 + ppm * 1e-6);
    const double h0 = 1e12, delay = 3 * ms;
    podx3_dll d;
    podx3_dll_init(&d, tpf_nom);
    double max_res = 0, sum_res = 0, sum2 = 0, max_ppm_late = 0;
    long nres = 0;
    const long pkts = 8000L * 30; // 30 s
    for (long i = 0; i < pkts; i++) {
        int dj = rnd() & 1;
        int frames_cum_dev = (int)(6 * (i + 1)) + dj; // sizes 5/6/7, mean exactly 6
        if ((i & 7) != 7) continue;
        double t_end = h0 + 6.0 * (i + 1) * tpf_true;
        double jitter = (urand() * 2 - 1) * ms + (urand() < 0.01 ? 2.3 * ms : 0);
        podx3_dll_update(&d, (uint64_t)frames_cum_dev, (uint64_t)(t_end + delay + jitter));
        double t_s = 6.0 * (i + 1) / 48000;
        if (t_s > 5) {
            podx3_clock c;
            podx3_dll_get(&d, &c);
            // Residual of the published line against the ideal line (true rate + mean delay).
            double ideal = h0 + delay + (double)c.anchor_frame * tpf_true;
            double res = ((double)c.anchor_host - ideal) / ms * 1000; // us
            if (fabs(res) > max_res) max_res = fabs(res);
            sum_res += res, sum2 += res * res, nres++;
            double perr = (d.tpf / tpf_true - 1) * 1e6;
            if (fabs(perr) > max_ppm_late) max_ppm_late = fabs(perr);
        }
    }
    double mean = sum_res / nres, sd = sqrt(sum2 / nres - mean * mean);
    double final_ppm = (d.tpf / tpf_true - 1) * 1e6;
    printf("dll %+5.0f ppm: tpf err final %+.2f ppm (max after 5 s %.2f), anchor vs ideal: mean %+.1f us, "
           "sd %.1f us, max %.1f us, clamped %llu, resets %llu\n",
           ppm, final_ppm, max_ppm_late, mean, sd, max_res, (unsigned long long)d.clamped,
           (unsigned long long)d.resets);
    CHECK(max_ppm_late < 50, "tpf error up to %.2f ppm", max_ppm_late);
    CHECK(max_res < 300, "anchor residual %.1f us", max_res);
    CHECK(d.resets == 0, "dll reset");
}

// Lock from a wrong start (stream begins mid-stall) and survive a 50+ ms gap.
static void test_dll_reset(void) {
    podx3_dll d;
    podx3_dll_init(&d, 500);
    uint64_t f = 0, h = 1000000;
    for (int i = 0; i < 2000; i++) {
        f += 48, h += 24000;
        if (i == 1000) h += 24000 * 100; // 100 ms hole (machine was busy)
        podx3_dll_update(&d, f, h);
    }
    CHECK(d.resets == 1, "expected one reset, got %llu", (unsigned long long)d.resets);
    CHECK(fabs(d.tpf - 500) < 0.01, "tpf after reset %.4f", d.tpf);
}

// ---------------------------------------------------------------------------
// Ring math across the slot wrap and near 2^32 frame numbers.

static float in_val(uint64_t f, int c) { return (float)(f & 0xffff) + c * 0.125f; }

static void test_ring(void) {
    static podx3_shm s;
    memset(&s, 0, sizeof s);
    uint64_t f0 = (1ull << 32) - 20000, w = f0;
    float buf[8 * 8];
    float dst[8 * 600];
    int bad = 0;
    for (int it = 0; it < 20000; it++) {
        uint32_t n = 5 + (rnd() % 3);
        for (uint32_t i = 0; i < n; i++)
            for (int c = 0; c < 8; c++) buf[i * 8 + c] = in_val(w + i, c);
        podx3_in_put(&s, w, buf, n);
        w += n;
        if (w - f0 < PODX3_SHM_RING_FRAMES) continue; // ring not filled yet
        // Random read window from well behind the valid range to past the writer.
        uint64_t f = w - (rnd() % 9000) + 50;
        uint32_t len = 1 + rnd() % 512;
        uint32_t got = podx3_in_get(&s, f, len, w, dst);
        uint64_t lo = w - (PODX3_SHM_RING_FRAMES - PODX3_IN_READ_MARGIN);
        uint32_t expect = 0;
        for (uint32_t i = 0; i < len; i++) {
            uint64_t fr = f + i;
            int valid = fr >= lo && fr < w;
            expect += valid;
            for (int c = 0; c < 8; c++) {
                float want = valid ? in_val(fr, c) : 0;
                if (dst[i * 8 + c] != want) bad++;
            }
        }
        if (got != expect) bad++;
    }
    CHECK(bad == 0, "in ring: %d mismatches", bad);

    // Output: write ahead of the reader, take, check clearing and late handling.
    uint64_t r = (1ull << 32) - 100;
    float ob[2 * 300], tk[2 * 300];
    bad = 0;
    for (int it = 0; it < 5000; it++) {
        uint32_t n = 256;
        uint64_t f = r + 300; // plug-in writes ~300 frames ahead
        for (uint32_t i = 0; i < n; i++) ob[i * 2] = (float)((f + i) & 0xffff), ob[i * 2 + 1] = -ob[i * 2];
        uint32_t late = podx3_out_put(&s, f, ob, n, r);
        if (late) bad++;
        // Helper consumes 5..7 frames at a time up to f.
        while (r + 7 < f) {
            uint32_t m = 5 + rnd() % 3;
            podx3_out_take(&s, r, m, tk);
            for (uint32_t i = 0; i < m; i++) {
                uint64_t fr = r + i;
                // Frames from earlier iterations that were written: anything in [old f, old f+n).
                float v = tk[i * 2];
                if (v != 0 && v != (float)(fr & 0xffff)) bad++;
            }
            r += m;
        }
    }
    CHECK(bad == 0, "out ring: %d mismatches", bad);
    // Late write: frames before r are dropped, the rest stored.
    for (int i = 0; i < 20; i++) ob[i * 2] = ob[i * 2 + 1] = 1;
    uint32_t late = podx3_out_put(&s, r - 5, ob, 20, r);
    CHECK(late == 5, "late %u", late);
    podx3_out_take(&s, r, 15, tk);
    int ones = 0;
    for (int i = 0; i < 30; i++) ones += tk[i] == 1;
    CHECK(ones == 30, "late write kept %d/30 samples", ones);
    podx3_out_take(&s, r, 15, tk);
    CHECK(tk[0] == 0 && tk[29] == 0, "take did not clear");
    printf("ring: in/out index math OK across slot wrap and 2^32\n");
}

// ---------------------------------------------------------------------------
// Plug-in timeline across generation changes, fallback and helper restarts.

static void test_pclock(void) {
    const double ms = 24000;
    const uint64_t h_start = 1000000000ull;
    int region_a, region_b; // stand-ins for two shm mappings
    podx3_clock_src s = {.valid = 1, .src = &region_a, .gen = 1,
                         .clk = {.anchor_frame = 0, .anchor_host = h_start - 24000000,
                                 .host_ticks_per_frame = 500 * (1 + 50e-6)}};
    podx3_pclock p;
    podx3_pclock_start(&p, 500, 4096, &s, h_start);
    CHECK(p.mode == PODX3_PCLOCK_USB, "should start on the USB clock");
    double prev_s = podx3_pclock_sample_at(&p, (double)h_start), zt_s_prev = 0;
    uint64_t zt_h_prev = 0;
    double max_step = 0, min_step = 1e9;
    int zt_bad = 0, nzt = 0;
    for (int t = 1; t <= 12000; t++) { // 12 s in 1 ms steps
        uint64_t now = h_start + (uint64_t)(t * ms);
        if (t == 3000) s.valid = 0; // helper lost the device
        if (t == 4000) { // new generation: frames restart at 0, slightly different rate
            s = (podx3_clock_src){.valid = 1, .src = &region_a, .gen = 2,
                                  .clk = {.anchor_frame = 0, .anchor_host = now - 12000000,
                                          .host_ticks_per_frame = 500 * (1 - 80e-6)}};
        }
        if (t == 7000) { // helper restarted: new region, generation 1 again
            s = (podx3_clock_src){.valid = 1, .src = &region_b, .gen = 1,
                                  .clk = {.anchor_frame = 96000, .anchor_host = now - 3000,
                                          .host_ticks_per_frame = 500 * (1 + 20e-6)}};
        }
        if (t == 9000) s.gen = 2; // generation change without the stream ever being invalid
        if (t > 4000 && t % 10 == 0) s.clk.anchor_host += 3; // DLL wiggle
        podx3_pclock_update(&p, &s, now);
        double cur = podx3_pclock_sample_at(&p, (double)now);
        double step = cur - prev_s;
        if (step > max_step) max_step = step;
        if (step < min_step) min_step = step;
        prev_s = cur;
        double zs;
        uint64_t zh;
        podx3_pclock_zero_ts(&p, now, &zs, &zh);
        if (zs != zt_s_prev || zh != zt_h_prev) {
            nzt++;
            if (zs < zt_s_prev || zh <= zt_h_prev || zh > now || fmod(zs, 4096) != 0) zt_bad++;
            if (zt_h_prev && zs == zt_s_prev + 4096) {
                double tpf = (double)(zh - zt_h_prev) / 4096;
                if (fabs(tpf / 500 - 1) > 0.002) zt_bad++;
            }
            zt_s_prev = zs, zt_h_prev = zh;
        }
    }
    printf("pclock: %llu re-anchors, sample step per 1 ms min %.3f max %.3f frames, %d zero timestamps, "
           "%d bad\n", (unsigned long long)p.rebases, min_step, max_step, nzt, zt_bad);
    CHECK(p.rebases == 4, "rebases %llu", (unsigned long long)p.rebases);
    CHECK(min_step > 47 && max_step < 49.5, "timeline not continuous (step %.3f..%.3f)", min_step, max_step);
    CHECK(zt_bad == 0, "%d bad zero timestamps", zt_bad);
    CHECK(nzt >= 138, "only %d zero timestamps", nzt);
    // Frame mapping in the final generation: sample t is USB frame t + base.
    double h = (double)(h_start + 12000 * 24000);
    double f_line = (double)p.clk.anchor_frame + (h - (double)p.clk.anchor_host) / p.clk.host_ticks_per_frame;
    double t_now = podx3_pclock_sample_at(&p, h);
    CHECK(fabs((double)podx3_pclock_frame(&p, floor(t_now)) - floor(f_line)) <= 1, "frame mapping off");
}

// ---------------------------------------------------------------------------

static atomic_int rt_stop;
static void *named_thread(void *arg) {
    (void)arg;
    pthread_setname_np("org.libusb.device-hotplug");
    while (!atomic_load(&rt_stop)) usleep(1000);
    return NULL;
}

static void test_rt(void) {
    pthread_t th;
    pthread_create(&th, NULL, named_thread, NULL);
    usleep(20000);
    int n = podx3_rt_named_threads("org.libusb.device-hotplug");
    int none = podx3_rt_named_threads("no-such-thread");
    atomic_store(&rt_stop, 1);
    pthread_join(th, NULL);
    printf("rt: named thread found %d, unnamed %d\n", n, none);
    CHECK(n == 1, "expected exactly one named thread, got %d", n);
    CHECK(none == 0, "matched a thread that does not exist");
}

// The helper hands out the region as xpc_shmem and the plug-in maps it with
// xpc_shmem_map; both sides must see each other's writes (a read-only client
// mapping would crash the plug-in host on its first WriteMix).
static void test_shmem(void) {
    mach_vm_address_t addr = 0;
    uint64_t len = podx3_shm_size();
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, len, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS, "mach_vm_allocate %d", kr);
    podx3_shm *a = (podx3_shm *)(uintptr_t)addr;
    a->magic = PODX3_SHM_MAGIC;
    xpc_object_t mem = xpc_shmem_create(a, (size_t)len);
    void *p = NULL;
    size_t mlen = mem ? xpc_shmem_map(mem, &p) : 0;
    CHECK(mlen == len && p && p != a, "xpc_shmem_map: len %zu (want %llu)", mlen, (unsigned long long)len);
    if (!p) return;
    podx3_shm *b = p;
    CHECK(b->magic == PODX3_SHM_MAGIC, "mapping does not show the helper's data");
    b->out_ring[PODX3_SHM_RING_FRAMES * 2 - 1] = 0.5f; // last float of the struct
    atomic_store(&b->plugin_io_running, 7);
    a->in_ring[3] = 0.25f;
    CHECK(a->out_ring[PODX3_SHM_RING_FRAMES * 2 - 1] == 0.5f && atomic_load(&a->plugin_io_running) == 7,
          "client writes not visible to the owner");
    CHECK(b->in_ring[3] == 0.25f, "owner writes not visible to the client");
    printf("shmem: xpc_shmem round trip OK (%zu bytes, sizeof %zu)\n", mlen, sizeof(podx3_shm));
    xpc_release(mem);
}

int main(void) {
    test_dll(0);
    test_dll(100);
    test_dll(-100);
    test_dll_reset();
    test_ring();
    test_pclock();
    test_rt();
    test_shmem();
    printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
    return failures != 0;
}
