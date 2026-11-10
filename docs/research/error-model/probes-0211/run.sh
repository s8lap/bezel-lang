#!/bin/bash
# Compile and run one probe with the r40 compiler; print compiler output, exit code, program output.
# Usage: run.sh <probe.bzl> [extra bezelc flags]
BEZELC=${BEZELC:-/var/tmp/bezel-scratch/r40/x/bezelc-0.0.0-a30bb11d-linux-x86_64/bin/bezelc}
OUT=${PROBE_OUT:-/var/tmp/bezel-scratch/ff/build}
mkdir -p "$OUT"
src=$1; shift
name=$(basename "$src" .bzl)
echo "== $name"
"$BEZELC" "$@" "$src" -o "$OUT/$name" > "$OUT/$name.compile.txt" 2>&1
rc=$?
sed -e "s|$(dirname "$src")/||g" "$OUT/$name.compile.txt"
echo "-- compile exit $rc"
if [ $rc -eq 0 ] && [ -x "$OUT/$name" ]; then
  timeout 60 "$OUT/$name" 2>&1
  echo "-- run exit $?"
fi
