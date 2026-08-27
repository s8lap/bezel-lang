#!/usr/bin/env bash
# Run the integration gate and push main ONLY on a trustworthy PASS. One command,
# one completion signal — the blessed way to land a batch without ad-hoc
# pgrep/log waiters (which have self-matched and deadlocked; see the
# build-test-efficiency notes). Backgroundable:
#   nohup scripts/gate_and_push.sh > /var/tmp/bezel-scratch/gateN.log 2>&1 &
# and rely on the single completion notification.
#
# fix #2917 (follow-on to #2633): THIS SCRIPT NOW GATES ON BOTH ORACLES.
#
# It used to call `scripts/gate.sh "$@"`, forwarding whatever single mode it was
# given — so `gate_and_push.sh --full` pushed on the ASan oracle ALONE. #2633
# measured why that is unsafe: `--full` is a DIFFERENT oracle, not a stronger
# one. ASan replaces the allocator and changes layout, so a wild read that
# FAULTS on the plain build can land in mapped memory and pass under ASan. Its
# evidence is one binary, one filter: 3 failures plain, 0 failures under ASan,
# where the underlying bug was a SIGSEGV on the default build. #2633 records the
# consequence — runs gated almost exclusively with `--full`, leaving that whole
# defect class "structurally invisible to the gate, however many times it ran".
#
# #2633 established the rule; nothing enforced it on the path that actually
# pushes. That is what this closes: the dual-oracle requirement now lives in the
# code that pushes, not in which script someone remembers to call. (Same
# treatment as the undercount banner, which became an acceptance CONDITION
# rather than a note — and caught a `GATE PASS` printed over 4,823 of ~15,300
# tests two gates later.)
#
# Usage:
#   scripts/gate_and_push.sh                 # BOTH oracles, then push  <- default
#   scripts/gate_and_push.sh --full          # accepted, NO-OP: the cycle already
#                                            #   runs ASan. Kept because it is the
#                                            #   spelling in CLAUDE.md and in every
#                                            #   habit; it now means "and plain too".
#   scripts/gate_and_push.sh -d DIR -t TAG -n N    # forwarded to gate_full_cycle.sh
#   scripts/gate_and_push.sh --single-oracle MODE  # ESCAPE HATCH, loud; see below
#
# REFUSED: --quick (shard-only). It is a diagnosis mode that skips coverage by
# design, so it can never be the basis of a push. Run it via scripts/gate.sh.
set -u

# --- completion signalling (2026-08-12) -------------------------------------
# WHY: twice in the 2026-08-12 run the coordinator lost hours waiting on a gate
# that had already finished, because the wait-loop matched a process pattern that
# also appeared in the waiter's own command line (see scripts/wait_pid.sh). Once,
# a PASSING gate sat unnoticed for an hour. Publish identity + a terminal marker
# so no caller ever has to pattern-match a process list again:
#   $BEZEL_GATE_PIDFILE  - this script's pid, written at start, removed at exit
#   $BEZEL_GATE_DONEFILE - written ONCE on exit, containing the exit status
# Wait with:  scripts/wait_pid.sh "$BEZEL_GATE_PIDFILE"
BEZEL_GATE_PIDFILE="${BEZEL_GATE_PIDFILE:-/var/tmp/bezel-scratch/gate.pid}"
BEZEL_GATE_DONEFILE="${BEZEL_GATE_DONEFILE:-/var/tmp/bezel-scratch/gate.done}"
mkdir -p "$(dirname "$BEZEL_GATE_PIDFILE")" 2>/dev/null || true
rm -f "$BEZEL_GATE_DONEFILE" 2>/dev/null || true
echo $$ > "$BEZEL_GATE_PIDFILE"
# fix #3158: ONE EXIT trap, doing BOTH jobs.
#
# Bash supports exactly one EXIT trap; a second `trap … EXIT` silently REPLACES
# the first. This script had two — this one, and the serialization-lock cleanup
# registered ~60 lines below once the lock is acquired. The later registration
# won, so on the normal path (lock acquired) `_bezel_gate_signal_done` NEVER RAN:
# no donefile was ever written and the pidfile was never removed. The waiter
# machinery added in 383d01c to end the #3010 self-matching-waiter class was
# therefore dead on arrival — it was layered on top of an existing EXIT trap
# (2ea36f2) that nothing pointed out.
#
# Measured before the fix, in two seconds and on the real path:
#   $ scripts/gate_and_push.sh --quick     # refused at the arg loop, AFTER the
#                                          # lock is taken and the trap replaced
#   exit=64   donefile: ABSENT   pidfile: left behind
# — exactly the state a completed 2026-08-13 gate left, which is why a donefile
# waiter learned nothing from a gate that had exited cleanly with rc=0.
#
# `rc=$?` MUST stay the first statement: any command run before it clobbers the
# status being reported. The lock release therefore comes after the capture, and
# the donefile is written with the SAVED rc.
_bezel_gate_cleanup_ran=0
_bezel_gate_signal_done() {
  local rc=$?
  # Idempotent: a future signal trap may also route here, and writing the
  # donefile twice would race a waiter that is already reading it.
  if [ "$_bezel_gate_cleanup_ran" = "1" ]; then return $rc; fi
  _bezel_gate_cleanup_ran=1
  # Release the serialization lock, if this run ever took it. Empty on every
  # path that exited before acquiring, so this is a no-op there.
  if [ -n "${_gate_serialize_pid:-}" ]; then
    kill "$_gate_serialize_pid" 2>/dev/null || true
    wait "$_gate_serialize_pid" 2>/dev/null || true
  fi
  # Publish the terminal marker LAST, so a waiter that sees the donefile knows
  # the lock is already released and may start its own gate immediately.
  echo "$rc" > "$BEZEL_GATE_DONEFILE" 2>/dev/null || true
  rm -f "$BEZEL_GATE_PIDFILE" 2>/dev/null || true
  return $rc
}
_gate_serialize_pid=""
trap _bezel_gate_signal_done EXIT
# ⚠ DO NOT add another `trap … EXIT` in this file — it would silently replace
# the handler above, which is the bug #3158 records. Add the work to
# _bezel_gate_signal_done instead. scripts/gate/tests/test_gate_and_push_trap.py
# fails the build if a second one appears.
# ---------------------------------------------------------------------------

cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate_and_push: not in a git repo" >&2; exit 2; }

# fix #3413: SAY WHICH TREE. This script cds to the launching directory's
# worktree root and then runs THAT tree's scripts/gate_full_cycle.sh, so what
# follows is about this path and this commit — but until now nothing said so,
# and gate_full_cycle.sh then cd'd away to a hardcoded checkout while this
# banner-less script's caller assumed the worktree. Print the identity before
# any work, and reuse it at the push boundary below.
#
# `pwd -P` is the canonical spelling the cycle records in its verdict and the
# comparison below uses; a symlinked lane path must not read as another tree.
# The wording deliberately avoids "GATE PASSED", "GATE FAILED …->",
# "PUSHED origin/main now" and "NOT pushing": gate_wait.sh treats those as
# TERMINAL log markers, and a waiter that matched this opening banner would
# report a verdict before the gate had run.
GATE_REPO_ROOT="$(pwd -P)"
echo "== gate_and_push: tree = $GATE_REPO_ROOT =="
echo "== gate_and_push: head = $(git rev-parse --short HEAD 2>/dev/null || echo '?') ($(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')) =="

# ── THE TREE MUST BE CLEAN ─────────────────────────────────────────────────
# A gate VALIDATES THE WORKING TREE but PUSHES HEAD. When they differ, the
# verdict does not describe what gets pushed — and the difference can be exactly
# the thing under test. Measured the hard way on 2026-08-13: the ratchet baseline
# was regenerated after a cherry-pick and left unstaged, so the seam phase read
# 84 from the tree and passed while HEAD still claimed 95. That push would have
# handed back 11 units of ratcheted headroom, and the EXACT-baseline rule would
# only have caught it on the NEXT gate — after the bad state was public.
#
# CLAUDE.md already says the tree is frozen during a gate; this makes it
# enforceable instead of remembered. Untracked files are fine (scratch, new test
# sources not yet added); only TRACKED modifications block, because those are the
# ones that make HEAD and the tested tree disagree.
_dirty="$(git diff --name-only HEAD 2>/dev/null)"
if [ -n "$_dirty" ]; then
  echo "gate_and_push: REFUSING — tracked files differ from HEAD:" >&2
  echo "$_dirty" | sed 's/^/    /' >&2
  echo "  A gate validates the WORKING TREE but pushes HEAD. Commit or restore" >&2
  echo "  these first, or the verdict does not describe what would be pushed." >&2
  exit 65
fi

# Make scripts/ importable as the `gate` package so `python3 -m gate.serialize`
# resolves from any cwd. Honor an operator's pre-set PYTHONPATH (e.g. a CI
# harness that pins one) by appending rather than overwriting.
export PYTHONPATH="${PYTHONPATH:+${PYTHONPATH}:}scripts"

