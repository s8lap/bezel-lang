#!/usr/bin/env bash
# Trustworthy full gate cycle: run scripts/gate.sh in BOTH oracles — plain, then
# --full (generated-code ASan) — and only report success when each pass is
# genuinely trustworthy.
#
# Why this wrapper exists (two lessons that each cost a near-miss):
#
#   1. A `== GATE PASS ==` banner can sit on top of a run that never executed
#      most of the suite. On 2026-08-05 gate 7 printed PASS while its own log
#      said "shard crash / undercount": 15 of 16 codegen shards had died and
#      5434 of 15123 tests actually ran. rc==0 is NOT sufficient — the absence
#      of the undercount banner is part of the acceptance condition, and a run
#      that undercounts is RETRIED rather than believed.
#
#   2. `--full` is a DIFFERENT oracle, not a stronger one (#2633). ASan changes
#      program behaviour and makes some bad reads benign, so the plain gate
#      catches defects --full masks, and vice versa. Both must run.
#
# ⚠ gate.sh reads MODE="${1:-default}" — the FIRST argument only. Passing it a
# flag it does not implement (e.g. --clean-first) silently DISPLACES --full and
# runs the "ASan" half as a plain gate while printing the wrong mode. This
# script therefore passes gate.sh nothing but the modes gate.sh implements.
#
# Usage:
#   scripts/gate_full_cycle.sh [-d LOGDIR] [-t TAG] [-n MAX_ATTEMPTS]
#
#   -d LOGDIR   where the two logs land (default /var/tmp/bezel-scratch/gate-cycle)
#   -t TAG      prefix for this cycle's log names (default: the current HEAD sha)
#   -n N        max attempts per mode before giving up (default 40)
#
# Exit codes: 0 = both modes trustworthy-green. Non-zero = the gate's own rc for
# a real failure, or 99 if a mode never produced a trustworthy run.
#
# Environment: BEZEL_NO_GC=1 is set for the gate runs — gate_and_push.sh's
# post-push worktree reap is slow (~1 worktree / 14s) and has no business
# running inside a gate cycle. Reap separately at consolidation.

set -u

REPO=/home/stepan/w/code/bezel-lang
cd "$REPO" || exit 1

# Scratch dir + cycle-verdict path (matches gate.sh:76-77). The cycle writes its
# OWN verdict here (Phase 3c, per the exit-code-trap) so downstream consumers
# (gate_and_push.sh, gate_wait.sh) read the CYCLE outcome — not a per-attempt
# gate.sh verdict, which lives at a per-run path inside run_until_trustworthy
# (set via GATE_VERDICT_FILE, honored at gate.sh:77).
S=/var/tmp/bezel-scratch; mkdir -p "$S"
CYCLE_VERDICT_FILE="${GATE_VERDICT_FILE:-$S/gate-verdict.json}"

LOGDIR=/var/tmp/bezel-scratch/gate-cycle
TAG=""
MAX_ATTEMPTS=40

while [ $# -gt 0 ]; do
  case "$1" in
    -d) LOGDIR="$2"; shift 2 ;;
    -t) TAG="$2"; shift 2 ;;
    -n) MAX_ATTEMPTS="$2"; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "gate_full_cycle: unknown argument '$1'" >&2; exit 64 ;;
  esac
done

[ -n "$TAG" ] || TAG="$(git rev-parse --short HEAD 2>/dev/null || echo notag)"
mkdir -p "$LOGDIR" || exit 1
CYCLE_LOG="$LOGDIR/${TAG}_cycle.log"

say() { echo "[gate_full_cycle $(date '+%H:%M:%S')] $*" | tee -a "$CYCLE_LOG"; }

