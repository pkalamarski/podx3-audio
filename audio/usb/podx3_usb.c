#include "podx3_usb.h"

#include <libusb.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_XFERS 32

struct podx3 {
    libusb_context *ctx;
    libusb_device_handle *h;
    uint16_t pid;
    int maxp_in, maxp_out;
    int interval_in, interval_out; // bInterval of the alt-1 endpoints
    bool claimed;

    podx3_stream_cfg cfg;
    struct libusb_transfer *in_x[MAX_XFERS], *out_x[MAX_XFERS];
    int inflight, inflight_in;
    bool stopping;    // stop resubmitting OUT
    bool stopping_in; // stop resubmitting IN (always set before stopping)
    bool dead;

    // OUT rate control: OUT frames follow IN frames (Linux sends as many
    // frames as the last IN packet carried); we track totals instead.
    bool in_flowing;
    int64_t out_lead; // out_frames - in_frames when IN started flowing

    podx3_stats st;
};

static const char *xfer_status_str(int s) {
    switch (s) {
    case LIBUSB_TRANSFER_COMPLETED: return "completed";
    case LIBUSB_TRANSFER_ERROR: return "error";
    case LIBUSB_TRANSFER_TIMED_OUT: return "timed out";
    case LIBUSB_TRANSFER_CANCELLED: return "cancelled";
    case LIBUSB_TRANSFER_STALL: return "stall";
    case LIBUSB_TRANSFER_NO_DEVICE: return "no device";
    case LIBUSB_TRANSFER_OVERFLOW: return "overflow";
    default: return "?";
    }
}

static void print_descriptors(libusb_device *d) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(d, &cfg) != 0) {
        printf("  (no active config descriptor)\n");
        return;
    }
    printf("  config %d: %d interfaces\n", cfg->bConfigurationValue, cfg->bNumInterfaces);
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[a];
            printf("  if %d alt %d class 0x%02x sub 0x%02x proto 0x%02x eps %d\n",
                   id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass,
                   id->bInterfaceSubClass, id->bInterfaceProtocol, id->bNumEndpoints);
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                static const char *types[] = {"control", "iso", "bulk", "interrupt"};
                static const char *sync[] = {"none", "async", "adaptive", "sync"};
                static const char *usage[] = {"data", "feedback", "implicit-fb", "reserved"};
                int type = ep->bmAttributes & 3;
                printf("    ep 0x%02x %-9s maxp %d (mult %d) bInterval %d", ep->bEndpointAddress,
                       types[type], ep->wMaxPacketSize & 0x7ff, ((ep->wMaxPacketSize >> 11) & 3) + 1,
                       ep->bInterval);
                if (type == 1)
                    printf(" sync %s usage %s", sync[(ep->bmAttributes >> 2) & 3],
                           usage[(ep->bmAttributes >> 4) & 3]);
                if (ep->extra_length) printf(" extra %d B", ep->extra_length);
                printf("\n");
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

static int read_alt1_endpoints(podx3_t *dev) {
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(libusb_get_device(dev->h), &cfg);
    if (r) return r;
    r = LIBUSB_ERROR_NOT_FOUND;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[a];
            if (id->bInterfaceNumber != PODX3_AUDIO_IF || id->bAlternateSetting != PODX3_AUDIO_ALT)
                continue;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                int maxp = (ep->wMaxPacketSize & 0x7ff) * (((ep->wMaxPacketSize >> 11) & 3) + 1);
                if (ep->bEndpointAddress == PODX3_EP_IN) {
                    dev->maxp_in = maxp;
                    dev->interval_in = ep->bInterval;
                } else if (ep->bEndpointAddress == PODX3_EP_OUT) {
                    dev->maxp_out = maxp;
                    dev->interval_out = ep->bInterval;
                }
            }
            r = (dev->maxp_in && dev->maxp_out) ? 0 : LIBUSB_ERROR_NOT_FOUND;
        }
    }
    libusb_free_config_descriptor(cfg);
    return r;
}

