# 11. Coordinated omission

**Claim.** A closed-loop load generator hides server stalls: against a service that takes 100 us and stalls for 200 ms once a second, driven on one schedule of 5000 requests per second, the p99 a simulated closed-loop client records from each request's actual send stays at the 100 us service time, while the p99 a simulated open-loop client records from the same request's intended send time is three orders of magnitude higher, for one identical server timeline. Supports: "Coordinated Omission" under "Measuring the tail", and "wrk2" under "Load generation and production workloads", both in README section 11, "Tail latency and production systems".

**Method.** `bench.c` simulates one server and one request schedule on a virtual clock: a `uint64_t` of simulated nanoseconds that the code advances by the service time of each request and never by reading the wall clock. The server handles one request at a time in 100 us; from 0.5 s, once every second, it is unavailable for 200 ms, and a request in service when a stall begins is extended by it while one arriving during a stall waits for it to end. Request i is due at i times 200 us (5000 requests per second, half the server's capacity) for both clients. The closed-loop client has one connection and sends nothing while a request is outstanding, so a request that falls due during a wait leaves the moment the reply arrives, and it records reply minus actual send. The open-loop client sends every request when it is due, the server serves in arrival order, and it records reply minus due time, as wrk2 does. In both cases request i starts at the later of its due time and the previous completion and completes at the same instant, so the two loops in `bench.c` walk one identical server timeline and differ only in what they subtract from the completion time; the program checks that both loops meet the same 20 stalls and complete their last request at the same instant. The third data set is the closed-loop samples re-recorded with HdrHistogram's `recordValueWithExpectedInterval` correction at the schedule interval of 200 us, which adds, for every sample of v at least twice the interval, the samples v minus the interval, v minus twice the interval, and so on down to the interval. Each client runs 20 simulated seconds per rep (5 with `QUICK=1`): 100000 requests each, 119980 corrected samples, with 20 stalls. The program reports p50, p90, p99, p99.9 and max of each data set by HdrHistogram's rank convention (the sample at rank round(p/100 times n), at least 1, of the sorted list; this is nearest-rank with rounding where the textbook rule takes a ceiling, and the two differ only at the corrected set's p99 and p99.9, where p times n over 100 is not an integer, by one 200 us step), the sample count, and the share of samples above 1 ms. One warmup rep is discarded, then `REPS` (default 31) reps run; percentiles are latency-like and the minimum over reps is reported with `cv`, counts and shares as medians. On a virtual clock every rep is identical and cv is 0 by construction; the one quantity this machine can move is the wall cost of the simulation itself, timed with `now_ns()` around each client's loop and reported as the minimum over reps of nanoseconds per simulated request, with the median and `cv` beside it. Every percentile, slow count and latency sum, the corrected sample count, and both loops' stall counts and last completion times are checked against a closed form derived separately in `bench.c` from the constants alone (the arithmetic sequence of latencies a stall leaves behind in each recording), and the three latency sums go through `SINK()`; any mismatch fails the run. This is a simulation, not a load generator: no request was sent and no stall was timed on this machine. It replaces the two-thread proposal in `misc/notes/sections/12-tail-latency.md` (a spinning consumer, a producer on a schedule, a real sleep for the stall) because only a virtual clock gives the two recordings exactly the same stall schedule; a two-thread version would add sleep and scheduling jitter to the same arithmetic and could not be checked against a closed form.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. Each client's simulation is a real loop that multiplies the request index by the interval to get the due time, takes the later of that and the previous completion, loads the server's next stall time, compares it with the request's completion, takes the stall path when it applies, and stores every latency to the sample array. The constants in the prologue are, in file order, 200000000 (`0xbebc200`, the stall), 200000 (`0x30d40`, the interval), 100000 (`0x186a0`, the service time), 200100000 (`0xbed48a0`, stall plus service) and 1000000000 (`0x3b9aca00`, the stall period), in simulated nanoseconds. The two loops are instruction for instruction the same except for the `csel` that picks the start time: the closed loop writes it over the due time in `x15`, so the `sub` that follows records completion minus send; the open loop keeps the due time in `x15` and the start in `x16`, so the same `sub` records completion minus due. Both are quoted in file order with the compiler's `; %bb` comment lines dropped; `...` marks omitted instructions and everything after a `;` on an instruction line is annotation.

