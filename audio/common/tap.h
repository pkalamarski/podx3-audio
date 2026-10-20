// Audio tap: serves one input channel to local clients over a Unix socket, as
// a raw stream of mono int16 little-endian samples at 12 kHz (48 kHz / 4).
// The control server (control/src/tap.ts) relays it to the web tuner.
//
// The USB callback only pushes samples into a lock-free ring (tap_push); a
// separate thread decimates and writes to clients, so a slow or stuck client
// can never stall USB. Clients that fall behind are dropped.
#pragma once

#include "ring.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TAP_DECIM 4
#define TAP_MAX_CLIENTS 8

typedef struct {
    ring_t ring; // mono float, 48 kHz
    int listen_fd;
    int clients[TAP_MAX_CLIENTS];
    pthread_t thread;
    _Atomic bool stop;
    char path[104];
    float acc;
    int acc_n;
} tap_t;

// Socket path shared with control/src/tap.ts.
static inline void tap_default_path(char *buf, size_t len) {
    snprintf(buf, len, "/tmp/podx3-tap-%d.sock", (int)getuid());
}

// Producer side (USB thread). Never blocks; drops samples when the ring is full.
static inline void tap_push(tap_t *t, float sample) {
    if (t && t->listen_fd >= 0) ring_write(&t->ring, &sample, 1);
}

static inline void tap_accept(tap_t *t) {
    for (;;) {
        int fd = accept(t->listen_fd, NULL, NULL);
        if (fd < 0) return;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        int slot = -1;
        for (int i = 0; i < TAP_MAX_CLIENTS; i++)
            if (t->clients[i] < 0) slot = i;
        if (slot < 0) close(fd);
        else t->clients[slot] = fd;
    }
}

static inline void *tap_thread(void *arg) {
    tap_t *t = arg;
    int16_t out[1024];
    while (!atomic_load(&t->stop)) {
        usleep(20000);
        tap_accept(t);
        int n = 0;
        float s;
        // Box-filter decimation: the tuner only needs the fundamental (< 1.5 kHz).
        while (n < 1024 && ring_pop(&t->ring, &s)) {
            t->acc += s;
            if (++t->acc_n == TAP_DECIM) {
                float v = t->acc / TAP_DECIM * 32767.0f;
                out[n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                t->acc = 0;
                t->acc_n = 0;
            }
        }
        if (!n) continue;
        for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
            if (t->clients[i] < 0) continue;
            ssize_t w = write(t->clients[i], out, (size_t)n * sizeof out[0]);
            if (w != (ssize_t)(n * sizeof out[0])) { // gone, or too slow to keep up
                close(t->clients[i]);
                t->clients[i] = -1;
            }
        }
    }
    return NULL;
}

// Returns 0 on success. On failure the tap stays disabled and tap_push is a no-op.
static inline int tap_start(tap_t *t, const char *path) {
    memset(t, 0, sizeof *t);
    t->listen_fd = -1;
    for (int i = 0; i < TAP_MAX_CLIENTS; i++) t->clients[i] = -1;
    if (path) snprintf(t->path, sizeof t->path, "%s", path);
    else tap_default_path(t->path, sizeof t->path);
    if (ring_init(&t->ring, 1, 16384)) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", t->path);
    unlink(t->path); // stale socket from a previous run
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 4)) {
        fprintf(stderr, "tap: %s: %s\n", t->path, strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    t->listen_fd = fd;
    if (pthread_create(&t->thread, NULL, tap_thread, t)) {
        close(fd);
        t->listen_fd = -1;
        return -1;
    }
    return 0;
}

static inline void tap_stop(tap_t *t) {
    if (t->listen_fd < 0) return;
    atomic_store(&t->stop, true);
    pthread_join(t->thread, NULL);
    for (int i = 0; i < TAP_MAX_CLIENTS; i++)
        if (t->clients[i] >= 0) close(t->clients[i]);
    close(t->listen_fd);
    unlink(t->path);
    t->listen_fd = -1;
}
