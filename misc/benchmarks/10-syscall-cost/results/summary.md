Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). One thread at default QoS, which macOS schedules on a P-core. A 67108864-byte file in the page cache, read at offset 33554432. Each row is the min over 31 reps of one pass of the stated number of calls, after one discarded warmup; the median and cv are shown beside it. cycles/call is min ns/call times the clock estimate; ns/byte is min ns/call divided by the bytes per call.

| variant | bytes per call | calls per rep | min ns/call | median ns/call | cv | cycles/call | ns/byte |
|:---|---:|---:|---:|---:|---:|---:|---:|
| pread 1 B | 1 | 65536 | 281.2 | 290.5 | 3.1 % | 1263 | 281 |
| pread 64 B | 64 | 65536 | 281.9 | 290.4 | 2.0 % | 1266 | 4.4 |
| pread 4096 B | 4096 | 65536 | 313.7 | 320.9 | 1.9 % | 1408 | 0.0766 |
| pread 65536 B | 65536 | 4096 | 1257.2 | 1370.9 | 3.4 % | 5645 | 0.0192 |
| pread 1048576 B | 1048576 | 256 | 20490.4 | 21189.3 | 1.5 % | 92002 | 0.0195 |
| memcpy 1 B | 1 | 65536 | 1.1 | 1.2 | 5.0 % | 5 | 1.11 |
| memcpy 64 B | 64 | 65536 | 1.3 | 1.4 | 9.2 % | 6 | 0.0208 |
| memcpy 4096 B | 4096 | 65536 | 37.5 | 42.5 | 5.2 % | 168 | 0.00914 |
| memcpy 65536 B | 65536 | 4096 | 519.9 | 546.0 | 4.2 % | 2334 | 0.00793 |
| memcpy 1048576 B | 1048576 | 256 | 11197.3 | 12204.8 | 2.5 % | 50276 | 0.0107 |
| getppid | 0 | 65536 | 83.1 | 87.2 | 1.6 % | 373 | - |
| clock_gettime (MONOTONIC_RAW) | 0 | 65536 | 12.2 | 12.7 | 2.2 % | 55 | - |

| derived | value | unit |
|:---|---:|:---|
| pread fixed cost, intercept of a line through the 1, 64 and 4096 B mins | 281.3 | ns |
| pread fixed cost in cycles | 1263 | cycles |
| memcpy fixed cost, same fit | 0.9 | ns |
| pread 64 B / pread 1 B | 1.00 | ratio |
| pread 4096 B / pread 1 B | 1.12 | ratio |
| pread 65536 B / pread 1 B | 4.47 | ratio |
| pread 1048576 B / pread 1 B | 72.86 | ratio |
| pread 1 B / getppid | 3.38 | ratio |
| getppid / clock_gettime | 6.81 | ratio |
| pread 1 B / clock_gettime | 23.05 | ratio |
| pread 1 B / memcpy 1 B | 253.83 | ratio |
| pread 4096 B / memcpy 4096 B | 8.38 | ratio |
| pread 1048576 B / memcpy 1048576 B | 1.83 | ratio |
| pread 1 B minus memcpy 1 B, the crossing at 1 B | 280.1 | ns |
| pread 4096 B minus memcpy 4096 B, the crossing at 4 KiB | 276.2 | ns |
| pread copy slope, 65536 to 1048576 B, from the mins | 0.01957 | ns/byte |
| memcpy copy slope, 65536 to 1048576 B, from the mins | 0.01086 | ns/byte |
| pread 1048576 B copy rate, from the median | 49.5 | GB/s |
| memcpy 1048576 B copy rate, from the median | 85.9 | GB/s |
| break-even request, pread fixed cost / pread copy slope | 14374 | bytes |
| pread 1 B ns/byte / pread 1048576 B ns/byte | 14393 | ratio |
| pread 1 B calls per second, from the median | 3.44 | million/s |
| pread 1048576 B calls per second, from the median | 47.2 | thousand/s |
