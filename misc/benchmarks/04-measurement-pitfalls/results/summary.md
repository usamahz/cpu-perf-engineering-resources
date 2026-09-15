Run of 2026-09-15T09:37:41Z. Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). Load average (1, 5, 15 min) 7.21 7.55 7.82 before the run and 6.79 7.46 7.78 after it. Array of 1048576 floats, 4194304 bytes, 256 pages of 16384 bytes; one full pass per timed region, times in ns per pass. REPS=30 passes per variant after a discarded warmup, RUNS=30 consecutive in-process passes and RUNS=30 fresh processes for the spread, 100 ms settle in every process between its cold pass and anything else it measures. Clock tick 41 ns (the smallest non-zero step of now_ns). Checksum over the 30 checksum passes: 27525120.0, expected 27525120.0.

Part 1: the same loop, three ways of using its result, passes taken round-robin.

| variant | median ns/pass | min ns/pass | max ns/pass | cv | median / (b) |
|---|---|---|---|---|---|
| (a) result discarded | 0 | 0 | 42 | 181.3 % | 0.000 |
| (b) result passed to SINK() | 497500 | 495667 | 520750 | 1.4 % | 1.000 |
| (c) result stored for the checksum | 498479 | 496291 | 533208 | 1.9 % | 1.002 |

Part 2: variant (b), one warm pass per sample, across passes and across processes. Part 3: the cold first pass, once from the in-process run and once per fresh process.

| series | n | min ns/pass | median ns/pass | max ns/pass | cv | max / min |
|---|---|---|---|---|---|---|
| p2 in-process, consecutive warm passes | 30 | 495625 | 498374 | 514500 | 1.0 % | 1.038 |
| p2 fresh process, one warm pass each | 30 | 495542 | 497500 | 536125 | 1.7 % | 1.082 |
| p3 cold first pass, single shot from the in-process run | 1 | 679708 | 679708 | 679708 | n/a | n/a |
| p3 cold first pass, one per fresh process | 30 | 646334 | 671688 | 729208 | 3.1 % | 1.128 |

Pairs of single warm passes of identical code (435 in-process pairs, 435 fresh-process pairs) whose larger value exceeds the smaller by:

| difference | in-process pairs | fresh-process pairs |
|---|---|---|
| more than 2 percent | 23.9 % | 19.1 % |
| more than 5 percent | 0.0 % | 6.2 % |
| more than 10 percent | 0.0 % | 0.0 % |

| derived | value | unit |
|---|---|---|
| (a) discarded median / (b) SINK median | 0.00 | percent |
| (a) discarded max / (b) SINK median | 0.008 | percent |
| (c) checksum median / (b) SINK median | 1.002 | ratio |
| (b) SINK median per element | 0.474 | ns/elem |
| (b) SINK median per element, times the clock estimate | 2.13 | cycles/elem |
| in-process spread, max / min minus 1 | 3.8 | percent |
| fresh-process spread, max / min minus 1 | 8.2 | percent |
| fresh-process median / in-process median | 0.998 | ratio |
| fresh-process cv / in-process cv | 1.68 | ratio |
| cold single shot / cold fresh-process median | 1.012 | ratio |
| cold single shot / cold fresh-process min | 1.052 | ratio |
