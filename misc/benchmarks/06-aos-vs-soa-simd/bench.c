/* 07-aos-vs-soa-simd: data layout sets the bytes a loop has to move.
 *
 * The same 1<<22 records of sixteen floats are stored twice: as an array
 * of 64-byte structs (AoS) and as sixteen separate float arrays (SoA).
 * Two kernels read one or two fields of every record: the sum of field 3,
 * and the sum of field 3 times field 7. With the struct layout every
 * 128-byte cache line that comes in from memory carries two records and
 * only 8 or 16 useful bytes, so the loop moves 64 bytes per record; with
 * the separate arrays it moves 4 or 8. Both layouts are timed as scalar
 * code and as whatever -O2 makes of them, and the array layout also with
 * NEON intrinsics and four accumulators.
 *
 * This file is compiled twice. The default translation unit holds main,
 * the -O2 kernels and the intrinsics. With SCALAR_TU defined it holds only
 * the four scalar kernels, and build.sh compiles that unit with
 * -fno-vectorize -fno-slp-vectorize so their loops stay one dependent
 * chain of fadd or fmadd. A float reduction cannot be vectorised without
 * permission to reorder the additions, so every kernel except the one
 * marked strict grants that permission with a scoped pragma; nothing
 * else from -ffast-math is on.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime, CLOCK_MONOTONIC_RAW and aligned_alloc under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <inttypes.h>
#include <stddef.h>

#define NOINLINE __attribute__((noinline))
#define NFIELDS 16

typedef struct {
    float f[NFIELDS];
} rec_t;
_Static_assert(sizeof(rec_t) == 64, "a record must be exactly 64 bytes");

/* Reassociation is the one fast-math permission a vectorised reduction
 * needs. The pragma is scoped to the kernel body, so the rest of the
 * program keeps strict IEEE ordering. GCC has no equivalent pragma, so
 * under GCC every kernel stays strict and the -O2 array variant does not
 * vectorise; the README notes this. */
#if defined(__clang__)
#define FP_REASSOC _Pragma("clang fp reassociate(on)")
#else
#define FP_REASSOC
#endif

/* Every kernel takes both layouts and reads only the one it is about, so
 * the timing loop can call any of them through one pointer type. */
typedef float (*kernel_fn)(const rec_t *aos, const float *f3, const float *f7, size_t n);

#if defined(SCALAR_TU)

/* Built with -fno-vectorize -fno-slp-vectorize. The reassociation
 * permission is granted here too, so the only difference from the -O2
 * unit is that the vectorisers are off. */

float sum3_aos_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)f3;
    (void)f7;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += aos[i].f[3];
    return s;
}

float dot37_aos_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)f3;
    (void)f7;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += aos[i].f[3] * aos[i].f[7];
    return s;
}

float sum3_soa_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)aos;
    (void)f7;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += f3[i];
    return s;
}

float dot37_soa_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)aos;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += f3[i] * f7[i];
    return s;
}

#else /* the -O2 translation unit */

float sum3_aos_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n);
float dot37_aos_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n);
float sum3_soa_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n);
float dot37_soa_scalar(const rec_t *aos, const float *f3, const float *f7, size_t n);

/* The struct layout at -O2. For the sum clang vectorises the loop
 * anyway, as a gather: it fills each vector one lane at a time with
 * loads 64 bytes apart, so there is still one 4-byte load per record.
 * For the product the cost model declines and the loop is interleaved
 * four ways as scalar fmadd instead. Neither changes the bytes moved:
 * the whole line comes in for every record either way. */
NOINLINE static float sum3_aos_o2(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)f3;
    (void)f7;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += aos[i].f[3];
    return s;
}

NOINLINE static float dot37_aos_o2(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)f3;
    (void)f7;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += aos[i].f[3] * aos[i].f[7];
    return s;
}

/* The array layout at -O2 with strict IEEE ordering, which is what a
 * reader gets by default. The vectoriser reports both loops as
 * vectorised, but without permission to reorder it must add the lanes
 * one at a time into one accumulator. For the sum that is the same
 * serial fadd chain as the scalar unit; for the product the multiply
 * stays vectorised (fmul.4s) and only the adds are serial, so the chain
 * is the fadd latency rather than the scalar unit's fmadd latency. Timed
 * to show that the remark alone proves nothing. */
NOINLINE static float sum3_soa_strict(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    (void)aos;
    (void)f7;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += f3[i];
    return s;
}

NOINLINE static float dot37_soa_strict(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    (void)aos;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += f3[i] * f7[i];
    return s;
}

