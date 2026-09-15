Single-core clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). The cycles column of a row uses the clock measured with as many concurrent copies of clock_estimate as the row has threads, because the all-core clock sits below the single-core one: 1 thread 4.49 GHz, 2 threads 4.40 GHz, 4 threads 3.96 GHz, 8 threads 3.91 GHz. 16777216 increments per thread, 11 timed rounds after one warmup round, wall time from a barrier after every thread has started until the last join. Median of nanoseconds per increment per thread; cycles is the median times the clock for that thread count. Compiled-in stride for pad128: 128 bytes; the OS reports a 128-byte line.

| threads | increment | layout | stride | median ns/increment/thread | min ns/increment/thread | cycles/increment | cv |
|---|---|---|---|---|---|---|---|
| 1 | store | adjacent | 8 B | 0.223 | 0.222 | 1.00 | 0.7 % |
| 1 | store | pad64 | 64 B | 0.223 | 0.222 | 1.00 | 0.7 % |
| 1 | store | pad128 | 128 B | 0.223 | 0.222 | 1.00 | 1.4 % |
| 1 | atomic | adjacent | 8 B | 1.562 | 1.557 | 7.01 | 0.6 % |
| 1 | atomic | pad64 | 64 B | 1.560 | 1.555 | 7.00 | 0.2 % |
| 1 | atomic | pad128 | 128 B | 1.558 | 1.555 | 6.99 | 0.5 % |
| 1 | register | one store at the end | 128 B | 0.223 | 0.222 | 1.00 | 0.5 % |
| 2 | store | adjacent | 8 B | 0.383 | 0.378 | 1.69 | 1.2 % |
| 2 | store | pad64 | 64 B | 0.229 | 0.227 | 1.01 | 0.5 % |
| 2 | store | pad128 | 128 B | 0.228 | 0.227 | 1.00 | 1.4 % |
| 2 | atomic | adjacent | 8 B | 7.125 | 5.595 | 31.35 | 13.7 % |
| 2 | atomic | pad64 | 64 B | 1.594 | 1.589 | 7.01 | 0.4 % |
| 2 | atomic | pad128 | 128 B | 1.597 | 1.589 | 7.03 | 0.4 % |
| 2 | register | one store at the end | 128 B | 0.229 | 0.227 | 1.00 | 1.0 % |
| 4 | store | adjacent | 8 B | 0.684 | 0.682 | 2.71 | 0.2 % |
| 4 | store | pad64 | 64 B | 0.257 | 0.256 | 1.02 | 10.4 % |
| 4 | store | pad128 | 128 B | 0.257 | 0.256 | 1.02 | 0.6 % |
| 4 | atomic | adjacent | 8 B | 26.339 | 25.549 | 104.30 | 2.4 % |
| 4 | atomic | pad64 | 64 B | 1.790 | 1.788 | 7.09 | 2.1 % |
| 4 | atomic | pad128 | 128 B | 1.791 | 1.788 | 7.09 | 0.1 % |
| 4 | register | one store at the end | 128 B | 0.257 | 0.256 | 1.02 | 0.5 % |
| 8 | store | adjacent | 8 B | 0.458 | 0.428 | 1.79 | 11.3 % |
| 8 | store | pad64 | 64 B | 0.485 | 0.257 | 1.90 | 29.2 % |
| 8 | store | pad128 | 128 B | 0.262 | 0.257 | 1.02 | 0.8 % |
| 8 | atomic | adjacent | 8 B | 244.958 | 229.171 | 957.79 | 3.3 % |
| 8 | atomic | pad64 | 64 B | 3.251 | 2.781 | 12.71 | 7.7 % |
| 8 | atomic | pad128 | 128 B | 1.811 | 1.805 | 7.08 | 0.2 % |
| 8 | register | one store at the end | 128 B | 0.258 | 0.257 | 1.01 | 0.2 % |

Layout ratios from the medians. A ratio of 1.00 means the layout made no difference.

| threads | store adjacent / pad128 | store pad64 / pad128 | atomic adjacent / pad128 | atomic pad64 / pad128 |
|---|---|---|---|---|
| 1 | 1.00 | 1.00 | 1.00 | 1.00 |
| 2 | 1.68 | 1.00 | 4.46 | 1.00 |
| 4 | 2.66 | 1.00 | 14.71 | 1.00 |
| 8 | 1.75 | 1.85 | 135.23 | 1.79 |

Per-thread cost relative to the same variant on one thread, from the medians. 1.00 is linear scaling: T threads do T times the work in the same wall time.

| threads | store adjacent | store pad128 | atomic adjacent | atomic pad128 | register |
|---|---|---|---|---|---|
| 1 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| 2 | 1.72 | 1.02 | 4.56 | 1.03 | 1.02 |
| 4 | 3.07 | 1.15 | 16.86 | 1.15 | 1.15 |
| 8 | 2.06 | 1.17 | 156.78 | 1.16 | 1.16 |
