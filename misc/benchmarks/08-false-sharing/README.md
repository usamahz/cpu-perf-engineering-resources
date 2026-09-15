# 08. False sharing

**Claim.** Threads doing atomic increments to different variables 8 bytes apart on one cache line run many times slower than the same threads on separate lines, 4.46 times at 2 threads and 135.23 times at 8 on this part; plain stores pay 1.68 to 2.66 times because the store queue coalesces them; padding each variable to 128 bytes restores linear scaling for both, and 64-byte padding is enough only between cores that share an L2. Supports: "Hoard: A Scalable Memory Allocator for Multithreaded Applications" and "Algorithms for Scalable Synchronization on Shared-Memory Multiprocessors" under "Locks, contention and allocators" in README section 8, Concurrency.

**Method.** `bench.c` starts T threads (1, 2, 4 and 8) and each increments its own 64-bit counter 16777216 (1<<24) times. Nothing varies but where the counters sit and how the increment reaches memory. Three layouts: `adjacent` puts the counters 8 bytes apart, all inside the first 64 bytes of one line; `pad64` puts them 64 bytes apart, two per 128-byte line, which is what a reader who pads to the x86 line size gets on this machine; `pad128` puts them 128 bytes apart, one per line as `sysctl hw.cachelinesize` reports it. Three increments: `store` keeps the count in a register and stores it to the slot after every increment through a `volatile` pointer, so the line sees one plain store per increment and the core is free to run ahead of it; `atomic` is a relaxed `__atomic_fetch_add`, which compiles to one `ldadd` that the core must complete on a line it holds in a writable state; `register` keeps the count in a register and stores it once after the loop, an add chain at one cycle per increment, which is the bound. The counters live in a static 1280-byte buffer aligned to 128 bytes with a guard line at each end, so the working set is at most eight lines, everything sits in L1, and the only memory traffic is coherence. A load-add-store increment was tried first and dropped: its single-thread time swung several-fold between identical runs, which is the core's store-to-load forwarding predictor and not the subject here. Wall time is `now_ns()` from the moment every thread has left a spin barrier until the last thread has been joined, so thread creation is outside the timed region; the number reported is that wall time divided by the increments per thread, nanoseconds per increment per thread, so a flat value across T is linear scaling. For each thread count the seven variants run round-robin, one warmup round discarded and then `REPS` (default 11) timed rounds, so noise that drifts over the run lands on every variant alike. The statistic is the median (a throughput-like quantity) with `cv`, the minimum alongside, and cycles per increment as the median times a clock estimate for that thread count: `run.sh` runs `../common/clock_estimate` once on its own for the single-core clock, then 2, 4 and 8 copies at once and takes the median across the copies, and passes them in as `CLOCK_GHZ` and `CLOCK_GHZ_T2`, `CLOCK_GHZ_T4` and `CLOCK_GHZ_T8`, because DVFS holds the all-core clock below the single-core one and an 8-thread cycle count at the single-core clock would overstate it by about 15 percent. The program also prints the compiled-in pad128 stride and the line size the OS reports (`sysctl hw.cachelinesize` on macOS, `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` on Linux) as separate `RESULT` lines, so a build on a 64-byte-line machine shows the mismatch. After every run the slots are summed, consumed with `SINK()` and compared with T times the increment count; a mismatch fails the run. Each thread also records the cpu it ran on at the start and end of its loop, because on this part the cost of sharing a line depends on whether the two cores share an L2 cluster and macOS decides that, not the program; the per-round times are printed as `samples` lines and the ids as `cpus` lines in `results/raw.txt`, one bracket per round in thread order, a `*` on a thread that moved during its loop, so any round can be matched with where its threads ran.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. The store loop is one `str` and one `add` per increment; the atomic loop is one `ldadd`; the register loop is one `add` with the empty asm keeping it, and its only store comes after the loop.

