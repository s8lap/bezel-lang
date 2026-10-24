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

# ── NATIVE-ASAN-LEG SKIP PHASE (#3682) ────────────────────────────────────
# The per-test ASan leg funnels through sanitizerRuntimeMaster() (via
# asanRuntimeObject() or buildRuntimeSanitizerObject()), and every
# caller follows the same shape: bind the result, then `if (rc < 0)
# return;` (or `== -1`) — a guard that swallows the empty result as a
# silent SKIP. ~49 sites banked across 14 files at #3682 (the rest of
# the >150 the spec's line-based grep counted were unbound variables,
# not real swallows). The conflating_helpers count is post-#3676
# (commit 1 of this ratchet) AND post-#3682-fix-forward (cycle-9 round-2:
# the brace-matching bug fix uncovered 72 additional real conflations
# in 71 files, rebanked from 0 → 73 in scripts/native_asan_leg_skips.baseline).
#
# Two banked per-file counts, exact-not-ceiling baseline + a per-file
# grandfather ceiling. Marker protocol: `// ASAN-LEG-ONLY: <reason>`
# exempts the next swallow, `// ASAN-LEG-DEBT: #NNNN <reason>`
# justifies a rise above the grandfathered ceiling (same shape as
# LOCALDECL-ONLY / LOCALDECL-DEBT, check_localdecl_only_walkers.py).
say "native-asan-leg phase: detector self-test + skip-swallow ratchet (#3682)"
if ! python3 scripts/check_native_asan_leg_skips.py --self-test \
      > "$LOGDIR/${TAG}_asanleg.log" 2>&1; then
  say "CYCLE FAILURE: check_native_asan_leg_skips SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_asanleg.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_asanleg.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_native_asan_leg_skips.py --strict \
      >> "$LOGDIR/${TAG}_asanleg.log" 2>&1; then
  say "CYCLE FAILURE: the native ASan-leg skip surface ROSE (#3682) — a"
  say "  new \`if (<asan-var> == -1|<0) return;\` site, a new"
  say "  \`-fsanitize=address\` helper with a post-link \`return -1;\`, or"
  say "  a marked-rather-than-fixed swallowing guard. Each row reports a"
  say "  clean PASS while the ASan leg is unavailable — the failure mode"
  say "  #3653's \`detect_leaks\` regression reached the gate unseen for."
  say "  See $LOGDIR/${TAG}_asanleg.log"
  sed -n '/NATIVE ASAN LEG SKIPS:/,$p' "$LOGDIR/${TAG}_asanleg.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "native-asan-leg phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_asanleg.log"))"

# ── UNINSTRUMENTED-EMIT PHASE (#3917) ──────────────────────────────────────
# Hand-rolled `codegen::emitObjectFile` runners that bypass
# `bezel::test::forceSanitizeAddress()` (the predicate `--full` flows
# through) leave gate.sh --full blind to generated-code ASan on those
# programs. The capability gap is real: a 3-arg call defaults
# `sanitizeAddress` to false at src/codegen/LlvmBackend.hpp:102, LSan
# still catches a leak, but a use-after-free read performed by
# generated code is invisible - no shadow-memory check is emitted on
# the program's own loads/stores.
#
# Spec §5 ("Ratchet first, fix second" per #4452): there is no runtime
# half available for this hole, and that is worth stating rather than
# leaving implicit. The source shape is the only observable, so this
# ratchet freezes the bypassing_call and overpromising_row surfaces
# while the burn-down (wrapper + 87-file repoint + 42-row name audit)
# runs separately. See scripts/check_uninstrumented_emit_runners.py
# docstring for the full counting criteria.
say "uninstrumented-emit phase: detector self-test + source ratchet (#3917)"
if ! python3 scripts/check_uninstrumented_emit_runners.py --self-test \
      > "$LOGDIR/${TAG}_emitasan.log" 2>&1; then
  say "CYCLE FAILURE: check_uninstrumented_emit_runners SELF-TEST failed —"
  say "  the detector is broken, so its verdict on the tree means nothing."
  say "  See $LOGDIR/${TAG}_emitasan.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_emitasan.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_uninstrumented_emit_runners.py --strict \
      >> "$LOGDIR/${TAG}_emitasan.log" 2>&1; then
  say "CYCLE FAILURE: a test lowers a program with emitObjectFile() without"
  say "  threading forceSanitizeAddress(), so --full does not instrument"
  say "  that program's generated code — a \"no UAF\" row there can only"
  say "  observe a signal death (#3917). See $LOGDIR/${TAG}_emitasan.log"
  sed -n '/UNINSTRUMENTED-EMIT RUNNERS:/,/See issue #3917/p' \
    "$LOGDIR/${TAG}_emitasan.log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "uninstrumented-emit phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_emitasan.log"))"
# ── TOOLCHAIN-SKIP PHASE (#3733) ────────────────────────────────────────────
# `runAotCheckedImpl`'s `clang --version` probe at tests/support/aot_support.hpp
# returns the SKIP branch on failure, and the literal diagnostic it emits
# ("<name>: SKIP — C toolchain (clang) unavailable for the ASan link") is
# read by 1,518 `wasSkipped` / `SKIP (not a failure)` guards across 270
# files (#3733, measured 2026-09-17 at b57a712e). On a box without clang
# the whole ASan/LSan tier silently reports clean PASS having run no
# sanitizer at all — the #3633 silent-skip class, counted nowhere. The
# runtime half of #3733 makes the absence LOUD (counter + print + shard
# JSON field + run_parallel.sh floor); this phase freezes the
# 2,305-call-site surface so it cannot grow while the burn-down runs.
#
# Both halves are wired into the same gate cycle: a rise in the script's
# source-side count without a contemporaneous drop in absorbing_sites
# fails here, and a nonzero `checked_toolchain_skips` total at the
# shard level fails inside tests/run_parallel.sh (which gate.sh already
# consumes). The script's --self-test plants a fixture tree in
# TemporaryDirectory() and asserts the detector's own behaviour; this
# phase runs --self-test FIRST for the same reason every sibling phase
# does — a green from a matcher that matched nothing is worth nothing.
say "toolchain-skip phase: detector self-test + surface ratchet (#3733)"
if ! python3 scripts/check_toolchain_skip_surface.py --self-test \
      > "$LOGDIR/${TAG}_toolchainskip.log" 2>&1; then
  say "CYCLE FAILURE: check_toolchain_skip_surface SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_toolchainskip.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_toolchainskip.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_toolchain_skip_surface.py --strict \
      >> "$LOGDIR/${TAG}_toolchainskip.log" 2>&1; then
  say "CYCLE FAILURE: the runAotChecked toolchain-SKIP surface GREW — a new"
  say "  call site absorbs \"SKIP — C toolchain (clang) unavailable\" without"
  say "  asserting, so a box without clang runs that row's ASan tier vacuously"
  say "  (#3733). See $LOGDIR/${TAG}_toolchainskip.log"
  sed -n '/check_toolchain_skip_surface:/,$p' "$LOGDIR/${TAG}_toolchainskip.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "toolchain-skip phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_toolchainskip.log"))"
