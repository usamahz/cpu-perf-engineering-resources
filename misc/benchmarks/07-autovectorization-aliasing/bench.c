/* 08-autovectorization-aliasing: what the vectoriser does with a loop whose
 * pointers it cannot prove distinct.
 *
 * One loop, a[i] = b[i] * s + c[i], is written several times. The loop
 * body never changes; only what the compiler is told about the pointers
 * does: nothing (plain), restrict, a pragma that asserts there is no
 * loop-carried dependence, an index with a stride the compiler cannot
 * see, and an extra store through a fourth pointer on every iteration.
 * A copy with the vectoriser switched off is the scalar baseline: the
 * code the loop is left with whenever the vectoriser gives up.
 *
 * Each variant runs over three distinct arrays. The plain and pragma
 * variants also run in place, with a == c, which is the saxpy y = s*x + y
 * and is exactly the call the compiler's runtime alias check exists to
 * protect: the check fails, and the scalar copy of the loop that the
 * compiler kept behind the check is what runs. Two sizes: one that sits
 * in the P-core L1d and one that streams from DRAM, so the ratio can be
 * read both where the core sets the pace and where memory does.
 *
 * DVFS is on and the core does not hold the clock_estimate peak under
 * these loops, and the scalar loop is bound by its issue rate, so its time,
 * and every ratio against it that memory does not set, moves with the
 * clock. PROBE=1 reads the clock with a short dependent add chain before
 * every timed pass and converts that pass to cycles with it. It is not the
 * default because the probe changes what the power controller sees: light
 * integer work between the passes lifts the clock the passes run at. The
 * default run is the sustained regime, and run.sh derives its clock from
 * the scalar loop, which the PROBE=1 pass shows to be one element per
 * cycle in every clock state the probe reads.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <inttypes.h>

#ifndef BUILD_LABEL
#define BUILD_LABEL "unlabelled"
#endif
#ifndef BUILD_FLAGS
#define BUILD_FLAGS "(not recorded)"
#endif

#define NOINLINE __attribute__((noinline))

/* The pragma spellings differ by compiler. GCC's ivdep is the closest
 * match to clang's assume_safety; novector exists from GCC 14. A compiler
 * with neither still builds, and main says so. */
#if defined(__clang__)
#define LOOP_ASSUME_SAFETY _Pragma("clang loop vectorize(assume_safety)")
#define LOOP_NO_VECTORIZE _Pragma("clang loop vectorize(disable)")
#define HAVE_ASSUME_SAFETY 1
#define HAVE_NO_VECTORIZE 1
#elif defined(__GNUC__) && __GNUC__ >= 14
#define LOOP_ASSUME_SAFETY _Pragma("GCC ivdep")
#define LOOP_NO_VECTORIZE _Pragma("GCC novector")
#define HAVE_ASSUME_SAFETY 1
#define HAVE_NO_VECTORIZE 1
#elif defined(__GNUC__)
#define LOOP_ASSUME_SAFETY _Pragma("GCC ivdep")
#define LOOP_NO_VECTORIZE
#define HAVE_ASSUME_SAFETY 1
#define HAVE_NO_VECTORIZE 0
#else
#define LOOP_ASSUME_SAFETY
#define LOOP_NO_VECTORIZE
#define HAVE_ASSUME_SAFETY 0
#define HAVE_NO_VECTORIZE 0
#endif

/* The kernels have external linkage so the compiler must keep the general
 * form of each: it cannot fold a call site's arguments into the body, and
 * the in-place call cannot leak into how the plain kernel is compiled. */

/* Plain pointers. The compiler cannot prove a, b and c are distinct. */
NOINLINE void axpy_plain(float *a, const float *b, const float *c, float s, size_t n) {
    for (size_t i = 0; i < n; i++) a[i] = b[i] * s + c[i];
}

/* restrict: the caller promises the arrays do not overlap. */
NOINLINE void axpy_restrict(float *restrict a, const float *restrict b,
                            const float *restrict c, float s, size_t n) {
    for (size_t i = 0; i < n; i++) a[i] = b[i] * s + c[i];
}

/* The pragma asserts that no iteration depends on an earlier one, which is
 * a weaker promise than restrict: a == c still satisfies it, since each
 * iteration reads and writes only its own element. */
NOINLINE void axpy_pragma(float *a, const float *b, const float *c, float s, size_t n) {
    LOOP_ASSUME_SAFETY
    for (size_t i = 0; i < n; i++) a[i] = b[i] * s + c[i];
}

