#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: 1<<22 records, 31 reps plus a warmup
#   QUICK=1 ./run.sh    smoke test: 1<<19 records, 5 reps, under ten seconds;
#                       writes results/quick-raw.txt and results/quick-summary.md
#                       and leaves README.md alone
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
  ../common/clock_estimate
} > "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} CLOCK_GHZ=$ghz" >> "$raw"
echo "---" >> "$raw"
# tee would hide a non-zero exit from bench, so its status goes through a
# file; a checksum mismatch must stop the run before the summary is built.
{ CLOCK_GHZ="$ghz" ./bench; echo $? > results/.bench-status; } | tee -a "$raw"
status=$(cat results/.bench-status)
rm -f results/.bench-status
if [ "$status" != 0 ]; then
  echo "bench exited with status $status; see $raw" >&2
  exit "$status"
fi

# Build the summary table from the RESULT lines. Names are
# <kernel>_<layout>_<variant>_<stat>; the stat is the last token and the
# rest is the row key. Derived rows use the medians and the bytes per
# record the program reports for each variant. The cycles column is the
# program's own median of per-pass conversions; the clock_estimate reading
# taken before the run is quoted beside the range of the per-variant
# median clocks so a reader can see whether the two agree.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function have(k) { return ((k, "median") in v) }
function ratio(a, b, what,   ka, kb) {
  ka = kern "_" a; kb = kern "_" b
  if (have(ka) && have(kb))
    printf "| %s %s / %s, %s | %s | ratio |\n", kern, a, b, what, fmt(v[ka, "median"] / v[kb, "median"], 2)
}
$1 == "RESULT" {
  name = $2; val = $3
  if (name == "records") { recs = val; next }
  if (name == "aos_bytes") { aosb = val; next }
  if (name == "soa_field_bytes") { soab = val; next }
  if (name == "evict_bytes") { evb = val; next }
  if (name == "reps" || name == "clock") next
  if (name == "sum3_reference") { ref3 = val; next }
  if (name == "dot37_reference") { ref37 = val; next }
  if (name == "tolerance") { tol = val; next }
  stat = name; sub(/.*_/, "", stat)
  k = name; sub(/_[a-z]+$/, "", k)
  if (!(k in seen)) { seen[k] = 1; order[++nk] = k }
  v[k, stat] = val
  if (stat == "ghz") {
    if (ghz_lo == "" || val + 0 < ghz_lo + 0) ghz_lo = val
    if (ghz_hi == "" || val + 0 > ghz_hi + 0) ghz_hi = val
  }
}
END {
  printf "%s records: the array of structs is %s bytes (64 per record) and each of the sixteen field arrays is %s bytes (4 per record); every timed pass starts after a %s-byte sweep that evicts the caches, so the data comes from memory. Median over the timed passes; effective GB/s is the bytes the kernel has to bring in per record (64 for the struct layout, 4 or 8 for the field arrays) divided by the median ns per record. Cycles per record: every pass is converted with the clock sampled just before it (min of three short dependent integer add chains), then the median of the conversions is taken; the per-variant median clock", recs, aosb, soab, evb
  if (ghz_lo == "") printf " was not sampled (no inline asm for this architecture, so the estimate before the run was used)"
  else if (ghz_lo == ghz_hi) printf " was %s GHz for every variant", ghz_lo
  else printf " ranged from %s to %s GHz", ghz_lo, ghz_hi
  printf ", and common/clock_estimate read %s GHz (%s) just before the run. Max rel err is the largest deviation of any pass from the double-precision reference (sum3 %s, dot37 %s; a deviation above %s fails the run).\n\n", ghz, clockhow, ref3, ref37, tol
  print "| kernel | variant | bytes/rec | median ns/rec | min ns/rec | cycles/rec | effective GB/s | cv | max rel err |"
  print "|---|---|---|---|---|---|---|---|---|"
  for (i = 1; i <= nk; i++) {
    k = order[i]
    kern = k; sub(/_.*/, "", kern)
    var = k; sub(/^[a-z0-9]+_/, "", var)
    printf "| %s | %s | %s | %s | %s | %s | %s | %.1f %% | %s |\n", kern, var, v[k, "bytes"], fmt(v[k, "median"], 4), fmt(v[k, "min"], 4), fmt(v[k, "cycles"], 3), fmt(v[k, "gbps"], 1), v[k, "cv"], v[k, "relerr"]
  }
  print ""
  print "| derived | value | unit |"
  print "|---|---|---|"
  n = split("sum3 dot37", ks, " ")
  for (i = 1; i <= n; i++) {
    kern = ks[i]
    if (have(kern "_aos_scalar") && have(kern "_soa_scalar"))
      printf "| %s bytes per record, aos / soa | %s | ratio |\n", kern, fmt(v[kern "_aos_scalar", "bytes"] / v[kern "_soa_scalar", "bytes"], 2)
    ratio("aos_scalar", "soa_scalar", "median ns/rec")
    ratio("aos_o2", "soa_o2", "median ns/rec")
    ratio("aos_o2", "soa_neon", "median ns/rec")
    ratio("aos_scalar", "aos_o2", "median ns/rec")
    ratio("soa_scalar", "soa_o2", "median ns/rec")
    ratio("soa_scalar", "soa_strict", "median ns/rec")
    ratio("soa_strict", "soa_o2", "median ns/rec")
    ratio("soa_o2", "soa_neon", "median ns/rec")
  }
  if (ghz_lo != "")
    printf "| clock, per-variant median of the per-pass samples, lowest / highest | %s / %s | GHz |\n", ghz_lo, ghz_hi
  printf "| clock, common/clock_estimate before the run | %s | GHz |\n", ghz
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
