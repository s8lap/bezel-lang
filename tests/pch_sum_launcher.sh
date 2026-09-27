#!/usr/bin/env bash
# Compiler launcher for the test-PCH owner target in relocatable-ccache mode
# (#5073, follow-up to #5010). Wired in tests/CMakeLists.txt as the
# CXX_COMPILER_LAUNCHER of bezel_tests_pch ONLY, and only when
# BEZEL_CCACHE_RELOCATABLE is ON.
#
# usage: pch_sum_launcher.sh <tree-root> <tree-root-real> <ccache> [KEY=VALUE...] -- <compiler> <args...>
#
# THE DEFECT IT FIXES. A clang PCH records the ABSOLUTE paths of its inputs
# (<tree>/build/tests/CMakeFiles/bezel_tests_pch.dir/cmake_pch.hxx{,.cxx}), and
# neither ccache base_dir nor -ffile-prefix-map rewrites them. Served from
# ccache, tree A's PCH lands in tree B still pointing into A; once A is removed
# every consumer fails with "malformed or corrupted precompiled file: could not
# find file". And a PCH compiled locally differs in bytes per tree, so its
# consumers -- which ccache keys on the PCH bytes -- miss in every new tree.
#
# THE FIX, in two halves:
#  1. The PCH itself is NEVER served from ccache: it is compiled directly
#     (CCACHE_DISABLE=1, which also neutralises a /usr/lib64/ccache masquerade
#     compiler), so it always references its own tree. One PCH compile per
#     tree; it is never shared.
#  2. After a successful PCH compile this writes <pch>.sum, and the consumers'
#     ccache runs with pch_external_checksum=true, which hashes <pch>.sum
#     INSTEAD of the PCH bytes. The .sum is content-derived and tree-independent:
#     sha256 over the compiler's --version, the PCH compile's argv, and the
#     sha256 of EVERY file in the PCH's -MD depfile, with the tree root (both
#     spellings) replaced by a placeholder. Two trees therefore get equal sums
#     exactly when their PCHs were built by the same compiler, from the same
#     flags, from byte-identical inputs -- so a consumer object cached in tree A
#     is served to tree B only when B's own PCH is equivalent to A's.
#
# STALENESS (the overriding requirement: a stale or foreign PCH must never be
# usable after a header change). The old <pch>.sum is deleted BEFORE the
# compiler runs, and the new one is renamed into place only after the compiler
# succeeded and every depfile entry was hashed. Any failure or interruption
# therefore leaves NO .sum, and ccache then falls back to hashing the PCH bytes
# themselves -- always correct, merely tree-local. The same fail-closed rule
# applies to anything this script cannot parse (a missing/odd depfile, an
# escaped path): no .sum, one warning line.
#
# Every non-PCH compile of the target (pch_owner.cpp) is passed straight to
# ccache with the given KEY=VALUE settings.
set -u

if [ "$#" -lt 4 ]; then
    echo "pch_sum_launcher.sh: usage: <tree> <tree-real> <ccache> [K=V...] -- <compiler> <args...>" >&2
    exit 2
fi
tree=$1; tree_real=$2; shift 2
launcher=()
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do launcher+=("$1"); shift; done
if [ "$#" -lt 2 ]; then
    echo "pch_sum_launcher.sh: missing '-- <compiler> <args...>'" >&2
    exit 2
fi
shift  # the --

is_pch=0; out=""; depfile=""
prev=""
for a in "$@"; do
    case "$a" in -emit-pch) is_pch=1 ;; esac
    case "$prev" in -o) out=$a ;; -MF) depfile=$a ;; esac
    prev=$a
done

if [ "$is_pch" -eq 0 ]; then
    exec "${launcher[@]}" "$@"
fi

warn() { echo "pch_sum_launcher.sh: warning: $*; no ${out:-<pch>}.sum written, PCH consumers will key on the PCH bytes (correct, but no cross-tree ccache hits)" >&2; }

if [ -z "$out" ]; then
    warn "no -o in the PCH compile"
    CCACHE_DISABLE=1 exec "$@"
fi

# Fail closed FIRST: from here on a stale .sum can never describe a new PCH.
rm -f -- "$out.sum" "$out.sum.tmp" || exit 1

CCACHE_DISABLE=1 "$@"
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

norm() {  # replace both spellings of the tree root with a fixed placeholder
    local s=$1
    s=${s//"$tree"/@TREE@}
    [ "$tree_real" = "$tree" ] || s=${s//"$tree_real"/@TREE@}
    printf '%s\n' "$s"
}

if [ -z "$depfile" ] || [ ! -f "$depfile" ]; then
    warn "no depfile (-MF) from the PCH compile"; exit 0
fi
# Make-syntax depfile: "target: dep dep \<newline> dep ...". Paths with
# escaped characters are not expected here; refuse rather than mis-split.
deptext=$(sed -e 's/\\$//' -- "$depfile" | tr '\n' ' ')
case "$deptext" in *\\*|*'$$'*) warn "escaped path in $depfile"; exit 0 ;; esac
case "$deptext" in *": "*) deptext=${deptext#*": "} ;; *) warn "unparsable $depfile"; exit 0 ;; esac
read -r -a deps <<<"$deptext"
if [ "${#deps[@]}" -eq 0 ]; then warn "empty depfile $depfile"; exit 0; fi
for d in "${deps[@]}"; do
    [ -f "$d" ] || { warn "depfile entry is not a file: $d"; exit 0; }
done

compiler_version=$(CCACHE_DISABLE=1 "$1" --version 2>&1) || { warn "'$1 --version' failed"; exit 0; }
dep_hashes=$(sha256sum -- "${deps[@]}") || { warn "hashing the PCH inputs failed"; exit 0; }

{
    echo "bezel-pch-sum v1"
    printf '%s\n' "$compiler_version"
    echo "--- argv"
    for a in "$@"; do norm "$a"; done
    echo "--- inputs"
    while IFS= read -r line; do norm "$line"; done <<<"$dep_hashes"
} | sha256sum | cut -d' ' -f1 >"$out.sum.tmp" || { rm -f -- "$out.sum.tmp"; warn "writing the sum failed"; exit 0; }
mv -f -- "$out.sum.tmp" "$out.sum" || { rm -f -- "$out.sum.tmp"; warn "renaming the sum failed"; exit 0; }
exit 0
