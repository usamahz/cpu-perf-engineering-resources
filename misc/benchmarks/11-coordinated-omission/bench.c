/* 12-coordinated-omission: a closed-loop load generator hides a stall.
 *
 * Everything runs on a virtual clock: a uint64_t of simulated nanoseconds
 * that the code advances by the service time of each request and never by
 * reading the wall clock. The claim is about how a load generator records
 * latency, not about any machine, and a virtual clock lets both recordings
 * be taken from one identical server timeline, so the gap between their
 * percentiles is the recording and nothing else. Wall time is read only to
 * cost the simulation itself. This replaces the two-thread proposal in
 * misc/notes/sections/12-tail-latency.md: two real threads would add sleep and
 * scheduling jitter to the same arithmetic and could not give the two
 * recordings exactly the same stall schedule.
 *
 * Server: one request at a time, SERVICE_NS each. From STALL_FIRST_NS,
 * once every STALL_PERIOD_NS, it is unavailable for STALL_NS: a request in
 * service when a stall begins is extended by it, and one that arrives
 * during a stall waits for it to end.
 *
 * Schedule: request i is due at i * INTERVAL_NS, for both clients. The
 * closed-loop client has one connection and sends nothing while a request
 * is outstanding, so a request that falls due during a wait leaves the
 * moment the reply arrives; the open-loop client sends every request when
 * it is due and the server, which serves in arrival order, starts it when
 * it is free. Either way request i starts at the later of its due time and
 * the previous completion and completes at the same instant, which is why
 * the two loops below walk one timeline and differ only in what they
 * subtract from the completion time.
 *
 * Closed loop: latency is reply minus the actual send. While the server is
 * stalled this client is waiting for one reply, so it sends nothing, and
 * the stall becomes one slow sample; the requests it then sends back to
 * back to catch up are each answered SERVICE_NS after they leave, so they
 * record the service time however late they were.
 *
 * Open loop: latency is reply minus the due time, as wrk2 records it, so
 * every request held up by a stall, directly or by the backlog behind it,
 * records its wait.
 *
 * Corrected: the closed-loop samples re-recorded the way HdrHistogram's
 * recordValueWithExpectedInterval does it, with the schedule interval as
 * the expected interval, adding the samples the client would have taken
 * on schedule had it not been stuck waiting.
 *
 * Every percentile, sum, slow count, corrected sample count, stall count
 * and end time the simulation produces is checked against a closed form
 * derived separately below, so a wrong loop fails the run rather than
 * printing a number.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* clock_gettime and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <inttypes.h>

#define NOINLINE __attribute__((noinline))

/* Simulated nanoseconds. */
#define SERVICE_NS      100000ull      /* 100 us per request */
#define STALL_NS        200000000ull   /* 200 ms per stall */
#define STALL_PERIOD_NS 1000000000ull  /* one stall per second */
#define STALL_FIRST_NS  500000000ull   /* first stall at 0.5 s */
#define INTERVAL_NS     200000ull      /* one request due every 200 us: 5000 per second */
#define SLOW_NS         1000000ull     /* a sample above 1 ms counts as slow */

typedef struct {
    uint64_t next_stall; /* simulated time the next stall begins */
    uint64_t stalls;     /* stalls that some request has met */
} server_t;

static void server_reset(server_t *s) {
    s->next_stall = STALL_FIRST_NS;
    s->stalls = 0;
}

/* Serve one request that begins at start; return when it completes. Every
 * stall that begins before the request would otherwise complete is
 * consumed here: one already over when the request arrived hit nobody,
 * one in progress at arrival delays the start, one that begins during
 * service extends it. With the constants above the server is never idle
 * for a whole stall, so the first case never occurs, but the model stays
 * right if the constants change. */
static inline uint64_t service(server_t *s, uint64_t start) {
    uint64_t done = start + SERVICE_NS;
    while (s->next_stall < done) {
        const uint64_t stall_end = s->next_stall + STALL_NS;
        if (start < s->next_stall) done += STALL_NS;
        else if (start < stall_end) done = stall_end + SERVICE_NS;
        s->next_stall += STALL_PERIOD_NS;
        s->stalls++;
    }
    return done;
}

