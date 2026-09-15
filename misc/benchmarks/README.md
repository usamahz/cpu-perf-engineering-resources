# Benchmarks

One reproducible measurement for each numbered section of the README from
1 to 14. Each directory holds the source, the exact build line, a run
script, the machine description, the raw numbers from the last run, and
the analysis. Nothing here is quoted from a vendor; every number was
produced by the code in the directory on the machine named in its README.

| Directory | Section | Claim it reproduces |
|---|---|---|
| [01-branch-misprediction](01-branch-misprediction/README.md) | 1. One instruction, end to end | A mispredicted branch costs on the order of the pipeline depth; sorted or branchless data removes it |
| [02-latency-vs-throughput](02-latency-vs-throughput/README.md) | 2. Microarchitecture | A dependency chain is bound by latency; independent accumulators are bound by issue throughput |
| [03-cache-latency](03-cache-latency/README.md) | 3. Memory hierarchy | Dependent-load latency steps at each cache level, and page-random access adds TLB cost |
| [04-measurement-pitfalls](04-measurement-pitfalls/README.md) | 4. Measurement | An unused result measures nothing; a single run is not a measurement |
| [05-roofline](05-roofline/README.md) | 5. Models | Arithmetic intensity predicts which roof binds a loop |
| [06-aos-vs-soa-simd](06-aos-vs-soa-simd/README.md) | 6. Single-thread optimisation | Layout decides bytes moved and whether the loop vectorises |
| [07-autovectorization-aliasing](07-autovectorization-aliasing/README.md) | 7. Compilers and codegen | The vectoriser gives up on possible aliasing; a qualifier fixes it |
| [08-false-sharing](08-false-sharing/README.md) | 8. Concurrency | Writers sharing a cache line serialise; padding restores scaling |
| [09-first-touch](09-first-touch/README.md) | 9. NUMA and multi-socket | Allocation is not placement; the first touch pays the fault and picks the home |
| [10-syscall-cost](10-syscall-cost/README.md) | 10. OS and I/O | A kernel crossing has a fixed cost that request size does not amortise |
| [11-coordinated-omission](11-coordinated-omission/README.md) | 11. Tail latency and production systems | A closed-loop load generator hides stalls that an open-loop one reports |
| [12-sgemm-naive-vs-blas](12-sgemm-naive-vs-blas/README.md) | 12. Inference on CPU | A packed, register-blocked microkernel takes a naive loop to the vector unit's ceiling; the vendor BLAS is further ahead only where it owns a matrix unit |
| [13-pcore-vs-ecore](13-pcore-vs-ecore/README.md) | 13. Hardware generations | The same code runs at different speeds by core type; a result without the core type is not comparable |
| [14-stream-bandwidth](14-stream-bandwidth/README.md) | 14. Benchmarks | Vendor bandwidth is a package number; one thread and cache-resident data cannot reveal it |

## The machine

Apple M4 Pro, macOS 26.2, Apple clang 17.0.0 (clang-1700.4.4.1), arm64.
10 performance cores and 4 efficiency cores, no SMT, no NUMA, 128-byte
cache lines, 16 KiB pages, 24 GiB unified memory. P-core: 128 KiB L1d and
a 16 MiB L2 shared by a cluster of 5. E-core: 64 KiB L1d and a 4 MiB L2
shared by 4. ISA: NEON, FP16, BF16, I8MM, DotProd, LSE2, SME, SME2. Apple
publishes no clock, so every run records an estimate from a dependent
one-cycle add chain (`common/clock_estimate.c`); it reads about 4.4 GHz on
a performance core and about 1.5 GHz on an efficiency core under
background QoS. `common/machine.sh` prints the description that heads
every `results/raw.txt`.

## The seven fields

Every README states, for its numbers: the CPU model and microarchitecture;
the cores used; the frequency (estimated as above) with the DVFS and SMT
state; the compiler and flags (from `build.sh`); the workload; the
baseline; and the method (timer, repetitions, warmup, statistic). A number
without all seven does not appear anywhere in this repository.

## Running

    ./run_all.sh            # every benchmark, serially, full sizes
    QUICK=1 ./run_all.sh    # smoke test, small sizes, under ten seconds each
    ./compile_all.sh        # compile only, for a platform you cannot run on

Each directory also runs on its own with `./run.sh`. A run rewrites
`results/raw.txt`, `results/summary.md` and the results table in the
directory's README; commit them together if you re-measure on a different
machine, and change the machine section of the README to match.

Timing uses `clock_gettime(CLOCK_MONOTONIC_RAW)`, at least ten repetitions
after a discarded warmup, the minimum for latency-like quantities and the
median for throughput-like ones, and the coefficient of variation is
printed with every row. Every result is consumed with a compiler barrier
or checked against a reference so the optimiser cannot delete the work;
each README quotes the relevant assembly.

## Portability

The sources are C11 with pthreads. NEON, Accelerate and macOS QoS calls
are behind `#if` guards so every file compiles on Linux x86-64 and arm64
(`compile_all.sh` runs in CI on Linux); kernels that need a feature the
host lacks skip with a message rather than fail. Results on another
machine will differ and are only comparable once the seven fields are
restated for it.
