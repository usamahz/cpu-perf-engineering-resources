#!/bin/sh
# Build, describe the machine and the load it is under, estimate the clock,
# run the benchmark, and write results/raw.txt and results/summary.md; then
# paste the summary into README.md between the results markers.
#
#   ./run.sh              full run: 3 x 512 MiB arrays, 21 timed rounds per configuration
#                         after at least 100 ms and 3 discarded warmup rounds
#   QUICK=1 ./run.sh      smoke test: 3 x 64 MiB arrays, 5 timed rounds, under ten seconds;
#                         writes results/quick-raw.txt and results/quick-summary.md
#                         and leaves README.md alone
#   REPS=n ./run.sh       override the timed round count
#   WARMUP_MS=m ./run.sh  override the warmup time per configuration
#   ARRAY_MIB=m ./run.sh  override the large array size in MiB per array
#   VENDOR_GBS=x ./run.sh the vendor figure to divide by (default 273, Apple's M4 Pro number)
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

# What the run competes with. A bandwidth benchmark that shares the machine
# with a browser or an active memory compressor reads lower and noisier, and
# the summary quotes these lines beside the results so a reader can tell a
# quiet run from a loaded one.
load_lines() {
  if command -v sysctl >/dev/null 2>&1 && sysctl -n vm.loadavg >/dev/null 2>&1; then
    echo "load average: $(sysctl -n vm.loadavg | tr -d '{}' | sed 's/^ *//; s/ *$//')"
    echo "swap: $(sysctl -n vm.swapusage)"
    vm_stat | awk 'NR == 1 { match($0, /[0-9]+ bytes/); ps = substr($0, RSTART, RLENGTH) + 0 }
      /^Pages free:/ { f = $3 } /^Pages occupied by compressor:/ { c = $5 }
      END { gsub(/\./, "", f); gsub(/\./, "", c); printf "memory: free %d MB  compressor %d MB\n", f * ps / 1e6, c * ps / 1e6 }'
  elif [ -r /proc/loadavg ]; then
    echo "load average: $(cut -d' ' -f1-3 /proc/loadavg)"
    echo "swap: $(awk '/^SwapTotal:/ {t=$2} /^SwapFree:/ {f=$2} END {printf "total = %dM  used = %dM  free = %dM", t/1024, (t-f)/1024, f/1024}' /proc/meminfo)"
    echo "memory: $(awk '/^MemAvailable:/ {printf "available %d MB", $2 * 1024 / 1e6}' /proc/meminfo)"
  else
    echo "load average: not available on this platform"
  fi
}

