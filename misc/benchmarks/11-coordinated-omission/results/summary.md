Virtual clock: 20 s simulated per client per rep. Both clients follow one schedule of 5000 requests/s (one due every 200 us) against a server with a 100 us service time and a 200 ms stall once per second (20 per run), so the server timeline is identical and only the recording differs; the corrected column is HdrHistogram's correction of the closed-loop samples at the expected interval of 200 us. Percentiles use HdrHistogram's rank convention (the sample at rank round(p/100 times n), at least 1), in simulated us, minimum over the 31 timed reps after a discarded warmup; the simulation is deterministic, so cv over reps is 0 by construction. Every percentile and share below, the latency sums behind them, the corrected sample count, and both loops' stall counts and last completion times equalled the closed form derived in bench.c (832 checks). The clock estimate, 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs), converts the wall cost of the simulation itself to cycles in the last table.

| statistic | closed loop, from actual send | open loop, from intended send | closed loop, corrected | cv over reps |
|---|---|---|---|---|
| p50 (us) | 100.0 | 100.0 | 100.0 | 0.0 % |
| p90 (us) | 100.0 | 150100.0 | 80300.0 | 0.0 % |
| p99 (us) | 100.0 | 195100.0 | 188100.0 | 0.0 % |
| p99.9 (us) | 100.0 | 199600.0 | 198900.0 | 0.0 % |
| max (us) | 200100.0 | 200100.0 | 200100.0 | 0.0 % |
| samples per run | 100000 | 100000 | 119980 | 0.0 % |
| share of samples above 1 ms (%) | 0.0200 | 39.8200 | 16.6028 | 0.0 % |

| derived | value | unit |
|---|---|---|
| open loop p99 / closed loop p99 | 1951.0 | ratio |
| open loop p99.9 / closed loop p99.9 | 1996.0 | ratio |
| open loop p90 / closed loop p90 | 1501.0 | ratio |
| open loop p50 / closed loop p50 | 1.00 | ratio |
| open loop max / closed loop max | 1.00 | ratio |
| corrected p99 / closed loop p99 | 1881.0 | ratio |
| corrected p99.9 / closed loop p99.9 | 1989.0 | ratio |
| open loop p99 / corrected p99 | 1.037 | ratio |
| open loop p90 / corrected p90 | 1.869 | ratio |
| closed loop max / closed loop p99 | 2001.0 | ratio |
| open loop share above 1 ms / closed loop share | 1991.0 | ratio |
| open-loop requests delayed by each stall | 2000 | requests |
| samples the correction adds for each stall | 999 | samples |
| slow samples the closed loop records for each stall | 1 | samples |
| stalls per run | 20 | stalls |

| simulation cost (wall time, this machine) | min ns per simulated request | cycles per request | median ns | cv over reps |
|---|---|---|---|---|
| closed loop | 0.76 | 3.42 | 0.93 | 25.1 % |
| open loop | 0.76 | 3.42 | 0.93 | 25.2 % |
