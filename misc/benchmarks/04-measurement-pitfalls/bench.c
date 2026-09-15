/* 05-measurement-pitfalls: two ways one microbenchmark can lie.
 *
 * The loop under test is a scalar float sum over 1<<20 floats, 4 MiB,
 * which fits the P-core L2 and not the L1d. Everything below times that
 * one loop, one full pass per timed region.
 *
 * Part 1 times it at -O2 with the result (a) thrown away, (b) handed to
 * SINK() and (c) stored for a printed checksum. In (a) the loop is not in
 * the binary at all: build.sh writes bench_O2.s and the README quotes it.
 *
 * Part 2 times variant (b) RUNS times in a row inside this process and,
 * through the loop in run.sh, once inside each of RUNS fresh processes.
 * Every pass is printed as a SAMPLE line so run.sh can report, besides
 * min, median, max and cv, how often two single runs of the same code
 * differ by more than a few percent.
 *
 * Part 3 is the worked example of one shot against thirty: the first pass
 * a process makes over its freshly mapped pages happens once per process,
 * so the in-process run can only ever report one number for it, while the
 * RUNS fresh processes give the distribution that number came from. The
 * pass is slower than a warm one because the page faults land inside the
 * timed region; that mechanism is 10-first-touch's, not this benchmark's.
 *
 * Every process takes its cold pass first, then fills the array and
 * repeats the loop for SETTLE_MS milliseconds so that DVFS and core
 * placement have settled before anything else is measured.
 *
 *   ./bench          the in-process parts, plus the one cold pass
 *   ./bench fresh    one cold pass and one warm pass, printed as SAMPLE
 *                    lines for run.sh to aggregate over RUNS processes
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime, CLOCK_MONOTONIC_RAW, MAP_ANONYMOUS under -std=c11 on glibc */
#endif
#define _DARWIN_C_SOURCE 1 /* MAP_ANON under -std=c11 on macOS */
#include "../common/timing.h"
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define NOINLINE __attribute__((noinline))

/* The loop under test. Without -ffast-math the compiler may not reassociate
 * a float reduction, so this is one dependent fadd per element: latency
 * bound, and short enough in the assembly to read in full. It is inlined
 * into each timing function below, which is what lets -O2 delete it in
 * the one place its result is dead. */
static float sum_f32(const float *a, size_t n) {
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += a[i];
    return s;
}

/* (a) The naive benchmark. The sum is computed and never looked at, so once
 * sum_f32 is inlined its loop has no observable effect and -O2 removes it.
 * The two clock reads are the only thing left between t0 and t1. */
NOINLINE static double time_discarded(const float *a, size_t n) {
    uint64_t t0 = now_ns();
    float s = sum_f32(a, n);
    uint64_t t1 = now_ns();
    (void)s;
    return (double)(t1 - t0);
}

/* (b) SINK() is an empty asm statement that claims to read s. The compiler
 * has to have s in a register or in memory before it, so the loop stays.
 * The asm is inside the timed region, which is where a barrier belongs:
 * after it the value is dead again and nothing else is kept alive. */
NOINLINE static double time_sink(const float *a, size_t n) {
    uint64_t t0 = now_ns();
    float s = sum_f32(a, n);
    SINK(s);
    uint64_t t1 = now_ns();
    return (double)(t1 - t0);
}

/* (c) The sum is stored where the caller can read it. main adds every
 * result into a checksum and prints it against the expected value, so the
 * store, and the loop that feeds it, cannot be removed. */
NOINLINE static double time_checksum(const float *a, size_t n, float *out) {
    uint64_t t0 = now_ns();
    *out = sum_f32(a, n);
    uint64_t t1 = now_ns();
    return (double)(t1 - t0);
}

/* Pages come from mmap rather than malloc so that nothing, not even the
 * allocator's own bookkeeping, touches them before the cold pass. */
static float *map_floats(size_t n) {
    void *p = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return (float *)p;
}

/* (i & 7) * 0.25 keeps every partial sum an exact multiple of 0.25 below
 * 2^22, so the float sum is exact and the checksum is a fixed number. */
static void fill(float *a, size_t n) {
    for (size_t i = 0; i < n; i++) a[i] = (float)(i & 7) * 0.25f;
}

/* Repeat the loop for about ms milliseconds and discard every result. */
static void settle(const float *a, size_t n, int ms) {
    uint64_t end = now_ns() + (uint64_t)ms * 1000000ull;
    while (now_ns() < end) time_sink(a, n);
}

/* The clock's own step: the smallest non-zero difference between two
 * consecutive readings. Any interval shorter than this reads as 0 or as
 * one step, which is what the discarded variant shows. */
static double clock_tick_ns(void) {
    double tick = 1e18;
    for (int i = 0; i < 10000; i++) {
        uint64_t t0 = now_ns(), t1 = now_ns();
        while (t1 == t0) t1 = now_ns();
        if ((double)(t1 - t0) < tick) tick = (double)(t1 - t0);
    }
    return tick;
}

static void result_stats(const char *key, stats_t s) {
    printf("RESULT %s_min_ns %.2f ns\n", key, s.min);
    printf("RESULT %s_median_ns %.2f ns\n", key, s.median);
    printf("RESULT %s_max_ns %.2f ns\n", key, s.max);
    printf("RESULT %s_cv_pct %.2f percent\n", key, 100.0 * s.cv);
}

