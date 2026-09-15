/* 10-first-touch: allocation is not placement.
 *
 * mmap of anonymous memory returns address space, not memory. The kernel
 * finds a physical page, zeroes it and maps it the first time each page is
 * written, inside a page fault, and on a NUMA kernel that fault is also
 * the moment the page's node is chosen. This program times that first pass
 * over a buffer against the passes that follow it and counts the faults
 * each pass takes, so the cost and the mechanism are shown side by side.
 * It then runs the same touches through malloc and free to show that an
 * allocator which recycles a block also recycles the placement made by
 * whoever touched the block first.
 *
 * Four cycles run per repetition, each on its own fresh mapping:
 *   A  mmap; pass 1 (one byte per page, faults); pass 2 (the same, no
 *      faults); pass 2 repeated; pass 3 (every word, bandwidth); munmap
 *   B  mmap; fill every word of the fresh mapping (bandwidth with the
 *      faults inside); munmap
 *   C  mmap; pass 1 and pass 2 again with every page timed on its own,
 *      to show how the cost is distributed across pages
 *   D  malloc; touch; touch; free, run after the others, to show what
 *      the allocator hands back on the next malloc
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime, MAP_ANONYMOUS and madvise on glibc under -std=c11 */
#endif
#include "../common/timing.h"
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define NOINLINE __attribute__((noinline))

/* A page whose touch takes longer than this did more than a page walk and
 * a store: on this machine a warm page-crossing store is a few ns and a
 * page fault is several hundred, so 200 ns separates the two cleanly. */
#define SLOW_NS 200

/* A whole pass whose average is above this did not run at the page-walk
 * cost: the warm touch is about 4 ns per page here, so 20 ns per page
 * means at least 2.5 % of the pages took a fault-sized stall. Used to
 * count the reps in which a stall recurred. */
#define SLOW_REP_NS_PER_PAGE 20

/* Minor plus major faults of this process. rusage is the one fault
 * counter user space can read on both macOS and Linux without a PMU. */
static long faults(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt + ru.ru_majflt;
}

/* One byte per page. The first time this runs over a fresh mapping every
 * store faults; afterwards each store is a TLB miss and a page walk. The
 * store cannot be removed: p escapes and the value is checked later. */
NOINLINE static void touch(unsigned char *p, size_t pages, size_t page, unsigned char v) {
    for (size_t i = 0; i < pages; i++) p[i * page] = v;
}

/* The same loop with a timestamp round every store, so the cost of each
 * page is known on its own. Only cycle C uses it; the timer calls make it
 * unfit for the headline numbers, and the clock behind now_ns() ticks
 * every 42 ns on this machine, which is fine for telling a fault from a
 * plain store but not for timing the plain store. */
NOINLINE static void touch_timed(unsigned char *p, size_t pages, size_t page, unsigned char v,
                                 uint64_t *ns) {
    for (size_t i = 0; i < pages; i++) {
        uint64_t t0 = now_ns();
        p[i * page] = v;
        uint64_t t1 = now_ns();
        ns[i] = t1 - t0;
    }
}

/* Reads back the byte each touch wrote; the caller checks the sum. */
NOINLINE static uint64_t sum_first_bytes(const unsigned char *p, size_t pages, size_t page) {
    uint64_t s = 0;
    for (size_t i = 0; i < pages; i++) s += p[i * page];
    return s;
}

/* Every word of the buffer, sequentially. The value varies with i so the
 * compiler cannot turn the loop into memset, and the sum of i*k over the
 * buffer is known in closed form, which is what the check uses. */
NOINLINE static void fill(uint64_t *p, size_t words, uint64_t k) {
    for (size_t i = 0; i < words; i++) p[i] = (uint64_t)i * k;
}

NOINLINE static uint64_t sum_words(const uint64_t *p, size_t words) {
    uint64_t s = 0;
    for (size_t i = 0; i < words; i++) s += p[i];
    return s;
}

/* sum of i*k for i in [0, n) modulo 2^64: k times n(n-1)/2. */
static uint64_t expected_fill_sum(uint64_t n, uint64_t k) {
    uint64_t tri = (n % 2 == 0) ? (n / 2) * (n - 1) : n * ((n - 1) / 2);
    return tri * k;
}

