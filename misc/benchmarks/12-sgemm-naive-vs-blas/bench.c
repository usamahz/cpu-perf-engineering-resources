/* 13-sgemm-naive-vs-blas: where the gap between a triple loop and the
 * vendor BLAS comes from.
 *
 * One SGEMM, C = A B with M = N = K = 1024 in float32, is computed four
 * ways on one thread: the naive i-j-k triple loop, the i-k-j order whose
 * inner loop the compiler vectorises, a blocked kernel with packing and a
 * hand-written 8x8 NEON register-tile microkernel, and the vendor BLAS
 * (Accelerate cblas_sgemm on macOS, OpenBLAS with -DUSE_OPENBLAS). Every
 * variant is checked against the naive result. A register-only NEON FMA
 * loop gives the ceiling any NEON code can reach, so the reader can see
 * how much of the gap the microkernel closes and what is left to a
 * different execution unit.
 *
 * A second table times a dot product over 1<<24 elements twice, int8 with
 * the NEON sdot instruction and float32 with fmla, at a streaming size and
 * at an L1-resident size, which is the arithmetic every int8 inference
 * runtime runs its inner loops on.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* clock_gettime, CLOCK_MONOTONIC_RAW and posix_memalign under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <inttypes.h>
#include <sys/resource.h>
#include <sys/time.h>
/* The intrinsics below (vfmaq_laneq_f32, vaddvq_f32, vdotq_s32, vaddlvq_s32)
 * are AArch64-only, so 32-bit Arm, which also defines __ARM_NEON, takes the
 * no-NEON path like x86 does. */
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define HAVE_NEON 1
#else
#define HAVE_NEON 0
#endif
#if defined(__APPLE__)
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
#define HAVE_BLAS 1
#define BLAS_NAME "Accelerate cblas_sgemm"
#elif defined(USE_OPENBLAS)
#include <cblas.h>
#define HAVE_BLAS 1
#define BLAS_NAME "OpenBLAS cblas_sgemm"
#else
#define HAVE_BLAS 0
#define BLAS_NAME "no BLAS"
#endif

#define NOINLINE __attribute__((noinline))
/* Make two pointers opaque to the optimiser. The dot kernels read memory
 * that never escapes, so clang treats a repeated call with the same
 * arguments as loop-invariant and would hoist it out of the pass loop; a
 * pointer that passes through a volatile asm is a new value each pass. */
#define OPAQUE(p, q) __asm__ volatile("" : "+r"(p), "+r"(q) : : "memory")

/* Blocking for a P-core with a 128 KiB L1d and a 16 MiB L2. A KC x NR
 * panel of B (8 KiB) and an MC x KC block of A (64 KiB) both sit in L1
 * while the microkernel sweeps them; the KC x N block of B (1 MiB at
 * N = 1024) sits in L2 and is packed once per K step. */
#define MR 8
#define NR 8
#define KC 256
#define MC 64

/* (1) Naive i-j-k. The inner loop is a reduction into one float, so
 * without -ffast-math the compiler may not reorder it and it stays a
 * scalar fmadd chain; B is read down a column, one cache line per k. */
NOINLINE static void sgemm_naive(int m, int n, int k, const float *restrict a,
                                 const float *restrict b, float *restrict c) {
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            float s = 0.0f;
            for (int p = 0; p < k; p++) s += a[(size_t)i * k + p] * b[(size_t)p * n + j];
            c[(size_t)i * n + j] = s;
        }
}

/* (2) i-k-j. The inner loop is a saxpy over contiguous rows of B and C,
 * which the compiler vectorises at -O2, but every FMA still carries a
 * load and a store of C, so it is bound by the load and store ports. */
NOINLINE static void sgemm_ikj(int m, int n, int k, const float *restrict a,
                               const float *restrict b, float *restrict c) {
    memset(c, 0, sizeof(float) * (size_t)m * (size_t)n);
    for (int i = 0; i < m; i++)
        for (int p = 0; p < k; p++) {
            const float aip = a[(size_t)i * k + p];
            const float *restrict bp = b + (size_t)p * n;
            float *restrict ci = c + (size_t)i * n;
            for (int j = 0; j < n; j++) ci[j] += aip * bp[j];
        }
}

