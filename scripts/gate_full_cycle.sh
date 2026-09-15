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
# The tree it gates (#3413): the repo root is derived from THIS SCRIPT's own
# location, so a gate follows the tree it was invoked from. The resolved path
# and HEAD are printed in the banner and recorded in the verdict. Launching
# another tree's copy of this script from your worktree is REFUSED (exit 66) —
# say BEZEL_GATE_REPO=<path> if gating a different tree is deliberate.
#
# Exit codes: 0 = both modes trustworthy-green. Non-zero = the gate's own rc for
# a real failure, 66 if the tree to gate could not be resolved or disagreed with
# the caller's (#3413), 67 if HEAD moved mid-cycle, or 99 if a mode never
# produced a trustworthy run.
#
# Environment: BEZEL_NO_GC=1 is set for the gate runs — gate_and_push.sh's
# post-push worktree reap is slow (~1 worktree / 14s) and has no business
# running inside a gate cycle. Reap separately at consolidation.
# Each gate.sh attempt also receives BEZEL_GATE_ATTEMPT=<n>, BEZEL_GATE_ORACLE=
# plain|asan and GATE_CANARY (forced to 0 on the --full pass — fix #3666: the
# canary caught nothing in 46 runs and cost a median 184 s under ASan; an
# operator's own GATE_CANARY=0 is honoured on both passes). The advisory
# clang-tidy tier runs ONCE per cycle here, after conformance, no longer
# inside gate.sh (where it ran once per oracle: ~547 s per cycle, no reader).

set -u

# ── WHICH TREE DOES THIS GATE VALIDATE? (fix #3413) ─────────────────────────
#
# This was two lines: `REPO=/home/stepan/w/code/bezel-lang; cd "$REPO"`. A
# literal. gate_and_push.sh cds to `git rev-parse --show-toplevel`, so launching
# it from a lane or integration worktree LOOKS like it gates that worktree — and
# then this script cd'd away to the shared checkout and gated whatever THAT tree
# had checked out. The verdict named a tree the operator never selected.
#
# The failure is silent and biased toward FALSE GREEN: the shared checkout
# normally sits on already-landed `main`, by construction the tree most likely
# to be clean, so a gate that misses its target cannot fail on the new work.
#
# MEASURED (Run 4 cycle 4, 2026-08-27): the cycle reported "BOTH oracles PASS,
# 19071/19071 gtests, 0 ASan" and pushed e9bd762, while its own
# gate-verdict.json recorded head=d34c8b4 — the previous day's commit on branch
# cycle-2-integration-v3. `git diff d34c8b4 e9bd762` is 56 files / +7775 lines.
# Nine commits reached main ungated. Two defects escaped through that hole and
# were found only by hand: #2837's regression test, whose assertions compare an
# 8-bit exit status against 501 and so were arithmetically unreachable from the
# day they were added, and a gap-pin whose rejection had been superseded two
# days earlier.
#
# The root now comes from the script's OWN location, so the gate follows the
# tree it was invoked from. BEZEL_GATE_REPO=<path> overrides it for the
# deliberate case — and, because it makes the operator NAME the tree, it is also
# the only way past the mismatch refusal below.
#
# FAIL CLOSED, ALWAYS: every failure to resolve a root exits, never falls back
# to a literal. A gate that cannot name the tree it validates must not run.

GATE_LAUNCH_DIR="$PWD"

# _gfc_root DIR -> the git worktree root containing DIR, as a PHYSICAL path, on
# stdout; empty output + rc 1 when DIR is not inside a worktree. `pwd -P`
# normalizes the symlinked lane paths scripts/lane_setup.sh creates, so two
# spellings of ONE tree can never read as a mismatch below (a false refusal on a
# correct tree would block every push, which is the one outcome worse than the
# bug being fixed).
_gfc_root() {
  local top
  top="$(git -C "$1" rev-parse --show-toplevel 2>/dev/null)" || return 1
  [ -n "$top" ] || return 1
  (cd "$top" 2>/dev/null && pwd -P) || return 1
}

if [ -n "${BEZEL_GATE_REPO:-}" ]; then
  REPO="$(_gfc_root "$BEZEL_GATE_REPO")" || REPO=""
  REPO_SOURCE="BEZEL_GATE_REPO override"
  if [ -z "$REPO" ]; then
    echo "gate_full_cycle: REFUSING — BEZEL_GATE_REPO='$BEZEL_GATE_REPO' is not inside a git worktree." >&2
    echo "  A gate that cannot name the tree it validates must not run (#3413)." >&2
    exit 66
  fi
else
  _gfc_self_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd -P)" || _gfc_self_dir=""
  REPO=""
  if [ -n "$_gfc_self_dir" ]; then
    REPO="$(_gfc_root "$_gfc_self_dir")" || REPO=""
  fi
  REPO_SOURCE="this script's own location"
  if [ -z "$REPO" ]; then
    echo "gate_full_cycle: REFUSING — cannot derive a repo root from this script's" >&2
    echo "  location ('${BASH_SOURCE[0]:-$0}'). A gate that cannot name the tree it" >&2
    echo "  validates must not run (#3413). Set BEZEL_GATE_REPO=<path> to say which." >&2
    exit 66
  fi
