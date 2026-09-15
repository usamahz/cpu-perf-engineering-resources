#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: 1 GiB buffer, 15 reps plus a warmup, about 6 seconds
#   QUICK=1 ./run.sh    smoke test: 64 MiB buffer, 5 reps, under ten seconds;
#                       writes results/quick-raw.txt and results/quick-summary.md
#                       and leaves README.md alone
#   REPS=n ./run.sh     override the repetition count
#   MIB=n ./run.sh      override the buffer size in MiB
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

# The state of the page queues just before the run, so the run is on record
# against it. On macOS "free" is small by design: the kernel keeps reclaimable
# file pages on the inactive and speculative queues rather than free.
if command -v vm_stat >/dev/null 2>&1; then
  vm_stat | awk '
    /page size of/ { gsub(/[^0-9]/, "", $8); ps = $8 }
    /^Pages free:/ { f = $3 } /^Pages active:/ { a = $3 } /^Pages inactive:/ { i = $3 }
    /^Pages speculative:/ { sp = $3 } /^Pages wired down:/ { w = $4 } /^Pages occupied by compressor:/ { c = $5 }
    END { gsub(/\./, "", f); gsub(/\./, "", a); gsub(/\./, "", i); gsub(/\./, "", sp); gsub(/\./, "", w); gsub(/\./, "", c)
          printf "memory before run (vm_stat, pages of %s bytes): free %s  active %s  inactive %s  speculative %s  wired %s  compressor %s\n", ps, f, a, i, sp, w, c }
  ' >> "$raw"
elif [ -r /proc/meminfo ]; then
  awk '/^MemFree:/ { f = $2 } /^MemAvailable:/ { av = $2 } /^Active:/ { a = $2 } /^Inactive:/ { i = $2 }
       END { printf "memory before run (/proc/meminfo, kB): free %s  available %s  active %s  inactive %s\n", f, av, a, i }' /proc/meminfo >> "$raw"
fi
echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default} MIB=${MIB:-default} CLOCK_GHZ=$ghz" >> "$raw"
echo "---" >> "$raw"

# A pipe into tee would return tee's status, not bench's, so run bench into
# a file and stop here on a failed checksum, a failed mmap or malloc, or a
# bad REPS rather than summarise partial output.
set +e
CLOCK_GHZ="$ghz" ./bench > "$raw.bench"
status=$?
set -e
tee -a "$raw" < "$raw.bench"
rm -f "$raw.bench"
if [ "$status" -ne 0 ]; then
  echo "bench exited with status $status; $raw holds its output, no summary written" >&2
  exit "$status"
fi
grep -q '^checksums: every pass wrote what the check read back' "$raw" || {
  echo "bench did not report clean checksums; no summary written" >&2
  exit 1
}

