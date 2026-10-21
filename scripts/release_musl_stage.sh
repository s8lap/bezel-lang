#!/bin/sh
# release_musl_stage.sh -- RUNS INSIDE the pinned Alpine container that
# scripts/release_static.sh starts; not meant to be run on the host.
#
# It builds the self-contained toolchain bundle (#6654) that a release bezelc
# uses to link FULLY STATIC executables with no clang on the machine:
#
#   $OUT/x86_64-linux-musl/
#     bin/ld.lld                         a static ld.lld (no dynamic deps)
#     crt1.o crti.o crtn.o libc.a        musl
#     libc++.a libc++abi.a libunwind.a   LLVM's C++ runtime
#     libclang_rt.builtins-x86_64.a      compiler-rt builtins
#     libpthread.a librt.a libdl.a libm.a  empty archives (musl's stubs) for
#                                        the dependent-library hints
#     libbezel_runtime.a                 src/runtime built against musl+libc++
#     LICENSES/                          the licenses of what is bundled
#
# Driver::selfContainedLinkArgs (src/driver/Driver.cpp) is the link line.
#
# WHY a static ld.lld built here. Alpine (and Fedora) ship lld only as a
# dynamically linked binary over libLLVM.so and as shared liblld*.so, so there
# is nothing static to extract, and embedding lld in bezelc would need static
# lld archives the Fedora build machine does not have. So the ELF driver of
# lld 17.0.6 (the version of Alpine's llvm17, whose static libraries it links)
# is compiled here from the pinned upstream source tarball, without CMake
# (llvm17's exported CMake package names test archives the package does not
# ship), and with two cuts that take it from 59 MB to 13 MB (MEASURED,
# stripped): no LLVM target (the targets lld initialises exist only for LTO)
# and no LTO (lld/ELF/LTO.cpp is replaced by a stub that rejects bitcode
# input). Bezel links native objects only, so neither is ever used.
#
# Environment: SRC (the repo, read-only), DL (the downloaded, sha256-checked
# tarballs), OUT (writable), JOBS.
set -eu

: "${SRC:?}" "${DL:?}" "${OUT:?}"
JOBS=${JOBS:-8}
LLD_VERSION=17.0.6

# ---- pinned packages (the versions the #6654 spike used, alpine 3.20) ------
apk add --no-cache \
  clang17=17.0.6-r1 compiler-rt=17.0.6-r1 musl-dev=1.2.5-r3 \
  libc++-dev=17.0.6-r1 libc++-static=17.0.6-r1 \
  llvm-libunwind-dev=17.0.6-r1 llvm-libunwind-static=17.0.6-r1 \
  llvm17-dev=17.0.6-r2 llvm17-static=17.0.6-r2 \
  zlib-static=1.3.2-r0 zstd-static=1.5.6-r0 \
  binutils=2.42-r1 file=5.45-r1 >/tmp/apk.log 2>&1 \
  || { tail -20 /tmp/apk.log >&2; echo "musl_stage: apk add failed" >&2; exit 1; }

LC=/usr/lib/llvm17/bin/llvm-config
BUILTINS=$(clang --print-file-name=libclang_rt.builtins-x86_64.a)
[ -f "$BUILTINS" ] || BUILTINS=$(clang -print-libgcc-file-name --rtlib=compiler-rt)
[ -f "$BUILTINS" ] || { echo "musl_stage: compiler-rt builtins not found" >&2; exit 1; }

W=/tmp/stage
B="$OUT/x86_64-linux-musl"
rm -rf "$W" "$B"
mkdir -p "$W" "$B/bin" "$B/LICENSES"

# ---- 1. the runtime, against musl + libc++ ----------------------------------
# The source list is CMakeLists.txt's add_library(bezel_runtime ...), so a file
# added there is built here too. Flags: the release build's (-O3 -DNDEBUG) and
# the runtime target's own (-std=c++20 -pthread); no debug info.
RT_SRCS=$(sed -n '/^add_library(bezel_runtime STATIC/,/)/p' "$SRC/CMakeLists.txt" \
  | grep -o 'src/runtime/[A-Za-z0-9_]*\.cpp')