```
_run_closed:
	mov	x8, #0                          ; free_at = 0
	cbz	x2, LBB1_7
	mov	x9, #0                          ; i = 0
	mov	w10, #49664                     ; =0xc200
	movk	w10, #3051, lsl #16             ; x10 = 200000000, the stall
	mov	w11, #3392                      ; =0xd40
	movk	w11, #3, lsl #16                ; x11 = 200000, the interval
	mov	w12, #34464                     ; =0x86a0
	movk	w12, #1, lsl #16                ; x12 = 100000, the service time
	mov	w13, #18592                     ; =0x48a0
	movk	w13, #3053, lsl #16             ; x13 = 200100000, stall plus service
	mov	w14, #51712                     ; =0xca00
	movk	w14, #15258, lsl #16            ; x14 = 1000000000, the stall period
	b	LBB1_3
LBB1_2:
	sub	x15, x8, x15                    ; latency = done - sent
	str	x15, [x1, x9, lsl #3]           ; lat[i] = latency
	add	x9, x9, #1
	cmp	x9, x2
	b.eq	LBB1_7
LBB1_3:
	mul	x15, x9, x11                    ; due = i * 200000
	cmp	x15, x8
	csel	x15, x15, x8, hi                ; sent = max(due, free_at); the due time is not kept
	ldr	x16, [x0]                       ; server->next_stall
	add	x8, x15, x12                    ; done = sent + service
	cmp	x16, x8
	b.hs	LBB1_2                          ; no stall before this request completes
	ldr	x17, [x0, #8]                   ; server->stalls
LBB1_5:
	add	x3, x16, x13                    ; stall_end + service
	add	x4, x16, x10                    ; stall_end
	cmp	x4, x15
	csel	x3, x3, x8, hi
	add	x8, x8, x10                     ; done + stall
	cmp	x16, x15
	csel	x8, x3, x8, ls                  ; extend, or wait for the stall to end
	add	x17, x17, #1                    ; stalls++
	add	x16, x16, x14                   ; next_stall += period
	cmp	x16, x8
	b.lo	LBB1_5
	stp	x16, x17, [x0]
	b	LBB1_2
LBB1_7:
	mov	x0, x8                          ; return free_at
	ret

_run_open:
	...                                     ; the same prologue, x10 to x14 as above
LBB2_2:
	sub	x15, x8, x15                    ; latency = done - due
	str	x15, [x1, x9, lsl #3]           ; lat[i] = latency
	add	x9, x9, #1
	cmp	x9, x2
	b.eq	LBB2_7
LBB2_3:
	mul	x15, x9, x11                    ; due = i * 200000
	cmp	x15, x8
	csel	x16, x15, x8, hi                ; start = max(due, free_at); the due time stays in x15
	ldr	x17, [x0]                       ; server->next_stall
	add	x8, x16, x12                    ; done = start + service
	cmp	x17, x8
	b.hs	LBB2_2                          ; no stall before this request completes
	...                                     ; the same stall path, then stp and b LBB2_2
```

The call site in `main` shows that the loops run between `clock_gettime` calls and return their last completion time, that their sample arrays go through `correct_co` and `summarise` (a `qsort` and the percentile reads), and that the three sums pass through the `SINK()` asm before the checks compare the stall counts with the reference.

