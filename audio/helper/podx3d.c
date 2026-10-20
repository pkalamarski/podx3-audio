// podx3d: USB helper for the PodX3Audio HAL plug-in.
//   podx3d [--foreground]
// Streams the POD X3 over libusb and exposes it through a shared memory region
// (common/shm.h) that the plug-in maps via the Mach service
// com.pkalamarski.podx3d. Normally runs as a LaunchDaemon (the sandboxed
// plug-in host can only look up system-domain services); --foreground logs to
// stderr as well, for debugging (the XPC listener only works under launchd).
#include "../common/dll.h"
#include "../common/shm.h"
#include "../common/shm_io.h"
#include "../common/tap.h"
#include "../usb/podx3_usb.h"
#include "rt.h"

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>

#define LIBUSB_THREAD_NAME "org.libusb.device-hotplug"
#define STALL_MS 500        // no IN frames for this long while streaming -> device lost
#define FIRST_FRAMES_MS 2000 // IN must start flowing within this after stream start
#define ERRORS_PER_S 50     // transfer errors per second that mean the stream is dead
#define SETTLE_FRAMES 12000 // DLL runs 250 ms before the plug-in may use the clock
#define OUT_RESYNC 96       // OUT cursor drift from the IN position that forces a resync

static os_log_t g_log;
static bool g_fg;
static atomic_bool g_stop;
static podx3_shm *g_shm;
static mach_vm_size_t g_shm_len;
static xpc_object_t g_shmem;
static double g_tpf_nominal;

static void logmsg(os_log_type_t type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logmsg(os_log_type_t type, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    os_log_with_type(g_log, type, "%{public}s", buf);
    if (g_fg) {
        fprintf(stdout, "%s\n", buf);
        fflush(stdout);
    }
}

// Under launchd, route the USB library's stderr messages into os_log, folding
// repeats (a busy device fails to open once per second).
static void *stderr_forwarder(void *arg) {
    FILE *f = fdopen((int)(intptr_t)arg, "r");
    char line[512], last[512] = "";
    unsigned repeats = 0;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strcmp(line, last)) {
            repeats++;
            continue;
        }
        if (repeats) os_log(g_log, "usb: (previous message repeated %u times)", repeats);
        repeats = 0;
        os_log(g_log, "usb: %{public}s", line);
        snprintf(last, sizeof last, "%s", line);
    }
    return NULL;
}

static void redirect_stderr(void) {
    int fds[2];
    if (pipe(fds)) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t th;
    pthread_create(&th, NULL, stderr_forwarder, (void *)(intptr_t)fds[0]);
    pthread_detach(th);
}

// ---------------------------------------------------------------------------
// Stream state; everything here runs on the USB thread (libusb callbacks are
// dispatched from podx3_poll on that thread).

typedef struct {
    uint64_t in_frames; // == in_write_frame
    podx3_dll dll;
    bool out_synced;
    uint64_t out_cursor;
    uint64_t out_resyncs, in_bad_pkts;
} stream_t;

static tap_t g_tap; // DI channel for the web tuner (control/src/tap.ts)

static void on_in(void *user, const uint8_t *data, int len, int status, uint64_t host, int idx, int n) {
    stream_t *s = user;
    podx3_shm *shm = g_shm;
    float conv[16 * PODX3_SHM_IN_CH];
    if (status != 0) {
        // A lost packet still took 125 us on the device: keep the frame
        // timeline honest with 6 frames of silence (only once audio flows).
        if (s->in_frames) {
            memset(conv, 0, 6 * PODX3_SHM_IN_CH * sizeof(float));
            podx3_in_put(shm, s->in_frames, conv, 6);
            s->in_frames += 6;
        }
        s->in_bad_pkts++;
    } else if (len % PODX3_IN_FRAME_BYTES) {
        s->in_bad_pkts++; // the 3-byte packet at stream start
    } else {
        int frames = len / PODX3_IN_FRAME_BYTES;
        while (frames > 0) {
            int chunk = frames > 16 ? 16 : frames;
            for (int i = 0; i < chunk * PODX3_SHM_IN_CH; i++)
                conv[i] = (float)podx3_s24_get(data + i * PODX3_BYTES_PER_SAMPLE) * (1.0f / 8388608.0f);
            podx3_in_put(shm, s->in_frames, conv, (uint32_t)chunk);
            for (int f = 0; f < chunk; f++) tap_push(&g_tap, conv[f * PODX3_SHM_IN_CH + 6]); // input 7 (DI)
            s->in_frames += (uint64_t)chunk;
            data += chunk * PODX3_IN_FRAME_BYTES;
            frames -= chunk;
        }
    }
    atomic_store_explicit(&shm->in_write_frame, s->in_frames, memory_order_release);
    if (idx != n - 1 || !s->in_frames) return;

    // Once per transfer (1 ms): the clock.
    podx3_dll_update(&s->dll, s->in_frames, host);
    podx3_clock c;
    podx3_dll_get(&s->dll, &c);
    podx3_clock_publish(shm, &c);
    atomic_store_explicit(&shm->heartbeat_host, host, memory_order_release);
    if (!atomic_load(&shm->streaming) && s->dll.frames_seen >= SETTLE_FRAMES) atomic_store(&shm->streaming, 1);
}

