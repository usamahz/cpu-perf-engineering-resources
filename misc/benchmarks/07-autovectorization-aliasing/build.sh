#!/bin/sh
# The exact compile lines for this benchmark. bench.c is built three times
# from the same source: plain -O2 (the primary build, as the brief asks),
# -O2 -fno-unroll-loops (the control: one element per scalar iteration and
# one vector per vector iteration, so the scalar-to-vector ratio is the
# vector width), and -O3. Each binary carries its label and flags so its
# RESULT lines say which build they came from. The bench_*.s files and
# bench_remarks.txt are what the README quotes; regenerate them here rather
# than trusting the README. Prints each line before it runs it and exits
# non-zero if any fails.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
BASE="-std=c11 -Wall -Wextra"
# The remarks pass prints one line per remark and no source echo, so the
# README can quote bench_remarks.txt verbatim.
case "$($CC --version 2>/dev/null | head -1)" in
  *clang*) REMARKS="-Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize -fno-caret-diagnostics" ;;
  *)       REMARKS="-fopt-info-vec-all -fno-diagnostics-show-caret" ;;
esac

: > bench_remarks.txt
build() {
  label=$1; opt=$2
  flags="$BASE $opt"
  # -lm: timing.h calls sqrt, which gcc on Linux does not inline under its
  # default -fmath-errno; harmless where the libc already provides it.
  echo "$CC $flags -DBUILD_LABEL='\"$label\"' -DBUILD_FLAGS='\"$flags\"' -o bench_$label bench.c -lm"
  $CC $flags -DBUILD_LABEL="\"$label\"" -DBUILD_FLAGS="\"$flags\"" -o "bench_$label" bench.c -lm
  echo "$CC $flags -S -o bench_$label.s bench.c"
  $CC $flags -S -o "bench_$label.s" bench.c
  # The vectoriser's own account of every loop. Remarks go to stderr; the
  # compile itself must still succeed.
  echo "$CC $flags $REMARKS -c -o /dev/null bench.c 2>> bench_remarks.txt"
  {
    echo "### $label: $CC $flags $REMARKS"
    $CC $flags $REMARKS -c -o /dev/null bench.c 2>&1 >/dev/null
    echo
  } >> bench_remarks.txt
}

build O2   "-O2"
build O2nu "-O2 -fno-unroll-loops"
build O3   "-O3"
