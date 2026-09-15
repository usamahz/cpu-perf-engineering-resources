# 12. SGEMM: naive loop, microkernel, vendor BLAS

**Claim.** From the same thread on one P-core, the vendor BLAS is tens of times faster than a naive triple loop at the same precision (hundreds of times here), and a packed, register-blocked NEON microkernel closes that gap only as far as the ceiling of the vector unit, about 44 times the naive loop and 87 percent of the measured NEON FMA peak; on this part the remaining 14 times is a matrix unit that Accelerate uses and that the compiler does not emit from plain C, so the usual statement that a microkernel is nearly the whole gap to the vendor BLAS holds where the BLAS runs on the vector unit, as on x86, and not here. Supports: "Anatomy of High-Performance Matrix Multiplication" and "BLIS: A Framework for Rapidly Instantiating BLAS Functionality" in README section 12, Inference on CPU, subsection "GEMM and BLAS".

**Second claim.** The dot product table carries a separate claim: int8 `sdot` does four times the multiply-adds of float32 `fmla` per instruction and per byte, so int8 wins by about four whether memory or the core binds. Supports: "Quantization and Training of Neural Networks for Efficient Integer-Arithmetic-Only Inference" in README section 12, Inference on CPU, subsection "Quantization".

**Method.** `bench.c` computes one SGEMM, C = A B with M = N = K = 1024 in float32 (4194304 bytes per matrix, 2147483648 flops per call), from fixed-seed uniform data in [-1, 1), four ways on one thread. `sgemm_naive` is the i-j-k triple loop, whose inner loop is a reduction into one float that the compiler may not reorder without `-ffast-math`, so it stays a dependent `fmadd` chain reading B down a column. `sgemm_ikj` swaps the two inner loops so the innermost is a saxpy over contiguous rows of B and C, which clang vectorises at `-O2`. `sgemm_blocked` is the Goto loop order: for each 256-deep K step it packs the 256 x 1024 block of B into 8-column panels, then for each 64-row block of A packs it into 8-row panels and runs `ukernel_8x8`, an 8x8 register-tile microkernel that keeps sixteen 128-bit accumulators for the whole k loop and does four loads and sixteen `fmla` per k. The fourth is the vendor BLAS, Accelerate `cblas_sgemm`, called twice: in one process with `VECLIB_MAXIMUM_THREADS=1`, and in a second process with the variable unset so the library picks its own thread count; `getrusage` CPU time over wall time around the calls is printed for both, which is how the thread count is verified. Two ceilings are measured alongside: the latency of one dependent scalar `fmadd` (a chain of eight in inline asm, minimum over runs), which bounds the naive loop at two flops per latency, and the NEON FMA peak (sixteen independent register chains, no memory), which bounds anything written in NEON; a third, 32 flops per cycle at the clock estimate (four 128-bit FMA pipes, four lanes, two flops), is computed rather than measured, and the microkernel is quoted against both. Every variant's C is compared element by element with the naive C and the run fails if the maximum absolute difference reaches 1e-2. The second table is a dot product over 16777216 elements, int8 with `sdot` (sixteen multiply-adds into four int32 lanes per instruction, inputs in [-16, 16) so no lane can overflow) against float32 with `fmla` (four per instruction), eight accumulators each, at the streaming size (33554432 and 134217728 bytes per pair, beyond the 16 MiB L2) and at an L1-resident size (8192 elements, 16384 and 65536 bytes, 2048 passes per sample); the int8 result is checked against an exact int64 sum and the float32 result against a double sum, and the per-cycle column divides the `sdot` or `fmla` rate by the clock estimate, counting only those instructions and not the loads beside them. Timing is `now_ns()` around one call (one pass for the dot products); one warmup of every variant is discarded, then the timed samples (`REPS`, default 11, for SGEMM and the two ceilings; `DOT_REPS`, default 31, for the dot products) are taken round-robin so drift lands on every variant alike. The statistic is the median of GFLOP/s or ops per nanosecond (throughput-like) with `cv`; the maximum is shown; the `fmadd` latency is the minimum (latency-like). Ratios are derived from the medians.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. Clang versions the naive loop and emits a contiguous path guarded by `n == 1` (`cmp w2, #3; ccmp w1, #1, #0, hi; cset w11, eq`), so for N = 1024 the path that runs is the scalar chain: one `fmadd` on `s0` per k, with B read at a stride of 4 N bytes (`x14`).