/* A stride the compiler cannot see. main passes 1 through a volatile, so
 * the compiler must plan for any value. */
NOINLINE void axpy_stride(float *a, const float *b, const float *c, float s, size_t n,
                          size_t inc) {
    for (size_t i = 0; i < n; i++) a[i * inc] = b[i * inc] * s + c[i * inc];
}

/* A store through a fourth pointer on every iteration. It is a float, so
 * type-based alias analysis cannot separate it from a, b or c. */
NOINLINE void axpy_last(float *a, const float *b, const float *c, float s, size_t n,
                        float *last) {
    for (size_t i = 0; i < n; i++) {
        a[i] = b[i] * s + c[i];
        *last = a[i];
    }
}

/* The same loop with the vectoriser told to leave it alone. */
NOINLINE void axpy_scalar(float *a, const float *b, const float *c, float s, size_t n) {
    LOOP_NO_VECTORIZE
    for (size_t i = 0; i < n; i++) a[i] = b[i] * s + c[i];
}

/* Reference: r = c, then r = b * s + r applied `times` times. Every kernel
 * performs exactly this sequence of operations on each element, in the
 * same order and with the same contraction of the multiply and add, so
 * the outputs must match bit for bit whether the loop was vectorised or
 * not. times is 1 for an out-of-place call and the number of calls per
 * timed sample for an in-place one. */
static void reference(float *r, const float *b, const float *c, float s, size_t n, int times) {
    memcpy(r, c, n * sizeof(float));
    for (int k = 0; k < times; k++)
        for (size_t i = 0; i < n; i++) r[i] = b[i] * s + r[i];
}

/* A 64-bit hash of an array of floats, taken over their bit patterns, so
 * two arrays hash alike only if they are the same bit for bit (up to a
 * chance of about 2^-64 per differing pair). Four independent FNV-style
 * lanes keep four multiplies in flight, so the 16 MiB array hashes in
 * well under a millisecond and the check does not dominate the run. The
 * outputs are checked with this rather than memcmp against a reference
 * array because memcmp brought a fourth and fifth 32 KiB array through
 * the 128 KiB L1d between samples, and the variant that followed a
 * particular one of those reads ran about 10 percent slower on identical
 * code; the hash reads only a, which the kernel has just written. */
static uint64_t hash_floats(const float *p, size_t n) {
    uint64_t h0 = 0xcbf29ce484222325ull, h1 = 0x84222325cbf29ce4ull;
    uint64_t h2 = 0x9ce484222325cbf2ull, h3 = 0x2325cbf29ce48422ull;
    const uint64_t k = 0x100000001b3ull;
    uint32_t w[4];
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        memcpy(w, p + i, sizeof w);
        h0 = (h0 ^ w[0]) * k;
        h1 = (h1 ^ w[1]) * k;
        h2 = (h2 ^ w[2]) * k;
        h3 = (h3 ^ w[3]) * k;
    }
    for (; i < n; i++) {
        memcpy(w, p + i, sizeof w[0]);
        h0 = (h0 ^ w[0]) * k;
    }
    return (h0 * 31 + h1) * 31 + (h2 * 31 + h3);
}

/* splitmix64: deterministic, so every run sees the same data. */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static double env_double(const char *name, double dflt) {
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : dflt;
}

/* The clock of the core this thread is on right now, measured the way
 * common/clock_estimate measures it: a dependent integer add chain cannot
 * retire faster than one add per cycle, so its wall time per add is the
 * cycle time, and an interrupt during a chain can only lower the reading,
 * which the min of three discards. Three chains of 32000 adds take about
 * 21 us and touch no memory, so the sample sits between the reset of a
 * and the timed pass without disturbing the cache state the reset left.
 * The probe is light work, and the power controller answers light work
 * between the passes with a higher clock for the passes themselves (about
 * 4.5 GHz against about 3.98 GHz without a probe, in L1), so the chains are
 * kept short to keep the probe's share of the run small; a reading is good
 * to about 1 percent against the 42 ns timer. Without inline asm for the
 * architecture there is no probe. */
#if defined(__aarch64__) || defined(__x86_64__)
#define HAVE_CLOCK_ASM 1
static uint64_t int_chain(uint64_t n) {
    uint64_t x = 0;
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
    return x;
}
#else
#define HAVE_CLOCK_ASM 0
#endif

#define CLOCK_PROBE_ADDS 32000ull

