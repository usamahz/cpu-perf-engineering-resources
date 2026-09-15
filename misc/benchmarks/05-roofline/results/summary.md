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