/* Closed loop: request i leaves at the later of its due time and the
 * previous reply, and its latency counts from when it left. Returns the
 * completion time of the last request. noinline keeps each loop a
 * separate symbol in the -S output the README quotes. */
static NOINLINE uint64_t run_closed(server_t *s, uint64_t *lat, uint64_t n) {
    uint64_t free_at = 0;
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t due = i * INTERVAL_NS;
        const uint64_t sent = due > free_at ? due : free_at;
        free_at = service(s, sent);
        lat[i] = free_at - sent;
    }
    return free_at;
}

/* Open loop: request i is sent when due; the server starts it at the
 * later of that and the previous completion, and its latency counts from
 * when it was due. The timeline is the closed loop's; only the
 * subtraction differs. */
static NOINLINE uint64_t run_open(server_t *s, uint64_t *lat, uint64_t n) {
    uint64_t free_at = 0;
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t due = i * INTERVAL_NS;
        const uint64_t start = due > free_at ? due : free_at;
        free_at = service(s, start);
        lat[i] = free_at - due;
    }
    return free_at;
}

/* HdrHistogram recordValueWithExpectedInterval: every sample is kept, and a
 * sample v with v >= 2 * interval also records v - interval, v - 2 *
 * interval, ... down to the last value that is still >= interval. The
 * early continue keeps the unsigned subtraction from wrapping. */
static NOINLINE uint64_t correct_co(const uint64_t *lat, uint64_t n, uint64_t interval,
                                    uint64_t *out, uint64_t cap) {
    uint64_t m = 0;
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t v = lat[i];
        if (m == cap) { fprintf(stderr, "corrected sample array full\n"); exit(1); }
        out[m++] = v;
        if (v < 2 * interval) continue;
        for (uint64_t missing = v - interval; missing >= interval; missing -= interval) {
            if (m == cap) { fprintf(stderr, "corrected sample array full\n"); exit(1); }
            out[m++] = missing;
        }
    }
    return m;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* HdrHistogram's rank convention (getValueAtPercentile): the sample at
 * rank round(p / 100 * n), at least 1, of the sorted list; p = 100 is the
 * maximum. This is nearest-rank with rounding where the textbook rule
 * takes a ceiling; the two differ only when p * n / 100 is not an integer,
 * which it is not for the corrected data set, so the rule is named here
 * and in the README rather than assumed. */
static uint64_t pct_index(uint64_t n, double p) {
    uint64_t k = (uint64_t)(p * (double)n / 100.0 + 0.5);
    if (k < 1) k = 1;
    if (k > n) k = n;
    return k - 1;
}

enum { P50, P90, P99, P999, PMAX, NSTAT };
static const double PCTS[NSTAT] = {50, 90, 99, 99.9, 100};
static const char *STAT_NAME[NSTAT] = {"p50", "p90", "p99", "p999", "max"};
static const char *STAT_LABEL[NSTAT] = {"p50", "p90", "p99", "p99.9", "max"};

enum { CLOSED, OPEN, CORRECTED, NCLIENT };
static const char *CLIENT_NAME[NCLIENT] = {"closed", "open", "corrected"};
static const char *CLIENT_LABEL[NCLIENT] = {"closed loop, from actual send",
                                            "open loop, from intended send",
                                            "closed loop, corrected"};