```
_worker_store:
LBB1_5:
	str	x8, [x20]            ; the count, to this thread's slot, every iteration
	add	x8, x8, #1
	subs	x21, x21, #1
	b.ne	LBB1_5

_worker_atomic:
LBB2_5:
	ldadd	x8, x9, [x20]        ; atomic add of 1 to the slot
	subs	x21, x21, #1
	b.ne	LBB2_5

_worker_reg:
LBB3_4:
	add	x21, x21, #1
	; InlineAsm Start
	; InlineAsm End
	subs	x20, x20, #1
	b.ne	LBB3_4
LBB3_5:
	...
	ldr	x8, [x19]
	str	x21, [x8]            ; one store of the count, after the loop
```

`run_once` is inlined into `main`. The two `clock_gettime` calls bracket only the join loop, which is where the wall time goes; after the second one the slots are summed, the sum goes through the `SINK()` asm and is compared with the expected total, so the increments are consumed and checked.

```
	bl	_clock_gettime
	...
LBB0_45:
	ldr	x0, [x23, x19, lsl #3]
	mov	x1, #0
	bl	_pthread_join
	add	x19, x19, #1
	cmp	x27, x19
	b.ne	LBB0_45
	...
	bl	_clock_gettime
	...
LBB0_48:
	ldr	x13, [x13]           ; slot t
	ldr	x14, [x14]           ; slot t+1
	add	x8, x13, x8
	add	x9, x14, x9
	...
	str	x8, [sp, #392]
	add	x9, sp, #392
	; InlineAsm Start
	; InlineAsm End
	ldr	x9, [sp, #344]
	cmp	x8, x9               ; sum against threads * increments
	b.eq	LBB0_55
```