# _legacy_rc_decision — the decision the ORIGINAL $?/banner-grep logic makes.
# Kept as the CROSS-CHECK assertion (Phase 3c dual-consume) and the FALLBACK when
# the verdict file is missing (back-compat; the writer is best-effort, per
# #3002). per the exit-code-trap: in THIS foreground call $? is the gate's own
# exit (no pipeline/backgrounding), so it is reliable HERE — the trap bites the
# EXTERNAL observer (gate_and_push/gate_wait), not this inner loop.
#
# Wave 3 (#3010): the canonical port of this logic now lives in
# gate.cycle.legacy_decision; this bash function stays as the dual-consume
# CROSS-CHECK (it and the Python module are proven byte-identical over every
# reachable rc x undercount by test_cycle.ZeroDisagreementProofTest). The
# 0-disagreement assertion below screams loudly if they ever diverge on a real
# gate. Keep for 1 landing gate, then retire (the Python module is the authority).
_legacy_rc_decision() { # rc log -> echoes accept|retry|bail
  case "$1" in
    0)  if grep -q 'shard crash / undercount' "$2" 2>/dev/null; then echo retry; else echo accept; fi ;;
    2|3) echo retry ;;
    *)  echo bail ;;
  esac
}

# write_cycle_verdict — write the cycle-level authoritative verdict (Phase 3c,
# per the exit-code-trap). Called from the EXIT trap on EVERY cycle exit so a
# downstream consumer (gate_and_push.sh, gate_wait.sh) reads the CYCLE outcome
# from one file instead of the cycle's $? — which is reliable in-process but not
# across the backgrounding boundary that bit the coordinator 4x. Best-effort
# (`|| true`): a verdict-write hiccup must never change the exit code.
write_cycle_verdict() { # rc
  local rc="$1"
  local head; head="$(git rev-parse HEAD 2>/dev/null || echo '')"
  PYTHONPATH=scripts python3 -m gate.verdict write --path "$CYCLE_VERDICT_FILE" \
    --rc "$rc" --head="$head" --mode=cycle --oracle=dual 2>/dev/null || true
}
trap 'rc=$?; write_cycle_verdict "$rc" || true; exit $rc' EXIT