# ── NESTED-BODY-SCOPE PHASE (Family L anchor, defect-campaign-2026-09 §2.8)
# ─────────────────────────────────────────────────────────────────────────
# Block getters, indexer accessors, property thunks and disposer cores
# lower a user body into a standalone MEMOIZED function from the middle
# of expression lowering. Any per-body emitter field they fail to save
# and reset leaks the caller's context into the callee, and the cached
# function is baked once - whichever caller was lowered first in
# `std::unordered_map` pointer-hash order freezes the result for the
# whole module. #2702 closed six emitters under one RAII guard
# (`Emitter::NestedBodyScope`); the open members are emitters not on the
# guard (#3936 emitDisposerCore, #4151 makePropertyThunk, #2748's three
# unguarded sites) and fields missing from its canonical set.
#
# This phase ratchets coverage so a new lazily-memoized emitter cannot
# ship without the guard. Two banked counts: `uncovered_emitters` (a
# function in src/codegen matching the seven #2702 + #4151 named
# patterns whose body lacks `NestedBodyScope VAR(*this);`) and
# `incomplete_field_saves` (a `NestedBodyScope` constructor DEFINITION
# whose init list omits a canonical field).
#
# --self-test runs first and is not optional, for the reason recorded in
# every sibling phase above: a green from a matcher that matched
# nothing is worth nothing.
say "nested-body-scope phase: detector self-test + LazyBodyEmitter ratchet (Family L, #2702)"
if ! python3 scripts/check_nested_body_scope_coverage.py --self-test \
      > "$LOGDIR/${TAG}_nbscope.log" 2>&1; then
  say "CYCLE FAILURE: check_nested_body_scope_coverage SELF-TEST failed —"
  say "  the detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_nbscope.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_nbscope.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_nested_body_scope_coverage.py --strict \
      >> "$LOGDIR/${TAG}_nbscope.log" 2>&1; then
  say "CYCLE FAILURE: a lazily-memoized emitter does not construct"
  say "  NestedBodyScope, OR constructs it without the canonical field set —"
  say "  the caller's state leaks module-wide (#3936, #2723, #2628, #4150,"
  say "  #4151, #2748). See $LOGDIR/${TAG}_nbscope.log"
  sed -n '/check_nested_body_scope_coverage:/,$p' "$LOGDIR/${TAG}_nbscope.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "nested-body-scope phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_nbscope.log"))"

# ── TIMEOUT-NEGATIVES PHASE (#3732) ────────────────────────────────────────
# The compile / run TIMEOUT is the third member of the #3007 family
# (#3007 infra, #3733 toolchain-SKIP, #3732 timeout) that surfaced as a
# Severity::Error diagnostic byte-for-byte indistinguishable from a real
# compile REJECT. The harness's channel routes it through
# `aot_detail::markTimeout()` instead — Severity::NOTE, current test failed,
# `hasErrors()` stays FALSE — so every `BEZEL_EXPECT_TRUE(hasErrors())` and
# `BEZEL_EXPECT_FALSE(<var>.ran)` site across tests/ is structurally immune
# to a timeout in one edit per harness. The source half freezes the small
# residual (~24 hand-rolled `.line > 0 && .column > 0` rows at
# b57a712e+channel — measured 4 actual bare-location rows today) so the
# surface cannot grow while the burn-down runs; the channel half retires
# the SHAPE the script counts, so the script is RETIRED at done criterion
# (unlike the toolchain-skip / native-asan-leg / ambiguous-ran-false
# ratchets, which freeze permanent shape hazards — spec §4 burn-down).
#
# Phase order: immediately after toolchain-skip, before null-guard-vacuous
# (spec §0.3 ordering: toolchain-skip, native-asan-leg, timeout-negatives,
# uninstrumented-emit; this lane lands the third of the four new phases,
# wired beside the existing ratchet phases that already proved the pattern).
#
# --self-test runs first and is not optional, for the reason recorded in
# every sibling phase above: a green from a matcher that matched nothing
# is worth nothing, and this detector's own self-test plants the seven
# fixtures that partition the surface against the sibling
# check_ambiguous_ran_false ratchet.
say "timeout-negatives phase: detector self-test + ratchet (#3732)"
if ! python3 scripts/check_timeout_satisfiable_negatives.py --self-test \
      > "$LOGDIR/${TAG}_timeoutneg.log" 2>&1; then
  say "CYCLE FAILURE: check_timeout_satisfiable_negatives SELF-TEST failed"
  say "  — the detector is broken, so its verdict on the tree means"
  say "  nothing. See $LOGDIR/${TAG}_timeoutneg.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_timeoutneg.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_timeout_satisfiable_negatives.py --strict \
      >> "$LOGDIR/${TAG}_timeoutneg.log" 2>&1; then
  say "CYCLE FAILURE: a new row pins a compile-time rejection with a"
  say "  hand-rolled location check that a compile TIMEOUT can satisfy — a"
  say "  negative test can go green because the harness gave up (#3732)."
  say "  See $LOGDIR/${TAG}_timeoutneg.log"
  sed -n '/check_timeout_satisfiable_negatives:/,$p' \
      "$LOGDIR/${TAG}_timeoutneg.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "timeout-negatives phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_timeoutneg.log"))"

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

