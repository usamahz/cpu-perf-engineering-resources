# 02. Latency versus throughput

**Claim.** Out-of-order execution hides independent work but not a dependency chain: the same number of floating-point adds over the same data runs several times faster as 8 independent accumulators than as one chain, and reaches the `fadd` throughput limit of four adds per cycle at 16, because the chain is bound by the add latency and the independent forms by the add throughput. Supports: "Performance Speed Limits" in README section 2, Microarchitecture, subsection "What the manuals leave out".

**Method.** `bench.c` fills an array of 16384 floats, 65536 bytes, with the values 0, 1, 2 and 3 from a fixed-seed LCG, then sums it with five kernels that are the same loop body with a different accumulator assignment: element `j` of every group of sixteen goes to accumulator `j mod K` for `K` = 1, 2, 4, 8 and 16, with each accumulator a separate `float` variable. The five kernels execute the same loads, the same `fadd`s and the same loop overhead; only the dependency graph changes. There is no `-ffast-math`, so the compiler must keep the order the source wrote, and an empty `__asm__("" : "+w"(s))` on each accumulator once per sixteen elements keeps every accumulator in its own scalar register (without it clang packs pairs of accumulators into `fadd.2s`, which turns four chains into two). The accumulators persist across 300 passes, so the one-accumulator kernel is one chain of 4915200 dependent adds. A timed region is four calls of a kernel, each call starting from zero and each result checked against the exact reference sum (the values are small integers, so every partial sum is exact in float whatever the association) and consumed with `SINK()`. Every region is preceded by a clock sample (the minimum over three dependent integer add chains of 2000000 adds, about 1.4 ms, which cannot run faster than one add per cycle) and converted to cycles with its own sample, because macOS moves threads and the P-core clusters change clock under load; `common/clock_estimate` is also run just before the benchmark for the machine field. One warmup region per kernel is discarded and 31 timed regions follow. The chain forms are latencies, so their nanosecond figure is the minimum; the independent forms are throughputs, so theirs is the median; cycles per element are the median of the per-region conversions in every row, since the minimum of a ratio would pick the region whose clock sample lagged a clock change. Six register-only inline-asm loops, 20000000 operations each, measure the same two limits with no loads at all: one dependent integer add chain, which is one cycle per add and so checks the clock samples; eight independent integer chains; one `fadd` chain; eight and sixteen independent `fadd` chains; and one `fmul` chain as a second calibration point.

**Generated code.** `build.sh` writes `bench_O2.s` with the same flags as the binary, and `run.sh` copies the loops below out of that file into this README on every full run, so the quoted code is the code that ran and cannot drift from the file it claims to come from. In the one-accumulator loop every `fadd` reads the register the previous one wrote, so the sixteen adds are one chain; the eight-accumulator loop has the same eight `ldp`s, the same sixteen scalar `fadd`s and the same five instructions of loop overhead, but the adds rotate through eight registers, so consecutive adds are independent. The `fadd_chain` loop is the register-only chain: eight dependent `fadd`s on a constant and no loads. A run of empty `; InlineAsm Start` / `; InlineAsm End` pairs is the `PIN` statements, which emit no instruction, and is shown as one line.

<!-- asm-loops:start -->
`sum1`, the inner loop (one accumulator, one chain):

```
LBB4_3:                                 ;   Parent Loop BB4_2 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	ldp	s1, s2, [x11, #-32]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	ldp	s1, s2, [x11, #-24]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	ldp	s1, s2, [x11, #-16]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	ldp	s1, s2, [x11, #-8]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	ldp	s1, s2, [x11]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	ldp	s1, s2, [x11, #8]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	ldp	s1, s2, [x11, #16]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	ldp	s1, s2, [x11, #24]
	fadd	s0, s0, s1
	fadd	s0, s0, s2
	; InlineAsm Start / InlineAsm End, 1 empty pair: the PIN statements, no instruction
	add	x10, x10, #16
	add	x11, x11, #64
	lsr	x12, x10, #4
	cmp	x12, #1023
	b.lo	LBB4_3
```

`sum8`, the inner loop (eight accumulators):

