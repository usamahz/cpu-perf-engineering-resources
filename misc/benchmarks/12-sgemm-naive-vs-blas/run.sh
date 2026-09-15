#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark twice
# (the vendor BLAS pinned to one thread, then at its default thread count),
# and write results/raw.txt and results/summary.md; then paste the summary
# into README.md between the results markers.
#
#   ./run.sh            full run: 1024x1024x1024 SGEMM, 11 reps plus a warmup;
#                       dot products over 1<<24 elements, 31 reps plus a warmup
#   QUICK=1 ./run.sh    smoke test: 256^3 and 1<<20 elements, 5 reps, under ten
#                       seconds; writes results/quick-raw.txt and
#                       results/quick-summary.md and leaves README.md alone
#   REPS=n ./run.sh     override the SGEMM repetition count (DOT_REPS=n for the
#                       dot products)
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
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} DOT_REPS=${DOT_REPS:-default} CLOCK_GHZ=$ghz" >> "$raw"

# Under POSIX sh a pipeline returns tee's status, so bench writes to a file
# first and its own exit code is what set -e sees: a MISMATCH or FAIL keeps
# the raw output for inspection and stops before summary.md and README.md
# are refreshed with bad numbers.
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
bench_failed() {
  tee -a "$raw" < "$tmp"
  echo "bench exited non-zero: $raw kept, $summary and README.md not refreshed" >&2
  exit 1
}
echo "--- VECLIB_MAXIMUM_THREADS=1: every variant, one thread" >> "$raw"
VECLIB_MAXIMUM_THREADS=1 ./bench > "$tmp" || bench_failed
tee -a "$raw" < "$tmp"
echo "--- MODE=blas, VECLIB_MAXIMUM_THREADS unset: the vendor BLAS at its default thread count" >> "$raw"
(unset VECLIB_MAXIMUM_THREADS; MODE=blas ./bench) > "$tmp" || bench_failed
tee -a "$raw" < "$tmp"