fi

cd "$REPO" || { echo "gate_full_cycle: REFUSING — cannot cd to '$REPO' (#3413)." >&2; exit 66; }

# The identity of the tree under gate, captured ONCE, here. Everything that
# reports on this cycle — the banner, the cycle log, the verdict — reads these
# variables, so a report can never describe a tree other than the one entered.
GATE_REPO_HEAD="$(git rev-parse HEAD 2>/dev/null || echo '')"
GATE_REPO_HEAD_SHORT="$(git rev-parse --short HEAD 2>/dev/null || echo '?')"
GATE_REPO_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"

# ── REFUSE A TREE THE CALLER IS NOT STANDING IN ─────────────────────────────
# Deriving the root from BASH_SOURCE fixes the common path, but not the one the
# coordinator hit twice in Run 6: invoking ANOTHER tree's copy of this script
# (`cd /my/worktree && /shared/checkout/scripts/gate_full_cycle.sh`). The script
# then correctly gates ITS OWN tree — which is still not the tree the operator
# meant, and the launcher still prints the worktree's HEAD.
#
# The rule is the tree, not just the sha: the tree is what gets configured,
# built, and tested, and two trees at one commit still differ in working-tree
# state and build state. Both HEADs are reported so an operator can see at a
# glance whether it was a mere path confusion or a genuinely different batch.
#
# A launch directory OUTSIDE any git worktree does NOT refuse: that is the
# documented detached-launch shape (`setsid nohup … &` from a scratch dir), and
# there is no second tree for the verdict to be confused with.
GATE_LAUNCH_ROOT="$(_gfc_root "$GATE_LAUNCH_DIR")" || GATE_LAUNCH_ROOT=""
if [ -n "$GATE_LAUNCH_ROOT" ] && [ "$GATE_LAUNCH_ROOT" != "$REPO" ] \
   && [ -z "${BEZEL_GATE_REPO:-}" ]; then
  _gfc_launch_head="$(git -C "$GATE_LAUNCH_ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
  {
    echo "== gate_full_cycle: REFUSING — this would gate a DIFFERENT tree than the caller's =="
    echo "==   tree it would gate : $REPO @ $GATE_REPO_HEAD_SHORT ($GATE_REPO_BRANCH)"
    echo "==     (root from $REPO_SOURCE)"
    echo "==   tree you launched from: $GATE_LAUNCH_ROOT @ $_gfc_launch_head"
    echo "==   A verdict that names one tree while the operator meant another is the"
    echo "==   #3413 false-green: Run 4 cycle 4 reported both oracles green over the"
    echo "==   previous day's commit and let nine ungated commits reach main."
    echo "==   Fix: cd into the tree you mean and run ITS scripts/gate_and_push.sh,"
    echo "==   or name the tree explicitly with BEZEL_GATE_REPO=<path> if deliberate."
  } >&2
  exit 66
fi

# The identity banner. Printed on stdout BEFORE any work, so a human or an agent
# reading a gate log from the top sees which tree the verdict below describes —
# the thing that was missing when Run 4's launcher printed the integration HEAD
# over a gate of the shared checkout.
#
# ⚠ This banner belongs HERE (the cycle's own stdout), never inside gate.sh.
# gate.sh's stdout is captured to $LOGDIR/${TAG}_{plain,full}.log, and the
# ASan-ran evidence check is `grep -qi 'asan\|sanitize'` over the --full log
# (see the assert-asan-ran block below). A repo path containing "asan" — an
# every-run occurrence under /var/tmp/bezel-scratch/build-asan-* — would satisfy
# that grep from the PATH alone, turning the silent-degradation detector into a
# tautology. An observability change is a change.
echo "== gate_full_cycle: TREE UNDER GATE =="
echo "==   repo: $REPO"
echo "==   HEAD: $GATE_REPO_HEAD_SHORT ($GATE_REPO_BRANCH)  — root from $REPO_SOURCE"
echo "==   launched from: $GATE_LAUNCH_DIR"

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
    # Print the contiguous comment block after the shebang. A literal line
    # range (this was `sed -n '2,40p'`) silently slides out of date every time
    # the header grows, and had already drifted past `set -u` into code.
    -h|--help) awk 'NR>1 && /^#/ {print; next} NR>1 {exit}' "$0"; exit 0 ;;
    *) echo "gate_full_cycle: unknown argument '$1'" >&2; exit 64 ;;
  esac
done

# The default tag is the sha captured on entry — the tree under gate, never a
# fresh read. `?` is what a repo with no commits yields; keep the original
# `notag` spelling rather than putting a `?` in every log filename.
if [ -z "$TAG" ]; then
  TAG="${GATE_REPO_HEAD_SHORT:-notag}"
  [ "$TAG" = "?" ] && TAG="notag"
