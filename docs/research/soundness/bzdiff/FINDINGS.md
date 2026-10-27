# bzdiff — findings of the model-vs-compiler differential (bzcore L1 vs `bezelc` r35)

**Everything below is measured on r35** (`bezelc 0.0.0-f6863e62`, the 2026-10-27 release build,
`--checked-disposal` and `--unchecked-disposal`, no ASan available); `main` may have fixed some of
it since. Model: `docs/research/soundness/bzcore-l1/bzcore.py` (spec pin v0.345), imported through the knobbed
verbatim copy `model/bzcore.py` (two knobs, see §3). Harness, printer and the re-run recipe:
[`README.md`](README.md). Draft issues: `drafts/*.json` (one per unmatched finding, repros in
`drafts/repros/`, measured output in `drafts/repros/measured.txt`).

## 1. Numbers

24,036 programs compared (the model's 42-shape selftest, then deterministic reservoir samples of
the enumerator per fragment), ~0.5 s per program (two compiles, every path run in both modes).

| sweep (features / bounds) | programs | level 1 agree (spec model) | both reject, family differs | disagree | level 1 agree (r35-gap model) | family differs | disagree | level 2 agree / compared |
|---|---|---|---|---|---|---|---|---|
| selftest (42 shapes) | 36 | 30 (83.3%) | 1 | 5 | 32 (88.9%) | 1 | 3 | 19/20 |
| locals+pins, v3 l4 (`using var` heads) | 3000 | 2466 (82.2%) | 250 | 284 | 2749 (91.6%) | 99 | 152 | 645/649 |
| locals+temps, v3 l4 | 3000 | 1572 (52.4%) | 647 | 781 | 2666 (88.9%) | 110 | 224 | 661/1220 |
| locals+pins+temps, v2 l4 `--if` | 3000 | 1858 (61.9%) | 791 | 351 | 2571 (85.7%) | 291 | 138 | 530/727 |
| locals+fields, v3 l4 (no replacing move) | 3000 | 2778 (92.6%) | 55 | 167 | 2778 (92.6%) | 55 | 167 | 888/895 |
| all but fields, v2 l3 `--try --while` | 3000 | 1848 (61.6%) | 609 | 543 | 2461 (82.0%) | 197 | 342 | 450/616 |
| all, v3 l4 (no replacing move) | 3000 | 1863 (62.1%) | 556 | 581 | 2608 (86.9%) | 163 | 229 | 669/907 |
| all, v3 l4 `--if` (no replacing move) | 6000 | 3911 (65.2%) | 1321 | 768 | 5009 (83.5%) | 589 | 402 | 825/1156 |
| **total** | **24036** | **16326 (67.9%)** | 4230 | 3480 | **20874 (86.8%)** | 1505 | 1657 | **4687/6190 (75.7%)** |

- *Level 1* compares the three-valued verdict at rule-family granularity (0047 §5.3 Q3). "Both
  reject, family differs" means a program that breaks two rules and is reported under the other
  one (e.g. the model's state-match vs the compiler's freeze on `var b := a; a := b;`); it is a
  verdict agreement and not a finding. The must-use lint and the preamble's §22.12 warnings are
  filtered. "r35-gap model" = the same model with r35's two known, open gaps switched off (#5582's
  unmarked `using var`, and F1's K8.4 admission): the residual disagreements are the findings.
- *Level 2* (both accept): every model path run on the checked build, N/D/U trace compared in
  order, outcome compared. 1,503 of the 6,190 compared programs disagree; **all but 4 of them are
  F1 (1,180 level-2 rows), F11 (91), F13's exception-edge sibling or F17 (35)** — the rest trace
  identically, including every K3.15 unwind, every catch-arm disposal order, every K9.5 RHS-first
  overwrite, every §28.2 fault. 495 compared programs end in an uncaught `throw`; on every one the
  model's termination disposal of the exception is absent on the compiler side (F9). 0 RACE outcomes.
- *Unchecked build*: 98 of the both-accepted programs crash (SIGSEGV) in the unchecked build —
  every one is a §28.2-warned use-after-dispose, undefined there by design; recorded, not a finding.
- Disagreement rows attributed to each finding, over all 24,036 (`findings_count.py`):
  F1 1951 · F3 412 · F4 170 · F15 132 · F11 91 · F13 89 · F10 88 · F6 79 · F7 77 · F17 32 ·
  F14 17 · F8 8 · F16 7 · F5 3 · F18 1 · unattributed 4 (siblings of F13/F17, see their entries).

