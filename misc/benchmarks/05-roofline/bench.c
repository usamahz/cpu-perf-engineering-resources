/* 06-roofline: which roof binds a loop is set by its arithmetic intensity.
 *
 * The roofs are measured first. The compute roof is the NEON FMA rate of
 * twenty independent accumulator chains fed from a 4 KiB buffer that never
 * leaves L1; the textbook sixteen chains are measured beside it and fall
 * short on this core, so the roof is the larger figure. There are three
 * memory roofs, one per stream mix, because on one core the write path is
 * not the read path and two read streams are not one: a float32 sum
 * streams a 512 MiB array, thirty-two times the 16 MiB L2 of a P-core
 * cluster, and is the roof for a loop that only reads; a copy of 256 MiB
 * into another 256 MiB is the roof for a loop that reads one array and
 * writes one; a triad over three 256 MiB arrays is the roof for a loop
 * that reads two and writes one. A write-only fill of 256 MiB is measured
 * beside them because it decides whether a write-allocate read has to be
 * counted in the bytes a storing loop moves. Every roof is taken on one
 * thread and on one thread per P-core, and taken twice, before and after
 * the kernels at that thread count; the higher median is the roof, since a
 * roof is a ceiling and the first case of a run is the one most exposed to
 * the clock still ramping, so the FMA loop also spins untimed before it.
 *
 * Five kernels of known intensity then run over 256 MiB of float32 and
 * their GFLOP/s is set against min(peak_flops, intensity * bandwidth), the
 * roofline prediction, with the bandwidth taken from the roof whose stream
 * mix matches the loop. Each kernel also runs over a slice that stays in
 * L1 (16 KiB of x and of y, or 64 KiB of x for fma_stream), which is the
 * rate the core alone allows that instruction mix: a kernel that runs no
 * faster from L1 than from DRAM is compute bound whatever the model says,
 * and one that runs far faster from L1 is bound by memory.
 *
 * Intensity counts one flop per multiply or add (an FMA is two) over the
 * bytes the loop moves past the caches. saxpy reads x and y and writes y,
 * 12 bytes for 2 flops. The stencil reads 4 bytes of in and writes 4 of
 * out for 13 flops; its six other loads per element hit L1. fma_stream is
 * the roof's own loop, the same function, with its load walking x instead
 * of the 4 KiB buffer, 40 flops over 4 bytes. The polynomial reads 4 bytes
 * for 64 flops and stores nothing; it is measured twice, once with the
 * Horner step as two instructions of inline asm and once as the plain
 * intrinsic, because the code clang emits for the two differs. No
 * write-allocate read is counted, because the fill row shows that this
 * part does not fetch a line it is about to overwrite whole.
 *
 * Every kernel is NEON so the flop count is exact and does not depend on
 * what the vectoriser chose to do. The scalar fallback exists so the file
 * compiles elsewhere; its rates are not roofs.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* clock_gettime and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif
#if defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#define HAVE_NEON 1
#else
#define HAVE_NEON 0
#endif

#define NOINLINE __attribute__((noinline))
#define ACC 20          /* FMA chains for the roof: 4 pipes x 4 cycles of latency, plus slack */
#define ACC16 16        /* the textbook count, measured as the ILP ceiling */
#define SUMACC 16       /* independent add chains for the bandwidth sum */
#define PCH 12          /* Horner chains: 12 x and 12 p vectors fill the 32 NEON registers */
#define DEG 32          /* polynomial degree, so 32 FMAs per element */
#define L1_FLOATS 1024  /* 4 KiB, the FMA roof's operand, resident in L1 */
#define L1N 4096        /* floats per L1-resident kernel slice: 16 KiB of x and 16 KiB of y */
#define FS_L1N (4 * L1N) /* fma_stream's L1 slice, 64 KiB of x alone, one block; a shorter block drains the chains too often */
#define SBLK (FS_L1N / 4) /* float32x4 vectors per fma_stream block */
#define MAX_THREADS 64

enum { K_FMA, K_FMA16, K_SUM, K_COPY, K_TRIAD, K_FILL, K_SAXPY, K_STENCIL, K_FSTREAM, K_POLY, K_POLYI, K_COUNT };
static const char *const kname[K_COUNT] = {
    "fma_peak", "fma_16acc", "read_bw", "copy_bw", "triad_bw", "write_bw",
    "saxpy", "stencil", "fma_stream", "poly", "poly_intrin"
};
enum { MODE_DRAM, MODE_L1 };

static float *g_big;             /* the read-bandwidth array; x and y are its two halves */
static float *g_x, *g_y;         /* kernel operands, 256 MiB each */
static float *g_z;               /* the triad's output, 256 MiB */
static size_t g_nbig, g_n;
static uint64_t g_fma_iters;
static int g_l1_passes;
static float g_coef[DEG + 1];    /* polynomial coefficients */
static float g_tap[7];           /* stencil taps */
static float g_l1[L1_FLOATS] __attribute__((aligned(128)));
static const float g_a = 0.5f;   /* saxpy scalar */
static const float g_fillv = 0.375f;
static const float g_fsc = 0.25f; /* FMA loop scale: a power of two, so x * g_fsc is exact */

/* Data are multiples of 1/8 so that the sum, copy, saxpy, stencil and
 * fma_stream results are exact in float32 and a double-precision reference
 * matches them exactly; the polynomial is checked to a tolerance instead. */
static float xval(size_t i) { return (float)((i * 5) & 7) / 8.0f; }
static float yval(size_t i) { return (float)((i * 3 + 1) & 7) / 8.0f; }

