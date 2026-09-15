/* 03-latency-vs-throughput: one dependency chain against many.
 *
 * The same floating-point adds over the same L1-resident array, written as
 * one accumulator (every add waits for the one before it) and as 2, 4, 8 and
 * 16 accumulators (independent chains the out-of-order core can overlap).
 * The chain form is paced by the add latency; the independent forms by how
 * many adds the core can issue per cycle. A second set of register-only
 * inline-asm loops measures the same two limits with no loads in the way:
 * integer add (a one-cycle chain, which also defines the cycle used to
 * convert nanoseconds), fadd and fmul chains, and 8 and 16 independent fadd
 * chains.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"

/* 16384 floats = 65536 bytes: half the P-core L1d (131072 bytes) and equal
 * to the E-core L1d, so every pass after the first is served from L1 and
 * the array is never the limit. */
#define N_ELEMS 16384

/* The values are 0, 1, 2 or 3, so every partial sum is a small integer and
 * exact in float whatever the association. The compiler cannot reassociate
 * the adds without -ffast-math, and the exact reference lets the program
 * check that no kernel was deleted or miscounted; because the sum is the
 * same under any association, the assembly in bench_O2.s is the check that
 * nothing was reassociated. PASSES is capped so the running total stays
 * below 2^24, where float stops being exact. */
#define MAX_VALUE 3
#define MAX_PASSES ((1 << 24) / (MAX_VALUE * N_ELEMS))

/* An empty asm with the accumulator as an in/out operand in a scalar FP
 * register. It emits no instruction, but it forces each accumulator to live
 * in its own scalar register at that point, so the SLP vectoriser cannot
 * pack two or four of them into one NEON register (which it does otherwise:
 * without this, sum4 compiles to fadd.2s and four chains become two). It
 * does not stop the compiler doing anything else with the loop. */
#if defined(__aarch64__)
#define PIN(x) __asm__("" : "+w"(x))
#elif defined(__x86_64__)
#define PIN(x) __asm__("" : "+x"(x))
#else
#define PIN(x) __asm__("" : "+g"(x))
#endif

/* Every kernel handles 16 elements per inner iteration and differs only in
 * which accumulator each element goes to: element j goes to accumulator
 * j mod K. So the five kernels execute the same loads, the same number of
 * fadds and the same loop overhead; only the dependency graph changes. */
#define ADD(acc, j) s##acc += a[i + j]
#define STEP16(A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15) \
    ADD(A0, 0); ADD(A1, 1); ADD(A2, 2); ADD(A3, 3); ADD(A4, 4); ADD(A5, 5); ADD(A6, 6); ADD(A7, 7); \
    ADD(A8, 8); ADD(A9, 9); ADD(A10, 10); ADD(A11, 11); ADD(A12, 12); ADD(A13, 13); ADD(A14, 14); ADD(A15, 15)

/* Each kernel keeps its accumulators across passes, so the chain form is one
 * chain of passes * N_ELEMS dependent adds and not PASSES short chains whose
 * ends the out-of-order window could overlap. noinline keeps the loops out
 * of main, so the assembly is easy to read and the fill pattern is not
 * visible to the optimiser. */
__attribute__((noinline)) static float sum1(const float *a, int passes) {
    float s0 = 0.0f;
    for (int p = 0; p < passes; p++)
        for (int i = 0; i < N_ELEMS; i += 16) {
            STEP16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            PIN(s0);
        }
    return s0;
}

__attribute__((noinline)) static float sum2(const float *a, int passes) {
    float s0 = 0.0f, s1 = 0.0f;
    for (int p = 0; p < passes; p++)
        for (int i = 0; i < N_ELEMS; i += 16) {
            STEP16(0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1);
            PIN(s0); PIN(s1);
        }
    return s0 + s1;
}

__attribute__((noinline)) static float sum4(const float *a, int passes) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (int p = 0; p < passes; p++)
        for (int i = 0; i < N_ELEMS; i += 16) {
            STEP16(0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3);
            PIN(s0); PIN(s1); PIN(s2); PIN(s3);
        }
    return (s0 + s1) + (s2 + s3);
}

__attribute__((noinline)) static float sum8(const float *a, int passes) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
    for (int p = 0; p < passes; p++)
        for (int i = 0; i < N_ELEMS; i += 16) {
            STEP16(0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7);
            PIN(s0); PIN(s1); PIN(s2); PIN(s3); PIN(s4); PIN(s5); PIN(s6); PIN(s7);
        }
    return ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
}

