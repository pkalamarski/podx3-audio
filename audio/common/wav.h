// Minimal PCM WAV writer (24-bit little endian, interleaved).
#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    int channels;
    uint32_t data_bytes;
} wav_writer;

static inline void wav_put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}
static inline void wav_put16(FILE *f, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static inline void wav_write_header(wav_writer *w, int rate) {
    int block = w->channels * 3;
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f);
    wav_put32(w->f, 36 + w->data_bytes);
    fwrite("WAVEfmt ", 1, 8, w->f);
    wav_put32(w->f, 16);
    wav_put16(w->f, 1); // PCM
    wav_put16(w->f, (uint16_t)w->channels);
    wav_put32(w->f, (uint32_t)rate);
    wav_put32(w->f, (uint32_t)(rate * block));
    wav_put16(w->f, (uint16_t)block);
    wav_put16(w->f, 24);
    fwrite("data", 1, 4, w->f);
    wav_put32(w->f, w->data_bytes);
}

static inline int wav_open(wav_writer *w, const char *path, int channels, int rate) {
    w->f = fopen(path, "wb");
    if (!w->f) return -1;
    w->channels = channels;
    w->data_bytes = 0;
    wav_write_header(w, rate);
    return 0;
}

static inline void wav_write(wav_writer *w, const uint8_t *s24, size_t frames) {
    size_t n = frames * (size_t)w->channels * 3;
    fwrite(s24, 1, n, w->f);
    w->data_bytes += (uint32_t)n;
}

static inline void wav_close(wav_writer *w, int rate) {
    if (!w->f) return;
    wav_write_header(w, rate);
    fclose(w->f);
    w->f = NULL;
}
