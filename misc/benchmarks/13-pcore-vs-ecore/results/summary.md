Shared clock estimate (dependent 1-cycle add chain, 400000000 adds, min of 7 runs): 4.49 GHz at default QoS, 1.04 GHz under taskpolicy -c background. Kernels: a dependent add chain of 50000000 adds per sample (clock from the minimum), sixteen-accumulator NEON FMA over a 16384-byte array, 2048 passes per sample (median), and a streaming sum over a 67108864-byte array of 32-bit values (median). GB is 1e9 bytes. 15 timed samples per kernel and placement after one discarded warmup. The per-cycle columns divide each FMA or stream sample by the mean of two 5000000-add clock brackets taken immediately before and after it, on the same basis (median of the per-sample quotients). Efficiency-core cpu ids are 0-3 on this part; "samples on E ids" is the share of timed samples that started and ended on one of them, and "cpu share" is the median of thread CPU time over wall time per sample. The "main thread, as spawned" placement makes no QoS call; the class the OS gave it is in the first table.

| placement | thread QoS class | cpu ids seen | samples on E ids | cpu share |
|---|---|---|---|---|
| main thread, as spawned | user-interactive | 9,13 | 0.0 % | 100.0 % |
| pthread, no QoS call | default | 10,12,13 | 0.0 % | 100.0 % |
| pthread, QOS_CLASS_BACKGROUND | background | 11,12 | 0.0 % | 100.0 % |
| main thread, taskpolicy -c background | background | 0,1,2,3 | 100.0 % | 75.8 % |

| placement | clock GHz (min) | clock GHz (median) | clock cv | FMA GFLOP/s | FMA cv | FMA per cycle (cv) | stream GB/s | stream cv | bytes per cycle (cv) |
|---|---|---|---|---|---|---|---|---|---|
| main thread, as spawned | 4.51 | 4.50 | 0.4 % | 144.1 | 1.5 % | 4.00 (1.2 %) | 82.2 | 14.9 % | 18.2 (15.0 %) |
| pthread, no QoS call | 4.50 | 4.49 | 1.1 % | 144.0 | 0.5 % | 4.01 (0.8 %) | 85.5 | 3.0 % | 19.1 (2.9 %) |
| pthread, QOS_CLASS_BACKGROUND | 4.51 | 4.50 | 0.4 % | 143.9 | 1.0 % | 4.00 (0.8 %) | 84.1 | 1.8 % | 18.7 (2.1 %) |
| main thread, taskpolicy -c background | 1.39 | 0.74 | 23.2 % | 12.0 | 26.1 % | 1.87 (29.3 %) | 8.4 | 27.8 % | 11.3 (29.4 %) |

The same three quantities on thread CPU time instead of wall time (secondary; equal to the rows above where cpu share is 100 %):

| placement | clock GHz (min) | clock GHz (median) | clock cv | FMA GFLOP/s | FMA cv | FMA per cycle (cv) | stream GB/s | stream cv | bytes per cycle (cv) |
|---|---|---|---|---|---|---|---|---|---|
| main thread, as spawned | 4.51 | 4.50 | 0.4 % | 144.1 | 1.5 % | 4.00 (1.1 %) | 82.2 | 14.9 % | 18.2 (15.0 %) |
| pthread, no QoS call | 4.51 | 4.49 | 1.0 % | 143.9 | 0.5 % | 4.01 (0.8 %) | 85.5 | 2.9 % | 19.1 (2.7 %) |
| pthread, QOS_CLASS_BACKGROUND | 4.51 | 4.50 | 0.4 % | 143.9 | 1.0 % | 4.00 (0.8 %) | 84.0 | 1.8 % | 18.7 (2.1 %) |
| main thread, taskpolicy -c background | 1.52 | 1.12 | 9.8 % | 16.0 | 17.5 % | 1.97 (17.6 %) | 10.9 | 8.2 % | 10.6 (13.4 %) |

Ratios of the rows above: clock (min) is min over min, the other three are median over median.

| ratio, main thread as spawned over | basis | clock (min) | clock (median) | FMA GFLOP/s | stream GB/s |
|---|---|---|---|---|---|
| pthread, no QoS call | wall time | 1.00 | 1.00 | 1.00 | 0.96 |
| pthread, no QoS call | thread cpu time | 1.00 | 1.00 | 1.00 | 0.96 |
| pthread, QOS_CLASS_BACKGROUND | wall time | 1.00 | 1.00 | 1.00 | 0.98 |
| pthread, QOS_CLASS_BACKGROUND | thread cpu time | 1.00 | 1.00 | 1.00 | 0.98 |
| main thread, taskpolicy -c background | wall time | 3.24 | 6.12 | 12.01 | 9.82 |
| main thread, taskpolicy -c background | thread cpu time | 2.96 | 4.03 | 9.01 | 7.52 |
