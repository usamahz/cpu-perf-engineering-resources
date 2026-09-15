# 06. Array of structs versus structure of arrays

**Claim.** Layout decides the bytes a loop has to move: summing one 4-byte field of a 64-byte struct in an array of structs brings in 16 times the data of the structure-of-arrays form, and at -O2, given permission to reassociate the float sum (a scoped `#pragma clang fp reassociate(on)` here; `-fassociative-math` or `-ffast-math` elsewhere), the structure-of-arrays loop auto-vectorises to full-width NEON loads and adds, while without that permission it keeps a serial add chain, and the array-of-structs loop either stays scalar (the product of two fields) or is vectorised only as a one-lane-at-a-time gather (the sum) that moves no fewer bytes. Supports: "Intel Optimization Reference Manual", "ispc: A SPMD Compiler for High-Performance CPU Programming" and "Auto-Vectorization in LLVM" in README section 6, Single-thread optimisation, subsection "Data layout and loop transforms".

**Method.** `bench.c` stores the same 4194304 (1<<22) records of sixteen floats twice: as an array of 64-byte structs, 268435456 bytes, and as sixteen separate float arrays of 16777216 bytes each. A record is one struct, or one index of the field arrays. Values are uniform in [0,1) with 24 significant bits from a fixed-seed splitmix64 generator, so every value is exact in float. Two kernels read the records: `sum3` returns the sum of field 3, and `dot37` the sum of field 3 times field 7. Each kernel is timed in six forms. `aos_scalar` and `soa_scalar` are the plain loops over each layout compiled in a separate translation unit with `-fno-vectorize -fno-slp-vectorize`, so each is one dependent chain of `fadd` or `fmadd`. `aos_o2` and `soa_o2` are the same loops at plain `-O2`. `soa_strict` is the field-array loop at `-O2` with strict IEEE ordering, which is what a reader gets by default. `soa_neon` is hand-written NEON intrinsics with four independent vector accumulators. Every kernel except `soa_strict` grants the compiler permission to reassociate the float sum with a scoped `#pragma clang fp reassociate(on)`, the single fast-math permission a vectorised reduction needs; nothing else from `-ffast-math` is on. The struct layout has to bring in 64 bytes per record, since a 128-byte line carries two whole records and the loop touches every line; the field arrays bring in 4 bytes per record for `sum3` and 8 for `dot37`. Before every timed pass a 134217728-byte sweep evicts the caches, so every pass reads its input from memory whichever layout it uses. The timing is `now_ns()` around one full pass; one warmup pass of every variant is discarded, then `REPS` (default 31) timed passes are taken round-robin so that noise drifting over the run lands on every variant alike. The statistic is the median of nanoseconds per record (a throughput-like quantity) with `cv`; the minimum is shown alongside. Effective GB/s is the bytes the kernel has to bring in per record divided by the median ns per record. Cycles per record is the median of the per-pass conversions: just before every timed pass, after the eviction sweep, the clock of the core the thread is on is sampled as benchmark 03 samples it (the minimum over three dependent integer add chains of 2000000 adds, about 1.3 ms, register-only so the sweep's effect on the caches stands), and that pass is converted with its own sample, because DVFS is on and one estimate taken before the run can land on a dip and shift the whole column; the median of the conversions is taken rather than the minimum, since the minimum of a ratio would pick the pass whose sample lagged a clock change. `../common/clock_estimate` is also run just before the benchmark; `run.sh` passes it in as `CLOCK_GHZ` for the record, the summary quotes it beside the range of the per-variant median clocks, and it is the conversion on an architecture with no inline asm for the chain. Every result is consumed with `SINK()` and compared with a double-precision reference computed from the struct copy; a deviation above 1e-2 relative fails the run, and the largest deviation seen per variant is printed and tabulated.

**Generated code.** `build.sh` writes `bench_O2.s` and `bench_scalar.s` from the same flags as the two object files. The scalar unit's loops are one load per field read (one for the sum, two for the product) and one dependent `fadd` or `fmadd`; the only difference between the layouts is the address step, 64 bytes against 4.

```
_sum3_aos_scalar:                       ; bench_scalar.s
LBB0_2:
	ldr	s1, [x8], #64             ; one float, then 64 bytes on to the next record
	fadd	s0, s0, s1                ; one dependent chain
	subs	x3, x3, #1
	b.ne	LBB0_2

_sum3_soa_scalar:
LBB2_1:
	ldr	s1, [x1], #4              ; one float, then 4 bytes on
	fadd	s0, s0, s1
	subs	x3, x3, #1
	b.ne	LBB2_1

_dot37_soa_scalar:
LBB3_1:
	ldr	s1, [x1], #4              ; two loads per record
	ldr	s2, [x2], #4
	fmadd	s0, s1, s2, s0            ; one dependent fmadd chain
	subs	x3, x3, #1
	b.ne	LBB3_1
```

At -O2 clang does vectorise the struct sum, but as a gather: each 4-lane register is filled by a scalar `ldr` and three single-lane `ld1.s` inserts 64 bytes apart, sixteen loads for sixteen records, and the loop advances 1024 bytes per iteration. The struct product is not vectorised (the cost model declines it, see the remarks below); it is interleaved into four scalar `fmadd` chains, eight loads for four records.

```
_sum3_aos_o2:                           ; bench_O2.s
LBB2_7:
	add	x11, x9, #76
	add	x12, x9, #140
	add	x13, x9, #204
	...
	ldr	s4, [x9, #12]             ; record 0, field 3
	ld1.s	{ v4 }[1], [x11]          ; record 1, field 3, one lane at a time
	ld1.s	{ v4 }[2], [x12]
	ld1.s	{ v4 }[3], [x13]
	...
	ldr	s7, [x9, #780]
	ld1.s	{ v7 }[1], [x11]
	ld1.s	{ v7 }[2], [x12]
	ld1.s	{ v7 }[3], [x13]
	fadd.4s	v0, v0, v4
	fadd.4s	v1, v1, v5
	fadd.4s	v2, v2, v6
	fadd.4s	v3, v3, v7
	add	x9, x9, #1024             ; sixteen records of 64 bytes
	subs	x10, x10, #16
	b.ne	LBB2_7

_dot37_aos_o2:
LBB6_5:
	ldur	s4, [x9, #-128]
	ldur	s5, [x9, #-64]
	ldr	s6, [x9]
	ldr	s7, [x9, #64]
	ldur	s16, [x9, #-112]
	ldur	s17, [x9, #-48]
	ldr	s18, [x9, #16]
	ldr	s19, [x9, #80]
	fmadd	s0, s4, s16, s0           ; four scalar chains, no vector instruction
	fmadd	s1, s5, s17, s1
	fmadd	s2, s6, s18, s2
	fmadd	s3, s7, s19, s3
	add	x9, x9, #256
	subs	x10, x10, #4
	b.ne	LBB6_5
```

The field-array loops with reassociation permitted are two `ldp` of 32 bytes each for the sum, four for the product, and four `fadd.4s` or `fmla.4s` into four vector accumulators, sixteen records per iteration; the intrinsics compile to the same instructions in a different order. Without reassociation (`soa_strict`) the loads are still wide but the sixteen lanes are moved out one at a time and added into `s0` in program order. For the sum that arithmetic is the same serial `fadd` chain as the scalar unit. For the product clang keeps the multiply vectorised, four `fmul.4s` off the chain, and serialises only the sixteen adds, so the dependent chain is the `fadd` latency rather than the scalar unit's `fmadd` latency; the strict product loop is therefore faster than `dot37 soa_scalar` and still far slower than the reassociated form, as the table shows.

```
_sum3_soa_o2:
LBB4_7:
	ldp	q4, q5, [x9, #-32]        ; 32 bytes, eight records
	ldp	q6, q7, [x9], #64
	fadd.4s	v0, v0, v4                ; four independent vector accumulators
	fadd.4s	v1, v1, v5
	fadd.4s	v2, v2, v6
	fadd.4s	v3, v3, v7
	subs	x10, x10, #16
	b.ne	LBB4_7

_dot37_soa_o2:
LBB8_7:
	ldp	q4, q5, [x10, #-32]
	ldp	q6, q7, [x10], #64
	ldp	q16, q17, [x9, #-32]
	ldp	q18, q19, [x9], #64
	fmla.4s	v0, v16, v4
	fmla.4s	v1, v17, v5
	fmla.4s	v2, v18, v6
	fmla.4s	v3, v19, v7
	subs	x11, x11, #16
	b.ne	LBB8_7

_sum3_soa_neon:
LBB5_3:
	ldp	q4, q5, [x9, #-32]
	fadd.4s	v0, v0, v4
	fadd.4s	v1, v1, v5
	ldp	q4, q5, [x9], #64
	fadd.4s	v2, v2, v4
	fadd.4s	v3, v3, v5
	add	x10, x8, #32
	add	x8, x8, #16
	cmp	x10, x3
	b.ls	LBB5_3

_sum3_soa_strict:
LBB3_7:
	ldp	q1, q2, [x9, #-32]
	mov	s3, v1[3]                 ; lanes moved out one by one
	mov	s4, v1[2]
	mov	s5, v1[1]
	...
	fadd	s0, s0, s1                ; sixteen dependent scalar adds into s0
	fadd	s0, s0, s5
	fadd	s0, s0, s4
	fadd	s0, s0, s3
	...
	subs	x10, x10, #16
	b.ne	LBB3_7

_dot37_soa_strict:
LBB7_7:
	ldp	q1, q2, [x10, #-32]
	ldp	q3, q4, [x10], #64
	ldp	q5, q6, [x9, #-32]
	ldp	q7, q16, [x9], #64
	fmul.4s	v1, v1, v5                ; the multiply stays vectorised, off the chain
	mov	s5, v1[3]
	mov	s17, v1[2]
	mov	s18, v1[1]
	fmul.4s	v2, v2, v6
	...
	fadd	s0, s0, s1                ; only the sixteen adds are serial
	fadd	s0, s0, s18
	fadd	s0, s0, s17
	fadd	s0, s0, s5
	...
	subs	x11, x11, #16
	b.ne	LBB7_7
```

The call site in `main` shows the work cannot be deleted: after the eviction sweep and the three inlined clock-sample chains (each a `clock_gettime` pair around a loop of eight `add x19, x19, #1`, then `x21`, then `x28`), the kernel is called through the variant table between two `clock_gettime` calls, its result is stored and passed through the `SINK()` asm, and then compared with the double reference.

```
	bl	_evict_caches
	...
LBB0_53:                                ; first clock-sample chain
	; InlineAsm Start
	add	x19, x19, #1
	add	x19, x19, #1
	...
	; InlineAsm End
	add	x8, x8, #8
	cmp	x8, x25
	b.lo	LBB0_53
	...                               ; two more chains, then the timed region
	add	x1, sp, #352
	mov	w0, #4
	bl	_clock_gettime
	ldp	x19, x20, [sp, #352]
	ldr	x8, [x21, #16]            ; the kernel from the variant table
	...
	blr	x8
	fmov	s8, s0
	add	x1, sp, #352
	mov	w0, #4
	bl	_clock_gettime
	ldp	x22, x23, [sp, #352]
	str	s8, [sp, #260]
	add	x8, sp, #260
	; InlineAsm Start
	; InlineAsm End
	fcvt	d0, s8
	ldr	d1, [x26, #16]            ; the double reference
	fabd	d2, d0, d1
	fdiv	d8, d2, d1
	fcmp	d8, d13
```

`results/raw.txt` ends with `checksums: every variant matched the double reference within 1e-02 relative`, and the references (2096667.738 for `sum3` and 1048326.786 for `dot37`) are printed above the timings.

**Vectoriser remarks.** `build.sh` also writes `bench_remarks.txt` from `-Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize` over both units. The lines for the twelve kernel loops, verbatim, with the function each belongs to on a comment line before it:

```
; sum3_aos_o2 (the gather above)
bench.c:111:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
; dot37_aos_o2
bench.c:120:5: remark: the cost-model indicates that vectorization is not beneficial [-Rpass-analysis=loop-vectorize]
bench.c:120:5: remark: interleaved loop (interleaved count: 4) [-Rpass=loop-vectorize]
; sum3_soa_strict and dot37_soa_strict (serial adds, see above)
bench.c:136:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
bench.c:143:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
; sum3_soa_o2 and dot37_soa_o2
bench.c:156:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
bench.c:164:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
; sum3_aos_scalar, dot37_aos_scalar, sum3_soa_scalar, dot37_soa_scalar (the unit built with the vectorisers off)
bench.c:63:5: remark: loop not vectorized [-Rpass-missed=loop-vectorize]
bench.c:72:5: remark: loop not vectorized [-Rpass-missed=loop-vectorize]
bench.c:81:5: remark: loop not vectorized [-Rpass-missed=loop-vectorize]
bench.c:89:5: remark: loop not vectorized [-Rpass-missed=loop-vectorize]
```

Two of these remarks would mislead on their own. The struct sum is reported as vectorised, but the vector is assembled one lane at a time and the loop moves exactly the bytes the scalar loop moves. The strict field-array loops are reported as vectorised at width 4, but their adds are a serial chain; the remark describes the loads (and, for the product, the multiply), not the adds. The intrinsics loops (lines 180 and 195) report `loop control flow is not understood by vectorizer`, which is the vectoriser declining code that is already vector; their remainder loops (187 and 202) never run because the record count is a multiple of sixteen. The clock-sample chain (line 283) is reported three times as not vectorised, once for each of the three copies clang inlined into `main`, which is what a register-only inline-asm loop should report.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core with 128-bit NEON for which Apple publishes no microarchitecture name. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT. `results/raw.txt` records `load average at start: 7.02 7.46 7.78`, so other processes belonging to the user were running; the benchmark is single-threaded, so it competed for a core only where the table's cv says so; the two `aos_scalar` rows, at 6.6 and 8.3 percent, carry most of it.
- Frequency: `estimated clock: 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)` from `../common/clock_estimate` at the same QoS just before the benchmark, and 4.512 to 4.513 GHz as the per-variant medians of the clock sampled before every timed pass, which is the conversion the cycles column uses. DVFS is on and cannot be disabled, which is why every pass carries its own sample; the 2.138 cycles per record of the `sum3 soa_strict` loop is within 1 percent of the 2.13-cycle scalar `fadd` latency benchmark 03 measures from a register-only chain, which checks the conversion and shows the thread stayed on a P-core. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1). `cc -std=c11 -O2 -Wall -Wextra -c -o bench_main.o bench.c`, `cc -std=c11 -O2 -Wall -Wextra -fno-vectorize -fno-slp-vectorize -DSCALAR_TU -c -o bench_scalar.o bench.c`, `cc -o bench bench_main.o bench_scalar.o -lm`. Reassociation is granted per kernel by pragma, not by flag; no `-ffast-math`, no `-march`.
- Workload: 4194304 records of sixteen floats. The struct array is 268435456 bytes, 16 times the 16777216-byte L2 shared by the core's cluster; each field array is 16777216 bytes, equal to it; a 134217728-byte sweep before every timed pass evicts both, so every pass streams from memory. Two kernels (`sum3`, `dot37`) in six forms each, twelve variants in one process.
- Baseline: the scalar one-chain loop over each layout (`aos_scalar`, `soa_scalar`), and for the layout comparison at `-O2` the struct loop `aos_o2` against `soa_o2`.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one full pass; one warmup pass per variant discarded; 31 timed passes per variant taken round-robin; median and cv reported, minimum shown; effective GB/s derived from the median; cycles per record as the median of the per-pass conversions, each pass converted with the clock sampled just before it; every result checked against the double reference and consumed with `SINK()`.

