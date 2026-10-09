# Proposal 0157 — The trust-admission report names a transfer of a collection-held source

| | |
|---|---|
| **Status** | graduated-v0.333 — superseded — graduated v0.333 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option E, the trust-admission report gains one entry, a transfer of an owned binding that an admitted store into a collection slot has made the source of an element, reported at the transfer, and the zero-report claim's exclusions name both flag-backstopped kills; no verdict changes; critique round 1 applied, 3 findings resolved, shared with the #5717 clarification (3 more); that round withdrew the over-report ground "a collection moved to a wider binding", a store §28.11 already rejects, for retention not being body-local, and restated the code-generation reference's report sentence to name every transfer of a local or `owned` parameter; landed with the v0.333 changelog entry) |
| **Tracking issue** | [#5713](https://github.com/s8lap/bezel-lang/issues/5713) (the defect); the report mode's compiler catch-up is [#4605](https://github.com/s8lap/bezel-lang/issues/4605) |
| **Target module(s)** | **spec** — §28.3 *Diagnostics* (*The trust-admission report*: one register entry, one exclusion clause), §28.11 *Storing a borrow* (one sentence); **rationale** §5.31 (one paragraph); **code-generation reference** §4.6's opt-in report sentence |
| **Written against** | spec **v0.332** at `6a3f45f` (main). |
| **Origin** | The v0.332 train's lane `U2` critic finding N7 (`scratch/v0332/U2.critic.md`), filed as #5713; the borrow-flow matrix's third run (`scratch/matrix/r3/`), cells R2/C3, R6/C3·route, R6/C5 and R6/C10·coll, each of which records #5713 as a report gap and not a missing bound. |

*Reference convention: unprefixed §-references are language-spec references at `6a3f45f` (v0.332); K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. The program below uses only members the stdlib companion declares.*

---

## 0. Thesis

§28.3's trust-admission report lists the sites where an admission rests on a K4.3-recorded trust clause and then claims: "**A compilation unit that emits no report under the mode carries no recorded trust clause**: every borrow it holds in a field, a collection slot or a pending frame is statically bounded ([K2.3]) or absent." Its collection entry reaches only "such a borrow" — "a parameter-, receiver- or field-path-sourced borrow". §28.11's collection-held clause is wider: "whether each admitted element's source actually outlives the collection's use of it is the programmer's obligation … the obligation carries the borrowed-field rule's **domain clause** … for each admitted element's source object while the collection retains the borrow", and since v0.332 the same obligation reaches a value that holds borrows ("A value that holds borrows copied into a collection under §9.7's scope comparison carries the same obligation for each source its carried borrows name"). An element whose source is a local of the same body is admitted by the scope comparison, which bounds it against the source's scope end only. A transfer of that source while the collection still holds the element is admitted as well — the consume-site freeze traces no collection (K5 clause 5's holder list; proposal 0153 declined adding one, option F) — and is exactly the clause's subject. No report line names it, so the zero-report claim is false:

```bezel
public method Run(sink: Sink)
  var item := new Item();
  var coll := new Array<Item>();
  coll.Add(item);                 // admitted: coll's scope does not outlive item's
  sink.Take(move item);           // a transfer under the collection-held clause: no report line
  coll[0].Use();                  // backstopped by K4.1 in the domain, a data race across domains
end Run;
```

This proposal adds the missing entry. It is a design change only in the sense that it chooses *where* the report speaks; no verdict and no generated code changes.

## 1. Ground truth

