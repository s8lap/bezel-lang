# 0048 - Explicit factory ownership: retire the static-return heuristic

**Status:** draft
**Tracking issue:** [#2035](https://github.com/s8lap/bezel-lang/issues/2035)
**Target:** Spec [§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) (the *Producers* table row for non-`owned` method calls; the *Factory methods and sourceless returns* subsection; the iterator-convention paragraph), [§28.6](../Bezel_Language_Spec.md#sec-leak-prevention) (two cross-reference tendrils), Appendix [C.3](../Bezel_Language_Spec.md#sec-appendix-c) (one bullet rewritten); rationale [§5.7](../Bezel_Design_Rationale.md#sec-factory-method-ownership) (amended with the retirement record; §5.5 cited as the doctrine it completes); Guide 01 §5 (one sentence). **No stdlib change** (§6.2: the sweep found zero reliant signatures). **No grammar change** (§4).
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

## 0. Summary

Today a plain (non-`owned`) class-typed return means two different things depending on a fact that is not part of the ownership vocabulary at all: from an **instance** method it is a *borrow of the receiver*; from a **static** method it is *treated as owned, with a warning* nudging the author toward writing `owned` (spec §28.3, *Factory methods and sourceless returns*; the v0.60 rule, rationale §5.7). The warning was designed as a bridge — "the v0.60 standard library adds `owned` to the most common factory methods; future revisions will complete the pass" (§5.7). The pass is complete: a sweep of the current stdlib (§1.4) finds **zero** static signatures that still rely on the heuristic. What remains is the cost side only: a core ownership rule users must memorize rather than read, and a refactoring seam — *change a method between instance and static and every call site silently changes meaning* — in exactly the layer that must not have silent seams.

This proposal retires the heuristic. **A static method (named or `operator`) whose declared return type is a class or interface type must announce its caller-side classification in the signature**: `owned T` (transfer), a `Task`-family type (v0.190's announced transfer), a §29.3/§30.7 iterable or enumerator interface (the iterator convention's announced caller-owned frame), or a bare type parameter with a non-empty potential source set (v0.194's announced borrow). Any other such signature is a **compile error at the declaration**, with a diagnostic that teaches the fix. Nothing else moves: instance methods, static property getters, extension methods, and every runtime-managed return are untouched (§3), and migration is mechanical because every affected declaration has carried the warning — with the exact fix in its text — since v0.60 (§6).

## 1. Problem and evidence

All citations verified against v0.197.

### 1.1 The heuristic today

The §28.3 *Producers* table row:

> | Method call returning `T` (no `owned`) | Borrowed — instance method: source is the receiver; static method: no source, treated as owned with a warning (see *Factory methods* below). […Task-family and bare-type-parameter exceptions…] |

and the *Factory methods and sourceless returns* subsection:

> When a method returns a class type `T` (no `owned`) and the call has no **structural** borrow source — the method is `static` and, if its declared return type is a bare type parameter, its potential source set is empty (*Bare-type-parameter returns* below, v0.194) — the return is treated as **owned** by the caller, with a warning nudging the API author toward the explicit `owned` return type.

The rule is *sound* — the callee-side borrow-return prohibition (§28.3 borrow-lifetime rules; completed by v0.196's property-getter-only narrowing of the static-storage admission) guarantees no call it classifies as owned can have a borrow-returning body, and the two rules are enforced together (the subsection's own non-normative note, citing #1499). The problem is not soundness. It is that a **transfer of a disposal obligation across an API boundary is decided by a heuristic instead of by the signature**, in the one language whose stated doctrine is the opposite: "Annotations on field, parameter, and return positions communicate API intent across module boundaries — they need to be explicit" (rationale §5.5). Local inference is signal-to-noise; *boundary* inference is a rule users must memorize, and the producers table's own row is the memorization: same syntax, opposite obligations, keyed on `static`.

### 1.2 The refactor hazard (the core motivation)

The staticness key makes an ordinary refactor a silent semantics change at every call site:

- **Instance → static.** `method Make(): Widget` (a receiver-sourced borrow; callers hold no obligation) becomes `static method Make(): Widget` — and every existing call site silently flips to *owned*: disposal obligations mint out of nothing, previously-correct callers now draw must-use warnings at best, and a caller that stored the "borrow" per the old contract is re-checked under a different regime. The declaration-site nudge warning fires, but nothing *stops* the flip, and no call site is pointed at.
- **Static → instance.** The reverse flips owned to borrowed: a caller's `using var x := Make();` silently degrades from an owned binding to a borrow-pin with no disposal obligation (§28.10, v0.180) — the discharge the caller thought it had quietly evaporates. Some downstream uses still error (an owned-field store now has a borrow RHS); the ones that don't just change meaning.

Traced concretely, in current syntax. Version 1 — a per-instance cache:

```bezel
public class Gateway
  private owned _probe: Connection;
  public constructor()
    _probe := new Connection("probe");
  end constructor;

  public method Probe(): Connection
    return _probe;                  // bare owned-field read: a receiver-sourced
  end Probe;                        //   borrow through a non-`owned` return (§28.6)
end Gateway;

// caller
using var g := new Gateway();
var c := g.Probe();                 // borrow of g — no obligation; do not dispose
```

Version 2 — someone hoists `Probe` to `static` (the cache becomes creation, say, or moves behind a global):

```bezel
public static method Probe(): Connection
  return new Connection("probe");   // accepted today: treated-owned-with-warning
end Probe;

// the same caller line, unchanged in text:
var c := Gateway.Probe();           // c is now OWNED — an obligation minted silently;
                                    //   undischarged, it auto-drops with a must-use
                                    //   warning; discharged-as-borrow code is now wrong
```

Every call site changed meaning; the only signal is a *warning at the declaration* (which the refactorer may own but the callers' authors never see) plus whatever downstream must-use warnings happen to fire. Under this proposal version 2 does not compile — the author writes `static method Probe(): owned Connection` (or keeps a static property over a `static owned` field, preserving the borrow contract), and either way each call site is re-checked against a contract that is *written down*, not implied by the callee's kind.

Both refactor directions become stable: the static form must write `owned`, and `owned` survives the move to instance form unchanged (an instance method may declare an `owned` return — the ordinary §28.8 transfer). The contract lives in the signature, so moving the method between kinds moves the contract with it. The future-directions review's framing applies verbatim: this is a trap for AI-regenerated code, which refactors constantly; a rule that is *derived from the signature* is checkable by the same reader (human, agent, or the compiler itself) at every regeneration, and "ownership silently keyed on staticness" is not.

There is a second, smaller silent seam the retirement closes for free. A method reference over an unmarked static factory has plain function type `() -> Widget` (§27.1 — the `owned` marker is part of function-type identity, and this signature has none), so the factory classification does not survive into the slot: an invocation through the stored value is no longer "a static method call" to the producers table. Unmarked static factories are today the one way an owner-minting call can hide behind a plain function slot. When the signature must say `owned`, the reference's type says it too (`() -> owned Widget`), and the slot's callers see the announced classification. (This is the same seam shape as §28.3's recorded `Task`-through-bare-`T`-slot residual — closed here for the concrete-type case by construction rather than by a new rule.)

### 1.3 The safety-net motives, re-examined

Rationale §5.7 records two reasons the warning (not an error) was chosen in v0.60: "(1) third-party libraries may not annotate their returns, and (2) adding `owned` to every factory method across the standard library in one step would be a large surface-area change."

- Motive (2) is discharged: the stdlib pass completed (§1.4). The factory subsection's own example list — `Stopwatch.StartNew()`, `File.ReadAllLines(...)`, `string.Split(...)`, `ToArray()` — is stale as a list of *heuristic clients*: all four are spelled `owned` in the current stdlib (§8.1, §7.3, §9.6, §5), and two of them (`Split`, `ToArray`) are not even static.
- Motive (1) inverts under the module model that has since landed. Modules compile to artifacts carrying their interfaces (§2.8); an unannotated third-party factory is caught **at the third party's own build**, at its own declaration, with a diagnostic containing the one-token fix — it can never reach a consumer as a compiled artifact. A heuristic that silently rewrote the *caller's* obligations was the wrong kindness at the wrong end of the boundary.

### 1.4 The stdlib sweep (migration inventory, attempted per the mandate)

Grep of `Bezel_Standard_Library.md` (v0.197): 246 `static method` declaration lines. After excluding `owned` returns, value-typed and runtime-managed returns (`DateTime`, `TimeSpan`, `Guid`, `string`, `Ptr<T>`, …), and no-return methods, the complete residue of static methods returning reference types without `owned` is:

| Signatures | Why they are outside the retired heuristic already |
|---|---|
| `Task.FromResult/FromException/FromCancelled/Delay/WhenAll/WhenAny` (stdlib §6, ~10 lines) | `Task`-family returns — v0.190's announced transfer, "no factory warning is emitted — the declared type is the announcement" (§28.3) |
| `Math.Min/Max/Clamp<T>` (stdlib §2.2) | bare-type-parameter selectors with non-empty source sets — v0.194's announced borrow |
| `BitConverter.Reinterpret<TTo>` (stdlib §2) | a stated pseudo-signature; the primitive gate "keeps every admissible instantiation value-typed, so the signature sits outside spec §28.3's bare-type-parameter rule (v0.195)" |
| `Deserialize(d: IDeserializer): T` (stdlib §2.14) | the *value-type* target shape of the by-name protocol; the class-target shape is already mandated as `owned T` / `owned Shared<C>` |

**Count of stdlib signatures relying on the treated-owned-with-warning classification: zero.** The proposed error's stdlib migration is empty, and the announced-set carve in §2.1 is exactly the set the stdlib's real signatures already occupy.

## 2. The design

### 2.1 The rule (proposed normative text)

Replace the body of §28.3's *Factory methods and sourceless returns* subsection (heading and anchor kept — the section still owns this topic, and external citations stay valid) with:

> A **static** method call has no structural borrow source: there is no receiver, and a static method may not re-expose static storage through a non-`owned` return (v0.196 — the property-getter-only admission below). A non-`owned` class- or interface-typed static return therefore names no one responsible for the result, and the signature is **rejected at the declaration** (vNEXT; retiring the v0.60 treated-owned-with-a-warning classification):
>
> A `static` method or `operator` declaration — wherever it occurs: a class, a `static` class ([§11.7](#sec-static-classes)), a record, an enum, or an interface's static member set ([§11.8](#sec-static-interface-members)) — whose declared return type, nullability stripped, is a class or interface type is a **compile error** unless one of the following holds:
>
> - the return type carries `owned` — the ordinary ownership-transferring return ([§28.8](#sec-ownership-transfer));
> - the declared type is a `Task`-family type (`Task`, `Task<T>`, `OwningTask<T>`) — unmarked, it is itself container-ownership-transferring (*Task-family calls produce owned frames* above, v0.190);
> - the declared type is one of the [§29.3](#sec-iterator-methods)/[§30.7](#sec-asynchronous-programming-async-iterators) iterable or enumerator interfaces (`IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, and the four `IOwning*` forms) — the iterator convention: the unmarked type announces a caller-owned frame carrying parameter-sourced borrows (*Returning a deferred-activation frame*, v0.184);
> - the declared type is a **bare type parameter** with a **non-empty** potential source set (*Bare-type-parameter returns* above, v0.194) — the announced argument-sourced borrow. An **empty-set** bare-type-parameter return is rejected exactly as a concrete class type is: spell `owned T`, admissible over every constraint domain under [§28.1](#sec-where-the-owned-modifier-can-appear)'s per-instantiation erasure (v0.196). A returned parameter whose constraints exclude class and interface substitutions (`where T: value`) is outside this rule entirely — no disposal obligation can arise at any admissible substitution, and §28.1 rejects the marker on such a declaration.
>
> Every admissible static signature is therefore **announced**: the caller-side classification of a static call is read off the declared return type with no heuristic, and the producers-table row above never classifies a static call. The caller of an `owned`-returning static factory discharges the obligation under the standard rules (store in an `owned` field, bind with `using var`, transfer, consume as an unbound owned temporary, or explicitly dispose) — unchanged in substance from the retired classification, which already treated these results as owned.
>
> Instance methods are untouched by this rule. A non-`owned` class-typed instance return remains a borrow whose source is the receiver, and an instance method that produces a fresh value must declare `owned` — [§28.6](#sec-leak-prevention) rejects a freshly-produced owned value returned through a non-`owned` return. Documentation phrases like "returns a fresh instance" carry no weight in any classification; the qualifier in the signature is the only transfer signal (unchanged from the v0.60 note, now with an error rather than a warning behind it).

The `where T: value` exclusion, the nullability-stripping, and the pre-substitution keying are all inherited unchanged from the rules the carve list cites; this subsection adds no new classification, only the rejection of the one unannounced shape.

### 2.2 The producers-table row (proposed rewrite)

> | Method call returning `T` (no `owned`) | Borrowed — instance method: source is the receiver; extension method: source is the receiver parameter ([§26.7](#sec-ownership-in-extensions)). A **static** callee never reaches this row (vNEXT): a static method's non-`owned` class- or interface-typed return type is admissible only where the declared type itself announces the classification — a `Task`-family type (owned; the exception below), a [§29.3](#sec-iterator-methods)/[§30.7](#sec-asynchronous-programming-async-iterators) iterable or enumerator interface (a caller-owned frame; the iterator convention), or a bare type parameter with a non-empty potential source set (a borrow; the second exception below) — and any other such static signature is rejected at its declaration (see *Factory methods and sourceless returns* below). **Exception (v0.190):** [Task-family sentence — unchanged.] **Second exception (v0.194):** [bare-type-parameter sentence — unchanged.] |

The two exception sentences stay verbatim (they cover instance calls too); only the "static method: no source, treated as owned with a warning" clause is replaced.

### 2.3 The diagnostic (proposed text)

Per the repo's teach-the-fix convention (0037 Q2 precedent):

```
error: static method `Make` returns class type `Widget` without `owned`
  A static call has no borrow source, so this signature does not say who
  disposes the result. Add `owned` to the return type:
      public static method Make(): owned Widget;
  Or, to re-expose a cached instance the declaring type keeps alive, declare
  a static property whose getter returns the `static owned` field (§28.3,
  §28.6). (`Task`-family and iterable return types need no marker — the
  unmarked type is itself the announcement.)
```

The same wording serves the operator and empty-set bare-`T` forms with the obvious substitutions (`static operator +` / `: owned T`). The current declaration-site warning's text (compiler `checkStaticFactoryNudgeWarning`, `src/ownership/Ownership.cpp`) is close to this already; the change is severity plus the second remedy.

### 2.4 Consequential edits

- **§28.3 iterator-convention paragraph** ("Iterator methods are exempt from this warning"): reframe from warning-exemption to membership in the announced set — first sentence becomes "Iterator methods are outside the static-return error above: …", and the closing "The warning is suppressed because adding `owned` to every iterator return type would be redundant noise" becomes the recorded *reason the announced set exists*. The exemption becomes **type-keyed** (any static method declaring an iterable/enumerator return type, iterator body or composed frame alike) rather than body-keyed — the announcement lives in the signature, which is the entire doctrine; a composed-frame static (`static method Evens(xs: Array<int>): IEnumerable<int> => xs.Where(IsEven);`) is admitted by the same carve that admits the iterator, exactly as *Returning a deferred-activation frame* already admits its body.
- **§28.3 bare-type-parameter caller/callee sides**: the two occurrences of "the factory warning nudging toward the explicit `owned T` spelling" become "the factory error requiring the explicit `owned T` spelling"; the callee-side empty-set paragraph's static-property remedy is unchanged and now matches the diagnostic.
- **§28.6**: the v0.196 static-storage sentence's clause "its callers classify the call as the sourceless-factory case and would mint a second owner" updates to the rejection framing ("the signature itself is rejected as the sourceless-factory case; were it admitted, every caller would mint a second owner"), and §28.3's mirror clause in the *Borrow lifetime rules* ("so every caller classifies the call as the sourceless-factory case (*Factory methods* above) and would mint a second owner") updates the same way; the *Factory methods* subsection's non-normative #1499 pairing note simplifies — the pairing argument survives as the soundness explanation, with "warning" struck.
- **Appendix C.3**: the bullet "A static method returning a class type without `owned` and without an identifiable borrow source produces an owned value; the same discharge rules as `new` apply" is rewritten as the invariant: *"A `static` method or operator whose declared return type (nullability stripped) is a class or interface type must announce its classification in the signature — `owned`, a `Task`-family type, a §29.3/§30.7 iterable or enumerator interface, or a bare type parameter with a non-empty potential source set; any other such static signature (including an empty-set bare-type-parameter return, spellable as `owned T` under §28.1's per-instantiation erasure) is a compile error at the declaration (§28.3)."* The rest of the existing bullet (the bare-`T` source-identifiability text and the v0.196 property-face rule) survives with the owned-with-warning clause struck.
- **Rationale §5.7**: gains the retirement record — the two v0.60 safety-net motives re-examined per §1.3, the completed stdlib pass, the refactor-hazard motivation, and the v0.196 erasure rule as the enabler that made `owned T` spellable on every signature the error now demands it of. §5.7's historical "(or returns a value documented as freshly allocated)" phrasing is already dead in the spec (the v0.197 condition is structural; the non-normative note gives documentation no weight) — the entry notes that the retirement finishes what that note started. Rationale §5.5's boundary-explicitness doctrine is cited as what the retirement completes.
- **Guide 01 §5** (the rule-of-thumb paragraph): "a *static* method's plain return is treated as owned, with a warning nudging the API author toward writing `owned` (the factory safety net)" becomes "a *static* method's class-typed return must say `owned` — the compiler rejects the unmarked form, so a static signature always tells you whether you own the result".
- **Changelog**: one migration-normative entry. Draft:
  > **§28.3 static factory returns: the sourceless-return classification is retired; the warning becomes an error.** A `static` method or operator whose declared return type is a class or interface type must announce its classification in the signature — `owned`, a `Task`-family type, a §29.3/§30.7 iterable or enumerator interface, or a bare type parameter with a non-empty potential source set; every other such signature is now a compile error at the declaration (previously: treated-owned-with-a-warning since v0.60). Migration is the one-token edit the v0.60 warning has recommended at every affected declaration: add `owned` to the return type (for a cached instance, re-expose it through a static property instead). Call sites need no edits — the retired classification already treated these results as owned, so the migrated signature yields the identical caller-side state; the one second-order edit is retyping a function-typed slot that stored a migrated factory's method reference (`() -> T` → `() -> owned T`). Producers table, *Factory methods and sourceless returns*, Appendix C.3, and rationale §5.7 updated.

## 3. The boundary, decided case by case

Each case the mandate asked to be decided, with the current-state citation:

- **Static property getters — outside, deliberately.** They were never under the heuristic: a static property read `T.P` is a **borrow of the declaring type's static storage** (producers row, v0.180), the getter is the *only* position that may re-expose a `static owned` field through a non-`owned` return (§28.6, v0.196), and a getter may not return a freshly-produced owned value at all (§28.6 — properties do not carry `owned`). The static property is thus the sanctioned *complement* of this error — the diagnostic's second remedy — not a target of it. No change.
- **Static indexer getters — moot.** "Static indexers are not supported" (§23.5). No rule needed.
- **Extension static members — moot.** "No `static` members (v0.183): a `static` modifier inside an extension block is a compile error" (§26.3). Extension *methods* are receiver-sourced (§26.7 — "The receiver is always a borrow") and instance-shaped for classification; untouched.
- **Instance methods, including "documented as freshly allocated" — explicitly unchanged.** The v0.60 rationale's "(or returns a value documented as freshly allocated)" arm does not exist in the v0.197 spec: the sourceless condition is structural, instance returns are receiver-sourced borrows, and an instance body producing a fresh value already *must* declare `owned` (§28.6 rejects the fresh-through-non-`owned` return). This proposal changes nothing on the instance side; it makes the static side match the instance side's existing explicitness.
- **`string` and runtime-managed returns — untouched, necessarily.** They carry no disposal obligation to transfer (§28.9 — "Function call producing an ordinary value type, `string`, or runtime-managed type: Not applicable"), and `owned` on such a return type is itself a compile error (§28.1's type requirement). The error keys on the declared type being a class or interface type; these types never enter it.
- **`Task`-family, iterable/enumerator, and non-empty-set bare-`T` returns — untouched.** Already announced by their declared types with no warning today ("No factory warning is emitted — the declared type is the announcement", stated three times in §28.3); they form the carve list, not the target.
- **Operators — covered.** Operators are `public static` methods (§25) whose "return slot's optional `owned` (grammar A.4) is the sole ownership annotation" and whose operands are plain borrows (no modifiers, v0.184). An unmarked class-typed operator result has no writable non-degenerate body today (fresh requires `owned`; parameter-sourced borrows are prohibited for concrete static returns; only the all-`null` nullable body survives); the error surfaces that at the declaration. Stdlib §2.19 and §11.8's `INumber<T>` already spell operator results `owned T`.
- **§11.8 static interface members — covered at the declaration.** Witnesses must match the (substituted) declared signature exactly, so an unmarked class-or-interface-typed abstract static declaration would force witnesses this rule rejects; erroring the interface declaration keeps the conformance surface coherent. Stdlib §2.19's members already comply (`Zero(): owned T`, `Parse(s: string): owned T`, v0.196).

## 4. Grammar delta

**None.** Appendix A.4's method, interface-member, and operator productions already carry the optional marker (`(":" "owned"? Type)?`); this proposal only rejects one semantic combination of existing syntax — the same over-admit-and-reject-semantically pattern A.4 documents throughout.

## 5. Interactions

- **§22.6 owned-overload ban — no interplay, stated for the record.** The ban is about *parameters* ("neither `owned` … nor `scoped` ever participates in overload resolution"); overloading is by parameter list, so return types — and `owned` on them — never distinguish or collide overloads. Adding `owned` to a return can neither create nor resolve an overload clash.
- **§27.1 function-type identity / §27.6 method references.** `() -> Widget` and `() -> owned Widget` are distinct types with no conversion. Migrated factories' method references change type accordingly — the caller-visible improvement of §1.2, and the one place migration can require a second edit (retyping a function-typed slot that stored an unmarked factory reference). The stdlib declares no such slot; in-tree tests may (§6.3).
- **§12.1/§11.2 override and implementation matching.** Statics are never `virtual`; the only cross-declaration matching for statics is §11.8's witness rule, handled in §3. No override-chain migration exists.
- **Owning-collection static returns (§28.3 owning-collection bullet, Appendix C.3).** `static method M(): OwningArray<T>` (unmarked) is today caller-classified as a borrow with no source and callee-unwritable (every possible return path is rejected). The declaration-site error subsumes this shape: nothing admissible changes; the diagnostic moves from every return site to the one declaration. Instance owning-collection borrows (bare field re-exposure) are untouched.
- **Nullable returns / all-`null` bodies.** `static method M(): Widget?` returning `null` on every path is today the degenerate writable unmarked body (factory-classified owned-currently-null at call sites). Under the error it is rejected at the declaration like any other; the spelling is `owned Widget?` (the producers-table null row already gives the owned-currently-null caller state).
- **§22.13 static initialization and §22.12 statics-and-domains.** Unaffected — this rule touches method signatures, not static fields; the `Task?` lazy-cache admission, program-exit disposal, and the rule-4 warning are all orthogonal. §9.8/stdlib §2.14 serialization (`Deserialize`: class targets already mandated `owned`) and §15.4.1 `FromCollectionLiteral` (already mandated `owned C<T>`) gain a uniform declaration-site error where their by-name shapes are misspelled, instead of silent non-recognition.
- **FFI (§32).** No interaction: `extern` declarations "may not carry `static`" (§32.1) and classes/interfaces are not admitted at the boundary at all (§32.5); the `owned CString` return contract (§32.6) is its own regime over a value type.
- **Runtime and checked disposal.** No runtime component; the change is entirely compile-time classification. Well-formed programs' generated code is bit-identical (callers already treated these results as owned).
- **Conformance (Appendix D).** No level change; the rule lands in Core §28 alongside the classification it replaces.
- **Proposal 0046 (ownership-kernel consolidation).** 0046's §2.1 axiom register records the static-return heuristic as axiom A1 and already handles both landing orders (its §6.1/Q5): if 0046 lands first, this proposal's graduation deletes/tombstones A1 in the kernel register; if this proposal lands first, 0046 never records A1 as live. No hard dependency either way.

## 6. Migration and compatibility

### 6.1 The shape of the break

Source-breaking, but with a property no other breaking change in the project's history has had this cleanly: **every break site is already individually diagnosed today**, because the v0.60 warning fires precisely on the signatures the error will reject, with the fix in its text — at the declaration for concrete class/interface returns (the #1600 declaration-site nudge), and at every call site for the empty-set bare-type-parameter shape (§28.3's caller-side text; the declaration-site error unifies the two at the better position). The warning *is* the migration inventory; migration is "apply the fix each warning has been printing."

The fix is semantics-preserving at every call site by construction: the retired classification already treated these calls as owned, and `owned` on the declaration yields the identical caller state (producers table, `owned`-returning-call row). No overload can re-resolve (§5), no override can mismatch (§5), and callers compile unchanged. The one second-order edit is the function-typed-slot retype of §5, which the type checker locates exactly.

### 6.2 Stdlib

None. §1.4's sweep: zero reliant signatures; the announced set plus explicit `owned` already covers all 246 static-method lines. The factory subsection's stale example list is corrected as part of the §2.1 rewrite (the examples move into the rationale entry as history).

### 6.3 In-tree corpus

The compiler's two dedicated suites (`tests/ownership/static_factory_owned_nudge_1600_test.cpp`, `tests/ownership/sourceless_static_factory_1499_test.cpp`) convert from warning-expectation to error-expectation tests; fixtures elsewhere that lean on the admitted-with-warning form get the one-token edit. Conformance cases derived from the rewritten §28.3 passages are hand-maintained (#1380) and are re-derived with the landing revision. The compiler catch-up issue (filed at landing, per the spec-ahead-of-impl pattern) owns the sweep; the failing tests locate it mechanically.

### 6.4 Third-party code

None exists (pre-users). When it does, the artifact model (§2.8) means the error is always delivered at the declaring module's own build — never at a consumer holding an artifact it cannot edit.

## 7. Staging

1. **Spec revision N** (any docs train; no sequencing dependency — the v0.196 erasure rule, the last enabler, has landed): §2.1–§2.4's edits, rationale §5.7 record, guide sentence, changelog entry. The error is normative from N.
2. **Compiler catch-up** (tracked issue filed at landing): flip `checkStaticFactoryNudgeWarning` to an error with the §2.3 text and the added static-property remedy; extend it from its current fresh-body-only trigger to the full signature predicate (concrete class/interface return, or empty-set bare-`T`, minus the announced set — the caller-side classification helper already computes exactly this set); error the §11.8 abstract-static and operator forms; convert the two suites; sweep fixtures. Each piece ships with the spec-cited regression test per house rule.
3. **No deprecation window beyond the one already served.** Recommended: land as an error in one revision. The P8 sketch's "warning → error under a deprecation revision" is satisfied in substance — the warning has been the deprecation notice for ~137 revisions, the project has no external users, and §1.4 shows an empty real inventory. If the owner prefers a formal bridge anyway, the compiler can ship one revision of error-by-default-downgradable-to-warning behind a flag; the spec text should still state the error unconditionally (Q2).

## 8. Alternatives considered and declined

- **Keep the heuristic (status quo).** Declined: its two recorded motives are discharged (§1.3), and what remains is pure cost — a memorized rule, both refactor hazards of §1.2, the function-slot laundering seam, and a producers-table row that must explain staticness. A safety net with no one left to catch is only a hole in the doctrine.
- **Infer ownership from the body** (classify each static return by whether its return paths produce fresh values). Declined for three graduated reasons. (1) It deepens the disease: the contract becomes derived from the *implementation*, so editing a body silently re-contracts every caller — the §1.2 hazard generalized from one refactor to every edit. (2) It breaks the module seam: callers compile against artifact interfaces (§2.8), which carry signatures, not bodies; a body-inferred obligation either can't cross the seam or must be smuggled into the interface as an invisible annotation — an unwritable `owned`. (3) The language already litigated announcement-by-declared-type as its doctrine (v0.190 tasks, v0.194 bare-`T`, v0.184 deferred-activation frames; rationale §5.7's v0.194 entry: "A generic signature's ownership behavior is readable off its shape"). Body inference is the opposite doctrine.
- **Error at the call site instead of the declaration.** Declined: the defect is the signature's, there is one declaration and many calls, and a call-site error could only say "the callee should have written `owned`" — a diagnostic aimed at someone who may not own the code. (The declaration-site choice also matches where the v0.60 warning already fires.)
- **A distinct `fresh` marker for factory returns.** Already declined, v0.196 (rationale §5.7): it would duplicate `owned`'s class-substitution semantics keyword-for-keyword. Not reopened.
- **Uniformity absolutism: also require `owned` on `Task`-family and iterable returns.** Declined: those unmarked types are *already* announcements — one type, one classification, stated in the signature — so they have neither the memorization cost (nothing is keyed on staticness) nor the refactor hazard (the classification is callee-kind-independent: "instance, static, or otherwise"). Requiring the marker there would be "redundant noise" (§28.3's own recorded phrase) and a mass migration with no informational gain. The line this proposal draws is not "everything writes `owned`"; it is "**nothing is classified by a fact outside the signature**."

## 9. Relation to the P8 sketch (divergences recorded)

Followed in substance and direction. Deliberate deepenings beyond the sketch's "a static method returning a class type must write `owned` (or restructure)": interface-typed returns included alongside class-typed (matching the classification domain the current rules actually use — flagged as Q4 since §28.3's current subsection heading text says only "class type"); operators and §11.8 abstract statics included (they are static methods in the grammar and conformance surface); the **empty-set bare-type-parameter case** included, which the sketch did not mention and which only became includable when v0.196's per-instantiation erasure made `owned T` spellable over mixed domains; the carve list stated type-keyed (subsuming the body-keyed iterator warning-exemption — a small simplification, recorded in §2.4). One refinement of the sketch's staging: "warning → error under a deprecation revision" is read as *the v0.60 warning was the deprecation*; direct-to-error is recommended (§7, item 3), with the formal bridge preserved as an owner option. No element of the sketch is contradicted.

## 10. Sizing

| Piece | Size |
|---|---|
| Spec edit | Small — one producers-table cell, one subsection body rewritten under its existing heading, one paragraph reframed, two §28.6 tendrils, one C.3 bullet, one changelog entry |
| Rationale edit | Small — one §5.7 amendment (retirement record; motives re-examined) |
| Stdlib | **None** (sweep-verified zero reliant signatures) |
| Guides | One sentence (Guide 01 §5) |
| Compiler | Small — flip an existing declaration-site warning to an error and widen its trigger to the full signature predicate (the caller-side classification helper already computes the announced set); retire the caller-side treated-owned path for well-formed programs; two dedicated test suites converted, fixture sweep |
| Tests / conformance | Bounded — the two named suites plus one-token fixture edits; §28.3-derived conformance cases re-derived at landing (#1380 hand-maintenance) |

## 11. Open questions / owner decisions

1. **Q1 — the ruling.** Adopt the retirement? Recommended: **yes** — zero-inventory migration, both refactor hazards closed, and the last asymmetry between §5.5's doctrine and the ownership boundary rules removed.
2. **Q2 — staging.** Direct-to-error in one revision (recommended, §7 item 3), or a one-revision compiler bridge (error downgradable to warning behind a flag) with the spec stating the error unconditionally either way?
3. **Q3 — diagnostic wording.** Adopt §2.3's text (both remedies: `owned` first, static-property re-exposure second)? Editor latitude on phrasing; the two-remedies structure is the recommendation.
4. **Q4 — interface-typed static returns.** Confirm the rule covers interface-typed returns, normalizing §28.3's "class type" phrasing to "class or interface type" (recommended — it matches the borrowable-type domain the classification and the compiler's predicate already use; declining leaves interface-typed static factories as a revived heuristic island).
5. **Q5 — §11.8 declaration-site coverage.** Confirm the error fires on abstract static interface declarations (recommended, §3 last bullet), not only on concrete bodies.
6. **Q6 — subsection heading.** Keep the *Factory methods and sourceless returns* heading and anchor (recommended, citation stability) or retitle to reflect the rejection posture? Cosmetic; editor's choice at landing.
