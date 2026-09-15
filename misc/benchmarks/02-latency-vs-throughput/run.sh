#!/bin/sh
# Build, describe the machine, estimate the clock, run the benchmark, and
# write results/raw.txt and results/summary.md; then paste the summary into
# README.md between the results markers, and the loops the README discusses
# from bench_O2.s between the asm markers.
#
#   ./run.sh            full run: 300 passes x 4 calls per timed region, 31 regions
#                       per kernel plus a warmup, 20M register-only ops per region;
#                       about 4 seconds
#   QUICK=1 ./run.sh    smoke test: 30 passes x 2 calls, 11 regions, 2M ops, under
#                       ten seconds; writes its raw and summary files under
#                       ${TMPDIR:-/tmp} and leaves results/ and README.md alone
#   REPS=n ./run.sh     override the repetition count (minimum 10)
set -eu
cd "$(dirname "$0")"

./build.sh

if [ ! -x ../common/clock_estimate ]; then
  echo "cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c"
  cc -O2 -o ../common/clock_estimate ../common/clock_estimate.c
fi

if [ "${QUICK:-0}" != 0 ]; then
  outdir=${TMPDIR:-/tmp}; outdir=${outdir%/}
  raw=$outdir/03-latency-vs-throughput-quick-raw.txt
  summary=$outdir/03-latency-vs-throughput-quick-summary.md
else
  mkdir -p results
  raw=results/raw.txt
  summary=results/summary.md
fi

{
  ../common/machine.sh
  ../common/clock_estimate
  echo "bench: QUICK=${QUICK:-0} REPS=${REPS:-default}"
  echo "---"
} > "$raw"

# A pipe into tee would hide a non-zero exit from bench, so write, then show.
if ./bench >> "$raw"; then
  cat "$raw"
else
  status=$?
  cat "$raw"
  echo "bench failed with status $status" >&2
  exit $status
fi