fi
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
  # fix #3413: --repo names the tree this verdict describes, and --head is the
  # sha captured when the cycle ENTERED that tree (not a fresh read at exit, so
  # a HEAD that moved under a running gate cannot silently relabel the verdict
  # as being about the new commit). Together they make the verdict self-
  # describing: Run 4 cycle 4's forensic needed exactly these two fields and
  # only had one of them.
  PYTHONPATH=scripts python3 -m gate.verdict write --path "$CYCLE_VERDICT_FILE" \
    --rc "$rc" --head="${GATE_REPO_HEAD:-}" --repo="${REPO:-}" \
    --mode=cycle --oracle=dual 2>/dev/null || true
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
#                                  or ceiling-killed shard(s) only (#3667) —
#                                  the latter bounded at three, then bail
#   poisoned            -> retry    rc==0 + undercount distrust (#3001)
#   fail                -> bail     rc==1: real regression
run_until_trustworthy() {
  local log="$1"; shift
  local mode_desc="${1:-plain}"; shift || true
  local i rc v_verdict v_rc dec old_dec reason _oracle _canary _ceil
  # fix #3666: tell each gate.sh attempt WHICH attempt it is and WHICH oracle
  # the cycle intends (plain, or asan for --full), and turn the canary OFF on
  # the ASan oracle. Measured over 46 gate.sh runs: the canary caught nothing
  # (every real failure — 4 plain, 1 ASan — was found by the shard phase) and
  # on --full it cost a median 184 s (up to 286 s) whenever it ran, so its
  # expected value there is negative (≈ −46 s/cycle). The plain-oracle canary
  # is untouched, and an operator's own GATE_CANARY=0 is honoured on BOTH
  # passes (a literal `_canary=1` would have clobbered a documented knob).
  # gate.sh echoes the attempt on its shard-ceiling line. BEZEL_GATE_ORACLE is
  # the INTENDED oracle, for the log and child processes — gate.sh still
  # derives its own from MODE, and assert-asan-ran below still checks the
  # OBSERVED evidence, so this export changes nothing a green gate proves.
  # Pinned functionally by scripts/gate/tests/test_gate_full_cycle_pins.py.
  _oracle=plain
  case " $* " in *" --full "*) _oracle=asan ;; esac
  _canary="${GATE_CANARY:-1}"; [ "$_oracle" = asan ] && _canary=0
  # fix #3667: ceiling-only retries seen so far (bounded at three, below).
  _ceil=0
  for i in $(seq 1 "$MAX_ATTEMPTS"); do
    # BEZEL_GATE_CYCLE=1 tells gate.sh that a DUAL-oracle run is driving it, so
    # it suppresses its "this is one oracle, not an acceptance verdict" note
    # (#2917). GATE_VERDICT_FILE points THIS gate's verdict at a PER-ATTEMPT path
    # (the default is a SHARED file every gate overwrites — gate.sh:277), so the
    # consumer reads its OWN gate's verdict, not a stale/concurrent one.
    local vf="${log%.log}.attempt${i}.verdict.json"
    BEZEL_NO_GC=1 BEZEL_GATE_CYCLE=1 GATE_VERDICT_FILE="$vf" \
      BEZEL_GATE_ATTEMPT="$i" BEZEL_GATE_ORACLE="$_oracle" GATE_CANARY="$_canary" \
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
        # fix #3667: a ceiling-only retry — gate.sh exit 3 because the run's
        # ONLY defect was a shard killed at BEZEL_SHARD_TIMEOUT (no FAIL, no
        # crash) — is bounded at THREE. Two retries ride the warm bincache and
        # clear a cold-cache or contention kill; a third says this machine
        # cannot finish a shard inside the ceiling, and MAX_ATTEMPTS (40) more
        # warm re-runs of the whole suite will not change that. Bail with 1 so
        # the cycle reports a failure, never a verdict. The phrase is the one
        # gate.sh prints on its `== gate: RETRY — …` line; keep them in step.
        if grep -q 'ceiling-killed shard(s) only' "$log" 2>/dev/null; then
          _ceil=$((_ceil + 1))
          if [ "$_ceil" -ge 3 ]; then
            say "$mode_desc attempt $i: CEILING-KILLED SHARD(S) ONLY for the third time — a shard cannot finish inside the ceiling on this machine; giving up instead of a fourth attempt (log kept: ${log%.log}.attempt${i}.log)"
            return 1
          fi
          say "$mode_desc attempt $i: CEILING-KILLED SHARD(S) ONLY (no FAIL/crash; ceiling-only retry $_ceil of 2) — retrying on the warm bincache (log kept: ${log%.log}.attempt${i}.log)"
          sleep 60; continue
        fi
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

say "cycle start: repo=$REPO head=$GATE_REPO_HEAD_SHORT ($GATE_REPO_BRANCH) root-from='$REPO_SOURCE' launched-from=$GATE_LAUNCH_DIR — logs in $LOGDIR"

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

