# 01. Branch misprediction

**Claim.** A mispredicted conditional branch costs about 20 cycles on this core, so a data-dependent branch over unsorted data is several times slower than the same branch over sorted data or a branchless form. Supports: "The microarchitecture of Intel, AMD, and VIA CPUs" in README section 1, One instruction, end to end, subsection "Fetch and decode".

**Method.** `bench.c` fills an array of 4194304 (1<<22) 32-bit integers, 16777216 bytes, with uniform values in [0,256) from a fixed-seed splitmix64 generator, and makes a sorted copy with `qsort`. Three kernels each return the sum of every element at or above a threshold: `sum_branchy` uses an `if`, `sum_branchless` uses an unconditional select that compiles to `csel`, and `sum_vector` is the same select with nothing to stop the compiler vectorising it. The two scalar kernels carry an empty `__asm__ volatile("" : "+r"(sum))` on the accumulator: inside the taken arm for the branchy loop, which forces the compiler to keep a real conditional branch on the data because a volatile asm cannot be executed speculatively, and after the select for the branchless loop, so that the two differ only in where the condition is resolved and both stay scalar. Each kernel runs over the unsorted array and the sorted one at two thresholds: 128, for which the condition `a[i] >= t` is true for half the elements, and 243, for which it is true for 13 of 256 values, about 5 percent. The compiled branch, `b.lo`, jumps over the add, so it is taken when the condition is false: half the time at 128 and about 95 percent of the time at 243. Either way the predictor can do no better on random data than guess the common direction, and the mispredict rate is the minority fraction. Sorting changes neither the work nor the result, only whether the branch is predictable. That gives twelve variants, all measured in one process: `now_ns()` around one full pass of one kernel, one warmup pass of every variant discarded, then `REPS` (default 31) timed passes taken round-robin so that noise drifting over the run lands on every variant alike. The statistic is the median of nanoseconds per element (a throughput-like quantity) with `cv`; the minimum is shown alongside. Cycles per element are the median multiplied by the clock estimate from `../common/clock_estimate`, which `run.sh` passes in as `CLOCK_GHZ`. Every timed result is consumed with `SINK()` and compared with a reference sum computed a different way, from a 256-bin histogram of the data; a mismatch fails the run.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. The branchy loop keeps a branch that depends on the loaded value (`b.lo` around the `add`); the branchless loop has a `csel` and only the loop back-edge; the vector loop is a NEON compare, mask and widening add over four 4-lane registers, sixteen elements per iteration.

```
_sum_branchy:
LBB1_2:
	subs	x1, x1, #1
	b.eq	LBB1_1
LBB1_3:
	ldr	w9, [x0], #4
	cmp	w9, w2
	b.lo	LBB1_2            ; data-dependent: skip the add when a[i] < t
	add	x8, x8, x9
	b	LBB1_2

_sum_branchless:
LBB2_1:
	ldr	w9, [x0], #4
	cmp	w9, w2
	csel	w9, wzr, w9, lo   ; select 0 or a[i]; no branch on the data
	add	x8, x8, x9
	subs	x1, x1, #1
	b.ne	LBB2_1            ; only the loop back-edge

_sum_vector:
LBB3_7:
	ldp	q17, q18, [x8, #-32]
	ldp	q19, q20, [x8], #64
	cmhi.4s	v21, v0, v17
	...
	bic.16b	v17, v17, v21
	...
	uaddw2.2d	v2, v2, v17
	uaddw.2d	v1, v1, v17
	...
	subs	x10, x10, #16
	b.ne	LBB3_7
```

The call site in `main` shows the work cannot be deleted: the kernel is called through the `kernels` table between two `clock_gettime` calls, its result goes through the `SINK()` asm and is then compared with the reference sum.

```
	bl	_clock_gettime
	adrp	x8, l___const.main.kernels@PAGE
	add	x8, x8, l___const.main.kernels@PAGEOFF
	add	x21, x8, w21, uxtb #4
	ldr	x8, [x21, #8]
	...
	blr	x8
	mov	x27, x0
	...
	bl	_clock_gettime
	...
	str	x27, [sp, #136]
	add	x8, sp, #136
	; InlineAsm Start
	; InlineAsm End
	...
	cmp	x27, x8
```

