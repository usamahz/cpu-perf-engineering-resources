#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers.
#
#   ./run.sh            full run: working sets 4 KiB to 1 GiB, 2^26 loads per
#                       size split over 10 runs plus a discarded warmup
#   QUICK=1 ./run.sh    smoke test: working sets to 64 MiB, 2^21 loads per
#                       size, a few seconds; writes results/quick-raw.txt and
#                       results/quick-summary.md and leaves README.md alone
#   REPS=n ./run.sh     override the run count (the loads per size are split
#                       across the runs)
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
  sh ../common/machine.sh
  ../common/clock_estimate
} > "$raw"
# The clock the README quotes is also the clock the cycles column uses.
clock=$(awk '/^estimated clock:/ {print $3}' "$raw")
echo "clock_estimate: $clock GHz"
echo >> "$raw"
if [ "${QUICK:-0}" != 0 ]; then echo "mode: QUICK" >> "$raw"; fi

# A pipe into tee returns tee's status, so bench writes to a file first and
# a non-zero exit stops the run before any summary or README is written.
if CLOCK_GHZ="$clock" ./bench 2>&1 > "$raw.bench" 2>&1; then
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
else
  cat "$raw.bench" >> "$raw"; cat "$raw.bench"; rm -f "$raw.bench"
  echo "bench exited with an error; see $raw" >&2; exit 1
fi
grep -q '^DONE$' "$raw" || { echo "bench did not finish" >&2; exit 1; }