## Results

<!-- results:start -->
4194304 records: the array of structs is 268435456 bytes (64 per record) and each of the sixteen field arrays is 16777216 bytes (4 per record); every timed pass starts after a 134217728-byte sweep that evicts the caches, so the data comes from memory. Median over the timed passes; effective GB/s is the bytes the kernel has to bring in per record (64 for the struct layout, 4 or 8 for the field arrays) divided by the median ns per record. Cycles per record: every pass is converted with the clock sampled just before it (min of three short dependent integer add chains), then the median of the conversions is taken; the per-variant median clock ranged from 4.512 to 4.513 GHz, and common/clock_estimate read 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs) just before the run. Max rel err is the largest deviation of any pass from the double-precision reference (sum3 2096667.738, dot37 1048326.786; a deviation above 1e-02 fails the run).

| kernel | variant | bytes/rec | median ns/rec | min ns/rec | cycles/rec | effective GB/s | cv | max rel err |
|---|---|---|---|---|---|---|---|---|
| sum3 | aos_scalar | 64 | 0.6685 | 0.6501 | 3.017 | 95.7 | 6.6 % | 5.53e-05 |
| sum3 | aos_o2 | 64 | 0.6247 | 0.6025 | 2.819 | 102.4 | 1.7 % | 5.91e-07 |
| sum3 | soa_scalar | 4 | 0.5552 | 0.5543 | 2.505 | 7.2 | 1.3 % | 5.53e-05 |
| sum3 | soa_strict | 4 | 0.4737 | 0.4735 | 2.138 | 8.4 | 1.9 % | 5.53e-05 |
| sum3 | soa_o2 | 4 | 0.0439 | 0.0422 | 0.198 | 91.1 | 2.1 % | 5.91e-07 |
| sum3 | soa_neon | 4 | 0.0456 | 0.0445 | 0.206 | 87.7 | 4.4 % | 5.91e-07 |
| dot37 | aos_scalar | 64 | 0.7871 | 0.7776 | 3.547 | 81.3 | 8.3 % | 1.90e-03 |
| dot37 | aos_o2 | 64 | 0.6583 | 0.6388 | 2.969 | 97.2 | 3.1 % | 1.49e-04 |
| dot37 | soa_scalar | 8 | 0.7647 | 0.7452 | 3.449 | 10.5 | 3.8 % | 1.90e-03 |
| dot37 | soa_strict | 8 | 0.4827 | 0.4825 | 2.178 | 16.6 | 3.4 % | 1.90e-03 |
| dot37 | soa_o2 | 8 | 0.0619 | 0.0613 | 0.279 | 129.3 | 2.5 % | 9.57e-06 |
| dot37 | soa_neon | 8 | 0.0621 | 0.0617 | 0.280 | 128.9 | 2.7 % | 9.57e-06 |

