# Proposal 0102 — Small records: the §22.5 named-move lift, the redundant-marker decline, the allocator register entry, the A4/projections joint note, the FFI hand-to-native direction, and the frozen-class-trees pressure note

| | |
|---|---|
| **Status** | graduated-v0.260 — **superseded — graduated v0.260** (ruled under the owner's 2026-08-18 delegation; one train with 0100 and 0101; charter rulings C4/C2/C6/C8 — the §22.5 named-move lift landed with the surviving positional-only rule restated with its reason and §22.6's confessed asymmetry deleted with no positive restatement (§1.3's drafting note as recommended); the carrier sweep took **two unenumerated carriers** beyond §1.3's list — A.7's `MoveExpr` commentary ("positional only (§22.5)") and rationale §9.9's clause-identity parenthetical; the redundant-marker decline recorded at rationale §3.10, the allocator/arena entry at §18 (Q1 as recommended), the A4/projections joint note beside §18's ref-returning-indexers entry (its 0096 cite adapted to the landed v0.258), the FFI hand-to-native direction at §16.7, the frozen-class-trees pressure note at stdlib §4.19 + rationale §12.9; the one test flip sequenced behind #3056 in the v0.260 Migration ledger, its spelling and harness re-verified at HEAD) |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **spec** — §22.5 (the named-argument rule's transfer-marker clause), §22.6 (the confessed named-argument asymmetry **sentence**, deleted — §1.3's drafting note), Appendix A.4 (`Argument` production commentary). **rationale** — §3.10 (the C2 decline record), §17 or §18 (the allocator/arena entry — §9 Q1), §18's *Ref-returning indexers* entry (the A4/projections joint note), §16.7 (the FFI hand-to-native one-liner), §12.9 and stdlib §4.19 (the frozen-class-trees pressure note). **tests** — one flip in `tests/ownership/owned_marker_transfer_test.cpp` (§1.4). |
| **Written against** | spec/stdlib **v0.255** (`git show 5fa5a95b:docs/*.md`) |
| **Currency** | **v0.256** (HEAD `8f41e99d` — the defect train `4130d2de` plus its gate fixes `e8f5387a`): every quoted §22.5/§22.6/§22.3 text, A.4's `Argument` production and commentary, and the cited rationale/stdlib passages are unchanged (v0.256's A.4 delta is the `ExpressionPropertyDecl` readonly comment, elsewhere in the appendix). §4's joint note gains its 0096 cross-cite below. |
| **Origin** | Charter C2, C4, C6, C8 and the *small records riding any convenient train* list, ruled under the owner's 2026-08-18 delegation. |

*Reference convention: unprefixed §-references are language-spec references; rationale and stdlib references are marked. Every quotation is from the v0.255 snapshot, re-verified unchanged at v0.256. Bold inside a quotation is the source's own unless marked* (emphasis added).

---

## 1. The §22.5 named-move lift — adopt (C4)

### 1.1 The rule today

§22.5, quoted in full:

> Named arguments may appear in any order after positional arguments. **The named form is for plain-value arguments only:** a `ref`/`out` argument, and an argument whose root is the transfer marker (`move x`, §28.3), is passed positionally — `M(out var result := n)` has no reading and does not parse (Appendix A.4 `Argument`), and `M(item := move x)` is a compile error naming this rule, **exactly as its v0.245 spelling was** *(emphasis added)*.

The sentence's own last clause is the finding: the rule is a carryover, preserved verbatim across the v0.246 flip.

### 1.2 The grammar-vestige evidence

Appendix A.4, quoted:

```
Argument        ::= ArgModifiers ArgExpression
                  | (Identifier ":=")? ArgExpression
                              /* ref/out arguments and move-rooted arguments
                                 are positional-only (§22.5): the named form
                                 is for plain values. … */
ArgModifiers    ::= "ref"
                  | "out" "var"?
                  | "out" "using" "var"
                  | "out" "owned" "var"
                  | "scoped"
                              /* The bare "owned" alternative was deleted at
                                 v0.246 (proposal 0090): the call-site transfer
                                 marker is part of the argument EXPRESSION —
                                 `M(move x)` parses through the plain
                                 ArgExpression path as an identifier-operand
                                 MoveExpr (A.7; §22.3, §28.8.1). … */
```

Read the two alternatives against each other. **`ref`, `out`, and `scoped` inhabit the first alternative, which has no `(Identifier ":=")?` prefix** — for them, positional-only is a *grammar shape*: there is no production that could produce a named form. **`move x` inhabits the second alternative, which does have the prefix** — so `M(item := move x)` **already parses** at v0.255 and is rejected only by §22.5's semantic sentence.

Until v0.245 that sentence was doing real work: the marker was `ArgModifiers ::= … | "owned"`, so `M(item := owned x)` was ungrammatical for the same shape reason as `ref`/`out`, and §22.5's clause merely restated it. v0.246 deleted that alternative and moved the marker into the argument *expression* — and the sentence stayed, now prohibiting a form the grammar admits and for which exactly one reading exists. **The exclusion is a vestige of a deleted production.**

### 1.3 The lift

Delete the transfer-marker clause from §22.5's sentence; delete "and move-rooted arguments" from A.4's `Argument` commentary. The rule becomes:

> Named arguments may appear in any order after positional arguments. **A `ref`/`out`/`scoped` argument is positional-only:** those modifiers inhabit the `ArgModifiers` alternative of `Argument` (Appendix A.4), which has no named form — `M(out var result := n)` has no reading and does not parse, the `:=` of the binding form and the `:=` of the named form being the same token. A **transfer-marked** argument carries no modifier — the marker is part of the argument expression (§22.3, §28.8.1) — so `M(item := move x)` is an ordinary named argument whose expression happens to be a `MoveExpr`, and is legal.

**The surviving rule, restated with its reason** (the charter's requirement): `ref`, `out`, and `scoped` stay positional-only because they are **true argument modifiers** — grammar-level prefixes on the argument, not parts of its expression — and for `out`'s binding forms (`out var x`, `out using var x`, `out owned var x`) a named spelling would put `:=` in two roles on one argument (`M(result := out var x)` or `M(out var result := n)`), which is the collision A.4's comment records as having *"no reading"*. `scoped` shares the alternative and therefore the shape; §22.3 already records that `scoped` *"remains an argument modifier: it marks a lending mode, not an action, and the asymmetry is deliberate"*, so the modifier/expression line this lift draws is the line §22.3 already drew.

**What it deletes.** The confessed asymmetry is **one sentence** — the closing sentence of §22.6's overload-applicability paragraph (the four-clause applicability rule), not a paragraph of its own:

> One asymmetry is stated rather than hidden: a move-rooted argument is positional-only (§22.5), so **for a named owned binding a pair's owned member is unreachable through named-argument syntax** — a caller who both transfers a named binding and names arguments passes the marked argument positionally; fresh producers, being plain-value expressions, reach the owned member by name (`M(item := new T())` is legal and owned-classified).

Proposal 0091 §1 made the same confession at drafting time. The lift **deletes that sentence**: after it, a pair's owned member is reachable by name through both routes, and the "fresh producers can, named bindings cannot" split — which no reader could predict — disappears. *(This deletion is also the shape 0100 §5.2 leans on from the other side — the two proposals record the v0.246 one-spelling-per-site precedent for different questions and cite each other.)*

*Drafting note (formerly §9 Q2, demoted here because C4's ruling already decides the deletion).* The one open choice is whether a **positive restatement** replaces the deleted sentence — *"a pair's owned member is reachable by name and positionally alike."* **Recommended: no** — it is a sentence about the absence of a rule, and §22.6's applicability paragraph is already the spec's longest. If the drafter wants the reassurance recorded, it belongs in the changelog's migration note, not in the normative text.

### 1.4 Migration: zero, plus one test

**Zero source migration.** The lift is pure widening: every program legal at v0.255 is legal after it with the same meaning, and the newly-legal shape is one that is a compile error today. No spelling changes; no diagnostic's *referent* changes except that one error stops firing.

**One test flips**, with a sequencing caveat that must be honoured:

```
tests/ownership/owned_marker_transfer_test.cpp :: named_owned_argument_rejected_spec_a7
    "class C public method M(s: Sink) "
    "  var x := new T(); s.Add(item := owned x); "
    "end M; end C;"   → BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
```

The test is written in the **pre-v0.246 marker spelling**, and it runs on **`parseAndCheckOwnership`** — the static ownership-check harness, *not* `runJit` (the file imports `runJit` for other tests; this one never touches it), so **no harness port is involved** in the flip. [#3056](https://github.com/s8lap/bezel-lang/issues/3056)'s corpus rewrite has not landed at HEAD — the pre-v0.246 call-site spelling remains at low-hundreds scale across roughly a hundred test files (this rework's reconstruction counts **332 occurrences across 79 files**, methodology: `owned <identifier>` at an argument or RHS root — after `(`, `,`, or `:=` — in joined `.cpp` string literals, lambda-parameter modifier positions excluded; the earlier draft's 344/102 used a wider net — the exact figure is #3056's to own). So the disposition is: **after #3056**, this test's source respells to `s.Add(item := move x)` and its assertion **inverts** to an accept — a two-line edit, respell plus inversion, on the harness it already has. Landing the lift *before* #3056 would leave a test whose assertion is right for the wrong reason. The proposal's sequencing note is therefore: **behind #3056**, exactly as proposal 0091 §8 sequenced its own resolution work.

**New regression tests** (spec-cited, `runAot`): `M(item := move x)` accepted and consuming `x`; the same at a §22.6 owned/plain overload pair, selecting the owned member (the asymmetry §22.6 confesses, now closed); `M(item := move obj._f)` and `M(item := move coll[i])` accepted (the field and slot take-outs are also move-rooted arguments and inhabit the same alternative); `M(out var result := n)` still rejected; `M(x := scoped s)` still rejected; and a named-plus-positional mixed call with a marked named argument, pinning the ordering rule's independence from the marker.

## 2. The redundant-marker asymmetry — decline, recorded (C2)

**The question.** The declaration side tolerates a redundant `owned`: §28.1 admits the marker on a **move-only value type** as *"admitted-but-redundant (the type's own move semantics already transfer the obligation)"*, and stdlib §6.8's `Domain.Spawn(...): owned Task` spells the marker on an announced return — stdlib §1: where a signature does spell `owned` on such a return (`GetEnumerator`, `Domain.Spawn`), *"it states the same contract with emphasis"*. The use side does not: `return move x;` under an `owned` return, `throw move x`, `_ := move x;`, and a marked store into an owning slot are all errors. The reviews' **option (c)** was to admit the use-site redundancy for symmetry — "every consume says `move`", greppable.

**Ruled: decline.** The markers stay errors at announced targets.

**The ground, recorded for rationale §3.10.**

1. **It is an owner-ruled precedent of this same programme.** Proposal 0090's **Q2** put exactly this choice — *rejected-as-redundant* versus *admitted-redundant* — and the owner ruled rejected-as-redundant at v0.246. The changelog records the ruling's cost being paid in the same breath: a fifth marker family, *"the marker on an owned-slot/`set(owned)` store — `arr[0] := owned b;`, `d["k"] := owned b;` — **compiled at v0.245** … and is newly rejected under Q2's announced-target rule; the migration is to **drop the marker**"* — three corpus lines, named by file and line number. A ruling that cost three lines to enforce four revisions ago is not reopened by a symmetry argument that brings no new evidence.
2. **The two sides are different things, and the line is nameable.** Declaration-side `owned` on a move-only type or a `Task`-family return is **type-level documentation**: it is written once, at the declaration, it is read by everyone who reads the signature, and it says something true about the slot that the type also says. Use-site marker redundancy is **per-expression spelling variance**: it is written per call, read by whoever reads that line, and it introduces two spellings for one site. §22.3's existing discipline is one spelling per site (*"writing it on a borrow, a fresh producer, or a non-`owned` parameter position is likewise an error"*), and v0.246's whole reading-model argument was that `owned` should mean *state* at declarations and `move` should mean *action* at sites. Admitting redundant action markers re-introduces the both-ways surface on the action side immediately after the language spent a 633-line migration cleaning it up.
3. **The grep argument is answered by the announcement.** "Every consume says `move`" is not achievable anyway: eight announced positions consume without a marker by design (K3.13's announcement set — announced targets, `throw`, the discard, owning `yield return`, Pattern B `await`, the reassignment displacement, move-only stores, the explicit `Dispose()`), and admitting the marker at *some* of them makes the grep less reliable rather than more, because a `move`-less consume no longer implies "not a consume" and a `move`-ful one no longer implies "a named-location consume".

**The record, and what it does not foreclose.** §3.10 gains one paragraph stating the question, the split (type-level documentation versus per-expression spelling variance), the v0.246 Q2 precedent, and the decline. It does **not** foreclose the reverse direction — tightening the declaration side by rejecting redundant `owned` on move-only types and `Task`-family returns — which is a separate question with its own costs (§28.1's per-instantiation erasure makes the marker's redundancy substitution-dependent, and stdlib §6.8's explicit spellings are load-bearing documentation) and is not raised here.

## 3. The allocator / arena register entry — adopt (C6)

**Drafted entry**, for rationale §18 (*Things deferred*), in that section's register:

> - **User-facing allocators and arena allocation.** A mechanism by which a caller supplies the allocator a callee's allocations draw from — an arena or bump allocator freed as a unit, a pool, a tracking allocator — as Zig's `std.mem.Allocator` parameter convention, C++'s allocator template parameters, or Rust's `allocator_api` provide. **Not in this version, and the reason is that Bezel's memory model already collects most of the payoff without the mechanism.** Allocation strategy is **implementation-defined**: nothing in this specification says where a `new` expression's storage comes from, and nothing observes it. Because ownership is tracked at compile time and every owned value's disposal point is *statically known* ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows), [§28.7](Bezel_Language_Spec.md#sec-reassigning-owned-fields)), an implementation is free to region-allocate a set of values whose lifetimes it can prove coincide and free them as a unit — arena and batch-free become an **implementation optimization**, invisible in the language, rather than an API the programmer routes by hand. What a *user-facing* allocator would cost is the reason it is not here: an allocator parameter infects every signature that allocates, transitively, so it reaches most of the standard library and most user code — the same blast-radius argument that declined a mandatory `owned` marker on task returns ([§12.10](#sec-calls-mint-storage-lends)) — and it crosses `owned`-transfer boundaries, where a value allocated from one arena is transferred to a holder whose lifetime the arena does not bound, which is a second, *dynamic* lifetime discipline running beside the static one and answerable only by the programmer. **Reopening condition:** a real embedded or hard-realtime adopter whose target forbids a general allocator, or measured allocation-pressure evidence from a running Bezel service that implementation-side region allocation cannot address. Absent one of those, the mechanism buys placement control the language deliberately does not otherwise expose.

**Placement (§9 Q1).** Drafted for **§18** (*Things deferred* — *"Not in this version; the design is open to them in future revisions"*), because the ruling carries a reopening condition and therefore describes an open design, not a rejected one. §17 (*Things explicitly rejected*) is the alternative and is not absurd — §17.10 carries a re-addition condition too — but §17's entries are things Bezel has decided **against** on principle (truthiness, `goto`, member hiding, `params`), and "we may add this if an embedded adopter arrives" is not that.

## 4. The A4 / projections joint note — keep A4, record the reopening shape (C8)

**A4 is kept.** The register states it as *"Accessor read faces are lend-only: property and indexer types carry no `owned`, and a getter may not return a fresh owned value; `set(owned)` is the announced write face (K3.1)"*, with the recorded reason *"Signature predictability: every accessor read classifies as a borrow with no marker position available; factories are methods."* The review's verdict stands: the accessor model **is** projections in all but name — a property or indexer read *projects* a view of the receiver's state, with the projection's mode fixed to "borrow" and the write face spelled separately as `set(owned)`. What A4 forbids is not projection; it is a *minting* accessor.

**What would reopen it, recorded as a joint note.** A4's recorded reason is that *no marker position is available* on an accessor read face. That is a fact about the **current accessor grammar**, not a law: a **projection form in the Hylo mould** — a `subscript`-style member that *yields* a projection rather than *returns* a value, with the yield's mode part of the declaration — manufactures exactly the missing position. The mode on the yield **is** the marker, so a projecting member could declare itself a lending projection or a transferring one, and A4's premise would no longer hold.

**Why it is a joint note and not an entry of its own.** That same form is what rationale §18's *Ref-returning indexers* entry needs:

> - **Ref-returning indexers.** Would let `dict[key]++` be one operation instead of two. The language currently has no ref returns ([§22.14](Bezel_Language_Spec.md#sec-no-params-no-ref-returns) of the spec); adding them is deferred.

A mutable in-place projection *is* a ref-returning indexer under another name, and a Hylo-style `subscript` is the design that delivers both. So the note lands **beside** that entry:

> *(Joint note with axiom [A4](Bezel_Language_Spec.md#sec-axiom-register), added under the 2026-08-18 review programme.)* A4 keeps accessor read faces lend-only, on the recorded ground that an accessor read has **no marker position available**. A **projection form** — a `subscript`-style member that yields a projection whose mode is declared at the yield, as in Hylo — manufactures that position, and is the same mechanism this entry's ref-returning indexers would need: a mutable in-place projection is a ref-returning indexer, and a transferring projection is the minting accessor A4 forbids. **The two therefore reopen together or not at all**: a proposal for either must address both, because a design that delivers one delivers the other's premise for free, and adopting one while declining the other would leave the language with a projection mechanism it refuses to use at half its positions. A third party rides on the same hinge: a transferring projection is a **minting accessor face**, and the accessor face's guaranteed-lend property is the load-bearing complement of the calls-mint/storage-lends doctrine that proposal 0096 completes (its §4 makes A4 load-bearing rather than incidental) — so a projection proposal must also confront the storage-lends half of that doctrine, not only A4's letter. Neither is proposed; the shape is recorded so a future proposal starts from the joint question rather than from the narrower one.

## 5. The FFI hand-to-native direction — one line beside rationale §16.7

Rationale §16.7 declines `ForeignHandle<T>` (the `GCHandle` analog, object↔`IntPtr`) on four recorded grounds, of which the second names the alternative: *"the liveness discipline it offers is **weaker** than the registry idiom's — a stale handle resolve is silent undefined behavior, a stale registry token is a graceful miss under a lock, so for retained callbacks (the only tier that wants it) the idiom **strictly dominates** on safety at the cost of one dictionary lookup"* *(emphasis added)*.

What the record does not say in one place is what a programmer with a Bezel object and a native library that wants to hold it should *do*. The one-liner, appended to that paragraph:

> **The recorded direction for handing a Bezel object to native code**, should the question recur: for the **callback** tier — the only tier with a live use case — the shipped mechanism is the `env` user-data marker (spec [§32.8](Bezel_Language_Spec.md#sec-callbacks)), which makes the callback parameter capture-admitting and lets the runtime recover the function value from the opaque `env` slot without the programmer ever seeing, minting, or storing an object address; for the **retained-object** tier, the direction is the **registry token** — an integer key into a Bezel-side registry consulted under a lock, whose stale resolve is a graceful miss rather than undefined behavior. Both preserve the no-object-address invariant of spec [§32.12](Bezel_Language_Spec.md#sec-foreign-memory), which is `ForeignHandle<T>`'s third recorded ground; a proposal that wants the handle must first say what it does with that invariant.

## 6. The frozen-class-trees pressure note

**The recorded deferral.** Cross-domain immutable sharing today reaches exactly one shape: `FrozenArray<T>` (stdlib §4.19), *"a **deeply immutable, fixed-size array** whose contents can never change after construction — the standard library's zero-copy shared-read primitive"*, which is the first member of the `Shared`-payload synchronized set *"admitted by **deep immutability** rather than synchronization machinery"*. Rationale §12.9 records the design as **MOVE and FREEZE**, with FREEZE instantiated by `FrozenArray<T>` and by `string`.

**The pressure, named.** stdlib §6.11's (`Locked<T>` / `RwLocked<T>`) *Taking data out* paragraph already records where it bites: a registry that must hand out live *references* to class-typed entries *"needs the entry type itself to be synchronized or deeply immutable (`Shared<T>` payloads are spec §28.13's derived set …; **frozen class trees are the recorded §4.19 deferral**) — a recorded gap, not an idiom this section can supply."* So the language has a shape it can express for **arrays of anything** and cannot express for **a tree of classes**: a configuration object, a parsed document, a routing table, a compiled schema — the canonical build-once-then-read-from-every-worker values.

**The note** (appended to stdlib §4.19 and cross-referenced from rationale §12.9):

> **Recorded pressure (2026-08-18 review programme; no change now).** `FrozenArray<T>` freezes a *buffer*; there is no form that freezes a **class tree** — a build-once object graph whose deep immutability would admit it to the same §28.13 set on the same ground. That is the likely **first user pressure** on this deferral: the values services actually want to share across domains read-only are configurations, parsed documents, routing tables, and compiled schemas — object graphs, not arrays — and today each must be either re-synthesised per domain, wrapped in a lock, or flattened into arrays of value records. Nothing changes here; the note exists so the next person to meet the gap finds it recorded rather than rediscovering it, and so that a proposal starts from the two questions the shape actually raises: **what makes a tree provably deeply immutable** (a `frozen`/`immutable` type property that propagates to every field type, checkable by the §3.6 derived-property machinery, versus a construction-phase-then-seal protocol that is a runtime state), and **how the borrow rules read a frozen tree's interior** (a borrow of a frozen node has no source-liveness constraint against mutation, but still has one against disposal — which is exactly `FrozenArray<T>`'s own `static owned readonly` singleton answer, scaled up).

## 7. Migration and compatibility (changelog house categories)

- **Newly accepted:** `M(item := move x)` and every named-argument form whose expression is move-rooted (§1). Nothing else in the proposal changes any program.
- **Newly rejected:** nothing.
- **Deleted:** §22.5's transfer-marker clause; §22.6's confessed-asymmetry sentence (§1.3); A.4's `Argument` commentary clause.
- **Doc-only:** §2 (rationale §3.10), §3 (rationale §18), §4 (rationale §18 + A4 joint note), §5 (rationale §16.7), §6 (stdlib §4.19 + rationale §12.9).
- **Test delta:** one flip (§1.4), sequenced behind [#3056](https://github.com/s8lap/bezel-lang/issues/3056).
- **No axiom is added, retired, narrowed, or widened.** A4 is explicitly **kept**; §4 records a reopening shape, which spends nothing.

## 8. Implementation sketch

- **§1 (the only compiler-facing item):** sema deletes one check — the named-argument rejection for move-rooted arguments — and the diagnostic that names §22.5. Overload resolution needs **no** change: §22.6's four-clause applicability rule already classifies a move-rooted argument as an owned production *"per argument position"*, and it never consulted the argument's positional-versus-named form. Parser: none (the form already parses). Size: small, and the review burden is the test matrix of §1.4 rather than the change.
- **§2–§6:** doc-only; no compiler work, no tests.

## 9. Drafting seams for the graduation train

- **Q1 — where the allocator entry lives.** §18 (recommended, §3's reasoning) or §17. Whichever is chosen, the reopening condition must survive the move; an entry in §17 without it would read as a flat rejection, which is not the ruling.
*(The former Q2 — whether §22.6's confessed-asymmetry sentence is deleted or amended — is demoted to §1.3's drafting note: C4's ruling decides the deletion; the only open choice is the positive restatement, recommended against.)*