# Run one gate mode until it produces a TRUSTWORTHY result. Phase 3c (per the
# exit-code-trap): the decision now comes from the AUTHORITATIVE gate-verdict.json
# that gate.sh writes on every exit, not from $?/banner greps. The verdict
# materializes the rc==0+undercount distrust as a POISONED verdict (#3001), so
# this consumer checks one structured field instead of re-implementing the
# distrust grep. $?/grep is kept as a CROSS-CHECK assertion (dual-consume): the
# verdict and the legacy logic are derived from the same signals inside gate.sh,
# so disagreement means a bug or a race — scream LOUDLY and take the conservative
# (non-accept) decision. A missing verdict falls back to the legacy $?/grep.
#
# Decision mapping (verdict -> cycle action):
#   pass (+trustworthy) -> accept   rc==0, no undercount
#   refused             -> retry    rc==2: preflight, builders active (transient)
#   retry               -> retry    rc==3: coverage short (#3002)
#   poisoned            -> retry    rc==0 + undercount distrust (#3001)
#   fail                -> bail     rc==1: real regression
run_until_trustworthy() {
  local log="$1"; shift
  local mode_desc="${1:-plain}"; shift || true
  local i rc v_verdict v_rc dec old_dec reason
  for i in $(seq 1 "$MAX_ATTEMPTS"); do
    # BEZEL_GATE_CYCLE=1 tells gate.sh that a DUAL-oracle run is driving it, so
    # it suppresses its "this is one oracle, not an acceptance verdict" note
    # (#2917). GATE_VERDICT_FILE points THIS gate's verdict at a PER-ATTEMPT path
    # (the default is a SHARED file every gate overwrites — gate.sh:277), so the
    # consumer reads its OWN gate's verdict, not a stale/concurrent one.
    local vf="${log%.log}.attempt${i}.verdict.json"
    BEZEL_NO_GC=1 BEZEL_GATE_CYCLE=1 GATE_VERDICT_FILE="$vf" \
      scripts/gate.sh "$@" > "$log" 2>&1
    rc=$?

    # Read the verdict (exit 0 iff trustworthy pass; 1 iff present-not-pass;
    # 2 iff missing/unparseable — consumer falls back to $?).
    v_verdict="$(PYTHONPATH=scripts python3 -m gate.verdict read --path "$vf" --field verdict 2>/dev/null)"
    v_rc=$?

    # Map the verdict to a decision (empty = verdict absent -> fallback below).
    # Wave 3 (#3010): the canonical verdict->decision mapping + the trustworthy
    # defense-in-depth now live in gate.cycle.classify (the inline `case` is
    # retired). classify prints accept/retry/bail (or empty for an absent
    # verdict); the v_rc trusted-pass predicate feeds the defense-in-depth cell
    # (pass + trustworthy=false -> retry, never accept — the #3002 belt).
    dec="$(PYTHONPATH=scripts python3 -m gate.cycle classify \
            --verdict "$v_verdict" $([ "$v_rc" -eq 0 ] || echo --not-trustworthy) 2>/dev/null)"

    # CROSS-CHECK (dual-consume): compute the legacy $?/grep decision and verify
    # the verdict agrees. Disagreement means a bug in the verdict write or a race
    # on the verdict file — scream LOUDLY and take the CONSERVATIVE non-accept
    # decision. per the exit-code-trap: a false accept is the worst outcome (55
    # pass-but-truncated gates over 24 days).
    #
    # Wave 3 (#3010): the reconciliation itself now lives in
    # gate.cycle.cross_check (conservative-non-accept-on-disagreement). The
    # legacy decision is the dual-consume CROSS-CHECK input; cross_check returns
    # the settled decision (agree -> that; disagree + either-accept -> bail;
    # disagree retry/bail -> verdict wins). Pinned over the full 3x3 grid by
    # test_cycle.CrossCheckTest. _legacy_rc_decision stays as the cross-check
    # source (proven byte-identical to gate.cycle.legacy_decision by
    # test_cycle.ZeroDisagreementProofTest).
    old_dec="$(_legacy_rc_decision "$rc" "$log")"
    if [ -n "$dec" ]; then
      # Dual-consume: also compute the legacy decision via gate.cycle and assert
      # it matches the bash _legacy_rc_decision. _legacy_rc_decision takes the log
      # and greps the undercount banner itself; the Python port takes the resolved
      # boolean, so harvest it once here.
      _uc=0; grep -q 'shard crash / undercount' "$log" 2>/dev/null && _uc=1
      _py_legacy="$(PYTHONPATH=scripts python3 -m gate.cycle legacy-decision \
                     --rc "$rc" $([ "$_uc" -eq 1 ] && echo --undercount) 2>/dev/null)"
      if [ -n "$_py_legacy" ] && [ "$_py_legacy" != "$old_dec" ]; then
        say "DUAL-CONSUME DISAGREEMENT (legacy): bash _legacy_rc_decision='$old_dec' but gate.cycle.legacy-decision='$_py_legacy' (rc=$rc) — the Python port diverged from the bash; see test_cycle.ZeroDisagreementProofTest"
      fi
      if [ "$dec" != "$old_dec" ]; then
        _cc="$(PYTHONPATH=scripts python3 -m gate.cycle cross-check \
                --verdict-decision "$dec" --legacy-decision "$old_dec" 2>/dev/null)"
        say "VERDICT CROSS-CHECK DISAGREEMENT: verdict=$v_verdict->$dec but \$?=$rc/grep->$old_dec (attempt $i) — cross_check -> ${_cc:-n/a}"
        say "  The verdict and legacy logic disagree — taking the conservative (non-accept) decision."
        dec="${_cc:-bail}"
      fi
    fi
    # FALLBACK: verdict missing/unparseable — use the legacy $?/grep decision.
    [ -z "$dec" ] && dec="$old_dec"

    # Act on the decision, preserving the per-case messages + evidence (#3001).
    case "$dec" in
      retry)
        # Determine the reason (verdict value, or rc in fallback mode) to keep
        # the original specific messages + sleep durations.
        reason="$v_verdict"
        [ -z "$reason" ] && case "$rc" in 2) reason=refused;; 3) reason=retry;; *) reason=poisoned;; esac
        if [ "$reason" = refused ]; then
          say "$mode_desc attempt $i: preflight refused (builders active) — waiting"
          sleep 45; continue
        fi
        # PRESERVE THE EVIDENCE (#3001). Every attempt writes to the same $log,
        # so each distrusted attempt was OVERWRITTEN by the next and only the
        # final (green) run survived — making an undercount undiagnosable: the
        # banner's own numbers (reported `ran` vs the registry total, which shard
        # dropped, whether a completion summary was missing) live in the
        # attempt's log and nowhere else. The 2026-08-07 close-out undercounted
        # three times in a row and left nothing to look at. Keep the canonical
        # path at the LAST attempt (callers grep it) and copy each aside.
        cp -f "$log" "${log%.log}.attempt${i}.log" 2>/dev/null || true
        if [ "$reason" = retry ]; then
          say "$mode_desc attempt $i: COVERAGE SHORT — $(grep -oE '[0-9]+ of [0-9]+ gtests ran' "$log" | tail -1) — retrying (log kept: ${log%.log}.attempt${i}.log)"
        else
          say "$mode_desc attempt $i: rc=$rc UNDERCOUNT banner present (verdict=$reason) — NOT trusted, retrying (log kept: ${log%.log}.attempt${i}.log)"
        fi
        sleep 60; continue ;;
      bail)
        say "$mode_desc attempt $i: REAL FAILURE rc=$rc verdict=${v_verdict:-n/a} — see $log"
        return "$rc" ;;
      accept)
        say "$mode_desc attempt $i: TRUSTWORTHY GREEN (verdict=${v_verdict:-pass}, rc=$rc) — $log"
        return 0 ;;
    esac
  done
  say "$mode_desc: no trustworthy run in $MAX_ATTEMPTS attempts — giving up"
  return 99
}

