# Proposal 0152 — A captured binding is counted by every assignment to it

| | |
|---|---|
| **Status** | graduated-v0.332 — superseded — graduated v0.332 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option B, a captured binding's source set is the union over its initializer and every assignment to it, in the declaring body and in every lambda body that captures it; critique round 1 applied, 4 blocking and 6 non-blocking findings resolved, shared with the #5687, #5689 and #5690 rulings; that round rewrote clause 1's worked example to the null-source route, counted a store into a captured record's settable member as a slot store is counted, stated the least fixpoint for a cycle through two captured function-typed locals, and corrected the #5689 clarification's "differ only" claim; landed with the v0.332 changelog entry) |
| **Tracking issue** | [#5688](https://github.com/s8lap/bezel-lang/issues/5688) (`spec-question`; the v0.331 borrow-flow matrix re-run's one root cause for twelve cells, R2–R7 × C6/C7, `scratch/matrix/r2/M3.md`); one `spec-tracking` compiler tracker is filed at landing (§6) |
| **Target module(s)** | **spec** — §28.3 (*Borrow lifetime rules*, the closure bullet's provenance clause 1: one sentence group), §27.5 (one sentence), §9.7 (the capture bullet: one clause), Appendix C.3 (one row); **rationale** — §4 (one paragraph after *Definite assignment across a closure*); **code-generation reference** — §4.4 (one table-row clause), §7.2 (one *Looks right, is rejected* pair); **`DECLINED.md`** — four `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.331** (HEAD `2679585`). Section numbers are v0.331's. |
| **Origin** | The v0.331 critique of proposal 0148 (lane `T1`, finding N6) asked what a closure's set is when the captured binding is reassigned after the closure forms. The borrow-flow matrix's v0.331 re-run (`scratch/matrix/r2/M3.md`) found that question to be the root cause of every open cell in its two columns, wrote an escaping program for each, and added a variant the issue does not state: an assignment made *inside* a lambda body, which the enclosing body's flow analysis never sees. The owner directed on 2026-10-08: "Please resolve and decide all known spec related issues. Do proposals for issues that need them." This one needs a proposal because the three candidate readings differ in what they admit, and the chosen one adds a counting rule — the union over a binding's assignments across bodies — that no current sentence states. |

*Reference convention: unprefixed §-references are language-spec references at v0.331; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

**Decision (option B).** A **captured binding** — a local or parameter that some lambda in the body captures — has **one source set wherever its source is read**: the union, over its initializer and **every assignment to it** in the declaring body and in every lambda body that captures it (a store that lands in the binding's own storage included — a slot of a captured inline array, §15.1.2, or a settable member of a captured record, §9.4), of what provenance clause 1 counts for a capture holding that assigned value — whatever the assignment's position relative to the capture or to the read. The set is read off the binding by clause 1 (a closure's set), by clause 6 (a captured function-typed local), by §9.7's bullets and by §28.14's storage-bounded admission alike, and by a read of the binding in the declaring body or in a lambda body. A binding no lambda captures contributes its reaching assignments, as the flow analysis traces them today. Where the sets depend on one another through captures (§27.5's recursive closure; two captured function-typed locals each assigned a lambda that calls the other), they are the least sets that satisfy the count.

The ground is §27.5's own sentence — "Modifications to a captured variable inside the lambda are visible outside, and vice versa" — read against K2.2 (a borrow's set under composition is the union) and K5.5 (the analysis is body-local and follows no invocation, §27.5's definite-assignment paragraph): captures share one heap cell, a closure reads that cell at invocations the enclosing body cannot see, and a lambda body writes it at points the enclosing body cannot see, so no assignment to a captured binding can be ordered against a closure's reads of it. The only set that is correct at every read is the union.

Measured reach: **zero** programs in `tests/` (3,207 `.cpp` and 5,502 `.bzl` files, the doc-example corpus and the 2,384 rule-corpus cases included), the documents and the proposals folder assign a captured binding whose value can carry a borrow from a source the formation-time set did not already carry. Every reassigned captured binding found is an `int`, `bool` or `string` counter, a capture-free function-typed local, or an owned binding already governed by the captured-owned rule and K5's drop-on-overwrite freeze. The compiler reads the set at the literal and treats a reassigned lambda-typed local as contributing *nothing* at a frame or `scoped` site (§6), so the rule is spec-ahead-of-impl.

## 1. The problem

Provenance clause 1 (§28.3, the closure bullet), verbatim: "A **lambda literal** contributes the sources of the borrows it captures — a captured borrowed local or parameter that binding's source; …". Read naturally, "that binding's source" is the source the binding has where the lambda is formed. The local-scope bullet admits a later assignment of a shorter-lived source into the binding whenever the source is declared in the same block: "A borrow may not be assigned to a local whose scope strictly encloses the borrow's source's scope". And §27.5 makes the closure read the new value: "Modifications to a captured variable inside the lambda are visible outside, and vice versa."

Three routes follow, each written by the matrix re-run (`scratch/matrix/r2/M3.md`, cells R2–R7 at C6 and C7):

```bezel
class Item
  public method N(): int => 1;
end Item;
class Holder
  private _f: (() -> int)? := null;
  public method Arm(): ()
    var b: Item? := null;            // no source
    var f := lambda() => b!.N();     // clause 1: set {} at formation
    _f := f;                         // capture-free: admitted at the field
    using var it := new Item();
    b := it;                         // same block: admitted; shared cell (§27.5)
  end Arm;                           // `it` disposed
  public method Fire(): int => _f!();  // N() on a disposed Item
end Holder;
```

The same shape with `var p := new Pair(null);` and `p := new Pair(it);` (R3), `var a: [1]Item? := [null];` and `a[0] := it;` (R4), `var r := Ref.None;` and `r := Ref.Some(it);` (R5): each starts null-source, so the closure is stored capture-free, and a later same-block assignment hands the stored closure a borrow of a `using var`. A span starts from a longer-lived source instead (spans have no null-source form, §9.7):

```bezel
class Maker
  public method Window(a: Array<int>): IEnumerable<int>
    var s := a.AsSpan();                 // carries {a}
    var f := lambda(x: int) => s[0] + x; // clause 1: set {a}
    var zs: Array<int> := [9];           // same block as `s`
    s := zs.AsSpan();                    // admitted; the cell now views zs
    return a.Select(f);                  // the frame carries {a}: parameter-sourced, admitted
  end Window;                            // zs freed; enumeration reads a freed window
end Maker;
```

And a captured function-typed local (R7): `var g := lambda() => 0; var f := lambda() => g(); _f := f; … g := lambda() => s[0];` — clause 6's "every assignment that reaches the read" is read at `f`'s formation, where only the capture-free assignment reaches, so `f` is stored capture-free and later calls a capture-bearing `g`.

**The variant the issue does not state.** An assignment to the captured binding made *inside a lambda body* is invisible to the enclosing body's flow analysis, so even the binding's own direct return reads a stale source:

```bezel
class Maker
  private owned _xs: Array<int> := [1, 2];
  public method Make(): ArraySpan<int>
    var s := _xs.AsSpan();            // receiver-sourced: {this}
    var zs: Array<int> := [9];
    var g := lambda() => do s := zs.AsSpan(); end;   // the lambda writes the cell
    g();
    return s;                         // the body's flow analysis still sources `s` at `this`: admitted
  end Make;                           // zs freed: the caller reads a freed window
end Maker;
```

Reading the set at the read site does not close this: the read site's flow analysis has no edge from the lambda body's assignment. Any resolution must count writes made inside a closure.

Severity: the routes through object references fault deterministically in checked mode (K4.1), but no static rule and no recorded K4.3 clause states the bound, which K4.3's third clause forbids. The span routes (R6, and R7 over a span) have no flag under v0.331's text (§9.7); lane `U2`'s proposal 0153 (#5707) makes a span read flag-checked, which changes their severity and nothing in this rule — a scope-bound source admitted into a stored closure is a static defect whatever the backstop.

## 2. Options

- **A — status quo.** Clause 1 reads the binding's source at formation; clause 6 reads a function-typed local's reaching assignments at the read. Unsound for every program in §1.
- **B — the union over every assignment to the captured binding, across bodies (recommended; the issue's reading 1, extended to in-lambda writes).** A captured binding has one source set, the union over its initializer and every assignment to it in the declaring body and in every lambda body that captures it, read wherever its source is read.
- **C — a freeze on the captured binding.** A captured binding whose value can carry a borrow may not be reassigned while a closure capturing it is live (the issue's reading 2). A stored closure's invocations are invisible to liveness (§28.14's ground for its freeze), so "live" must mean "formed": no reassignment anywhere after formation, and — the closure being live while its body runs — no assignment inside any lambda body that captures it.
- **D — the union over the declaring body's assignments, and a rejection of every assignment inside a lambda body to a captured binding whose type carries borrows** (the issue's reading 1 plus the task brief's reading 3).
- **E — a flow-sensitive union.** A closure's set is the union of the assignments that reach its formation, every assignment after it in the declaring body, and every assignment in any lambda body that captures the binding; the binding's own reads take their reaching assignments plus the in-lambda ones.

## 3. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A | unsound: twelve cells, §1 | — | contradicts §27.5's shared cell | — |
| **B** | sound: every value the cell can hold is counted at every read | one counting sentence; one set per cell, whoever reads it; a per-body pre-scan the compiler already has for reassigned locals (§6) | K2.2's union; the same reading §27.5's definite-assignment rule gives invocations ("the analysis does not follow invocations"); §28.14's tested sources inherit it with no new text | zero sites (§5) |
| C | sound | one rule, but two parts (after formation; inside a body) | a new freeze keyed on formation rather than on liveness, unlike K5's; bars the in-body cache idiom (`if cache = null then cache := _store.Find(); end`) and the long-lived re-point (`b := p`) | zero sites now; rejects shapes B admits |
| D | sound | two rules where B has one | the in-body rejection is C's second part; the declaring-body half is B | zero sites now; rejects the in-body cache idiom |
| E | sound | three sets for one cell (the closure's, a second closure's, the binding's own), each position-dependent | a captured binding's own reads would still need the in-lambda union, so the formation cut buys precision only for assignments *before* formation that a later one overwrites and for the binding's own reads before a later declaring-body assignment; an invocation before a later assignment is over-rejected by E as by B | zero sites separate it from B |

Simplicity and consistency prefer B: it states one count, the one K2.2 already prescribes for a value that can hold several borrows, and it needs no new mechanism — the §27.5 shared cell is the mechanism, and the rule only stops pretending the cell has a single value. C and D reject programs that are sound under B for no measured gain. E's extra precision is unmeasurable in the corpus and costs the "one set per cell" property that makes B checkable by a pre-scan.

**Why the union is enough.** A closure's set must bound every value the closure can read. Under B every assignment to the cell is in the set, and every reader of the cell — the closure, another closure, the declaring body — reads the same set, so a borrow of a scope-bound source assigned anywhere makes the closure unstorable and unreturnable (the sink rule), makes a frame or storage-bounded holder that rides it bounded by that source (clauses 4 and 6, §28.14), and makes the binding's own return or escape rejected (§28.3's callee-side list, §9.7's bullets). The count is flow-insensitive only for captured bindings, exactly the bindings whose reads and writes the flow analysis cannot order.

**Why not the local-scope bullet.** Tightening that bullet (rejecting a same-block reassignment from a later-declared source) closes the same-block route only: the in-lambda write, the slot store and the function-typed reassignment from an earlier-declared local (`g := lambda() => s[0]` with `s` declared before `g`) are not scope crossings. B closes all of them in one sentence.

## 4. Decision

Option B, as a delegated ruling. Clause 1 states the count; §27.5 points at it from the shared-cell sentence; §9.7's capture bullet and Appendix C.3 carry one clause and one row; the rationale records the grounds and the declines.

Five readings the critic should check, decided here:

1. **Which bindings.** A local or parameter of any state that some lambda captures — a plain, `using var`, `const` or owned local, a plain, `scoped`, `ref` or `out` parameter. The count per assigned value is clause 1's own: a borrowed value its source; an owned value the enclosing scope (so an owned captured binding's set is unchanged by its assignments, which are rejected inside a closure and frozen outside while the closure lives); a value that holds borrows the sources it carries; a function value its set, transitively; a value that carries no borrow nothing. A `ref` or `out` parameter contributes the declaring body's scope whatever it holds.
2. **Which assignments.** The initializer; a plain assignment `b := e`; a compound assignment; a store into a slot of a captured inline-array binding (`a[0] := it`, §15.1.2's "a slot store adds `e`'s sources"); a `ref` or `out` argument position naming the binding (the location's post-call set, *A `ref` or plain `out` location outlives the call*). A store through a member or index chain that lands in the binding's own storage (§9.4's *own storage*) — a slot of a captured inline array, a settable member of a captured record (`p.Item := it` over a body record with a settable `Item: Item?`), at any depth (`a[0].Item := it`) — is an assignment to the binding, adding the stored value's sources as §15.1.2's slot store does. An assignment to a field of a captured *object* (`b.Child := it`, `b` class-typed) lands beyond a class-typed hop, in the object, and is a field store judged by the field-store bullet (a scope-bound local is rejected there), not an assignment to the binding.
3. **Which bodies.** The declaring body and every lambda body, at any nesting depth, that captures the binding (a lambda nested in a lambda captures outward, provenance clause 1's last clause). A lambda body that declares its own local of the same name cannot: §21.1's *Local names* forbids the shadowing.
4. **Self-capture.** `var fact: (int) -> int; fact := lambda(n: int) => (if n <= 1 then 1 else n * fact(n - 1));` — the lambda captures `fact`, whose set would include the lambda's own set. The union is the least fixpoint: the self-capture contributes nothing beyond `fact`'s other assignments, so `fact` stays capture-free. The same holds for a cycle through two bindings (`var f: (() -> int)? := null; var g := lambda() => f!(); f := lambda() => g();`): each set is the least solution, the sources the cycle's assignments carry in from outside it. Stated in the clause for every cycle, not only self-capture.
5. **Reads before any formation.** The union counts assignments after a read too, and in-lambda ones before the lambda exists: `using var it := new Item(); var b := it; use(b); b := _field; var f := lambda() => b; return f();` is rejected (`f`'s set is `{it, this}`) although `b` holds `_field`'s value at every invocation. This is the over-rejection B accepts for one set per cell, and it is one of a family: an assignment counts against every read, so one made after the closure's last invocation (`var r := f(); … b := it; return r;`, `r` carrying `it`) or after a read of the binding itself (`if c then return b; end … var f := lambda() => b; … b := it;`, the early `return b;` rejected) is counted there too. Zero sites for the family (§5): no captured binding in the corpus is assigned a borrow-carrying value at all. The remedy is a second binding: `var c := _field; var f := lambda() => c;`.

## 5. Measurement

A sweep (`scratch/v0332/` lane `U1`'s `measure.py`) over every `.bzl` file, every `.cpp` test's embedded program and every ```` ```bezel ```` fence in `docs/` found each lambda, the enclosing body's locals and parameters it names, and every assignment to such a binding outside its declaration — before the lambda, after it, or inside a lambda body — then classified the binding by its declared type or initializer.

- **`tests/`** (152 candidate bindings in 91 files): 98 are `int`/`bool`/`string` counters and accumulators (`total := total + x`, `count := count + 1`, `s := p + "!"`), which carry no borrow and contribute nothing under clause 1 whatever their value. 32 are function-typed locals; 28 of them are reassigned to a **capture-free** lambda from inside a nested lambda (`f := lambda() => 42;` in `closure_captured_cell_returned_uaf_4198`, `closure_local_reassign_captured_cell_4227`, `closure_captured_cell_mutated_shared_nested_uaf_4240`, `lambda_nested_local_decl_capture_cell_leak_4261`) — the union stays empty, and `return f;` stays admitted. 4 are function-typed locals reassigned in a nested lambda to a closure over a `using var` of the same block, then invoked only in that block (`lambda_body_escape_3743__enclosing_lambda_local_same_depth_accepted`, `lambda_body_escape_3727__owned_local_assign_to_same_depth_local_accepted`, and their `.cpp` twins): the union adds `r`, the closure and the capturing `mk` are invoked where `r` lives, and nothing escapes — still admitted. The rejected lit rows of the same families (`closure_escape_outer_local_861`, `record_closure_capture_escape_2144__outer_local_store_rejected`, `choice_closure_capture_escape_2180__choice_local_outer_local_store_rejected`, `lambda_body_escape_3727`/`3743`'s `*_rejected` rows) assign across a block boundary and are rejected by the local-scope bullet before and after. The remaining 22 are 8 owned bindings (`var w := new Widget(); … w := new Widget();` in `s27_5_0003`, `var arr: [1 owned]Widget` in `s28_9_0006`, `sub2 := src.Ev.Subscribe(…)` in `event_raise_reentrant_dispose_908`, the `#4271`/`#4286`/`#4314`/`#4321` reassignment-becomes-owned rows), whose count clause 1 fixes at the enclosing scope, and 14 that are false positives or already rejected: a declaration the regex read as a reassignment (`var c := await t`, `var h := new Box(x)`, `var r := inner()`), a local declared inside the lambda body itself (`y := h.item` in `#4286`, `y := w` in `#4225`), `holder := r` in the `lambda_body_escape_3727`/`3743` `*_rejected` rows (an owned local of a lambda body stored to an enclosing local: the scope-bound rejection, four rows), and `h := h2` in `readonly_field_narrowing_2085__captured_root_invalidated_rejected` (two parameters, already rejected for narrowing). **No pinned verdict moves.**
- **Documents:** the code-generation reference's two hits are owned bindings in §7.2's merge examples (`var a := new Buffer();`, `var x: Buffer;` with owned/borrowed arms — the K7.3 rejection, no lambda involved). Guides 01, 04 and 05 reassign no captured binding; Guide 01 §9's `scoped` examples capture `conn` and read it. The proposals folder's hits (`exploration-ui-immediate-mode`, `0014`, `0134`, `0135`) are non-normative sketches of `int` and record counters.
- **Rule corpus** (`tests/conformance/rules/cases/`, the 120 v0.329 cases included): `l5_0001`, `l5_0011` (an `int` `calls` counter), `s27_5_0003` and `s28_9_0006` (owned). No case quotes clause 1's "that binding's source" or §27.5's shared-cell sentence (checked against `manifest.tsv`'s `rule:` column), so no `rule:` header moves.
- **Doc-example corpus** (`tests/conformance/cases/`): in the `tests/` sweep; no borrow-carrying hit.

**Newly rejected:** a closure stored, returned, passed at a set-erasing position or ridden by a returned frame or storage-bounded holder whose captured binding is assigned, anywhere in the declaring body or in a lambda body that captures it, a borrow of a scope-bound local, `using var`, temporary or `owned` parameter (or, for a sink, any source at all, where the formation-time value carried none); the return or escape of a captured binding that a lambda body assigns such a source (`g(); return s;`); and the §4 reading-5 shape. Zero sites. **Newly admitted:** nothing.

## 6. Compiler facts and the tracker

Source-read at `2679585`; no build was run by this lane.

- `src/ownership/Ownership_Checks.cpp:17277–17360` (`deferredActivationCaptureSources`): a frame's capture-set contribution is computed from the lambda literal's captures at the literal; for a local traced to a literal, "A reassigned local is not verifiable … contributes NOTHING (conservative admit)" (`:17330–17333`, the `scopedAssignedSyms_` gate). The compiler therefore *admits* the R7 shape where the ruling rejects it.
- `Ownership_Checks.cpp:20460–20478` (`scopedPreScanStmt`'s assignment arm): a per-body pre-scan already records every assigned local (`scopedAssignedSyms_`) and every lambda-literal initializer (`scopedLambdaLocalInit_`); `collectReassignedOwnedLocals` (`lambda_body_captured_reassigned_owned_alias_dispose_4314_test.cpp`'s fix) walks nested lambda bodies for owned reassignments. The union is a third such pre-pass: for each captured binding, the sources of every assignment in any body.
- `Ownership_Checks.cpp:5150` (`checkClosureEscapesOnReturn`) and the field-store path read the captured identifiers' state at the literal; the R2–R6 shapes (`_f := f;` before `b := it;`) are accepted today.
- `Ownership_Checks.cpp:19308` and `:19798`: the across-suspension and read-identifier walks do not descend into `LambdaExpr` — the same blind spot, for #5687 (lane `U1`'s ruling).

**Tracker (filed at landing, `spec-tracking`):** compute a captured binding's source set as the union over every assignment to it in the declaring body and in every lambda body that captures it (slot and record-member stores into the binding's own storage, and `ref`/`out` positions, included), and read that set at the closure's formation, at the binding's own reads, at a frame's capture-set contribution and at §28.14's admission; drop the "reassigned local contributes nothing" admit at `:17332`. Probe first with the §1 programs and with `g(); return s;`. Rule-corpus cases: §7.

## 7. Corpus cases to write (after landing)

- `s28_3_NNNN` reject: the §1 `Holder.Arm` program — `_f := f;` over `b: Item?` later assigned a `using var`.
- `s28_3_NNNN` reject: `return f();` over `var b := _item; var f := lambda() => b; using var it := new Item(); b := it;`.
- `s28_3_NNNN` reject: the R7 shape — `var g := lambda() => 0; var f := lambda() => g(); … g := lambda() => s[0]; return f;`.
- `s28_3_NNNN` reject: the in-lambda write — `var g := lambda() => do s := zs.AsSpan(); end; g(); return s;` over a receiver-sourced `s`.
- `s9_7_NNNN` reject: `return a.Select(f);` over a captured span reassigned to a local array (§1's `Window`).
- `s15_1_NNNN` reject: `a[0] := it;` after `_f := lambda() => a[0]!.N();` over `var a: [1]Item? := [null];`.
- `s28_3_NNNN` reject: `p.Item := it;` after `_f := lambda() => p.Item!.N();` over a body record `p` with a settable `Item: Item?` property, initialized `null`.
- `s28_3_NNNN` reject: the two-binding cycle — `var f: (() -> Item)? := null; var g := lambda() => f!(); f := lambda() => do var x := g(); return it; end;` with `it` a `using var`, then `return g;`.
- `s28_3_NNNN` accept (run): a captured counter `var n := 0; xs.ForEach(lambda(x) => do n := n + x; end);`; a function-typed local reassigned to a capture-free lambda from a nested lambda and returned (`closure_captured_cell_returned_uaf_4198`'s shape); the recursive closure of §27.5; `var b: Item? := null; var f := lambda() => b!.N(); b := p; return f();` with `p` a plain parameter (set `{p}`, invocation in scope).

## 8. Self-critique (adversarial pass)

- *Does the union over-reject captured counters?* No: an `int`, `bool` or `string` value contributes nothing under clause 1 at every assignment, so the union is empty whatever the counter does.
- *Does it bar the recursive closure?* No: reading 4 of §4 states the least fixpoint.
- *A captured binding reassigned only before the lambda forms, from a shorter-lived source it no longer holds?* Rejected (§4, reading 5) — one of the stated over-rejection family (with an assignment after the closure's last invocation and one after an early read of the binding), zero sites, one-line remedy.
- *Does the rule reach a binding assigned through a `ref` argument?* The current value of a `ref` argument is read at the call and the location's post-call set is the call's union (*A `ref` or plain `out` location outlives the call*); that post-call set is one of the binding's assignments for this count (§4, reading 2).
- *A capture of a loop variable?* Each iteration's binding is fresh (§27.5), so a `for` variable has one assignment per binding; the union is that assignment.
- *Does §28.14's admission need its own sentence?* No: its "captured source is §28.3's notion here as everywhere" inherits the count, so a later-declared source assigned to a captured binding fails the declared-before test and the storage freeze covers every source in the union. Bound 2(b)'s "known non-hole" concerns reassignment of the *source* under the freeze, not of the captured binding, and is unchanged.
- *Neighbouring matrix cells.* R1/C6 and R1/C7: unchanged (an owned captured binding's count is the enclosing scope at every assignment). R8/C6, R8/C7: decided by the same union, per instantiation as clause 1 reads it (a `T` value a generic body reads out of a body-local collection is local-sourced under the class-or-interface assumption, and the union counts it like any other assignment; it can only add rejections), so no cell opens. R9: unchanged (nothing contributed at any value). Every other column that reads a closure's set — C1–C5, C8, C10, C12, C13 — reads the union, so no cell opens; C9's closure routes are decided by the same count, its in-body `await` read being #5687's ruling. The twelve cells R2–R7 × C6/C7 become **decided — rejected**.
- *Dependency on lane `U2` (#5707, span reads and the flag).* None in the rule: the hole is a static admission of a scope-bound source, which no flag covers; `U2`'s answer changes only the severity note for the span rows.