[ -n "$RT_SRCS" ] || { echo "musl_stage: no runtime sources in CMakeLists.txt" >&2; exit 1; }
mkdir -p "$W/rt"
for f in $RT_SRCS; do echo "$f"; done \
  | xargs -P "$JOBS" -I{} sh -c 'clang++ -stdlib=libc++ -std=c++20 -pthread -O3 -DNDEBUG \
      -ffile-prefix-map="$1"=. -c "$1/{}" -o "$2/rt/$(basename {} .cpp).o"' _ "$SRC" "$W"
ar rcsD "$B/libbezel_runtime.a" "$W"/rt/*.o
echo "musl_stage: runtime $(echo "$RT_SRCS" | wc -w) sources -> $(wc -c <"$B/libbezel_runtime.a") bytes"

# ---- 2. a static ld.lld ------------------------------------------------------
cd "$W"
for t in lld cmake libunwind; do
  tar xf "$DL/$t-$LLD_VERSION.src.tar.xz"
  mv "$t-$LLD_VERSION.src" "$t"
done
mkdir -p gen/lld/Common ov/llvm/Config obj
# No LLVM targets: empty target lists for TargetSelect.h's InitializeAll*, and
# no native-target macros (their inline initialisers would name X86).
for f in Targets AsmPrinters AsmParsers Disassemblers TargetMCAs TargetExegesis; do
  sed -E '/^LLVM_[A-Z_]+\(/d' "/usr/lib/llvm17/include/llvm/Config/$f.def" >"ov/llvm/Config/$f.def"
done
grep -v 'define LLVM_NATIVE_' /usr/lib/llvm17/include/llvm/Config/llvm-config.h \
  >ov/llvm/Config/llvm-config.h
/usr/lib/llvm17/bin/llvm-tblgen -gen-opt-parser-defs -I /usr/lib/llvm17/include \
  lld/ELF/Options.td -o gen/Options.inc
: >gen/VCSVersion.inc
echo "#define LLD_VERSION_STRING \"$LLD_VERSION\"" >gen/lld/Common/Version.inc
cat >gen/lto_stub.cpp <<'CPP'
// Replaces lld/ELF/LTO.cpp: this ld.lld links native objects only.
#include "LTO.h"
#include "InputFiles.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/LTO/LTO.h"
using namespace lld;
using namespace lld::elf;
BitcodeCompiler::BitcodeCompiler() = default;
BitcodeCompiler::~BitcodeCompiler() = default;
void BitcodeCompiler::add(BitcodeFile &f) {
  error(toString(&f) + ": LLVM bitcode input is not supported (this ld.lld is built without LTO)");
}
std::vector<InputFile *> BitcodeCompiler::compile() { return {}; }
CPP
cat >gen/main.cpp <<'CPP'
// ld.lld with only the ELF driver; upstream lld.cpp's non-test path.
#include "lld/Common/Driver.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"
namespace lld {
int unsafeLldMain(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
                  llvm::raw_ostream &stderrOS, llvm::ArrayRef<DriverDef> drivers,
                  bool exitEarly);
}
LLD_HAS_DRIVER(elf)
int main(int argc, char **argv) {
  llvm::InitLLVM x(argc, argv);
  return lld::unsafeLldMain(llvm::ArrayRef<const char *>(argv, argv + argc),
                            llvm::outs(), llvm::errs(),
                            {{lld::Gnu, &lld::elf::link}}, /*exitEarly=*/true);
}
CPP
LLD_CXXFLAGS="-Iov -Igen -Illd/include -Illd/ELF $($LC --cxxflags) -O2 -DNDEBUG -w"
export LLD_CXXFLAGS
{ ls lld/ELF/*.cpp lld/ELF/Arch/*.cpp | grep -v '^lld/ELF/LTO\.cpp$' | sed 's/$/ elf/'
  ls lld/Common/*.cpp | sed 's/$/ common/'
  echo "gen/lto_stub.cpp gen"
  echo "gen/main.cpp gen"; } >srcs.txt
xargs -P "$JOBS" -n 2 sh -c \
  'clang++ $LLD_CXXFLAGS -c "$0" -o "obj/$(basename "$0" .cpp).$1.o" || exit 255' <srcs.txt
LLD_LIBS=$($LC --link-static --libs binaryformat bitwriter core debuginfodwarf \
  demangle lto mc object option passes support targetparser)
# libxml2 is only for the LLVMWindowsManifest component, which is not linked.
LLD_SYS=$($LC --link-static --system-libs | sed 's/-lxml2//')
# shellcheck disable=SC2086
clang++ -static -O2 -Wl,--gc-sections -s -L"$($LC --libdir)" -o "$B/bin/ld.lld" \
  obj/*.o $LLD_LIBS $LLD_SYS
file "$B/bin/ld.lld" | grep -q 'statically linked' \
  || { file "$B/bin/ld.lld" >&2; echo "musl_stage: ld.lld is not static" >&2; exit 1; }
"$B/bin/ld.lld" --version

# ---- 3. musl, the C++ runtime, builtins ------------------------------------
for f in crt1.o crti.o crtn.o libc.a libc++.a libc++abi.a libunwind.a; do
  cp "/usr/lib/$f" "$B/$f"
done
cp "$BUILTINS" "$B/libclang_rt.builtins-x86_64.a"
# musl's libpthread/librt/libdl/libm are empty archives; the static archives'
# dependent-library hints name them, and lld fails on a hint it cannot find.
for l in pthread rt dl m; do printf '!<arch>\n' >"$B/lib$l.a"; done

# ---- 4. licenses -------------------------------------------------------------
cp "$W/lld/LICENSE.TXT" "$B/LICENSES/LLVM-LICENSE.TXT"
cp "$DL/musl-1.2.5-COPYRIGHT" "$B/LICENSES/musl-COPYRIGHT"
# bin/ld.lld statically links the zlib and zstd archives of the pinned Alpine
# packages (zlib-static, zstd-static; #6713). Their notices are the pinned
# upstream LICENSE files of those versions (sha256-checked by release_static.sh).
cp "$DL/zlib-1.3.2-LICENSE" "$B/LICENSES/zlib-LICENSE"
cp "$DL/zstd-1.5.6-LICENSE" "$B/LICENSES/zstd-LICENSE"
cat >"$B/LICENSES/README" <<'EOF'
Third-party components bundled in this directory:

  bin/ld.lld, libc++.a, libc++abi.a, libunwind.a,
  libclang_rt.builtins-x86_64.a   LLVM 17.0.6 (lld, libc++, libc++abi,
                                  libunwind, compiler-rt): Apache License 2.0
                                  with LLVM Exceptions, see LLVM-LICENSE.TXT
  crt1.o crti.o crtn.o libc.a     musl 1.2.5: MIT, see musl-COPYRIGHT
  bin/ld.lld (statically linked)  zlib 1.3.2: zlib License, see zlib-LICENSE
                                  zstd 1.5.6: BSD-3-Clause (zstd is dual
                                  BSD / GPLv2; the BSD terms are used),
                                  see zstd-LICENSE
  bin/ld.lld (statically linked)  libstdc++ and libgcc (Alpine gcc
                                  13.2.1_git20240309; the stage's clang++
                                  links them statically): GPL-3.0 with the
                                  GCC Runtime Library Exception, see
                                  gcc-COPYING3 and gcc-COPYING.RUNTIME

Programs bezelc links in self-contained mode contain parts of musl, libc++,
libc++abi, libunwind and compiler-rt.
EOF

echo "musl_stage: bundle ready"
ls -la "$B" "$B/bin"
