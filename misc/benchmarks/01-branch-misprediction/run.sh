#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: 1<<22 elements, 31 reps plus a warmup
#   QUICK=1 ./run.sh    smoke test: 1<<18 elements, 5 reps, under ten seconds;
#                       writes results/quick-raw.txt and results/quick-summary.md
#                       and leaves README.md alone
#   REPS=n ./run.sh     override the repetition count (at least 10 outside QUICK)
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

# REPS is for bench only: clock_estimate reads REPS too, so unset it there
# and let the estimate keep its own run count.
{
  ../common/machine.sh
  env -u REPS ../common/clock_estimate
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

# Build the summary table from the RESULT lines. Names are
# t<threshold>_<order>_<kernel>_<stat>; derived rows use the medians and the
# cycles derived from them. t<threshold>_cond_true is the fraction of
# elements for which a[i] >= t; the compiled branch skips the add, so it is
# taken for the rest, and the mispredict rate is the minority fraction.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function key(t, o, k) { return "t" t "_" o "_" k }
function minority(p) { return (p < 1 - p) ? p : 1 - p }
$1 == "RESULT" {
  name = $2; val = $3
  if (name ~ /^t[0-9]+_cond_true$/) { t = name; sub(/^t/, "", t); sub(/_cond_true$/, "", t); cond_true[t] = val; next }
  if (name ~ /_checksum$/) next
  if (name == "clock") next
  stat = name; sub(/.*_/, "", stat)
  k = name; sub(/_[a-z]+$/, "", k)
  if (!(k in seen)) { seen[k] = 1; order[++nk] = k }
  v[k, stat] = val
}
/^elements / { elems = $2; bytes = $6 }
END {
  printf "Clock estimate %s GHz (%s). Array of %s 32-bit elements, %s bytes. Median over the timed passes; cycles per element is median ns per element times the clock estimate.\n\n", ghz, clockhow, elems, bytes
  print "| variant | condition true | median ns/elem | min ns/elem | cycles/elem | cv |"
  print "|---|---|---|---|---|---|"
  for (i = 1; i <= nk; i++) {
    k = order[i]
    t = k; sub(/^t/, "", t); sub(/_.*/, "", t)
    label = k; gsub(/_/, " ", label)
    printf "| %s | %.1f %% | %s | %s | %s | %.1f %% |\n", label, 100 * cond_true[t], fmt(v[k, "median"], 3), fmt(v[k, "min"], 3), fmt(v[k, "cycles"], 2), v[k, "cv"]
  }
  print ""
  print "| derived | value | unit |"
  print "|---|---|---|"
  n = split("128 243", ts, " ")
  for (i = 1; i <= n; i++) {
    t = ts[i]
    ub = key(t, "unsorted", "branchy"); sb = key(t, "sorted", "branchy")
    ul = key(t, "unsorted", "branchless"); uv = key(t, "unsorted", "vector")
    printf "| t%s branchy unsorted / branchy sorted | %s | ratio |\n", t, fmt(v[ub, "median"] / v[sb, "median"], 2)
    printf "| t%s branchy unsorted / branchless unsorted | %s | ratio |\n", t, fmt(v[ub, "median"] / v[ul, "median"], 2)
    printf "| t%s branchy sorted / branchless sorted | %s | ratio |\n", t, fmt(v[sb, "median"] / v[key(t, "sorted", "branchless"), "median"], 2)
    printf "| t%s branchless unsorted / vector unsorted | %s | ratio |\n", t, fmt(v[ul, "median"] / v[uv, "median"], 2)
    printf "| t%s extra cycles per element, branchy unsorted minus branchy sorted | %s | cycles |\n", t, fmt(v[ub, "cycles"] - v[sb, "cycles"], 2)
    printf "| t%s implied cost per mispredict (extra cycles / %.4f mispredict rate) | %s | cycles |\n", t, minority(cond_true[t]), fmt((v[ub, "cycles"] - v[sb, "cycles"]) / minority(cond_true[t]), 1)
  }
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
