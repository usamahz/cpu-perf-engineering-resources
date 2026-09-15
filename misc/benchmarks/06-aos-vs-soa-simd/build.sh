#!/bin/sh
# The exact compile lines for this benchmark. bench.c is compiled twice:
# the default unit holds main, the -O2 kernels and the NEON intrinsics;
# with SCALAR_TU it holds only the four scalar kernels, and that unit is
# built with both vectorisers off so their loops stay one dependent chain.
# bench_O2.s, bench_scalar.s and bench_remarks.txt are what the README
# quotes; regenerate them here rather than trusting the README. Prints
# each line before it runs it and exits non-zero if any fails.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
CFLAGS="-std=c11 -O2 -Wall -Wextra"
case "$($CC --version 2>/dev/null | head -1)" in
  *clang*)
    NOVEC="-fno-vectorize -fno-slp-vectorize"
    REMARKS="-Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize"
    ;;
  *)
    NOVEC="-fno-tree-vectorize -fno-tree-slp-vectorize"
    REMARKS="-fopt-info-vec-all"
    ;;
esac

echo "$CC $CFLAGS -c -o bench_main.o bench.c"
$CC $CFLAGS -c -o bench_main.o bench.c
echo "$CC $CFLAGS $NOVEC -DSCALAR_TU -c -o bench_scalar.o bench.c"
$CC $CFLAGS $NOVEC -DSCALAR_TU -c -o bench_scalar.o bench.c
echo "$CC -o bench bench_main.o bench_scalar.o -lm"
$CC -o bench bench_main.o bench_scalar.o -lm
rm -f bench_main.o bench_scalar.o

echo "$CC $CFLAGS -S -o bench_O2.s bench.c"
$CC $CFLAGS -S -o bench_O2.s bench.c
echo "$CC $CFLAGS $NOVEC -DSCALAR_TU -S -o bench_scalar.s bench.c"
$CC $CFLAGS $NOVEC -DSCALAR_TU -S -o bench_scalar.s bench.c

# The vectoriser's own account of every loop in both units, kept for the
# README. Remarks go to stderr; the compile itself must still succeed.
echo "$CC $CFLAGS $REMARKS -c -o /dev/null bench.c 2> bench_remarks.txt"
$CC $CFLAGS $REMARKS -c -o /dev/null bench.c 2> bench_remarks.txt
echo "$CC $CFLAGS $NOVEC $REMARKS -DSCALAR_TU -c -o /dev/null bench.c 2>> bench_remarks.txt"
$CC $CFLAGS $NOVEC $REMARKS -DSCALAR_TU -c -o /dev/null bench.c 2>> bench_remarks.txt
