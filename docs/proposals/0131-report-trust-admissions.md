# Proposal 0131 — The trust-admission report: a per-build diagnostic mode that names every recorded K4.3 clause at its site

| | |
|---|---|
| **Status** | under-review — owner-directed 2026-09-22: drafted for graduation in the v0.307 train; adversarial critique received 2026-09-22 and resolved (`scratch/finalize-B/resolutions.md`: F-1, F-2, F-3, F-6, N-13, N-14) |
| **Tracking issue** | to be filed at landing |
| **Target module(s)** | **spec** — §28.3 (*Diagnostics*: one new paragraph defining the mode, its report, and the claim it licenses; K4 clause 3: one sentence naming the mode as the register's enumerator). **rationale** — a new §5.31 decision record (why a mode, why a warning, why not always-on; §5.30 is proposal 0128's, in the same revision). **code-generation reference** — §4.6 (one sentence). **Guide 01** — §2's back-reference paragraph (one sentence naming the mode). Toolchain: the driver flag `--report-trust-admissions` (`src/main.cpp`, `src/driver/Driver.hpp`). No grammar change; no program's acceptance changes; the default build emits nothing new. |
| **Written against** | spec **v0.306** (`cafdfc5`) |
| **Origin** | The 2026-09-22 "path of least resistance" investigation: make the good memory-management pattern the shortest spelling; make the compiler say what it already knows. This is the second half — the compiler decides, at every field store, collection store and async call, whether the admission it is making rests on a recorded trust clause; today it says so only in a source comment (`Ownership_Checks.cpp:8073`: *"parameter- and receiver-sourced borrows are admissible with a recorded trust clause"*). |

*Reference convention: unprefixed §-references are language-spec references; rationale and stdlib references are marked; references to this proposal's own sections are written "this proposal's §N". K tags are §28.3's kernel ids. Quotations are drawn at v0.306 and are verbatim; bold inside a quotation is the source's own. ▲ marks proposed surface, landed nowhere.*

---

## 0. Thesis

K4's third clause — *"Recorded trust, or nothing"* — is the ownership kernel's honesty device: an admission that can be neither statically compared nor flag-backstopped must *"either reject statically or carry an individually recorded trust clause"*, and *"no new admission may be made without recording its clause or its backstop."* The register is therefore enumerable in the text; at v0.306 it is enumerable in the compiler too, because every admission on it is decided by a structural check that already distinguishes the trust-bearing outcome from the rejected one. What the language lacks is a way to *ask*. A programmer who wants to know "where in this module am I trusting a lifetime the compiler did not prove?" reads the ownership checker's comments.

This proposal defines a **per-build diagnostic mode** — off by default, selected explicitly for a build, with no effect on acceptance — under which the compiler emits one **warning** at every site where it makes a K4.3-recorded admission, naming the clause, the borrow, its source, the holder, the obligation, and the static remedies available at that site (`scoped`, a `SlotHandle` key, `Shared<T>`, the owning family, awaiting to settlement). The mode licenses one precise claim: a compilation unit that emits **no** report under it contains **no** recorded trust clause — every borrow it holds is statically bounded or absent — and it states, with equal precision, what that claim does not cover.

## 1. The current state, precisely

### 1.1 The clause

§28.3, K4 clause 3 (v0.306):

> 3. **Recorded trust, or nothing.** Where an admission's temporal guarantee can neither be statically compared (K2.3) nor flag-backstopped — the domain clause: a borrowed-from object transferred to, or made reachable from, another domain stays alive, so no flag is ever set — the rule must either reject statically or carry an **individually recorded trust clause**. The recorded clauses (field-held, collection-held, and frame-held borrows versus domain transfer; [§28.3], [§30.1]) are the model's honest limit, and no new admission may be made without recording its clause or its backstop.

### 1.2 The complete recorded set at v0.306

Every occurrence of `K4.3`, `residual-trust`, `residual trust`, `trust clause` and `recorded trust` in the spec was enumerated (`grep -n` over `docs/Bezel_Language_Spec.md`: 24 hits at lines 3051, 4600, 5717, 6739, 6754, 6763, 7050, 7052, 7068, 7232, 7273, 7715, 7744, 7964, 7980, 7982, 8391, 8399, 8494, 8496, 8517, 8845, 8975, 9344) and classified. Four admission *families* carry a K4.3 clause; every other hit is a restatement, an exclusion, or a different regime.

