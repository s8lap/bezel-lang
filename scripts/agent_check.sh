#!/usr/bin/env bash
#
# agent_check.sh — the ONE build+test command a producer/fix lane should use.
#
# Why this exists: a lane must build and run a TARGETED test subset — resource-
# capped, foreground, and WITHOUT the footguns that cost real time in practice:
#
#   * running the FULL suite (tests/run_parallel.sh / all of bezel_tests) — that
#     is the integration GATE's job; a lane running it spawns ~100 fork+link+exec
#     tasks and only wastes the shared budget.
#   * `--gtest_filter` — the test binary IGNORES it (see tests/test_main.cpp) and
#     silently runs the WHOLE suite.
#   * background build-waiters — `dev_heavy build` is already foreground/blocking;
#     a lane that "arms a waiter and yields" just stalls.
#   * a NEW test *.cpp not being picked up — CMake globs tests at CONFIGURE time,
#     so `cmake --build` alone can't see it. This wrapper reconfigures first.
#   * stray probe outputs (bezelc -o <name>) landing in the repo root.
#
# This wrapper encodes the correct protocol so a lane runs ONE command and cannot
# get it wrong. Resource-capping is delegated to scripts/dev_heavy.sh (the shared
# bezel-heavy.slice cgroup that bounds TOTAL cpu+mem across all concurrent lanes
# without serializing) — this script adds NO lock of its own.
#
# The suite is several per-area binaries (bezel_tests_<group>, see
# tests/CMakeLists.txt). You do NOT need to know which one holds your test: this
# wrapper enumerates the binaries (~30 ms each) to find every binary a filter
# matches and runs exactly those. `--area <group>` narrows the BUILD too, which
# is the point of the split — after a src/ change, relinking one area's binary
# instead of all of them is the difference between ~50 MB and ~600 MB of link
# output on an I/O-contended box.
#
# Usage:
#   scripts/agent_check.sh <filter> [filter2 ...]    build the suite + run ONLY
#                                                     BEZEL_TEST_FILTER=<filter>
#                                                     (substring match on gtest
#                                                     suite-or-name; NOT the file
#                                                     basename). One or more.
#   scripts/agent_check.sh --area <group> <filter>   same, but build+run ONLY
#                                                     that area's binary (groups:
#                                                     see build/tests/test_binaries.txt).
#   scripts/agent_check.sh --select [BASE]            auto-derive the subset from
#                                                     the lane's OWN diff vs BASE
#                                                     (default: merge-base HEAD
#                                                     against the branch's own
#                                                     @{upstream}, falling back
#                                                     to origin/main then
#                                                     HEAD~1 — fix #3725; NEVER
#                                                     the bare local `main`,
#                                                     which nothing fast-
#                                                     forwards) via select_tests.py
#                                                     --variant lane (#2100), and
#                                                     run it via BEZEL_TEST_LIST.
#                                                     Still a targeted lane check
#                                                     — NOT acceptance; the gate
#                                                     remains the authority.
#   scripts/agent_check.sh --lit [--area <group>] <filter>
#                                                     same, and ALSO run the lit
#                                                     diagnostic track (opt-in;
#                                                     once per commit, see below)
#   scripts/agent_check.sh --build-only               reconfigure + build bezel_tests
#   scripts/agent_check.sh --probe FILE.bzl [args..]  compile ONE .bzl with bezelc
#                                                     (output -> the scratch dir)
#   scripts/agent_check.sh --probe --asan FILE.bzl    compile+RUN one .bzl under
#                                                     ASan/LSan with a hang timeout
#                                                     (for memory/disposal fixes)
#
# It NEVER runs the full suite. For whole-suite validation, that is the gate:
# scripts/gate.sh (coordinator only).
#
# Scratch dir (per #3547): logs and --probe outputs go to
# /var/tmp/bezel-scratch/agent-probe/<worktree-basename>, so concurrent lanes in
# distinct worktrees no longer share one directory and clobber each other's
# configure/test logs and --probe object/output files. Set BEZEL_AGENT_SCRATCH
# to override the scratch dir entirely (e.g. to opt back into one shared
# location). The effective scratch dir is printed once at startup.
#
# Exit code: non-zero if the build fails or ANY requested filter has a failure.

set -uo pipefail

