Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). STREAM triad a[i] = b[i] + s*c[i] over three float64 arrays. Large case: 536870912 bytes (512 MiB) per array, split into one contiguous partition per thread and streamed once per round. Cache case: the first 1048576 bytes (1 MiB) of each array per thread, 3 MiB per thread in all, repeated for 512 passes per round, so a thread does the same work per round in both cases. 24 bytes counted per element, decimal GB/s. Median over 21 timed rounds per configuration, after discarded warmup rounds covering at least 100 ms and numbering at least 3; the best round is what STREAM would report. Fastest and slowest thread are the medians of each round's quickest and slowest partition; scaling is against one thread over the same working set. A round has a late thread when its span exceeds the slowest thread's own time by more than 20 percent, which means some thread was not running while the others were. The single-thread cache configuration runs first and again last. Vendor figure: 273 GB/s. Load at the start of this run: load average 11.55 8.77 8.24; swap total = 13312.00M  used = 12138.06M  free = 1173.94M  (encrypted); free 446 MB  compressor 11642 MB.

| per array | threads | median GB/s | best GB/s | cv | fraction of 273 GB/s | scaling vs 1 thread | fastest thread GB/s | slowest thread GB/s | rounds with a late thread |
|---|---|---|---|---|---|---|---|---|---|
| 1 MiB per thread (L2) | 1 | 123.2 | 129.8 | 1.3 % | 0.451 | 1.00 | 123.2 | 123.2 | 0 |
| 1 MiB per thread (L2) | 2 | 238.1 | 247.1 | 1.1 % | 0.872 | 1.93 | 119.1 | 119.0 | 0 |
| 1 MiB per thread (L2) | 4 | 458.5 | 466.9 | 1.1 % | 1.680 | 3.72 | 138.7 | 114.6 | 0 |
| 1 MiB per thread (L2) | 8 | 691.9 | 715.0 | 1.7 % | 2.534 | 5.62 | 106.4 | 86.5 | 0 |
| 1 MiB per thread (L2) | 10 | 776.9 | 844.4 | 5.0 % | 2.846 | 6.31 | 92.7 | 77.7 | 0 |
| 512 MiB | 1 | 119.2 | 124.1 | 7.9 % | 0.437 | 1.00 | 119.2 | 119.2 | 0 |
| 512 MiB | 2 | 214.2 | 216.6 | 2.2 % | 0.785 | 1.80 | 107.8 | 107.1 | 0 |
| 512 MiB | 4 | 223.7 | 227.8 | 2.0 % | 0.819 | 1.88 | 56.5 | 55.9 | 0 |
| 512 MiB | 8 | 225.8 | 229.0 | 1.2 % | 0.827 | 1.89 | 28.4 | 28.2 | 0 |
| 512 MiB | 10 | 225.5 | 229.3 | 0.8 % | 0.826 | 1.89 | 22.6 | 22.6 | 0 |
| 512 MiB | 14 | 149.4 | 182.5 | 10.5 % | 0.547 | 1.25 | 20.6 | 10.7 | 0 |
| 1 MiB per thread (L2), run again last | 1 | 120.4 | 124.3 | 0.9 % | 0.441 | - | 120.4 | 120.4 | 0 |

| derived | value | unit |
|---|---|---|
| highest 1 MiB per thread median (10 threads) / vendor figure | 2.85 | ratio |
| highest 1 MiB per thread median (10 threads) / highest 512 MiB median (8 threads) | 3.44 | ratio |
| 1 MiB per thread, 1 thread, run again last / run first | 0.98 | ratio |
| vendor figure / 512 MiB single thread | 2.29 | ratio |
| 512 MiB 10 threads / 1 thread | 1.89 | ratio |
| 512 MiB 14 threads / 10 threads | 0.66 | ratio |
| 512 MiB 14 threads, fastest thread / slowest thread | 1.93 | ratio |
| highest 512 MiB median (8 threads) / vendor figure | 0.827 | fraction |
| highest 512 MiB median (8 threads) / single thread | 1.89 | ratio |
| highest 512 MiB median (8 threads) x 32/24, the interface traffic if the store read a before writing it | 301.1 | GB/s |