__attribute__((noinline)) static float sum16(const float *a, int passes) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
    float s8 = 0.0f, s9 = 0.0f, s10 = 0.0f, s11 = 0.0f;
    float s12 = 0.0f, s13 = 0.0f, s14 = 0.0f, s15 = 0.0f;
    for (int p = 0; p < passes; p++)
        for (int i = 0; i < N_ELEMS; i += 16) {
            STEP16(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
            PIN(s0); PIN(s1); PIN(s2); PIN(s3); PIN(s4); PIN(s5); PIN(s6); PIN(s7);
            PIN(s8); PIN(s9); PIN(s10); PIN(s11); PIN(s12); PIN(s13); PIN(s14); PIN(s15);
        }
    return (((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7))) +
           (((s8 + s9) + (s10 + s11)) + ((s12 + s13) + (s14 + s15)));
}

/* Register-only loops. Each asm block is 8 or 16 operations; the loop
 * counter is the only other work and is independent of them. The integer
 * chain is the same measurement as common/clock_estimate.c and defines the
 * cycle used to convert every nanosecond figure below. */
#if defined(__aarch64__) || defined(__x86_64__)
#define HAVE_ASM 1
#else
#define HAVE_ASM 0
#endif

#if defined(__aarch64__)
#define I1(r) "add " r ", " r ", #1\n\t"
#define FADD1(d, s) "fadd " d ", " d ", " s "\n\t"
#define FMUL1(d, s) "fmul " d ", " d ", " s "\n\t"
#define FREG "+w"
#define FIN "w"
#define FIN16 "w"          /* 32 NEON registers: the constant stays in one */
#define S(n) "%s" #n       /* operand n as a scalar single register */
#elif defined(__x86_64__)
#define I1(r) "add $1, " r "\n\t"
#define FADD1(d, s) "addss " s ", " d "\n\t"
#define FMUL1(d, s) "mulss " s ", " d "\n\t"
#define FREG "+x"
#define FIN "x"
#define FIN16 "m"          /* 16 xmm registers, all accumulators: the constant comes from memory */
#define S(n) "%" #n
#endif

#if HAVE_ASM
/* One chain of dependent integer adds: latency 1 cycle, so ns per add is the
 * cycle time. */
__attribute__((noinline)) static uint64_t int_chain(uint64_t adds) {
    uint64_t x = 0;
    for (uint64_t i = 0; i < adds; i += 8)
        __asm__ volatile(I1("%0") I1("%0") I1("%0") I1("%0")
                         I1("%0") I1("%0") I1("%0") I1("%0") : "+r"(x));
    return x;
}