#if HAVE_NEON
/* Reduce four lanes in double so the reduction adds no float32 rounding. */
static inline double hsum_d(float32x4_t v) {
    float t[4];
    vst1q_f32(t, v);
    return ((double)t[0] + (double)t[1]) + ((double)t[2] + (double)t[3]);
}

/* One Horner step, p' = c + p * x. fmla accumulates into its destination,
 * so the coefficient has to be copied into a fresh register first; this is
 * the whole cost the ISA imposes on Horner. k_poly_intrin does the same
 * step as vfmaq_f32 and the README shows what clang makes of each. */
static inline float32x4_t horner_step(float32x4_t c, float32x4_t p, float32x4_t x) {
    float32x4_t r;
    __asm__("mov %0.16b, %1.16b\n\tfmla %0.4s, %2.4s, %3.4s"
            : "=&w"(r) : "w"(c), "w"(p), "w"(x));
    return r;
}
#endif

/* The FMA loop: N accumulator chains, one load per N FMAs, from
 * base + ((i * 4) & mask). With mask = L1_FLOATS - 1 the load cycles a
 * 4 KiB buffer and the loop is the compute roof; with mask = SIZE_MAX the
 * same instructions walk memory and the loop is the fma_stream kernel, so
 * the two differ only in where the load goes. The accumulators start at
 * distinct multiples of 1/32 so no two chains are the same expression and
 * the optimiser cannot merge them, and the scale is 1/4, so that over one
 * 64 KiB block of x (multiples of 1/8) every partial sum is a multiple of
 * 1/32 below 2^19 and exact in float32; the pairwise reduction is exact
 * for the same reason and one hsum_d finishes it. */
#if HAVE_NEON
#define FMA_KERNEL(NAME, N)                                                     \
    NOINLINE static double NAME(const float *base, size_t mask, uint64_t iters) { \
        float32x4_t acc[N];                                                     \
        for (int k = 0; k < N; k++) acc[k] = vdupq_n_f32((float)(k + 1) / 32.0f); \
        const float32x4_t cv = vdupq_n_f32(g_fsc);                              \
        for (uint64_t i = 0; i < iters; i++) {                                  \
            const float32x4_t v = vld1q_f32(base + ((i * 4) & mask));           \
            for (int k = 0; k < N; k++) acc[k] = vfmaq_f32(acc[k], v, cv);      \
        }                                                                       \
        for (int w = N; w > 1; w = (w + 1) / 2)                                 \
            for (int k = 0; k < w / 2; k++) acc[k] = vaddq_f32(acc[k], acc[w - 1 - k]); \
        return hsum_d(acc[0]);                                                  \
    }
#else
#define FMA_KERNEL(NAME, N)                                                     \
    NOINLINE static double NAME(const float *base, size_t mask, uint64_t iters) { \
        float acc[N * 4];                                                       \
        for (int k = 0; k < N * 4; k++) acc[k] = (float)(k / 4 + 1) / 32.0f;    \
        for (uint64_t i = 0; i < iters; i++) {                                  \
            const float *v = base + ((i * 4) & mask);                           \
            for (int k = 0; k < N * 4; k++) acc[k] = acc[k] + v[k & 3] * g_fsc;  \
        }                                                                       \
        double s = 0;                                                           \
        for (int k = 0; k < N * 4; k++) s += (double)acc[k];                    \
        return s;                                                               \
    }
#endif
FMA_KERNEL(k_fma, ACC)
FMA_KERNEL(k_fma16, ACC16)

/* Read roof: sum a[lo, hi) with SUMACC accumulators so the add latency
 * never limits the load stream. hi - lo is a multiple of 4 * SUMACC. */
NOINLINE static double k_sum(const float *a, size_t lo, size_t hi) {
#if HAVE_NEON
    float32x4_t acc[SUMACC];
    for (int k = 0; k < SUMACC; k++) acc[k] = vdupq_n_f32(0.0f);
    for (size_t i = lo; i + 4 * SUMACC <= hi; i += 4 * SUMACC)
        for (int k = 0; k < SUMACC; k++) acc[k] = vaddq_f32(acc[k], vld1q_f32(a + i + 4 * k));
    double s = 0;
    for (int k = 0; k < SUMACC; k++) s += hsum_d(acc[k]);
    return s;
#else
    float acc[SUMACC] = { 0 };
    for (size_t i = lo; i + SUMACC <= hi; i += SUMACC)
        for (int k = 0; k < SUMACC; k++) acc[k] += a[i + k];
    double s = 0;
    for (int k = 0; k < SUMACC; k++) s += (double)acc[k];
    return s;
#endif
}

/* Copy roof: one load stream and one store stream, 8 bytes per element.
 * The empty asm stops clang replacing the loop with a memcpy call, so the
 * streams measured are this loop's own loads and stores. */
NOINLINE static void k_copy(const float *restrict x, float *restrict y, size_t lo, size_t hi) {
#if HAVE_NEON
    for (size_t i = lo; i + 16 <= hi; i += 16) {
        vst1q_f32(y + i,      vld1q_f32(x + i));
        vst1q_f32(y + i + 4,  vld1q_f32(x + i + 4));
        vst1q_f32(y + i + 8,  vld1q_f32(x + i + 8));
        vst1q_f32(y + i + 12, vld1q_f32(x + i + 12));
        CLOBBER();
    }
#else
    for (size_t i = lo; i < hi; i++) { y[i] = x[i]; CLOBBER(); }
#endif
}

