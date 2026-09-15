#!/bin/sh
# Build, describe the machine, estimate the clock, run the in-process parts
# once and the fresh-process part RUNS times, write results/raw.txt and
# results/summary.md, and paste the summary into README.md between the
# results markers.
#
#   ./run.sh            full run: 1<<20 floats, REPS=30 passes, RUNS=30 processes
#   QUICK=1 ./run.sh    smoke test: 1<<17 floats, 10 passes, 10 processes, under
#                       ten seconds; writes its raw and summary files under
#                       ${TMPDIR:-/tmp} and leaves results/ and README.md alone
#   REPS=n RUNS=n       override the pass count and the process count (10 or more)
#   SETTLE_MS=n         override the settle phase each process runs after its
#                       cold pass (default 100 ms, 20 ms under QUICK)
#
# The load average is recorded before and after the run. The spread this
# benchmark measures is the point of it, so a run taken with other work on
# the machine measures that work; the summary and README carry the load so
# a reader can tell.
set -eu
cd "$(dirname "$0")"

if [ "${QUICK:-0}" != 0 ]; then
  reps=${REPS:-10}
  runs=${RUNS:-10}
else
  reps=${REPS:-30}
  runs=${RUNS:-30}
fi
case "$reps$runs" in *[!0-9]*) echo "REPS and RUNS must be integers" >&2; exit 1 ;; esac
if [ "$reps" -lt 10 ] || [ "$runs" -lt 10 ]; then
  echo "REPS and RUNS must be at least 10 (got REPS=$reps RUNS=$runs)" >&2
  exit 1
fi

./build.sh

if [ ! -x ../common/clock_estimate ]; then
  echo "cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c"
  cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c
fi

if [ "${QUICK:-0}" != 0 ]; then
  outdir=${TMPDIR:-/tmp}; outdir=${outdir%/}
  raw=$outdir/05-measurement-pitfalls-quick-raw.txt
  summary=$outdir/05-measurement-pitfalls-quick-summary.md
else
  mkdir -p results
  raw=results/raw.txt
  summary=results/summary.md
fi

# The 1, 5 and 15 minute load averages, as sysctl or /proc report them.
loadavg() {
  if command -v sysctl >/dev/null 2>&1 && sysctl -n vm.loadavg >/dev/null 2>&1; then
    sysctl -n vm.loadavg | tr -d '{}' | sed 's/^ *//; s/ *$//'
  elif [ -r /proc/loadavg ]; then
    cut -d' ' -f1-3 /proc/loadavg
  else
    echo "unknown"
  fi
}

load_before=$(loadavg)
{
  ../common/machine.sh
  echo "load average before the run (1, 5, 15 min): $load_before"
  ../common/clock_estimate
} > "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")
echo "bench: QUICK=${QUICK:-0} REPS=$reps RUNS=$runs SETTLE_MS=${SETTLE_MS:-default}" >> "$raw"
export REPS="$reps" RUNS="$runs"
load1=${load_before%% *}
if [ "$load1" != unknown ] && [ "$(echo "$load1" | awk '{print ($1 > 1.0)}')" = 1 ]; then
  echo "warning: 1 minute load average is $load1; the spreads below include other work on the machine" >&2
fi

# bench's exit status is what decides whether the run is valid, so its
# output goes through a temporary file rather than straight into tee,
# which would hide the status under plain sh.
tmp=$(mktemp "${TMPDIR:-/tmp}/bench05.XXXXXX")
trap 'rm -f "$tmp"' EXIT
run_bench() {
  rc=0
  ./bench "$@" > "$tmp" || rc=$?
  if [ "$rc" -ne 0 ]; then
    cat "$tmp" >&2
    echo "./bench $* failed with status $rc; see $raw" >&2
    exit "$rc"
  fi
}

# The first execution of a binary the linker just wrote pays extra inside
# its cold pass, over and above the page faults every process pays, so it
# is the warmup of the binary itself: recorded, relabelled, not aggregated.
run_bench fresh
{
  echo "---"
  echo "== ./bench fresh: first execution of the binary build.sh just wrote, discarded as the warmup of the binary itself"
  sed 's/^SAMPLE /DISCARDED /' "$tmp"
  echo "---"
  echo "== ./bench: the in-process parts, one process"
} >> "$raw"
run_bench
tee -a "$raw" < "$tmp"

{
  echo "---"
  echo "== ./bench fresh, $runs processes, one cold pass and one warm pass each"
} >> "$raw"
i=1
while [ "$i" -le "$runs" ]; do
  run_bench fresh
  cat "$tmp" >> "$raw"
  i=$((i + 1))
done
load_after=$(loadavg)
echo "load average after the run (1, 5, 15 min): $load_after" >> "$raw"