# Summary tables from the RESULT lines. Names are <kernel>_<stat> with stat
# in ns_min, ns_median, ns_figure_min or ns_figure_median (the program picks
# min for a chain, a latency, and median for the independent forms,
# throughputs), cv, cycles_median (each rep converted with the clock sampled
# just before it, then the median), per_cycle and ghz_median. Everything in
# the derived table is computed from the figures shown in the tables above it.
awk '
function fmt(x, d) { return sprintf("%." d "f", x) }
function line(k, label) {
  printf "| %s | %s | %s | %.1f %% | %s | %s | %s |\n", label,
         fmt(v[k, "ns_min"], 4), fmt(v[k, "ns_median"], 4), v[k, "cv"],
         (k in fig) ? fig[k] : "n/a",
         ((k, "cycles") in v) ? fmt(v[k, "cycles"], 3) : "n/a",
         ((k, "cycles") in v) ? fmt(v[k, "per_cycle"], 2) : "n/a"
}
/^estimated clock:/ { common_ghz = $3 }
/^array / { elems = $2; bytes = $5; maxv = $7 }
/^passes per call / { passes = $4; calls = $8; reps = $10 }
$1 == "RESULT" {
  name = $2; val = $3
  if (name == "fsum_checksum") { checksum = val; next }
  k = name
  if (sub(/_ns_min$/, "", k)) v[k, "ns_min"] = val
  else if (sub(/_ns_median$/, "", k)) v[k, "ns_median"] = val
  else if (sub(/_ns_figure_min$/, "", k)) fig[k] = "min"
  else if (sub(/_ns_figure_median$/, "", k)) fig[k] = "median"
  else if (sub(/_cv$/, "", k)) v[k, "cv"] = val
  else if (sub(/_cycles_median$/, "", k)) v[k, "cycles"] = val
  else if (sub(/_per_cycle$/, "", k)) v[k, "per_cycle"] = val
  else if (sub(/_ghz_median$/, "", k)) {
    v[k, "ghz"] = val
    if (ghz_lo == "" || val < ghz_lo) ghz_lo = val
    if (ghz_hi == "" || val > ghz_hi) ghz_hi = val
  }
  else next
  seen[k] = 1
}
END {
  have_cycles = (("fsum_acc1", "cycles") in v)
  printf "Array of %s floats, %s bytes, values %s, checksum %s per call. Each timed region is %s calls of %s passes; %s regions per kernel after one discarded warmup, one thread at default QoS. The ns figure is the min for a chain (a latency) and the median for the independent forms (throughputs).", elems, bytes, maxv, checksum, calls, passes, reps
  if (have_cycles) {
    printf " Cycles: every region is converted with the clock sampled just before it (min of three short dependent integer add chains), then the median is taken; the per-kernel median clock"
    if (ghz_lo == ghz_hi) printf " was %s GHz for every kernel", ghz_lo
    else printf " ranged from %s to %s GHz", ghz_lo, ghz_hi
    printf ", and common/clock_estimate read %s GHz just before the run.", common_ghz
  }
  else printf " No inline asm on this architecture, so no cycles."
  print "\n"
  print "| float array sum | min ns/elem | median ns/elem | cv | ns figure | cycles/elem | elem/cycle |"
  print "|---|---|---|---|---|---|---|"
  n = split("1 2 4 8 16", accs, " ")
  peak = 0
  for (i = 1; i <= n; i++) {
    k = "fsum_acc" accs[i]
    if (!(k in seen)) continue
    line(k, accs[i] == 1 ? "1 accumulator (one chain)" : accs[i] " accumulators")
    if (((k, "cycles") in v) && v[k, "per_cycle"] > peak) { peak = v[k, "per_cycle"]; peak_k = accs[i] }
  }
  if ("asm_int_chain" in seen) {
    print ""
    print "| register-only chains, inline asm | min ns/op | median ns/op | cv | ns figure | cycles/op | ops/cycle |"
    print "|---|---|---|---|---|---|---|"
    line("asm_int_chain", "int add, one chain (1 cycle by construction)")
    line("asm_int_indep8", "int add, 8 independent chains")
    line("asm_fadd_chain", "fadd, one chain")
    line("asm_fadd_indep8", "fadd, 8 independent chains")
    line("asm_fadd_indep16", "fadd, 16 independent chains")
    line("asm_fmul_chain", "fmul, one chain")
  }
  print ""
  print "| derived | value | unit |"
  print "|---|---|---|"
  if (have_cycles) {
    c1 = v["fsum_acc1", "cycles"]
    for (i = 2; i <= n; i++) {
      k = "fsum_acc" accs[i]
      if ((k, "cycles") in v) printf "| speedup of %s accumulators over the chain (cycles/elem ratio) | %s | ratio |\n", accs[i], fmt(c1 / v[k, "cycles"], 2)
    }
    printf "| fadd latency, from the 1-accumulator array chain | %s | cycles |\n", fmt(c1, 2)
    if (("asm_fadd_chain", "cycles") in v) printf "| fadd latency, from the register-only chain | %s | cycles |\n", fmt(v["asm_fadd_chain", "cycles"], 2)
    if (("asm_fmul_chain", "cycles") in v) printf "| fmul latency, from the register-only chain | %s | cycles |\n", fmt(v["asm_fmul_chain", "cycles"], 2)
    printf "| fadd throughput, array plateau (%s accumulators) | %s | adds/cycle |\n", peak_k, fmt(peak, 2)
    if (("asm_fadd_indep16", "cycles") in v) printf "| fadd throughput, register-only 16 chains | %s | adds/cycle |\n", fmt(v["asm_fadd_indep16", "per_cycle"], 2)
    if (("asm_fadd_indep8", "cycles") in v) printf "| fadd throughput, register-only 8 chains | %s | adds/cycle |\n", fmt(v["asm_fadd_indep8", "per_cycle"], 2)
    if (("asm_int_indep8", "cycles") in v) printf "| int add throughput, register-only 8 chains | %s | adds/cycle |\n", fmt(v["asm_int_indep8", "per_cycle"], 2)
    printf "| chains needed for the plateau if latency were fixed (latency x throughput) | %s | chains |\n", fmt(c1 * peak, 1)
    if (("fsum_acc8", "cycles") in v) printf "| 8 accumulators as a fraction of the plateau (elem/cycle ratio) | %s | ratio |\n", fmt(v["fsum_acc8", "per_cycle"] / peak, 2)
    for (i = 1; i <= n; i++) {
      k = "fsum_acc" accs[i]
      if ((k, "cycles") in v) printf "| cycles each chain waits per add, %s accumulator%s (%s x cycles/elem) | %s | cycles |\n", accs[i], (accs[i] == 1 ? "" : "s"), accs[i], fmt(accs[i] * v[k, "cycles"], 2)
    }
    if (("asm_fadd_indep8", "cycles") in v) printf "| cycles each chain waits per add, register-only 8 chains (8 x cycles/add) | %s | cycles |\n", fmt(8 * v["asm_fadd_indep8", "cycles"], 2)
    printf "| clock, per-kernel median of the per-rep samples, lowest / highest | %s / %s | GHz |\n", ghz_lo, ghz_hi
    printf "| clock, common/clock_estimate before the run | %s | GHz |\n", common_ghz
  } else {
    printf "| speedup of 8 accumulators over the chain (min ns / median ns) | %s | ratio |\n", fmt(v["fsum_acc1", "ns_min"] / v["fsum_acc8", "ns_median"], 2)
    printf "| speedup of 16 accumulators over the chain (min ns / median ns) | %s | ratio |\n", fmt(v["fsum_acc1", "ns_min"] / v["fsum_acc16", "ns_median"], 2)
  }
}' "$raw" > "$summary"