# ── LIT-SOURCE-NOT-IGNORED PHASE (#6523) ───────────────────────────────────
# The .gitignore rule that suppressed lit's in-tree build artifacts was written
# `tests/lit/*[!.]`, which reads as "a name containing no dot" and is not: git
# matches a gitignore glob against the path's BASENAME and lets `*` cross `/`,
# so `[!.]` tested only the LAST CHARACTER. Every new `*.bzl` row matched, and
# because git does not descend into an excluded directory it matched at EVERY
# depth too, so a row written into tests/lit/sema/ was swallowed as well.
#
# NOT silent: `git add tests/lit/<row>.bzl` prints "the following paths are
# ignored" and exits 1 (for a subdir it names the DIRECTORY as ignored). The
# hazard is that the row is absent from the commit while `git status` never
# mentions it, and that one non-zero path inside a multi-path `git add` is easy
# to lose.
#
# AND THAT RULE WAS NOT USELESS — replacing it carelessly is a real regression
# that review caught here. Measured on this tree with a real bezelc on PATH, a
# hand-run `lit tests/lit` (2513 tests, lit 18.1.8) leaves 2323 files under
# tests/lit: 1127 EXTENSIONLESS objects named for the row stem — written by the
# 2463 `-o`-less `%bezelc -c %s` RUN lines, because bezelc fills an empty
# outputPath with defaultOutput() (src/main.cpp:254), which strips the
# extension — plus 1196 dotted files. The 1127 were suppressed ONLY by the
# broken rule. An earlier attempt replaced it with `*.bzl.tmp` shapes, which
# left all 1127 exposed while this phase still reported GREEN.
#
# So BOTH halves are pinned, and the checker is the control that makes this
# phase mean something: it fails on the pre-fix rule AND on a rule that drops
# the suppression. Do not read this phase as "the .gitignore must ignore
# nothing". A lit source must be addable at every depth, AND every artifact
# shape lit is known to leave in the source tree must stay ignored (those are
# what turn `git status --porcelain` into a 1127-line wall that hides real WIP).
# The self-test reconstructs FOUR .gitignore states — the pre-fix rule, one
# with no suppression at all, and the two #6548 shapes (the #6523 block sitting
# BELOW the repo's own `__pycache__/` rule, and its anchored test-times rule) —
# and REQUIRES this detector to fail on each, on a path of the right SHAPE, so
# a green here is not a matcher that matched nothing and not a ratchet that
# certifies only the half it was written against.
say "lit-source-not-ignored phase: detector self-test + ratchet (#6523)"
if ! python3 scripts/check_lit_sources_not_ignored.py --self-test \
      > "$LOGDIR/${TAG}_litignore.log" 2>&1; then
  say "CYCLE FAILURE: check_lit_sources_not_ignored SELF-TEST failed — the"
  say "  detector no longer reproduces the .gitignore defect it exists to"
  say "  prevent, so its verdict means nothing. See $LOGDIR/${TAG}_litignore.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_litignore.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_lit_sources_not_ignored.py --strict \
      >> "$LOGDIR/${TAG}_litignore.log" 2>&1; then
  say "CYCLE FAILURE: .gitignore swallows a new lit source, or stops suppressing"
  say "  a lit build artifact (#6523). A swallowed source is silently absent from"
  say "  the commit; an exposed artifact FAILs untracked_guard.sh. See"
  say "  $LOGDIR/${TAG}_litignore.log"
  sed -n '/check_lit_sources_not_ignored:/,$p' "$LOGDIR/${TAG}_litignore.log" 2>/dev/null \
    | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "lit-source-not-ignored phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_litignore.log"))"

# ── LEAK-EXEMPT STATUS PHASE (#4557) ───────────────────────────────────────
# scripts/gate/sanitizer_leak_exempt.txt declares every test whose
# generated-code sanitizer LEAK report is a deliberate assertion (#4537), not
# a defect. Measured 2026-09-21: 38 rows, 19 citing no issue at all, and of
# 15 distinct issues cited, 13 CLOSED — a fix had landed on the CENTRAL case
# and left a residual axis leaking, with the axis's own tracker lost the
# moment the issue closed. The file had become the project's only record of
# a set of known, unfixed leaks, with no owner and no way to tell a
# deliberate, permanent leak-vs-crash/leak-vs-UAF trade from a fix nobody has
# scoped out yet.
#
# Every row's reason now must open with a required status:{defect,trade,
# residue} token (sanitizer_leak_exempt.txt's own header has the full
# contract); defect/residue rows also require issue:NNNN naming their OPEN
# tracker, cross-checked against the committed snapshot
# scripts/gate/leak_exempt_issue_state.tsv (refreshed by the separate,
# explicitly-manual scripts/check_leak_exempt_issue_state.sh — the gate stays
# hermetic; see that snapshot's own header for why the state check cannot be
# a live `gh` call here).
#
# --self-test runs first and is not optional, for the reason recorded in
# every sibling phase above: a green from a matcher that matched nothing is
# worth nothing, and this detector's own self-test proves both the structural
# half and the snapshot cross-check fail closed on synthetic fixtures,
# including the exact #4557 shape (a status:defect/residue row citing an
# issue the snapshot marks CLOSED).
say "leak-exempt-status phase: detector self-test + status ratchet (#4557)"
if ! python3 scripts/check_leak_exempt_status.py --self-test \
      > "$LOGDIR/${TAG}_leakexempt.log" 2>&1; then
  say "CYCLE FAILURE: check_leak_exempt_status SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_leakexempt.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_leakexempt.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_leak_exempt_status.py --strict \
      >> "$LOGDIR/${TAG}_leakexempt.log" 2>&1; then
  say "CYCLE FAILURE: a sanitizer_leak_exempt.txt row has no status: token, an"
  say "  invalid one, a defect/residue row with no issue: tracker, or a"
  say "  defect/residue row citing an issue the committed snapshot marks"
  say "  CLOSED (#4557) — a known leak has lost its owner again. See"
  say "  $LOGDIR/${TAG}_leakexempt.log"
  sed -n '/check_leak_exempt_status:/,$p' "$LOGDIR/${TAG}_leakexempt.log" 2>/dev/null \
    | head -60 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "leak-exempt-status phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_leakexempt.log"))"

# ── SEAM-ACCESSOR CONFORMANCE PHASE (#4539) ────────────────────────────────
# check_imported_symbol_reads.py ratchets the READS of the AST-vs-symbol-table
# import seam; #4539 named the class it is blind to: a read that is correct
# and necessary for the local path, consumed by code with no imported-side
# fallback on its failure branch. Three such consumers shipped silently in one
# cycle (#4480, #4508, #4510) before anyone wrote a cross-module test for the
# shape. #4510's own fix gave its consumer a BOTH-SOURCES ACCESSOR — resolves
# through the AST when present, through the symbol table when not — joining
# two pre-existing ones (formalOutParamType2 #2657/#3186/#3188/#3206,
# formalCtorOutParamType #2668/#3772). This phase ratchets that every
# registered accessor keeps at least one tests/module/ (cross-module) test
# citing one of its issues — #4539's own "cheapest, already demonstrated
# twice" candidate (a conformance obligation), not the harder "detect a
# missing fallback automatically" one (investigated and declined for this
# script — see its own module docstring for why a text-pattern scanner over
# src/ cannot reliably tell a real gap from an ALREADY-CORRECT or
# structurally-unreachable site without the same judgement a human lane
# applied to find #4589/#4590/#4591/#4592).
#
# --self-test runs first and is not optional, for the reason recorded in
# every sibling phase above.
say "seam-accessor-conformance phase: detector self-test + coverage ratchet (#4539)"
if ! python3 scripts/check_seam_accessor_conformance.py --self-test \
      > "$LOGDIR/${TAG}_seamaccessor.log" 2>&1; then
  say "CYCLE FAILURE: check_seam_accessor_conformance SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_seamaccessor.log"
  sed -n '1,20p' "$LOGDIR/${TAG}_seamaccessor.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_seam_accessor_conformance.py --strict \
      >> "$LOGDIR/${TAG}_seamaccessor.log" 2>&1; then
  say "CYCLE FAILURE: a registered both-sources accessor (#4539) has lost its"
  say "  tests/module/ cross-module conformance coverage — an accessor with"
  say "  no test proving the imported path still matches the local twin is"
  say "  the exact regression #4480/#4508/#4510 shipped silently. See"
  say "  $LOGDIR/${TAG}_seamaccessor.log"
  sed -n '/check_seam_accessor_conformance:/,$p' "$LOGDIR/${TAG}_seamaccessor.log" \
    2>/dev/null | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "seam-accessor-conformance phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_seamaccessor.log"))"

