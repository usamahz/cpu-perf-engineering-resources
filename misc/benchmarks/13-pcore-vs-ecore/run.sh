#!/bin/sh
# Build, describe the machine, estimate the clock at default QoS and under
# the background clamp, run the benchmark in-process (main thread, default
# thread, background-QoS thread) and again as a whole process under
# taskpolicy -c background, and write results/raw.txt and
# results/summary.md; then paste the summary into README.md between the
# results markers.
#
#   ./run.sh            full run: 5e7 adds, 2048 FMA passes, 64 MiB stream,
#                       15 reps plus a warmup per kernel and placement
#   QUICK=1 ./run.sh    smoke test: 1e7 adds, 256 passes, 16 MiB, 5 reps,
#                       under ten seconds; writes 14-pcore-vs-ecore-quick-raw.txt
#                       and 14-pcore-vs-ecore-quick-summary.md under $TMPDIR
#                       (or /tmp) and leaves results/ and README.md alone
#   REPS=n ./run.sh     override the repetition count
set -eu
cd "$(dirname "$0")"

./build.sh

if [ ! -x ../common/clock_estimate ]; then
  echo "cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c"
  cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c
fi

if [ "${QUICK:-0}" != 0 ]; then
  # Smoke-test output stays out of results/ so it is never committed.
  outdir=${TMPDIR:-/tmp}; outdir=${outdir%/}
  raw=$outdir/14-pcore-vs-ecore-quick-raw.txt
  summary=$outdir/14-pcore-vs-ecore-quick-summary.md
  clock_env="REPS=3 CLOCK_ITERS_M=50"
else
  mkdir -p results
  raw=results/raw.txt
  summary=results/summary.md
  clock_env="REPS=7"
fi

# bench writes to a file rather than through a pipe so its own exit
# status, not tee's, is the one this script acts on: an allocation or
# pthread_create failure exits 1 without printing a FAIL line.
tmp=$(mktemp "${TMPDIR:-/tmp}/14-pcore-vs-ecore.XXXXXX")
trap 'rm -f "$tmp"' EXIT
run_bench() {
  status=0
  "$@" > "$tmp" || status=$?
  tee -a "$raw" < "$tmp"
  if [ $status -ne 0 ]; then
    echo "bench exited with status $status; see $raw"
    exit $status
  fi
}

have_taskpolicy=0
if command -v taskpolicy >/dev/null 2>&1; then have_taskpolicy=1; fi