echo "wrote $raw and $summary"
if [ "${QUICK:-0}" != 0 ]; then
  echo "QUICK run: README.md not refreshed"
  exit 0
fi
python3 ../common/refresh_readme.py .

# The README quotes the inner loops of sum1, sum8 and fadd_chain and the
# timed call site in main. Copy them out of bench_O2.s here rather than by
# hand, so the quoted code is the code that ran and cannot drift from the
# file it claims to come from. The patterns are clang's: a loop is the block
# from the label clang annotates "Inner Loop Header" to the branch back to
# it; the call site is the block from a clock_gettime call, through the
# first indirect call (the kernel, through the ks table) with no other call
# in between, to the next clock_gettime call. Runs of empty InlineAsm
# Start/End pairs (the PIN statements) are shown as one line.

# The inner loop of function $1 (the name without any leading underscore).
inner_loop() {
  awk -v fn="$1" '
    !infn { if ($0 ~ ("^_?" fn ":")) infn = 1; next }
    /\.cfi_endproc/ { exit }
    /^\.?LBB[0-9]+_[0-9]+:/ { label = $1; sub(/:$/, "", label); labelline = $0 }
    !cap && /Inner Loop Header/ && label != "" {
      cap = 1; loop = label
      if ($0 != labelline) print labelline
    }
    cap { print; if ($0 ~ (loop "$") && $0 !~ /:/) exit }
  ' bench_O2.s
}

# The timed call site in main.
call_site() {
  awk '
    function is_call()     { return $1 == "bl" || $1 == "call" || $1 == "callq" }
    function is_indirect() { return $1 == "blr" || (($1 == "call" || $1 == "callq") && $2 ~ /^\*/) }
    !inmain { if ($0 ~ /^_?main:/) inmain = 1; next }
    /\.cfi_endproc/ { exit }
    cap { print; if (is_call() && $2 ~ /clock_gettime/) exit; next }
    is_indirect() { if (buffering) { cap = 1; printf "%s%s\n", buf, $0 }; next }
    is_call() {
      if ($2 ~ /clock_gettime/) { buffering = 1; buf = $0 "\n" }
      else { buffering = 0; buf = "" }
      next
    }
    buffering { buf = buf $0 "\n" }
  ' bench_O2.s
}

# Replace each run of adjacent empty InlineAsm Start/End pairs by one line
# that says how many there were.
collapse_pins() {
  awk '
    { l[NR] = $0 }
    END {
      i = 1
      while (i <= NR) {
        n = 0
        while (i < NR && l[i] ~ /InlineAsm Start$/ && l[i + 1] ~ /InlineAsm End$/) { n++; i += 2 }
        if (n) printf "\t; InlineAsm Start / InlineAsm End, %d empty pair%s: the PIN statements, no instruction\n", n, (n == 1 ? "" : "s")
        else { print l[i]; i++ }
      }
    }
  '
}

# Replace the block between <!-- $1:start --> and <!-- $1:end --> in
# README.md with the file $2.
splice() {
  awk -v start="<!-- $1:start -->" -v end="<!-- $1:end -->" -v f="$2" '
    $0 == start { print; while ((getline l < f) > 0) print l; skip = 1; next }
    $0 == end { skip = 0 }
    !skip { print }
  ' README.md > README.md.tmp && mv README.md.tmp README.md
}

excerpt=$(mktemp "${TMPDIR:-/tmp}/03-latency-vs-throughput-asm.XXXXXX")
ok=1
first=1
for fn in sum1 sum8 fadd_chain; do
  body=$(inner_loop "$fn" | collapse_pins)
  if [ -z "$body" ]; then ok=0; break; fi
  case "$fn" in
    sum1) what="one accumulator, one chain" ;;
    sum8) what="eight accumulators" ;;
    fadd_chain) what="the register-only fadd chain, no loads" ;;
  esac
  if [ "$first" = 1 ]; then first=0; else printf '\n' >> "$excerpt"; fi
  printf '`%s`, the inner loop (%s):\n\n```\n%s\n```\n' "$fn" "$what" "$body" >> "$excerpt"
done
if [ "$ok" = 1 ]; then
  splice asm-loops "$excerpt"
  body=$(call_site)
  if [ -n "$body" ]; then
    printf '```\n%s\n```\n' "$body" > "$excerpt"
    splice asm-callsite "$excerpt"
    echo "updated the asm blocks in README.md from bench_O2.s"
  else
    ok=0
  fi
fi
if [ "$ok" != 1 ]; then
  echo "could not find the quoted loops in bench_O2.s (the patterns are for clang output); README.md asm blocks left as they were" >&2
fi
rm -f "$excerpt"