int podx3_open(podx3_t **out, bool verbose) {
    podx3_t *dev = calloc(1, sizeof(*dev));
    int r = libusb_init_context(&dev->ctx, NULL, 0);
    if (r) {
        fprintf(stderr, "libusb_init: %s\n", libusb_strerror(r));
        free(dev);
        return r;
    }
    dev->pid = PODX3_PID;
    dev->h = libusb_open_device_with_vid_pid(dev->ctx, PODX3_VID, PODX3_PID);
    if (!dev->h) {
        dev->pid = PODX3_LIVE_PID;
        dev->h = libusb_open_device_with_vid_pid(dev->ctx, PODX3_VID, PODX3_LIVE_PID);
    }
    if (!dev->h) {
        fprintf(stderr, "POD X3 not found (or cannot be opened)\n");
        r = LIBUSB_ERROR_NO_DEVICE;
        goto fail;
    }

    int config = -1;
    r = libusb_get_configuration(dev->h, &config);
    if (verbose) {
        struct libusb_device_descriptor dd;
        libusb_get_device_descriptor(libusb_get_device(dev->h), &dd);
        printf("POD X3 %04x:%04x bcdDevice %x.%02x, speed %d, configuration %d (r=%d)\n", dd.idVendor,
               dd.idProduct, dd.bcdDevice >> 8, dd.bcdDevice & 0xff,
               libusb_get_device_speed(libusb_get_device(dev->h)), config, r);
    }

    r = libusb_claim_interface(dev->h, PODX3_AUDIO_IF);
    if (r == LIBUSB_ERROR_NOT_FOUND) {
        // On macOS the interfaces only appear after a configuration is selected,
        // even though the device reports configuration 1. If interface 0 does not
        // exist, interface 1 does not either, so nobody else can be using it.
        fprintf(stderr, "interface 0 not present, configuration %d -> selecting configuration 1\n", config);
        r = libusb_set_configuration(dev->h, 1);
        if (r == 0) r = libusb_claim_interface(dev->h, PODX3_AUDIO_IF);
    }
    if (r) {
        fprintf(stderr, "claim interface 0: %s\n", libusb_strerror(r));
        goto fail;
    }
    dev->claimed = true;

    if (verbose) print_descriptors(libusb_get_device(dev->h));

    r = read_alt1_endpoints(dev);
    if (r) {
        fprintf(stderr, "alt %d endpoints 0x%02x/0x%02x not found\n", PODX3_AUDIO_ALT, PODX3_EP_IN, PODX3_EP_OUT);
        goto fail;
    }
    r = libusb_set_interface_alt_setting(dev->h, PODX3_AUDIO_IF, PODX3_AUDIO_ALT);
    if (r) {
        fprintf(stderr, "set alt %d: %s\n", PODX3_AUDIO_ALT, libusb_strerror(r));
        goto fail;
    }
    if (verbose)
        printf("interface 0 alt 1: IN 0x%02x maxp %d bInterval %d, OUT 0x%02x maxp %d bInterval %d\n",
               PODX3_EP_IN, dev->maxp_in, dev->interval_in, PODX3_EP_OUT, dev->maxp_out, dev->interval_out);
    *out = dev;
    return 0;

fail:
    podx3_close(dev);
    return r ? r : LIBUSB_ERROR_OTHER;
}

void podx3_close(podx3_t *dev) {
    if (!dev) return;
    if (dev->inflight) fprintf(stderr, "warning: closing with %d transfers in flight\n", dev->inflight);
    for (int i = 0; i < MAX_XFERS; i++) {
        if (!dev->inflight) {
            if (dev->in_x[i]) libusb_free_transfer(dev->in_x[i]);
            if (dev->out_x[i]) libusb_free_transfer(dev->out_x[i]);
        }
    }
    if (dev->h) {
        if (dev->claimed) {
            libusb_set_interface_alt_setting(dev->h, PODX3_AUDIO_IF, 0);
            libusb_release_interface(dev->h, PODX3_AUDIO_IF);
        }
        libusb_close(dev->h);
    }
    if (dev->ctx) libusb_exit(dev->ctx);
    free(dev);
}

uint16_t podx3_pid(const podx3_t *dev) { return dev->pid; }
int podx3_maxp_in(const podx3_t *dev) { return dev->maxp_in; }
int podx3_maxp_out(const podx3_t *dev) { return dev->maxp_out; }
const podx3_stats *podx3_get_stats(const podx3_t *dev) { return &dev->st; }

// ---------------------------------------------------------------------------
// Streaming

static int nominal_frames_per_pkt(void) { return PODX3_RATE / 8000; } // 6 at high speed

static void fill_out_xfer(podx3_t *dev, struct libusb_transfer *x) {
    int max_frames = dev->maxp_out / PODX3_OUT_FRAME_BYTES;
    uint8_t *p = x->buffer;
    for (int i = 0; i < x->num_iso_packets; i++) {
        int n = nominal_frames_per_pkt();
        if (dev->in_flowing) {
            int64_t err = (int64_t)dev->st.in_frames + dev->out_lead - (int64_t)dev->st.out_frames;
            if (err > n) n++;
            else if (err < -n) n--;
        }
        if (n > max_frames) n = max_frames;
        if (dev->cfg.out_cb) dev->cfg.out_cb(dev->cfg.user, p, n);
        else memset(p, 0, (size_t)n * PODX3_OUT_FRAME_BYTES);
        x->iso_packet_desc[i].length = (unsigned)(n * PODX3_OUT_FRAME_BYTES);
        p += n * PODX3_OUT_FRAME_BYTES;
        dev->st.out_frames += (uint64_t)n;
        dev->st.out_pkts++;
    }
    x->length = (int)(p - x->buffer);
}

