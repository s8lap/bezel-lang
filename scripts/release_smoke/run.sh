#!/bin/sh
# release_smoke/run.sh -- the release tarball's smoke test (release_static.sh).
#
# Compiles every <name>.bzl next to this script with BEZELC, runs it, and
# checks its stdout against <name>.expected and its exit code against 0.
# With --static it also checks that each executable is fully static. With
# --no-compiler it first checks that the machine has no C compiler or linker,
# so a pass proves the self-contained link needed none (the bare-container
# leg). POSIX sh: it runs in a stock ubuntu or alpine image.
#
# Usage: run.sh [--static] [--no-compiler] BEZELC WORKDIR
set -eu

STATIC=0
NOCC=0
while [ $# -gt 0 ]; do
  case "$1" in
    --static) STATIC=1 ;;
    --no-compiler) NOCC=1 ;;
    *) break ;;
  esac
  shift
done
[ $# -eq 2 ] || { echo "usage: run.sh [--static] [--no-compiler] BEZELC WORKDIR" >&2; exit 2; }
BEZELC=$1
WORK=$2
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$WORK"

if [ "$NOCC" = 1 ]; then
  for t in cc gcc clang c++ g++ clang++ ld ld.lld ld.bfd ld.gold; do
    if command -v "$t" >/dev/null 2>&1; then
      echo "smoke: FAIL: '$t' is installed here; the no-compiler leg needs a bare machine" >&2
      exit 1
    fi
  done
  echo "smoke: no C compiler or linker on this machine"
fi

"$BEZELC" --version
fails=0
for src in "$HERE"/*.bzl; do
  name=$(basename "$src" .bzl)
  exe="$WORK/$name"
  if ! "$BEZELC" "$src" -o "$exe"; then
    echo "smoke: FAIL $name: bezelc could not build it" >&2
    fails=$((fails + 1))
    continue
  fi
  rc=0
  "$exe" >"$WORK/$name.out" 2>"$WORK/$name.err" || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "smoke: FAIL $name: exit code $rc (stderr: $(head -c 400 "$WORK/$name.err"))" >&2
    fails=$((fails + 1))
    continue
  fi
  if ! cmp -s "$WORK/$name.out" "$HERE/$name.expected"; then
    echo "smoke: FAIL $name: stdout differs from $name.expected:" >&2
    cat "$WORK/$name.out" >&2
    fails=$((fails + 1))
    continue
  fi
  link=""
  if [ "$STATIC" = 1 ]; then
    if command -v file >/dev/null 2>&1; then
      info=$(file -b "$exe")
      case "$info" in
        *"statically linked"*) link="static (file)" ;;
        *) echo "smoke: FAIL $name: not static: $info" >&2; fails=$((fails + 1)); continue ;;
      esac
    fi
    if command -v ldd >/dev/null 2>&1; then
      # glibc's ldd: "not a dynamic executable" (rc 1); musl's: an error too.
      lout=$(ldd "$exe" 2>&1 || true)
      case "$lout" in
        *"not a dynamic executable"*|*"Not a valid dynamic program"*|*"statically linked"*)
          link="${link:+$link, }static (ldd)" ;;
        *) echo "smoke: FAIL $name: ldd lists dependencies: $lout" >&2; fails=$((fails + 1)); continue ;;
      esac
    fi
    [ -n "$link" ] || { echo "smoke: FAIL $name: --static needs file or ldd" >&2; exit 1; }
  fi
  echo "smoke: ok $name (rc 0, stdout = $name.expected${link:+, $link})"
  sed 's/^/    | /' "$WORK/$name.out"
done
if [ "$fails" -ne 0 ]; then
  echo "smoke: $fails program(s) FAILED" >&2
  exit 1
fi
echo "smoke: all programs passed"