## 2. Findings (deduplicated by root cause)

Class key: **compiler bug** (model and spec agree) · **spec gap** · **model bug** · **printer artefact**.
Each entry: minimal repro (model syntax; the `.bzl` is in `drafts/repros/` where a draft exists),
model verdict/trace vs r35, the spec sentence, tracker match or draft, campaign label.

### F1 — a field read of an unbound temporary escapes (K8.4) and the temporary is never disposed (K8.1) — compiler bug, soundness + leak — filed as [#7402](https://github.com/s8lap/bezel-lang/issues/7402)

`tbrw b / use b` = `var y := new R(new Q()).f; y.Use();`. Model: `reject:K8.4-temporary-escape`
(under the r35-gap model: accept, path FAULT with trace `N1 N2 D2 D1`). r35: accepts, zero
diagnostics, runs `N1 N2 U1 END` in both modes — no `D:` at all. Spec K8.4 (§28.3.1, v0.345):
*"a borrow or frame that would escape it is rejected, never lifetime-extended … among them a
binding, store, capture, `return` … of the borrow"*; K8.1 / §28.3.11: *"disposed at the end of the
full expression … as a member or index access"*. The largest single source of disagreement
(1,951 rows: every program with the statement; 1,180 level-2 rows are its leak). Related #6775
(receiver/argument temporaries not disposed, campaign:producer-temporaries); the escape half is
unmatched. **Draft `drafts/f1_temp_field_escape.json`**, `campaign:producer-temporaries`.

### F2 — unmarked `using var y := x` over an owned binding is a borrow-pin, not an error — known gap

`new a / usingb y a`. Model: `reject:§28.10-unmarked-owned-into-using-var`; r35: accepts as a
borrow-pin (trace `N1 N2 U2 D2 D1` for `… / use y`). §28.10 *Adopting a named owned binding*:
*"An unmarked `using var y := x;` is a compile error when its right-hand side is a named owned
binding"*. **Matched: #5582** (open spec-tracking, v0.323). Modelled away by the r35-gap knob;
189 rows under the spec model. `campaign:using-heads`.

### F3 — a borrow assigned to an owned-state local is accepted (state match not enforced) — compiler bug, soundness

