/* Shared timing and statistics helpers for the benchmarks in this repo.
 *
 * Every benchmark includes this header, times with a monotonic raw clock,
 * repeats the measured region REPS times, and reports min, median and the
 * coefficient of variation. Report min for latency-like quantities and
 * median for throughput-like ones; never a single run.
 */
#ifndef CPU_PERF_TIMING_H
#define CPU_PERF_TIMING_H

/* CLOCK_MONOTONIC_RAW is a Linux extension hidden behind the feature macros
 * under -std=c11; ask for it before any system header is included. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Keep a value alive so the optimiser cannot delete the work that produced it. */
#define SINK(x) __asm__ volatile("" : : "r,m"(x) : "memory")
/* Force the compiler to assume memory has changed (a full compiler barrier). */
#define CLOBBER() __asm__ volatile("" : : : "memory")

typedef struct {
    double min, median, mean, max, cv; /* cv = stddev / mean */
    int n;
} stats_t;

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static inline stats_t stats(double *v, int n) {
    stats_t s;
    double *c = (double *)malloc(sizeof(double) * (size_t)n);
    memcpy(c, v, sizeof(double) * (size_t)n);
    qsort(c, (size_t)n, sizeof(double), cmp_double);
    double sum = 0, sq = 0;
    for (int i = 0; i < n; i++) sum += c[i];
    s.mean = sum / n;
    for (int i = 0; i < n; i++) sq += (c[i] - s.mean) * (c[i] - s.mean);
    s.min = c[0];
    s.max = c[n - 1];
    s.median = (n % 2) ? c[n / 2] : 0.5 * (c[n / 2 - 1] + c[n / 2]);
    s.cv = (s.mean > 0) ? sqrt(sq / n) / s.mean : 0;
    s.n = n;
    free(c);
    return s;
}

/* Print one result row. Unit is a free-form string ("ns", "GB/s", "GFLOP/s"). */
static inline void print_row(const char *label, stats_t s, const char *unit) {
    printf("%-40s min %12.3f  median %12.3f  max %12.3f  cv %5.1f%%  n %d  %s\n",
           label, s.min, s.median, s.max, 100.0 * s.cv, s.n, unit);
}

/* Read REPS / QUICK from the environment so CI can run a short smoke test. */
static inline int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : dflt;
}

#endif
