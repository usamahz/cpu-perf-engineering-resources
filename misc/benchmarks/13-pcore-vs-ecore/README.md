# 13. P-core versus E-core

**Claim.** On a heterogeneous part the same code runs at very different speeds depending on which core type it lands on, and by a different factor for each kind of bottleneck, so a benchmark result that does not state the core type and the clock cannot be compared with another. Supports: "Microarchitectural Comparison and In-core Modeling of State-of-the-art CPUs" under "Independent measurement across vendors", and "Testing AMD's Bergamo: Zen 4c Spam" under "AMD EPYC", both in README section 13, "Hardware generations": the first reports its rates with the clock each socket sustained and the second measures a same-ISA core with a different design and clock ceiling, which is what one part with two core types reproduces on a single machine.

**Method.** `bench.c` runs three kernels with three different bottlenecks under four placements and reports each kernel's rate per placement. The kernels: `dep_add_chain`, the same eight-instruction dependent add chain as `../common/clock_estimate.c`, 50000000 adds per sample, which retires one add per cycle on every core here and so measures the clock of whichever core the thread is on; `fma_l1`, a NEON `vfmaq_f32` loop with sixteen independent accumulators over a 16384-byte array of floats (inside the 65536-byte E-core L1d and the 131072-byte P-core L1d), 2048 passes per sample, 268435456 flops, one 16-byte load per sixteen FMAs so the FMA pipes and not the load ports bind it; and `stream_sum`, a reduction over a 67108864-byte array of 32-bit values, four times the 16777216-byte P-core cluster L2 and sixteen times the 4194304-byte E-core cluster L2, so each pass streams from memory as a single read stream. The placements: the main thread as spawned, which makes no QoS call (macOS reports it as user-interactive, first table); a pthread that makes no QoS call, a control for the thread route itself; a pthread that calls `pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0)`, the efficiency-core hint, then blocks once so the scheduler can place it; and, run by `run.sh` as a second process, the main thread under `taskpolicy -c background`, which clamps the whole process to that class. Nothing is pinned, since macOS has no affinity API. Instead every sample records the cpu id the thread was on at its start and end (`pthread_cpu_number_np`, `sched_getcpu` on Linux) and the thread's CPU time over the sample (`CLOCK_THREAD_CPUTIME_ID`), so the table shows where the scheduler actually put the work and how much of the wall time the thread had. Each placement first spins for 200 ms so DVFS settles, then takes one discarded warmup and `REPS` (default 15) timed samples of each kernel, round-robin, one of each kernel per rep, `now_ns()` around one kernel call. The clock is the minimum of nanoseconds per add (latency-like); GFLOP/s and GB/s are medians (throughput-like); cv is printed for all. The per-cycle columns divide each FMA or stream sample by the mean of two short add-chain brackets, 5000000 adds each, run immediately before and after that kernel call on the same basis, so a clock that moves within a rep is divided out sample by sample; the median of those quotients is the column. Every kernel result, brackets included, is consumed with `SINK()` and checked: the add chains must return their counts, the streaming sum must equal a reference accumulated while the array was filled, and all 64 FMA accumulator lanes must equal a double-precision reference bit for bit, which holds because the data are 0.5 and 1.0 and every partial sum is a multiple of 0.25 below 2^22. A mismatch fails the run. `run.sh` also runs the shared `../common/clock_estimate` at default QoS and under the clamp, records `uptime` for the load the rest of the machine carried, and puts all of it at the top of `results/raw.txt`.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. The clock kernel is the inline asm chain and a counter; the FMA loop is one `ldr q24` and sixteen `fmla.4s` into sixteen live registers, so sixteen dependent chains run in parallel; the streaming sum is a NEON `ldp` and widening `uaddw` loop over sixteen elements per iteration.