# --- locate the repo/worktree root (so this works from any agent worktree) ----
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "agent_check: not inside a git worktree" >&2; exit 2; }
# NB: do NOT `cd "$ROOT"` — every build/test path below is absolute, and staying
# in the caller's cwd keeps a RELATIVE `--probe FILE.bzl` resolving where the
# agent expects (its own cwd), not the repo root.
DH="$ROOT/scripts/dev_heavy.sh"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${JOBS:-16}"
# per #3547: qualify by worktree basename so concurrent lanes (distinct
# worktrees under /mnt/fast/worktrees/) don't clobber each other's configure/test
# logs and --probe outputs in one shared directory. BEZEL_AGENT_SCRATCH
# overrides entirely (e.g. to opt back into the old shared location).
SCRATCH="${BEZEL_AGENT_SCRATCH:-/var/tmp/bezel-scratch/agent-probe/$(basename "$ROOT")}"
mkdir -p "$SCRATCH"
echo "agent_check: scratch dir: $SCRATCH" >&2

die() { echo "agent_check: $*" >&2; exit 2; }
[ -x "$DH" ] || die "scripts/dev_heavy.sh not found/executable at $DH"

# --- the per-area test binaries (CMake-generated manifest) --------------------
# tests/CMakeLists.txt emits build/tests/test_binaries.txt as
# "<group> <TAB> <path> <TAB> <#sources>" for every binary it built, largest
# first. Reading it (rather than hardcoding names) is why a new area can never
# be missed here. AREA, when set, restricts BOTH the build target and the run.
MANIFEST="$BUILD_DIR/tests/test_binaries.txt"
AREA=""
test_binaries() {  # -> one absolute path per line
  [ -r "$MANIFEST" ] || return 1
  if [ -n "$AREA" ]; then
    awk -F'\t' -v a="$AREA" '$1==a {print $2}' "$MANIFEST"
  else
    cut -f2 "$MANIFEST"
  fi
}
test_groups() { [ -r "$MANIFEST" ] && cut -f1 "$MANIFEST" | paste -sd' ' -; }

# --- reconfigure (picks up any newly-added test *.cpp) + build a target -------
# Reconfigure is quiet unless it fails; the build hard-stops on failure so a
# stale binary can never be mistaken for a passing run.
# Skip the reconfigure when nothing it exists for can have changed: CMake
# globs tests/ at CONFIGURE time (a NEW or REMOVED test *.cpp needs it), and
# CMakeLists.txt edits re-run cmake by themselves inside the build. So stamp the
# sorted test-source path list (names only) and reconfigure only when it
# differs, or on a tree never configured by this script (no cache, not
# RelWithDebInfo per #3013, or no manifest).
TESTLIST_STAMP="$BUILD_DIR/.agent_check_testlist.sha"
testlist_sha() { (cd "$ROOT" && find tests -name '*.cpp' | LC_ALL=C sort | sha1sum | cut -d' ' -f1); }
reconfigure_needed() {
  grep -qx 'CMAKE_BUILD_TYPE:STRING=RelWithDebInfo' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || return 0
  [ -r "$MANIFEST" ] || return 0
  [ "$(cat "$TESTLIST_STAMP" 2>/dev/null)" = "$(testlist_sha)" ] || return 0
  return 1
}
reconfigure() {
  reconfigure_needed || return 0
  local sha; sha="$(testlist_sha)"
  # -DCMAKE_BUILD_TYPE=RelWithDebInfo is REQUIRED (#3013): without it CMake leaves the build type
  # empty, which drops -DNDEBUG and leaves asserts live — 55 of 59 lane trees
  # were in that state, validating a different bezelc than the gate accepts.
  "$DH" build cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$SCRATCH/agent_configure.log" 2>&1 \
    || { echo "agent_check: CONFIGURE FAILED (see $SCRATCH/agent_configure.log):" >&2
         tail -20 "$SCRATCH/agent_configure.log" >&2; exit 1; }
  printf '%s\n' "$sha" > "$TESTLIST_STAMP"
}
# Stale-binary guard (origin: 0bd1fb11, "a failed/partial build can never leave
# an old binary behind"). It used to `rm -f` every binary before EVERY build,
# forcing a relink (~2 s sema, ~9 s codegen) even when nothing changed. Now the
# build system decides and we PROVE the result: after the build each binary
# must be at least as new as libbezel_core.a (all it links from); an older one
# (the dependency graph missed a relink) is removed and rebuilt once, and a
# still-stale or missing binary is a hard failure.
build_target() {  # $1 = cmake target ; $2 = binary to verify after the build
  local target="$1" bin="$2" core_a="$BUILD_DIR/libbezel_core.a"
  echo "== agent_check: build $target (dev_heavy, foreground) =="
  reconfigure
  "$DH" build cmake --build "$BUILD_DIR" -j"$JOBS" --target "$target" \
    || { echo "agent_check: BUILD FAILED ($target)" >&2; exit 1; }
  if [ -n "$bin" ] && [ -f "$core_a" ] && [ -e "$bin" ] && [ "$bin" -ot "$core_a" ]; then
    echo "agent_check: $bin is OLDER than libbezel_core.a - forcing a relink" >&2
    rm -f "$bin"
    "$DH" build cmake --build "$BUILD_DIR" -j"$JOBS" --target "$target" \
      || { echo "agent_check: BUILD FAILED ($target, forced relink)" >&2; exit 1; }
  fi
  [ -z "$bin" ] || [ -x "$bin" ] || { echo "agent_check: $bin not produced" >&2; exit 1; }
}