# gate-infra preflight-lockfile lane: acquire an exclusive flock on the gate's
# lockfile so two concurrent `gate_and_push.sh` invocations cannot both run.
# Without this, the preflight sees the OTHER gate's `cmake`/`run_parallel.sh`
# children as "builders active" and both gates deadlock: each one waits for
# builders (siblings) that never go away because the OTHER gate is the source.
# Acquired BEFORE the argument parse so the lockfile is held even when the
# caller passes --help (a long-lived watcher's --help probe is also gated, and
# that's the correct behavior — the active gate owns the lock until it exits).
# Bypass with GATE_SKIP_LOCK=1 (e.g. for a deliberately-overlapping diagnosis
# run); on a held lock, print a clear single-line message and exit 0 so the
# caller sees a clean "another gate is running" abort, not a phantom failure.
#
# The flock + sibling detection logic now lives in scripts/gate/serialize.py
# (post-run infra migration, see test_gate_serialization.py). This script is
# a thin wrapper: spawn the helper in a coproc so the lock is held for THIS
# script's lifetime, read the ACQUIRED/REFUSED verdict from the coproc's
# stdout, and kill the helper on EXIT (the kernel auto-releases the flock on
# process death — see serialize.py: `test_lockfile_released_on_subprocess_exit`).
mkdir -p /var/tmp/bezel-scratch 2>/dev/null || true
GATE_LOCK="/var/tmp/bezel-scratch/gate.lock"
if [ -z "${GATE_SKIP_LOCK:-}" ]; then
  # coproc: the helper's stdin/stdout are COPROC[1]/COPROC[0]; COPROC_PID is
  # the helper's PID. We read one line (ACQUIRED or REFUSED) and either
  # register a cleanup trap (ACQUIRED — the daemon keeps the lock alive
  # for this script) or print the refused-message and exit 0 (REFUSED —
  # the daemon already exited; we just need to wait+reap).
  coproc python3 -m gate.serialize acquire --path "$GATE_LOCK" --hold
  _gate_serialize_pid=$COPROC_PID
  IFS= read -r _gate_serialize_verdict <&${COPROC[0]} || _gate_serialize_verdict=""
  if [ "$_gate_serialize_verdict" != "ACQUIRED" ]; then
    # REFUSED (or read failed — treat as refused; the helper will reap itself).
    # Surface the holder's pid if we can read it from /proc/locks; otherwise
    # just print the path. Exit 0 — the caller (CI, a waiter) sees a clean
    # "another gate is running" rather than a gate failure, and the active
    # gate keeps its lock undisturbed.
    wait "$_gate_serialize_pid" 2>/dev/null || true
    _holder=""
    if command -v fuser >/dev/null 2>&1; then
      _holder=$(fuser "$GATE_LOCK" 2>/dev/null | tr -d ' ' || true)
    elif [ -r /proc/locks ]; then
      _holder=$(awk -v lk="$GATE_LOCK" 'index($0, lk){print $5; exit}' /proc/locks 2>/dev/null || true)
    fi
    if [ -n "$_holder" ]; then
      echo "gate_and_push: another gate is running (lock $GATE_LOCK held by pid $_holder); exiting cleanly." >&2
    else
      echo "gate_and_push: another gate is running (lock $GATE_LOCK held); exiting cleanly." >&2
    fi
    exit 0
  fi
  # ACQUIRED — the daemon must die (so the kernel releases the flock) when this
  # bash script exits. SIGTERM is caught by the helper's signal handler (clean
  # release); SIGKILL is the unconditional fallback.
  #
  # fix #3158: this used to register its own `trap … EXIT` here, which REPLACED
  # the donefile/pidfile handler set at the top of the file — bash keeps only one
  # EXIT trap. The release now lives INSIDE that single handler, keyed on
  # `_gate_serialize_pid` being non-empty, which is set immediately above. No
  # second trap.
fi

# The authoritative verdict file path (matches gate.sh:77 / gate_full_cycle.sh).
# Phase 3c (per the exit-code-trap): the gate writes its outcome here on every
# exit; this consumer cross-checks it against $? before pushing.
S=/var/tmp/bezel-scratch
GATE_VERDICT="${GATE_VERDICT_FILE:-$S/gate-verdict.json}"

