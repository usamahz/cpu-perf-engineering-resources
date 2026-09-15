#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: 1<<16 calls per rep for the small sizes, a
#                       256 MiB byte budget for the large ones, 31 reps plus
#                       a warmup
#   QUICK=1 ./run.sh    smoke test: 1<<12 calls, 16 MiB budget, 5 reps, under
#                       ten seconds; writes results/quick-raw.txt and
#                       results/quick-summary.md and leaves README.md alone
#   REPS=n ./run.sh     override the repetition count
set -eu
cd "$(dirname "$0")"

./build.sh

if [ ! -x ../common/clock_estimate ]; then
  echo "cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c"
  cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c
fi

mkdir -p results
if [ "${QUICK:-0}" != 0 ]; then
  raw=results/quick-raw.txt
  summary=results/quick-summary.md
else
  raw=results/raw.txt
  summary=results/summary.md
fi

{
  ../common/machine.sh
  # Section 11 says a syscall number is not comparable without the kernel
  # version, the boot parameters and the mitigation state. machine.sh only
  # prints the Darwin release, so record the xnu build string here. macOS
  # exposes the boot arguments through sysctl (empty means none are set)
  # but nothing about mitigations; Linux exposes all three.
  if sysctl -n kern.version >/dev/null 2>&1; then
    echo "kernel: $(sysctl -n kern.version)"
    bootargs=$(sysctl -n kern.bootargs 2>/dev/null || true)
    echo "boot args: ${bootargs:-none set}"
    echo "mitigation state: not exposed by macOS"
  else
    echo "kernel: $(uname -v)"
    if [ -r /proc/cmdline ]; then echo "boot args: $(cat /proc/cmdline)"; fi
    if [ -d /sys/devices/system/cpu/vulnerabilities ]; then
      for f in /sys/devices/system/cpu/vulnerabilities/*; do
        echo "mitigation $(basename "$f"): $(cat "$f")"
      done
    fi
  fi
  ../common/clock_estimate
} > "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} CLOCK_GHZ=$ghz" >> "$raw"
echo "---" >> "$raw"
# A pipe into tee returns tee's status, so bench writes to a file first and
# a non-zero exit stops the run before any summary or README is written.
if CLOCK_GHZ="$ghz" ./bench > "$raw.bench" 2>&1; then
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
else
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
  echo "bench exited with an error; see $raw" >&2; exit 1
fi
# The pipe takes tee's status, so a checksum mismatch would otherwise still
# produce a summary and refresh the README.
if grep -q '^FAIL' "$raw"; then
  echo "bench failed; see $raw"
  exit 1
fi

# Build the summary table from the RESULT lines. Per-variant names are
# <variant>_<stat> where <variant> is pread_<bytes>, memcpy_<bytes>, getppid
# or clock_gettime; the fit results are pread_fixed, pread_copy_slope and
# their memcpy twins. Derived rows use the min for anything latency-like and
# the median for the copy rates. Portable awk: no gawk extensions.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function label(k,   b) {
  if (k == "getppid") return "getppid"
  if (k == "clock_gettime") return "clock_gettime (MONOTONIC_RAW)"
  b = k; sub(/^[a-z]+_/, "", b)
  return (k ~ /^pread/ ? "pread " : "memcpy ") b " B"
}
function cyc(k) { return ((k "_cycles") in R) ? fmt(R[k "_cycles"], 0) : "n/a" }
function nsb(k) { return ((k "_nsbyte") in R) ? sprintf("%.3g", R[k "_nsbyte"]) : "-" }
function ratio(a, b) { return fmt(R[a "_min"] / R[b "_min"], 2) }
$1 == "RESULT" {
  R[$2] = $3
  if ($2 ~ /_bytes$/) { k = $2; sub(/_bytes$/, "", k); order[++nk] = k }
}
/^read offset / { off = $3 }
/^file / { fbytes = $4 }
END {
  printf "Clock estimate %s GHz (%s). One thread at default QoS, which macOS schedules on a P-core. A %s-byte file in the page cache, read at offset %s. Each row is the min over %s reps of one pass of the stated number of calls, after one discarded warmup; the median and cv are shown beside it. cycles/call is min ns/call times the clock estimate; ns/byte is min ns/call divided by the bytes per call.\n\n", ghz, clockhow, fbytes, off, R["reps"]
  print "| variant | bytes per call | calls per rep | min ns/call | median ns/call | cv | cycles/call | ns/byte |"
  print "|:---|---:|---:|---:|---:|---:|---:|---:|"
  for (i = 1; i <= nk; i++) {
    k = order[i]
    printf "| %s | %s | %s | %s | %s | %.1f %% | %s | %s |\n", label(k), R[k "_bytes"], R[k "_calls"], fmt(R[k "_min"], 1), fmt(R[k "_median"], 1), R[k "_cv"], cyc(k), nsb(k)
  }
  print ""
  print "| derived | value | unit |"
  print "|:---|---:|:---|"
  printf "| pread fixed cost, intercept of a line through the 1, 64 and 4096 B mins | %s | ns |\n", fmt(R["pread_fixed"], 1)
  if ("pread_fixed_cycles" in R) printf "| pread fixed cost in cycles | %s | cycles |\n", fmt(R["pread_fixed_cycles"], 0)
  printf "| memcpy fixed cost, same fit | %s | ns |\n", fmt(R["memcpy_fixed"], 1)
  printf "| pread 64 B / pread 1 B | %s | ratio |\n", ratio("pread_64", "pread_1")
  printf "| pread 4096 B / pread 1 B | %s | ratio |\n", ratio("pread_4096", "pread_1")
  printf "| pread 65536 B / pread 1 B | %s | ratio |\n", ratio("pread_65536", "pread_1")
  printf "| pread 1048576 B / pread 1 B | %s | ratio |\n", ratio("pread_1048576", "pread_1")
  printf "| pread 1 B / getppid | %s | ratio |\n", ratio("pread_1", "getppid")
  printf "| getppid / clock_gettime | %s | ratio |\n", ratio("getppid", "clock_gettime")
  printf "| pread 1 B / clock_gettime | %s | ratio |\n", ratio("pread_1", "clock_gettime")
  printf "| pread 1 B / memcpy 1 B | %s | ratio |\n", ratio("pread_1", "memcpy_1")
  printf "| pread 4096 B / memcpy 4096 B | %s | ratio |\n", ratio("pread_4096", "memcpy_4096")
  printf "| pread 1048576 B / memcpy 1048576 B | %s | ratio |\n", ratio("pread_1048576", "memcpy_1048576")
  printf "| pread 1 B minus memcpy 1 B, the crossing at 1 B | %s | ns |\n", fmt(R["pread_1_min"] - R["memcpy_1_min"], 1)
  printf "| pread 4096 B minus memcpy 4096 B, the crossing at 4 KiB | %s | ns |\n", fmt(R["pread_4096_min"] - R["memcpy_4096_min"], 1)
  printf "| pread copy slope, 65536 to 1048576 B, from the mins | %s | ns/byte |\n", fmt(R["pread_copy_slope"], 5)
  printf "| memcpy copy slope, 65536 to 1048576 B, from the mins | %s | ns/byte |\n", fmt(R["memcpy_copy_slope"], 5)
  printf "| pread 1048576 B copy rate, from the median | %s | GB/s |\n", fmt(R["pread_1048576_bytes"] / R["pread_1048576_median"], 1)
  printf "| memcpy 1048576 B copy rate, from the median | %s | GB/s |\n", fmt(R["memcpy_1048576_bytes"] / R["memcpy_1048576_median"], 1)
  printf "| break-even request, pread fixed cost / pread copy slope | %s | bytes |\n", fmt(R["pread_fixed"] / R["pread_copy_slope"], 0)
  printf "| pread 1 B ns/byte / pread 1048576 B ns/byte | %s | ratio |\n", fmt(R["pread_1_nsbyte"] / R["pread_1048576_nsbyte"], 0)
  printf "| pread 1 B calls per second, from the median | %s | million/s |\n", fmt(1000 / R["pread_1_median"], 2)
  printf "| pread 1048576 B calls per second, from the median | %s | thousand/s |\n", fmt(1000000 / R["pread_1048576_median"], 1)
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
