#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t       *buf;
    size_t         cap;     // power of two, in samples
    size_t         mask;
    _Atomic size_t head;    // monotonic write index
    _Atomic size_t tail;    // monotonic read index
} ring_t;

// Initialize. cap is rounded up to the next power of two. Returns 0 on success.
int  ring_init(ring_t *r, size_t cap);
void ring_free(ring_t *r);

// SPSC write/read. Return number of int16 samples actually transferred.
size_t ring_write(ring_t *r, const int16_t *src, size_t n);
size_t ring_read (ring_t *r,       int16_t *dst, size_t n);

size_t ring_avail_read (const ring_t *r);
size_t ring_avail_write(const ring_t *r);