static void on_out(void *user, uint8_t *data, int nframes) {
    stream_t *s = user;
    podx3_shm *shm = g_shm;
    if (!s->out_synced) {
        if (!s->in_frames) {
            memset(data, 0, (size_t)nframes * PODX3_OUT_FRAME_BYTES);
            return;
        }
        s->out_cursor = s->in_frames;
        s->out_synced = true;
    }
    // OUT frame counts follow IN frame counts (library), so the cursor should
    // stay near the IN position; if it wanders, the plug-in's frame->time
    // mapping no longer holds for output.
    int64_t d = (int64_t)(s->out_cursor - s->in_frames);
    if (d > OUT_RESYNC || d < -OUT_RESYNC) {
        s->out_cursor = s->in_frames;
        s->out_resyncs++;
    }
    // Publish first: a WriteMix racing with the take then sees the frames as gone.
    atomic_store_explicit(&shm->out_read_frame, s->out_cursor + (uint64_t)nframes, memory_order_release);
    float tmp[64 * PODX3_SHM_OUT_CH];
    while (nframes > 0) {
        int chunk = nframes > 64 ? 64 : nframes;
        podx3_out_take(shm, s->out_cursor, (uint32_t)chunk, tmp);
        for (int i = 0; i < chunk * PODX3_SHM_OUT_CH; i++) {
            float x = tmp[i];
            int32_t v = x >= 1.0f ? 8388607 : x <= -1.0f ? -8388608 : (int32_t)lrintf(x * 8388607.0f);
            podx3_s24_put(data + i * PODX3_BYTES_PER_SAMPLE, v);
        }
        s->out_cursor += (uint64_t)chunk;
        data += chunk * PODX3_OUT_FRAME_BYTES;
        nframes -= chunk;
    }
}

static double ms_since(uint64_t t0) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

static void sleep_unless_stopped(int ms) {
    for (int i = 0; i < ms / 50 && !atomic_load(&g_stop); i++) usleep(50000);
}