# ── §9.6 STRING-MEMBER TRANSCRIPTION PHASE (R35, the #7144 reviewer's R and S) ─
# Two tables in the compiler transcribe stdlib §9.6's `String` member list by
# hand: `kMintMethods` (src/ownership/Ownership_Checks.cpp) and the arity
# tables (src/sema/ExprTypecheck_Calls.cpp). A hand-maintained mirror of a
# document drifts — and it HAS drifted: #7133 added `PadCenter` to §9.6 and
# the mint set did not learn it, and #7153 added `PadCenter` and `ToString` to
# §9.6 and the arity table did not learn them. Each omission let a program
# through a gate that should have rejected it.
#
# These two ratchets DERIVE the expected set from the document itself, so a
# new §9.6 member cannot be added without a matching transcription. Both fail
# CLOSED (exit 2) when they cannot parse their inputs, so a broken checker
# never reads as a pass.
#
# They are BUILD-FREE SOURCE ratchets: they read the document and the source
# as text and compile nothing.
#
# ⚠ THE SCRIPT PATHS ARE SPELLED OUT AT EACH CALL SITE, not through a
# variable. `scripts/ratchet_sweep.sh` DISCOVERS build-free source ratchets
# by grepping this gate for the literal pattern `scripts/check_<name>.py`; a
# `python3 "scripts/${_sm_check}.py"` loop is invisible to that grep, so the
# two ratchets would be run by the gate but NEVER SWEPT — the one mechanism
# that runs every build-free ratchet on a tree the gate did not build.
# Keep the paths literal.
run_string_ratchet() {
  _sm_name="$1"
  _sm_script="$2"
  if ! python3 "$_sm_script" --self-test \
        > "$LOGDIR/${TAG}_${_sm_name}.log" 2>&1; then
    say "CYCLE FAILURE: ${_sm_name} SELF-TEST failed — the checker cannot"
    say "  detect a mismatch, so its verdict means nothing. See"
    say "  $LOGDIR/${TAG}_${_sm_name}.log"
    sed -n '1,20p' "$LOGDIR/${TAG}_${_sm_name}.log" 2>/dev/null | sed 's/^/  | /' \
      | tee -a "$CYCLE_LOG" >/dev/null
    exit 1
  fi
  if ! python3 "$_sm_script" --strict \
        >> "$LOGDIR/${TAG}_${_sm_name}.log" 2>&1; then
    say "CYCLE FAILURE: ${_sm_name} reports a mismatch between stdlib §9.6"
    say "  and the compiler's hand-maintained transcription of it — a member"
    say "  §9.6 declares that the compiler does not know about. See"
    say "  $LOGDIR/${TAG}_${_sm_name}.log"
    sed -n '/check_string_/,$p' "$LOGDIR/${TAG}_${_sm_name}.log" \
      2>/dev/null | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
    exit 1
  fi
  say "${_sm_name} phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_${_sm_name}.log"))"
}
run_string_ratchet check_string_mint_methods scripts/check_string_mint_methods.py
run_string_ratchet check_string_arity_table scripts/check_string_arity_table.py

# ── §7.4 NULLABLE-RECEIVER / STRING-PRODUCER GUARD PHASE (#7139) ───────────
# `Emitter::isPlusOneStringProducer` classifies a §9.6 string-producing
# instance method's receiver as a +1 PRODUCER without asking whether the
# receiver's DECLARED type is `string?`. That is sound ONLY because sema
# rejects every member access on an un-narrowed nullable receiver (§7.4,
# v0.343) — a `string?` receiver that reaches codegen has been narrowed
# non-null, and §7.11 (v0.343) says a narrowed operand IS a non-nullable
# operand.
#
# The premise is a CROSS-COMPONENT invariant, and it had no mechanical
# guard: before #7139 the recognizer consulted `tt->nullable` and every
# string-producing §9.6 instance method leaked its producer's reference on
# a narrowed `string?` receiver. The hazard that remains is the other
# direction — a future sema change admitting an un-narrowed `string?`
# receiver would make codegen classify a BORROW as a +1 producer, i.e. an
# over-release, with no diagnostic and no test failure.
#
# The ratchet pairs the two sides: the codegen arm must not consult
# nullability, the `?.` call arm must keep excluding String, every sema
# rejection of an un-narrowed nullable receiver must name §7.4, and the
# #7139 regression test must still pin that rejection.
#
# The script path is spelled out LITERALLY — see the note on the phase
# above: scripts/ratchet_sweep.sh discovers build-free source ratchets by
# grepping this gate for `scripts/check_<name>.py`.
run_string_ratchet check_nullable_string_producer_guard \
  scripts/check_nullable_string_producer_guard.py

# ── STRING-VIEW OWNERSHIP PHASE (#6469) ────────────────────────────────────
# `ast::QualifiedName::parts` is a `std::vector<std::string_view>`
# (src/ast/Node.hpp:42), and 139 `vector<string_view>` declarations exist
# across src/ and tests/. Pushing a `std::string` TEMPORARY into one leaves a
# view into storage that dies at the end of that full-expression. The defect
# is invisible to EVERY oracle this project owns: it does not crash, it
# compiles clean (a string_view binds to a std::string rvalue — that implicit
# conversion IS the trap), and `runAot`'s `exitCode == 0` and `assertNoLeak`
# are both satisfied because the program still runs.
#
# The #6460 fix (978588d0) shipped `qn->parts.push_back(std::string(colName))`
# in typeCall's collection-literal factory arm; the temporary died with the
# statement, so `TypeRef::headName()` (src/sema/TypeRef.hpp:80) read
# `parts[0]` as the EMPTY string and `ys.Count` / `ys[0]` lowered to NOTHING
# at exit 0, while the identical program under an explicit `var ys:
# Array<int>` annotation worked. Only a stdout oracle separates those two.
#
# This is the cheap guard #6469 asked for (its Proposal, option 2). The
# baseline is 0: the two live instances the detector found were in
# tests/sema/method_symbol_param_{modifier_3308,out_null_postcond_3313}_test.cpp
# and are fixed by this commit.
#
# --self-test runs first and is not optional, for the reason recorded in
# every sibling phase above: the detector's self-test plants the #6469 shape
# verbatim and three of the look-alike shapes that must NOT count.
say "string-view-ownership phase: detector self-test + ratchet (#6469)"
if ! python3 scripts/check_string_view_ownership.py --self-test \
      > "$LOGDIR/${TAG}_strviewown.log" 2>&1; then
  say "CYCLE FAILURE: check_string_view_ownership SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $LOGDIR/${TAG}_strviewown.log"
  sed -n '1,30p' "$LOGDIR/${TAG}_strviewown.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_string_view_ownership.py --strict \
      >> "$LOGDIR/${TAG}_strviewown.log" 2>&1; then
  say "CYCLE FAILURE: a std::string TEMPORARY (or a .c_str()/.data()/"
  say "  .substr() of one) is being pushed into a vector<string_view> — the"
  say "  view dangles and reads back as the EMPTY string, at exit 0, with no"
  say "  crash and no diagnostic (#6469). Push a static literal, or"
  say "  ctx.internString(...). See $LOGDIR/${TAG}_strviewown.log"
  sed -n '/string_view_ownership:/,$p' "$LOGDIR/${TAG}_strviewown.log" \
    2>/dev/null | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "string-view-ownership phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_strviewown.log"))"

