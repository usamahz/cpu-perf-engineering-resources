# Benchmark author brief

Each benchmark reproduces one claim from one README section on the machine
in this repo. It is the part of the repo a reader can run, so it must be
small, honest, and self-describing. Read `misc/benchmarks/common/timing.h`,
`misc/benchmarks/common/machine.sh` and `misc/benchmarks/common/clock_estimate.c`
before writing anything; use them, do not reinvent them.

## Machine

Apple M4 Pro, macOS 26.2, Apple clang 17.0.0 (clang-1700.4.4.1), arm64.
10 performance cores and 4 efficiency cores, no SMT, no NUMA, 128-byte
cache lines, 16 KiB pages, 24 GiB unified memory. P-core: 128 KiB L1d,
16 MiB L2 shared by a cluster of 5. E-core: 64 KiB L1d, 4 MiB L2 shared
by 4. ISA: NEON, FP16, BF16, I8MM, DotProd, LSE2, SME, SME2. Apple
publishes no clock; `common/clock_estimate.c` measures about 4.4 GHz on a
P-core at default QoS and about 1.5 GHz on an E-core under
`taskpolicy -c background`. No Linux `perf`; no PMU access from user
space. Accelerate.framework provides the vendor BLAS (`cblas_sgemm`).

## Layout

    misc/benchmarks/NN-slug/
      bench.c          one file, C11, includes ../common/timing.h
      build.sh         the exact compile line(s); prints them; exits non-zero on failure
      run.sh           builds, runs machine.sh + clock_estimate, runs bench,
                       writes results/raw.txt and results/summary.md
      README.md        claim, method, machine (seven fields), results, analysis, limits
      results/raw.txt  full stdout of the last run (committed)
      results/summary.md  markdown table produced by run.sh (committed)

`run.sh` must honour `QUICK=1` (small sizes, few reps, finishes in under
ten seconds) so CI can smoke-test it, and `REPS=n`. Default runs finish in
under two minutes. Everything must build with `cc` on macOS arm64 and
compile (at least) on Linux x86-64 and arm64; guard platform-specific
code with `#if` and fall back or skip cleanly with a message.

## Measurement rules

1. Use `now_ns()` from `timing.h`. Never a single run: `REPS` at least 10,
   at least one warmup run that is discarded, report min for latency-like
   quantities and median for throughput-like, always with `cv`.
2. Every result must be consumed with `SINK()` or checked against a
   reference value so the optimiser cannot delete the work. Prove it: the
   README shows the relevant line of `cc -O2 -S` output or a checksum
   printed by the program.
3. State the exact flags. `-O2` unless the claim is about a flag. No
   `-ffast-math` unless the claim is about it.
4. Sizes are chosen from the cache sizes above and stated in bytes.
5. Threads: use pthreads. Pin nothing (macOS has no affinity API); use
   `pthread_set_qos_class_self_np` or `taskpolicy -c background` when the
   claim is about core type, and say so.
6. Print a machine-readable line per result: `RESULT <name> <value> <unit>`
   in addition to the human row from `print_row()`, so `run.sh` can build
   the summary table with `awk`.

## README.md template (fill every section; no placeholders)

    # NN. Title

    **Claim.** One sentence. Supports: "<entry title>" in README section N.

    **Method.** What the program does, what is varied, what is held fixed,
    what is compared, how many runs, which statistic.

    **Machine.** The seven fields as a list: CPU model and microarchitecture;
    cores used; frequency (from clock_estimate), turbo/DVFS state, SMT;
    compiler and flags; workload; baseline; method.

    ## Results

    <!-- results:start -->
    (run.sh pastes results/summary.md here)
    <!-- results:end -->

    ## Analysis

    Three to eight sentences. Ratios and mechanisms. Numbers quoted here must
    be the numbers in the table above.

    ## Limits

    What this machine cannot show (no NUMA, no SMT, no PMU) and what would
    change on an x86 server part.

    ## Reproduce

        ./run.sh            # full run, ~N seconds
        QUICK=1 ./run.sh    # smoke test

`run.sh` replaces the block between the markers with the contents of
`results/summary.md` (use the shared `misc/benchmarks/common/refresh_readme.py`).

## Style

Plain English, British spelling except `quantization`, no em dashes, no
exclamation marks, no marketing. Comments in the C explain why, not what.
