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
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate_and_push: not in a git repo" >&2; exit 2; }

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
      # DUAL-CONSUME: cross_check said accept for a missing verdict (fallback);
      # if it said otherwise, the Python port diverged from the legacy branch.
      [ "$_push_dec" != accept ] && \
        echo "== gate_and_push: DUAL-CONSUME DISAGREEMENT — missing verdict but cross_check said '$_push_dec' (expected accept/fallback) — see test_cycle.CrossCheckTest ==" >&2
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