# ── DANGLING-CAPTURE PHASE (#6475) ─────────────────────────────────────────
# The phase immediately above is a TEXT ratchet over src/ AND tests/; this one
# covers src/ only, with the compiler's OWN analysis of the actual type.
#
# ⚠ SCOPE OF THIS PHASE — read before assuming it closes a gap. It catches a
# view captured from a TEMPORARY argument (`push_back(std::string("x"))`).
# It does NOT catch a view captured from an IDENTIFIER argument
# (`push_back(localStdStringVar)` where `local` dies with its scope): that
# shape is missed by -Wdangling-capture, by -Wdangling-gsl, by
# bugprone-dangling-handle, AND by the text phase above, and is filed as
# #6479. tests/ is not gated here either — the PCH does not exist until the
# test binary is built — so tests/ rests on the text phase alone.
#
# Why it is not already gated, measured rather than assumed:
#  * clang 21.1.8 (Fedora 21.1.8-6.fc43) has -Wdangling-capture DEFAULT-ON: it
#    fires with no -W flag at all, with -Wno-all, and with -Wall alone. So the
#    diagnostic is already produced by every build.
#  * but BEZEL_WERROR is OFF by default (CMakeLists.txt:111), so the warning
#    prints into the build log and NOTHING blocks on it.
#  * and clang-tidy, which reports the same site via bugprone-dangling-handle,
#    runs only as an ADVISORY phase (below, ~line 1814) that always exits 0,
#    and that silently SKIPS — also exit 0 — when compile_commands.json is
#    absent. Two instruments, neither blocking. That is the defect.
#
# SCOPE src/ ONLY, and that is a measured constraint rather than a preference.
# Every tests/ TU in compile_commands.json carries a forced PCH include
# (-Xclang -include-pch -Xclang .../bezel_tests_pch.dir/cmake_pch.hxx.pch)
# that exists only after the test binary is BUILT. Sweeping tests/ before that
# makes clang die with "unable to read PCH file" and emit no diagnostic for ANY
# file — a checker reporting 0 because the compiler never ran. The phase
# therefore fails CLOSED if any TU does not compile, which is what makes that
# trap impossible to repeat silently. The tests/ scope is not left uncovered:
# the phase above is build-free and scans src/ AND tests/ as text.
#
# The baseline is 0, and it is EXACT, not a ceiling: above fails, below fails
# as STALE HEADROOM. The count is DISTINCT SOURCE LOCATIONS, not diagnostic
# instances — one bad line in a widely-included header is emitted once per
# including TU (MEASURED: a single planted line in src/ast/Node.hpp produced 47
# raw instances but 1 location), so counting instances would make the baseline
# a function of the #include graph.
#
# Like every sibling phase: --self-test runs first and is not optional. It
# compiles planted fixtures with the REAL clang and asserts the per-shape
# outcome, so a check whose instrument had gone dead fails rather than
# reporting a confident 0.
say "dangling-capture phase: instrument self-test + ratchet (#6475)"
if ! python3 scripts/check_dangling_capture.py --self-test \
      > "$LOGDIR/${TAG}_danglingcap.log" 2>&1; then
  say "CYCLE FAILURE: check_dangling_capture SELF-TEST failed — the"
  say "  instrument is broken or -Wdangling-capture is no longer firing, so"
  say "  its verdict means nothing. See $LOGDIR/${TAG}_danglingcap.log"
  sed -n '1,30p' "$LOGDIR/${TAG}_danglingcap.log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_dangling_capture.py --strict \
      >> "$LOGDIR/${TAG}_danglingcap.log" 2>&1; then
  say "CYCLE FAILURE: a value is captured by reference into storage that dies"
  say "  at the end of its full-expression — clang's own -Wdangling-capture"
  say "  fired in src/ (#6475). The view dangles and reads back as the EMPTY"
  say "  string, at exit 0, with no crash. Push a static literal, an owning"
  say "  copy, or ctx.internString(...). See $LOGDIR/${TAG}_danglingcap.log"
  sed -n '/dangling_capture:/,$p' "$LOGDIR/${TAG}_danglingcap.log" \
    2>/dev/null | head -40 | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "dangling-capture phase: GREEN ($(tail -1 "$LOGDIR/${TAG}_danglingcap.log"))"

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
# Bumped 560 -> 824 (r21-jobserver, non-author review follow-up): the floor
# had sat at its introduction value for months while the directory grew to
# 809 tests unbanked, then +15 for three new jobserver test modules/cases
# added alongside this comment. Same discipline as the rule-corpus floor
# below ("the floor is the harness's current test count, so headroom cannot
# accumulate unbanked. It only ever moves up") — measured via
# `python3 -m unittest discover -s scripts/gate/tests`, not guessed.
# Bumped 824 -> 895 (r21-orch-tools): measured 860 before the coordinator
# tooling tests, 895 with them (35 added: pick_stage/ledger, handback_check,
# run_status, worktree ownership), 2 skipped. Then 895 -> 899 for the
# subject-only issue-rule tests (scripts/pick_issues.sh), then 899 -> 905
# for the recorded/inferred --kind tests, then 905 -> 927 for the review
# round-4 tests (rebase/bisect hooks, patch-id note recovery, --continue,
# empty picks, trailing-flag hangs, handback read-only), then 927 -> 934 for
# the duplicate-patch-id / pruned-target / quoted-recipe tests.
_gs_floor=934
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