static void xfer_done(podx3_t *dev, struct libusb_transfer *x) {
    if (x->status != LIBUSB_TRANSFER_COMPLETED) {
        dev->st.xfer_errors++;
        dev->st.last_error = x->status;
        if (dev->st.xfer_errors <= 10)
            fprintf(stderr, "transfer ep 0x%02x: %s\n", x->endpoint, xfer_status_str(x->status));
        if (x->status == LIBUSB_TRANSFER_NO_DEVICE) dev->dead = true;
    }
}

static void resubmit_or_retire(podx3_t *dev, struct libusb_transfer *x) {
    bool is_in = x->endpoint & 0x80;
    if (!(is_in ? dev->stopping_in : dev->stopping) && !dev->dead) {
        int r = libusb_submit_transfer(x);
        if (r == 0) return;
        fprintf(stderr, "resubmit ep 0x%02x: %s\n", x->endpoint, libusb_strerror(r));
        dev->dead = true;
    }
    dev->inflight--;
    if (is_in) dev->inflight_in--;
}

static void LIBUSB_CALL in_cb(struct libusb_transfer *x) {
    podx3_t *dev = x->user_data;
    uint64_t now = mach_absolute_time();
    dev->st.in_xfers++;
    xfer_done(dev, x);
    if (x->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < x->num_iso_packets; i++) {
            const struct libusb_iso_packet_descriptor *pd = &x->iso_packet_desc[i];
            const uint8_t *data = x->buffer + (size_t)i * dev->maxp_in;
            dev->st.in_pkts++;
            if (pd->status != LIBUSB_TRANSFER_COMPLETED) dev->st.in_pkts_err++;
            if (pd->actual_length == 0) dev->st.in_pkts_empty++;
            dev->st.len_hist[pd->actual_length > 255 ? 255 : pd->actual_length]++;
            dev->st.in_bytes += pd->actual_length;
            int frames = (int)pd->actual_length / PODX3_IN_FRAME_BYTES;
            if (frames && !dev->in_flowing) {
                dev->in_flowing = true;
                dev->out_lead = (int64_t)dev->st.out_frames - (int64_t)dev->st.in_frames;
            }
            dev->st.in_frames += (uint64_t)frames;
            if (dev->cfg.in_cb)
                dev->cfg.in_cb(dev->cfg.user, data, (int)pd->actual_length, pd->status, now, i,
                               x->num_iso_packets);
        }
    }
    resubmit_or_retire(dev, x);
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *x) {
    podx3_t *dev = x->user_data;
    dev->st.out_xfers++;
    xfer_done(dev, x);
    if (!dev->stopping && !dev->dead) fill_out_xfer(dev, x);
    resubmit_or_retire(dev, x);
}