/* The array layout at -O2 with reassociation permitted: the loop the
 * claim is about. Clang vectorises it four lanes wide and interleaves
 * four times, which is sixteen records per iteration into four
 * independent vector accumulators. */
NOINLINE static float sum3_soa_o2(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)aos;
    (void)f7;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += f3[i];
    return s;
}

NOINLINE static float dot37_soa_o2(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    FP_REASSOC
    (void)aos;
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += f3[i] * f7[i];
    return s;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define HAVE_NEON 1
#include <arm_neon.h>

/* Hand-written NEON with four accumulators, the shape the compiler
 * reaches on its own above. Four chains cover the add latency; the
 * remainder loop only runs when n is not a multiple of sixteen. */
NOINLINE static float sum3_soa_neon(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    (void)aos;
    (void)f7;
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        a0 = vaddq_f32(a0, vld1q_f32(f3 + i));
        a1 = vaddq_f32(a1, vld1q_f32(f3 + i + 4));
        a2 = vaddq_f32(a2, vld1q_f32(f3 + i + 8));
        a3 = vaddq_f32(a3, vld1q_f32(f3 + i + 12));
    }
    float s = vaddvq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)));
    for (; i < n; i++) s += f3[i];
    return s;
}

NOINLINE static float dot37_soa_neon(const rec_t *aos, const float *f3, const float *f7, size_t n) {
    (void)aos;
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        a0 = vfmaq_f32(a0, vld1q_f32(f3 + i), vld1q_f32(f7 + i));
        a1 = vfmaq_f32(a1, vld1q_f32(f3 + i + 4), vld1q_f32(f7 + i + 4));
        a2 = vfmaq_f32(a2, vld1q_f32(f3 + i + 8), vld1q_f32(f7 + i + 8));
        a3 = vfmaq_f32(a3, vld1q_f32(f3 + i + 12), vld1q_f32(f7 + i + 12));
    }
    float s = vaddvq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)));
    for (; i < n; i++) s += f3[i] * f7[i];
    return s;
}
#endif

typedef struct {
    const char *name;
    int bytes_per_rec; /* bytes the kernel has to bring in per record */
    kernel_fn fn;
} variant_t;

static const variant_t sum3_variants[] = {
    {"aos_scalar", 64, sum3_aos_scalar},
    {"aos_o2", 64, sum3_aos_o2},
    {"soa_scalar", 4, sum3_soa_scalar},
    {"soa_strict", 4, sum3_soa_strict},
    {"soa_o2", 4, sum3_soa_o2},
#if defined(HAVE_NEON)
    {"soa_neon", 4, sum3_soa_neon},
#endif
};

static const variant_t dot37_variants[] = {
    {"aos_scalar", 64, dot37_aos_scalar},
    {"aos_o2", 64, dot37_aos_o2},
    {"soa_scalar", 8, dot37_soa_scalar},
    {"soa_strict", 8, dot37_soa_strict},
    {"soa_o2", 8, dot37_soa_o2},
#if defined(HAVE_NEON)
    {"soa_neon", 8, dot37_soa_neon},
#endif
};

#define NVARIANTS ((int)(sizeof(sum3_variants) / sizeof(sum3_variants[0])))

typedef struct {
    const char *name;
    const variant_t *variants;
    double ref;
} kernel_t;

/* splitmix64: deterministic, so every run sees the same data and the
 * reference sums are the same across runs. */
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

/* Sweep a buffer far larger than the L2 so every timed pass starts with
 * its data out of cache. A 16 MiB field array is exactly the size of the
 * P-core L2, so without this the second array variant in a rep would
 * find much of its input left behind by the first. */
NOINLINE static void evict_caches(const uint64_t *buf, size_t words) {
    uint64_t s = 0;
    for (size_t i = 0; i < words; i++) s += buf[i];
    SINK(s);
}

/* The clock of the core this thread is on right now, measured the way
 * common/clock_estimate and benchmark 03 measure it: a dependent integer
 * add chain cannot retire faster than one add per cycle, so its wall time
 * per add is the cycle time, and an interrupt or a migration during a
 * sample can only lower the reading, which the min of three discards.
 * Three chains of 2000000 adds take about 1.3 ms and touch no memory, so
 * the sample sits between the eviction sweep and the timed pass without
 * disturbing the cache state the sweep left. Every pass is converted to
 * cycles with its own sample because DVFS is on and macOS may move the
 * thread, so one estimate taken before the run can land on a dip and
 * shift the whole cycles column. Without inline asm for the architecture
 * the sample falls back to the estimate run.sh passes in. */
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