`new a / new b / asgb b a` = `var a := new R(..); var b := new R(..); b := a;`. Model:
`reject:state-match`. r35: accepts; checked run `N1 N2 N3 N4 D4 D3 D2 D1` (the second disposal of
`a`'s object is the idempotent no-op), **unchecked: SIGSEGV** (double free). §28.3.3 *State match
on reassignment*: *"Assigning a borrow to an owned-state LHS … is rejected — the mismatch would
silently drop or double-hold a disposal obligation"*. Also `new a / new b / disp a / asgb b a`
(accepted with the §28.2 warning only). **Matched: #4846** (same repro shape, same reasoning);
#7087 is the sibling for a runtime-managed RHS. 412 rows. `campaign:borrow-provenance`.

### F4 — `var a := move c` over a borrow `c` is accepted — compiler bug, soundness (double free) — filed as [#7403](https://github.com/s8lap/bezel-lang/issues/7403)

`new b / brw c b / move a c`; with a field borrow `new a / fbrw c a / move b c` (checked trace
`D1 D2`: the Q disposed through the alias before the holder's cascade). Model:
`reject:K3-consume-of-non-owned`. r35: accepts, zero diagnostics; unchecked SIGSEGV. §28.3.7:
*"Three spelling errors follow …: the marker on a borrow or fresh-producer right-hand side"*.
Unmatched (searched: move/borrow/alias, double-free). **Draft `drafts/f4_move_of_borrow.json`**,
`campaign:borrow-provenance`. 170 rows.

### F5 — the replacing `move x.f with e` does not parse on r35 — known gap (r35 snapshot)

`new a / fmove q a / use q` → `with Name := value, … end is the retired pre-v0.208 record-update
spelling … expected identifier in with init-assignment`. The §9.5 `with` suffix is consumed before
the §28.8.1 `move … with` clause (`src/parser/Parser_Exprs.cpp`, the `with` PrimarySuffix arm; the
corpus case `s28_8_1_0019` fails the same way on r35). **Matched: #5923** (spec-tracking, parser
delta 1). The `fields` fragment was therefore swept with `--no-fmove` (K9.1/K9.5/§28.8.1's
exchange, the slot-displaced alias and the model README's F1 shape are untestable on r35).

### F6 — after `x.Dispose(); Take(move x);` the moved-from state is lost: a later read compiles silently and SIGSEGVs in checked mode — compiler bug, soundness — filed as [#7404](https://github.com/s8lap/bezel-lang/issues/7404)

`new a / disp a / send a / use a` (also `… / brw c a`, `… / send a`, `… / fst a`). Model: the
§28.2 warning at the consume (v0.345: *"a consume exactly as a read does"*), then
`reject:K1.3-read-of-moved`. r35: the warning, then accepts the read; `--checked-disposal`
**SIGSEGV** (a null slot, no `DisposedAccessException` possible). Sibling, different rule:
`using b / disp b / brw a b / disp a` — the alias of a disposed binding may call `Dispose()`
(§28.6 error) with only the warning. K1.3: *"moved-from — … the binding is unreadable until
reassigned"*. Related mechanism #3979 / #4122 (checkRead's poisoning); their demand that the
`move` itself be rejected is superseded by v0.345's §28.2 letter, so the remaining defect is
drafted: **`drafts/f6_dispose_then_move_then_read.json`**, `campaign:borrow-provenance`. 79 rows.

### F7 — a `using var` can be moved out of after `Dispose()` (A5 bypassed) — compiler bug — filed as [#7405](https://github.com/s8lap/bezel-lang/issues/7405)

`using a / disp a / send a` (also `move b a`, `usingm b a`). Model: `reject:A5-move-out-of-using-var`;
r35: accepts with the §28.2 warning (without the `Dispose()` the move is correctly rejected).
§28.3.7: *"A `using var` … cannot be moved out of, only borrowed from (axiom A5)"*. Memory-safe on
r35 (the object is already disposed), a declared-intent violation. Unmatched. **Draft
`drafts/f7_using_var_moved_after_dispose.json`**, `campaign:using-heads`. 77 rows.

### F8 — a `using var` borrow-pin does not freeze its source — compiler bug, soundness (stale read / UAF) — filed as [#7406](https://github.com/s8lap/bezel-lang/issues/7406)

`new a / brw b a / usingb p b / send a / use p`. Model: `reject:K5.1-frozen-transfer`. r35: accepts;
with a disposing sink the read through `p` raises `DisposedAccessException` (checked) / reads freed
memory `U:0` (unchecked); with the harness's holder sink it is the model's RACE shape. The
un-pinned `var b := a; Take(move a); b.Use();` is correctly rejected. §28.10 *initialized from a
borrow*: *"the borrow stays live until the using-var's scope ends … the frozen kills (a move, a
transfer, a reassignment of the binding) are compile errors"*. Unmatched (no tracker item mentions
the pin; #4882 is the pin's *escape* half). **Draft `drafts/f8_borrow_pin_not_frozen.json`**,
`campaign:using-heads`. 8 rows (the shape needs four statements).

### F9 — an uncaught exception is never disposed — compiler bug (low) / spec wording — filed as [#7407](https://github.com/s8lap/bezel-lang/issues/7407)

Every model path ending in an uncaught `throw` disposes the exception at termination (§31.1:
*"or after the exception terminates the program if uncaught"*; K3.8's disposal-moment column);
r35 prints the unhandled-exception report, exit 70, and the thrown object's destructor (and its
owned-field cascade) never runs — 495 compared programs, excluded from the trace comparison and
counted. §33.3's shutdown sequence and §31.1 may simply disagree on whether this object is
reclaimed by exit; the draft asks for one sentence either way. **Draft
`drafts/f9_uncaught_exception_not_disposed.json`**, `campaign:records-statics-misc`.

### F10 — a field store through a moved-from local compiles and SIGSEGVs (checked mode too) — compiler bug, soundness — filed as [#7408](https://github.com/s8lap/bezel-lang/issues/7408)

`new a / new b / send b / fst b` = `Take(move b); b.f := new Q();` (also the K3.1 form `b.f := q;`).
Model: `reject:K1.3-read-of-moved`. r35: zero diagnostics, SIGSEGV in both modes (a plain read
`b.Use()` at the same point is rejected). K1.3 + K1.5 (the holder of a field read/store must be
readable). Unmatched. **Draft `drafts/f10_field_store_on_moved.json`**,
`campaign:borrow-provenance`. 88 rows.

### F11 — drop-on-overwrite is skipped for a local that adopted the owned state — compiler bug, leak — filed as [#7409](https://github.com/s8lap/bezel-lang/issues/7409)

`decl b / asg b / asg b` = `var b: R; b := new R(..); b := new R(..);`. Model trace
`N1 N2 N3 N4 D2 D1 D4 D3`; r35 `N1 N2 N3 N4 D4 D3` — the first value leaks. With an initialized
declaration the overwrite disposes correctly. §28.3.3 *Adoption* + rule 1 (*"disposes the
displaced value per drop-on-overwrite"*), K3.11. Unmatched (#5457/#5325 are other overwrite leaks).
**Draft `drafts/f11_adopted_local_overwrite_leak.json`**, `campaign:producer-temporaries`. 91 rows.

### F13 — a local consumed on one branch is rejected at scope end instead of disposed per path — compiler bug (over-rejection) + exception-edge leak — filed as [#7410](https://github.com/s8lap/bezel-lang/issues/7410)

`new b / if { } else { send b }` (any `move`/owned-argument consume on one arm, scope end after).
Model: accept, per-path trace. r35: `owned value in b would be dropped here without disposal
(§28.3 …)` — an error, while the `disp b` control compiles and disposes per path. K7.4: *"owned ⊓
moved-from … = maybe-consumed — … scope-end disposal per-path"*. Sibling on the exception edge
(no normal scope end, so no rejection): `new b / try { use b } catch e { throw b } / tthrow` and
`new a / new c / if { } else { throw a } / tthrow` — the maybe-consumed local is silently **not**
disposed by the final unwind (the model's T1 trace shows `D2 D1`, r35 does not). Related #3967 /
#5810 (the same missing per-path drop flag, at *reassignment*). **Draft
`drafts/f13_conditional_consume_scope_end.json`**, `campaign:borrow-provenance`. 89 rows + 3
exception-edge siblings.

### F14 — no §28.2 warning at a field store through a dominated-disposed binding — compiler bug (diagnostic) — filed as [#7411](https://github.com/s8lap/bezel-lang/issues/7411)

`new c / disp c / fst c`: model warns `§28.2-use-after-dispose`; r35 silent (the runtime backstop
fires: `DisposedAccessException`). K1 *explicitly-disposed*: *"a straight-line read falls under
§28.2's use-after-dispose warning regime"*. **Draft
`drafts/f14_missing_warning_field_store_after_dispose.json`**, `campaign:borrow-provenance`. 17 rows.

### F15 — may a `using var` be thrown? — spec gap (A5 vs K3.8) — filed as [#7412](https://github.com/s8lap/bezel-lang/issues/7412)

`using c / throw c`. Model: `reject:A5-move-out-of-using-var` (A5: *"never movable out"*;
§28.3.7: *"cannot be moved out of, only borrowed from"*). r35: accepts; caught form disposes the
exception exactly once after the arm (`D2 D1`, the pin's scope end disposes nothing further) —
memory-safe. K3.8 lists `throw` as an owned-consuming position of an owned binding and nothing
says whether the pin bars it (it explicitly admits `Dispose()`, §28.6). **Draft spec-question
`drafts/f15_spec_question_throw_of_using_var.json`**, `campaign:using-heads`. 132 rows.

### F16 — reassigning a maybe-consumed local is rejected — known narrow rejection

`new a / brw b a / if { asgb b a } else { send a } / asg a` → `cannot reassign a: it may have
already been consumed on another branch (maybe-consumed join, §28.3 K1.4)`. Model accepts
(§28.3.3 rule 4, v0.334+). **Matched: #5810** (spec-tracking, "lift #3967's narrow rejection").
7 rows.

### F17 — the exception edge skips a local that a LATER `var b := move a;` consumes — compiler bug, leak — filed as [#7413](https://github.com/s8lap/bezel-lang/issues/7413)

`new a / new c / if { tthrow } else { } / move b a`: on the throwing path r35 disposes `c` but
not `a` (`N1..N6 D4 D3`, model `… D4 D3 D2 D1`); with `use a` or `Take(move a)` as the later
statement the unwind disposes `a` correctly, so the later K3.4 binding transfer is what disables
the earlier cleanup. K3.15 edge parity. Unmatched. **Draft
`drafts/f17_unwind_skips_later_consumed_local.json`**, `campaign:producer-temporaries`. 32 rows
(+1 sibling with `asgm`).

### F18 — the freeze counts a borrow's use on the other arm as live — compiler bug (over-rejection, low) — filed as [#7414](https://github.com/s8lap/bezel-lang/issues/7414)

`new a / brw b a / if { asg a } else { use b } / tdisp` → rejected by the consume-site freeze
although `b` is dead on the consuming arm (K5.5: backward liveness). **Draft
`drafts/f18_freeze_liveness_across_arms.json`**, `campaign:borrow-provenance`. 1 row.

## 3. Model bugs and modelling decisions (fixed in `model/bzcore.py` or recorded)

- **M1 (fixed in the copy, knob `try_every_point_raises`).** The original model takes only `throw`
  statements as raise sites and leaves a handler without one *unchecked* ("dead code"). The
  compiler — correctly, since every call, constructor and disposer may raise — checks every
  handler with the K7 meet over every point of the body: `new a / try { send a } catch e { use a }`
  is rejected (maybe-consumed), `decl b / try { tuse } catch e { throw b }` is rejected (K1.3 /
  definite assignment), `catch e { disp e }` and `catch e { throw e }` are §28.6 / §31.1 errors.
  With the knob on, the model agrees on all of these (the machine still raises only at `throw`).
  Before the fix these were 200+ spurious "model accepts, compiler rejects" rows in the try sweep.
- **M2 (recorded).** Dead code after an `if` whose both arms throw is not checked by the model
  (K7.5 bottom absorbs); the compiler checks it (`new c / decl b / if { throw c } else { tthrow }
  / asgm c b` → definite-assignment error). One row; a legitimate compiler choice.
- **Knob `S28_10_reject_unmarked_pin`** (the r35-gap configuration): F2's rule off makes an
  unmarked `using var y := x` over an owned `x` a borrow-pin, which is what r35 implements.
- `RULES['K8_4_reject_escape']=False` (the model's own knob) is the second r35-gap setting; with it
  the model's machine disposes the temporary at its boundary, so the comparison exposes F1's leak.
- The model's `send` makes the object foreign (RACE on a later read); the printer's sink keeps it
  alive in-domain, so a RACE is reported from the `S` marker alignment (0 occurred among the
  both-accepted programs — every RACE shape in the fragment is a K5.1 rejection on both sides,
  F8 excepted, which the compiler accepts and the model rejects).

## 4. Printer limits and artefacts

- `send` is `R.SinkR(move x)` / `R.SinkQ(move x)` into a `static owned readonly OwningArray`
  holder (printed `S` first), not `Domain.Spawn`: `Spawn`'s owned row needs a sendable (sealed,
  transfer-closed) class and an `Exception`-derived class is not sendable on r35, which would make
  `throw` and `send` unprintable on the same class. Consequence: the sent object stays alive; a
  stale read is a RACE by marker alignment, never a runtime fault. (Observed on the way: objects
  held by the static holder are not disposed by the §22.13 exit pass either — outside the fragment,
  not pursued.)
- `R` derives from `Exception` (so `throw x` type-checks); ids come from a static counter
  (`Ids.Next()`), whose §22.12 declaration-site warnings are filtered by line number.
- Paths are driven by a command-line digit string (`Ids.Choose()`), at most 9 choice points per
  program (`int.Parse`); the model's bounds never exceed 3.
- One artefact found and fixed during the run: the first sink printed the id through a member
  call, which faulted when a disposed object was sent (`new c / new a / disp c / send c`), a
  spurious level-2 FAULT.
- The replacing `move` (`fmove`) is printed but unparseable on r35 (F5); the `fields` sweeps used
  `--no-fmove`.
- Rule families are matched by regex over r35's diagnostic text (`families.py`); every message
  seen in the 24,036 programs is mapped (no `UNMAPPED` family survives).

## 5. What the differential cannot see

- **No ASan** (`--link=system --sanitize=address` fails here): the unchecked-mode evidence is
  SIGSEGV-or-not, so a silent heap-use-after-free or double free with no crash is invisible
  (F3/F4 happened to crash; F10's SIGSEGV is a null deref). Leaks are seen only through the trace
  (a missing `D:`), which is exact in this fragment.
- **r35 snapshot** (2026-10-27 release build): every finding is marked "measured on r35"; main may
  have fixed some. #5923 (F5) in particular blocks the whole K9 replacing-move surface.
- The fragment is Layer 1: no calls other than the sink and `Use()`, no collections, closures,
  async, generics, nullability; `while` bounded to two iterations; bounds vars ≤ 3, ≤ 4
  statements (+ one construct). Samples of 3,000–6,000 per fragment, not the full enumeration
  (the full `all --if` space at v3 l4 is 1.9 M programs ≈ 11 days at this rate).
- The termination disposal of an uncaught exception (F9) and the exit pass's static disposal are
  not observable in the trace comparison; both are excluded and counted.
- Warnings are compared as sets of families; the compiler's *position* and the model's are not.