static double clock_sample(void) {
#if HAVE_CLOCK_ASM
    double best = 1e300;
    for (int i = 0; i < 3; i++) {
        uint64_t t0 = now_ns();
        uint64_t x = int_chain(CLOCK_PROBE_ADDS);
        uint64_t t1 = now_ns();
        SINK(x);
        double ns = (double)(t1 - t0) / (double)CLOCK_PROBE_ADDS;
        if (ns < best) best = ns;
    }
    return 1.0 / best; /* GHz: adds per ns at one add per cycle */
#else
    return 0.0;
#endif
}

/* The RESULT lines carry the build label, with "probe" appended when the
 * clock probe is on, so run.sh can tell the two regimes apart. */
static const char *LABEL = BUILD_LABEL;
static int probe = 0;

typedef enum {
    V_PLAIN, V_RESTRICT, V_PRAGMA, V_STRIDE, V_LAST, V_SCALAR, V_PLAIN_INPLACE, V_PRAGMA_INPLACE,
    NVARIANTS
} variant_t;

typedef struct {
    const char *name;
    const char *label;
    int inplace;
} variant_desc;

static const variant_desc variants[NVARIANTS] = {
    {"plain", "plain", 0},
    {"restrict", "restrict", 0},
    {"pragma", "pragma", 0},
    {"stride", "stride", 0},
    {"last", "last", 0},
    {"scalar", "scalar", 0},
    {"plain_inplace", "plain in place", 1},
    {"pragma_inplace", "pragma in place", 1},
};

static void call(variant_t v, float *a, const float *b, const float *c, float s, size_t n,
                 size_t inc, float *last) {
    switch (v) {
    case V_PLAIN: axpy_plain(a, b, c, s, n); break;
    case V_RESTRICT: axpy_restrict(a, b, c, s, n); break;
    case V_PRAGMA: axpy_pragma(a, b, c, s, n); break;
    case V_STRIDE: axpy_stride(a, b, c, s, n, inc); break;
    case V_LAST: axpy_last(a, b, c, s, n, last); break;
    case V_SCALAR: axpy_scalar(a, b, c, s, n); break;
    case V_PLAIN_INPLACE: axpy_plain(a, b, a, s, n); break;
    case V_PRAGMA_INPLACE: axpy_pragma(a, b, a, s, n); break;
    default: break;
    }
}