`results/raw.txt` ends with `checksums: every run summed to threads * increments`, and every variant's `RESULT ..._checksum` line carries the expected sum and `ok`.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro; Armv9-class out-of-order cores for which Apple publishes no microarchitecture name. Ten performance cores in two clusters of five, each cluster sharing a 16 MiB L2, and four efficiency cores sharing a 4 MiB L2. In the `cpus` lines of `results/raw.txt` the ids 4 to 8 and 9 to 13 behave as the two performance clusters and 0 to 3 as the efficiency cores. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: 1, 2, 4 and 8 threads at default QoS, nothing pinned since macOS has no affinity API; in this run every thread ended every round on a performance core, and the placement of each round is recorded in the `cpus` lines. At 2 and 4 threads the threads usually end inside one cluster; the exceptions are five of the 154 rounds (atomic adjacent t2 round 1, store adjacent t4 round 8, store pad64 t4 rounds 4 and 8, atomic adjacent t4 round 1), so every pad64 round but two ended with its threads in one cluster. At 8 threads macOS splits them 3+5 in 33 of the 77 rounds and 4+4 in 28, with 2+6 in 11, 1+7 in 3 and 0+8 in 2, the lopsided splits all in the two long atomic cells whose threads migrate mid-loop; a 3+5 split leaves at least one of the four pad64 pairs with a member in each cluster, and in this run exactly one. No SMT on this part.
- Frequency: 4.49 GHz on one core, the `estimated clock: 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)` line that `../common/clock_estimate` wrote to `results/raw.txt` just before the benchmark; 4.40, 3.96 and 3.91 GHz as the median of 2, 4 and 8 copies run at once, which is the all-core clock under DVFS and what the cycles column of the 2-, 4- and 8-thread rows uses; each copy's reading is in `results/raw.txt`. The register row at 1.00 to 1.02 cycles per increment at every thread count shows both that the cores held those clocks and that the padded rows' rise of 1.15 to 1.17 times per increment at 4 and 8 threads is the clock and nothing else. DVFS is on and cannot be disabled. Machine condition: `machine.sh` recorded `load average at start: 7.66 7.58 7.82`, so other processes belonging to the user were running on the 14 cpus alongside the benchmark's 1 to 8 threads; the benchmark competed with them for cores only where the table's cv says so, and the cells with cv above 10 percent are bimodal with thread placement, as the Limits section sets out, rather than scattered by that load.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -pthread -o bench bench.c`.
- Workload: 16777216 increments per thread of a 64-bit counter, seven variants (store, atomic, register increments over adjacent, pad64 and pad128 layouts), counters in a 1280-byte static buffer aligned to 128 bytes, at most eight lines touched; no memory bandwidth involved, the traffic is coherence between L1 caches and between the two L2 clusters.
- Baseline: the `pad128` layout of the same increment (one counter per 128-byte line), the single-thread run of the same variant, and the `register` variant with no store in the loop.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) from a spin barrier after every thread has started until the last join, divided by the increments per thread; one warmup round per thread count discarded; 11 timed rounds round-robin over the seven variants; median and cv reported, minimum shown; cycles per increment from the median and the clock estimate for that thread count; every run's slot sum consumed with `SINK()` and checked against threads times increments.

## Results

<!-- results:start -->
Single-core clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). The cycles column of a row uses the clock measured with as many concurrent copies of clock_estimate as the row has threads, because the all-core clock sits below the single-core one: 1 thread 4.49 GHz, 2 threads 4.40 GHz, 4 threads 3.96 GHz, 8 threads 3.91 GHz. 16777216 increments per thread, 11 timed rounds after one warmup round, wall time from a barrier after every thread has started until the last join. Median of nanoseconds per increment per thread; cycles is the median times the clock for that thread count. Compiled-in stride for pad128: 128 bytes; the OS reports a 128-byte line.

| threads | increment | layout | stride | median ns/increment/thread | min ns/increment/thread | cycles/increment | cv |
|---|---|---|---|---|---|---|---|
| 1 | store | adjacent | 8 B | 0.223 | 0.222 | 1.00 | 0.7 % |
| 1 | store | pad64 | 64 B | 0.223 | 0.222 | 1.00 | 0.7 % |
| 1 | store | pad128 | 128 B | 0.223 | 0.222 | 1.00 | 1.4 % |
| 1 | atomic | adjacent | 8 B | 1.562 | 1.557 | 7.01 | 0.6 % |
| 1 | atomic | pad64 | 64 B | 1.560 | 1.555 | 7.00 | 0.2 % |
| 1 | atomic | pad128 | 128 B | 1.558 | 1.555 | 6.99 | 0.5 % |
| 1 | register | one store at the end | 128 B | 0.223 | 0.222 | 1.00 | 0.5 % |
| 2 | store | adjacent | 8 B | 0.383 | 0.378 | 1.69 | 1.2 % |
| 2 | store | pad64 | 64 B | 0.229 | 0.227 | 1.01 | 0.5 % |
| 2 | store | pad128 | 128 B | 0.228 | 0.227 | 1.00 | 1.4 % |
| 2 | atomic | adjacent | 8 B | 7.125 | 5.595 | 31.35 | 13.7 % |
| 2 | atomic | pad64 | 64 B | 1.594 | 1.589 | 7.01 | 0.4 % |
| 2 | atomic | pad128 | 128 B | 1.597 | 1.589 | 7.03 | 0.4 % |
| 2 | register | one store at the end | 128 B | 0.229 | 0.227 | 1.00 | 1.0 % |
| 4 | store | adjacent | 8 B | 0.684 | 0.682 | 2.71 | 0.2 % |
| 4 | store | pad64 | 64 B | 0.257 | 0.256 | 1.02 | 10.4 % |
| 4 | store | pad128 | 128 B | 0.257 | 0.256 | 1.02 | 0.6 % |
| 4 | atomic | adjacent | 8 B | 26.339 | 25.549 | 104.30 | 2.4 % |
| 4 | atomic | pad64 | 64 B | 1.790 | 1.788 | 7.09 | 2.1 % |
| 4 | atomic | pad128 | 128 B | 1.791 | 1.788 | 7.09 | 0.1 % |
| 4 | register | one store at the end | 128 B | 0.257 | 0.256 | 1.02 | 0.5 % |
| 8 | store | adjacent | 8 B | 0.458 | 0.428 | 1.79 | 11.3 % |
| 8 | store | pad64 | 64 B | 0.485 | 0.257 | 1.90 | 29.2 % |
| 8 | store | pad128 | 128 B | 0.262 | 0.257 | 1.02 | 0.8 % |
| 8 | atomic | adjacent | 8 B | 244.958 | 229.171 | 957.79 | 3.3 % |
| 8 | atomic | pad64 | 64 B | 3.251 | 2.781 | 12.71 | 7.7 % |
| 8 | atomic | pad128 | 128 B | 1.811 | 1.805 | 7.08 | 0.2 % |
| 8 | register | one store at the end | 128 B | 0.258 | 0.257 | 1.01 | 0.2 % |

Layout ratios from the medians. A ratio of 1.00 means the layout made no difference.

| threads | store adjacent / pad128 | store pad64 / pad128 | atomic adjacent / pad128 | atomic pad64 / pad128 |
|---|---|---|---|---|
| 1 | 1.00 | 1.00 | 1.00 | 1.00 |
| 2 | 1.68 | 1.00 | 4.46 | 1.00 |
| 4 | 2.66 | 1.00 | 14.71 | 1.00 |
| 8 | 1.75 | 1.85 | 135.23 | 1.79 |

Per-thread cost relative to the same variant on one thread, from the medians. 1.00 is linear scaling: T threads do T times the work in the same wall time.

| threads | store adjacent | store pad128 | atomic adjacent | atomic pad128 | register |
|---|---|---|---|---|---|
| 1 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| 2 | 1.72 | 1.02 | 4.56 | 1.03 | 1.02 |
| 4 | 3.07 | 1.15 | 16.86 | 1.15 | 1.15 |
| 8 | 2.06 | 1.17 | 156.78 | 1.16 | 1.16 |
<!-- results:end -->

## Analysis

With one thread the three layouts cost the same, 0.223 ns per increment for the store in every layout and 1.562, 1.560 and 1.558 for the atomic, so the layout itself is free and everything below is the price of a second core touching the line. For the atomic increment the claim holds in full: adjacent counters cost 4.46 times the padded ones at 2 threads, 14.71 at 4 and 135.23 at 8 (7.125, 26.339 and 244.958 ns per increment per thread against 1.597, 1.791 and 1.811), while the padded atomic scales at 1.03, 1.15 and 1.16 relative to one thread, the same 1.02, 1.15 and 1.16 as the register row, so that residual is the all-core clock and not the memory system, which the cycles column confirms at 7.03, 7.09 and 7.08 cycles against 6.99 on one thread. The contended cost grows faster than the thread count, 156.78 times the single-thread cost at 8 threads, because every `ldadd` has to be performed with the line held writable by its core, so the T cores take turns on one line, and at 8 threads the line also has to cross between the two L2 clusters. For the plain store the claim holds only in part on this core: adjacent costs 1.68, 2.66 and 1.75 times padded at 2, 4 and 8 threads, because the store queue lets the core keep running while the line is fetched and many queued stores commit on each acquisition, whereas the padded store costs the register variant's price to within two percent (0.228 against 0.229, 0.257 against 0.257, 0.262 against 0.258), so a store to a private line is free and the whole adjacent penalty is line traffic. The pad64 layout, two counters per 128-byte line but one per 64-byte half, matches pad128 at 2 and 4 threads (ratios 1.00, 1.00, 1.00 and 1.00), where all but two pad64 rounds ended with their threads inside one cluster, and the two that did not, store rounds 4 and 8 at 4 threads, cost 0.275 and 0.353 against that cell's minimum of 0.256 and are the whole of its 10.4 percent cv without moving its median; at 8 threads pad64 costs 1.85 times pad128 for the store and 1.79 for the atomic, where the split is 3+5 in 33 of the 77 rounds and in seven of the store cell's eleven: the four rounds whose pairs all sat inside one cluster (rounds 1, 5, 7 and 10 of its `cpus` line) cost its minimum, 0.257, to within one percent, the same as the pad128 minimum, and six of the seven rounds with a pair straddling the clusters cost nearly twice that; the seventh, round 11, cost 0.281, only a little above the minimum, with a straddling pair at both ends of its loop, which a start-and-end snapshot cannot explain. Sharing is therefore tracked at 64 bytes between cores that share an L2 and at 128 bytes between clusters, and the 128 bytes that `sysctl` reports is the figure to pad to; the 8-thread atomic pad64 cell's 1.79 is not explained by the snapshot, since every one of its rounds had threads move mid-loop and ended with two threads on one cpu, so its number mixes line traffic with the scheduler. The uncontended atomic costs 7.00 cycles (7.01, 7.00 and 6.99 across the three layouts) against 1.00 for a store because each `ldadd` to the same address is a read-modify-write that the core orders after the previous one, which also caps how much one core can gain from queueing them.

## Limits

There is no PMU access from user space on macOS, so line transfers are not counted; the store queue running ahead of the line and the cluster-level granularity are inferred from the timings and the recorded cpu ids, not observed. There is no affinity API, so which cluster a thread lands in is the scheduler's choice, and the `cpus` lines record only where a thread was when its loop began and ended: the pad64 cells at 8 threads mix rounds with and without a pair straddling the clusters, so their medians describe this scheduler as much as this cache and a rerun can put the median on either side of the gap; a thread that moved out and back leaves no mark, which is the likely story of store pad64 round 11 at 8 threads, and sampling the cpu every 2^20 increments inside the loop would be the way to apportion such a round. The four cells with cv above 10 percent in this run are bimodal in their `samples` lines rather than scattered, which points at placement and not at other work on the machine, though the `cpus` snapshot sorts the two modes in only three of them: the 2-thread adjacent atomic has five rounds at 5.595 to 5.757 and six at 7.125 to 7.642, and the snapshot does not separate them, since round 4 is slow with neither thread marked as moved and rounds 1, 5 and 8 are fast with one that was; the 4-thread pad64 store has two slow rounds, 4 and 8, the only pad64 rounds at 2 or 4 threads that ended with a pair straddling the clusters, that its median, 0.257, does not feel; the 8-thread adjacent store is bimodal with the split, its eight 4+4 rounds within eight percent of its minimum, 0.428, and two of its three 3+5 rounds over a third dearer, the third nine percent dearer with a thread marked as moved; the 8-thread pad64 store is bimodal with the straddling pair as above; and the two long 8-thread atomic cells have threads migrating in every round, so the scheduler is part of what their medians measure and the 8-thread adjacent atomic figure is the one a rerun moves most. The 1-, 2- and 4-thread cells other than the two named have cv below 3 percent, and the previous full run, kept in the git history of `results/`, put every ratio in the two derived tables within five percent of this one, the cells named here included, though a placement that lands more of the eleven rounds on the far side of a gap would move those medians further. The plain store result is specific to a core whose store queue coalesces repeated stores to one address; what an x86 part does with this pure store stream is not measured here. The large x86 false-sharing figures in the literature are for load-add-store increments (`counter[i]++`), where every increment also needs the line for its load, and this benchmark dropped that form; a pure store stream on x86 is expected to sit between this store column and the atomic one, and the x86 atomic is a `lock xadd` with the same serialising behaviour as `ldadd`. x86 server parts have 64-byte lines, so pad64 would be the correct padding there, except that Intel's adjacent-line prefetcher pulls in 128-byte pairs, which is why 128 bytes is the portable choice. A two-socket part would add a NUMA case this machine cannot show, the line crossing the socket interconnect at several hundred nanoseconds a transfer, and SMT would add the case where two threads share one core and its L1, so their counters on one line cost nothing. The 1<<24 increments per thread are chosen so the full run takes about a minute; at 1<<27 the 8-thread adjacent atomic cell alone takes about half a minute per round, and `ITERS_LOG2=27` runs it.

## Reproduce

    ./run.sh                 # full run, about 65 seconds, of which the clock estimates are about 3
    QUICK=1 ./run.sh         # smoke test, about 5 seconds; writes results/quick-raw.txt and
                             # results/quick-summary.md and leaves README.md alone
    REPS=21 ./run.sh         # more timed rounds per variant
    ITERS_LOG2=27 ./run.sh   # 1<<27 increments per thread, several minutes

`run.sh` builds with `build.sh`, writes the machine description, the single-core clock estimate and the 2-, 4- and 8-copy clock estimates to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from. The `cpus` lines in `results/raw.txt` give the cpu id each thread ended on in each round, in thread order, so any cell can be checked against where its threads ran.