| derived | value | unit |
|---|---|---|
| sum3 bytes per record, aos / soa | 16.00 | ratio |
| sum3 aos_scalar / soa_scalar, median ns/rec | 1.20 | ratio |
| sum3 aos_o2 / soa_o2, median ns/rec | 14.23 | ratio |
| sum3 aos_o2 / soa_neon, median ns/rec | 13.70 | ratio |
| sum3 aos_scalar / aos_o2, median ns/rec | 1.07 | ratio |
| sum3 soa_scalar / soa_o2, median ns/rec | 12.65 | ratio |
| sum3 soa_scalar / soa_strict, median ns/rec | 1.17 | ratio |
| sum3 soa_strict / soa_o2, median ns/rec | 10.79 | ratio |
| sum3 soa_o2 / soa_neon, median ns/rec | 0.96 | ratio |
| dot37 bytes per record, aos / soa | 8.00 | ratio |
| dot37 aos_scalar / soa_scalar, median ns/rec | 1.03 | ratio |
| dot37 aos_o2 / soa_o2, median ns/rec | 10.63 | ratio |
| dot37 aos_o2 / soa_neon, median ns/rec | 10.60 | ratio |
| dot37 aos_scalar / aos_o2, median ns/rec | 1.20 | ratio |
| dot37 soa_scalar / soa_o2, median ns/rec | 12.35 | ratio |
| dot37 soa_scalar / soa_strict, median ns/rec | 1.58 | ratio |
| dot37 soa_strict / soa_o2, median ns/rec | 7.80 | ratio |
| dot37 soa_o2 / soa_neon, median ns/rec | 1.00 | ratio |
| clock, per-variant median of the per-pass samples, lowest / highest | 4.512 / 4.513 | GHz |
| clock, common/clock_estimate before the run | 4.50 | GHz |
<!-- results:end -->

