# 10. Syscall cost

**Claim.** A kernel crossing has a fixed cost, 83 ns here for the bare trap and 281 ns for a 1-byte `pread`, that does not shrink with the size of the request, so a 1-byte read costs about as much as a 4 KiB read; only fewer crossings, by batching or bypass, amortise it. Supports: "vdso(7)", "FlexSC: Flexible System Call Scheduling with Exception-Less System Calls" and "Efficient IO with io_uring" in README section 10, OS and I/O, subsection "Syscalls and asynchronous I/O".

**Method.** `bench.c` fills a 67108864-byte (64 MiB) buffer from a fixed-seed splitmix64 generator, writes it to a file in a fresh `mkdtemp` directory under `$TMPDIR`, calls `fsync`, and reads the whole file back once with `pread` in 1 MiB pieces, checking every byte against the buffer, so that every page is in the page cache before anything is timed. The timed loops then call `pread` for 1, 64, 4096, 65536 and 1048576 bytes at one fixed page-aligned offset, 33554432 (the middle of the file), into one 16 KiB-aligned destination buffer. The request size is the only thing that changes between those five rows; the descriptor, the offset, the page-cache pages and the destination are the same. Three references sit beside them, all measured in the same process and the same passes: `getppid()`, the smallest call that still traps into the kernel; `clock_gettime(CLOCK_MONOTONIC_RAW)`, the clock `now_ns()` reads, which the kernel answers from a user-mapped page without a trap on macOS (the commpage) and Linux (the vDSO); and a user-space `memcpy` of each of the five sizes from the buffer that holds the same bytes, at the same offset, into the same destination, which is what the copy costs without the crossing. Each pass is `now_ns()` around a loop of a fixed number of calls: 65536 calls for the sizes up to 4096 bytes, and for the two large sizes a 268435456-byte budget divided by the size, which is 4096 calls at 65536 bytes and 256 at 1048576, so a pass of large reads takes about as long as a pass of small ones. One warmup pass of every variant is discarded, then `REPS` (default 31) timed passes are taken round-robin, one pass of every variant per rep, so noise drifting over the run lands on every variant alike. The quantity is nanoseconds per call, which is latency-like, so the statistic is the minimum over the passes with the median and `cv` beside it; the two copy rates in GB/s are throughput-like and use the median. Nanoseconds per byte is the minimum divided by the request size, and cycles per call is the minimum multiplied by the clock estimate from `../common/clock_estimate`, which `run.sh` passes in as `CLOCK_GHZ`. The fixed cost is the intercept of a least-squares line through the minimums at 1, 64 and 4096 bytes, where the copy is at most a few tens of nanoseconds and cannot swamp the crossing; the per-byte cost of the kernel copy is the slope between 65536 and 1048576 bytes, where the crossing is a rounding error. Every `pread` loop folds the byte count each call returns and the first and last byte it wrote into a sum that is compared with a value computed from the source buffer, the destination is `memcmp`ed against the source after every pass, the `memcpy` loop is checked the same way, every `getppid` result is summed and compared with the parent pid times the call count, and the `clock_gettime` readings are summed and printed as a checksum; every sum also goes through `SINK()`. A mismatch fails the run.

**Generated code.** `build.sh` writes `bench_O2.s` from the same flags as the binary. Each of the four timed loops keeps its call: `bl _pread`, `bl _memcpy`, `bl _getppid` and `bl _clock_gettime`, and the value each call produces is added into the accumulator that the function returns.

```
_loop_pread:
LBB1_2:
	mov	x0, x22
	mov	x1, x21
	mov	x2, x20
	mov	w3, #33554432          ; the fixed offset, folded in by the compiler
	bl	_pread
	ldrb	w8, [x21]              ; dst[0]
	ldurb	w9, [x24, #-1]         ; dst[len - 1]
	add	x10, x0, x23           ; sum += bytes returned
	add	x8, x10, x8
	add	x23, x8, x9
	subs	x19, x19, #1
	b.ne	LBB1_2

_loop_memcpy:
LBB2_2:
	; InlineAsm Start            ; CLOBBER(): memory may have changed
	; InlineAsm End
	mov	x0, x22
	mov	x1, x21
	mov	x2, x20                ; runtime length, so a real libc call
	bl	_memcpy
	ldrb	w8, [x22]
	ldurb	w9, [x24, #-1]
	add	x10, x23, x20
	add	x8, x10, x8
	add	x23, x8, x9
	subs	x19, x19, #1
	b.ne	LBB2_2

_loop_getppid:
LBB3_2:
	bl	_getppid
	add	x20, x20, w0, sxtw     ; sum += pid
	subs	x19, x19, #1
	b.ne	LBB3_2

_loop_clock:
LBB4_2:
	mov	x1, sp
	mov	w0, #4                 ; CLOCK_MONOTONIC_RAW
	bl	_clock_gettime
	ldr	x8, [sp, #8]           ; tv_nsec
	add	x20, x8, x20
	subs	x19, x19, #1
	b.ne	LBB4_2
```