/* Eight independent integer chains: paced by the number of integer ALUs. */
__attribute__((noinline)) static uint64_t int_indep8(uint64_t adds) {
    uint64_t x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0;
    for (uint64_t i = 0; i < adds; i += 16)
        __asm__ volatile(I1("%0") I1("%1") I1("%2") I1("%3") I1("%4") I1("%5") I1("%6") I1("%7")
                         I1("%0") I1("%1") I1("%2") I1("%3") I1("%4") I1("%5") I1("%6") I1("%7")
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
                           "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7));
    return x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7;
}

/* One chain of dependent scalar float adds: ns per add times the clock is
 * the fadd latency in cycles. */
__attribute__((noinline)) static float fadd_chain(uint64_t adds) {
    float x = 0.0f;
    const float one = 1.0f;
    for (uint64_t i = 0; i < adds; i += 8)
        __asm__ volatile(FADD1(S(0), S(1)) FADD1(S(0), S(1)) FADD1(S(0), S(1)) FADD1(S(0), S(1))
                         FADD1(S(0), S(1)) FADD1(S(0), S(1)) FADD1(S(0), S(1)) FADD1(S(0), S(1))
                         : FREG(x) : FIN(one));
    return x;
}

/* The same for fmul, as a second calibration point for the method: a
 * different unit with a different latency, measured the same way. The
 * multiplier is 1.0 so the value stays finite. */
__attribute__((noinline)) static float fmul_chain(uint64_t muls) {
    float x = 1.0f;
    const float one = 1.0f;
    for (uint64_t i = 0; i < muls; i += 8)
        __asm__ volatile(FMUL1(S(0), S(1)) FMUL1(S(0), S(1)) FMUL1(S(0), S(1)) FMUL1(S(0), S(1))
                         FMUL1(S(0), S(1)) FMUL1(S(0), S(1)) FMUL1(S(0), S(1)) FMUL1(S(0), S(1))
                         : FREG(x) : FIN(one));
    return x;
}

/* Independent scalar float chains, 8 and 16, with the constant as the last
 * operand. If the plateau is reached with 8 chains, latency times
 * throughput is at most 8; if only with 16, it is more. */
__attribute__((noinline)) static float fadd_indep8(uint64_t adds) {
    float x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0;
    const float one = 1.0f;
    for (uint64_t i = 0; i < adds; i += 16)
        __asm__ volatile(
            FADD1(S(0), S(8)) FADD1(S(1), S(8)) FADD1(S(2), S(8)) FADD1(S(3), S(8))
            FADD1(S(4), S(8)) FADD1(S(5), S(8)) FADD1(S(6), S(8)) FADD1(S(7), S(8))
            FADD1(S(0), S(8)) FADD1(S(1), S(8)) FADD1(S(2), S(8)) FADD1(S(3), S(8))
            FADD1(S(4), S(8)) FADD1(S(5), S(8)) FADD1(S(6), S(8)) FADD1(S(7), S(8))
            : FREG(x0), FREG(x1), FREG(x2), FREG(x3), FREG(x4), FREG(x5), FREG(x6), FREG(x7)
            : FIN(one));
    return (x0 + x1 + x2 + x3) + (x4 + x5 + x6 + x7);
}

__attribute__((noinline)) static float fadd_indep16(uint64_t adds) {
    float x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0;
    float x8 = 0, x9 = 0, x10 = 0, x11 = 0, x12 = 0, x13 = 0, x14 = 0, x15 = 0;
    const float one = 1.0f;
#if defined(__x86_64__)
    /* gcc counts each "+" operand twice against its limit of 30 asm operands,
     * so the sixteen chains are split over two statements of eight. The CPU
     * still overlaps them: asm volatile only pins the compiler's order. */
    for (uint64_t i = 0; i < adds; i += 16) {
        __asm__ volatile(
            FADD1(S(0), S(8)) FADD1(S(1), S(8)) FADD1(S(2), S(8)) FADD1(S(3), S(8))
            FADD1(S(4), S(8)) FADD1(S(5), S(8)) FADD1(S(6), S(8)) FADD1(S(7), S(8))
            : FREG(x0), FREG(x1), FREG(x2), FREG(x3), FREG(x4), FREG(x5), FREG(x6), FREG(x7)
            : FIN16(one));
        __asm__ volatile(
            FADD1(S(0), S(8)) FADD1(S(1), S(8)) FADD1(S(2), S(8)) FADD1(S(3), S(8))
            FADD1(S(4), S(8)) FADD1(S(5), S(8)) FADD1(S(6), S(8)) FADD1(S(7), S(8))
            : FREG(x8), FREG(x9), FREG(x10), FREG(x11), FREG(x12), FREG(x13), FREG(x14), FREG(x15)
            : FIN16(one));
    }
#else
    for (uint64_t i = 0; i < adds; i += 16)
        __asm__ volatile(
            FADD1(S(0), S(16)) FADD1(S(1), S(16)) FADD1(S(2), S(16)) FADD1(S(3), S(16))
            FADD1(S(4), S(16)) FADD1(S(5), S(16)) FADD1(S(6), S(16)) FADD1(S(7), S(16))
            FADD1(S(8), S(16)) FADD1(S(9), S(16)) FADD1(S(10), S(16)) FADD1(S(11), S(16))
            FADD1(S(12), S(16)) FADD1(S(13), S(16)) FADD1(S(14), S(16)) FADD1(S(15), S(16))
            : FREG(x0), FREG(x1), FREG(x2), FREG(x3), FREG(x4), FREG(x5), FREG(x6), FREG(x7),
              FREG(x8), FREG(x9), FREG(x10), FREG(x11), FREG(x12), FREG(x13), FREG(x14), FREG(x15)
            : FIN16(one));
#endif
    return (x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7) +
           (x8 + x9 + x10 + x11 + x12 + x13 + x14 + x15);
}
#endif /* HAVE_ASM */

/* The clock of the core this thread is on right now: the min over three
 * short dependent integer add chains, about 1.4 ms in all. The chain cannot
 * run faster than one add per cycle, so an interrupt or a migration during
 * a sample can only lower the reading, and the min of three discards it. */
static double clock_sample(void) {
#if HAVE_ASM
    double best = 1e300;
    for (int i = 0; i < 3; i++) {
        uint64_t t0 = now_ns();
        uint64_t x = int_chain(2000000ull);
        uint64_t t1 = now_ns();
        SINK(x);
        double ns = (double)(t1 - t0) / 2000000.0;
        if (ns < best) best = ns;
    }
    return 1.0 / best; /* GHz: adds per ns at one add per cycle */
#else
    return 0;
#endif
}

/* Time `body` reps times after one discarded warmup. Just before every rep
 * the clock is sampled, because the P-cores share DVFS domains per cluster
 * and macOS may move the thread, so the clock during one rep is not the
 * clock during another; each rep is converted to cycles with its own
 * sample. `body` is a statement; everything it produces must go through
 * SINK() or a checksum inside it. */
typedef struct { stats_t ns, cyc, ghz; } tm_t;

#define TIME(reps, items, body, out)                                          \
    do {                                                                      \
        double *tv = (double *)malloc(sizeof(double) * (size_t)(reps));       \
        double *cv_ = (double *)malloc(sizeof(double) * (size_t)(reps));      \
        double *gv = (double *)malloc(sizeof(double) * (size_t)(reps));       \
        { body; }                                                             \
        for (int rr = 0; rr < (reps); rr++) {                                 \
            gv[rr] = clock_sample();                                          \
            uint64_t t0 = now_ns();                                           \
            body;                                                             \
            uint64_t t1 = now_ns();                                           \
            tv[rr] = (double)(t1 - t0) / (double)(items);                     \
            cv_[rr] = tv[rr] * gv[rr];                                        \
        }                                                                     \
        (out).ns = stats(tv, (reps));                                         \
        (out).cyc = stats(cv_, (reps));                                       \
        (out).ghz = stats(gv, (reps));                                        \
        free(tv);                                                             \
        free(cv_);                                                            \
        free(gv);                                                             \
    } while (0)

/* Print the human rows and the RESULT lines for one measurement. A chain is
 * a latency, so its ns figure is the min; the independent forms are
 * throughputs, so theirs is the median. Cycles are always the median of the
 * per-rep conversions: a clock sample can lag a clock change by one rep,
 * and the min of a ratio would pick exactly that rep. The RESULT name says
 * which ns statistic was used, so run.sh does not have to know the rule. */
static void report(const char *label, const char *name, const char *item, tm_t t,
                   int is_latency) {
    char unit[32];
    const char *stat = is_latency ? "min" : "median";
    double figure = is_latency ? t.ns.min : t.ns.median;
    snprintf(unit, sizeof unit, "ns/%s", item);
    print_row(label, t.ns, unit);
    printf("RESULT %s_ns_min %.4f ns/%s\n", name, t.ns.min, item);
    printf("RESULT %s_ns_median %.4f ns/%s\n", name, t.ns.median, item);
    printf("RESULT %s_ns_figure_%s %.4f ns/%s\n", name, stat, figure, item);
    printf("RESULT %s_cv %.1f percent\n", name, 100 * t.ns.cv);
    if (t.ghz.median > 0) {
        snprintf(unit, sizeof unit, "cycles/%s", item);
        print_row("  same, in cycles (per-rep clock)", t.cyc, unit);
        printf("  clock before each rep: min %.3f  median %.3f  max %.3f GHz;  %s: %s %.4f ns/%s;  cycles: median %.3f = %.2f %s/cycle\n",
               t.ghz.min, t.ghz.median, t.ghz.max, is_latency ? "latency" : "throughput", stat, figure, item,
               t.cyc.median, 1.0 / t.cyc.median, item);
        printf("RESULT %s_cycles_median %.3f cycles/%s\n", name, t.cyc.median, item);
        printf("RESULT %s_per_cycle %.3f %s/cycle\n", name, 1.0 / t.cyc.median, item);
        printf("RESULT %s_ghz_median %.3f GHz\n", name, t.ghz.median);
    }
}

int main(void) {
    const int quick = env_int("QUICK", 0);
    int reps = env_int("REPS", quick ? 11 : 31);
    const int passes = env_int("PASSES", quick ? 30 : 300);
    /* Several calls per timed region so the fastest kernel still runs for
     * about a millisecond; each call starts fresh accumulators and is
     * checked on its own. */
    const int calls = env_int("CALLS", quick ? 2 : 4);
    const uint64_t asm_ops = (uint64_t)env_int("ASM_OPS_M", quick ? 2 : 20) * 1000000ull;
    if (reps < 10) reps = 10;
    if (passes < 1 || passes > MAX_PASSES) {
        fprintf(stderr, "PASSES must be between 1 and %d to keep the float checksum exact\n", MAX_PASSES);
        return 2;
    }
    if (calls < 1) {
        fprintf(stderr, "CALLS must be at least 1\n");
        return 2;
    }

    /* A small LCG keeps the values out of the compiler's sight; the seed is
     * read at run time so nothing about the array is a compile-time constant. */
    float *a = (float *)malloc(N_ELEMS * sizeof(float));
    if (!a) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    uint32_t r = (uint32_t)env_int("SEED", 12345) | 1u;
    double exact = 0;
    for (int i = 0; i < N_ELEMS; i++) {
        r = r * 1664525u + 1013904223u;
        a[i] = (float)((r >> 30) & MAX_VALUE);
        exact += a[i];
    }
    const double reference = exact * passes;

    printf("array %d floats  bytes %zu  values 0..%d  exact sum per pass %.0f  reference per call %.0f\n",
           N_ELEMS, N_ELEMS * sizeof(float), MAX_VALUE, exact, reference);
    printf("passes per call %d  calls per rep %d  reps %d (plus 1 warmup, discarded)  register-only ops per rep %llu\n",
           passes, calls, reps, (unsigned long long)asm_ops);
    printf("RESULT fsum_checksum %.0f sum\n", reference);

    tm_t t;

#if HAVE_ASM
    /* Let DVFS settle before the first measurement: about 100 ms of
     * dependent adds, discarded. */
    {
        uint64_t x = 0;
        for (int i = 0; i < 100; i++) x += int_chain(4000000ull);
        SINK(x);
    }
    printf("cycles: each rep is converted with the clock sampled just before it "
           "(min of three 2000000-add dependent integer chains)\n");
    {
        uint64_t x;
        TIME(reps, asm_ops, x = int_chain(asm_ops); SINK(x), t);
    }
    report("int add, one chain (1 cycle by construction)", "asm_int_chain", "add", t, 1);
#else
    printf("no inline asm for this architecture: clock unknown, cycles rows skipped\n");
#endif

    printf("\n== float array sum, one thread ==\n");
    const struct {
        const char *name;
        const char *label;
        float (*fn)(const float *, int);
    } ks[] = {
        {"fsum_acc1", "float sum, 1 accumulator (one chain)", sum1},
        {"fsum_acc2", "float sum, 2 accumulators", sum2},
        {"fsum_acc4", "float sum, 4 accumulators", sum4},
        {"fsum_acc8", "float sum, 8 accumulators", sum8},
        {"fsum_acc16", "float sum, 16 accumulators", sum16},
    };
    int failures = 0;
    for (size_t k = 0; k < sizeof ks / sizeof ks[0]; k++) {
        float (*fn)(const float *, int) = ks[k].fn;
        float last = 0;
        int bad = 0;
        TIME(reps, (uint64_t)calls * (uint64_t)passes * N_ELEMS,
             for (int c = 0; c < calls; c++) {
                 last = fn(a, passes);
                 SINK(last);
                 bad += ((double)last != reference);
             },
             t);
        printf("checksum %-11s %12.0f  reference %12.0f  %s\n", ks[k].name, (double)last, reference,
               bad ? "MISMATCH" : "ok");
        failures += bad;
        report(ks[k].label, ks[k].name, "elem", t, k == 0);
    }

#if HAVE_ASM
    printf("\n== register-only chains (inline asm), no loads ==\n");
    {
        uint64_t x;
        float f;
        TIME(reps, asm_ops, x = int_indep8(asm_ops); SINK(x), t);
        report("int add, 8 independent chains", "asm_int_indep8", "add", t, 0);

        TIME(reps, asm_ops, f = fadd_chain(asm_ops); SINK(f), t);
        report("fadd, one chain", "asm_fadd_chain", "add", t, 1);

        TIME(reps, asm_ops, f = fadd_indep8(asm_ops); SINK(f), t);
        report("fadd, 8 independent chains", "asm_fadd_indep8", "add", t, 0);

        TIME(reps, asm_ops, f = fadd_indep16(asm_ops); SINK(f), t);
        report("fadd, 16 independent chains", "asm_fadd_indep16", "add", t, 0);

        TIME(reps, asm_ops, f = fmul_chain(asm_ops); SINK(f), t);
        report("fmul, one chain", "asm_fmul_chain", "mul", t, 1);
    }
#endif

    free(a);
    if (failures) {
        printf("FAIL: %d checksum mismatches\n", failures);
        return 1;
    }
    printf("checksums: every call of every kernel matched the reference sum\n");
    return 0;
}
