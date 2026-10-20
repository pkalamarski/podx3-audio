// Userspace USB transport for the Line 6 POD X3 audio interface (interface 0).
//
// Rules this module enforces (see README):
//  - never calls libusb_set_configuration when the device is already configured,
//    so the control app holding interface 1 keeps working;
//  - never touches interface 1;
//  - never cancels transfers while traffic is flowing: stopping means "stop
//    resubmitting and wait for every in-flight transfer to complete".
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define PODX3_VID 0x0e41
#define PODX3_PID 0x414a
#define PODX3_LIVE_PID 0x414b

#define PODX3_RATE 48000
#define PODX3_IN_CHANNELS 8
#define PODX3_OUT_CHANNELS 2
#define PODX3_BYTES_PER_SAMPLE 3
#define PODX3_IN_FRAME_BYTES (PODX3_IN_CHANNELS * PODX3_BYTES_PER_SAMPLE)   // 24
#define PODX3_OUT_FRAME_BYTES (PODX3_OUT_CHANNELS * PODX3_BYTES_PER_SAMPLE) // 6

#define PODX3_AUDIO_IF 0
#define PODX3_AUDIO_ALT 1
#define PODX3_EP_IN 0x86
#define PODX3_EP_OUT 0x02

typedef struct podx3 podx3_t;

// Called from the USB event thread for every completed IN packet.
// data: interleaved S24_3LE frames (24 bytes each); len: actual_length in bytes
// (may not be a multiple of 24 if the format assumption is wrong - callers check).
// host_time: mach_absolute_time() at transfer completion; pkt_index: index of
// the packet within its transfer (packets complete together, one per 125 us).
typedef void (*podx3_in_cb)(void *user, const uint8_t *data, int len, int status,
                            uint64_t host_time, int pkt_index, int pkts_in_xfer);

// Called from the USB event thread to fill one OUT packet with nframes stereo
// S24_3LE frames (6 bytes each). If NULL, silence is sent.
typedef void (*podx3_out_cb)(void *user, uint8_t *data, int nframes);

typedef struct {
    int n_xfers;        // IN transfers in flight (default 16; costs no latency)
    int n_xfers_out;    // OUT transfers in flight (default 6; each adds 1 ms of output
                        // latency, and libusb/darwin schedules >= 4 frames ahead)
    int pkts_per_xfer;  // iso packets per transfer (default 8 = 1 ms at high speed)
    bool enable_in;
    bool enable_out;
    podx3_in_cb in_cb;
    podx3_out_cb out_cb;
    void *user;
} podx3_stream_cfg;

typedef struct {
    uint64_t in_xfers, out_xfers;
    uint64_t in_pkts, in_pkts_err, in_pkts_empty;
    uint64_t in_bytes, in_frames;
    uint64_t out_pkts, out_frames;
    uint64_t xfer_errors;
    uint64_t len_hist[256]; // histogram of IN actual_length (clamped to 255)
    int last_error;         // last libusb transfer status != COMPLETED
} podx3_stats;

// Opens the device, claims interface 0 and selects altsetting 1.
// verbose: print descriptor information.
int podx3_open(podx3_t **out, bool verbose);
// Selects alt 0, releases interface 0, closes. Streams must be stopped.
void podx3_close(podx3_t *dev);

uint16_t podx3_pid(const podx3_t *dev);
int podx3_maxp_in(const podx3_t *dev);
int podx3_maxp_out(const podx3_t *dev);

// Starts the streams. OUT is submitted before IN (the device only produces
// IN data while it receives OUT traffic).
int podx3_stream_start(podx3_t *dev, const podx3_stream_cfg *cfg);
// Runs libusb events until *stop becomes true, then drains gracefully.
// Returns 0 on clean stop, <0 if the stream died (device error).
int podx3_run(podx3_t *dev, volatile atomic_bool *stop);
// One event-loop slice. Returns <0 if the stream died or nothing is in flight.
int podx3_poll(podx3_t *dev, int timeout_ms);
// Graceful stop without podx3_run: stop resubmitting and wait for completion.
int podx3_stream_stop(podx3_t *dev);

const podx3_stats *podx3_get_stats(const podx3_t *dev);

// Optional Linux-style device init (vendor request 0x67). Not needed for audio
// according to the Linux driver; kept for experiments.
int podx3_linux_init(podx3_t *dev, uint32_t *firmware_out);

// Gives the calling thread a Mach time-constraint (real-time) policy suited to
// servicing 1 ms isochronous completions. Returns 0 on success.
int podx3_make_thread_realtime(void);
// Same for every thread of the process, including libusb's internal darwin
// event thread that receives IOKit completions (call after podx3_open).
int podx3_make_all_threads_realtime(void);

// Installs SIGINT/SIGTERM handlers that set *flag.
void podx3_install_stop_signals(volatile atomic_bool *flag);

// S24_3LE helpers
static inline int32_t podx3_s24_get(const uint8_t *p) {
    int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
    return (v << 8) >> 8; // sign extend
}
static inline void podx3_s24_put(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}
