/* Estimate the clock of the core this thread runs on.
 *
 * A chain of dependent integer adds retires at one add per cycle on every
 * core this list covers, so wall time per add is the cycle time. Apple does
 * not publish M-series clocks, so every benchmark README quotes this
 * estimate as its frequency field. Run it under the same QoS/affinity as
 * the benchmark it accompanies. Takes the min over REPS runs so DVFS ramp
 * and interrupts only make the estimate lower, never higher.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "timing.h"

int main(void) {
    const uint64_t n = (uint64_t)env_int("CLOCK_ITERS_M", 400) * 1000000ull;
    int reps = env_int("REPS", 7);
    double best = 1e300;
    for (int r = 0; r < reps; r++) {
        uint64_t x = 0;
        uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < n; i += 8) {
#if defined(__aarch64__)
            __asm__ volatile(
                "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
                "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
                : "+r"(x));
#elif defined(__x86_64__)
            __asm__ volatile(
                "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
                "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
                : "+r"(x));
#else
#error "unsupported architecture"
#endif
        }
        uint64_t t1 = now_ns();
        SINK(x);
        double ns = (double)(t1 - t0);
        if (ns < best) best = ns;
    }
    double ghz = (double)n / best;
    printf("estimated clock: %.2f GHz (dependent 1-cycle add chain, %llu adds, min of %d runs)\n",
           ghz, (unsigned long long)n, reps);
    return 0;
}
