#include "test_util.h"
#include "../src/sample.h"
#include <stdint.h>

TEST(s16_to_s32_left_justifies) {
    int16_t in[4]  = { 0, 1, -1, 12345 };
    int32_t out[4] = { 0 };
    samples_s16_to_s32(in, out, 4);
    ASSERT_EQ_LL(out[0], 0);
    ASSERT_EQ_LL(out[1], 65536);
    ASSERT_EQ_LL(out[2], -65536);
    ASSERT_EQ_LL(out[3], 12345LL * 65536);
}

TEST(s16_to_s32_full_scale_extremes) {
    int16_t in[2]  = { INT16_MAX, INT16_MIN };
    int32_t out[2] = { 0 };
    samples_s16_to_s32(in, out, 2);
    ASSERT_EQ_LL(out[0], 0x7FFF0000LL);
    ASSERT_EQ_LL(out[1], INT32_MIN);
}

TEST(s24_to_s32_left_justifies) {
    // ALSA S24_LE: 24-bit value in the low three bytes of a 32-bit word.
    int32_t in[4]  = { 1, -1, 0x7FFFFF, -0x800000 };
    int32_t out[4] = { 0 };
    samples_s24_to_s32(in, out, 4);
    ASSERT_EQ_LL(out[0], 256);
    ASSERT_EQ_LL(out[1], -256);
    ASSERT_EQ_LL(out[2], 0x7FFFFF00LL);
    ASSERT_EQ_LL(out[3], INT32_MIN);
}

TEST(s24_to_s32_ignores_padding_byte) {
    // The top byte of an S24_LE container is padding and may hold anything
    // (zero instead of sign extension, or junk). Only the low 24 bits count.
    int32_t in[3]  = { 0x00FFFFFF, (int32_t)0xAB000001, 0x12800000 };
    int32_t out[3] = { 0 };
    samples_s24_to_s32(in, out, 3);
    ASSERT_EQ_LL(out[0], -256);         // 0xFFFFFF is -1 in 24-bit
    ASSERT_EQ_LL(out[1], 256);          // junk top byte, value 1
    ASSERT_EQ_LL(out[2], INT32_MIN);    // 0x800000 is the 24-bit minimum
}

void run_sample_tests(void) {
    RUN(s16_to_s32_left_justifies);
    RUN(s16_to_s32_full_scale_extremes);
    RUN(s24_to_s32_left_justifies);
    RUN(s24_to_s32_ignores_padding_byte);
}
