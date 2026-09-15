#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh              full run: 1<<24 increments per thread, 11 rounds plus a warmup
#   QUICK=1 ./run.sh      smoke test: 1<<20 increments, 5 rounds, under ten seconds;
#                         writes results/quick-raw.txt and results/quick-summary.md
#                         and leaves README.md alone
#   REPS=n ./run.sh       override the round count
#   ITERS_LOG2=27 ./run.sh  1<<27 increments per thread (several minutes)
set -eu
cd "$(dirname "$0")"

./build.sh

CC=${CC:-cc}
if [ ! -x ../common/clock_estimate ]; then
  echo "$CC -O2 -o ../common/clock_estimate ../common/clock_estimate.c"
  $CC -O2 -o ../common/clock_estimate ../common/clock_estimate.c
fi

mkdir -p results
if [ "${QUICK:-0}" != 0 ]; then
  raw=results/quick-raw.txt
  summary=results/quick-summary.md
else
  raw=results/raw.txt
  summary=results/summary.md
fi

# REPS is for bench; clock_estimate reads REPS too, so blank it there.
../common/machine.sh > "$raw"
REPS= ../common/clock_estimate >> "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")

# The all-core clock sits below the single-core one under DVFS, so the
# cycles column of a T-thread row must use the clock T busy cores run at:
# run T copies of clock_estimate at once, record every copy's reading, and
# take the median across copies as CLOCK_GHZ_T<T>. The 1-thread rows use
# the single-core estimate above.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
clocks=" CLOCK_GHZ_T1=$ghz"
for T in 2 4 8; do
  i=0
  while [ "$i" -lt "$T" ]; do
    REPS= ../common/clock_estimate > "$tmp/clock_$T.$i" &
    i=$((i + 1))
  done
  wait
  cat "$tmp"/clock_"$T".* | awk '/^estimated clock:/ {print $3}' | sort -n > "$tmp/clock_$T.all"
  g=$(awk '{a[NR] = $1} END {printf "%.2f\n", (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2}' "$tmp/clock_$T.all")
  readings=$(tr '\n' ' ' < "$tmp/clock_$T.all" | sed 's/ $//')
  printf "clock with %d concurrent copies of clock_estimate: median %s GHz (each copy: %s)\n" "$T" "$g" "$readings" >> "$raw"
  clocks="$clocks CLOCK_GHZ_T$T=$g"
done
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} ITERS_LOG2=${ITERS_LOG2:-default} CLOCK_GHZ=$ghz$clocks" >> "$raw"
echo "---" >> "$raw"
# A pipe into tee returns tee's status, so bench writes to a file first and
# a non-zero exit stops the run before any summary or README is written.
if env CLOCK_GHZ="$ghz" $clocks ./bench > "$raw.bench" 2>&1; then
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
else
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
  echo "bench exited with an error; see $raw" >&2; exit 1
fi

# Build the summary table from the RESULT lines. Names are
# <increment>_<layout>_t<threads>_<stat> (the register variant has no
# layout); derived tables use the medians.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
function ratio(a, b) { return (b > 0) ? fmt(a / b, 2) : "n/a" }
$1 == "RESULT" {
  name = $2; val = $3
  if (name == "iters") { iters = val; next }
  if (name == "reps") { reps = val; next }
  if (name == "line_compiled") { line = val; next }
  if (name == "line_reported") { line_os = val; next }
  if (name == "clock") next
  if (name ~ /^clock_t[0-9]+$/) { t = name; sub(/^clock_t/, "", t); clock_t[t] = val; next }
  n = split(name, part, "_")
  stat = part[n]
  t = part[n - 1]; sub(/^t/, "", t)
  variant = part[1]
  for (i = 2; i <= n - 2; i++) variant = variant "_" part[i]
  if (!((t, variant) in seen)) { seen[t, variant] = 1; order[++nk] = t SUBSEP variant }
  if (!(t in tseen)) { tseen[t] = 1; tlist[++nt] = t }
  v[variant, t, stat] = val
}
END {
  stride["adjacent"] = 8; stride["pad64"] = 64; stride["pad128"] = 128
  clocks = ""
  for (i = 1; i <= nt; i++) {
    t = tlist[i]
    clocks = clocks (i > 1 ? ", " : "") t " thread" (t > 1 ? "s " : " ") clock_t[t] " GHz"
  }
  linetext = (line_os > 0) ? "the OS reports a " line_os "-byte line" : "the OS does not report a line size on this platform"
  printf "Single-core clock estimate %s GHz (%s). The cycles column of a row uses the clock measured with as many concurrent copies of clock_estimate as the row has threads, because the all-core clock sits below the single-core one: %s. %s increments per thread, %s timed rounds after one warmup round, wall time from a barrier after every thread has started until the last join. Median of nanoseconds per increment per thread; cycles is the median times the clock for that thread count. Compiled-in stride for pad128: %s bytes; %s.\n\n", ghz, clockhow, clocks, iters, reps, line, linetext
  print "| threads | increment | layout | stride | median ns/increment/thread | min ns/increment/thread | cycles/increment | cv |"
  print "|---|---|---|---|---|---|---|---|"
  for (i = 1; i <= nk; i++) {
    split(order[i], kv, SUBSEP); t = kv[1]; variant = kv[2]
    if (variant == "register") { inc = "register"; lay = "one store at the end"; st = "128 B" }
    else { split(variant, p, "_"); inc = p[1]; lay = p[2]; st = stride[lay] " B" }
    printf "| %s | %s | %s | %s | %s | %s | %s | %s %% |\n", t, inc, lay, st, fmt(v[variant, t, "median"], 3), fmt(v[variant, t, "min"], 3), fmt(v[variant, t, "cycles"], 2), v[variant, t, "cv"]
  }
  print ""
  print "Layout ratios from the medians. A ratio of 1.00 means the layout made no difference."
  print ""
  print "| threads | store adjacent / pad128 | store pad64 / pad128 | atomic adjacent / pad128 | atomic pad64 / pad128 |"
  print "|---|---|---|---|---|"
  for (i = 1; i <= nt; i++) {
    t = tlist[i]
    printf "| %s | %s | %s | %s | %s |\n", t,
      ratio(v["store_adjacent", t, "median"], v["store_pad128", t, "median"]),
      ratio(v["store_pad64", t, "median"], v["store_pad128", t, "median"]),
      ratio(v["atomic_adjacent", t, "median"], v["atomic_pad128", t, "median"]),
      ratio(v["atomic_pad64", t, "median"], v["atomic_pad128", t, "median"])
  }
  print ""
  print "Per-thread cost relative to the same variant on one thread, from the medians. 1.00 is linear scaling: T threads do T times the work in the same wall time."
  print ""
  print "| threads | store adjacent | store pad128 | atomic adjacent | atomic pad128 | register |"
  print "|---|---|---|---|---|---|"
  for (i = 1; i <= nt; i++) {
    t = tlist[i]
    printf "| %s | %s | %s | %s | %s | %s |\n", t,
      ratio(v["store_adjacent", t, "median"], v["store_adjacent", 1, "median"]),
      ratio(v["store_pad128", t, "median"], v["store_pad128", 1, "median"]),
      ratio(v["atomic_adjacent", t, "median"], v["atomic_adjacent", 1, "median"]),
      ratio(v["atomic_pad128", t, "median"], v["atomic_pad128", 1, "median"]),
      ratio(v["register", t, "median"], v["register", 1, "median"])
  }
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