CYCLE_ARGS=()
SINGLE_MODE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --full)
      # Historical spelling meaning "also run ASan". The cycle runs ASan
      # unconditionally, so this is now a no-op rather than a mode selector.
      # Accepted rather than rejected: every existing habit and CLAUDE.md spell
      # it this way, and silently UPGRADING them to both oracles is the safe
      # direction. Rejecting would only push callers back to bare gate.sh.
      echo "== gate_and_push: note — '--full' is a NO-OP here; this gate runs BOTH oracles (#2917) =="
      shift ;;
    --quick)
      echo "gate_and_push: REFUSING --quick." >&2
      echo "  --quick is shard-only: it skips coverage by design, so it cannot be the" >&2
      echo "  basis of a push (#2633/#2917). Use it for diagnosis via scripts/gate.sh," >&2
      echo "  then land the batch with a full scripts/gate_and_push.sh run." >&2
      exit 64 ;;
    --single-oracle)
      # Deliberate single-oracle gate, for bisection or for a tree where one mode
      # is known-broken for an unrelated reason. It still pushes — the point is
      # that the operator CHOSE it explicitly, rather than getting it by
      # forwarding a flag they did not think about, which is the exact failure
      # #2917 is about.
      [ $# -ge 2 ] || { echo "gate_and_push: --single-oracle needs a MODE (default|--full|--quick)" >&2; exit 64; }
      SINGLE_MODE="$2"; shift 2 ;;
    -d|-t|-n)
      [ $# -ge 2 ] || { echo "gate_and_push: $1 needs a value" >&2; exit 64; }
      CYCLE_ARGS+=("$1" "$2"); shift 2 ;;
    -h|--help)
      sed -n '2,38p' "$0"; exit 0 ;;
    *)
      echo "gate_and_push: unknown argument '$1' (see --help)" >&2; exit 64 ;;
  esac
done

if [ -n "$SINGLE_MODE" ]; then
  echo "== gate_and_push: SINGLE-ORACLE GATE ('$SINGLE_MODE') — NOT the full acceptance =="
  echo "==   #2633: the two oracles catch DIFFERENT defect classes; neither dominates.  =="
  echo "==   A push on one oracle is blind to the other's class. Chosen explicitly.     =="
  if [ "$SINGLE_MODE" = "default" ]; then set --; else set -- "$SINGLE_MODE"; fi
  GATE_CMD=(scripts/gate.sh "$@")
else
  GATE_CMD=(scripts/gate_full_cycle.sh "${CYCLE_ARGS[@]+"${CYCLE_ARGS[@]}"}")
fi