`results/raw.txt` ends with `checksums: every variant matched its reference sum`, and the reference sums (401130148 at threshold 128 and 52694301 at 243) are printed above the timings.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core for which Apple publishes no microarchitecture name and no pipeline depth. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT. Machine condition: `load average at start: 7.75 7.54 7.83` in `results/raw.txt`, so other processes belonging to the user were running on the 14 logical CPUs during the committed run; the benchmark is a single thread, so it competed for a core only where the cv column of the table says so, and no row reads above 5.0 percent.
- Frequency: `estimated clock: 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)` in `results/raw.txt`, measured by `../common/clock_estimate` at the same QoS just before the benchmark. DVFS is on and cannot be disabled; the predictable loops sit at one cycle per element, the floor set by their one-cycle loop-carried chains (the counter, the pointer and the accumulator), which shows the core held that clock during the run. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c`.
- Workload: the sum of every element at or above a threshold over 4194304 32-bit values, 16777216 bytes, which is 128 times the 131072-byte P-core L1d and equal to the 16777216-byte L2 shared by the core's cluster, so each pass streams the array sequentially through the cache hierarchy and the hardware prefetcher keeps up. Twelve variants: three kernels, unsorted and sorted data, thresholds 128 and 243.
- Baseline: the branchy loop over the sorted data (the same instructions, one mispredict per pass) and the scalar branchless select over the same data.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one full pass; one warmup pass per variant discarded; 31 timed passes per variant taken round-robin; median and cv reported, minimum shown; cycles per element derived from the median and the clock estimate; every result checked against a histogram reference sum and consumed with `SINK()`.

## Results

<!-- results:start -->
Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). Array of 4194304 32-bit elements, 16777216 bytes. Median over the timed passes; cycles per element is median ns per element times the clock estimate.

| variant | condition true | median ns/elem | min ns/elem | cycles/elem | cv |
|---|---|---|---|---|---|
| t128 unsorted branchy | 50.0 % | 2.386 | 2.363 | 10.71 | 0.7 % |
| t128 unsorted branchless | 50.0 % | 0.226 | 0.226 | 1.02 | 4.1 % |
| t128 unsorted vector | 50.0 % | 0.056 | 0.056 | 0.25 | 5.0 % |
| t128 sorted branchy | 50.0 % | 0.225 | 0.224 | 1.01 | 1.1 % |
| t128 sorted branchless | 50.0 % | 0.226 | 0.226 | 1.02 | 0.8 % |
| t128 sorted vector | 50.0 % | 0.056 | 0.056 | 0.25 | 1.2 % |
| t243 unsorted branchy | 5.1 % | 0.508 | 0.507 | 2.28 | 1.1 % |
| t243 unsorted branchless | 5.1 % | 0.227 | 0.226 | 1.02 | 1.0 % |
| t243 unsorted vector | 5.1 % | 0.056 | 0.056 | 0.25 | 2.7 % |
| t243 sorted branchy | 5.1 % | 0.223 | 0.223 | 1.00 | 0.9 % |
| t243 sorted branchless | 5.1 % | 0.227 | 0.226 | 1.02 | 1.0 % |
| t243 sorted vector | 5.1 % | 0.056 | 0.056 | 0.25 | 3.3 % |

| derived | value | unit |
|---|---|---|
| t128 branchy unsorted / branchy sorted | 10.62 | ratio |
| t128 branchy unsorted / branchless unsorted | 10.54 | ratio |
| t128 branchy sorted / branchless sorted | 0.99 | ratio |
| t128 branchless unsorted / vector unsorted | 4.06 | ratio |
| t128 extra cycles per element, branchy unsorted minus branchy sorted | 9.70 | cycles |
| t128 implied cost per mispredict (extra cycles / 0.4995 mispredict rate) | 19.4 | cycles |
| t243 branchy unsorted / branchy sorted | 2.27 | ratio |
| t243 branchy unsorted / branchless unsorted | 2.24 | ratio |
| t243 branchy sorted / branchless sorted | 0.99 | ratio |
| t243 branchless unsorted / vector unsorted | 4.07 | ratio |
| t243 extra cycles per element, branchy unsorted minus branchy sorted | 1.28 | cycles |
| t243 implied cost per mispredict (extra cycles / 0.0505 mispredict rate) | 25.3 | cycles |
<!-- results:end -->

## Analysis

The branchy loop over unsorted data costs 10.71 cycles per element at threshold 128; the same loop over sorted data costs 1.01, and the branchless select over unsorted data 1.02, so the ratios are 10.62 and 10.54 while branchy sorted against branchless sorted is 0.99. The predictable forms sit at the floor of one iteration per cycle, which is set by three equal one-cycle loop-carried chains, the `subs` on the counter, the post-incremented pointer and the `add` on the accumulator, so any one of them alone gives the same floor; that is why the sorted branchy loop at 243, where the add executes for only one element in twenty, sits at the same floor, 1.00 cycles per element, and why the floor doubles as a check on the clock estimate. The extra 9.70 cycles per element of the unpredictable branch is therefore misprediction cost alone, and at a 0.4995 mispredict rate that is 19.4 cycles per mispredict: the time from the flush until the next branch resolves, which is the depth of the pipeline from fetch to the branch unit with the load the branch depends on sitting in that path. At threshold 243 the condition is true for 5.1 percent of the elements; the emitted `b.lo` is taken when it is false, so the predictor settles on the common, taken direction and mispredicts the roughly one in twenty that fall through to the add, which makes the unsorted loop cost 2.28 cycles per element, 2.27 times the sorted loop and 2.24 times the branchless select, with an implied cost per mispredict of 25.3 cycles, an upper estimate because the predictor mispredicts at least 0.0505 of the elements rather than exactly that fraction. Going from a condition that is true for 50.0 percent of the elements to one that is true for 5.1 percent turns the branch from one taken half the time into one taken almost always, yet the sorted branchy cost stays at the floor (1.01 cycles per element at 128, 1.00 at 243) and the unsorted cost falls from 10.71 to 2.28: what a branch costs is set by how often it is mispredicted, not by how often it is taken. The branchless select does not care about order or threshold, 1.02 cycles per element in all four of its cells, because there is no branch to predict. The compiler's own vectorised form of the same select runs at 0.25 cycles per element in all four of its cells, 4.06 times faster than the scalar select at threshold 128 and 4.07 at 243, because it has no per-element chain and handles a full vector of elements per instruction, and it is likewise unaffected by order.

## Limits

There is no PMU access from user space on macOS, so mispredict counts are inferred from the fraction of elements for which the condition holds rather than counted; the 25.3-cycle figure at threshold 243 is therefore an upper estimate. Apple publishes no pipeline depth, so the 19.4 cycles cannot be checked against a specification; it is the effective cost of a branch whose condition depends on a load, which includes the time for that load after a flush empties the machine, and a branch on a register already in hand would show fewer cycles. The array cannot be shrunk to fit L1 to remove the memory side: an array of a few thousand elements repeated many times is learned by the predictor within a few passes, since the global branch history identifies the position in the sequence, and that measures predictor capacity rather than misprediction cost. The 16777216-byte array equals the L2 size, so part of each pass streams from DRAM; the sequential access lets the prefetcher hide that, as the one cycle per element of the predictable loops shows, but a random access pattern would add memory latency on top of the branch cost. DVFS is on and the load average at the start of the committed run was 7.75 on 14 logical CPUs, which is why the cv column is shown; median and min agree within 1 percent on every row, and the ratios do not depend on the clock. On an x86 server part the picture is the same with different constants: the compiler emits `cmov` for the select and AVX2 or AVX-512 masks for the vector form, and SMT lets a second thread use the cycles a flushed pipeline wastes, which lowers the visible cost per core but not for the thread that mispredicted. Two things to check in `bench_O2.s` before quoting a Linux gcc `-O2` run: gcc 12 and later vectorise at `-O2` only under the very-cheap cost model, which does not vectorise `sum_vector` for an unknown trip count, so the vector row would be scalar `cmov` and equal the branchless row; and gcc may leave a branch in `sum_branchless` rather than if-convert it. Look for `cmov` in `sum_branchless` and packed compares in `sum_vector`; the branchy unsorted against branchy sorted comparison, which carries the claim, depends on neither.

## Reproduce

    ./run.sh            # full run, about 2 seconds
    QUICK=1 ./run.sh    # smoke test, about 1 second; writes results/quick-raw.txt and
                        # results/quick-summary.md (gitignored) and leaves README.md alone
    REPS=51 ./run.sh    # more timed passes per variant; fewer than 10 is refused outside QUICK

`run.sh` builds with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `REPS` reaches only `bench`; the clock estimate keeps its own 7 runs. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from.
