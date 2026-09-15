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
