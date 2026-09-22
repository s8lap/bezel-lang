# Proposal 0127 — `scoped` primary-constructor parameters: the class/interface-typed form admitted, the capture field as §28.14's back-reference field

| | |
|---|---|
| **Status** | graduated-v0.307 — superseded — graduated v0.307 (owner-directed 2026-09-22; adversarial critique received and every finding resolved 2026-09-22 (`scratch/finalize-A/resolutions.md`); landed with the v0.307 changelog entry) |
| **Tracking issue** | [#4601](https://github.com/s8lap/bezel-lang/issues/4601) (spec-ahead-of-impl, filed at landing) |
| **Target module(s)** | **spec** — §22.10 (*Primary constructors*: the header's admitted-modifier sentence; the `scoped` bullet under *Capture rule and layout stability*), §28.14 (*Class- and interface-typed `scoped` parameters: scoped construction* — the callee-side field sentence gains the capture-field parenthetical), Appendix A.4 (the shared-production over-admission table's primary-constructor row; the `ParamModifiers` grammar comment), Appendix C.5 (one row); **code-generation reference** §4.2 (the `scoped` bullet) and its pin; **Guide 01** (the four-shapes paragraph, re-validation pin); `DECLINED.md` (the `scoped` primary-constructor parameters `deferred` row, amended: the class/interface-typed form lifted, the function-typed form the remaining bound); no stdlib contact; no rationale contact beyond the changelog record |
| **Written against** | spec **v0.306** (`cafdfc5`) |
| **Origin** | The 2026-09-22 "path of least resistance" investigation (make the good memory-management spelling the shortest one; make the compiler say what it already knows). The spec's own *Recorded v1 bound; liftable* at §22.10, and `DECLINED.md`'s `deferred` row for it ("no evidence bar stated"), invite exactly this lift; the compiler already synthesizes the field the contract needs. |

*Reference convention: unprefixed §-references are language-spec references; K tags are §28.3's kernel ids. Quotations are drawn at v0.306.*

---

## 0. Thesis

§28.14's scoped construction is the one fully static back-reference the language has — "the source provably outlives the bounding object *including its destructor*; the guarantee is fully static with **no residual trust**" — and its *storage shape* on the callee side is deliberately nothing new: "the callee may store the borrow in a direct non-owned field of the bounding object — the ordinary back-reference field of §28.5/§28.3 … No new field kind is introduced". §22.10's primary constructor synthesizes exactly such a field for any parameter a body references ("captured as a compiler-generated private field"), and for an `owned` parameter it already carries the modifier's meaning into the capture ("captured as a compiler-generated **`owned` private field**, disposed by the generated `Dispose()`"). The two mechanisms compose without a seam: a class/interface-typed `scoped` primary-constructor parameter, referenced in a body, captures as a non-owned field of the constructed object — which *is* the bounding object of the constructor-call form (§28.14 form 1). The recorded ground for the bound — "the §28.14 scoped-storage contract needs an explicitly declared field" — is not true of this form: the field is declared, by the compiler, in the same object.

The result is that the shortest spelling of a holder-over-a-source becomes the checked one:

```bezel
public class Statement(scoped account: BankAccount)
  public method Print()
    Console.WriteLine(account.Balance.ToString());
  end Print;
end Statement;

method PrintStatement(customer: Customer)
  using var acct := new BankAccount(customer);
  using var stmt := new Statement(scoped acct);   // frozen `acct`; `stmt` disposed first
  stmt.Print();
end PrintStatement;
```

The **function-typed** `scoped` primary-constructor parameter is *not* lifted here: its capture field would have to be a `scoped` field, and a `scoped` field carries obligations (storage, never-read-out, quiescence at the bounding object's disposal) that §28.14 states over a declared field the author can see. It stays the recorded v1 bound, liftable, with the lifting design stated in §4.

## 1. The current state, precisely

§22.10, the header sentence (v0.306):

> A primary-constructor parameter list admits **plain and `owned` parameters only**; `ref` and `out` are rejected semantically (the grammar admits them through the shared Param production, the same over-admit-and-reject-semantically pattern Appendix A.4 uses elsewhere): a constructor's parameters initialize object state, and a by-reference alias or a caller-out slot has no meaning in that role.

§22.10, *Capture rule and layout stability*, the `scoped` bullet:

> - A **`scoped`** primary-constructor parameter is rejected in this revision (the header admits plain and `owned` parameters only, above): the [§28.14](…) scoped-storage contract needs an explicitly declared field — use a regular constructor. (Recorded v1 bound; liftable.)

§22.10's capture rule, which the lift rides on:

> A primary-constructor parameter is captured as a compiler-generated private field **if and only if** it is referenced in a body position — inside the body of any method, property accessor, constructor, or destructor. Parameters used only in field initializers or in the inheritance clause's base call are *not* captured: their values flow directly into the field they initialize or the base argument they supply, and no separate parameter-named field exists.

§28.14, *Class- and interface-typed `scoped` parameters: scoped construction*, the callee side:

> **Callee side:** the callee may store the borrow in a direct non-owned field of the bounding object — the ordinary back-reference field of [§28.5](…)/[§28.3](…) — and may forward it only to another `scoped` parameter with the same bounding object. It may not return it or store it anywhere else. No new field kind is introduced: `scoped` on the *parameter* **upgrades** [§28.3](…)'s residual-trust back-reference admission to a static bound at the participating call sites; a plain (non-`scoped`) borrow parameter stored per §28.3 remains legal with the trust residual.

and the caller side:

> **Caller side:** the argument carries the call-site marker — `new StreamReader(scoped stream)` ([§22.3](…)) — and is admitted under the bounding-object forms above with *the argument's borrow source* in place of a closure's capture set: the source must satisfy the same declared-before/self/`this`-derived tests, and it is **frozen** until the bound's scope exits. A **fresh producer** argument is rejected (`new StreamReader(scoped new FileStream(...))` — the unbound owned temporary dies at the end of the full expression; [§9.7](…)'s no-temporary-source rule, identical remedy: bind the source first).

§28.14's bounding-object form 1: "a constructor call → the constructed object", admitted when that object is bound to "a `using var`/`await using var` local `L` … where every captured **local** source is declared **before** `L`" or is "`this` (per forms 3/4)"; "A bounding object that is none of the admitted resolutions — a plain expression, a parameter, a returned value that reaches no admitted slot — admits **no** capture-bearing arguments".

§22.3's `scoped` bullet states the parameter kinds: "On a *function-typed* parameter … On a **class- or interface-typed** parameter …" — and Appendix A.4's `ParamModifiers` comment closes the set: "Other parameter types are rejected semantically."

`DECLINED.md`'s row (the `deferred` register): *"`scoped` primary-constructor parameters — `deferred` — spec §22.10 (*Primary constructors* — "Recorded v1 bound; liftable") — Marked **liftable**, no evidence bar stated. Rejected in this revision because "the §28.14 scoped-storage contract needs an explicitly declared field"; the header admits plain and `owned` parameters only, and the recorded remedy is a regular constructor."* This proposal reopens that deferral by its own stated condition (liftable, no bar) — it is a proposal, as the process requires, and not a re-ask against a `rejected` row.

**What the compiler does today** (§5 item 3b check). The rejection is a *parser* diagnostic, `src/parser/Parser_Decls.cpp:261-292`: "`scoped` is not permitted on a primary-constructor parameter (§22.10) — the scoped-storage contract (§28.14) needs an explicitly declared field; use a regular constructor instead". It is pinned by one test, `tests/codegen/owned_primary_ctor_param_disposal_1594_test.cpp:252-266` (`scoped_primary_ctor_param_rejected_22_10`), and by one corpus case, `tests/conformance/rules/cases/s22_10_0011.bzl` (banked `pass`). Everything the lift needs already exists: the capture field synthesis keyed on body references (`src/sema/Symbol.hpp:747-766`, `FieldSymbol::posParam` with `posParamBodyRef`/`posParamInitRef`), the class/interface-typed `scoped` admission (`checkScopedIsFunctionTyped`, `src/sema/TypeResolution_Names.cpp:1311,1349`), and the call-site machinery (`MethodOwnership::scopedBoundingLocal`, `src/ownership/Ownership_Checks.cpp:17818`; `scopedArgIsClassBorrow`, `:17720`; `scopedCollectCaptures`, `:17875`). Guide 01 teaches the rejection as one of "Four shapes the compiler rejects" (line 151).

## 2. The rule (exact normative wording)

§22.10's header sentence becomes:

> A primary-constructor parameter list admits **plain, `owned`, and class- or interface-typed `scoped` parameters**; `ref` and `out` are rejected semantically (the grammar admits them through the shared Param production, the same over-admit-and-reject-semantically pattern Appendix A.4 uses elsewhere): a constructor's parameters initialize object state, and a by-reference alias or a caller-out slot has no meaning in that role. A **function-typed** `scoped` primary-constructor parameter is likewise rejected semantically (the capture rule below).

§22.10's `scoped` bullet under *Capture rule and layout stability* becomes:

> - A **class- or interface-typed `scoped`** primary-constructor parameter ([§28.14](#sec-scoped-callback-parameters)'s scoped construction) follows the same capture rule: referenced in a body position, it is captured as a compiler-generated **non-owned** private field — exactly the direct non-owned field of the bounding object that §28.14's callee side admits, the constructed object being the bounding object — and a reference in a field initializer or in the base call is the ordinary callee-side use of the parameter: a store into a non-owned field of the object being constructed, or an argument to the base constructor, admitted against a `scoped` base parameter (a forward whose bounding object is the same constructed object) and as an ordinary borrow argument against a plain one. The capture field is not disposed and does not enter the trivial-dispose computation ([§28.9](#sec-runtime-managed-types)). At the call site the argument carries the marker — `new Box(scoped src)` ([§22.3](#sec-parameter-modifiers)) — and is admitted under §28.14's bounding-object forms exactly as at a declared constructor: the holder is a `using var` local or `this`, the source is declared before the holder, a fresh producer is rejected, and the source is frozen until the holder's scope exits. The freeze is keyed on the signature, not on the capture: a `scoped` parameter referenced in no body and no initializer captures no field, and the call site freezes its source all the same, harmlessly. A **function-typed** `scoped` primary-constructor parameter is rejected: its capture field would be a `scoped` field, whose storage, never-read-out, and quiescence obligations §28.14 states over a declared field — use a regular constructor and a declared `scoped` field. (Recorded v1 bound; liftable.)

§28.14's callee-side bullet gains a parenthetical after "the ordinary back-reference field of §28.5/§28.3": *"(a primary-constructor parameter's compiler-generated capture field is such a field, [§22.10](#sec-capture-rule-and-layout-stability))"* — and, after "It may not return it or store it anywhere else", the plain-argument disposition the §22.10 base-call clause relies on (critique N-2): *"; passing it as a plain (non-`scoped`) argument is an ordinary borrow argument, and a store the receiving callee makes is that callee's own §28.3 admission with its residual"*. §22.10's bullet also states the classification of a body reference: a read of the capture field — a borrow sourced at `this` (§28.3's producers table), returned or passed on exactly as any back-reference field's read is (critique N-1).

Appendix A.4's over-admission table row for the primary-constructor header lists what is rejected: *"`ref`/`out` parameters, and a function-typed `scoped` parameter"*. (At v0.306 that row lists `ref`/`out` only — a pre-existing minor inconsistency with §22.10's "plain and `owned` only", which the lift resolves rather than widens.) The `ParamModifiers` grammar comment gains one line: *"On a primary-constructor parameter only the class/interface-typed form is admitted (§22.10)."* Appendix C.5 gains one row stating the admitted modifier set of a primary-constructor parameter.

The exact `old → new` pairs, every one verified unique against HEAD, are in `scratch/train-A/edits.md` (group A).

### 2.1 The design question, answered: class/interface-typed only

**Recommended: admit the class- and interface-typed form only.** Grounds:

1. **The capture field is already the contract's field.** §28.14's callee side for a passive borrow needs "a direct non-owned field of the bounding object" and introduces "no new field kind". §22.10 synthesizes a private field for a body-referenced parameter; for a class/interface-typed `scoped` parameter that field is non-owned (the parameter is a borrow) and belongs to the constructed object, which is form 1's bounding object. Nothing is synthesized that §28.14 does not already describe for a hand-written constructor. The proof of the static bound (declared-before + freeze + reverse-declaration-order disposal, §28.14's "No quiescence obligation arises" paragraph) does not read the field at all — it reads the call site and the scopes — so it is unchanged.
2. **The function-typed form is genuinely different.** Its capture field would have to be a `scoped` field (§28.14: the callee stores a callback "only in `scoped` fields of the bounding object itself"), and a `scoped` field is a field *kind* with obligations: never read out as an escaping value (including into a deferred-activation call), overwritable and clearable, and — for a user-authored type — the **quiescence** residual, "the temporal guarantee is the author's, with K4.1's backstop". Synthesizing one silently would attach an author obligation to a field the author never wrote and cannot clear, which is the opposite of what the 2026-09-22 investigation asks for (say what the compiler already knows; do not hide what the author must do). v1 bound 5 ("Scoped-field right-hand sides: only a `scoped` parameter of the enclosing callable, or `null`") is *satisfied* by the synthesized capture — the RHS is the enclosing constructor's own `scoped` parameter — so bound 5 is not the blocker; the field kind and its quiescence obligation are.
3. **Narrowest sound lift.** The class/interface form widens no admission at either arm: the caller-side test is §28.14's unchanged bounding-object resolution, the callee-side store is the field §28.14 already admits, and the reject set (fresh producer, plain-`var` holder, declared-after source, value-typed parameter) is unchanged.

**Stated as liftable: the function-typed form.** Its lift would state: the capture field is a compiler-generated `scoped` private field; a body reference to the parameter is a read of that field under §28.14's `scoped`-field rules (invocation admitted; read-out as an escaping value rejected; overwrite and clear unavailable, a parameter name not being assignable — which is itself a reason to prefer a declared field); the class inherits the quiescence obligation as a user-authored bounding object. That is a design change with its own §28.14 text and its own tests; it is not this proposal.

### 2.2 The mechanics, traced

- **Capture-iff-used, kept.** A `scoped` parameter referenced in no body position and no initializer captures no field — the same rule as for a plain or `owned` parameter. The call site cannot see the body, and §28.14's admission is keyed on the *signature* ("`scoped` is part of function-type identity", §27.1) — so the caller's source is frozen whether or not a field exists. Is the freeze then pointless? Yes, and harmless: it is exactly the situation of a hand-written `constructor(scoped src: Source) end constructor;` that stores nothing, which §28.14 already admits with the same freeze. The freeze is the price of the announcement, not of the store; the announcement is the parameter's.
- **Layout stability, unchanged.** The capture-iff-used consequence — adding or removing a body reference changes the field set — applies to the `scoped` parameter exactly as to any other. The remedies paragraph (declare the field explicitly; a public property; a non-primary constructor) applies verbatim; declaring the field explicitly with `private _src: Source := src;` is itself an admitted callee-side store (an initializer reference).
- **The call-site marker, kept.** `new Box(scoped src)` — §22.3's "mark-it-at-the-site discipline"; it announces the freeze at the site the reader sees. Nothing about the primary-constructor spelling removes the need: the signature is the same signature a declared constructor would have.
- **The bounding-object forms, unchanged.** Form 1 (a constructor call → the constructed object) resolves; the admission then requires the constructed object to be bound to a `using var` local `L` with the source declared before `L`, or to be `this` (form 3/4). A plain-`var` holder — `var b := new Box(scoped s);` — resolves no admitted bounding object and "admits **no** capture-bearing arguments" (`tests/ownership/scoped_construction_1545_test.cpp:189`, `non_using_var_bounding_rejects_borrow`; corpus `s28_14_0019`). A fresh producer — `new Box(scoped new Source())` — is rejected by §9.7's no-temporary-source rule (`:83`, `fresh_producer_rejected_spec_9_7`; corpus `s22_10_0016`).
- **`owned` primary parameters beside `scoped` ones.** Independent: `class Box(owned a: A, scoped b: B)` captures `a` as an `owned` field (disposed by the generated `Dispose()`, ordered after declared fields) and `b` as a non-owned field (never disposed). §22.10's owned-capture bullet's "both in an owned-consuming initializer and in a body position: compile error" concerns `owned` only; a `scoped` parameter may be referenced in an initializer *and* a body — both are borrow reads.
- **The base call.** §22.10 puts the parameters in scope in the inheritance clause's base call. A `scoped` parameter passed to a `scoped` base-constructor parameter is §28.14's forwarding case — "forward it only to another `scoped` parameter with the same bounding object" — and the bounding object is the same constructed object (the base subobject is not a separate object). Passed to a plain base parameter it is an ordinary borrow argument; if the base stores it under §28.3's back-reference rule, that store lands in the same object and the caller-side static bound still holds (the object's teardown is one event), though the base's own admission is the trust-residual one — the same situation as forwarding a `scoped` parameter to a plain parameter of any helper. That general disposition is now stated in §28.14's callee-side bullet itself (edit A3; critique N-2) and §22.10 cites it, so the base call is an instance of the rule, not a base-call-specific one (§9 Q2, resolved).
- **Disposal order.** Holder before source: `using var s := …; using var b := new Box(scoped s);` — reverse declaration order (§21.6 rule 1; §28.10), with the holder's destructor running at its disposal (§28.4); `s` is intact throughout, which is the whole point. Corpus `s22_10_0019` pins the order.
- **Records (§9).** A record's positional list is `PositionalField ::= Identifier ":" Type SerializedClause? EqualityClause?` (Appendix A.3) — no modifier slot, so `record R(scoped x: T)` does not parse, and §9.7 makes every reference-typed record field a borrow with the borrow-bearing-record rules, no bounding object to freeze against. Nothing in this proposal reaches records; the lift is stated for the class header only. (A `scoped` parameter on a record's *body* constructor is a pre-existing question outside this proposal — §9 Q3.)
- **`this`-bounded construction.** `_child := new Box(scoped this);` inside a constructor or method — form 1 resolves the constructed object; it is assigned to an `owned` field of `this`, so form 3 applies with `this` as the bound, and the captured source is `this` itself: admitted by the `this` arm. Unchanged from the declared-constructor case.

