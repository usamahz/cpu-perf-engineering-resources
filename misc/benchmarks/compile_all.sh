#!/bin/sh
# Compile (do not run) every benchmark on the current platform. Used by CI
# on Linux to prove the sources are portable beyond the machine they were
# measured on. Platform-only code paths must be guarded in the sources.
set -eu
cd "$(dirname "$0")"
status=0
for d in [0-9][0-9]-*/; do
  d=${d%/}
  if cc -std=c11 -O2 -Wall -Wextra -I common -c -o /dev/null "$d/bench.c"; then
    echo "compiled $d/bench.c"
  else
    echo "FAILED $d/bench.c"
    status=1
  fi
done
cc -std=c11 -O2 -Wall -I common -c -o /dev/null common/clock_estimate.c && echo "compiled common/clock_estimate.c"
exit $status
