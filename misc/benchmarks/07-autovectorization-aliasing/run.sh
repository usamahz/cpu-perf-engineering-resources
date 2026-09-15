#!/bin/sh
# Build, describe the machine, estimate the clock, run the three builds of
# the benchmark plus a PROBE=1 pass of the primary build, and write
# results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: 1<<22 and 1<<13 floats per array, 31 reps
#                       plus a warmup, three builds and the probe pass
#   QUICK=1 ./run.sh    smoke test: 1<<18 and 1<<13 floats, 11 reps, under
#                       ten seconds; prints the summary and leaves results/
#                       and README.md alone
#   REPS=n ./run.sh     override the repetition count
set -eu
cd "$(dirname "$0")"

./build.sh

if [ ! -x ../common/clock_estimate ]; then
  echo "cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c"
  cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c
fi

if [ "${QUICK:-0}" != 0 ]; then
  # Nothing from a smoke test belongs in results/, which is committed.
  tmp=$(mktemp -d)
  raw=$tmp/quick-raw.txt
  summary=$tmp/quick-summary.md
else
  tmp=
  mkdir -p results
  raw=results/raw.txt
  summary=results/summary.md
fi
out=$(mktemp)
cleanup() { rm -f "$out"; if [ -n "$tmp" ]; then rm -rf "$tmp"; fi; }
trap cleanup EXIT

{
  ../common/machine.sh
  ../common/clock_estimate
} > "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} CLOCK_GHZ=$ghz" >> "$raw"

# The primary build first, so it is the first column and the one the
# derived rows compare the others against. The binary's exit status must
# stop the run (a piped tee would hide it), so its output goes through a
# file.
run_bench() {
  desc=$1; shift
  echo "--- $desc" | tee -a "$raw"
  if ! "$@" > "$out"; then
    cat "$out" | tee -a "$raw"
    echo "$desc exited non-zero; its output is above" >&2
    exit 1
  fi
  cat "$out" | tee -a "$raw"
}
for b in O2 O2nu O3; do run_bench "bench_$b" env CLOCK_GHZ="$ghz" "./bench_$b"; done
# The same primary binary with a clock probe before every timed pass. It
# reads the clock the passes run at and converts each pass to cycles, and
# it shows the scalar loop at one element per cycle; it is a separate pass
# because the probe itself changes the clock the passes run at.
run_bench "bench_O2 PROBE=1" env CLOCK_GHZ="$ghz" PROBE=1 ./bench_O2