# ── IMPORTED-SYMBOL SEAM PHASE (#3152) ─────────────────────────────────────
# The second source check, and it is here for the same reason as the first: a
# new `dynamic_cast<const ast::TypeDecl*>(sym->decl)` classification read is
# invisible to BOTH sharded oracles. It answers correctly for every local
# symbol — which is what the whole suite is built from — and wrongly only
# across a module boundary, silently, with no diagnostic. Neither gate.sh pass
# can see that; a source ratchet can.
#
# The count may only go DOWN (a family from the audit landing) and may never go
# up. --self-test runs first and is not optional, for the reason recorded in
# check_vacuous_tests: a green from a matcher that matched nothing is worth
# nothing, and the multi-line spelling of this cast is how most real sites are
# written.
say "imported-symbol phase: detector self-test + seam ratchet (#3152)"
if ! python3 scripts/check_imported_symbol_reads.py --self-test \
      > "$LOGDIR/${TAG}_seam.log" 2>&1; then
  say "CYCLE FAILURE: check_imported_symbol_reads SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_seam.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_seam.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_imported_symbol_reads.py --strict \
      >> "$LOGDIR/${TAG}_seam.log" 2>&1; then
  say "CYCLE FAILURE: the imported-symbol seam GREW — a new AST-gated"
  say "  classification read answers wrong for every import (#3152). See"
  say "  $LOGDIR/${TAG}_seam.log"
  sed -n '/IMPORTED-SYMBOL SEAM/,$p' "$LOGDIR/${TAG}_seam.log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "imported-symbol phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_seam.log"))"

# ── ENUMERABLE-WRAP GRID PHASE (#3419) ─────────────────────────────────────
# The third source check, and it guards the same structural failure as the
# second: a classification answered independently at N sites, where the sites
# drift and nothing makes them fail together. Here the question is the §29.2
# (declared head × source family) wrap grid. The site that MINTS a wrapper
# frame and the site that CLAIMS it must agree exactly — a claim narrower than
# the mint LEAKS, a claim wider DOUBLE-FREES — and six such defects landed in
# the 2026-08-27 run alone, four of them found by accident by a lane sweeping
# for something else.
#
# Neither sharded oracle can see this class. Every spelling answers correctly
# for the cells its own tests cover and wrongly only for the cells the OTHER
# spelling covers, so a new spelling ships green. A source ratchet can see it.
#
# --self-test runs first and is not optional, for the reason recorded in
# check_vacuous_tests and repeated in check_imported_symbol_reads: a green from
# a matcher that matched nothing is worth nothing, and the multi-LINE spelling
# of these chains is how most real sites are written.
say "enumerable-grid phase: detector self-test + grid ratchet (#3419)"
if ! python3 scripts/check_enumerable_grid_spellings.py --self-test \
      > "$LOGDIR/${TAG}_grid.log" 2>&1; then
  say "CYCLE FAILURE: check_enumerable_grid_spellings SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_grid.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_grid.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_enumerable_grid_spellings.py --strict \
      >> "$LOGDIR/${TAG}_grid.log" 2>&1; then
  say "CYCLE FAILURE: the enumerable-wrap grid GREW — a new hand-spelling of"
  say "  the (declared head × source family) grid drifts from the wrap-creator"
  say "  that mints the frame (#3419). See $LOGDIR/${TAG}_grid.log"
  sed -n '/ENUMERABLE-WRAP GRID/,$p' "$LOGDIR/${TAG}_grid.log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "enumerable-grid phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_grid.log"))"

# ── LOCALDECL-ONLY WALKER PHASE (#3591) ────────────────────────────────────
# §21.1's runtime `const` is `ast::LocalConstDecl`, a DIFFERENT AST node from
# `ast::LocalDecl` with differently-named fields. A walker spelled
# `dynamic_cast<const ast::LocalDecl*>(s)` therefore silently skips every
# `const` local. That has been fixed SEVEN separate times one site at a time
# (#2802, #3375, #3402, #3416, #3417, #3427, #3436), with symptoms from a
# 420 B leak to a use-after-free — and until now it was the one duplicated-
# classification family with no detector, while its three smaller siblings
# above all had gate-enforced ones. Fixing instance N+1 by hand is not a
# ratchet; this is.
say "localdecl-only-walker phase: detector self-test + walker-family ratchet (#3591)"
if ! python3 scripts/check_localdecl_only_walkers.py --self-test \
      > "$LOGDIR/${TAG}_localdecl.log" 2>&1; then
  say "CYCLE FAILURE: check_localdecl_only_walkers SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_localdecl.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_localdecl.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_localdecl_only_walkers.py --strict \
      >> "$LOGDIR/${TAG}_localdecl.log" 2>&1; then
  say "CYCLE FAILURE: the LocalDecl-only walker family GREW — a new"
  say "  dynamic_cast<[const] ast::LocalDecl*> site with no sibling"
  say "  LocalConstDecl handling silently skips every §21.1 const local (#3591)."
  say "  See $LOGDIR/${TAG}_localdecl.log"
  sed -n '/LOCALDECL-ONLY WALKER FAMILY/,$p' "$LOGDIR/${TAG}_localdecl.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "localdecl-only-walker phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_localdecl.log"))"