// One open -> stream -> (loss) -> close cycle. Returns after the device is
// closed again.
static void run_session(podx3_t *dev) {
    static stream_t s;
    memset(&s, 0, sizeof s);
    podx3_dll_init(&s.dll, g_tpf_nominal);

    // New generation: readers see streaming=0 before the counters restart.
    atomic_store(&g_shm->streaming, 0);
    uint32_t gen = atomic_fetch_add(&g_shm->generation, 1) + 1;
    atomic_store(&g_shm->in_write_frame, 0);
    atomic_store(&g_shm->out_read_frame, 0);
    memset(g_shm->out_ring, 0, sizeof g_shm->out_ring);

    podx3_stream_cfg cfg = {.n_xfers = 16, .n_xfers_out = 6, .pkts_per_xfer = 8, .enable_in = true,
                            .enable_out = true, .in_cb = on_in, .out_cb = on_out, .user = &s};
    if (podx3_stream_start(dev, &cfg)) {
        logmsg(OS_LOG_TYPE_ERROR, "stream start failed");
        return;
    }
    logmsg(OS_LOG_TYPE_DEFAULT, "streaming, generation %u", gen);

    const podx3_stats *st = podx3_get_stats(dev);
    uint64_t t_start = mach_absolute_time(), t_progress = t_start, t_err = t_start, t_report = t_start;
    uint64_t last_frames = 0, err_mark = 0, rep_frames = 0;
    bool announced = false;
    const char *why = NULL;
    while (!atomic_load(&g_stop)) {
        if (podx3_poll(dev, 20) < 0) {
            why = "stream died (device gone or resubmit failed)";
            break;
        }
        if (s.in_frames != last_frames) last_frames = s.in_frames, t_progress = mach_absolute_time();
        if (!s.in_frames && ms_since(t_start) > FIRST_FRAMES_MS) {
            why = "no IN audio after stream start";
            break;
        }
        if (s.in_frames && ms_since(t_progress) > STALL_MS) {
            why = "IN stalled";
            break;
        }
        if (ms_since(t_err) >= 1000) {
            if (st->xfer_errors - err_mark >= ERRORS_PER_S) {
                why = "sustained transfer errors (alt setting reset by another process?)";
                break;
            }
            err_mark = st->xfer_errors, t_err = mach_absolute_time();
        }
        if (!announced && atomic_load(&g_shm->streaming)) {
            announced = true;
            logmsg(OS_LOG_TYPE_DEFAULT, "clock locked, %+.1f ppm vs host", (s.dll.tpf / g_tpf_nominal - 1) * 1e6);
        }
        double rep_ms = ms_since(t_report);
        if (rep_ms >= (g_fg ? 2000 : 60000)) {
            logmsg(OS_LOG_TYPE_DEFAULT,
                   "gen %u: in %.0f frames/s, clock %+.1f ppm (dll resets %llu), xfer errors %llu, bad pkts %llu, "
                   "out resyncs %llu | plug-in io %u, in underruns %llu, out late %llu",
                   gen, (s.in_frames - rep_frames) / (rep_ms / 1000), (s.dll.tpf / g_tpf_nominal - 1) * 1e6,
                   (unsigned long long)s.dll.resets, (unsigned long long)st->xfer_errors,
                   (unsigned long long)s.in_bad_pkts, (unsigned long long)s.out_resyncs,
                   atomic_load(&g_shm->plugin_io_running), (unsigned long long)atomic_load(&g_shm->plugin_in_underruns),
                   (unsigned long long)atomic_load(&g_shm->plugin_out_late));
            rep_frames = s.in_frames, t_report = mach_absolute_time();
        }
    }
    atomic_store(&g_shm->streaming, 0);
    if (why) logmsg(OS_LOG_TYPE_ERROR, "device lost: %s", why);
    else logmsg(OS_LOG_TYPE_DEFAULT, "stopping");
    podx3_stream_stop(dev); // graceful: IN first, then OUT; may time out on a dead device
}

static void *usb_thread(void *arg) {
    (void)arg;
    pthread_setname_np("podx3d.usb");
    if (podx3_make_thread_realtime()) logmsg(OS_LOG_TYPE_ERROR, "could not make the USB thread real-time");
    bool reported_absent = false;
    while (!atomic_load(&g_stop)) {
        podx3_t *dev = NULL;
        if (podx3_open(&dev, false)) {
            if (!reported_absent) logmsg(OS_LOG_TYPE_DEFAULT, "POD X3 not available, retrying every second");
            reported_absent = true;
            sleep_unless_stopped(1000);
            continue;
        }
        reported_absent = false;
        // Without RT, libusb's completion thread stalls 4-6 ms once per second.
        int n = podx3_rt_named_threads(LIBUSB_THREAD_NAME);
        if (n != 1) logmsg(OS_LOG_TYPE_ERROR, "libusb event thread real-time: %d threads changed", n);
        logmsg(OS_LOG_TYPE_DEFAULT, "POD X3 opened (pid %04x)", podx3_pid(dev));
        run_session(dev);
        podx3_close(dev);
        sleep_unless_stopped(1000);
    }
    return NULL;
}

// ---------------------------------------------------------------------------

