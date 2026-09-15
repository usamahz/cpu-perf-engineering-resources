#!/bin/sh
# The exact compile lines for this benchmark. Prints each line before it
# runs it and exits non-zero if either fails. bench_O2.s is the assembly
# the README quotes; regenerate it here rather than trusting the README.
#
# On macOS the vendor BLAS is Accelerate (-framework Accelerate). On Linux
# set USE_OPENBLAS=1 to link OpenBLAS instead; with neither the BLAS row is
# skipped at run time with a message. Linux links libm explicitly because
# timing.h calls sqrt and gcc keeps the call for the errno path.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
CFLAGS="-std=c11 -O2 -Wall -Wextra"
LDFLAGS=""
case "$(uname -s)" in
  Darwin) LDFLAGS="-framework Accelerate" ;;
  *) LDFLAGS="-lm"
     if [ "${USE_OPENBLAS:-0}" != 0 ]; then CFLAGS="$CFLAGS -DUSE_OPENBLAS"; LDFLAGS="-lopenblas -lm"; fi ;;
esac
echo "$CC $CFLAGS -o bench bench.c $LDFLAGS"
$CC $CFLAGS -o bench bench.c $LDFLAGS
echo "$CC $CFLAGS -S -o bench_O2.s bench.c"
$CC $CFLAGS -S -o bench_O2.s bench.c
