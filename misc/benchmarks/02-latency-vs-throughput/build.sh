#!/bin/sh
# The exact compile lines for this benchmark. -O2 and no -ffast-math: the
# claim rests on the compiler keeping the float adds in the order the source
# wrote them. Prints each line before it runs it and exits non-zero if either
# fails. bench_O2.s is the assembly the README quotes; regenerate it here
# rather than trusting the README.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
CFLAGS="-std=c11 -O2 -Wall -Wextra"
echo "$CC $CFLAGS -o bench bench.c -lm -lpthread"
$CC $CFLAGS -o bench bench.c -lm -lpthread
echo "$CC $CFLAGS -S -o bench_O2.s bench.c"
$CC $CFLAGS -S -o bench_O2.s bench.c