/* The closed form. In every recording the sorted samples are a run of
 * n_fast copies of SERVICE_NS followed by `copies` copies (one per stall)
 * of each value of an arithmetic sequence, so a percentile is a division
 * away and the sum is a formula. Derived here independently of the loops
 * above, from the constants alone.
 *
 * Closed loop: every sample is SERVICE_NS except one of SERVICE_NS +
 * STALL_NS per stall, the request that met the stall; the requests sent
 * back to back after it each record SERVICE_NS.
 *
 * Open loop: a stall that begins at a due instant with the server idle
 * holds request 0 for the whole stall; request k then starts when request
 * k - 1 finishes, so its latency is STALL + SERVICE - k * (INTERVAL -
 * SERVICE), until that would fall to SERVICE and the schedule is back on
 * time. So K = ceil(STALL / (INTERVAL - SERVICE)) requests per stall carry
 * a latency above SERVICE, in steps of INTERVAL - SERVICE.
 *
 * Corrected: each stalled sample v = SERVICE + STALL adds v - INTERVAL,
 * v - 2 INTERVAL, ... down to the last value still at least INTERVAL, that
 * is M = floor(v / INTERVAL) - 1 values in steps of INTERVAL, which
 * together with the stalled sample itself form one sequence of M + 1
 * values.
 *
 * Both loops end when the last request completes; it is due after the
 * last recovery, so it starts on time and completes SERVICE after it is
 * due. */
typedef struct {
    uint64_t n_fast, copies, nvals, first, step, n;
} model_t;

static uint64_t model_value_at(const model_t *m, uint64_t idx) {
    if (idx < m->n_fast) return SERVICE_NS;
    uint64_t d = (idx - m->n_fast) / m->copies;
    if (d >= m->nvals) d = m->nvals - 1;
    return m->first + d * m->step;
}

static uint64_t model_sum(const model_t *m) {
    return m->n_fast * SERVICE_NS +
           m->copies * (m->nvals * m->first + m->step * (m->nvals * (m->nvals - 1) / 2));
}

/* Samples above SLOW_NS: none of the fast run, and every tail value past it. */
static uint64_t model_slow(const model_t *m) {
    uint64_t d = 0;
    while (d < m->nvals && m->first + d * m->step <= SLOW_NS) d++;
    return m->copies * (m->nvals - d);
}

typedef struct {
    uint64_t q[NSTAT]; /* simulated ns */
    uint64_t n, sum, slow;
} summary_t;

static summary_t summarise(uint64_t *v, uint64_t n) {
    summary_t s;
    qsort(v, (size_t)n, sizeof *v, cmp_u64);
    s.n = n;
    s.sum = 0;
    s.slow = 0;
    for (uint64_t i = 0; i < n; i++) {
        s.sum += v[i];
        if (v[i] > SLOW_NS) s.slow++;
    }
    for (int k = 0; k < NSTAT; k++) s.q[k] = v[pct_index(n, PCTS[k])];
    return s;
}

