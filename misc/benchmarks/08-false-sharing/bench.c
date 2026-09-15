/* 09-false-sharing: private counters on one cache line against padded ones.
 *
 * T threads each increment their own 64-bit counter ITERS times. Nothing
 * varies but where the counters sit and how the increment reaches memory:
 *
 *   adjacent  8-byte stride: every counter in the first 64 bytes of one line
 *   pad64     64-byte stride: two counters per 128-byte line, one per 64-byte
 *             half, which is the x86 line size a reader may pad to
 *   pad128    128-byte stride: one counter per line at the size this
 *             machine reports (sysctl hw.cachelinesize)
 *
 *   store     the count lives in a register and is stored to the slot after
 *             every increment (str, add per iteration through a volatile
 *             pointer), so the line sees one plain store per increment and
 *             the core is free to run ahead of the store
 *   atomic    a relaxed atomic fetch-add (one ldadd on this machine), which
 *             the core must complete on a line it holds in a writable state
 *             before the next one issues
 *   register  the count lives in a register and is stored once at the end;
 *             an add chain at one cycle per increment, the bound
 *
 * A load-add-store increment was tried first and dropped: its single-thread
 * time swung several-fold between otherwise identical runs, which is the
 * core's store-to-load forwarding predictor and not the subject here.
 *
 * Wall time runs from the moment every thread has left a spin barrier until
 * the last one has been joined, so thread creation is outside the timed
 * region. For each thread count the variants run round-robin, one discarded
 * warmup round then REPS timed rounds, so noise that drifts over the run
 * lands on every variant alike. ns per increment per thread is a
 * throughput-like quantity, so the median is reported with min and cv
 * beside it. After every run the slots are summed and checked against
 * T * ITERS; a mismatch fails the run.
 *
 * Each thread also records the cpu it ran on, because on this part the
 * cost of sharing a line depends on whether the two cores share an L2
 * cluster, and macOS decides that, not the program. The per-round times
 * and cpu ids go to stdout as "samples" and "cpus" lines so raw.txt can
 * explain the spread of a cell.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sched.h>
#endif

/* The pad128 stride and the buffer alignment. A compile-time constant so
 * the layouts are fixed at build time; main also asks the OS for its line
 * size at run time and prints both, so a build on a 64-byte-line machine
 * shows the mismatch instead of a sysctl figure nobody read. */
#define LINE 128
#define MAX_T 8
/* One guard line each side so nothing else the program touches can share a
 * line with a counter. */
#define BUF_LINES (MAX_T + 2)

/* Sense-reversing spin barrier. macOS has no pthread_barrier_t, and a
 * condition variable would put a scheduler wakeup inside the timed region.
 * The two words sit on separate lines so the arrivals do not slow the
 * waiters' spin. */
typedef struct {
    _Alignas(LINE) atomic_int count;
    _Alignas(LINE) atomic_int generation;
    int total;
} barrier_t;

static void barrier_wait(barrier_t *b) {
    int gen = atomic_load_explicit(&b->generation, memory_order_acquire);
    if (atomic_fetch_add_explicit(&b->count, 1, memory_order_acq_rel) + 1 == b->total) {
        atomic_store_explicit(&b->count, 0, memory_order_relaxed);
        atomic_store_explicit(&b->generation, gen + 1, memory_order_release);
    } else {
        while (atomic_load_explicit(&b->generation, memory_order_acquire) == gen) {
        }
    }
}

/* Which cpu the calling thread is on, or -1 where the platform cannot say.
 * macOS has no affinity API, so this is the only way to see whether the
 * scheduler put two threads in the same L2 cluster. Cheap, and called
 * outside the increment loop. */
static int current_cpu(void) {
#if defined(__APPLE__)
    size_t n = 0;
    return pthread_cpu_number_np(&n) == 0 ? (int)n : -1;
#elif defined(__linux__)
    return sched_getcpu();
#else
    return -1;
#endif
}

/* The cache line size the OS reports, or 0 where the platform cannot say.
 * Printed beside the compiled-in LINE so the two can be compared. */
static long reported_line_size(void) {
#if defined(__APPLE__)
    int64_t n = 0;
    size_t len = sizeof n;
    return sysctlbyname("hw.cachelinesize", &n, &len, NULL, 0) == 0 ? (long)n : 0;
#elif defined(__linux__) && defined(_SC_LEVEL1_DCACHE_LINESIZE)
    long n = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    return n > 0 ? n : 0;
#else
    return 0;
#endif
}

typedef struct {
    uint64_t *slot; /* this thread's counter, or where the register variant stores its result */
    uint64_t iters;
    barrier_t *bar;
    int cpu_start, cpu_end; /* where the thread ran when the loop began and ended */
} worker_t;

/* store: volatile makes the compiler emit every store; without it the loop
 * folds to one store of ITERS, which is the register variant. */
