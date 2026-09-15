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