/* Triad roof: z = x + a * y, two load streams and one store stream, 12
 * bytes per element. This is saxpy's stream mix, but out of place: the
 * stored lines are not the ones just loaded. */
NOINLINE static void k_triad(const float *restrict x, const float *restrict y, float *restrict z, size_t lo, size_t hi, float a) {
#if HAVE_NEON
    const float32x4_t av = vdupq_n_f32(a);
    for (size_t i = lo; i + 16 <= hi; i += 16) {
        vst1q_f32(z + i,      vfmaq_f32(vld1q_f32(x + i),      vld1q_f32(y + i),      av));
        vst1q_f32(z + i + 4,  vfmaq_f32(vld1q_f32(x + i + 4),  vld1q_f32(y + i + 4),  av));
        vst1q_f32(z + i + 8,  vfmaq_f32(vld1q_f32(x + i + 8),  vld1q_f32(y + i + 8),  av));
        vst1q_f32(z + i + 12, vfmaq_f32(vld1q_f32(x + i + 12), vld1q_f32(y + i + 12), av));
    }
#else
    for (size_t i = lo; i < hi; i++) z[i] = x[i] + a * y[i];
#endif
}

/* Write roof: a store stream with no loads, 4 bytes per element. The empty
 * asm stops clang turning the loop into a memset_pattern16 call. */
NOINLINE static void k_fill(float *y, size_t lo, size_t hi, float v) {
#if HAVE_NEON
    const float32x4_t vv = vdupq_n_f32(v);
    for (size_t i = lo; i + 16 <= hi; i += 16) {
        vst1q_f32(y + i, vv);
        vst1q_f32(y + i + 4, vv);
        vst1q_f32(y + i + 8, vv);
        vst1q_f32(y + i + 12, vv);
        CLOBBER();
    }
#else
    for (size_t i = lo; i < hi; i++) { y[i] = v; CLOBBER(); }
#endif
}

/* saxpy: 2 flops per element, 12 bytes (read x, read y, write y). */
NOINLINE static void k_saxpy(const float *restrict x, float *restrict y, size_t lo, size_t hi, float a) {
#if HAVE_NEON
    const float32x4_t av = vdupq_n_f32(a);
    for (size_t i = lo; i + 16 <= hi; i += 16) {
        vst1q_f32(y + i,      vfmaq_f32(vld1q_f32(y + i),      vld1q_f32(x + i),      av));
        vst1q_f32(y + i + 4,  vfmaq_f32(vld1q_f32(y + i + 4),  vld1q_f32(x + i + 4),  av));
        vst1q_f32(y + i + 8,  vfmaq_f32(vld1q_f32(y + i + 8),  vld1q_f32(x + i + 8),  av));
        vst1q_f32(y + i + 12, vfmaq_f32(vld1q_f32(y + i + 12), vld1q_f32(x + i + 12), av));
    }
#else
    for (size_t i = lo; i < hi; i++) y[i] = a * x[i] + y[i];
#endif
}

/* Seven-point stencil: out[i] = sum_k tap[k] * in[i + k - 3], one multiply
 * and six FMAs, 13 flops per element. Seven loads per four elements, all
 * but one of them L1 hits, and one store. lo >= 4 and hi <= n - 4 keep the
 * shifted loads in bounds. */
NOINLINE static void k_stencil(const float *restrict in, float *restrict out, size_t lo, size_t hi) {
#if HAVE_NEON
    const float32x4_t c0 = vdupq_n_f32(g_tap[0]), c1 = vdupq_n_f32(g_tap[1]);
    const float32x4_t c2 = vdupq_n_f32(g_tap[2]), c3 = vdupq_n_f32(g_tap[3]);
    const float32x4_t c4 = vdupq_n_f32(g_tap[4]), c5 = vdupq_n_f32(g_tap[5]);
    const float32x4_t c6 = vdupq_n_f32(g_tap[6]);
    for (size_t i = lo; i + 4 <= hi; i += 4) {
        float32x4_t r = vmulq_f32(c0, vld1q_f32(in + i - 3));
        r = vfmaq_f32(r, c1, vld1q_f32(in + i - 2));
        r = vfmaq_f32(r, c2, vld1q_f32(in + i - 1));
        r = vfmaq_f32(r, c3, vld1q_f32(in + i));
        r = vfmaq_f32(r, c4, vld1q_f32(in + i + 1));
        r = vfmaq_f32(r, c5, vld1q_f32(in + i + 2));
        r = vfmaq_f32(r, c6, vld1q_f32(in + i + 3));
        vst1q_f32(out + i, r);
    }
#else
    for (size_t i = lo; i < hi; i++) {
        float r = g_tap[0] * in[i - 3];
        for (int k = 1; k < 7; k++) r += g_tap[k] * in[i + k - 3];
        out[i] = r;
    }
#endif
}

/* The roof's own loop fed from memory: k_fma with the mask open, so its
 * load walks x, 40 flops over the 4 bytes read. It is called once per
 * 64 KiB block so the float32 sums stay exact and each block's result is
 * added in double; the block is long enough that draining the twenty
 * chains at its end costs under one percent. hi - lo is a multiple of
 * 4 * SBLK, which measure() arranges. */
static double k_fma_stream(const float *x, size_t lo, size_t hi) {
    double s = 0;
    for (size_t i = lo; i + 4 * SBLK <= hi; i += 4 * SBLK) s += k_fma(x + i, SIZE_MAX, SBLK);
    return s;
}

