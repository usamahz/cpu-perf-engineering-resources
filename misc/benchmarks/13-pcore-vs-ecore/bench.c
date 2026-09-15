/* 14-pcore-vs-ecore: the same code on a performance core and on an
 * efficiency core of the same part.
 *
 * Three kernels with three different bottlenecks run under four
 * placements: the main thread as spawned (no QoS call; the class it
 * reports is printed), a pthread that makes no QoS call (a control for
 * the thread route itself), a pthread that asks for QOS_CLASS_BACKGROUND
 * (the efficiency-core hint), and, via run.sh, the main thread of a
 * second process launched under taskpolicy -c background, which clamps
 * the whole process to that class. The kernels are a dependent add chain
 * (retires one add per cycle, so it is a clock estimate), an L1-resident
 * NEON FMA loop (bound by the FMA pipes) and a streaming sum over 64 MiB
 * (one read stream, bound by how many misses one core can keep in flight
 * rather than by the memory system). macOS gives user space no affinity
 * API, so nothing is pinned; instead every sample records the cpu id the
 * thread was on when it started and ended, and how much of the wall time
 * the thread was actually running, so the reader can see where the
 * scheduler put the work rather than trust the hint. A different P to E
 * ratio per kernel is the point: a number without the core type and
 * clock cannot be compared with another.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime, CLOCK_MONOTONIC_RAW and sched_getcpu under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <sched.h>
#endif

#define NOINLINE __attribute__((noinline))

/* ---- kernel 1: dependent add chain, the clock estimate --------------- */

/* The same asm as ../common/clock_estimate.c, so the two estimates agree.
 * Every core this list covers retires one dependent add per cycle, so
 * adds per nanosecond is the clock in GHz for the core the thread was on. */
NOINLINE static uint64_t dep_add_chain(uint64_t n) {
    uint64_t x = 0;
    for (uint64_t i = 0; i < n; i += 8) {
#if defined(__aarch64__)
        __asm__ volatile(
            "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
            "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
            : "+r"(x));
#elif defined(__x86_64__)
        __asm__ volatile(
            "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
            "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
            : "+r"(x));
#else
        /* Any other target: the empty asm keeps each add a real dependent
         * instruction rather than letting the compiler fold the loop. */
        for (int k = 0; k < 8; k++) {
            x += 1;
            __asm__ volatile("" : "+r"(x));
        }
#endif
    }
    return x;
}

/* ---- kernel 2: L1-resident FMA loop, compute-bound -------------------- */

#if defined(__ARM_NEON)
#include <arm_neon.h>
typedef float32x4_t v4f;
static inline v4f v4_splat(float f) { return vdupq_n_f32(f); }
static inline v4f v4_load(const float *p) { return vld1q_f32(p); }
static inline v4f v4_fma(v4f acc, v4f a, v4f b) { return vfmaq_f32(acc, a, b); }
static inline void v4_store(float *p, v4f v) { vst1q_f32(p, v); }
#else
/* Hosts without NEON get the GCC vector extension and a separate multiply
 * and add. This path exists so the file compiles on x86-64; it is not a
 * measurement of FMA throughput. With v * v the same for all sixteen
 * accumulators the compiler emits one multiply and sixteen adds per
 * vector, so the flops count in report() overstates the work this path
 * does by nearly two. The result is still exact on the data used here,
 * so the checksum holds, and main() prints a warning on such a build. */
typedef float v4f __attribute__((vector_size(16)));
static inline v4f v4_splat(float f) { v4f v = {f, f, f, f}; return v; }
static inline v4f v4_load(const float *p) { v4f v; memcpy(&v, p, sizeof v); return v; }
static inline v4f v4_fma(v4f acc, v4f a, v4f b) { return acc + a * b; }
static inline void v4_store(float *p, v4f v) { memcpy(p, &v, sizeof v); }
#endif