static void *worker_store(void *arg) {
    worker_t *w = (worker_t *)arg;
    volatile uint64_t *p = w->slot;
    uint64_t n = w->iters;
    uint64_t x = 0;
    barrier_wait(w->bar);
    w->cpu_start = current_cpu();
    for (uint64_t i = 0; i < n; i++) *p = ++x;
    w->cpu_end = current_cpu();
    return NULL;
}

/* atomic: one read-modify-write per increment. The builtin avoids punning
 * the shared buffer to _Atomic. */
static void *worker_atomic(void *arg) {
    worker_t *w = (worker_t *)arg;
    uint64_t *p = w->slot;
    uint64_t n = w->iters;
    barrier_wait(w->bar);
    w->cpu_start = current_cpu();
    for (uint64_t i = 0; i < n; i++) __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
    w->cpu_end = current_cpu();
    return NULL;
}

/* register: the empty asm keeps one add per iteration; without it the loop
 * folds to x = n. */
static void *worker_reg(void *arg) {
    worker_t *w = (worker_t *)arg;
    uint64_t n = w->iters;
    uint64_t x = 0;
    barrier_wait(w->bar);
    w->cpu_start = current_cpu();
    for (uint64_t i = 0; i < n; i++) {
        x += 1;
        __asm__ volatile("" : "+r"(x));
    }
    w->cpu_end = current_cpu();
    *w->slot = x;
    return NULL;
}

typedef struct {
    const char *name;
    size_t stride; /* bytes between consecutive threads' counters */
    void *(*fn)(void *);
} variant_t;

static const variant_t variants[] = {
    {"store_adjacent", 8, worker_store},
    {"store_pad64", 64, worker_store},
    {"store_pad128", LINE, worker_store},
    {"atomic_adjacent", 8, worker_atomic},
    {"atomic_pad64", 64, worker_atomic},
    {"atomic_pad128", LINE, worker_atomic},
    {"register", LINE, worker_reg},
};
#define NV ((int)(sizeof variants / sizeof variants[0]))

static _Alignas(LINE) uint64_t buf[BUF_LINES * LINE / sizeof(uint64_t)];

/* One timed run: zero the slots, start T threads, wait at the barrier with
 * them, time until join, check the sum. Returns wall nanoseconds and fills
 * cpus[t] with the cpu each thread ended on, negated if it moved during
 * the loop. */