# Build the test suite (all areas, or just --area's) with the same stale-binary
# guard the single-binary version had: EVERY binary we are about to run is
# removed first, so a failed/partial build can never leave an old one behind to
# be mistaken for a passing run. The manifest is re-read AFTER the build (a
# fresh tree has none before it) and every listed binary must exist.
build_tests() {
  local target=bezel_tests b n=0
  [ -n "$AREA" ] && target="bezel_tests_$AREA"
  if [ -r "$MANIFEST" ]; then
    if [ -n "$AREA" ] && [ -z "$(test_binaries)" ]; then
      die "unknown --area '$AREA'; known areas: $(test_groups)"
    fi
  fi
  build_target "$target" ""
  [ -r "$MANIFEST" ] || die "build succeeded but the test-binary manifest is missing ($MANIFEST)"
  if [ -n "$AREA" ] && [ -z "$(test_binaries)" ]; then
    die "unknown --area '$AREA'; known areas: $(test_groups)"
  fi
  local core_a="$BUILD_DIR/libbezel_core.a" restale=()
  while IFS= read -r b; do
    [ -n "$b" ] || continue
    [ -x "$b" ] || die "build reported success but $b is missing"
    { [ -f "$core_a" ] && [ "$b" -ot "$core_a" ]; } && restale+=("$b")
    n=$(( n + 1 ))
  done < <(test_binaries)
  if [ "${#restale[@]}" -gt 0 ]; then
    echo "agent_check: ${#restale[@]} test binary(ies) OLDER than libbezel_core.a - forcing a relink" >&2
    rm -f "${restale[@]}"
    build_target "$target" ""
    for b in "${restale[@]}"; do
      { [ -x "$b" ] && ! [ "$b" -ot "$core_a" ]; } || die "$b still stale/missing after a forced relink"
    done
  fi
  [ "$n" -gt 0 ] || die "the test-binary manifest is empty ($MANIFEST)"
  warn_stale_other_areas
}

