#!/bin/bash
# Compile every example with the r35 pre-release in checked (default) and unchecked disposal
# mode; record verdict, diagnostics, run output and exit code into results.md.
B=${BEZELC:-/tmp/claude-0/-home-user-bezel-lang/3f619ddf-d843-5ab0-a977-e5b3105857a0/scratchpad/rel35/bezelc-0.0.0-f6863e62-linux-x86_64/bin/bezelc}
OUT=out; mkdir -p "$OUT"
{
echo "# Results — $($B --version 2>&1) — $(date -u +%F)"
for f in k53_*.bzl k52_*.bzl k313_*.bzl; do
  n=${f%.bzl}
  echo; echo "## $n"
  for mode in checked unchecked; do
    flag=--checked-disposal; [ $mode = unchecked ] && flag=--unchecked-disposal
    if $B $flag "$f" -o "$OUT/$n.$mode" > "$OUT/$n.$mode.compile.txt" 2>&1; then
      verdict=accept; grep -qi "warning" "$OUT/$n.$mode.compile.txt" && verdict=accept-with-warning
      timeout 20 "./$OUT/$n.$mode" > "$OUT/$n.$mode.run.txt" 2>&1; rc=$?
      echo "- **$mode**: compile=$verdict; run exit=$rc"
      [ -s "$OUT/$n.$mode.compile.txt" ] && { echo '  diagnostics:'; sed 's/^/      /' "$OUT/$n.$mode.compile.txt" | head -6; }
      echo '  output:'; sed 's/^/      /' "$OUT/$n.$mode.run.txt" | head -12
    else
      echo "- **$mode**: compile=REJECT"
      echo '  diagnostics:'; sed 's/^/      /' "$OUT/$n.$mode.compile.txt" | head -8
    fi
  done
done
} | tee results.md