say "cycle start at $(git rev-parse --short HEAD) — logs in $LOGDIR"

# STALENESS PREFLIGHT. A gate that validates a STALE compiler is the worst
# possible outcome: it reports green for code that was never built. This is not
# hypothetical — `cmake --build --target bezelc` has been observed printing
# "Built target bezelc" and NOT relinking, leaving a binary older than both its
# own object file and the sources (the #2426 unreliable-incremental family).
# During this run that cost a false "the fix does not work" finding and a
# lane's time before a binary mtime check settled it.
#
# gate.sh rebuilds before it tests, so this preflight is about the state we
# ENTER in: if the tree already carries a stale binary that the rebuild
# phase WILL NOT fix, say so loudly here. Advisory, not fatal — the gate's
# own reconfigure+build is still the mechanism that fixes it; this only
# guarantees the condition is never silent.
#
# STALE-SUPPRESSION HYGIENE (this run). Every lane commit edits src/ and
# makes at least one source newer than build/bezelc — so the original
# "fire whenever any source is newer than bezelc" check fired on EVERY
# gate, by design, and was noise. The rebuild phase WILL relink bezelc
# whenever a stale .o file (one where source > .o) gets recompiled, so
# the warning is REDUNDANT in that case and was correctly suppressing
# useful signal by being constant.
#
# The warning is load-bearing ONLY for the #2426 case, whose structural
# fingerprint is: some source X is newer than bezelc AND X's .o is ALSO
# newer than X. That ordering means cmake already rebuilt the .o and
# thinks the library is up-to-date — it will not touch it again, will
# not relink bezelc, and a green gate on that state would lie. Fire only
# when this fingerprint holds; suppress otherwise (the rebuild will fix
# the staleness, the warning would be redundant).
#
# Header-only changes need a slightly different check: a .hpp/.h file has
# no own .o, but cmake's compiler_depend manifest tracks header→.o deps.
# If ANY bezelc-related .o (in the libraries that feed bezelc:
# bezel_core static lib + bezel_runtime static lib + bezelc binary's own
# main.cpp.o) is newer than the header, cmake processed it — same #2426
# shape, fire.
#
# Only the NEWEST source newer than bezelc is checked: that is the source
# whose edit drove the staleness, and a single .o>source match is enough
# to fingerprint the #2426 bug for this run. (Sources older than the
# newest one are not necessarily up-to-date either, but the rebuild
# phase walks ALL of them regardless; we only need to detect the case
# where the rebuild WILL NOT cover the staleness, and any source newer
# than bezelc whose .o is also newer than it is exactly that case.)
if [ -x "$REPO/build/bezelc" ]; then
  fire_stale=0
  newest_src="$(find "$REPO/src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
                  -newer "$REPO/build/bezelc" -print -quit 2>/dev/null)"
  # bezelc links THREE object trees (gate_full_cycle.sh:78 — link.txt):
  # libbezel_core.a, libbezel_runtime.a, and the binary's own main.cpp.o.
  # The #2426 fingerprint applies to ANY of them — a runtime source newer
  # than bezelc with its .o ALSO newer than it is exactly the same bug
  # shape, because cmake believes the library is up-to-date and won't
  # relink bezelc. All three roots must be searched; missing one would
  # silently swallow that class of #2426.
  CORE_OBJ_ROOTS="$REPO/build/CMakeFiles/bezel_core.dir $REPO/build/CMakeFiles/bezel_runtime.dir $REPO/build/CMakeFiles/bezelc.dir"
  if [ -n "$newest_src" ]; then
    case "$newest_src" in
      *.cpp)
        # Source file: its .o mirrors its path under each of the three
        # object roots above. If ANY candidate .o is newer than the
        # source, cmake already rebuilt it and the rebuild phase won't
        # touch it again — #2426 shape.
        src_rel="${newest_src#$REPO/}"
        fire_stale=0
        for root in $CORE_OBJ_ROOTS; do
          o="$root/$src_rel.o"
          if [ -f "$o" ] && [ "$o" -nt "$newest_src" ]; then
            fire_stale=1; break
          fi
        done
        ;;
      *)
        # Header: any bezel-related .o newer than the header means cmake
        # processed it (and was supposed to relink) — #2426 shape.
        newer_o="$(find $CORE_OBJ_ROOTS -name '*.o' -newer "$newest_src" \
                       -print -quit 2>/dev/null || true)"
        [ -n "$newer_o" ] && fire_stale=1
        ;;
    esac
  fi
  if [ "$fire_stale" -eq 1 ]; then
    say "STALE: build/bezelc is older than $(basename "$newest_src") AND"
    say "STALE: its .o (or a dependent one, for headers) IS newer than the"
    say "STALE: source — cmake already rebuilt it but did not relink bezelc"
    say "STALE: (the #2426 unreliable-incremental family). The gate's own"
    say "STALE: rebuild phase may repeat the bug; if a gate reports green"
    say "STALE: while this warning stands, do NOT believe the result."
  fi