The call site in `main` shows the work cannot be deleted: each loop is called between two `clock_gettime` calls, its return value is kept in a register, the destination is `memcmp`ed against the source, and the sum is compared with the reference.

```
	bl	_clock_gettime
	...
	bl	_loop_pread
	mov	x27, x0
	add	x1, sp, #224
	mov	w0, #4
	bl	_clock_gettime
	...
	mov	w9, #33554432
	add	x1, x8, x9
	mov	x2, x28
	bl	_memcmp
	cbz	w0, LBB0_46
```

`results/raw.txt` ends with `checksums: every pread and memcpy pass matched its reference sum and the source bytes; every getppid returned the parent pid`, and the `clock_gettime` checksum is printed as `RESULT clock_checksum`.

**Machine.**

- CPU model and microarchitecture: Apple M4 Pro, one performance core; an Armv9-class out-of-order core for which Apple publishes no microarchitecture name. macOS 26.2, kernel `Darwin Kernel Version 25.2.0: Tue Nov 18 21:09:56 PST 2025; root:xnu-12377.61.12~1/RELEASE_ARM64_T6041` from `sysctl -n kern.version`; `kern.bootargs` is empty, so no boot arguments are set; macOS exposes no mitigation state, Apple does not publish whether XNU unmaps the kernel on exit on this part, and nothing here can measure it, so the `getppid` row is the whole crossing whatever it contains. `run.sh` records all three lines after the `machine.sh` output at the top of `results/raw.txt`.
- Cores used: one thread at default QoS, which macOS schedules on a P-core; nothing pinned, since macOS has no affinity API. 10 P-cores and 4 E-cores on the part, no SMT. Machine condition: `results/raw.txt` records `load average at start: 10.21 8.24 8.04`, the 1, 5 and 15 minute averages on 14 logical CPUs, so other processes belonging to the user were running; the benchmark is single-threaded, so it competed for a core only where the table's cv column says so.
- Frequency: `estimated clock: 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs)` from `results/raw.txt`, measured by `../common/clock_estimate` at the same QoS just before the benchmark. DVFS is on and cannot be disabled; the cycles column is the minimum time multiplied by this estimate, so it is a ceiling if the core ran slower during a kernel entry. No SMT on this part.
- Compiler and flags: Apple clang 17.0.0 (clang-1700.4.4.1), `cc -std=c11 -O2 -Wall -Wextra -o bench bench.c -lm`.
- Workload: `pread` of 1, 64, 4096, 65536 and 1048576 bytes from a page-cached 67108864-byte file at offset 33554432, 65536 calls per pass for the three small sizes and 4096 and 256 calls for the two large ones; `getppid` and `clock_gettime(CLOCK_MONOTONIC_RAW)`, 65536 calls per pass. The 1 MiB request is 64 of the 16 KiB pages and fits the 16 MiB L2, so after the first pass the source pages are cache resident in both the kernel copy and the user copy.
- Baseline: a user-space `memcpy` of the same size from a buffer holding the same bytes, at the same offset, into the same destination (the copy without the crossing); `getppid` (the crossing without a copy); `clock_gettime` (a system call that never crosses).
- Method: `now_ns()` (`CLOCK_MONOTONIC_RAW`) around one pass; one warmup pass per variant discarded; 31 timed passes per variant taken round-robin; minimum reported for nanoseconds per call and everything derived from it, median for the two copy rates, cv printed for every row; every result consumed with `SINK()` and checked against a reference.

## Results

<!-- results:start -->
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
<!-- results:end -->

## Analysis

