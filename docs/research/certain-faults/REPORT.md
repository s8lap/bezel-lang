# Certain faults the language admits (or only warns about) — a hunt against spec v0.345

Research lane, 2026-10-27. Spec and stdlib read at `origin/main` (v0.345); every verdict below was **measured** with the r35 release compiler
`bezelc 0.0.0-f6863e62` in both disposal modes (`probes/run_all.sh`, results in
`probes/results.md`, one `.bzl` per probe under `probes/`). Issue search: the full tracker
title list (7,365 rows via the REST issues endpoint) plus the bodies of the
issues named below. Nothing in `docs/`, `tests/` or `src/` was touched by the lane. Its compiler bugs were filed afterwards as #7366–#7370.

The pattern hunted: a program whose failure is **certain on every execution that reaches the
site** (or that is certainly meaningless), which the language admits silently or only warns
about, where the compiler already has — or nearly has — the analysis that sees it. The owner's
reference finding (K5.3-b: alias use after an explicit `Dispose()`) is **excluded** here: its
proposal lane (proposal 0205, pending in the v0.346 train) covers both the alias half and the
same-variable half (its bzcore mutation table rows "0205 (b)"), so §28.2's dominating-kill
warning on the *same local* (probe `p09a`) is reported only as supporting evidence, not as a
candidate.

## Ranked table

Rank = certainty of the fault × how silent today (none > warning) × unchecked-build harm
(silent wrong read / UB > deterministic exception) × low migration cost × analysis already
exists. "Today" is the measured compiler; "spec" is v0.345.