#if defined(__aarch64__)
#define HAVE_FMADD_LATENCY 1
/* Latency of one dependent scalar fmadd, measured the way clock_estimate
 * measures the integer add: a chain of eight on one register. The naive
 * loop's inner reduction is exactly this chain, so its ceiling is two
 * flops per latency. */
NOINLINE static double fmadd_latency_ns(uint64_t n) {
    float s = 0.0f;
    const float a = 1.0000001f, b = 0.9999999f;
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < n; i += 8)
        __asm__ volatile("fmadd %s0, %s1, %s2, %s0\n\tfmadd %s0, %s1, %s2, %s0\n\t"
                         "fmadd %s0, %s1, %s2, %s0\n\tfmadd %s0, %s1, %s2, %s0\n\t"
                         "fmadd %s0, %s1, %s2, %s0\n\tfmadd %s0, %s1, %s2, %s0\n\t"
                         "fmadd %s0, %s1, %s2, %s0\n\tfmadd %s0, %s1, %s2, %s0\n\t"
                         : "+w"(s) : "w"(a), "w"(b));
    uint64_t t1 = now_ns();
    SINK(s);
    return (double)(t1 - t0) / (double)n;
}
#else
#define HAVE_FMADD_LATENCY 0
#endif

#if HAVE_NEON
/* Pack an mc x kc block of A into panels of MR rows: for each k the MR
 * values A[p*MR+r][k] are contiguous, so the microkernel loads a column of
 * the tile as two vectors instead of gathering it down a stride. */
static void pack_a(const float *a, int lda, int mc, int kc, float *dst) {
    for (int p = 0; p < mc; p += MR)
        for (int q = 0; q < kc; q++)
            for (int r = 0; r < MR; r++) *dst++ = a[(size_t)(p + r) * lda + q];
}

/* Pack a kc x nc block of B into panels of NR columns, so the microkernel
 * reads B as a stream of NR-wide rows and touches one line per k. */
static void pack_b(const float *b, int ldb, int kc, int nc, float *dst) {
    for (int p = 0; p < nc; p += NR)
        for (int q = 0; q < kc; q++)
            for (int col = 0; col < NR; col++) *dst++ = b[(size_t)q * ldb + p + col];
}

/* 8x8 register-tile microkernel: C[0..8)[0..8) += Apanel (8 x kc) times
 * Bpanel (kc x 8). Sixteen 128-bit accumulators hold the tile for the
 * whole k loop; each k step is four loads and sixteen FMAs, each FMA
 * broadcasting one lane of A. Four FMA pipes times the three-cycle fmadd
 * latency this benchmark measures need twelve independent chains in
 * flight; sixteen leaves slack, which is why the tile is 8x8 and not
 * 4x4. */
#define TILE_ROW(r, av, lane)                                  \
    c0##r = vfmaq_laneq_f32(c0##r, b0, av, lane);              \
    c1##r = vfmaq_laneq_f32(c1##r, b1, av, lane);
