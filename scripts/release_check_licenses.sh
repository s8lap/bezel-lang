#!/bin/bash
# release_check_licenses.sh -- fails when a third-party component statically
# linked into a staged release binary has no licence file in the staged tree
# (#6713). Run by scripts/release_static.sh on the tree it is about to pack.
#
# Usage: release_check_licenses.sh STAGED_ROOT      (the <name>/ directory)
#
# The component list is the one MEASURED for #6713 and #6715 from the link
# lines and `strings` of a --full release build (bezelc static on musl). Each rule is: a marker string that is
# present in the binary only when the component is linked in, and the licence
# file that must then exist. A component this table does not know is not
# detected: extend the table when a library is added to a link.
#
#   marker in the binary           component              licence file
#   bezelc:
#     (always)                     LLVM 21 (static)       LICENSES/LLVM-LICENSE.TXT
#     "basic_string::_M_create"    libstdc++, libgcc      LICENSES/gcc-COPYING3, gcc-COPYING.RUNTIME
#     "Mark Adler"                 zlib 1.3.2             LICENSES/zlib-LICENSE
#     "Src size is incorrect"      zstd                   LICENSES/zstd-LICENSE
#     "No error information"       musl (strerror(0))     LICENSES/musl-COPYRIGHT
#   lib/bezel/x86_64-linux-musl/bin/ld.lld:
#     (always)                     LLVM 17 lld, musl      LLVM-LICENSE.TXT, musl-COPYRIGHT
#     "basic_string::_M_create"    libstdc++, libgcc      gcc-COPYING3, gcc-COPYING.RUNTIME
#     "Mark Adler"                 zlib                   zlib-LICENSE
#     "Src size is incorrect"      zstd                   zstd-LICENSE
set -u
ROOT=${1:?usage: release_check_licenses.sh STAGED_ROOT}
FAIL=0

# need DIR MARKER-PRESENT(0/1) FILE COMPONENT
need() {
  local dir=$1 present=$2 file=$3 what=$4
  [ "$present" = 1 ] || return 0
  if [ ! -s "$dir/$file" ]; then
    echo "release_check_licenses: $what is linked in but $dir/$file is missing" >&2
    FAIL=1
  fi
}
# ---- the rule table: DATA, edit here ---------------------------------------
# Each row: ROOT-RELATIVE-LICENCE-DIR | MARKER | FILE | COMPONENT
#   MARKER  ALWAYS       the component is always linked in
#           STATIC       the binary is a statically linked executable
#           str:<text>   `strings` of the binary contains <text>
#   FILE    a licence file that must exist (non-empty) in the dir, or
#           readme:<text>, meaning the dir's README must contain <text>
BEZELC_RULES='
LICENSES|ALWAYS|LLVM-LICENSE.TXT|LLVM (bezelc)
LICENSES|str:basic_string::_M_create|gcc-COPYING3|libstdc++/libgcc (bezelc)
LICENSES|str:basic_string::_M_create|gcc-COPYING.RUNTIME|libstdc++/libgcc (bezelc)
LICENSES|str:Mark Adler|zlib-LICENSE|zlib (bezelc)
LICENSES|str:Src size is incorrect|zstd-LICENSE|zstd (bezelc)
LICENSES|str:No error information|musl-COPYRIGHT|musl (bezelc, static)
'
LLD_RULES='
lib/bezel/x86_64-linux-musl/LICENSES|ALWAYS|LLVM-LICENSE.TXT|LLVM (ld.lld, libc++, libunwind, compiler-rt)
lib/bezel/x86_64-linux-musl/LICENSES|ALWAYS|musl-COPYRIGHT|musl (libc.a, ld.lld)
lib/bezel/x86_64-linux-musl/LICENSES|str:basic_string::_M_create|gcc-COPYING3|libstdc++/libgcc (ld.lld)
lib/bezel/x86_64-linux-musl/LICENSES|str:basic_string::_M_create|gcc-COPYING.RUNTIME|libstdc++/libgcc (ld.lld)
lib/bezel/x86_64-linux-musl/LICENSES|str:Mark Adler|zlib-LICENSE|zlib (ld.lld)
lib/bezel/x86_64-linux-musl/LICENSES|str:Src size is incorrect|zstd-LICENSE|zstd (ld.lld)
'

# apply_rules BINARY RULES
apply_rules() {
  local bin=$1 rules=$2 dir marker file what present out
  out=$(strings -a "$bin") || { echo "release_check_licenses: strings failed on $bin" >&2; FAIL=1; return; }
  while IFS='|' read -r dir marker file what; do
    [ -n "$dir" ] || continue
    case "$marker" in
      ALWAYS) present=1 ;;
      STATIC) present=0; file "$bin" | grep -q 'statically linked' && present=1 ;;
      str:*)  present=0; grep -qF -- "${marker#str:}" <<<"$out" && present=1 ;;
      *) echo "release_check_licenses: bad marker '$marker'" >&2; FAIL=1; continue ;;
    esac
    [ "$present" = 1 ] || continue
    case "$file" in
      readme:*)
        grep -qF -- "${file#readme:}" "$ROOT/$dir/README" 2>/dev/null || {
          echo "release_check_licenses: $what: $dir/README does not mention '${file#readme:}'" >&2; FAIL=1; } ;;
      *)
        [ -s "$ROOT/$dir/$file" ] || {
          echo "release_check_licenses: $what is linked in but $dir/$file is missing" >&2; FAIL=1; } ;;
    esac
  done <<<"$rules"
}
# every licence file must be listed in its README table.
readme_covers() {
  local dir=$ROOT/$1 f
  [ -s "$dir/README" ] || { echo "release_check_licenses: $1/README is missing" >&2; FAIL=1; return; }
  for f in "$dir"/*; do
    [ "$(basename "$f")" = README ] && continue
    grep -qF "$(basename "$f")" "$dir/README" || {
      echo "release_check_licenses: $f is not listed in $1/README" >&2; FAIL=1; }
  done
}

[ -x "$ROOT/bin/bezelc" ] || { echo "release_check_licenses: no $ROOT/bin/bezelc" >&2; exit 1; }
apply_rules "$ROOT/bin/bezelc" "$BEZELC_RULES"
readme_covers LICENSES
if [ -d "$ROOT/lib/bezel/x86_64-linux-musl" ]; then
  [ -x "$ROOT/lib/bezel/x86_64-linux-musl/bin/ld.lld" ] || { echo "release_check_licenses: no bundled ld.lld" >&2; exit 1; }
  apply_rules "$ROOT/lib/bezel/x86_64-linux-musl/bin/ld.lld" "$LLD_RULES"
  readme_covers lib/bezel/x86_64-linux-musl/LICENSES
fi

[ "$FAIL" = 0 ] && echo "release_check_licenses: ok"
exit "$FAIL"