## Analysis

At -O2 the struct layout costs 0.6247 ns per record for the sum and the field array 0.0439, a ratio of 14.23, and for the product 0.6583 against 0.0619, a ratio of 10.63, where the byte ratios are 16.00 and 8.00. The effective bandwidth column shows why: the struct passes draw 102.4 and 97.2 GB/s of line traffic and the field-array passes 91.1 and 129.3 GB/s, so all four loops run at the bandwidth one core can pull from memory and the time ratio is the ratio of bytes moved; the product exceeds 8.00 because two field streams draw 129.3 GB/s where one struct stream draws 97.2, and the sum falls short of 16.00 because the struct pass draws 102.4 GB/s and the single field stream 91.1, a difference this benchmark measures but cannot explain without a PMU (the gather issues two loads per 128-byte line and the field loop eight, so more lines may be in flight for the struct pass, but that is a hypothesis). The scalar rows hide the layout effect, `aos_scalar` over `soa_scalar` being 1.20 for the sum and 1.03 for the product, because a one-chain float reduction is bound by the `fadd` latency: `sum3 soa_strict` at 2.138 cycles per record is within 1 percent of the 2.13-cycle scalar `fadd` latency benchmark 03 measures from a register-only chain, and the struct forms sit at 3.017 and 3.547 cycles per record where memory rather than the chain sets the pace. `sum3 soa_scalar`, the same one-chain `fadd` loop taken one record per iteration, reads 2.505 cycles per record, 17 percent above the strict loop's 2.138; with no PMU the cause (a taken branch every two cycles, or the add consuming the load result directly rather than a lane moved out of a wide load) is not resolved here, and it does not change the layout comparison. Vectorising the struct sum changed nothing that matters, `aos_scalar` over `aos_o2` being 1.07, because the gather still issues one load per record and brings in every line whole; the struct product, which the cost model refused to vectorise, gains only 1.20 from its four scalar chains for the same reason. Vectorising the field-array loop is worth 12.65 for the sum and 12.35 for the product over the scalar chain, and the compiler's own code matches the hand-written intrinsics (`soa_o2` over `soa_neon` is 0.96 and 1.00), so the intrinsics buy nothing here. `soa_strict` shows that a `vectorized loop` remark is not a speedup: without permission to reassociate, clang loads four lanes at a time but adds them one by one into one accumulator, and the loop runs at 10.79 and 7.80 times the reassociated form; for the sum the strict loop is the same `fadd` chain as `soa_scalar` and runs 1.17 times faster than it for the unresolved reason above, while for the product it is 1.58 times faster than `soa_scalar` because the multiply stays vectorised and the chain is the 2.178-cycle `fadd` latency rather than the scalar unit's 3.449-cycle `fmadd` chain. The max rel err column carries a second lesson: the one-chain forms of the product come out 1.90e-03 low, because addends below half an ulp of a float accumulator the size of the 1048326.786 reference vanish and the product of two uniform values crowds towards zero, while the four-accumulator vector forms hold sixteen partial sums a sixteenth the size and land within 9.57e-06.