```
	bl	_clock_gettime
	...
	bl	_run_closed
	mov	x25, x0                         ; end_closed
	...
	bl	_clock_gettime
	...
	bl	_clock_gettime
	...
	bl	_run_open
	mov	x28, x0                         ; end_open
	...
	bl	_clock_gettime
	...
	bl	_correct_co
	mov	x26, x0                         ; nk, the corrected sample count
	...
	bl	_summarise
	...
	bl	_summarise
	...
	bl	_summarise
	ldr	x8, [sp, #640]
	str	x8, [sp, #472]
	add	x8, sp, #472
	; InlineAsm Start
	; InlineAsm End
	...
	cmp	x19, x23                        ; closed-loop stalls against the reference
	b.eq	LBB0_24
```

`results/raw.txt` ends with `checksums: 832 closed-form checks passed over 32 runs`, and every per-rep line above the timings prints the sample counts and percentiles that were checked.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core for which Apple publishes no microarchitecture name. `machine.sh` output is at the top of `results/raw.txt`. The percentile tables do not depend on the machine at all, because the clock is virtual; only the simulation cost table does.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT.
- Frequency: `estimated clock: 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)`, as `results/raw.txt` prints it, measured by `../common/clock_estimate` at the same QoS just before the benchmark (`run.sh` clears `REPS` for it, so `REPS=n` reaches only the benchmark). The machine condition was `load average at start: 11.47 8.53 8.14`, as `results/raw.txt` prints it: other processes belonging to the user were running, and since the benchmark is single-threaded it competed for cores only where the table's cv says so, which is the 25.1 % and 25.2 % of the simulation cost rows. DVFS is on and cannot be disabled, and the whole benchmark is well under 0.1 s of wall time, which is shorter than the clock takes to settle: the per-rep costs in `results/raw.txt` fall through the reps, which is the other thing the cv of the simulation cost rows measures and why their minimum is the reported figure. Only the simulation cost table depends on the clock. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c`.
- Workload: a discrete-event simulation of one server (100 us service, 200 ms stall once a second from 0.5 s) under one schedule of 5000 requests per second, recorded by a closed-loop client from the actual send and by an open-loop client from the intended send, 20 simulated seconds per client per rep, plus the HdrHistogram correction of the closed-loop samples at the 200 us expected interval. The sample arrays are 800000 bytes for each client (100000 64-bit slots) and 1376128 bytes for the corrected set (172016 slots allocated, 119980 used), as `results/raw.txt` prints; they exceed the 131072-byte P-core L1d and sit in the 16777216-byte L2; the simulation loops write them sequentially and `qsort` reads them back.
- Baseline: the closed-loop recording of the timeline, which is what a load generator that waits for each reply and times from its own send produces; the open-loop recording of the same timeline and the corrected closed-loop data are compared with it.
- Method: virtual clock in simulated nanoseconds; percentiles by HdrHistogram's rank convention (the sample at rank round(p/100 times n), at least 1, of the sorted list); one warmup rep discarded; 31 timed reps; minimum over reps for percentiles and for the simulation cost per request, median for counts and shares, cv printed for all; every percentile, slow count and sum, the corrected count, and both loops' stall counts and last completions checked against the closed form in `bench.c`; sums consumed with `SINK()`; the wall cost of each client's loop measured with `now_ns()` (`CLOCK_MONOTONIC_RAW`) and divided by its request count.

## Results

<!-- results:start -->
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
<!-- results:end -->

## Analysis