/* Sixteen independent accumulators: enough chains to cover the FMA latency
 * on four pipes, so the loop is bound by FMA throughput and not by the
 * latency of one chain. One 16-byte load feeds sixteen FMAs, so the loop
 * is not bound by the load ports either. The accumulators start at
 * different values so no two chains compute the same thing and the
 * compiler cannot merge them. */
#define NACC 16
#define ACC_LIST \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

NOINLINE static void fma_l1(const float *x, size_t n, int passes, float *out) {
#define X(k) v4f a##k = v4_splat((float)(k));
    ACC_LIST
#undef X
    for (int p = 0; p < passes; p++) {
        for (size_t i = 0; i < n; i += 4) {
            v4f v = v4_load(x + i);
#define X(k) a##k = v4_fma(a##k, v, v);
            ACC_LIST
#undef X
        }
    }
#define X(k) v4_store(out + 4 * (k), a##k);
    ACC_LIST
#undef X
}

/* ---- kernel 3: streaming sum, bandwidth-bound ------------------------- */

/* A plain reduction that clang vectorises into widening NEON adds. The
 * array is 64 MiB, four times the P-core cluster L2 and sixteen times the
 * E-core cluster L2, so every pass streams from memory. One read stream
 * on one core does not reach the memory system's limit (section 15 moves
 * more from one P-core with three streams); what binds it is how many
 * misses the core keeps in flight and how far its prefetcher runs ahead,
 * both of which are core properties and smaller on an efficiency core. */
NOINLINE static uint64_t stream_sum(const uint32_t *a, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; i++) s += a[i];
    return s;
}

/* ---- placement and accounting helpers --------------------------------- */

/* CPU time of the calling thread. Wall time includes the slices other
 * threads got on the same core; the ratio of the two says how much of a
 * sample the thread was actually running, which matters on the shared
 * efficiency cores. */
static inline uint64_t thread_cpu_ns(void) {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return 0;
#endif
}

/* The cpu id the calling thread is on right now, or -1 if the platform
 * does not say. */
static int cpu_id(void) {
#if defined(__APPLE__)
    size_t c = 0;
    return pthread_cpu_number_np(&c) == 0 ? (int)c : -1;
#elif defined(__linux__)
    return sched_getcpu();
#else
    return -1;
#endif
}

/* How many cpu ids at the bottom of the numbering are efficiency cores,
 * or -1 if unknown. Apple numbers the efficiency cluster first; the
 * README shows the evidence for that on this part. */
static int efficiency_cpu_count(void) {
#if defined(__APPLE__)
    char name[32] = {0};
    size_t len = sizeof name - 1;
    int n = 0;
    size_t nlen = sizeof n;
    if (sysctlbyname("hw.perflevel1.name", name, &len, NULL, 0) == 0 &&
        strncmp(name, "Efficiency", 10) == 0 &&
        sysctlbyname("hw.perflevel1.logicalcpu", &n, &nlen, NULL, 0) == 0 && n > 0)
        return n;
#endif
    return -1;
}

static const char *qos_name_self(void) {
#if defined(__APPLE__)
    qos_class_t q = QOS_CLASS_UNSPECIFIED;
    int rel = 0;
    if (pthread_get_qos_class_np(pthread_self(), &q, &rel) != 0) return "unknown";
    switch (q) {
    case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
    case QOS_CLASS_USER_INITIATED: return "user-initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    default: return "unspecified";
    }
#else
    return "n/a";
#endif
}

/* Ask for the efficiency-core hint. Returns 1 if the platform has the
 * call, 0 if it does not. */
static int request_background_qos(void) {
#if defined(__APPLE__)
    return pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0) == 0;
#else
    return 0;
#endif
}

/* ---- one placement --------------------------------------------------- */

typedef struct {
    uint64_t adds;        /* dependent adds per clock sample */
    uint64_t bracket_adds; /* dependent adds per short clock bracket */
    size_t fma_floats;    /* floats in the L1-resident array */
    int fma_passes;       /* passes over that array per FMA sample */
    size_t stream_elems;  /* 32-bit elements in the streaming array */
    int reps;
    uint64_t warm_ns;     /* spin this long before timing so DVFS has settled */
    const float *xf;
    const uint32_t *xs;
    uint64_t stream_ref;
    double fma_ref[NACC * 4];
    int ecount;           /* efficiency cpu ids are [0, ecount), or -1 */
} config_t;