static double run_once(const variant_t *V, int T, uint64_t iters, int *ok, int *cpus) {
    pthread_t th[MAX_T];
    worker_t w[MAX_T];
    barrier_t bar;
    char *base = (char *)buf + LINE; /* skip the leading guard line */
    atomic_init(&bar.count, 0);
    atomic_init(&bar.generation, 0);
    bar.total = T + 1;
    memset(buf, 0, sizeof buf);
    for (int t = 0; t < T; t++) {
        w[t].slot = (uint64_t *)(base + (size_t)t * V->stride);
        w[t].iters = iters;
        w[t].bar = &bar;
        if (pthread_create(&th[t], NULL, V->fn, &w[t]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    barrier_wait(&bar);
    uint64_t t0 = now_ns();
    for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
    uint64_t t1 = now_ns();
    uint64_t sum = 0;
    for (int t = 0; t < T; t++) {
        sum += *w[t].slot;
        cpus[t] = (w[t].cpu_start == w[t].cpu_end) ? w[t].cpu_end : -w[t].cpu_end;
    }
    SINK(sum);
    if (sum != (uint64_t)T * iters) {
        *ok = 0;
        fprintf(stderr, "MISMATCH %s t%d: slots sum to %llu, want %llu\n", V->name, T,
                (unsigned long long)sum, (unsigned long long)((uint64_t)T * iters));
    }
    return (double)(t1 - t0);
}

static double env_double(const char *name, double dflt) {
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : dflt;
}

int main(void) {
    const int quick = env_int("QUICK", 0);
    const int reps = env_int("REPS", quick ? 5 : 11);
    /* 1<<24 keeps the full run near one minute: the 8-thread adjacent
     * atomic cell costs over 100 ns per increment. ITERS_LOG2=27 is the
     * long form. Even the fastest cell (register, about a quarter of a
     * nanosecond per increment) then lasts milliseconds, far above the
     * clock resolution and the join overhead. */
    const int log2iters = env_int("ITERS_LOG2", quick ? 20 : 24);
    const uint64_t iters = 1ull << log2iters;
    /* run.sh passes the clock_estimate results so cycles can be derived:
     * CLOCK_GHZ from one copy on an otherwise idle machine, and
     * CLOCK_GHZ_T<n> from n concurrent copies, because the all-core clock
     * sits below the single-core one under DVFS and a cycle count for a
     * T-thread row has to use the clock those T cores actually ran at. */
    const double ghz = env_double("CLOCK_GHZ", 0.0);
    const long line_reported = reported_line_size();
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    static const int threads[] = {1, 2, 4, 8};
    const int nthreads = (int)(sizeof threads / sizeof threads[0]);

    if (reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }
    if (ncpu < 1) ncpu = 1;

    printf("increments per thread %llu (1<<%d)  reps %d (plus 1 warmup round, discarded)  "
           "online cpus %ld  pad128 stride %d bytes  line reported by the OS %ld bytes  "
           "single-core clock %.2f GHz\n",
           (unsigned long long)iters, log2iters, reps, ncpu, LINE, line_reported, ghz);
    printf("layouts: adjacent (8-byte stride, all counters in one 64-byte span), "
           "pad64 (64-byte stride, two per 128-byte line), pad128 (128-byte stride, one per line)\n");
    printf("increments: store (str per increment, count in a register), "
           "atomic (relaxed fetch-add), register (one str at the end)\n");
    printf("RESULT iters %llu increments\n", (unsigned long long)iters);
    printf("RESULT reps %d runs\n", reps);
    printf("RESULT line_compiled %d bytes\n", LINE);
    printf("RESULT line_reported %ld bytes\n", line_reported);
    if (line_reported > 0 && line_reported != LINE)
        printf("note: the OS reports a %ld-byte line but pad128 is compiled for %d bytes\n",
               line_reported, LINE);
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);

    double *samples = (double *)malloc(sizeof(double) * (size_t)reps * NV);
    double *col = (double *)malloc(sizeof(double) * (size_t)reps);
    /* cpus[(v * reps + r) * MAX_T + t]: where thread t of variant v ended round r */
    int *cpus = (int *)malloc(sizeof(int) * (size_t)reps * NV * MAX_T);
    if (!samples || !col || !cpus) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    int all_ok = 1;
    for (int ti = 0; ti < nthreads; ti++) {
        const int T = threads[ti];
        int ok[NV];
        if (T > ncpu) {
            printf("skipping %d threads: only %ld online cpus\n", T, ncpu);
            continue;
        }
        /* The clock for this thread count: T concurrent copies of
         * clock_estimate, falling back to the single-core figure. */
        char clockvar[32];
        snprintf(clockvar, sizeof clockvar, "CLOCK_GHZ_T%d", T);
        const double ghz_t = env_double(clockvar, ghz);
        if (ghz_t > 0) printf("RESULT clock_t%d %.2f GHz\n", T, ghz_t);
        for (int v = 0; v < NV; v++) ok[v] = 1;
        /* Round r = -1 is the warmup: every variant runs once and the time
         * is dropped, so page faults on the buffer, the first thread
         * creations and the clock ramp fall outside the samples. */
        for (int r = -1; r < reps; r++) {
            for (int v = 0; v < NV; v++) {
                int scratch[MAX_T];
                int *where = (r >= 0) ? cpus + ((size_t)v * reps + r) * MAX_T : scratch;
                double ns = run_once(&variants[v], T, iters, &ok[v], where) / (double)iters;
                if (r >= 0) samples[v * reps + r] = ns;
            }
        }
        for (int v = 0; v < NV; v++) {
            const uint64_t want = (uint64_t)T * iters;
            memcpy(col, samples + v * reps, sizeof(double) * (size_t)reps);
            stats_t s = stats(col, reps);
            char label[64];
            snprintf(label, sizeof label, "%s t%d", variants[v].name, T);
            print_row(label, s, "ns/increment/thread");
            printf("RESULT %s_t%d_median %.4f ns\n", variants[v].name, T, s.median);
            printf("RESULT %s_t%d_min %.4f ns\n", variants[v].name, T, s.min);
            printf("RESULT %s_t%d_cv %.1f percent\n", variants[v].name, T, 100.0 * s.cv);
            if (ghz_t > 0)
                printf("RESULT %s_t%d_cycles %.3f cycles\n", variants[v].name, T, s.median * ghz_t);
            printf("RESULT %s_t%d_checksum %llu %s\n", variants[v].name, T,
                   (unsigned long long)want, ok[v] ? "ok" : "MISMATCH");
            if (!ok[v]) all_ok = 0;
        }
        /* Per-round samples and placement, so a slow round can be matched
         * with where its threads ran. samples: ns per increment per thread
         * for each timed round in order. cpus: the cpu id each thread ended
         * on, in thread order, a trailing * if it moved during the loop.
         * Not RESULT lines; they explain the spread of a cell. */
        for (int v = 0; v < NV; v++) {
            printf("samples %s t%d:", variants[v].name, T);
            for (int r = 0; r < reps; r++) printf(" %.4f", samples[v * reps + r]);
            printf("\n");
            printf("cpus %s t%d:", variants[v].name, T);
            for (int r = 0; r < reps; r++) {
                printf(" [");
                for (int t = 0; t < T; t++) {
                    int c = cpus[((size_t)v * reps + r) * MAX_T + t];
                    printf("%s%d%s", t ? " " : "", c < 0 ? -c : c, c < 0 ? "*" : "");
                }
                printf("]");
            }
            printf("\n");
        }
    }
    free(cpus);
    free(col);
    free(samples);
    if (!all_ok) {
        printf("FAIL: at least one run did not sum to threads * increments\n");
        return 1;
    }
    printf("checksums: every run summed to threads * increments\n");
    return 0;
}