int main(int argc, char **argv) {
    const int quick = env_int("QUICK", 0);
    const int reps = env_int("REPS", quick ? 10 : 30);
    const int runs = env_int("RUNS", quick ? 10 : 30);
    const size_t n = quick ? ((size_t)1 << 17) : ((size_t)1 << 20);
    const int settle_ms = env_int("SETTLE_MS", quick ? 20 : 100);
    const int fresh = argc > 1 && strcmp(argv[1], "fresh") == 0;
    const double expected = (double)(n / 8) * 7.0;
    const long page = sysconf(_SC_PAGESIZE);

    if (reps < 10 || runs < 10) {
        fprintf(stderr, "REPS and RUNS must be at least 10\n");
        return 1;
    }

    float *a = map_floats(n);

    /* Part 3, cold. The very first pass over the mapping: nothing has
     * touched these pages, so each one is faulted in and zero-filled inside
     * the timed region. A process gets exactly one such pass, which is why
     * it serves as the worked example: this process reports it as a single
     * shot, and run.sh repeats it across fresh processes for the
     * distribution the single shot is one sample of. */
    double cold = time_sink(a, n);
    fill(a, n);
    settle(a, n, settle_ms);

    if (fresh) {
        time_sink(a, n); /* warmup, discarded */
        double warm = time_sink(a, n);
        printf("fresh process: cold first-touch pass %.0f ns, warm pass %.0f ns\n", cold, warm);
        printf("SAMPLE p3_cold %.0f ns\n", cold);
        printf("SAMPLE p2_fresh %.0f ns\n", warm);
        munmap(a, n * sizeof(float));
        return 0;
    }

    printf("floats %zu  bytes %zu  page %ld B  pages %zu  reps %d (plus 1 warmup, discarded)  "
           "runs %d  settle %d ms  one thread, default QoS\n",
           n, n * sizeof(float), page, n * sizeof(float) / (size_t)page, reps, runs, settle_ms);
    printf("PARAM floats %zu\nPARAM bytes %zu\nPARAM page_bytes %ld\nPARAM pages %zu\n"
           "PARAM reps %d\nPARAM runs %d\nPARAM settle_ms %d\n",
           n, n * sizeof(float), page, n * sizeof(float) / (size_t)page, reps, runs, settle_ms);

    double tick = clock_tick_ns();
    printf("clock tick %.0f ns (smallest non-zero step of now_ns())\n", tick);
    printf("RESULT clock_tick_ns %.0f ns\n", tick);

    int nmax = reps > runs ? reps : runs;
    double *ta = (double *)malloc(sizeof(double) * (size_t)nmax);
    double *tb = (double *)malloc(sizeof(double) * (size_t)nmax);
    double *tc = (double *)malloc(sizeof(double) * (size_t)nmax);
    if (!ta || !tb || !tc) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    /* Part 1: the same loop, three ways of using its result. One warmup
     * pass of each is discarded, then the three are taken round-robin so
     * that noise drifting over the run lands on every variant alike. */
    float s = 0.0f;
    double check = 0.0;
    time_discarded(a, n);
    time_sink(a, n);
    time_checksum(a, n, &s);
    for (int r = 0; r < reps; r++) {
        ta[r] = time_discarded(a, n);
        tb[r] = time_sink(a, n);
        tc[r] = time_checksum(a, n, &s);
        check += s;
    }
    stats_t sa = stats(ta, reps), sb = stats(tb, reps), sc = stats(tc, reps);
    print_row("p1 (a) result discarded", sa, "ns/pass");
    print_row("p1 (b) result passed to SINK()", sb, "ns/pass");
    print_row("p1 (c) result stored for the checksum", sc, "ns/pass");
    result_stats("p1_discarded", sa);
    result_stats("p1_sink", sb);
    result_stats("p1_checksum", sc);
    int ok = check == expected * reps;
    printf("checksum %.1f over %d passes, expected %.1f (%.1f per pass): %s\n",
           check, reps, expected * reps, expected, ok ? "ok" : "MISMATCH");
    printf("RESULT p1_checksum_value %.1f sum\n", check);
    if (!ok) {
        printf("FAIL: checksum mismatch\n");
        return 2;
    }

    /* Part 2, in-process: RUNS consecutive timed passes of variant (b),
     * the counterpart of the RUNS fresh processes that run.sh launches.
     * Each pass is also printed as a SAMPLE line so that run.sh can count,
     * for this series as for the fresh processes, how many pairs of single
     * passes differ by more than a given percentage. */
    time_sink(a, n); /* warmup, discarded */
    for (int r = 0; r < runs; r++) tb[r] = time_sink(a, n);
    stats_t s2 = stats(tb, runs);
    print_row("p2 in-process, consecutive passes", s2, "ns/pass");
    result_stats("p2_inproc", s2);
    for (int r = 0; r < runs; r++) printf("SAMPLE p2_inproc %.0f ns\n", tb[r]);

    /* Part 3, the single cold pass taken at the top of main: the one number
     * an in-process run can give for a pass that happens once per process. */
    printf("p3 cold, first pass over untouched pages (single shot)  %.0f ns/pass\n", cold);
    printf("RESULT p3_cold_single_ns %.0f ns\n", cold);

    free(ta);
    free(tb);
    free(tc);
    munmap(a, n * sizeof(float));
    return 0;
}