static int shm_create(void) {
    g_shm_len = podx3_shm_size();
    mach_vm_address_t addr = 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, g_shm_len, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        logmsg(OS_LOG_TYPE_FAULT, "mach_vm_allocate: %d", kr);
        return -1;
    }
    g_shm = (podx3_shm *)(uintptr_t)addr; // zero-filled
    g_shm->magic = PODX3_SHM_MAGIC;
    g_shm->version = PODX3_SHM_VERSION;
    g_shm->ring_frames = PODX3_SHM_RING_FRAMES;
    g_shm->in_channels = PODX3_SHM_IN_CH;
    g_shm->out_channels = PODX3_SHM_OUT_CH;
    g_shm->sample_rate = PODX3_RATE;
    g_shmem = xpc_shmem_create(g_shm, (size_t)g_shm_len);
    return g_shmem ? 0 : -1;
}

static void peer_connected(xpc_connection_t peer) {
    logmsg(OS_LOG_TYPE_DEFAULT, "client connected: pid %d uid %d", xpc_connection_get_pid(peer),
           xpc_connection_get_euid(peer));
    xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
        if (xpc_get_type(msg) == XPC_TYPE_ERROR) {
            if (msg == XPC_ERROR_CONNECTION_INVALID) logmsg(OS_LOG_TYPE_DEFAULT, "client disconnected");
            return;
        }
        if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) return;
        const char *op = xpc_dictionary_get_string(msg, "op");
        xpc_object_t reply = xpc_dictionary_create_reply(msg);
        if (!reply) return;
        if (op && !strcmp(op, "map")) {
            xpc_dictionary_set_value(reply, "shm", g_shmem);
            xpc_dictionary_set_uint64(reply, "size", g_shm_len);
            xpc_dictionary_set_uint64(reply, "version", PODX3_SHM_VERSION);
        } else {
            xpc_dictionary_set_string(reply, "error", "unknown op");
        }
        xpc_connection_send_message(peer, reply);
        xpc_release(reply);
    });
    xpc_connection_resume(peer);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--foreground") || !strcmp(argv[i], "-f")) g_fg = true;
        else {
            fprintf(stderr, "usage: %s [--foreground]\n", argv[0]);
            return 2;
        }
    }
    g_log = os_log_create("com.pkalamarski.podx3", "podx3d");
    if (!g_fg) redirect_stderr();
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_tpf_nominal = 1e9 / PODX3_RATE * tb.denom / tb.numer; // 500 on Apple Silicon

    if (shm_create()) return 1;
    logmsg(OS_LOG_TYPE_DEFAULT, "podx3d starting: shm %llu bytes, %.3f ticks/frame",
           (unsigned long long)g_shm_len, g_tpf_nominal);

    // SIGTERM (launchd, kill) and SIGINT: stop the stream gracefully - a
    // process that dies with transfers in flight leaves the device mid-stream.
    static pthread_t usb;
    dispatch_queue_t q = dispatch_queue_create("com.pkalamarski.podx3d", DISPATCH_QUEUE_SERIAL);
    int sigs[] = {SIGTERM, SIGINT};
    for (int i = 0; i < 2; i++) {
        signal(sigs[i], SIG_IGN);
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0, q);
        dispatch_source_set_event_handler(src, ^{
            logmsg(OS_LOG_TYPE_DEFAULT, "signal received, shutting down");
            atomic_store(&g_stop, true);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                pthread_join(usb, NULL);
                tap_stop(&g_tap);
                logmsg(OS_LOG_TYPE_DEFAULT, "stopped");
                exit(0);
            });
        });
        dispatch_resume(src);
    }

    if (tap_start(&g_tap, NULL)) logmsg(OS_LOG_TYPE_ERROR, "tuner tap unavailable");
    if (pthread_create(&usb, NULL, usb_thread, NULL)) return 1;

    xpc_connection_t listener =
        xpc_connection_create_mach_service(PODX3_MACH_SERVICE, q, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t ev) {
        if (xpc_get_type(ev) == XPC_TYPE_CONNECTION) {
            peer_connected(ev);
        } else if (xpc_get_type(ev) == XPC_TYPE_ERROR) {
            const char *d = xpc_dictionary_get_string(ev, XPC_ERROR_KEY_DESCRIPTION);
            logmsg(OS_LOG_TYPE_ERROR, "XPC listener: %s%s", d ? d : "error",
                   g_fg ? " (expected when not started by launchd: USB only, no plug-in clients)" : "");
        }
    });
    xpc_connection_resume(listener);
    dispatch_main();
}