typedef struct {
    double wall_ns, cpu_ns;
    /* FMA and stream samples only: the mean clock in GHz of two short add
     * chains run immediately before and after the kernel, on wall time and
     * on thread CPU time. The per-cycle columns divide by these rather than
     * by the rep's own clock sample, which ran tens of milliseconds earlier
     * and, on the clamped efficiency cluster, often in a different DVFS
     * state. */
    double bracket_wall_ghz, bracket_cpu_ghz;
    int cpu_start, cpu_end;
} sample_t;

enum { K_CLOCK, K_FMA, K_STREAM, NKERNELS };

typedef struct {
    const config_t *cfg;
    const char *tag;
    int use_thread;
    int want_background;
    /* filled in by the run */
    int qos_call_ok;
    char qos[32];
    sample_t *samples[NKERNELS]; /* reps each */
    int mismatches;
} run_t;

/* One short add chain; returns its clock in GHz on both bases. Checked
 * and sunk like the long one. */
static void clock_bracket(run_t *r, double *wall_ghz, double *cpu_ghz) {
    const config_t *c = r->cfg;
    uint64_t c0 = thread_cpu_ns();
    uint64_t w0 = now_ns();
    uint64_t x = dep_add_chain(c->bracket_adds);
    uint64_t w1 = now_ns();
    uint64_t c1 = thread_cpu_ns();
    SINK(x);
    if (x != c->bracket_adds) {
        r->mismatches++;
        fprintf(stderr, "MISMATCH %s bracket: got %" PRIu64 " want %" PRIu64 "\n", r->tag, x, c->bracket_adds);
    }
    *wall_ghz = (double)c->bracket_adds / (double)(w1 - w0);
    *cpu_ghz = c1 > c0 ? (double)c->bracket_adds / (double)(c1 - c0) : 0;
}

static void run_kernels(run_t *r) {
    const config_t *c = r->cfg;
    if (r->want_background) {
        r->qos_call_ok = request_background_qos();
        /* Block once so the scheduler places the thread according to its
         * new class before any timing starts. */
        usleep(20000);
    }
    snprintf(r->qos, sizeof r->qos, "%s", qos_name_self());

    /* Spin until the DVFS governor has had time to reach its steady clock
     * for this thread. Discarded. */
    uint64_t t0 = now_ns();
    while (now_ns() - t0 < c->warm_ns) SINK(dep_add_chain(c->adds / 16));

    /* Samples are taken round-robin, one of every kernel per rep, so
     * machine noise that drifts over the run lands on all three alike
     * instead of on whichever kernel ran last. Rep -1 is the warmup. */
    float out[NACC * 4];
    for (int rep = -1; rep < c->reps; rep++) {
        for (int k = 0; k < NKERNELS; k++) {
            sample_t s;
            s.bracket_wall_ghz = s.bracket_cpu_ghz = 0;
            s.cpu_start = cpu_id();
            if (k == K_CLOCK) {
                uint64_t c0 = thread_cpu_ns();
                uint64_t w0 = now_ns();
                uint64_t x = dep_add_chain(c->adds);
                uint64_t w1 = now_ns();
                uint64_t c1 = thread_cpu_ns();
                SINK(x);
                if (x != c->adds) {
                    r->mismatches++;
                    fprintf(stderr, "MISMATCH %s clock: got %" PRIu64 " want %" PRIu64 "\n", r->tag, x, c->adds);
                }
                s.wall_ns = (double)(w1 - w0);
                s.cpu_ns = (double)(c1 - c0);
            } else {
                /* The kernel sits between two short clock brackets so the
                 * per-cycle figure divides by the clock the kernel saw. */
                double bw0, bc0, bw1, bc1;
                clock_bracket(r, &bw0, &bc0);
                uint64_t c0 = thread_cpu_ns();
                uint64_t w0 = now_ns();
                if (k == K_FMA) {
                    fma_l1(c->xf, c->fma_floats, c->fma_passes, out);
                    uint64_t w1 = now_ns();
                    uint64_t c1 = thread_cpu_ns();
                    SINK(out);
                    for (int j = 0; j < NACC * 4; j++) {
                        if ((double)out[j] != c->fma_ref[j]) {
                            r->mismatches++;
                            fprintf(stderr, "MISMATCH %s fma lane %d: got %.1f want %.1f\n", r->tag, j, (double)out[j], c->fma_ref[j]);
                            break;
                        }
                    }
                    s.wall_ns = (double)(w1 - w0);
                    s.cpu_ns = (double)(c1 - c0);
                } else {
                    uint64_t sum = stream_sum(c->xs, c->stream_elems);
                    uint64_t w1 = now_ns();
                    uint64_t c1 = thread_cpu_ns();
                    SINK(sum);
                    if (sum != c->stream_ref) {
                        r->mismatches++;
                        fprintf(stderr, "MISMATCH %s stream: got %" PRIu64 " want %" PRIu64 "\n", r->tag, sum, c->stream_ref);
                    }
                    s.wall_ns = (double)(w1 - w0);
                    s.cpu_ns = (double)(c1 - c0);
                }
                clock_bracket(r, &bw1, &bc1);
                s.bracket_wall_ghz = 0.5 * (bw0 + bw1);
                s.bracket_cpu_ghz = 0.5 * (bc0 + bc1);
            }
            s.cpu_end = cpu_id();
            if (rep >= 0) r->samples[k][rep] = s;
        }
    }
}