static inline void ukernel_8x8(int kc, const float *restrict ap, const float *restrict bp,
                               float *restrict c, int ldc) {
    float32x4_t c00 = vld1q_f32(c + 0 * ldc), c10 = vld1q_f32(c + 0 * ldc + 4);
    float32x4_t c01 = vld1q_f32(c + 1 * ldc), c11 = vld1q_f32(c + 1 * ldc + 4);
    float32x4_t c02 = vld1q_f32(c + 2 * ldc), c12 = vld1q_f32(c + 2 * ldc + 4);
    float32x4_t c03 = vld1q_f32(c + 3 * ldc), c13 = vld1q_f32(c + 3 * ldc + 4);
    float32x4_t c04 = vld1q_f32(c + 4 * ldc), c14 = vld1q_f32(c + 4 * ldc + 4);
    float32x4_t c05 = vld1q_f32(c + 5 * ldc), c15 = vld1q_f32(c + 5 * ldc + 4);
    float32x4_t c06 = vld1q_f32(c + 6 * ldc), c16 = vld1q_f32(c + 6 * ldc + 4);
    float32x4_t c07 = vld1q_f32(c + 7 * ldc), c17 = vld1q_f32(c + 7 * ldc + 4);
    for (int q = 0; q < kc; q++) {
        const float32x4_t b0 = vld1q_f32(bp), b1 = vld1q_f32(bp + 4);
        const float32x4_t a0 = vld1q_f32(ap), a1 = vld1q_f32(ap + 4);
        TILE_ROW(0, a0, 0) TILE_ROW(1, a0, 1) TILE_ROW(2, a0, 2) TILE_ROW(3, a0, 3)
        TILE_ROW(4, a1, 0) TILE_ROW(5, a1, 1) TILE_ROW(6, a1, 2) TILE_ROW(7, a1, 3)
        ap += MR;
        bp += NR;
    }
    vst1q_f32(c + 0 * ldc, c00); vst1q_f32(c + 0 * ldc + 4, c10);
    vst1q_f32(c + 1 * ldc, c01); vst1q_f32(c + 1 * ldc + 4, c11);
    vst1q_f32(c + 2 * ldc, c02); vst1q_f32(c + 2 * ldc + 4, c12);
    vst1q_f32(c + 3 * ldc, c03); vst1q_f32(c + 3 * ldc + 4, c13);
    vst1q_f32(c + 4 * ldc, c04); vst1q_f32(c + 4 * ldc + 4, c14);
    vst1q_f32(c + 5 * ldc, c05); vst1q_f32(c + 5 * ldc + 4, c15);
    vst1q_f32(c + 6 * ldc, c06); vst1q_f32(c + 6 * ldc + 4, c16);
    vst1q_f32(c + 7 * ldc, c07); vst1q_f32(c + 7 * ldc + 4, c17);
}

/* (3) Blocked: the Goto loop order. For each K step pack B once into L2,
 * then for each M block pack A into L1 and run the microkernel over every
 * 8x8 tile, B panel outermost so it stays in L1 while the A block streams. */
NOINLINE static void sgemm_blocked(int m, int n, int k, const float *a, const float *b, float *c,
                                   float *apack, float *bpack) {
    memset(c, 0, sizeof(float) * (size_t)m * (size_t)n);
    for (int pc = 0; pc < k; pc += KC) {
        const int kc = (k - pc < KC) ? k - pc : KC;
        pack_b(b + (size_t)pc * n, n, kc, n, bpack);
        for (int ic = 0; ic < m; ic += MC) {
            const int mc = (m - ic < MC) ? m - ic : MC;
            pack_a(a + (size_t)ic * k + pc, k, mc, kc, apack);
            for (int jr = 0; jr < n; jr += NR)
                for (int ir = 0; ir < mc; ir += MR)
                    ukernel_8x8(kc, apack + (size_t)ir * kc, bpack + (size_t)jr * kc,
                                c + (size_t)(ic + ir) * n + jr, n);
        }
    }
}

/* The NEON ceiling: sixteen independent FMA chains on registers only, no
 * memory. Different starting values keep the chains distinct expressions.
 * The empty volatile asm gives the function a side effect; without it
 * clang proves the function touches no memory and is free to move the
 * call across the clock_gettime calls that time it. */
NOINLINE static float neon_fma_peak(uint64_t iters, float x, float y) {
    __asm__ volatile("" : "+r"(iters));
    float32x4_t acc[16];
    for (int i = 0; i < 16; i++) acc[i] = vdupq_n_f32(1e-3f * (float)(i + 1));
    const float32x4_t vx = vdupq_n_f32(x), vy = vdupq_n_f32(y);
    for (uint64_t it = 0; it < iters; it++)
        for (int i = 0; i < 16; i++) acc[i] = vfmaq_f32(acc[i], vx, vy);
    float32x4_t s = acc[0];
    for (int i = 1; i < 16; i++) s = vaddq_f32(s, acc[i]);
    return vaddvq_f32(s);
}

