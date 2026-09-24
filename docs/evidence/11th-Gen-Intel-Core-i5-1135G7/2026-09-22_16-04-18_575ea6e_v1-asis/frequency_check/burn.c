/* Steady compute load for a frequency-stability check: FP multiply-add chains
 * that converge (no inf/denormals) interleaved with an xorshift integer chain. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 60.0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double c = 1.0, d = 1.0;
    unsigned long x = 88172645463325252ul, n = 0;
    for (;;) {
        for (int i = 0; i < 1000000; i++) {
            c = c * 0.5 + 1.0; d = d * 0.25 + 0.75;
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        }
        n++;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9 >= secs) break;
    }
    printf("%lu %g\n", n, c + d + (double)(x & 0xff));
    return 0;
}