static void *thread_main(void *arg) {
    run_kernels((run_t *)arg);
    return NULL;
}

/* Print the human rows and the RESULT lines for one placement. */
static void report(const run_t *r) {
    const config_t *c = r->cfg;
    const int reps = c->reps;
    double *v = (double *)malloc(sizeof(double) * (size_t)reps * NKERNELS);
    char label[96];

    /* Placement evidence: the set of cpu ids seen, the share of samples
     * that started and ended on an efficiency-core id, and the share of
     * wall time the thread was running. */
    uint64_t seen = 0;
    int on_e = 0, total = 0;
    for (int k = 0; k < NKERNELS; k++) {
        for (int i = 0; i < reps; i++) {
            const sample_t *s = &r->samples[k][i];
            if (s->cpu_start >= 0 && s->cpu_start < 64) seen |= 1ull << s->cpu_start;
            if (s->cpu_end >= 0 && s->cpu_end < 64) seen |= 1ull << s->cpu_end;
            if (c->ecount > 0 && s->cpu_start >= 0 && s->cpu_start < c->ecount &&
                s->cpu_end >= 0 && s->cpu_end < c->ecount) on_e++;
            total++;
            /* Capped at 1: the two clocks are read a few ns apart, so the
             * ratio can exceed 1 by accounting granularity. */
            double sh = s->cpu_ns > 0 ? s->cpu_ns / s->wall_ns : 0;
            v[k * reps + i] = sh > 1 ? 1 : sh;
        }
    }
    stats_t share = stats(v, reps * NKERNELS);
    char cpus[256] = "";
    if (seen == 0) {
        snprintf(cpus, sizeof cpus, "unknown");
    } else {
        size_t len = 0;
        for (int id = 0; id < 64; id++)
            if (seen & (1ull << id))
                len += (size_t)snprintf(cpus + len, sizeof cpus - len, "%s%d", len ? "," : "", id);
    }
    printf("%s: thread qos %s%s, cpu ids seen %s, thread cpu share of wall %.1f%% (median)\n",
           r->tag, r->qos,
           r->want_background ? (r->qos_call_ok ? " (QOS_CLASS_BACKGROUND requested)" : " (QoS API absent, hint not applied)") : "",
           cpus, 100.0 * share.median);
    printf("RESULT %s_qos %s class\n", r->tag, r->qos);
    printf("RESULT %s_cpus %s ids\n", r->tag, cpus);
    if (c->ecount > 0)
        printf("RESULT %s_ecore_samples %.1f percent\n", r->tag, 100.0 * on_e / total);
    else
        printf("RESULT %s_ecore_samples n/a percent\n", r->tag);
    printf("RESULT %s_cpu_share %.1f percent\n", r->tag, 100.0 * share.median);

    /* Wall time is the primary measure, as everywhere in this repository.
     * Thread CPU time is reported alongside it because the efficiency
     * cores are shared with whatever else the system runs at background
     * QoS: the two agree when cpu share is 100 percent and diverge when
     * the thread was descheduled mid-sample. */
    const char *basis[2] = {"", "_cpu"};
    int have_cpu = 1;
    for (int k = 0; k < NKERNELS && have_cpu; k++)
        for (int i = 0; i < reps; i++)
            if (r->samples[k][i].cpu_ns <= 0 || (k != K_CLOCK && r->samples[k][i].bracket_cpu_ghz <= 0)) have_cpu = 0;
    double flops = 2.0 * 4.0 * NACC * (double)(c->fma_floats / 4) * (double)c->fma_passes;
    double bytes = (double)c->stream_elems * sizeof(uint32_t);
    for (int b = 0; b < 2; b++) {
        if (b == 1 && !have_cpu) {
            printf("%s: thread CPU time not available on this platform\n", r->tag);
            printf("RESULT %s_clock_cpu_ghz n/a GHz\nRESULT %s_clock_cpu_median_ghz n/a GHz\n"
                   "RESULT %s_fma_cpu_gflops n/a GFLOP/s\nRESULT %s_stream_cpu_gbps n/a GB/s\n"
                   "RESULT %s_fma_cpu_per_cycle n/a FMA/cycle\nRESULT %s_fma_cpu_per_cycle_cv n/a percent\n"
                   "RESULT %s_stream_cpu_bytes_per_cycle n/a B/cycle\nRESULT %s_stream_cpu_bytes_per_cycle_cv n/a percent\n",
                   r->tag, r->tag, r->tag, r->tag, r->tag, r->tag, r->tag, r->tag);
            break;
        }
        const char *how = b ? "thread cpu time" : "wall time";

        /* Clock: ns per add is latency-like, so the minimum is the estimate. */
        for (int i = 0; i < reps; i++) {
            const sample_t *s = &r->samples[K_CLOCK][i];
            v[i] = (b ? s->cpu_ns : s->wall_ns) / (double)c->adds;
        }
        stats_t sc = stats(v, reps);
        snprintf(label, sizeof label, "%s clock, %s", r->tag, how);
        print_row(label, sc, "ns/add");
        printf("RESULT %s_clock%s_ghz %.3f GHz\n", r->tag, basis[b], 1.0 / sc.min);
        printf("RESULT %s_clock%s_median_ghz %.3f GHz\n", r->tag, basis[b], 1.0 / sc.median);
        printf("RESULT %s_clock%s_cv %.2f percent\n", r->tag, basis[b], 100.0 * sc.cv);

        /* FMA and stream are throughput-like, so the median. */
        for (int i = 0; i < reps; i++) {
            const sample_t *s = &r->samples[K_FMA][i];
            v[i] = flops / (b ? s->cpu_ns : s->wall_ns);
        }
        stats_t sf = stats(v, reps);
        snprintf(label, sizeof label, "%s fma, %s", r->tag, how);
        print_row(label, sf, "GFLOP/s");
        printf("RESULT %s_fma%s_gflops %.2f GFLOP/s\n", r->tag, basis[b], sf.median);
        printf("RESULT %s_fma%s_cv %.2f percent\n", r->tag, basis[b], 100.0 * sf.cv);

        for (int i = 0; i < reps; i++) {
            const sample_t *s = &r->samples[K_STREAM][i];
            v[i] = bytes / (b ? s->cpu_ns : s->wall_ns);
        }
        stats_t ss = stats(v, reps);
        snprintf(label, sizeof label, "%s stream, %s", r->tag, how);
        print_row(label, ss, "GB/s");
        printf("RESULT %s_stream%s_gbps %.2f GB/s\n", r->tag, basis[b], ss.median);
        printf("RESULT %s_stream%s_cv %.2f percent\n", r->tag, basis[b], 100.0 * ss.cv);

        /* Per-cycle rates divide each FMA or stream sample by the mean of
         * the two short clock brackets around it, so a clock that moves
         * within a rep (the shared efficiency cluster under DVFS) is
         * divided out sample by sample rather than median by median. On
         * wall time a descheduling that lands in a bracket but not in the
         * kernel still distorts one sample; the CPU-time basis does not
         * have that problem, which is why the README reads the clamped
         * row's per-cycle figures from the CPU-time table. */
        for (int i = 0; i < reps; i++) {
            const sample_t *sf_ = &r->samples[K_FMA][i];
            double clk = b ? sf_->bracket_cpu_ghz : sf_->bracket_wall_ghz;
            v[i] = flops / (b ? sf_->cpu_ns : sf_->wall_ns) / clk / 8.0;
        }
        stats_t sfc = stats(v, reps);
        snprintf(label, sizeof label, "%s fma per cycle, %s", r->tag, how);
        print_row(label, sfc, "FMA/cycle");
        printf("RESULT %s_fma%s_per_cycle %.3f FMA/cycle\n", r->tag, basis[b], sfc.median);
        printf("RESULT %s_fma%s_per_cycle_cv %.2f percent\n", r->tag, basis[b], 100.0 * sfc.cv);
        for (int i = 0; i < reps; i++) {
            const sample_t *ss_ = &r->samples[K_STREAM][i];
            double clk = b ? ss_->bracket_cpu_ghz : ss_->bracket_wall_ghz;
            v[i] = bytes / (b ? ss_->cpu_ns : ss_->wall_ns) / clk;
        }
        stats_t ssc = stats(v, reps);
        snprintf(label, sizeof label, "%s bytes per cycle, %s", r->tag, how);
        print_row(label, ssc, "B/cycle");
        printf("RESULT %s_stream%s_bytes_per_cycle %.2f B/cycle\n", r->tag, basis[b], ssc.median);
        printf("RESULT %s_stream%s_bytes_per_cycle_cv %.2f percent\n", r->tag, basis[b], 100.0 * ssc.cv);
    }
    free(v);
}

