/* 02-branch-misprediction: what an unpredictable conditional branch costs.
 *
 * One loop sums every element of a random byte-valued array that is at or
 * above a threshold. The same loop runs over the data unsorted and sorted,
 * and a branchless select runs over both. Sorting changes nothing about the
 * work or the result, only whether the branch is predictable, so the gap
 * between the unsorted and sorted branchy runs is the misprediction cost
 * on its own. A second threshold makes the condition true for one element
 * in twenty instead of one in two, which shows that predictability, not
 * the taken fraction, sets the cost.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <inttypes.h>

#define NOINLINE __attribute__((noinline))

/* Branchy. The taken arm holds an empty volatile asm on the accumulator.
 * The asm costs nothing, but the compiler may not execute it speculatively,
 * so it cannot if-convert the arm into a csel or vectorise the loop: a real
 * conditional branch on the data has to stay. This is the shape any loop
 * keeps when the taken arm does something the compiler cannot hoist, such
 * as a store it cannot prove safe or a call. */
NOINLINE static uint64_t sum_branchy(const uint32_t *a, size_t n, uint32_t t) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = a[i];
        if (v >= t) {
            sum += v;
            __asm__ volatile("" : "+r"(sum));
        }
    }
    return sum;
}

/* Branchless. The select is unconditional, so the compiler emits csel and
 * no branch depends on the data. The same empty asm sits on the
 * accumulator, but outside the select, so the only difference from
 * sum_branchy is where the condition is resolved. The asm also keeps the
 * loop scalar, which is the like-for-like comparison with the branchy loop. */
NOINLINE static uint64_t sum_branchless(const uint32_t *a, size_t n, uint32_t t) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = a[i];
        sum += (v >= t) ? v : 0;
        __asm__ volatile("" : "+r"(sum));
    }
    return sum;
}

/* The same select with no asm at all, which is what a reader would write.
 * At -O2 clang vectorises it into a NEON compare, and-mask and widening add
 * over several lanes per instruction. Included so the scalar branchless
 * number is not mistaken for the best the compiler can do. */
NOINLINE static uint64_t sum_vector(const uint32_t *a, size_t n, uint32_t t) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = a[i];
        sum += (v >= t) ? v : 0;
    }
    return sum;
}

/* splitmix64: deterministic, so every run and every variant sees the same
 * data and the checksums are comparable across runs. */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static double env_double(const char *name, double dflt) {
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : dflt;
}

typedef uint64_t (*kernel_fn)(const uint32_t *, size_t, uint32_t);

typedef struct {
    const char *kernel_name;
    kernel_fn fn;
} kernel_t;