static double clock_sample(double fallback_ghz) {
#if HAVE_CLOCK_ASM
    (void)fallback_ghz;
    const uint64_t adds = 2000000ull;
    double best = 1e300;
    for (int i = 0; i < 3; i++) {
        uint64_t t0 = now_ns();
        uint64_t x = int_chain(adds);
        uint64_t t1 = now_ns();
        SINK(x);
        double ns = (double)(t1 - t0) / (double)adds;
        if (ns < best) best = ns;
    }
    return 1.0 / best; /* GHz: adds per ns at one add per cycle */
#else
    return fallback_ghz;
#endif
}

int main(void) {
    const int quick = env_int("QUICK", 0);
    const size_t n = quick ? ((size_t)1 << 19) : ((size_t)1 << 22);
    const int reps = env_int("REPS", quick ? 5 : 31);
    /* run.sh passes the clock_estimate result taken before the run. It is
     * printed for the record and used only where clock_sample has no
     * inline asm; every timed pass otherwise carries its own sample. */
    const double ghz = env_double("CLOCK_GHZ", 0.0);
    /* A float accumulator near 1e6 has an ulp of 1/16, so a serial chain
     * of four million adds drifts: for the product kernel, whose addends
     * crowd towards zero, more of them fall below half an ulp and vanish
     * than round up, and the one-chain variants come out about 2e-3 low.
     * The four-accumulator vector forms hold sixteen partial sums a
     * sixteenth the size and land within 1e-5. The tolerance sits above
     * the worst chain and fifty times below what a loop over half the
     * data would give; the largest deviation per variant is printed so
     * the accuracy difference between the forms is on record. */
    const double tol = 1e-2;
    const size_t evict_bytes = (size_t)128 << 20;

    if (reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }

    rec_t *aos = (rec_t *)aligned_alloc(128, n * sizeof(rec_t));
    float *soa[NFIELDS];
    for (int k = 0; k < NFIELDS; k++) soa[k] = (float *)aligned_alloc(128, n * sizeof(float));
    uint64_t *evict = (uint64_t *)aligned_alloc(128, evict_bytes);
    if (!aos || !evict) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    for (int k = 0; k < NFIELDS; k++) {
        if (!soa[k]) {
            fprintf(stderr, "allocation failed\n");
            return 1;
        }
    }

    /* Same value into both layouts, uniform in [0, 1) with 24 significant
     * bits so every value is an exact float. All sixteen fields are
     * filled so both layouts hold the full data set; the kernels then
     * read one or two of them. */
    uint64_t state = 0x0123456789ABCDEFull;
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < NFIELDS; k++) {
            float v = (float)(splitmix64(&state) >> 40) * (1.0f / 16777216.0f);
            aos[i].f[k] = v;
            soa[k][i] = v;
        }
    }
    for (size_t i = 0; i < evict_bytes / sizeof(uint64_t); i++) evict[i] = i;

    /* References in double, from the struct copy, so they come from a
     * different layout and a different precision than any kernel. */
    double ref3 = 0.0, ref37 = 0.0;
    for (size_t i = 0; i < n; i++) {
        ref3 += (double)aos[i].f[3];
        ref37 += (double)aos[i].f[3] * (double)aos[i].f[7];
    }

    kernel_t kernels[2] = {
        {"sum3", sum3_variants, ref3},
        {"dot37", dot37_variants, ref37},
    };
    const int nk = 2, nv = NVARIANTS;

    printf("records %zu  record bytes %zu  aos bytes %zu  soa field bytes %zu  soa fields %d  reps %d (plus 1 warmup, discarded)  evict sweep %zu bytes  clock %.2f GHz\n",
           n, sizeof(rec_t), n * sizeof(rec_t), n * sizeof(float), NFIELDS, reps, evict_bytes, ghz);
    printf("reference (double, from the struct copy): sum3 %.3f  dot37 %.3f  tolerance %.0e relative\n", ref3, ref37, tol);
    printf("RESULT records %zu count\n", n);
    printf("RESULT aos_bytes %zu bytes\n", n * sizeof(rec_t));
    printf("RESULT soa_field_bytes %zu bytes\n", n * sizeof(float));
    printf("RESULT evict_bytes %zu bytes\n", evict_bytes);
    printf("RESULT reps %d count\n", reps);
    printf("RESULT sum3_reference %.3f sum\n", ref3);
    printf("RESULT dot37_reference %.3f sum\n", ref37);
    printf("RESULT tolerance %.0e relative\n", tol);
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);
#if HAVE_CLOCK_ASM
    printf("cycles: each pass is converted with the clock sampled just before it (min of three dependent integer add chains of 2000000 adds), then the median of the conversions is taken\n");