/* Degree-32 polynomial by Horner on every element, summed: 32 FMAs per
 * element, 64 flops over the 4 bytes read; the reduction adds are not
 * counted. PCH vectors are in flight at once so the 32-deep dependent chain
 * of each does not bound the rate; the coefficient loop is unrolled by two
 * so the register allocator can reuse the dead register instead of
 * rotating. The sum keeps the traffic read-only, which is what puts the
 * intensity in the tens; a stored result would halve it. hi - lo is a
 * multiple of 4; the tail after the last full block runs one chain. STEP
 * is the Horner step: the inline asm for k_poly, vfmaq_f32 for
 * k_poly_intrin, which computes the same thing. */
#if HAVE_NEON
#define HORNER_KERNEL(NAME, STEP)                                               \
    NOINLINE static double NAME(const float *x, size_t lo, size_t hi) {         \
        double s = 0;                                                           \
        size_t i = lo;                                                          \
        for (; i + 4 * PCH <= hi; i += 4 * PCH) {                               \
            float32x4_t xv[PCH], p[PCH];                                        \
            const float32x4_t top = vdupq_n_f32(g_coef[DEG]);                   \
            for (int k = 0; k < PCH; k++) { xv[k] = vld1q_f32(x + i + 4 * k); p[k] = top; } \
            for (int j = DEG - 1; j >= 1; j -= 2) {                             \
                const float32x4_t ca = vdupq_n_f32(g_coef[j]);                  \
                const float32x4_t cb = vdupq_n_f32(g_coef[j - 1]);              \
                for (int k = 0; k < PCH; k++) {                                 \
                    float32x4_t t = STEP(ca, p[k], xv[k]);                      \
                    p[k] = STEP(cb, t, xv[k]);                                  \
                }                                                               \
            }                                                                   \
            float32x4_t r = p[0];                                               \
            for (int k = 1; k < PCH; k++) r = vaddq_f32(r, p[k]);               \
            s += hsum_d(r);                                                     \
        }                                                                       \
        for (; i + 4 <= hi; i += 4) {                                           \
            float32x4_t xv = vld1q_f32(x + i), p = vdupq_n_f32(g_coef[DEG]);    \
            for (int j = DEG - 1; j >= 0; j--) p = STEP(vdupq_n_f32(g_coef[j]), p, xv); \
            s += hsum_d(p);                                                     \
        }                                                                       \
        return s;                                                               \
    }
#else
#define HORNER_KERNEL(NAME, STEP)                                               \
    NOINLINE static double NAME(const float *x, size_t lo, size_t hi) {         \
        double s = 0;                                                           \
        for (size_t i = lo; i < hi; i++) {                                      \
            float xv = x[i], p = g_coef[DEG];                                   \
            for (int j = DEG - 1; j >= 0; j--) p = g_coef[j] + p * xv;          \
            s += (double)p;                                                     \
        }                                                                       \
        return s;                                                               \
    }
#endif
HORNER_KERNEL(k_poly, horner_step)
HORNER_KERNEL(k_poly_intrin, vfmaq_f32)

/* One thread per P-core, no pinning (macOS has none); every thread asks for
 * user-interactive QoS so the scheduler prefers P-cores. A spin barrier
 * gives a sharp common start; the master thread is worker 0, so a
 * ten-thread case has exactly ten threads and nothing else to schedule. */
typedef struct {
    _Atomic int count;
    _Atomic int gen;
    int n;
} barrier_t;

static void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}

static void barrier_wait(barrier_t *b) {
    int gen = atomic_load_explicit(&b->gen, memory_order_acquire);
    if (atomic_fetch_add_explicit(&b->count, 1, memory_order_acq_rel) == b->n - 1) {
        atomic_store_explicit(&b->count, 0, memory_order_relaxed);
        atomic_store_explicit(&b->gen, gen + 1, memory_order_release);
    } else {
        while (atomic_load_explicit(&b->gen, memory_order_acquire) == gen) cpu_relax();
    }
}

static void want_pcore(void) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

/* One per thread, each on its own cache line so the out fields never share. */
typedef struct {
    int id;
    size_t lo, hi;
    double out;
} __attribute__((aligned(128))) worker_t;

static worker_t g_w[MAX_THREADS];
static barrier_t g_bar;
static int g_kind, g_mode, g_passes; /* g_passes: kernel calls per timed pass, more than one from L1 */
static _Atomic int g_quit;

static void run_slice(worker_t *w) {
    double out = 0;
    for (int p = 0; p < g_passes; p++) {
        /* The read-only kernels are pure functions of global memory and
         * loop-invariant bounds; the barrier tells the compiler memory may
         * have changed, so it cannot hoist one call out of the pass loop. */
        CLOBBER();
        switch (g_kind) {
        case K_FMA:     out += k_fma(g_l1, L1_FLOATS - 1, g_fma_iters); break;
        case K_FMA16:   out += k_fma16(g_l1, L1_FLOATS - 1, g_fma_iters); break;
        case K_SUM:     out += k_sum(g_big, w->lo, w->hi); break;
        case K_COPY:    k_copy(g_x, g_y, w->lo, w->hi); break;
        case K_TRIAD:   k_triad(g_x, g_y, g_z, w->lo, w->hi, g_a); break;
        case K_FILL:    k_fill(g_y, w->lo, w->hi, g_fillv); break;
        case K_SAXPY:   k_saxpy(g_x, g_y, w->lo, w->hi, g_a); break;
        case K_STENCIL: k_stencil(g_x, g_y, w->lo, w->hi); break;
        case K_FSTREAM: out += k_fma_stream(g_x, w->lo, w->hi); break;
        case K_POLY:    out += k_poly(g_x, w->lo, w->hi); break;
        case K_POLYI:   out += k_poly_intrin(g_x, w->lo, w->hi); break;
        default: break;
        }
    }
    w->out = out;
}