/* float32 dot product, eight fmla accumulators, four MACs per instruction. */
NOINLINE static float dot_f32_fmla(const float *restrict a, const float *restrict b, size_t n) {
    float32x4_t acc[8];
    for (int i = 0; i < 8; i++) acc[i] = vdupq_n_f32(0.0f);
    for (size_t i = 0; i < n; i += 32)
        for (int j = 0; j < 8; j++)
            acc[j] = vfmaq_f32(acc[j], vld1q_f32(a + i + 4 * j), vld1q_f32(b + i + 4 * j));
    float32x4_t s = acc[0];
    for (int i = 1; i < 8; i++) s = vaddq_f32(s, acc[i]);
    return vaddvq_f32(s);
}

#if defined(__ARM_FEATURE_DOTPROD)
#define HAVE_SDOT 1
/* int8 dot product, eight sdot accumulators, sixteen MACs per instruction
 * into four int32 lanes. Inputs are kept in [-16, 16) so no lane can
 * overflow int32 whatever the data. */
NOINLINE static int64_t dot_int8_sdot(const int8_t *restrict a, const int8_t *restrict b, size_t n) {
    int32x4_t acc[8];
    for (int i = 0; i < 8; i++) acc[i] = vdupq_n_s32(0);
    for (size_t i = 0; i < n; i += 128)
        for (int j = 0; j < 8; j++)
            acc[j] = vdotq_s32(acc[j], vld1q_s8(a + i + 16 * j), vld1q_s8(b + i + 16 * j));
    int32x4_t s = acc[0];
    for (int i = 1; i < 8; i++) s = vaddq_s32(s, acc[i]);
    return (int64_t)vaddlvq_s32(s);
}
#else
#define HAVE_SDOT 0
#endif
#endif /* HAVE_NEON */

/* splitmix64: deterministic, so every run sees the same data and the
 * checksums are comparable across runs. */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static float unit_float(uint64_t *state) {
    return (float)(splitmix64(state) >> 40) * (2.0f / 16777216.0f) - 1.0f; /* [-1, 1) */
}

static void *alloc_aligned(size_t bytes) {
    void *p = NULL;
    if (posix_memalign(&p, 128, bytes) != 0 || !p) {
        fprintf(stderr, "allocation of %zu bytes failed\n", bytes);
        exit(1);
    }
    return p;
}

static double max_abs_diff(const float *x, const float *y, size_t n) {
    double e = 0;
    for (size_t i = 0; i < n; i++) {
        double d = fabs((double)x[i] - (double)y[i]);
        if (d > e) e = d;
    }
    return e;
}

static double cpu_seconds(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + 1e-6 * (double)ru.ru_utime.tv_usec +
           (double)ru.ru_stime.tv_sec + 1e-6 * (double)ru.ru_stime.tv_usec;
}

enum { V_NAIVE, V_IKJ, V_BLOCKED, V_BLAS, V_COUNT };
static const char *const vkey[V_COUNT] = {"naive_ijk", "ikj_autovec", "blocked_neon_8x8", "blas"};

typedef struct {
    int m, n, k;
    const float *a, *b;
    float *c, *apack, *bpack;
} gemm_args_t;

/* Run one variant once. Returns 0 if the variant is unavailable here. */
static int run_variant(int v, const gemm_args_t *g) {
    switch (v) {
    case V_NAIVE: sgemm_naive(g->m, g->n, g->k, g->a, g->b, g->c); return 1;
    case V_IKJ: sgemm_ikj(g->m, g->n, g->k, g->a, g->b, g->c); return 1;
    case V_BLOCKED:
#if HAVE_NEON
        sgemm_blocked(g->m, g->n, g->k, g->a, g->b, g->c, g->apack, g->bpack);
        return 1;
#else
        return 0;
#endif
    case V_BLAS:
#if HAVE_BLAS
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, g->m, g->n, g->k, 1.0f, g->a, g->k,
                    g->b, g->n, 0.0f, g->c, g->n);
        return 1;
#else
        return 0;
#endif
    default: return 0;
    }
}

