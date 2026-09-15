# 14. Memory bandwidth by thread count and working set

**Claim.** Vendor peak memory bandwidth is a package number: one core reaches only a fraction of it, and a benchmark that runs single-threaded (or that fits in cache) cannot reveal it, so a bandwidth figure without thread count and working set is not a measurement. Supports: "STREAM Benchmark Reference Information" and "Memory Bandwidth and Machine Balance in Current High Performance Computers" in README section 14, Benchmarks, subsection "Microbenchmark suites".

**Method.** `bench.c` runs the STREAM triad `a[i] = b[i] + s * c[i]` over three float64 arrays of 536870912 bytes each (512 MiB, 67108864 elements) with two working sets, each swept over a thread count. The large case streams the whole arrays, 32 times the 16777216-byte L2 of one P-core cluster and 14 times the 37748736-byte sum of every L2 on the part, so a pass comes from DRAM; it runs with 1, 2, 4, 8, 10 and 14 pthreads at default QoS, each thread taking a static contiguous partition cut on a 128-byte line boundary. The cache case gives thread t the t-th 1048576 bytes (1 MiB) of each array, 3145728 bytes per thread, which fits the cluster's L2 and not the 131072-byte L1d, and runs with 1, 2, 4, 8 and 10 threads, so that five threads on one cluster hold 15 MiB of its 16 MiB L2; 14 is left out because four threads would put 12 MiB into the 4194304-byte L2 of the efficiency cluster. Each thread repeats its slice for 512 passes per round, the passes that move the bytes of one pass over a 512 MiB array, so a thread does the same work per round in both cases and a round lasts about as long whatever the thread count. The single-thread cache configuration runs first and again after the whole sweep, because its figure follows the core and its cluster rather than the memory side and moves from run to run; the repeat shows how far it moves within one run. The main thread is worker 0, so a run with T threads has exactly T runnable threads. Every thread first-touches its partition before the first barrier, so the page faults land on the thread that will stream the pages, and any fault or reclaim that still lands after that is absorbed by the discarded warmup rounds. A round is a spin barrier, the triad over each partition, and a second spin barrier; every thread reads `now_ns()` after leaving the first barrier and before arriving at the second, and the round time is the latest end minus the earliest start across the threads, the span in which the work was done, which no single thread being descheduled can shorten. The same stamps give each thread's own rate, reported as the fastest and slowest partition, and a round whose span exceeds the slowest thread's own time by more than 20 percent is counted as a round with a late thread: some thread was not running while the others were, which is the scheduler and not the hardware, and the count is a column of the table so that a slow core (the span equals its own time) can be told from a thread the scheduler held back. Warmup is by time and by count: rounds are discarded until at least 100 ms have run and at least 3 rounds have gone, because a core that was idle takes tens of milliseconds to reach its working clock, a single short round would be timed inside that ramp, and a single long round of page faults would satisfy the time on its own without the loop having run at speed; then 21 rounds are timed. Bytes are counted the STREAM way, 24 per element for the two reads and the write the loop asks for, and GB/s is decimal (bytes per nanosecond), the unit the vendor uses. Bandwidth is throughput-like, so the statistic is the median with `cv`; the best round, which is what STREAM reports, is shown beside it, and scaling is against one thread over the same working set. Every value in `b` and `c` is a small integer, so `b + s*c` is exact whether or not the compiler fuses the multiply and add, and the sum of `a` is an exact integer however it is ordered; `a` is reset to -1 before each configuration, and after it the sum of `a` goes through `SINK()` and is compared with a reference computed in integer arithmetic. A mismatch fails the run. `run.sh` records the load average, swap use and memory compressor occupancy at the start of the run at the top of `results/raw.txt`, and the summary quotes them above the table, because a bandwidth benchmark that shares the machine with a browser or an active compressor reads lower and noisier and a reader should be able to tell a quiet run from a loaded one.