static void *xmalloc(size_t bytes) {
    void *p = malloc(bytes);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

static int fail(const char *what, uint64_t got, uint64_t want) {
    fprintf(stderr, "CHECK FAILED: %s: got %" PRIu64 " want %" PRIu64 "\n", what, got, want);
    return 1;
}

int main(void) {
    const int quick = env_int("QUICK", 0) != 0;
    const int reps = env_int("REPS", quick ? 5 : 31);
    if (reps < 3) { fprintf(stderr, "REPS must be at least 3\n"); return 1; }

    const uint64_t sim_s = quick ? 5 : 20;
    const uint64_t run_ns = sim_s * 1000000000ull;
    const uint64_t n = run_ns / INTERVAL_NS; /* requests due in the run, for both clients */
    /* Stalls that begin before the run ends: k with FIRST + k * PERIOD < run. */
    const uint64_t stalls = run_ns > STALL_FIRST_NS
        ? (run_ns - STALL_FIRST_NS + STALL_PERIOD_NS - 1) / STALL_PERIOD_NS : 0;
    const uint64_t gap = INTERVAL_NS - SERVICE_NS;
    const uint64_t K = (STALL_NS + gap - 1) / gap; /* open-loop requests delayed per stall */
    const uint64_t stalled = SERVICE_NS + STALL_NS; /* the one slow closed-loop sample per stall */

    /* The closed form assumes each stall begins at a due instant with the
     * server idle and on schedule, that the schedule is back on time
     * before the next stall and before the last request is due, and that
     * the stalled sample is long enough for the correction to add at
     * least one sample. The constants satisfy all of these; say so rather
     * than print a wrong reference if they are edited. */
    if (INTERVAL_NS <= SERVICE_NS || STALL_FIRST_NS % INTERVAL_NS || STALL_PERIOD_NS % INTERVAL_NS ||
        K * INTERVAL_NS > STALL_PERIOD_NS || stalls == 0 ||
        STALL_FIRST_NS + (stalls - 1) * STALL_PERIOD_NS + K * INTERVAL_NS > (n - 1) * INTERVAL_NS ||
        stalled < 2 * INTERVAL_NS || SLOW_NS <= SERVICE_NS || SLOW_NS >= stalled) {
        fprintf(stderr, "the constants violate an assumption of the closed-form reference\n");
        return 1;
    }
    const uint64_t M = stalled / INTERVAL_NS - 1; /* samples the correction adds per stall */
    const uint64_t end_want = (n - 1) * INTERVAL_NS + SERVICE_NS;
    const model_t model[NCLIENT] = {
        [CLOSED] = {n - stalls, stalls, 1, stalled, 0, n},
        [OPEN] = {n - stalls * K, stalls, K, stalled - (K - 1) * gap, gap, n},
        [CORRECTED] = {n - stalls, stalls, M + 1, stalled - M * INTERVAL_NS, INTERVAL_NS, n + stalls * M},
    };

    /* The correction adds at most one sample per INTERVAL_NS of recorded
     * closed-loop latency, which is n service times plus one stall per
     * stall met; two spare stalls cover a loop that meets one the closed
     * form does not expect, so that it fails a check rather than the
     * capacity. */
    const uint64_t cap_corr = n + (n * SERVICE_NS + (stalls + 2) * STALL_NS) / INTERVAL_NS + 16;
    uint64_t *closed = xmalloc(n * sizeof(uint64_t));
    uint64_t *open = xmalloc(n * sizeof(uint64_t));
    uint64_t *corr = xmalloc(cap_corr * sizeof(uint64_t));
    double *q = xmalloc(sizeof(double) * (size_t)(NCLIENT * NSTAT * reps));
    double *cnt = xmalloc(sizeof(double) * (size_t)(NCLIENT * reps));
    double *slow = xmalloc(sizeof(double) * (size_t)(NCLIENT * reps));
    double *cost = xmalloc(sizeof(double) * (size_t)(2 * reps));
#define Q(c, k, r) q[((c) * NSTAT + (k)) * reps + (r)]
#define C(c, r) cnt[(c) * reps + (r)]
#define S(c, r) slow[(c) * reps + (r)]
#define T(c, r) cost[(c) * reps + (r)]

    printf("virtual clock; %" PRIu64 " s simulated per client per rep; service %" PRIu64
           " us; stall %" PRIu64 " ms once per second from %" PRIu64 " ms, %" PRIu64
           " stalls per run\n",
           sim_s, SERVICE_NS / 1000, STALL_NS / 1000000, STALL_FIRST_NS / 1000000, stalls);
    printf("schedule for both clients: %" PRIu64 " requests/s, one due every %" PRIu64 " us, %" PRIu64
           " requests per run; correction: expected interval %" PRIu64 " us\n",
           1000000000ull / INTERVAL_NS, INTERVAL_NS / 1000, n, INTERVAL_NS / 1000);
    printf("closed form: %" PRIu64 " open-loop requests delayed per stall, %" PRIu64
           " samples added per stall by the correction, %" PRIu64 " corrected samples per run, "
           "last completion at %" PRIu64 " us\n",
           K, M, n + stalls * M, end_want / 1000);
    printf("sample arrays: closed %" PRIu64 " B, open %" PRIu64 " B, corrected %" PRIu64 " B (capacity)\n",
           (uint64_t)(n * sizeof(uint64_t)), (uint64_t)(n * sizeof(uint64_t)),
           (uint64_t)(cap_corr * sizeof(uint64_t)));
    printf("reps %d (plus 1 warmup, discarded); percentiles by HdrHistogram's rank convention; "
           "latencies in simulated us; per-rep line: n p50 p90 p99 p99.9 max slow(>%" PRIu64 " ms)\n\n",
           reps, SLOW_NS / 1000000);
    printf("PARAM sim_seconds %" PRIu64 "\n", sim_s);
    printf("PARAM service_us %" PRIu64 "\n", SERVICE_NS / 1000);
    printf("PARAM stall_ms %" PRIu64 "\n", STALL_NS / 1000000);
    printf("PARAM rate_per_s %" PRIu64 "\n", (uint64_t)(1000000000ull / INTERVAL_NS));
    printf("PARAM interval_us %" PRIu64 "\n", (uint64_t)(INTERVAL_NS / 1000));
    printf("PARAM slow_ms %" PRIu64 "\n", (uint64_t)(SLOW_NS / 1000000));
    printf("PARAM reps %d\n\n", reps);

    uint64_t sum_total = 0, checks = 0;
    for (int r = 0; r <= reps; r++) {
        server_t srv;

        server_reset(&srv);
        const uint64_t t0 = now_ns();
        const uint64_t end_closed = run_closed(&srv, closed, n);
        const uint64_t t1 = now_ns();
        const uint64_t stalls_closed = srv.stalls;

        server_reset(&srv);
        const uint64_t t2 = now_ns();
        const uint64_t end_open = run_open(&srv, open, n);
        const uint64_t t3 = now_ns();
        const uint64_t stalls_open = srv.stalls;

        const uint64_t nk = correct_co(closed, n, INTERVAL_NS, corr, cap_corr);

        summary_t sm[NCLIENT];
        sm[CLOSED] = summarise(closed, n);
        sm[OPEN] = summarise(open, n);
        sm[CORRECTED] = summarise(corr, nk);
        SINK(sm[CLOSED].sum);
        SINK(sm[OPEN].sum);
        SINK(sm[CORRECTED].sum);

        /* Reference checks against the closed form. The two loops must
         * agree with it on the timeline (stall count and last completion),
         * the correction on how many samples it added, and every recording
         * on its sum, its slow count and every percentile. The closed and
         * open sample counts are n by construction and are not checks. */
        int bad = 0;
        bad |= stalls_closed != stalls && fail("closed-loop stalls", stalls_closed, stalls);
        bad |= stalls_open != stalls && fail("open-loop stalls", stalls_open, stalls);
        bad |= end_closed != end_want && fail("closed-loop last completion", end_closed, end_want);
        bad |= end_open != end_want && fail("open-loop last completion", end_open, end_want);
        bad |= nk != model[CORRECTED].n && fail("corrected count", nk, model[CORRECTED].n);
        checks += 5;
        for (int c = 0; c < NCLIENT; c++) {
            char what[96];
            snprintf(what, sizeof what, "%s sum", CLIENT_NAME[c]);
            bad |= sm[c].sum != model_sum(&model[c]) && fail(what, sm[c].sum, model_sum(&model[c]));
            snprintf(what, sizeof what, "%s slow samples", CLIENT_NAME[c]);
            bad |= sm[c].slow != model_slow(&model[c]) && fail(what, sm[c].slow, model_slow(&model[c]));
            for (int k = 0; k < NSTAT; k++) {
                const uint64_t want = model_value_at(&model[c], pct_index(model[c].n, PCTS[k]));
                snprintf(what, sizeof what, "%s %s", CLIENT_NAME[c], STAT_LABEL[k]);
                bad |= sm[c].q[k] != want && fail(what, sm[c].q[k], want);
            }
            checks += 2 + NSTAT;
        }
        if (bad) return 1;

        printf("%s %d:", r == 0 ? "warmup" : "rep", r);
        for (int c = 0; c < NCLIENT; c++) {
            printf(" %s n %" PRIu64 " %.1f %.1f %.1f %.1f %.1f slow %.4f%%", CLIENT_NAME[c], sm[c].n,
                   sm[c].q[P50] / 1e3, sm[c].q[P90] / 1e3, sm[c].q[P99] / 1e3, sm[c].q[P999] / 1e3,
                   sm[c].q[PMAX] / 1e3, 100.0 * (double)sm[c].slow / (double)sm[c].n);
            if (c + 1 < NCLIENT) printf(" |");
        }
        printf(" | sim %.1f + %.1f ns/req\n", (double)(t1 - t0) / (double)n,
               (double)(t3 - t2) / (double)n);
        if (r == 0) continue;

        for (int c = 0; c < NCLIENT; c++) {
            sum_total += sm[c].sum;
            C(c, r - 1) = (double)sm[c].n;
            S(c, r - 1) = 100.0 * (double)sm[c].slow / (double)sm[c].n;
            for (int k = 0; k < NSTAT; k++) Q(c, k, r - 1) = (double)sm[c].q[k] / 1e3;
        }
        T(0, r - 1) = (double)(t1 - t0) / (double)n;
        T(1, r - 1) = (double)(t3 - t2) / (double)n;
    }
    printf("\n");

    /* Percentiles are latency-like, so the minimum over reps is reported;
     * on a virtual clock every rep is identical and cv is 0 by
     * construction. Counts and shares are medians. The wall cost of the
     * simulation per request is the one number here that this machine and
     * its load can move; it is latency-like too (interference and the
     * clock ramp only add to it), so its minimum is reported, with the
     * median and cv beside it. */
    for (int c = 0; c < NCLIENT; c++) {
        for (int k = 0; k < NSTAT; k++) {
            char label[80];
            snprintf(label, sizeof label, "%s %s", CLIENT_LABEL[c], STAT_LABEL[k]);
            stats_t st = stats(&Q(c, k, 0), reps);
            print_row(label, st, "us");
            printf("RESULT %s_%s_min %.1f us\n", CLIENT_NAME[c], STAT_NAME[k], st.min);
            printf("RESULT %s_%s_median %.1f us\n", CLIENT_NAME[c], STAT_NAME[k], st.median);
            printf("RESULT %s_%s_cv %.2f percent\n", CLIENT_NAME[c], STAT_NAME[k], 100.0 * st.cv);
        }
        stats_t sc = stats(&C(c, 0), reps);
        printf("RESULT %s_count_median %.0f samples\n", CLIENT_NAME[c], sc.median);
        printf("RESULT %s_count_cv %.2f percent\n", CLIENT_NAME[c], 100.0 * sc.cv);
        stats_t ss = stats(&S(c, 0), reps);
        printf("RESULT %s_slow_median %.4f percent\n", CLIENT_NAME[c], ss.median);
        printf("RESULT %s_slow_cv %.2f percent\n", CLIENT_NAME[c], 100.0 * ss.cv);
    }
    {
        stats_t sc = stats(&T(0, 0), reps);
        print_row("closed loop simulation cost", sc, "ns/request");
        printf("RESULT closed_sim_min %.2f ns/request\n", sc.min);
        printf("RESULT closed_sim_median %.2f ns/request\n", sc.median);
        printf("RESULT closed_sim_cv %.2f percent\n", 100.0 * sc.cv);
        stats_t so = stats(&T(1, 0), reps);
        print_row("open loop simulation cost", so, "ns/request");
        printf("RESULT open_sim_min %.2f ns/request\n", so.min);
        printf("RESULT open_sim_median %.2f ns/request\n", so.median);
        printf("RESULT open_sim_cv %.2f percent\n", 100.0 * so.cv);
    }
    printf("RESULT stalls_per_run %" PRIu64 " stalls\n", stalls);
    printf("RESULT open_delayed_per_stall %" PRIu64 " requests\n", K);
    printf("RESULT corrected_added_per_stall %" PRIu64 " samples\n", M);
    printf("RESULT checks_passed %" PRIu64 " checks\n", checks);
    printf("RESULT latency_sum_total %" PRIu64 " simulated_ns\n", sum_total);
    printf("checksums: %" PRIu64 " closed-form checks passed over %d runs (stall counts, last "
           "completions, corrected sample counts, latency sums, slow counts, every percentile); "
           "%" PRIu64 " simulated ns of recorded latency over the %d timed reps\n",
           checks, reps + 1, sum_total, reps);

    free(closed);
    free(open);
    free(corr);
    free(q);
    free(cnt);
    free(slow);
    free(cost);
    return 0;
}