```
_dep_add_chain:
LBB3_2:
	; InlineAsm Start
	add	x8, x8, #1
	add	x8, x8, #1
	...                       ; eight dependent adds
	add	x8, x8, #1
	; InlineAsm End
	add	x9, x9, #8
	cmp	x9, x0
	b.lo	LBB3_2

_fma_l1:
LBB5_5:
	ldr	q24, [x10], #16
	fmla.4s	v23, v24, v24
	fmla.4s	v22, v24, v24
	fmla.4s	v21, v24, v24
	...                       ; sixteen fmla.4s into v0 to v7 and v16 to v23
	fmla.4s	v1, v24, v24
	add	x9, x9, #4
	fmla.4s	v0, v24, v24
	cmp	x9, x1
	b.lo	LBB5_5

_stream_sum:
LBB6_7:
	ldp	q16, q17, [x8, #-32]
	uaddw2.2d	v1, v1, v16
	uaddw.2d	v0, v0, v16
	ldp	q16, q18, [x8], #64
	uaddw2.2d	v4, v4, v17
	...
	subs	x10, x10, #16
	b.ne	LBB6_7
```

The call site in `run_kernels` shows the work cannot be deleted: the leading bracket (`bl _clock_bracket`) returns, the two `clock_gettime` calls that start the sample run (`w0` is 16 for `CLOCK_THREAD_CPUTIME_ID` and 4 for `CLOCK_MONOTONIC_RAW`), `fma_l1` is called, the two calls that end the sample run in the other order, the output array goes through the `SINK()` asm, and a loop then compares all 64 lanes with the reference and branches to the mismatch path on any difference. The other two kernels have the same shape with a scalar compare, and `clock_bracket` itself times and checks its chain the same way.

```
	bl	_clock_bracket
	add	x1, sp, #128
	mov	w0, #16
	bl	_clock_gettime
	...
	mov	w0, #4
	bl	_clock_gettime
	...
	bl	_fma_l1
	add	x1, sp, #128
	mov	w0, #4
	bl	_clock_gettime
	...
	mov	w0, #16
	bl	_clock_gettime
	...
	add	x9, sp, #144
	str	x9, [sp, #80]
	add	x10, sp, #80
	; InlineAsm Start
	; InlineAsm End
LBB2_18:
	ldr	s0, [x9, x8, lsl #2]
	fcvt	d0, s0
	ldr	d1, [x26, x8, lsl #3]
	fcmp	d1, d0
	b.ne	LBB2_26
	add	x8, x8, #1
	cmp	x8, #64
	b.ne	LBB2_18
```

Both processes in `results/raw.txt` end with `checksums: every sample matched its reference`; the references (streaming sum 36034921960712949, FMA lanes summing to 83960288.0) are printed above the timings.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, 10 performance cores and 4 efficiency cores; Apple publishes no microarchitecture names. cpu ids 0 to 3 are the efficiency cores: `hw.perflevel1` (named Efficiency) reports 4 logical cpus, the process clamped with `taskpolicy -c background` was only ever observed on ids 0 to 3, and those ids ran the add chain at roughly a third of the performance-core clock at best (1.39 GHz on wall time and 1.52 GHz on thread CPU time against 4.51, all from the fastest sample) and at a sixth and a quarter of it at the median (0.74 and 1.12 GHz against 4.50; the wall figures include time descheduled). `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread at a time, under four placements; nothing pinned. The cpu ids each placement actually ran on are in the first table. Machine condition: `load average at start: 10.50 8.47 8.13`, the line of that name in `results/raw.txt`; other processes belonging to the user were running, and the benchmark is single-threaded, so it competed for a core only where the table's cv and cpu share say so.
- Frequency: 4.49 GHz on a performance core, from `../common/clock_estimate` at default QoS just before the benchmark (`estimated clock: 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)` in `results/raw.txt`), matching the benchmark's own per-placement minimum on the three performance-core placements (4.50 to 4.51 GHz, second table). On the efficiency cores under the clamp the figure to normalise to is the one on thread CPU time, 1.12 GHz median and 1.52 GHz peak with cv 9.8 % (third table), because the thread had only 75.8 % of its wall time on that shared core: the wall-time figures (1.04 GHz from `clock_estimate`, 1.39 GHz peak and 0.74 GHz median from the benchmark) are depressed by the time the thread was descheduled, and `clock_estimate` is a wall-time minimum over 7 windows of about 0.4 s that cannot exclude that. DVFS is on and cannot be disabled; on the clamped cluster the clock moved between samples even on CPU time, which the median and cv columns show. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -pthread -o bench bench.c`.
- Workload: the three kernels above, 50000000 dependent adds, 268435456 flops over 16384 bytes, and one pass over 67108864 bytes per sample, plus two 5000000-add clock brackets around every FMA and stream sample.
- Baseline: the main thread as spawned, with no QoS call, which macOS reports as user-interactive and placed on performance cores.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one kernel call; a 200 ms settling spin per placement; one warmup per kernel discarded; 15 timed samples per kernel taken round-robin across the three kernels; minimum for the clock, median for GFLOP/s and GB/s, cv on all; cpu id at the start and end of every sample and thread CPU time per sample; per-cycle figures from the bracket clocks; every result consumed with `SINK()` and checked against a reference.

