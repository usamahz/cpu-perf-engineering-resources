/* 11-syscall-cost: what one kernel crossing costs, and why the size of the
 * request does not amortise it.
 *
 * A 64 MiB file is written to a temporary directory and read back once so
 * every page of it sits in the page cache. The timed loops then call
 * pread() at one fixed, page-aligned offset for 1, 64, 4096, 65536 and
 * 1048576 bytes, so the only thing that changes between rows is how many
 * bytes the kernel copies out; the crossing, the descriptor lookup and the
 * page-cache lookup are the same every time. Three references sit beside
 * those rows: getppid(), the smallest call that still traps into the
 * kernel; clock_gettime(), which the kernel answers from a user-mapped
 * page without a trap on both macOS and Linux; and a user-space memcpy of
 * the same sizes from a buffer holding the same bytes, which is what the
 * copy costs on its own. The gap between pread and memcpy at each size is
 * the crossing; the gap between the 1-byte and 4096-byte pread is what
 * the copy adds; and the fixed cost is the intercept of a straight line
 * fitted through the three small sizes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* pread, mkdtemp and CLOCK_MONOTONIC_RAW under -std=c11 on glibc */
#endif
#include "../common/timing.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#define NOINLINE __attribute__((noinline))

#define FILE_BYTES ((size_t)64 << 20)
#define READ_OFFSET ((off_t)32 << 20) /* page aligned, in the middle of the file */
#define NSIZES 5
static const size_t sizes[NSIZES] = {1, 64, 4096, 65536, 1048576};

/* n calls of pread for len bytes at the same offset. The byte count each
 * call returns and the first and last byte it wrote are folded into the
 * sum, so no call can be dropped and every call's data is used. main
 * compares the sum with a value computed from the source buffer and also
 * memcmps the destination against the source after the loop. */
NOINLINE static uint64_t loop_pread(int fd, unsigned char *dst, size_t len, off_t off, size_t n) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        ssize_t got = pread(fd, dst, len, off);
        sum += (uint64_t)got + dst[0] + dst[len - 1];
    }
    return sum;
}

/* The same copy without the kernel. CLOBBER() tells the compiler memory
 * may have changed, so it cannot treat a second identical memcpy as
 * redundant and has to issue the call every time round. len is a runtime
 * value, so this is a real call into the libc memcpy, not an inlined
 * constant-size move. */
NOINLINE static uint64_t loop_memcpy(unsigned char *dst, const unsigned char *src, size_t len, size_t n) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        CLOBBER();
        memcpy(dst, src, len);
        sum += (uint64_t)len + dst[0] + dst[len - 1];
    }
    return sum;
}

/* The smallest call that still enters the kernel: no descriptor, no copy,
 * one word read from the process structure. */
NOINLINE static uint64_t loop_getppid(size_t n) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (uint64_t)getppid();
    return sum;
}

/* The same clock now_ns() reads. Served without a trap from the commpage on
 * macOS and from the vDSO on Linux, so it shows what a "system call" costs
 * when it never actually crosses. */
NOINLINE static uint64_t loop_clock(size_t n) {
    uint64_t sum = 0;
    struct timespec ts;
    for (size_t i = 0; i < n; i++) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        sum += (uint64_t)ts.tv_nsec;
    }
    return sum;
}

/* splitmix64: deterministic, so the file, the source buffer and the
 * reference sums agree across runs. */
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

/* Least-squares line t = a + b * s through the given points. */
static void fit_line(const double *s, const double *t, int n, double *a, double *b) {
    double ms = 0, mt = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) { ms += s[i]; mt += t[i]; }
    ms /= n; mt /= n;
    for (int i = 0; i < n; i++) { sxx += (s[i] - ms) * (s[i] - ms); sxy += (s[i] - ms) * (t[i] - mt); }
    *b = sxy / sxx;
    *a = mt - *b * ms;
}

static int write_all(int fd, const unsigned char *p, size_t len) {
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += (size_t)w; len -= (size_t)w;
    }
    return 0;
}

enum { V_PREAD = 0, V_MEMCPY = NSIZES, V_GETPPID = 2 * NSIZES, V_CLOCK = 2 * NSIZES + 1, NVARIANTS = 2 * NSIZES + 2 };