**Family A — the field-held borrow** (§28.3 store rule clauses 1–5; restated at §28.6 for an owned field's value reaching another object's borrow field — *"each admissible store carries the [K4.3]-recorded trust clause and [K4.1] backstop of every field-held borrow"* — and for an owned field passed as a plain argument that the callee stores; at §31.1 for a borrowing field inside a thrown instance; at §22.16 for a contract-clause capture whose source is a field path, *"in the same residual-trust regime as every field-held borrow"*).
- *Site:* the assignment or field initializer that stores a **parameter-, receiver- or field-path-sourced** class- or interface-typed borrow into a **non-`owned`** class- or interface-typed field. (A scope-bound local or temporary is rejected there, not admitted — no report; an `owned` field is a transfer — no borrow.)
- *Obligation:* the source object outlives every read through the field's *current value* (§28.6's precision), and is neither transferred to another domain nor consumed into a domain-reachable construction while the field holds it (clause 4).
- *Static remedies:* declare the parameter `scoped` (§28.14's scoped construction — *"no residual trust"*); store a `SlotHandle` key and look the object up at use (stdlib §4.21, proposal 0130); hold a `Shared<T>` handle instead of a borrow (§28.13); take ownership (`owned` field, `move`).
- *Computed today:* yes — `checkBorrowStoreEscape` (`src/ownership/Ownership_Checks.cpp:7329`), `Dest::Field` leg: the rejected legs report at `:8016`–`:8077`; the admitted parameter/receiver leg falls through silently, with the comment at `:8073` quoted in this proposal's header.

**Family B — the collection-held borrow** (§28.11 *Storing a borrow*; the element-slot policy's derivation tag *"[K2.3]/[K4.3] for stored borrows"*; the producers table's indexer row, whose read side names *"the owner-outlives-use residual being the same trust regime as field-held borrows"*).
- *Site:* a store — `Add`, `Insert`, `AddRange`, `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `Push`, `Enqueue`, the indexer-set — into a **borrowing-family** collection at a class or interface element type, or into the **key slot** of an `OwningDictionary<K, V>`, whose argument is a parameter-, receiver- or field-sourced borrow. (A fresh unbound temporary there is §15.4's existing fresh-store *warning* — a statically-certain hazard, not a trust clause — and is not this report. A store into a purely local collection whose scope the source outlives is statically bounded — no report.)
- *Obligation:* each admitted element's source outlives the collection's use of it; the domain clause verbatim.
- *Static remedies:* the owning family (the collection owns); `SlotHandle` keys in place of element borrows (stdlib §4.21); the borrowing snapshot discipline (stdlib §4.5: the owner outlives the snapshot's every use).
- *Computed today:* yes — `checkCollectionBorrowStoreEscape` (`:8131`) and `checkIndexerSetBorrowStoreEscape` (`:8657`): the `Escapes` receiver kind rejects a scope-bound source (`:8481`–`:8510`, `:8840`–`:8877`); a parameter/receiver/field source on the same receiver is the admitted trust-bearing outcome; and the comment at `:8117`–`:8121` records that *"an unrooted or structurally-complex receiver (e.g. the result of a call) is conservatively left unclassified — no diagnostic — matching the 'residual is trust' posture"*. Under the mode that unclassified branch **reports** too: it is an admission the checker did not decide, which is exactly a trust residual.

**Family C — the frame-held borrow** (§30.1 *Borrowed parameters, `this`, and captures are held until settlement*: *"The obligation is a [K4.3]-recorded trust clause"*; K6.4; §30.5's Pattern A operand residual — *"the binding's own borrow chain carries the true-task-outlives-the-use residual ([K2.2] + [K4.3])"* — and kind 3's `return await` over a borrowed source, *"a caller-side residual of [K4.3]'s recorded family"*).
- *Site:* the spec discharges the obligation *"trivially by the dominant shape — awaiting the task before any source's scope exits"* and names *"the abandonment idioms"* as *"the shapes that carry a live residual"*. The report therefore attaches where the frame provably outlives the synchronous call with a borrow-sourced receiver, argument or capture: `Task.FireAndForget(call)`; a task binding not awaited on every path before a captured source's scope exit; a minted task stored into a field or collection; a `return await` whose source is a borrow of anything but process-lived static storage (kind 3's non-trivial residual); and a Pattern A `await` of a plain **borrowed** task operand (a task parameter, a task field read) — the §30.5 residual, reported at the `await`.
- *Obligation:* every borrow source reaching the call outlives settlement; the domain clause verbatim.
- *Static remedies:* await to settlement in the sources' scope; `TaskScope` (stdlib §6.10, *"whose disposal quiesces its children"*); transfer the inputs (`owned` parameters, sendable copies).
- *Computed today:* partially. The abandonment forms are recognized structurally — `Task.FireAndForget` is a named escape sink (`Ownership_Checks.cpp:11010`–`:11020`, `:14984`) and the await-to-settlement obligation is tracked for the call-bounded callback case (`:14920`–`:14976`); the general "not awaited before the source's scope exits" predicate over an arbitrary task binding is the one this family needs and the checker does not yet compute for plain borrow arguments. The normative paragraph (this proposal's §2.1) mandates it alongside the structurally recognized forms — the zero-report claim depends on it, since a bound task not awaited before its source's scope exits is exactly a frame that outlives the call with a live residual — so the predicate is spec-ahead-of-implementation, carried by the tracking issue as a delta, not a design bound (this proposal's §9 Q3).

**Family D — the storage-bounded admission's two residuals** (§28.14): the **registration-lifetime residual** at bounding forms 3 and 4 — *"the registration must not be extended past the bounding object"*, *"deliberately the same recorded residual-trust shape as the quiescence rule ([K2.3] + [K4.1])"*, *"Recorded, not silent."* — and the **quiescence obligation** for user-authored bounding types — *"the recorded residual of [K2.3]'s admission posture"*.
- *Site:* the `scoped` call whose bounding object resolves by form 3 or form 4 (the registration residual); and each `scoped` **field** declaration of a user class (the quiescence residual: its holder's teardown must sever or drain).
- *Obligation:* do not `move` the handle-holding owned field or the storing owned child out of the bounding object; the bounding object's disposal invokes no stored callback after it completes.
- *Static remedies:* none inside the language for the registration residual (the record says why: freezing such fields *"would require flow-marking fields by what initialized them"*); for quiescence, the compiler-known bounding types (`TaskScope`, the stdlib §6.5 sources) whose guarantee is normative.
- *Computed today:* the bounding-object resolution is computed at every `scoped` call site, and the `scoped` field is a declared kind (`scopedCalleeScan`, `Ownership_Checks.cpp:18595`–`:18640`); the report is a line at each.

**Excluded from the register — and therefore from the report — each with its reason.**

| Hit | Why it is not a K4.3 admission site |
|---|---|
| §28.14 scoped construction (the class-typed `scoped` parameter) | *"the guarantee is fully static with **no residual trust**"* — it is the remedy, not a clause |
| §9.7 record-carried borrows | *"there is **no trust residual for record-carried borrows**"* — rejected statically instead |
| K4.2's site-announced kills — `Dispose()`/discard while borrowed, `throw x`, Pattern B `await` (and §28.6's explicit disposal of an owned field followed by a read) | a different posture: keyword-visible **and** flag-backstopped; runtime-checked, not trusted |
| §22.12 rules 2–4's domain-confinement obligations on statics | already **declaration-site warnings** in the default build; the obligation is domain confinement of process-global storage, stated by §22.12 rather than K4.3 |
| §32.10's documented trust at the FFI boundary | *"trusted but unverified"* is the whole chapter's regime, keyword-visible at every `extern` call; a mode that reported it would report every foreign call |
| Axiom A10's certification grants (`CancellationToken`'s source-outlives-settlement, the §30.11 completion-source callback admission) | an axiom's named allowlist, *"by construction not derivable"*; the obligation is stated at the type, not recorded as a K4.3 clause (this proposal's §9 Q2 asks whether the report should nonetheless name it) |
| §15.4's fresh-store warning; §28.2's use-after-dispose warning; the must-use warning | already emitted in the default build; not trust clauses |
| K5.6's slot-kill-by-move (`move box.f` into a transfer) | not a new admission — the domain clause of Family A/B attached to the holder that admitted the borrow; it is reported once, at that holder's store |
| A frame returned under a mint-announcing return type carrying parameter/receiver sources (`method Ping(c: Conn): Task => c.PingAsync();`) | **type-announced** (K6.2 + K2.4, §28.3 *Returning a deferred-activation frame*): *"every caller re-establishes the returned frame's constraints against its own receiver and arguments"* — the return records no clause; the trust attaches at the caller that abandons the frame, which Family C's other forms report there |

### 1.3 What the mode does not exist to change

No admission is added, removed or narrowed. Every program that compiles at v0.306 compiles under the mode with the same verdict and the same generated code; the mode's only effect is diagnostics, and the default build's diagnostics are unchanged.

## 2. The rule

### 2.1 The normative paragraph ▲ (spec §28.3, *Diagnostics*, appended after the recommended-messages list)

> **The trust-admission report.** A conforming implementation offers a per-build **trust-admission report mode**. In that mode, at every site where the compiler makes an admission that rests on a [K4.3]-recorded trust clause — a parameter-, receiver- or field-path-sourced borrow stored into a non-`owned` class- or interface-typed field (the borrowed-field store rule above, and its restatements at [§28.6], [§31.1] and [§22.16]); such a borrow stored into a borrowing-family collection or the key slot of an `OwningDictionary<K, V>` ([§28.11]'s store rule), including a store whose receiver the structural check leaves unclassified; a borrow held by a pending frame past the call that minted it — a task passed to `Task.FireAndForget`, stored, or bound and not awaited on every path before a held source's scope exits, a Pattern A `await` of a borrowed task operand, and a `return await` over a borrowed source that is not process-lived static storage ([§30.1], [§30.5]); and a [§28.14] admission carrying the registration-lifetime residual (bounding forms 3 and 4) or the quiescence obligation (a user-declared `scoped` field) — the compiler **emits a warning** naming the rule and its clause, the borrow and its source, the holder, the obligation (the source outlives the holder's use of it, and is neither transferred to another domain nor consumed into a domain-reachable construction while held), and the static remedies available at that site: `scoped` ([§28.14]), a `SlotHandle` key in place of the borrow (stdlib §4.21), a `Shared<T>` handle ([§28.13]), an owning-family holder ([§28.11]), or awaiting the frame to settlement ([§30.1]). The report is informative: it changes no verdict and no generated code, and outside the mode it is not emitted. **A compilation unit that emits no report under the mode carries no recorded trust clause**: every borrow it holds in a field, a collection slot or a pending frame is statically bounded ([K2.3]) or absent. The claim reaches exactly the register above — not the site-announced kills ([K4.2], runtime-checked under [§28.2]), not [§22.12]'s statics obligations (their own declaration-site warnings), not the documented trust of the foreign-function boundary ([§32.10]), and not the certification grants of axiom [A10] — and it is per compilation unit: a program-wide claim needs every artifact in the build's closure compiled under the mode.

### 2.2 The register sentence ▲ (K4 clause 3, appended)

> The recorded set is enumerable, and the trust-admission report mode ([§28.3](#sec-diagnostics)) enumerates it at every site.

### 2.3 Why a warning, and why the spec

**Severity.** Appendix D classifies diagnostic outcomes as *accept / warn / reject* and states that *"a mandated warning [is satisfied] by any warning"*. The specification has no third class — "informational" appears once, as an adjective on a §2.8.6 verdict, and "note" nowhere as a diagnostic kind — so a report that was neither a warning nor an error would be a new category with no conformance meaning, and the rule corpus's `warn` tier could not observe it (a `warn` case passes when the compile *says something*, judged by the warning regex). The report is therefore a **warning**, gated on the mode. The precedent for a warning that is not a hazard: §22.12 rule 4's declaration-site warning names an *obligation* the programmer discharges by confinement, *"not compiler-observable"*; this report names an obligation the programmer discharges by lifetime discipline.

**Home.** The mode belongs in the specification, beside §28.2's disposal-checking modes, for one reason: the *claim* it licenses is a statement about the spec's own K4.3 register, and it is only meaningful if every conforming implementation reports the same set. A toolchain-only flag could report whatever its author thought a trust clause was; a spec'd mode is held to the register, and the register is held to it (the K4 sentence: a future admission that records a clause must also be reportable, which is a cheap and useful ratchet on "no new admission without recording its clause"). The flag's spelling stays in the toolchain, exactly as `--checked-disposal` does today (`grep -c 'checked-disposal' docs/Bezel_Language_Spec.md` → 0).

**Not always-on.** §5.28's doctrine reserves default-build warnings for *statically-certain hazards*: *"a statically-certain dangle emitted silently is the one place the ceremony exemption's silence stops being honest."* A trust admission is not a hazard — it is the sanctioned spelling of the back-reference (Guide 01 §2: *"That is the right trade for most back-references"*), and a default-build warning on every `_owner := owner;` would be the noise that erodes the signal the five lints carry (rationale §5.28, as extended by proposal 0129). The mode is an audit: off by default, selected explicitly for a build — the default build emits nothing, whatever §28.2's disposal default is.

## 3. Traced examples, and the shapes that must NOT report

| Program (fragment) | Mode on | Default | Family |
|---|---|---|---|
| `constructor(owner: Customer) _owner := owner; end constructor;` (`_owner` a plain field) | **reports** | silent | A |
| `constructor(scoped owner: Customer) _owner := owner; end constructor;` | silent | silent | — (§28.14: no residual) |
| `constructor(owned owner: Customer) _owner := move owner; end constructor;` (`owned _owner`) | silent | silent | — (transfer) |
| `constructor(owner: Customer) _owner := owner; end constructor;` where `_owner: SlotHandle` and `owner: SlotHandle` | silent | silent | — (a value; no borrow) |
| `method Register(item: Item) _index.Add(item); end` (`_index: Array<Item>` field) | **reports** | silent | B |
| `method Register(owned item: Item) _pool.Add(move item); end` (`_pool: OwningArray<Item>`) | silent | silent | — (owning family) |
| `method Register(item: Item) using var tmp := new Array<Item>(); tmp.Add(item); Use(tmp); end` | silent | silent | — (purely local collection the parameter outlives) |
| `conns.Add(new Connection("a"))` on a borrowing `Array<Connection>` | §15.4's fresh-store **warning** — in both builds | warns | — (not a trust clause) |
| `Task.FireAndForget(c.PingAsync());` with `c` a plain parameter | **reports** | silent | C |
| `await c.PingAsync();` | silent | silent | — (discharged trivially) |
| `using var t := c.PingAsync(); await t;` | silent | silent | — (awaited in scope) |
| `public async method Report(t: Task<Data>): Task … await t …` | **reports** at the `await` | silent | C (§30.5's plain-operand residual) |
| `return await _cache;` over an admitted static task field | silent | silent | — (kind 3, process-lived static: *"discharged trivially"*) |
| `return await _task;` over an owned field of `this` read without `move` | **reports** | silent | C (kind 3, non-static source) |
| `using var sub := pub.Clicked.Subscribe(lambda() => Use(this));` inside a class, bound by form 3 into an `owned` field | **reports** (registration residual) | silent | D |
| `private scoped _onDone: () -> ();` in a user class | **reports** (quiescence) | silent | D |
| `x.Dispose();` while a borrow of `x` lives; `throw ex;`; Pattern B `await` | silent (both) | silent | — (K4.2) |
| `static readonly _shared: Widget` | §22.12 rule 4's **warning** — in both builds | warns | — |
| an `extern` call passing a callback | silent (both) | silent | — (§32.10) |

**What must not change:** every row's *verdict*. Nothing in the table is newly accepted or rejected, and the default column is byte-for-byte v0.306.

## 4. Alternatives considered and declined

`DECLINED.md`'s index was checked: no row records a decision on a trust-admission report or on making the K4.3 register enumerable by tooling. Adjacent and untouched: *Diagnostic identity — an error-code registry or message taxonomy* (`non-goal`, Appendix D) — this proposal mandates *when* a report is produced and what it names, never its code or wording, within that non-goal.

1. **Toolchain-only (a `bezelc` flag with no spec text).** Declined: the claim "zero reports ⇒ no recorded trust clause" is a statement about the spec's register, and a flag the spec does not know cannot make it. The K4 register sentence is the cheap ratchet the spec gains by owning the mode.
2. **Always-on warnings at every admission.** Declined by §5.28's doctrine and by the numbers: the back-reference is the *taught* shape (Guide 01 §2, spec §28.5), and a default-build warning on it converts every idiomatic class into a wall of diagnostics — the erosion the five lints (rationale §5.28) were designed not to cause. `grep -rn 'private _owner: \|private _parent: \|private _host: ' tests/ docs/` → 12 sites in the tree that would warn on every build.
3. **A third diagnostic class ("note"/"informational").** Declined: Appendix D has two sub-error outcomes and no third; a note would be invisible to every conformance instrument the project has (the rule corpus's `warn` tier included) and would need its own Appendix D category. The compiler's `Severity::Note` (`src/support/Diagnostic.hpp:19`) is an implementation detail, not a conformance class.
4. **A per-site acknowledgement marker** (`trusted owner`, a suppression spelling). Declined on 0081's recorded ground — *"a suppression spelling that left the hazard in place would be a new way to write the silent dangle"* — transposed: the marker that makes the bound *static* already exists (`scoped`), and an annotation that merely silences the report would be a no-op that reads like a guarantee. Project-level tuning of the mode (on/off) is the only knob.
5. **Report the FFI's documented trust and A10's grants too.** Declined for v1: the FFI's regime is keyword-visible at every `extern` call (the audit is `grep extern`), and A10 is an axiom's allowlist rather than a K4.3 clause; both are recorded exclusions in the normative text so the claim's reach is exact, and this proposal's §9 Q2 keeps A10 open.

## 5. Kernel and body-locality classification

**Kernel.** A **derivation** from K4.3's own text — *"an individually recorded trust clause"* — and K4.1's backstop: the mode enumerates the recorded set and adds no admission, no rejection and no axiom; the register sentence added to K4 clause 3 is an obligation on future admissions (reportability), in the shape of the clause's existing *"no new admission may be made without recording its clause"*. No new register row is owed.

**Body locality (proposal 0105 §1).** **Local.** Every report is emitted at a site the body-local checks already decide — the store-site structural check, the call-site bounding-object resolution, the abandonment forms — from information inside the body being checked; no per-body fact crosses a declaration, and nothing is re-elaborated.

## 6. Migration and compatibility

- **Newly defined:** the trust-admission report mode (spec §28.3 *Diagnostics*), the K4.3 register's reportability sentence, the driver flag `--report-trust-admissions`.
- **Newly warned:** nothing in the default build. Under the mode: every Family A–D site of this proposal's §1.2 (a measured count for the tree is this proposal's §9 Q1).
- **Newly rejected:** nothing.
- **Doc-only:** rationale §5.31; the code-generation reference's §4.6 sentence; Guide 01 §2's sentence.

## 7. Implementation sketch

- **Driver.** `src/main.cpp:130`–`:153` parses `--checked-disposal`, `--checked-contracts` and `--experimental-scoped-await-bind` into `bezel::codegen::CodegenOptions` (`src/codegen/LlvmBackend.hpp:30`); `Driver::compileFile`/`compileObject` (`src/driver/Driver.hpp:53`–`:57`, `:143`) carry the options through — the comment at `Driver.hpp:139` states that *"a field added to CodegenOptions later is forwarded here by default"*. Add `bool reportTrustAdmissions = false;` beside `checkedDisposal`, parsed from `--report-trust-admissions` at `main.cpp` (usage text at `:26`–`:70`), and hand it to the ownership pass. (It is an ownership-checker option living in a struct named for codegen; the plumbing precedent outweighs the name, and renaming the struct is a separate refactor.)
- **Family A.** `MethodOwnership::checkBorrowStoreEscape` (`Ownership_Checks.cpp:7329`): on the `Dest::Field` leg, where today the parameter/receiver source falls through without a diagnostic (the `:8073` comment), emit `Severity::Warning` when the mode is on, with the source symbol, the field, and the remedy list. Field initializers reach the same check.
- **Family B.** `checkCollectionBorrowStoreEscape` (`:8131`) and `checkIndexerSetBorrowStoreEscape` (`:8657`): where `kind == RecvKind::Escapes` and the source is a parameter/receiver/field (today: `continue`/`return false`), report; where the receiver is *unclassified* (the `:8117` comment's branch), report with a "receiver not structurally classified" clause. The `OwningDictionary` key slot rides the same path (§28.11: *"the same check reaches the key slot"*).
- **Family C.** The `Task.FireAndForget` sink (`:11010`–`:11020`), the task-store sinks, and the `return await` classification (the kind-3 leg in the §30.5 lowering) each gain a report when at least one of the minted frame's carried sources is a plain borrow; the Pattern A plain-operand `await` reports at the await. The general not-awaited-before-scope-exit predicate over a task binding is mandated by §2.1 and not yet computed for plain borrow arguments: it is the spec-ahead-of-implementation delta the tracking issue carries (the call-bounded callback tracking at `:14920`–`:14976` is the seed), and until it lands the implementation's silence there is a defect against §28.3, not a recorded bound. A return under a mint-announcing type is not a sink (this proposal's §1.2 exclusions).
- **Family D.** `scopedCalleeScan` (`:18595`) and the bounding-object resolution at `scoped` call sites: report at form 3/4 resolutions and at each user `scoped` field declaration.
- **Tests.** lit/FileCheck under `tests/lit/ownership/` for each `warn` row and each silent row of this proposal's §3 with and without the flag; the corpus cases of this proposal's §8 as the spec-cited pins.

Size: **S–M** — the sites exist; the work is a flag, a message builder, and the Family C structural forms.

## 8. Corpus accounting

New cases under `tests/conformance/rules/cases/`, `doc: spec`, `section: 28.3`, ids continuing this section's range at 0090:

| Case | Expectation | `flags:` | Pins |
|---|---|---|---|
| `s28_3_0090` | `warn`, `diagnostic: trust` | `--report-trust-admissions` | Family A: a plain constructor parameter stored into a non-`owned` field reports |
| `s28_3_0091` | `quiet` | `--report-trust-admissions` | the `scoped` construction twin of 0090 reports nothing |
| `s28_3_0092` | `quiet` | `--report-trust-admissions` | a keys-based store (`SlotHandle` values into an `Array<SlotHandle>` field beside an owned `OwningSlotMap<Node>`) reports nothing |
| `s28_3_0093` | `warn`, `diagnostic: trust` | `--report-trust-admissions` | Family B: a parameter borrow `Add`ed to a borrowing `Array<Item>` field reports |
| `s28_3_0094` | `quiet` | — | the same program as 0093 in the default build says nothing (the mode is opt-in) |
| `s28_3_0095` | `warn`, `diagnostic: trust` | `--report-trust-admissions` | Family C: `Task.FireAndForget` of a task minted over a plain-parameter receiver reports |
| `s28_3_0096` | `quiet` | `--report-trust-admissions` | the awaited twin of 0095 (`await c.PingAsync();` in the sources' scope) reports nothing |

The `warn` tier is the right instrument (AUTHORING: *"A `warn` case passes when the compile succeeds and says something"*), and it counts *because* the report is a warning (this proposal's §2.3); no harness gap arises. Case 0092 depends on proposal 0130's type landing in the same train; if 0130 is held back, 0092 is re-authored over an `int`-keyed `OwningDictionary` and a counter (the form (a) registry), which carries no borrow either.

## 9. Open questions

- **Q1 — the measured count.** How many Family A–D sites does the tree carry today? Not measured here (no compiler in this environment; the grep in this proposal's §4 item 2 finds 12 back-reference field declarations by name, a floor, not a count). The implementation lane should run the mode over `tests/` and `docs/` examples and report the number in the tracking issue; it is the cost of alternative 2 and the value of the mode, in one figure.
- **Q2 — A10's grants.** `CancellationToken`'s *"the `CancellationTokenSource` must outlive the settlement of every task observing one of its tokens"* is an obligation in the same temporal shape as Family C's, granted by axiom rather than recorded as a clause. Should the report name it (at every async call receiving a token) so the zero-report claim covers cancellation too? Recommended: no in v1 — the token is the *certified* exception and reporting it would fire on every cancellable call — but the exclusion is stated in the normative text so the claim's boundary is exact either way.
- **Q3 — Family C's reach.** The normative text mandates the structurally recognized abandonment forms, the two `await` residuals, *and* the general "task bound and not awaited before a held source's scope exits" predicate — a flow analysis the checker performs today only for call-bounded callbacks (`:14920`). It is mandated rather than lifted because the zero-report claim is only true with it: a bound-and-abandoned task is §30.1's *"shapes that carry a live residual"* as much as `FireAndForget` is. Spec-ahead-of-implementation; the tracking issue records it as the delta. Resolved (critique N-13).
- **Q4 — one report per site, or per source?** A store of a borrow whose source set has two members (a `??` join) rests on two obligations. Recommended: one report naming every source, matching how §28.3's diagnostics name the borrow's surviving use.
- **Q5 — the flag's interaction with `-c`.** The report is compile-time and per unit, so `-c` library builds report exactly as executables do (unlike `--sanitize=address`, which `main.cpp:197`–`:203` rejects with `-c`). Nothing to decide; stated so the coordinator does not add a guard.
