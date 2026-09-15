# 09. First touch

**Claim.** Allocation is not placement: the first touch of freshly mapped anonymous memory pays a page fault per page (16 KiB here), which is the moment the kernel chooses where the page lives, so the first pass over a buffer costs many times more than a later pass over the same pages. Supports: the section lead sentence (a page's node is decided at first touch, not when memory is allocated or a policy is set), "NUMA (Non-Uniform Memory Access): An Overview" in "NUMA and Linux memory placement" and "move_pages(2)" in "Migration, balancing and measured effects", README section 9, "NUMA and multi-socket".

**Method.** `bench.c` maps 1073741824 bytes (1 GiB) of private anonymous memory with `mmap`, which on this machine is 65536 pages of 16384 bytes, and runs four cycles per repetition, each on a fresh mapping. Cycle A is the headline: the `mmap` call is timed on its own; pass 1 stores one byte into every page (the first touch, where every page faults); pass 2 stores one byte into every page again; pass 2 is then repeated a third time; pass 3 stores every 64-bit word of the buffer sequentially; `munmap` is timed on its own. Cycle B maps a fresh buffer and runs the full sequential write straight away, so the faults are inside the bandwidth number. Cycle C runs pass 1 and pass 2 on a fresh mapping with a timestamp round every store, to show how the cost is spread across pages rather than what it costs in total (the clock behind `now_ns()` ticks every 42 ns on this machine, which tells a fault from a plain store but cannot time the plain store); for pass 2 it also reports where the pages above 200 ns sit in the buffer, as the number of contiguous runs they form and the length of the longest run. Cycle D, run after the others, calls `malloc` for the same 1073741824 bytes, touches one byte per page, touches again, and frees; the warmup cycle is the allocator's first request for a block that size and every timed cycle asks again after a `free`, which is where a pool shows itself. Before and after every pass, outside the timed window, the program reads `getrusage` and reports the minor plus major faults the pass took, so each row states its fault count next to its time. Each pass is timed as a whole with `now_ns()`; one warmup repetition is discarded and `REPS` (default 15) timed repetitions follow. Per-page costs are latency-like and reported as the min over repetitions; the two bandwidths are throughput-like and reported as the median; every row carries its cv. For each touch pass that takes no counted faults the program also counts the repetitions whose whole-pass average is above 20 ns per page, which is at least 2.5 % of the pages at the pass 1 cost, and reports the worst repetition, so a stall that recurs on a later pass is on record rather than hidden in the cv. Every pass writes a distinct value, and after the last touch of each mapping the program reads the byte back from every page and compares the sum with the expected value; the full writes use `p[i] = i * k` and their sum is compared with `k * n(n-1)/2` computed in closed form. Every buffer pointer and every checksum is passed to `SINK()`; a mismatch fails the run and `run.sh` then writes no summary. Cycles per page for the touch passes are the min multiplied by the clock estimate from `../common/clock_estimate`, which `run.sh` passes in as `CLOCK_GHZ`. `run.sh` also writes the page queue counts from `vm_stat` (free, active, inactive, speculative, wired, compressor) into `results/raw.txt` just before the benchmark starts, so the memory state of the committed run is on record.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. The touch loop is one byte store per page and nothing else (clang also emits a vectorised path for a stride of one, which never runs here); the full write is two paired 64-bit stores per iteration with the value carried in registers, so it cannot become `memset`:

```
_touch:
LBB1_13:
	strb	w3, [x8]          ; one byte into the page
	add	x8, x8, x2        ; next page
	subs	x9, x9, #1
	b.ne	LBB1_13

_fill:
LBB3_4:
	add	x15, x2, x9
	add	x16, x10, x9
	add	x17, x11, x9
	stp	x9, x15, [x13, #-16]
	stp	x16, x17, [x13], #32
	add	x9, x9, x12
	subs	x14, x14, #4
	b.ne	LBB3_4
```

The call site in `main` shows that the work cannot be deleted or moved: each pass is a call to `_touch` between two `clock_gettime` calls with nothing else inside the pair, the `getrusage` reads sit outside it, the buffer pointer then goes through the `SINK()` asm, the bytes are read back by `_sum_first_bytes`, that result goes through `SINK()` again and is compared with the expected sum.

```
	bl	_getrusage
	...
	bl	_clock_gettime
	...
	mov	w3, #17                         ; =0x11
	bl	_touch
	...
	bl	_clock_gettime
	...
	bl	_getrusage
	...
	bl	_clock_gettime
	...
	mov	w3, #34                         ; =0x22
	bl	_touch
	...
	bl	_clock_gettime
	...
	bl	_getrusage
	...
	bl	_clock_gettime
	...
	mov	w3, #51                         ; =0x33
	bl	_touch
	...
	bl	_clock_gettime
	...
	str	x21, [sp, #880]
	add	x8, sp, #880
	; InlineAsm Start
	; InlineAsm End
	mov	x0, x21
	mov	x1, x19
	mov	x2, x27
	bl	_sum_first_bytes
	str	x0, [sp, #872]
	add	x8, sp, #872
	; InlineAsm Start
	; InlineAsm End
	cmp	x0, x24
	b.eq	LBB0_56
```

The full write is called the same way (`bl _fill` between `clock_gettime` calls, then `SINK()`, `bl _sum_words`, `SINK()`, `cmp x0, x22`). `results/raw.txt` ends with `checksums: every pass wrote what the check read back`, and the two expected fill sums are printed above the timings.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core for which Apple publishes no microarchitecture name. 16384-byte pages, 128-byte cache lines, 24 GiB unified memory, all from `sysctl` at the top of `results/raw.txt`. Page faults are handled by the XNU kernel on the faulting core, in the faulting thread's context.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT. Kernel background work, which this benchmark turns out to be sensitive to, runs on whichever core the kernel picks. Machine condition: the `load average at start` line in `results/raw.txt` reads `10.05 8.17 8.01` on 14 logical cores, so other processes belonging to the user were running; the benchmark is single-threaded, so it competed for a core only where the table's cv says so, which in this run is the 128.3 % cv of pass 2 repeated, one rep at 47.4 ns per page against a 4.3 ns min.
- Frequency: `estimated clock: 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)`, the line `../common/clock_estimate` wrote to `results/raw.txt` at the same QoS just before the benchmark. DVFS is on and cannot be disabled; the per-page costs are dominated by kernel time and memory time, so the clock only matters for the cycles per page row. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c`.
- Workload: a 1073741824-byte private anonymous mapping (65536 pages), 64 times the 16777216-byte L2 of the core's cluster, so nothing about the buffer is cache resident between passes. Per mapping: one byte per page three times, then every word once; a second fresh mapping written in full; a third with every store timed; then `malloc` and `free` of the same size, 15 timed cycles after the first. Page queues just before the run, from `vm_stat` in `results/raw.txt`: 23031 pages free (360 MiB, against the 65536 pages the buffer needs), 293209 active, 293353 inactive, 5237 speculative, 200560 wired and 712782 held by the compressor; on macOS the free count is small by design, since reclaimable file pages stay on the inactive and speculative queues.
- Baseline: pass 2 repeated and the malloc touches, the same loop over the same pages with no faults, for the per-page cost; pass 3 for the full write; the mmap passes for the malloc cycle.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around each whole pass, with `getrusage` read before and after the pass outside the timed window; one warmup repetition discarded; 15 timed repetitions; min and cv for per-page costs, median and cv for bandwidths, min shown for both; cycles per page from the min and the clock estimate; every store checked by reading the buffer back and every result consumed with `SINK()`.

## Results

<!-- results:start -->
Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). Page 16384 bytes; buffer 1073741824 bytes = 65536 pages; one thread on a P-core at default QoS. Per-page rows are the min over 15 timed reps after one discarded warmup (latency-like); bandwidth rows are the median (throughput-like); cv is over the reps. Faults are from getrusage, the median over reps. Page queues just before the run (vm_stat, in pages): free 23031  active 293209  inactive 293353  speculative 5237  wired 200560  compressor 712782; that is 360 MiB free against a 1024 MiB buffer, with the inactive and speculative queues reclaimable on demand.

| step | min | median | cv | faults per rep | unit |
|---|---:|---:|---:|---:|---|
| mmap call, whole buffer | 3.00 | 4.67 | 21.0 % | | us |
| pass 1, first touch, one byte per page | 668.8 | 678.0 | 6.3 % | 65536 | ns/page |
| pass 2, second touch, one byte per page | 167.3 | 239.1 | 35.3 % | 0 | ns/page |
| pass 2 repeated, third touch | 4.3 | 4.5 | 128.3 % | | ns/page |
| pass 3, full sequential write | 79.0 | 91.6 | 4.5 % | 0 | GB/s |
| fresh fill, full write of a new mapping | 15.5 | 18.2 | 4.9 % | 65536 | GB/s |
| munmap, whole buffer | 172.1 | 184.9 | 10.4 % | | ns/page |
| malloc call, after free of the same size | 0.33 | 0.33 | 195.0 % | | us |
| malloc then first touch | 4.2 | 4.2 | 1.6 % | 0 | ns/page |
| malloc then second touch | 4.2 | 4.3 | 2.1 % | | ns/page |
| free | 0.01 | 0.01 | 14.3 % | | ns/page |

Per-page timing (cycle C, every store timed on its own with a 42 ns clock; a page above 200 ns did more than a page walk and a store):

| timed pass | pages above 200 ns, median over reps | min | max | cost | unit |
|---|---:|---:|---:|---:|---|
| pass 1, first touch | 100.0 % | | | 584 | ns, median page, min over reps |
| pass 2, second touch | 25.3 % | 24.6 % | 59.9 % | 574 | ns, mean of the pages above 200 ns, min over reps |

Where the slow pages of the timed pass 2 sit in the buffer, per rep (a run is a maximal stretch of neighbouring pages all above 200 ns):

| timed pass 2, slow pages | median | min | max | unit |
|---|---:|---:|---:|---|
| contiguous runs | 261 | 148 | 263 | runs |
| longest run | 251 | 68 | 28767 | pages |

Reps in which a fault-sized stall recurred on a pass with no counted faults (whole-pass average above 20 ns per page, which is at least 2.5 % of the pages at the pass 1 cost):

| pass | reps above the threshold | worst rep | unit |
|---|---:|---:|---|
| pass 2, second touch | 15 of 15 | 497.9 | ns/page |
| pass 2 repeated, third touch | 1 of 15 | 47.4 | ns/page |
| malloc then first touch | 0 of 15 | 4.4 | ns/page |
| malloc then second touch | 0 of 15 | 4.6 | ns/page |

| derived | value | unit |
|---|---:|---|
| pass 1 / pass 2 repeated (min ns per page; the headline ratio) | 155.0 | ratio |
| pass 1 / malloc then first touch | 158.2 | ratio |
| pass 1 / pass 2 (pass 2 carries the uncounted stalls described in the Analysis) | 4.0 | ratio |
| pass 3 / fresh fill (median GB/s) | 5.0 | ratio |
| mmap call per page | 0.046 | ns/page |
| pass 1 per page at 4.49 GHz | 3003 | cycles/page |
| pass 1 over the whole buffer (pages x min ns per page) | 43.8 | ms |
| pass 3 over the whole buffer (bytes / median GB/s) | 11.7 | ms |
| fresh fill over the whole buffer (bytes / median GB/s) | 59.1 | ms |
| faults on the first malloc of this size (warmup cycle) | 65536 | faults |
| malloc returned the block it had just freed | 15 of 15 | cycles |
<!-- results:end -->

## Analysis

The `mmap` call for 1 GiB takes 3.00 us, 0.046 ns per page, and every one of the 65536 pages then faults on its first store in pass 1 at 668.8 ns per page (3003 cycles at 4.49 GHz, 43.8 ms for the buffer), so the mapping is address space and the memory arrives page by page, each in a trap that finds, zeroes and maps 16 KiB; on a NUMA kernel that trap is also where the page's node is chosen. Pass 2 takes 0 faults and pass 2 repeated runs at 4.3 ns per page, 155.0 times less than the first touch, which is the cost of a page walk and a store once the page exists. Pass 2 itself, at 167.3 ns per page min and 239.1 median with a cv of 35.3 %, sits between the two because on this kernel a share of freshly faulted pages stalls again on the next pass: the timed pass shows 25.3 % of pages (24.6 to 59.9 % across reps) above 200 ns in pass 2 at a mean of 574 ns, the same size as the 584 ns median page of pass 1, while `getrusage` records 0 faults for the pass, and the stalled pages sit in contiguous runs in the order they were faulted, 261 runs per rep at the median with the longest run 251 pages, and in the worst rep one run of 28767 pages. The pattern is consistent with the kernel clearing the reference state of batches of pages it has just handed out and taking a fast fault it does not count when they are next touched: it hits every rep of pass 2 (15 of 15 above 20 ns per page, worst 497.9), recurs on pass 2 repeated in 1 of 15 reps (worst 47.4 ns per page, which is that row's 128.3 % cv and why the min is the statistic quoted), and in none of the 15 malloc cycles, so it is concentrated in the passes that follow the faults, and it is why the pass 1 / pass 2 ratio is 4.0 rather than 155.0. In bandwidth terms the first pass over the buffer is 5.0 times slower than the second: a full write of a fresh mapping runs at 18.2 GB/s with 65536 faults inside it, and the same write over touched pages at 91.6 GB/s with 0 faults, 59.1 ms against 11.7 ms, which is the 43.8 ms of faults plus the write to within 4 ms. The malloc cycle shows what a pool does to placement: the allocator's first request for 1 GiB took 65536 faults, exactly like `mmap`, but every `free` returned in 0.01 ns per page and every later `malloc` handed back the block it had just freed (15 of 15 cycles) in 0.33 us with the pages still resident, so the first touch after `malloc` cost 4.2 ns per page with 0 faults, 158.2 times less than the first touch of a fresh mapping. Whoever touched those pages first decided where they live, and a `free` followed by a `malloc` does not revisit that decision; `munmap`, which does give the pages back, costs 172.1 ns per page, about a quarter of getting them.

## Limits

This machine has one memory system and no NUMA nodes, so the benchmark shows the fault and its cost but not the placement it decides. On a NUMA machine under the default local policy pass 1 is where each page is bound to the node of the core that took the fault, and the malloc cycle is the case that goes wrong in practice: a buffer initialised by one thread and recycled through a pool keeps that thread's node for every later user, and a policy set on the range after the fact, with `mbind(2)` on Linux, changes nothing unless the move flags are set. There is no PMU and no kernel tracing without disabling system protection, so faults are counted with `getrusage` and the fault-sized stalls in pass 2 that it does not count are characterised by their size, share and position rather than attributed to a named kernel path. What the numbers show is that the share of pass 2 pages that stall does not track free memory: `run.sh` records the page queues just before the run, this run started with 23031 free pages (360 MiB) against the 65536 the buffer needs, yet the faults were served at the usual cost, pass 2 took 0 counted faults at the median and 1 at most (one rep, in `results/raw.txt`), and the stalled share ran from 24.6 to 59.9 % across reps; probes of the same loop during development on a 64 MiB buffer with 296 to 537 MiB free, several times the buffer, put the share anywhere between none and a fifth of the pages from one probe to the next. The stalled pages always arrive as contiguous runs of pages in the order they were faulted, with no counted fault, which fits kernel work done on batches of just-faulted pages, such as the page scanner clearing the referenced state of pages it has just placed on the active queue, and not a per-page cost or a shortage of free pages; the run structure and timing support that reading, but the kernel path is inference. The share also varies from run to run with whatever background work the kernel is doing, which is where the cv of pass 2 and its 59.9 % worst rep come from, and why the headline ratio is taken from pass 2 repeated, the pass the stalls mostly leave alone. When memory does run short the picture changes in a way `getrusage` would show: the first touch costs more and pages can be taken back and refaulted in pass 2 as counted faults, and neither happened here. The clock behind `now_ns()` ticks every 42 ns, which is fine for the whole-pass numbers and for telling a 600 ns page from a 5 ns one, but the per-page median of pass 1 is quantised to that step. macOS uses 16 KiB pages and exposes no huge pages for anonymous memory on arm64, so there is one variant. Linux would show two: with 4 KiB pages a 1 GiB buffer takes 262144 faults, each zeroing a quarter as much, and with transparent huge pages a 2 MiB fault zeroes 128 times more per trap than a 16 KiB fault here, and 512 times more than a 4 KiB one, so the per-page cost, the fault count and the bandwidth of the fresh fill all move; `THP=never` and `THP=always` set the mapping's advice on Linux for either case, and `MAP_POPULATE` would move the faults into the `mmap` call without changing which thread's node they land on. The malloc row is an allocator policy, not a kernel one: macOS libmalloc kept the 1 GiB block and its pages across `free`, while glibc returns a block that large to the kernel with `munmap`, so on Linux the malloc row would look like pass 1 unless a pool sits in front of the allocator; both allocators keep the placement of whatever they do recycle. On an x86 server with 4 KiB pages each fault zeroes a quarter as much, so the cost per page is lower and the count per GiB four times higher, and the interesting number there is the node the pages land on, which `move_pages(2)` can report page by page.

## Reproduce

    ./run.sh            # full run, about 6 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds; 64 MiB and 5 reps; writes
                        # results/quick-raw.txt and results/quick-summary.md and
                        # leaves README.md alone
    REPS=31 ./run.sh    # more timed repetitions
    MIB=256 ./run.sh    # a smaller buffer

`run.sh` builds with `build.sh`, writes the machine description, the clock estimate and the `vm_stat` page queue counts to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers; if `bench` exits non-zero or does not print the clean-checksum line, no summary is written and the README is left alone. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from. On Linux, `THP=never` or `THP=always` in the environment selects the page size variant.