static int run_size(size_t n, int reps) {
    const size_t bytes = n * sizeof(float);
    /* Enough calls per timed sample to cover 1<<18 elements, so a sample
     * at the small size is tens of microseconds and the timer's resolution
     * does not show in the numbers. */
    const int inner = (n >= ((size_t)1 << 18)) ? 1 : (int)(((size_t)1 << 18) / n);
    const float s = 0.75f;
    /* The compiler sees an unknown stride; the machine sees 1. */
    volatile size_t vinc = 1;
    const size_t inc = vinc;

    float *a = (float *)aligned_alloc(128, bytes);
    float *b = (float *)aligned_alloc(128, bytes);
    float *c = (float *)aligned_alloc(128, bytes);
    float *ref_out = (float *)aligned_alloc(128, bytes);
    float *ref_in = (float *)aligned_alloc(128, bytes);
    if (!a || !b || !c || !ref_out || !ref_in) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    uint64_t state = 0x0123456789ABCDEFull ^ (uint64_t)n;
    for (size_t i = 0; i < n; i++) {
        b[i] = (float)(splitmix64(&state) >> 40) * (1.0f / 16777216.0f);
        c[i] = (float)(splitmix64(&state) >> 40) * (1.0f / 16777216.0f);
        a[i] = 0.0f;
    }
    /* The references are hashed once and freed, so that only a, b and c
     * are live while the samples are taken. */
    reference(ref_out, b, c, s, n, 1);
    reference(ref_in, b, c, s, n, inner);
    const uint64_t h_out = hash_floats(ref_out, n), h_in = hash_floats(ref_in, n);
    const float last_ref = ref_out[n - 1];
    free(ref_in);
    free(ref_out);

    printf("size: n %zu floats, %zu bytes per array, %zu bytes for the three arrays, "
           "%d calls per sample, reps %d (plus 1 warmup, discarded)\n",
           n, bytes, 3 * bytes, inner, reps);
    printf("checksum: out of place 0x%016" PRIx64 ", in place after %d calls 0x%016" PRIx64 "\n",
           h_out, inner, h_in);
    printf("RESULT %s_n%zu_checksum 0x%016" PRIx64 " hash\n", LABEL, n, h_out);

    const size_t cells = (size_t)NVARIANTS * (size_t)reps;
    double *samples = (double *)malloc(sizeof(double) * cells);
    double *clocks = (double *)malloc(sizeof(double) * cells);
    double *cycles = (double *)malloc(sizeof(double) * cells);
    if (!samples || !clocks || !cycles) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    int mismatches = 0;
    float last_val = 0.0f;
    /* One sample of every variant per rep, so noise that drifts over the
     * run lands on every variant alike, in a fresh seeded random order each
     * rep, so that no variant owns a slot of the round: with a fixed order
     * and the old memcmp check, the variant in one slot read about 10
     * percent slower in L1 on identical code, whichever variant it was. The
     * hash removed the cause; the shuffle is there so that anything of the
     * kind that remains is dealt out to all of them. Every sample starts
     * with a = c, whether or not the variant is in place, so all variants
     * see the same cache state and the in-place ones start from the same
     * input. With PROBE=1 the clock probe runs after the reset and before
     * the timed call; it touches no memory. */
    uint64_t order_state = 0x2545F4914F6CDD1Dull ^ (uint64_t)n;
    int order[NVARIANTS];
    for (int r = -1; r < reps; r++) {
        for (int v = 0; v < NVARIANTS; v++) order[v] = v;
        if (r >= 0)
            for (int v = NVARIANTS - 1; v > 0; v--) {
                int j = (int)(splitmix64(&order_state) % (uint64_t)(v + 1));
                int t = order[v]; order[v] = order[j]; order[j] = t;
            }
        for (int slot = 0; slot < NVARIANTS; slot++) {
            const int v = order[slot];
            memcpy(a, c, bytes);
            double g = probe ? clock_sample() : 0.0;
            uint64_t t0 = now_ns();
            for (int k = 0; k < inner; k++) call((variant_t)v, a, b, c, s, n, inc, &last_val);
            uint64_t t1 = now_ns();
            SINK(a[n - 1]);
            SINK(last_val);
            const uint64_t want = variants[v].inplace ? h_in : h_out;
            if (hash_floats(a, n) != want || (v == V_LAST && last_val != last_ref)) {
                mismatches++;
                fprintf(stderr, "MISMATCH n %zu %s\n", n, variants[v].name);
            }
            if (r >= 0) {
                size_t cell = (size_t)v * (size_t)reps + (size_t)r;
                samples[cell] = (double)(t1 - t0) / ((double)n * inner);
                clocks[cell] = g;
                /* Converted per pass with that pass's clock; the median of
                 * the conversions is reported, not the product of two
                 * medians, so a pass whose clock dipped is converted with
                 * the clock it actually ran at. */
                cycles[cell] = samples[cell] * g;
            }
        }
    }

    for (int v = 0; v < NVARIANTS; v++) {
        char label[80];
        snprintf(label, sizeof label, "%s n%zu %s", LABEL, n, variants[v].label);
        stats_t st = stats(samples + (size_t)v * (size_t)reps, reps);
        print_row(label, st, "ns/elem");
        printf("RESULT %s_n%zu_%s_median %.4f ns/elem\n", LABEL, n, variants[v].name, st.median);
        printf("RESULT %s_n%zu_%s_min %.4f ns/elem\n", LABEL, n, variants[v].name, st.min);
        printf("RESULT %s_n%zu_%s_cv %.2f percent\n", LABEL, n, variants[v].name, 100.0 * st.cv);
        if (probe) {
            stats_t sc = stats(cycles + (size_t)v * (size_t)reps, reps);
            printf("RESULT %s_n%zu_%s_cycles %.3f cycles/elem\n", LABEL, n, variants[v].name,
                   sc.median);
        }
    }
    /* Ratios are paired within a round: each variant's sample over the plain
     * sample of the same round, then the median of those. The core moves
     * between clock states during a run, in stretches of many rounds, and
     * the L1 times of every variant move with it, so a ratio of two medians
     * can land on different states for its two sides while the samples of
     * one round almost always share a state. */
    double *ratios = (double *)malloc(sizeof(double) * (size_t)reps);
    if (!ratios) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    for (int v = 0; v < NVARIANTS; v++) {
        int base = (v == V_PRAGMA_INPLACE) ? V_PLAIN_INPLACE : V_PLAIN;
        int num = v, den = base;
        const char *stat = (v == V_PRAGMA_INPLACE) ? "vspragmainplace" : "vsplain";
        if (v == V_PLAIN) continue;
        if (v == V_PRAGMA_INPLACE) { num = V_PLAIN_INPLACE; den = V_PRAGMA_INPLACE; }
        for (int r = 0; r < reps; r++)
            ratios[r] = samples[(size_t)num * (size_t)reps + (size_t)r] /
                        samples[(size_t)den * (size_t)reps + (size_t)r];
        stats_t st = stats(ratios, reps);
        char label[80];
        snprintf(label, sizeof label, "%s n%zu %s / %s", LABEL, n, variants[num].label, variants[den].label);
        print_row(label, st, "ratio, paired per round");
        printf("RESULT %s_n%zu_%s_%s %.3f ratio\n", LABEL, n, variants[v].name, stat, st.median);
        printf("RESULT %s_n%zu_%s_%scv %.2f percent\n", LABEL, n, variants[v].name, stat, 100.0 * st.cv);
    }
    free(ratios);
    /* The clock the timed passes at this size ran at, over every probe
     * taken during them. */
    if (probe) {
        stats_t sg = stats(clocks, (int)cells);
        printf("%s n%zu clock before each timed pass: min %.3f  median %.3f  max %.3f GHz  "
               "(%d probes)\n", LABEL, n, sg.min, sg.median, sg.max, (int)cells);
        printf("RESULT %s_n%zu_clock_median %.3f GHz\n", LABEL, n, sg.median);
        printf("RESULT %s_n%zu_clock_min %.3f GHz\n", LABEL, n, sg.min);
        printf("RESULT %s_n%zu_clock_max %.3f GHz\n", LABEL, n, sg.max);
    }

    free(cycles);
    free(clocks);
    free(samples);
    free(c);
    free(b);
    free(a);
    return mismatches;
}

