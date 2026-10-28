#!/bin/bash
# Compile every probe with the r35 release compiler in three modes (checked+report, checked, unchecked), run the
# executables, record verdicts/trust lines/exit codes/output in results.md, and delete the binaries (disk is tight).
B=${BEZELC:-/tmp/claude-0/-home-user-bezel-lang/3f619ddf-d843-5ab0-a977-e5b3105857a0/scratchpad/rel35/bezelc-0.0.0-f6863e62-linux-x86_64/bin/bezelc}
cd "$(dirname "$0")"
mkdir -p /var/tmp/bezel-scratch
OUT=$(mktemp -d /var/tmp/bezel-scratch/trustshape-probes.XXXXXX)
{
echo "# Probe results — $($B --version 2>&1 | head -1) — $(date -u +%F)"
echo
echo "Modes: report = --checked-disposal --report-trust-admissions; checked = --checked-disposal; unchecked = --unchecked-disposal. 'trust lines' counts 'trust admission recorded' warnings. Exit 70 = uncaught DisposedAccessException (r35's convention)."
for f in k[0-9]*_*.bzl; do
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
    if grep -qE "^[^ ].*:[0-9]+:[0-9]+: (warning|error)" "$exe.compile.txt"; then
      grep -E "^[^ ].*:[0-9]+:[0-9]+: (warning|error)" "$exe.compile.txt" | cut -c1-170 | sed 's/^/      /' | head -6
    fi
    if [ $verdict != REJECT ]; then
      timeout 20 "$exe" > "$exe.run.txt" 2>&1; rc=$?
      echo "  run exit=$rc; output: $(head -c 300 "$exe.run.txt" | tr '\n' '|')"
    fi
    rm -f "${exe:?}"
  done
done
} | tee results.md
rm -rf "${OUT:?}"