# ── DEPRECATED-JIT-ORACLE PHASE (#3439) ────────────────────────────────────
# `runJit`/`runJitChecked` skip the ownership and definite-assignment passes
# and skip verifyModule, so a test on them proves less than its name suggests.
# CLAUDE.md has said "the count must only ever go down" — with the measured
# numbers beside it — for months, and on 2026-08-23 the count still rose 60→61
# (an uncalled `runJitOnly` helper, #3439) and survived FIVE DAYS and two full
# gate cycles. Nothing failed except that the rule lived in PROSE. Its three
# sibling checks above are scripts with self-tests; this one was a paragraph.
# That asymmetry is what this phase removes.
say "jit-oracle phase: detector self-test + deprecation ratchet (#3439)"
if ! python3 scripts/check_deprecated_jit_oracle.py --self-test \
      > "$LOGDIR/${TAG}_jitoracle.log" 2>&1; then
  say "CYCLE FAILURE: check_deprecated_jit_oracle SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_jitoracle.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_jitoracle.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_deprecated_jit_oracle.py --strict \
      >> "$LOGDIR/${TAG}_jitoracle.log" 2>&1; then
  say "CYCLE FAILURE: the deprecated JIT oracle count moved — a new test was"
  say "  written on the LENIENT oracle, or a port was not banked with --update"
  say "  (#3439). See $LOGDIR/${TAG}_jitoracle.log"
  sed -n '/check_deprecated_jit_oracle:/,$p' "$LOGDIR/${TAG}_jitoracle.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "jit-oracle phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_jitoracle.log"))"

# ── AMBIGUOUS-RAN-FALSE PHASE (#3892) ──────────────────────────────────────
# `runAot`'s `JitResult::ran` is false for FOUR outcomes, not one: a genuine
# compile-time rejection, a compiler crash, a runtime signal (the compiled
# program segfaulted/aborted), or a compile/run timeout / forced-ASan report
# — all four surface as an ordinary `Severity::Error` diagnostic at
# `SourceLocation{}` (0:0). A row that pins "the compiler rejects this" with
# nothing but `BEZEL_EXPECT_FALSE(r.ran)` (or `hasErrors()`, or a bare
# severity check) cannot tell them apart, and MEASURABLY passes for the wrong
# reason: reproduced directly (see scripts/check_ambiguous_ran_false.py's
# docstring) with a program that compiles clean and stack-overflows at
# runtime, which reports `r.ran=0` and a bare `!r.ran` assertion against it
# as `[ OK ]`. Two fixes in one batch (#3889, #3891) each independently hit
# this and each wrote the correct guard from scratch — #3891 copied #3889's
# helper verbatim, which is the "went from nonexistent to duplicated within
# one batch" evidence #3892 records. This phase ratchets what remains
# unprotected so the count can only fall.
say "ambiguous-ran-false phase: detector self-test + ratchet (#3892)"
if ! python3 scripts/check_ambiguous_ran_false.py --self-test \
      > "$LOGDIR/${TAG}_ranfalse.log" 2>&1; then
  say "CYCLE FAILURE: check_ambiguous_ran_false SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_ranfalse.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_ranfalse.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_ambiguous_ran_false.py --strict \
      >> "$LOGDIR/${TAG}_ranfalse.log" 2>&1; then
  say "CYCLE FAILURE: a new row pins a compile-time rejection using only"
  say "  \`.ran\`/\`hasErrors()\` with no message-text or real-location check"
  say "  (#3892) — it cannot distinguish a rejection from a runtime crash/"
  say "  signal/timeout/ASan report. See $LOGDIR/${TAG}_ranfalse.log"
  sed -n '/check_ambiguous_ran_false:/,$p' "$LOGDIR/${TAG}_ranfalse.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "ambiguous-ran-false phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_ranfalse.log"))"

# ── NULL-GUARD VACUOUS PHASE (#3992) ───────────────────────────────────────
# A pointer-typed local bound from a lookup/finder call and then guarded with
# a bare truthy `if (ptr) { ...EXPECT... }` (braced or brace-less) before any
# EXPECT runs inside it executes ZERO assertions when the lookup returns
# null — a compiler regression, a fixture that stopped parsing, or simply a
# fixture bug. gtest still reports `[ OK ]`: the same three-outcomes-
# collapsed-to-one-boolean discriminator problem #3892 already names for
# `runAot`'s `.ran`, generalized to any pointer-returning lookup helper.
#
# Found LIVE TWICE while fixing #3985 (tests/sema/base_member_lookup_test.cpp
# ::interface_member_resolves and tests/sema/member_access_resolution_test.cpp
# ::qualified_field_access_resolves — one compounded with a genuine parse
# failure, the other with NO parse defect at all, just a fixture declaring
# an unrelated class first) — "a prose ratchet is not a ratchet" (CLAUDE.md)
# applies directly: this shape recurred with no gate-enforced check for it,
# exactly the asymmetry that motivated check_deprecated_jit_oracle.py.
#
# --self-test runs first and is not optional, for the reason recorded in
# every sibling phase above: a green from a matcher that matched nothing is
# worth nothing, and this detector's own self-test reconstructs BOTH known
# live instances as its ground truth.
say "null-guard-vacuous phase: detector self-test + ratchet (#3992)"
if ! python3 scripts/check_null_guard_vacuous.py --self-test \
      > "$LOGDIR/${TAG}_nullguard.log" 2>&1; then
  say "CYCLE FAILURE: check_null_guard_vacuous SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_nullguard.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_nullguard.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_null_guard_vacuous.py --strict \
      >> "$LOGDIR/${TAG}_nullguard.log" 2>&1; then
  say "CYCLE FAILURE: a new row guards \`if (<ptr>) { ...EXPECT... }\` with no"
  say "  unconditional non-null assertion on <ptr> first (#3992) — a null"
  say "  lookup would execute zero assertions and still report [ OK ]. See"
  say "  $LOGDIR/${TAG}_nullguard.log"
  sed -n '/check_null_guard_vacuous:/,$p' "$LOGDIR/${TAG}_nullguard.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "null-guard-vacuous phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_nullguard.log"))"

