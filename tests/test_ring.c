#include "test_util.h"
#include "../src/ring.h"
#include <string.h>

TEST(init_rounds_up_to_pow2) {
    ring_t r;
    ASSERT(ring_init(&r, 1000) == 0);
    ASSERT_EQ_LL(r.cap, 1024);
    ASSERT_EQ_LL(r.mask, 1023);
    ring_free(&r);
}

TEST(write_then_read_returns_same_data) {
    ring_t r;
    ring_init(&r, 16);
    int16_t in[6]  = { 1, -2, 3, -4, 5, -6 };
    int16_t out[6] = { 0 };
    ASSERT_EQ_LL(ring_write(&r, in, 6), 6);
    ASSERT_EQ_LL(ring_avail_read(&r), 6);
    ASSERT_EQ_LL(ring_read(&r, out, 6), 6);
    for (int i = 0; i < 6; i++) ASSERT_EQ_LL(out[i], in[i]);
    ASSERT_EQ_LL(ring_avail_read(&r), 0);
    ring_free(&r);
}

TEST(write_clipped_when_full) {
    ring_t r;
    ring_init(&r, 4);  // cap = 4
    int16_t in[6] = { 10, 20, 30, 40, 50, 60 };
    ASSERT_EQ_LL(ring_write(&r, in, 6), 4);
    ASSERT_EQ_LL(ring_avail_write(&r), 0);
    int16_t out[4] = { 0 };
    ASSERT_EQ_LL(ring_read(&r, out, 4), 4);
    ASSERT_EQ_LL(out[0], 10);
    ASSERT_EQ_LL(out[3], 40);
    ring_free(&r);
}

TEST(read_clipped_when_empty) {
    ring_t r;
    ring_init(&r, 8);
    int16_t out[4];
    ASSERT_EQ_LL(ring_read(&r, out, 4), 0);
    ring_free(&r);
}

TEST(wraps_around_correctly) {
    ring_t r;
    ring_init(&r, 8);  // cap = 8
    int16_t a[6] = { 1, 2, 3, 4, 5, 6 };
    ring_write(&r, a, 6);
    int16_t tmp[4];
    ring_read(&r, tmp, 4);                 // head=6, tail=4
    int16_t b[5] = { 7, 8, 9, 10, 11 };
    ASSERT_EQ_LL(ring_write(&r, b, 5), 5); // wraps: writes 2 then 3
    int16_t out[7];
    ASSERT_EQ_LL(ring_read(&r, out, 7), 7);
    int16_t expected[7] = { 5, 6, 7, 8, 9, 10, 11 };
    for (int i = 0; i < 7; i++) ASSERT_EQ_LL(out[i], expected[i]);
    ring_free(&r);
}

void run_ring_tests(void) {
    RUN(init_rounds_up_to_pow2);
    RUN(write_then_read_returns_same_data);
    RUN(write_clipped_when_full);
    RUN(read_clipped_when_empty);
    RUN(wraps_around_correctly);
}
