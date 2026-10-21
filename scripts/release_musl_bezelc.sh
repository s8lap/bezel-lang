#!/bin/sh
# release_musl_bezelc.sh -- RUNS INSIDE the pinned Alpine 3.23 container that
# scripts/release_static.sh --full starts; not meant to be run on the host.
#
# It builds the release bin/bezelc FULLY STATIC against musl (#6715): the
# existing CMake path (-DBEZEL_STATIC_RELEASE=ON -DBEZEL_STATIC_RELEASE_FULL=ON)
# with Alpine's clang 21 and static LLVM 21, musl, libstdc++/libgcc, zlib and
# zstd. glibc is LGPL-2.1 and a static link of it would carry a relink
# obligation; musl is MIT and carries none.
#
# WHY a second Alpine image. The 3.20 image that builds the self-contained
# toolchain (release_musl_stage.sh) tops out at LLVM 18, and bezelc needs
# LLVM 20 or newer (Intrinsic::getOrInsertDeclaration). Alpine 3.23 ships
# llvm21 21.1.2; the dev build and the gate use Fedora's LLVM 21.1.8.
#
# Environment: SRC (the repo, read-only), BUILD (writable CMake build dir,
# kept between runs), OUT (writable; receives bezelc and pins.txt), APKCACHE
# (writable apk cache, kept between runs), JOBS, VERSION.
#
# Bounding. Like release_musl_stage.sh, this build is bounded by JOBS and by
# the capped slice the container runs in (release_static.sh starts it through
# scripts/dev_heavy.sh with --cgroup-parent=bezel-heavy.slice). The host's
# build-token pool (scripts/build_token.sh, /var/tmp/bezel-scratch/jobserver)
# is not reachable from the container, so the launcher is told to run each job
# directly (BEZEL_JOBSERVER=off, as gate.sh does) and -j"$JOBS" bounds it.
set -eu

: "${SRC:?}" "${BUILD:?}" "${OUT:?}" "${APKCACHE:?}" "${VERSION:?}"
JOBS=${JOBS:-8}

# ---- pinned packages (alpine 3.23.6; the versions the #6715 spike used) -----
# bash: CMakeLists.txt puts scripts/build_token.sh (a bash script) in front of
# every compile and link. musl-dev and gcc come in as dependencies anyway; they
# are pinned because their libc.a and libstdc++.a/libgcc.a are linked into
# bezelc and named in its LICENSES/README.
PKGS="clang21=21.1.2-r2 llvm21-dev=21.1.2-r1 llvm21-static=21.1.2-r1
  musl-dev=1.2.5-r24 gcc=15.2.0-r2 libstdc++-dev=15.2.0-r2
  zlib-static=1.3.2-r1 zstd-static=1.5.7-r2
  cmake=4.1.3-r0 samurai=1.2-r7 bash=5.3.3-r1"
# The apks are cached in APKCACHE, so a re-run does not download 360 MB again.
ln -sf "$APKCACHE" /etc/apk/cache
ok=0
for i in 1 2 3; do
  # shellcheck disable=SC2086
  if apk add $PKGS >/tmp/apk.log 2>&1; then ok=1; break; fi
  echo "musl_bezelc: apk add attempt $i failed" >&2; tail -3 /tmp/apk.log >&2
done
[ "$ok" = 1 ] || { tail -20 /tmp/apk.log >&2; echo "musl_bezelc: apk add failed" >&2; exit 1; }
printf '%s\n' $PKGS >"$OUT/pins.txt"

LLVM_PREFIX=/usr/lib/llvm21
export PATH="$LLVM_PREFIX/bin:$PATH"

# ---- Alpine packaging defect: archives LLVMExports names but does not ship --
# Alpine's llvm21 LLVMExports-*.cmake declares imported targets for LLVM's test
# libraries, whose archives neither llvm21-dev nor llvm21-static installs, and
# find_package(LLVM) fails on the missing files. The same defect is why
# release_musl_stage.sh builds ld.lld without CMake for llvm17. bezelc links
# none of them, so empty archives satisfy the existence check. Only these four
# are stubbed: any other missing archive is a different problem and stops here.
STUBS="libLLVMTestingAnnotations.a libLLVMTestingSupport.a libllvm_gtest.a libllvm_gtest_main.a"
for f in $(grep -ho '_IMPORT_PREFIX}/lib/[^"]*\.a' "$LLVM_PREFIX"/lib/cmake/llvm/LLVMExports-*.cmake \
           | sed "s|^_IMPORT_PREFIX}|$LLVM_PREFIX|" | sort -u); do
  [ -e "$f" ] && continue
  case " $STUBS " in
    *" $(basename "$f") "*) printf '!<arch>\n' >"$f"; echo "musl_bezelc: stubbed $f" ;;
    *) echo "musl_bezelc: LLVMExports names $f, which is missing and not a known stub" >&2; exit 1 ;;
  esac
done

clang++ --version | head -1
export BEZEL_JOBSERVER=off
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DBEZEL_STATIC_RELEASE=ON \
  -DBEZEL_STATIC_RELEASE_FULL=ON \
  -DBEZEL_PROGRAM_LINKER=default \
  -DBEZEL_VERSION_OVERRIDE="$VERSION" >"$BUILD/release-configure.log" 2>&1 \
  || { tail -40 "$BUILD/release-configure.log" >&2; echo "musl_bezelc: configure failed" >&2; exit 1; }
grep 'Bezel:' "$BUILD/release-configure.log" || true
cmake --build "$BUILD" -j"$JOBS" --target bezelc

cp "$BUILD/bezelc" "$OUT/bezelc"
echo "musl_bezelc: bezelc ready ($(wc -c <"$OUT/bezelc") bytes)"
