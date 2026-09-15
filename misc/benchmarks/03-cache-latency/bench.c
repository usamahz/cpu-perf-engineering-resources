/* 04-cache-latency: dependent-load latency across the memory hierarchy.
 *
 * A pointer chase over a random single-cycle permutation of cache-line-sized
 * nodes. Each step is one load whose address is the value the previous load
 * returned, so the core cannot overlap steps and the time per step is the
 * load-to-use latency of wherever the line lives. The working set doubles
 * from 4 KiB to 1 GiB and the curve steps up as it leaves each level.
 *
 * Two placements. Line mode packs nodes 128 bytes apart, so a working set of
 * W bytes touches W/128 lines and W/page pages. Page mode puts one node on
 * every page of a span, at a line offset inside the page chosen so that every
 * offset is used equally often, so the same number of lines is spread over
 * 128 times as many pages. Comparing a page-mode span with the line-mode
 * working set that has the same line count isolates what address translation
 * adds on top of the cache miss.
 *
 * The line offset in page mode has to be balanced, not merely random. One
 * L1 way on this part is 16 KiB, the same as a page, so the L1 set index is
 * exactly the line offset within the page and page mode can only ever use
 * 128 sets. Random offsets put the nodes into those sets unevenly (at 1024
 * nodes, about half of them land in sets holding more than 8), which shows
 * up as associativity misses that look like translation cost. Using each
 * offset exactly n/128 times gives page mode the same set occupancy as its
 * line-mode partner, so any difference between the two is translation.
 */
#define _GNU_SOURCE 1 /* clock_gettime and posix_memalign under -std=c11 on glibc */
#include "../common/timing.h"
#include <inttypes.h>
#include <unistd.h>

#define LINE 128u /* the M4 Pro line; on a 64-byte machine this still gives one node per line */

typedef struct node {
    struct node *next;
    uint64_t pad[(LINE - sizeof(void *)) / sizeof(uint64_t)];
} node_t;

/* splitmix64: fixed seed so every run builds the same cycles and the
 * printed end node is a reproducible checksum. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static inline uint64_t rng(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline uint32_t rng_below(uint32_t n) {
    return (uint32_t)(((rng() >> 32) * (uint64_t)n) >> 32);
}

/* Sattolo's shuffle: j < i (never j == i) makes the result one n-cycle, so a
 * chase from any node visits every node before it repeats. */
static void sattolo(uint32_t *perm, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) perm[i] = i;
    for (uint32_t i = n - 1; i > 0; i--) {
        uint32_t j = rng_below(i);
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
}

/* The timed region. Kept out of line so its loop is a named symbol in the
 * -S output the README quotes. */
static __attribute__((noinline)) const node_t *chase(const node_t *p, uint64_t steps) {
    for (uint64_t i = 0; i < steps; i++) p = p->next;
    return p;
}

/* Same dependent add chain as common/clock_estimate.c. Always run once
 * before the first measurement: the quarter second of dependent adds lets
 * DVFS ramp the core to its steady clock, which a 4 KiB chase on its own is
 * too short to do, and the result cross-checks the CLOCK_GHZ run.sh passes. */
static double clock_ghz_inprocess(void) {
#if defined(__aarch64__) || defined(__x86_64__)
    const uint64_t n = 200000000ull;
    double best = 1e300;
    for (int r = 0; r < 5; r++) {
        uint64_t x = 0;
        uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < n; i += 8) {
#if defined(__aarch64__)
            __asm__ volatile(
                "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
                "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
                : "+r"(x));
#else
            __asm__ volatile(
                "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
                "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
                : "+r"(x));
#endif
        }
        uint64_t t1 = now_ns();
        SINK(x);
        if ((double)(t1 - t0) < best) best = (double)(t1 - t0);
    }
    return (double)n / best;
#else
    return 0.0; /* no one-add-per-cycle chain known for this ISA; cycles are then not reported */
#endif
}

static const char *human(size_t b, char *buf, size_t len) {
    if (b >= (1u << 30)) snprintf(buf, len, "%zu GiB", b >> 30);
    else if (b >= (1u << 20)) snprintf(buf, len, "%zu MiB", b >> 20);
    else snprintf(buf, len, "%zu KiB", b >> 10);
    return buf;
}

/* One working set in one placement. bytes is the working set (line mode) or
 * the span (page mode); stride is LINE or the page size. Returns 0 on
 * allocation failure so the caller can stop cleanly. */