# Summary table from the RESULT lines. Portable awk: no gawk extensions.
#
# The documented-level column and the ratio rows need the cache sizes from
# the machine description. On macOS machine.sh prints them on the
# "P-core L1d:" line; on Linux it prints the lscpu "L1d cache:", "L2 cache:"
# and "L3 cache:" lines, whose totals are divided by the instance count when
# lscpu gives one. Where neither is present the level column reads "unknown"
# and the ratio rows fall back to the 32 KiB and 1 MiB rows.
awk '
function lscpu_bytes(   n, u, inst) {
  n = $3; u = $4
  if (n ~ /[KMG]/) { u = n; sub(/^[0-9.]+/, "", u); sub(/[^0-9.]+$/, "", n) }
  n = n + 0
  if (u ~ /^K/) n *= 1024; else if (u ~ /^M/) n *= 1048576; else if (u ~ /^G/) n *= 1073741824
  inst = 1
  if ($0 ~ /\([0-9]+ instances?\)/) { inst = $0; sub(/.*\(/, "", inst); inst = inst + 0 }
  return (inst > 0) ? n / inst : n
}
/^P-core L1d:/ { l1 = $3; l2 = $(NF-1) }
/^L1d cache:/ { l1 = lscpu_bytes() }
/^L2 cache:/ { l2 = lscpu_bytes() }
/^L3 cache:/ { l3 = lscpu_bytes() }
/^RESULT steps_per_run/ { steps = $3 }
/^RESULT runs/ { runs = $3 }
/^RESULT clock/ { clock = $3 }
/^RESULT pagesize/ { page = $3 }
/^RESULT (line|page)_[0-9]+_/ {
  split($2, a, "_"); m = a[1]; b = a[2] + 0; s = a[3]
  v[m, b, s] = $3
  if (!((m, b) in seen)) { seen[m, b] = 1; n[m]++; order[m, n[m]] = b }
}
function human(b) {
  if (b >= 1073741824) return sprintf("%d GiB", b / 1073741824)
  if (b >= 1048576) return sprintf("%d MiB", b / 1048576)
  return sprintf("%d KiB", b / 1024)
}
function level(b) {
  if (l1 == "" || l2 == "") return "unknown"
  if (b <= l1) return "L1 (" human(l1) ")"
  if (b <= l2) return "L2 (" human(l2) ")"
  if (l3 != "" && b <= l3) return "L3 (" human(l3) ")"
  return "memory"
}
function cyc(m, b) { return ((m, b, "cyc") in v) ? v[m, b, "cyc"] : "n/a" }
# Largest row of mode m whose size is at most cap; 0 if there is none.
function largest_at_most(m, cap,   i, b, best) {
  best = 0
  for (i = 1; i <= n[m]; i++) { b = order[m, i]; if (b <= cap && b > best) best = b }
  return best
}
END {
  printf "One thread on a P-core at default QoS. Each row is the min of %s runs of %s dependent loads after one discarded warmup run; cv is the standard deviation over the runs divided by their mean. Clock %s GHz from clock_estimate; cycles/load is ns/load times the clock.\n\n", runs, steps, clock
  printf "**Line mode.** Random single cycle over 128-byte nodes packed 128 bytes apart.\n\n"
  printf "| working set | lines | pages | documented level | ns/load | cycles/load | cv |\n"
  printf "|---:|---:|---:|:---|---:|---:|---:|\n"
  for (i = 1; i <= n["line"]; i++) {
    b = order["line", i]
    printf "| %s | %s | %s | %s | %s | %s | %s%% |\n", human(b), v["line", b, "lines"], v["line", b, "pages"], level(b), v["line", b, "ns"], cyc("line", b), v["line", b, "cv"]
  }
  printf "\n**Page mode.** One 128-byte node per %s page. The line offsets within the pages are shuffled but balanced, so each of the %d line slots of a page, and so each L1 set, holds the same number of nodes as in the line-mode partner.\n\n", human(page), page / 128
  printf "| span | pages (= lines) | ns/load | cycles/load | cv | line mode, same line count | ns/load | page over line |\n"
  printf "|---:|---:|---:|---:|---:|---:|---:|---:|\n"
  for (i = 1; i <= n["page"]; i++) {
    b = order["page", i]
    partner = v["page", b, "lines"] * 128
    pns = v["line", partner, "ns"]
    ratio = (pns > 0) ? sprintf("%.2f", v["page", b, "ns"] / pns) : "n/a"
    printf "| %s | %s | %s | %s | %s%% | %s | %s | %s |\n", human(b), v["page", b, "pages"], v["page", b, "ns"], cyc("page", b), v["page", b, "cv"], human(partner), pns, ratio
  }
  # Representative rows: an L1 hit well inside the L1 and an L2 hit well
  # inside the L2, so the ratios do not sit on a boundary on other parts.
  l1b = 0; l2b = 0; note = ""
  if (l1 != "" && l2 != "") { l1b = largest_at_most("line", l1 / 4); l2b = largest_at_most("line", l2 / 16) }
  if (l1b == 0 || l2b == 0) {
    l1b = 32768; l2b = 1048576
    note = " The machine description gives no cache sizes, so the L1 and L2 rows are the 32 KiB and 1 MiB rows."
  }
  memb = order["line", n["line"]]
  tlbb = order["page", n["page"]]; tlbp = v["page", tlbb, "lines"] * 128
  printf "\n**Ratios.** The L1 row is the largest line-mode row at or below a quarter of the L1 and the L2 row the largest at or below a sixteenth of the L2.%s\n\n", note
  printf "| ratio | value |\n|:---|---:|\n"
  printf "| L2 over L1: line %s / line %s | %.1f |\n", human(l2b), human(l1b), v["line", l2b, "ns"] / v["line", l1b, "ns"]
  printf "| memory over L2: line %s / line %s | %.1f |\n", human(memb), human(l2b), v["line", memb, "ns"] / v["line", l2b, "ns"]
  printf "| memory over L1: line %s / line %s | %.1f |\n", human(memb), human(l1b), v["line", memb, "ns"] / v["line", l1b, "ns"]
  printf "| TLB cost on top, same cache footprint: page %s span / line %s, both %s lines | %.1f |\n", human(tlbb), human(tlbp), v["page", tlbb, "lines"], v["page", tlbb, "ns"] / v["line", tlbp, "ns"]
  printf "| TLB cost on top, against an L2 hit with no TLB pressure: page %s span / line %s | %.1f |\n", human(tlbb), human(l2b), v["page", tlbb, "ns"] / v["line", l2b, "ns"]
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
else
  python3 ../common/refresh_readme.py .
fi