# #2426 (build-tooling, not a compiler bug): --area restricts the BUILD to one
# area's binary on purpose (that's the whole point — relink ~50 MB instead of
# ~600 MB after a src/ change). Every OTHER area's binary is therefore left
# EXACTLY as it was before this run, including possibly still linked against
# an OLDER libbezel_core.a. That is harmless as long as nothing ever executes
# those binaries directly without coming back through this script (or a full
# rebuild) first: agent_check.sh's own default (no --area) mode and
# scripts/gate.sh both unconditionally rebuild the FULL `bezel_tests` target
# before running anything, so neither path is exposed to it. But a stale
# OTHER-area binary invoked directly — e.g. `BEZEL_TEST_FILTER=...
# build/tests/bezel_tests_ownership` by hand while chasing a specific
# failure, a very natural thing to do mid-investigation — silently reports
# results computed against pre-edit object code. This is a real, mechanically
# confirmed hazard (not a CMake dependency-graph bug: a full `--target
# bezel_tests` rebuild was verified, repeatedly, to correctly detect and
# relink every stale area binary here) and a plausible contributor to at
# least one of the three false failures #2426 catalogs — dispatched_getter_
# borrow_pin_2174 (tests/ownership) went red the same day a codegen-area fix
# touched LlvmBackend_impl.hpp and was very likely tested with `--area
# codegen`/`--area module`, never rebuilding ownership.
#
# So: after any --area build, compare every OTHER area binary's mtime against
# the just-(possibly-)rebuilt libbezel_core.a and warn, loudly and by name,
# about any that are older. Purely additive (stderr only) — no behaviour or
# timing change to the build itself.
warn_stale_other_areas() {
  [ -n "$AREA" ] || return 0
  [ -r "$MANIFEST" ] || return 0
  local core_a="$BUILD_DIR/libbezel_core.a"
  [ -f "$core_a" ] || return 0
  local grp path rest stale=()
  while IFS=$'\t' read -r grp path rest; do
    [ -n "$grp" ] || continue
    [ "$grp" = "$AREA" ] && continue
    [ -x "$path" ] || continue
    [ "$path" -ot "$core_a" ] && stale+=("$grp: $path")
  done < "$MANIFEST"
  if [ "${#stale[@]}" -gt 0 ]; then
    echo "agent_check: WARNING (#2426) — --area $AREA left ${#stale[@]} other area binar$([ "${#stale[@]}" -eq 1 ] && echo y || echo ies) OLDER than the just-built libbezel_core.a:" >&2
    printf '  %s\n' "${stale[@]}" >&2
    echo "  They still hold PRE-EDIT object code. Do not run them directly — re-run agent_check.sh without --area, or rebuild that target explicitly, first." >&2
  fi
}

# Which binaries hold at least one test matching $1? BEZEL_TEST_ENUMERATE lists
# what a process WOULD run without running it (~30 ms), so this is free — and it
# means a lane never has to know which area its test lives in.
binaries_matching() {  # $1 = filter -> "<path> <count>" per matching binary
  local filter="$1" b n
  while IFS= read -r b; do
    [ -n "$b" ] || continue
    n=$(BEZEL_TEST_ENUMERATE=1 BEZEL_TEST_FILTER="$filter" "$b" 2>/dev/null | grep -c .)
    [ "${n:-0}" -gt 0 ] && echo "$b ${n}"
  done < <(test_binaries)
  return 0
}

# Run one binary's matching tests as $3 disjoint shards (the harness's own
# BEZEL_SHARD_COUNT/INDEX, as tests/run_parallel.sh uses), then print ONE merged
# result: every shard's per-test output, then a single summary in the harness's
# exact format with passed/failed summed, and the NATIVE SANITIZER / NATIVE
# SANITIZER INFRA / CHECKED TOOLCHAIN / INFRA lines printed once with their
# counts summed. Returns 124 if any shard timed out, else 1 if any failed.
run_sharded() {  # $1 filter $2 bin $3 shards $4 merged-log
  local f="$1" bin="$2" n="$3" out="$4" i r rc=0 pids=() sl=()
  for (( i=0; i<n; i++ )); do
    sl[i]="$SCRATCH/shard_${i}.$(basename "$bin").log"
    mkdir -p "$SCRATCH/shard_tmp/$i"
    ( "$DH" suite timeout "$BEZEL_GTEST_TIMEOUT" env TMPDIR="$SCRATCH/shard_tmp/$i" \
        BEZEL_TEST_FILTER="$f" BEZEL_SHARD_COUNT="$n" BEZEL_SHARD_INDEX="$i" "$bin" \
        > "${sl[i]}" 2>&1 ) &
    pids+=($!)
  done
  for i in "${!pids[@]}"; do
    wait "${pids[i]}"; r=$?
    if [ "$r" -eq 124 ]; then rc=124; elif [ "$r" -ne 0 ] && [ "$rc" -ne 124 ]; then rc=1; fi
  done
  {
    cat "${sl[@]}" | grep -a -vE '^[0-9]+ passed, [0-9]+ failed|^(NATIVE SANITIZER( INFRA)?|CHECKED TOOLCHAIN|INFRA|SELF-CONTAINED LINK): [0-9]+ '
    cat "${sl[@]}" | awk '
      /^[0-9]+ passed, [0-9]+ failed/ { p += $1; fl += $3
        for (j = 2; j <= NF; j++) if ($j == "total") tot = $(j-1) }
      /^(NATIVE SANITIZER( INFRA)?|CHECKED TOOLCHAIN|INFRA): [0-9]+ / {
        k = $0; sub(/: .*/, "", k)
        if (!(k in line)) { line[k] = $0; order[++no] = k }
        v = $0; sub(/^[^:]*: /, "", v); sub(/ .*/, "", v); cnt[k] += v }
      # #6706: this line carries THREE counts; sum each one positionally.
      /^SELF-CONTAINED LINK: [0-9]+ / { scl = $0; nsc = 0
        for (j = 1; j <= NF; j++) if ($j ~ /^[0-9]+$/) sc[++nsc] += $j }
      END {
        filt = tot - p - fl
        printf "\n%d passed, %d failed", p, fl
        if (filt > 0) printf ", %d filtered", filt
        printf ", %d total\n", tot
        for (q = 1; q <= no; q++) { k = order[q]; l = line[k]
          sub(/: [0-9]+ /, ": " cnt[k] " ", l); print l }
        if (scl != "") { n = split(scl, w, " "); m = 0; o = ""
          for (j = 1; j <= n; j++) { if (w[j] ~ /^[0-9]+$/) w[j] = sc[++m]
            o = o (j > 1 ? " " : "") w[j] }
          print o } }'
  } | tee "$out"
  return "$rc"
}

# --- probe mode: compile (and optionally run under ASan) ONE .bzl -------------
if [ "${1:-}" = "--probe" ]; then
  shift
  asan=0; [ "${1:-}" = "--asan" ] && { asan=1; shift; }
  file="${1:-}"; shift || true
  [ -n "$file" ] && [ -f "$file" ] || die "--probe needs a .bzl file: agent_check.sh --probe [--asan] FILE.bzl"
  base="$(basename "${file%.bzl}")"
  BEZELC="$BUILD_DIR/bezelc"
  build_target bezelc "$BEZELC"
  if [ "$asan" = 1 ]; then
    out="$SCRATCH/${base}.asan.out"
    echo "== agent_check: probe --asan $file (output -> $out) =="
    "$DH" probe "$BEZELC" --sanitize=address -o "$out" "$file" "$@" \
      || { echo "agent_check: probe compile FAILED (see stderr above)" >&2; exit 1; }
    echo "== agent_check: running under ASan/LSan (20s hang timeout) =="
    "$DH" probe timeout 20 "$out"; rc=$?
    echo "agent_check: probe run exit=$rc  (124=timeout/hang, ASan reports above)"
    exit "$rc"
  else
    out="$SCRATCH/${base}.o"
    echo "== agent_check: probe compile $file -> $out =="
    "$DH" probe "$BEZELC" -c -o "$out" "$file" "$@"; exit $?
  fi
fi

# --- --no-lit: strip it from ANYWHERE in the remaining args, before any mode
# is dispatched (issue #3923). --no-lit skips the lit diagnostic track (see
# the default-run section below). Since R30 the lit track is OPT-IN: `--lit`
# runs it, the default is to SKIP it, and `--no-lit` is still accepted (an
# explicit skip, which also wins over `--lit`) so older briefs keep working.
# `--lit` is stripped by the same loop, for the same reason. This used to be stripped
# only INSIDE the default filter-run path's own arg loop, so `--no-lit` typed
# BEFORE `--area` (a natural thing to type - a real lane hit exactly this:
# `agent_check.sh --no-lit --area codegen f1 f2 f3`) never reached that loop:
# `--area` was $2, not $1, so the dedicated --area block below (which only
# ever looks at $1) never matched, and both `--area` and its group-name
# argument fell through, UNRECOGNIZED, into the plain filter loop alongside
# the real filters - silently exploding a 3-filter run into "everything
# whose suite name contains 'codegen'". Stripping --no-lit here, before
# --area/--build-only/--select are dispatched, means the two flags now
# compose in EITHER order.
NO_LIT=0
RUN_LIT=0
# --jobs N / AGENT_CHECK_JOBS: shards per test binary for a filtered run
# (default min(4, nproc/4); 1 = the exact old serial behaviour). Stripped here
# for the same compose-in-any-order reason as --no-lit.
_ncpu="$(nproc 2>/dev/null || echo 4)"
SHARDS="${AGENT_CHECK_JOBS:-$(( _ncpu / 4 < 4 ? _ncpu / 4 : 4 ))}"
[ "$SHARDS" != 0 ] || SHARDS=1   # nproc < 4
_ac_args=()
_next_jobs=0
for _a in "$@"; do
  if [ "$_next_jobs" = 1 ]; then SHARDS="$_a"; _next_jobs=0; continue; fi
  case "$_a" in
    --no-lit) NO_LIT=1 ;;
    --lit) RUN_LIT=1 ;;
    --jobs) _next_jobs=1 ;;
    --jobs=*) SHARDS="${_a#--jobs=}" ;;
    *) _ac_args+=("$_a") ;;
  esac