# ── GATE SELF-TEST PHASE (#3415) ───────────────────────────────────────────
# The fourth source-level check, and the one that guards the checkers rather
# than the tree. `scripts/gate/tests/` holds the self-tests for the gate's own
# Python — verdict reading, telemetry, canary, repo identity, serialization,
# the gate_and_push EXIT-trap pin. Nothing in the repo executed them. Every
# reference to that directory outside it was a comment, and CLAUDE.md asserted
# that one of them "now fails the build", which was never true.
#
# That is the worst place in the tree to have an unenforced guard. These tests
# certify the machinery that decides whether ~19,000 gtests were really green;
# an undetected regression in it does not fail a gate, it makes a gate report
# green while validating nothing. Exactly that happened once: a gate was found
# validating the WRONG TREE, and nine commits reached main ungated. Two of the
# self-tests here (test_repo_identity, test_gate_and_push_trap) pin the fixes
# for that class, and until now nothing ran them.
#
# 33s, build-free, fail-closed — the same shape as the two phases above, and it
# belongs in the CYCLE for the same reason: it is a property of the tree, so
# running it twice inside gate.sh would only duplicate it.
#
# ⚠ COUNT RATCHETS, NOT JUST THE EXIT STATUS. `unittest` exits 0 for "OK" and
# also for "OK (skipped=590)", and a discovery failure that collects nothing
# exits 0 with "Ran 0 tests". A bare `if ! python3 …` would therefore accept
# precisely the outcome this phase exists to prevent — the directory unenforced
# with a green tick on top. So the phase asserts a FLOOR on tests run and a CAP
# on tests skipped, and both are checked even when the run reports OK.
#
# The skip cap is what keeps the two environment-gated tests honest. Two tests
# in test_integration_bash_python drive the REAL gate_and_push.sh against the
# REAL lockfile; they self-skip when a gate is running (they would unlink a
# lockfile a live gate holds) or when the tree is dirty (the entry point refuses
# with exit 65 by design). Inside a gate BOTH conditions hold, so 2 skips here
# is the expected steady state — and the cap means a third muted test fails the
# phase instead of quietly joining them.
say "gate-selftest phase: scripts/gate/tests (#3415)"
_gs_log="$LOGDIR/${TAG}_gate_selftest.log"
_gs_floor=560
_gs_skip_cap=4
# fix #3463: THE PHASE MUST NOT BE ABLE TO REACH THIS GATE'S OWN MARKERS.
#
# Four modules in that directory drive the real scripts/gate_and_push.sh, which
# publishes the running gate's pid and exit status to $BEZEL_GATE_PIDFILE /
# $BEZEL_GATE_DONEFILE. On Run 7 cycle 1 this phase deleted the live gate's
# pidfile and wrote "1" — a FAILED verdict — into its donefile, 20 seconds into
# a cycle that went on to pass both oracles; the coordinator's
# `scripts/wait_pid.sh` exited 3 about fifty minutes early. Per-module
# isolation (scripts/gate/tests/marker_isolation.py) fixes the four we know
# about; redirecting here is what makes the FIFTH one safe before anyone
# notices it exists. Per-command, so the rest of the cycle is untouched.
_gs_markers="$LOGDIR/${TAG}_gate_selftest_markers"
mkdir -p "$_gs_markers" 2>/dev/null || true
if ! BEZEL_GATE_PIDFILE="$_gs_markers/gate.pid" \
     BEZEL_GATE_DONEFILE="$_gs_markers/gate.done" \
     python3 -m unittest discover -s scripts/gate/tests -v \
      > "$_gs_log" 2>&1; then
  say "CYCLE FAILURE: the gate's OWN self-tests failed — the machinery that"
  say "  decides whether the suite was green is itself broken, so this cycle's"
  say "  verdict would mean nothing (#3415). See $_gs_log"
  sed -n '/^\(FAIL\|ERROR\):/,$p' "$_gs_log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
_gs_ran="$(sed -n 's/^Ran \([0-9]*\) tests\?.*/\1/p' "$_gs_log" | tail -1)"
_gs_skipped="$(sed -n 's/^OK (.*skipped=\([0-9]*\).*/\1/p' "$_gs_log" | tail -1)"
[ -n "$_gs_skipped" ] || _gs_skipped=0
if [ -z "$_gs_ran" ] || [ "$_gs_ran" -lt "$_gs_floor" ]; then
  say "CYCLE FAILURE: the gate self-tests reported OK but ran only"
  say "  ${_gs_ran:-0} tests (floor $_gs_floor) — discovery collected almost"
  say "  nothing, so the OK certifies nothing (#3415). See $_gs_log"
  exit 1
fi
if [ "$_gs_skipped" -gt "$_gs_skip_cap" ]; then
  say "CYCLE FAILURE: the gate self-tests skipped $_gs_skipped tests (cap"
  say "  $_gs_skip_cap) — an unenforced guard with a green tick is worse than"
  say "  an honest gap (#3415). See $_gs_log"
  sed -n '/skipped/p' "$_gs_log" 2>/dev/null | head -20 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "gate-selftest phase: GREEN ($_gs_ran tests, $_gs_skipped skipped)"