static int run_one(const char *mode, size_t bytes, size_t stride, size_t page,
                   uint64_t steps, int reps, double ghz) {
    size_t n = bytes / stride;
    unsigned char *buf = NULL;
    if (posix_memalign((void **)&buf, page, bytes) != 0 || !buf) {
        printf("%s %zu bytes: allocation failed, skipping\n", mode, bytes);
        return 0;
    }
    uint32_t *perm = (uint32_t *)malloc(sizeof(uint32_t) * n);
    uint32_t *off = (uint32_t *)malloc(sizeof(uint32_t) * n);
    if (!perm || !off) {
        printf("%s %zu bytes: allocation failed, skipping\n", mode, bytes);
        free(perm); free(off); free(buf);
        return 0;
    }
    /* Page mode: balanced shuffled offsets. Offset i % (page / LINE) uses every
     * line slot of a page, and so every L1 set, exactly n/128 times (see the
     * header comment); the Fisher-Yates shuffle then decorrelates the offset
     * from the page index so the chase order is random in both. */
    int page_mode = stride > LINE;
    uint32_t slots = (uint32_t)(stride / LINE);
    for (size_t i = 0; i < n; i++) off[i] = page_mode ? (uint32_t)(i % slots) : 0;
    if (page_mode)
        for (size_t i = n - 1; i > 0; i--) {
            uint32_t j = rng_below((uint32_t)i + 1);
            uint32_t t = off[i];
            off[i] = off[j];
            off[j] = t;
        }
    sattolo(perm, (uint32_t)n);

    /* Writing every node's next pointer also faults every page in, so the
     * warmup run below never pays a page fault. */
    for (size_t i = 0; i < n; i++) {
        node_t *from = (node_t *)(buf + i * stride + (size_t)off[i] * LINE);
        node_t *to = (node_t *)(buf + (size_t)perm[i] * stride + (size_t)off[perm[i]] * LINE);
        from->next = to;
    }
    const node_t *start = (const node_t *)(buf + (size_t)off[0] * LINE);

    /* Prove it is one cycle: the first return to the start must be at step n. */
    uint64_t len = 0;
    const node_t *q = start;
    do { q = q->next; len++; } while (q != start && len < n + 1);
    if (len != n) {
        printf("%s %zu bytes: permutation is not a single cycle (%" PRIu64 " of %zu), aborting\n",
               mode, bytes, len, n);
        exit(1);
    }

    double *v = (double *)malloc(sizeof(double) * (size_t)reps);
    const node_t *p = chase(start, steps); /* warmup, discarded */
    SINK(p);
    for (int r = 0; r < reps; r++) {
        uint64_t t0 = now_ns();
        p = chase(p, steps);
        uint64_t t1 = now_ns();
        SINK(p);
        v[r] = (double)(t1 - t0) / (double)steps;
    }
    size_t end_index = (size_t)((const unsigned char *)p - buf) / stride;
    stats_t s = stats(v, reps);

    char hb[32], label[64];
    size_t lines = n;
    size_t pages = page_mode ? n : (bytes + page - 1) / page;
    snprintf(label, sizeof label, "%s %s (%zu lines, %zu pages)", mode, human(bytes, hb, sizeof hb), lines, pages);
    print_row(label, s, "ns/load");
    if (ghz > 0)
        printf("    cycles/load min %.1f  median %.1f  end node %zu\n",
               s.min * ghz, s.median * ghz, end_index);
    else
        printf("    end node %zu\n", end_index);
    printf("RESULT %s_%zu_ns %.3f ns/load\n", mode, bytes, s.min);
    printf("RESULT %s_%zu_median %.3f ns/load\n", mode, bytes, s.median);
    if (ghz > 0) printf("RESULT %s_%zu_cyc %.1f cycles/load\n", mode, bytes, s.min * ghz);
    printf("RESULT %s_%zu_cv %.1f %%\n", mode, bytes, 100.0 * s.cv);
    printf("RESULT %s_%zu_lines %zu lines\n", mode, bytes, lines);
    printf("RESULT %s_%zu_pages %zu pages\n", mode, bytes, pages);
    printf("RESULT %s_%zu_end %zu node\n", mode, bytes, end_index);
    fflush(stdout);

    free(v); free(perm); free(off); free(buf);
    return 1;
}

int main(void) {
    int quick = env_int("QUICK", 0);
    int reps = env_int("REPS", 10);
    uint64_t total = (uint64_t)env_int("STEPS", quick ? (1 << 21) : (1 << 26));
    size_t max_bytes = (size_t)env_int("MAX_MIB", quick ? 64 : 1024) << 20;
    long ps = sysconf(_SC_PAGESIZE);
    size_t page = ps > 0 ? (size_t)ps : 16384;
    if (page < LINE) page = LINE;
    if (reps < 2) reps = 2;
    uint64_t steps = total / (uint64_t)reps;

    double local = clock_ghz_inprocess();
    double ghz = 0;
    const char *env = getenv("CLOCK_GHZ");
    if (env && *env) ghz = atof(env);
    const char *ghz_src = "CLOCK_GHZ from the environment";
    if (ghz <= 0) { ghz = local; ghz_src = "dependent add chain measured in this process"; }

    printf("04-cache-latency: dependent-load pointer chase, one thread, default QoS\n");
    printf("node %u B, page %zu B, %" PRIu64 " loads per run, %d runs plus 1 discarded warmup, min reported\n",
           LINE, page, steps, reps);
    if (ghz > 0) printf("clock %.2f GHz (%s), in-process add chain %.2f GHz\n", ghz, ghz_src, local);
    else printf("clock unknown on this ISA, cycles not reported\n");
    printf("RESULT steps_per_run %" PRIu64 " loads\n", steps);
    printf("RESULT runs %d runs\n", reps);
    printf("RESULT pagesize %zu B\n", page);
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);
    fflush(stdout);

    /* Line mode: contiguous, one node per line, 4 KiB upwards. */
    printf("\nline mode: nodes %u B apart\n", LINE);
    for (size_t b = 4096; b <= max_bytes; b <<= 1)
        if (!run_one("line", b, LINE, page, steps, reps, ghz)) break;

    /* Page mode: one node per page, from the span whose line count matches
     * the smallest line-mode working set, so every row has a partner. */
    printf("\npage mode: one node per %zu B page, line offsets shuffled and balanced over the %zu slots of a page\n",
           page, page / LINE);
    for (size_t b = 4096 * (page / LINE); b <= max_bytes; b <<= 1)
        if (!run_one("page", b, page, page, steps, reps, ghz)) break;

    printf("DONE\n");
    return 0;
}