static void *worker_main(void *arg) {
    worker_t *w = (worker_t *)arg;
    want_pcore();
    for (;;) {
        barrier_wait(&g_bar);
        if (atomic_load_explicit(&g_quit, memory_order_acquire)) break;
        run_slice(w);
        barrier_wait(&g_bar);
    }
    return NULL;
}

/* Double-precision references, computed from the value formulas and the
 * coefficients rather than from the arrays or the kernels. */
static double dsum(const float *a, size_t lo, size_t hi) {
    double s = 0;
    for (size_t i = lo; i < hi; i++) s += (double)a[i];
    return s;
}
static double ref_x(size_t lo, size_t hi) {
    double s = 0;
    for (size_t i = lo; i < hi; i++) s += (double)xval(i);
    return s;
}
static double ref_y0(size_t lo, size_t hi) {
    double s = 0;
    for (size_t i = lo; i < hi; i++) s += (double)yval(i);
    return s;
}
/* Sum of y after the stencil over one region: y0 at the four edge elements
 * each side, the stencil of x in between. */
static double ref_stencil(size_t base, size_t len) {
    double s = ref_y0(base, base + 4) + ref_y0(base + len - 4, base + len);
    for (size_t i = base + 4; i < base + len - 4; i++) {
        double r = 0;
        for (int k = 0; k < 7; k++) r += (double)g_tap[k] * (double)xval(i + k - 3);
        s += r;
    }
    return s;
}
/* fma_stream over [0, span): every block adds its twenty starting values
 * (four lanes each) and ACC times the scaled sum of its x. */
static double ref_fma_stream(size_t span) {
    double start = 0;
    for (int k = 0; k < ACC; k++) start += 4.0 * (double)(k + 1) / 32.0;
    return (double)(span / (4 * SBLK)) * start + (double)ACC * (double)g_fsc * ref_x(0, span);
}
/* x takes eight values, so the polynomial sum is a histogram times eight
 * double-precision Horner evaluations. */
static double ref_poly(size_t lo, size_t hi) {
    double hist[8] = { 0 }, s = 0;
    for (size_t i = lo; i < hi; i++) hist[(i * 5) & 7] += 1.0;
    for (int v = 0; v < 8; v++) {
        double xv = (double)v / 8.0, p = (double)g_coef[DEG];
        for (int j = DEG - 1; j >= 0; j--) p = (double)g_coef[j] + p * xv;
        s += hist[v] * p;
    }
    return s;
}

static void init_y(void) {
    for (size_t i = 0; i < g_n; i++) g_y[i] = yval(i);
}

static int g_mismatches = 0;
static double g_fma_ref[2] = { 0, 0 }; /* single-thread checksums of the two FMA kernels */

typedef struct {
    double median, min, max, cv;
    int warmups;
} rate_t;

/* Run one kernel on T threads: untimed passes until warm_ns have elapsed
 * (at least one), then reps timed ones, median and cv of the rate. Every
 * result is consumed with SINK() and compared with a reference computed
 * another way. tag, if given, names one of the two roof measurements. */