# Rule-corpus harness phase (tests/conformance/rules). Build-free and fast, so
# it sits beside the gate self-tests rather than in the conformance phase, which
# needs a bezelc. Three checks, all fail-closed:
#   1. the harness's own unit tests — the baseline ratchet decides whether a
#      corpus run is a pass, so a broken ratchet makes the verdict meaningless,
#      exactly as #3415 argued for the gate's own tests;
#   2. `corpus.py check` — manifest.tsv is GENERATED from the case headers, and
#      a stale one means the runner is running a different corpus than the tree
#      holds;
#   3. `corpus.py lint` — the manifest/cases bijection and the case-header
#      contract.
# The corpus itself is not run here: it needs a bezelc and a classified
# baseline (tests/conformance/rules/README.md, "Gate wiring").
say "rule-corpus phase: tests/conformance/rules harness"
_rc_log="$LOGDIR/${TAG}_rule_corpus.log"
# Exact-not-a-ceiling, as the deprecated-JIT-oracle baseline is: the floor is
# the harness's current test count, so headroom cannot accumulate unbanked. It
# only ever moves up — a lane that deletes a test lowers it deliberately.
_rc_floor=140
if ! python3 -m unittest discover -s tests/conformance/rules -v \
      > "$_rc_log" 2>&1; then
  say "CYCLE FAILURE: the rule-corpus harness self-tests failed — the ratchet"
  say "  that decides whether a corpus run passed is itself broken."
  say "  See $_rc_log"
  sed -n '/^\(FAIL\|ERROR\):/,$p' "$_rc_log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
_rc_ran="$(sed -n 's/^Ran \([0-9]*\) tests\?.*/\1/p' "$_rc_log" | tail -1)"
if [ -z "$_rc_ran" ] || [ "$_rc_ran" -lt "$_rc_floor" ]; then
  say "CYCLE FAILURE: the rule-corpus self-tests reported OK but ran only"
  say "  ${_rc_ran:-0} tests (floor $_rc_floor) — discovery collected almost"
  say "  nothing, so the OK certifies nothing. See $_rc_log"
  exit 1
fi
if ! python3 tests/conformance/rules/corpus.py check >> "$_rc_log" 2>&1; then
  say "CYCLE FAILURE: tests/conformance/rules/manifest.tsv is stale or the"
  say "  corpus does not lint — run 'python3 tests/conformance/rules/corpus.py"
  say "  build'. See $_rc_log"
  tail -20 "$_rc_log" 2>/dev/null | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "rule-corpus phase: GREEN ($_rc_ran harness tests, manifest current)"

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

