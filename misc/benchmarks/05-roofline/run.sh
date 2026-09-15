#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: 256 MiB kernel arrays, 512 MiB bandwidth array,
#                       21 timed passes after 100 ms of warm-up passes, 1 and 10 threads
#   QUICK=1 ./run.sh    smoke test: 64 MiB arrays, 5 timed passes, under ten seconds;
#                       writes results/quick-raw.txt and results/quick-summary.md
#                       and leaves README.md alone
#   REPS=n ./run.sh     override the number of timed passes
#   THREADS=n ./run.sh  override the thread count of the all-cores case
#   WARMUP_MS=n ./run.sh  discard passes for at least n ms per case (default 100)
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
  ../common/clock_estimate
} > "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} THREADS=${THREADS:-default} CLOCK_GHZ=$ghz" >> "$raw"
echo "---" >> "$raw"
# A pipe into tee returns tee's status, so bench writes to a file first and
# a non-zero exit stops the run before any summary or README is written.
if CLOCK_GHZ="$ghz" ./bench > "$raw.bench" 2>&1; then
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
else
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
  echo "bench exited with an error; see $raw" >&2; exit 1
fi

# Build the summary table from the RESULT lines. Roofs are <roof>_<T>t
# (the higher median of the two measurements, with _min, _max, _cv, the
# _other median and which was _kept), kernels <kernel>_<T>t (from DRAM) and
# <kernel>_l1_<T>t (from L1), predictions <kernel>_<T>t_pred with the roof
# names in _roof and _bwroof, and every rate has _cv beside it. Derived
# rows use the medians.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function ratio(a, b) { return (b > 0) ? fmt(a / b, 2) : "n/a" }
$1 == "RESULT" { v[$2] = $3; u[$2] = $4 }
END {
  T = v["threads"]
  split("1 " T, ts, " ")
  nts = (T == 1) ? 1 : 2
  printf "Clock estimate %s GHz (%s), one thread at default QoS. Kernel arrays %s bytes each, bandwidth array %s bytes, L1 slices %s bytes of x and of y per thread (%s bytes of x for fma_stream). Rates are medians over %s timed passes after at least %s ms of discarded warm-up passes. Each roof is measured before and after the kernels at that thread count and the higher median is the roof; the row shows that measurement and the other median is in the last column. The roofline prediction is min(FMA peak, intensity x memory roof) at the same thread count, where the memory roof is the one whose stream mix matches the loop: the read bandwidth for a loop that only reads, the copy bandwidth for one that reads one array and writes one, the triad bandwidth for one that reads two and writes one. FMA peak is 20 accumulator chains; the 16-chain row is the same loop with the textbook count.\n\n", ghz, clockhow, v["kernel_bytes"], v["bandwidth_bytes"], v["l1_slice_bytes"], v["fma_stream_l1_slice_bytes"], v["reps"], v["warmup_ms"]
  print "| roof | threads | median | min | max | cv | other measurement |"
  print "|---|---|---|---|---|---|---|"
  nr = split("fma_peak fma_16acc read_bw copy_bw triad_bw write_bw", roofs, " ")
  split("FMA peak (20 chains);FMA 16 chains;read bandwidth (sum);copy bandwidth (copy);triad bandwidth (triad);write bandwidth (fill)", rlabel, ";")
  for (i = 1; i <= nts; i++) {
    t = ts[i]
    for (r = 1; r <= nr; r++) {
      k = roofs[r] "_" t "t"
      printf "| %s | %s | %s %s | %s %s | %s %s | %.1f %% | %s %s (%s) |\n", rlabel[r], t, fmt(v[k], 1), u[k], fmt(v[k "_min"], 1), u[k], fmt(v[k "_max"], 1), u[k], v[k "_cv"], fmt(v[k "_other"], 1), u[k], (v[k "_kept"] == "before") ? "after the kernels" : "before the kernels"
    }
  }
  print ""
  print "| kernel | threads | intensity flop/B | memory roof | from DRAM GFLOP/s | cv | from L1 GFLOP/s | cv | prediction GFLOP/s | binding roof | DRAM / prediction | DRAM / L1 | DRAM traffic GB/s |"
  print "|---|---|---|---|---|---|---|---|---|---|---|---|---|"
  nk = split("saxpy stencil fma_stream poly poly_intrin", ks, " ")
  for (i = 1; i <= nts; i++) {
    t = ts[i]
    for (j = 1; j <= nk; j++) {
      k = ks[j] "_" t "t"; l = ks[j] "_l1_" t "t"
      printf "| %s | %s | %s | %s | %s | %.1f %% | %s | %.1f %% | %s | %s | %s | %s | %s |\n", ks[j], t, fmt(v[ks[j] "_ai"], 3), v[k "_bwroof"], fmt(v[k], 1), v[k "_cv"], fmt(v[l], 1), v[l "_cv"], fmt(v[k "_pred"], 1), v[k "_roof"], ratio(v[k], v[k "_pred"]), ratio(v[k], v[l]), fmt(v[k] / v[ks[j] "_ai"], 1)
    }
  }
  print ""
  print "| derived | value | unit |"
  print "|---|---|---|"
  for (i = 1; i <= nts; i++) {
    t = ts[i]; s = (t == 1) ? "1 thread" : t " threads"
    printf "| ridge point, FMA peak / read bandwidth, %s | %s | flop/B |\n", s, fmt(v["ridge_read_" t "t"], 2)
    printf "| ridge point, FMA peak / copy bandwidth, %s | %s | flop/B |\n", s, fmt(v["ridge_copy_" t "t"], 2)
    printf "| ridge point, FMA peak / triad bandwidth, %s | %s | flop/B |\n", s, fmt(v["ridge_triad_" t "t"], 2)
  }
  if (nts == 2) {
    printf "| FMA peak, %s threads / 1 thread | %s | ratio |\n", T, ratio(v["fma_peak_" T "t"], v["fma_peak_1t"])
    printf "| read bandwidth, %s threads / 1 thread | %s | ratio |\n", T, ratio(v["read_bw_" T "t"], v["read_bw_1t"])
    printf "| copy bandwidth, %s threads / 1 thread | %s | ratio |\n", T, ratio(v["copy_bw_" T "t"], v["copy_bw_1t"])
    printf "| triad bandwidth, %s threads / 1 thread | %s | ratio |\n", T, ratio(v["triad_bw_" T "t"], v["triad_bw_1t"])
    printf "| write bandwidth, %s threads / 1 thread | %s | ratio |\n", T, ratio(v["write_bw_" T "t"], v["write_bw_1t"])
  }
  for (i = 1; i <= nts; i++) {
    t = ts[i]; s = (t == 1) ? "1 thread" : t " threads"
    if (("fma_per_cycle_" t "t") in v) printf "| FMA instructions per cycle per core at the clock estimate, %s | %s | fmla/cycle |\n", s, fmt(v["fma_per_cycle_" t "t"], 2)
    printf "| FMA 16 chains / FMA peak, %s | %s | ratio |\n", s, ratio(v["fma_16acc_" t "t"], v["fma_peak_" t "t"])
    printf "| copy bandwidth / read bandwidth, %s | %s | ratio |\n", s, ratio(v["copy_bw_" t "t"], v["read_bw_" t "t"])
    printf "| triad bandwidth / read bandwidth, %s | %s | ratio |\n", s, ratio(v["triad_bw_" t "t"], v["read_bw_" t "t"])
    printf "| write bandwidth / read bandwidth, %s | %s | ratio |\n", s, ratio(v["write_bw_" t "t"], v["read_bw_" t "t"])
    printf "| fma_stream from DRAM / FMA peak, %s | %s | ratio |\n", s, ratio(v["fma_stream_" t "t"], v["fma_peak_" t "t"])
    printf "| fma_stream from L1 / FMA peak, %s | %s | ratio |\n", s, ratio(v["fma_stream_l1_" t "t"], v["fma_peak_" t "t"])
    printf "| poly from DRAM / FMA peak, %s | %s | ratio |\n", s, ratio(v["poly_" t "t"], v["fma_peak_" t "t"])
    printf "| poly_intrin from DRAM / poly from DRAM, %s | %s | ratio |\n", s, ratio(v["poly_intrin_" t "t"], v["poly_" t "t"])
    printf "| saxpy DRAM traffic / triad bandwidth, %s | %s | ratio |\n", s, ratio(v["saxpy_" t "t"] / v["saxpy_ai"], v["triad_bw_" t "t"])
    printf "| saxpy read traffic, 8 of its 12 bytes, / read bandwidth, %s | %s | ratio |\n", s, ratio(v["saxpy_" t "t"] / v["saxpy_ai"] * 8 / 12, v["read_bw_" t "t"])
    printf "| stencil DRAM traffic / copy bandwidth, %s | %s | ratio |\n", s, ratio(v["stencil_" t "t"] / v["stencil_ai"], v["copy_bw_" t "t"])
    printf "| stencil from L1 / FMA peak, %s | %s | ratio |\n", s, ratio(v["stencil_l1_" t "t"], v["fma_peak_" t "t"])
  }
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
