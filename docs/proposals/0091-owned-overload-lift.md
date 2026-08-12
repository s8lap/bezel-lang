# Proposal 0091 — Lifting the §22.6 owned-overload ban: ownership-mode applicability, and the retirement of the `Owned`-suffix dual naming

| | |
|---|---|
| **Status** | under-review — drafted 2026-08-12 (owner-directed, the Train E follow-on questions) |
| **Tracking issue** | [#3086](https://github.com/s8lap/bezel-lang/issues/3086) |
| **Target module(s)** | **spec** — §22.6 (the ordinary-parameter ban paragraph is replaced by the ownership-mode applicability rule; the `out owned` and `scoped` paragraphs stand, re-grounded; the *Practical pattern* section keeps its `out owned` Try teaching and loses the `AppendOwned` remedy sentence), §22.3 (one cross-reference sentence), §11.3 (the clash rule reads the new distinguishability), §30.10 (the helper list respells with the stdlib merge), Appendix C.1 (the "`owned` never distinguishes" invariant row narrows), Appendix A — **no grammar delta** (the marker is already part of the argument expression; the lift is entirely semantic). **stdlib** — §6.7 (`FromOwnedResult` → the `FromResult` owned overload; `WhenAllOwned`/`WhenEachOwned` → `WhenAll`/`WhenEach` overloads), §1 (one naming-convention sentence). **rationale** — §3.10 gains the follow-on's resolution record. **guides** — Guide 4's two `*Owned` helper mentions respell at graduation (that file is owned by the in-flight v0.249 train; sequencing note in §10). |
| **Written against** | spec/stdlib v0.248 |
| **Origin** | E-f1, the first of the two owner-confirmed open questions recorded at the v0.246 graduation of proposal 0090 (changelog v0.246; rationale §3.10's reversal record; §22.6's own in-text note) — directed to a full proposal by the owner 2026-08-12 ("Please work to address all"). |

*Reference convention: unprefixed §-references are language spec references; stdlib and rationale references are marked; K/A tags are §28.3's kernel ids.*

---

## 0. Thesis

§22.6 bans overloading on ordinary-parameter `owned`: `M(t: T)` beside `M(owned t: T)` is a compile error, and the prescribed remedy is dual naming — "`Append` versus `AppendOwned`, or the `FromResult` / `FromOwnedResult` pattern." The ban's recorded reason is a **visibility** claim (§22.6, v0.248): "the call-site syntax (`M(value)`) does not visibly distinguish the two forms, so permitting them as overloads would force the caller to know the resolution outcome to write a correct ownership transfer."

That premise is gone. Since v0.169 a named owned binding passed to an `owned` parameter carries a call-site marker, and since v0.246 the marker is the unified verb: `M(move x)` transfers, `M(x)` borrows — visibly distinct at every call site, with no declaration-token ambiguity left. §22.6's own text concedes it ("the call-site marker `M(move x)` … satisf[ies] this paragraph's own carve-out criterion"), rationale §3.10 concedes it twice (the v0.169 record and the v0.246 reversal record), and the v0.246 changelog records E-f1 as the open question. What kept the ban standing was never a reason — it was scope: "lifting it drags stdlib renames and resolution rules with it." This proposal is those renames and those resolution rules.

**Proposed:** two same-name members may differ by `owned` on ordinary parameters. Resolution needs **no ranking change**: the argument's ownership classification is part of **applicability**, so at most one member of such a pair survives at any argument, and the caller's spelling — not the resolution outcome — decides which:

```bezel
public class Registry
  public method Append(item: Widget);         // borrows; caller keeps ownership
  public method Append(owned item: Widget);   // takes; caller's binding is consumed
end Registry;

r.Append(w);            // borrow — the plain overload; w untouched
r.Append(move w);       // transfer — the owned overload; w moved-from (§22.3, K3.3)
r.Append(new Widget()); // fresh producer: an owned production — the owned overload
```

The stdlib's ban-bred dual names merge (`FromOwnedResult` → `FromResult`; the convention-bred `WhenAllOwned`/`WhenEachOwned` → `WhenAll`/`WhenEach`), and the `out owned` and `scoped` halves of the ban deliberately **stand** (§3). The `TryX` acquisition names all stay: they are operation names, not ownership suffixes (§4, category iv–v).

## 1. The exact rule

**Amendment sketch (§22.6, replacing the ordinary-parameter ban paragraph):**

> Ordinary-parameter `owned` joins the distinguishability list: overloads may differ in number, types, the `ref`/`out` modifiers, **or the presence of `owned` on an ordinary parameter**. At an argument position where two applicable candidates differ only by ordinary `owned`, the argument expression's **ownership classification** ([K1](Bezel_Language_Spec.md#sec-kernel-k1); the §28.3 producers table — a property of the expression's form alone, never of the candidate under trial) completes applicability: an **owned production** — a move-rooted argument (`move x`, §22.3's call-site marker), a fresh producer, an `owned`-returning call, a `move obj._f` result — is applicable only to the **`owned`** candidate; a **borrow** is applicable only to the **plain** candidate; a **`null` literal** (polymorphic-null, K1 — no value to own, compatible with either) is applicable to both, and the call falls to the ordinary ambiguity rule. The classification is consulted only where candidates differ by the marker; a single declared member keeps today's compatibility rules unchanged. **No ranking change**: the marker participates in applicability, never in the better-than relation — at most one member of a pair survives applicability at any non-null argument, so the exact-match ranking, the defaulted-arity tie-break, and the non-generic preference operate on the surviving set exactly as written.
>
> **Declaration guard (the #1873 stance, narrowed).** A pair differing only by ordinary `owned` is declarable exactly when, at every differing position, the declared type — nullability stripped — is a class type, an interface type, or a type parameter whose constraints confine every substitution to class or interface kind (`where T: class`). At any other position the marker can erase per instantiation (§28.1), collapsing the pair to identical signatures, and the declaration-site clash error of §11.3 stands — reported at the second declaration, uniformly, exactly as #1873 ruled for the whole ban.

Verified against §22.6's actual text: the applicability facts this rule keys on are the compatibility rules the spec **already states** — a borrow may not feed an `owned` parameter (K3.3: only owned productions discharge there), and `M(move x)` at a non-`owned` position is an error naming the borrow default (§22.3; v0.246 Q6). The lift re-uses them as applicability filters, the same way `ref`/`out` mismatches already filter; the one genuinely new decision is fresh-producer routing (§2 consequence 2). And the v0.246 named-argument seam ruling simplifies binding exactly as anticipated: a move-rooted argument is **positional-only** (§22.5 — `M(item := move x)` is a compile error), so named-argument reordering never has to reconcile with a marker; classification-keyed applicability reads named plain-value arguments (`M(item := new T())` is legal and owned-classified) with no special case.

## 2. Worked consequences

1. **The recorded harm inverts.** The v0.169 reason was that the caller would need the resolution outcome to write a correct transfer. Under this rule the dependency runs the other way, always: the resolution outcome is a function of what the caller wrote. `Append(x)` cannot consume `x` (an unmarked read of a named binding is a borrow, K3.13 — the no-silent-consume rule is untouched and is what makes the rule sound); `Append(move x)` cannot borrow. There is no call whose meaning the caller must look up.
2. **Fresh producers select the transfer overload.** A fresh production is owned (K1.5); within a pair it is applicable to the `owned` candidate only. This is the useful reading — the alternative (borrow of an unbound temporary, K8) hands the callee a reference that dies at the statement boundary, which at a storing callee is exactly the #1813 trap shape proposal 0081 lints. A caller who genuinely wants a temporary *inspected* but not kept binds it first (`using var t := new Widget(); r.Append(t);`) — the standard remedy, unchanged. Purely additive: where only one member is declared, today's rules (including fresh-at-plain call-bounded borrows) are untouched.
3. **`null` at a pair is ambiguous, loudly.** A nullable-typed pair position fed a bare `null` keeps both candidates applicable and the existing ambiguity error fires, citing both — §22.6's decline-to-guess stance, no new machinery. (Realistically no stdlib pair takes a nullable marked position; §9 Q4 offers the prefer-plain alternative if the owner wants one.)
4. **Interface conformance is per-signature, already marker-aware.** Signature matching reads `owned` today (§28.1's erasure paragraph names §11.2/§12.1; §2.8.6 classifies an `owned` flip as breaking). An interface may declare both members of a pair; a class implements each separately (bodies may delegate); an override repeats the modifier as always (§12). The diamond rule's "same signature" reads marker-aware and is unchanged. A *generic* interface pair falls under the declaration guard like any pair.
5. **Artifact interface: zero new carriage.** §2.8.2 clause 1 already carries ownership modifiers per member; a pair is two member records. Delta classification (§2.8.6): *adding* the `owned` member beside an existing plain one is a preserving addition; *flipping* a parameter plain↔owned remains breaking. No new category.
6. **Constructors participate.** §22.6's constructor sentence applies unchanged: `new StreamReader(inner)` / `new StreamReader(move inner)` pairs become declarable — the shape proposal 0006's stream family already reaches for.
7. **Method references resolve by function type.** `(owned T) -> ()` and `(T) -> ()` are distinct function types (§27.1), so an overloaded reference to a pair resolves by target type under §27.6's existing rules; without a disambiguating target it is the existing cited-candidates error.
8. **Defaults.** A pair whose marked position is defaulted on both sides makes an argument-omitting call fill defaults on both — the existing two-defaults-filled ambiguity stance applies (§22.6). Stdlib pairs should not default the marked position (one sentence in stdlib §1).

## 3. What deliberately stands (the un-lifted halves, argued)

- **`out owned` vs `out` stays banned.** The lifted half keys on what the caller *passes* — an expression with a K1 classification. The `out` half would key on how the caller *binds* a result (`out var` vs `out using var`), making `out` the only position where resolution consults a binding form; §22.6's recorded costs (a dedicated `out _` ambiguity remedy — the discard is compatible with both members of such a pair) are unchanged by v0.246, which touched no `out` spelling (`out owned var x` stays declarative — 0090 Q5). And the corpus adds a semantic reason the original text understates: every shipped `out owned`/"pair" is **operation-distinct**, not ownership-distinct — `TryGetValue` leaves the entry where `TryPopValue` removes it; `TryPeek` leaves where `TryPop`/`TryDequeue` remove. Merging them would key a *mutation of the receiver* to how the caller binds an out-result. Guide 2 teaches the naming as load-bearing ("`Get`/`Peek` borrow; `Pop`/`Drain`/`Take` transfer — the name alone tells you whether you now owe a disposal"); that pedagogy survives intact.
- **`scoped` stays banned.** The carve-out criterion is not satisfiable there: a function-typed `scoped` argument carries **no call-site marker at all** (admission is capture-based, §28.14), and lifting only the class-typed `M(scoped x)` face would make `scoped` resolution-significant for one operand kind and invisible for the other — a split inside one modifier. §22.6's "no resolution-by-call-site-form rule" sentence retires (the lifted half is precisely such a rule, for `owned`, where the form is total); the `scoped` rejection re-grounds on the markerless function-typed case.

## 4. The stdlib program (what merges, what stays, and why)

Enumerated by category, counted at v0.248 (occurrences: stdlib doc / spec / tests):

**(i) The ban-forced pair — merges.** `Task.FromResult<T>(value: T): Task<T>` (non-class `T`, compiler-special-cased) beside `Task.FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class` (stdlib §6.7; FromOwnedResult: 2 / 6 / 41). Merged: the second becomes `FromResult<T>(owned value: T): OwningTask<T> where T: class`. The pair is doubly separated — the constraint split already makes at most one candidate applicable per instantiation; the marker rule is the belt on those braces — and the reader-facing gain is one name: `Task.FromResult(42)`, `Task.FromResult(move conn)`, `Task.FromResult(new Config())`.

**(ii) Convention-bred dual names, already type-distinguished — merge recommended.** `WhenAllOwned` (12 / 5 / 38, + 2 Guide 4 lines shared with the next) and `WhenEachOwned` (4 / 0 / 23), both stdlib §6.7. Their parameter types already differ from their duals' (`Array<Task<T>>` vs `owned OwningArray<OwningTask<T>>`), so these pairs were declarable as overloads even under the ban — the dual naming is the ban's *culture*, not its letter (as is 0006's `MemoryStream.FromBytes` pair, type-distinguished and already same-named in that accepted proposal). Merged: `Task.WhenAll(tasks)` / `await Task.WhenAll(move tasks)`, `Task.WhenEach(tasks)` / `Task.WhenEach(move tasks)` — the call-site marker announces the consuming form, which is the teaching the current names duplicate.

**(iii) Return-side `Owned` names — do NOT merge.** `Json.ParseOwned<T>` / `Binary.ParseOwned<T>` (15 / 0 / 23) are distinguished at the **return**, where no call-site marker exists and no overloading-by-return exists; the name is the §28.3 announcement discipline doing its job at a static producer, and the facade naming was owner-ruled at v0.232. (The §28.1 observation that a single `Parse<T>(): owned T` would erase per instantiation is deliberately *not* pursued — the value/class facade split was ruled for its diagnostic quality; reopening it is not this proposal.)

**(iv) The `out owned` Try pairs — stay, with the un-lifted half.** `TryGetValue`/`TryPopValue` (OwningDictionary §4.9; 10 / 3 test-heavy), `TryPeek`/`TryPop` and `TryPeek`/`TryDequeue` (§4.11), `TryTake` (§4.10), `TryPopFirst`/`TryPopLast` (§4.5/§4.7), `Channel<T>.TryReceive` (§6.9). Operation-distinct per §3; no rename.

**(v) The Try tiers that were never ownership duals — untouched, stated so this proposal does not over-reach.** The Try-parse tier (`TryParse` on the §9 primitives, `Guid`/`DateTime` family, `Convert.TryTo*`), shape (b) acquisitions (`Mutex.TryAcquire`, `Weak<T>.TryUpgrade`), shape (d) attempts (`TryAdd`, `TrySet*`, `TryWrite*`), and the `*OrNull` tier signal *decline or absence* (stdlib §1's Try-convention bullet); no borrowing/owning same-operation dual exists behind any of them. They are out of scope by construction.

**§22.6's own text.** The *Practical pattern* section survives — it teaches the `out owned` Try convention, which stands — and the ordinary-parameter remedy sentence ("`Append` versus `AppendOwned` …") retires. `AppendOwned` was never API: 0 stdlib, 0 tests; its three total occurrences (spec 1, rationale 2) are all inside the ban's own records.

## 5. Corpus evidence

- **The premise shift is total.** Every ordinary-`owned` argument in the corpus is marker-bearing or fresh: the v0.246 flip left ~184 `M(owned x)` / `:= owned x` test lines pending their mechanical respell to `move` under #3056 (plus 5 `case owned` scrutinees) — i.e., the marker discipline the resolution rule reads is already universal; only its verb is mid-migration.
- **The ban's regression tests** (tests/lit/sema/): `sema_overload__owned_only_difference_diagnoses.bzl` **flips** to an admitted-pair test; `sema_overload__owned_generic_typeparam_collapse_1873.bzl` **flips** to the narrowed declaration guard (its recorded interpretation — rejected at declaration, not per instantiation — is preserved, now scoped to erasure-capable positions); `sema_overload__scoped_only_difference_diagnoses.bzl`, `sema_overload__out_vs_out_owned_clashes.bzl`, and `sema_overload__distinct_names_out_vs_out_owned_ok.bzl` **stand** (the un-lifted halves).
- **The rename surface** (category i+ii): ~102 test lines + 18 stdlib lines + 11 spec lines + 2 Guide 4 lines, every one a single-token name substitution; no call-site shape changes (the argument spellings are already marker-correct).
- **No pair exists anywhere** — the ban made pairs undeclarable, so the lift changes the meaning of zero existing declarations and zero existing calls.

## 6. Migration and compatibility (changelog categories)

- **Newly defined:** pair declarability; the ownership-mode applicability clause; the erasable-position declaration guard.
- **Newly rejected:** nothing that exists — the guard rejects a subset of declarations that are all rejected today by the wider ban.
- **Renames (breaking; zero users; hard, no alias — the 0090 Q3 precedent):** `FromOwnedResult` → `FromResult`, `WhenAllOwned` → `WhenAll`, `WhenEachOwned` → `WhenEach`; the old names leave the stdlib in the same revision.
- **Nothing flips:** no existing program changes meaning; single-member call sites resolve exactly as today.
- **Interface carriage:** none new (§2 consequence 5).

## 7. Alternatives considered

1. **Status quo.** Declined: the ban's recorded reason is deleted from under it; keeping it means §3.10 records "re-examined and kept" with no surviving reason beyond migration cost — a timing argument at its weakest point (pre-users) and weakening every revision the dual-name surface grows.
2. **Admit-but-don't-rename** (the lift without the stdlib merge). Viable as staging; declined as an end state: the tax the ban levied was the shipped vocabulary, and the rule change alone leaves it collected while removing the rule that justified it. Offered as Q6's fallback.
3. **Marker-as-preference** (both candidates applicable; the marked form *preferred*). Declined: with both applicable, an unmarked call at a pair would silently select — re-creating exactly the resolution-outcome-knowledge harm the v0.169 record named. Applicability keeps every selection derivable from the call site's spelling.
4. **Purely syntactic applicability** (move-rooted → owned; everything else, fresh producers included → plain). Declined: it inverts K1.5 (fresh productions *are* owned) and routes fresh arguments into the borrow-of-temporary reading — useless at best, the #1813 shape at worst (§2 consequence 2).
5. **Mandatory marker on fresh producers at pairs** (making applicability purely syntactic the other way). Declined: breaches K3.13's fresh-producers-need-no-marker principle and the v0.246 Q2 one-spelling-per-site discipline for a symmetry no reader needs.

## 8. Implementation sketch

- **sema** (overload resolution): the applicability filter consults the argument's producers-table classification — position-independent by K1, already computed for diagnosis; the §11.3 clash check narrows to the guard + the standing halves; the compiler-known `FromResult` class-`T` special-casing merges with the new overload's constraint path; diagnostics: the indistinguishable-overload message re-scopes, the ambiguity message gains the null-at-pair citation.
- **parser:** nothing — no grammar delta.
- **stdlib/tests:** the category (i)+(ii) renames as one mechanical commit; spec-cited regression tests per `tests/README.md`: pair resolution at each argument kind (marked, borrow, fresh, null), the declaration guard (concrete, `where T: class`, and rejected-erasable), interface-pair conformance, constructor pair, method-reference-by-target-type, and the flipped/standing lit tests of §5.
- **Sequencing:** after #3056's marker respell lands (resolution reads the very marker the corpus is still adopting); the Guide 4 respell rides graduation, sequenced with the v0.249 train that owns that file.
- **Size:** moderate — one resolution-phase filter, one guard, ~135 mechanical rename lines.

## 9. Questions for the owner

- **Q1** — lift the ordinary-parameter half via ownership-mode applicability (§1–§2)? Recommended.
- **Q2** — keep the `out owned` and `scoped` halves as argued (§3)? Recommended; the alternative (a symmetric `out` lift keyed to binding forms) re-opens the recorded `out _` ambiguity cost for pairs the stdlib would not even use (its take-forms mutate).
- **Q3** — fresh-producer routing to the `owned` candidate (§2 consequence 2)? Recommended; the alternative (ambiguity error demanding `move`-or-bind) is stricter but taxes the dominant construction idiom (`Append(new Widget())`).
- **Q4** — `null` literal at a pair: ordinary ambiguity error (recommended, zero new machinery) or prefer-the-plain-candidate (silent, and wrong whenever the callee's owned form has distinct semantics)?
- **Q5** — the erasable-position declaration guard (recommended — #1873's declaration-site stance, narrowed) or per-instantiation collapse errors (later, noisier, and precisely what #1873's record declined)?
- **Q6** — the stdlib merge program: categories (i)+(ii) (recommended) / (i) only / admit-but-don't-rename?

## 10. Interactions

- **Proposal 0092 (sibling, this train):** both extend the marker's reach; the surfaces are disjoint (argument roots here; pattern slots there) and the joint reading-model claim stays uniform — `move`'s operand is always the location being emptied.
- **#3056 (v0.246 impl catch-up):** hard sequencing, §8 — the respell must land before resolution keys on the marker; docs-first graduation is unaffected.
- **Proposal 0006 (accepted, ungraduated):** its `MemoryStream.FromBytes` same-name pair is type-distinguished (legal either way) and its `StreamReader` owned-wrapping constructors become expressible as pairs under §2 consequence 6; 0006 adopts the merged convention at its own landing — no edit now.
- **Proposal 0081:** §2 consequence 2 removes the fresh-at-borrowing-store shape at paired positions by construction; the lint keeps covering unpaired ones.
- **Rationale §3.10:** the E-f1 follow-on resolves; the section gains the lift record at graduation (the v0.169 and v0.246 entries stay pinned).
- **§2.8.6:** no new delta category (§2 consequence 5).
- **v0.249 train (in flight):** this proposal cites v0.248 texts only and touches no normative file; the one contact surface (Guide 4's two rename lines) is graduation-time work, flagged in §8.