static double env_double(const char *name, double dflt) {
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : dflt;
}

static void *map_anon(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap of %zu bytes failed\n", bytes);
        exit(1);
    }
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    /* Linux only: THP=never or THP=always picks the 4 KiB or 2 MiB variant
     * of the experiment; unset leaves the system default. macOS has no
     * equivalent and always uses its 16 KiB page here. */
    const char *thp = getenv("THP");
    if (thp && strcmp(thp, "never") == 0) madvise(p, bytes, MADV_NOHUGEPAGE);
    if (thp && strcmp(thp, "always") == 0) madvise(p, bytes, MADV_HUGEPAGE);
#endif
    return p;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Per-page distribution of one timed pass: median; the share and mean of
 * the pages above SLOW_NS; and how those pages sit in the buffer, as the
 * number of contiguous runs they form and the longest run. Stalls that
 * arrive in long runs of neighbouring pages point at something that walks
 * the pages in the order they were faulted, not at a per-page cost. */
static void page_distribution(const uint64_t *ns, size_t pages, uint64_t *scratch,
                              double *median, double *slow_fraction, double *slow_mean,
                              double *slow_runs, double *slow_longest_run) {
    memcpy(scratch, ns, pages * sizeof(uint64_t));
    qsort(scratch, pages, sizeof(uint64_t), cmp_u64);
    *median = (double)scratch[pages / 2];
    size_t nslow = 0, runs = 0, run = 0, longest = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < pages; i++) {
        if (ns[i] >= SLOW_NS) {
            nslow++;
            sum += ns[i];
            if (run++ == 0) runs++;
            if (run > longest) longest = run;
        } else {
            run = 0;
        }
    }
    *slow_fraction = (double)nslow / (double)pages;
    *slow_mean = nslow ? (double)sum / (double)nslow : 0.0;
    *slow_runs = (double)runs;
    *slow_longest_run = (double)longest;
}

/* Reps whose whole-pass average is above SLOW_REP_NS_PER_PAGE. */
static int slow_reps(const double *v, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) k += (v[i] > SLOW_REP_NS_PER_PAGE);
    return k;
}

static void report(const char *label, const char *key, double *v, int n, const char *unit,
                   double ghz, int cycles) {
    stats_t s = stats(v, n);
    print_row(label, s, unit);
    printf("RESULT %s_min %.3f %s\n", key, s.min, unit);
    printf("RESULT %s_median %.3f %s\n", key, s.median, unit);
    printf("RESULT %s_max %.3f %s\n", key, s.max, unit);
    printf("RESULT %s_cv %.2f percent\n", key, 100.0 * s.cv);
    if (cycles && ghz > 0) printf("RESULT %s_cycles %.0f cycles/page\n", key, s.min * ghz);
}

static void report_count(const char *label, const char *key, double *v, int n) {
    stats_t s = stats(v, n);
    printf("%-40s min %12.0f  median %12.0f  max %12.0f  n %d  faults\n", label, s.min, s.median,
           s.max, s.n);
    printf("RESULT %s_median %.0f faults\n", key, s.median);
    printf("RESULT %s_max %.0f faults\n", key, s.max);
}