static rate_t measure(int kind, int mode, int T, int reps, double warm_ns, const char *tag) {
    const size_t n = (kind == K_SUM) ? g_nbig : g_n;
    /* The L1 slice is 16 KiB of x and 16 KiB of y per thread, or 64 KiB of
     * x alone for fma_stream, whose block is that long; its passes are
     * fewer so the work per timed pass is the same. DRAM slices are
     * multiples of 64 KiB, which every kernel's block divides, and the last
     * thread takes the remainder, also a multiple because n is. */
    const size_t slice = (kind == K_FSTREAM) ? (size_t)FS_L1N : (size_t)L1N;
    const int passes = (mode == MODE_L1) ? ((kind == K_FSTREAM) ? g_l1_passes / 4 : g_l1_passes) : 1;
    const size_t chunk = (mode == MODE_L1) ? slice : (n / (size_t)T) & ~(size_t)(FS_L1N - 1);
    for (int t = 0; t < T; t++) {
        g_w[t].id = t;
        g_w[t].lo = (size_t)t * chunk;
        g_w[t].hi = (mode == MODE_DRAM && t == T - 1) ? n : g_w[t].lo + chunk;
        if (kind == K_STENCIL) {
            /* Each L1 slice keeps its own four-element edges; the DRAM
             * chunks abut, so only the ends of the whole array are trimmed. */
            if (mode == MODE_L1) { g_w[t].lo += 4; g_w[t].hi -= 4; }
            else {
                if (g_w[t].lo < 4) g_w[t].lo = 4;
                if (g_w[t].hi > n - 4) g_w[t].hi = n - 4;
            }
        }
        g_w[t].out = 0;
    }
    if (kind == K_SUM || kind == K_COPY || kind == K_TRIAD || kind == K_FILL || kind == K_SAXPY || kind == K_STENCIL) init_y();
    g_bar.n = T;
    atomic_store(&g_bar.count, 0);
    atomic_store(&g_bar.gen, 0);
    atomic_store(&g_quit, 0);
    g_kind = kind;
    g_mode = mode;
    g_passes = passes;

    pthread_t th[MAX_THREADS];
    for (int t = 1; t < T; t++)
        if (pthread_create(&th[t], NULL, worker_main, &g_w[t]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }

    /* Work per timed pass: flops, or bytes for the bandwidth roofs. */
    const double elems = (mode == MODE_L1) ? (double)T * passes * slice : (double)n;
    const double stencil_elems = (mode == MODE_L1) ? (double)T * passes * (L1N - 8) : (double)(n - 8);
    double work;
    const char *unit = "GFLOP/s";
    switch (kind) {
    case K_FMA:     work = (double)T * (double)g_fma_iters * ACC * 8.0; break;
    case K_FMA16:   work = (double)T * (double)g_fma_iters * ACC16 * 8.0; break;
    case K_SUM:     work = (double)g_nbig * sizeof(float); unit = "GB/s"; break;
    case K_COPY:    work = (double)g_n * 2.0 * sizeof(float); unit = "GB/s"; break;
    case K_TRIAD:   work = (double)g_n * 3.0 * sizeof(float); unit = "GB/s"; break;
    case K_FILL:    work = (double)g_n * sizeof(float); unit = "GB/s"; break;
    case K_SAXPY:   work = elems * 2.0; break;
    case K_STENCIL: work = stencil_elems * 13.0; break;
    case K_FSTREAM: work = elems * 2.0 * ACC; break;
    default:        work = elems * 2.0 * DEG; break;
    }

    double *rate = (double *)malloc(sizeof(double) * (size_t)reps);
    double cs = 0;
    int warm = 0;
    const uint64_t warm_start = now_ns();
    for (int r = 0; r < reps;) {
        uint64_t t0 = now_ns();
        barrier_wait(&g_bar);
        run_slice(&g_w[0]);
        barrier_wait(&g_bar);
        uint64_t t1 = now_ns();
        cs = 0;
        for (int t = 0; t < T; t++) cs += g_w[t].out;
        SINK(cs);
        if (warm == 0 || (double)(t1 - warm_start) < warm_ns) { warm++; continue; }
        rate[r++] = work / (double)(t1 - t0); /* per ns = G per s */
    }
    atomic_store_explicit(&g_quit, 1, memory_order_release);
    barrier_wait(&g_bar);
    for (int t = 1; t < T; t++) pthread_join(th[t], NULL);

    /* Reference for the checksum. The in-place kernels leave their result
     * in y, which is summed here, untimed, in double. */
    const size_t span = (mode == MODE_L1) ? (size_t)T * slice : n;
    const double total_passes = (double)(warm + reps) * passes;
    double ref = 0, tol = 1e-9;
    switch (kind) {
    case K_FMA:
    case K_FMA16:
        if (T == 1) { g_fma_ref[kind] = cs; ref = cs; }
        else ref = (double)T * g_fma_ref[kind];
        break;
    case K_SUM:     ref = ref_x(0, g_n) + ref_y0(0, g_n); break;
    case K_COPY:    cs = dsum(g_y, 0, g_n); ref = ref_x(0, g_n); break;
    case K_TRIAD:   cs = dsum(g_z, 0, g_n); ref = ref_x(0, g_n) + (double)g_a * ref_y0(0, g_n); break;
    case K_FILL:    cs = dsum(g_y, 0, g_n); ref = (double)g_n * (double)g_fillv; break;
    case K_SAXPY:
        cs = dsum(g_y, 0, span);
        ref = ref_y0(0, span) + total_passes * (double)g_a * ref_x(0, span);
        break;
    case K_STENCIL:
        cs = dsum(g_y, 0, span);
        if (mode == MODE_L1) for (int t = 0; t < T; t++) ref += ref_stencil((size_t)t * L1N, L1N);
        else ref = ref_stencil(0, n);
        break;
    case K_FSTREAM:
        /* Returned per timed pass, not accumulated, like the polynomials. */
        ref = (double)passes * ref_fma_stream(span);
        break;
    default:
        ref = (double)passes * ref_poly(0, span);
        tol = 1e-4; /* float32 Horner against double Horner */
        break;
    }
    SINK(cs);
    double relerr = (ref != 0) ? fabs(cs - ref) / fabs(ref) : fabs(cs - ref);
    if (relerr > tol) {
        g_mismatches++;
        fprintf(stderr, "MISMATCH %s: got %.10g want %.10g\n", kname[kind], cs, ref);
    }

    stats_t s = stats(rate, reps);
    char label[96], key[64];
    snprintf(label, sizeof label, "%s%s %d thread%s%s%s%s", kname[kind], mode == MODE_L1 ? " from L1" : "",
             T, T == 1 ? "" : "s", tag ? " (" : "", tag ? tag : "", tag ? " the kernels)" : "");
    snprintf(key, sizeof key, "%s%s_%dt%s%s", kname[kind], mode == MODE_L1 ? "_l1" : "", T,
             tag ? "_" : "", tag ? tag : "");
    print_row(label, s, unit);
    printf("RESULT %s %.3f %s\n", key, s.median, unit);
    printf("RESULT %s_min %.3f %s\n", key, s.min, unit);
    printf("RESULT %s_max %.3f %s\n", key, s.max, unit);
    printf("RESULT %s_cv %.2f percent\n", key, 100.0 * s.cv);
    printf("RESULT %s_warmups %d passes\n", key, warm);
    printf("checksum %s %.10g reference %.10g relative error %.1e\n", key, cs, ref, relerr);
    free(rate);
    rate_t out = { s.median, s.min, s.max, s.cv, warm };
    return out;
}

static int default_threads(void) {
    int t = 0;
#if defined(__APPLE__)
    int v = 0;
    size_t len = sizeof v;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &v, &len, NULL, 0) == 0 && v > 0) t = v;
