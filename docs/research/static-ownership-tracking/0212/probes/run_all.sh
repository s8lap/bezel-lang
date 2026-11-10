#!/bin/bash
# Compile every probe in a directory with a release compiler in three modes (checked+report, checked, unchecked),
# run the executables, record verdicts / trust lines / exit codes / output, and delete the binaries.
# Usage: run_all.sh [PROBE_DIR] [GLOB] > results.md     (defaults: this directory, 'p*.bzl')
# The compiler is r40 (bezelc 0.0.0-a30bb11d, built from origin/main a30bb11df); override with BEZELC=.
# Adapted from docs/research/trust-admissions-by-kind/probes/run_all.sh (scratch under /var/tmp, never /tmp).
B=${BEZELC:-/var/tmp/bezel-scratch/r40/x/bezelc-0.0.0-a30bb11d-linux-x86_64/bin/bezelc}
DIR=${1:-$(dirname "$0")}
GLOB=${2:-p*.bzl}
cd "$DIR" || exit 1
mkdir -p /var/tmp/bezel-scratch/own/p
OUT=$(mktemp -d /var/tmp/bezel-scratch/own/p/probes.XXXXXX)
echo "# Probe results — $($B --version 2>&1 | head -1) — $(date -u +%F)"
echo
echo "Modes: report = --checked-disposal --report-trust-admissions; checked = --checked-disposal; unchecked = --unchecked-disposal. 'trust lines' counts 'trust admission recorded' warnings. Exit 70 = uncaught DisposedAccessException."
for f in $GLOB; do
  n=${f%.bzl}
  echo; echo "## $n"
  sed -n 2p "$f" | sed 's#^// #> #'
  for mode in report checked unchecked; do
    case $mode in report) flags="--checked-disposal --report-trust-admissions";; checked) flags="--checked-disposal";; unchecked) flags="--unchecked-disposal";; esac
    exe="${OUT:?}/${n:?}.${mode:?}"
    if $B $flags -o "$exe" "$f" > "$exe.compile.txt" 2>&1; then
      verdict=accept; grep -qi "warning" "$exe.compile.txt" && verdict=accept-with-warning
    else
      verdict=REJECT
    fi
    trust=$(grep -c "trust admission recorded" "$exe.compile.txt")
    echo "- **$mode**: compile=$verdict; trust lines=$trust"
    if grep -qE "^[^ ].*:[0-9]+:[0-9]+: " "$exe.compile.txt"; then
      grep -E "^[^ ].*:[0-9]+:[0-9]+: " "$exe.compile.txt" | cut -c1-230 | sed 's/^/      /' | head -6
    fi
    if [ $verdict != REJECT ]; then
      timeout 20 "$exe" > "$exe.run.txt" 2>&1; rc=$?
      echo "  run exit=$rc; output: $(head -c 300 "$exe.run.txt" | tr '\n' '|')"
    fi
    rm -f "${exe:?}"
  done
done
rm -rf "${OUT:?}"
