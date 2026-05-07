#include <stdio.h>

int tests_run = 0;
int tests_failed = 0;

void run_ring_tests(void);
void run_level_tests(void);
void run_recorder_tests(void);

int main(void) {
    run_ring_tests();
    run_level_tests();
    run_recorder_tests();
    printf("\n%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