#endif
    if (t <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        t = n > 0 ? (int)n : 1;
    }
    return t;
}

static float *alloc_floats(size_t n) {
    void *p = NULL;
    if (posix_memalign(&p, 16384, n * sizeof(float)) != 0 || !p) {
        fprintf(stderr, "allocation of %zu bytes failed\n", n * sizeof(float));
        exit(1);
    }
    return (float *)p;
}

static double env_double(const char *name, double dflt) {
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : dflt;
}

/* Spin the FMA roof loop untimed for spin_ns before anything is measured,
 * so the clock has ramped before the first roof, which is otherwise the
 * coldest case of the run. */
static void prespin(double spin_ns) {
    const uint64_t t0 = now_ns();
    double s = 0;
    int passes = 0;
    do { s += k_fma(g_l1, L1_FLOATS - 1, g_fma_iters); passes++; } while ((double)(now_ns() - t0) < spin_ns);
    SINK(s);
    printf("warm-up: %d untimed FMA passes over %.0f ms before the first roof\n", passes, (double)(now_ns() - t0) / 1e6);
}

/* The roofs, in the order they are measured, and the kernels. */
#define NROOF 6
static const int roofs[NROOF] = { K_FMA, K_FMA16, K_SUM, K_COPY, K_TRIAD, K_FILL };
enum { R_PEAK, R_PEAK16, R_READ, R_COPY, R_TRIAD, R_FILL }; /* indices into roofs[] */
#define NKERN 5
static const int kern[NKERN] = { K_SAXPY, K_STENCIL, K_FSTREAM, K_POLY, K_POLYI };
/* The memory roof whose stream mix matches each kernel: saxpy reads two
 * arrays and writes one like the triad, the stencil reads one and writes
 * one like the copy, the rest only read. */
static const int kern_bw[NKERN] = { R_TRIAD, R_COPY, R_READ, R_READ, R_READ };
static const char *const bwname[NROOF] = { "", "", "read", "copy", "triad", "" };
static const double kern_ai[NKERN] = { 2.0 / 12.0, 13.0 / 8.0, 2.0 * ACC / 4.0, 2.0 * DEG / 4.0, 2.0 * DEG / 4.0 };

