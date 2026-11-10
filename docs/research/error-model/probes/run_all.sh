#!/bin/bash
# Run every probe with the r40 compiler and write RESULTS.txt beside this script.
# The cost probe (p09) prints timings that vary by a few percent between runs.
cd "$(dirname "$0")" || exit 1
{
  echo "compiler: $(${BEZELC:-/var/tmp/bezel-scratch/r40/x/bezelc-0.0.0-a30bb11d-linux-x86_64/bin/bezelc} --version 2>&1 | head -1)"
  for p in p*.bzl; do ./run.sh "$p"; echo; done
} > RESULTS.txt 2>&1