static void gemm_table(int quick, int reps, int blas_only) {
    const int m = quick ? 256 : 1024, n = m, k = m;
    const size_t elems = (size_t)m * (size_t)n;
    if (m % MR || n % NR || MC % MR) {
        fprintf(stderr, "M and N must be multiples of the %dx%d tile\n", MR, NR);
        exit(1);
    }
    const double flops = 2.0 * (double)m * (double)n * (double)k;
    const float *a, *b;
    {
        float *ta = (float *)alloc_aligned(sizeof(float) * (size_t)m * (size_t)k);
        float *tb = (float *)alloc_aligned(sizeof(float) * (size_t)k * (size_t)n);
        uint64_t state = 0x0123456789ABCDEFull;
        for (size_t i = 0; i < (size_t)m * (size_t)k; i++) ta[i] = unit_float(&state);
        for (size_t i = 0; i < (size_t)k * (size_t)n; i++) tb[i] = unit_float(&state);
        a = ta;
        b = tb;
    }
    float *c = (float *)alloc_aligned(sizeof(float) * elems);
    float *ref = (float *)alloc_aligned(sizeof(float) * elems);
    float *apack = (float *)alloc_aligned(sizeof(float) * MC * KC);
    float *bpack = (float *)alloc_aligned(sizeof(float) * KC * (size_t)n);
    const char *vt = getenv("VECLIB_MAXIMUM_THREADS");
    char blas_key[64];
    snprintf(blas_key, sizeof blas_key, "blas_threads_%s", (vt && *vt) ? vt : "default");

    printf("sgemm: M = N = K = %d float32, %zu bytes per matrix, %.0f flops per call, reps %d (plus 1 warmup, discarded)\n",
           m, sizeof(float) * elems, flops, reps);
    printf("blas: %s, VECLIB_MAXIMUM_THREADS=%s\n", BLAS_NAME, (vt && *vt) ? vt : "(unset)");
    printf("RESULT sgemm_dim %d n\n", m);
    printf("RESULT sgemm_matrix_bytes %zu bytes\n", sizeof(float) * elems);
    printf("RESULT sgemm_flops %.0f flops\n", flops);
    printf("RESULT sgemm_reps %d reps\n", reps);

    /* The naive result is the reference every other variant is checked
     * against; it is computed once here, outside any timing. */
    sgemm_naive(m, n, k, a, b, ref);
    double checksum = 0;
    for (size_t i = 0; i < elems; i++) checksum += ref[i];
    printf("reference: naive C, checksum (sum of all entries) %.6f\n", checksum);
    printf("RESULT sgemm_checksum %.6f sum\n", checksum);

    /* Samples are taken round-robin, one call of every variant per rep, so
     * machine noise that drifts over the run lands on every variant alike
     * instead of on whichever ran last. CPU time is accumulated around the
     * BLAS call only, so cpu/wall says how many threads the library used. */
    double *samples = (double *)malloc(sizeof(double) * V_COUNT * (size_t)reps);
    double maxerr[V_COUNT] = {0, 0, 0, 0};
    double blas_cpu = 0, blas_wall = 0;
    int available[V_COUNT] = {1, 1, 1, 1};
    int failures = 0;
    for (int r = -1; r < reps; r++) {
        for (int v = 0; v < V_COUNT; v++) {
            if ((blas_only && v != V_BLAS) || !available[v]) continue;
            gemm_args_t g = {m, n, k, a, b, c, apack, bpack};
            memset(c, 0, sizeof(float) * elems);
            CLOBBER();
            double cpu0 = (v == V_BLAS) ? cpu_seconds() : 0;
            uint64_t t0 = now_ns();
            int ok = run_variant(v, &g);
            uint64_t t1 = now_ns();
            double cpu1 = (v == V_BLAS) ? cpu_seconds() : 0;
            SINK(c);
            if (!ok) {
                available[v] = 0;
                continue;
            }
            double e = max_abs_diff(c, ref, elems);
            if (e > maxerr[v]) maxerr[v] = e;
            if (r >= 0) {
                samples[v * reps + r] = flops / (double)(t1 - t0); /* GFLOP/s */
                if (v == V_BLAS) {
                    blas_cpu += cpu1 - cpu0;
                    blas_wall += (double)(t1 - t0) * 1e-9;
                }
            }
        }
    }
    for (int v = 0; v < V_COUNT; v++) {
        if (blas_only && v != V_BLAS) continue;
        const char *key = (v == V_BLAS) ? blas_key : vkey[v];
        if (!available[v]) {
            printf("%-40s skipped: not available on this build (%s)\n", key,
                   v == V_BLAS ? "no BLAS linked; build with -framework Accelerate or -DUSE_OPENBLAS"
                               : "needs NEON");
            printf("RESULT %s_skipped 1 flag\n", key);
            continue;
        }
        stats_t s = stats(samples + v * reps, reps);
        print_row(key, s, "GFLOP/s");
        printf("RESULT %s_gflops %.3f GFLOP/s\n", key, s.median);
        printf("RESULT %s_gflops_max %.3f GFLOP/s\n", key, s.max);
        printf("RESULT %s_ms %.3f ms\n", key, flops / s.median * 1e-6);
        printf("RESULT %s_cv %.2f percent\n", key, 100.0 * s.cv);
        printf("RESULT %s_maxerr %.3e abs\n", key, maxerr[v]);
        if (v == V_BLAS) printf("RESULT %s_cpu_over_wall %.2f ratio\n", key, blas_cpu / blas_wall);
        if (maxerr[v] >= 1e-2) {
            failures++;
            fprintf(stderr, "MISMATCH %s: max abs error %.3e against naive\n", key, maxerr[v]);
        }
    }

#if HAVE_FMADD_LATENCY
    if (!blas_only) {
        const uint64_t chain = quick ? 1u << 22 : 1u << 26;
        for (int r = -1; r < reps; r++) {
            double ns = fmadd_latency_ns(chain);
            if (r >= 0) samples[r] = ns;
        }
        stats_t s = stats(samples, reps);
        print_row("fmadd_latency", s, "ns");
        printf("RESULT fmadd_latency_ns %.4f ns\n", s.min);
        printf("RESULT fmadd_latency_cv %.2f percent\n", 100.0 * s.cv);
    }
#endif
#if HAVE_NEON
    if (!blas_only) {
        /* Enough iterations for a few milliseconds per sample: 16 FMAs of
         * 4 lanes is 128 flops per iteration. */
        const uint64_t iters = quick ? 1u << 20 : 1u << 23;
        float peak_sum = 0;
        for (int r = -1; r < reps; r++) {
            uint64_t t0 = now_ns();
            float s = neon_fma_peak(iters, 1.0000001f, 0.9999999f);
            uint64_t t1 = now_ns();
            SINK(s);
            peak_sum += s;
            if (r >= 0) samples[r] = 128.0 * (double)iters / (double)(t1 - t0);
        }
        stats_t s = stats(samples, reps);
        print_row("neon_fma_peak", s, "GFLOP/s");
        printf("RESULT neon_fma_peak_gflops %.3f GFLOP/s\n", s.median);
        printf("RESULT neon_fma_peak_cv %.2f percent\n", 100.0 * s.cv);
        printf("neon_fma_peak checksum %.6e\n", (double)peak_sum);
    }
#endif
    free(samples);
    free((void *)a);
    free((void *)b);
    free(c);
    free(ref);
    free(apack);
    free(bpack);
    if (failures) {
        printf("FAIL: %d sgemm variants disagreed with the naive result\n", failures);
        exit(1);
    }
    printf("sgemm: every variant matched the naive result to within 1e-2\n");
}