fi

# ── VACUOUS-ASSERTION PHASE (#3138) ────────────────────────────────────────
# A static source check, so it runs FIRST: a tree carrying an assertion that
# cannot fail is rejected in under a second instead of after two ~20-minute
# sharded passes agree that it is green — which is exactly what they would do,
# because a vacuous assertion passes under BOTH oracles by construction. That is
# the point: this is the one defect class neither gate.sh pass can see, so the
# phase belongs in the CYCLE (which runs once) rather than in gate.sh (which
# runs twice and would only duplicate a source scan).
#
# `runAot` fills JitResult::diagnostics only on a REJECT, so a test asserting the
# ABSENCE of a Warning on a program it also asserts RAN is a tautology — it
# claims coverage it does not have. #2992's entire regression file was in that
# state (7 of 7 tests), with a fails-without recipe that could not reproduce.
#
# --self-test runs FIRST and is not optional: the detector's own first draft
# reported the whole tree clean because a `^BEZEL_TEST` regex matched zero
# wrapped macros. A green from a matcher that matched nothing is worth nothing,
# so the phase proves the detector still goes red on a known-positive fixture
# before it is allowed to certify the tree.
say "vacuous-assertion phase: detector self-test + tree scan (#3138)"
if ! python3 scripts/check_vacuous_tests.py --self-test \
      > "$LOGDIR/${TAG}_vacuous.log" 2>&1; then
  say "CYCLE FAILURE: check_vacuous_tests SELF-TEST failed — the detector is"
  say "  broken, so its verdict on the tree means nothing. See $LOGDIR/${TAG}_vacuous.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_vacuous.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_vacuous_tests.py --strict \
      >> "$LOGDIR/${TAG}_vacuous.log" 2>&1; then
  say "CYCLE FAILURE: vacuous assertion(s) found — a test that cannot fail is"
  say "  worse than no test (#3138). See $LOGDIR/${TAG}_vacuous.log"
  sed -n '/VACUOUS/,$p' "$LOGDIR/${TAG}_vacuous.log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "vacuous-assertion phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_vacuous.log"))"