## 3. False-positive / rejected-shape analysis — what must NOT change verdict

| Shape | v0.306 verdict | v0.307 verdict | Why |
|---|---|---|---|
| `class Box(scoped n: int)` | reject (parser: the §22.10 bound) | **reject** — value-typed `scoped` (§28.14 admits function-, class- and interface-typed only; A.4 "Other parameter types are rejected semantically") | the type restriction is §28.14's and untouched; corpus `s22_10_0017` |
| `class Box(scoped f: () -> ())` | reject (parser) | **reject** — the function-typed form stays the v1 bound | §2.1 item 2 |
| `class Box(ref x: T)`, `class Box(out x: T)` | reject | reject | unchanged; corpus `s22_10_0006`/`0007` |
| `using var b := new Box(scoped new Source());` | reject (twice: the parser bound, and §9.7) | **reject** — §9.7's no-temporary-source rule | corpus `s22_10_0016`; the case becomes single-ground |
| `var b := new Box(scoped s);` (plain-`var` holder) | reject (parser) | **reject** — no admitted bounding object (§28.14) | `scoped_construction_1545_test.cpp:189`; corpus `s28_14_0019` (declared-constructor shape, so the case is independent of this lift) |
| `using var b := new Box(scoped s); using var s := …;` (source after holder) | reject | reject — unresolved identifier, then §28.14's declared-before test | `:96` |
| `using var s := …; using var b := new Box(scoped s); s.Dispose();` | reject | reject — the freeze covers explicit disposal (K5 clause 7) | `:121` |
| `class Box(scoped src: Source) … method Leak(): Source => src;` | reject (parser) | **accept** — a body read of `src` is a read of the capture field, a receiver-sourced borrow (K1.5; §28.3's producers table, "Field access `obj.f` where `f` is not `owned` → Borrowed (source: `obj`)"), and a receiver-sourced borrow return is announcement-table row 4 | critique N-1. §28.14's "may not return it" binds the callee that *declares* the parameter (the constructor); the 1545 pin at `:262` (`callee_return_of_scoped_borrow_rejected`) is a **method's own** `scoped` parameter returned (`method Leak(scoped s: Source): Source return s;`) — a different shape. Sound: the borrow is bounded by the holder, which is bounded by the frozen source; a declared constructor's `_src` field returned reads identically today |
| `class Box(scoped src: Source) … method Stash(o: Other) o.Keep(src); end` where `Keep` stores | reject (parser) | **accept**, trust residual — the same field read passed as a plain argument; `Keep`'s store is `Keep`'s own §28.3 admission with its K4.3 residual (edit A3's new clause) | critique N-1. The 1545 pin at `:283` (`callee_store_into_other_object_field_rejected`) is `o.src := s` **inside the callee that declares** `scoped s` — a direct store into another object, the shape §28.14 forbids; it does not reach a method reading the capture field |
| `class Box(src: Source) public method Touch() src.Ping(); end` (plain, unmarked) | accept, trust residual | accept, trust residual — unchanged | §28.14: "a plain (non-`scoped`) borrow parameter stored per §28.3 remains legal with the trust residual" |
| `class Box(scoped src: Source) public method Touch() src.Ping(); end` at a `using var` holder with a declared-before source | reject | **accept** — the lift | corpus `s22_10_0011` (re-authored) |
| the same with no body reference at all | reject | **accept**, no capture field, source still frozen | corpus `s22_10_0018` |

No program that compiles at v0.306 changes verdict: the lift admits only programs the parser rejects today. Every reject in the table is rejected on a ground that survives the lift, and each survives the deletion test independently of the parser bound; the two rows the lift *admits* beyond the plain body read (`Leak`, `Stash`) are admitted on grounds §28.3 already states for any back-reference field, which is what "no new field kind is introduced" buys.

## 4. Alternatives considered and declined

1. **Keep the bound** (status quo). Declined: the recorded reason ("needs an explicitly declared field") is false for the class/interface form — the compiler declares the field — and the guide-taught remedy (a hand-written constructor plus a hand-written field) is longer than the unchecked plain-parameter spelling, so the checked back-reference is the *ceremony-heavy* option today. The 2026-09-22 investigation's criterion is that the sound spelling be the shortest one.
2. **Lift both forms at once.** Declined for v1 on §2.1 item 2's ground: the function-typed capture is a `scoped` field with author obligations (quiescence) that a synthesized field hides. Recorded liftable with the design sketched; not a `DECLINED.md` `rejected` row — the existing `deferred` row is amended to name the function-typed form as the remaining bound.
3. **Admit `scoped` in the header but capture unconditionally (always synthesize the field).** Declined: it would make `scoped` the one modifier exempt from capture-iff-used, for no gain — the freeze does not depend on the field's existence (§2.2), and unconditional capture would silently add a field to a class whose bodies never read the parameter, a layout-stability surprise §22.10 exists to avoid.
4. **Drop the call-site marker for the primary-constructor spelling.** Declined: §22.3's marker discipline is per call, not per callee kind; a call site cannot tell a primary constructor from a declared one, and the marker announces the freeze the caller is accepting. (Same ground as `DECLINED.md`'s *Named-argument spellings for `ref`, `out`, and `scoped` arguments* row: positional, marked, visible.)

`DECLINED.md` index check (2026-09-22): the only rows naming `scoped` and primary constructors are the `deferred` row quoted in §1 (reopened here by proposal, as its class allows) and the v1-admission-bounds row (items 3, 5, 6 — untouched). No `rejected` row is contradicted.

## 5. Kernel and body-locality classification

- **Kernel.** A **derivation**, no axiom change: the call-site admission is [K2.3](../Bezel_Language_Spec.md#sec-kernel-k2) + [K5.7](../Bezel_Language_Spec.md#sec-kernel-k5) under axiom **A11**'s forms, exactly as §28.14 states for scoped construction ("derives: K2.3 + K5.7, under axiom A11's forms"); the capture field is a K2.3 store into the bounding object; the freeze is K5.7. The class-side capture is §22.10's existing rule (K1 per field: a non-owned field carries no obligation). No new axiom, no register row.
- **Body-locality** (proposal 0105 §1): **local**. The call-site check reads the call expression, the holder's declaration form, and the source's declaration position in the same body; the callee-side check reads the class's own bodies against its own synthesized field. No cross-body fact is consumed that §28.14 does not already consume.

## 6. Migration and compatibility (changelog house categories)

- **Newly admitted:** a class- or interface-typed `scoped` primary-constructor parameter, captured as a non-owned field when body-referenced; the call site admitted under §28.14's unchanged forms.
- **Newly rejected:** nothing. Every rejection in §3 pre-exists on its own ground.
- **Newly defined:** the plain-argument disposition of a class/interface-typed `scoped` parameter, stated in §28.14's callee-side bullet (an ordinary borrow argument; a receiving callee's store is its own §28.3 admission) — of which the base-call disposition of a `scoped` primary parameter (§2.2, the base call bullet) is the instance; and the classification of a body reference to the capture as an ordinary receiver-sourced field read (§22.10).
- **Newly warned:** nothing.
- **Doc-only:** §28.14's parenthetical; Appendix A.4's table row and grammar comment (which also repairs the row's pre-existing omission of `scoped`); Appendix C.5's row; the code-generation reference's §4.2 bullet and pin; Guide 01's four-shapes paragraph and pin; `DECLINED.md`'s row amendment.
- **Spec-ahead-of-impl:** the parser bound (`Parser_Decls.cpp:283-292`) must narrow to function-typed `scoped` only; the test `scoped_primary_ctor_param_rejected_22_10` and corpus `s22_10_0011` flip. The tracking-issue text is in the lane report.

## 7. Implementation sketch

- `src/parser/Parser_Decls.cpp:261-292` — the `scoped` branch of the primary-constructor modifier loop: keep the rejection for a *syntactically function-typed* parameter type (A.5 `FunctionType` is recognizable at parse time) and drop it otherwise; the diagnostic text becomes "a function-typed `scoped` primary-constructor parameter is not permitted (§22.10) — declare a `scoped` field and a regular constructor". Value-typed `scoped` then reaches sema's existing `checkScopedIsFunctionTyped` (`src/sema/TypeResolution_Names.cpp:1349` for method parameters — confirm the primary-constructor parameter path at `:1311`, the field-symbol branch, reaches the same check; if not, route `FieldSymbol::posParam` with `modifier == "scoped"` through it).
- `src/sema/Symbol.hpp:747-766` — `posParamBodyRef`/`posParamInitRef` already classify references; the synthesized capture for a `scoped` posParam is a plain (non-owned) field, the branch the plain-parameter capture already takes. Nothing new to synthesize; verify the capture site (`src/sema/TypeResolution_impl.hpp:86`, `TypeResolution_Checks.cpp:5544`) does not key on `modifier.empty()`.
- `src/ownership/Ownership_Checks.cpp` — a read of the parameter name in a body is a read of the capture field of `this`: the same classification the ownership pass gives a captured plain parameter today (critique N-1 — a classification, not a restriction: the read is a receiver-sourced borrow, returnable and passable like any back-reference field's). The scoped-construction callee-side checks the 1545 tests pin (`this.src := src` admitted; return and other-object store rejected) apply to the *constructor's* uses of the parameter, which for a primary constructor are its initializer and base-call references only: a store into a non-owned field of `this` and a base-call forward are admitted, a plain argument is an ordinary borrow argument (§28.14's new clause). Caller side: `scopedFormalAt` (`:17635`) resolves the constructor's formals — a primary constructor's formals are the header's `positionalFields`; confirm `calleeOrChainFormals` (`:545`) / `resolveChainCtorParams` (`:509`) return them with the `scoped` modifier intact.
- Tests (spec-cited, fail-without/pass-with): the re-authored `scoped_primary_ctor_param_rejected_22_10` becomes an admitted-shape test plus a function-typed-rejected control; a disposal-order test mirroring corpus `s22_10_0019`; a freeze test (`s.Dispose()` under a live primary-constructed holder rejected); the value-typed control. Size: **S**.

## 8. Corpus accounting

| Case | Expectation | What it pins |
|---|---|---|
| `s22_10_0011` (re-authored) | `accept` | the admitted shape: body-referenced class-typed `scoped` primary parameter, `using var` source declared before, `new Box(scoped s)` at a `using var` holder |
| `s22_10_0016` | `reject` (`diagnostic: temporary`) | a fresh producer at the scoped primary-constructor argument site (§9.7's no-temporary-source rule) — twin of `0011` |
| `s22_10_0017` | `reject` (`diagnostic: scoped`) | `scoped` on a value-typed primary parameter — the §28.14/A.4 type restriction, untouched by the lift |
| `s22_10_0018` | `accept` | capture-iff-used: a `scoped` primary parameter referenced in no body captures no field; the call site is still admitted |
| `s22_10_0019` | `run`, `output: box` / `source` | disposal order — holder before source (§21.6 rule 1, §28.10, §28.4) |
| `s28_14_0019` | `reject` (`diagnostic: bounding object`) | a plain-`var` holder resolves no admitted bounding object — declared-constructor shape, independent of this lift, closing a §28.14 coverage gap the 1545 test has and the corpus lacked |

`baseline.tsv`: `s22_10_0011` is banked `pass` against the *old* expectation; at the landing it must be re-banked (it will read `fail` — the compiler has not caught up — until the parser change lands). The five new cases land unclassified.

## 9. Open questions

- **Q1 — scope of the lift:** class/interface-typed only (recommended), or both forms with the synthesized `scoped` field and the class inheriting quiescence?
- **Q2 — the base call:** is passing a `scoped` primary parameter to a *plain* base-constructor parameter an ordinary borrow argument (recommended, §2.2), or should it be rejected as a store outside the admitted forms because the base may store it under the trust regime? The same question exists today for a declared constructor forwarding to any plain parameter, so the recommendation is to state the current reading rather than invent a base-call-specific rule.
- **Q3 — records' body constructors:** `record R … public constructor(scoped x: T)` — outside this proposal; is it rejected today? (No probe possible in this environment; `scoped_construction_1545_test.cpp:234` pins only the record-*typed* parameter rejection.) Flagged for the critic as a pre-existing question, not a lift.
- **Q4 — the diagnostic on a value-typed primary `scoped` parameter:** today the parser rejects it on the §22.10 ground; after the lift sema rejects it on the type ground. Corpus `s22_10_0017` cannot tell the two apart (both contain `scoped`); the classification run's diagnostic report will.