#else
    printf("cycles: no inline asm for this architecture, so every pass is converted with the %.2f GHz estimate taken before the run\n", ghz);
#endif
#if !defined(HAVE_NEON)
    printf("neon variant: skipped, this target has no NEON\n");
#endif

    /* Samples are taken round-robin, one pass of every kernel and variant
     * per rep, so machine noise that drifts over the run lands on every
     * variant alike instead of on whichever ran last. Each pass starts
     * after the eviction sweep. */
    const size_t cells = (size_t)nk * (size_t)nv * (size_t)reps;
    double *samples = (double *)malloc(sizeof(double) * cells);
    double *clocks = (double *)malloc(sizeof(double) * cells);
    double *cycles = (double *)malloc(sizeof(double) * cells);
    double *relerr = (double *)calloc((size_t)nk * (size_t)nv, sizeof(double));
    if (!samples || !clocks || !cycles || !relerr) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    int mismatches = 0;
    for (int r = -1; r < reps; r++) {
        for (int k = 0; k < nk; k++) {
            for (int v = 0; v < nv; v++) {
                const variant_t *var = &kernels[k].variants[v];
                evict_caches(evict, evict_bytes / sizeof(uint64_t));
                double g = clock_sample(ghz);
                uint64_t t0 = now_ns();
                float s = var->fn(aos, soa[3], soa[7], n);
                uint64_t t1 = now_ns();
                SINK(s);
                double d = fabs((double)s - kernels[k].ref) / kernels[k].ref;
                if (!(d <= tol)) {
                    mismatches++;
                    fprintf(stderr, "MISMATCH %s %s: got %.3f want %.3f\n", kernels[k].name, var->name, s, kernels[k].ref);
                }
                if (d > relerr[k * nv + v]) relerr[k * nv + v] = d;
                if (r >= 0) {
                    size_t cell = (size_t)(k * nv + v) * (size_t)reps + (size_t)r;
                    samples[cell] = (double)(t1 - t0) / (double)n;
                    clocks[cell] = g;
                    /* Converted per pass with that pass's clock; the median
                     * of the conversions is reported, since the min of a
                     * ratio would pick the pass whose sample lagged a clock
                     * change. */
                    cycles[cell] = samples[cell] * g;
                }
            }
        }
    }

    for (int k = 0; k < nk; k++) {
        for (int v = 0; v < nv; v++) {
            const variant_t *var = &kernels[k].variants[v];
            char label[64], key[64];
            snprintf(label, sizeof label, "%s %s", kernels[k].name, var->name);
            snprintf(key, sizeof key, "%s_%s", kernels[k].name, var->name);
            size_t base = (size_t)(k * nv + v) * (size_t)reps;
            stats_t s = stats(samples + base, reps);
            stats_t sc = stats(cycles + base, reps);
            stats_t sg = stats(clocks + base, reps);
            print_row(label, s, "ns/rec");
            if (sg.median > 0) {
                print_row("  same, in cycles (per-pass clock)", sc, "cycles/rec");
                printf("  clock before each pass: min %.3f  median %.3f  max %.3f GHz\n", sg.min, sg.median, sg.max);
            }
            printf("RESULT %s_bytes %d B/rec\n", key, var->bytes_per_rec);
            printf("RESULT %s_median %.4f ns/rec\n", key, s.median);
            printf("RESULT %s_min %.4f ns/rec\n", key, s.min);
            printf("RESULT %s_cv %.2f percent\n", key, 100.0 * s.cv);
            /* Bytes per record over ns per record is bytes per ns, which is GB/s with GB = 1e9 bytes. */
            printf("RESULT %s_gbps %.2f GB/s\n", key, (double)var->bytes_per_rec / s.median);
            if (sg.median > 0) {
                printf("RESULT %s_cycles %.3f cycles/rec\n", key, sc.median);
                printf("RESULT %s_ghz %.3f GHz\n", key, sg.median);
            }
            printf("RESULT %s_relerr %.2e relative\n", key, relerr[k * nv + v]);
        }
    }

    free(relerr);
    free(cycles);
    free(clocks);
    free(samples);
    free(evict);
    for (int k = 0; k < NFIELDS; k++) free(soa[k]);
    free(aos);
    if (mismatches) {
        printf("FAIL: %d checksum mismatches\n", mismatches);
        return 1;
    }
    printf("checksums: every variant matched the double reference within %.0e relative\n", tol);
    return 0;
}

#endif /* SCALAR_TU */