# ── GATE-SMART-PUSH-SELFTEST PHASE (smart-push, user directive 2026-09-18) ──
# The smart-push block at scripts/gate_and_push.sh's push boundary sits on the
# critical path: every gate run that is about to push goes through it. The four
# cases (no-op / remote-advanced / normal / diverged) are pinned by source AND
# by functional tests in scripts/gate/tests/test_gate_and_push_smart_push.py,
# and those tests are also picked up by the existing gate-selftest phase
# (discover scans scripts/gate/tests/). This phase re-runs JUST them so a
# failure surfaces as its own line in the cycle log — the gate_selftest floor
# of 560 + skip cap of 4 lets a regression slip silently if the smart-push
# tests stop running for any reason; this phase pins that they ran AND that
# they ran in non-trivial number.
#
# Smart-push tests are fast (~0.2s, all git/text work in temp dirs), so the
# re-run cost is negligible. Build-free, fail-closed, with the same floor/cap
# ratchet pattern the sibling phases use.
say "gate-smart-push-selftest phase: test_gate_and_push_smart_push (smart-push, 2026-09-18)"
_sps_log="$LOGDIR/${TAG}_smart_push_selftest.log"
_sps_floor=10
_sps_skip_cap=0
# Same isolation pattern as gate-selftest: redirect the gate's terminal
# markers away from the live gate's paths. The smart-push tests do not
# actually invoke scripts/gate_and_push.sh (they extract the block and
# source it in a subprocess), but the markers redirect is cheap defence
# in depth — a future test in this file might.
_sps_markers="$LOGDIR/${TAG}_smart_push_selftest_markers"
mkdir -p "$_sps_markers" 2>/dev/null || true
if ! BEZEL_GATE_PIDFILE="$_sps_markers/gate.pid" \
     BEZEL_GATE_DONEFILE="$_sps_markers/gate.done" \
     python3 -m unittest scripts.gate.tests.test_gate_and_push_smart_push -v \
      > "$_sps_log" 2>&1; then
  say "CYCLE FAILURE: the smart-push self-tests failed — the push-boundary"
  say "  race detector (no-op / remote-advanced / normal / diverged) is broken"
  say "  or out of date, so a green gate followed by 'PUSH FAILED' on a remote"
  say "  advance could reappear (smart-push, user directive 2026-09-18). See $_sps_log"
  sed -n '/^\(FAIL\|ERROR\):/,$p' "$_sps_log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
_sps_ran="$(sed -n 's/^Ran \([0-9]*\) tests\?.*/\1/p' "$_sps_log" | tail -1)"
_sps_skipped="$(sed -n 's/^OK (.*skipped=\([0-9]*\).*/\1/p' "$_sps_log" | tail -1)"
[ -n "$_sps_skipped" ] || _sps_skipped=0
if [ -z "$_sps_ran" ] || [ "$_sps_ran" -lt "$_sps_floor" ]; then
  say "CYCLE FAILURE: the smart-push self-tests reported OK but ran only"
  say "  ${_sps_ran:-0} tests (floor $_sps_floor) — discovery collected almost"
  say "  nothing, so the OK certifies nothing. See $_sps_log"
  exit 1
fi
if [ "$_sps_skipped" -gt "$_sps_skip_cap" ]; then
  say "CYCLE FAILURE: the smart-push self-tests skipped $_sps_skipped tests (cap"
  say "  $_sps_skip_cap) — an unenforced guard with a green tick is worse than"
  say "  an honest gap. See $_sps_log"
  sed -n '/skipped/p' "$_sps_log" 2>/dev/null | head -20 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "gate-smart-push-selftest phase: GREEN ($_sps_ran tests, $_sps_skipped skipped)"

# src/ citation phase (#4455). Build-free: it cross-references the §-citations
# in src/ against the section headings every document defines, so it sits here
# beside the gate self-tests rather than in the compile path.
#
# A citation is how a reader finds the authority for what the code does
# (CLAUDE.md, "Comments"). One that resolves nowhere is worse than none: it
# claims an authority and spends the reader's time disproving it. The first run
# found 298 sites — 165 citing subsections of a RETIRED §24, 113 issue numbers
# wearing a section sigil, 20 naming sections no document has.
#
# Self-test first, as every other ratchet here does: the checker reads ALL the
# documents' headings, and a checker that read only the spec's would call every
# rationale citation dangling — which is exactly what the first measurement did.
say "commit-trailer phase: scripts/check_commit_trailers.py"
_ct_log="$LOGDIR/${TAG}_commit_trailers.log"
if ! python3 scripts/check_commit_trailers.py --self-test > "$_ct_log" 2>&1; then
  say "CYCLE FAILURE: check_commit_trailers SELF-TEST failed — the checker"
  say "  cannot be trusted to report on the batch. See $_ct_log"
  tail -20 "$_ct_log" 2>/dev/null | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_commit_trailers.py --range origin/main..HEAD >> "$_ct_log" 2>&1; then
  say "CYCLE FAILURE: a commit in this batch does not end with exactly the one"
  say "  trailer CLAUDE.md mandates. Five lanes in cycle 17 emitted a Sonnet"
  say "  trailer plus a Claude-Session line because a host reminder told them"
  say "  to; CLAUDE.md overrides it. Rewrite with git filter-branch --msg-filter."
  say "  See $_ct_log"
  tail -40 "$_ct_log" 2>/dev/null | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "commit-trailer phase: GREEN (every commit carries exactly one mandated trailer)"

say "src-citation phase: scripts/check_src_citations.py"
_sc_log="$LOGDIR/${TAG}_src_citations.log"
if ! python3 scripts/check_src_citations.py --self-test > "$_sc_log" 2>&1; then
  say "CYCLE FAILURE: check_src_citations SELF-TEST failed — the checker cannot"
  say "  be trusted to report on the tree. See $_sc_log"
  tail -20 "$_sc_log" 2>/dev/null | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if ! python3 scripts/check_src_citations.py >> "$_sc_log" 2>&1; then
  say "CYCLE FAILURE: src/ carries §-citations that resolve to no document —"
  say "  a citation is how a reader finds the authority for the code (#4455)."
  say "  See $_sc_log"
  tail -40 "$_sc_log" 2>/dev/null | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "src-citation phase: GREEN (every §-citation in src/ resolves)"

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
# baseline (tests/conformance/rules/README.md, "Gate wiring") — see the
# RULE-CORPUS BASELINE PHASE below the doc-example conformance sweep, which
# runs it against baseline.tsv once a bezelc exists (#4621).
say "rule-corpus phase: tests/conformance/rules harness"
_rc_log="$LOGDIR/${TAG}_rule_corpus.log"
# Exact-not-a-ceiling, as the deprecated-JIT-oracle baseline is: the floor is
# the harness's current test count, so headroom cannot accumulate unbanked. It
# only ever moves up — a lane that deletes a test lowers it deliberately.
_rc_floor=154
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