int podx3_stream_start(podx3_t *dev, const podx3_stream_cfg *cfg_in) {
    dev->cfg = *cfg_in;
    if (dev->cfg.n_xfers <= 0) dev->cfg.n_xfers = 16;
    if (dev->cfg.n_xfers > MAX_XFERS) dev->cfg.n_xfers = MAX_XFERS;
    if (dev->cfg.n_xfers_out <= 0) dev->cfg.n_xfers_out = 6;
    if (dev->cfg.n_xfers_out > MAX_XFERS) dev->cfg.n_xfers_out = MAX_XFERS;
    if (dev->cfg.pkts_per_xfer <= 0) dev->cfg.pkts_per_xfer = 8;
    int np = dev->cfg.pkts_per_xfer;
    if (!dev->cfg.enable_out) {
        // The device must never be polled for IN without OUT traffic: an
        // IN-only session left the POD's audio engine silent until power cycle.
        fprintf(stderr, "IN without OUT is not allowed\n");
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    dev->stopping = dev->stopping_in = dev->dead = dev->in_flowing = false;
    memset(&dev->st, 0, sizeof(dev->st));

    // OUT first: the device only streams IN while it receives OUT data.
    for (int pass = 0; pass < 2; pass++) {
        bool is_out = pass == 0;
        if (is_out ? !dev->cfg.enable_out : !dev->cfg.enable_in) continue;
        int count = is_out ? dev->cfg.n_xfers_out : dev->cfg.n_xfers;
        for (int i = 0; i < count; i++) {
            struct libusb_transfer **slot = is_out ? &dev->out_x[i] : &dev->in_x[i];
            if (!*slot) {
                *slot = libusb_alloc_transfer(np);
                int maxp = is_out ? dev->maxp_out : dev->maxp_in;
                uint8_t *buf = calloc((size_t)np, (size_t)maxp);
                libusb_fill_iso_transfer(*slot, dev->h, is_out ? PODX3_EP_OUT : PODX3_EP_IN, buf, np * maxp,
                                         np, is_out ? out_cb : in_cb, dev, 0);
                (*slot)->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
                if (!is_out) libusb_set_iso_packet_lengths(*slot, (unsigned)maxp);
            }
            if (is_out) fill_out_xfer(dev, *slot);
            int r = libusb_submit_transfer(*slot);
            if (r) {
                fprintf(stderr, "submit %s #%d: %s\n", is_out ? "OUT" : "IN", i, libusb_strerror(r));
                podx3_stream_stop(dev);
                return r;
            }
            dev->inflight++;
            if (!is_out) dev->inflight_in++;
        }
    }
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int podx3_stream_stop(podx3_t *dev) {
    // Mirror the Linux release order: IN first, OUT keeps running until every IN
    // transfer has completed, then OUT drains. No transfer is ever cancelled.
    dev->stopping_in = true;
    double deadline = now_s() + 3.0;
    while (dev->inflight_in > 0 && now_s() < deadline) {
        struct timeval tv = {0, 50000};
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    }
    dev->stopping = true;
    while (dev->inflight > 0 && now_s() < deadline) {
        struct timeval tv = {0, 50000};
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    }
    if (dev->inflight > 0) {
        fprintf(stderr, "warning: %d transfers did not complete within 3 s\n", dev->inflight);
        return -1;
    }
    return 0;
}

int podx3_poll(podx3_t *dev, int timeout_ms) {
    if (dev->dead || dev->inflight == 0) return -1;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    if (r && r != LIBUSB_ERROR_INTERRUPTED) {
        fprintf(stderr, "handle_events: %s\n", libusb_strerror(r));
        return -1;
    }
    return dev->dead ? -1 : 0;
}

int podx3_run(podx3_t *dev, volatile atomic_bool *stop) {
    while (!atomic_load(stop))
        if (podx3_poll(dev, 100) < 0) break;
    bool died = dev->dead;
    podx3_stream_stop(dev);
    return died ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Linux podhd_dev_start() equivalent (optional)

static int read_data(podx3_t *dev, uint16_t addr, uint8_t *buf, uint8_t len) {
    int r = libusb_control_transfer(dev->h, 0x40, 0x67, (uint16_t)((len << 8) | 0x21), addr, NULL, 0, 1000);
    if (r < 0) return r;
    uint8_t got = 0xff;
    for (int i = 0; i < 50 && got == 0xff; i++) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        r = libusb_control_transfer(dev->h, 0xc0, 0x67, 0x0012, 0, &got, 1, 1000);
        if (r < 0) return r;
    }
    if (got != len) return LIBUSB_ERROR_IO;
    r = libusb_control_transfer(dev->h, 0xc0, 0x67, 0x0013, 0, buf, len, 1000);
    return r < 0 ? r : 0;
}

int podx3_linux_init(podx3_t *dev, uint32_t *firmware_out) {
    int r = libusb_control_transfer(dev->h, 0x40, 0x67, 0x11, 0, NULL, 0, 1000);
    if (r < 0) return r;
    uint8_t fw[3] = {0};
    r = libusb_control_transfer(dev->h, 0xc0, 0x67, 0x11, 0, fw, 3, 1000);
    if (r < 0) return r;
    if (firmware_out) *firmware_out = ((uint32_t)fw[0] << 16) | ((uint32_t)fw[1] << 8) | fw[2];
    uint8_t buf[8];
    for (int i = 0; i <= 16; i++) {
        r = read_data(dev, (uint16_t)(0xf000 + 0x08 * i), buf, 8);
        if (r) return r;
    }
    return 0;
}

// ---------------------------------------------------------------------------

static int set_rt_policy(thread_act_t th) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ns_to_abs = (double)tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)(1000000 * ns_to_abs),     // 1 ms
        .computation = (uint32_t)(200000 * ns_to_abs), // 0.2 ms
        .constraint = (uint32_t)(1000000 * ns_to_abs),
        .preemptible = 1,
    };
    kern_return_t kr = thread_policy_set(th, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    return kr == KERN_SUCCESS ? 0 : -1;
}

int podx3_make_thread_realtime(void) {
    thread_act_t self = mach_thread_self();
    int r = set_rt_policy(self);
    mach_port_deallocate(mach_task_self(), self);
    return r;
}

int podx3_make_all_threads_realtime(void) {
    thread_act_array_t threads;
    mach_msg_type_number_t n;
    if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return -1;
    int fails = 0;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        fails += set_rt_policy(threads[i]) != 0;
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(thread_act_t));
    return fails ? -1 : 0;
}

static volatile atomic_bool *g_stop_flag;
static void on_signal(int sig) {
    (void)sig;
    if (g_stop_flag) atomic_store(g_stop_flag, true);
}

void podx3_install_stop_signals(volatile atomic_bool *flag) {
    g_stop_flag = flag;
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