# The shared clock estimate runs with its own REPS so a large benchmark
# REPS does not multiply its runtime; the benchmark itself carries its own
# per-placement estimate from the same add chain.
{
  ../common/machine.sh
  # What else the machine was doing: the clamped placement shares the
  # efficiency cores with every other background thread, and the P-core
  # rates move with package load too.
  echo "uptime: $(uptime)"
  env $clock_env ../common/clock_estimate
  if [ $have_taskpolicy = 1 ]; then
    printf 'taskpolicy -c background: '
    env $clock_env taskpolicy -c background ../common/clock_estimate
  else
    echo "taskpolicy not available on this platform: background clock estimate skipped"
  fi
} > "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
bg_ghz=$(awk '/^taskpolicy -c background: estimated clock:/ {print $6}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default}" >> "$raw"
echo "---" >> "$raw"
run_bench ./bench
invocations=1

if [ $have_taskpolicy = 1 ]; then
  echo "--- MAIN_ONLY=1 MAIN_TAG=process_background taskpolicy -c background ./bench" | tee -a "$raw"
  run_bench env MAIN_ONLY=1 MAIN_TAG=process_background taskpolicy -c background ./bench
  invocations=2
else
  echo "--- taskpolicy not available on this platform: process_background placement skipped" | tee -a "$raw"
fi

# Belt and braces: every invocation must have printed its closing line.
if grep -q '^FAIL' "$raw" || [ "$(grep -c '^checksums: every sample matched' "$raw")" -ne $invocations ]; then
  echo "checksum failure or incomplete output recorded in $raw"
  exit 1
fi

# Build the summary table from the RESULT lines. Names are
# <placement>_<metric>; placements are listed in the order they ran, and
# derived rows compare each placement with the main thread at default QoS.
awk -v ghz="$ghz" -v bg_ghz="$bg_ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function ratio(a, b) { return (b > 0) ? fmt(a / b, 2) : "n/a" }
function has(t) { return (t in seen) }
BEGIN {
  ntags = split("main_default thread_default thread_background process_background", tags, " ")
  desc["main_default"] = "main thread, as spawned"
  desc["thread_default"] = "pthread, no QoS call"
  desc["thread_background"] = "pthread, QOS_CLASS_BACKGROUND"
  desc["process_background"] = "main thread, taskpolicy -c background"
}
$1 == "RESULT" {
  name = $2; val = $3
  for (i = 1; i <= ntags; i++) {
    t = tags[i]
    if (index(name, t "_") == 1) {
      metric = substr(name, length(t) + 2)
      v[t, metric] = val
      if (!(t in seen)) { seen[t] = 1; order[++n] = t }
      next
    }
  }
}
/^adds per clock sample:/ { adds = $NF }
/^adds per clock bracket:/ { bracket_adds = $5 }
/^fma array bytes:/ { fma_bytes = $NF }
/^fma passes per sample:/ { fma_passes = $NF }
/^stream array bytes:/ { stream_bytes = $NF }
/^reps:/ { reps = $2 }
/^efficiency core cpu ids:/ { eids = $5 }
END {
  printf "Shared clock estimate (%s): %s GHz at default QoS", clockhow, ghz
  if (bg_ghz != "") printf ", %s GHz under taskpolicy -c background", bg_ghz
  printf ". Kernels: a dependent add chain of %s adds per sample (clock from the minimum), sixteen-accumulator NEON FMA over a %s-byte array, %s passes per sample (median), and a streaming sum over a %s-byte array of 32-bit values (median). GB is 1e9 bytes. %d timed samples per kernel and placement after one discarded warmup. The per-cycle columns divide each FMA or stream sample by the mean of two %s-add clock brackets taken immediately before and after it, on the same basis (median of the per-sample quotients). Efficiency-core cpu ids are %s on this part; \"samples on E ids\" is the share of timed samples that started and ended on one of them, and \"cpu share\" is the median of thread CPU time over wall time per sample. The \"main thread, as spawned\" placement makes no QoS call; the class the OS gave it is in the first table.\n\n", adds, fma_bytes, fma_passes, stream_bytes, reps, bracket_adds, eids
  print "| placement | thread QoS class | cpu ids seen | samples on E ids | cpu share |"
  print "|---|---|---|---|---|"
  for (i = 1; i <= n; i++) {
    t = order[i]
    e = v[t, "ecore_samples"]; if (e != "n/a") e = e " %"
    printf "| %s | %s | %s | %s | %s %% |\n", desc[t], v[t, "qos"], v[t, "cpus"], e, v[t, "cpu_share"]
  }
  print ""
  print "| placement | clock GHz (min) | clock GHz (median) | clock cv | FMA GFLOP/s | FMA cv | FMA per cycle (cv) | stream GB/s | stream cv | bytes per cycle (cv) |"
  print "|---|---|---|---|---|---|---|---|---|---|"
  for (i = 1; i <= n; i++) {
    t = order[i]
    g = v[t, "clock_median_ghz"]
    printf "| %s | %s | %s | %s %% | %s | %s %% | %s (%s %%) | %s | %s %% | %s (%s %%) |\n", desc[t], fmt(v[t, "clock_ghz"], 2), fmt(g, 2), fmt(v[t, "clock_cv"], 1),
      fmt(v[t, "fma_gflops"], 1), fmt(v[t, "fma_cv"], 1), fmt(v[t, "fma_per_cycle"], 2), fmt(v[t, "fma_per_cycle_cv"], 1),
      fmt(v[t, "stream_gbps"], 1), fmt(v[t, "stream_cv"], 1), fmt(v[t, "stream_bytes_per_cycle"], 1), fmt(v[t, "stream_bytes_per_cycle_cv"], 1)
  }
  print ""
  print "The same three quantities on thread CPU time instead of wall time (secondary; equal to the rows above where cpu share is 100 %):"
  print ""
  print "| placement | clock GHz (min) | clock GHz (median) | clock cv | FMA GFLOP/s | FMA cv | FMA per cycle (cv) | stream GB/s | stream cv | bytes per cycle (cv) |"
  print "|---|---|---|---|---|---|---|---|---|---|"
  for (i = 1; i <= n; i++) {
    t = order[i]
    g = v[t, "clock_cpu_median_ghz"]
    if (g == "n/a") { printf "| %s | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |\n", desc[t]; continue }
    printf "| %s | %s | %s | %s %% | %s | %s %% | %s (%s %%) | %s | %s %% | %s (%s %%) |\n", desc[t], fmt(v[t, "clock_cpu_ghz"], 2), fmt(g, 2), fmt(v[t, "clock_cpu_cv"], 1),
      fmt(v[t, "fma_cpu_gflops"], 1), fmt(v[t, "fma_cpu_cv"], 1), fmt(v[t, "fma_cpu_per_cycle"], 2), fmt(v[t, "fma_cpu_per_cycle_cv"], 1),
      fmt(v[t, "stream_cpu_gbps"], 1), fmt(v[t, "stream_cpu_cv"], 1), fmt(v[t, "stream_cpu_bytes_per_cycle"], 1), fmt(v[t, "stream_cpu_bytes_per_cycle_cv"], 1)
  }
  print ""
  print "Ratios of the rows above: clock (min) is min over min, the other three are median over median."
  print ""
  print "| ratio, main thread as spawned over | basis | clock (min) | clock (median) | FMA GFLOP/s | stream GB/s |"
  print "|---|---|---|---|---|---|"
  m = "main_default"
  for (i = 2; i <= n; i++) {
    t = order[i]
    printf "| %s | wall time | %s | %s | %s | %s |\n", desc[t], ratio(v[m, "clock_ghz"], v[t, "clock_ghz"]),
      ratio(v[m, "clock_median_ghz"], v[t, "clock_median_ghz"]),
      ratio(v[m, "fma_gflops"], v[t, "fma_gflops"]), ratio(v[m, "stream_gbps"], v[t, "stream_gbps"])
    if (v[t, "clock_cpu_ghz"] != "n/a" && v[m, "clock_cpu_ghz"] != "n/a")
      printf "| %s | thread cpu time | %s | %s | %s | %s |\n", desc[t], ratio(v[m, "clock_cpu_ghz"], v[t, "clock_cpu_ghz"]),
        ratio(v[m, "clock_cpu_median_ghz"], v[t, "clock_cpu_median_ghz"]),
        ratio(v[m, "fma_cpu_gflops"], v[t, "fma_cpu_gflops"]), ratio(v[m, "stream_cpu_gbps"], v[t, "stream_cpu_gbps"])
  }
  if (!has("process_background")) print "| main thread, taskpolicy -c background | wall time | not run | not run | not run | not run |"
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