| # | Candidate | Spec today | Compiler today (checked / unchecked) | Analysis exists? | Reach (tests/docs that flip) | Class |
|---|---|---|---|---|---|---|
| 1 | **Owned-field overwrite or explicit field `Dispose()` under a live, straight-line local borrow of the field's current value** (`var b := _w; _w := new …; b.Name`) | admitted, flag-backstopped (§28.7 *Overwrite while borrowed*, K5.6) | **no diagnostic**; DisposedAccessException / **silent wrong read** (prints empty) | yes: the checker already names the field as the borrow's source; §7.4's "no intervening call/await" window is the certainty predicate | 2 gtests + rule case `s28_7_0005` (pins "admitted") | design change; reopens K5.6's recorded posture (rationale §5.1, DECLINED rows citing K5.6) — the K5.6 sibling of 0205(b) |
| 2 | **Constant index outside an inline array's `[N]T` bounds** (`a[5]` on `[3]int`; also a constant index into a `const string`) | IndexOutOfRangeException at run time; §15.1.2 says the compiler *already folds* a constant index to elide the check | **no diagnostic**; exception in both modes | yes — the fold that proves "in range" proves "out of range" | 16 test sites, all deliberate fault demonstrations | clarification-grade design change; mirrors §5.2/§5.4/§6.5/§17.14's constant-operand posture; no decline |
| 3 | **Division / `mod` by a constant zero with a non-constant dividend** (`x / 0`, `x mod Z`, `d / 0m`) | DivideByZeroException at run time (§6.1/§6.4); §17.14 covers only fully-constant operands | **no diagnostic**; exception in both modes | yes — `ConstFold.cpp`'s `containsConstDivByZero` walk | 2–3 fault tests (`decimal_521`, `coalesce_test`); #6961 (open) already claims the all-constant case | clarification; verbatim copy of §6.5's shift-count rule ("compile error when the count is a constant") |
| 4 | **Second await of a `using var`-bound (borrowed) `OwningTask<T>` on a straight line** | InvalidOperationException at run time (§30.4, §30.5, C.6); the *owned-local* twin is a compile error | **no diagnostic** (p05a); exception both modes; owned twin rejected (p05b) | yes — the "already awaited (consumed)" tracking, plus §29.11's spent-enumeration-right flow (the exact sister rule, static where visible) | guide 04 example (`guide4_0007`), rule case `s30_4_0002`, 1 gtest (#1785) — all deliberate fault demos | design change (§30.5 chose runtime for the borrowed operand at v0.190, #1785, by removing a contradicting sentence — no decline record) |
| 5 | **Open-ended range literal directly in a `for`** (`for var i in 10.. do`) | **must warn**; InvalidOperationException at `GetEnumerator` (§16.5) | warning, then **"not yet implemented: codegen for this for statement"** (compiler bug, see B1) | trivially syntactic (the warning predicate) | spec §16.5 example, `spec_0083`, rule case `s16_5_0001` (expect warn) | design change: warning → error; no decline, no rationale entry |
| 6 | **Null-forgiving `x!` on a definitely-null local** (`var w: W? := null; w!.Name`; `if w = null then w!.Name`) | NullReferenceException at run time (§7.8) | **no diagnostic**; exception both modes | nearly: §7.4 tracks non-null facts per local; "definitely null" is the mirror fact with the same establishing and invalidating forms | rule case `s7_8_0002` (expects run-time "caught null"); a flow-sensitive count is needed (110 raw hits, nearly all with an intervening write) | design change (new flow fact); no decline |
| 7 | **Fresh store into a borrowing-family collection** (`ws.Add(new Widget(…))`) | **must warn** (§15.4); the slot dangles at the K8 boundary; "the program stays accepted" | warning; DisposedAccessException / **silent wrong read** | yes — the lint | 13 tests pin the warning; §4.5 of the code-gen reference and Guide 5 teach it as a warning | design change reopening rationale §5.28's ruled Q1 ("rejection declined because it cannot be scoped by signature") — the lint's own predicate *is* that scoping |
| 8 | **Owning-collection slot kill (indexer-set, `RemoveAt`, `Clear`) after a same-slot element borrow, straight line** (`var w := pool[0]; pool.RemoveAt(0); w.Name`) | admitted; §28.11 "valid until its slot is mutated … not statically tracked per slot"; lint B **retired** v0.338 (proposal 0181) | retired warning still emitted for `RemoveAt` (#6573); **silent** for the indexer-set; fault / **silent wrong read** | yes — the (retired, still compiled) lint; the constant-index leg is a recorded liftable | 2 rule cases (`s28_11_0008/9`), code-gen reference §7.5 teaches the silent fault | design change; reopens a v0.338 retirement (bar: proposal 0181's grounds) — the slot instance of #1 |
| 9 | `requires` clause statically `false` after constant substitution (`Half(-4)` under `requires(n > 0)`) | **already a compile error** (§22.16 prover floor: "folding to `false` is a compile error") | compiles; ContractViolationException at run time — **compiler bug B2**, and rule case `s22_16_0001` pins the run-time verdict against the spec | yes (§17.14 folding) | — | no rule change needed: file the divergence |

Not promoted (reasons in the closing section): mutation during iteration (§29.12), the
terminal-accessor warning (§29.11), `case` expression over a constant scrutinee, send after
`Close()`, the "empty iteration" and "reversed bounds" warnings, the second-`Dispose()` warning,
null tests on non-nullable operands.

---

## 1. Owned-field overwrite / field `Dispose()` under a live straight-line borrow (K5.6)

**Program** (`probes/p10_field_overwrite_under_borrow.bzl`; `p10b` uses `_w.Dispose()` instead
of the overwrite):

```bezel
public class Holder
  private owned _w: Widget := new Widget("first");
  public method Swap()
    var b := _w;                      // borrow of the field's CURRENT value
    _w := new Widget("second");       // drop-on-overwrite disposes exactly that value
    Console.WriteLine(b.Name);        // certain fault
  end Swap;
end Holder;
```

**Spec (v0.345).** §28.7 *Overwrite while borrowed*: "Reassigning an owned **field or
collection slot** is not statically frozen (K5.6; …); a borrow of the displaced value is
invalidated at the overwrite, caught as `DisposedAccessException` in checked disposal mode and
undefined unchecked — the same runtime-checked posture as explicit `Dispose()` (rationale
§5.1)." K5 clause 6: "fields and collection slots are not frozen — the clause bounds what the
freeze's *subject* is: slot aliases are not body-local, and a slot kill disposes — the K4
backstop is real there". Rule case `s28_7_0005` pins exactly this program as `accept`.

**Compiler today.** Accepts with **no diagnostic** in both modes. Checked: exit 70
`DisposedAccessException`. Unchecked: exit 0, prints an **empty line** — the silent wrong read
the owner's K5.3-b finding exhibited with `a := new Widget("second")`.

**Proposed rule.** Within one body, a store to an owned field path `P` (or an explicit
`P.Dispose()` / `_ := P`) that is dominated by a local borrow `var b := P` and dominates a later
use of `b`, with no operation between the borrow and the kill that can write `P` (§7.4's
invalidation forms: a call, accessor, user operator, function-value invocation or `await` —
the same window the nullability narrowing already uses), is a compile error naming the borrow,
the kill and the use. Conditional kills, aliases of `this`, and kills reached through calls
stay under the backstop.

**Class.** Design change; it contradicts a recorded decision (K5.6; rationale §5.1's "fields
are not frozen"; DECLINED rows 45/46/132 cite K5.6 with "Reopening bar: a body-local alias
analysis for fields (none is contemplated)"). The reopening argument is the owner's own K5.3-b
argument applied one clause over: no *alias analysis* is needed for the straight-line subset —
the checker already resolves `var b := _w` to the field as the borrow's source
(`Ownership.cpp`: "a field read is a borrow", `BorrowSource` population), the overwrite names
the same path, and the "no unseen writer in between" predicate already exists for §7.4. The
decline's ground ("slot aliases are not body-local") is true of the *general* case and does not
touch this subset, where the fault is certain on every run. Note this is the field twin of
proposal 0205(b): the bzcore model has no fields (K9), so 0205 does not cover it — do not merge,
but cite it.

**Reach.** `tests/codegen/checked_disposal_bounded_retention_4602_test.cpp:352` (a checked-mode
retention test built on this shape), `generic_ctor_string_field_retain_4965_test.cpp:384`
(`var old := _v; _v := x;` — at a string `T`, no disposal, so not caught by a class-typed rule),
and rule case `s28_7_0005` (would flip to `reject`). Nothing in the guides teaches the shape;
Guide 1 §142 states the opposite discipline ("what a field read … points at can still be
overwritten under the holder, and that is checked mode's business").

**Risks / over-rejection.** A method that reads `_w` into a local, calls a helper that *might*
reassign `_w`, then overwrites — the intervening call takes the shape out of the rule, so no
over-rejection; the cost is that such programs keep today's posture. The remedy for a rejected
program is one line: use `b` before the overwrite, or take the value out (`using var old :=
move _w with new Widget("second");`, §28.8.1).

**Existing issues.** None open on the straight-line field case. Related: #5706/#5707 (span
sourced from a field slot, proposal 0153 — declined options B/C/F cite K5.6), #4216 (closed:
lambda-body reassignment target bypassed the freeze), RELAXATIONS.md §5 (the K5.2 clarification
for `a.Dispose(); a := …`).

## 2. Constant index outside an inline array's bounds

**Programs** (`p04a`, `p04b`, `p04c`):

```bezel
var a: [3]int := [1, 2, 3];
Console.WriteLine(a[5]);          // p04a — literal index
const K: int := 3;
Console.WriteLine(a[K]);          // p04b — named constant
const S: string := "abc";
Console.WriteLine(S[7]);          // p04c — constant string receiver
```

**Spec.** §15.1.2 (*Members and operations*): "Indexer `arr[i]` for read and write. Bounds
checking applies; a constant index proven in-range at compile time elides the check." §31.4:
`IndexOutOfRangeException` "array or string index out of bounds". `N` is part of the type
(§15.1) and `.Length` is "compile-time constant for inline arrays" (§15.5). No sentence makes
the constant out-of-range case a compile error, unlike every other constant-operand fault in
the language: §5.2 (`byte(300)` — measured reject, `p21`), §5.4 (`Priority(99)` — measured
reject, `p19`), §6.5 (`x << 40` — measured reject, `p20`, keyed on the *count* alone), §17.14.

**Compiler today.** All three compile with **no diagnostic** and raise
`IndexOutOfRangeException` (exit 70) in both modes.

**Proposed rule.** An index that is a §17.14 constant expression outside `0..<N` on an
inline-array receiver — and outside `0..<Length` on a constant-string receiver — is a compile
error at the index, "the same posture as §5.1's constant-narrowing range error". Only §17.14
constants qualify (a `var i := 5` is not one); loop variables and computed indices are
untouched.

**Class.** Clarification-grade design change (one sentence in §15.1.2 and §4.6); no decline
exists; the compiler already folds the index (`foldLocalConstArraySize`, the in-range elision).

**Reach.** 16 test sites use a constant out-of-range index on purpose to provoke the exception
(`tests/codegen/exceptions_test.cpp` `a[10]`/`a[5]`, `inline_array_test.cpp` `a[3]`/`a[4]`/
`a[5]`, `uncaught_exit_test.cpp`, `integration_test.cpp`, …); each migrates by indexing through
a non-constant. 74 tests mention `IndexOutOfRangeException` in all; the rest use heap arrays or
computed indices. No doc example indexes an inline array out of range.

**Risks.** None found: an inline array's extent is in its type, so the fault cannot depend on
state. The constant-string leg is marginal (who indexes a `const string` by a literal?) and
can be dropped without weakening the array leg.

**Existing issues.** None. (#6236, #6259 are codegen bugs on inline-array indexing, unrelated.)

## 3. Division / `mod` by a constant zero, non-constant dividend

**Programs** (`p01a`, `p01b`, `p01c`): `x / 0`, `x mod Z` (`const Z := 0`), `d / 0m` with `x`,
`d` call results.

**Spec.** §6.1: "Division and `mod` raise `DivideByZeroException` when the right operand is
zero"; §6.4 likewise for `decimal`. §17.14 makes the fault a compile error only when the whole
expression is constant (`5 / 0`). Contrast §6.5, which keys on the *count* operand alone: "A
count outside the domain … is a fault: a **compile error** when the count is a compile-time
constant, and `OverflowException` at runtime otherwise" — measured (`p20`): `x << 40` is
rejected although `x` is a call result.

**Compiler today.** All three compile silently and fault at run time (exit 70). So does the
all-constant `5 / 0` (`p01d`) — that is #6961 (open, bug/sema).

**Proposed rule.** "A divisor or `mod` operand that is a compile-time constant (§17.14) equal
to zero is a compile error at the operator, whatever the dividend — the posture of §6.5's
shift count." Integer and `decimal` alike.

**Class.** Clarification; it extends #6961's fix by one predicate (constant *right operand*
instead of constant *expression*). The analysis is `src/sema/ConstFold.cpp`'s
`containsConstDivByZero` walk (#1211, #1688, #3617) applied to the right operand alone.

**Reach.** `tests/codegen/decimal_521_test.cpp` (`var b := 0.0m; a / b` — a `var`, not a
constant, so **unaffected**), `coalesce_test.cpp` (`var z := 1 / 0;` — already #6961's case),
`lifted_operators_486_test.cpp` (`10 / 0`, constant — #6961). Of 59 raw `/ 0` matches in
tests and docs, every real-code hit is a constant-expression case already owed to #6961 or a
`var` divisor; zero docs examples. Migration cost ≈ 0.

**Risks.** None; a constant zero divisor has no runtime-dependent outcome.

**Existing issues.** #6961 (open) — the all-constant case; this candidate is its natural
extension and should be filed as a follow-on, not a duplicate.

## 4. Second await of a borrowed `OwningTask<T>` binding

**Program** (`p05a`):

```bezel
using var t := Task.FromOwnedResult<Widget>(new Widget("a"));
using var a := await t;            // first award spends the task
Console.WriteLine(a.Name);
using var b := await t;            // certain InvalidOperationException
```

**Spec.** §30.4: "`OwningTask<T>` is single-consumer by type … the first await spends the task
regardless of outcome (success, fault, or cancellation), and a second await raises
`InvalidOperationException`". §30.5: when the operand is borrowed (a `using var` binding, a
plain parameter, a field read, a slot) "the await **spends** the task … a second await raises
`InvalidOperationException`"; C.6: "a second await a compile-time moved-from read on an owned
operand and `InvalidOperationException` on a borrowed one". The sister rule §29.11 for the
lazy query's enumeration right — "an affine discipline … it is the *right* that is spent, not
the binding's ownership … A second enumeration … is a compile error where the flow analysis can
see it statically; where a re-enumeration is reachable only at runtime, it raises
`InvalidOperationException`" — is exactly the layered reading §30.5 declines to apply to the
task's award.

**Compiler today.** `p05a` compiles with no diagnostic; checked and unchecked both print `a`
then `Unhandled exception: System.InvalidOperationException` (exit 70). The owned-local twin
`p05b` is rejected: "`OwningTask<T>` `t` was already awaited (consumed) — … single-consumer"
(`Ownership_Checks.cpp:21310`). The lazy-query twin `p12` is rejected per §29.11.

**Proposed rule.** The award of an `OwningTask<T>` through a *named binding* — owned or
borrowed — spends an affine **award right** tracked by provenance exactly as §29.11 tracks the
enumeration right: a second await of the same binding that the first dominates is a compile
error (the diagnostic names the first award); a re-award reachable only at run time keeps the
`InvalidOperationException`. The binding's ownership and disposal are unchanged (the `using var`
still disposes at scope end; a spent task "finds no result to dispose").

**Class.** Design change (§30.4/§30.5/C.6 text). It reopens no decline: #1785 (v0.190) chose
the runtime verdict for the `using var` case by deleting "one sentence claiming the using-var
case is a static consume" as a *contradiction* with the single-consumer paragraph, not on a
recorded reason; the static-where-visible / runtime-otherwise split is the language's own
rule one section earlier (§29.11), and the analysis (spent-state tracking on the owned twin;
provenance-keyed affine tracking on queries) exists on both sides.

**Reach.** Guide 04 teaches the shape *as* a run-time fault (`tests/conformance/cases/
guide4_0007.bzl`: "`using var tok2 := await t; // runtime InvalidOperationException: already
spent`"), rule case `s30_4_0002` (expects "caught"), and
`tests/codegen/owningtask_builtin_class_second_await_1785_test.cpp` (asserts the IOE). Three
sites, all deliberate demonstrations; the dozens of other double-await programs found by grep
are `Task<T>` (Pattern A, multi-award, unaffected) or separate programs reusing the name `t`.

**Risks / over-rejection.** A retry-after-fault shape (`try using var a := await t; catch …;
end; using var b := await t;`) — §30.4 says the first await spends the task *whatever its
outcome*, so the second is certain to fault; rejecting it is correct and the diagnostic should
say "bind a fresh task". Loops: an await inside a loop on a binding declared outside it is the
same back-edge question §28.2's dominating-Dispose warning already answers; a conservative
first cut claims straight-line dominance only. Harm is lower than #1–#2 (a deterministic
exception in both modes, no UB), which is why it ranks fourth despite total silence.

**Existing issues.** #1785 (closed, the split as spec'd), #5834 (closed, borrowed-operand
disposal), #1826. None proposes the static half.

## 5. Open-ended range literal in a `for`

**Program** (`p02`): `for var i in 10.. do Console.WriteLine(i); end;`

**Spec.** §16.3: "Calling `GetEnumerator` on an open-ended range raises
`InvalidOperationException`. The compiler warns when an open-ended range literal appears
directly in a `for` statement or other iteration context." §16.5 *Open-ended ranges*:
"Iterating an open-ended range raises `InvalidOperationException` at the call to
`GetEnumerator`, before any iteration occurs. The compiler emits a warning … `// WARNING:
open-ended range cannot be iterated`"; "Open-ended ranges are designed for slicing and pattern
matching, not iteration." The *variable* case ("For ranges stored in variables, the check is
deferred to runtime") is a genuine run-time question and is untouched.

**Compiler today.** Emits the §16.5 warning, then fails with "not yet implemented: codegen for
this for statement" (both modes) — the program neither runs nor faults; see **B1**.

**Proposed rule.** An open-ended range *literal* as the source of a `for`/`await for` (or as
the receiver of a direct `GetEnumerator()` call) is a compile error. There is no execution on
which the loop body runs; the warning describes a program that cannot mean anything.

**Class.** Design change (warning → error), but the smallest kind: no decline, no rationale
record, no issue ever argued for the warning; the predicate is already implemented as the
warning's. Reach: the spec's own §16.5 example comment, `tests/conformance/cases/spec_0083.bzl`
(extracted from it), rule case `s16_5_0001` (`expect: warn`), and the lit/gtest pins of the
warning text (`tests/sema/range_default_3108_test.cpp`,
`tests/lit/sema/sema_dict_iteration_530__range_iteration_unaffected_spec_16_5.bzl`) — all flip
from warn to reject. Harm is a deterministic exception (no UB), so it ranks below the silent
ones.

**Risks.** None: the literal form has no runtime-dependent reading.

**Existing issues.** None on the verdict; #658/#68 (closed) are parser history. The codegen NYI
is unfiled (B1).

## 6. Null-forgiving on a definitely-null operand

**Programs** (`p08a`, `p08b`):

```bezel
var w: Widget? := null;
Console.WriteLine(w!.Name);                     // p08a
var w := Find();                                // Find(): owned Widget?
if w = null then Console.WriteLine(w!.Name); end;   // p08b — inside the null branch
```

**Spec.** §7.8/§17.8: "`x!` asserts non-nullness. If `x` is null at runtime,
`NullReferenceException` is raised." §7.4 tracks, flow-sensitively per local, "whether its
value is known non-null", established by the forms it lists and invalidated by exactly
reassignment, `ref`/`out` passing and mutable capture across a call/await. The symmetric fact
— known *null* — is never tracked, so `x!` on a local whose every reaching definition is the
`null` literal (or inside the true branch of `x = null` / `x is null`) is admitted.

**Compiler today.** Both compile with no diagnostic (p08b only draws the unrelated must-use
warning) and raise `NullReferenceException` (exit 70) in both modes.

**Proposed rule.** Track "definitely null" as §7.4's mirror fact (established by a `null`
initializer or assignment, by the true branch of `x = null`/`x is null` and the false branch of
`x <> null`/`x is not null`, invalidated by §7.4's invalidation forms and at joins where any
edge is not definitely null); `x!` on a definitely-null carrier is a compile error, and
`x?.M`, `x ?? e` there may draw an unused-operand warning. Only §7.4's carriers (locals,
non-`ref` parameters, tracked `readonly` fields) participate.

**Class.** Design change (a new flow fact); no decline. The machinery is §7.4's with the
polarity flipped; the invalidation discipline ("deliberately syntactic and conservative") needs
no new escape analysis.

**Reach.** Rule case `s7_8_0002` (`var w: string? := null; var s := w!;` — expects run-time
"caught null") is the exact shape and would flip. The grep over tests found 110
`:= null … x!` pairs, but nearly all have an intervening `out` write, loop or branch assignment
(`var got: int? := null; … TryReceive(out got) … got!`), which the invalidation forms clear;
a flow-sensitive count was not run. Guides show no definitely-null `!`.

**Risks.** The `if w = null then … w! …` shape sometimes appears as a deliberate "unreachable"
assertion in hand-written code; the diagnostic should name `throw` as the spelling for that.
Harm is a deterministic exception (no UB) and the shape is rare in practice, hence rank 6
despite total certainty and silence.

**Existing issues.** None. (#916/#915 closed: null literal into a non-nullable.)

## 7. Fresh store into a borrowing-family collection (§15.4 lint)

**Program** (`p07`): `var ws := new Array<Widget>(); ws.Add(new Widget("a"));
Console.WriteLine(ws[0].Name);`

**Spec.** §15.4: the argument "is an unbound owned temporary … freed at its K8 boundary, so the
collection retains a reference to a freed instance, backstopped by checked disposal mode.
Because the retention is **statically certain** to outlive the value, the compiler **must
warn**". Rationale §5.28 (ruled Q1): "Rejection was declined because it cannot be scoped by
signature — `coll.Add(new X())` and `sink.Consume(new X())` are indistinguishable
plain-parameter calls … So the shape stays admitted … with a targeted lint"; the collection
*literal* form of the same hazard is already a hard error ("the recorded literal-only
asymmetry").

**Compiler today.** The warning fires (long text naming the K8 boundary and both remedies);
checked: `DisposedAccessException`; **unchecked: exit 0 and an empty line** — the silent wrong
read.

**Proposed rule.** A fresh unbound owned temporary of class type in a store position of a
**compiler-known** borrowing-family member (`Add`/`Insert`/indexer-set, `OwningDictionary`'s
key) — exactly the lint's predicate — is a compile error; user-declared `Add`s stay admitted
under the signature-level trust the decline named.

**Class.** Design change reopening rationale §5.28's ruled Q1 (owner-ruled 2026-08-04) and its
v0.243 extension (proposal 0081). Reopening argument (new evidence against the stated reason):
the stated reason was that rejection "cannot be scoped by signature"; the lint that was adopted
instead *is* scoped — to the compiler-known collection members — and has run at that scope
since v0.243 without a false positive on record, so the scoping objection no longer describes
the rule that would be promoted. The literal form is already rejected on the same ground ("a
language construct whose storage semantics the compiler fully knows"); a compiler-known stdlib
member's storage semantics are equally known.

**Reach.** 13 tests pin the warning (`fresh-store` in `tests/`), rule cases under §15.4/§28.11
with `expect: warn`, the code-gen reference §4.5 and Guide 5 teach the warn-not-reject
posture; #7141 (open) reports the warning missing on an `IList<T>`-typed receiver, #3393 (open)
that the spec's own archetype does not lower. Migration is the remedy the warning names (bind
first, or the owning family) — one line per site.

**Risks / why not higher.** The fault is certain *on any later read of the slot*; a collection
that is filled and never read does not fault (it is merely dead). The owner ruled this exact
question once with reasons; the ranking reflects that reopening cost. Harm (silent wrong read
unchecked) is as high as #1.

**Existing issues.** #3047 (open, proposal 0081's lints), #7141, #3393, #3873 (open:
`OwningDictionary` key rejection gap).

## 8. Owning-collection slot kill after a same-slot borrow, straight line

**Programs** (`p27a`, `p27b`): `var w := pool[0]; pool.RemoveAt(0); w.Name` and
`var w := pool[0]; pool[0] := new Widget("b"); w.Name`.

**Spec.** §28.11: "A borrow so produced is valid until its slot is mutated — an indexer-set,
`Remove`/`RemoveAt`, `Clear()` or the collection's own disposal disposes the referent — and is
not statically tracked per slot: a use of it afterwards is a mid-scope kill under the
checked-mode backstop". The element-borrow use-after-removal warning (lint B) was **retired at
v0.338** (proposal 0181): "changed no verdict … described the one shape of a dangle the
checked-mode backstop already faults deterministically … A lint over the same shape is
tooling's to offer"; the constant-index indexer-set leg was a recorded liftable that "lose[s]
its subject with the warning".

**Compiler today.** `p27a` still draws the retired warning (#6573, open) and faults;
`p27b` is **silent** and faults; unchecked, both print an empty line.

**Proposed rule.** The slot instance of candidate #1: a structural kill of slot `i` of a
collection binding `c` (`RemoveAt(i)`, indexer-set at `i`, `Clear()`, `c.Dispose()`),
dominated by `var w := c[i]` with `i` a constant or the same unwritten local, and dominating a
use of `w`, with no intervening call on `c`, is a compile error.

**Class.** Design change reopening a very recent retirement. The retirement's grounds were
"moved no verdict" and "the backstop already faults" — an error moves the verdict, and "the
backstop faults" is the very ground the owner now weighs the other way for K5.3-b. Still, this
is the most contentious entry: proposal 0181 is six revisions old and the owner's 2026-10-13
criterion (usefulness vs. ease of workaround) was applied to it explicitly. Recommend folding
it into #1's proposal as the collection leg rather than reopening 0181 alone.

**Reach.** Rule cases `s28_11_0008`/`0009` (lint B's, per #6573), code-gen reference §7.5
("compiles SILENTLY; FAULTS in checked mode"), Guide 5 §118.

**Existing issues.** #6573 (open), #5936 (closed, KEEP lint A), DECLINED row 698.

## 9. `requires` statically false at a constant call — already a compile error on paper

**Program** (`p03a`): `Half(-4)` under `requires(n > 0)`.

**Spec.** §22.16 *The current prover floor*: "a clause whose expression (after call-site
substitution, for `requires`) is a constant expression folding to `true` (folding to `false` is
a compile error at the clause — a statically violated contract …)". So `Half(-4)` is a compile
error by the spec as written — the language already took the "certain fault → compile error"
position here.

**Compiler today.** Compiles; `ContractViolationException` at run time in both modes.
`p37a`/`p37b`: `static requires(n > 0)` is rejected *at the declaration* ("undischargeable at
the proven tier when its expression names a parameter"), whereas §22.16 says a `static
requires` is discharged at every call by substitution and errors at an undischargeable *call*.

**Not a candidate rule — a divergence to file (B2, B3).** The rule corpus is on the compiler's
side against the spec: `s22_16_0001` calls `a.Withdraw(-5m)` under `requires(amount > 0m)` and
expects "caught" at run time; by the prover-floor sentence that call is a compile error. Either
the corpus case needs a non-constant argument or the spec's folding sentence needs a
spec-question. 38 tests mention `ContractViolationException`; `contract_clauses_requires_
runtime_2053_test.cpp` (`c.M(-1)`, `new C(-1)`) and `contract_clauses_override_inherit_
requires_3611_test.cpp` (`d.Half(-4)`) use constant violating arguments and would flip under
the spec's own rule.

---

## Compiler bugs hit along the way (verdict ≠ spec) — to file, not filed

| id | Probe | Spec says | Compiler does | Existing issue |
|---|---|---|---|---|
| B1 | `p02` open-ended range literal in `for` | accept with the §16.5 warning; `InvalidOperationException` at `GetEnumerator` | warning, then **"not yet implemented: codegen for this for statement"** — the build fails | none found (#3522 is the user-`IEnumerable` NYI) |
| B2 | `p03a` `Half(-4)` under `requires(n > 0)` | compile error (§22.16 prover floor: constant after substitution folding to `false`) | compiles, `ContractViolationException` at run time; rule case `s22_16_0001` pins the run-time verdict | none |
| B3 | `p37a`/`p37b` `static requires(n > 0)` | discharged per call site by substitution; a compile error at an *undischargeable call* (`Half(Seven())`), none at `Half(8)` | rejected at the declaration: "undischargeable … when its expression names a parameter" | none |
| B4 | `p28` `t.Dispose(); var v := await t;` on a `Task<int>` | §28.2: a use ("read, borrowed from, or consumed") after a dominating explicit `Dispose()` **must warn**; K4.1: a later use through any alias faults in checked mode | **no warning**; checked mode prints `5`; unchecked segfaults | none (#3979 is the `move`-after-Dispose cascade) |
| B5 | `p29` `e.Dispose(); e.MoveNext()` | §29.7: "a `MoveNext()` after disposal is an ordinary instance member access on a disposed instance … `DisposedAccessException` in checked mode" | warning OK; checked mode prints `True`; unchecked segfaults | none |
| B6 | `p11` `g.First()` over a minting iterator | §29.11 **must warn** (fault stated as certain) | silent; faults checked, empty output unchecked | #3047 (open) — known |
| B7 | `p27a` | lint B retired v0.338 | still emitted | #6573 (open) — known |
| B8 | `p01d` `var y := 5 / 0;` | §17.14 compile error | compiles, faults | #6961 (open) — known |

Also observed, consistent with the owner's finding: every unchecked-mode use-after-dispose
probe either segfaults (`p00`, `p09a`, `p28`, `p29`) or **prints an empty line and exits 0**
(`p07`, `p10`, `p11`, `p27a`, `p27b`) — the silent wrong read.

## Supporting measurement for the K5.3-b lane (not a candidate here)

§28.2 inverts certainty and severity: the *dominating* kill (fault certain) is a warning
(`p09a`: warning, then `DisposedAccessException` / segfault), while the *conditional* kill
(fault uncertain) is an error (`p38`: "may have already been disposed on another branch … may
not be read here"). Rationale §5.1's v0.184 note calls the conditional rejection "free
precision, not a borrow checker" because "the per-path consumption tracking exists regardless";
the same sentence applies to the dominating case. Proposal 0205(b)'s mutation table already
counts this half (the 0205 lane's model run: "same-variable
dominated use rejected … 3396 fault on every path, 0 on some path, 948 never fault" — the 948
being programs whose post-kill use is a second `Dispose()` or a non-member read).

## Looked like a hit, but is not (and why)

- **Mutation during iteration (§29.12, `p06`).** The warning fires correctly; the fault is a
  deterministic `InvalidOperationException` in *every* build mode (no UB), the predicate is
  deliberately "advance reachable from the call, not dominance" so the common conditional
  `if p(x) then xs.Remove(x); end;` is warned although it faults only on runs where the guard
  holds, and the owner ruled KEEP-as-warning at v0.339 (#5936, DECLINED row 536) three
  revisions ago. Promoting only the unconditional subset would split one lint in two for a rare
  shape with the lowest harm on the table. Not an improvement.
- **Terminal accessor over a minting iterator (§29.11 #3047, `p11`).** Not even implemented
  yet; the spec records an *over-warned* shape (adopt-then-transfer under an exhausting
  accessor) where the fault is *not* certain, and an unused accessor result faults on nothing.
  An error would need both carve-outs. Implement the warning first.
- **`case` expression over a constant scrutinee matched by no arm (`p24`).** The non-exhaustive
  warning already fires (§19.6); the constant-scrutinee subset is dead code in practice and the
  gain is taste-sized.
- **`Send` after `Close()` on a sender minted from that channel (`p15`).** Certain
  `ChannelClosedException`, but the compiler tracks no sender→channel provenance; this is new
  analysis, not nearly-existing analysis. Same verdict for `Pop`/`First` on a provably empty
  fresh collection (needs emptiness tracking) and `int.Parse("abc")` (needs stdlib semantics).
- **Constant empty-iteration step warning (§16.4, `p17`) and the reversed-bounds pattern
  warning (§16.6).** Not faults: the loop body never runs / the arm never matches; the
  programs are well-formed and the warnings are already the right severity.
- **Inverse candidates (programs certainly fine that the compiler flags).** (a) A second
  `Dispose()` on the same binding warns (`p09b`) although §28.2 defines it as a harmless
  no-op — but §28.2 states the warning deliberately ("the no-op carve-out states the runtime
  contract … and suppresses no diagnostic"), the cost is a warning on dead code, and nothing is
  rejected. (b) `w = null`, `w ?? o` and `w!` on a **non-nullable** `w` compile silently
  (`p25`, `p25b`): meaningless but harmless; §8.4 defines `nonNull = null` as `false`. A lint is
  tooling's; no reject-side cost exists to remove. No reject-side over-rejection with a measured
  cost was found in this sweep.
- **Same-variable use after a dominating `Dispose()` (`p09a`).** A genuine certain-fault
  warning — excluded only because proposal 0205(b) already claims it.

## Files

- `REPORT.md` — this report.
- `probes/*.bzl`, `probes/common_widget.txt`, `probes/run_all.sh`, `probes/results.md`
  (`probes/out/` holds the binaries and per-mode compile/run logs).
- `reach.py` — the reach grep; `warn_hits.txt`,
  `fault_hits.txt` — the spec sweeps for "warn" and "backstop" sentences.
