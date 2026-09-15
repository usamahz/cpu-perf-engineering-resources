# 04. Measurement pitfalls

**Claim.** A microbenchmark whose result is never used measures nothing, and a single run is not a measurement: with the result consumed the loop takes real time, without it the optimiser deletes the loop, and two single runs of identical code differ by more than 2 percent in a fifth to a quarter of pairs, and by more than 5 percent in almost every pair that contains the one pass in thirty that lands far out, so a comparison with one run per side cannot resolve a difference of a few percent. Supports: "Google Benchmark User Guide" in README section 4, Measurement, subsection "Microbenchmarks that lie".

**Method.** One loop, a scalar float sum over 1048576 (1<<20) floats, 4194304 bytes, filled with `(i & 7) * 0.25` so that every partial sum is exact and the total is a fixed 917504. Without `-ffast-math` the compiler may not reassociate a float reduction, so the loop is one dependent `fadd` per element and its cost does not depend on where the data sits. Part 1 times that loop at `-O2` in three variants that differ only in what happens to the sum: (a) nothing, (b) `SINK(s)`, an empty asm statement that claims to read it, (c) a store to a caller-visible variable that `main` adds into a checksum and prints against the expected value. One warmup pass of each variant is discarded, then REPS=30 timed passes are taken round-robin, (a), (b), (c), (a), (b), (c), so that noise drifting over the run lands on every variant alike. Part 2 takes variant (b) and times RUNS=30 consecutive passes inside one process and, through the loop in `run.sh`, one pass inside each of RUNS=30 fresh processes; every process, fresh or not, takes its cold pass first, fills the array, repeats the loop for 100 ms so that DVFS and core placement have settled, and discards one more warmup pass before it measures anything. Both series report min, median, max, cv and max over min, and, because the claim is about what one run per side can resolve, `run.sh` also counts over every pair of single passes in a series (435 pairs of 30) the share whose larger value exceeds the smaller by more than 2, 5 and 10 percent. Part 3 is the worked example of one shot against thirty: the first pass a process makes over its freshly mapped array happens once per process, so the in-process run can only report it as a single number, and the 30 fresh processes give the distribution that number is one sample of; the pass is slower than a warm one because the 256 page faults land inside the timed region, which is the mechanism of `09-first-touch` and is not measured further here. The statistic throughout is the median: the claim is about the spread, and the median is the figure the outliers under study cannot drag, so it is the reference for every ratio; min is reported beside it as the latency-like figure for a fixed amount of work, and max and cv show the spread itself. Every timed region is two `now_ns()` readings around one pass; the program also reports the smallest non-zero step of the clock. The first execution of the binary `build.sh` has just written pays extra inside its cold pass, so `run.sh` runs it once, records the output in `results/raw.txt` and discards it as the warmup of the binary itself. `run.sh` records the load average before and after the run, because a run taken with other work on the machine measures that work as well as the code. One thread, default QoS, nothing pinned.

**Generated code.** `build.sh` writes `bench_O2.s` with `cc -std=c11 -O2 -S` from the same flags as the binary, and `run.sh` copies the three timing functions to the end of `results/raw.txt`. In `_time_discarded` the two `clock_gettime` calls follow each other directly, the only instructions between them being the load of the first reading and the two argument moves for the second call, and there is no `fadd` anywhere in the function:

```
_time_discarded:
	mov	x1, sp
	mov	w0, #4                          ; =0x4
	bl	_clock_gettime
	ldp	x19, x20, [sp]
	mov	x1, sp
	mov	w0, #4                          ; =0x4
	bl	_clock_gettime
```

In `_time_sink` the loop is there, unrolled sixteen elements per iteration with one dependent `fadd` per element, which is what a float reduction has to be without `-ffast-math`:

```
_time_sink:
LBB1_7:                                 ; =>This Inner Loop Header: Depth=1
	ldp	q1, q2, [x9, #-32]
	mov	s3, v1[3]
	...
	ldp	q17, q18, [x9], #64
	...
	fadd	s0, s0, s1
	fadd	s0, s0, s5
	fadd	s0, s0, s4
	fadd	s0, s0, s3
	fadd	s0, s0, s2
	fadd	s0, s0, s16
	fadd	s0, s0, s7
	fadd	s0, s0, s6
	fadd	s0, s0, s17
	fadd	s0, s0, s21
	fadd	s0, s0, s20
	fadd	s0, s0, s19
	fadd	s0, s0, s18
	fadd	s0, s0, s24
	fadd	s0, s0, s23
	fadd	s0, s0, s22
	subs	x10, x10, #16
	b.ne	LBB1_7
```

and the sum is stored to the stack for the asm statement, the `InlineAsm` marker, before the second clock read; that store is the whole cost of `SINK()`:

```
LBB1_15:
	str	s0, [sp, #12]
	add	x8, sp, #12
	; InlineAsm Start
	; InlineAsm End
	add	x1, sp, #16
	mov	w0, #4                          ; =0x4
	bl	_clock_gettime
```