## Results

<!-- results:start -->
Shared clock estimate (dependent 1-cycle add chain, 400000000 adds, min of 7 runs): 4.49 GHz at default QoS, 1.04 GHz under taskpolicy -c background. Kernels: a dependent add chain of 50000000 adds per sample (clock from the minimum), sixteen-accumulator NEON FMA over a 16384-byte array, 2048 passes per sample (median), and a streaming sum over a 67108864-byte array of 32-bit values (median). GB is 1e9 bytes. 15 timed samples per kernel and placement after one discarded warmup. The per-cycle columns divide each FMA or stream sample by the mean of two 5000000-add clock brackets taken immediately before and after it, on the same basis (median of the per-sample quotients). Efficiency-core cpu ids are 0-3 on this part; "samples on E ids" is the share of timed samples that started and ended on one of them, and "cpu share" is the median of thread CPU time over wall time per sample. The "main thread, as spawned" placement makes no QoS call; the class the OS gave it is in the first table.

| placement | thread QoS class | cpu ids seen | samples on E ids | cpu share |
|---|---|---|---|---|
| main thread, as spawned | user-interactive | 9,13 | 0.0 % | 100.0 % |
| pthread, no QoS call | default | 10,12,13 | 0.0 % | 100.0 % |
| pthread, QOS_CLASS_BACKGROUND | background | 11,12 | 0.0 % | 100.0 % |
| main thread, taskpolicy -c background | background | 0,1,2,3 | 100.0 % | 75.8 % |

| placement | clock GHz (min) | clock GHz (median) | clock cv | FMA GFLOP/s | FMA cv | FMA per cycle (cv) | stream GB/s | stream cv | bytes per cycle (cv) |
|---|---|---|---|---|---|---|---|---|---|
| main thread, as spawned | 4.51 | 4.50 | 0.4 % | 144.1 | 1.5 % | 4.00 (1.2 %) | 82.2 | 14.9 % | 18.2 (15.0 %) |
| pthread, no QoS call | 4.50 | 4.49 | 1.1 % | 144.0 | 0.5 % | 4.01 (0.8 %) | 85.5 | 3.0 % | 19.1 (2.9 %) |
| pthread, QOS_CLASS_BACKGROUND | 4.51 | 4.50 | 0.4 % | 143.9 | 1.0 % | 4.00 (0.8 %) | 84.1 | 1.8 % | 18.7 (2.1 %) |
| main thread, taskpolicy -c background | 1.39 | 0.74 | 23.2 % | 12.0 | 26.1 % | 1.87 (29.3 %) | 8.4 | 27.8 % | 11.3 (29.4 %) |

The same three quantities on thread CPU time instead of wall time (secondary; equal to the rows above where cpu share is 100 %):

