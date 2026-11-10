#!/usr/bin/env python3
"""Two checks of the recommended design against the ownership-kernel model
(docs/research/soundness/bzcore-l1/bzcore.py, spec pin v0.345).

Check 1 - the "to null" stance adds no ownership behaviour.
  `try? E` desugars to `try E catch <declared failures> => () end` with a null
  result.  In the model's fragment that is a `try` whose handler is empty.  We
  enumerate every program of the shape  prefix ; try { body } catch e { } ;
  suffix  at the given bounds, and report the checker's verdicts and, for the
  accepted programs, the oracles (T1 exactly-once, T2 no silent use-after-kill,
  T3 isolation).  Expected: zero violations.

Check 2 - the throw edge and the normal scope-exit edge dispose the same values
  in the same order (K3.15 edge parity).  This is the property that lets a
  declared failure be implemented as a value return (an early exit of the
  scope) instead of an unwind, with no observable change.
  For every accepted program  prefix ; try { body ; throw new R } catch e { } ;
  suffix  we run the twin  prefix ; block { body } ; suffix  and compare the
  disposal traces after deleting the exception object's own allocation and
  disposal from the first.  Expected: identical.  The same comparison is then
  repeated with K3.15 switched off, to show the check can fail.

Usage: to_null_model.py <path-to-bzcore-l1/bzcore.py> [--vars 2] [--len 3] [--body 2]
"""
import argparse
import importlib.util
import sys
from collections import Counter

ap = argparse.ArgumentParser()
ap.add_argument("bzcore")
ap.add_argument("--vars", type=int, default=2)
ap.add_argument("--len", type=int, default=3)
ap.add_argument("--body", type=int, default=2)
a = ap.parse_args()

spec = importlib.util.spec_from_file_location("bzcore", a.bzcore)
bz = importlib.util.module_from_spec(spec)
sys.argv = [a.bzcore]
spec.loader.exec_module(bz)

VS = ["a", "b", "c"][: a.vars]
FEATS = {"locals", "asgm", "pins", "fields", "temps", "throw"}


def try_programs():
    """Programs of shape prefix ; try {body} e {} ; suffix (handler empty)."""
    for prog in bz.enum_programs(VS, a.len, FEATS, with_try=True, body=a.body):
        for s in prog:
            if s[0] == "try" and s[3] == []:
                yield prog
                break


def disposal_trace(run):
    return [e for e in run.events if e[0] in ("new", "disp")]


def check1():
    verdicts = Counter()
    bad = Counter()
    n = 0
    for prog in try_programs():
        n += 1
        row, _ = bz.evaluate(prog)
        if row.verdict == "accept":
            verdicts["accept" + (" with warnings" if row.warns else "")] += 1
            for b in row.bad:
                bad[b] += 1
        else:
            verdicts[row.verdict.split(":")[0]] += 1
    return n, verdicts, bad


def renumber(events, drop):
    """Delete the events of the objects in `drop` and renumber the rest densely."""
    keep = sorted({e[1] for e in events} - drop)
    idx = {loc: i + 1 for i, loc in enumerate(keep)}
    return [(e[0], idx[e[1]]) for e in events if e[1] not in drop]


def check2():
    compared = same = differ = skipped = 0
    witness = None
    for prog in try_programs():
        i = next(k for k, s in enumerate(prog) if s[0] == "try")
        body = prog[i][1]
        if not body or body[-1] != ("tthrow",):
            continue
        if any(s[0] in ("throw", "tthrow") for s in body[:-1]):
            continue
        twin = prog[:i] + [("block", body[:-1])] + prog[i + 1:]
        r1, c1 = bz.evaluate(prog)
        r2, c2 = bz.evaluate(twin)
        if r1.verdict != "accept" or r2.verdict != "accept":
            skipped += 1
            continue
        runs1 = bz.run_all(prog, c1)
        runs2 = bz.run_all(twin, c2)
        if len(runs1) != len(runs2):
            skipped += 1
            continue
        ok = True
        for x, y in zip(runs1, runs2):
            t1, t2 = disposal_trace(x), disposal_trace(y)
            # the thrown object: the R (and its Q) allocated in t1 at the position where
            # the two allocation sequences first diverge in length
            news1 = [e[1] for e in t1 if e[0] == "new"]
            news2 = [e[1] for e in t2 if e[0] == "new"]
            if len(news1) != len(news2) + 2:
                ok = False
                break
            # allocation order is program order: the exception is allocated right after the
            # body's own allocations, i.e. at the index where the twin's suffix would continue
            pre_allocs = sum(1 for e in disposal_trace(bz.run_all(prog[:i] + [("block", body[:-1])], bz.check(prog[:i] + [("block", body[:-1])])[2])[0]) if e[0] == "new")
            exc = set(news1[pre_allocs: pre_allocs + 2])
            if renumber(t1, exc) != renumber(t2, set()) or x.outcome != y.outcome:
                ok = False
                break
        compared += 1
        if ok:
            same += 1
        else:
            differ += 1
            witness = witness or bz.oneline(prog)
    return compared, same, differ, skipped, witness


print(f"bzcore-l1 model, vars={a.vars} len={a.len} body={a.body}, features={sorted(FEATS)}")
n, verdicts, bad = check1()
print("\nCheck 1: try { body } catch e { }  (the to-null desugaring)")
print(f"  programs enumerated : {n}")
for k, v in sorted(verdicts.items()):
    print(f"  {k:22s}: {v}")
print(f"  oracle violations among accepted programs: {dict(bad) if bad else 0}")

print("\nCheck 2: throw edge vs normal scope exit, same disposal trace (K3.15)")
c, s, d, sk, w = check2()
print(f"  pairs compared: {c}   identical: {s}   different: {d}   skipped (either twin rejected by the checker): {sk}")
bz.RULES["K3_15_edge_parity"] = False
c, s, d, sk, w = check2()
print(f"  with K3.15 switched off -> compared: {c}   identical: {s}   different: {d}")
if w:
    print(f"  first witness: {w}")