# Conformance-harness self-test phase (tests/conformance, #4506). Build-free
# and fast, so it sits beside the gate self-tests and the rule-corpus harness
# tests above rather than in the conformance phase below, which needs a built
# bezelc. This directory holds the DOC-EXTRACTED conformance harness's own
# unit tests — the code that decides which spec examples are wrapped,
# chained, `env`-bound and classified (classify_wrap, needs_async_main, the
# `env` anchor regex, the chaining provider, `_snippet`, etc.) — as distinct
# from the spec-example sweep run later in this script
# (tests/conformance/run_conformance.sh), which actually needs a built
# bezelc. Until now nothing ran these: a regression here could flip
# conformance rows silently and the sweep would just report a different
# PASS/SKIP split with exit 0 (#4506). (tests/conformance/rules is a
# SEPARATE, hand-authored corpus with its own harness/self-tests, already
# covered by the rule-corpus phase above; this phase does not touch it.)
#
# Same ratchet shape as gate-selftest (#3415) and rule-corpus above: COUNT
# RATCHETS, NOT JUST THE EXIT STATUS. `unittest` exits 0 for "Ran 0 tests"
# and for "OK (skipped=N)" alike, so a bare exit-status check would accept
# precisely the outcome this phase exists to prevent — the directory
# unenforced with a green tick on top. So this phase asserts a FLOOR on
# tests run and a CAP on tests skipped.
say "conformance-harness-selftest phase: tests/conformance/test_conformance.py (#4506)"
_ch_log="$LOGDIR/${TAG}_conformance_harness_selftest.log"
# Exact-not-a-ceiling, as the deprecated-JIT-oracle and rule-corpus baselines
# are: this is the harness's current test count (95 at #4506's filing,
# raised to 101 by fix #4556's runner.py/_snippet changes — re-measure with
# `python3 -m unittest discover -s tests/conformance -p 'test_conformance.py'`
# rather than trusting either number) — it only ever moves up; a lane that
# deletes a test lowers it deliberately in the same commit.
_ch_floor=101
_ch_skip_cap=0
# `-p 'test_conformance.py'` (not a bare `-s tests/conformance`) so this phase
# can never silently absorb tests/conformance/rules/test_rules.py (already
# run, with its own floor, by the rule-corpus phase above) even if discovery
# behavior around subdirectories without __init__.py changes across Python
# versions — confirmed empirically that a bare `-s tests/conformance` does
# NOT currently descend into rules/, but the explicit pattern removes the
# dependency on that behavior entirely.
if ! python3 -m unittest discover -s tests/conformance -p 'test_conformance.py' -v \
      > "$_ch_log" 2>&1; then
  say "CYCLE FAILURE: the conformance harness's own self-tests failed — the"
  say "  code that decides which spec examples are wrapped, chained, env-bound"
  say "  and classified is broken, so the conformance sweep's PASS/SKIP split"
  say "  cannot be trusted (#4506). See $_ch_log"
  sed -n '/^\(FAIL\|ERROR\):/,$p' "$_ch_log" 2>/dev/null | head -40 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
_ch_ran="$(sed -n 's/^Ran \([0-9]*\) tests\?.*/\1/p' "$_ch_log" | tail -1)"
_ch_skipped="$(sed -n 's/^OK (.*skipped=\([0-9]*\).*/\1/p' "$_ch_log" | tail -1)"
[ -n "$_ch_skipped" ] || _ch_skipped=0
if [ -z "$_ch_ran" ] || [ "$_ch_ran" -lt "$_ch_floor" ]; then
  say "CYCLE FAILURE: the conformance harness self-tests reported OK but ran"
  say "  only ${_ch_ran:-0} tests (floor $_ch_floor) — discovery collected"
  say "  almost nothing, so the OK certifies nothing (#4506). See $_ch_log"
  exit 1
fi
if [ "$_ch_skipped" -gt "$_ch_skip_cap" ]; then
  say "CYCLE FAILURE: the conformance harness self-tests skipped $_ch_skipped"
  say "  tests (cap $_ch_skip_cap) — an unenforced guard with a green tick is"
  say "  worse than an honest gap (#4506). See $_ch_log"
  sed -n '/skipped/p' "$_ch_log" 2>/dev/null | head -20 \
    | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "conformance-harness-selftest phase: GREEN ($_ch_ran tests, $_ch_skipped skipped)"

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
# Surface the PASS/FAIL/SKIP tally in the cycle log, and ratchet it (#4506
# item 3). This comment used to just NAME the healthy green ("100/0, 16 error
# + 83 fragment + 1 program, 337 skip") without checking it — that is the
# v0.138-era 437-row corpus, and the comment had been silently wrong (the
# committed corpus's actual PASS/SKIP split moved on) for over a year with
# nothing noticing, because a comment is not a check. `run_conformance.sh`'s
# own Plan-A 5.1 guard only asserts PASS>0, so a corpus regression that drops
# PASS from 254 to, say, 60 (every doc-example case in one section starts
# erroring) would still exit 0 here.
#
# Floor on PASS (can only rise — a drop below the last-measured count is a
# regression), ceiling on SKIP (can only fall — a rise means something that
# used to run now doesn't, e.g. a case flipping from `fragment` to
# `skip:*`). Measured against $REPO/build/bezelc at this cycle's HEAD:
# PASS=254 (40 error + 211 fragment + 3 program), SKIP=414, FAIL=0 — exactly
# the 2026-09-21 coverage-pass numbers recorded in
# tests/conformance/README.md. Update both in the SAME commit that changes
# the corpus (a regeneration per #4505, or a coverage pass per the README's
# precedent), exactly as the rule-corpus floor above is updated when that
# harness's own test count changes.
_cf_log="$LOGDIR/${TAG}_conformance.log"
_cf_pass_floor=254
_cf_skip_cap=414
_cf_pass="$(sed -n 's/^PASS: *\([0-9]*\)/\1/p' "$_cf_log" | tail -1)"
_cf_fail="$(sed -n 's/^FAIL: *\([0-9]*\)/\1/p' "$_cf_log" | tail -1)"
_cf_skip="$(sed -n 's/^SKIP: *\([0-9]*\)/\1/p' "$_cf_log" | tail -1)"
grep -E '^(PASS|FAIL|SKIP):' "$_cf_log" \
  | sed 's/^/  conformance /' | tee -a "$CYCLE_LOG" >/dev/null
if [ -z "$_cf_pass" ] || [ -z "$_cf_skip" ]; then
  say "CYCLE FAILURE: could not parse PASS:/SKIP: tallies out of $_cf_log —"
  say "  the conformance sweep's own output format changed underneath this"
  say "  check (#4506 item 3)."
  exit 1
fi
if [ "$_cf_pass" -lt "$_cf_pass_floor" ]; then
  say "CYCLE FAILURE: conformance sweep PASS=$_cf_pass, below the floor of"
  say "  $_cf_pass_floor (FAIL=${_cf_fail:-?}, SKIP=${_cf_skip:-?}) — a case"
  say "  that used to compile/run no longer does (#4506 item 3). See $_cf_log"
  exit 1
fi
if [ "$_cf_skip" -gt "$_cf_skip_cap" ]; then
  say "CYCLE FAILURE: conformance sweep SKIP=$_cf_skip, above the cap of"
  say "  $_cf_skip_cap (PASS=${_cf_pass:-?}, FAIL=${_cf_fail:-?}) — a case that"
  say "  used to run has fallen back to skip (#4506 item 3). See $_cf_log"
  exit 1