# Aggregate the SAMPLE lines into RESULT lines. For the fresh processes:
# min, median, max, cv = stddev/mean, the statistics timing.h reports. For
# both warm series: the share of pairs of single passes whose larger value
# exceeds the smaller by more than 2, 5 and 10 percent, which is the
# question a one-run-per-side comparison asks. Computed into a variable
# first so awk has finished reading the file before tee appends to it.
agg=$(awk '
    function emit(name, out,    i, j, x, k, sum, mean, sq, med) {
      k = n[name]
      for (i = 1; i < k; i++) {
        x = v[name, i]
        for (j = i - 1; j >= 0 && v[name, j] > x; j--) v[name, j + 1] = v[name, j]
        v[name, j + 1] = x
      }
      sum = 0; for (i = 0; i < k; i++) sum += v[name, i]
      mean = sum / k
      sq = 0; for (i = 0; i < k; i++) sq += (v[name, i] - mean) * (v[name, i] - mean)
      med = (k % 2) ? v[name, int(k / 2)] : (v[name, k / 2 - 1] + v[name, k / 2]) / 2
      printf "%-40s min %12.3f  median %12.3f  max %12.3f  cv %5.1f%%  n %d  ns/pass\n", out, v[name, 0], med, v[name, k - 1], (mean > 0) ? 100 * sqrt(sq / k) / mean : 0, k
      printf "RESULT %s_min_ns %.2f ns\n", out, v[name, 0]
      printf "RESULT %s_median_ns %.2f ns\n", out, med
      printf "RESULT %s_max_ns %.2f ns\n", out, v[name, k - 1]
      printf "RESULT %s_cv_pct %.2f percent\n", out, (mean > 0) ? 100 * sqrt(sq / k) / mean : 0
      printf "RESULT %s_n %d runs\n", out, k
    }
    function pairs(name, out,    i, j, k, a, b, r, np, c2, c5, c10) {
      k = n[name]; np = 0; c2 = 0; c5 = 0; c10 = 0
      for (i = 0; i < k; i++) for (j = i + 1; j < k; j++) {
        a = v[name, i]; b = v[name, j]
        r = (a > b) ? a / b : b / a
        np++
        if (r > 1.02) c2++
        if (r > 1.05) c5++
        if (r > 1.10) c10++
      }
      printf "%-40s pairs %d  over 2%%: %d (%.1f%%)  over 5%%: %d (%.1f%%)  over 10%%: %d (%.1f%%)\n", out " pairs of single passes", np, c2, 100 * c2 / np, c5, 100 * c5 / np, c10, 100 * c10 / np
      printf "RESULT %s_pairs %d pairs\n", out, np
      printf "RESULT %s_pairs_over_2pct %.1f percent\n", out, 100 * c2 / np
      printf "RESULT %s_pairs_over_5pct %.1f percent\n", out, 100 * c5 / np
      printf "RESULT %s_pairs_over_10pct %.1f percent\n", out, 100 * c10 / np
    }
    $1 == "SAMPLE" { v[$2, n[$2]++] = $3 }
    END {
      if (n["p2_fresh"] == 0) { print "no fresh-process samples" > "/dev/stderr"; exit 1 }
      if (n["p2_inproc"] == 0) { print "no in-process samples" > "/dev/stderr"; exit 1 }
      emit("p2_fresh", "p2_fresh")
      emit("p3_cold", "p3_cold")
      pairs("p2_inproc", "p2_inproc")
      pairs("p2_fresh", "p2_fresh")
    }' "$raw")
{
  echo "---"
  echo "== aggregates computed by run.sh from the SAMPLE lines: the $runs fresh processes, and the pairs of single passes in both warm series"
  printf '%s\n' "$agg"
} | tee -a "$raw"

# The proof that the discarded variant has no loop: the three timing
# functions as the compiler emitted them, copied so the committed raw.txt
# carries them (bench_O2.s itself is a build product and not committed).
{
  echo "---"
  echo "== bench_O2.s: the three timing functions as emitted by cc -std=c11 -O2 -S"
  awk '/^_?time_(discarded|sink|checksum):/ { p = 1 } p { print } /\.cfi_endproc/ { if (p) print ""; p = 0 }' bench_O2.s
} >> "$raw"

# Summary tables from the RESULT lines: Part 1 by variant, Parts 2 and 3
# by series, the pair shares, then the derived ratios the analysis quotes.
awk -v ghz="$ghz" -v clockhow="$clockhow" -v load_before="$load_before" -v load_after="$load_after" '
  function ns(x) { return sprintf("%.0f", x) }
  function pct(x) { return sprintf("%.1f %%", x) }
  function r3(x) { return sprintf("%.3f", x) }
  function r2(x) { return sprintf("%.2f", x) }
  function p1row(label, k) {
    printf "| %s | %s | %s | %s | %s | %s |\n", label, ns(v[k "_median_ns"]), ns(v[k "_min_ns"]), ns(v[k "_max_ns"]), pct(v[k "_cv_pct"]), r3(v[k "_median_ns"] / v["p1_sink_median_ns"])
  }
  function serow(label, k, n) {
    printf "| %s | %s | %s | %s | %s | %s | %s |\n", label, n, ns(v[k "_min_ns"]), ns(v[k "_median_ns"]), ns(v[k "_max_ns"]), pct(v[k "_cv_pct"]), r3(v[k "_max_ns"] / v[k "_min_ns"])
  }
  function pairrow(t) {
    printf "| more than %s percent | %s | %s |\n", t, pct(v["p2_inproc_pairs_over_" t "pct"]), pct(v["p2_fresh_pairs_over_" t "pct"])
  }
  function drow(label, x, u) { printf "| %s | %s | %s |\n", label, x, u }
  $1 == "date:" { date = $2 }
  $1 == "PARAM" { p[$2] = $3 }
  $1 == "RESULT" { v[$2] = $3 }
  END {
    warm = v["p2_inproc_median_ns"]
    printf "Run of %s. Clock estimate %s GHz (%s). Load average (1, 5, 15 min) %s before the run and %s after it. Array of %s floats, %s bytes, %s pages of %s bytes; one full pass per timed region, times in ns per pass. REPS=%s passes per variant after a discarded warmup, RUNS=%s consecutive in-process passes and RUNS=%s fresh processes for the spread, %s ms settle in every process between its cold pass and anything else it measures. Clock tick %s ns (the smallest non-zero step of now_ns). Checksum over the %s checksum passes: %s, expected %s.\n\n", date, ghz, clockhow, load_before, load_after, p["floats"], p["bytes"], p["pages"], p["page_bytes"], p["reps"], p["runs"], p["runs"], p["settle_ms"], ns(v["clock_tick_ns"]), p["reps"], v["p1_checksum_value"], sprintf("%.1f", p["floats"] / 8 * 7 * p["reps"])
    print "Part 1: the same loop, three ways of using its result, passes taken round-robin."
    print ""
    print "| variant | median ns/pass | min ns/pass | max ns/pass | cv | median / (b) |"
    print "|---|---|---|---|---|---|"
    p1row("(a) result discarded", "p1_discarded")
    p1row("(b) result passed to SINK()", "p1_sink")
    p1row("(c) result stored for the checksum", "p1_checksum")
    print ""
    print "Part 2: variant (b), one warm pass per sample, across passes and across processes. Part 3: the cold first pass, once from the in-process run and once per fresh process."
    print ""
    print "| series | n | min ns/pass | median ns/pass | max ns/pass | cv | max / min |"
    print "|---|---|---|---|---|---|---|"
    serow("p2 in-process, consecutive warm passes", "p2_inproc", p["runs"])
    serow("p2 fresh process, one warm pass each", "p2_fresh", v["p2_fresh_n"])
    printf "| p3 cold first pass, single shot from the in-process run | 1 | %s | %s | %s | n/a | n/a |\n", ns(v["p3_cold_single_ns"]), ns(v["p3_cold_single_ns"]), ns(v["p3_cold_single_ns"])
    serow("p3 cold first pass, one per fresh process", "p3_cold", v["p3_cold_n"])
    print ""
    printf "Pairs of single warm passes of identical code (%s in-process pairs, %s fresh-process pairs) whose larger value exceeds the smaller by:\n\n", v["p2_inproc_pairs"], v["p2_fresh_pairs"]
    print "| difference | in-process pairs | fresh-process pairs |"
    print "|---|---|---|"
    pairrow("2")
    pairrow("5")
    pairrow("10")
    print ""
    print "| derived | value | unit |"
    print "|---|---|---|"
    drow("(a) discarded median / (b) SINK median", sprintf("%.2f", 100 * v["p1_discarded_median_ns"] / v["p1_sink_median_ns"]), "percent")
    drow("(a) discarded max / (b) SINK median", sprintf("%.3f", 100 * v["p1_discarded_max_ns"] / v["p1_sink_median_ns"]), "percent")
    drow("(c) checksum median / (b) SINK median", r3(v["p1_checksum_median_ns"] / v["p1_sink_median_ns"]), "ratio")
    drow("(b) SINK median per element", r3(v["p1_sink_median_ns"] / p["floats"]), "ns/elem")
    drow("(b) SINK median per element, times the clock estimate", r2(v["p1_sink_median_ns"] / p["floats"] * ghz), "cycles/elem")
    drow("in-process spread, max / min minus 1", sprintf("%.1f", 100 * (v["p2_inproc_max_ns"] / v["p2_inproc_min_ns"] - 1)), "percent")
    drow("fresh-process spread, max / min minus 1", sprintf("%.1f", 100 * (v["p2_fresh_max_ns"] / v["p2_fresh_min_ns"] - 1)), "percent")
    drow("fresh-process median / in-process median", r3(v["p2_fresh_median_ns"] / warm), "ratio")
    drow("fresh-process cv / in-process cv", r2(v["p2_fresh_cv_pct"] / v["p2_inproc_cv_pct"]), "ratio")
    drow("cold single shot / cold fresh-process median", r3(v["p3_cold_single_ns"] / v["p3_cold_median_ns"]), "ratio")
    drow("cold single shot / cold fresh-process min", r3(v["p3_cold_single_ns"] / v["p3_cold_min_ns"]), "ratio")
  }' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: results/ and README.md not touched"
else
  python3 ../common/refresh_readme.py .
fi
