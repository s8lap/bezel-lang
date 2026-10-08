# Proposal 0151 — Static stores admit only static-storage sources

| | |
|---|---|
| **Status** | graduated-v0.331 — superseded — graduated v0.331 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option C, a class- or interface-typed borrow reaches a static only from static storage; critique round 1 applied, 4 blocking and 9 non-blocking findings resolved, shared with the #5681, #5682 and #5683 rulings; that round restated the rule as a borrow-side restriction, defined a static-storage source as a bare read, and replaced two remedies that were terminal field moves; landed with the v0.331 changelog entry) |
| **Tracking issue** | [#5676](https://github.com/s8lap/bezel-lang/issues/5676) (`spec-defect`, `soundness`; the v0.330 borrow-flow matrix's root cause B3, cell R2/C2); one `spec-tracking` compiler tracker is filed at landing (§6) |
| **Target module(s)** | **spec** — §28.3 (*Property stores of a borrow-carrying value*, its static clause; a new *Static stores* bullet after it), §22.12 (rule 2's **Ownership** store-semantics bullet, one sentence), K4.3 (one sentence), Appendix C.3 (one row); **rationale** — §12.15 (one paragraph); **code-generation reference** — §7.6 (one *Looks right, is rejected* pair); **`DECLINED.md`** — four `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.330** (HEAD `1ec0886`). Section numbers are v0.330's. |
| **Origin** | The v0.330 borrow-flow matrix audit (`scratch/matrix/`, lane M1, cross-checked by X1 as CONFIRMED): a class-typed borrow stored into an admitted mutable static is admitted by §28.3's field-store residual, but a static is reachable from every domain and the residual's flag backstop assumes a same-domain disposal. The owner directed on 2026-10-08: "Please resolve and decide all known spec related issues. Do proposals for issues that need them." This one needs a proposal because it removes a stated admission. |

*Reference convention: unprefixed §-references are language-spec references at v0.330; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

**Decision (option C).** A store of a class- or interface-typed value into a non-`owned` static field — a mutable static §22.12 rule 2 admits, or a `static readonly` field in its own static initialization — or into a static property not through `set(owned)` admits a class- or interface-typed **borrow** only when it is **static-storage-sourced**: a read of a `static` class- or interface-typed field (`f` or `T.f`) or a static property read `T.P` — not a member reached *through* one (`T.f.g`, `T.P.Peek()`), whose source is the referent's replaceable state. A freshly-produced owned value (discharged, process-lived — the §22.13 exemption) and `null` stay admitted exactly where they are today (a fresh value at a block-form static property of a non-(i)/(ii) type stays §28.6's fresh-store error). A borrow of any other source — a plain or `scoped` parameter, `this`, a field or getter reached through the receiver, a capture, a local, an `owned` parameter, a temporary, a function-value invocation's result — is a compile error at the store. The field-store bullet's residual no longer reaches a static.

The ground is K4.3's own third clause: a trust admission must be either statically bounded or flag-backstopped, and a static-held borrow is neither. K4.1's flag is read by a later use *in the disposing domain*; a static is read from every domain; the domain clause names a transfer or a consume of the source, and a static store makes none. The rule also restores a claim the spec already makes: §28.3's *Static field access* producers row and its callee-side list's item 2 say every admissible static form "keeps its referents alive to process exit", which a stored borrow falsified.

Measured reach: **zero** programs in `tests/`, the documents and the rule corpus store a non-static-sourced borrow into a static. The compiler admits the store today (§6), so the rule is spec-ahead-of-impl.

## 1. The problem

§28.3's *Property stores of a borrow-carrying value* bullet, on a static store, verbatim: "A borrow of a scope-bound local or of an `owned` parameter is rejected; a class- or interface-typed borrow is admitted where the field-store bullet admits it, under its residual and domain clause". The field-store bullet's clause 2 admits "borrows sourced from a parameter that is **not `owned`** — a plain or `scoped` parameter — or from the receiver", and its clause 3 makes the admission "a K4.3-recorded trust clause … with K4.1's backstop as the named safety net". Clause 4, the domain clause, bounds "not transferring the source's object to another domain — and, its extension, not consuming it into a construction that makes it reachable from other domains".

None of that bounds a static-held borrow. The source is neither transferred nor consumed; the *borrow* is published, by the store, to every domain (§22.12: "Static state is **process-global** — one instance across all domains"). The owner domain disposes the source on its own schedule; another domain's use through the static races that disposal, and K4.1's flag — "a member access through the stored borrow after the source's disposal raises `DisposedAccessException`" — is a read of the object the other domain may be mid-use in. K4.3's recorded set ("field-held, collection-held, comparer-held and frame-held borrows versus domain transfer") has no static-held clause, and K4.3 says "no new admission may be made without recording its clause or its backstop". The matrix's counterexample:

```bezel
module M;
class Gate
  private static _m: Mutex? := null;           // §22.12 rule 2 form (i)
  public static method Publish(m: Mutex)
    _m := m;                                   // plain-parameter borrow: the field-store residual
  end Publish;
  public static method Work()
    using var g := _m!.Acquire();              // runs in the spawned domain; no transfer anywhere
  end Work;
end Gate;
class Program
  public static method Main(): int
    using var t := Domain.Spawn(lambda() => Gate.Work());
    do
      using var m := new Mutex();              // owned by the main domain
      Gate.Publish(m);
    end;                                       // m disposed here, racing Work's use of Gate._m
    return 0;
  end Main;
end Program;
```

The same text also contradicts itself. §28.3's *Static field access `T.f`* producers row: "every admissible form keeps its referents alive to process exit (§22.13; §22.12 rule 2's overwrite-never-disposes store semantics)". The callee-side list's item 2 admits a static-storage-sourced return from a static property getter on the same ground: "every admissible static field form keeps its referents alive to process exit". Both are true only while every value a static ever holds was *discharged into it*. A stored borrow is owned elsewhere and dies when its owner says so; after `Gate.Publish(m)` above, `Gate.M` (a static property over `_m`) would lend a borrow of a soon-dead object as K2's always-live source.

