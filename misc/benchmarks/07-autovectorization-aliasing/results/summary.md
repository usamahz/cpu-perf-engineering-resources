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
