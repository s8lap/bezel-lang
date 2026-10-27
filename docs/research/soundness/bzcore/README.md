# bzcore — a toy executable model of the ownership kernel (research prototype)

A ~450-line Python rendering of proposal 0047's Layer-1 calculus, limited to class-typed
**locals**: the K1 states, the K3 consumes with their post-states (K3.3 `send`, K3.4 `move`,
K3.9 `disp`, K3.11 drop-on-overwrite), K4.2 site-announced kills, K5's freeze over a backward
liveness pass (K5.1–K5.3, K5.5), K7's join, K3.14 affine scope-end disposal, §28.2's
use-after-dispose warning and K4.1's flagged heap. **Not modelled:** fields (K9), frames (K6),
full-expression temporaries (K8), exceptions (K3.15), `using var` (A5), collections, generics.
It is not a reference model and it proves nothing; it shows the shape of three tools the report
recommends, on real numbers.

Spec pin: v0.344 (the kernel text of §28.3.1 as read on 2026-10-27). No dependencies.

```
python3 -I bzcore.py examples                     # the spec's worked shapes: verdict, warnings, run trace
python3 -I bzcore.py enumerate --vars 3 --len 5   # 308,526 straight-line programs, 50,529 accepted, 0 violations (3.5 s)
python3 -I bzcore.py enumerate --vars 3 --len 4 --if
python3 -I bzcore.py mutate --vars 3 --len 4 --if  # delete / weaken one kernel clause at a time (11 s)
python3 -I bzcore.py mutate --vars 3 --len 5       # straight-line only (40 s)
```

Outputs of the runs recorded for the report are in `out/`.

## What it checks

For every accepted program and every branch path:

- **T1 exactly-once disposal** — every allocation that stays in this domain has exactly one
  `disp` event; none is disposed and then sent; the scope-end machinery never re-disposes.
- **T2 no silent use-after-kill** — a read through any alias of a disposed object takes the
  checked-mode `FAULT` (K4.1); a read of a moved binding never executes (`STUCK` is a defect).
- **T3 isolation (the one-sink form)** — `send x` moves the object to another domain where no
  flag is ever set (K4.3's domain clause); any later read through a surviving borrow is a
  `RACE`. This is the v0.180 counterexample's shape (rationale §5.1).

Scope-end disposal is driven by the **static** classification the checker computed, as a
compiler's emission would be, with the per-path runtime tag consulted only for
*maybe-consumed*. That is what lets a join mutation show its leak.

## What the mutation table says (vars 3, len 4, with a branch; 67,002 programs, 12,327 accepted)

| mutation | verdicts changed | newly accepted / unsound | witness |
|---|---|---|---|
| K5.1 deleted (transfers unfrozen) | 78 | 78 / 36 | `new a; brw b a; send a; use b` → RACE |
| K5.2 deleted (drop-on-overwrite unfrozen) | 72 | 72 / **0** | every new acceptance is flag-backstopped |
| K5.3 flipped (Dispose() frozen too) | 60 | 0 / 0 (60 newly rejected) | pure over-rejection |
| K7.3 weakened (owned ⊓ borrowed → borrowed) | 144 | 144 / 144 | `new a; decl b; if {asg b} else {asgb b a}` → leak |
| K7.4 weakened (owned ⊓ moved → owned) | 3,303 | 3,303 / 2,499 (5,139 unsound overall) | `new a; if {} else {disp a}; send a` |
| K1.3 deleted (moved-from readable) | 7,857 | 7,857 / 7,260 | `decl a; use a` → STUCK |
| K3.13 flipped (unmarked read consumes) | 2,646 | 1,008 / **0** | a different language, equally sound |
| scope end ignores the explicitly-disposed post-state | 0 | 0 / 0, but 1,521 accepted programs double-dispose | machine-side defect, invisible to verdicts |
| §28.2 warning dropped | 0 | 0 / 0 | 450 → 177 warned programs |

Two readings matter for the report. First, the harness separates the clauses that carry
*soundness* (K5.1, K7.3, K7.4, K1.3) from those that carry *ergonomics or readability*
(K5.2, K5.3, K3.13): deleting K5.2 admits 72 more programs and every one of them is
backstopped by the disposed flag, exactly as K5.2's own text says ("the flag it does set is
irrelevant once (i) fails" — a reader's-notice rule). Second, a machine-side rule (the
post-state of K3.9 at scope end) changes no verdict at all: a test that compares verdicts
would never see it, only a trace-level oracle does — 0047 §5.3's level 2.

## How this would grow into the recommended tool

1. Add fields and the replacing `move` (K9), temporaries (K8), `throw`/`try` with edge parity
   (K3.15), `using var` (A5): 0047 §3.1's full L1 grammar, roughly doubling the file.
2. A `bzlprint` that emits each program as a real `.bzl` module (0047 §5.3's example shows the
   shape) and a harness that compares the model's three-valued verdict and disposal trace with
   `bezelc`'s — the differential oracle against the compiler, where 325 of the 475
   soundness-labelled issues live.
3. Read the K3 consume table from a machine-readable twin kept beside the spec, so the
   model's rule table and the spec's table cannot drift.
4. When stable, port to Lean 4 as 0047's S1 — the Python model is S0's executable twin and the
   bridge corpus is already built by then.