int main(void) {
    const int quick = env_int("QUICK", 0);
    const int reps = env_int("REPS", quick ? 11 : 31);
    /* run.sh passes the clock_estimate result taken before the run; it is
     * printed for the record. The cycles this program reports come only
     * from the PROBE=1 pass, which reads the clock itself. */
    const double ghz = env_double("CLOCK_GHZ", 0.0);
    probe = env_int("PROBE", 0) != 0;
    if (probe && !HAVE_CLOCK_ASM) {
        printf("note: no inline asm for this architecture, so PROBE=1 has nothing to measure\n");
        return 0;
    }
    if (probe) LABEL = BUILD_LABEL "probe";
    /* 1<<22 floats is 16 MiB per array, 48 MiB for three, three times the
     * 16 MiB L2 of a P-core cluster: the loop streams from DRAM. 1<<13 is
     * 32 KiB per array, 96 KiB for three, inside the 128 KiB P-core L1d.
     * QUICK shrinks the large size to 1 MiB per array. */
    const size_t n_large = quick ? ((size_t)1 << 18) : ((size_t)1 << 22);
    const size_t n_small = (size_t)1 << 13;

    if (reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }
    printf("build: %s = " BUILD_FLAGS "%s\n", LABEL, probe ? " PROBE=1" : "");
    printf("clock estimate %.2f GHz  reps %d  quick %d  probe %d\n", ghz, reps, quick, probe);
    if (!HAVE_ASSUME_SAFETY)
        printf("note: this compiler has no assume-safety pragma; the pragma variant is a plain loop\n");
    if (!HAVE_NO_VECTORIZE)
        printf("note: this compiler has no novector pragma; the scalar variant may be vectorised\n");
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);
    if (probe)
        printf("cycles: each pass is converted with the clock sampled just before it (min of three "
               "dependent integer add chains of %llu adds), then the median of the conversions is "
               "taken\n", CLOCK_PROBE_ADDS);

    /* A few hundred milliseconds of work before the first timed sample so
     * DVFS has settled; the large size runs first for the same reason. */
    {
        volatile uint64_t x = 0;
        uint64_t t0 = now_ns();
        while (now_ns() - t0 < 200000000ull) x++;
    }

    int mismatches = 0;
    mismatches += run_size(n_large, reps);
    mismatches += run_size(n_small, reps);
    if (mismatches) {
        printf("FAIL: %d output mismatches\n", mismatches);
        return 1;
    }
    printf("outputs: every variant matched the reference bit for bit\n");
    return 0;
}