## 2. Options

- **A — status quo.** Keep the admission under the field-store residual.
- **B — a recorded K4.3 static-held clause.** Record "static-held borrows versus the source's disposal" as a trust clause, reported by the trust-admission report.
- **C — admit only static-storage sources, besides fresh values and `null` (recommended, the issue's fix).** Reject a parameter-, receiver-, local- or capture-sourced class or interface borrow at the store.
- **D — reject every borrow store into a static,** the static-sourced one included; a static holds only what was discharged into it.
- **E — a warning** at the store (or at the declaration), with the obligation stated as rule 4's warnings state theirs.

## 3. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A | unsound: a cross-domain use-after-free, no deterministic fault (§1) | — | contradicts the always-live claim (§1) and K4.3's third clause | — |
| B | unsound: K4.3 says a clause carries a backstop; across domains the flag is none — the clause would record a race | one sentence | K4.3's own text forbids it | none |
| **C** | sound: a static holds only process-lived referents (fresh, discharged) or borrows of other process-lived storage | one bullet, a decidable source check the store site already performs for clause 1 | restores the always-live claim; mirrors App. C.1's reference-free rule for value types (the always-live holder admits only what is always live); the remedies are the existing idioms (`new`, `move`, `Other._shared`) | zero sites (§5) |
| D | sound | one bullet | over-rejects: an admitted static form never disposes a displaced referent (§22.13), so a static-sourced borrow cannot dangle; bars republishing static storage (`_tx := Other._shared;`, the stdlib §6.5 idiom's shape) | the same zero sites, plus any static-to-static republish (none found) |
| E | unsound: the hazard is memory unsafety, rule 4's violation class; §22.12 warns only for form (iii)'s wrong-or-torn-values class | one sentence | breaks §22.12's warn/error partition (rationale §12.15) | none |

Simplicity prefers C over B and E once soundness is required: the source check is the one the store site already runs for clause 1 (a scope-bound local), extended from "scope-bound" to "not static storage". Consistency prefers C over D: the admitted set is exactly what K2's always-live source promises.

**Why no `scoped`-style static bound.** §28.14's storage-bounded admission bounds a stored borrow by the holder *object*'s life, checked statically against the sources. The holder here is process-lived, so the bound would be "the source outlives the process", which only static storage satisfies — option C already.

## 4. Decision

Option C, as a delegated ruling. The rule is stated once, as a new §28.3 bullet (*Static stores of a class- or interface-typed value*) right after the property-store bullet, which now points at it; §22.12's **Ownership** store-semantics bullet, K4.3 and Appendix C.3 carry one sentence or row each; rationale §12.15 records the grounds and the declines.

Four readings the critic should check, decided here:

1. **`static readonly` fields are in scope.** Their only assignment sites are static initialization (§22.13), where a borrow of a static constructor's local is already rejected by clause 1; the rule makes the admitted set uniform across static forms. A `static readonly _a: ILogger := Other.Logger;` (static-sourced) stays admitted.
2. **`scoped` static fields** are not reached: a `scoped` field is declared only at a function type (§28.14; §28.3 *Set-erasing positions*), so it never holds a class- or interface-typed value, and §28.14 governs what it holds.
3. **Interface-typed statics** take the same rule; `IReadOnlySpan<TElement>` is read as the span (§9.7) and takes §9.7's static-field bullet (null-source only), unchanged.
4. **Chains through static storage are not static storage.** `T.f.g`, `T.P.Peek()`, `T.f[i]` read the referent's own field, getter or slot, which an instance store or §28.7's drop-on-overwrite can replace while another domain reads the static; only the bare read (`f`, `T.f`) and the static property read `T.P` (whose getter, by §28.3 callee-side item 2, returns only such a bare read) are the always-live source. This matches item 2, which admits only a *bare* static field read from a static property getter. A local alias of a static read (`var s := Other._shared; _m := s;`) is likewise not admitted — the bare-read form is the spelling, as in item 2 — no site in the measurement writes it.

## 5. Measurement

- **`tests/`:** every mutable static of a form-(i)/(ii) type found (`grep` for `static … : (Mutex|Semaphore|Atomic|AtomicInteger|ChannelSender|SharedEventSource|Task)…` without `readonly`: `sema_static_domains_1363__*`, `static_mutable_admission_1556_test.cpp`, `static_method_borrow_return_1547_test.cpp`, `builtin_class_field_no_ctor_da_4025_test.cpp`, `sema_static_class__static_owned_mutable_field_ok_1557.bzl`) is initialized or assigned from a fresh producer (`new …`, `Make()`, `Compute()`, `inbox.NewSender()` — `NewSender(): owned ChannelSender<T>`, stdlib §4) or `null`. No store from a parameter, receiver, local or capture.
- **Documents:** the spec's and Guide 04's `_cfg := LoadConfigAsync();` cache, stdlib §6.5's `TickBus.Tap := inbox.NewSender();`, the code-generation reference's `_cfg: Task<Config>? := null` — all fresh. The reference's `_v := x;` (line 1919) is an instance field of a generic class, not a static.
- **Rule corpus** (`tests/conformance/rules/`, the 120 v0.329 cases included): `s22_12_0013` and `s22_13_0009` store `Compute()`; no case stores a borrow into a static. No case quotes the edited sentences (checked against `manifest.tsv`), so no `rule:` header moves.
- **Doc-example corpus** (`tests/conformance/cases/`): `codegen_0066`, `spec_0158`, `spec_0159`, `guide4_0017` — the same cache shape, fresh.

**Newly rejected:** a class- or interface-typed borrow of a plain or `scoped` parameter, of `this` or anything reached through it, of a capture, of a local in the borrowed state, of a function-value invocation's result, or reached through a static's referent (`T.f.g`, `T.P.Peek()`), stored into a non-`owned` static field or a settable static property. Zero sites. **Newly admitted:** nothing.

## 6. Compiler facts and the tracker

Source-read at `1ec0886`; no build was available to this lane.

- `src/ownership/Ownership_Checks.cpp:15340–15350` (`targetIsNonOwnedField`'s static arm): "a STATIC field follows the static-init model, not the §28.6 instance-field leak rule. Every store into a non-`owned` static field DISCHARGES its producer" — the function returns `false` for a static target, so the §28.6 fresh-store and leak paths skip it.
- `checkBorrowStoreEscape` (`Ownership_Checks.cpp:9552`) rejects a borrow of an *owned scope-bound local* stored into a field, static or instance (its comment at `:9583` names "an unqualified static field"); nothing reads a parameter, receiver or capture source at a static target. `_m := m;` with `m` a plain parameter is therefore **accepted today**; so is a store of a field reached through `this`.
- The remedy forms are all accepted: `_m := new Mutex();` (`static_mutable_admission_1556`), `Holder._m` re-exposed through a static property (`static_method_borrow_return_1547`, `static_property_mutex_stays_accepted`).

**Tracker (filed at landing, `spec-tracking`):** reject, at the store site, a class- or interface-typed borrow whose source is not static storage, into a non-`owned` static field or a settable static property; admit fresh producers, `null`, `move`, and bare static field / static property reads. Probe first with the §1 program and with `static readonly` assignment in a static constructor from a parameter of a static helper. Rule-corpus cases to write after landing: §7.

## 7. Corpus cases to write (after landing)

- `s28_3_NNNN` reject: `_m := m;` from a plain parameter into a `Mutex?` mutable static.
- `s28_3_NNNN` reject: `_cfg := t;` from a plain `Task<Config>` parameter into the form-(ii) cache.
- `s28_3_NNNN` reject: a static property assignment `Gate.M := this._mine;` from an instance method (receiver-sourced).
- `s28_3_NNNN` reject: `_m := Holder.Pool.Get();` — a getter result reached through a static's referent is not static storage.
- `s28_3_NNNN` accept (run): `_m := move m;` from an `owned` parameter; `_shared := Other._impl;` static-sourced; `_m := new Mutex();`.
- `s22_13_NNNN` reject: a static constructor assigning a `static readonly` field from a local helper's parameter (clause 1 already; the new bullet's citation).

## 8. Self-critique (adversarial pass)

- *Does the rule over-reject the back-reference idiom?* No: the back-reference is an instance field (`_owner: Customer`), untouched. A process-global registry of borrowed objects was never sound; the idiom for it is `static owned readonly _registry: Locked<…>` holding owned values (§22.12 rule 4(a)).
- *A `move` from a `using var`?* Rejected already (§28.3: `using var` is never movable out of). The remedy text says "from an owned binding".
- *Does rejecting at the store leave an escape through a static property's block-form setter?* The property-store bullet judges the *assignment* `T.P := e` as a store into a static field of the property's declared type, "whatever form the property is declared in", so the setter body never sees a non-static-sourced `value` of class type. A setter body that stores its `value` into a *different* static is a second static store, judged by the same rule at that site; its `value` is a plain parameter of the setter's body (§23.3) and is rejected there.
- *A static-sourced borrow of a mutable static that is later overwritten?* §22.13: overwriting never disposes the displaced referent, so the borrow stays valid to process exit. The rule relies on exactly that sentence; it is named in the rationale paragraph.
- *Neighbouring matrix cells.* R1/C2 (owned values into statics): unchanged — fresh values and `move` are the admitted operands. R3–R6/C2: unchanged, null-source only. R7/C2: a function-typed static is form (iii)'s hazard class and the sink rule; unchanged. R8/C2 and R9/C2: unchanged. R2/C1 and R2/C3 (instance fields, collections): unchanged, their residual stays recorded. The cell R2/C2 becomes **decided — rejected except static-sourced**, and no neighbouring cell opens.
- *Could a generic body launder it?* `class Reg<T> … static _t: T` is rejected at the declaration (form (v)); `static readonly _t: T` is assigned only in static initialization, where the rule reads the operand's source as everywhere.