The closed-loop client records 100.0 us at p50, p90, p99 and p99.9 and only its max, 200100.0 us, shows that the server ever stalled; the open-loop recording of the same timeline gives 150100.0 us at p90, 195100.0 us at p99 and 199600.0 us at p99.9, so the p99 ratio is 1951.0, the p99.9 ratio 1996.0, and the two agree only at p50 (ratio 1.00) and at max (ratio 1.00). The mechanism is what the closed loop does during a stall: it is waiting for one reply, so it sends nothing and each 200 ms stall becomes exactly 1 slow sample among the 100000 it records per run (0.0200 percent of them above 1 ms, too few to reach p99.9, which is why its max is 2001.0 times its own p99), and the 1000 requests that fell due while it waited then leave back to back and are each answered 100.0 us after they leave, so they record the service time however late they were. The open-loop recording charges each request its wait from the time it was due, so 2000 requests per stall (the 1000 due during the stall, and as many again due while the backlog drains at the server's spare capacity of one request per 200 us) record an arithmetic sequence from 200100.0 us down; 39.8200 percent of its 100000 samples are above 1 ms, 1991.0 times the closed-loop share, and the p99 lands at 195100.0 us because the slowest 1 percent of that sequence are the requests that waited for almost the whole stall. The HdrHistogram correction rebuilds most of the tail from the closed-loop data alone: it adds 999 samples per stall (the samples a client sending every 200 us would have taken while it was stuck), which raises the corrected p99 from 100.0 us to 188100.0 us, 1881.0 times the recorded value and within a ratio of 1.037 of the open-loop p99, and the corrected p99.9 to 198900.0 us. It does not reproduce the open-loop distribution lower down, because all it knows is the expected interval: it fills the stall itself in 200 us steps and knows nothing about the recovery, during which the server ran at full rate while the schedule kept adding one delayed request every 200 us, so the corrected set has 16.6028 percent of its 119980 samples above 1 ms against the open loop's 39.8200 percent, and its p90 is 80300.0 us against 150100.0 us, a ratio of 1.869. The correction restores the shape of the stall; it cannot invent the queue behind it, so timing each request from its intended send time, as wrk2 does, is the recording that needs no correction. The wall cost of the simulation is under a nanosecond per simulated request on this core (0.76 ns, 3.42 cycles, for either loop), so the 32 runs of the simulation take well under 0.1 s in total; the second or so that `./run.sh` takes is the compile and the clock estimate.

## Limits

The clock is virtual, so this benchmark shows nothing about the machine: no NUMA, no SMT and no PMU are involved, and the percentile tables would be identical on an x86 server part, on an E-core, or in an interpreter; only the simulation cost table would change, and it is there to satisfy the repository's timing rules, not to support the claim. The percentile tables are a computation checked against a closed form, not a measurement; a reader who wants the wall-clock version can build the two-thread program that `misc/notes/sections/12-tail-latency.md` proposes and should expect the same ratios with sleep and scheduling jitter added. The model is deliberately simple: one server, first come first served, a deterministic service time, stalls that begin on the schedule's due instants with an idle server and end well before the next stall, a schedule at half the server's capacity, an open-loop client with unbounded queueing and no timeouts, and a closed-loop client with one connection that catches up on the slots it missed. A closed-loop client that drops the missed slots instead records 1000 fewer samples per stall and the same percentiles; it was not used because its server timeline would then differ from the open loop's. A real closed-loop generator with many connections hides a stall in the same way but at its own rate, and a real service has variable service times and stalls that begin mid-request, which change the constants in the tables but not the direction of any ratio. The correction here uses the schedule interval of 200 us, which is the expected interval HdrHistogram asks for; a different expected interval gives a different corrected distribution, and no interval recovers the recovery queue, which is the point of the p90 comparison above and the reason the open-loop recording is preferred. The simulation also omits the recorder's own pauses (the case LatencyUtils corrects), network time, and the noise a real generator adds; on a real system the open-loop p99 would include those and the closed-loop p99 would still not.

## Reproduce

    ./run.sh            # full run, about 1 second, nearly all of it the compile and the clock estimate
    QUICK=1 ./run.sh    # smoke test, about 1 second; writes results/quick-raw.txt and
                        # results/quick-summary.md and leaves README.md alone
    REPS=51 ./run.sh    # more timed reps (only the simulation cost rows and the rep and
                        # check counts change; the clock estimate keeps its own 7 runs)

`run.sh` builds with `build.sh`, writes the machine description and clock estimate to the top of `results/raw.txt`, appends the benchmark output, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from.