# ── CLANG-TIDY PHASE (fix #3666) — ADVISORY, ONCE PER CYCLE ────────────────
# The clang-tidy tier (scripts/check_tidy.sh, changed TUs only) used to be a
# gate.sh phase, so it ran once per ORACLE. Measured over 46 gate.sh runs:
# 277 s median per run, ~547 s per two-oracle cycle — 24% of a ~2230 s cycle —
# uncached, and over IDENTICAL inputs both times (the HEAD-moved check below
# pins HEAD for the whole cycle, so the merge-base and the changed-file
# list cannot differ between the passes). The second run was a pure duplicate.
# It is advisory end to end (check_tidy.sh always exits 0; gate.sh never called
# `fail` on it) and has NO reader: no trend rule, telemetry field or self-test
# names a `tidy` phase. So it runs HERE, once, after the last verdict-bearing
# phase: a red cycle and every retry attempt pay nothing, and every green cycle
# still leaves the advisory findings in $LOGDIR for a human to read.
#
# ⚠ ADVISORY MEANS ADVISORY. Nothing in this block may `exit` or `return` on
# its result — findings, a tool failure, dev_heavy refusing to start, a
# missing compile database: every outcome is one `say` line and then the
# cycle carries on. What a green cycle proves is unchanged by this block; it
# can only ADD lines to the log. Pinned by
# scripts/gate/tests/test_gate_full_cycle_pins.py (exactly one invocation,
# after conformance-GREEN, before the HEAD-moved check, no exit/return in
# its block).
say "tidy phase: clang-tidy over changed src/*.cpp (advisory, once per cycle — fix #3666)"
_tidy_log="$LOGDIR/${TAG}_tidy.log"
if command -v clang-tidy >/dev/null 2>&1 && command -v run-clang-tidy >/dev/null 2>&1; then
  # fix #3718: the base is THIS BRANCH'S UPSTREAM, never the bare local `main`.
  # Nothing in the run workflow ever fast-forwards local `main`: an integration
  # branch TRACKS `origin/main` and gate_and_push.sh lands through that same
  # upstream (it resolves its push destination from branch.<b>.{remote,merge}
  # rather than a hardcoded "origin main" for exactly this reason, ~:517-536).
  # So local `main` is whatever it was when someone last checked it out, and
  # `git merge-base HEAD main` quietly widened this advisory sweep to every
  # file landed since then. Measured 2026-09-03 on the R12 integration head:
  # 16 changed src/**/*.cpp against the stale local `main` (ed5f814) versus 1
  # against origin/main (98931b5) — 2.7x-16x the files the cycle changed, at
  # >=15 min a cycle — and it re-rots the moment origin/main advances, i.e.
  # every cycle. What a merge-base needs is the local remote-TRACKING ref, so
  # this reads `@{upstream}` rather than the branch.<b>.merge pair
  # gate_and_push.sh reads (that one is the remote's OWN branch name, which is
  # not a rev here). `origin/main` is the fallback for a detached HEAD or a
  # lane branch with no upstream configured at all; `HEAD~1` if even that is
  # absent, as before. Pinned in test_gate_full_cycle_pins.py, both as source
  # (no code line may spell `merge-base HEAD main`) and functionally, against
  # a temp repo whose local `main` is deliberately stale.
  _tidy_up="$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)"
  if [ -z "$_tidy_up" ] || ! git rev-parse --verify --quiet "${_tidy_up}^{commit}" >/dev/null 2>&1; then
    _tidy_up="origin/main"
  fi
  _tidy_base="$(git merge-base HEAD "$_tidy_up" 2>/dev/null || true)"
  # `if` rather than `[ … ] && …`: a trailing short-circuit that evaluates
  # false is rc 1, which under a future `set -e` would end the cycle from
  # inside an advisory block (issue 3666's SHARPEN, same hazard).
  if [ -z "$_tidy_base" ]; then _tidy_base="HEAD~1"; fi
  _tidy_t0="$(date +%s)"
  # The same invocation gate.sh used to make, through the resource-capped
  # slice. `|| true` so that even under a future `set -e` this line cannot end
  # the cycle.
  scripts/dev_heavy.sh build scripts/check_tidy.sh build --changed "$_tidy_base" \
    > "$_tidy_log" 2>&1 || true
  _tidy_s=$(( $(date +%s) - _tidy_t0 ))
  # check_tidy.sh prints `check-tidy: N finding(s) …` when clang-tidy ran;
  # otherwise (no changed TU, no compile database, tool missing) its LAST
  # `check-tidy:` line says why — surface that rather than a bogus 0.
  # `|| true` on both (sharpen, issue 3666 review): under a future
  # `set -e -o pipefail` a no-match grep (rc 1 — the ordinary tool-absent
  # path) or an unreadable log (sed rc 2) would otherwise abort a GREEN
  # cycle from inside an advisory block. Pinned functionally under
  # `set -eu -o pipefail` with an empty log.
  _tidy_n="$(sed -n 's/^check-tidy: \([0-9][0-9]*\) finding(s).*/\1/p' "$_tidy_log" 2>/dev/null | tail -1)" || true
  _tidy_why="$(grep '^check-tidy:' "$_tidy_log" 2>/dev/null | tail -1)" || true
  [ -n "$_tidy_why" ] || _tidy_why="no check-tidy output (see log)"
  # A `0 finding(s)` from a run-clang-tidy that did not run is not a count:
  # check_tidy.sh prints it BEFORE its `… exited $rc … possible tool/config
  # failure` line, so when that (last) line is present it wins.
  case "$_tidy_why" in
    *"possible tool/config failure"*) _tidy_n="" ;;
  esac
  if [ -n "$_tidy_n" ]; then
    say "tidy phase: done in ${_tidy_s}s — $_tidy_n finding(s) vs $(git rev-parse --short "$_tidy_base" 2>/dev/null || echo "$_tidy_base") (advisory; log $_tidy_log)"
  else
    say "tidy phase: done in ${_tidy_s}s — no findings count: $_tidy_why (advisory; log $_tidy_log)"
  fi
  grep -E '^check-tidy:' "$_tidy_log" 2>/dev/null | head -10 | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null || true
else
  say "tidy phase: skipped — clang-tidy or run-clang-tidy not on PATH (advisory tier disabled)"
fi

# fix #3413 (the other half): THE TREE MUST STILL BE THE ONE THAT WAS GATED.
# CLAUDE.md freezes the tree for the duration of a cycle, but nothing enforced
# it, and the consequence is the same class as the hardcoded root — a green
# verdict about a commit that is no longer what a push would ship. It is also
# actively DANGEROUS downstream: gate_and_push.sh reads the verdict with
# --expect-head, a mismatch there returns "stale" (exit 2), and its stale branch
# falls back to $? and PUSHES with only a note. Catch it here, where the answer
# is still knowable, and fail closed.
_now_head="$(git rev-parse HEAD 2>/dev/null || echo '')"
if [ "$_now_head" != "$GATE_REPO_HEAD" ]; then
  say "CYCLE FAILURE: HEAD MOVED DURING THE CYCLE — gated ${GATE_REPO_HEAD:-?}, tree is now ${_now_head:-?}"
  say "  Everything above describes the commit that was gated, not the one now"
  say "  checked out. Refusing to report green for a tree that changed under it"
  say "  (#3413); the tree is frozen for the duration of a gate."
  exit 67
fi

say "cycle COMPLETE ($REPO @ $GATE_REPO_HEAD_SHORT): both oracles trustworthy-green + conformance green"
exit 0