#if HAVE_NEON
static void dot_table(int quick, int reps) {
    /* The streaming size is 1<<24 elements: 16 MiB per int8 array and
     * 64 MiB per float32 array, both beyond the 16 MiB L2. The L1 size is
     * 1<<13 elements, 8 KiB per int8 array and 32 KiB per float32 array,
     * inside the 128 KiB L1d, repeated so that each sample does the same
     * number of multiply-adds as one streaming pass. */
    const size_t n_stream = quick ? ((size_t)1 << 20) : ((size_t)1 << 24);
    const size_t n_l1 = (size_t)1 << 13;
    const size_t passes_l1 = n_stream / n_l1;
    int8_t *ia = (int8_t *)alloc_aligned(n_stream), *ib = (int8_t *)alloc_aligned(n_stream);
    float *fa = (float *)alloc_aligned(sizeof(float) * n_stream);
    float *fb = (float *)alloc_aligned(sizeof(float) * n_stream);
    uint64_t state = 0xFEDCBA9876543210ull;
    for (size_t i = 0; i < n_stream; i++) {
        uint64_t z = splitmix64(&state);
        ia[i] = (int8_t)((z >> 59) - 16); /* [-16, 16) */
        ib[i] = (int8_t)(((z >> 54) & 31) - 16);
    }
    for (size_t i = 0; i < n_stream; i++) fa[i] = unit_float(&state);
    for (size_t i = 0; i < n_stream; i++) fb[i] = unit_float(&state);
    /* The L1-resident arrays are copies of the first n_l1 elements placed
     * next to each other in one small block. Reading them from the heads
     * of the two large allocations instead ran the float32 loop at about
     * half this rate on this core, at some separations of the two streams
     * and not others, an address-placement effect that has nothing to do
     * with the arithmetic being compared. */
    int8_t *il1 = (int8_t *)alloc_aligned(2 * n_l1);
    float *fl1 = (float *)alloc_aligned(2 * sizeof(float) * n_l1);
    int8_t *ia_l1 = il1, *ib_l1 = il1 + n_l1;
    float *fa_l1 = fl1, *fb_l1 = fl1 + n_l1;
    memcpy(ia_l1, ia, n_l1);
    memcpy(ib_l1, ib, n_l1);
    memcpy(fa_l1, fa, sizeof(float) * n_l1);
    memcpy(fb_l1, fb, sizeof(float) * n_l1);

    /* References computed a different way: exact int64 for the int8 dot,
     * double accumulation for the float32 dot, with sum|a b| for its
     * tolerance. */
    int64_t iref_stream = 0, iref_l1 = 0;
    double fref_stream = 0, fabs_stream = 0, fref_l1 = 0, fabs_l1 = 0;
    for (size_t i = 0; i < n_stream; i++) {
        iref_stream += (int64_t)ia[i] * ib[i];
        fref_stream += (double)fa[i] * fb[i];
        fabs_stream += fabs((double)fa[i] * fb[i]);
        if (i < n_l1) {
            iref_l1 += (int64_t)ia[i] * ib[i];
            fref_l1 += (double)fa[i] * fb[i];
            fabs_l1 += fabs((double)fa[i] * fb[i]);
        }
    }
    printf("dot: %zu elements streaming (%zu bytes int8 pair, %zu bytes float32 pair); %zu elements L1-resident (%zu and %zu bytes) times %zu passes; reps %d (plus 1 warmup)\n",
           n_stream, 2 * n_stream, 2 * sizeof(float) * n_stream, n_l1, 2 * n_l1,
           2 * sizeof(float) * n_l1, passes_l1, reps);
    printf("dot references: int8 stream %" PRId64 ", int8 l1 %" PRId64 ", f32 stream %.6f, f32 l1 %.6f\n",
           iref_stream, iref_l1, fref_stream, fref_l1);
    printf("RESULT dot_int8_checksum %" PRId64 " sum\n", iref_stream);
    printf("RESULT dot_f32_checksum %.6f sum\n", fref_stream);
    printf("RESULT dot_stream_elements %zu elements\n", n_stream);
    printf("RESULT dot_stream_bytes_int8 %zu bytes\n", 2 * n_stream);
    printf("RESULT dot_stream_bytes_f32 %zu bytes\n", 2 * sizeof(float) * n_stream);
    printf("RESULT dot_l1_elements %zu elements\n", n_l1);
    printf("RESULT dot_l1_bytes_int8 %zu bytes\n", 2 * n_l1);
    printf("RESULT dot_l1_bytes_f32 %zu bytes\n", 2 * sizeof(float) * n_l1);
    printf("RESULT dot_l1_passes %zu passes\n", passes_l1);
    printf("RESULT dot_reps %d reps\n", reps);

    int failures = 0;
    /* Variants: 0 int8 stream, 1 f32 stream, 2 int8 l1, 3 f32 l1. Samples
     * are taken round-robin, one pass of every variant per rep, so drift
     * over the run lands on every variant alike. */
    static const char *const dkey[4] = {"dot_int8_sdot_stream", "dot_f32_fmla_stream",
                                        "dot_int8_sdot_l1", "dot_f32_fmla_l1"};
    double *all = (double *)malloc(sizeof(double) * 4 * (size_t)reps);
    double maxrel[4] = {0, 0, 0, 0};
    int skipped[4] = {0, 0, 0, 0};
    for (int r = -1; r < reps; r++) {
        for (int v = 0; v < 4; v++) {
            const int is_int = (v % 2 == 0), is_l1 = (v >= 2);
            const size_t n = is_l1 ? n_l1 : n_stream;
            const size_t passes = is_l1 ? passes_l1 : 1;
            const double ops = 2.0 * (double)n * (double)passes;
            uint64_t t0, t1;
            if (is_int) {
#if HAVE_SDOT
                int64_t acc = 0;
                const int8_t *pa = is_l1 ? ia_l1 : ia, *pb = is_l1 ? ib_l1 : ib;
                t0 = now_ns();
                for (size_t p = 0; p < passes; p++) {
                    OPAQUE(pa, pb);
                    int64_t d = dot_int8_sdot(pa, pb, n);
                    SINK(d);
                    acc += d;
                }
                t1 = now_ns();
                int64_t want = (is_l1 ? iref_l1 : iref_stream) * (int64_t)passes;
                if (acc != want) {
                    failures++;
                    fprintf(stderr, "MISMATCH %s: got %" PRId64 " want %" PRId64 "\n", dkey[v], acc, want);
                }
#else
                skipped[v] = 1;
                continue;
#endif
            } else {
                double acc = 0;
                const float *pa = is_l1 ? fa_l1 : fa, *pb = is_l1 ? fb_l1 : fb;
                t0 = now_ns();
                for (size_t p = 0; p < passes; p++) {
                    OPAQUE(pa, pb);
                    float d = dot_f32_fmla(pa, pb, n);
                    SINK(d);
                    acc += (double)d;
                }
                t1 = now_ns();
                double want = (is_l1 ? fref_l1 : fref_stream) * (double)passes;
                double tol = 1e-5 * (is_l1 ? fabs_l1 : fabs_stream) * (double)passes;
                double rel = fabs(acc - want) / tol;
                if (rel > maxrel[v]) maxrel[v] = rel;
                if (fabs(acc - want) > tol) {
                    failures++;
                    fprintf(stderr, "MISMATCH %s: got %.6f want %.6f tolerance %.6f\n", dkey[v], acc, want, tol);
                }
            }
            if (r >= 0) all[v * reps + r] = ops / (double)(t1 - t0);
        }
    }
    for (int v = 0; v < 4; v++) {
        if (skipped[v]) {
            printf("%-40s skipped: this build has no sdot (needs __ARM_FEATURE_DOTPROD)\n", dkey[v]);
            printf("RESULT %s_skipped 1 flag\n", dkey[v]);
            continue;
        }
        stats_t s = stats(all + v * reps, reps);
        print_row(dkey[v], s, "ops/ns");
        printf("RESULT %s_opsns %.3f ops/ns\n", dkey[v], s.median);
        printf("RESULT %s_cv %.2f percent\n", dkey[v], 100.0 * s.cv);
        if (v % 2 == 1) printf("RESULT %s_err_over_tol %.3f fraction\n", dkey[v], maxrel[v]);
    }
    free(all);
    free(il1);
    free(fl1);
    free(ia);
    free(ib);
    free(fa);
    free(fb);
    if (failures) {
        printf("FAIL: %d dot product mismatches\n", failures);
        exit(1);
    }
    printf("dot: every result matched its reference\n");
}
#endif

int main(void) {
    const int quick = env_int("QUICK", 0);
    const int reps = env_int("REPS", quick ? 5 : 11);
    const int dot_reps = env_int("DOT_REPS", quick ? 5 : 31);
    const char *mode = getenv("MODE");
    const int blas_only = (mode && strcmp(mode, "blas") == 0);
    if (reps < 3 || dot_reps < 3) {
        fprintf(stderr, "REPS and DOT_REPS must be at least 3\n");
        return 1;
    }
    gemm_table(quick, reps, blas_only);
    if (blas_only) return 0;
#if HAVE_NEON
    dot_table(quick, dot_reps);
#else
    (void)dot_reps;
    printf("dot table skipped: this build has no NEON\n");
#endif
    return 0;
}