run_until_trustworthy "$LOGDIR/${TAG}_plain.log" "plain" || exit $?

# --full folds whole-suite generated-code AddressSanitizer into the same sharded
# pass (BEZEL_AOT_FORCE_SANITIZE=address). Confirm from the log that ASan was
# actually on: a mode that silently degraded to plain would report a green that
# means something else entirely.
run_until_trustworthy "$LOGDIR/${TAG}_full.log" "full(ASan)" --full || exit $?

# fix #3011: THIS IS A HARD FAILURE, NOT A WARNING.
#
# It was `say "WARNING: …"` followed by an unconditional
# `say "cycle COMPLETE: both oracles trustworthy-green"; exit 0` — so a cycle
# that ran the PLAIN oracle twice announced dual-oracle green and pushed. The
# check already existed and already knew; it just had no teeth.
#
# The whole point of #2917/#2633 is that plain and ASan are DIFFERENT oracles,
# neither dominating. A cycle that silently ran one of them twice has not
# established the thing this script exists to establish, and "both oracles
# trustworthy-green" is then a false statement in the log that a human will
# later cite as evidence.
#
# gate.sh's mode allow-list (#3011) stops the typo that caused this. This is the
# belt to that braces: a POSITIVE allow-list missing a name is variant 1 of the
# category-defect taxonomy, so the downstream evidence check must stand on its
# own rather than trusting the upstream list to be complete.
#
# Phase 3c partial: the verdict carries an `oracle` field (gate.sh sets it from
# ASAN=1 when MODE=--full). Cross-check the last --full pass's verdict says
# oracle=asan. The log grep stays AUTHORITATIVE: it checks OBSERVED evidence
# (sanitizer output in the log), while the verdict's oracle is the INTENDED mode
# — the grep catches a mode that silently degraded even if the verdict says asan.
#
# Wave 3 (#3010): the consolidated ASan-ran evidence check now lives in
# gate.cycle.assert_asan_ran (OBSERVED log grep + INTENDED verdict oracle). This
# call IS the check; the inline grep below is the dual-consume CROSS-CHECK
# (proven byte-identical over every evidence combination by
# test_cycle.AssertAsanRanTest). The two MUST agree — disagreement means the
# Python port diverged from the bash, and the conservative outcome is to treat
# the cycle as NOT dual-oracle green. Keep the inline grep for 1 landing gate,
# then retire it (gate.cycle is the authority).
_fvf=$(ls -1 "$LOGDIR/${TAG}_full.attempt"*.verdict.json 2>/dev/null | sort -V | tail -1)
_asan_args=(--log-path "$LOGDIR/${TAG}_full.log")
[ -n "$_fvf" ] && _asan_args+=(--verdict-path "$_fvf")
# Run the check once, capturing the exit + the stderr reason (the reason goes to
# stderr; stdout is empty on failure). `if ! var=$(...)` negates the command's
# exit while still capturing its output. The single invocation avoids the
# wasteful double-call the original draft had.
if ! _a_reason="$(PYTHONPATH=scripts python3 -m gate.cycle assert-asan-ran "${_asan_args[@]}" 2>&1 1>/dev/null)"; then
  say "CYCLE FAILURE: ASan-ran evidence check FAILED — ${_a_reason#cycle: }"
  say "  Refusing to report dual-oracle green for a cycle that ran one oracle twice."
  say "  Log: $LOGDIR/${TAG}_full.log"
  exit 1