if "${GATE_CMD[@]}"; then
  # Phase 3c (per the exit-code-trap): cross-check the gate's exit against the
  # AUTHORITATIVE verdict file. $? is reliable HERE (a foreground command — no
  # pipeline/backgrounding), but the verdict is the structured source an EXTERNAL
  # observer (the coordinator, gate_wait.sh) will rely on across the
  # backgrounding boundary that bit it 4x. Assert they agree so the verdict
  # never silently contradicts the exit code a push is about to trust.
  #
  # --expect-head rejects a STALE verdict from a previous gate on the same shared
  # path (#3000 shape: a reader that proceeds on a document not its own). Exit 2
  # from the read CLI = missing/unparseable/stale — the writer is best-effort,
  # so fall back to $? (back-compat). Exit 1 = verdict present but NOT a
  # trustworthy pass — the exit and the verdict DISAGREE, which must never be
  # silently pushed on.
  #
  # Wave 3 (#3010): the conservative-non-accept reconciliation now lives in
  # gate.cycle.cross_check. At the push boundary the decision is binary
  # (accept=push / bail=refuse): legacy_decision is accept ($?=0 here — the gate
  # succeeded), verdict_decision is accept iff the verdict is a trustworthy pass
  # (read exit 0), bail iff present-not-pass (read exit 1), absent iff
  # missing/stale (read exit 2 -> fallback to legacy accept). cross_check returns
  # accept on agreement / bail on disagreement (never accept on disagreement —
  # the policy that stopped the 55-truncated-gates class). The legacy 3-way read
  # branch below is the dual-consume CROSS-CHECK (asserted to agree with
  # cross_check); keep it for 1 landing gate, then retire.
  _head="$(git rev-parse HEAD 2>/dev/null || echo '')"

  # ── fix #3413: A PRESENT VERDICT THAT NAMES ANOTHER TREE OR COMMIT IS A
  # ── REFUSAL, NOT A NOTE. This check must come BEFORE the --expect-head read.
  #
  # `verdict read --expect-head` collapses THREE different situations into
  # exit 2 — missing, unparseable, and stale — and the handler for exit 2 below
  # is "no usable cycle verdict (writer best-effort) — relying on $?", which
  # PUSHES. So a verdict that loudly disagreed about which commit was gated was
  # downgraded to a note and the push went ahead. That is not hypothetical: it
  # is the path Run 4 cycle 4 (2026-08-27) took. It pushed e9bd762 while
  # gate-verdict.json recorded head=d34c8b4 — the previous day's commit on
  # cycle-2-integration-v3, 56 files / +7775 lines away — and the only trace was
  # that note. Nine commits landed ungated.
  #
  # Reading the fields DIRECTLY separates the three cases: exit 2 here means the
  # verdict (or the field) is genuinely absent, which keeps the documented
  # best-effort fallback intact for old verdicts; a value that disagrees is
  # positive evidence that the gate validated something else, and the only safe
  # response to that is to refuse.
  _v_head="$(PYTHONPATH=scripts python3 -m gate.verdict read --path "$GATE_VERDICT" \
       --field head 2>/dev/null)"; _vhrc=$?
  if [ "$_vhrc" -ne 2 ] && [ -n "$_v_head" ] && [ "$_v_head" != "$_head" ]; then
    echo "== gate_and_push: REFUSING TO PUSH — the verdict describes a DIFFERENT COMMIT ==" >&2
    echo "   about to push : $_head" >&2
    echo "   verdict gated : $_v_head   ($GATE_VERDICT)" >&2
    echo "   A green verdict for another commit is not evidence about this one (#3413)." >&2
    echo "   This is the exact shape that let nine ungated commits reach main on 2026-08-27." >&2
    echo "   If the cycle's (best-effort) verdict write failed and this is a STALE file from" >&2
    echo "   an earlier gate, delete it and re-gate — do not push over an unexplained one." >&2
    exit 1
  fi
  # The `-n` guards above and below cover the read reporting the field PRESENT
  # but BLANK. That is unreachable for a cycle that exited 0 (both writers set
  # head/repo before the EXIT trap is even registered), and treating an
  # unforeseen blank as a refusal would be a false-closed on a green gate — so
  # blank falls back, a DISAGREEING VALUE refuses. Absent (exit 2) and blank are
  # the fallback; only positive evidence of another subject blocks the push.
  #
  # The same question, asked of the TREE. A verdict written before #3413 has no
  # repo field: exit 2 / empty, and this check stands down (back-compat) rather
  # than assuming agreement — an absent field is never evidence of a match.
  _v_repo="$(PYTHONPATH=scripts python3 -m gate.verdict read --path "$GATE_VERDICT" \
       --field repo 2>/dev/null)"; _vrrc=$?
  if [ "$_vrrc" -ne 2 ] && [ -n "$_v_repo" ] && [ "$_v_repo" != "$GATE_REPO_ROOT" ]; then
    echo "== gate_and_push: REFUSING TO PUSH — the verdict describes a DIFFERENT WORKING TREE ==" >&2
    echo "   about to push from : $GATE_REPO_ROOT" >&2
    echo "   verdict gated tree : $_v_repo   ($GATE_VERDICT)" >&2
    echo "   The gate validated another checkout; its result says nothing about this one (#3413)." >&2
    exit 1
  fi

  _v_out="$(PYTHONPATH=scripts python3 -m gate.verdict read --path "$GATE_VERDICT" \
       --expect-head "$_head" 2>/dev/null)"; _vrc=$?
  case "$_vrc" in
    0) _vdec=accept ;;        # verdict is pass+trustworthy
    1) _vdec=bail ;;          # present but not a trustworthy pass -> refuse
    *) _vdec="" ;;            # missing/stale -> fallback to $? (accept)
  esac
  # The push decision via gate.cycle: cross-check the verdict decision against
  # the legacy ($?=0 -> accept). Empty _vdec -> cross_check returns the legacy
  # (accept) — the fallback. bail -> refuse to push.
  _push_dec="$(PYTHONPATH=scripts python3 -m gate.cycle cross-check \
      --verdict-decision "${_vdec}" --legacy-decision accept 2>/dev/null)"
  if [ "$_push_dec" = accept ]; then
    : # verdict agrees (or absent) -> push.
  else
    if [ "$_vrc" -eq 2 ]; then
      echo "== gate_and_push: note — no usable cycle verdict at $GATE_VERDICT (writer best-effort) — relying on \$? =="
      # DUAL-CONSUME: cross_check returns accept for a missing verdict (the
      # documented fallback). If it returned anything else, the Python port
      # diverged from the legacy branch — and a non-accept reconciliation must
      # NEVER push (the conservative-non-accept policy that stopped the
      # 55-truncated-gates class). Refuse rather than fall through to push on a
      # disagreement.
      if [ "$_push_dec" != accept ]; then
        echo "== gate_and_push: DUAL-CONSUME DISAGREEMENT — missing verdict but cross_check said '$_push_dec' (expected accept/fallback) — see test_cycle.CrossCheckTest ==" >&2
        echo "== gate_and_push: REFUSING TO PUSH — cross_check returned '$_push_dec'; a non-accept reconciliation must never push. ==" >&2
        exit 1
      fi
    else
      echo "== gate_and_push: REFUSING TO PUSH — the gate exited 0 but the verdict file is NOT a trustworthy pass ==" >&2
      echo "   The exit code and the verdict disagree (per the exit-code-trap, this is exactly the class that pushed 55 truncated gates)." >&2
      echo "   Verdict: $GATE_VERDICT — inspect before trusting either source." >&2
      # DUAL-CONSUME: cross_check must have said bail for a present-not-pass
      # verdict against an accept legacy. If it said accept, the Python port is
      # wrong (it would push on the disagreement the whole policy exists to stop).
      [ "$_push_dec" != bail ] && \
        echo "== gate_and_push: DUAL-CONSUME DISAGREEMENT — present-not-pass verdict but cross_check said '$_push_dec' (expected bail) — see test_cycle.CrossCheckTest ==" >&2
      exit 1
    fi
  fi

  # fix #3010: THE LAST CHECK BEFORE A PUSH IS THE GATE'S OWN TELEMETRY.
  #
  # Both oracles have reported green by here. That is exactly the state in which
  # the 55 recorded pass-but-truncated gates were pushed: each one's coverage
  # field contradicted its verdict, in a file nobody read. This is advisory
  # inside gate.sh (a reporting step must not change a gate's exit code) and
  # BLOCKING here, which is the only place it can stop something.
  #
  # Exit 2 = the log could not be read at all. That is NOT a pass: it means
  # nothing was checked, and treating "no findings" and "no check" alike is the
  # #3000 shape.
  if [ -x scripts/gate_trend.sh ]; then
    trend_out=$(scripts/gate_trend.sh --last 2>&1); trend_rc=$?
    printf '%s\n' "$trend_out" | sed 's/^/   trend| /'
    case "$trend_out" in
      *"records scanned"*) ;;
      *) echo "== gate_and_push: REFUSING TO PUSH — gate_trend.sh produced no scan line;" >&2
         echo "   the telemetry check did not run, which is not the same as passing." >&2
         exit 1 ;;
    esac
    if [ "$trend_rc" -ne 0 ]; then
      echo "== gate_and_push: REFUSING TO PUSH — the gate's own telemetry contradicts its verdict (see above)." >&2
      echo "   Both oracles reported green; the recorded numbers say the run was not what it claimed." >&2
      echo "   This is the check that would have caught 55 pass-but-truncated gates over 24 days." >&2
      exit 1
    fi
  else
    echo "== gate_and_push: REFUSING TO PUSH — scripts/gate_trend.sh is missing or not executable." >&2
    exit 1
  fi

  echo "== gate_and_push: GATE PASS -> pushing =="
  if git push; then
    echo "== gate_and_push: PUSHED origin/main now $(git rev-parse --short origin/main) =="
    # Auto-GC on land: reap worktrees whose commits are now in origin/main
    # (branch refs kept; WIP/locked/current worktrees never touched), drop stale
    # ASan trees, and quarantine >48h root scratch. Best-effort — a GC hiccup
    # must never fail a push that already succeeded. Set BEZEL_NO_GC=1 to skip.
    if [ "${BEZEL_NO_GC:-0}" != 1 ] && [ -x scripts/gc_scratch.sh ]; then
      echo "== gate_and_push: auto-GC (landed worktrees + stale builds + root) =="
      scripts/gc_scratch.sh --landed-only --builds --root --apply || \
        echo "== gate_and_push: GC reported an issue (ignored; push already landed) =="
    fi
  else
    echo "== gate_and_push: PUSH FAILED (gate passed; resolve remote + push manually) =="
    exit 3
  fi
else
  rc=$?
  echo "== gate_and_push: GATE FAILED (rc=$rc) -> NOT pushing (main left with the batch committed; diagnose) =="
  exit 1
fi