done
[ "$_next_jobs" = 0 ] || die "--jobs needs a number"
case "$SHARDS" in ''|*[!0-9]*|0) die "--jobs/AGENT_CHECK_JOBS must be a positive integer (got '$SHARDS')" ;; esac
set -- "${_ac_args[@]}"

# --- --area <group>: restrict the BUILD and the RUN to one test binary --------
# Parsed before the modes below so it composes with all of them.
if [ "${1:-}" = "--area" ]; then
  shift
  AREA="${1:-}"; shift || true
  [ -n "$AREA" ] || die "--area needs a group name (see build/tests/test_binaries.txt)"
fi

# --- build-only ---------------------------------------------------------------
if [ "${1:-}" = "--build-only" ]; then
  build_tests
  echo "== agent_check: build OK ($(test_binaries | wc -l) test binaries) =="
  exit 0
fi

# --- select mode: auto-derive the lane's subset from its own diff (#2100) -----
# The 'lane' selector variant (issue-number + fine-§ + diagnostic-string; no
# directory fallback) is deliberately tight — median ~2.5% of the suite — so
# this stays a targeted lane check, not a gate. Runs sharded (8) via
# run_parallel, whose subset accounting (BEZEL_TEST_LIST -> selected==ran)
# keeps the drop-detection contract. An EMPTY selection is a hard error: pass
# explicit filters instead of trusting a selector that mapped nothing.
if [ "${1:-}" = "--select" ]; then
  shift
  base="${1:-}"
  if [ -z "$base" ]; then
    # fix #3725 (sibling of #3718 in gate_full_cycle.sh:~895-922): the base is
    # THIS BRANCH'S UPSTREAM, never the bare local `main`. Nothing in the run
    # workflow ever fast-forwards local `main` — an integration/lane branch
    # tracks `origin/main`, and gate_and_push.sh lands through that same
    # upstream — so `git merge-base HEAD main` silently widened every
    # `--select` run to everything landed since local `main` was last checked
    # out. Measured in this worktree: 10628 tests selected against the stale
    # local `main` vs 6022 against `origin/main` for the same HEAD (the
    # sibling issue measured 9424 vs 3225 at 61b146c) — a lane-only check
    # running ~1.8x the intended subset, and select_tests.py's OWN default
    # (scripts/select_tests.py:~377) already resolves against `origin/main`,
    # so this made the two scripts disagree with each other. Duplicated
    # verbatim from gate_full_cycle.sh's tidy-base resolution rather than
    # extracted to a shared helper: that file is owned by a different lane in
    # this batch and factoring the snippet out would require editing it too.
    _up="$(git -C "$ROOT" rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)"
    if [ -z "$_up" ] || ! git -C "$ROOT" rev-parse --verify --quiet "${_up}^{commit}" >/dev/null 2>&1; then
      _up="origin/main"
    fi
    base="$(git -C "$ROOT" merge-base HEAD "$_up" 2>/dev/null || true)"
    if [ -z "$base" ]; then base="HEAD~1"; fi
  fi
  [ -n "$base" ] || die "--select: cannot determine a base (pass one: agent_check.sh --select <ref>)"
  build_tests
  list="$SCRATCH/select_$$.list"
  python3 "$ROOT/scripts/select_tests.py" --repo "$ROOT" --base "$base" \
    --variant lane -o "$list" || die "--select: selector failed"
  n="$(grep -c . "$list" 2>/dev/null || true)"
  [ "${n:-0}" -gt 0 ] || die "--select: selection is EMPTY for base $base — the diff mapped to no tests; pass explicit filters instead"
  echo "== agent_check: --select: $n diff-selected tests (base $base; dev_heavy suite, 8 shards) =="
  log="$SCRATCH/test_select.log"
  "$DH" suite env BEZEL_TEST_LIST="$list" "$ROOT/tests/run_parallel.sh" 8 $(test_binaries | paste -sd' ' -) 2>&1 | tee "$log"
  rc=${PIPESTATUS[0]}
  grep -E 'WARNING: only' "$log" || true
  if [ "$rc" -ne 0 ]; then
    echo "agent_check: --select had failures (exit $rc)"; exit 1
  fi
  echo "== agent_check: --select green ($n selected) =="
  exit 0