`_time_checksum` carries the same loop under the label `LBB3_7` and ends with `str s0, [x19]`, the store through the out pointer, before its second clock read. `results/raw.txt` carries the line `checksum 27525120.0 over 30 passes, expected 27525120.0 (917504.0 per pass): ok`, and the run fails if the two differ.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core for which Apple publishes no microarchitecture name. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT.
- Frequency: 4.49 GHz estimated by `../common/clock_estimate` (a dependent one-cycle add chain, minimum of 7 runs) at the same QoS just before the benchmark. DVFS is on and cannot be disabled, which is why every process runs the loop for 100 ms before it measures; the cycles per element row in the table is derived from this estimate. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c`; no `-ffast-math`, no `-march`.
- Workload: a scalar float sum over 1048576 floats, 4194304 bytes, which is 32 times the 131072-byte P-core L1d and a quarter of the 16777216-byte L2 shared by the core's cluster, so a warm pass runs from L2; one full pass per timed region; the loop is a dependent `fadd` chain, so it is bound by that latency and not by the cache. The cold pass of Part 3 is the same loop over 256 untouched 16384-byte pages.
- Baseline: variant (b), the warm pass whose sum goes to `SINK()`. Part 1 ratios are against its median over the round-robin passes, Part 2 ratios against its median over the 30 consecutive in-process passes, and Part 3 compares the single shot with the fresh-process series of the same cold pass.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one pass; one discarded warmup pass per variant, then 30 timed passes taken round-robin for Part 1; 30 consecutive passes and 30 fresh processes, each with a 100 ms settle and a discarded warmup pass, for Part 2, with the share of pairs of single passes differing by more than 2, 5 and 10 percent counted over both series; the cold pass is the first pass each process makes; median with min, max and cv = stddev / mean; every result through `SINK()` or the checksum; the first execution of the freshly linked binary discarded as its warmup. Load: `run.sh` records the 1, 5 and 15 minute load averages before and after the run at the top and bottom of `results/raw.txt` and in the summary preamble below. Machine condition: the `load average at start` line of `results/raw.txt` reads 7.21 7.55 7.82 (1, 5, 15 min), so other processes belonging to the user were running; the benchmark is single-threaded, so it competed for a core only where the cv column of the table says so, and the spread rows are an upper bound on what the code alone does.

## Results

<!-- results:start -->
Run of 2026-09-15T09:37:41Z. Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). Load average (1, 5, 15 min) 7.21 7.55 7.82 before the run and 6.79 7.46 7.78 after it. Array of 1048576 floats, 4194304 bytes, 256 pages of 16384 bytes; one full pass per timed region, times in ns per pass. REPS=30 passes per variant after a discarded warmup, RUNS=30 consecutive in-process passes and RUNS=30 fresh processes for the spread, 100 ms settle in every process between its cold pass and anything else it measures. Clock tick 41 ns (the smallest non-zero step of now_ns). Checksum over the 30 checksum passes: 27525120.0, expected 27525120.0.

Part 1: the same loop, three ways of using its result, passes taken round-robin.

| variant | median ns/pass | min ns/pass | max ns/pass | cv | median / (b) |
|---|---|---|---|---|---|
| (a) result discarded | 0 | 0 | 42 | 181.3 % | 0.000 |
| (b) result passed to SINK() | 497500 | 495667 | 520750 | 1.4 % | 1.000 |
| (c) result stored for the checksum | 498479 | 496291 | 533208 | 1.9 % | 1.002 |

Part 2: variant (b), one warm pass per sample, across passes and across processes. Part 3: the cold first pass, once from the in-process run and once per fresh process.

| series | n | min ns/pass | median ns/pass | max ns/pass | cv | max / min |
|---|---|---|---|---|---|---|
| p2 in-process, consecutive warm passes | 30 | 495625 | 498374 | 514500 | 1.0 % | 1.038 |
| p2 fresh process, one warm pass each | 30 | 495542 | 497500 | 536125 | 1.7 % | 1.082 |
| p3 cold first pass, single shot from the in-process run | 1 | 679708 | 679708 | 679708 | n/a | n/a |
| p3 cold first pass, one per fresh process | 30 | 646334 | 671688 | 729208 | 3.1 % | 1.128 |

Pairs of single warm passes of identical code (435 in-process pairs, 435 fresh-process pairs) whose larger value exceeds the smaller by:

| difference | in-process pairs | fresh-process pairs |
|---|---|---|
| more than 2 percent | 23.9 % | 19.1 % |
| more than 5 percent | 0.0 % | 6.2 % |
| more than 10 percent | 0.0 % | 0.0 % |

| derived | value | unit |
|---|---|---|
| (a) discarded median / (b) SINK median | 0.00 | percent |
| (a) discarded max / (b) SINK median | 0.008 | percent |
| (c) checksum median / (b) SINK median | 1.002 | ratio |
| (b) SINK median per element | 0.474 | ns/elem |
| (b) SINK median per element, times the clock estimate | 2.13 | cycles/elem |
| in-process spread, max / min minus 1 | 3.8 | percent |
| fresh-process spread, max / min minus 1 | 8.2 | percent |
| fresh-process median / in-process median | 0.998 | ratio |
| fresh-process cv / in-process cv | 1.68 | ratio |
| cold single shot / cold fresh-process median | 1.012 | ratio |
| cold single shot / cold fresh-process min | 1.052 | ratio |
<!-- results:end -->

## Analysis

With the sum thrown away, a pass reads as 0 ns at the median and never more than 42 ns, one 41 ns tick of the clock: the compiler inlined the loop, found its result dead and removed it, so the timed region is two clock reads and nothing else, and the variant comes to 0.00 percent of the real loop at the median and 0.008 percent at its maximum. Handing the same sum to `SINK()` or storing it for the checksum costs 497500 ns and 498479 ns per pass, a ratio of 1.002, so the two ways of consuming a result agree and the consumption itself, one store, costs nothing at this scale; the checksum 27525120.0 matches the expected value, and at 0.474 ns per element, 2.13 cycles at the 4.49 GHz estimate, the loop runs at the latency of its dependent `fadd` chain, which is why the array sitting in L2 rather than L1 does not show. The cost of the loop is well defined: thirty consecutive passes in one process put the warm pass between 495625 ns and 514500 ns, a max over min of 1.038 with a median of 498374 ns and cv 1.0 percent, and thirty fresh processes taking one warm pass each have a median of 497500 ns, 0.998 of the in-process median, and a minimum of 495542 ns. A single run is not: one of the thirty fresh processes took 536125 ns for its warm pass, 1.082 times the minimum, so a comparison with one run per side has one chance in thirty of reporting an 8.2 percent difference that is not there, and 27 of the 29 pairs that contain that pass, 6.2 percent of the 435, differ by more than 5 percent; the in-process series has no such outlier, and its passes still differ by more than 2 percent in 23.9 percent of pairs, the fresh processes in 19.1 percent, so one run per side cannot resolve a 2 percent difference, while on this run no pair in either series differs by more than 10 percent and none of the in-process pairs by more than 5 percent, which says that the size one run can resolve is a property of the run and not of the method, and only the medians of thirty, which agree within 0.998, are a measurement. The cold pass says the same in one line: the in-process run has one number for it, 679708 ns, and thirty fresh processes taking the same pass have a min of 646334 ns, a median of 671688 ns and a max of 729208 ns, cv 3.1 percent and max over min 1.128; the single shot sits at 1.012 of the median and 1.052 of the minimum, and nothing in the one number says where in that span it fell. The load average was 7.21 before the run and 6.79 after it, other work on the 14-core part, so the spreads above include whatever of that work reached the benchmark's core; the in-process series shows no sign of it, the fresh-process outlier may be it, and a run at a load below 1 is the one to compare against.

## Limits

No PMU is reachable from user space, so the page-fault share of the cold pass can only be had by subtraction from the warm pass, and the clock cannot be fixed, so DVFS stays a hidden variable behind the settle phase and the min and median. There is no affinity API, so a fresh process may start on an efficiency core and migrate, one contributor to the fresh-process spread that this machine cannot separate out. There is no SMT, so the sibling-thread interference that widens spreads on a server part is absent, and no NUMA, so first touch decides nothing about placement. Nothing here randomises or records code, stack or heap layout, so the spread includes whatever the one layout this binary got costs, and the significance test that layout randomisation makes valid is not something these numbers can offer. `run.sh` records the load average but cannot control it; the spread on a loaded machine is the load, not the code, which is why the load is in the table. The first execution of a freshly linked binary pays more in its cold pass than any later execution; `run.sh` records that run in `results/raw.txt` and discards it, because this machine offers no way to say how much of it is the kernel's first look at the new file and how much is ordinary noise. On an x86 server the dead-loop result is the same, because it comes from the language semantics and not from this compiler; the clock reads as a few tens of nanoseconds rather than 0 because the TSC-backed clock has a finer tick; Linux pages are 4 KiB, so the cold pass takes four times as many faults, and Linux serves a read fault on anonymous memory with the shared zero page, so its first-touch cost differs from macOS in both directions; and with SMT, turbo and two sockets the run-to-run spread is wider, which is the reason the repetition rules exist.

## Reproduce

    ./run.sh            # full run, about 5 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds; writes its raw and summary files
                        # under ${TMPDIR:-/tmp} and leaves results/ and README.md alone
    REPS=50 RUNS=50 ./run.sh    # more passes per variant and more fresh processes

`SETTLE_MS=n` overrides the settle phase. `run.sh` refuses `REPS` or `RUNS` below 10, builds with `build.sh`, writes the machine description, the load average and the clock estimate to the top of `results/raw.txt`, appends the benchmark output, the fresh-process samples and the load average after the run, aggregates the samples into `RESULT` lines, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. Take the full run on a machine with nothing else running: the summary preamble carries the load average, and the warning `run.sh` prints when the 1 minute figure is above 1 means the spreads include other work. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from.