int main(void) {
    const int quick = env_int("QUICK", 0);
    const int reps = env_int("REPS", quick ? 5 : 31);
    /* run.sh passes the clock_estimate result so cycles can be derived. */
    const double ghz = env_double("CLOCK_GHZ", 0.0);
    /* Small requests get a fixed number of calls per rep; large ones are
     * capped by a byte budget so a rep of 1 MiB reads takes as long as a
     * rep of 1-byte reads rather than a thousand times longer. */
    const size_t small_calls = quick ? ((size_t)1 << 12) : ((size_t)1 << 16);
    const size_t byte_budget = quick ? ((size_t)16 << 20) : ((size_t)256 << 20);

    if (reps < 3) {
        fprintf(stderr, "REPS must be at least 3\n");
        return 1;
    }

    size_t calls[NSIZES];
    for (int i = 0; i < NSIZES; i++) {
        size_t by_budget = byte_budget / sizes[i];
        calls[i] = by_budget < small_calls ? by_budget : small_calls;
    }

    /* Source buffer: the bytes the file will hold, kept for the memcpy
     * baseline and for checking what pread returns. */
    unsigned char *src = NULL, *dst = NULL;
    if (posix_memalign((void **)&src, 16384, FILE_BYTES) || posix_memalign((void **)&dst, 16384, sizes[NSIZES - 1])) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    uint64_t state = 0x0123456789ABCDEFull;
    for (size_t i = 0; i < FILE_BYTES; i += 8) {
        uint64_t v = splitmix64(&state);
        memcpy(src + i, &v, 8);
    }
    memset(dst, 0, sizes[NSIZES - 1]);

    /* The file lives in the temp directory, on whatever file system that
     * is; every timed read is served from the page cache so the file
     * system only matters for the write and the single read that follow. */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char dir[4096], path[4096];
    snprintf(dir, sizeof dir, "%s/syscall-cost.XXXXXX", tmp);
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path, sizeof path, "%s/data", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); return 1; }
    uint64_t t0 = now_ns();
    if (write_all(fd, src, FILE_BYTES) < 0 || fsync(fd) < 0) { perror("write"); return 1; }
    uint64_t t1 = now_ns();
    /* Read the whole file back once so every page is resident and any
     * first-touch cost is paid before timing starts. */
    for (size_t off = 0; off < FILE_BYTES; off += sizes[NSIZES - 1]) {
        ssize_t got = pread(fd, dst, sizes[NSIZES - 1], (off_t)off);
        if (got != (ssize_t)sizes[NSIZES - 1] || memcmp(dst, src + off, (size_t)got) != 0) {
            fprintf(stderr, "file does not read back as written at offset %zu\n", off);
            return 1;
        }
    }
    uint64_t t2 = now_ns();

    printf("file %s  bytes %zu  written and fsynced in %.0f ms, read back and verified in %.0f ms\n",
           path, FILE_BYTES, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    printf("read offset %lld  reps %d (plus 1 warmup, discarded)  clock %.2f GHz\n",
           (long long)READ_OFFSET, reps, ghz);
    for (int i = 0; i < NSIZES; i++)
        printf("size %zu bytes: %zu calls per rep\n", sizes[i], calls[i]);
    if (ghz > 0) printf("RESULT clock %.2f GHz\n", ghz);
    printf("RESULT reps %d runs\n", reps);

    /* Reference sums, from the source buffer rather than from the loops. */
    uint64_t ref[NSIZES];
    for (int i = 0; i < NSIZES; i++)
        ref[i] = (uint64_t)calls[i] * (sizes[i] + src[READ_OFFSET] + src[READ_OFFSET + sizes[i] - 1]);
    const uint64_t ppid = (uint64_t)getppid();

    /* Samples are taken round-robin, one pass of every variant per rep, so
     * noise that drifts over the run lands on every variant alike. */
    double *samples = (double *)malloc(sizeof(double) * NVARIANTS * (size_t)reps);
    int mismatches = 0;
    uint64_t clock_sum = 0;
    for (int r = -1; r < reps; r++) {
        for (int v = 0; v < NVARIANTS; v++) {
            uint64_t sum, want;
            size_t n;
            uint64_t a, b;
            if (v < V_MEMCPY) {
                int i = v - V_PREAD;
                n = calls[i];
                a = now_ns();
                sum = loop_pread(fd, dst, sizes[i], READ_OFFSET, n);
                b = now_ns();
                want = ref[i];
                if (memcmp(dst, src + READ_OFFSET, sizes[i]) != 0) {
                    mismatches++;
                    fprintf(stderr, "MISMATCH pread %zu: destination differs from source\n", sizes[i]);
                }
            } else if (v < V_GETPPID) {
                int i = v - V_MEMCPY;
                n = calls[i];
                a = now_ns();
                sum = loop_memcpy(dst, src + READ_OFFSET, sizes[i], n);
                b = now_ns();
                want = ref[i];
            } else if (v == V_GETPPID) {
                n = small_calls;
                a = now_ns();
                sum = loop_getppid(n);
                b = now_ns();
                want = ppid * n;
            } else {
                n = small_calls;
                a = now_ns();
                sum = loop_clock(n);
                b = now_ns();
                want = sum; /* no reference exists for a clock reading; the sum is printed instead */
                clock_sum += sum;
            }
            SINK(sum);
            if (sum != want) {
                mismatches++;
                fprintf(stderr, "MISMATCH variant %d: got %" PRIu64 " want %" PRIu64 "\n", v, sum, want);
            }
            if (r >= 0) samples[v * reps + r] = (double)(b - a) / (double)n;
        }
    }

    double pread_min[NSIZES], memcpy_min[NSIZES];
    for (int v = 0; v < NVARIANTS; v++) {
        char label[64], key[64];
        size_t bytes = 0, n = small_calls;
        if (v < V_MEMCPY) {
            bytes = sizes[v - V_PREAD]; n = calls[v - V_PREAD];
            snprintf(label, sizeof label, "pread %zu B", bytes);
            snprintf(key, sizeof key, "pread_%zu", bytes);
        } else if (v < V_GETPPID) {
            bytes = sizes[v - V_MEMCPY]; n = calls[v - V_MEMCPY];
            snprintf(label, sizeof label, "memcpy %zu B", bytes);
            snprintf(key, sizeof key, "memcpy_%zu", bytes);
        } else if (v == V_GETPPID) {
            snprintf(label, sizeof label, "getppid");
            snprintf(key, sizeof key, "getppid");
        } else {
            snprintf(label, sizeof label, "clock_gettime MONOTONIC_RAW");
            snprintf(key, sizeof key, "clock_gettime");
        }
        stats_t s = stats(samples + v * reps, reps);
        print_row(label, s, "ns/call");
        printf("RESULT %s_bytes %zu bytes\n", key, bytes);
        printf("RESULT %s_calls %zu calls/rep\n", key, n);
        printf("RESULT %s_min %.3f ns/call\n", key, s.min);
        printf("RESULT %s_median %.3f ns/call\n", key, s.median);
        printf("RESULT %s_cv %.2f percent\n", key, 100.0 * s.cv);
        if (bytes) printf("RESULT %s_nsbyte %.5f ns/byte\n", key, s.min / (double)bytes);
        if (ghz > 0) printf("RESULT %s_cycles %.1f cycles/call\n", key, s.min * ghz);
        if (v < V_MEMCPY) pread_min[v - V_PREAD] = s.min;
        else if (v < V_GETPPID) memcpy_min[v - V_MEMCPY] = s.min;
    }

    /* Fixed cost: intercept of a line through the three small sizes (1, 64
     * and 4096 bytes), where the copy is at most a few tens of nanoseconds
     * and cannot swamp the crossing. Per-byte cost of the copy: slope
     * between the two large sizes, where the crossing is a rounding error. */
    double sx[3] = {(double)sizes[0], (double)sizes[1], (double)sizes[2]};
    double a_p, b_p, a_m, b_m;
    fit_line(sx, pread_min, 3, &a_p, &b_p);
    fit_line(sx, memcpy_min, 3, &a_m, &b_m);
    double big_p = (pread_min[4] - pread_min[3]) / (double)(sizes[4] - sizes[3]);
    double big_m = (memcpy_min[4] - memcpy_min[3]) / (double)(sizes[4] - sizes[3]);
    printf("pread: fixed cost %.1f ns (intercept over 1, 64, 4096 B), slope %.5f ns/byte there; copy slope %.5f ns/byte between 65536 and 1048576 B\n",
           a_p, b_p, big_p);
    printf("memcpy: fixed cost %.1f ns (intercept over 1, 64, 4096 B), slope %.5f ns/byte there; copy slope %.5f ns/byte between 65536 and 1048576 B\n",
           a_m, b_m, big_m);
    printf("RESULT pread_fixed %.1f ns\n", a_p);
    printf("RESULT pread_small_slope %.5f ns/byte\n", b_p);
    printf("RESULT pread_copy_slope %.5f ns/byte\n", big_p);
    printf("RESULT memcpy_fixed %.1f ns\n", a_m);
    printf("RESULT memcpy_small_slope %.5f ns/byte\n", b_m);
    printf("RESULT memcpy_copy_slope %.5f ns/byte\n", big_m);
    if (ghz > 0) printf("RESULT pread_fixed_cycles %.0f cycles\n", a_p * ghz);
    printf("RESULT clock_checksum %" PRIu64 " sum\n", clock_sum);

    close(fd);
    unlink(path);
    rmdir(dir);
    free(samples);
    free(src);
    free(dst);
    if (mismatches) {
        printf("FAIL: %d checksum mismatches\n", mismatches);
        return 1;
    }
    printf("checksums: every pread and memcpy pass matched its reference sum and the source bytes; every getppid returned the parent pid\n");
    return 0;
}