fi

# Per-invocation timeout for gtest binary runs (#5450). A full-area codegen
# binary (~17,800 tests) can run 70+ minutes wall when genuinely wedged (the
# process stays alive holding a lane slot indefinitely). Four lanes were killed
# by a 600s external timeout and all reported tests passing — confirming that a
# timeout must be set ABOVE the 10-minute mark to avoid false-killing a
# contended-but-valid run. 3600s (1 hour) is comfortably above that while still
# catching a genuine hang.
# Exposed as an env var so lanes can override it (e.g. BEZEL_GTEST_TIMEOUT=600
# for a quick probe). The probe path (--probe) has its own separate 20s limit.
: "${BEZEL_GTEST_TIMEOUT:=3600}"

# --- targeted test run (the default) ------------------------------------------
# --no-lit and --area were already consumed above (composing in either
# order); anything left here must be either a real gtest filter or a mistake.

# Footgun guards BEFORE we build, so mistakes fail fast.
[ "$#" -ge 1 ] || die "no test filter given.
  Run a TARGETED subset:  scripts/agent_check.sh <filter> [filter2 ...]
  (whole-suite validation is the GATE's job: scripts/gate.sh — do NOT run it in a lane)"
for f in "$@"; do
  case "$f" in
    --gtest_filter*|--gtest*) die "the test binary IGNORES $f — pass a BARE substring, e.g. 'scripts/agent_check.sh my_area_1234'" ;;
    "") die "empty filter would run the WHOLE suite — give a real substring (the gate runs the full suite)" ;;
    run_parallel*|--full|--all) die "'$f' is a full-suite run — that is the gate's job, not a lane's" ;;
    # issue #3923, widened by #3934: a gtest suite-or-name substring can
    # NEVER begin with `-` at all, one dash or two — `-`/`--` are not valid
    # C++ identifier characters, and every suite/test name is a C++
    # identifier with an area prefix (e.g. `codegen_...`). So ANY argument
    # that still starts with `-` at this point is ALMOST CERTAINLY a
    # mistyped/misplaced flag, not a real filter (`-area codegen` and
    # `-no-lit` are exactly as much a typo as `--area`/`--no-lit` with a
    # dropped dash — same failure mode, one dash short). Refuse it loudly
    # instead of silently running it as a filter substring (which
    # previously could explode scope to nearly a whole area with no
    # indication anything was wrong — see the issue's measured repro).
    -*) die "'$f' looks like a mistyped or misplaced flag, not a filter — refusing to treat it as one (a gtest suite-or-name substring can never start with '-'). Recognized flags: --area <group>, --lit, --no-lit, --probe, --build-only, --select (see the usage comment at the top of this script). If you meant --area, it must be followed by its group name." ;;
  esac
