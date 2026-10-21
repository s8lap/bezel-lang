#!/usr/bin/env bash
# release_static.sh -- build a relocatable bezelc and package it as a release
# tarball: bezelc-<version>-linux-x86_64.tar.gz plus a .sha256 file.
#
# WHY. The normal build/ tree links the shared libLLVM and bakes the build
# tree's ABSOLUTE path to libbezel_runtime.a into the driver, so its bezelc only
# works on the machine that built it. The release build configures a SEPARATE
# build dir with -DBEZEL_STATIC_RELEASE=ON (see CMakeLists.txt): static LLVM,
# static libgcc (and libstdc++ when libstdc++-static is installed), and a
# runtime archive found relative to the executable (../lib/bezel/). It never
# touches build/.
#
# What the installed compiler needs at run time (Driver::linkCommandLine):
#   * `clang` on PATH -- bezelc emits an object with its built-in LLVM, then
#     shells out to `clang <obj> -o <exe> -lm -lpthread <runtime.a> -lstdc++`;
#   * libstdc++ development files (libstdc++.so for -lstdc++) and the C
#     runtime/crt files clang uses, i.e. a working `clang hello.c` setup;
#   * lib/bezel/libbezel_runtime.a from this tarball, next to bin/.
# The generated programs link clang's DEFAULT linker (-DBEZEL_PROGRAM_LINKER=
# default), so lld is not required.
#
# Version: an exact `v<semver>` tag on HEAD gives <semver>; otherwise
# <project VERSION from CMakeLists.txt>-<short sha>. It is passed to CMake as
# BEZEL_VERSION_OVERRIDE so `bezelc --version` matches the tarball name.
#
# Usage: scripts/release_static.sh [--full] [--allow-dirty]
#                                  [--build-dir DIR] [--out DIR] [-j N]
#   --full         also link bezelc with -static (needs glibc-static and
#                  libstdc++-static; see the CMake BEZEL_STATIC_RELEASE_FULL)
#   --allow-dirty  package a tree with uncommitted changes (version gets
#                  a "-dirty" suffix)
#   --build-dir    default: <repo>/scratch/build-static (build-static-full with --full)
#   --out          default: <repo>/scratch/release (release-full with --full); the
#                  two variants share a tarball name, so they get separate dirs
#   -j N           build parallelism (default 8; builds go through
#                  scripts/dev_heavy.sh and the machine's build-token pool)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=""
OUT_DIR=""
JOBS=8
FULL=OFF
ALLOW_DIRTY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --full) FULL=ON ;;
    --allow-dirty) ALLOW_DIRTY=1 ;;
    --build-dir) BUILD_DIR="$2"; shift ;;
    --out) OUT_DIR="$2"; shift ;;
    -j) JOBS="$2"; shift ;;
    -h|--help) sed -n '2,/^set -e/{/^set -e/d;p}' "$0"; exit 0 ;;
    *) echo "release_static: unknown argument '$1'" >&2; exit 2 ;;
  esac
  shift
done

die() { echo "release_static: $*" >&2; exit 1; }

SUFFIX=""; [ "$FULL" = ON ] && SUFFIX="-full"
[ -n "$BUILD_DIR" ] || BUILD_DIR="$ROOT/scratch/build-static$SUFFIX"
[ -n "$OUT_DIR" ] || OUT_DIR="$ROOT/scratch/release$SUFFIX"

[ "$(uname -s)-$(uname -m)" = "Linux-x86_64" ] || die "only Linux x86_64 is supported"
# Every tool a check below relies on must be present here: a missing nm, ldd or
# strings would otherwise let its guard pass silently, and a missing objdump
# would make the README claim "fully static".
for tool in clang clang++ cmake tar gzip sha256sum nm objdump strings ldd; do
  command -v "$tool" >/dev/null || die "'$tool' not found on PATH"
done

# ---- version ---------------------------------------------------------------
cd "$ROOT"
DIRTY=""
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  [ "$ALLOW_DIRTY" = 1 ] || die "working tree has uncommitted changes (use --allow-dirty)"
  DIRTY="-dirty"
fi
SHORT=$(git rev-parse --short=8 HEAD)
TAG=$(git tag --points-at HEAD --list 'v[0-9]*' | sort -V | tail -1)
if [ -n "$TAG" ]; then
  VERSION="${TAG#v}$DIRTY"
