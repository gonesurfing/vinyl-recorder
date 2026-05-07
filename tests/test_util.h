#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

extern int tests_run;
extern int tests_failed;

#define TEST(name) static void test_##name(void)

#define RUN(name) do { \
    tests_run++; \
    int _failed_before = tests_failed; \
    test_##name(); \
    if (tests_failed > _failed_before) printf("FAIL %s\n", #name); \
    else                               printf("PASS %s\n", #name); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  ASSERT %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_LL(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  ASSERT %s:%d: expected %lld, got %lld\n", \
                __FILE__, __LINE__, _b, _a); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_NEAR(a, b, tol) do { \
    double _a = (a), _b = (b), _d = fabs(_a - _b); \
    if (_d > (tol)) { \
        fprintf(stderr, "  ASSERT %s:%d: expected %g, got %g (|diff|=%g > %g)\n", \
                __FILE__, __LINE__, _b, _a, _d, (double)(tol)); \
        tests_failed++; \
        return; \
    } \
} while (0)