int main(void) {
    const int quick = env_int("QUICK", 0);
    const int reps = env_int("REPS", quick ? 5 : 21);
    const double warm_ns = 1e6 * env_double("WARMUP_MS", 100.0);
    const double spin_ns = 1e6 * env_double("SPIN_MS", 300.0);
    const double ghz = env_double("CLOCK_GHZ", 0.0); /* run.sh passes the clock_estimate result */
    int T = env_int("THREADS", default_threads());
    if (T < 1) T = 1;
    if (T > MAX_THREADS) T = MAX_THREADS;
    if (reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }
    want_pcore();

    /* Floats per kernel array. The QUICK arrays are 64 MiB each so that even
     * ten threads, whose two clusters hold 32 MiB of L2 between them, still
     * stream from DRAM. */
    g_n = quick ? ((size_t)16 << 20) : ((size_t)64 << 20);
    g_nbig = 2 * g_n;                                        /* floats in the bandwidth array */
    /* A roof pass is kept about as long as a kernel pass (a few ms) so that
     * a thread descheduled by other work on the machine costs the two the
     * same; a barrier-synchronised pass lasts as long as its slowest thread. */
    g_fma_iters = quick ? ((uint64_t)1 << 21) : ((uint64_t)1 << 23);
    g_l1_passes = (int)(g_n / 16 / L1N);                     /* L1 work per pass is a sixteenth of the one-thread DRAM work */
    if ((size_t)T * FS_L1N > g_n) {
        fprintf(stderr, "too many threads for the L1 slices\n");
        return 1;
    }

    printf("06-roofline: kernel arrays %zu B each (x and y), bandwidth array %zu B, L1 slices %zu B of x and of y per thread x %d passes "
           "(fma_stream: %zu B of x x %d passes), FMA roof %llu iterations x %d chains (and x %d), threads 1 and %d, reps %d timed passes "
           "after at least %.0f ms of discarded warm-up passes, clock %.2f GHz, neon %d\n",
           g_n * sizeof(float), g_nbig * sizeof(float), (size_t)L1N * sizeof(float), g_l1_passes,
           (size_t)FS_L1N * sizeof(float), g_l1_passes / 4,
           (unsigned long long)g_fma_iters, ACC, ACC16, T, reps, warm_ns / 1e6, ghz, HAVE_NEON);
    if (!HAVE_NEON) printf("note: no NEON on this build, kernels run scalar and the roofs are not peak\n");
    printf("RESULT threads %d threads\n", T);
    printf("RESULT kernel_bytes %zu B\n", g_n * sizeof(float));
    printf("RESULT bandwidth_bytes %zu B\n", g_nbig * sizeof(float));
    printf("RESULT l1_slice_bytes %zu B\n", (size_t)L1N * sizeof(float));
    printf("RESULT l1_passes %d passes\n", g_l1_passes);
    printf("RESULT fma_stream_l1_slice_bytes %zu B\n", (size_t)FS_L1N * sizeof(float));
    printf("RESULT fma_stream_l1_passes %d passes\n", g_l1_passes / 4);
    printf("RESULT reps %d passes\n", reps);
    printf("RESULT warmup_ms %.0f ms\n", warm_ns / 1e6);
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);

    /* Coefficients: taps of a binomial smoother for the stencil, 1/(j+1) for
     * the polynomial, which converges on [0, 1) so no value overflows. */
    static const float taps[7] = { 1.0f / 64, 6.0f / 64, 15.0f / 64, 20.0f / 64, 15.0f / 64, 6.0f / 64, 1.0f / 64 };
    for (int j = 0; j <= DEG; j++) g_coef[j] = 1.0f / (float)(j + 1);
    for (int k = 0; k < 7; k++) g_tap[k] = taps[k];
    for (int i = 0; i < L1_FLOATS; i++) g_l1[i] = 0.5f + (float)(i & 255) / 512.0f;

    /* First touch on the main thread so every page exists before timing. */
    g_big = alloc_floats(g_nbig);
    g_x = g_big;
    g_y = g_big + g_n;
    g_z = alloc_floats(g_n);
    for (size_t i = 0; i < g_n; i++) g_x[i] = xval(i);
    init_y();
    for (size_t i = 0; i < g_n; i++) g_z[i] = 0.0f;

    for (int k = 0; k < NKERN; k++) printf("RESULT %s_ai %.4f flop/B\n", kname[kern[k]], kern_ai[k]);

    prespin(spin_ns);

    const int threads[2] = { 1, T };
    for (int ti = 0; ti < 2; ti++) {
        const int t = threads[ti];
        if (ti == 1 && t == 1) break;
        const char *ts = (t == 1) ? "" : "s";

        /* Roofs before the kernels, the kernels, the roofs again. */
        rate_t before[NROOF], after[NROOF], roof[NROOF];
        rate_t dram[NKERN], l1[NKERN];
        for (int r = 0; r < NROOF; r++) before[r] = measure(roofs[r], MODE_DRAM, t, reps, warm_ns, "before");
        for (int k = 0; k < NKERN; k++) {
            dram[k] = measure(kern[k], MODE_DRAM, t, reps, warm_ns, NULL);
            l1[k] = measure(kern[k], MODE_L1, t, reps, warm_ns, NULL);
        }
        for (int r = 0; r < NROOF; r++) after[r] = measure(roofs[r], MODE_DRAM, t, reps, warm_ns, "after");

        /* A roof is a ceiling: the measurement with the higher median is
         * the roof, and the other median is printed beside it. */
        for (int r = 0; r < NROOF; r++) {
            const int keep_after = after[r].median > before[r].median;
            roof[r] = keep_after ? after[r] : before[r];
            const rate_t other = keep_after ? before[r] : after[r];
            const char *unit = (roofs[r] == K_FMA || roofs[r] == K_FMA16) ? "GFLOP/s" : "GB/s";
            printf("roof %s %d thread%s: %.1f %s before the kernels, %.1f after, using the %s measurement\n",
                   kname[roofs[r]], t, ts, before[r].median, unit, after[r].median, keep_after ? "second" : "first");
            printf("RESULT %s_%dt %.3f %s\n", kname[roofs[r]], t, roof[r].median, unit);
            printf("RESULT %s_%dt_min %.3f %s\n", kname[roofs[r]], t, roof[r].min, unit);
            printf("RESULT %s_%dt_max %.3f %s\n", kname[roofs[r]], t, roof[r].max, unit);
            printf("RESULT %s_%dt_cv %.2f percent\n", kname[roofs[r]], t, 100.0 * roof[r].cv);
            printf("RESULT %s_%dt_other %.3f %s\n", kname[roofs[r]], t, other.median, unit);
            printf("RESULT %s_%dt_kept %s measurement\n", kname[roofs[r]], t, keep_after ? "after" : "before");
        }
        const double peak = roof[R_PEAK].median;
        printf("ridge point %d thread%s: %.2f flop/B against the read roof, %.2f against the copy roof, %.2f against the triad roof\n",
               t, ts, peak / roof[R_READ].median, peak / roof[R_COPY].median, peak / roof[R_TRIAD].median);
        for (int r = R_READ; r <= R_TRIAD; r++) printf("RESULT ridge_%s_%dt %.3f flop/B\n", bwname[r], t, peak / roof[r].median);
        if (ghz > 0) printf("RESULT fma_per_cycle_%dt %.2f fmla/cycle/core\n", t, peak / (8.0 * ghz * t));

        for (int k = 0; k < NKERN; k++) {
            const double line = kern_ai[k] * roof[kern_bw[k]].median;
            const double pred = line < peak ? line : peak;
            printf("roofline %s %d thread%s: intensity %.3f flop/B, memory line %.1f GFLOP/s from the %s roof, compute roof %.1f GFLOP/s, "
                   "prediction %.1f GFLOP/s (%s), measured %.1f from DRAM and %.1f from L1\n",
                   kname[kern[k]], t, ts, kern_ai[k], line, bwname[kern_bw[k]], peak, pred,
                   line < peak ? "memory" : "compute", dram[k].median, l1[k].median);
            printf("RESULT %s_%dt_pred %.3f GFLOP/s\n", kname[kern[k]], t, pred);
            printf("RESULT %s_%dt_roof %s roof\n", kname[kern[k]], t, line < peak ? "memory" : "compute");
            printf("RESULT %s_%dt_bwroof %s bandwidth\n", kname[kern[k]], t, bwname[kern_bw[k]]);
        }
    }
    free(g_big);
    free(g_z);
    if (g_mismatches) {
        printf("FAIL: %d checksum mismatches\n", g_mismatches);
        return 1;
    }
    printf("checksums: every kernel matched its reference\n");
    return 0;
}