| placement | clock GHz (min) | clock GHz (median) | clock cv | FMA GFLOP/s | FMA cv | FMA per cycle (cv) | stream GB/s | stream cv | bytes per cycle (cv) |
|---|---|---|---|---|---|---|---|---|---|
| main thread, as spawned | 4.51 | 4.50 | 0.4 % | 144.1 | 1.5 % | 4.00 (1.1 %) | 82.2 | 14.9 % | 18.2 (15.0 %) |
| pthread, no QoS call | 4.51 | 4.49 | 1.0 % | 143.9 | 0.5 % | 4.01 (0.8 %) | 85.5 | 2.9 % | 19.1 (2.7 %) |
| pthread, QOS_CLASS_BACKGROUND | 4.51 | 4.50 | 0.4 % | 143.9 | 1.0 % | 4.00 (0.8 %) | 84.0 | 1.8 % | 18.7 (2.1 %) |
| main thread, taskpolicy -c background | 1.52 | 1.12 | 9.8 % | 16.0 | 17.5 % | 1.97 (17.6 %) | 10.9 | 8.2 % | 10.6 (13.4 %) |

Ratios of the rows above: clock (min) is min over min, the other three are median over median.

| ratio, main thread as spawned over | basis | clock (min) | clock (median) | FMA GFLOP/s | stream GB/s |
|---|---|---|---|---|---|
| pthread, no QoS call | wall time | 1.00 | 1.00 | 1.00 | 0.96 |
| pthread, no QoS call | thread cpu time | 1.00 | 1.00 | 1.00 | 0.96 |
| pthread, QOS_CLASS_BACKGROUND | wall time | 1.00 | 1.00 | 1.00 | 0.98 |
| pthread, QOS_CLASS_BACKGROUND | thread cpu time | 1.00 | 1.00 | 1.00 | 0.98 |
| main thread, taskpolicy -c background | wall time | 3.24 | 6.12 | 12.01 | 9.82 |
| main thread, taskpolicy -c background | thread cpu time | 2.96 | 4.03 | 9.01 | 7.52 |
<!-- results:end -->

## Analysis

The efficiency-core hint moved nothing. The thread that asked for `QOS_CLASS_BACKGROUND` reports that class, yet it ran on cpu ids 11 and 12, performance cores, at 4.51 GHz, and its ratios against the main thread are 1.00, 1.00, 1.00 and 0.98; the control thread with no QoS call is the same (1.00, 1.00, 1.00 and 0.96), and the two stream ratios below one are the baseline's own noise, the main thread's stream cv of 14.9 % against 3.0 % and 1.8 % on the two pthreads, so the thread route itself costs nothing. Only the process clamp changed the placement: under `taskpolicy -c background` every sample started and ended on cpu ids 0 to 3, and there the same binary ran the add chain at 1.39 GHz at best against 4.51 (ratio 3.24 on the fastest sample, 6.12 on the median), the FMA loop at 12.0 GFLOP/s against 144.1 (12.01) and the streaming sum at 8.4 GB/s against 82.2 (9.82): three kernels, three different factors, from one change to the launch line. Those wall-time factors mix two things, a slower core and a shared core: the clamped thread had 75.8 % of its wall time, so its wall-time median clock of 0.74 GHz is the 1.12 GHz it ran at on thread CPU time cut by the slices other background work took, and the wall-time per-cycle cells on that row (1.87 and 11.3, cv 29.3 % and 29.4 %) are blurred by it too, because a descheduling that lands in a bracket but not in the kernel, or the reverse, distorts the quotient. The CPU-time table separates the two. On it the performance core sustains 4.00 FMA per cycle (cv 1.1 %), four 128-bit FMA pipes fully fed, and the efficiency core 1.97 (cv 17.6 %), half the pipes, so the FMA gap is approximately the median clock gap times the width gap: 4.03 times 4.00 over 1.97 is 8.2, against 9.01 measured; the remainder is that the medians of a clock column and of a rate column do not fall on the same samples when the clock moves (cv 9.8 % and 17.5 %). The streaming gap is larger than the clock gap, not smaller: the efficiency core moves 10.6 bytes per cycle (cv 13.4 %) against 18.2 on the performance core (19.1 and 18.7 on the two pthread rows), so its bandwidth falls by more than its clock (7.52 against 4.03 on the median clock), because one read stream is limited by how many misses the core keeps in flight and how far its prefetcher runs ahead, not by the memory system, and the efficiency core has less of both. The efficiency core is not one speed either: on CPU time its clock ran between 1.52 GHz (the fastest sample, the min column) and 1.12 GHz (median) with cv 9.8 %, which is DVFS on a clamped cluster whose clock follows whatever else runs there, so a result from it needs the QoS state and the clock as well as the core type before a reader can compare it.