# Build the summary table from the RESULT lines. Every value quoted in the
# README comes from this table; the derived rows use the min for per-page
# costs and the median for bandwidths, as the program does.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
$1 == "RESULT" { v[$2] = $3; u[$2] = $4 }
/^page [0-9]+ bytes/ { reps = $0; sub(/.*reps /, "", reps); sub(/ .*/, "", reps) }
/^memory before run \(vm_stat/ { mem = $0; sub(/^memory before run \(vm_stat, pages of [0-9]+ bytes\): /, "", mem); memfree = $10; memhow = "vm_stat" }
/^memory before run \(\/proc\/meminfo/ { mem = $0; sub(/^memory before run \(\/proc\/meminfo, kB\): /, "", mem); memhow = "/proc/meminfo" }
END {
  pages = v["pages"]; bytes = v["buffer_bytes"]; page = v["page_bytes"]
  printf "Clock estimate %s GHz (%s). Page %s bytes; buffer %s bytes = %s pages; one thread on a P-core at default QoS. Per-page rows are the min over %s timed reps after one discarded warmup (latency-like); bandwidth rows are the median (throughput-like); cv is over the reps. Faults are from getrusage, the median over reps.", ghz, clockhow, page, bytes, pages, reps
  if (memhow == "vm_stat") printf " Page queues just before the run (%s, in pages): %s; that is %.0f MiB free against a %.0f MiB buffer, with the inactive and speculative queues reclaimable on demand.", memhow, mem, memfree * page / 1048576, bytes / 1048576
  else if (memhow != "") printf " Memory just before the run (%s): %s.", memhow, mem
  printf "\n\n"
  print "| step | min | median | cv | faults per rep | unit |"
  print "|---|---:|---:|---:|---:|---|"
  printf "| mmap call, whole buffer | %s | %s | %.1f %% | | us |\n", fmt(v["mmap_call_min"], 2), fmt(v["mmap_call_median"], 2), v["mmap_call_cv"]
  printf "| pass 1, first touch, one byte per page | %s | %s | %.1f %% | %s | ns/page |\n", fmt(v["pass1_min"], 1), fmt(v["pass1_median"], 1), v["pass1_cv"], v["pass1_faults_median"]
  printf "| pass 2, second touch, one byte per page | %s | %s | %.1f %% | %s | ns/page |\n", fmt(v["pass2_min"], 1), fmt(v["pass2_median"], 1), v["pass2_cv"], v["pass2_faults_median"]
  printf "| pass 2 repeated, third touch | %s | %s | %.1f %% | | ns/page |\n", fmt(v["pass2r_min"], 1), fmt(v["pass2r_median"], 1), v["pass2r_cv"]
  printf "| pass 3, full sequential write | %s | %s | %.1f %% | %s | GB/s |\n", fmt(v["pass3_min"], 1), fmt(v["pass3_median"], 1), v["pass3_cv"], v["pass3_faults_median"]
  printf "| fresh fill, full write of a new mapping | %s | %s | %.1f %% | %s | GB/s |\n", fmt(v["fresh_fill_min"], 1), fmt(v["fresh_fill_median"], 1), v["fresh_fill_cv"], v["fresh_fill_faults_median"]
  printf "| munmap, whole buffer | %s | %s | %.1f %% | | ns/page |\n", fmt(v["munmap_min"], 1), fmt(v["munmap_median"], 1), v["munmap_cv"]
  printf "| malloc call, after free of the same size | %s | %s | %.1f %% | | us |\n", fmt(v["malloc_call_min"], 2), fmt(v["malloc_call_median"], 2), v["malloc_call_cv"]
  printf "| malloc then first touch | %s | %s | %.1f %% | %s | ns/page |\n", fmt(v["malloc_touch_min"], 1), fmt(v["malloc_touch_median"], 1), v["malloc_touch_cv"], v["malloc_touch_faults_median"]
  printf "| malloc then second touch | %s | %s | %.1f %% | | ns/page |\n", fmt(v["malloc_retouch_min"], 1), fmt(v["malloc_retouch_median"], 1), v["malloc_retouch_cv"]
  printf "| free | %s | %s | %.1f %% | | ns/page |\n", fmt(v["free_min"], 2), fmt(v["free_median"], 2), v["free_cv"]
  print ""
  print "Per-page timing (cycle C, every store timed on its own with a 42 ns clock; a page above 200 ns did more than a page walk and a store):"
  print ""
  print "| timed pass | pages above 200 ns, median over reps | min | max | cost | unit |"
  print "|---|---:|---:|---:|---:|---|"
  printf "| pass 1, first touch | %.1f %% | | | %s | ns, median page, min over reps |\n", v["timed_pass1_slow_fraction"], fmt(v["timed_pass1_page_median_min"], 0)
  printf "| pass 2, second touch | %.1f %% | %.1f %% | %.1f %% | %s | ns, mean of the pages above 200 ns, min over reps |\n", v["timed_pass2_slow_fraction"], v["timed_pass2_slow_fraction_min"], v["timed_pass2_slow_fraction_max"], fmt(v["timed_pass2_slow_mean_min"], 0)
  print ""
  print "Where the slow pages of the timed pass 2 sit in the buffer, per rep (a run is a maximal stretch of neighbouring pages all above 200 ns):"
  print ""
  print "| timed pass 2, slow pages | median | min | max | unit |"
  print "|---|---:|---:|---:|---|"
  printf "| contiguous runs | %s | %s | %s | runs |\n", v["timed_pass2_slow_runs_median"], v["timed_pass2_slow_runs_min"], v["timed_pass2_slow_runs_max"]
  printf "| longest run | %s | %s | %s | pages |\n", v["timed_pass2_slow_longest_run_median"], v["timed_pass2_slow_longest_run_min"], v["timed_pass2_slow_longest_run_max"]
  print ""
  printf "Reps in which a fault-sized stall recurred on a pass with no counted faults (whole-pass average above %s ns per page, which is at least 2.5 %% of the pages at the pass 1 cost):\n", v["slow_rep_threshold"]
  print ""
  print "| pass | reps above the threshold | worst rep | unit |"
  print "|---|---:|---:|---|"
  printf "| pass 2, second touch | %s of %s | %s | ns/page |\n", v["pass2_slow_reps"], v["reps"], fmt(v["pass2_max"], 1)
  printf "| pass 2 repeated, third touch | %s of %s | %s | ns/page |\n", v["pass2r_slow_reps"], v["reps"], fmt(v["pass2r_max"], 1)
  printf "| malloc then first touch | %s of %s | %s | ns/page |\n", v["malloc_touch_slow_reps"], v["reps"], fmt(v["malloc_touch_max"], 1)
  printf "| malloc then second touch | %s of %s | %s | ns/page |\n", v["malloc_retouch_slow_reps"], v["reps"], fmt(v["malloc_retouch_max"], 1)
  print ""
  print "| derived | value | unit |"
  print "|---|---:|---|"
  printf "| pass 1 / pass 2 repeated (min ns per page; the headline ratio) | %s | ratio |\n", fmt(v["pass1_min"] / v["pass2r_min"], 1)
  printf "| pass 1 / malloc then first touch | %s | ratio |\n", fmt(v["pass1_min"] / v["malloc_touch_min"], 1)
  printf "| pass 1 / pass 2 (pass 2 carries the uncounted stalls described in the Analysis) | %s | ratio |\n", fmt(v["pass1_min"] / v["pass2_min"], 1)
  printf "| pass 3 / fresh fill (median GB/s) | %s | ratio |\n", fmt(v["pass3_median"] / v["fresh_fill_median"], 1)
  printf "| mmap call per page | %s | ns/page |\n", fmt(1000 * v["mmap_call_min"] / pages, 3)
  if ("pass1_cycles" in v) printf "| pass 1 per page at %s GHz | %s | cycles/page |\n", ghz, v["pass1_cycles"]
  printf "| pass 1 over the whole buffer (pages x min ns per page) | %s | ms |\n", fmt(pages * v["pass1_min"] / 1e6, 1)
  printf "| pass 3 over the whole buffer (bytes / median GB/s) | %s | ms |\n", fmt(bytes / v["pass3_median"] / 1e6, 1)
  printf "| fresh fill over the whole buffer (bytes / median GB/s) | %s | ms |\n", fmt(bytes / v["fresh_fill_median"] / 1e6, 1)
  printf "| faults on the first malloc of this size (warmup cycle) | %s | faults |\n", v["malloc_first_cycle_faults"]
  printf "| malloc returned the block it had just freed | %s of %s | cycles |\n", v["malloc_same_address_cycles"], v["malloc_cycles"]
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
