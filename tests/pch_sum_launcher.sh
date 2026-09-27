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
# RACES AND SILENT STALENESS (#5085) -- see the stamp check below, and
# tests/pch_verify.sh, which re-checks <pch>.inputs before every build and
# deletes a PCH whose inputs changed since it was built (make alone misses a
# package update that installs headers with OLD mtimes, and clang does not
# revalidate system headers from a PCH).
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

norm() {  # replace both spellings of the tree root with a fixed placeholder
    local s=$1
    s=${s//"$tree"/@TREE@}
    [ "$tree_real" = "$tree" ] || s=${s//"$tree_real"/@TREE@}
    printf '%s\n' "$s"
}

# #5085: an input that changes WHILE the PCH compiles (a `dnf upgrade`
# replacing a same-size header) must not yield a .sum that describes the new
# content next to a .pch built from the old: clang does not revalidate system
# headers from a PCH at all (and user headers by size only under
# -fno-pch-timestamp), so a consumer would compile against the stale PCH and
# be stored under the key of a correct tree. So: take a stamp BEFORE the
# compile, hash the inputs AFTER it, and only then check that no input has an
# mtime or ctime AT OR AFTER the stamp's (nanosecond stat, inclusive, so an
# edit in the stamp's own timestamp tick counts; a false positive only costs a
# recompile). ctime also catches a rename-install that keeps the package's old
# mtime. If one has, the hashes may not match what clang read: recompile
# (the header is normally stable again), and after $attempts tries fail the
# build loudly with no .pch, .sum or .inputs left behind.
attempts=3
stamp="$out.stamp"
cleanup() { rm -f -- "$out.sum.tmp" "$out.inputs.tmp" "$stamp"; }
for attempt in $(seq 1 "$attempts"); do
    # Fail closed FIRST: from here on a stale .sum can never describe a new
    # PCH, and a stale .inputs can never vouch for one (tests/pch_verify.sh).
    rm -f -- "$out.sum" "$out.inputs" || exit 1
    cleanup
    : >"$stamp" && ref=$(stat -c %.9Y -- "$stamp") || exit 1

    CCACHE_DISABLE=1 "$@"
    rc=$?
    if [ "$rc" -ne 0 ]; then cleanup; exit "$rc"; fi

    if [ -z "$depfile" ] || [ ! -f "$depfile" ]; then
        cleanup; warn "no depfile (-MF) from the PCH compile"; exit 0
    fi
    # Make-syntax depfile: "target: dep dep \<newline> dep ...". Paths with
    # escaped characters are not expected here; refuse rather than mis-split.
    deptext=$(sed -e 's/\\$//' -- "$depfile" | tr '\n' ' ')
    case "$deptext" in *\\*|*'$$'*) cleanup; warn "escaped path in $depfile"; exit 0 ;; esac
    case "$deptext" in *": "*) deptext=${deptext#*": "} ;; *) cleanup; warn "unparsable $depfile"; exit 0 ;; esac
    read -r -a deps <<<"$deptext"
    if [ "${#deps[@]}" -eq 0 ]; then cleanup; warn "empty depfile $depfile"; exit 0; fi
    for d in "${deps[@]}"; do
        [ -f "$d" ] || { cleanup; warn "depfile entry is not a file: $d"; exit 0; }
    done

    compiler_version=$(CCACHE_DISABLE=1 "$1" --version 2>&1) || { cleanup; warn "'$1 --version' failed"; exit 0; }
    dep_hashes=$(sha256sum -- "${deps[@]}") || { cleanup; warn "hashing the PCH inputs failed"; exit 0; }

    times=$(stat -c '%.9Y %.9Z %n' -- "${deps[@]}") || { cleanup; warn "stat of the PCH inputs failed"; exit 0; }
    changed=$(awk -v r="$ref" '$1 >= r || $2 >= r { print substr($0, index($0, $3)); exit }' <<<"$times")
    if [ -z "$changed" ]; then break; fi
    echo "pch_sum_launcher.sh: PCH input changed during the PCH compile (attempt $attempt/$attempts): $changed" >&2
    if [ "$attempt" -eq "$attempts" ]; then
        cleanup
        rm -f -- "$out"
        echo "pch_sum_launcher.sh: error: PCH inputs kept changing; removed $out. Re-run the build once the headers are stable." >&2
        exit 1
    fi
done

{
    echo "bezel-pch-sum v1"
    printf '%s\n' "$compiler_version"
    echo "--- argv"
    for a in "$@"; do norm "$a"; done
    echo "--- inputs"
    while IFS= read -r line; do norm "$line"; done <<<"$dep_hashes"
} | sha256sum | cut -d' ' -f1 >"$out.sum.tmp" || { cleanup; warn "writing the sum failed"; exit 0; }
# .inputs: the exact input hashes this PCH was built from, in `sha256sum -c`
# form, for tests/pch_verify.sh (the within-tree half of #5085). Written
# BEFORE the .sum is published, so a published .sum always has one.
printf '%s\n' "$dep_hashes" >"$out.inputs.tmp" \
    && mv -f -- "$out.inputs.tmp" "$out.inputs" \
    && mv -f -- "$out.sum.tmp" "$out.sum" \
    || { rm -f -- "$out.sum" "$out.inputs"; cleanup; warn "publishing the sum failed"; exit 0; }
cleanup
exit 0
