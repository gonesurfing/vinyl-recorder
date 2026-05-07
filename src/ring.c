#include "ring.h"
#include <stdlib.h>
#include <string.h>

static size_t next_pow2(size_t n) {
    if (n < 2) return 2;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

int ring_init(ring_t *r, size_t cap) {
    r->cap  = next_pow2(cap);
    r->mask = r->cap - 1;
    r->buf  = (int16_t *)calloc(r->cap, sizeof(int16_t));
    if (!r->buf) return -1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return 0;
}

void ring_free(ring_t *r) {
    free(r->buf);
    r->buf = NULL;
    r->cap = r->mask = 0;
}

size_t ring_avail_read(const ring_t *r) {
    size_t h = atomic_load_explicit((_Atomic size_t *)&r->head, memory_order_acquire);
    size_t t = atomic_load_explicit((_Atomic size_t *)&r->tail, memory_order_relaxed);
    return h - t;
}

size_t ring_avail_write(const ring_t *r) {
    return r->cap - ring_avail_read(r);
}

size_t ring_write(ring_t *r, const int16_t *src, size_t n) {
    size_t avail = ring_avail_write(r);
    if (n > avail) n = avail;
    if (n == 0) return 0;
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t pos  = head & r->mask;
    size_t first = (pos + n > r->cap) ? r->cap - pos : n;
    memcpy(&r->buf[pos], src, first * sizeof(int16_t));
    if (n > first) memcpy(&r->buf[0], src + first, (n - first) * sizeof(int16_t));
    atomic_store_explicit(&r->head, head + n, memory_order_release);
    return n;
}

size_t ring_read(ring_t *r, int16_t *dst, size_t n) {
    size_t avail = ring_avail_read(r);
    if (n > avail) n = avail;
    if (n == 0) return 0;
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t pos  = tail & r->mask;
    size_t first = (pos + n > r->cap) ? r->cap - pos : n;
    memcpy(dst, &r->buf[pos], first * sizeof(int16_t));
    if (n > first) memcpy(dst + first, &r->buf[0], (n - first) * sizeof(int16_t));
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    return n;
}