A 1-byte `pread` costs 281.2 ns and a 4096-byte one costs 313.7 ns, a ratio of 1.12, with the 64-byte read at 281.9 ns and a ratio of 1.00: across a 4096-fold change in request size the call barely moves, the fitted fixed cost is 281.3 ns, 1263 cycles, and the same fit for `memcpy` gives 0.9 ns. The crossing, the read minus the copy, is 280.1 ns at 1 byte and 276.2 ns at 4 KiB, so what the kernel adds for copying 4 KiB is less than the 37.5 ns a user-space `memcpy` of 4 KiB takes, and everything else is the same fixed price on every call. `getppid` costs 83.1 ns, 373 cycles, which is the trap, the kernel entry and exit and one word read; the 1-byte `pread` is 3.38 times that, so most of its fixed cost is the file path, the descriptor and page-cache lookups and the copy setup rather than the mode switch itself, which is the ordering FlexSC reports between a null call and a real one. `clock_gettime` costs 12.2 ns; `getppid` is 6.81 times that and the 1-byte read 23.05 times, because the clock never enters the kernel: a call that looks like a system call but is answered from a user-mapped page is a different thing from one that traps, which is what vdso(7) exists to tell you. The fixed cost only stops mattering once the copy is larger than it: the kernel copies at 0.01957 ns per byte between 64 KiB and 1 MiB, so the break-even request is 14374 bytes, and at 1 MiB the read costs 72.86 times the 1-byte call while moving 1048576 times the data, so its cost per byte is 14393 times lower. That is the whole case for batching: 3.44 million 1-byte calls a second move 3.44 MB per second, while 1 MiB requests, at 47.2 thousand calls a second, move 49.5 GB/s, so about one crossing in seventy-three moves about fourteen thousand times the bytes. Beyond 4 KiB the kernel copy is slower per byte than `memcpy` (0.01957 against 0.01086 ns per byte, 49.5 against 85.9 GB/s, and the 1 MiB read is 1.83 times the 1 MiB `memcpy`), which is consistent with the kernel walking the page cache one page at a time and looking each page up before it copies; so even a large read pays for the crossing and then some, and the two ways out are to batch many requests into one crossing, as `io_uring` does with a single submission, or to map the data into user space once and never cross again, as `AF_XDP` and DPDK do.

## Limits

There is no PMU access from user space on macOS, so the fixed cost cannot be split into the trap, the kernel entry and exit, the VFS path and the cache and TLB state each evicts; the direct cost is all this benchmark measures, and FlexSC's point that the indirect cost can exceed it is not tested here. Apple does not publish whether XNU unmaps the kernel on exit on this part and nothing here can measure it, so the `getppid` row is the whole crossing whatever it contains; on an x86 server with KPTI the SOSP 2019 paper in section 10 measures a constant cost several times larger for an empty call before any work is done, and a Linux kernel with retpolines and a mitigated `copy_to_user` will move the absolute numbers and the break-even size, though not the shape. The read is always the same 1 MiB of the file, so the source pages are hot in L2 for both the kernel and the user copy; a read that misses the page cache goes to the device and the crossing becomes a rounding error, which is why the claim is about cached and small I/O. The temp directory is on the boot APFS volume; the file system does not enter the timed loops, only the setup, but a network file system with a different page-cache path would. DVFS is on, the load average at the start of the run is the one the Machine section quotes, and the cv column shows what that cost each row; the ratios do not depend on the clock. On Linux, `getppid` is the classic null system call and `clock_gettime` goes through the vDSO, so the same program applies; the ratios between the three kinds of call will hold while the constants change with the kernel version, its boot parameters and its mitigation state, all three of which section 10 says to record; `run.sh` writes the first two from `sysctl` and notes that macOS exposes no mitigation state, and on Linux it prints `uname -v`, `/proc/cmdline` and every file under `/sys/devices/system/cpu/vulnerabilities`.

## Reproduce

    ./run.sh            # full run, about 4 to 5 seconds
    QUICK=1 ./run.sh    # smoke test, about 2 seconds; writes results/quick-raw.txt and
                        # results/quick-summary.md and leaves README.md alone
    REPS=51 ./run.sh    # more timed passes per variant

`run.sh` builds with `build.sh`, writes the machine description, the kernel version and boot arguments, and the clock estimate to the top of `results/raw.txt`, appends the benchmark output, stops if that output contains a `FAIL` line, builds `results/summary.md` from the `RESULT` lines with awk, and pastes it into this README between the results markers. `build.sh` also regenerates `bench_O2.s`, which is where the quoted assembly comes from. The file is created under `$TMPDIR` (or `/tmp`) and removed when the run ends.