```
LBB7_3:                                 ;   Parent Loop BB7_2 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	ldp	s16, s17, [x11, #-32]
	fadd	s7, s7, s16
	fadd	s6, s6, s17
	ldp	s16, s17, [x11, #-24]
	fadd	s5, s5, s16
	fadd	s4, s4, s17
	ldp	s16, s17, [x11, #-16]
	fadd	s3, s3, s16
	fadd	s2, s2, s17
	ldp	s16, s17, [x11, #-8]
	fadd	s1, s1, s16
	fadd	s0, s0, s17
	ldp	s16, s17, [x11]
	fadd	s7, s7, s16
	fadd	s6, s6, s17
	ldp	s16, s17, [x11, #8]
	fadd	s5, s5, s16
	fadd	s4, s4, s17
	ldp	s16, s17, [x11, #16]
	fadd	s3, s3, s16
	fadd	s2, s2, s17
	ldp	s16, s17, [x11, #24]
	fadd	s1, s1, s16
	fadd	s0, s0, s17
	; InlineAsm Start / InlineAsm End, 8 empty pairs: the PIN statements, no instruction
	add	x10, x10, #16
	add	x11, x11, #64
	lsr	x12, x10, #4
	cmp	x12, #1023
	b.lo	LBB7_3
```

`fadd_chain`, the inner loop (the register-only fadd chain, no loads):

```
LBB10_2:                                ; =>This Inner Loop Header: Depth=1
	; InlineAsm Start
	fadd	s0, s0, s1
	fadd	s0, s0, s1
	fadd	s0, s0, s1
	fadd	s0, s0, s1
	fadd	s0, s0, s1
	fadd	s0, s0, s1
	fadd	s0, s0, s1
	fadd	s0, s0, s1

	; InlineAsm End
	add	x8, x8, #8
	cmp	x8, x0
	b.lo	LBB10_2
```
<!-- asm-loops:end -->

The call site in `main` shows the work cannot be deleted: inside the timed loop the kernel is called through the `ks` table (the `blr`) between two `clock_gettime` calls, its result is stored for the `SINK()` asm (the `InlineAsm` pair), widened (`fcvt`) and compared with the reference (`fcmp`), and a mismatch is counted (the `cinc ... ne`).

<!-- asm-callsite:start -->
```
	bl	_clock_gettime
	ldp	x20, x28, [sp, #376]
	ldr	x8, [sp, #256]                  ; 8-byte Folded Reload
	mov	x26, x8
LBB0_87:                                ;   Parent Loop BB0_83 Depth=1
                                        ;     Parent Loop BB0_86 Depth=2
                                        ; =>    This Inner Loop Header: Depth=3
	mov	x0, x19
	mov	x1, x23
	blr	x22
	str	s0, [sp, #344]
	; InlineAsm Start
	; InlineAsm End
	fcvt	d14, s0
	fcmp	d11, d14
	cinc	w24, w24, ne
	subs	w26, w26, #1
	b.ne	LBB0_87
; %bb.88:                               ;   in Loop: Header=BB0_86 Depth=2
	add	x1, sp, #376
	mov	w0, #4                          ; =0x4
	bl	_clock_gettime
```
<!-- asm-callsite:end -->