int main(void) {
    const int quick = env_int("QUICK", 0);
    const size_t n = quick ? ((size_t)1 << 18) : ((size_t)1 << 22);
    const int reps = env_int("REPS", quick ? 5 : 31);
    /* run.sh passes the clock_estimate result so cycles can be derived. */
    const double ghz = env_double("CLOCK_GHZ", 0.0);
    /* The condition is true for half the values at 128 and for 13 of 256
     * at 243, about 5 percent. The compiled branch skips the add, so it is
     * taken when the condition is false; either way the mispredict rate on
     * random data is the minority fraction, and both are unpredictable. */
    const uint32_t thresholds[2] = {128, 243};
    const kernel_t kernels[3] = {
        {"branchy", sum_branchy},
        {"branchless", sum_branchless},
        {"vector", sum_vector},
    };
    const char *orders[2] = {"unsorted", "sorted"};

    /* The brief wants at least 10 reps behind any reported number; the
     * smoke test may use fewer. */
    const int min_reps = quick ? 3 : 10;
    if (reps < min_reps) {
        fprintf(stderr, "REPS must be at least %d%s\n", min_reps, quick ? " under QUICK=1" : "");
        return 1;
    }

    uint32_t *unsorted = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *sorted = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!unsorted || !sorted) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    /* Fill from the top byte of the generator, then build a histogram so
     * the reference sums come from a different computation than the
     * kernels under test. */
    uint64_t hist[256] = {0};
    uint64_t state = 0x0123456789ABCDEFull;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(splitmix64(&state) >> 56);
        unsorted[i] = v;
        hist[v]++;
    }
    memcpy(sorted, unsorted, n * sizeof(uint32_t));
    qsort(sorted, n, sizeof(uint32_t), cmp_u32);

    uint64_t ref[2] = {0, 0};
    double cond_true[2] = {0, 0};
    for (int ti = 0; ti < 2; ti++) {
        uint64_t count = 0;
        for (uint32_t v = thresholds[ti]; v < 256; v++) {
            ref[ti] += (uint64_t)v * hist[v];
            count += hist[v];
        }
        cond_true[ti] = (double)count / (double)n;
    }

    printf("elements %zu  bytes per array %zu  reps %d (plus 1 warmup, discarded)  clock %.2f GHz\n",
           n, n * sizeof(uint32_t), reps, ghz);
    for (int ti = 0; ti < 2; ti++) {
        printf("threshold %u: condition true for fraction %.4f  reference sum %" PRIu64 "\n",
               thresholds[ti], cond_true[ti], ref[ti]);
        printf("RESULT t%u_cond_true %.4f fraction\n", thresholds[ti], cond_true[ti]);
        printf("RESULT t%u_checksum %" PRIu64 " sum\n", thresholds[ti], ref[ti]);
    }
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);

    /* Variant v = ti * 6 + oi * 3 + ki. Samples are taken round-robin, one
     * pass of every variant per rep, so machine noise that drifts over the
     * run lands on every variant alike instead of on whichever ran last. */
    const int nvariants = 2 * 2 * 3;
    double *samples = (double *)malloc(sizeof(double) * (size_t)nvariants * (size_t)reps);
    int mismatches = 0;
    for (int r = -1; r < reps; r++) {
        for (int v = 0; v < nvariants; v++) {
            int ti = v / 6, oi = (v / 3) % 2, ki = v % 3;
            const uint32_t *a = oi ? sorted : unsorted;
            uint64_t t0 = now_ns();
            uint64_t s = kernels[ki].fn(a, n, thresholds[ti]);
            uint64_t t1 = now_ns();
            SINK(s);
            if (s != ref[ti]) {
                mismatches++;
                fprintf(stderr, "MISMATCH %s %s t%u: got %" PRIu64 " want %" PRIu64 "\n",
                        orders[oi], kernels[ki].kernel_name, thresholds[ti], s, ref[ti]);
            }
            if (r >= 0) samples[v * reps + r] = (double)(t1 - t0) / (double)n;
        }
    }

    for (int v = 0; v < nvariants; v++) {
        int ti = v / 6, oi = (v / 3) % 2, ki = v % 3;
        char label[64], key[64];
        snprintf(label, sizeof label, "t%u %s %s", thresholds[ti], orders[oi], kernels[ki].kernel_name);
        snprintf(key, sizeof key, "t%u_%s_%s", thresholds[ti], orders[oi], kernels[ki].kernel_name);
        stats_t s = stats(samples + v * reps, reps);
        print_row(label, s, "ns/elem");
        printf("RESULT %s_median %.4f ns/elem\n", key, s.median);
        printf("RESULT %s_min %.4f ns/elem\n", key, s.min);
        printf("RESULT %s_cv %.2f percent\n", key, 100.0 * s.cv);
        if (ghz > 0) printf("RESULT %s_cycles %.3f cycles/elem\n", key, s.median * ghz);
    }

    free(samples);
    free(sorted);
    free(unsorted);
    if (mismatches) {
        printf("FAIL: %d checksum mismatches\n", mismatches);
        return 1;
    }
    printf("checksums: every variant matched its reference sum\n");
    return 0;
}