else
  PROJ_VERSION=$(sed -n '/^project(bezel/,/)/s/^ *VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt | head -1)
  [ -n "$PROJ_VERSION" ] || die "could not read project VERSION from CMakeLists.txt"
  VERSION="$PROJ_VERSION-$SHORT$DIRTY"
fi
NAME="bezelc-$VERSION-linux-x86_64"
echo "release_static: version $VERSION -> $NAME.tar.gz"

# ---- configure + build -----------------------------------------------------
mkdir -p "$BUILD_DIR" "$OUT_DIR"
# Release (no -g): nothing in the tarball needs debug info, and it keeps the
# runtime archive the user links small.
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBEZEL_STATIC_RELEASE=ON \
  -DBEZEL_STATIC_RELEASE_FULL="$FULL" \
  -DBEZEL_PROGRAM_LINKER=default \
  -DBEZEL_VERSION_OVERRIDE="$VERSION" >"$BUILD_DIR/release-configure.log" 2>&1 \
  || { tail -30 "$BUILD_DIR/release-configure.log" >&2; die "configure failed (log: $BUILD_DIR/release-configure.log)"; }
grep -E "static release|WARNING" -A2 "$BUILD_DIR/release-configure.log" || true

"$ROOT/scripts/dev_heavy.sh" build cmake --build "$BUILD_DIR" -j"$JOBS" --target bezelc \
  || die "build failed"

BIN="$BUILD_DIR/bezelc"
RT="$BUILD_DIR/libbezel_runtime.a"
[ -x "$BIN" ] && [ -f "$RT" ] || die "build produced no bezelc / libbezel_runtime.a"

# The release binary must not depend on the shared libLLVM or name the build
# tree's runtime archive.
if ldd "$BIN" 2>/dev/null | grep -q 'libLLVM'; then
  die "bezelc links libLLVM dynamically -- BEZEL_STATIC_RELEASE did not take effect"
fi
# Users link the runtime against THEIR glibc: it must not reference the
# glibc 2.38 C23 strto* entry points (see the BEZEL_STATIC_RELEASE block).
# nm's output is captured first: a failing nm must stop the run, not pass the
# check, and grep -q on a live pipe can SIGPIPE nm into a false pass.
NM_UNDEF=$(nm -u "$RT") || die "nm -u failed on $RT, so the __isoc23_ check cannot run"
if grep -q '__isoc23_' <<<"$NM_UNDEF"; then
  die "libbezel_runtime.a references __isoc23_* (glibc 2.38+ only)"
fi
if strings "$BIN" | grep -qF "$BUILD_DIR"; then
  die "bezelc embeds the build-dir path $BUILD_DIR"
fi

# ---- assemble --------------------------------------------------------------
STAGE="$OUT_DIR/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/$NAME/bin" "$STAGE/$NAME/lib/bezel"
STRIP=$(command -v llvm-strip || command -v strip || true)
cp "$BIN" "$STAGE/$NAME/bin/bezelc"
cp "$RT" "$STAGE/$NAME/lib/bezel/libbezel_runtime.a"
if [ -n "$STRIP" ]; then
  "$STRIP" --strip-all "$STAGE/$NAME/bin/bezelc"
  "$STRIP" --strip-debug "$STAGE/$NAME/lib/bezel/libbezel_runtime.a"
fi
cp "$ROOT/LICENSE" "$STAGE/$NAME/LICENSE"

GLIBC_FLOOR=$(objdump -T "$STAGE/$NAME/bin/bezelc" 2>/dev/null \
  | grep -o 'GLIBC_[0-9][0-9.]*' | sort -uV | tail -1 || true)
GLIBCXX_FLOOR=$(objdump -T "$STAGE/$NAME/bin/bezelc" 2>/dev/null \
  | grep -o 'GLIBCXX_[0-9][0-9.]*' | sort -uV | tail -1 || true)
if [ -n "$GLIBC_FLOOR" ]; then
  LINKAGE="LLVM linked in statically and depends dynamically on glibc (needs ${GLIBC_FLOOR/_/ } or newer)"
  if [ -n "$GLIBCXX_FLOOR" ]; then
    LINKAGE="$LINKAGE and libstdc++.so.6 (needs $GLIBCXX_FLOOR or newer)"
  else
    LINKAGE="LLVM and libstdc++ linked in statically and depends dynamically on glibc (needs ${GLIBC_FLOOR/_/ } or newer)"
  fi
else
  LINKAGE="no shared-library dependencies (fully static)"
fi
# The GCC installation clang selected (its libstdc++ headers built the runtime).
GCC_MAJOR=$(clang++ -v 2>&1 | sed -n 's|^Selected GCC installation: .*/||p')
NEEDED=$(objdump -p "$STAGE/$NAME/bin/bezelc" 2>/dev/null | awk '/NEEDED/{print "    " $2}' || true)

# The floor a PROGRAM bezelc links needs. It is set by the runtime archive,
# which was compiled against this machine's glibc headers (e.g. glibc >= 2.38
# headers redirect strtol to __isoc23_strtol), not by how bezelc itself is
# linked: link one program with the staged tree and read its symbol versions.
PROBE="$OUT_DIR/probe"
rm -rf "$PROBE"; mkdir -p "$PROBE"
cat >"$PROBE/probe.bzl" <<'BZL'
module Probe;

public class Program
  public static method Main()
    Console.WriteLine("probe");
  end Main;
end Program;
BZL
(cd "$PROBE" && "$STAGE/$NAME/bin/bezelc" probe.bzl -o probe) \
  || die "the staged compiler could not link a program"
PROBE_SYMS=$(objdump -T "$PROBE/probe") \
  || { rm -rf "$PROBE"; die "objdump -T failed on the probe program"; }
rm -rf "$PROBE"
PROG_GLIBC=$(grep -o 'GLIBC_[0-9][0-9.]*' <<<"$PROBE_SYMS" | sort -uV | tail -1 || true)
[ -n "$PROG_GLIBC" ] \
  || die "could not read the glibc floor of the probe program (objdump -T lists no GLIBC_ symbol versions)"
# Empty is a valid answer here (the probe may not reference libstdc++); the
# README below falls back to "libstdc++".
PROG_GLIBCXX=$(grep -o 'GLIBCXX_[0-9][0-9.]*' <<<"$PROBE_SYMS" | sort -uV | tail -1 || true)

cat >"$STAGE/$NAME/README.md" <<EOF
# bezelc $VERSION (Linux x86_64)

The Bezel compiler. \`bin/bezelc\` has $LINKAGE.
${NEEDED:+Shared libraries it loads:
$NEEDED
}
## Requirements

\`bezelc\` compiles a \`.bzl\` file to a native object with its built-in LLVM
backend, then links the executable by running \`clang\` from \`PATH\`:

    clang <obj> -o <exe> -lm -lpthread <prefix>/lib/bezel/libbezel_runtime.a -lstdc++

So you need:

- \`clang\` on \`PATH\` (any recent version; built and tested with clang 21);
- the C and C++ development files clang links against: glibc (crt files,
  libm, libpthread) and libstdc++ (\`libstdc++.so\` for \`-lstdc++\`). The
  runtime archive was built against the libstdc++ headers of GCC $GCC_MAJOR
  and this build machine's glibc headers, so the programs it links need
  ${PROG_GLIBC/_/ } and ${PROG_GLIBCXX:-libstdc++} or newer, at link time and at
  run time, whatever the floor of \`bezelc\` itself.
  - Fedora: \`sudo dnf install clang libstdc++-devel\`
  - Debian/Ubuntu: \`sudo apt install clang\`. Its dependencies pull in
    \`libc6-dev\` and \`libstdc++-<N>-dev\`, which supplies the C++ library
    for \`-lstdc++\`. Do not name \`libstdc++-dev\`: on Ubuntu 22.04 and 24.04
    it is a virtual package with no installable candidate, so apt refuses
    the whole command.

## Layout

    bin/bezelc                    the compiler
    lib/bezel/libbezel_runtime.a  the Bezel runtime, found relative to bin/
    LICENSE                       Apache License 2.0

Keep \`bin/\` and \`lib/\` together. Symlinking \`bin/bezelc\` into \`PATH\` is
fine (the runtime is located through the real path of the executable).

## Usage

    bezelc hello.bzl -o hello && ./hello
    bezelc --help

Built from commit $(git rev-parse HEAD).
EOF

mkdir -p "$OUT_DIR"
TARBALL="$OUT_DIR/$NAME.tar.gz"
# Reproducible-ish archive: fixed order, owner and mtime (the commit time).
MTIME=$(git log -1 --format=%ct HEAD)
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$MTIME" \
  -C "$STAGE" -cf - "$NAME" | gzip -9 -n >"$TARBALL"
(cd "$OUT_DIR" && sha256sum "$NAME.tar.gz" >"$NAME.tar.gz.sha256")
rm -rf "$STAGE"

# ---- smoke test: the extracted binary runs and finds its runtime -----------
SMOKE="$OUT_DIR/smoke"
rm -rf "$SMOKE"; mkdir -p "$SMOKE"
tar -C "$SMOKE" -xzf "$TARBALL"
"$SMOKE/$NAME/bin/bezelc" --version
cat >"$SMOKE/hello.bzl" <<'BZL'
module Smoke;

public class Program
  public static method Main()
    Console.WriteLine("hello from bezel");
  end Main;
end Program;
BZL
(cd "$SMOKE" && "$SMOKE/$NAME/bin/bezelc" hello.bzl -o hello && ./hello) \
  || die "smoke test failed: the extracted compiler could not build hello.bzl"
rm -rf "$SMOKE"

echo "release_static: wrote $TARBALL ($(du -h "$TARBALL" | cut -f1))"
cat "$OUT_DIR/$NAME.tar.gz.sha256"
