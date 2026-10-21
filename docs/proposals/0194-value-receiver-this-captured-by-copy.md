# Proposal 0194 — A closure in a value-kind member body captures `this` by copy

| | |
|---|---|
| **Status** | graduated-v0.340 — superseded — graduated v0.340 (accepted 2026-10-21 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-21 direction for this train ("let's do the next train that fixes all current spec issues"), [#6694](https://github.com/s8lap/bezel-lang/issues/6694), option C — a closure formed in a copyable value kind's instance member body, a lambda naming `this` or a bound method reference over it, captures the fixed receiver value by copy at its formation, into its heap capture cell, and contributes what a captured local of the receiver's type contributes: nothing over a type that holds no borrows, so the closure is capture-free and may be returned, stored or set-erased, and the carried borrows' sources otherwise, under §28.3's sink rule (the leg #6630/#6634 settled); a move-only receiver's `this` stays §28.9's move-only capture, a borrow bounded by the call, read per instantiation for a generic choice (§35.11), and a bound method reference over it stays §27.6's formation error; in a value kind's constructor body the copy is a whole-value read of `this` gated by §22.7 rule 1, whose rule 3 exemption now names a class's `this`; §9.4's store rejection reads a lambda body inside a value kind's member or constructor body as a member body; critique round 1 applied (the constructor-body lambda store; §22.7 rule 3's first sentence scoped; the one newly rejected gtest row named; §28.14's admission bounds and §35.11's list extended; the code-generation reference's table row replaced by a *Looks right, is rejected* pair); landed with the v0.340 changelog entry) |
| **Tracking issue** | [#6694](https://github.com/s8lap/bezel-lang/issues/6694) (the question); [#6634](https://github.com/s8lap/bezel-lang/issues/6634) and [#6630](https://github.com/s8lap/bezel-lang/issues/6630) (the measured stack-use-after-return and the settled borrow-carrying leg, lane `r34-sink`); [#6318](https://github.com/s8lap/bezel-lang/issues/6318) (`case this` inside a lambda in a choice method, held behind this decision). One `spec-tracking` compiler tracker is filed at landing (§7). |
| **Target module(s)** | **spec**: §9.4 (*The receiver value is fixed at the call*: one passage after the frame sentence), §22.7 rule 3 (its first sentence scoped to a class's `this`; one clause for a value kind's constructor body), §28.3 (the producers table's `this` row: one clause; the closure bullet's provenance clause 1: one clause), §28.9 (the move-only capture paragraph: one sentence), §28.14 (*v1 admission bounds* item 1: two clauses), §35.11 (the instantiation-enforced list: one item), Appendix C (the holds-borrows capture bullet: one clause). **rationale**: §9.15 (one dated paragraph). **code-generation reference**: §7.3 gains one *Looks right, is rejected* pair. **`DECLINED.md`**: three `declined-at-proposal` rows. No stdlib edit, no grammar change, no new keyword, no kernel edit. |
| **Written against** | spec/stdlib **v0.339** at `d60128c46` (main). Section numbers are v0.339's. |
| **Origin** | The #6630/#6634 reviewer (lane `r34-sink`, 2026-10-21) found two readings of v0.339 for a closure over `this` in a `record Pt(x: int)` method: §9.4 + K8.4 make `this` a borrow of a call-bounded temporary whose capture is an escape, rejected; §28.3's provenance clause 1, §27.6's by-copy receiver capture and §9.4's frame rule make a borrow-free value a copy that contributes nothing. The compiler accepts the program and captures a stack address (#6634: `3774` then a garbage value; `--probe --asan` reports stack-use-after-return in `lam$0`). The owner deferred the borrow-free leg to "a later decision train" (2026-10-21) and shipped the settled legs — a class receiver's `this` and a borrow-carrying value receiver's `this` are rejected at the sinks. This is that train; lane `B1` of v0.340. |

*Reference convention: unprefixed §-references are language-spec references at v0.339; "stdlib §" references are to the standard library companion; K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares; none invents an API. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

In every instance member body of a value kind `this` binds a borrow of the receiver value (§9.4), and the receiver value is fixed at the call: "the receiver value is a **full-expression temporary** of the call (K8), and the borrow `this` is bounded by it (K8.4)". K8.4 says an escaping temporary-sourced borrow "is rejected, never lifetime-extended", naming "a binding, store, capture, `return`" among the escapes. Read through those two sentences, `return lambda(): int => this.x;` in `record Pt(x: int)` is a compile error, the reading #6634 filed with ("the spec settles it … `this` is a borrow").

Three other passages read the same program the other way. §28.3's provenance clause 1 counts a captured local or parameter of a value type by the borrows it carries and says "a captured value that carries no borrow — a value type that holds none, a null-source value, a `string` — contributes nothing, and lives in its heap capture cell". §27.6 captures a value-shaped receiver of a bound method reference **by copy**, so `p.Adder` over a borrow-free record is a capture-free function value whatever `Adder`'s body does. §9.4 itself lets an `async` or iterator member hold "the fixed receiver value in its frame, as the frame holds a plain value argument, the value's borrows becoming carried sources of the frame" — a copy of the receiver held past the call, bounded by its carried borrows and by nothing else. And `var self := this; return lambda() => self.x;` is admitted by every reading: a value local that holds no borrows contributes nothing. The rejecting reading therefore forbids one spelling of a program whose one-line rewrite is admitted with identical semantics, and gains no soundness from it.

**Decision.** A closure formed in a copyable value kind's instance member body — a lambda that names `this`, explicitly or through a bare member read that resolves to the receiver, or a bound method reference over `this` — captures the fixed receiver value **by copy**, taken at the closure's formation into its heap capture cell. The copy is unobservable: nothing writes what `this` denotes for the whole activation, and §9.4's store rejection reads a lambda body inside the member body as the member body, so no chain rooted at the captured `this` can write the copy either. The capture contributes to the closure's source set exactly what a captured local of the receiver's type contributes (provenance clause 1): **nothing** where the type holds no borrows — the closure is capture-free and goes wherever a capture-free function value goes, returned, stored in a field or collection, passed to a set-erasing position — and **the sources of the borrows the value carries** where it holds them, so the closure meets the sink rule as any capture-bearing closure does (#6630's leg, unchanged). A **move-only** receiver's `this` is §28.9's move-only capture, a borrow of the in-place storage bounded by the call, and such a closure may not outlive the activation (a bound method reference over it stays §27.6's formation error); for a generic value kind both counts are read per instantiation. In a value kind's **constructor** body the capture copies the value under construction, so it is a whole-value read of `this` under §22.7 rule 1's observation gate — admitted where every field the gate names is definitely assigned, the closure holding the value as of its formation — and a lambda body there is read as a member body for §9.4's store rejection, since the closure outlives the construction window.

**Reach, measured (§6):** no rule-corpus, doc-corpus, specification, guide or standard-library program changes verdict; no gtest or lit row is newly rejected by the by-copy leg. The constructor clause narrows §22.7 rule 3's stated admission for value kinds, and newly rejects one gtest row (`tests/sema/record_ctor_this_observation_gate_4014_test.cpp:273`, a borrow-bearing record's constructor storing a `this`-closure into a function-typed component before `Extra := w;`), which the settled borrow-carrying leg also rejects. The compiler change is a codegen copy plus that gate (§7).

---

## 1. The problem, in the text

### 1.1 Two readings

| Passage | What it says at v0.339 | Reading |
|---|---|---|
| §9.4, *The receiver value is fixed at the call* | "The receiver value is a **full-expression temporary** of the call ([K8]), and the borrow `this` is bounded by it ([K8.4])" | reject: a capture escapes the boundary |
| K8.4 | "a borrow or frame that would escape it is **rejected, never lifetime-extended** … The escapes rejected are every escape [K2.3] can compare — among them a binding, store, capture, `return` or `yield return` of the borrow in the producing body" | reject |
| §28.3, producers table, `this` row | "Borrowed (source: caller) — for a copyable value-kind receiver, the call's fixed receiver value, a full-expression temporary bounded by the call (§9.4; K8.4)" | reject |
| changelog v0.298 (#4154), pinned history | sinks "stay closed, a closure over `this` returned or field-stored included" — written of the class receiver, with no class/value distinction | reject, read literally |
| §28.3, closure bullet, provenance clause 1 | "a captured value that carries no borrow — a value type that holds none, a null-source value, a `string` — contributes nothing, and lives in its heap capture cell" | copy: the value is what is captured, and it carries nothing |
| §27.6 | a method reference captures the instance "**by copy** for value-shaped receivers — a record, an enum, a copyable choice, a non-owning inline array, a primitive, the unit value …" | copy |
| §9.4, the frame sentence | an `async` or iterator member "holds the fixed receiver value in its frame, as the frame holds a plain value argument (§29.4), the value's borrows becoming carried sources of the frame (K6)" | copy: a holder outliving the call keeps the value, bounded by its carried borrows only |
| §9.7, closure bullet | "A borrow-bearing record may not be captured by a closure that escapes its source's scope: the capture contributes the sources of the borrows the record carries" | copy, bounded by the carried borrows (said of a captured record value; `this` is such a value) |

Neither reading is stated for `this`: provenance clause 1 counts "a captured local or parameter", and K8.4 names a capture "of the borrow". The text has an answer for every captured value of a value type and for every captured `this` of a class, and none for the one case that is both.

```bezel
public record Pt(X: int)
  public method Adder(): (int) -> int => lambda(d: int) => this.X + d;   // v0.339: §9.4 + K8.4 reject it; §28.3 cl. 1 + §27.6 admit it. ▲ v0.340: admitted, `this` copied
  public method Add(d: int): int => X + d;
  public method Adder2(): (int) -> int => this.Add;                       // v0.339: admitted — §27.6 captures `this` by copy
  public method Adder3(): (int) -> int
    var self := this;                                                      // a copy; `self` holds no borrow
    return lambda(d: int) => self.X + d;                                   // v0.339: admitted — provenance clause 1
  end Adder3;
end Pt;
```

The three members compute the same function value. Under the rejecting reading the first is a compile error and the other two are admitted.

### 1.2 What the compiler does (`d60128c46`, source read; no build in this container)

- **Sema** has no rule for a `this` capture in a value-kind body at all: the ownership pass's closure-escape walker keys on captured locals and parameters (`grep -n 'capturesThis\|thisCapture' src/ownership src/sema` finds nothing). Lane `r34-sink` (#6630/#6634) adds the sink-rule rejection for a class receiver's `this` and for a value receiver that carries a borrow; the owner's 2026-10-21 comment on #6694 keeps the borrow-free value receiver out of that lane.
- **Codegen** stores the receiver *pointer* into the closure environment for every `this` capture: `src/codegen/LlvmBackend_Exprs2.cpp:1551` ("the `this` capture stores the receiver pointer directly (by value, as `this` is not reassignable so it needs no shared cell)"), `:4180` ("`this` is stored by value; every other capture stores the address of its storage"). For a class receiver that pointer is the object; for a value receiver it is the address of the call's fixed-value copy on the callee's stack, which is dead once the member returns. #6634 measured the consequence (QUOTED): the record repro prints `3774` then `-877698370` where `7` is expected, and `--probe --asan` reports stack-use-after-return in `lam$0`; the choice repro prints `3774` then `Unhandled exception: System.MatchFailureException`, the dead frame's tag matching no arm.
- The #6318 codegen fix (`case this` inside a lambda in a choice method) is held because it would turn an IR-verifier rejection into that silent wrong answer.

So a fix is owed under either reading: a sema rejection of the borrow-free leg (option B), or a codegen copy (option C).

---

## 2. Options

- **A — Status quo.** Leave the two readings in the text; the compiler keeps capturing an address. Not sound; not considered further.
- **B — Reject** (#6634's reading; the v0.298 sentence read literally). `this` in a value-kind body is a borrow of a K8-bounded temporary, a capture is an escape of that borrow, and every closure over a value kind's `this` is held to the sink rule by a non-empty set — never returned, stored or passed to a set-erasing position — whatever the receiver type holds. The remedy is `var self := this;`. One sema check covers classes and every value kind alike.
- **C — Capture by copy** (this proposal; the issue's recommendation). The capture copies the fixed receiver value at formation; the set is the carried borrows'; a move-only receiver's `this` stays a borrow; a value kind's constructor `this` capture is gated by §22.7 rule 1, and a lambda body in that constructor is a member body for §9.4's store rejection.
- **D — Lifetime-extend the receiver temporary** to the closure's life, so that the captured borrow stays valid. Against K8.4 in so many words ("rejected, never lifetime-extended") and rationale §5.24; a closure is runtime-managed and has no release point the temporary could ride.
- **E — C, with a value kind's constructor capture ungated**, reading §22.7 rule 3 ("closures observe at invocation, not at capture") for value kinds as for classes.

---

## 3. Evaluation

**Soundness.** B and C are both sound; D is not expressible in the kernel. Under C the hazard #6634 measured cannot arise: the closure reads its own copy, the copy's storage is the capture cell the closure keeps alive, and the one thing a copy could get wrong — divergence from the receiver — is excluded by §9.4 itself: for the whole activation `this` denotes one value, nothing writes it ("no store that runs during the call … changes what `this` denotes, and the activation's completion stores nothing back"), and a store rooted at `this` is rejected at the member body — a lambda body inside the member body included, which the §9.4 edit states so that the capture cell is not a writable copy of the receiver either. A borrow-bearing receiver's copy carries exactly the borrows the receiver carried, so its closure's set is non-empty and the sink rule (the leg `r34-sink` ships) rejects every escape; within the body it is bound, invoked and passed as any capture-bearing closure is. A move-only receiver cannot be copied, so C keeps §28.9's rule for it — a borrow of the in-place storage, bounded by the call — and §9.4's write-point posture for a borrowing `case this` inside such a closure is unchanged. E is unsound in the way §22.7 rule 1 exists to prevent: a copy taken before `Extra := w;` holds the null watermark where `Extra` is a non-nullable field, and the closure invoked after construction reads it — the by-copy capture is the observation, and rule 1 must gate it as it gates `this` as an argument.

**Simplicity.** C adds no mechanism: the by-copy capture is §27.6's rule for a bound reference's value-shaped receiver and §9.4's rule for a frame, and the count is provenance clause 1's existing clause for a captured value; the text gains one sentence at §9.4 and one clause at each place that enumerates captures. B adds a rejection with no soundness content — `var self := this;` is admitted by every reading and computes the same closure — and makes `lambda() => this.X` rejected beside `this.Get` (a method reference, by copy, admitted): two spellings of one closure with opposite verdicts. B also costs the natural record-method closure (`Adder` above) and every choice-method lambda that matches `this` (#6318's shape).

**Consistency.** Under C every holder that keeps a copyable receiver past the call holds it the same way: a frame (§9.4), a bound method reference (§27.6), a returned `this` (§9.7's receiver-sourced form 1 for a borrow-bearing value, a plain value return otherwise), a `var self := this;` local, and now a closure. The slogan is one sentence: *a copyable value's `this`, held past the call, is a copy bounded by what it carries.* K8.4 is untouched: no borrow escapes the boundary, because what escapes is a value, not the borrow. Rationale §9.15's three options for the fixed receiver already declined "by address, with the overwrite visible" on the ground that "every member body's correctness would depend on every caller's closures" — a captured address would reintroduce that dependence after the call, which is what the measured stack-use-after-return is.

**Migration reach** (§6). The by-copy leg rejects nothing: the borrow-carrying leg is unchanged and the borrow-free leg was never rejected by the compiler. Newly admitted against the rejecting reading: a returned, stored or set-erased closure over a borrow-free value kind's `this` — zero sites in the rule corpus, the doc corpus, the documents and the guides; zero gtest or lit rows pin either verdict for the borrow-free leg. Newly rejected, against §22.7 rule 3's v0.339 text ("is not gated by rule 1", stated for classes and records alike): a value kind's constructor capture of `this` before every field the gate names is assigned — one gtest row, `4014:273`, which the settled borrow-carrying leg rejects too; and a `this`-rooted store inside a lambda in a value kind's constructor body — zero sites.

---

## 4. Decision

**Option C.** The rules, stated at their homes:

> ▲ **§9.4** (*The receiver value is fixed at the call*, after the frame sentence). A closure formed in the body — a lambda that names `this`, explicitly or through a bare member read resolving to the receiver, or a bound method reference over it ([§27.6]) — captures the fixed receiver value **by copy**, taken at the closure's formation into its heap capture cell ([§27.5]), as a bound reference captures a value-shaped receiver and as a frame holds the value above; the copy is unobservable, since nothing writes what `this` denotes for the whole activation and the store rejection above reads a lambda body inside the member body as the member body, and one inside a value kind's constructor body as a member body too, a function value outliving the construction window ([§22.8]) — a chain rooted at the captured `this` that reaches the value is rejected there. The capture contributes to the closure's source set exactly what a captured local of the receiver's type contributes ([§28.3]'s closure bullet, provenance clause 1): nothing where the type holds no borrows ([§3.6]), so such a closure is capture-free and goes wherever a capture-free function value goes — returned, stored, passed to a set-erasing position — and the sources of the borrows the value carries where it holds them (in the body, `this`'s, [K2]), so the closure meets the sink rule as any capture-bearing one does and is bound, invoked and passed within the body under that bullet. In a **move-only** receiver's member body (below) a lambda's capture is [§28.9]'s move-only capture — a borrow of the in-place storage, bounded by the call — and such a closure may not outlive the activation; a bound method reference over that `this` is [§27.6]'s formation error. For a generic value kind both counts are read at each instantiation ([§3.6] rule 2): at one whose classification is owning, a closure over `this` that would outlive the activation is a requirement of the constructed type, rejected at every site that names that type, as [§19.3]'s write-point rule is at a move-only substitution. In a value kind's **constructor** body the same copy is a whole-value read of `this` under [§22.7] rule 1, and the closure holds the value as of its formation: a later store in the body does not reach it.

> ▲ **§22.7 rule 3.** Its first sentence reads "Capturing a class's `this` in a lambda or method reference is not a synchronous observation …", and after "stays the standard observer pattern).": In a **value kind's** constructor body the capture is instead a copy of the value under construction ([§9.4]), so it observes at capture: it is a whole-value read of `this` and rule 1 gates it as one, admitted where every field the gate names is definitely assigned, the closure holding the value as of its formation.

> ▲ **§28.3, producers table, `this` row.** "… a full-expression temporary bounded by the call ([§9.4]; [K8.4]), a closure formed in the body capturing that value by copy ([§9.4]); for a move-only receiver, …"

> ▲ **§28.3, provenance clause 1** (after "lives in its heap capture cell)"). "; a captured `this` of a value-kind member body, which the capture copies ([§9.4]), what a captured local of the receiver's type contributes — nothing where the type holds no borrows, otherwise the sources of the borrows the value carries"

> ▲ **§28.9** (after the move-only capture sentence). A **move-only receiver's** `this` ([§9.4] *Move-only receivers*) is captured the same way, its source the member body's outermost scope — the storage is lent to the activation in place — so such a closure may not outlive the call; a **copyable** value receiver's `this` is captured by copy ([§9.4]).

> ▲ **§28.14** (*v1 admission bounds* item 1). The exempt capture: "a captured local or parameter of a value type that holds no borrows (…), or a copyable value receiver's `this` of such a type, which the capture copies ([§9.4]), lives in its heap capture cell"; the tested captures: "locals and parameters of a value type that holds borrows, and a copyable value receiver's `this` of such a type ([§9.4])".

> ▲ **§35.11** (the instantiation-enforced list). "… [§9.4]'s rejection of an `async` or iterator member call on a move-only receiver, and its call bound on a closure over a move-only `this` — are enforced at the instantiation or use site …"

> ▲ **Appendix C** (the holds-borrows capture bullet). "A closure that captures a local or parameter of a value type that holds borrows — or a method reference bound to such a receiver, or `this` in a value-kind member body, which the capture copies ([§9.4]) — carries the sources of the borrows that value holds in its source set, …"

Consequences, each already the rule on at least one neighbouring position:

1. **A borrow-free value kind's `this`-closure is capture-free.** `return lambda(d: int) => this.X + d;` over `record Pt(X: int)`, `a.Add(lambda(): string => (case this of A(n) => $"A{n}", B => "B"));` over a value-like choice, and an enum method's `lambda() => int(this)` are admitted at every sink — the same verdict §27.6 gives `this.Add` and provenance clause 1 gives `var self := this;`.
2. **A borrow-bearing value kind's `this`-closure carries the receiver's borrows** (#6630's leg): `return lambda() => this.Item.Name;` over `record View(Item: Item)` is the sink rule's rejection, as is `a.Add(lambda(): string => (case this of One(x) => x.GetLabel(), Nothing => "none"))` over a choice holding a class payload; `var f := lambda() => this.Item.Name; return f();` is admitted. The sources are the receiver's (K2's `this`), so a deferred-activation frame built in the body over such a closure is receiver-sourced for *Returning a deferred-activation frame* exactly as one built over `this` in a class is.
3. **A move-only receiver's `this`-closure may not outlive the call** (§28.9): over an owning choice, `var f := lambda() => (case this of Ready(c) => c.Id, Empty => -1); return f();` is admitted and `return f;` is rejected, as a closure over a captured owning inline array is; §19.3's write-point rule for a borrowing `case this` inside the lambda is unchanged; `this.M` there is §27.6's formation error, as over any move-only receiver. In a generic choice the leg is the instantiation's: `return lambda() => (case this of Some(v) => 1, None => 0);` in `Opt<T>` is admitted at `Opt<int>`, rejected wherever `Opt<Item>` is named (the carried borrow) and wherever `Opt<Bundle>` over an owning `Bundle` is named (the call bound).
4. **The copy is not a mutable receiver.** `lambda() => do this.X := 1; end` inside a record method is §9.4's store rejection wherever the lambda stands in the body; `lambda() => do this.Obj.X := 1; end` through a class-typed hop stays the mediating shape, legal.
5. **A constructor's `this`-closure is an observation.** Over `record Pt(X: int)`, `var f := lambda() => this.X;` before `X := x;` is rule 1's compile error and after it is admitted, `f` holding the value as of its formation; storing such a closure into the record's own function-typed component in the constructor body is initialization (§9.4's *Constructor bodies are not receiver positions*), admitted where the gate holds at the capture — the copy then holds that component at its default or null watermark — and, where the record holds borrows, the sink rule's rejection (a function-typed holder); a store into the record's own storage written *inside* the lambda is §9.4's store rejection, the lambda body being read as a member body. §22.7 rule 3's "closures observe at invocation" is the class receiver's rule, where the capture is the reference.
6. **Nothing moves for a class receiver.** `this` in a class body is captured as the reference it is; its closure carries the receiver source and meets the sink rule (the settled leg); §28.14's storage-bounded admission is unchanged.

Kernel: derivation — K8.4 (no borrow escapes the boundary: what the closure holds is a copy), K1.6 and K2.2 (a copied value carries only the borrows it holds, unioned into the closure's set), provenance clause 1's existing count for a captured value. No axiom touched. Body-locality: local — the count reads the receiver type's holds-borrows property (§3.6), per instantiation for a generic value kind as §9.7's generic-body paragraph already reads it.

---

## 5. The exact normative edits (▲)

The machine-actionable list is `scratch/v0340/B1.json` (edits with ids `B1-6694-*`; every `old` occurs exactly once at `d60128c46`). In prose:

**Spec.**
- **§9.4**: the passage of §4 inserted after "the value's borrows becoming carried sources of the frame ([K6](#sec-kernel-k6))."
- **§22.7 rule 3**: the first sentence scoped to "a class's `this`", and the clause of §4 inserted before "An admitted `this`-capturing closure that is nevertheless *invoked before construction completes*".
- **§28.3**, producers table, `this` row: the clause of §4.
- **§28.3**, closure bullet, provenance clause 1: the clause of §4.
- **§28.9**: the sentence of §4 after "and may not outlive it."
- **§28.14**: the two clauses of §4 in *v1 admission bounds* item 1.
- **§35.11**: the item of §4 in the instantiation-enforced list.
- **Appendix C**: the clause of §4 in the holds-borrows capture bullet.

**Rationale.** §9.15 gains one dated paragraph (v0.340, proposal 0194) after the v0.336 paragraph, recording the decision, the four grounds and the three declines.

**Code-generation reference.** §7.3 gains one *Looks right, is rejected* pair (its note also carries the move-only and constructor legs; §4.4's *Closures by position* table is keyed on positions, not on captures, so it gains no row) before the `Show(o: Object)` block: `record Pt(X: int)` returning `lambda(d: int) => this.X + d` (fine) beside `record View(Item: Item)` returning `lambda() => this.Item.Name` (ERROR: the copied `this` carries `Item`'s borrow). No pins in prose.

**`DECLINED.md`.** Three `declined-at-proposal` rows (§9), inserted at the group's end.

**Stdlib, grammar, guides.** No edit: the stdlib declares no closure over a value-kind `this`; the grammar is untouched; the guides' records and choices form no lambda over `this` (the §6 census).

---

## 6. Measurement

Census method (`scratch/v0340/B1work/scan6694b.py`): every `record`, `choice` or `enum` body in `tests/` (gtest C++ string programs, lit `.bzl`, both conformance corpora) and in `docs/*.md` (the specification, the standard library, the rationale, the guides, the tour, the code-generation reference; the changelog excluded as pinned history) scanned for a `lambda` whose text names `this`; every hit read by hand. The critic's re-scan (`scratch/v0340/B1crit/scan_implicit.py`) adds the implicit spelling — a lambda naming one of the type's instance members bare — and finds two more gtest rows, both bound and invoked in the body or rejected on another ground (`tests/codegen/readonly_record_single_assignment_2207_test.cpp:87`, `var f := lambda() => _name.Length; return f();`, expects `3`; `:163`, the §22.8 reject row whose lit twin is listed below); no verdict moves.

| Set | gtest | lit | rule corpus | doc corpus | docs |
|---|---|---|---|---|---|
| value-kind bodies containing a lambda that names `this` | **2 files, 3 hits**: `tests/sema/record_ctor_this_observation_gate_4014_test.cpp:273`; `tests/codegen/record_this_value_6320_test.cpp:123`, `:126` (one program) | 0 (`tests/lit/sema/readonly_record_single_assignment_2207__this_escape_stored_lambda_rejected.bzl` forms one inside a record method and is rejected on §22.8's single-assignment ground, unchanged) | 0 | 0 | 0 |
| … escaping (returned, stored, set-erased) over a **borrow-free** receiver — the leg this proposal admits | 0 | 0 | 0 | 0 | 0 |
| … escaping over a **borrow-carrying** receiver — the settled leg | 1 (`4014:273`: `Deferred := lambda(): int => this.Peek();` in the constructor of a record holding a class-typed `Extra`; stored into the record's function-typed component, invoked after construction, expects `42`) | 0 | 0 | 0 | 0 |
| … bound and invoked within the body (admitted under every reading) | 1 (`6320:126`: `var f := lambda(): Tag => this; a.Add(f());` in a value-like choice's method, expects `7`) | 0 | 0 | 0 | 0 |
| #6634's repros (p4 record, p5 choice) | unpinned | — | — | — | — |

**Verdicts.** No lit, rule-corpus or doc-corpus program changes verdict; no banked `baseline.tsv` row moves. No gtest row is newly rejected by the by-copy leg. `4014:273` flips accept → reject under the constructor clause (the capture precedes `Extra := w;`, and §22.7 rule 3 no longer exempts a value kind's capture) and under the settled borrow-carrying leg as well (`r34-sink`: a closure carrying `Extra`'s borrow stored into a function-typed holder); `6320:126` keeps its `7` once codegen copies the receiver. #6634's two repros become `run` rows printing `7` and `A7` (§7). Newly admitted against the rejecting reading: zero measured sites.

**Words.** Spec, token-level over the six edited spans: net about +230 words, two thirds of them in §9.4; the rationale and the code-generation reference are excluded.

---

## 7. Compiler facts and the tracker

One `spec-tracking` issue at landing, "§9.4 / §28.3 (v0.340): a closure over a copyable value receiver's `this` is captured by copy — proposal 0194", carrying:

1. **Codegen copies the fixed receiver value into the closure environment** for a `this` capture in a value-kind member body — a cell holding the value's bytes, retained and released as a captured value local's cell is (the runtime-managed references it holds included) — in place of the receiver pointer `LlvmBackend_Exprs2.cpp:1551`/`:4180` store today. The class receiver's pointer capture is unchanged. The move-only receiver keeps its in-place pointer, which the sink rule bounds to the call.
2. **Sema (lane `r34-sink`, #6630/#6634) must not reject the borrow-free leg**: the sink-rule check for a value receiver's `this` capture keys on the receiver type's holds-borrows property (§3.6), per instantiation for a generic value kind, exactly as the check for a captured value local does; a borrow-free `this` capture contributes nothing. The constructor clause (§22.7 rule 1 gating a `this` capture in a value kind's constructor body) lands with it: `tests/sema/record_ctor_this_observation_gate_4014_test.cpp:273` `lambda_capture_of_this_before_field_assigned_accepted_spec_22_7` flips accept → reject (two grounds; the diagnostic to pin is the sink rule's or rule 1's, the tracker to choose and name).
3. **#6318 lands after 1–2**: `case this` inside a lambda in a choice method then reads the copied value.
4. **Rows to add**: #6634's p4 (`Pt.Maker` returned, prints `7`) and p5 (`Tag.Maker` returned, prints `A7`) as `runAot` rows, with the `Clobber` call kept so the dead-frame reading would be visible; an ASan leg (`BEZEL_AOT_FORCE_SANITIZE=address`) over p4, since the fix moves a capture into the heap; `4014:273` re-cut to a reject row.

---

## 8. Corpus cases to write (after landing)

- §9.4 / §28.3: `record Pt(X: int)` returning `lambda(d: int) => this.X + d` — `run`, stdout `12` over `new Pt(7).Adder()(5)`; the same closure added to an `Array<(int) -> int>` field — `accept`.
- §9.4 / §28.3: `record View(Item: Item)` returning `lambda() => this.Item.Name` — `reject`, `diagnostic: sink` (the settled leg, keyed on the receiver's borrow); `var f := lambda() => this.Item.Name; return f();` — `accept`.
- §9.4 / §28.9: an owning choice's method returning `lambda() => (case this of …)` — `reject`; the same closure invoked in the body — `accept`.
- §22.7: `record Pt(X: int)` whose constructor forms `var f := lambda() => this.X;` before `X := x;` — `reject`; after it — `accept`.
- §9.4: `lambda() => do this.X := 1; end` inside a record method — `reject` (the store rejection through a lambda body).

---

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **Option B, rejecting every closure over a value kind's `this` at the sinks** (#6634's reading; the v0.298 sentence read for value kinds). It rejects one spelling of a closure whose rewrite `var self := this; return lambda() => self.X;` every reading admits with identical semantics, and makes `lambda() => this.X` rejected beside the admitted `this.Get` (§27.6, by copy); no soundness is gained once the capture is a copy, and the natural record-method and choice-method closures are lost. Reopening bar: none stated.
- **Option D, lifetime-extending the receiver temporary to the closure's life.** K8.4 states the posture in so many words ("rejected, never lifetime-extended"; rationale §5.24), and a closure is runtime-managed (§28.9) with no release point the temporary could ride. Reopening bar: none stated.
- **Option E, a value kind's constructor capture ungated** — §22.7 rule 3's "closures observe at invocation" read for value kinds. The copy is taken at formation, so a capture before a non-nullable field's assignment holds the null watermark and the closure invoked after construction reads it; the by-copy capture is the observation rule 1 exists to gate. Reopening bar: a value-kind `this` capture that is not a copy.

---

## 10. Self-critique (adversarial pass)

- **Is this a design change?** The text as a §9.4 + K8.4 reader applies it rejects the program (`this` is a K8.4-bounded borrow; a capture is a named escape), and the v0.298 entry closed the sink for "a closure over `this`" without a value carve. Admitting the borrow-free leg changes that stated verdict, so the stricter gate (process §2) is taken: a proposal, although the compiler never rejected the program and three passages already state the by-copy reading for every neighbouring position.
- **Does the copy open a mutable-receiver loophole?** A store rooted at the captured `this` would land in the capture cell, a copy — observable across invocations of one closure and nowhere else. The §9.4 edit reads a lambda body inside the member body as the member body, so the store is rejected where every other `this`-rooted store is. The mediating shape (a store beyond a class- or interface-typed hop) stays legal and lands in the borrowed object, as outside the lambda.
- **The constructor leg.** A record constructor's `this` names the value under construction, whose storage is the construction's temporary, so a pointer capture dangles exactly as the member-body capture does; by copy is the only sound capture there too, and the copy is an observation. Rule 1's gate already names "`this` as an argument; `this` stored anywhere" — a copy into a capture cell is both. Measured: one row, already flipping under the settled leg.
- **Generic value kinds.** `record Box<T>(V: T)`'s method returning `lambda() => this.V` is capture-free at `Box<int>` and carries `V`'s borrow at `Box<Item>` — provenance clause 1's "in a generic body, read at each instantiation at which the type, after substitution, holds borrows" applies unchanged, the count being a captured value's.
- **A lambda in a value kind's constructor body that stores into `this`.** Constructor bodies are not receiver positions, but the closure outlives the construction window (§22.8's lexical rule for `readonly` fields), and under the copy the store would land in the capture cell; the §9.4 edit reads such a lambda body as a member body, so the store is rejected. Measured: zero sites.
- **Generic move-only instantiations.** A generic choice's method body type-checks once (§35.11); the call bound on a closure over `this` at an owning instantiation is enforced at the constructed type, as §19.3's write-point rule is, and §35.11's list names it.
- **Extension members.** `this` in an extension member over a value-kind receiver is the receiver parameter (§26.7) and "read[s] identically" (§9.4): captured by copy, counted by what it carries. No separate sentence is needed.
- **Boxed receivers.** A member call on a boxed receiver runs over the payload; a closure formed in that body copies the payload. The box's immutability (§8.6, §28.9) is untouched, since nothing is written back.
- **Method references over `this` inside the body.** Already by copy (§27.6); the §9.4 sentence names them so a reader finds one rule for both spellings.
- **Matrix cells.** The borrow-flow matrix's closure cells (R2/C6, R3/C6) quote provenance clause 1 fragments that end before the inserted clause ("contributes nothing, and lives in its heap capture cell" is quoted whole by `s28_3_0148` too, and survives verbatim); no cell concerns a value-kind `this` capture. The disposal matrix's R10/C4 (runtime-managed captures in constructors) keeps its `exempt` verdict: a value copied into a cell carries no obligation.
- **Composition.** Lane `B1`'s other edits touch §28.3's adoption paragraph and state-match bullet 1 (#6574) and §28.11's operations table (#6566); lanes `B2`, `B3` and `E` name §5.1, §12.1, §22.6, §27.1.1, §8.3, §10.6, §19.x, §35.5, §2.7, §11.6, §30.x, stdlib §2.18, rationale §5.3/§11/§12 and the code-generation reference §3 — no shared paragraph with this proposal's anchors. The README row and the `DECLINED.md` rows anchor on the group-closing headings for sequential insertion.
- **History narration.** The normative edits state the current rule only; the story is in rationale §9.15's dated paragraph and the changelog.
