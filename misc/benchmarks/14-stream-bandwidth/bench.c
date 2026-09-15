/* 15-stream-bandwidth: what one thread, then more, get from memory against
 * the package figure the vendor states, and what the same loop reports
 * when its data never leaves the caches.
 *
 * The STREAM triad a[i] = b[i] + s * c[i] runs over three float64 arrays.
 * Two working sets, each swept over a thread count:
 *
 *   512 MiB per array (QUICK: 64 MiB): 32 times the 16 MiB L2 of one
 *   cluster and 14 times the 36 MiB sum of every L2 on the part, so each
 *   pass streams from DRAM. Run with 1, 2, 4, 8, 10 and 14 threads, the
 *   arrays split into one contiguous partition per thread.
 *
 *   1 MiB per array per thread: thread t runs the same loop over the t-th
 *   MiB of each array, 3 MiB per thread, which fits the L2 of the cluster
 *   the thread is on and not its 128 KiB L1d. Run with 1, 2, 4, 8 and 10
 *   threads, so that five threads per cluster hold 15 MiB in a 16 MiB L2;
 *   14 is left out because four threads would put 12 MiB in the 4 MiB L2
 *   of the efficiency cluster. Each thread repeats its slice for as many
 *   passes as move the bytes of one pass over a large array, so a thread
 *   does the same work per round in both cases. The single-thread cache
 *   configuration runs again at the end of the sweep, because its figure
 *   follows the core and cluster clock rather than the memory side and
 *   moves from run to run; the repeat shows how far within one run.
 *
 * Threads take a static contiguous partition each, cut on a cache-line
 * boundary. The main thread is worker 0, so a T-thread run has exactly T
 * runnable threads and nothing spins on a fifteenth core. Every thread
 * fills its own partition before the first barrier, so the page faults
 * happen on the thread that will stream the pages; any fault or reclaim
 * that still lands after that is absorbed by the discarded warmup rounds.
 * A round is: spin barrier, triad passes, spin barrier. Every thread reads
 * the clock after leaving the first barrier and again before arriving at
 * the second, and the round time is the latest end minus the earliest
 * start over all threads: the span in which the work was done, which no
 * thread being descheduled can shorten. The same stamps give each thread's
 * own time, so the fastest and slowest partition can be reported, and a
 * round whose span exceeds the slowest thread's own time by more than
 * LATE_FRACTION is counted as one in which some thread was not running
 * when the others were: the tell that separates a thread on a slow core
 * (span equals its own time) from a thread the scheduler held back.
 *
 * Warmup is by time and count: rounds are discarded until at least
 * WARMUP_MS (default 100) have run and at least WARMUP_MIN_ROUNDS have
 * been discarded, because a core that was idle takes tens of milliseconds
 * to reach its working clock, a single short round would be timed inside
 * that ramp, and a single long round (pages being faulted or reclaimed)
 * would satisfy the time on its own without the loop having run at speed.
 * Then REPS rounds are timed. The main thread announces each round's
 * phase (warmup, timed, stop) through a shared word before the start
 * barrier, so no thread needs to know the count in advance.
 *
 * Bytes are counted the STREAM way: 24 per element, the two reads and one
 * write the loop asks for, not whatever the cache moves for the store.
 * GB/s is decimal (bytes per nanosecond), the unit the vendor uses.
 * Bandwidth is throughput-like, so the median is reported with cv; the
 * best round, which is what STREAM reports, is shown beside it.
 *
 * Every value in b and c is a small integer, so b + s*c is exact whether
 * or not the compiler fuses the multiply and add, and the sum of a is an
 * exact integer in a double however it is ordered. a is reset before each
 * configuration and its sum is checked against an integer reference after,
 * so a configuration that did not write a fails the run.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define NOINLINE __attribute__((noinline))
#define LINE 128 /* what this machine reports: sysctl hw.cachelinesize */
#define LINE_ELEMS (LINE / sizeof(double))
#define MAX_T 64
#define BYTES_PER_ELEM 24 /* STREAM triad: read b, read c, write a */
#define WARMUP_MIN_ROUNDS 3
#define LATE_FRACTION 0.2 /* span more than 20 percent over the slowest thread's own time */

