#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: 20 simulated seconds per client, 31 reps plus a warmup
#   QUICK=1 ./run.sh    smoke test: 5 simulated seconds, 5 reps, under ten seconds;
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

# REPS is for bench only: clock_estimate reads REPS too, so unset it there
# and keep its minimum-of-7-runs default that the README quotes.
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
if ./bench > "$raw.bench" 2>&1; then
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
else
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
  echo "bench exited with an error; see $raw" >&2; exit 1
fi

# Build the summary table from the RESULT lines. Latency names are
# <client>_<stat>_<min|median|cv>; counts and shares are
# <client>_<count|slow>_<median|cv>; the simulation cost is
# <client>_sim_<min|median|cv>; the rest are single values.
awk -v ghz="$ghz" -v clockhow="$clockhow" '
function fmt(x, d) { return sprintf("%." d "f", x) }
$1 == "PARAM" { p[$2] = $3; next }
$1 == "RESULT" { v[$2] = $3; next }
END {
  split("closed open corrected", cl, " ")
  split("p50 p90 p99 p999 max", st, " ")
  split("p50 p90 p99 p99.9 max", sl, " ")
  printf "Virtual clock: %s s simulated per client per rep. Both clients follow one schedule of %s requests/s (one due every %s us) against a server with a %s us service time and a %s ms stall once per second (%s per run), so the server timeline is identical and only the recording differs; the corrected column is HdrHistogram'\''s correction of the closed-loop samples at the expected interval of %s us. Percentiles use HdrHistogram'\''s rank convention (the sample at rank round(p/100 times n), at least 1), in simulated us, minimum over the %s timed reps after a discarded warmup; the simulation is deterministic, so cv over reps is 0 by construction. Every percentile and share below, the latency sums behind them, the corrected sample count, and both loops'\'' stall counts and last completion times equalled the closed form derived in bench.c (%s checks). The clock estimate, %s GHz (%s), converts the wall cost of the simulation itself to cycles in the last table.\n\n", p["sim_seconds"], p["rate_per_s"], p["interval_us"], p["service_us"], p["stall_ms"], v["stalls_per_run"], p["interval_us"], p["reps"], v["checks_passed"], ghz, clockhow
  print "| statistic | closed loop, from actual send | open loop, from intended send | closed loop, corrected | cv over reps |"
  print "|---|---|---|---|---|"
  for (i = 1; i <= 5; i++) {
    cv = 0
    for (j = 1; j <= 3; j++) if (v[cl[j] "_" st[i] "_cv"] > cv) cv = v[cl[j] "_" st[i] "_cv"]
    printf "| %s (us) | %s | %s | %s | %.1f %% |\n", sl[i], fmt(v["closed_" st[i] "_min"], 1), fmt(v["open_" st[i] "_min"], 1), fmt(v["corrected_" st[i] "_min"], 1), cv
  }
  cv = 0
  for (j = 1; j <= 3; j++) if (v[cl[j] "_count_cv"] > cv) cv = v[cl[j] "_count_cv"]
  printf "| samples per run | %s | %s | %s | %.1f %% |\n", fmt(v["closed_count_median"], 0), fmt(v["open_count_median"], 0), fmt(v["corrected_count_median"], 0), cv
  cv = 0
  for (j = 1; j <= 3; j++) if (v[cl[j] "_slow_cv"] > cv) cv = v[cl[j] "_slow_cv"]
  printf "| share of samples above %s ms (%%) | %s | %s | %s | %.1f %% |\n", p["slow_ms"], fmt(v["closed_slow_median"], 4), fmt(v["open_slow_median"], 4), fmt(v["corrected_slow_median"], 4), cv
  print ""
  print "| derived | value | unit |"
  print "|---|---|---|"
  printf "| open loop p99 / closed loop p99 | %s | ratio |\n", fmt(v["open_p99_min"] / v["closed_p99_min"], 1)
  printf "| open loop p99.9 / closed loop p99.9 | %s | ratio |\n", fmt(v["open_p999_min"] / v["closed_p999_min"], 1)
  printf "| open loop p90 / closed loop p90 | %s | ratio |\n", fmt(v["open_p90_min"] / v["closed_p90_min"], 1)
  printf "| open loop p50 / closed loop p50 | %s | ratio |\n", fmt(v["open_p50_min"] / v["closed_p50_min"], 2)
  printf "| open loop max / closed loop max | %s | ratio |\n", fmt(v["open_max_min"] / v["closed_max_min"], 2)
  printf "| corrected p99 / closed loop p99 | %s | ratio |\n", fmt(v["corrected_p99_min"] / v["closed_p99_min"], 1)
  printf "| corrected p99.9 / closed loop p99.9 | %s | ratio |\n", fmt(v["corrected_p999_min"] / v["closed_p999_min"], 1)
  printf "| open loop p99 / corrected p99 | %s | ratio |\n", fmt(v["open_p99_min"] / v["corrected_p99_min"], 3)
  printf "| open loop p90 / corrected p90 | %s | ratio |\n", fmt(v["open_p90_min"] / v["corrected_p90_min"], 3)
  printf "| closed loop max / closed loop p99 | %s | ratio |\n", fmt(v["closed_max_min"] / v["closed_p99_min"], 1)
  printf "| open loop share above %s ms / closed loop share | %s | ratio |\n", p["slow_ms"], fmt(v["open_slow_median"] / v["closed_slow_median"], 1)
  printf "| open-loop requests delayed by each stall | %s | requests |\n", v["open_delayed_per_stall"]
  printf "| samples the correction adds for each stall | %s | samples |\n", v["corrected_added_per_stall"]
  printf "| slow samples the closed loop records for each stall | %s | samples |\n", fmt(v["closed_slow_median"] / 100 * v["closed_count_median"] / v["stalls_per_run"], 0)
  printf "| stalls per run | %s | stalls |\n", v["stalls_per_run"]
  print ""
  print "| simulation cost (wall time, this machine) | min ns per simulated request | cycles per request | median ns | cv over reps |"
  print "|---|---|---|---|---|"
  printf "| closed loop | %s | %s | %s | %.1f %% |\n", fmt(v["closed_sim_min"], 2), fmt(v["closed_sim_min"] * ghz, 2), fmt(v["closed_sim_median"], 2), v["closed_sim_cv"]
  printf "| open loop | %s | %s | %s | %.1f %% |\n", fmt(v["open_sim_min"], 2), fmt(v["open_sim_min"] * ghz, 2), fmt(v["open_sim_median"], 2), v["open_sim_cv"]
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
