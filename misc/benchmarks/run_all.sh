#!/bin/sh
# Build and run every benchmark, one after another, never concurrently.
# QUICK=1 runs each with small sizes for a smoke test (CI); REPS=n overrides
# the repetition count. Results and README tables are refreshed in place.
set -eu
cd "$(dirname "$0")"
cc -O2 -o common/clock_estimate common/clock_estimate.c
status=0
for d in [0-9][0-9]-*/; do
  d=${d%/}
  printf '\n=== %s\n' "$d"
  if (cd "$d" && ./run.sh); then
    printf '=== %s ok\n' "$d"
  else
    printf '=== %s FAILED\n' "$d"
    status=1
  fi
done
exit $status