**Vendor figure.** Apple states 273 GB/s for M4 Pro in the launch announcement, [Apple introduces M4 Pro and M4 Max](https://www.apple.com/newsroom/2024/10/apple-introduces-m4-pro-and-m4-max/) (30 October 2024): "M4 Pro supports up to 64GB of fast unified memory and 273GB/s of memory bandwidth". The tech specs page for this machine, [MacBook Pro (14-inch, M4 Pro or M4 Max, 2024)](https://support.apple.com/en-us/121553), lists the same "273GB/s memory bandwidth" under Chip for the 14-core M4 Pro. Apple publishes the total and nothing about the interface behind it; 273 × 10^9 bytes per second is 32 bytes per transfer at 8533 million transfers per second, the arithmetic of a 256-bit interface at the LPDDR5X-8533 data rate, which is to say the figure is the transfer rate of the memory interface and not the outcome of any loop. `run.sh` passes it to the program as `VENDOR_GBS` so the fractions in the table are computed from it, and it appears in the table as `vendor figure`.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. The triad is a NEON loop over eight elements per iteration: four `ldp` of two 128-bit registers each for `b` and `c`, four `fmla.2d` with `s` broadcast in `v1`, and two `stp` into `a`, followed by a scalar `fmadd` loop for the remainder. `restrict` on the three pointers is why there is no runtime overlap check in front of it.

```
_triad:
LBB2_4:
	ldp	q2, q3, [x10, #-32]      ; b[i..i+3]
	ldp	q4, q5, [x10], #64       ; b[i+4..i+7]
	ldp	q6, q7, [x11, #-32]      ; c[i..i+3]
	ldp	q16, q17, [x11], #64     ; c[i+4..i+7]
	fmla.2d	v2, v6, v1               ; b + s*c, two lanes each
	fmla.2d	v3, v7, v1
	fmla.2d	v4, v16, v1
	fmla.2d	v5, v17, v1
	stp	q2, q3, [x9, #-32]       ; a[i..i+3]
	stp	q4, q5, [x9], #64        ; a[i+4..i+7]
	subs	x12, x12, #8
	b.ne	LBB2_4
```

The timed region in `_worker` (and the identical one in `main`) is a `clock_gettime`, a loop of `bl _triad` over the passes, and a second `clock_gettime`, so the kernel is a real call with its result in memory between the two clock reads:

```
_worker:
	bl	_clock_gettime
	...
LBB1_12:
	...
	mov	x3, x20
	bl	_triad
	add	w27, w27, #1
	ldr	w8, [x19, #40]
	cmp	w27, w8
	b.lt	LBB1_12
LBB1_13:
	mov	x1, sp
	mov	w0, #4
	bl	_clock_gettime
```

After the rounds `main` sums `a` with a dependent `fadd` chain over `ldp q` loads, stores the sum and passes its address to the `SINK()` asm, then runs the integer reference loop and compares the two:

```
LBB0_94:
	ldp	q0, q1, [x8, #-32]
	...
	fadd	d14, d0, d7
	subs	x9, x9, #8
	b.ne	LBB0_94
	...
	str	d14, [sp, #576]
	add	x11, sp, #576
	; InlineAsm Start
	; InlineAsm End
LBB0_96:
	and	x11, x8, #0x3fe
	ubfx	x12, x8, #3, #10
	mul	x12, x12, x13
	...
	b.ne	LBB0_96
	...
	ucvtf	d9, x8
	fcmp	d14, d9
	b.eq	LBB0_101
```

`results/raw.txt` ends with `checksums: every configuration produced the reference sum of a`, and each configuration's `RESULT ..._checksum` line carries the reference value (137304735744 for the 512 MiB arrays; 268173312, 536346624, 1072693248, 2145386496 and 2681733120 for the cache case with 1, 2, 4, 8 and 10 slices) with `ok` beside it.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro in a MacBook Pro (14-inch, 2024), model identifier Mac16,7; 10 performance and 4 efficiency cores, Armv9-class out-of-order cores for which Apple publishes no microarchitecture name; unified memory on the package. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: 1, 2, 4, 8, 10 and 14 threads over the large arrays and 1, 2, 4, 8 and 10 over the cache slices, at default QoS, nothing pinned since macOS has no affinity API; the main thread is one of the workers. From 4 threads the partitions of the large arrays run at the same rate because the total is memory-bound, which says nothing about placement; at 14 the slowest partition runs at about half the rate of the fastest in every run and the round ends when it does, consistent with four threads on E-cores, and placement can be neither set nor read on macOS. No SMT on this part.
- Frequency: 4.49 GHz, the `estimated clock` line in `results/raw.txt`, measured by `../common/clock_estimate` (dependent 1-cycle add chain, 400000000 adds, min of 7 runs) at the same QoS just before the benchmark. DVFS is on and cannot be disabled, which is why warmup is by time; the multi-thread plateau is set by the memory side and the single-thread and cache-resident figures by the core and its cluster, so the clock bears on those.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -pthread -o bench bench.c`. No `-ffast-math`; clang's default `-ffp-contract=on` fuses `b + s*c` into `fmla`, which is exact on the integer-valued data.
- Workload: the STREAM triad over three arrays of 536870912 bytes (512 MiB, 67108864 float64 each), 1610612736 bytes counted per pass at 24 bytes per element, first touched by the thread that streams each partition; and the cache case, 1048576 bytes of each array per thread for 512 passes per round, 1610612736 bytes counted per thread per round.
- Baseline: the vendor figure of 273 GB/s, and the single-thread run over the 512 MiB arrays.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) read by every thread after the start barrier and before the end barrier; round time is the latest end minus the earliest start; warmup rounds discarded until at least 100 ms have run and at least 3 rounds have gone; 21 timed rounds per configuration; median and cv reported, best round shown, fastest and slowest partition as medians over rounds, and the count of rounds whose span exceeded the slowest thread's own time by more than 20 percent; the sum of `a` consumed with `SINK()` and checked against an integer reference after every configuration. The machine condition is the `load average at start` line in `results/raw.txt`, load average 11.55 8.77 8.24, with 12138.06M of 13312.00M swap in use, 446 MB free and 11642 MB held by the memory compressor: other processes belonging to the user were running, and the benchmark, single-threaded except where the threads column says otherwise, competed for cores only where the table's cv says so.

## Results

<!-- results:start -->
Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). STREAM triad a[i] = b[i] + s*c[i] over three float64 arrays. Large case: 536870912 bytes (512 MiB) per array, split into one contiguous partition per thread and streamed once per round. Cache case: the first 1048576 bytes (1 MiB) of each array per thread, 3 MiB per thread in all, repeated for 512 passes per round, so a thread does the same work per round in both cases. 24 bytes counted per element, decimal GB/s. Median over 21 timed rounds per configuration, after discarded warmup rounds covering at least 100 ms and numbering at least 3; the best round is what STREAM would report. Fastest and slowest thread are the medians of each round's quickest and slowest partition; scaling is against one thread over the same working set. A round has a late thread when its span exceeds the slowest thread's own time by more than 20 percent, which means some thread was not running while the others were. The single-thread cache configuration runs first and again last. Vendor figure: 273 GB/s. Load at the start of this run: load average 11.55 8.77 8.24; swap total = 13312.00M  used = 12138.06M  free = 1173.94M  (encrypted); free 446 MB  compressor 11642 MB.

| per array | threads | median GB/s | best GB/s | cv | fraction of 273 GB/s | scaling vs 1 thread | fastest thread GB/s | slowest thread GB/s | rounds with a late thread |
|---|---|---|---|---|---|---|---|---|---|
| 1 MiB per thread (L2) | 1 | 123.2 | 129.8 | 1.3 % | 0.451 | 1.00 | 123.2 | 123.2 | 0 |
| 1 MiB per thread (L2) | 2 | 238.1 | 247.1 | 1.1 % | 0.872 | 1.93 | 119.1 | 119.0 | 0 |
| 1 MiB per thread (L2) | 4 | 458.5 | 466.9 | 1.1 % | 1.680 | 3.72 | 138.7 | 114.6 | 0 |
| 1 MiB per thread (L2) | 8 | 691.9 | 715.0 | 1.7 % | 2.534 | 5.62 | 106.4 | 86.5 | 0 |
| 1 MiB per thread (L2) | 10 | 776.9 | 844.4 | 5.0 % | 2.846 | 6.31 | 92.7 | 77.7 | 0 |
| 512 MiB | 1 | 119.2 | 124.1 | 7.9 % | 0.437 | 1.00 | 119.2 | 119.2 | 0 |
| 512 MiB | 2 | 214.2 | 216.6 | 2.2 % | 0.785 | 1.80 | 107.8 | 107.1 | 0 |
| 512 MiB | 4 | 223.7 | 227.8 | 2.0 % | 0.819 | 1.88 | 56.5 | 55.9 | 0 |
| 512 MiB | 8 | 225.8 | 229.0 | 1.2 % | 0.827 | 1.89 | 28.4 | 28.2 | 0 |
| 512 MiB | 10 | 225.5 | 229.3 | 0.8 % | 0.826 | 1.89 | 22.6 | 22.6 | 0 |
| 512 MiB | 14 | 149.4 | 182.5 | 10.5 % | 0.547 | 1.25 | 20.6 | 10.7 | 0 |
| 1 MiB per thread (L2), run again last | 1 | 120.4 | 124.3 | 0.9 % | 0.441 | - | 120.4 | 120.4 | 0 |

| derived | value | unit |
|---|---|---|
| highest 1 MiB per thread median (10 threads) / vendor figure | 2.85 | ratio |
| highest 1 MiB per thread median (10 threads) / highest 512 MiB median (8 threads) | 3.44 | ratio |
| 1 MiB per thread, 1 thread, run again last / run first | 0.98 | ratio |
| vendor figure / 512 MiB single thread | 2.29 | ratio |
| 512 MiB 10 threads / 1 thread | 1.89 | ratio |
| 512 MiB 14 threads / 10 threads | 0.66 | ratio |
| 512 MiB 14 threads, fastest thread / slowest thread | 1.93 | ratio |
| highest 512 MiB median (8 threads) / vendor figure | 0.827 | fraction |
| highest 512 MiB median (8 threads) / single thread | 1.89 | ratio |
| highest 512 MiB median (8 threads) x 32/24, the interface traffic if the store read a before writing it | 301.1 | GB/s |
<!-- results:end -->

## Analysis

One thread streaming 512 MiB per array moves 119.2 GB/s, 0.437 of the 273 GB/s Apple states, so the vendor figure is 2.29 times what a single thread can pull, and a single-threaded STREAM on this machine reports a number that says nothing about the package. Two threads reach 214.2 GB/s and from 4 threads the total settles at 223.7 to 225.8 GB/s, 0.819 to 0.827 of the vendor figure and 1.88 to 1.89 times the single-thread rate; 8 and 10 threads add under one percent and only split the same total into smaller shares, 56.5 down to 22.6 GB/s per thread, with the fastest and slowest partition agreeing at each of those counts (56.5 against 55.9, 28.4 against 28.2, 22.6 against 22.6). That plateau is the sustainable bandwidth in McCalpin's sense, what unit-stride loops get from memory, and it is 0.827 of the interface rate at best. With 14 threads the total falls to 149.4 GB/s, 0.66 of the 10-thread figure, because the four extra threads land on the efficiency cores: the slowest partition runs at 10.7 GB/s against 20.6 for the fastest (1.93 times), a static partition waits for the slowest, and with no idle core left the cv rises to 10.5 percent, though no round had a late thread, so the round time was the slow threads' own time and not the scheduler's. The same loop over 1 MiB per array per thread, data that never leaves the L2, gives 123.2 GB/s from one thread, 0.451 of the vendor figure, and 120.4 when the configuration runs again at the end of the run; that single-thread figure follows the core and its cluster rather than the memory and moves by tens of GB/s from run to run (see Limits), so nothing here rests on it. What does not move is what happens when threads are added: 2 threads give 238.1 GB/s, 4 give 458.5, 8 give 691.9 and 10 give 776.9 GB/s, 2.85 times the 273 GB/s the memory interface can carry and 3.44 times the highest DRAM total, from a loop that caused no memory traffic at all, because each cluster's L2 serves its own threads while DRAM is one resource shared by all of them; a cache-resident figure is a number in the same unit that is not a memory bandwidth, which is why the STREAM run rules insist on arrays far larger than the caches. The same loop on the same machine therefore reports 119.2, 214.2, 225.8 or 149.4 GB/s depending on nothing but the thread count, and 123.2 or 776.9 GB/s from data that never touched DRAM, and none of them is 273: a bandwidth figure that does not say which thread count and working set produced it does not say which of these it is.

## Limits

There is no PMU access from user space on macOS, so where the single-thread limit sits cannot be shown: 119.2 GB/s from one thread against 225.5 from ten says one core's DRAM figure is bounded by the misses it keeps in flight against the fabric's latency and not by the interface, but the count cannot be read. The single-thread cache-resident figure is not a stable number on this machine: over ten earlier runs of the same binary while writing this README it read between 117.8 and 176.4 GB/s across the first and the repeated configuration, sometimes below the single-thread DRAM figure and sometimes 1.4 times it, with a within-run cv of no more than 4 percent that does not expose the drift, and the program cannot attribute it (the cluster's clock, which cluster the thread landed on, or what the other work on the machine held in that cluster's L2); the run in the table, 123.2 first and 120.4 again, lies inside that range. That is why the cache leg of the claim rests on the thread sweep: in the same ten runs the 10-thread cache-resident total read between 652 and 1053 GB/s, never under 2.3 times the vendor figure, and the 8-thread total between 666 and 792; the run in the table, 776.9 and 691.9, lies inside both ranges and every run makes the same point. There is no affinity API, so thread placement is inferred from the per-thread rates rather than set; the 14-thread cell depends on where the scheduler puts the threads, and the cache-resident totals on how many threads share each cluster's L2, which the program can neither choose nor observe. This run shared the machine with other work: the load average of 11.55 8.77 8.24 at its start, the swap use and the compressor occupancy quoted above the table show other processes belonging to the user and an active memory compressor competing for cores and memory, the cv and late-thread columns report what that did to each row (the single-thread DRAM row's cv of 7.9 percent is the widest of any row but the 14-thread one), and in two of those ten earlier runs the 10-thread DRAM row read 163 and 178 GB/s with no late round, a thread taken off its core in the middle of its passes, which shows as a slow partition and not as a late one; a late-thread count of zero therefore does not mean no thread was descheduled, and the single-thread DRAM configuration discarded exactly the minimum of 3 warmup rounds, so the first rounds after first touch were long enough (page reclaim on a machine with 446 MB free) to cover the 100 ms on their own. The 24-byte convention counts what the loop asks for; a store that read `a` from DRAM before overwriting it would make the 8-thread row 301.1 GB/s of interface traffic, above the 273 GB/s a 256-bit LPDDR5X-8533 interface can carry, so at this size the store path does not fetch `a` from DRAM and the interface utilisation behind the 0.827 lies between 0.827 and 1.0; a compiler that emitted non-temporal stores would change the moved bytes without changing the counted ones. No interface reaches its transfer rate under a real access stream, since refresh, page opens and read-to-write turnarounds all take cycles, so the plateau is short of the vendor figure on every machine; only the fraction is specific to this one. The 14-thread result is a property of the static partition as much as of the cores: a dynamic partition or ten threads confined to P-cores would hold the plateau, and the point is that the thread count and its placement must be reported, not that 14 is wrong. On an x86 server the constants move against the single thread: a socket has eight or twelve DDR5 channels and a core's line fill buffers bound it at a smaller fraction of the socket figure than here, so more threads are needed to reach the plateau; with two sockets the pages' first touch decides which channels answer (benchmark 10), a thread count without a socket count says even less, and SMT siblings share the same miss resources so they add threads without adding bandwidth.

## Reproduce

    ./run.sh              # full run, about 6 seconds
    QUICK=1 ./run.sh      # smoke test, about 3 seconds; writes results/quick-raw.txt and
                          # results/quick-summary.md and leaves README.md alone
    REPS=51 ./run.sh      # more timed rounds per configuration
    ARRAY_MIB=1024 ./run.sh   # larger arrays (3 GiB in all)
    VENDOR_GBS=307 ./run.sh   # divide by another vendor figure

`run.sh` builds with `build.sh`, writes the machine description, the load the machine is under and the clock estimate to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from.