fi
say "conformance phase: GREEN"
say "  PASS=$_cf_pass (floor $_cf_pass_floor), SKIP=$_cf_skip (cap $_cf_skip_cap), FAIL=${_cf_fail:-0}"

# ── RULE-CORPUS BASELINE PHASE (#4621) ─────────────────────────────────────
# The rule-corpus phase far above (tests/conformance/rules) only runs the
# corpus HARNESS's own self-tests and `corpus.py check` (manifest currency) —
# it never actually runs the corpus itself against baseline.tsv, so a
# REGRESSION (banked pass, observed fail) or an UNBANKED row (banked fail,
# observed pass) could sit red indefinitely with no cycle ever seeing it.
# That is exactly what happened: v0.307 (#4608) changed 3 case files and
# added 35 more without re-banking baseline.tsv, and nothing failed (#4621).
# This phase is the missing half, run HERE rather than beside the build-free
# harness phase because it needs a real bezelc — same reason the doc-example
# conformance sweep just above needs one, and the corpus's own README.md
# ("Gate wiring") says so explicitly: "a phase that can only be green [before
# classification] is not a check." Classification has now happened (v0.306,
# refreshed by #4621's banking pass), so REGRESSION/UNBANKED are real,
# checkable failure conditions.
#
# --self-test runs FIRST and is not optional, exactly like every sibling
# ratchet phase above: it proves, build-free, that runner.py's compare()
# still classifies all five (banked, observed) combinations per README.md's
# ratchet table, and that this checker's own summary-line parser and
# floor/cap logic fail closed on a vacuous or malformed summary rather than
# passing by default.
say "rule-corpus-baseline phase: detector self-test + full-corpus ratchet (#4621)"
_rcb_log="$LOGDIR/${TAG}_rule_corpus_baseline.log"
if ! python3 scripts/gate_rule_corpus_baseline.py --self-test \
      > "$_rcb_log" 2>&1; then
  say "CYCLE FAILURE: gate_rule_corpus_baseline SELF-TEST failed — the"
  say "  detector is broken, so its verdict means nothing. See $_rcb_log"
  sed -n '1,30p' "$_rcb_log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if [ ! -x "$REPO/build/bezelc" ]; then
  say "CYCLE FAILURE: $REPO/build/bezelc not executable — the rule corpus"
  say "  cannot run"
  exit 1
fi
if ! python3 scripts/gate_rule_corpus_baseline.py --strict --jobs 8 \
      --bezelc "$REPO/build/bezelc" \
      >> "$_rcb_log" 2>&1; then
  say "CYCLE FAILURE: the rule corpus has a REGRESSION or UNBANKED row"
  say "  against tests/conformance/rules/baseline.tsv, or ran an"
  say "  unexpectedly small number of cases (#4621). See $_rcb_log"
  tail -30 "$_rcb_log" 2>/dev/null | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "rule-corpus-baseline phase: GREEN ($(tail -1 "$_rcb_log"))"

# ── INTERNAL-RESIDUAL PHASE (#4761) ─────────────────────────────────────────
# The compiler has a deliberate "I cannot lower this but I will return something"
# convention: a lowering arm that fails its job emits `ctx_.diagnostics().
# report(Severity::Warning, span, "internal: ...")` and continues, rather than
# aborting compilation. The literal `internal:` prefix is the project's marker
# for silent-wrong-code paths — "not a diagnostic" in the spec-cited wording,
# because the program compiles and exits 0, and three measurable reasons
# (per the issue body) keep that shape from being caught today:
#
#   1. runAot's JitResult::diagnostics is populated ONLY on a REJECT; a
#      Warning / Note / `internal:` on a program that compiles and runs is
#      never surfaced through the harness. The stderr that carried it is
#      discarded on success.
#   2. lit FileCheck: only 27 of the lit corpus pin `CHECK-NOT: internal:`.
#   3. The rule-corpus verdict cannot see it on accept rows either.
#
# The `#3079` / `#2434` silent-degrade class has been sighted 7 times. Every
# sighting: an `internal:` warning on a program that COMPILED and EXITED 0.
# None of the above can see it; the script below can.
#
# `scripts/check_internal_residual.py` measures the total `internal:` count
# across the lit corpus + non-rejects main-conformance cases + run/accept
# rules-cases (per the script's docstring: three populations, one shared
# diagnostic prefix). The baseline in scripts/internal_residual.baseline is
# EXACT, not a ceiling — a count below the baseline fails as STALE HEADROOM
# (a fix landed but was not banked), and `--update` refuses to raise without
# `--force` (a rise is a deliberate act that names the new emitter in its
# commit body).
#
# The baseline was measured at r20-c25 (5a528344) against
# /mnt/fast/lane-builds/r20-l1-e031b0e1/bezelc (SHA = 5a528344 = r20-c25):
# `total_internal_lines = 0` — the current tree emits none on the corpus.
# Update it with `python3 scripts/check_internal_residual.py --update` after
# porting a fix that removes the last remaining emission; the script refuses
# a rise without `--force`.
#
# Like rule-corpus-baseline (#4621), this phase needs a built bezelc, so it
# sits beside that one rather than in the source-ratchet block above. The
# --self-test runs FIRST for the same reason every sibling phase pins: a green
# from a matcher that matched nothing is worth nothing, and the detector's
# self-test (synthetic tree + fake bezelc) is build-free.
say "internal-residual phase: detector self-test + \`internal:\` count ratchet (#4761)"
_ir_log="$LOGDIR/${TAG}_internal_residual.log"
if ! python3 scripts/check_internal_residual.py --self-test \
      > "$_ir_log" 2>&1; then
  say "CYCLE FAILURE: check_internal_residual SELF-TEST failed — the"
  say "  detector is broken, so its verdict on the tree means nothing."
  say "  See $_ir_log"
  sed -n '1,30p' "$_ir_log" 2>/dev/null | sed 's/^/  | /' \
    | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
if [ ! -x "$REPO/build/bezelc" ]; then
  say "CYCLE FAILURE: $REPO/build/bezelc not executable — the internal-"
  say "  residual detector cannot run against the corpus"
  exit 1
fi
if ! python3 scripts/check_internal_residual.py --strict \
      --bezelc "$REPO/build/bezelc" \
      >> "$_ir_log" 2>&1; then
  say "CYCLE FAILURE: the \`internal:\` emission count moved against"
  say "  scripts/internal_residual.baseline — a silent-wrong-code"
  say "  emitter shipped green, or the baseline is stale headroom (#4761)."
  say "  See $_ir_log"
  tail -30 "$_ir_log" 2>/dev/null | sed 's/^/  | /' | tee -a "$CYCLE_LOG" >/dev/null
  exit 1
fi
say "internal-residual phase: GREEN ($(tail -1 "$_ir_log"))"

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