## Limits

There is no PMU access from user space on macOS, so the core type is inferred, from the cpu id (Apple does not document that the efficiency cluster is numbered first; the evidence for it on this part is in the machine section) and from the clock, and the per-cycle figures come from the add-chain brackets rather than a cycle counter. They assume the kernel between the brackets ran at the clock the brackets saw; a core whose power management reacts to the vector load itself would break that, and without a counter this benchmark cannot tell a lower clock during the FMA loop from fewer FMAs per cycle. The machine was not idle during the committed run (load average at start 10.50 8.47 8.13, the `results/raw.txt` line quoted in the machine section), and that reaches the performance-core row too: in this run the performance cores sustained 4.00 to 4.01 FMA per cycle, and in the eight earlier full runs that day, with the add-chain clock the same to within about 5 %, between 3.76 and 4.01, a difference this benchmark records but cannot explain without a counter. The efficiency cores are shared with the system's own background work, so the clamped placement had only 75.8 % of its wall time and its rows carry cv of 23.2 % to 29.4 % on wall time; the tables show the same quantities on thread CPU time to separate the slower core from the shared core, but the ratios on the clamped row move between runs and are not the ratio of the two core designs at their ceilings, since the clamp also lowers the efficiency cluster's clock: across the eight earlier full runs that day the wall-time stream ratio was between 5.5 and 13.8, the wall-time FMA ratio between 8.7 and 15.7 and the CPU-time median clock ratio between 3.0 and 4.2, while the efficiency core's FMA per cycle on CPU time stayed between 1.98 and 2.09; this run's 9.82, 12.01 and 4.03 fall inside those ranges and its 1.97 just under the last, so the ratio columns on that row are a range, and what the run reproduces is the per-cycle reading and the ordering of the three factors. The measurement the first supported entry makes, the clock a socket sustains with every core running vector code against its single-core turbo, is a different one and is left for a follow-up; this benchmark holds one thread and varies the core type. On this macOS build a thread-level background class did not restrict the thread to efficiency cores while performance cores were idle; other builds and other load may behave differently, which is exactly why the benchmark records where each sample ran instead of trusting the hint. On Linux the sources compile and `sched_getcpu` gives the id, but the id-to-core-type mapping has to come from the cpu topology, there is no QoS API, so the background-thread placement runs without the hint and says so, and `run.sh` skips the clamped placement; the clamp has to be replaced by a cpuset or `taskset`, which pins rather than hints. On x86-64 the file compiles through a non-NEON fallback for `fma_l1` that exists to compile, not to measure: clang turns its sixteen `acc + v * v` into one multiply and sixteen adds, so the flops count overstates that path's work and the binary prints a note saying so. On an x86 hybrid client part the efficiency cores also lack AVX-512 and have narrower vector units, so the FMA ratio would carry an instruction-set difference on top of clock and width, and Thread Director rather than a QoS class decides the placement. On a server part with one core type the ratio does not exist, but the same warning applies across sockets, generations and clock policies: a rate without its core and clock is a number without units.

## Reproduce

    ./run.sh            # full run, about 10 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds; writes 13-pcore-vs-ecore-quick-raw.txt
                        # and 13-pcore-vs-ecore-quick-summary.md under $TMPDIR (or /tmp)
                        # and leaves results/ and README.md alone
    REPS=31 ./run.sh    # more timed samples per kernel and placement

`run.sh` builds with `build.sh`, writes the machine description, the load average and the shared clock estimate at default QoS and under `taskpolicy -c background` to the top of `results/raw.txt`, runs `./bench` (three in-process placements) and `MAIN_ONLY=1 MAIN_TAG=process_background taskpolicy -c background ./bench` (the clamped placement) and appends both, stopping on a non-zero exit from either, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from.