# Build the summary table from the RESULT lines. SGEMM rows are
# <variant>_<stat>; dot rows are dot_<type>_<insn>_<size>_<stat>. Derived
# rows use the medians, the clock estimate and the measured ceilings. The
# 32 flops per cycle bound is four 128-bit FMA pipes times four lanes times
# two flops, the vector width and pipe count of this P-core.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function have(k) { return ((k "_gflops") in v) }
function gemm_row(k, label,   cells) {
  if (k "_skipped" in v) { printf "| %s | skipped | | | | | |\n", label; return }
  if (!have(k)) return
  printf "| %s | %s | %s | %s | %s %% | %s | %s |\n", label, fmt(v[k "_gflops"], 2), fmt(v[k "_gflops_max"], 2),
         fmt(v[k "_ms"], 2), fmt(v[k "_cv"], 1), sprintf("%.1e", v[k "_maxerr"]), fmt(v[k "_gflops"] / v["naive_ijk_gflops"], 1)
}
function dot_row(k, label, data, ops_per_insn, bytes_per_pair, base) {
  if (k "_skipped" in v) { printf "| %s | %s | skipped | | | | |\n", label, data; return }
  if (!((k "_opsns") in v)) return
  printf "| %s | %s | %s | %s %% | %s | %s | %s |\n", label, data, fmt(v[k "_opsns"], 1), fmt(v[k "_cv"], 1),
         fmt(v[k "_opsns"] * bytes_per_pair / 2, 1), fmt(v[k "_opsns"] / ops_per_insn / ghz, 2),
         (base in v) ? fmt(v[k "_opsns"] / v[base], 2) : "n/a"
}
$1 == "RESULT" { v[$2] = $3; next }
/^blas: / { blas = $2 " " $3; sub(/,$/, "", blas) }
END {
  printf "Clock estimate %s GHz (%s). SGEMM C = A B with M = N = K = %s in float32, %s bytes per matrix, %s flops per call, one thread unless stated; median GFLOP/s over %s timed calls per variant after a discarded warmup, sampled round-robin. Every variant is checked against the naive result (max abs error below 1e-2); the naive checksum, the sum of every entry of C, is %s.\n\n", ghz, clockhow, v["sgemm_dim"], v["sgemm_matrix_bytes"], v["sgemm_flops"], v["sgemm_reps"], v["sgemm_checksum"]
  print "| variant | median GFLOP/s | max GFLOP/s | median ms per call | cv | max abs error vs naive | ratio to naive |"
  print "|---|---|---|---|---|---|---|"
  gemm_row("naive_ijk", "naive i-j-k triple loop")
  gemm_row("ikj_autovec", "i-k-j, inner loop auto-vectorised")
  gemm_row("blocked_neon_8x8", "blocked, packed, 8x8 NEON register-tile microkernel")
  gemm_row("blas_threads_1", blas ", 1 thread")
  gemm_row("blas_threads_default", blas ", default threads")
  print ""
  print "| ceiling | value | unit |"
  print "|---|---|---|"
  if ("fmadd_latency_ns" in v) {
    printf "| scalar fmadd latency, dependent chain, min of %s runs | %s | ns |\n", v["sgemm_reps"], fmt(v["fmadd_latency_ns"], 3)
    printf "| naive chain bound, two flops per fmadd latency | %s | GFLOP/s |\n", fmt(2 / v["fmadd_latency_ns"], 2)
  }
  if ("neon_fma_peak_gflops" in v)
    printf "| NEON FMA peak, sixteen register chains, no memory (cv %s %%) | %s | GFLOP/s |\n", fmt(v["neon_fma_peak_cv"], 1), fmt(v["neon_fma_peak_gflops"], 2)
  bound = 32 * ghz
  printf "| NEON FMA bound, 32 flops per cycle at the clock estimate | %s | GFLOP/s |\n", fmt(bound, 2)
  print ""
  print "| derived | value | unit |"
  print "|---|---|---|"
  n = v["naive_ijk_gflops"]; ikj = v["ikj_autovec_gflops"]; blk = v["blocked_neon_8x8_gflops"]
  b1 = v["blas_threads_1_gflops"]; bd = v["blas_threads_default_gflops"]; pk = v["neon_fma_peak_gflops"]
  ighz = (pk > 0) ? pk / 32 : 0
  printf "| i-k-j / naive | %s | ratio |\n", fmt(ikj / n, 1)
  if (blk > 0) {
    printf "| blocked / i-k-j | %s | ratio |\n", fmt(blk / ikj, 2)
    printf "| blocked / naive | %s | ratio |\n", fmt(blk / n, 1)
  }
  if ("fmadd_latency_ns" in v) {
    printf "| fmadd latency in cycles at the clock estimate | %s | cycles |\n", fmt(v["fmadd_latency_ns"] * ghz, 1)
    printf "| naive as a fraction of the fmadd chain bound | %s | percent |\n", fmt(100 * n * v["fmadd_latency_ns"] / 2, 1)
  }
  if (blk > 0 && pk > 0) printf "| blocked as a fraction of the NEON FMA peak | %s | percent |\n", fmt(100 * blk / pk, 1)
  if (blk > 0) printf "| blocked as a fraction of the 32 flops per cycle bound | %s | percent |\n", fmt(100 * blk / bound, 1)
  if (ikj > 0 && pk > 0) printf "| i-k-j as a fraction of the NEON FMA peak | %s | percent |\n", fmt(100 * ikj / pk, 1)
  if (pk > 0) {
    printf "| NEON FMA peak as a fraction of the 32 flops per cycle bound | %s | percent |\n", fmt(100 * pk / bound, 1)
    printf "| clock implied by the NEON FMA peak at 32 flops per cycle | %s | GHz |\n", fmt(ighz, 2)
  }
  if (b1 > 0) {
    printf "| BLAS 1 thread / naive | %s | ratio |\n", fmt(b1 / n, 0)
    if (blk > 0) printf "| BLAS 1 thread / blocked | %s | ratio |\n", fmt(b1 / blk, 1)
    if (pk > 0) printf "| BLAS 1 thread / NEON FMA peak | %s | ratio |\n", fmt(b1 / pk, 1)
    printf "| BLAS 1 thread, CPU time over wall time | %s | ratio |\n", fmt(v["blas_threads_1_cpu_over_wall"], 2)
  }
  if (bd > 0 && b1 > 0) {
    printf "| BLAS default threads / BLAS 1 thread | %s | ratio |\n", fmt(bd / b1, 2)
    printf "| BLAS default threads, CPU time over wall time | %s | ratio |\n", fmt(v["blas_threads_default_cpu_over_wall"], 2)
  }
  print ""
  printf "Dot product, one thread: int8 with the NEON sdot instruction (sixteen multiply-adds per instruction, four int32 lanes) against float32 with fmla (four per instruction), eight accumulators each; a multiply and an add count as two ops. Streaming: %s elements, %s bytes for the int8 pair and %s bytes for the float32 pair, beyond the 16 MiB L2. L1-resident: %s elements, %s and %s bytes, %s passes per sample so each sample does the same multiply-adds as one streaming pass. Median over %s samples after a discarded warmup, sampled round-robin. The per-cycle column divides the sdot or fmla rate by the clock estimate and counts only those instructions, not the loads beside them.\n\n", v["dot_stream_elements"], v["dot_stream_bytes_int8"], v["dot_stream_bytes_f32"], v["dot_l1_elements"], v["dot_l1_bytes_int8"], v["dot_l1_bytes_f32"], v["dot_l1_passes"], v["dot_reps"]
  print "| kernel | data | median ops/ns | cv | bytes/ns | sdot or fmla per cycle at the clock estimate | ratio to f32 fmla |"
  print "|---|---|---|---|---|---|---|"
  dot_row("dot_int8_sdot_stream", "int8 sdot", "streaming", 32, 2, "dot_f32_fmla_stream_opsns")
  dot_row("dot_f32_fmla_stream", "f32 fmla", "streaming", 8, 8, "dot_f32_fmla_stream_opsns")
  dot_row("dot_int8_sdot_l1", "int8 sdot", "L1-resident", 32, 2, "dot_f32_fmla_l1_opsns")
  dot_row("dot_f32_fmla_l1", "f32 fmla", "L1-resident", 8, 8, "dot_f32_fmla_l1_opsns")
  if (ighz > 0 && ("dot_int8_sdot_l1_opsns" in v) && ("dot_f32_fmla_l1_opsns" in v)) {
    print ""
    print "| derived | value | unit |"
    print "|---|---|---|"
    printf "| L1-resident sdot per cycle at the clock implied by the NEON FMA peak | %s | per cycle |\n", fmt(v["dot_int8_sdot_l1_opsns"] / 32 / ighz, 2)
    printf "| L1-resident fmla per cycle at the clock implied by the NEON FMA peak | %s | per cycle |\n", fmt(v["dot_f32_fmla_l1_opsns"] / 8 / ighz, 2)
    printf "| L1-resident bytes per cycle at the clock implied by the NEON FMA peak, sdot and fmla | %s and %s | bytes |\n", fmt(v["dot_int8_sdot_l1_opsns"] / ighz, 1), fmt(v["dot_f32_fmla_l1_opsns"] * 4 / ighz, 1)
    print "| bound from sixteen 128-bit loads per eight instructions at three loads per cycle | 1.50 | per cycle |"
  }
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