done

build_tests

rc_all=0
for f in "$@"; do
  echo "== agent_check: BEZEL_TEST_FILTER=$f (dev_heavy suite, capped) =="
  log="$SCRATCH/test_${f//[^A-Za-z0-9_.]/_}.log"
  : > "$log"
  # Which per-area binaries hold this filter? Enumerating is ~30 ms per binary
  # and runs NOTHING, so the lane never has to know the area — and a filter that
  # legitimately spans two areas runs in BOTH.
  matches="$(binaries_matching "$f")"
  # A filter matching ZERO tests is almost always a typo — or the file basename
  # passed instead of the gtest SUITE-or-name substring. The harness exits 0 on
  # 0 matches, so without this check it would report a FALSE GREEN. With the
  # suite split this must be judged over ALL binaries, never one.
  if [ -z "$matches" ]; then
    echo "agent_check: ERROR filter '$f' matched 0 tests in any of $(test_binaries | wc -l) test binaries — a typo, or you passed the FILE basename instead of the gtest suite-or-name substring. Nothing was verified; treating as FAILURE (not a false green)."
    # Suggest what they probably meant. Suite names are area-prefixed and
    # abbreviated, so they routinely differ from the FILE name that inspired
    # the filter (file `..._case_type_ws_2419_test.cpp` -> suite
    # `codegen_serialization_case_ws_2419`). Every such mistake observed so far
    # still carried the issue number, so try tokens in order of how much they
    # NARROW the search: numeric tokens (issue numbers) first — they are the
    # shortest but by far the most distinctive — then longest-first for the
    # rest. Stop at the first token that finds anything.
    for tok in $(printf '%s\n' "$f" | tr -cs 'A-Za-z0-9' '\n' |
                 awk 'length($0)>=3 {k = ($0 ~ /^[0-9]+$/) ? 0 : 1;
                                     print k"\t"length($0)"\t"$0}' |
                 sort -k1,1n -k2,2nr | cut -f3); do
      hits="$(for b in $(test_binaries); do
                BEZEL_TEST_ENUMERATE=1 "$b" 2>/dev/null
              done | grep -iF "$tok" | cut -d. -f1 | sort -u | head -8)"
      if [ -n "$hits" ]; then
        echo "agent_check: did you mean one of these? (suites containing '$tok')"
        printf '    %s\n' $hits
        echo "agent_check: (list all with: BEZEL_TEST_ENUMERATE=1 <binary from build/tests/test_binaries.txt>)"
        break
      fi
    done
    rc_all=1
    continue
  fi
  f_ran=0; f_rc=0
  while read -r bin want; do
    [ -n "$bin" ] || continue
    echo "-- $(basename "$bin"): $want matching test(s) --"
    blog="$SCRATCH/test_${f//[^A-Za-z0-9_.]/_}.$(basename "$bin").log"
    # timeout wraps env so timeout(1) is the direct child of systemd-run and can
    # monitor + signal the whole process tree (env forks the actual binary).
    # Exit 124 = timeout fired; the n<want check below also catches this case
    # but the explicit 124 check gives a clearer message (#5450).
    nsh=$SHARDS; [ "$nsh" -le "$want" ] || nsh=$want
    if [ "$nsh" -le 1 ]; then
      "$DH" suite timeout "$BEZEL_GTEST_TIMEOUT" env BEZEL_TEST_FILTER="$f" "$bin" 2>&1 | tee "$blog"
      rc=${PIPESTATUS[0]}
    else
      run_sharded "$f" "$bin" "$nsh" "$blog"; rc=$?
    fi
    if [ "$rc" -eq 124 ]; then
      echo "agent_check: $(basename "$bin") timed out after ${BEZEL_GTEST_TIMEOUT}s (exit 124) — likely a hang; see $blog"
      f_rc=1
    elif [ "$rc" -ne 0 ]; then
      f_rc=1
    fi
    cat "$blog" >> "$log"
    summ="$(grep -a -oE '[0-9]+ passed, [0-9]+ failed' "$blog" | tail -1)"
    n="$(printf '%s' "$summ" | awk '{print $1 + $3}')"   # passed + failed
    f_ran=$(( f_ran + ${n:-0} ))
    # Enumeration said this binary holds $want matching tests; anything less
    # actually running means the process died mid-run. Never a green.
    if [ "${n:-0}" -lt "$want" ]; then
      echo "agent_check: $(basename "$bin") ran ${n:-0} of $want matching tests — the run was CUT SHORT (crash/abort); not trustworthy."
      f_rc=1
    fi
  done <<< "$matches"
  if [ "$f_rc" -ne 0 ]; then
    echo "agent_check: FILTER '$f' had failures"; rc_all=1
  else
    echo "agent_check: filter '$f' green ($f_ran passed across $(printf '%s\n' "$matches" | grep -c .) binaries)"
  fi