# Build the summary table from the RESULT lines. Names are
# <build>_n<elements>_<variant>_<stat>, where variant may itself contain an
# underscore (plain_inplace); the stat is the last token. One table per
# size with a column pair per build (plus cycles for the probe pass), then
# the paired ratios per size with a column pair per build, then the clock
# and cycles rows.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function have(b, n, var, stat) { return ((b SUBSEP n SUBSEP var SUBSEP stat) in v) }
function val(b, n, var, stat) { return v[b SUBSEP n SUBSEP var SUBSEP stat] }
function med(b, n, var) { return val(b, n, var, "median") }
function cyc(b, n, var) { return val(b, n, var, "cycles") }
# One row of the ratio table: the paired per-round median of var over its
# base (plain, or plain in place for pragma in place), with its cv.
function row(n, var, stat, label,    i, b) {
  printf "| %s", label
  for (i = 1; i <= nb; i++) {
    b = border[i]
    printf " | %s | %s %%", fmt(val(b, n, var, stat), 2), fmt(val(b, n, var, stat "cv"), 1)
  }
  print " |"
}
/^build: / {
  b = $2; flags = $0; sub(/^build: [A-Za-z0-9]+ = /, "", flags)
  if (!(b in bseen)) { bseen[b] = 1; border[++nb] = b; bflags[b] = flags }
}
/^size: n / {
  n = "n" $3
  if (!(n in nseen)) { nseen[n] = 1; norder[++nn] = n; per[n] = $5; three[n] = $9; calls[n] = $15 }
}
$1 == "RESULT" {
  name = $2; x = $3
  if (name == "clock" || name ~ /_checksum$/) next
  k = split(name, t, "_")
  b = t[1]; n = t[2]; stat = t[k]
  var = t[3]; for (i = 4; i < k; i++) var = var "_" t[i]
  if (var != "clock" && !((n SUBSEP var) in vseen)) { vseen[n SUBSEP var] = 1; vorder[n, ++nv[n]] = var }
  v[b SUBSEP n SUBSEP var SUBSEP stat] = x
  if (stat == "cycles") hascyc[b] = 1
}
END {
  p = border[1]; small = norder[nn]; large = norder[1]; probeb = ""
  for (i = 1; i <= nb; i++) if (have(border[i], small, "clock", "median")) probeb = border[i]
  printf "Clock estimate %s GHz before the run (%s). Median of nanoseconds per element over the timed samples with cv; minima are in `results/raw.txt`. Ratios are paired: each variant over the plain kernel (or plain in place over pragma in place) in the same pass, then the median of those with its cv. Builds:", ghz, clockhow
  for (i = 1; i <= nb; i++) printf "%s %s = `%s`", (i > 1 ? "," : ""), border[i], bflags[border[i]]
  printf "."
  if (probeb != "") printf " %s is the %s binary run with PROBE=1: a clock probe (three dependent add chains of 32000 adds, min of three) before every timed pass, which reads the clock the passes run at and converts each pass to cycles with its own reading (the cycles/elem column is the median of those conversions). The probe changes that clock, so its column stands beside the default run rather than in place of it.", probeb, p
  print "\n"
  for (j = 1; j <= nn; j++) {
    n = norder[j]; el = n; sub(/^n/, "", el)
    printf "%s floats per array: %s bytes per array, %s bytes for the three arrays, %s call%s per timed sample.\n\n", el, per[n], three[n], calls[n], (calls[n] == 1 ? "" : "s")
    printf "| variant"
    for (i = 1; i <= nb; i++) printf " | %s ns/elem | cv%s", border[i], (border[i] in hascyc) ? " | cycles/elem" : ""
    print " |"
    printf "|---"
    for (i = 1; i <= nb; i++) printf "|---|---%s", (border[i] in hascyc) ? "|---" : ""
    print "|"
    for (m = 1; m <= nv[n]; m++) {
      var = vorder[n, m]; label = var; gsub(/_inplace/, " in place", label)
      printf "| %s", label
      for (i = 1; i <= nb; i++) {
        b = border[i]
        printf " | %s | %s %%", fmt(med(b, n, var), 4), fmt(val(b, n, var, "cv"), 1)
        if (b in hascyc) printf " | %s", fmt(cyc(b, n, var), 3)
      }
      print " |"
    }
    print ""
    printf "| ratio, %s floats", el
    for (i = 1; i <= nb; i++) printf " | %s | cv", border[i]
    print " |"
    printf "|---"
    for (i = 1; i <= nb; i++) printf "|---|---"
    print "|"
    row(n, "scalar", "vsplain", "scalar / plain")
    row(n, "plain_inplace", "vsplain", "plain in place / plain")
    row(n, "pragma_inplace", "vspragmainplace", "plain in place / pragma in place")
    row(n, "restrict", "vsplain", "restrict / plain")
    row(n, "pragma", "vsplain", "pragma / plain")
    row(n, "stride", "vsplain", "stride / plain")
    row(n, "last", "vsplain", "last / plain")
    print ""
  }
  # Clock and cycles. The scalar loop is one element per cycle (the PROBE=1
  # column shows it), so in the default run its L1 median is the cycle time
  # of the median L1 pass, and the L1 cycles figure is derived from it.
  print "| derived | value | unit |"
  print "|---|---|---|"
  printf "| clock estimate before the run (%s) | %s | GHz |\n", clockhow, ghz
  for (i = 1; i <= nb; i++) {
    b = border[i]
    if (have(b, small, "clock", "median")) continue
    printf "| %s clock of the median scalar pass in L1, 1 / (%s scalar median) | %s | GHz |\n", b, small, fmt(1 / med(b, small, "scalar"), 2)
  }
  if (probeb != "")
    for (j = 1; j <= nn; j++) {
      n = norder[j]
      printf "| %s %s clock before each timed pass, median (min, max) | %s (%s, %s) | GHz |\n", n, probeb, med(probeb, n, "clock"), val(probeb, n, "clock", "min"), val(probeb, n, "clock", "max")
    }
  pg = 1 / med(p, small, "scalar")
  for (j = 1; j <= nn; j++) {
    n = norder[j]
    printf "| %s %s scalar cycles per element at the clock estimate | %s | cycles |\n", n, p, fmt(med(p, n, "scalar") * ghz, 3)
    if (n == small)
      printf "| %s %s plain cycles per element, scalar loop as the cycle | %s | cycles |\n", n, p, fmt(med(p, n, "plain") * pg, 3)
    printf "| %s %s plain, 12 bytes per element | %s | GB/s |\n", n, p, fmt(12 / med(p, n, "plain"), 1)
    printf "| %s %s scalar, 12 bytes per element | %s | GB/s |\n", n, p, fmt(12 / med(p, n, "scalar"), 1)
  }
  # From memory the vector loop is bound by memory and the scalar loop by
  # its issue rate, so their ratio moves with the clock: the measured ratio
  # is at the clock the run held, and this row is what one element per
  # cycle at the clock estimate would give.
  printf "| %s %s scalar / plain if the scalar loop had held the clock estimate | %s | ratio |\n", large, p, fmt((1 / ghz) / med(p, large, "plain"), 2)
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: results/ and README.md not touched; the summary follows"
  echo
  cat "$summary"
else
  python3 ../common/refresh_readme.py .
fi