int main(void) {
    const int quick = env_int("QUICK", 0);
    const size_t mib = (size_t)env_int("MIB", quick ? 64 : 1024);
    const size_t bytes = mib << 20;
    const int reps = env_int("REPS", quick ? 5 : 15);
    /* run.sh passes the clock_estimate result so cycles can be derived. */
    const double ghz = env_double("CLOCK_GHZ", 0.0);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t pages = bytes / page;
    const size_t words = bytes / sizeof(uint64_t);
    /* Distinct values per pass so a check can tell which pass wrote last. */
    const unsigned char v1 = 0x11, v2 = 0x22, v3 = 0x33, v4 = 0x44, v5 = 0x55;
    const uint64_t k3 = 0x9E3779B97F4A7C15ull, kb = 0xD1B54A32D192ED03ull;

    if (reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }
    if (bytes % page != 0 || pages < 16) {
        fprintf(stderr, "buffer must be a multiple of the page size and at least 16 pages\n");
        return 1;
    }

    printf("page %zu bytes  buffer %zu bytes (%zu MiB)  pages %zu  words %zu  reps %d (plus 1 warmup, discarded)  clock %.2f GHz\n",
           page, bytes, mib, pages, words, reps, ghz);
    printf("RESULT page_bytes %zu bytes\n", page);
    printf("RESULT buffer_bytes %zu bytes\n", bytes);
    printf("RESULT pages %zu pages\n", pages);
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);

    /* Sample arrays, one entry per timed rep. */
    double *mmap_us = malloc(sizeof(double) * (size_t)reps);
    double *pass1 = malloc(sizeof(double) * (size_t)reps);
    double *pass2 = malloc(sizeof(double) * (size_t)reps);
    double *pass2r = malloc(sizeof(double) * (size_t)reps);
    double *pass3 = malloc(sizeof(double) * (size_t)reps);
    double *munmap_ns = malloc(sizeof(double) * (size_t)reps);
    double *f_pass1 = malloc(sizeof(double) * (size_t)reps);
    double *f_pass2 = malloc(sizeof(double) * (size_t)reps);
    double *f_pass3 = malloc(sizeof(double) * (size_t)reps);
    double *fresh = malloc(sizeof(double) * (size_t)reps);
    double *f_fresh = malloc(sizeof(double) * (size_t)reps);
    double *d1_median = malloc(sizeof(double) * (size_t)reps);
    double *d1_slow = malloc(sizeof(double) * (size_t)reps);
    double *d2_slow = malloc(sizeof(double) * (size_t)reps);
    double *d2_slow_mean = malloc(sizeof(double) * (size_t)reps);
    double *d2_runs = malloc(sizeof(double) * (size_t)reps);
    double *d2_longest = malloc(sizeof(double) * (size_t)reps);
    double *malloc_us = malloc(sizeof(double) * (size_t)reps);
    double *mtouch1 = malloc(sizeof(double) * (size_t)reps);
    double *mtouch2 = malloc(sizeof(double) * (size_t)reps);
    double *f_mtouch1 = malloc(sizeof(double) * (size_t)reps);
    double *free_ns = malloc(sizeof(double) * (size_t)reps);
    double *same_addr = malloc(sizeof(double) * (size_t)reps);
    /* Per-page timings for cycle C, faulted in now so they cost nothing later. */
    uint64_t *d1 = malloc(pages * sizeof(uint64_t));
    uint64_t *d2 = malloc(pages * sizeof(uint64_t));
    uint64_t *scratch = malloc(pages * sizeof(uint64_t));
    if (!mmap_us || !pass1 || !pass2 || !pass2r || !pass3 || !munmap_ns || !f_pass1 || !f_pass2 ||
        !f_pass3 || !fresh || !f_fresh || !d1_median || !d1_slow || !d2_slow ||
        !d2_slow_mean || !d2_runs || !d2_longest || !malloc_us || !mtouch1 || !mtouch2 ||
        !f_mtouch1 || !free_ns || !same_addr || !d1 || !d2 || !scratch) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    memset(d1, 0, pages * sizeof(uint64_t));
    memset(d2, 0, pages * sizeof(uint64_t));
    memset(scratch, 0, pages * sizeof(uint64_t));

    int mismatches = 0;
    const uint64_t want3 = expected_fill_sum((uint64_t)words, k3);
    const uint64_t wantb = expected_fill_sum((uint64_t)words, kb);
    printf("fill checksums: pass 3 expects %" PRIu64 ", fresh fill expects %" PRIu64 "\n", want3, wantb);

    for (int r = -1; r < reps; r++) {
        /* Cycle A: the headline passes on one mapping. The fault counter is
         * read outside every timed window, so a window holds one call to
         * touch() and nothing else. */
        uint64_t t0 = now_ns();
        unsigned char *p = (unsigned char *)map_anon(bytes);
        uint64_t t1 = now_ns();
        long fa = faults();
        uint64_t t1b = now_ns();
        touch(p, pages, page, v1);
        uint64_t t2 = now_ns();
        long fb = faults();
        uint64_t t2b = now_ns();
        touch(p, pages, page, v2);
        uint64_t t3 = now_ns();
        long fc = faults();
        uint64_t t3b = now_ns();
        touch(p, pages, page, v3);
        uint64_t t4 = now_ns();
        SINK(p);
        uint64_t s = sum_first_bytes(p, pages, page);
        SINK(s);
        if (s != (uint64_t)pages * v3) {
            mismatches++;
            fprintf(stderr, "MISMATCH rep %d touch: got %" PRIu64 " want %" PRIu64 "\n", r, s,
                    (uint64_t)pages * v3);
        }
        long fd = faults();
        uint64_t t5 = now_ns();
        fill((uint64_t *)p, words, k3);
        uint64_t t6 = now_ns();
        long fe = faults();
        SINK(p);
        s = sum_words((const uint64_t *)p, words);
        SINK(s);
        if (s != want3) {
            mismatches++;
            fprintf(stderr, "MISMATCH rep %d pass 3: got %" PRIu64 " want %" PRIu64 "\n", r, s, want3);
        }
        uint64_t t7 = now_ns();
        munmap(p, bytes);
        uint64_t t8 = now_ns();

        /* Cycle B: fill a fresh mapping, faults included. */
        unsigned char *q = (unsigned char *)map_anon(bytes);
        long ff = faults();
        uint64_t t9 = now_ns();
        fill((uint64_t *)q, words, kb);
        uint64_t t10 = now_ns();
        long fg = faults();
        SINK(q);
        s = sum_words((const uint64_t *)q, words);
        SINK(s);
        if (s != wantb) {
            mismatches++;
            fprintf(stderr, "MISMATCH rep %d fresh fill: got %" PRIu64 " want %" PRIu64 "\n", r, s, wantb);
        }
        munmap(q, bytes);

        /* Cycle C: pass 1 and pass 2 with every page timed. */
        unsigned char *c = (unsigned char *)map_anon(bytes);
        touch_timed(c, pages, page, v1, d1);
        touch_timed(c, pages, page, v2, d2);
        SINK(c);
        s = sum_first_bytes(c, pages, page);
        SINK(s);
        if (s != (uint64_t)pages * v2) {
            mismatches++;
            fprintf(stderr, "MISMATCH rep %d timed touch: got %" PRIu64 " want %" PRIu64 "\n", r, s,
                    (uint64_t)pages * v2);
        }
        munmap(c, bytes);

        if (r < 0) continue;
        mmap_us[r] = (double)(t1 - t0) / 1e3;
        pass1[r] = (double)(t2 - t1b) / (double)pages;
        pass2[r] = (double)(t3 - t2b) / (double)pages;
        pass2r[r] = (double)(t4 - t3b) / (double)pages;
        pass3[r] = (double)bytes / (double)(t6 - t5);
        munmap_ns[r] = (double)(t8 - t7) / (double)pages;
        f_pass1[r] = (double)(fb - fa);
        f_pass2[r] = (double)(fc - fb);
        f_pass3[r] = (double)(fe - fd);
        fresh[r] = (double)bytes / (double)(t10 - t9);
        f_fresh[r] = (double)(fg - ff);
        double unused_mean, unused_median, unused_runs, unused_longest;
        page_distribution(d1, pages, scratch, &d1_median[r], &d1_slow[r], &unused_mean, &unused_runs,
                          &unused_longest);
        page_distribution(d2, pages, scratch, &unused_median, &d2_slow[r], &d2_slow_mean[r],
                          &d2_runs[r], &d2_longest[r]);
        printf("rep %2d: mmap %.1f us  pass1 %.0f ns/page (%ld faults)  pass2 %.0f ns/page (%ld faults)  pass2r %.0f ns/page  pass3 %.1f GB/s (%ld faults)  munmap %.0f ns/page  fresh fill %.1f GB/s (%ld faults)  timed pass2 slow pages %.1f%% in %.0f runs, longest %.0f pages\n",
               r, mmap_us[r], pass1[r], fb - fa, pass2[r], fc - fb, pass2r[r], pass3[r], fe - fd,
               munmap_ns[r], fresh[r], fg - ff, 100.0 * d2_slow[r], d2_runs[r], d2_longest[r]);
    }

    /* Cycle D: malloc and free. The warmup cycle is the allocator's first
     * request for a block this size; every timed cycle asks again after a
     * free, which is where a pool or cache shows itself. */
    unsigned char *prev = NULL;
    for (int r = -1; r < reps; r++) {
        uint64_t t0 = now_ns();
        unsigned char *m = (unsigned char *)malloc(bytes);
        uint64_t t1 = now_ns();
        if (!m) {
            fprintf(stderr, "malloc of %zu bytes failed\n", bytes);
            return 1;
        }
        long fa = faults();
        uint64_t t1b = now_ns();
        touch(m, pages, page, v4);
        uint64_t t2 = now_ns();
        long fb = faults();
        uint64_t t2b = now_ns();
        touch(m, pages, page, v5);
        uint64_t t3 = now_ns();
        SINK(m);
        uint64_t s = sum_first_bytes(m, pages, page);
        SINK(s);
        if (s != (uint64_t)pages * v5) {
            mismatches++;
            fprintf(stderr, "MISMATCH malloc cycle %d: got %" PRIu64 " want %" PRIu64 "\n", r, s,
                    (uint64_t)pages * v5);
        }
        uint64_t t4 = now_ns();
        free(m);
        uint64_t t5 = now_ns();
        if (r < 0) {
            printf("malloc warmup (the first request for %zu bytes): %ld faults on first touch, %.0f ns/page, address %p; not a result\n",
                   bytes, fb - fa, (double)(t2 - t1b) / (double)pages, (void *)m);
            printf("RESULT malloc_first_cycle_faults %ld faults\n", fb - fa);
        } else {
            malloc_us[r] = (double)(t1 - t0) / 1e3;
            mtouch1[r] = (double)(t2 - t1b) / (double)pages;
            mtouch2[r] = (double)(t3 - t2b) / (double)pages;
            f_mtouch1[r] = (double)(fb - fa);
            free_ns[r] = (double)(t5 - t4) / (double)pages;
            same_addr[r] = (m == prev) ? 1.0 : 0.0;
            printf("malloc cycle %2d: malloc %.1f us  touch %.0f ns/page (%ld faults)  touch again %.0f ns/page  free %.0f ns/page  address %p (%s as previous cycle)\n",
                   r, malloc_us[r], mtouch1[r], fb - fa, mtouch2[r], free_ns[r], (void *)m,
                   same_addr[r] ? "same" : "different");
        }
        prev = m;
    }

    printf("\nPer-page costs are min over reps (latency-like); bandwidths are median (throughput-like); cv over reps in every row.\n");
    report("mmap call", "mmap_call", mmap_us, reps, "us", ghz, 0);
    report("pass 1 first touch", "pass1", pass1, reps, "ns/page", ghz, 1);
    report("pass 2 second touch", "pass2", pass2, reps, "ns/page", ghz, 1);
    report("pass 2 repeated", "pass2r", pass2r, reps, "ns/page", ghz, 1);
    report("pass 3 full write", "pass3", pass3, reps, "GB/s", ghz, 0);
    report("munmap", "munmap", munmap_ns, reps, "ns/page", ghz, 0);
    report("fresh fill (faults inside)", "fresh_fill", fresh, reps, "GB/s", ghz, 0);
    report_count("pass 1 faults", "pass1_faults", f_pass1, reps);
    report_count("pass 2 faults", "pass2_faults", f_pass2, reps);
    report_count("pass 3 faults", "pass3_faults", f_pass3, reps);
    report_count("fresh fill faults", "fresh_fill_faults", f_fresh, reps);
    report("timed pass 1 per-page median", "timed_pass1_page_median", d1_median, reps, "ns", ghz, 0);
    {
        stats_t s1 = stats(d1_slow, reps), s2 = stats(d2_slow, reps);
        printf("%-40s median %6.2f%%  min %6.2f%%  max %6.2f%%  n %d\n",
               "timed pass 1 pages above 200 ns", 100.0 * s1.median, 100.0 * s1.min, 100.0 * s1.max, s1.n);
        printf("RESULT timed_pass1_slow_fraction %.2f percent\n", 100.0 * s1.median);
        printf("%-40s median %6.2f%%  min %6.2f%%  max %6.2f%%  n %d\n",
               "timed pass 2 pages above 200 ns", 100.0 * s2.median, 100.0 * s2.min, 100.0 * s2.max, s2.n);
        printf("RESULT timed_pass2_slow_fraction %.2f percent\n", 100.0 * s2.median);
        printf("RESULT timed_pass2_slow_fraction_min %.2f percent\n", 100.0 * s2.min);
        printf("RESULT timed_pass2_slow_fraction_max %.2f percent\n", 100.0 * s2.max);
    }
    report("timed pass 2 mean of slow pages", "timed_pass2_slow_mean", d2_slow_mean, reps, "ns", ghz, 0);
    {
        /* Where the slow pages of pass 2 sit: in how many contiguous runs,
         * and how long the longest run is, per rep. */
        stats_t sr = stats(d2_runs, reps), sl = stats(d2_longest, reps);
        printf("%-40s min %12.0f  median %12.0f  max %12.0f  n %d  runs\n",
               "timed pass 2 runs of slow pages", sr.min, sr.median, sr.max, sr.n);
        printf("RESULT timed_pass2_slow_runs_min %.0f runs\n", sr.min);
        printf("RESULT timed_pass2_slow_runs_median %.0f runs\n", sr.median);
        printf("RESULT timed_pass2_slow_runs_max %.0f runs\n", sr.max);
        printf("%-40s min %12.0f  median %12.0f  max %12.0f  n %d  pages\n",
               "timed pass 2 longest run of slow pages", sl.min, sl.median, sl.max, sl.n);
        printf("RESULT timed_pass2_slow_longest_run_min %.0f pages\n", sl.min);
        printf("RESULT timed_pass2_slow_longest_run_median %.0f pages\n", sl.median);
        printf("RESULT timed_pass2_slow_longest_run_max %.0f pages\n", sl.max);
    }
    report("malloc call (after free)", "malloc_call", malloc_us, reps, "us", ghz, 0);
    report("malloc then first touch", "malloc_touch", mtouch1, reps, "ns/page", ghz, 1);
    report("malloc then second touch", "malloc_retouch", mtouch2, reps, "ns/page", ghz, 1);
    report("free", "free", free_ns, reps, "ns/page", ghz, 0);
    report_count("malloc then first touch faults", "malloc_touch_faults", f_mtouch1, reps);
    {
        /* In how many reps a fault-sized stall recurred on a pass that
         * took no counted faults. */
        printf("reps above %d ns/page: pass 2 %d, pass 2 repeated %d, malloc then first touch %d, malloc then second touch %d, of %d\n",
               SLOW_REP_NS_PER_PAGE, slow_reps(pass2, reps), slow_reps(pass2r, reps),
               slow_reps(mtouch1, reps), slow_reps(mtouch2, reps), reps);
        printf("RESULT slow_rep_threshold %d ns/page\n", SLOW_REP_NS_PER_PAGE);
        printf("RESULT pass2_slow_reps %d count\n", slow_reps(pass2, reps));
        printf("RESULT pass2r_slow_reps %d count\n", slow_reps(pass2r, reps));
        printf("RESULT malloc_touch_slow_reps %d count\n", slow_reps(mtouch1, reps));
        printf("RESULT malloc_retouch_slow_reps %d count\n", slow_reps(mtouch2, reps));
        printf("RESULT reps %d count\n", reps);
    }
    {
        int same = 0;
        for (int r = 0; r < reps; r++) same += (same_addr[r] > 0.5);
        printf("malloc returned the address it had just freed in %d of %d cycles\n", same, reps);
        printf("RESULT malloc_same_address_cycles %d count\n", same);
        printf("RESULT malloc_cycles %d count\n", reps);
    }

    free(mmap_us); free(pass1); free(pass2); free(pass2r); free(pass3); free(munmap_ns);
    free(f_pass1); free(f_pass2); free(f_pass3); free(fresh); free(f_fresh);
    free(d1_median); free(d1_slow); free(d2_slow); free(d2_slow_mean); free(d2_runs); free(d2_longest);
    free(malloc_us); free(mtouch1); free(mtouch2); free(f_mtouch1); free(free_ns); free(same_addr);
    free(d1); free(d2); free(scratch);
    if (mismatches) {
        printf("FAIL: %d checksum mismatches\n", mismatches);
        return 1;
    }
    printf("checksums: every pass wrote what the check read back\n");
    return 0;
}