```
_sgemm_naive:
LBB1_20:
	ldr	s1, [x3, x24, lsl #2]     ; A[i][k]
	ldr	s2, [x22, x25]            ; B[k][j], x25 += 4N each k
	fmadd	s0, s1, s2, s0            ; the dependent chain
	add	x24, x24, #1
	add	x25, x25, x14
	cmp	x9, x24
	b.ne	LBB1_20
```

The i-k-j inner loop is vectorised and unrolled: per sixteen elements, four `ldp` (a row of B and a row of C), four `fmla` with the broadcast A element in `v1`, and two `stp` back to C, so every FMA carries two loads and one store.

```
_sgemm_ikj:
LBB4_12:
	ldp	q2, q3, [x6, #-32]
	ldp	q4, q5, [x6], #64
	ldp	q6, q7, [x7, #-32]
	ldp	q16, q17, [x7]
	fmla.4s	v6, v2, v1
	fmla.4s	v7, v3, v1
	fmla.4s	v16, v4, v1
	fmla.4s	v17, v5, v1
	stp	q6, q7, [x7, #-32]
	stp	q16, q17, [x7], #64
	subs	x24, x24, #16
	b.ne	LBB4_12
```

The microkernel's k loop is two `ldp` (eight floats of packed B in `v24`, `v25`; eight of packed A in `v26`, `v27`) and sixteen by-element `fmla` into sixteen accumulators that never touch memory inside the loop: sixteen FMAs per four loads, against one per three memory operations in the loop above.

```
_sgemm_blocked:
LBB5_51:
	ldp	q24, q25, [x0], #32
	ldp	q26, q27, [x20], #32
	fmla.4s	v1, v24, v26[0]
	fmla.4s	v0, v25, v26[0]
	fmla.4s	v3, v24, v26[1]
	fmla.4s	v2, v25, v26[1]
	...
	fmla.4s	v17, v24, v27[3]
	fmla.4s	v16, v25, v27[3]
	subs	w30, w30, #1
	b.ne	LBB5_51
```

The two dot product loops have the same shape as each other: eight `ldp` (sixteen 128-bit loads) and eight `sdot` or eight `fmla` per iteration, so any difference between them is the instruction, not the loop.

```
_dot_int8_sdot:
LBB7_2:
	ldp	q16, q17, [x9]
	ldp	q18, q19, [x10]
	sdot.4s	v7, v16, v18
	sdot.4s	v6, v17, v19
	...
	add	x8, x8, #128
	cmp	x8, x2
	b.lo	LBB7_2

_dot_f32_fmla:
LBB8_2:
	ldp	q16, q17, [x9, #-64]
	ldp	q18, q19, [x10, #-64]
	fmla.4s	v7, v18, v16
	fmla.4s	v6, v19, v17
	...
	add	x8, x8, #32
	cmp	x8, x2
	b.lo	LBB8_2
```

