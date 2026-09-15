Array of 16384 floats, 65536 bytes, values 0..3, checksum 7328400 per call. Each timed region is 4 calls of 300 passes; 31 regions per kernel after one discarded warmup, one thread at default QoS. The ns figure is the min for a chain (a latency) and the median for the independent forms (throughputs). Cycles: every region is converted with the clock sampled just before it (min of three short dependent integer add chains), then the median is taken; the per-kernel median clock ranged from 4.151 to 4.513 GHz, and common/clock_estimate read 4.49 GHz just before the run.

| float array sum | min ns/elem | median ns/elem | cv | ns figure | cycles/elem | elem/cycle |
|---|---|---|---|---|---|---|
| 1 accumulator (one chain) | 0.4659 | 0.4697 | 4.1 % | min | 2.112 | 0.47 |
| 2 accumulators | 0.2434 | 0.2480 | 1.1 % | median | 1.119 | 0.89 |
| 4 accumulators | 0.1394 | 0.1409 | 6.1 % | median | 0.636 | 1.57 |
| 8 accumulators | 0.0810 | 0.0824 | 5.5 % | median | 0.371 | 2.69 |
| 16 accumulators | 0.0554 | 0.0612 | 3.8 % | median | 0.255 | 3.93 |

| register-only chains, inline asm | min ns/op | median ns/op | cv | ns figure | cycles/op | ops/cycle |
|---|---|---|---|---|---|---|
| int add, one chain (1 cycle by construction) | 0.2216 | 0.2227 | 1.7 % | min | 1.005 | 0.99 |
| int add, 8 independent chains | 0.0360 | 0.0381 | 4.8 % | median | 0.171 | 5.83 |
| fadd, one chain | 0.4653 | 0.4794 | 4.0 % | min | 2.135 | 0.47 |
| fadd, 8 independent chains | 0.0870 | 0.0871 | 1.4 % | median | 0.393 | 2.54 |
| fadd, 16 independent chains | 0.0554 | 0.0554 | 1.2 % | median | 0.250 | 4.00 |
| fmul, one chain | 0.6890 | 0.6935 | 1.4 % | min | 3.126 | 0.32 |

| derived | value | unit |
|---|---|---|
| speedup of 2 accumulators over the chain (cycles/elem ratio) | 1.89 | ratio |
| speedup of 4 accumulators over the chain (cycles/elem ratio) | 3.32 | ratio |
| speedup of 8 accumulators over the chain (cycles/elem ratio) | 5.69 | ratio |
| speedup of 16 accumulators over the chain (cycles/elem ratio) | 8.28 | ratio |
| fadd latency, from the 1-accumulator array chain | 2.11 | cycles |
| fadd latency, from the register-only chain | 2.13 | cycles |
| fmul latency, from the register-only chain | 3.13 | cycles |
| fadd throughput, array plateau (16 accumulators) | 3.93 | adds/cycle |
| fadd throughput, register-only 16 chains | 4.00 | adds/cycle |
| fadd throughput, register-only 8 chains | 2.54 | adds/cycle |
| int add throughput, register-only 8 chains | 5.83 | adds/cycle |
| chains needed for the plateau if latency were fixed (latency x throughput) | 8.3 | chains |
| 8 accumulators as a fraction of the plateau (elem/cycle ratio) | 0.69 | ratio |
| cycles each chain waits per add, 1 accumulator (1 x cycles/elem) | 2.11 | cycles |
| cycles each chain waits per add, 2 accumulators (2 x cycles/elem) | 2.24 | cycles |
| cycles each chain waits per add, 4 accumulators (4 x cycles/elem) | 2.54 | cycles |
| cycles each chain waits per add, 8 accumulators (8 x cycles/elem) | 2.97 | cycles |
| cycles each chain waits per add, 16 accumulators (16 x cycles/elem) | 4.08 | cycles |
| cycles each chain waits per add, register-only 8 chains (8 x cycles/add) | 3.14 | cycles |
| clock, per-kernel median of the per-rep samples, lowest / highest | 4.151 / 4.513 | GHz |
| clock, common/clock_estimate before the run | 4.49 | GHz |