enum { PHASE_WARMUP, PHASE_TIMED, PHASE_STOP };

/* Sense-reversing spin barrier. macOS has no pthread_barrier_t, and a
 * condition variable would put a scheduler wakeup inside the timed round.
 * The words sit on separate lines so arrivals do not slow the spin. The
 * phase word is written by the main thread before it arrives, and the
 * acquire and release pairs in the barrier carry it to every thread. */
typedef struct {
    _Alignas(LINE) atomic_int count;
    _Alignas(LINE) atomic_int generation;
    _Alignas(LINE) atomic_int phase;
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

/* The kernel under test. restrict says the three arrays do not overlap,
 * so the compiler need not version the loop behind a runtime overlap
 * check; at -O2 clang emits a NEON loop of ldp, fmla and stp. noinline
 * keeps it one loop in one place in the assembly, and s comes from the
 * environment so nothing about the multiply can be folded away. */
NOINLINE static void triad(double *restrict a, const double *restrict b,
                           const double *restrict c, size_t n, double s) {
    for (size_t i = 0; i < n; i++) a[i] = b[i] + s * c[i];
}

typedef struct {
    double *a, *b, *c;
    size_t lo, hi;  /* this thread's partition, [lo, hi) */
    int passes;     /* triad passes per round */
    double s;
    barrier_t *bar;
    uint64_t *t0;   /* this thread's clock after the start barrier, per timed round */
    uint64_t *t1;   /* and before the end barrier; REPS entries each */
} worker_t;

/* First touch of this thread's partition, and the reset of a. */
static void fill(const worker_t *w) {
    for (size_t i = w->lo; i < w->hi; i++) {
        w->a[i] = -1.0;
        w->b[i] = (double)(i & 1023);
        w->c[i] = (double)((i >> 3) & 1023);
    }
}

static void passes(const worker_t *w) {
    const size_t n = w->hi - w->lo;
    for (int p = 0; p < w->passes; p++)
        triad(w->a + w->lo, w->b + w->lo, w->c + w->lo, n, w->s);
}

static void *worker(void *arg) {
    const worker_t *w = (const worker_t *)arg;
    int timed = 0;
    fill(w);
    for (;;) {
        barrier_wait(w->bar);
        int phase = atomic_load_explicit(&w->bar->phase, memory_order_acquire);
        if (phase == PHASE_STOP) break;
        uint64_t t0 = now_ns();
        passes(w);
        uint64_t t1 = now_ns();
        if (phase == PHASE_TIMED) {
            w->t0[timed] = t0;
            w->t1[timed] = t1;
            timed++;
        }
        barrier_wait(w->bar);
    }
    return NULL;
}

/* The main thread's copy of the worker loop: it is worker 0 and decides
 * the phase of every round. Returns the number of warmup rounds. */
static int main_rounds(const worker_t *w, int reps, double warm_ns) {
    int timed = 0, warm = 0;
    uint64_t warm_start = now_ns();
    for (;;) {
        int phase;
        if (timed == reps)
            phase = PHASE_STOP;
        else if (warm >= WARMUP_MIN_ROUNDS && (double)(now_ns() - warm_start) >= warm_ns)
            phase = PHASE_TIMED;
        else
            phase = PHASE_WARMUP;
        atomic_store_explicit(&w->bar->phase, phase, memory_order_release);
        barrier_wait(w->bar);
        if (phase == PHASE_STOP) break;
        uint64_t t0 = now_ns();
        passes(w);
        uint64_t t1 = now_ns();
        if (phase == PHASE_TIMED) {
            w->t0[timed] = t0;
            w->t1[timed] = t1;
            timed++;
        } else {
            warm++;
        }
        barrier_wait(w->bar);
    }
    return warm;
}

/* The integer reference for the sum of a over [0, n) after the triad. s is
 * integer-valued, so this is the exact value the double sum must equal. */
static uint64_t reference_sum(size_t n, uint64_t s) {
    uint64_t ref = 0;
    for (size_t i = 0; i < n; i++) ref += (i & 1023) + s * ((i >> 3) & 1023);
    return ref;
}

static double env_double(const char *name, double dflt) {
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : dflt;
}

typedef struct {
    double *wall, *fastest, *slowest; /* GB/s per timed round */
    int warmups;
    int late;   /* timed rounds whose span exceeded the slowest thread's own time by LATE_FRACTION */
    int ok;
} config_result_t;

/* One configuration: T threads over n elements, passes per round, warmup
 * rounds until warm_ns have run, then reps timed rounds. Fills res with
 * GB/s per round for the whole run, for the thread that finished first and
 * for the one that finished last, counts the rounds in which some thread
 * was not running while the others were, and checks the sum of a against
 * the integer reference. */
static void run_config(double *a, double *b, double *c, size_t n, int T, int npasses,
                       int reps, double warm_ns, double s, config_result_t *res) {
    pthread_t th[MAX_T];
    worker_t w[MAX_T];
    barrier_t bar;
    /* Partition on a cache-line boundary so no two threads write one line. */
    const size_t chunk = ((n / (size_t)T + LINE_ELEMS - 1) / LINE_ELEMS) * LINE_ELEMS;
    uint64_t *stamps = (uint64_t *)malloc(sizeof(uint64_t) * 2 * (size_t)reps * (size_t)T);
    if (!stamps) {
        fprintf(stderr, "allocation failed\n");
        exit(1);
    }
    atomic_init(&bar.count, 0);
    atomic_init(&bar.generation, 0);
    atomic_init(&bar.phase, PHASE_WARMUP);
    bar.total = T;
    for (int t = 0; t < T; t++) {
        size_t lo = (size_t)t * chunk, hi = lo + chunk;
        w[t].a = a;
        w[t].b = b;
        w[t].c = c;
        w[t].lo = lo < n ? lo : n;
        w[t].hi = hi < n ? hi : n;
        w[t].passes = npasses;
        w[t].s = s;
        w[t].bar = &bar;
        w[t].t0 = stamps + (size_t)(2 * t) * (size_t)reps;
        w[t].t1 = stamps + (size_t)(2 * t + 1) * (size_t)reps;
    }
    for (int t = 1; t < T; t++) {
        if (pthread_create(&th[t], NULL, worker, &w[t]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    fill(&w[0]);
    res->warmups = main_rounds(&w[0], reps, warm_ns);
    for (int t = 1; t < T; t++) pthread_join(th[t], NULL);

    const double bytes = (double)BYTES_PER_ELEM * (double)n * (double)npasses;
    res->late = 0;
    for (int r = 0; r < reps; r++) {
        uint64_t first = w[0].t0[r], last = w[0].t1[r], longest = 0;
        double hi = 0.0, lo = 1e300;
        for (int t = 0; t < T; t++) {
            if (w[t].t0[r] < first) first = w[t].t0[r];
            if (w[t].t1[r] > last) last = w[t].t1[r];
            uint64_t own = w[t].t1[r] - w[t].t0[r];
            if (own > longest) longest = own;
            double tb = (double)BYTES_PER_ELEM * (double)(w[t].hi - w[t].lo) * (double)npasses;
            double gbs = tb / (double)own;
            if (gbs > hi) hi = gbs;
            if (gbs < lo) lo = gbs;
        }
        res->wall[r] = bytes / (double)(last - first); /* bytes per ns = GB/s */
        res->fastest[r] = hi;
        res->slowest[r] = lo;
        /* The span can only exceed the longest own time if some thread's
         * window started or ended outside the others', which on a machine
         * with no idle core means the scheduler held it back. */
        if ((double)(last - first) > (1.0 + LATE_FRACTION) * (double)longest) res->late++;
    }
    free(stamps);

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += a[i];
    SINK(sum);
    const uint64_t ref = reference_sum(n, (uint64_t)s);
    res->ok = sum == (double)ref;
    if (!res->ok)
        fprintf(stderr, "MISMATCH t%d n=%zu: sum of a is %.1f, want %" PRIu64 "\n", T, n, sum, ref);
}

typedef struct {
    int cache;   /* 1: small_mib per array per thread, L2-resident; 0: big_mib per array, streamed */
    int threads;
    int again;   /* a repeat of an earlier configuration, keyed separately */
} config_t;

int main(void) {
    const int quick = env_int("QUICK", 0);
    const int reps = env_int("REPS", quick ? 5 : 21);
    const size_t big_mib = (size_t)env_int("ARRAY_MIB", quick ? 64 : 512);
    const size_t small_mib = 1;
    const double warm_ns = 1e6 * env_double("WARMUP_MS", 100.0);
    const double s = env_double("SCALAR", 3.0);
    /* Apple's stated figure for M4 Pro; see the README for the source. */
    const double vendor = env_double("VENDOR_GBS", 273.0);
    /* run.sh passes the clock_estimate result so the summary can quote it. */
    const double ghz = env_double("CLOCK_GHZ", 0.0);
    static const config_t configs[] = {
        {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {1, 8, 0}, {1, 10, 0},
        {0, 1, 0}, {0, 2, 0}, {0, 4, 0}, {0, 8, 0}, {0, 10, 0}, {0, 14, 0},
        {1, 1, 1},
    };
    const int nconfigs = (int)(sizeof configs / sizeof configs[0]);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    if (reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }
    if (s != (double)(uint64_t)s || s < 1.0 || s > 1024.0) {
        fprintf(stderr, "SCALAR must be an integer in [1, 1024] so the checksum is exact\n");
        return 1;
    }
    if (ncpu < 1) ncpu = 1;

    const size_t n_big = (big_mib << 20) / sizeof(double);
    const size_t n_small = (small_mib << 20) / sizeof(double);
    /* As many passes over a thread's small slice as move the bytes of one
     * pass over a large array, so a thread does the same work per round in
     * both cases and a round lasts about as long whatever the count. */
    const int small_passes = (int)(n_big / n_small);
    const size_t bytes_per_array = n_big * sizeof(double);

    double *a, *b, *c;
    /* Page-aligned so every partition boundary and every array start sits
     * at the same offset within a line and a page. */
    if (posix_memalign((void **)&a, 16384, bytes_per_array) != 0 ||
        posix_memalign((void **)&b, 16384, bytes_per_array) != 0 ||
        posix_memalign((void **)&c, 16384, bytes_per_array) != 0) {
        fprintf(stderr, "allocation of 3 x %zu bytes failed\n", bytes_per_array);
        return 1;
    }

    printf("arrays 3 x %zu bytes (%zu MiB each, %zu float64)  cache case 3 x %zu bytes (%zu MiB each) per thread, %d passes per round  "
           "reps %d timed rounds after at least %.0f ms and %d discarded warmup rounds  online cpus %ld  scalar %.1f  "
           "vendor %.0f GB/s  clock %.2f GHz\n",
           bytes_per_array, big_mib, n_big, n_small * sizeof(double), small_mib, small_passes, reps, warm_ns / 1e6,
           WARMUP_MIN_ROUNDS, ncpu, s, vendor, ghz);
    printf("bytes counted per element: %d (read b, read c, write a); GB/s is decimal, bytes per nanosecond\n",
           BYTES_PER_ELEM);
    printf("RESULT array_bytes %zu bytes\n", bytes_per_array);
    printf("RESULT cache_array_bytes %zu bytes\n", n_small * sizeof(double));
    printf("RESULT cache_passes %d passes\n", small_passes);
    printf("RESULT bytes_per_element %d bytes\n", BYTES_PER_ELEM);
    printf("RESULT reps %d rounds\n", reps);
    printf("RESULT warmup_ms %.0f ms\n", warm_ns / 1e6);
    printf("RESULT warmup_min_rounds %d rounds\n", WARMUP_MIN_ROUNDS);
    printf("RESULT late_percent %.0f percent\n", 100.0 * LATE_FRACTION);
    printf("RESULT online_cpus %ld cpus\n", ncpu);
    printf("RESULT vendor_bandwidth %.0f GB/s\n", vendor);
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);

    config_result_t res;
    res.wall = (double *)malloc(sizeof(double) * (size_t)reps);
    res.fastest = (double *)malloc(sizeof(double) * (size_t)reps);
    res.slowest = (double *)malloc(sizeof(double) * (size_t)reps);
    if (!res.wall || !res.fastest || !res.slowest) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    int all_ok = 1;
    double t1_median[2] = {0.0, 0.0}; /* single-thread median per working set, for the scaling column */
    for (int ci = 0; ci < nconfigs; ci++) {
        const config_t cfg = configs[ci];
        const int T = cfg.threads;
        /* The cache case gives every thread its own small slice, so n grows
         * with the thread count and the partition is exactly one slice. */
        const size_t n = cfg.cache ? n_small * (size_t)T : n_big;
        const int npasses = cfg.cache ? small_passes : 1;
        const size_t mib = cfg.cache ? small_mib : big_mib;
        if (T > ncpu) {
            printf("skipping %d threads: only %ld online cpus\n", T, ncpu);
            continue;
        }
        run_config(a, b, c, n, T, npasses, reps, warm_ns, s, &res);
        stats_t st = stats(res.wall, reps);
        stats_t fast = stats(res.fastest, reps);
        stats_t slow = stats(res.slowest, reps);
        char label[64], key[64];
        snprintf(label, sizeof label, "triad %zu MiB/array%s t%d%s%s", mib, cfg.cache ? "/thread" : "", T,
                 cfg.cache ? " (L2)" : "", cfg.again ? " again" : "");
        snprintf(key, sizeof key, "triad_%zuMiB_t%d%s", mib, T, cfg.again ? "_again" : "");
        print_row(label, st, "GB/s");
        printf("  fastest thread median %.2f GB/s  slowest thread median %.2f GB/s  warmup rounds discarded %d  "
               "rounds with a late thread %d\n",
               fast.median, slow.median, res.warmups, res.late);
        printf("RESULT %s_median %.2f GB/s\n", key, st.median);
        printf("RESULT %s_best %.2f GB/s\n", key, st.max);
        printf("RESULT %s_min %.2f GB/s\n", key, st.min);
        printf("RESULT %s_cv %.1f percent\n", key, 100.0 * st.cv);
        printf("RESULT %s_fastest_thread %.2f GB/s\n", key, fast.median);
        printf("RESULT %s_slowest_thread %.2f GB/s\n", key, slow.median);
        printf("RESULT %s_fraction %.3f of_vendor\n", key, st.median / vendor);
        if (T == 1 && !cfg.again) t1_median[cfg.cache] = st.median;
        if (!cfg.again && t1_median[cfg.cache] > 0)
            printf("RESULT %s_scaling %.2f x_one_thread\n", key, st.median / t1_median[cfg.cache]);
        printf("RESULT %s_warmups %d rounds\n", key, res.warmups);
        printf("RESULT %s_late_rounds %d rounds\n", key, res.late);
        printf("RESULT %s_checksum %" PRIu64 " %s\n", key, reference_sum(n, (uint64_t)s), res.ok ? "ok" : "MISMATCH");
        if (!res.ok) all_ok = 0;
    }

    free(res.slowest);
    free(res.fastest);
    free(res.wall);
    free(c);
    free(b);
    free(a);
    if (!all_ok) {
        printf("FAIL: at least one configuration did not produce the reference sum\n");
        return 1;
    }
    printf("checksums: every configuration produced the reference sum of a\n");
    return 0;
}
