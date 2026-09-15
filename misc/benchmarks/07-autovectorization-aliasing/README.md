# 07. Auto-vectorisation and aliasing

**Claim.** Clang 17 does not give up on `a[i] = b[i] * s + c[i]` when it cannot prove the three pointers distinct: it vectorises the loop behind a runtime alias check, so `restrict` removes only the check and changes nothing the table can see (restrict / plain is 0.99 to 1.00 at every level and size), and the cost is paid when the check fails at run time, where the in-place call `a == c` runs the scalar fallback the compiler kept behind the check, 5.29 times slower in L1 and 2.64 times slower from memory at `-O2` (3.94 in L1, the vector width, with unrolling off), which `#pragma clang loop vectorize(assume_safety)` keeps on the vector loop. Supports: "Auto-Vectorization in LLVM" and "Options to Emit Optimization Reports (Clang)" in README section 7, Compilers and codegen, subsection "Target flags and auto-vectorisation".

**Method.** `bench.c` writes the loop `a[i] = b[i] * s + c[i]` six times over `float` arrays passed as function parameters. The loop body never changes; only what the compiler is told about the pointers does. `axpy_plain` has plain pointers. `axpy_restrict` qualifies all three with `restrict`. `axpy_pragma` keeps plain pointers and puts `#pragma clang loop vectorize(assume_safety)` on the loop. `axpy_stride` indexes `a[i * inc]`, `b[i * inc]`, `c[i * inc]` with `inc` read from a `volatile` in `main`, so the compiler must plan for any stride while the machine always sees 1. `axpy_last` adds `*last = a[i]` through a fourth `float *` on every iteration, a store that type-based alias analysis cannot separate from the arrays. `axpy_scalar` is the plain loop with `#pragma clang loop vectorize(disable)`: the code the loop is left with when the vectoriser gives up, and the baseline for every ratio. The stride and last spellings are there because clang does not refuse the plain loop; it emits a runtime alias check and vectorises anyway, and the two shapes that might be expected to defeat that check (an unknown stride, a store through a pointer argument) are measured to show that they do not defeat it either. Every variant is called over three distinct arrays, and `axpy_plain` and `axpy_pragma` are also called in place with `a == c`, which is the saxpy `y = s*x + y` and is exactly the overlap the runtime check exists to catch: the plain kernel's check fails and its scalar fallback runs, while the pragma's promise (no iteration depends on an earlier one) still holds for `a == c`, so its vector loop runs and is correct. Two sizes: 4194304 (1<<22) floats per array, 16777216 bytes each and 50331648 bytes for the three, three times the 16 MiB P-core L2, so the loop streams from DRAM; and 8192 (1<<13) floats, 32768 bytes each and 98304 bytes for the three, inside the 128 KiB P-core L1d, where the core rather than memory sets the pace. Three builds of the same file: `-O2` (label O2, the primary build, as the brief asks), `-O2 -fno-unroll-loops` (O2nu, a control: one element per scalar iteration and one vector per vector iteration, so the scalar-to-vector ratio is the vector width) and `-O3`. All eight variants at both sizes are timed in each binary with `now_ns()` around one call (32 back-to-back calls at the small size, so a sample is tens of microseconds and the timer's 42 ns resolution does not show), one warmup pass of every variant discarded, then `REPS` (default 31, 11 under `QUICK=1`) timed passes: one sample of every variant per pass, in a fresh seeded random order each pass, so noise drifting over the run lands on every variant alike and no variant owns a fixed slot of the round. Before every sample `a` is overwritten with `c` outside the timed region, so every variant starts from the same cache state and the in-place ones from the same input. Every output is consumed with `SINK()` and checked, outside the timed region, against the 64-bit hash of a reference computed by a plain scalar loop (a four-lane FNV-style hash over the bit patterns of the floats, so two arrays agree only if they are the same bit for bit); the in-place reference applies the update the same number of times as the timed calls; a mismatch fails the run. The hash replaced a `memcmp` against a reference array in an earlier version of this harness: with five 32 KiB arrays passing through the 128 KiB L1d between samples, the variant in one particular slot of each round read 11 percent slower on identical code (it was `restrict` in the committed order, and it moved to whichever kernel took that slot), and reading only `a`, which the kernel has just written, removed it along with most of the spread in L1. The statistic is the median of nanoseconds per element (a throughput-like quantity) with `cv`, and every ratio is paired: each variant's sample over the plain kernel's sample of the same pass (plain in place over pragma in place for the last row), then the median of those with its `cv`. Pairing matters because the core moves between clock states during a run, in stretches of many passes, and the L1 time of every variant moves with it, so a ratio of two medians can land on different states for its two sides while the samples of one pass almost always share a state. `run.sh` runs the three binaries and then the primary one again with `PROBE=1`, which reads the clock with a short dependent add chain (three chains of 32000 adds, min of three, 21 us) before every timed pass and converts that pass to cycles with its own reading; it is a separate pass because the probe is light work and the power controller answers it with a higher clock for the passes themselves, so it is reported beside the default run, not in place of it.

**Generated code.** `build.sh` writes `bench_O2.s`, `bench_O2nu.s`, `bench_O3.s` and `bench_remarks.txt` from the same flags as the binaries (these are regenerated by every build and are not committed). The vectoriser's own account of the six loops, from `-Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize` on the primary build, quoted verbatim from `bench_remarks.txt` (line 77 is the plain loop, 83 restrict, 91 pragma, 98 stride, 105 last, 114 scalar):

```
bench.c:77:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
bench.c:83:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
bench.c:91:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
bench.c:98:5: remark: vectorized loop (vectorization width: 4, interleaved count: 4) [-Rpass=loop-vectorize]
bench.c:105:5: remark: vectorized loop (vectorization width: 4, interleaved count: 2) [-Rpass=loop-vectorize]
bench.c:114:5: remark: the cost-model indicates that vectorization is not beneficial [-Rpass-missed=loop-vectorize]
bench.c:114:5: remark: the cost-model indicates that interleaving is not beneficial [-Rpass-missed=loop-vectorize]
```

`-Rpass-missed=loop-vectorize` prints nothing for the plain loop: there is no missed vectorisation to report, because the loop was vectorised. The `-O3` build gives the same seven lines. The `-O2 -fno-unroll-loops` build gives, for each of the five vectorised loops, `the cost-model indicates that interleaving is beneficial but is explicitly disabled or interleave count is set to 1 [-Rpass-analysis=loop-vectorize]` followed by `vectorized loop (vectorization width: 4, interleaved count: 1) [-Rpass=loop-vectorize]`, and for the scalar loop `loop not vectorized: vectorization and interleaving are explicitly disabled, or the loop has already been vectorized [-Rpass-analysis=loop-vectorize]`. No remark in any build mentions aliasing, because the vectoriser did not need to prove anything: it versioned the loop instead. The assembly below is quoted from `bench_O2nu.s`, where each loop is one vector per iteration and shortest to read; the checks and the structure are the same in `bench_O2.s` and `bench_O3.s`, which differ only in the interleaved main loop shown at the end. The assembler's own comments are dropped, the comments after `;` are mine, and every elision is marked `...`. In the plain kernel the version check is two unsigned subtractions before the vector loop. `a - b` and `a - c` must both be at least 64 bytes, which means `a` may not start inside the 64 bytes at or above the start of `b` or of `c`; if either does, control goes to `LBB0_3`, the scalar loop. `a == c` gives a difference of 0 and takes that branch.

```
_axpy_plain:
	cbz	x3, LBB0_5
	cmp	x3, #7
	b.hi	LBB0_6            ; at least 8 elements: consider the vector loop
	mov	x8, #0
LBB0_3:                           ; scalar fallback, one element per iteration
	...                       ; five instructions of address setup
LBB0_4:
	ldr	s1, [x11], #4
	ldr	s2, [x10], #4
	fmadd	s1, s1, s0, s2
	str	s1, [x8], #4
	subs	x9, x9, #1
	b.ne	LBB0_4
LBB0_5:
	ret
LBB0_6:
	mov	x8, #0
	sub	x9, x0, x1
	cmp	x9, #64
	b.lo	LBB0_3            ; a within 64 bytes above b: scalar
	sub	x9, x0, x2
	cmp	x9, #64
	b.lo	LBB0_3            ; a within 64 bytes above c: scalar
	and	x8, x3, #0xfffffffffffffffc
	dup.4s	v1, v0[0]
	...                       ; four moves
LBB0_9:                           ; NEON, four floats per iteration
	ldr	q2, [x12], #16
	ldr	q3, [x11], #16
	fmla.4s	v3, v1, v2
	str	q3, [x10], #16
	subs	x9, x9, #4
	b.ne	LBB0_9
	cmp	x8, x3
	b.ne	LBB0_3            ; remainder after the last full vector: scalar
	b	LBB0_5
```

The restrict kernel has no such check; the pragma kernel is instruction for instruction the same as the restrict one. Both go from the trip-count test straight to the same NEON loop, and keep a scalar loop only for the remainder after the last full vector.

```
_axpy_restrict:
	cbz	x3, LBB1_8
	cmp	x3, #3
	b.hi	LBB1_3            ; at least 4 elements: vector loop, no alias check
	mov	x8, #0
	b	LBB1_6
LBB1_3:
	and	x8, x3, #0xfffffffffffffffc
	dup.4s	v1, v0[0]
	...                       ; four moves
LBB1_4:
	ldr	q2, [x12], #16
	ldr	q3, [x11], #16
	fmla.4s	v3, v1, v2
	str	q3, [x10], #16
	subs	x9, x9, #4
	b.ne	LBB1_4
	cmp	x8, x3
	b.eq	LBB1_8
LBB1_6:                           ; remainder: the scalar loop
	...
LBB1_8:
	ret
```

The stride kernel guesses. Before the same two alias checks it tests `inc == 1` and runs the same NEON loop when the guess holds; any other stride takes the scalar loop, whose address arithmetic carries the stride in `x10`.

```
_axpy_stride:
	cbz	x3, LBB3_10
	mov	x8, #0
	cmp	x3, #8
	b.lo	LBB3_8
	cmp	x4, #1
	b.ne	LBB3_8            ; stride is not 1: scalar loop
	mov	x8, #0
	sub	x9, x0, x1
	cmp	x9, #64
	b.lo	LBB3_8
	sub	x9, x0, x2
	cmp	x9, #64
	b.lo	LBB3_8
	and	x8, x3, #0xfffffffffffffffc
	dup.4s	v1, v0[0]
	...                       ; four moves
LBB3_6:
	ldr	q2, [x12], #16
	ldr	q3, [x11], #16
	fmla.4s	v3, v1, v2
	str	q3, [x10], #16
	subs	x9, x9, #4
	b.ne	LBB3_6
	cmp	x8, x3
	b.eq	LBB3_10
LBB3_8:                           ; scalar loop, any stride
	sub	x9, x3, x8
	mul	x8, x8, x4
	lsl	x8, x8, #2
	lsl	x10, x4, #2
LBB3_9:
	ldr	s1, [x1, x8]
	ldr	s2, [x2, x8]
	fmadd	s1, s1, s0, s2
	str	s1, [x0, x8]
	add	x8, x8, x10
	subs	x9, x9, #1
	b.ne	LBB3_9
LBB3_10:
	ret
```

The fourth pointer in `axpy_last` turns the two subtractions into five full range comparisons (`cmp`, `ccmp`, `cset` for each pair among `a`, `b`, `c` and `last`), raises the minimum trip count for the vector path to 20, and the per-iteration store is sunk out of the loop: the vector loop is the same four instructions, and lane 3 of the last vector is stored to `*last` once, after it.

```
_axpy_last:
	cbz	x3, LBB4_5
	cmp	x3, #19
	b.hi	LBB4_6            ; at least 20 elements: consider the vector loop
	...                       ; the scalar loop, with str s1, [x4] on every iteration
LBB4_6:
	mov	x8, #0
	lsl	x10, x3, #2
	add	x13, x0, x10
	add	x14, x4, #4
	add	x11, x1, x10
	cmp	x11, x0
	ccmp	x13, x1, #0, hi
	cset	w9, hi            ; a overlaps b
	...                       ; the same for a and c, last and b, last and c
	cmp	x13, x4
	ccmp	x14, x0, #0, hi
	b.hi	LBB4_3            ; a overlaps last: scalar
	tbnz	w9, #0, LBB4_3
	tbnz	w10, #0, LBB4_3
	tbnz	w11, #0, LBB4_3
	tbnz	w12, #0, LBB4_3
	and	x8, x3, #0xfffffffffffffffc
	dup.4s	v1, v0[0]
	...                       ; four moves
LBB4_12:
	ldr	q3, [x12], #16
	ldr	q2, [x11], #16
	fmla.4s	v2, v1, v3
	str	q2, [x10], #16
	subs	x9, x9, #4
	b.ne	LBB4_12
	mov	s1, v2[3]
	str	s1, [x4]          ; *last written once, after the loop
	...
```

The scalar kernel is the fallback loop on its own, and it is the same six instructions in all three builds; neither `-O2` nor `-O3` unrolls it.

```
_axpy_scalar:
	cbz	x3, LBB5_2
LBB5_1:
	ldr	s1, [x1], #4
	ldr	s2, [x2], #4
	fmadd	s1, s1, s0, s2
	str	s1, [x0], #4
	subs	x3, x3, #1
	b.ne	LBB5_1
LBB5_2:
	ret
```

In `bench_O2.s` and `bench_O3.s` the plain, restrict, pragma, stride and scalar kernels are instruction for instruction the same at the two levels, and the vector ones differ from the `-fno-unroll-loops` build only in interleaving: the same two 64-byte checks (or none), then a main loop of sixteen floats per iteration using `ldp`, four `fmla.4s` and `stp`, and a four-float epilogue loop.

```
LBB0_11:                          ; -O2 and -O3: sixteen floats per iteration
	ldp	q2, q3, [x10, #-32]
	ldp	q4, q5, [x10], #64
	ldp	q6, q7, [x11, #-32]
	ldp	q16, q17, [x11], #64
	fmla.4s	v6, v1, v2
	fmla.4s	v7, v1, v3
	fmla.4s	v16, v1, v4
	fmla.4s	v17, v1, v5
	stp	q6, q7, [x9, #-32]
	stp	q16, q17, [x9], #64
	subs	x12, x12, #16
	b.ne	LBB0_11
```

The call site in `run_size` of `bench_O2.s` shows the work cannot be deleted: each kernel is called between two `clock_gettime` calls with `x0 = a`, `x1 = b`, `x2 = c` (or `x2 = x19`, the same register as `a`, for the in-place cases), the last element of `a` is then loaded into the `SINK()` asm, and the four lanes of the hash (their initial constants are built with `movk` just below) are run over the whole array and compared with the reference hash.

```
	bl	_clock_gettime
	...
	fmov	s0, #0.75000000
	mov	x0, x19
	mov	x1, x21
	mov	x2, x22
	mov	x3, x20
	bl	_axpy_restrict
	...
	fmov	s0, #0.75000000
	mov	x0, x19
	mov	x1, x21
	mov	x2, x19           ; in place: c is a
	...
	mov	x3, x20
	bl	_axpy_plain
	...
	bl	_clock_gettime
	...
	ldr	s0, [x19, x8, lsl #2]
	str	s0, [sp, #324]
	add	x8, sp, #324
	; InlineAsm Start
	; InlineAsm End
	...
	mov	x10, #33826                     ; =0x8422
	movk	x10, #40164, lsl #16
	movk	x10, #52210, lsl #32
	movk	x10, #8997, lsl #48
	...
```

`results/raw.txt` ends with `outputs: every variant matched the reference bit for bit` for each binary, and the reference hashes are printed above the timings.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core with 128-bit NEON for which Apple publishes no microarchitecture name. `machine.sh` output is at the top of `results/raw.txt`.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT. Machine condition: `load average at start: 7.02 7.46 7.78`, the `machine.sh` line in `results/raw.txt`, over 14 logical CPUs, so other processes belonging to the user were running; the benchmark is single-threaded and competed for a core only where the table's cv says so: the from-memory rows, where another process's memory traffic would show, sit at 0.7 to 4.7 percent, and the L1 rows at 0.1 to 9.9 percent, which is the clock movement described under Frequency.
- Frequency: `estimated clock: 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)`, the `../common/clock_estimate` line in `results/raw.txt`, measured at the same QoS just before the benchmark; that is a peak, and the core does not hold it under these loops. DVFS is on and cannot be disabled; the benchmark spins for 200 ms before its first sample and runs the large size first so the clock has settled. The scalar loop is six instructions with one taken branch per element and retires one element per cycle, which the `PROBE=1` pass shows directly: with the clock read before every pass it is 1.007 cycles per element in L1 and 1.022 from memory, at whatever clock the probe read (its readings ran from 3.879 to 4.599 GHz). So in the default run the median L1 scalar time gives the clock of the median pass, 4.02 GHz for the primary build (4.03 for the other two), about 11 percent under the estimate, and the scalar loop's 1.119 cycles per element at the estimate is that gap, not a slower loop. The in-L1 ratios do not depend on the clock, because every loop in L1 scales with it; the from-memory ratios do, because the scalar loop is bound by its issue rate and the vector loop by memory: scalar / plain from memory is 2.62 at the clock the run held and would be 2.35 if the scalar loop had held the estimate. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1). Three builds of the same file: `cc -std=c11 -Wall -Wextra -O2` (O2, primary), `cc -std=c11 -Wall -Wextra -O2 -fno-unroll-loops` (O2nu) and `cc -std=c11 -Wall -Wextra -O3` (O3), each with `-DBUILD_LABEL` and `-DBUILD_FLAGS` so its output names its build, and `-lm` on the link line for the `sqrt` in `timing.h`. No `-march`, no `-ffast-math`; the baseline target of this toolchain already has NEON.
- Workload: `a[i] = b[i] * s + c[i]` over `float` arrays, `s = 0.75`, data uniform in [0, 1) from a fixed-seed splitmix64 generator. Two sizes: 4194304 floats per array (16777216 bytes each, 50331648 for the three, three times the 16 MiB L2, streamed from DRAM) and 8192 floats per array (32768 bytes each, 98304 for the three, inside the 128 KiB L1d). Eight variants per size: plain, restrict, pragma, stride, last, scalar, plain in place and pragma in place.
- Baseline: the scalar kernel (the same loop with the vectoriser disabled) in the same binary, and the plain kernel over distinct arrays, paired within each pass.
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one call, or 32 back-to-back calls at the small size; one warmup pass per variant discarded; 31 timed passes, every variant once per pass in a seeded random order; `a` reset from `c` before every sample outside the timed region; median and cv reported, minimum in `results/raw.txt`; ratios paired within a pass, median and cv of those; a fourth pass of the primary binary with a clock probe before every timed pass, giving cycles per element from that pass's own clock; every output hashed and compared with the reference hash and consumed with `SINK()`.

## Results

<!-- results:start -->
Clock estimate 4.50 GHz before the run (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). Median of nanoseconds per element over the timed samples with cv; minima are in `results/raw.txt`. Ratios are paired: each variant over the plain kernel (or plain in place over pragma in place) in the same pass, then the median of those with its cv. Builds: O2 = `-std=c11 -Wall -Wextra -O2`, O2nu = `-std=c11 -Wall -Wextra -O2 -fno-unroll-loops`, O3 = `-std=c11 -Wall -Wextra -O3`, O2probe = `-std=c11 -Wall -Wextra -O2 PROBE=1`. O2probe is the O2 binary run with PROBE=1: a clock probe (three dependent add chains of 32000 adds, min of three) before every timed pass, which reads the clock the passes run at and converts each pass to cycles with its own reading (the cycles/elem column is the median of those conversions). The probe changes that clock, so its column stands beside the default run rather than in place of it.

4194304 floats per array: 16777216 bytes per array, 50331648 bytes for the three arrays, 1 call per timed sample.

| variant | O2 ns/elem | cv | O2nu ns/elem | cv | O3 ns/elem | cv | O2probe ns/elem | cv | cycles/elem |
|---|---|---|---|---|---|---|---|---|---|
| plain | 0.0947 | 3.7 % | 0.0962 | 2.3 % | 0.0958 | 2.3 % | 0.0962 | 3.0 % | 0.394 |
| restrict | 0.0956 | 3.6 % | 0.0965 | 2.1 % | 0.0961 | 3.9 % | 0.0960 | 1.3 % | 0.394 |
| pragma | 0.0957 | 4.1 % | 0.0963 | 2.0 % | 0.0958 | 3.0 % | 0.0965 | 2.7 % | 0.393 |
| stride | 0.0948 | 2.5 % | 0.0962 | 2.1 % | 0.0958 | 3.3 % | 0.0960 | 2.7 % | 0.397 |
| last | 0.0958 | 3.0 % | 0.0961 | 1.4 % | 0.0958 | 2.2 % | 0.0969 | 1.4 % | 0.397 |
| scalar | 0.2487 | 3.0 % | 0.2482 | 1.8 % | 0.2487 | 2.4 % | 0.2502 | 1.1 % | 1.022 |
| plain in place | 0.2504 | 1.3 % | 0.2505 | 2.0 % | 0.2512 | 1.1 % | 0.2523 | 0.7 % | 1.038 |
| pragma in place | 0.0798 | 1.7 % | 0.0804 | 1.3 % | 0.0798 | 2.1 % | 0.0801 | 1.6 % | 0.328 |

| ratio, 4194304 floats | O2 | cv | O2nu | cv | O3 | cv | O2probe | cv |
|---|---|---|---|---|---|---|---|---|
| scalar / plain | 2.62 | 2.5 % | 2.57 | 2.9 % | 2.61 | 3.7 % | 2.60 | 2.7 % |
| plain in place / plain | 2.64 | 3.4 % | 2.59 | 2.9 % | 2.63 | 2.3 % | 2.62 | 2.8 % |
| plain in place / pragma in place | 3.13 | 1.7 % | 3.10 | 2.0 % | 3.14 | 2.1 % | 3.14 | 1.6 % |
| restrict / plain | 1.00 | 3.3 % | 1.00 | 3.1 % | 0.99 | 4.7 % | 1.00 | 3.1 % |
| pragma / plain | 1.00 | 3.8 % | 1.00 | 2.9 % | 1.00 | 3.9 % | 1.01 | 3.2 % |
| stride / plain | 1.00 | 3.4 % | 0.99 | 2.0 % | 1.00 | 3.7 % | 1.00 | 2.9 % |
| last / plain | 1.01 | 2.7 % | 1.00 | 2.3 % | 1.01 | 2.7 % | 1.01 | 3.2 % |

8192 floats per array: 32768 bytes per array, 98304 bytes for the three arrays, 32 calls per timed sample.

| variant | O2 ns/elem | cv | O2nu ns/elem | cv | O3 ns/elem | cv | O2probe ns/elem | cv | cycles/elem |
|---|---|---|---|---|---|---|---|---|---|
| plain | 0.0471 | 5.2 % | 0.0631 | 3.5 % | 0.0470 | 8.6 % | 0.0420 | 0.6 % | 0.191 |
| restrict | 0.0469 | 7.3 % | 0.0629 | 4.1 % | 0.0470 | 6.4 % | 0.0421 | 0.5 % | 0.191 |
| pragma | 0.0471 | 6.0 % | 0.0633 | 4.3 % | 0.0470 | 4.9 % | 0.0421 | 3.4 % | 0.191 |
| stride | 0.0472 | 6.7 % | 0.0629 | 6.4 % | 0.0470 | 4.7 % | 0.0421 | 5.0 % | 0.191 |
| last | 0.0471 | 6.7 % | 0.0634 | 3.1 % | 0.0470 | 4.8 % | 0.0421 | 2.0 % | 0.191 |
| scalar | 0.2487 | 5.5 % | 0.2484 | 2.9 % | 0.2483 | 2.8 % | 0.2227 | 0.1 % | 1.007 |
| plain in place | 0.2487 | 4.8 % | 0.2486 | 4.0 % | 0.2487 | 3.0 % | 0.2228 | 1.2 % | 1.008 |
| pragma in place | 0.0474 | 5.1 % | 0.0636 | 6.7 % | 0.0472 | 9.9 % | 0.0423 | 2.8 % | 0.191 |

| ratio, 8192 floats | O2 | cv | O2nu | cv | O3 | cv | O2probe | cv |
|---|---|---|---|---|---|---|---|---|
| scalar / plain | 5.29 | 7.0 % | 3.93 | 5.0 % | 5.27 | 6.9 % | 5.30 | 0.6 % |
| plain in place / plain | 5.29 | 5.5 % | 3.94 | 4.4 % | 5.28 | 7.3 % | 5.31 | 1.4 % |
| plain in place / pragma in place | 5.27 | 6.6 % | 3.90 | 6.7 % | 5.28 | 8.4 % | 5.28 | 2.7 % |
| restrict / plain | 1.00 | 8.5 % | 1.00 | 4.7 % | 1.00 | 7.2 % | 1.00 | 0.8 % |
| pragma / plain | 1.00 | 7.4 % | 1.00 | 5.6 % | 1.00 | 6.8 % | 1.00 | 3.4 % |
| stride / plain | 1.00 | 8.3 % | 1.00 | 7.5 % | 1.00 | 7.9 % | 1.00 | 5.1 % |
| last / plain | 1.00 | 7.6 % | 1.00 | 4.1 % | 1.00 | 5.9 % | 1.00 | 2.2 % |

| derived | value | unit |
|---|---|---|
| clock estimate before the run (dependent 1-cycle add chain, 400000000 adds, min of 7 runs) | 4.50 | GHz |
| O2 clock of the median scalar pass in L1, 1 / (n8192 scalar median) | 4.02 | GHz |
| O2nu clock of the median scalar pass in L1, 1 / (n8192 scalar median) | 4.03 | GHz |
| O3 clock of the median scalar pass in L1, 1 / (n8192 scalar median) | 4.03 | GHz |
| n4194304 O2probe clock before each timed pass, median (min, max) | 4.107 (3.879, 4.599) | GHz |
| n8192 O2probe clock before each timed pass, median (min, max) | 4.544 (4.063, 4.599) | GHz |
| n4194304 O2 scalar cycles per element at the clock estimate | 1.119 | cycles |
| n4194304 O2 plain, 12 bytes per element | 126.7 | GB/s |
| n4194304 O2 scalar, 12 bytes per element | 48.3 | GB/s |
| n8192 O2 scalar cycles per element at the clock estimate | 1.119 | cycles |
| n8192 O2 plain cycles per element, scalar loop as the cycle | 0.189 | cycles |
| n8192 O2 plain, 12 bytes per element | 254.8 | GB/s |
| n8192 O2 scalar, 12 bytes per element | 48.3 | GB/s |
| n4194304 O2 scalar / plain if the scalar loop had held the clock estimate | 2.35 | ratio |
<!-- results:end -->

## Analysis

The premise of the old form of this claim does not hold on this compiler, and the mechanism behind it does: Clang 17 vectorises every spelling but the scalar one at width 4, the remarks say so, and the restrict, pragma, stride and last variants run at 0.99 to 1.01 of the plain time at every level and size, because the plain kernel already runs the same NEON loop behind a check that costs two subtractions and two compares per call, and neither shape suggested for defeating that check does so, since an unknown stride is met with a guess (`inc == 1`, then the same check) and a store through a fourth `float *` with a longer check and the store sunk out of the loop. What the check protects is measured directly: called in place, with `a == c`, the plain kernel takes 0.2487 ns per element in L1 against 0.0471 out of place, 5.29 times paired, and the scalar kernel takes 0.2487, 5.29 times the plain kernel, so the in-place call is running the scalar fallback the compiler kept inside the same function, at the scalar loop's speed, with nothing changed but the arguments. That ratio exceeds the vector width because the scalar loop retires one element per cycle (1.007 cycles per element in the probe column) while the `-O2` vector loop, sixteen floats per iteration, takes 0.189 cycles per element with the scalar loop as the cycle (0.191 in the probe column), about three cycles per iteration and about 64 bytes loaded and stored per cycle, 254.8 GB/s; with unrolling off the vector loop is one iteration per cycle like the scalar loop (its 0.0631 ns per element is four floats in the 0.2484 ns the scalar loop takes for one) and the ratio is the width, 3.94 in place and 3.93 for the scalar kernel. From memory the same in-place call costs 2.64 times the out-of-place one and the scalar kernel 2.62 times, with 2.57 and 2.61 for the scalar kernel at the other two levels, because the vector loop is now bound by memory at 126.7 GB/s while the scalar loop, at 48.3 GB/s and 1.022 cycles per element in the probe column, is still bound by its issue rate; this ratio moves with the clock, 2.62 at the 4.02 GHz the run held and 2.35 had the core held the 4.50 GHz estimate, where the in-L1 ratios do not. The pragma is the qualifier that fits the in-place call, since its promise (no iteration depends on an earlier one) is true for `a == c` where the restrict promise is not; pragma in place runs 5.27 times faster than plain in place in L1 and 3.13 times from memory, where it also touches two arrays instead of three and so beats even the out-of-place plain kernel (0.0798 against 0.0947). The optimisation level changes none of this: `-O3` gives 5.28 in L1 and 2.63 from memory for the in-place call, the scalar loop is the same six instructions in all three builds (0.2487, 0.2484 and 0.2483 ns per element in L1), and the only column that differs, the 0.0631 of the `-fno-unroll-loops` build against 0.0471, is the interleaving, not the level. The L1 medians depend on which clock state the core was in for the median pass: the same `-O2` plain kernel reads 0.0471 ns per element in the default run and 0.0420 in the probe pass, whose probe read 4.544 GHz where the default run's scalar median implies 4.02, and both come to 0.19 cycles per element (0.189 and 0.191). In this run the five vectorised medians of each L1 column landed on the same side of that step (0.0629 to 0.0634 in the `-fno-unroll-loops` column), so the medians and the paired ratios agree; when the core changes state partway through a binary's run they need not, and the paired ratios still read 1.00, which is what the pairing is for. The honest form of the claim for this toolchain is therefore: the vectoriser emits NEON behind a runtime alias check, the scalar loop it keeps for the aliasing case is 5.29 times slower in cache and 2.64 times slower from memory, `restrict` removes the check and changes nothing else, and it is the arguments at the call, not the qualifier, that decide which loop runs.

## Limits

There is no PMU access from user space on macOS, so the cost of the check itself cannot be counted; it is below the noise here (restrict against plain, 0.99 to 1.00) because it runs once per call over 8192 or 4194304 elements, and a loop called on short arrays would see it. The 64-byte distance and the minimum trip counts of 8 and 20 are this compiler's choices and will move between versions. This loop is the easy case for the vectoriser: three unit-stride pointers of one type whose ranges it can bound, so a check is always possible. The cases where clang 17 does refuse on aliasing grounds are the ones a check cannot cover: an address loaded inside the loop from memory the loop's stores may reach, which needs the stored type to alias the pointer's (`char` data, a pointer or a length reloaded from a struct, or `-fno-strict-aliasing`), or a non-affine index such as `a[idx[i]]`, which `restrict` does not fix. Only `a == c` was measured for the overlapping case; `a == b` is the other overlap that leaves the result unchanged and fails the check in the same way, an `a` that starts below `b` or `c` passes the check because every write trails the reads of that element, and an `a` that starts a few elements above either turns the loop into a recurrence with a different answer. The GB/s column counts what the loop issues, 12 bytes per element; from DRAM the out-of-place store also allocates the line for `a`, so the memory system moves more than that. DVFS is on and the clock is not one number: the estimate before the run is a peak (4.50 GHz), the timed passes of the default run sat at 4.02 GHz, 11 percent under it, and the probe pass, whose light work between passes lifts the clock, read 4.544 GHz before its L1 passes; the core also moves between these states within a run, so a nanosecond median in L1 can sit on either side of that step (the cv column shows it, and the probe column's 0.0420 against the default run's 0.0471 for the same plain kernel is the two sides of it), which the paired ratios and the probe column's cycles do not. Other processes belonging to the user were running during the run (load average 7.02 at its start, stated under Machine); the from-memory rows, where their memory traffic would show, have cv of 0.7 to 4.7 percent, and every cv in the table is under 10 percent. The scalar loop is taken as one element per cycle on the strength of the probe column (1.007 and 1.022 cycles per element), which is also the assumption behind `clock_estimate`. On an x86 server part the same versioning appears from both compilers (GCC reports it with `-fopt-info-vec-all` and `#pragma GCC ivdep` plays the pragma's part), the vector loop is 8 floats wide with AVX2 and 16 with AVX-512 so the in-cache ratio grows with the width, the scalar loop is not one iteration per cycle on every core, and a single core's share of DRAM bandwidth is smaller, so the memory-bound ratio shrinks.

## Reproduce

    ./run.sh            # full run, about 5 seconds including the three builds and the probe pass
    QUICK=1 ./run.sh    # smoke test, about 3 seconds; prints the summary and leaves
                        # results/ and README.md alone
    REPS=51 ./run.sh    # more timed passes per variant
    PROBE=1 ./bench_O2  # the probe pass on its own, after ./build.sh

`run.sh` builds the three binaries with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the output of each binary and of the probe pass (a non-zero exit from any of them stops the run before the summary is built), builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, `bench_O2nu.s`, `bench_O3.s` and `bench_remarks.txt`, which are where the quoted assembly and remarks come from; they are build products, not committed, so build first and then read them.
