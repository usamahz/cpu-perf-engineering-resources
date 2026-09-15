Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). Page 16384 bytes; buffer 1073741824 bytes = 65536 pages; one thread on a P-core at default QoS. Per-page rows are the min over 15 timed reps after one discarded warmup (latency-like); bandwidth rows are the median (throughput-like); cv is over the reps. Faults are from getrusage, the median over reps. Page queues just before the run (vm_stat, in pages): free 23031  active 293209  inactive 293353  speculative 5237  wired 200560  compressor 712782; that is 360 MiB free against a 1024 MiB buffer, with the inactive and speculative queues reclaimable on demand.

| step | min | median | cv | faults per rep | unit |
|---|---:|---:|---:|---:|---|
| mmap call, whole buffer | 3.00 | 4.67 | 21.0 % | | us |
| pass 1, first touch, one byte per page | 668.8 | 678.0 | 6.3 % | 65536 | ns/page |
| pass 2, second touch, one byte per page | 167.3 | 239.1 | 35.3 % | 0 | ns/page |
| pass 2 repeated, third touch | 4.3 | 4.5 | 128.3 % | | ns/page |
| pass 3, full sequential write | 79.0 | 91.6 | 4.5 % | 0 | GB/s |
| fresh fill, full write of a new mapping | 15.5 | 18.2 | 4.9 % | 65536 | GB/s |
| munmap, whole buffer | 172.1 | 184.9 | 10.4 % | | ns/page |
| malloc call, after free of the same size | 0.33 | 0.33 | 195.0 % | | us |
| malloc then first touch | 4.2 | 4.2 | 1.6 % | 0 | ns/page |
| malloc then second touch | 4.2 | 4.3 | 2.1 % | | ns/page |
| free | 0.01 | 0.01 | 14.3 % | | ns/page |

Per-page timing (cycle C, every store timed on its own with a 42 ns clock; a page above 200 ns did more than a page walk and a store):

| timed pass | pages above 200 ns, median over reps | min | max | cost | unit |
|---|---:|---:|---:|---:|---|
| pass 1, first touch | 100.0 % | | | 584 | ns, median page, min over reps |
| pass 2, second touch | 25.3 % | 24.6 % | 59.9 % | 574 | ns, mean of the pages above 200 ns, min over reps |

Where the slow pages of the timed pass 2 sit in the buffer, per rep (a run is a maximal stretch of neighbouring pages all above 200 ns):

| timed pass 2, slow pages | median | min | max | unit |
|---|---:|---:|---:|---|
| contiguous runs | 261 | 148 | 263 | runs |
| longest run | 251 | 68 | 28767 | pages |

Reps in which a fault-sized stall recurred on a pass with no counted faults (whole-pass average above 20 ns per page, which is at least 2.5 % of the pages at the pass 1 cost):

| pass | reps above the threshold | worst rep | unit |
|---|---:|---:|---|
| pass 2, second touch | 15 of 15 | 497.9 | ns/page |
| pass 2 repeated, third touch | 1 of 15 | 47.4 | ns/page |
| malloc then first touch | 0 of 15 | 4.4 | ns/page |
| malloc then second touch | 0 of 15 | 4.6 | ns/page |

| derived | value | unit |
|---|---:|---|
| pass 1 / pass 2 repeated (min ns per page; the headline ratio) | 155.0 | ratio |
| pass 1 / malloc then first touch | 158.2 | ratio |
| pass 1 / pass 2 (pass 2 carries the uncounted stalls described in the Analysis) | 4.0 | ratio |
| pass 3 / fresh fill (median GB/s) | 5.0 | ratio |
| mmap call per page | 0.046 | ns/page |
| pass 1 per page at 4.49 GHz | 3003 | cycles/page |
| pass 1 over the whole buffer (pages x min ns per page) | 43.8 | ms |
| pass 3 over the whole buffer (bytes / median GB/s) | 11.7 | ms |
| fresh fill over the whole buffer (bytes / median GB/s) | 59.1 | ms |
| faults on the first malloc of this size (warmup cycle) | 65536 | faults |
| malloc returned the block it had just freed | 15 of 15 | cycles |
