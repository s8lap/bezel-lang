#!/bin/bash
# Compile every probe with the r35 release compiler under --report-trust-admissions (checked disposal is the
# default; the flag is also passed explicitly, and the no-flag twin is compiled to show the default build).
# Run probes (those with a Main) are also executed. Results in results.md.
B=${BEZELC:-/tmp/claude-0/-home-user-bezel-lang/3f619ddf-d843-5ab0-a977-e5b3105857a0/scratchpad/rel35/bezelc-0.0.0-f6863e62-linux-x86_64/bin/bezelc}
cd "$(dirname "$0")"
OUT=out; mkdir -p "$OUT"
{
echo "# Probe results — $($B --version 2>&1 | head -1) — $(date -u +%F)"
echo
echo "Legend: verdict = accept | accept-with-warning | REJECT; 'trust lines' = count of 'trust admission recorded' warnings under the flag; the default-build column is the same program with no flag."
for f in [a-e][0-9][0-9]_*.bzl; do
  n=${f%.bzl}
  echo; echo "## $n"
  sed -n 2p "$f" | sed 's#^// #> #'
  hasmain=0; grep -q "static method Main" "$f" && hasmain=1
  for mode in flag noflag; do
    if [ $mode = flag ]; then flags="--checked-disposal --report-trust-admissions"; else flags="--checked-disposal"; fi
    if [ $hasmain = 1 ]; then outarg="-o $OUT/$n.$mode"; else outarg="-c -o $OUT/$n.$mode.o"; fi
    if $B $flags $outarg "$f" > "$OUT/$n.$mode.compile.txt" 2>&1; then
      verdict=accept; grep -qi "warning" "$OUT/$n.$mode.compile.txt" && verdict=accept-with-warning
    else
      verdict=REJECT
    fi
    trust=$(grep -c "trust admission recorded" "$OUT/$n.$mode.compile.txt")
    echo "- **$mode**: compile=$verdict; trust lines=$trust"
    if [ -s "$OUT/$n.$mode.compile.txt" ]; then
      echo '  diagnostics (first line of each, cut):'
      grep -E "^[^ ].*:[0-9]+:[0-9]+: (warning|error)" "$OUT/$n.$mode.compile.txt" | cut -c1-200 | sed 's/^/      /' | head -8
    fi
    if [ $hasmain = 1 ] && [ $verdict != REJECT ]; then
      timeout 20 "./$OUT/$n.$mode" > "$OUT/$n.$mode.run.txt" 2>&1; rc=$?
      echo "  run exit=$rc; output:"; sed 's/^/      /' "$OUT/$n.$mode.run.txt" | head -5
    fi
  done
done
} | tee results.md