1. **The clause exists and is recorded.** K4 clause 3 lists "field-held, collection-held, comparer-held and frame-held borrows versus domain transfer" and adds "The recorded set is enumerable, and the trust-admission report mode ([§28.3]) enumerates it at every site." §28.3 field-store bullet clause 5 attaches the domain clause "to **every** residual-trust borrow admission: collection-held borrows". §9.7's collection bullet (v0.332): "A kill of a carried source while the collection retains the value — a move, transfer, reassignment or disposal of the source, which no traced holder sees … — is §28.11's collection-held clause".
2. **Which kills are trust and which are flag.** K4 reads three bits off the consume table. The K4.2 site-announced kills (explicit disposal, `throw`, Pattern B `await`) and displacement (K3.11) set the flag at the stated moment: a later read through the element meets K4.1's backstop in checked mode, and a disposed object crosses no domain. The family-1 transfers (K3.1–K3.4, K3.12), the owning `yield return` (K3.7) and the `return await` deferral (K3.6) move the obligation and set no flag the backstop can schedule: the object lives on under a new holder, which may hand it to another domain. Only the second group needs the recorded clause. (Matrix cell R6/C5's note and #5680's ruling record the same split for a slot overwrite: flag-backstopped, "not a new admission and not a K4.3 clause".)
3. **Where a local-sourced element can arise.** A borrow of a scope-bound binding never lands in a field (§28.3 field-store bullet clause 1), a static (*Static stores*) or a task frame unreported (the frame entry names a stored or abandoned task). Locals of record, inline-array or choice type, closures and iterator-frame locals are K5.5-traced holders, under which a transfer of a carried source is frozen (rejected). A method-local **collection** — of either family, and an `OwningDictionary` key slot — is the one holder that admits a body-sourced element and leaves its source's transfer admitted. It holds class borrows, values that hold borrows (§9.7), and iterator, enumerator or lazy-query frames carrying such borrows (an owned frame in an owning collection, or a borrow of a frame local), which K6 clause 4 puts under "the K4.3 frame clause … for a frame no traced local holds" (cell R6/C10·coll).
4. **The kills are body-local.** Every transfer of a local or `owned` parameter is spelled in its declaring body (a closure may not consume its captured owned bindings, K5 clause 5). So whether a transfer follows such a store is decidable over one body.
5. **The compiler.** The report mode is not implemented ([#4605](https://github.com/s8lap/bezel-lang/issues/4605); `grep -rn "report-trust" src/` finds nothing). The rule corpus has seven report-mode cases (`s28_3_0090`–`0096`); none has a body-sourced store followed by a transfer, so none changes its expectation.

## 2. Measurement

- **Verdicts:** none change. The report "changes no verdict and no generated code". Newly rejected: nothing. Newly admitted: nothing.
- **Newly warned, under the mode only:** a transfer that a body-sourced collection store reaches. `tests/`: the mode is unimplemented, so no gtest or lit test runs under it; the seven corpus cases do not contain the shape (read above). `docs/`: no example is presented under the mode. Reach in programs built under the mode is unmeasurable until #4605 lands; the shape is the issue's repro and the v0.332 #5691 counterexample (`coll.Add(xs.AsSpan()); sink.Take(move xs);`).
- **Sweeps.** Rule-corpus `rule:` headers quoting the edited sentences: `s28_3_0092` paraphrases the zero-report claim, whose quoted words are unedited (checked with `corpus.py lint --strict` on a scratch copy). Guide 01 §5 describes the mode by its field-store line only and stays true. The code-generation reference §4.6 sentence "every borrow you store in a plain field, a borrowing collection, or an abandoned task frame" over-states the store entry (a body-sourced store is not reported at the store) and omits the transfer; it is edited in place.

## 3. Options

- **A — status quo.** The claim stays false for a class-typed local-sourced element (since the report's introduction at v0.307) and, since v0.332, for the span and value forms.
- **B — narrow the claim (#5713 reading 2).** Exclude "the collection-held clause at a local-sourced element" from the zero-report claim, with the reason that the kill is a site-visible transfer. Keeps a hole in the one sentence the mode exists for: a reader of a silent report would have to re-audit every transfer in the unit by hand.
- **C — freeze the source (#5713 reading 3).** Trace a local collection as a K5.5 holder of its elements' sources, so the transfer is rejected. This is proposal 0153's option F, declined at v0.332: a collection is reachable through class-borrow aliases a body-local trace cannot see (K5.6's ground). It would also turn a recorded trust admission into a rejection — a verdict change the issue does not ask for.
- **D — report every body-sourced store (#5713 reading 1, the store-site form).** Widen the collection entry to every admitted store whose element source is a local of the body. Sound, and the simplest text, but it reports admissions that rest on no trust: a store whose source is never transferred while the collection holds it is bounded by the scope comparison and the flag. The report would list every local collection a program builds, and an audit whose lines mostly name no obligation stops being read (rationale §5.28's signal argument, applied to the opt-in mode).
- **E — report the transfer (#5713 reading 1, the kill-site form; recommended).** A new entry: a transfer (the consume table's rows K3.1–K3.4, K3.6, K3.7, K3.12) of an owned binding, on a path from an admitted store into a collection slot of an element that carries a borrow of it — a borrow sourced at it, a value that holds one, a frame (or a borrow of one) that carries one — reported at the transfer. The flag-backstopped kills stay outside, and the claim's exclusion list names them both (K4.2's and displacement's).
- **F — E plus the store.** Report both the store and the transfer. Two lines for one obligation; the store line names nothing E's transfer line does not.

## 4. Evaluation and decision

| | A | B | C | D | E | F |
|---|---|---|---|---|---|---|
| Zero-report claim true | no | yes, by exclusion | yes | yes | yes | yes |
| Verdict changes | 0 | 0 | rejects the issue's repro and its span twin | 0 | 0 | 0 |
| Lines that name no live obligation | — | — | — | every body-sourced store with no later transfer | over-report only where the collection is already dead or the element removed | as D |
| New analysis | none | none | a K5.5 holder (declined) | none (store-site, structural) | one body-local forward reach, store → transfer | as E |
| Consistency with the register's other entries | — | carves a hole in the register | contradicts the 0153 option-F decline | the frame entry is already flow-sensitive ("bound and not awaited on every path before a held source's scope exits"), so structural purity buys nothing | names the obligation where it comes into force, as the frame entry does at abandonment | redundant |

**Decision: E.** Four consequences:

1. **The entry.** ▲ "a **transfer** of an owned binding (a local or an `owned` parameter) by an obligation-moving kill that sets no flag the K4.1 backstop can schedule (the consume table's rows K3.1–K3.4, K3.6, K3.7 and K3.12), where an admitted store has placed into a collection slot or the key slot of an `OwningDictionary<K, V>` an element that carries a borrow of that binding (a borrow sourced at it, K2.1; a value that holds such a borrow, §9.7's collection bullet; an iterator, enumerator or lazy-query frame whose carried sources include it, or a borrow of one, transitively, K6 clause 4), on a path from the store to the transfer or, for a store in a lambda body, anywhere in the enclosing body, reported at the transfer (§28.11's collection-held clause at an element sourced in the body, which the consume-site freeze leaves admitted: a collection is none of K5 clause 5's traced holders)". The warning names the transfer, the collection store, the element and its source, the obligation and the remedies the register already lists (a `SlotHandle` key, a `Shared<T>` handle, an owning-family holder) — in practice also removing the element first, which the report need not name.
2. **The claim's exclusions name the flag-backstopped kills whole.** "not the site-announced kills (K4.2, runtime-checked under §28.2)" becomes ▲ "not the flag-backstopped kills (the site-announced kills, K4.2, and a reassignment's drop-on-overwrite of the displaced value, K3.11, each runtime-checked under §28.2)". A reassignment of a body-sourced element's source under a collection was already a flag-backstopped kill outside the register (K4 clause 3 needs a recorded clause only where "neither statically compared nor flag-backstopped"); the old wording's gloss "statically bounded … or absent" did not except it.
3. **§28.11 says where the report speaks.** One sentence after the store rule's residual: ▲ "The trust-admission report (§28.3) names a parameter-, receiver- or field-path-sourced element at its store, and an element sourced in the body at a transfer of its source that the store reaches."
4. **Over-report is chosen and recorded.** The entry does not ask whether the collection still holds the element at the transfer (a `Remove`, a `Clear`, or the collection's scope end may come first): "retains" is not body-local, since a callee handed the collection as a plain argument may `Remove` or `Clear` it. A line at a transfer under a collection that no longer holds the element is an over-report the audit can dismiss; a silence under one that still does would falsify the claim. (A collection that is later moved out of the source's scope — `outer := move coll;` into a wider local, `_f := move coll;`, `return coll;` — is not a case here: the store into it is already rejected, because §28.11's "provably outlives that scope — the collection is a field, is reachable from one, escapes the method, or is a local of strictly wider scope" is read over the collection's fate in the body, as §9.7 reads it for `var ys := xs.ToArray(); return ys;`, and K2.3 rejects a borrow reaching a wider binding where scopes compare.)

**Kernel classification (process §9).** Derivation: K4 clause 3 ("the recorded set is enumerable, and the trust-admission report mode enumerates it at every site") applied to the collection-held clause at a body-sourced element; K4's flag-set bit (read off the consume table's Disposal-moment column) selects the transfers; K5 clause 5 (the traced holders) and clause 6 say why the transfer is admitted. No new axiom, no new clause.

**Body-locality (proposal 0105 §1): local.** The entry reads one body: the store and the transfer are both in it (item 4 of §1).

**Declined (rows for `DECLINED.md`):**

| Topic | Verdict | Reason | Reopening bar |
|---|---|---|---|
| Keeping the report's collection entry at parameter-, receiver- and field-path-sourced stores, the zero-report claim false at a body-sourced element whose source is transferred (option A, the status quo) | `declined-at-proposal` | the claim is the mode's purpose; a silent report would not mean what it says | —(none stated) |
| Narrowing the zero-report claim to exclude the collection-held clause at a body-sourced element (option B; #5713 reading 2) | `declined-at-proposal` | keeps a hole in the claim a reader cannot see from the report; the transfer is decidable over one body | —(none stated) |
| Reporting every admitted store of a body-sourced element into a collection, at the store (option D; #5713 reading 1's store-site form) | `declined-at-proposal` | reports admissions that rest on no trust (a source never transferred while held is bounded by the scope comparison and the flag); the lines that name an obligation would be lost among the ones that name none | a measured program, built under the mode, in which the transfer-site line misleads where a store-site line would not |
| Reporting both the store and the transfer (option F) | `declined-at-proposal` | two lines for one obligation | —(none stated) |

Option C is proposal 0153's option F, already a `declined-at-proposal` row; its row gains a dated annotation naming this proposal as the report-side answer.

## 5. The normative edits

Exact `old → new` pairs are `scratch/v0333/V3.json` (ids `V3-5713-*`):

1. **§28.3 *The trust-admission report*:** the new entry after the collection entry; the exclusion clause of the zero-report claim.
2. **§28.11 *Storing a borrow*:** the report sentence after the residual's span sentence.
3. **Rationale §5.31:** one paragraph recording the entry, the decision for the transfer site, and the declines.
4. **Code-generation reference §4.6:** the opt-in report sentence restated in place (it named a store into a borrowing collection without its source condition and omitted the transfer).
5. **`DECLINED.md`:** four `declined-at-proposal` rows; the 0153 option-F row annotated. **`README.md`:** this proposal's live-queue row.

## 6. Changelog, migration, trackers

- **Migration.** Newly rejected: nothing. Newly admitted: nothing. Newly warned under `--report-trust-admissions` only: a transfer reached from a body-sourced collection store. Doc-only: §28.3's exclusion clause; §28.11's sentence; rationale §5.31; the code-generation reference sentence. Spec-ahead-of-impl: the entry is part of the unimplemented mode — a comment on [#4605](https://github.com/s8lap/bezel-lang/issues/4605) naming the entry, no new tracker.
- **Corpus cases to write after landing** (`--report-trust-admissions`): warn at `sink.Take(move item)` after `coll.Add(item)` over a local `Array<Item>`; warn at `sink.Take(move xs)` after `coll.Add(xs.AsSpan())`; warn at `pool.Add(move item)` (an `owned` argument, K3.3, into a local `OwningArray<Item>`) after `coll.Add(item)`; quiet for `coll.Add(item); item.Dispose();` (a K4.2 kill); quiet for `coll.Add(item);` with no transfer; quiet for `coll.Add(item); item := new Item();` (displacement).

## 7. The matrix cells after the change

No verdict moves. The cells that recorded #5713 as a report gap — R2/C3, R6/C3·route, R6/C5 (the `ref spans[0]` face) and R6/C10·coll — keep their verdicts and lose the caveat: the collection-held clause at a body-sourced element now has its report line. Neighbours re-walked: R3/C3, R4/C3 and R5/C3 (value-type elements, the same entry through "a value that holds such a borrow"); R2/C10 and R6/C10·coll (a frame in a local collection, through the frame arm); R1/C3 (an owned element: no borrow, no entry); R2/C1 (a field never holds a body-sourced borrow); R2/C9 (a task in a collection is the frame entry's "stored" task, reported at the store). None opens.