The work cannot be deleted: each SGEMM variant writes C through a pointer that `main` then reads back in `max_abs_diff` against the reference, with `SINK(c)` between the timer read and the check; each dot product result goes through `SINK()` and into a running sum that is compared with the reference; and the two register-only ceilings carry an empty volatile asm (`neon_fma_peak`) or are themselves inline asm (`fmadd_latency_ns`) so that clang cannot mark them memory-free and move the call across the `clock_gettime` calls, which it did in an earlier draft. The pass loop over the L1-resident dot product passes both pointers through `OPAQUE()` each pass, because clang otherwise proves the arrays never escape and hoists the pure call out of the loop. `results/raw.txt` ends each table with `every variant matched` and prints the checksums (the sum of C is -8579.542461; the int8 reference is 4265427).

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro; the single-thread rows ran on one performance core, an Armv9-class out-of-order core with four 128-bit FMA pipes, for which Apple publishes no microarchitecture name. The SME/AMX matrix unit that Accelerate runs `cblas_sgemm` on is a separate execution unit whose placement Apple does not document: the default-threads row, 2.00 times one thread at 1.97 threads' worth of CPU on a part with two P-core clusters, is consistent with one unit per cluster, but the library's thread choice cannot be observed, so this README does not assert it. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. The "default threads" BLAS row is the only multi-threaded one: Accelerate chose its own threads, and its CPU time over wall time of 1.97 says about two cores were busy. 10 P-cores and 4 E-cores on the part, no SMT. `machine.sh` recorded `load average at start: 11.47 8.53 8.14` in `results/raw.txt`, so other processes belonging to the user were running; the benchmark is single-threaded unless stated, so it competed for cores only where the table's cv says so.
- Frequency: `estimated clock: 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)`, the line `../common/clock_estimate` wrote to `results/raw.txt` at the same QoS just before the benchmark; every per-cycle figure in the tables (the `fmadd` latency in cycles, the 32 flops per cycle bound, the `sdot` or `fmla` per cycle column) divides by this estimate. DVFS is on and cannot be disabled, so the clock during the vector runs is not known directly: the measured `fmadd` latency of 0.690 ns is 3.1 cycles at the estimate, and the NEON FMA peak of 129.12 GFLOP/s is 89.7 percent of the 144.00 GFLOP/s that 32 flops per cycle would give at it, which reads as 4.03 GHz if the peak loop issues four `fmla` every cycle; the derived rows under the dot table repeat the per-cycle figures at that implied clock. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c -framework Accelerate`. No `-ffast-math`, no `-march`; the sdot and NEON intrinsics are available in the default arm64 macOS target.
- Workload: SGEMM with M = N = K = 1024 in float32, 4194304 bytes per matrix, so A and B together are half the 16777216-byte L2 and 64 times the 131072-byte P-core L1d; the microkernel's blocks are 65536 bytes of packed A and 8192 bytes per packed B panel (both L1-sized) and 1048576 bytes of packed B per K step (L2-sized). Dot products over 16777216 elements: 33554432 bytes of int8 and 134217728 bytes of float32 per pair, streamed from memory; and 8192 elements, 16384 and 65536 bytes, repeated 2048 times in L1.
- Baseline: the naive i-j-k loop for the SGEMM table (every ratio is to it), and the float32 `fmla` dot product for the int8 table. The NEON FMA peak, the 32 flops per cycle bound and the `fmadd` latency are the ceilings the ratios are read against.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one call; one warmup per variant discarded; 11 timed calls per SGEMM variant and 31 per dot product, round-robin; median and cv reported, maximum shown, minimum for the latency; every SGEMM result checked against the naive C (maximum absolute difference below 1e-2) and every dot product against an exact or double-precision reference; every result consumed with `SINK()`. The numbers below are whatever the last `./run.sh` wrote; rerun it to regenerate them on this machine.

## Results

<!-- results:start -->
Clock estimate 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). SGEMM C = A B with M = N = K = 1024 in float32, 4194304 bytes per matrix, 2147483648 flops per call, one thread unless stated; median GFLOP/s over 11 timed calls per variant after a discarded warmup, sampled round-robin. Every variant is checked against the naive result (max abs error below 1e-2); the naive checksum, the sum of every entry of C, is -8579.542461.

| variant | median GFLOP/s | max GFLOP/s | median ms per call | cv | max abs error vs naive | ratio to naive |
|---|---|---|---|---|---|---|
| naive i-j-k triple loop | 2.54 | 2.56 | 843.70 | 0.3 % | 0.0e+00 | 1.0 |
| i-k-j, inner loop auto-vectorised | 32.94 | 33.18 | 65.20 | 0.5 % | 0.0e+00 | 12.9 |
| blocked, packed, 8x8 NEON register-tile microkernel | 111.77 | 112.60 | 19.21 | 0.5 % | 0.0e+00 | 43.9 |
| Accelerate cblas_sgemm, 1 thread | 1588.13 | 1605.20 | 1.35 | 1.4 % | 0.0e+00 | 624.0 |
| Accelerate cblas_sgemm, default threads | 3180.08 | 3228.09 | 0.68 | 0.7 % | 0.0e+00 | 1249.5 |

| ceiling | value | unit |
|---|---|---|
| scalar fmadd latency, dependent chain, min of 11 runs | 0.690 | ns |
| naive chain bound, two flops per fmadd latency | 2.90 | GFLOP/s |
| NEON FMA peak, sixteen register chains, no memory (cv 1.3 %) | 129.12 | GFLOP/s |
| NEON FMA bound, 32 flops per cycle at the clock estimate | 144.00 | GFLOP/s |

| derived | value | unit |
|---|---|---|
| i-k-j / naive | 12.9 | ratio |
| blocked / i-k-j | 3.39 | ratio |
| blocked / naive | 43.9 | ratio |
| fmadd latency in cycles at the clock estimate | 3.1 | cycles |
| naive as a fraction of the fmadd chain bound | 87.8 | percent |
| blocked as a fraction of the NEON FMA peak | 86.6 | percent |
| blocked as a fraction of the 32 flops per cycle bound | 77.6 | percent |
| i-k-j as a fraction of the NEON FMA peak | 25.5 | percent |
| NEON FMA peak as a fraction of the 32 flops per cycle bound | 89.7 | percent |
| clock implied by the NEON FMA peak at 32 flops per cycle | 4.03 | GHz |
| BLAS 1 thread / naive | 624 | ratio |
| BLAS 1 thread / blocked | 14.2 | ratio |
| BLAS 1 thread / NEON FMA peak | 12.3 | ratio |
| BLAS 1 thread, CPU time over wall time | 1.00 | ratio |
| BLAS default threads / BLAS 1 thread | 2.00 | ratio |
| BLAS default threads, CPU time over wall time | 1.97 | ratio |

Dot product, one thread: int8 with the NEON sdot instruction (sixteen multiply-adds per instruction, four int32 lanes) against float32 with fmla (four per instruction), eight accumulators each; a multiply and an add count as two ops. Streaming: 16777216 elements, 33554432 bytes for the int8 pair and 134217728 bytes for the float32 pair, beyond the 16 MiB L2. L1-resident: 8192 elements, 16384 and 65536 bytes, 2048 passes per sample so each sample does the same multiply-adds as one streaming pass. Median over 31 samples after a discarded warmup, sampled round-robin. The per-cycle column divides the sdot or fmla rate by the clock estimate and counts only those instructions, not the loads beside them.

| kernel | data | median ops/ns | cv | bytes/ns | sdot or fmla per cycle at the clock estimate | ratio to f32 fmla |
|---|---|---|---|---|---|---|
| int8 sdot | streaming | 118.6 | 1.6 % | 118.6 | 0.82 | 3.97 |
| f32 fmla | streaming | 29.9 | 1.0 % | 119.5 | 0.83 | 1.00 |
| int8 sdot | L1-resident | 191.0 | 2.6 % | 191.0 | 1.33 | 4.02 |
| f32 fmla | L1-resident | 47.5 | 1.0 % | 190.1 | 1.32 | 1.00 |

| derived | value | unit |
|---|---|---|
| L1-resident sdot per cycle at the clock implied by the NEON FMA peak | 1.48 | per cycle |
| L1-resident fmla per cycle at the clock implied by the NEON FMA peak | 1.47 | per cycle |
| L1-resident bytes per cycle at the clock implied by the NEON FMA peak, sdot and fmla | 47.3 and 47.1 | bytes |
| bound from sixteen 128-bit loads per eight instructions at three loads per cycle | 1.50 | per cycle |
<!-- results:end -->

## Analysis

The naive loop runs at 2.54 GFLOP/s, 87.8 percent of the 2.90 GFLOP/s that one dependent `fmadd` chain at 0.690 ns per link (3.1 cycles at the clock estimate) allows, so it is bound by the latency of its own accumulator and the strided reads of B cost only the rest. Reordering the loops so the compiler can vectorise gives 32.94 GFLOP/s, 12.9 times the naive loop but 25.5 percent of the 129.12 GFLOP/s NEON FMA peak, because each `fmla` still carries two loads and a store of C, so the loop is bound by the load and store ports. The 8x8 microkernel with packing does sixteen `fmla` per four loads and holds the tile in registers for a whole K step, and reaches 111.77 GFLOP/s: 3.39 times the auto-vectorised loop, 43.9 times the naive one, 86.6 percent of the measured NEON FMA peak and 77.6 percent of the 144.00 GFLOP/s that 32 flops per cycle at the clock estimate would give, so it stops within about 13 percent of the ceiling of the vector unit, and the rest is packing, loop overhead and the absence of prefetching that a production kernel trims. The vendor BLAS on one thread does 1588.13 GFLOP/s, 624 times the naive loop, at a CPU time over wall time of 1.00, so it is one thread; that is 14.2 times the microkernel and 12.3 times the NEON peak itself, which no NEON code can reach, because Accelerate runs SGEMM on the SME matrix unit rather than the FMA pipes, a separate execution unit that the compiler does not emit from plain C (Apple clang 17 ships SME intrinsics, but the auto-vectoriser does not use them, so a C microkernel stays on the NEON pipes). With its default thread count the library reaches 3180.08 GFLOP/s, 2.00 times the single thread at 1.97 threads' worth of CPU, so two threads got two threads' worth on a part with two P-core clusters, which is consistent with one matrix unit per cluster; Apple documents neither the unit's placement nor how the library picks its thread count, so the table shows the two-fold gain and not its cause. The claim therefore holds in its first half and splits in its second: the BLAS is tens of times faster (hundreds here), and the microkernel closes the gap as far as the vector unit reaches, but the 14.2 times above that is a different execution unit that only the library reaches on this part. In the dot product table the int8 `sdot` loop does 118.6 ops per nanosecond against 29.9 for float32 `fmla` when streaming, 3.97 times, and both move 118.6 and 119.5 bytes per nanosecond at 0.82 and 0.83 `sdot` or `fmla` per cycle, so both are at the same memory bandwidth and int8 wins by carrying a multiply-add per byte instead of per four. In L1 the two loops run at 1.33 and 1.32 `sdot` or `fmla` per cycle at the clock estimate (1.48 and 1.47 at the 4.03 GHz the NEON peak implies, against the 1.50 bound from sixteen 128-bit loads per eight instructions at three loads per cycle), so both are bound by their loads, and `sdot` does 191.0 ops per nanosecond against 47.5, 4.02 times, because each instruction holds sixteen multiply-adds instead of four: the same factor of four from bytes when memory binds and from lanes when the core binds, which is what int8 quantization buys on a CPU before any lookup-table or matrix-extension trick.

## Limits

There is no PMU access from user space on macOS, so the per-cycle figures are derived from the clock estimate rather than counted, the clock during the vector runs is inferred from the NEON peak rather than read, and the SME unit's own clock, placement and occupancy cannot be observed; the BLAS row is a black box that says only what Accelerate achieves, not why. Apple documents neither the SME throughput nor how `cblas_sgemm` uses it, so the 12.3-times-NEON-peak ratio is a measurement with no specification to check it against, and the two-thread scaling of the default-threads row is reported without a cause. Every variant matched the naive C to the bit (maximum absolute error 0), because all of them accumulate each element in ascending k with fused multiply-adds, so the 1e-2 tolerance was never exercised; a BLAS that split K differently would show a small nonzero error and still pass. Nothing between i-k-j and the full microkernel separates register tiling from packing from blocking, so the table shows what the three together buy and not what each one does; a register tile without packing would be the next variant to add. The microkernel handles only M and N that are multiples of 8 and does no edge tiles, prefetching or K-blocking of C, which is part of why it stops at 86.6 percent of the peak; a production kernel such as BLIS or KleidiAI would take a few more percent. The L1-resident dot product rate depends on where the two arrays sit relative to each other: read from the heads of the two large streaming allocations instead of from adjacent copies in one block, the float32 loop ran at about half the rate on this core at some separations and not others, so `bench.c` places the L1 copies side by side, and the streaming rows are the ones the second claim rests on. The auto-vectorised row is a property of this compiler: a different clang or GCC may unroll differently or fail to vectorise, and the 12.9 would move with it. The BLAS default-threads row was taken with the library's threads unpinned and with other user processes running (the load average at start was 11.47), and its cv of 0.7 percent is the only sign that its two threads kept their cores; the ratios were taken round-robin under the same conditions. On an x86 server part the picture changes in one place: SGEMM in float32 has no matrix unit to go to (AMX handles bf16 and int8, not fp32), so MKL or OpenBLAS run it on the AVX-512 FMA ports, and the vendor BLAS lands within a few tens of percent of a well-written microkernel rather than 14.2 times above it; the naive-to-microkernel ratio is then nearly the whole gap, which is the form of the claim the section's entries describe. The int8 comparison maps to VNNI (`vpdpbusd`, also four times the multiply-adds of an fp32 FMA per instruction) on x86, and to eight times or more with AMX or SME2 int8 tile products, neither of which this table measures.

## Reproduce

    ./run.sh            # full run, about 15 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds; 256^3 and 1<<20 elements; writes
                        # results/quick-raw.txt and results/quick-summary.md and leaves README.md alone
    REPS=31 ./run.sh    # more timed calls per SGEMM variant (DOT_REPS=n for the dot products)

`run.sh` builds with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the benchmark output from two processes (`VECLIB_MAXIMUM_THREADS=1` for every variant, then `MODE=blas` with the variable unset for the BLAS at its default thread count), builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers; the benchmark's exit status is checked before the summary step, so a variant that disagrees with the naive result keeps `raw.txt` for inspection and leaves `summary.md` and this README alone. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from. On Linux, `USE_OPENBLAS=1 ./build.sh` links OpenBLAS as the vendor BLAS and `-lm` is linked either way; without OpenBLAS the BLAS rows are skipped with a message, and a build without NEON skips the microkernel and the dot product table.