fi
# DUAL-CONSUME CROSS-CHECK: the inline grep must agree with gate.cycle. The grep
# is the OBSERVED-evidence authority (it caught the original silent-degradation
# defect); gate.cycle wraps it. If they disagree, the Python port is wrong.
if ! grep -qi 'asan\|sanitize' "$LOGDIR/${TAG}_full.log"; then
  if [ -n "$_fvf" ]; then
    say "DUAL-CONSUME DISAGREEMENT (asan-ran): gate.cycle assert-asan-ran passed but the inline log grep found no 'asan|sanitize' — the Python port is wrong; see test_cycle.AssertAsanRanTest"
  fi
  say "CYCLE FAILURE: the --full log mentions no sanitizer — the ASan oracle did NOT run."
  say "  Refusing to report dual-oracle green for a cycle that ran one oracle twice."
  say "  Log: $LOGDIR/${TAG}_full.log"
  exit 1
fi
# Legacy verdict-oracle cross-check (the INTENDED-mode half). gate.cycle
# assert-asan-ran already gates on this (it reads the same verdict's oracle field
# when --verdict-path is passed), so reaching here means it passed — this block is
# the dual-consume belt for that half, kept for 1 landing gate, then retired. It
# would only fire if assert-asan-ran and the direct read disagree (a Python-port
# bug); in that case it still refuses dual-oracle green, the conservative outcome.
if [ -n "$_fvf" ]; then
  _ora="$(PYTHONPATH=scripts python3 -m gate.verdict read --path "$_fvf" --field oracle 2>/dev/null)"
  if [ "$_ora" != asan ]; then
    say "DUAL-CONSUME DISAGREEMENT (asan-ran): gate.cycle assert-asan-ran passed but the verdict oracle='${_ora:-n/a}' (expected asan) — $_fvf"
    say "CYCLE FAILURE: the --full verdict says oracle='${_ora:-n/a}' (expected asan)"
    say "  Refusing to report dual-oracle green — the ASan oracle's verdict does not confirm it ran."
    exit 1
  fi
fi

# Conformance phase: the spec-example sweep (tests/conformance). This is a
# SEPARATE oracle from the gtest suite — it compiles + runs every spec example
# fragment against the just-built bezelc and checks the manifest expectations.
# per Plan-A 5.1: until this wiring NO gate ran conformance (grep found zero
# references in the gate scripts), so a conformance regression could NOT block a
# push. Wired here, in the ACCEPTANCE cycle (gate_and_push.sh → this script),
# not in gate.sh: conformance is oracle-independent (it does not run under ASan),
# so running it in both gate.sh passes would be redundant; one run per cycle is
# correct and keeps it off the gate.sh critical path other lanes edit
# concurrently. The runner cages each case itself (MemoryMax/MemorySwapMax=0 +
# timeout 30, sequential), and its own Plan-A 5.1 guard (PASS>0) fails a run that
# validated nothing. A conformance FAIL ⇒ cycle FAIL ⇒ no push.
say "conformance phase: running spec-example sweep"
if [ ! -x "$REPO/build/bezelc" ]; then
  say "CYCLE FAILURE: $REPO/build/bezelc not executable — conformance cannot run"
  exit 1
fi
if ! "$REPO/tests/conformance/run_conformance.sh" "$REPO/build/bezelc" \
      > "$LOGDIR/${TAG}_conformance.log" 2>&1; then
  say "CYCLE FAILURE: conformance sweep FAILED — see $LOGDIR/${TAG}_conformance.log"
  tail -15 "$LOGDIR/${TAG}_conformance.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
# Surface the PASS/FAIL/SKIP tally in the cycle log. The healthy green over the
# current manifest is 100/0 (16 error + 83 fragment + 1 program, 337 skip).
grep -E '^(PASS|FAIL|SKIP):' "$LOGDIR/${TAG}_conformance.log" \
  | sed 's/^/  conformance /' | tee -a "$CYCLE_LOG" >/dev/null
say "conformance phase: GREEN"

say "cycle COMPLETE: both oracles trustworthy-green + conformance green"
exit 0