done

# --- lit diagnostic track (opt-in: --lit; default is to skip) -----------------
# Opt-in since R30: with ~17 lanes each calling this script many times per
# fix, a default-on lit run (2461 bezelc compiles; ~6s idle, ~40s measured on
# the loaded box) was paid on every iteration. A lane runs `--lit` ONCE per
# commit, before committing, whenever the commit touches src/ or tests/lit;
# the integrator runs it on staging per batch and the gate always runs it.
# A gtest FILTER cannot catch a change to a SHARED diagnostic STRING: a lane
# editing one diagnostic breaks lit tests filed under UNRELATED issue numbers
# (e.g. a #2159 change broke the #2145 reject lits — invisible to a `2159` gtest
# filter and to the entire gtest track). The gate always runs lit, but catching
# the drift HERE saves a full gate cycle. The whole track is ~6s on the
# already-built bezelc; run it via the same `check-lit` target the gate uses
# (config/bezelc wiring guaranteed) unless the lane is pure codegen-value work.
if [ "${RUN_LIT:-0}" = 1 ] && [ "${NO_LIT:-0}" != 1 ]; then
  echo "== agent_check: lit diagnostic track (real bezelc + FileCheck) — catches shared-diagnostic drift a gtest filter cannot =="
  litlog="$SCRATCH/test_lit.log"
  "$DH" suite cmake --build "$BUILD_DIR" --target check-lit 2>&1 | tee "$litlog"
  litrc=${PIPESTATUS[0]}
  if [ "$litrc" -ne 0 ]; then
    echo "agent_check: LIT diagnostic track FAILED — a change to a shared diagnostic string likely broke a pinned CHECK (grep '$litlog' for 'FAIL: Bezel-lit'); update the expectation or fix the diagnostic."
    rc_all=1
  else
    echo "agent_check: lit diagnostic track green"
  fi
else
  echo "agent_check: lit diagnostic track NOT RUN (opt-in) — before committing a change that touches src/ or tests/lit, run once with --lit and quote its 'Passed:' line"
fi

[ "$rc_all" -eq 0 ] && echo "== agent_check: ALL requested filters green ==" \
                    || echo "== agent_check: some filters FAILED (see above) =="
exit "$rc_all"