## Limits

There is no PMU access from user space on macOS, so the bytes moved are inferred from the layout and the 128-byte line, not counted; a line that the prefetcher fetched and the loop did not use would not show. Apple does not document the system-level cache, so the 134217728-byte sweep is assumed, not shown, to clear it; the sweep is eight times the L2 and the effective GB/s figures sit where memory streaming would put them. The bandwidth ceiling here is one core's, roughly 80 to 130 GB/s (the struct rows draw 81.3 to 102.4 and the vectorised field-array rows 87.7 to 129.3); more threads would raise it, and the ratio between the layouts would hold only until the package bandwidth binds. DVFS is on and other processes were running (the load average is in the Machine section), which is why the cv column is shown and why every pass carries its own clock sample: one estimate taken before the run can land on a DVFS dip or a peak the passes do not share (in this run the 4.50 GHz estimate and the 4.512 to 4.513 GHz per-variant medians agree within 0.3 percent, but single per-pass samples in `results/raw.txt` dipped to 3.808 GHz, 16 percent below the median), which would shift the whole cycles column by that much, and the per-pass conversion removes that; the `sum3 soa_strict` row against benchmark 03's 2.13-cycle `fadd` latency is the check on it, and the ratios and the effective GB/s do not depend on the clock at all. The sample is a register-only chain, so it leaves the eviction sweep's work intact, but it runs for about 1.3 ms between the sweep and the pass, during which the hardware prefetcher is idle and any line the sweep left in flight settles; the effective GB/s figures match runs without the sample to within their cv. On an x86 server part the line is 64 bytes, so the 64-byte struct is one line per record and the byte ratio is the same 16, but a single core there draws far less bandwidth than this one, which pushes the struct forms further behind while the scalar `fadd` chain stays at the core's latency, so the scalar rows would show more of the layout effect. Clang on x86 may vectorise the struct sum with a real gather instruction and the vector forms would be AVX2 or AVX-512 with 8 or 16 lanes, so the `.s` needs checking. GCC has no scoped reassociation pragma, so under GCC the `soa_o2` rows compile like `soa_strict` and run at the chain latency, and the vector form of the reduction needs `-ffast-math` or `-fassociative-math` on the whole unit; `build.sh` builds with either compiler and prints which flags it used.

## Reproduce

    ./run.sh            # full run, about 4 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds; writes results/quick-raw.txt and
                        # results/quick-summary.md and leaves README.md alone
    REPS=51 ./run.sh    # more timed passes per variant

`run.sh` builds with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, `bench_scalar.s` and `bench_remarks.txt`, which is where the quoted assembly and remarks come from.