/* splitmix64: deterministic, so every run sees the same streaming data
 * and the checksum is comparable across runs. */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void) {
    const int quick = env_int("QUICK", 0);
    const int main_only = env_int("MAIN_ONLY", 0);
    const char *main_tag = getenv("MAIN_TAG");
    if (!main_tag || !*main_tag) main_tag = "main_default";

    config_t c;
    c.reps = env_int("REPS", quick ? 5 : 15);
    c.adds = quick ? 10000000ull : 50000000ull;
    c.bracket_adds = c.adds / 10;
    c.fma_floats = 4096;                    /* 16384 bytes: inside both L1d sizes */
    c.fma_passes = quick ? 256 : 2048;
    c.stream_elems = quick ? ((size_t)1 << 22) : ((size_t)1 << 24); /* 16 MiB or 64 MiB */
    c.warm_ns = quick ? 100000000ull : 200000000ull;
    c.ecount = efficiency_cpu_count();
    if (c.reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }

    /* FMA data: values 0.5 and 1.0, squares 0.25 and 1.0, so every partial
     * sum is a multiple of 0.25 and stays below 2^22 at the sizes above.
     * All of them are exactly representable in float, so the float
     * accumulators must equal the double reference bit for bit; a loop
     * the compiler shortened would not. */
    float *xf = NULL;
    if (posix_memalign((void **)&xf, 64, c.fma_floats * sizeof(float)) != 0) return 1;
    double lane_sq[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < c.fma_floats; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        xf[i] = ((h >> 28) & 1) ? 1.0f : 0.5f;
        lane_sq[i % 4] += (double)xf[i] * (double)xf[i];
    }
    for (int k = 0; k < NACC; k++)
        for (int l = 0; l < 4; l++) c.fma_ref[4 * k + l] = (double)k + (double)c.fma_passes * lane_sq[l];
    if ((double)c.fma_passes * (double)(c.fma_floats / 4) + NACC >= 4194304.0) {
        fprintf(stderr, "FMA sizes too large for an exact float check\n");
        return 1;
    }

    uint32_t *xs = (uint32_t *)malloc(c.stream_elems * sizeof(uint32_t));
    if (!xs) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    uint64_t state = 0x0123456789ABCDEFull, ref = 0;
    for (size_t i = 0; i < c.stream_elems; i++) {
        xs[i] = (uint32_t)splitmix64(&state);
        ref += xs[i];
    }
    c.xf = xf;
    c.xs = xs;
    c.stream_ref = ref;

    printf("adds per clock sample: %" PRIu64 "\n", c.adds);
    printf("adds per clock bracket: %" PRIu64 " (one before and one after every FMA and stream sample)\n", c.bracket_adds);
#if !defined(__ARM_NEON)
    printf("note: built without NEON; fma_l1 is the fallback path, which compiles but does not measure FMA throughput\n");
#endif
    printf("fma array bytes: %zu\n", c.fma_floats * sizeof(float));
    printf("fma passes per sample: %d\n", c.fma_passes);
    printf("fma flops per sample: %.0f\n", 2.0 * 4.0 * NACC * (double)(c.fma_floats / 4) * (double)c.fma_passes);
    printf("stream array bytes: %zu\n", c.stream_elems * sizeof(uint32_t));
    printf("reps: %d (plus 1 warmup per kernel, discarded, after a %" PRIu64 " ms settling spin)\n",
           c.reps, (uint64_t)(c.warm_ns / 1000000ull));
    if (c.ecount > 0)
        printf("efficiency core cpu ids: 0-%d (hw.perflevel1.logicalcpu)\n", c.ecount - 1);
    else
        printf("efficiency core cpu ids: unknown on this platform\n");
    printf("stream reference sum: %" PRIu64 "\n", ref);
    printf("RESULT stream_checksum %" PRIu64 " sum\n", ref);
    double fma_check = 0;
    for (int j = 0; j < NACC * 4; j++) fma_check += c.fma_ref[j];
    printf("fma reference (sum of 64 lanes): %.1f\n", fma_check);
    printf("RESULT fma_checksum %.1f sum\n", fma_check);

    run_t runs[3];
    int nruns = 0;
    runs[nruns++] = (run_t){.cfg = &c, .tag = main_tag, .use_thread = 0, .want_background = 0};
    if (!main_only) {
        runs[nruns++] = (run_t){.cfg = &c, .tag = "thread_default", .use_thread = 1, .want_background = 0};
        runs[nruns++] = (run_t){.cfg = &c, .tag = "thread_background", .use_thread = 1, .want_background = 1};
    }

    int mismatches = 0;
    for (int i = 0; i < nruns; i++) {
        run_t *r = &runs[i];
        for (int k = 0; k < NKERNELS; k++) r->samples[k] = (sample_t *)calloc((size_t)c.reps, sizeof(sample_t));
        if (r->use_thread) {
            pthread_t t;
            if (pthread_create(&t, NULL, thread_main, r) != 0) {
                fprintf(stderr, "pthread_create failed\n");
                return 1;
            }
            pthread_join(t, NULL);
        } else {
            run_kernels(r);
        }
        report(r);
        mismatches += r->mismatches;
        for (int k = 0; k < NKERNELS; k++) free(r->samples[k]);
    }

    free(xs);
    free(xf);
    if (mismatches) {
        printf("FAIL: %d checksum mismatches\n", mismatches);
        return 1;
    }
    printf("checksums: every sample matched its reference\n");
    return 0;
}
