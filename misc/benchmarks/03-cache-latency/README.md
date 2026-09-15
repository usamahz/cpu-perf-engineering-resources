# 03. Dependent-load latency across the memory hierarchy

**Claim.** Dependent-load latency steps up at each level of the memory hierarchy: the first step lands at the documented 128 KiB L1; the transition out of the shared 16 MiB L2 is spread from 4 MiB to 64 MiB because TLB reach ends first and the L2 is shared with four other cores; and random access across many pages adds TLB-miss cost on top. Supports: "What Every Programmer Should Know About Memory" and "Measuring Cache and TLB Performance and Their Effect on Benchmark Run Times" in README section 3, Memory hierarchy, subsection "Cache geometry, replacement and misses in flight".

**Method.** `bench.c` builds a random single-cycle permutation (Sattolo's shuffle, fixed seed) over 128-byte nodes and follows it with `p = p->next`, one dependent load per step, so no step can start before the previous load returns and the time per step is the load-to-use latency of wherever the line lives. The working set doubles from 4 KiB to 1 GiB. Two placements are run. Line mode packs the nodes 128 bytes apart, so a working set of W bytes touches W/128 lines and W/16384 pages. Page mode puts one node on every 16 KiB page of a span, at a line offset within its page, so the same number of lines is spread over 128 times as many pages. The offsets are balanced across the 128 lines of a page (each offset is used n/128 times, then the list is shuffled), so page mode has the same L1 set occupancy as its line-mode partner and any difference is translation. That balance is needed because one L1 way on this part is 16 KiB, the size of a page, so the L1 set index is exactly the line offset within the page and page mode can only ever use 128 sets; random offsets would fill those sets unevenly and the associativity misses would read as translation cost. Each page-mode span is paired in the table with the line-mode working set that has the same line count, so the "page over line" column is the cost of address translation on top of the same cache footprint. Every size runs 2^26 loads in total: one discarded warmup run of 6710886 loads, then 10 timed runs of 6710886 loads each, timed with `now_ns()`; the min over the runs is reported (latency-like quantity) with the cv over the runs. Cycles per load is ns per load times the clock that `common/clock_estimate` measured at the top of `results/raw.txt`; the program also runs the same dependent add chain itself before the first measurement, which lets DVFS reach the steady clock and cross-checks the value (4.46 GHz in-process against 4.49 GHz from `clock_estimate` in the committed run). Before timing, the program walks the cycle once and aborts unless the first return to the start node is at step n, which proves the permutation is one n-cycle; it prints the node it ended on as a checksum, and the returned pointer is passed to `SINK()`.

**Generated code.** `build.sh` writes `bench_O2.s` with `cc -std=c11 -O2 -Wall -Wextra -S` from the same flags as the binary, and that file is where the assembly below comes from. The hot loop cannot be removed or shortened by the optimiser because the next address depends on the previous load and the final pointer is consumed. `_chase` is exactly one load per iteration, with the counter off the dependency chain:

    _chase:
        cbz     x1, LBB2_2
    LBB2_1:
        ldr     x0, [x0]
        subs    x1, x1, #1
        b.ne    LBB2_1
    LBB2_2:
        ret

and the timed region in `run_one` is `clock_gettime`, `bl _chase`, `clock_gettime`, then the store of the returned pointer that the `SINK()` asm statement (the `InlineAsm` markers) consumes:

        bl      _clock_gettime
        ldp     x27, x24, [sp, #176]
        mov     x0, x26
        mov     x1, x25
        bl      _chase
        mov     x26, x0
        add     x1, sp, #176
        mov     w0, #4
        bl      _clock_gettime
        ldp     x8, x9, [sp, #176]
        str     x26, [sp, #152]
        ; InlineAsm Start
        ; InlineAsm End

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, arm64; Apple publishes no microarchitecture name. P-core: 128 KiB L1d, 16 MiB L2 shared by a cluster of 5 cores; 128-byte cache lines, 16 KiB pages, 24 GiB unified memory (all from `sysctl`, printed at the top of `results/raw.txt`).
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing is pinned because macOS has no affinity API. Machine condition: load average at start 8.01 7.60 7.85 (the `load average at start` line in `results/raw.txt`), so other processes belonging to the user were running on the 14 logical cores; the benchmark is single-threaded, so it competed for a core only where the table's cv says so.
- Frequency: 4.49 GHz from `common/clock_estimate` (dependent one-cycle add chain, min of 7 runs), DVFS on, no SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c`.
- Workload: single-threaded pointer chase over a random single-cycle permutation of 128-byte nodes, working sets 4 KiB to 1 GiB doubling, in line placement (128-byte stride) and page placement (one node per 16 KiB page, line offsets balanced across the page).
- Baseline: the 32 KiB line-mode row (an L1 hit) and the 1 MiB line-mode row (an L2 hit) for the level ratios, and for each page-mode span the line-mode working set with the same line count.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around 6710886 dependent loads; 10 runs after one discarded warmup run; min and cv reported; cycles are ns times the estimated clock.

## Results

<!-- results:start -->
One thread on a P-core at default QoS. Each row is the min of 10 runs of 6710886 dependent loads after one discarded warmup run; cv is the standard deviation over the runs divided by their mean. Clock 4.49 GHz from clock_estimate; cycles/load is ns/load times the clock.

**Line mode.** Random single cycle over 128-byte nodes packed 128 bytes apart.

| working set | lines | pages | documented level | ns/load | cycles/load | cv |
|---:|---:|---:|:---|---:|---:|---:|
| 4 KiB | 32 | 1 | L1 (128 KiB) | 0.665 | 3.0 | 0.4% |
| 8 KiB | 64 | 1 | L1 (128 KiB) | 0.665 | 3.0 | 0.3% |
| 16 KiB | 128 | 1 | L1 (128 KiB) | 0.665 | 3.0 | 0.8% |
| 32 KiB | 256 | 2 | L1 (128 KiB) | 0.665 | 3.0 | 0.8% |
| 64 KiB | 512 | 4 | L1 (128 KiB) | 0.665 | 3.0 | 1.5% |
| 128 KiB | 1024 | 8 | L1 (128 KiB) | 0.666 | 3.0 | 2.4% |
| 256 KiB | 2048 | 16 | L2 (16 MiB) | 6.167 | 27.7 | 3.8% |
| 512 KiB | 4096 | 32 | L2 (16 MiB) | 5.927 | 26.6 | 1.1% |
| 1 MiB | 8192 | 64 | L2 (16 MiB) | 5.947 | 26.7 | 0.8% |
| 2 MiB | 16384 | 128 | L2 (16 MiB) | 6.048 | 27.2 | 0.3% |
| 4 MiB | 32768 | 256 | L2 (16 MiB) | 7.224 | 32.4 | 0.9% |
| 8 MiB | 65536 | 512 | L2 (16 MiB) | 7.974 | 35.8 | 1.4% |
| 16 MiB | 131072 | 1024 | L2 (16 MiB) | 16.984 | 76.3 | 6.9% |
| 32 MiB | 262144 | 2048 | memory | 60.041 | 269.6 | 4.0% |
| 64 MiB | 524288 | 4096 | memory | 112.160 | 503.6 | 0.4% |
| 128 MiB | 1048576 | 8192 | memory | 117.349 | 526.9 | 0.1% |
| 256 MiB | 2097152 | 16384 | memory | 120.041 | 539.0 | 0.1% |
| 512 MiB | 4194304 | 32768 | memory | 121.492 | 545.5 | 0.4% |
| 1 GiB | 8388608 | 65536 | memory | 123.707 | 555.4 | 0.3% |

**Page mode.** One 128-byte node per 16 KiB page. The line offsets within the pages are shuffled but balanced, so each of the 128 line slots of a page, and so each L1 set, holds the same number of nodes as in the line-mode partner.

| span | pages (= lines) | ns/load | cycles/load | cv | line mode, same line count | ns/load | page over line |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 512 KiB | 32 | 0.665 | 3.0 | 1.1% | 4 KiB | 0.665 | 1.00 |
| 1 MiB | 64 | 0.665 | 3.0 | 0.4% | 8 KiB | 0.665 | 1.00 |
| 2 MiB | 128 | 0.665 | 3.0 | 0.3% | 16 KiB | 0.665 | 1.00 |
| 4 MiB | 256 | 2.010 | 9.0 | 0.8% | 32 KiB | 0.665 | 3.02 |
| 8 MiB | 512 | 2.019 | 9.1 | 1.0% | 64 KiB | 0.665 | 3.04 |
| 16 MiB | 1024 | 2.105 | 9.5 | 0.7% | 128 KiB | 0.666 | 3.16 |
| 32 MiB | 2048 | 6.115 | 27.5 | 0.8% | 256 KiB | 6.167 | 0.99 |
| 64 MiB | 4096 | 14.029 | 63.0 | 1.3% | 512 KiB | 5.927 | 2.37 |
| 128 MiB | 8192 | 14.740 | 66.2 | 0.7% | 1 MiB | 5.947 | 2.48 |
| 256 MiB | 16384 | 15.031 | 67.5 | 1.1% | 2 MiB | 6.048 | 2.49 |
| 512 MiB | 32768 | 15.050 | 67.6 | 0.7% | 4 MiB | 7.224 | 2.08 |
| 1 GiB | 65536 | 15.760 | 70.8 | 2.0% | 8 MiB | 7.974 | 1.98 |

**Ratios.** The L1 row is the largest line-mode row at or below a quarter of the L1 and the L2 row the largest at or below a sixteenth of the L2.

| ratio | value |
|:---|---:|
| L2 over L1: line 1 MiB / line 32 KiB | 8.9 |
| memory over L2: line 1 GiB / line 1 MiB | 20.8 |
| memory over L1: line 1 GiB / line 32 KiB | 186.0 |
| TLB cost on top, same cache footprint: page 1 GiB span / line 8 MiB, both 65536 lines | 2.0 |
| TLB cost on top, against an L2 hit with no TLB pressure: page 1 GiB span / line 1 MiB | 2.7 |
<!-- results:end -->

## Analysis

The line-mode curve is flat at 0.665 to 0.666 ns, 3.0 cycles, from 4 KiB through 128 KiB, and the first step is at 256 KiB, the first working set larger than the documented 128 KiB L1: 6.167 ns, 27.7 cycles, which then holds within 5.927 to 6.167 ns up to 2 MiB, so an L2 hit costs 8.9 times an L1 hit on this part. The transition out of the L2 is not one step: the curve leaves the plateau at 4 MiB (7.224 ns) and 8 MiB (7.974 ns), is 16.984 ns at 16 MiB, where the working set is the whole documented L2 and the cluster's other four cores were also using it (cv 6.9%, the least stable row, because how much of it still hits depends on what the other cores hold), 60.041 ns at 32 MiB (cv 4.0%), and 112.160 to 123.707 ns from 64 MiB to 1 GiB. Those last rows are the memory latency of the part plus the page walk that a 65536-page working set forces on nearly every load, since that is sixteen to thirty-two times the second-level TLB reach measured below; the page-mode table prices the walk at 14.029 minus 5.927, about 8.1 ns, so DRAM alone is about 116 ns, and the 20.8 and 186.0 ratios over an L2 and an L1 hit carry the same walk. The page-mode table explains the early climb at 4 MiB and 8 MiB: page mode holds L1 latency up to 128 pages and steps to 2.010 ns at 256 pages (3.02 times its partner), so the first-level TLB reach ends between 128 and 256 pages, that is between 2 MiB and 4 MiB of 16 KiB pages, and the 4 MiB and 8 MiB line-mode working sets cross 256 and 512 pages, so translation misses arrive before the L2 runs out. From 256 through 1024 pages the page-mode cost is a flat plateau, 2.010, 2.019 and 2.105 ns against partners at 0.665 to 0.666 ns, about 1.4 ns or 6 cycles for a second-level TLB hit on top of an L1 hit; the L1 holds all 1024 of those nodes (8 per set) and not the 2048 of the next row (16 per set), so it is eight-way with one 16 KiB way per page, which is why the line offsets had to be balanced. From 2048 pages on, page mode therefore cannot hit L1 and those rows compare an L2 hit plus translation against an L2 hit: at 2048 pages the second-level TLB hit adds nothing the table can see, 6.115 against 6.167 ns (0.99 times; the 256 KiB partner is the noisiest row of the L2 plateau, cv 3.8%, and its min sits above the 512 KiB to 2 MiB rows), its cost hidden under the L2 latency, until the page walk appears at 4096 pages, 14.029 ns against 5.927 (2.37 times), so the second-level TLB reach ends between 2048 and 4096 pages, that is between 32 MiB and 64 MiB, and beyond it every load pays a page-table walk on top of an L2 hit. Apple documents neither TLB size, so those two boundaries are measured here, not confirmed against a datasheet, unlike the 128 KiB and 16 MiB cache boundaries. At the largest span the ratio is 2.0: 15.760 ns for 65536 lines spread over 65536 pages against 7.974 ns for the same 65536 lines on 512 pages, which is the claim's last clause, that random access across many pages adds TLB-miss cost on top of the cache miss itself; the partner's 512 pages already exceed the first-level TLB, so this ratio understates the cost against an L2 hit with no TLB pressure, which is 15.760 over 5.947, or 2.7.

## Limits

No PMU is reachable from user space on macOS, so nothing here counts L1, L2 or TLB misses; the levels are inferred from where the curve steps, the L1 associativity from which page-mode rows still hit it, and the two TLB boundaries are inferred only, since Apple publishes no TLB geometry. The L2 is shared by a cluster of five P-cores and other processes were running during the committed run (load average 8.01 at start; the 16 MiB and 32 MiB line-mode rows carry cv 6.9% and 4.0%, the two largest in line mode, and the 16 MiB row spans 16.984 to 20.471 ns across its ten runs in `results/raw.txt`), so the exact latencies between 4 MiB and 64 MiB will move on a quiet machine; the steps will not. The part has no SMT and no NUMA, so there is no remote-node row, and the memory latency is that of a unified memory system with one node. macOS on arm64 has no transparent huge pages, so page mode really does touch one 16 KiB page per node and the memory rows really do pay a page walk; on a machine with 2 MiB pages (Linux with THP, or an x86 server with huge pages) the line-mode memory rows would drop by roughly the walk cost, about 8 ns here, and the page-mode step would move out by a factor of 128 in span, so a 1 GiB span would sit inside the second-level TLB unless THP is disabled for the run. An x86 server part would show four steps rather than three: a 32 to 48 KiB L1, a private 1 to 2 MiB L2, a shared L3 of tens of MiB, then memory at a higher latency than here, with a remote NUMA node higher still; its 64-byte lines make a 128-byte node cover two lines, and its 4 KiB pages (which `bench.c` picks up from `sysconf`) give a second-level TLB with reach of only a few MiB, so the page-mode step would arrive at a much smaller span. `run.sh` reads the cache sizes from the machine description (`sysctl` here, `lscpu` on Linux) for the level column and the ratio rows; where neither gives them the column reads "unknown" and the ratios fall back to the 32 KiB and 1 MiB rows.

## Reproduce

    ./run.sh            # full run, about 65 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds: 2^21 loads per size, working sets to
                        # 64 MiB; writes results/quick-raw.txt and results/quick-summary.md
                        # and leaves README.md alone

`REPS=n` sets the number of timed runs (the 2^26 loads per size are split across them), `STEPS=n` sets that total, and `MAX_MIB=n` caps the largest working set. `run.sh` builds with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from.