`results/raw.txt` prints `checksum <kernel> <sum> reference <sum> ok` for every kernel, the reference sum 7328400 per call is derived from the values as they were generated, and the run ends with `checksums: every call of every kernel matched the reference sum`.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core for which Apple publishes no microarchitecture name, no pipe counts and no latency table. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT. Machine condition: load average at start 7.75 7.54 7.83 (the `load average at start` line in `results/raw.txt`), so other processes belonging to the user were running; the benchmark is single-threaded, so it competed for a core only where the cv column of the table says so.
- Frequency: 4.49 GHz estimated by `../common/clock_estimate` (a dependent one-cycle add chain, minimum of 7 runs) just before the run, and per-kernel medians of the in-process samples taken before every timed region between 4.151 GHz (the sixteen-accumulator kernel) and 4.513 GHz (the results paragraph gives the range). DVFS is on and cannot be disabled, which is why every region carries its own clock sample; the integer add chain row of the second table reads 1.005 cycles per add against 1 by construction, and that difference is the size of the conversion error. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c`. No `-ffast-math`.
- Workload: the sum of 16384 floats, 65536 bytes, which is half the 131072-byte P-core L1d and equal to the 65536-byte E-core L1d, so every pass after the first is an L1 hit; 300 passes per call and four calls per timed region, 19660800 adds per region; five accumulator counts. Six register-only loops of 20000000 operations each.
- Baseline: the one-accumulator chain, and the register-only chains, which give the latency and throughput constants with no loads in the loop.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around each region; one warmup region per kernel discarded; 31 timed regions; minimum ns for chains and median ns for independent forms, cv shown; a clock sample before every region and the median of the per-region conversions as the cycles figure; every call checked against the exact reference sum and consumed with `SINK()`.

## Results

<!-- results:start -->
Array of 16384 floats, 65536 bytes, values 0..3, checksum 7328400 per call. Each timed region is 4 calls of 300 passes; 31 regions per kernel after one discarded warmup, one thread at default QoS. The ns figure is the min for a chain (a latency) and the median for the independent forms (throughputs). Cycles: every region is converted with the clock sampled just before it (min of three short dependent integer add chains), then the median is taken; the per-kernel median clock ranged from 4.151 to 4.513 GHz, and common/clock_estimate read 4.49 GHz just before the run.

| float array sum | min ns/elem | median ns/elem | cv | ns figure | cycles/elem | elem/cycle |
|---|---|---|---|---|---|---|
| 1 accumulator (one chain) | 0.4659 | 0.4697 | 4.1 % | min | 2.112 | 0.47 |
| 2 accumulators | 0.2434 | 0.2480 | 1.1 % | median | 1.119 | 0.89 |
| 4 accumulators | 0.1394 | 0.1409 | 6.1 % | median | 0.636 | 1.57 |
| 8 accumulators | 0.0810 | 0.0824 | 5.5 % | median | 0.371 | 2.69 |
| 16 accumulators | 0.0554 | 0.0612 | 3.8 % | median | 0.255 | 3.93 |

| register-only chains, inline asm | min ns/op | median ns/op | cv | ns figure | cycles/op | ops/cycle |
|---|---|---|---|---|---|---|
| int add, one chain (1 cycle by construction) | 0.2216 | 0.2227 | 1.7 % | min | 1.005 | 0.99 |
| int add, 8 independent chains | 0.0360 | 0.0381 | 4.8 % | median | 0.171 | 5.83 |
| fadd, one chain | 0.4653 | 0.4794 | 4.0 % | min | 2.135 | 0.47 |
| fadd, 8 independent chains | 0.0870 | 0.0871 | 1.4 % | median | 0.393 | 2.54 |
| fadd, 16 independent chains | 0.0554 | 0.0554 | 1.2 % | median | 0.250 | 4.00 |
| fmul, one chain | 0.6890 | 0.6935 | 1.4 % | min | 3.126 | 0.32 |

| derived | value | unit |
|---|---|---|
| speedup of 2 accumulators over the chain (cycles/elem ratio) | 1.89 | ratio |
| speedup of 4 accumulators over the chain (cycles/elem ratio) | 3.32 | ratio |
| speedup of 8 accumulators over the chain (cycles/elem ratio) | 5.69 | ratio |
| speedup of 16 accumulators over the chain (cycles/elem ratio) | 8.28 | ratio |
| fadd latency, from the 1-accumulator array chain | 2.11 | cycles |
| fadd latency, from the register-only chain | 2.13 | cycles |
| fmul latency, from the register-only chain | 3.13 | cycles |
| fadd throughput, array plateau (16 accumulators) | 3.93 | adds/cycle |
| fadd throughput, register-only 16 chains | 4.00 | adds/cycle |
| fadd throughput, register-only 8 chains | 2.54 | adds/cycle |
| int add throughput, register-only 8 chains | 5.83 | adds/cycle |
| chains needed for the plateau if latency were fixed (latency x throughput) | 8.3 | chains |
| 8 accumulators as a fraction of the plateau (elem/cycle ratio) | 0.69 | ratio |
| cycles each chain waits per add, 1 accumulator (1 x cycles/elem) | 2.11 | cycles |
| cycles each chain waits per add, 2 accumulators (2 x cycles/elem) | 2.24 | cycles |
| cycles each chain waits per add, 4 accumulators (4 x cycles/elem) | 2.54 | cycles |
| cycles each chain waits per add, 8 accumulators (8 x cycles/elem) | 2.97 | cycles |
| cycles each chain waits per add, 16 accumulators (16 x cycles/elem) | 4.08 | cycles |
| cycles each chain waits per add, register-only 8 chains (8 x cycles/add) | 3.14 | cycles |
| clock, per-kernel median of the per-rep samples, lowest / highest | 4.151 / 4.513 | GHz |
| clock, common/clock_estimate before the run | 4.49 | GHz |
<!-- results:end -->

## Analysis

The one-accumulator loop costs 2.112 cycles per element and the sixteen-accumulator loop 0.255, a ratio of 8.28, for the same loads, the same adds and the same loop overhead; only the dependency graph differs. The chain is bound by latency: each `fadd` cannot start until the previous one finishes, so 2.112 cycles per element is the `fadd` latency itself, and the register-only chain with no loads reads 2.135, which shows the loads add nothing because they are L1 hits issued well ahead of the add that needs them; the same method reads 1.005 for the integer add it is calibrated on and 3.13 for `fmul`, so the 2.11 is a two-cycle adder plus a few percent that this method cannot resolve. The independent forms are bound by throughput: sixteen accumulators reach 3.93 elements per cycle and the sixteen register-only chains 4.00, so the core sustains four `fadd`s per cycle, and past that point more chains cannot help. Two and four accumulators run 1.89 and 3.32 times faster than the chain, close to the chain count, because the out-of-order core overlaps the chains and the loads and loop counter were never on the critical path. Eight accumulators run 5.69 times faster than the chain, 0.69 of the plateau, not the 8 that a fixed latency of 2.11 and a throughput of 3.93 (8.3 chains for the plateau) would give; the eight register-only chains show a similar shortfall, 2.54 adds per cycle against the array form's 2.69, so the loads are not the cause of the gap to the plateau. What changes is how long each chain waits per add: 2.11 cycles with one accumulator, 2.24 with two, 2.54 with four, 2.97 with eight (3.14 for the eight register-only chains), so the effective latency rises as more chains compete for the four pipes, and the plateau needs sixteen accumulators here rather than the 8.3 that latency times throughput predicts. A plausible mechanism is that each `fadd` is bound to one of the four pipes when it is dispatched, so one pipe can hold two ready adds while another sits idle, but with no PMU access that stays a hypothesis; what the table establishes is that eight separate accumulators are 5.69 times faster than one and sixteen are 8.28 times faster, and that the two limits, 2.11 cycles per add on a chain and 4.00 adds per cycle across sixteen register-only chains, are the latency and throughput of the instruction.

## Limits

There is no PMU access from user space on macOS, so pipe assignment and issue stalls cannot be counted, and the rise in per-chain wait from 2.11 to 2.97 cycles between one and eight accumulators is measured but not explained. Apple publishes no latency or throughput table for this core, so the 2.11 cycles and 4.00 per cycle cannot be checked against a specification; they are what the core does. DVFS is on, macOS may move the thread between cores, and this run was taken at a load average of 7.75 with other processes belonging to the user running, which is why every region carries its own clock sample, why the cv column is shown, and why the integer add chain reads 1.005 cycles rather than 1; the nanosecond columns move with the clock (a kernel whose regions land on a core at a lower clock reads proportionally more nanoseconds per element and the same cycles per element, which happened to the sixteen-accumulator kernel in this run: its median clock sample was 4.151 GHz against 4.513 for the register-only sixteen chains, its median 0.0612 ns per element is 10 percent above its own minimum of 0.0554, and its 0.255 cycles per element sit next to the register-only 0.250), and the cycles and ratios are the numbers to compare across runs. The array fits L1, so this says nothing about a loop whose loads miss; a chain that includes a cache miss is bound by the miss, not the add. The kernels are scalar by construction; with `-ffast-math` the compiler would reassociate the one-accumulator loop into a vector reduction with several accumulators per register and reach the plateau on its own, which is the correct fix in real code when the rounding change is acceptable. On an x86 server part the shape is the same and the constants differ: the compiler emits `addss` in the same chain, the scalar add latency and the number of add pipes set where the knee is, and their product is the most a loop can gain from extra accumulators; AVX2 or AVX-512 multiplies the per-element throughput by the lane count on top; and SMT lets a second thread use the issue slots the chain form leaves idle, which raises the per-core throughput but not the speed of the thread that owns the chain.

## Reproduce

    ./run.sh            # full run, about 4 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds; writes its raw and summary files
                        # under ${TMPDIR:-/tmp} and leaves results/ and README.md alone
    REPS=51 ./run.sh    # more timed regions per kernel

`run.sh` builds with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, and `run.sh` then copies the three inner loops and the timed call site out of that file into the two asm blocks above with awk, so the quoted assembly is never older than the binary. The source also compiles and runs on x86-64 with clang (`addss` and `mulss` in the inline asm, checked under Rosetta on this machine with `cc -target x86_64-apple-macos`, every checksum matching) and compiles without the register-only loops on other architectures, where the cycles columns are skipped. It has not been built with gcc on Linux, and the line most likely to need a change there is the sixteen-chain kernel, whose asm asks for all sixteen `xmm` registers as accumulators with the constant in memory; if a gcc build stops on that kernel with an impossible-constraints error, the fallback is to guard it with `#if` and skip it with a message on that platform.
