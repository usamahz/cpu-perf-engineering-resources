# 05. Roofline

**Claim.** Which roof binds a loop is predictable from its arithmetic intensity, on one core and on all ten P-cores: a streaming loop with under one flop per byte is bound by the memory roof, a loop with ten flops per byte and the roof's own instruction mix reaches the compute roof (0.96 of the FMA peak on one core, 0.92 on ten), and a Horner polynomial with sixteen flops per byte is bound by the compute roof, no faster from L1 than from DRAM, at 0.73 to 0.75 of the peak. Supports: "Roofline: An Insightful Visual Performance Model for Multicore Architectures" in README section 5, Models, under "Roofline and the execution-cache-memory model".

**Method.** `bench.c` measures the roofs, then five kernels of known intensity, all on one thread and again on one thread per P-core (10 threads, pthreads, no pinning since macOS has none, every thread at user-interactive QoS so the scheduler prefers P-cores). The compute roof is the NEON `fmla` rate of 20 independent float32x4 accumulator chains fed by one L1 load per 20 FMAs from a 4096-byte buffer, 8388608 iterations per pass, a few milliseconds like every other pass so that a thread descheduled by other work costs the roofs and the kernels alike; the same loop with the textbook 16 chains is measured beside it, because on this core 16 chains fall short of the peak and the roof has to be the larger figure. There are three memory roofs, one per stream mix, because on one core the write path is not the read path and two read streams are not one: a float32 sum with 16 independent add chains over a 536870912-byte array, thirty-two times the 16777216-byte L2 of a P-core cluster and sixteen times the two clusters together, is the roof for a loop that only reads; a copy of 268435456 bytes into another 268435456 (8 bytes per element) is the roof for a loop that reads one array and writes one; a triad `z = x + a*y` over three 268435456-byte arrays (12 bytes per element) is the roof for a loop that reads two and writes one. A write-only fill of 268435456 bytes is measured beside them, because it decides whether a write-allocate read has to be counted. Every roof is measured twice at each thread count, before the kernels and again after them, and the higher median is the roof, since a roof is a ceiling; the table shows that measurement's median, min, max and cv with the other median beside it. Before the first roof the FMA loop spins untimed for 300 ms so the clock has ramped, and every case discards untimed passes until at least 100 ms have run (at least one pass; `WARMUP_MS` changes it), so the first case of the run is warmed like the rest. The kernels run over 268435456 bytes of float32 each for `x` and `y`, the two halves of the bandwidth array: `saxpy` (`y = a*x + y`, 2 flops per element over 12 bytes: read `x`, read `y`, write `y`, intensity 0.167 flop/B), a 7-point 1-D `stencil` (`out[i] = sum of tap[k] * in[i+k-3]`, one multiply and six FMAs, 13 flops over 8 bytes: 4 read and 4 written, the six other loads per element being L1 hits, intensity 1.625 flop/B), `fma_stream`, which is the roof's own function with its address mask opened so its one load per 20 FMAs walks `x` instead of cycling the 4096-byte buffer (40 flops over the 4 bytes read, intensity 10.0 flop/B; it is called once per 65536-byte block and the block sums are added in double), `poly`, a degree-32 polynomial by Horner on every element with the results summed (32 FMAs, 64 flops, over the 4 bytes read, intensity 16.0 flop/B; the reduction adds are not counted), and `poly_intrin`, the same polynomial with the Horner step written as `vfmaq_f32` instead of two instructions of inline asm, because clang emits different code for the two (see Generated code). No write-allocate read is counted in the bytes: the fill row shows this part does not fetch a line it is about to overwrite whole, since counting one would put the fill above the read and write streams together. Each kernel also runs over an L1-resident slice per thread, 16384 bytes of `x` and of `y` (65536 bytes of `x` alone for `fma_stream`, one block, since a shorter block drains its twenty chains too often), repeated 1024 times per pass (256 for `fma_stream`), a sixteenth of the one-thread DRAM pass and about two thirds of a ten-thread one, so a pass lasts long enough to time; that rate is what the core alone allows the instruction mix. The roofline prediction for each kernel is min(FMA peak, intensity x memory roof) at the same thread count, with the memory roof whose stream mix matches the loop (the triad for `saxpy`, the copy for the stencil, the read roof for the three that only read), and the table shows measured over predicted, measured from DRAM over measured from L1, and the DRAM traffic the measured rate implies. Every kernel is NEON intrinsics so the flop count is exact. Each measurement is `now_ns()` around one pass, then `REPS` (default 21) timed passes after the warm-up; the statistic is the median (throughput-like) with `cv`, minimum and maximum shown for the roofs. A spin barrier starts and ends every pass on all threads; the master thread is worker 0. Every result is consumed with `SINK()` and compared with a double-precision reference computed from the value formulas and the coefficients rather than from the arrays: the data are multiples of 1/8, so the sum, copy, triad, fill, saxpy, stencil and `fma_stream` checksums match exactly (the FMA loop's accumulators start at multiples of 1/32 and its scale is 1/4, so over one block every partial sum is exact in float32) and the polynomials to 1e-4; a mismatch fails the run.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. The FMA loop is `and`, `lsl`, one `ldr q` and twenty `fmla` per iteration, one per chain, on distinct registers, and it is one function: the roof calls it with the mask 1023 over the L1 buffer and `fma_stream` calls it with the mask open (`x1 = -1`) and 4096 iterations per 65536-byte block of `x`; the sum is `ldp q` pairs feeding sixteen `fadd` chains; the copy is two `ldp` and two `stp` per sixteen elements with the asm barrier that stops clang calling `memcpy`; the triad and saxpy are `ldp` pairs, `fmla`, `stp`; the fill is two `stp q` per sixteen elements; the stencil is seven loads, one `fmul` and six `fmla` by element, one store; the asm-step Horner loop is `ld1r` for the pair of coefficients, then per chain the asm's `mov` and `fmla` twice, 24 `mov.16b` and 24 `fmla` per iteration with no spill or reload; the intrinsic-step loop is 22 `mov.16b`, 24 `fmla` and eleven reloads of the twelve `x` vectors from the stack per iteration, because with `vfmaq_f32` clang keeps the copies of the coefficient and spills `x` instead.

```
_k_fma:
LBB2_2:
	and	x9, x8, x1                     ; x1 = 1023 for the roof, -1 for fma_stream
	lsl	x9, x9, #2
	ldr	q29, [x0, x9]
	fmla.4s	v18, v22, v29
	fmla.4s	v0, v22, v29
	...                                ; twenty chains on distinct registers
	fmla.4s	v28, v22, v29
	subs	x2, x2, #1
	b.ne	LBB2_2

_run_slice (fma_stream, inlined):
LBB4_30:
	mov	x0, x21                        ; x + block
	mov	x1, #-1                        ; mask open
	mov	w2, #4096                      ; iterations per 65536-byte block
	bl	_k_fma
	fadd	d9, d9, d0
	add	x21, x21, #16, lsl #12         ; next block
	...
	b.ls	LBB4_30

_k_sum:
LBB7_2:
	ldp	q24, q25, [x9, #-128]
	fadd.4s	v23, v23, v24
	fadd.4s	v22, v22, v25
	...                                ; eight ldp, sixteen fadd chains
	b.ls	LBB7_2

_k_copy:
LBB8_2:
	ldp	q0, q1, [x10, #-32]
	stp	q0, q1, [x9, #-32]
	ldp	q0, q1, [x10], #64
	stp	q0, q1, [x9], #64
	; InlineAsm Start
	; InlineAsm End
	...
	b.ls	LBB8_2

_k_triad:
LBB9_2:
	ldp	q1, q2, [x11, #-32]            ; x
	ldp	q3, q4, [x10, #-32]            ; y
	fmla.4s	v1, v0, v3
	fmla.4s	v2, v0, v4
	stp	q1, q2, [x9, #-32]             ; z
	...
	b.ls	LBB9_2

_k_fill:
LBB10_2:
	stp	q0, q0, [x9, #-32]
	stp	q0, q0, [x9], #64
	...
	b.ls	LBB10_2

_k_saxpy:
LBB11_2:
	ldp	q1, q2, [x10, #-32]            ; y
	ldp	q3, q4, [x9, #-32]             ; x
	fmla.4s	v1, v0, v3
	fmla.4s	v2, v0, v4
	stp	q1, q2, [x10, #-32]            ; y
	...
	b.ls	LBB11_2

_k_stencil:
LBB12_2:
	ldur	q6, [x10, #-12]
	fmul.4s	v6, v6, v0[0]
	ldur	q7, [x10, #-8]
	fmla.4s	v6, v7, v1[0]
	...                                ; five more load and fmla pairs
	str	q6, [x9], #16
	...
	b.ls	LBB12_2

_k_poly:
LBB13_3:
	ld1r.4s	{ v10 }, [x14]                 ; coefficient j
	sub	x16, x14, #4
	ld1r.4s	{ v11 }, [x16]                 ; coefficient j - 1
	mov.16b	v12, v10                       ; chain 0, step j: copy c, then p = c + p * x
	fmla.4s	v12, v9, v2
	mov.16b	v9, v11                        ; chain 0, step j - 1, back into v9
	fmla.4s	v9, v12, v2
	...                                ; twelve chains, 24 mov and 24 fmla per iteration
	b.hi	LBB13_3

_k_poly_intrin:
LBB14_3:
	ld1r.4s	{ v10 }, [x14]
	sub	x16, x14, #4
	mov.16b	v11, v10
	ldp	q0, q1, [sp, #192]              ; 32-byte Folded Reload: x vectors
	fmla.4s	v11, v1, v9
	ld1r.4s	{ v9 }, [x16]
	mov.16b	v12, v10
	fmla.4s	v12, v0, v8
	...                                ; 22 mov, 24 fmla, six ldp and five ldr reloads per iteration
	b.hi	LBB14_3
```

The copy per step is what AArch64 imposes on Horner: `fmla` adds into its destination, and the coefficient is shared by every chain, so `c + p * x` needs a fresh copy of `c` before the `fmla`. The asm step makes that one `mov` and one `fmla` and leaves the register allocation to clang, which keeps all twelve `x` and twelve `p` vectors in registers; given the intrinsic instead, clang 17 at `-O2` keeps the same number of copies but spills the `x` vectors and reloads them every step, and the `poly_intrin` rows measure what that costs.

The call site in `measure` shows the work cannot be deleted: `run_slice` is called between the two `clock_gettime` calls with the barrier on either side, the per-thread results are summed, and the sum goes through the `SINK()` asm before the checksum comparison.

```
	bl	_clock_gettime
	...
	ldaddal	w22, w10, [x23]                ; barrier
	...
	bl	_run_slice
	...
	ldaddal	w22, w10, [x23]                ; barrier
	...
	bl	_clock_gettime
	...
LBB1_64:
	ldr	d0, [x11], #128                ; g_w[t].out
	fadd	d9, d9, d0
	subs	x10, x10, #1
	b.ne	LBB1_64
LBB1_65:
	str	d9, [sp, #152]
	add	x10, sp, #152
	; InlineAsm Start
	; InlineAsm End
```

The read-only kernels are pure functions of global memory and loop-invariant bounds, so `run_slice` puts a compiler barrier (`CLOBBER()`) in its pass loop; without it clang is entitled to hoist one call out of the loop, and did so in an experiment where the array pointer never escaped. `results/raw.txt` prints every checksum next to its reference with the relative error and ends with `checksums: every kernel matched its reference`; the ten-thread FMA checksums are exactly ten times the one-thread ones, which is the check that every thread did the whole work.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro; an Armv9-class out-of-order core with four 128-bit NEON pipes, for which Apple publishes no microarchitecture name, no FMA latency and no memory bandwidth. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread, then ten threads, one per performance core, all at user-interactive QoS (`pthread_set_qos_class_self_np`), nothing pinned since macOS has no affinity API. 10 P-cores in two clusters of 5 and 4 E-cores on the part, no SMT. Machine condition: `load average at start: 6.79 7.46 7.78` in `results/raw.txt`, so other processes of the user's were running; the one-thread cases had a core to themselves and the ten-thread cases competed for the P-cores only where the table's cv says so.
- Frequency: `estimated clock: 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)`, the line `../common/clock_estimate` writes to `results/raw.txt`, taken just before the benchmark on one thread at default QoS; DVFS is on and cannot be disabled, and the all-core clock under ten threads is not measured, so the ten-thread FMA-per-cycle row is only indicative. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c -lpthread`.
- Workload: the roofs, a 20-chain `fmla` loop over a 4096-byte L1-resident buffer, a 16-chain float32 sum over 536870912 bytes, a copy of 268435456 bytes and a triad over three arrays of 268435456 bytes; a write-only fill over 268435456 bytes; and five NEON kernels over 268435456 bytes each of `x` and `y`, `saxpy` at 0.167 flop/B, a 7-point stencil at 1.625 flop/B, the roof's own loop fed from `x` at 10.0 flop/B and a degree-32 Horner polynomial at 16.0 flop/B in two codings, each also over an L1-resident slice per thread (16384 bytes of `x` and of `y`, 65536 bytes of `x` for `fma_stream`), on one thread and on ten.
- Baseline: the roofline prediction min(FMA peak, intensity x memory roof) at the same thread count, with the memory roof whose stream mix matches the loop, and each kernel's own rate from L1.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one pass bracketed by a spin barrier; a 300 ms untimed spin of the FMA loop before the first roof; per case, untimed passes discarded until at least 100 ms have run, then 21 timed passes; median and cv reported, minimum and maximum shown for the roofs, each roof measured before and after the kernels with the higher median kept; every result consumed with `SINK()` and checked against a double-precision reference.

## Results

<!-- results:start -->
Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs), one thread at default QoS. Kernel arrays 268435456 bytes each, bandwidth array 536870912 bytes, L1 slices 16384 bytes of x and of y per thread (65536 bytes of x for fma_stream). Rates are medians over 21 timed passes after at least 100 ms of discarded warm-up passes. Each roof is measured before and after the kernels at that thread count and the higher median is the roof; the row shows that measurement and the other median is in the last column. The roofline prediction is min(FMA peak, intensity x memory roof) at the same thread count, where the memory roof is the one whose stream mix matches the loop: the read bandwidth for a loop that only reads, the copy bandwidth for one that reads one array and writes one, the triad bandwidth for one that reads two and writes one. FMA peak is 20 accumulator chains; the 16-chain row is the same loop with the textbook count.

| roof | threads | median | min | max | cv | other measurement |
|---|---|---|---|---|---|---|
| FMA peak (20 chains) | 1 | 143.7 GFLOP/s | 139.4 GFLOP/s | 144.4 GFLOP/s | 0.8 % | 142.1 GFLOP/s (after the kernels) |
| FMA 16 chains | 1 | 128.8 GFLOP/s | 127.7 GFLOP/s | 129.4 GFLOP/s | 0.3 % | 128.3 GFLOP/s (after the kernels) |
| read bandwidth (sum) | 1 | 92.7 GB/s | 90.5 GB/s | 94.4 GB/s | 1.1 % | 91.0 GB/s (before the kernels) |
| copy bandwidth (copy) | 1 | 123.3 GB/s | 119.3 GB/s | 125.5 GB/s | 1.4 % | 123.1 GB/s (after the kernels) |
| triad bandwidth (triad) | 1 | 125.1 GB/s | 122.4 GB/s | 130.9 GB/s | 1.4 % | 125.0 GB/s (after the kernels) |
| write bandwidth (fill) | 1 | 141.0 GB/s | 131.8 GB/s | 141.8 GB/s | 2.2 % | 139.7 GB/s (after the kernels) |
| FMA peak (20 chains) | 10 | 1231.1 GFLOP/s | 1156.2 GFLOP/s | 1232.1 GFLOP/s | 1.3 % | 1229.5 GFLOP/s (after the kernels) |
| FMA 16 chains | 10 | 1102.2 GFLOP/s | 1087.8 GFLOP/s | 1104.2 GFLOP/s | 0.3 % | 1101.3 GFLOP/s (after the kernels) |
| read bandwidth (sum) | 10 | 246.3 GB/s | 239.1 GB/s | 248.0 GB/s | 1.1 % | 246.1 GB/s (before the kernels) |
| copy bandwidth (copy) | 10 | 238.4 GB/s | 233.1 GB/s | 239.1 GB/s | 0.9 % | 235.1 GB/s (after the kernels) |
| triad bandwidth (triad) | 10 | 227.6 GB/s | 222.2 GB/s | 229.4 GB/s | 0.9 % | 224.3 GB/s (after the kernels) |
| write bandwidth (fill) | 10 | 253.3 GB/s | 231.3 GB/s | 256.5 GB/s | 2.4 % | 199.4 GB/s (after the kernels) |

| kernel | threads | intensity flop/B | memory roof | from DRAM GFLOP/s | cv | from L1 GFLOP/s | cv | prediction GFLOP/s | binding roof | DRAM / prediction | DRAM / L1 | DRAM traffic GB/s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| saxpy | 1 | 0.167 | triad | 24.3 | 1.2 % | 43.4 | 2.7 % | 20.9 | memory | 1.17 | 0.56 | 145.8 |
| stencil | 1 | 1.625 | copy | 80.5 | 1.5 % | 90.4 | 1.8 % | 143.7 | compute | 0.56 | 0.89 | 49.5 |
| fma_stream | 1 | 10.000 | read | 138.4 | 2.3 % | 140.7 | 2.3 % | 143.7 | compute | 0.96 | 0.98 | 13.8 |
| poly | 1 | 16.000 | read | 104.7 | 0.8 % | 103.9 | 0.6 % | 143.7 | compute | 0.73 | 1.01 | 6.5 |
| poly_intrin | 1 | 16.000 | read | 79.6 | 0.8 % | 80.5 | 2.1 % | 143.7 | compute | 0.55 | 0.99 | 5.0 |
| saxpy | 10 | 0.167 | triad | 39.1 | 1.0 % | 368.3 | 1.2 % | 37.9 | memory | 1.03 | 0.11 | 234.8 |
| stencil | 10 | 1.625 | copy | 381.8 | 1.0 % | 762.5 | 0.7 % | 387.5 | memory | 0.99 | 0.50 | 235.0 |
| fma_stream | 10 | 10.000 | read | 1134.5 | 1.3 % | 1210.6 | 0.1 % | 1231.1 | compute | 0.92 | 0.94 | 113.5 |
| poly | 10 | 16.000 | read | 918.5 | 0.6 % | 920.2 | 18.6 % | 1231.1 | compute | 0.75 | 1.00 | 57.4 |
| poly_intrin | 10 | 16.000 | read | 700.7 | 7.1 % | 710.5 | 0.6 % | 1231.1 | compute | 0.57 | 0.99 | 43.8 |

| derived | value | unit |
|---|---|---|
| ridge point, FMA peak / read bandwidth, 1 thread | 1.55 | flop/B |
| ridge point, FMA peak / copy bandwidth, 1 thread | 1.17 | flop/B |
| ridge point, FMA peak / triad bandwidth, 1 thread | 1.15 | flop/B |
| ridge point, FMA peak / read bandwidth, 10 threads | 5.00 | flop/B |
| ridge point, FMA peak / copy bandwidth, 10 threads | 5.16 | flop/B |
| ridge point, FMA peak / triad bandwidth, 10 threads | 5.41 | flop/B |
| FMA peak, 10 threads / 1 thread | 8.57 | ratio |
| read bandwidth, 10 threads / 1 thread | 2.66 | ratio |
| copy bandwidth, 10 threads / 1 thread | 1.93 | ratio |
| triad bandwidth, 10 threads / 1 thread | 1.82 | ratio |
| write bandwidth, 10 threads / 1 thread | 1.80 | ratio |
| FMA instructions per cycle per core at the clock estimate, 1 thread | 4.00 | fmla/cycle |
| FMA 16 chains / FMA peak, 1 thread | 0.90 | ratio |
| copy bandwidth / read bandwidth, 1 thread | 1.33 | ratio |
| triad bandwidth / read bandwidth, 1 thread | 1.35 | ratio |
| write bandwidth / read bandwidth, 1 thread | 1.52 | ratio |
| fma_stream from DRAM / FMA peak, 1 thread | 0.96 | ratio |
| fma_stream from L1 / FMA peak, 1 thread | 0.98 | ratio |
| poly from DRAM / FMA peak, 1 thread | 0.73 | ratio |
| poly_intrin from DRAM / poly from DRAM, 1 thread | 0.76 | ratio |
| saxpy DRAM traffic / triad bandwidth, 1 thread | 1.17 | ratio |
| saxpy read traffic, 8 of its 12 bytes, / read bandwidth, 1 thread | 1.05 | ratio |
| stencil DRAM traffic / copy bandwidth, 1 thread | 0.40 | ratio |
| stencil from L1 / FMA peak, 1 thread | 0.63 | ratio |
| FMA instructions per cycle per core at the clock estimate, 10 threads | 3.43 | fmla/cycle |
| FMA 16 chains / FMA peak, 10 threads | 0.90 | ratio |
| copy bandwidth / read bandwidth, 10 threads | 0.97 | ratio |
| triad bandwidth / read bandwidth, 10 threads | 0.92 | ratio |
| write bandwidth / read bandwidth, 10 threads | 1.03 | ratio |
| fma_stream from DRAM / FMA peak, 10 threads | 0.92 | ratio |
| fma_stream from L1 / FMA peak, 10 threads | 0.98 | ratio |
| poly from DRAM / FMA peak, 10 threads | 0.75 | ratio |
| poly_intrin from DRAM / poly from DRAM, 10 threads | 0.76 | ratio |
| saxpy DRAM traffic / triad bandwidth, 10 threads | 1.03 | ratio |
| saxpy read traffic, 8 of its 12 bytes, / read bandwidth, 10 threads | 0.64 | ratio |
| stencil DRAM traffic / copy bandwidth, 10 threads | 0.99 | ratio |
| stencil from L1 / FMA peak, 10 threads | 0.62 | ratio |
<!-- results:end -->

## Analysis

On ten threads the kernels land where their intensity puts them: saxpy at 0.167 flop/B runs at 39.1 GFLOP/s against 37.9 predicted from the triad roof (1.03) and moves 234.8 GB/s, while from L1 the same loop runs at 368.3 GFLOP/s, so from DRAM it delivers 0.11 of what the cores allow and nothing but memory binds it; the stencil at 1.625 flop/B is under the ten-thread ridge point of 5.16 flop/B against the copy roof and does the same, 381.8 GFLOP/s against 387.5 predicted (0.99), 235.0 GB/s of traffic, 0.50 of its 762.5 from L1. `fma_stream`, the roof's own loop with its load walking `x` at 10.0 flop/B, is to the right of the ridge and reaches the compute roof: 138.4 GFLOP/s from DRAM on one thread, 0.96 of the 143.7 GFLOP/s FMA peak and 0.98 of its own 140.7 from L1, moving 13.8 GB/s; on ten threads it reaches 1134.5, 0.92 of the 1231.1 peak, where the same loop from L1 reaches 0.98, so at 113.5 GB/s, under half the read roof, the DRAM stream still costs it six percent that a roofline, which has no latency term, does not see. The polynomial at 16.0 flop/B is bound by the compute roof but does not reach it: 104.7 GFLOP/s from DRAM and 103.9 from L1 on one thread (1.01), 918.5 and 920.2 on ten (1.00), 0.73 and 0.75 of the peak, and what sits under the roof is its instruction mix, the register copy each Horner step needs before an `fmla` that adds into its destination, and twelve chains in flight where the roof needs twenty (sixteen reach 0.90 of the peak in the roof table); written with the intrinsic instead of the two-instruction asm step it runs at 0.76 of the asm form on one thread and on ten (79.6 and 700.7 GFLOP/s), the price of the spills and reloads in the Generated code section. On one thread the ridge point moves to 1.55 flop/B against the read roof (1.17 against the copy roof), because one core gets 92.7 GB/s of the 246.3 GB/s that ten get (2.66 times) while its FMA peak scales 8.57 times, so the stencil crosses to the compute side: the model names the compute roof, and the loop delivers 80.5 GFLOP/s, 0.56 of the peak and 0.89 of the 90.4 it manages from L1, which is itself only 0.63 of the peak, a ceiling set by its seven loads per four elements rather than by either roof. One-thread saxpy is the lesson about the memory roof: the model names the right roof, since its 24.3 GFLOP/s is 0.56 of its L1 rate and far below the compute roof, but the loop sits at 1.17 of the prediction from the triad roof, because the one-core memory roof is not one number: the read stream stops at 92.7 GB/s, the write stream at 141.0, the copy at 123.3 and the triad at 125.1, and saxpy's 145.8 GB/s is a read stream at 1.05 of the read roof with an in-place write riding beside it, which the out-of-place triad, whose stores go to lines the core has not just loaded, does not match; on ten threads, where the memory controller rather than the core is the limit and the roofs converge (the copy at 0.97 and the triad at 0.92 of the read roof), the same loop lands at 1.03. The fill row settles the byte count for the storing loops: on ten threads the fill moves 253.3 GB/s, 1.03 times the read roof, and a write-allocate read would double that to a figure no row in the table approaches, so a stored line costs 4 bytes of traffic here, not 8. The 4.00 `fmla` per cycle per core at the 4.49 GHz clock estimate on one thread is the four NEON pipes, all busy; the ten-thread figure of 3.43 at the same clock estimate means the all-core clock is lower under DVFS, not that pipes idle, which is also why the ten-thread FMA peak is 8.57 times the one-thread peak rather than ten.

## Limits

There is no PMU access from user space on macOS, so the bytes moved are counted from the loops rather than read from a memory controller counter, and the absence of a write-allocate read is inferred from the fill row rather than observed. Apple publishes neither the FMA latency nor the memory bandwidth of this part, so the roofs are what the code reaches, which is what the CS Roofline Toolkit also does. DVFS is on, the one-thread clock estimate is taken before the run and the all-core clock is not measured, so the FMA-per-cycle rows are indicative. This run was taken with other processes of the user's running (the load average in the Machine section), which is why the cv column is shown. Inside the run the two measurements of every roof agree within 2 percent (the roof table's last column) except the ten-thread fill, whose second measurement (199.4 GB/s against 253.3; `results/raw.txt` shows all 21 of its passes near the lower figure, so the whole case ran slow rather than one pass) is a thread that did not have a P-core for the whole case; the 18.6 percent cv of the ten-thread polynomial from L1 and the 7.1 percent cv of `poly_intrin` from DRAM are single passes cut the same way, which the median discards. The earlier full run with this code (the previous `results/raw.txt` in the repository history) put the one-thread roofs a few percent lower and the ten-thread FMA roofs where they are now, and since the two measurements inside a run agree, that between-run movement of the one-thread roof is the clock under DVFS and the other load, not measurement noise. The one-core memory roof is not one number here: the read stream, the write stream, the copy, the triad and saxpy each stop at a different rate, which is consistent with reads being limited by the core's outstanding misses while writes are posted, so on one thread a kernel that writes in place can sit above a prediction made from any roof whose stores go elsewhere, and the three memory roofs are there to make that visible rather than to hide it. `fma_stream` on ten threads is 0.92 of the peak from DRAM against 0.98 from L1 with a longer block making no difference, which is memory latency under ten concurrent streams that the prefetcher does not fully hide, and which a bandwidth roof cannot express. There is no NUMA and no SMT to add ceilings. On an x86 server part the picture changes in constants and in one mechanism: a single core's DRAM bandwidth is a much smaller fraction of the socket's, so the one-thread ridge point is far to the left and the stencil would be memory bound on one core as well; AVX2 or AVX-512 raises the FMA roof per core; `vfmadd213ps` can put the polynomial's running value in the destination, so Horner needs no register copy there and the polynomial can sit closer to its FMA roof; and a write-allocate read is normally paid unless non-temporal stores are used, which raises the stencil's bytes per element from 8 to 12 and saxpy's from 12 to 16.

## Reproduce

    ./run.sh            # full run, about 15 seconds
    QUICK=1 ./run.sh    # smoke test, about 8 seconds; writes results/quick-raw.txt and
                        # results/quick-summary.md and leaves README.md alone
    REPS=51 ./run.sh    # more timed passes per case
    THREADS=4 ./run.sh  # a different all-cores thread count
    WARMUP_MS=500 ./run.sh  # longer warm-up per case

`run.sh` builds with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from.