{
  ../common/machine.sh
  load_lines
  ../common/clock_estimate
} > "$raw"
ghz=$(awk '/^estimated clock:/ {print $3}' "$raw")
clockhow=$(sed -n 's/^estimated clock: [0-9.]* GHz (\(.*\))$/\1/p' "$raw")
loadavg=$(sed -n 's/^load average: //p' "$raw")
swap=$(sed -n 's/^swap: //p' "$raw")
mem=$(sed -n 's/^memory: //p' "$raw")
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} WARMUP_MS=${WARMUP_MS:-default} ARRAY_MIB=${ARRAY_MIB:-default} VENDOR_GBS=${VENDOR_GBS:-default} CLOCK_GHZ=$ghz" >> "$raw"
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
# triad_<size>MiB_t<threads>[_again]_<stat>; rows keep the order the program
# printed them, and the derived rows use the medians. The _again key is the
# single-thread cache configuration run a second time at the end of the sweep.
awk -v ghz="$ghz" -v clockhow="$clockhow" -v loadavg="$loadavg" -v swap="$swap" -v mem="$mem" '
function fmt(x, d) { return sprintf("%." d "f", x) }
$1 == "RESULT" {
  name = $2; val = $3
  if (name == "array_bytes") { bytes = val; big = val / 1048576; next }
  if (name == "cache_array_bytes") { cbytes = val; small = val / 1048576; next }
  if (name == "cache_passes") { passes = val; next }
  if (name == "bytes_per_element") { bpe = val; next }
  if (name == "reps") { reps = val; next }
  if (name == "warmup_ms") { warm = val; next }
  if (name == "warmup_min_rounds") { warmrounds = val; next }
  if (name == "late_percent") { late = val; next }
  if (name == "online_cpus") { ncpu = val; next }
  if (name == "vendor_bandwidth") { vendor = val; next }
  if (name == "clock") next
  if (name !~ /^triad_/) next
  stat = name; sub(/^triad_[0-9]+MiB_t[0-9]+(_again)?_/, "", stat)
  k = name; sub("_" stat "$", "", k)
  if (!(k in seen)) { seen[k] = 1; order[++nk] = k }
  v[k, stat] = val
}
END {
  printf "Clock estimate %s GHz (%s). STREAM triad a[i] = b[i] + s*c[i] over three float64 arrays. Large case: %d bytes (%d MiB) per array, split into one contiguous partition per thread and streamed once per round. Cache case: the first %d bytes (%d MiB) of each array per thread, %d MiB per thread in all, repeated for %d passes per round, so a thread does the same work per round in both cases. %d bytes counted per element, decimal GB/s. Median over %d timed rounds per configuration, after discarded warmup rounds covering at least %d ms and numbering at least %d; the best round is what STREAM would report. Fastest and slowest thread are the medians of each round\047s quickest and slowest partition; scaling is against one thread over the same working set. A round has a late thread when its span exceeds the slowest thread\047s own time by more than %d percent, which means some thread was not running while the others were. The single-thread cache configuration runs first and again last. Vendor figure: %d GB/s. Load at the start of this run: load average %s; swap %s; %s.\n\n", ghz, clockhow, bytes, big, cbytes, small, 3 * small, passes, bpe, reps, warm, warmrounds, late, vendor, loadavg, swap, mem
  print "| per array | threads | median GB/s | best GB/s | cv | fraction of " vendor " GB/s | scaling vs 1 thread | fastest thread GB/s | slowest thread GB/s | rounds with a late thread |"
  print "|---|---|---|---|---|---|---|---|---|---|"
  bestc = 0; bestck = ""; bestb = 0; bestbk = ""
  for (i = 1; i <= nk; i++) {
    k = order[i]
    sz = k; sub(/^triad_/, "", sz); sub(/_t[0-9]+(_again)?$/, "", sz); sub(/MiB$/, "", sz)
    t = k; sub(/^triad_[0-9]+MiB_t/, "", t); again = sub(/_again$/, "", t)
    cache = (sz + 0 == small + 0)
    where = cache ? " per thread (L2)" : ""
    if (again) where = where ", run again last"
    scaling = ((k, "scaling") in v) ? fmt(v[k, "scaling"], 2) : "-"
    printf "| %s MiB%s | %s | %s | %s | %.1f %% | %s | %s | %s | %s | %d |\n", sz, where, t, fmt(v[k, "median"], 1), fmt(v[k, "best"], 1), v[k, "cv"], fmt(v[k, "fraction"], 3), scaling, fmt(v[k, "fastest_thread"], 1), fmt(v[k, "slowest_thread"], 1), v[k, "late_rounds"]
    if (again) continue
    if (cache && v[k, "median"] > bestc) { bestc = v[k, "median"]; bestck = t }
    if (!cache && v[k, "median"] > bestb) { bestb = v[k, "median"]; bestbk = t }
  }
  print ""
  print "| derived | value | unit |"
  print "|---|---|---|"
  c1 = "triad_" small "MiB_t1"; c1a = c1 "_again"; b1 = "triad_" big "MiB_t1"
  b10 = "triad_" big "MiB_t10"; b14 = "triad_" big "MiB_t14"
  printf "| highest %d MiB per thread median (%s threads) / vendor figure | %s | ratio |\n", small, bestck, fmt(bestc / vendor, 2)
  printf "| highest %d MiB per thread median (%s threads) / highest %d MiB median (%s threads) | %s | ratio |\n", small, bestck, big, bestbk, fmt(bestc / bestb, 2)
  if ((c1 in seen) && (c1a in seen)) printf "| %d MiB per thread, 1 thread, run again last / run first | %s | ratio |\n", small, fmt(v[c1a, "median"] / v[c1, "median"], 2)
  printf "| vendor figure / %d MiB single thread | %s | ratio |\n", big, fmt(vendor / v[b1, "median"], 2)
  if (b10 in seen) printf "| %d MiB 10 threads / 1 thread | %s | ratio |\n", big, fmt(v[b10, "median"] / v[b1, "median"], 2)
  if ((b14 in seen) && (b10 in seen)) printf "| %d MiB 14 threads / 10 threads | %s | ratio |\n", big, fmt(v[b14, "median"] / v[b10, "median"], 2)
  if (b14 in seen) printf "| %d MiB 14 threads, fastest thread / slowest thread | %s | ratio |\n", big, fmt(v[b14, "fastest_thread"] / v[b14, "slowest_thread"], 2)
  printf "| highest %d MiB median (%s threads) / vendor figure | %s | fraction |\n", big, bestbk, fmt(bestb / vendor, 3)
  printf "| highest %d MiB median (%s threads) / single thread | %s | ratio |\n", big, bestbk, fmt(bestb / v[b1, "median"], 2)
  printf "| highest %d MiB median (%s threads) x 32/24, the interface traffic if the store read a before writing it | %s | GB/s |\n", big, bestbk, fmt(bestb * 32 / 24, 1)
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
