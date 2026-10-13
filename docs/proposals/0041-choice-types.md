# 0041 - `choice` types: closed nominal sums with payloads

**Status:** graduated-v0.200 — superseded — graduated v0.200 (all four stages: the kind + owning classification + serialization + stdlib `Result<T, E>`)
**Tracking issue:** [#2028](https://github.com/s8lap/bezel-lang/issues/2028)
**Target:** Spec — one new top-level section ("Choices", placed after §10 Enums; final number is the landing editor's mechanical choice) plus scoped edits to §3.1/§3.2/§3.6 (type kinds, storage classes, derived-property table), §8.6/§8.7 (boxing, `Dispose`), §9.7 (borrow-carrying transitivity), §9.8 (serialization synthesis, stage 3 only), §14.2 (conformance clause; `new()` consequence), §15.3 (move-only category membership), §17.11 (`default`), §19.3/§19.6 (patterns, exhaustiveness), §28.1 (`owned` positions; the v0.196 erasure list), §28.3 (producers rows, owned-consuming positions, consume-site freeze list), §28.11 (element category), §30.14 (sendable), §32.5 (FFI not-allowed list), Appendix A.2/A.3/A.7, Appendix B.1, Appendix C (C.1/C.3/C.7 rows), Appendix D.2. Stdlib — one new `System` section for `Result<T, E>` (stage 4 only). Rationale — decision records (a new §9-family entry; the §18 sum-types deferred entry resolved; pointers from §5.14/§5.15).
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

This proposal expands item **P1** of [`future-directions-2026-07-18.md`](papers/future-directions-2026-07-18.md) to graduation depth. Where it sharpens or diverges from the P1 sketch, the divergence is recorded explicitly in §11.

## 0. Summary

A **`choice`** is a closed nominal sum: a value type whose values are drawn from a fixed, named set of *cases*, where each case may carry a positional payload. Conceptually it sits between enums (closed, named, no payloads) and records (open field product, no case discrimination); in §3.1's list ordering the new kind lands between enums and classes (§2.2). Construction is qualified (`FetchOutcome.Failed(504, "upstream timeout")`); consumption is `case` with positional destructuring and full static exhaustiveness (no `_` needed when every case is covered).

The part only Bezel has to solve — and the part Bezel is best equipped to solve — is ownership. The design derives a three-way classification from the payloads, exactly as §9.7 derives a record's borrow-bearing status from its fields: **value-like** choices are ordinary copyable values; **borrow-bearing** choices get §9.7's lifetime tracking verbatim; **owning** choices (some payload marked `owned`) join §15.3's move-only value-type category — the extension rationale §5.14 explicitly left the door open for — and carry a disposal obligation that the existing affine machinery (§28.3 discharge, auto-drop, drop-on-overwrite, exception-path discharge) satisfies with **no new runtime concept**. Matching a borrowed choice gives payload borrows (§19.3 unchanged); matching an owned choice in an announced-consuming form transfers owned payloads into the arm's bindings, mirroring owning iteration (§29.2) and the v0.180 transfer-marker discipline.

The stdlib ships exactly one choice: `Result<T, E>`. `Option<T>` is explicitly declined — `T?` is Bezel's option type, and two spellings of absence would be a wound.

## 1. Problem and evidence

### 1.1 The current encodings and their costs

Bezel's own error-handling guide steers users toward "return the outcome, don't throw" for expected failure (Guide 2 §5: "Exceptions are for the unexpected"), and then the language makes the outcome-return awkward. The three current encodings, with their costs verified against v0.197:

1. **Nullable return `T?`** (Guide 2 §5's first idiom). Covers presence/absence; cannot carry *why*. A parse failure that wants to report a position and message has nowhere to put them.

2. **The Try pattern** (Guide 2 §5's second idiom; the stdlib-wide convention of stdlib §1's Try bullet, v0.171–v0.195). A `bool` return, plus an `out owned conn: Connection? when true` parameter, plus `out using var` at the call site. This works — but count the machinery: the §22.3 `out`/`out owned` parameter kinds, §22.4's inline-`out` declarations, §22.4's *nullability-postcondition clause* (`when true`, v0.183 — a bespoke signature clause invented largely so callers can skip the `!` that the nullable-out shape forces), the §17.11 rule that makes the out nullable in the first place (no `default` for a non-nullable class), and stdlib §1's four sanctioned Try shapes (a)–(d) with a recorded history of one *unimplementable* shape shipping by accident (the pre-v0.171 non-nullable `out owned` form). Four interacting features and a convention document, to express "value or failure," and the failure side still carries only what a second out-parameter can.

3. **`sealed abstract` class hierarchies** (§19.6, rationale §9.5). The real sum-type workhorse today — closed, exhaustively matched, cross-module sound. But heavyweight for a *return type*: three class declarations, heap allocation per outcome, reference semantics where value semantics are wanted, type-test patterns instead of destructuring (§19.3: "Destructuring applies to records only"; class leaves are matched as `var c: Circle`), and — since v0.196 — the generic-leaf *determination rule*, a page of necessary machinery (§19.6 "Generic leaves") that exists precisely because class-hierarchy sums encode the discriminant in the *runtime type*, where generic instantiation makes coverage subtle. A `choice`'s discriminant is a case *name*, not a type, so the entire determination problem does not arise for it (§2.7 below).

The gap is not expressiveness — everything is encodable — it is that the language's *stated domain* (services, tools, parsers, protocol code) has "expected failure with information" as its bread-and-butter shape, and the natural spelling is missing.

### 1.2 The deferral has expired

Rationale §18 defers sum types with one sentence of reasoning: "the design space (Swift, Rust, Scala 3) is large and unsettled." That assessment has expired. The space has converged on exactly the shape every mainstream entrant now ships: **closed nominal sums, payload-carrying cases, exhaustive matching**. What remains genuinely open across languages — GADTs, anonymous/structural unions, pattern synonyms — is exactly what this proposal does *not* take (§10).

### 1.3 Every hard ingredient already exists in v0.197

- **Exhaustiveness over closed sets** — §19.6 (enum bullet, sealed-hierarchy bullet, the v0.182 guarded-arm posture).
- **Positional destructuring with per-slot discards and alternatives** — §19.3, grammar A.7 (`QualifiedName "(" PositionalPatternList ")"`).
- **Derived type properties computed from component types, per-instantiation for generics** — §3.6 rules 1–4; §9.7 (borrow-bearing), §30.14 (sendable), §9.2 (auto-equality).
- **The move-only value-type category** — §15.3, with rationale §5.14's explicit open door: "A future revision may extend the move-only category if user-defined move-only types prove necessary." This proposal is that necessity.
- **Per-instantiation category-dependent semantics** — rationale §5.15's recorded "type-parameter-category-dependent specialization" (v0.102, `OwningBuffer<T>`/`Span<T>`/`ReadOnlySpan<T>`), with its own recorded liftable bound: "Future revisions may generalize the mechanism if other types benefit."
- **Announced-consumption forms** — the owning-iteration yield (§29.2 "Iterator yield ownership", v0.184), Pattern B await (§30.5, v0.184), the v0.180 transfer marker (§28.3 "Move semantics for owned locals").
- **The full-expression boundary with the `case`-scrutinee extension** — §28.3 "Unbound owned temporaries" (v0.193), which already keeps a `case` scrutinee's temporary alive through arm selection and the selected arm's completion.
- **Conformance staging for the move-only category** — Appendix D.2's "Move-only value types" row (v0.188, proposal 0036).

Nothing in this proposal requires new theory. It is composition.

## 2. The design

### 2.1 Declaration

```bezel
public choice FetchOutcome
  Fetched(owned response: Response),      // owned class payload
  NotModified,                            // payload-free case
  Failed(status: int, message: string);   // value payloads

  public method IsSuccess(): bool => case this of
    Fetched(_)   => true,
    NotModified  => true,
    Failed(_, _) => false
  end;
end FetchOutcome;
```

Proposed normative text (the new section's opening):

> A **choice** is a value type whose values are drawn from a fixed, named set of **cases**. Each value has exactly one **active case** at a time; the active case determines which payload components the value carries. A case declares zero or more positional **payload fields**, each `Identifier ":" Type` with an optional leading `owned` marker; a case with no payload list is **payload-free**. Case names are PascalCase by convention and must be distinct within the choice; a case name may not collide with any member name of the choice (the [§11.3] indistinguishable-members rule applies with cases counted as members). A choice declares at least one case. Choices are implicitly sealed and cannot derive from any type; a choice may implement interfaces (§2.10 of this proposal). Choices cannot have destructors, constructors, instance fields, block-form or storage-backed properties, indexers, or nested types — the same restriction set as enums ([§10.5]), for the same reasons, plus one: a choice's instance storage *is* its cases' payloads, declared in the case list and reachable only through matching.
>
> A choice payload field may not, directly or transitively through value-typed components (record fields, inline-array elements, other choices' payloads), have the declaring choice's own type: a value type cannot contain itself. Recursive sums are expressed with a class node in the cycle (an `owned` class payload whose class holds the nested choice), exactly as recursive records are.

The `owned` payload marker's admissibility follows §28.1's type requirement verbatim: admissible on class types, interface types, and bare type parameters; admitted-but-redundant on move-only value types; a compile error on any other type. For a **bare type parameter** payload the marker follows §28.1's v0.196 per-instantiation erasure — the payload slot is owned at class/interface substitutions and the marker is erased at value and runtime-managed substitutions. (Case-payload position is added to §28.1's position list; this is the entire spec edit the generic story needs — §2.12.)

### 2.2 Placement in the type system

§3.1 gains one kind between enums and classes:

> - **Choices** — user-defined value types whose values carry one of a fixed set of named cases, each with optional payloads. (§ the new Choices section)

§3.2's value-type sentence gains "choices" alongside records and enums, with the owning classification (§2.5) called out as belonging to the move-only sub-category, exactly as owning inline arrays are today: "(Owning inline arrays *and owning-classified choices* are a special sub-category — move-only value types …)".

§3.6's derived-property table gains one row:

| Property | Defined in | Classifies |
|---|---|---|
| **choice classification** | the new Choices section | every choice type, as value-like, borrow-bearing, or owning — the classification on which a choice's copyability, lifetime tracking, and disposal obligation depend |

and the existing **borrow-bearing** row's "Classifies" column extends to "records (and inline arrays, choices)". §3.6's common rules 1–4 apply unchanged: computed never declared; per-instantiation for generics; part of the public contract (adding an `owned` payload to a public choice is a breaking change exactly as adding a destructor to a class is); named in diagnostics.

### 2.3 Members: the enum surface, and why

A choice body may declare, after the case list, exactly what an enum body may (§10.4, grammar A.4 `EnumMember`): instance methods, expression-bodied read-only properties, static methods, static readonly fields (including `static owned readonly`), `const` members, and operator overloads. `this` in a choice instance method is a **borrow** of the receiver value; owned-consuming operations through `this` are rejected (the same reasoning as §19.3's move-only scrutinee rule: a method invocation must not silently consume its receiver, and for a generic choice this keeps method-body checking independent of the instantiation's classification — §2.12). Choice methods are statically dispatched; `virtual`/`abstract`/`override` are rejected as on enums (§10.5), and the §10.5 `Equals`/`GetHashCode`/`ToString` plain-member replacement rule carries over for the boxable classifications.

**Why methods at all?** Three reasons, in strength order. (1) The enum precedent is direct and its restriction history is instructive: enums started without interface conformance and gained it in v0.61 because the restriction was "artificial — there is no semantic obstacle" (rationale §9.4); declining methods here would repeat that mistake with a known outcome. (2) The stdlib's own `Result<T, E>` wants `IsOk()`/`IsError()` (§5), and the alternative — extension blocks on every choice — pushes a type's basic surface out of its declaration. (3) The `case this of` expression body composes cleanly (§19.7: `case … end` is self-delimited), so choice methods cost the grammar nothing beyond the `EnumMember` reuse.

**Why not constructors, fields, or storage properties?** A choice's construction *is* case selection — a user constructor would have to pick a case, which the qualified case form already spells better; independent instance storage alongside the payload union would make every value part-product-part-sum, destroying the "exactly one active case" invariant that exhaustiveness and disposal both lean on.

### 2.4 Construction and payload argument ownership

Construction is qualified, resolving exactly as enum constants do (§10.1): `FetchOutcome.NotModified` for a payload-free case (a value expression, not a call), `FetchOutcome.Failed(504, "upstream timeout")` for a payload case. Inside a `case` over a choice scrutinee the prefix may be omitted in patterns (§2.6), but in *expression* position the qualified form is required, exactly as for enum constants outside their scrutinee. For a **generic** choice the type arguments are written at the construction (`Result<int, string>.Ok(value)`): §14.5's posture — "class/record-level type arguments are never inferred from constructor arguments" — extends to case construction unchanged. Return-type-directed inference for the common `return …Ok(v);` shape was considered and deliberately left out of v1 (it is target-typing of a *type's* arguments, a mechanism the language reserves for literals; recorded liftable if the spelling proves heavy in practice).

Payload arguments bind to the case's payload fields as to a parameter list, and the `owned` payload marker has exactly an `owned` parameter's call-site semantics (§22.3, §28.3):

```bezel
var resp := new Response(headers, stream);
return FetchOutcome.Fetched(owned resp);      // call-site marker; resp is moved-from after
```

- An argument to an **owned payload slot** is an owned-consuming position: a fresh producer or unbound owned temporary transfers with no marker (no live binding is invalidated); a named owned binding requires the §22.3 call-site marker (`owned resp`) and is left moved-from; the consume-site freeze (§28.3, v0.180) rejects the transfer while a live local borrow of the binding exists. `owned` payload slots join §28.3's owned-consuming-positions list by reference to the `owned`-argument bullet.
- An argument to a **plain class- or interface-typed payload slot** is a borrow, and §9.7's **no-temporary-source rule** applies verbatim: constructing a borrow-bearing choice from a temporary bound to a borrow-carrying payload is rejected (bind the source to a named local first).
- Value, `string`, and runtime-managed payload arguments copy/share per their type kinds.

The constructed value's own ownership follows its classification: a value-like construction is an ordinary value; a borrow-bearing construction is a borrow-carrying value under §9.7's rules; an owning construction is an **owned move-only value** — §28.3's producers table row "Function call producing a move-only value type" covers it verbatim (the caller takes ownership and must discharge by binding, storing, returning, or consuming it), and a construction not bound to any name is an unbound owned temporary under §28.3's full-expression machinery.

A method returning an owning choice writes the plain type (`method Fetch(url: string): FetchOutcome`); `owned` on the return is admitted-but-redundant per §28.1's move-only clause, exactly as for owning inline arrays.

### 2.5 The three-way derived classification

Proposed normative text, modeled sentence-for-sentence on §9.7:

> The **classification** of a choice type is a derived type property ([§3.6]) computed from the payload fields of *all* its cases:
>
> 1. A choice is **owning** if any case has a payload field marked `owned` (a class, interface, or — at a class or interface instantiation — bare-type-parameter payload), or a payload field of a move-only value type ([§15.3] — an owning inline array or another owning choice, transitively).
> 2. Otherwise, a choice is **borrow-bearing** if any case has a *borrow-carrying* payload field, under exactly [§9.7]'s definition: a plain (unmarked) field of class or interface type (nullable or not); an inline-array field with reference-typed elements; a borrow-bearing record; or — transitively — a borrow-bearing choice.
> 3. Otherwise the choice is **value-like**: every payload is a value, a `string`, a runtime-managed type, or (transitively) a value-like record or choice. A value-like choice is an ordinary copyable value type.
>
> Payloads of runtime-managed reference types (`string`, function types, boxed values) are classification-neutral, exactly as in [§9.7]: such values have unbounded lifetime by construction.

**Borrow-bearing choices** are subject to §28.3's borrow-lifetime rules through §9.7's existing bullet list, applied verbatim with "record" read as "record or choice": may not outlive any carried borrow's source; may not be constructed from a temporary source; may not be stored in a class instance field or (outside the null-source form) a static field; constrained returns, closures, and collection copies; forbidden as `async` parameters and results (§30.1). A live local of borrow-bearing *choice* type counts as a live local borrow of every carried borrow's source for the consume-site freeze, exactly as the v0.186 sentence states for records. No new rule text is needed beyond the word extension — this is the cheapest of the three classifications to specify.

**Owning choices join §15.3.** §15.3's opening sentence ("The single move-only value type kind in this specification is the owning inline array") is replaced by a two-kind enumeration, and its structure-not-declaration paragraph gains the choice clause. Every §15.3 rule then applies to owning choices as written: local-to-local assignment moves (unmarked — copies are impossible, so there is nothing for a marker to disambiguate); parameter passing is by value-move; return transfers; a move-only value may be a class field (the class's destructor disposes it) and may not be a record field; locals are auto-disposed at scope end; exception edges discharge via the implicit `try`/`finally`; `using var` is admitted with pin-only effect.

Two owning-choice rules are *stronger* than the inline-array precedent, deliberately:

- **Disposal is defined by the active case**: disposing an owning choice value disposes the active case's `owned` payloads (in payload declaration order) and, recursively, any move-only payloads; payload-free and value-payload cases dispose nothing. This is the whole disposal story — there is no per-slot moved-from state to consider, because payloads are reachable only through matching, never as assignable slots.
- **Drop-on-overwrite is defined and safe.** Whole-value reassignment of an owning-choice local or field *disposes the displaced value* per §28.7, like an owned class binding — **not** like §15.3's owning-inline-array carve-out (the warning-backed manual discipline, tracking #1385). The carve-out exists because an inline array's slots can be individually moved-from, leaving the aggregate in a partial state the compiler cannot cheaply dispose; a choice has no partial states — the value is always wholly owned or wholly moved-from — so the sound automatic behavior is available and is taken. Reassignment is a consume of the displaced value and joins the consume-site freeze accordingly. (This also means `x := null;` on an owning `C?` local disposes the displaced value's payloads — §2.8.)

Owning choices integrate with §28.3's discharge machinery with no new mechanism: an owned owning-choice local reaches a discharge form (transfer, store into a move-only-typed class field, return, an explicit consuming match — §2.6) or is **auto-dropped at scope end** with the must-use warning, and exception paths discharge implicitly. The §28.3 diagnostics name the classification per §3.6 rule 4.

### 2.6 Match semantics

#### 2.6.1 The borrowing match (the default; §19.3 unchanged)

Matching a choice value **non-destructively** is the default everywhere, and §19.3's rules apply verbatim: a pattern binding is a borrow whose source is the scrutinee's source; the scrutinee retains its ownership state; the construct never moves from the scrutinee. For a value-like choice, payload bindings are copies (value payloads) or shared references (`string`/runtime-managed payloads) per the standard type-kind rules; for borrow-bearing and owning choices, class-typed payload bindings are borrows. An **unmarked read of a named owned binding in scrutinee position stays a borrowing match** — this is exactly §19.3's existing v0.184 rule for move-only scrutinees ("the binding is a borrow … the case construct never moves from the scrutinee"), now load-bearing: it is what makes *peeking* at an owned outcome legal and non-consuming.

```bezel
var outcome := client.Fetch(url);       // owned move-only local
case outcome of                          // borrowing match; outcome unchanged
  Failed(var s, _) when s >= 500 => ScheduleRetry(),
  _                              => ()
end;
// outcome is still owned here; discharge or auto-drop per §28.3.
```

#### 2.6.2 The owning match

An **owning match** transfers the selected case's `owned` payloads into the arm's bindings and consumes the scrutinee value. It is triggered in exactly two scrutinee forms — both *announced*, in the §28.3 sense that something at the site tells the reader the value dies:

1. **An unbound owned temporary** of owning-choice classification — a fresh construction, an owning-choice-returning call, a `move` result. This is the dominant shape and needs no marker, as fresh producers never do (no live binding is invalidated):

   ```bezel
   case client.Fetch(url) of                 // owning match over the temporary
     Fetched(var r)       => RenderOwned(owned r),   // r: owned Response in the arm
     NotModified          => RenderCached(),
     Failed(var s, var m) => ShowError(s, m)
   end;
   ```

   The scrutinee temporary's full expression is the **whole `case` construct** — §28.3's v0.193 `case`-scrutinee extension, applied unchanged — so payloads not transferred into bindings (discarded slots like `Fetched(_)`, and the payloads of the value when a `_` or non-destructuring arm is selected) are disposed when the construct completes, by the same unbound-temporary pass that disposes the container. An exception out of an arm disposes the not-yet-consumed remainder during unwinding (§28.3 "Unbound owned temporaries", exception clause).

2. **A named owned binding under an explicit scrutinee transfer marker** — `case owned outcome of …`. This is the v0.180 marker discipline (§28.3 "Move semantics for owned locals") extended to one more position, with its exact spelling and its exact rules: the marker is required because the unmarked read is a borrow (bullet 2.6.1), an unmarked read never silently consumes, the marker on a borrowed or fresh-producer scrutinee is an error, and the consumed binding is left **moved-from** when the construct completes. Because the transfer hands payloads **alive** to the arm's bindings — no disposed flag is set, nothing faults — the consume **joins the consume-site freeze list**: `case owned x of …` while a live local borrow of `x`'s current value exists is a compile error, precisely the reasoning that put the owning-iterator `yield return` on the freeze list (§29.2, v0.184) and kept Pattern B await off it (§30.5's await sets the disposed flag; this construct does not).

   ```bezel
   var outcome := client.Fetch(url);
   case owned outcome of                     // marked consume; outcome moved-from after `end`
     Fetched(var r) => do
       using var resp := owned r;            // adopt: scope-bound disposal (§28.10)
       Render(resp);
     end,
     NotModified          => RenderCached(),
     Failed(var s, var m) => ShowError(s, m)
   end;
   ```

In an owning match, an `owned`-payload binding (`var r` above) is an **owned local of the arm body's scope**, and §28.3's standard rules apply to it on every arm-exit edge — exactly the §29.2 owning-iteration-variable model, word for word: falling off the arm still owning the value auto-drops it with the must-use warning naming the binding and the standard remedies (`using var u := owned r;` adoption in a `do … end` arm, an owned-consuming call, an explicit `r.Dispose()`, `_ := r;`); exception edges discharge implicitly. In *expression-position* arms (which cannot contain statements, §19.2), the warning-free discharges are an owned-consuming call (`RenderOwned(owned r)`) or §19.2's helper-method extraction; this is the same expression-position pressure §19.2 already documents, not a new constraint. Value payloads bind as copies; discarded owned slots (`Fetched(_)`) are disposed at construct completion as above. Nested destructuring of an owned payload does not arise: owned payloads are class- or interface-typed, and classes do not destructure (§19.3).

**What does not trigger an owning match**, for the record: an unmarked named-binding scrutinee (borrowing, above); a borrowed scrutinee of any provenance (field read, parameter, `this`); and the position of the *construct's result* — whether the `case`'s value flows into an owned slot is the §28.3 arm-meet producers row's business and has no bearing on how the scrutinee is consumed. Coupling the match kind to the enclosing expression context was considered and declined (§11, divergence D2): it would make the binding modes *inside* the arms depend on context arbitrarily far outside the construct.

#### 2.6.3 Guards: borrow during trial, transfer on selection

A guard runs while arm selection is still undecided, and a declined guard must leave the value intact for later arms. Normative rule:

> In an owning match, pattern bindings are **borrows of the scrutinee's payloads during pattern matching and guard evaluation**; ownership transfers into the selected arm's bindings **when an arm is selected** (its pattern matched and its guard, if any, evaluated true). A guard therefore observes borrows and cannot consume a binding (owned-consuming positions reject a borrow); after selection, the arm body sees the bindings in their owning-match states.

This two-phase rule is the only sound reading that keeps guards available — the alternative, forbidding guards on owning-match arms, was considered and declined as a real expressiveness loss (`Failed(var s, _) when s >= 500 => Retry()` over a consumed outcome is legitimate) with no compensating simplification: the implementation must in any case defer the physical transfer past guard evaluation, since a declined guard falls through to the next arm.

If **no arm** of an owning match selects: in statement position the construct completes with no action and the consumed value's payloads are disposed at construct completion (the value was consumed by the announced form; its contents have nowhere to go); in expression position `MatchFailureException` is raised (§19.6) and the payloads are disposed during unwinding by the standard exception-path machinery. Exhaustive matches — the normal case, §2.7 — never reach either.

#### 2.6.4 Case patterns

- A **payload-free case** is matched by its bare name (`NotModified`), a constant-style pattern resolving through the scrutinee-type prefix-omission rule (extended from enums, §10.6/§19.8: within a `case` whose scrutinee is a choice, the choice prefix may be omitted). Grammar A.7's `Constant ::= … | QualifiedName` and the name-resolution disambiguation note already carry this — a case name resolves to a case, not a type, so the type-test reading never fires.
- A **payload case** is matched by a positional destructuring pattern `CaseName(slot, …)` with the case's exact payload arity — grammar A.7's existing `QualifiedName "(" PositionalPatternList ")"` production, unchanged. §19.3's destructuring sentence is amended from "applies to records only" to "applies to records and choice cases"; all slot rules carry over: naked identifiers bind, `var x` binds explicitly, `_` discards per slot, per-slot `|` alternatives among binding-free members, nested patterns recurse.
- A payload case may **not** be matched bare (`Fetched` without parentheses is a compile error naming the arity) — matching a payload while silently ignoring it should be visible as `Fetched(_)`, the same explicitness §19.3 demands of record destructuring arity.

#### 2.6.5 Worked disposal traces (the four paths of an owning match)

The graduating text should carry these traces (or their equivalents) as the section's examples, because they are where implementers and reviewers will look for the exactly-once argument. All four ride existing machinery; none introduces a new disposal site kind.

**(a) Normal path, payload bound and adopted.**

```bezel
case client.Fetch(url) of
  Fetched(var r) => do
    using var resp := owned r;   // r moved into resp; resp disposed at arm-body end
    Render(resp);
  end,
  NotModified          => RenderCached(),
  Failed(var s, var m) => ShowError(s, m)
end;
```

The scrutinee temporary is produced owned (the move-only-call producers row); arm selection transfers `response`'s ownership into `r`; the adopt idiom moves it into `resp`, disposed at the `do … end` block's close; the container value, now payload-empty, is reclaimed at construct completion with nothing left to dispose. Exactly one disposal of the `Response`, at a statically known site.

**(b) Normal path, payload discarded.**

```bezel
case client.Fetch(url) of
  Fetched(_)  => Count("hit"),     // owned payload never bound
  _           => Count("other")
end;
```

The `Fetched(_)` discard binds nothing; the payload stays with the consumed scrutinee value and is disposed at construct completion by the unbound-temporary pass — the same "disposal runs after every use within the expression" ordering §28.3 already defines. The `_` arm on a `NotModified`/`Failed` value disposes nothing (those cases carry no owned payloads).

**(c) Guard-declined path.**

```bezel
case owned outcome of
  Fetched(var r) when r.IsStale() => do _ := r; RefetchLater(); end,
  Fetched(var r)                  => do using var resp := owned r; Render(resp); end,
  _                               => ()
end;
```

During the first arm's guard, `r` is a **borrow** (§2.6.3) — `r.IsStale()` is an ordinary borrowing call, and a consuming use in the guard would be rejected. If the guard declines, no transfer has happened; the second arm's selection performs it. Two arms binding the same payload are two candidate *recipients*, not two owners: transfer happens once, at selection.

**(d) Exception path.**

```bezel
case owned outcome of
  Fetched(var r)       => do Validate(r); Publish(owned r); end,   // Validate may throw
  NotModified          => (),
  Failed(var s, var m) => throw new UpstreamException(m)
end;
```

If `Validate(r)` throws, `r` — an owned local of the arm-body scope — is disposed by §28.3's exception-path discharge exactly as any owned local is; the consumed scrutinee (already payload-transferred) has nothing further to dispose. If the `Failed` arm's `throw` runs, the scrutinee's payloads (`s`, `m` — values) need no disposal; for a hypothetical owned payload in a throwing arm, the same unwind pass covers it. The moved-from `outcome` binding is skipped by every pass, as always.

#### 2.6.6 Diagnostics

Per §3.6 rule 4 and §28.3's diagnostics discipline, the new errors name the classification and the rule. Recommended messages, for the graduating text's diagnostics list:

- *"Payload case `Fetched` requires a payload pattern: write `Fetched(...)` with 1 slot, or `Fetched(_)` to discard."* (Bare payload-case pattern, §2.6.4.)
- *"`case owned` requires a named owned binding of an owning choice type as its scrutinee; `x` is borrowed."* / *"… is a fresh producer, which needs no marker."* (Marker misuse, §2.6.2 — mirroring §28.3's marked-borrow-RHS error.)
- *"Cannot consume `outcome` here: the borrow held by `b` (declared at line N) is still live."* (The owning match on the freeze list — §28.3's existing message, reused verbatim.)
- *"`FetchOutcome` is an owning choice (owned payload `response` of case `Fetched`); it cannot be boxed / is not sendable / does not satisfy `new()`."* (Property-keyed rejections, each naming the payload that decides the classification — §3.6 rule 4's "structural reason".)
- *"Case arm does not cover `Failed`: its slot pattern `>= 500` is refutable; static exhaustiveness counts only irrefutable-slot arms."* (The §2.7 one-level rule, made legible.)
- The §28.3 must-use warning at an owning-match binding's implicit drop names the binding and the arm-position remedies (§2.6.2).

### 2.7 Exhaustiveness

§19.6's static-proof list gains one bullet, drafted to normative quality:

> - **Choice scrutinee:** every declared case is covered by an unguarded arm — a payload-free case by a constant-style pattern naming it, directly or as a member of a `|` alternative list; a payload-carrying case by a positional destructuring pattern for it whose every slot is **irrefutable** (a binding or `_`). Guarded arms contribute nothing (the v0.182 rule), and an arm whose slot patterns are refutable (a constant, range, relational, alternative, or nested destructuring slot) covers only part of its case's value space and likewise contributes nothing to the static proof; coverage is counted per case name, one level deep. When every case is covered, no `_` arm is required — including across module boundaries: the case set is closed by the declaration, so the [§19.6] cross-module rule applies with "leaf type" read as "declared case", and adding a case to a published choice is a source-breaking change for consumers exactly as adding a sealed leaf is.

Two properties of this rule are worth calling out because they are *simpler* than the machinery the sealed-hierarchy encoding needs:

- **Generic choices need no determination rule.** For `case r of Ok(var v) => …, Error(var e) => … end` over a `Result<int, ParseError>` scrutinee, coverage is by case *name*; the bindings simply take the substituted payload types. The entire §19.6 v0.196 generic-leaves apparatus — root instantiations, inhabiting substitutions, rigid parameters, the `?`-pre-image split — exists because class-hierarchy sums put the discriminant in the runtime *type*; a choice's discriminant is a name, and the problem dissolves. (The spec's own §19.6 example hierarchy — `Result<T>`/`Ok<T>`/`Err<T>` as sealed classes — is the shape this proposal gives a first-class spelling; the landing editor may want to keep that example as-is for the class rule and add the choice contrast beside it.)
- **One-level counting is deliberate.** Nested refutable patterns (`Ok(0)`, a choice case inside a slot) are admitted syntactically and contribute nothing statically, exactly as guards do. A multi-level coverage solver (usefulness/exhaustiveness over pattern matrices) is real machinery with real diagnostic-quality obligations; v1 declines it, recorded liftable — a program wanting nested totality writes nested `case` constructs, which the compiler *does* prove exhaustive level by level.

**`|` alternatives.** §19.3's alternative-member rule is amended to admit **payload-free choice case names** alongside constants, ranges, and relationals: `NotModified | Failed(_, _)` remains illegal, but for a choice with several payload-free cases, `Idle | Closed => Reset()` is a single arm. The defense: §19.3's recorded reasoning (rationale §3.8, cited in §19.3) excludes *types* from alternatives because types alter exhaustiveness and binding analysis; a payload-free case is exactly constant-shaped — nullary, binding-free, name-resolved, and *countable toward exhaustiveness* the same way enum constants in alternative lists already are (§19.6's enum bullet: "directly or as a member of a `|` alternative list"). Payload-carrying case patterns stay excluded from alternatives even in the all-discard form `Fetched(_)`: they are destructurings, not constants, and admitting them buys one comma at the cost of a new pattern-class distinction ("binding-free destructuring") the language does not otherwise need. Separate arms spell it today.

### 2.8 Nullable `C?`

`C?` is the ordinary nullable value type (§7.2): a `C?` scrutinee adds §19.6's explicit-`null`-arm requirement on top of the choice clause, `?.`/`??`/narrowing compose as for any nullable, and boxing strips the wrapper per §8.6's nullable-value-type rule (for the boxable classifications).

For an **owning** `C`, the wrapper `C?` is itself move-only (it contains a move-only value), with two stated consequences: `null` assignment into an owned-state `C?` location is drop-on-overwrite — the displaced value's payloads are disposed (§2.5; sound because choices have no partial states, unlike #1385's inline-array carve-out) — and `default(C?)` is a valid null move-only value, the precedent being §17.11's `default([16 owned]Connection?)`. The polymorphic-null-state row of §28.3's producers table applies to `var x: C? := null;` unchanged.

### 2.9 Equality, hashing, `ToString`

The §9.2 package extends to choices as one derived property, with the same use-site error discipline:

> A **value-like** choice is **auto-equality-bearing** ([§3.6], the [§9.2] property) when every payload field of every case is an [§9.2] **admissible component type**, with one addition to the recursive rule: another auto-equality-bearing choice is admissible. The generated `=` is true exactly when both operands have the same active case and every corresponding payload compares equal by its type's `=`; the generated `GetHashCode` combines the case discriminant with the per-payload hashes (order-sensitive, [§8.3]); the generated `ToString` produces `"TypeName.CaseName(field1: value1, …)"`, and `"TypeName.CaseName"` for a payload-free case. Methods, computed properties, statics, and non-equality operators neither qualify nor disqualify, per [§9.2]'s membership list.
>
> A choice that is **not** auto-equality-bearing — any borrow-bearing or owning choice (class-typed payloads are outside the admissible-component rule, exactly as class-typed record fields are), and any value-like choice with an inadmissible payload — draws [§9.2]'s **use-site compile error** on `=`, `<>`, and direct `Equals`/`GetHashCode` calls, unless the author declares the operators under [§25.2]'s co-declaration rule. Declaring the choice is fine; comparing it is the error, with the same remedies [§9.2] names.

The recommendation for borrow-bearing/owning is deliberately **not equatable by default**, and one recorded tension must be surfaced honestly: §15.3 *does* give the other move-only kind (owning inline arrays) borrowing equality "provided their element types support equality" — which, for class elements, means per-slot *reference* equality. This proposal follows the **record** precedent instead of the inline-array one, for the reason §9.2's own design note gives: reference equality smuggled inside a value type's `=` "is virtually never the intended semantics," and a `Failed`-vs-`Fetched` comparison that answers `false`-or-`true` by pointer identity of a `Response` is the exact footgun. An author who wants it writes the operators; the language does not guess. (Owner decision Q4.)

Consequences that fall out via §3.6: value-like auto-equality-bearing choices are **equality-bearing** and usable as hash-container keys; the others are not (the §8.3/§9.2 equality-bearing row, unchanged).

### 2.10 Boxing and interfaces

§8.6 extends by classification, each arm riding an existing paragraph:

- **Value-like** choices box per the general rule; the box is runtime-managed.
- **Borrow-bearing** choices box under §8.6's "Boxing of borrow-bearing records" paragraph verbatim (the boxed copy carries the same borrows, extends no lifetime, may not escape the sources' scopes).
- **Owning** choices **cannot be boxed** — §8.6's move-only paragraph, word for word: boxing copies, move-only forbids copying. A program that needs an owning choice's contents to cross a boxed-value boundary transfers the payloads out through a consuming match first.

A choice may declare interface conformance (the `Inheritance` slot, §3 grammar; interfaces only, like records — §9.6), providing matching public members per §10.5's enum model. For an **owning-classified** choice the conformance declaration remains legal — constraint satisfaction (`where T: IFoo`, §14.2 conformance clause 2/4 extended with a choice clause) and generic dispatch work without boxing — but the *conversion* of an owning value to an interface or `Object` type is rejected at the conversion site (it is a boxing, forbidden above). For a generic choice this is per-instantiation (§3.6 rule 2): `Result<T, E>: SomeInterface` would convert at value-like instantiations and reject at owning ones. The stdlib's `Result` declares no conformances, deliberately (§5). `Dispose` stays off the surface everywhere: §8.7's list ("Static classes, records, enums, primitives, and inline arrays do not implement `IDisposable`") gains choices — an owning choice's disposal obligation is discharged by its *holder* (scope end, owning field, consuming match), never by calling `Dispose()` on it, exactly as for owning inline arrays.

### 2.11 `default`, `with`, and the small surface

- **`default(C)` is a compile error for every choice type.** A sum has no principled zero: electing the first-declared case would make program meaning depend on declaration order (reordering cases — otherwise behavior-neutral — would change every `default`), and restricting eligibility to first-case-payload-free would make *eligibility* order-dependent, which is worse. The enum precedent (underlying-0 with a warning) does not transfer — an enum value *is* its integer (§10.3) and 0 exists whether or not named; a choice value is a case, and no case is distinguished. Consequences stated in the spec text: choices never satisfy `where T: new()`'s value-type arm (§14.2 keys it on `default`); `serialized skip` on a choice-typed component requires the nullable form (§9.8's existing skip rule reaches this outcome by itself). Programs wanting an empty state declare a payload-free case and construct it explicitly, or use `C?`. Recorded liftable if evidence arrives; owner decision Q8.
- **`with` does not apply to choices.** §9.5 requires a record-typed source; a choice's payloads are not independently replaceable storage (replacing one field of `Failed` is constructing a new `Failed`). The §9.5 class-source diagnostic pattern extends: a `with` on a choice-typed source is a compile error naming the remedy (construct the case anew).
- `nameof(FetchOutcome.Failed)` yields `"Failed"` (§17.9's qualified-name rule); `typeof(FetchOutcome)` is ordinary; `is`/`as` over choice types follow value-type rules (§17.4) and are rarely useful (the static type is always exact — choices are sealed).

### 2.12 Generics

A choice may declare type parameters and constraints (`GenericParams?`/`GenericConstraints*` in the grammar, as for records). Payload fields may use the parameters. Classification is evaluated **per instantiation** under §3.6 rule 2, with the payload marker's meaning fixed by §28.1's v0.196 erasure (case-payload position joins §28.1's position list — §2.1): `Result<T, E>` with `Ok(owned value: T)` is value-like at `T = int, E = ParseError` (markers erased, payloads admissible values), borrow-bearing never (the marker never reads as a borrow), and owning at `T = Connection` (marker means owned). An *unmarked* bare-type-parameter payload is classification-neutral at value/runtime-managed substitutions and **borrow-carrying** at class/interface substitutions — the same reading as a concrete unmarked class payload, making the marked/unmarked split uniform: unmarked class payload = borrow (borrow-bearing choice), marked = owned (owning choice), at every level of genericity.

Method bodies of a generic choice type-check once, against `this`-as-borrow (§2.3), so their checking is classification-independent; classification-dependent *admissibility* rules (boxing conversions, sendability, equality, conformance-conversion) are enforced at the instantiation or use site with §3.6 rule 2's diagnostics naming the property, the offending type argument, and the structural reason.

**The §5.15 precedent, engaged head-on.** §3.6's reservation paragraph states that dispatching *behavior* (not merely admissibility) on a derived type property "is reserved to compiler-known standard-library types … not a user-available mechanism in this revision," with the C-compatible property as the one scoped exception. A choice's classification does flip behavior per instantiation — copy vs. move on assignment, disposal vs. none at scope end — for a *user-declared* type. This proposal therefore amends the reservation with a second scoped exception, and the scoping is the defense: the dispatch is performed **by the language on a language construct** — derived from structure, identical for every user, with no user-programmable hook — precisely the character of the `OwningBuffer<T>` specialization that rationale §5.15 recorded as generalizable "if other types benefit." It is *not* the user-available behavior-dispatch mechanism §3.6 fences off (users still cannot write code conditioned on a type parameter's category); it is one more compiler-known specialization, now keyed to a declaration form instead of three stdlib names. The future-directions document's own watch item (P1's note under "Reunifying the `Owning*` family") already flags this as the small, user-visible version of the v0.97-era dispatch — the amendment text should cite it. (Owner decision Q9 confirms the wording.)

**No variance.** Variance in Bezel attaches to interface and function-type parameters only (§12.9, §14.3); choices, like records and classes, are invariant, and this proposal adds nothing. Stated for completeness because sum types attract variance requests: `Result<Derived, E>` is not a `Result<Base, E>`, and the conversion is an explicit re-wrap by match.

### 2.13 Sendability

§30.14's value-type bullet already decides most of this, and the proposal states the composed result explicitly in the sendable section:

> A choice is **sendable** iff it is value-like and every payload field's type is sendable — the [§30.14] value-type rule ("not borrow-bearing and not move-only") with the per-field transitivity the classification computation already supplies; a sendable choice crosses as a copy. Borrow-bearing choices are never sendable (they carry origin-domain borrows). **Owning choices are not directly sendable in this revision** — the [§30.14] move-only exclusion, unchanged — but the transfer-closure rule 2 for sendable *classes* gains the parallel clause it has for owning inline arrays: an owning-choice-typed field whose every `owned` payload class is itself a sendable class type is an admissible field of a transfer-closed class, and crosses inside its owner's ownership tree.

The brief's one-line formula ("sendable iff all payloads sendable") is thereby *sharpened*, not contradicted: for value-like choices the two statements coincide; for owning choices with sendable payloads the simple formula would admit direct crossing, and this proposal deliberately does not — direct sendability for move-only values is a single future widening that should be decided once, for owning inline arrays and owning choices together, not smuggled in here (§11, divergence D4; owner decision Q10).

### 2.14 FFI

Choices are **not FFI-compatible** in v1. §32.5's "Not allowed" list gains "choices" alongside records-other-than-§32.13-forms and enums, with the same posture: the honest C spelling of a sum (a tag field plus a union, with the tag discipline by convention) is a real design with real ABI questions (union layout, tag width, niche packing), and §32.13's C-compatible-record machinery is the future home if demand materializes — a `C-compatible choice` would need its own layout promise on the §32.13 model. Nothing in the v1 design forecloses it; nothing in v1 pays for it.

### 2.15 Serialization synthesis (§9.8 extension — staged separately)

A choice opts in by declaring `: ISerializable` / `: IDeserializable` (stdlib §2.14), exactly as records and enums do. Eligibility and wire shape:

> A choice is **serialization-eligible** when it is value-like and every payload field's type is serialization-eligible **and record-admissible** ([§9.8]'s record clause — value-shaped storage throughout; a class-, interface-, or heap-collection-typed payload is exactly what the record clause rejects, and the borrow-bearing/owning classifications are therefore never eligible — the same `Deserialize`-has-no-owner argument applies verbatim). An ineligible conformance declaration is the [§9.8] compile error at the declaration, naming the first offending payload.
>
> **Wire shape.** The synthesized `Serialize` writes an object whose distinguished leading field is the **case discriminator** `"$case"` — the case's declared name, or its `serialized "<name>"` rename — followed by the active case's payload fields by name, in declaration order, each encoded per its [§9.8] component rule; a payload-free case writes the object with only the discriminator. The `$case` field name joins the `$`-reserved set of stdlib [§2.15]'s JSON mapping (`$id`, `$ref`, `$type`); a payload rename beginning with `$` is a compile error. The synthesized `Deserialize` reads the discriminator **first**, consumed by case dispatch before any component binding — it is structural, not a data field, and is never subject to `serialized optional` or the unknown-field policy — exactly the [§9.8] polymorphic-registry discipline; an undeclared discriminator string raises `SerializationException`; the selected case's payloads then read under the standard missing/duplicate/unknown rules, and the value is constructed by the case's own construction form. The `serialized` clause on a **case** admits the rename form only (a case is not omittable — the enum-constant rule); on a **payload field** the full [§9.8] vocabulary applies (`skip` requiring a defaultable payload type).

Worked JSON forms for `FetchOutcome` (value-like variant, for illustration — the real `FetchOutcome` above is owning and therefore ineligible; the example type for the spec text should be a value-like one):

```
{"$case": "Failed", "status": 504, "message": "upstream timeout"}
{"$case": "NotModified"}
```

and round-trip rules by example: reading `{"$case": "Failed", "message": "x"}` raises `SerializationException` (missing required component `status`); reading `{"$case": "Gone"}` raises `SerializationException` (undeclared discriminator); reading `{"status": 504, "$case": "Failed", …}` is **also** an error — the discriminator is the *leading* field on write and is required first on read (the §9.8 `$type` discipline: dispatch must select the case before component binding can begin; a tolerant-field-order reading was considered and declined because the binary format's stream shape and the object-graph identity machinery both assume dispatch-before-components).

The wire form is chosen for the same reasons §9.8 chose names over integers for enums: stable under case reordering, self-describing in text formats, and — via the leading-discriminator convention — implementable by independent readers without lookahead. A choice all of whose cases are payload-free still writes the object form (uniformity; such a type should usually be an enum, and the guide can say so).

This entire subsection is **stage 3** (§9): serialization synthesis is itself an Extended feature (Appendix D.2), the wire vocabulary is frozen stdlib surface, and nothing in stages 1–2 depends on it.

## 3. Grammar delta (Appendix A)

**A.2** — `TopDecl` gains the alternative:

```
TopDecl         ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl
                  | ChoiceDecl
                  | ExtensionDecl | TypeAliasDecl | ExternBlock
```

**A.3** — one new declaration family, reusing `RecordDecl`'s positional-field shape (per the mandate) extended with the payload `owned` marker, and `EnumDecl`'s member surface:

```
ChoiceDecl      ::= Visibility? "choice" Identifier GenericParams?
                    Inheritance?
                    GenericConstraints*
                    ChoiceCases
                    ChoiceMember* "end" Identifier? ";"
ChoiceCases     ::= ChoiceCase ("," ChoiceCase)* ";"
ChoiceCase      ::= Identifier CasePayload? SerializedClause?
                              /* SerializedClause on a case admits the rename
                                 form only ("serialized \"name\""); `skip` and
                                 `optional` are rejected semantically, as on
                                 enum constants (§9.8). */
CasePayload     ::= "(" CaseField ("," CaseField)* ")"
CaseField       ::= "owned"? Identifier ":" Type SerializedClause?
                              /* The payload field form is RecordDecl's
                                 PositionalField plus the optional leading
                                 `owned` marker; the marker's admissibility
                                 follows §28.1's type requirement, including
                                 the v0.196 bare-type-parameter erasure.
                                 Semantically, Inheritance names interfaces
                                 only (as for records). */
ChoiceMember    ::= MethodDecl | ExpressionPropertyDecl
                  | StaticReadonlyDecl | StaticConstDecl
                  | OperatorDecl
                              /* Identical to EnumMember; kept as a named
                                 production so the sections can restrict
                                 independently if they ever diverge. */
```

**A.7** — one production edit and one comment edit:

```
CaseExpr        ::= "case" "owned"? Expression "of"
                    CaseArm ("," CaseArm)* ","? "end" "case"?
                              /* The optional `owned` is the scrutinee
                                 transfer marker (§2.6.2): admitted only when
                                 the scrutinee is a named owned binding of
                                 owning-choice classification; anywhere else
                                 it is a compile error — the standard
                                 over-admit-and-reject pattern. `owned` is a
                                 reserved keyword, so no expression can begin
                                 with it and the parse is unambiguous. */
```

The `Pattern` production is **unchanged**: payload-free cases ride `Constant`'s `QualifiedName` alternative and payload cases ride the existing `QualifiedName "(" PositionalPatternList ")"` destructuring production; the name-resolution disambiguation comment gains one clause (a name resolving to a choice case is a case pattern). `AltPattern`'s binding-free-members comment gains payload-free case names (§2.7). **A.8** is unchanged.

**Appendix B.1** — `choice` joins the unconditionally reserved keyword list (§4). **Appendix C** — C.1 gains the choice declaration restrictions and the no-self-containment rule; C.3 gains the owning-choice rows (exactly-once disposal of the active case's owned payloads; boxing rejection; drop-on-overwrite disposes); C.7 gains the choice-exhaustiveness and owning-match-freeze rows.

## 4. The keyword, and the collision scan

`choice` is proposed as a **fully reserved** keyword (B.1), not a contextual one: it introduces a type declaration, exactly the position class every existing declaration introducer (`class`, `record`, `enum`, `interface`, `extension`) occupies as a full keyword, and a contextual introducer would buy nothing but parser lookahead.

The B.3 discipline ("new keywords … drawn from … words not currently used as identifiers in the standard library") was verified by scan, 2026-07-18, against v0.197:

- **Stdlib document:** every occurrence of `choice` is English prose ("the choice of algorithm", "the right choice when…"); zero type, member, parameter, or constant uses.
- **Spec document:** prose only (including §19.6's "the choice is locally decidable" style); no grammar token, no example identifier.
- **Tests:** 8 files contain the word, all in C++ comments ("Design choice recorded…"); zero occurrences as an identifier inside embedded Bezel source.
- **Guides:** one prose occurrence (Guide 1), zero in Guide 2; none as identifiers.

Reserving it is therefore migration-free in this repository (§8). The alternatives `union` (C-freighted: readers expect untagged memory overlay) and widening `enum` (§10) are declined; `choice` also reads correctly at the use site the feature is for — a `case` over a `choice` is the sentence the design wants users to say. (Owner decision Q1.)

## 5. Standard library: `Result<T, E>` — and only `Result`

One new auto-imported `System` section (placement after §2.9 `IEquatable<T>` suggested; stage 4):

```bezel
public choice Result<T, E>
  Ok(owned value: T),
  Error(owned error: E);

  public method IsOk(): bool => case this of
    Ok(_)    => true,
    Error(_) => false
  end;

  public method IsError(): bool => not IsOk();
end Result;
```

Design points, each decided:

- **Unconstrained `T`, `E`, with `owned` payload markers.** Under §28.1's v0.196 erasure (extended to payload position, §2.1), `Result<int, ParseError>` is value-like (markers erased; Core, copyable, auto-equality-bearing when the payloads are admissible), while `Result<Connection, IoError>` is owning per instantiation — the classification-3-per-instantiation shape P1 named, delivered by the same machinery that made `Zero(): owned T` spellable in stdlib §2.19 (v0.196). No constraints are imposed: constraining `T: class` would amputate the dominant value-payload uses; constraining categories apart would fork the type.
- **Case names `Ok` / `Error`.** Full-word PascalCase per the stdlib naming rules (stdlib §1's acronym/abbreviation conventions; the stdlib does not abbreviate — `Err` would be its only abbreviation). `Error` collides with no stdlib type (the exception family is `*Exception`). (Owner decision Q6.)
- **`IsOk`/`IsError` only; no `Unwrap`, no combinators.** `Unwrap()`-style faulting extractors are declined: a method receives `this` as a borrow (§2.3), so an owned-payload extractor is *unspellable as a method* — correctly, since extraction is consumption and consumption is the owning match's job; the three-line `case` is the extractor, and it is exhaustiveness-checked where `Unwrap` would fault. `Map`/`AndThen` combinators are declined for v1 with reasons recorded: over owned payloads they need ownership-polymorphic function parameters (`(owned T) -> owned U` vs `(T) -> U` per instantiation), which is the retired-and-deferred owned-generic-argument territory of §14.4/rationale §18; over value payloads they save one `case` at the cost of opening exactly that door. Liftable when evidence arrives, as extension blocks on `Result` if the ownership story stays value-only. *(Amendment, docs v0.233 — proposal 0075 Q4, owner-ruled 2026-07-29: the recorded extension-block route proved structurally unavailable — §26.2 never admitted choices to the extendable list, and no §14.2 constraint names the value-like bound — so the value-payload combinators landed instead as compiler-provided per-instantiation members, spec §35.2 / stdlib §2.20. Register amendment only; the decline's reasoning above is unchanged.)*
- **No conformances declared.** Keeps `Result` classification-clean at every instantiation (§2.10) and defers nothing that matters: equality at value-like instantiations is automatic.
- **Naming note for the landing editor:** the spec's §19.6 generic-leaves example declares example-scope classes named `Result<T>`/`Ok<T>`/`Err<T>`; user shadowing of stdlib type names is legal (§3.4's non-reservation note), but the example should be renamed or annotated at landing to avoid teaching the shadowed spelling.

Worked usage at both classifications — the two shapes the stdlib section should show:

```bezel
// Value-like instantiation (Core): parse outcome with information.
public method ParsePort(text: string): Result<int, string>
  if not int.TryParse(text, out var value) then
    return Result<int, string>.Error($"not a number: {text}");
  end;
  if value < 1 or value > 65535 then
    return Result<int, string>.Error($"out of range: {value}");
  end;
  return Result<int, string>.Ok(value);
end ParsePort;

case ParsePort(arg) of
  Ok(var p)    => Listen(p),
  Error(var m) => Console.WriteLine($"bad port: {m}")
end;                                  // exhaustive; no _ needed
```

```bezel
// Owning instantiation (Extended): connect-or-explain.
public method Connect(host: string): Result<Connection, string>
  var conn := TryOpen(host);          // illustrative TryOpen(host): owned Connection?
  if conn = null then
    return Result<Connection, string>.Error($"unreachable: {host}");
  end;
  return Result<Connection, string>.Ok(owned conn);    // conn narrowed non-null (§7.4);
                                                       // owned payload slot: marker consumes
end Connect;

case Connect(host) of                 // owning match over the temporary
  Ok(var c) => do
    using var conn := owned c;        // adopt; disposed at block end
    conn.Send(payload);
  end,
  Error(var m) => Console.WriteLine($"giving up: {m}")
end;
```

(`ParsePort` deliberately *uses* the shape-(c) `TryParse` internally and *returns* a choice: the two idioms compose — Try stays the right shape for value extraction with a boring failure, and the choice return is where the failure carries information.)

**`Option<T>` is explicitly declined.** `T?` is Bezel's option type: it has language-level syntax (`?.`, `??`, narrowing, patterns, §19.6's nullable clause), stdlib-wide integration (the Try shapes, `FirstOrNull`), and a design lineage the rationale defends throughout. Shipping `Option<T>` beside it would create two spellings of absence, two conversion questions at every API boundary, and an eternal style war — the "wound" P1 names. A `choice` with one meaningful payload and one empty case is what `T?` already is. This decline should land in `DECLINED.md` alongside D-0002.

## 6. Conformance staging (Appendix D)

Recommended split, on proposal 0036's precedent:

- **Value-like and borrow-bearing choices: Core.** These are the everyday outcome types — `Failed`-style results, parser outcomes, protocol states — and they exercise only Core machinery (value semantics, §9.7-style borrow tracking, patterns, exhaustiveness). Everything not listed in D.2 is Core by default; no D.2 row is added for them.
- **The owning classification: Extended**, as new clauses on D.2's existing **"Move-only value types"** row (the row 0036 created): the `owned` payload marker's owning reading, §15.3 membership, the owning match (§2.6.2–2.6.3), the freeze rows, the drop-on-overwrite rule, and the transfer-closure field clause of §2.13. D.3's dependency note extends symmetrically: the row still depends only on Core, and no other Extended feature depends on it.

The defense is 0036's, transplanted: the move-only category is the language's deepest value-semantics machinery, a Core implementation loses **no** program a Core user writes (an outcome type whose payload is an owned `Connection` is inherently resource-management territory — the same programs already need §28's full owned-class machinery, which *is* Core, but the move-only *value* packaging of it is the separable, clause-bounded part 0036 already demoted), and D.1's clean-rejection rule gives Core implementations a crisp behavior: a choice declaration or instantiation whose classification is owning is rejected with the feature-naming diagnostic. One consequence stated plainly: `Result<Connection, E>` is an Extended-level program while `Result<int, E>` is Core — a per-instantiation conformance boundary, which D.2's move-only row already has today via `where T: value`'s move-only instantiation checks (the row's §7.10/§14.2 citations). (Owner decision Q5.)

## 7. Interactions (the honest enumeration)

| Feature | Interaction | Status in this design |
|---|---|---|
| §3.1/§3.2 type kinds | New kind; value type; owning sub-category move-only | §2.2; one-line edits |
| §3.6 derived properties | New classification row; borrow-bearing row extended; **reservation paragraph amended** (behavior dispatch, second scoped exception) | §2.2, §2.12; Q9 |
| §5 conversions | No implicit conversions to/from choices; no user conversion operators (§25.4 unchanged) | Nothing to do; stated |
| §7 nullability | `C?` ordinary; owning `C?` move-only wrapper; null-assignment disposes | §2.8 |
| §8.3/§8.4/§8.5 | Equality/hash/ToString via the §9.2 package; §10.5's plain-member replacement carries over | §2.9 |
| §8.6 boxing | Per classification; owning cannot box | §2.10 |
| §8.7 | Choices join the no-`IDisposable` list | §2.10 |
| §9.7 | Definition reused verbatim (borrow-carrying fields); rules extended by word choice ("record or choice") | §2.5 |
| §9.8 serialization | `$case`-discriminated object form; eligibility value-like only; stage 3 | §2.15 |
| §10 enums | Untouched; the guide gains a "no payloads ⇒ enum" pointer | §2.15 note |
| §14.2 constraints | `new()` never satisfied (no `default`); conformance clause gains choices; `where T: value` admits choices with move-compatible-operations discipline for possibly-owning instantiations | §2.11, §2.10, §2.12 |
| §14.4 / §28.1 | No `owned` generic arguments (unchanged); **case-payload position joins §28.1's list** with the v0.196 erasure | §2.1, §2.12 |
| §15.3 | Category gains second kind; owning choices *escape* the reassignment carve-out (#1385) with reasons | §2.5 |
| §17.11 `default` | Compile error for all choices | §2.11; Q8 |
| §17.13 discard | `_ := c;` on an owning-choice value: the move-only discard clause of §17.13 applies verbatim (disposes at end of statement) | Falls out; stated in spec text |
| §19.3 patterns | Destructuring extended to cases; alternatives admit payload-free cases; move-only scrutinee borrow rule now load-bearing | §2.6.1, §2.6.4, §2.7 |
| §19.6 exhaustiveness | New choice bullet; no determination rule needed for generic choices | §2.7 |
| §22.3/§28.3 markers | Payload arguments = owned-parameter semantics; scrutinee marker `case owned x of` joins the v0.180 family; owning match joins the consume-site freeze and the owned-consuming positions list | §2.4, §2.6.2 |
| §28.3 producers | Construction rows covered by existing move-only-call row; arm-meet row unaffected (a `case`'s *result* ownership is orthogonal to its scrutinee's match kind) | §2.4, §2.6.2 |
| §28.3 full expression | v0.193 `case`-scrutinee extension is exactly the owning-temporary-match lifetime rule | §2.6.2 |
| §28.5/§28.7 | Owning choice as class field: disposed by destructor (move-only field rule); drop-on-overwrite defined | §2.5 |
| §28.11 element category | Owning choices take the move-only element row: rejected by `OwningBuffer<T>`/`Span<T>`/owning collections (§14.4's move-only exclusions), admissible as class fields | §2.5; stated in spec text |
| §29/§30 | An owning choice as an iterator/async value follows move-only rules (§29.8/§30.1's frame clauses for move-only parameters apply); borrow-bearing choices forbidden as async parameters/results per §9.7/§30.1 verbatim | Falls out; stated |
| §30.14 sendability | Value-like: per payloads; owning: field-admission via transfer-closure rule 2, no direct sendability | §2.13; Q10 |
| §32 FFI | Not FFI-compatible; §32.5 list edit | §2.14 |
| §22.4 / Guide 2 | The Try pattern remains valid; the guide's §5 gains the choice-return alternative and the Try shape becomes one idiom among two | §8 |
| Appendix A/B/C/D | Per §3, §4, §6 | — |

Known **implementation** hot spots, flagged for the impl issues at landing: the two-phase guard rule (§2.6.3) needs the transfer commit point in the match lowering; the per-instantiation classification needs the §3.6 rule-2 plumbing that `OwningBuffer<T>` already has; the layout (tag + payload union, niche-packing freedom) is implementation-defined and deliberately unspecified beyond value-type semantics.

## 8. Migration and compatibility

**Purely additive.** No existing program changes meaning: `choice` is unused as an identifier anywhere in the repository (§4's scan), so reserving it breaks nothing here (and the project has no external users — CLAUDE.md); no existing grammar production is narrowed; no existing type's classification, layout, or rules change; every edit to shared sections (§19.3, §19.6, §28.3, §30.14, …) is a new clause keyed on a type kind that no existing program can name. The stdlib gains one type in stage 4; nothing is renamed or removed. The Try convention (stdlib §1) and `sealed abstract` hierarchies remain fully supported — this proposal adds the third spelling and repositions the guides' *recommendations* (Guide 2 §5 and §6's table gain the choice-return row as the first choice for expected failure with information), which is a docs-revision matter, not a compatibility one.

## 9. Staging

Each stage is a separately gateable spec train per `spec-change-process.md`; stages 1–2 are the language, 3–4 ride behind it.

1. **Stage 1 — the kind, Core surface.** Grammar (§3 minus nothing), the new section (declaration, taxonomy, classification for value-like + borrow-bearing, construction, borrowing match, exhaustiveness, equality, boxing, nullable, `default`/`with`, generics, sendability for value-like, FFI exclusion), §19.3/§19.6 edits, B.1 reservation, C rows, rationale records. Compiler follow-up issues filed at landing per house practice.
2. **Stage 2 — the owning classification, Extended.** §15.3 membership, `owned` payload semantics, the owning match (§2.6.2–2.6.3), §28.3 list edits, drop-on-overwrite, D.2 row clauses, transfer-closure field clause. Depends on stage 1 only.
3. **Stage 3 — serialization synthesis** (§2.15): §9.8 extension + stdlib §2.15 `$case` reservation. Depends on stage 1; independent of stage 2.
4. **Stage 4 — stdlib `Result<T, E>`** (§5) + guide repositioning + the `DECLINED.md` Option entry. Depends on stages 1–2 (the `owned` payload markers) for full generality; a value-only interim is deliberately not offered (shipping `Result` twice with different markers would be a breaking wire/API change).

Sizing: stage 1 is the large train (a new type kind: grammar, one new section, pattern/exhaustiveness edits — comparable to a mid-sized historical revision); stage 2 is medium (list edits into §28.3's machinery, each individually small but soundness-critical); stages 3–4 are small.

## 10. Alternatives considered and declined

- **Widening `enum` with payload-carrying constants.** Declined. The enum contract is load-bearing and payload-hostile at every joint: an enum value *is* its underlying integer (§10.3 — a payload has no integer), explicit values and duplicate-constant aliasing (§10.2) are meaningless for payload cases, §5.4's integer casts and the "raw value" state cannot exist for sums, serialization writes bare name strings (§9.8) where payloads need structure, and every existing enum rule would need a "unless it has payloads" fork — the definition of a kind split hiding inside one keyword. Swift's `enum` proves the merged spelling *can* work, but Swift built it that way from day one; retrofitting it onto Bezel's integer-backed enums would break the §10 contract that FFI-adjacent and constant-heavy code relies on. Two keywords, two clean contracts.
- **Class-backed sums (status quo, or sugar over `sealed abstract`).** Declined as the *primary* answer; retained as the open-hierarchy answer. The heap allocation and reference semantics are wrong for outcome returns (P1's core complaint); destructuring is unavailable for classes by recorded decision (§19.3, rationale §5.17); and the v0.196 determination rule shows the generic-coverage cost of type-encoded discriminants. Sugar that *generates* hidden classes would inherit all of that while adding an abstraction layer the diagnostics would leak through. `sealed abstract` hierarchies remain the right tool for open-ish, behavior-carrying, identity-bearing sums — the design draws the line at value-shaped outcomes, and says so in the guide.
- **Structural / anonymous unions (`A | B` types).** Declined. Anti-nominal: no place to hang case names (exhaustiveness would key on types, resurrecting the determination problem and the §19.3 type-in-alternatives exclusion), unclear identity (`A | B` vs `B | A`, nesting flattening), an inference and subtyping surface that pervades the whole type system, and a direct collision with the project's recorded nominal-products posture (D-0002 anonymous types; the records-vs-tuples argument of rationale §18). A payload's *meaning* ("this `int` is a status") is exactly what nominal cases record and structural unions erase.
- **`Option<T>` in the stdlib.** Declined with reasons in §5.
- **Marker-free consumption of named owned scrutinees (the Pattern-B-await reading of P1's sketch).** Declined; reasons and the divergence record in §11 (D2).
- **A `ref`-struct-style new lifetime kind for borrow-bearing choices.** Never seriously on the table: §9.7's borrow-bearing-record machinery was built precisely so aggregates carrying borrows need no new type kind (rationale §5.15's span discussion), and choices ride it unchanged.

## 11. Recorded divergences from the P1 sketch

- **D1 — payload marker spelling.** P1's sketch writes `Fetched(response: owned Response)` — `owned` inside the type position. This proposal writes `Fetched(owned response: Response)`. Reason: `owned` is a declaration-slot marker, never a type constituent, everywhere in v0.197 (§28.1's position list; §14.4's rejection of `owned` in generic arguments; even function-type slots attach it to the slot, not the type). The sketch's spelling would create the language's only type-position `owned`; the parameter-shaped spelling reuses `Param`'s exact geometry and inherits §28.1's admissibility and v0.196 erasure rules for free.
- **D2 — the consumption trigger.** P1 recommends "scrutinee in an owned-consuming position ⇒ owning match; otherwise borrowing match — mirroring §30.5's operand rules." This proposal instead triggers the owning match on (i) unbound owned temporaries and (ii) an explicit scrutinee marker `case owned x of`, and keeps unmarked named-binding scrutinees borrowing. Three reasons. First, "owned-consuming position" is a property of where the *construct's result* flows; keying the scrutinee's consumption (and thereby the binding modes inside every arm) to context outside the construct is action at a distance the marker discipline exists to prevent. Second, §30.5's marker-free named-operand consume is justified there by the full announced-kill triad — the `await` keyword, a single-consumer-by-type operand, and the disposed-flag runtime backstop (§30.5, v0.184) — and two of the three are absent here: a choice is legitimately multiply-inspected (peeking must stay cheap and non-consuming), and a moved value sets no flag ("Transfer has none," §28.3). Third, scrutinee position *has* an unmarked borrow reading to disambiguate — §19.3's v0.184 move-only rule — so the v0.180 principle ("the transfer is spelled where it is not already announced") demands a marker; note this is exactly why the marker here is *not* the "admitted-but-redundant" no-op §28.1 makes it in other move-only positions, a divergence-inside-the-divergence recorded in the spec text.
- **D3 — payload-free cases in `|` alternatives.** P1 did not address alternatives; this proposal admits payload-free case names into them (and into exhaustiveness counting) while keeping destructurings out — §2.7's defense.
- **D4 — sendability sharpening.** The brief's "sendable iff all payloads sendable" is delivered exactly for value-like choices; owning choices are field-admissible in transfer-closed classes but not directly sendable (§2.13), keeping the move-only exclusion uniform with owning inline arrays rather than widening it for one kind.
- **D5 — equality for the non-value-like classifications.** P1 left equality open; this proposal declines it by default (the §9.2 record precedent) *against* the §15.3 inline-array borrowing-equality precedent, with the tension surfaced as Q4 rather than resolved silently.

## 12. Open questions / owner decisions

1. **Q1 — the kind ruling and the keyword.** Add the `choice` kind with keyword `choice` (vs. `union`, vs. widening `enum` — §10)? Recommended: **yes, `choice`** (collision scan clean, §4).
2. **Q2 — the owning-match trigger** (§2.6.2, divergence D2): explicit scrutinee marker `case owned x of` + fresh-temporary auto-consume (recommended), vs. P1's context-keyed reading, vs. Pattern-B-style marker-free named consume? Recommended: **the marker**; this is the proposal's one genuinely novel spelling and deserves an explicit ruling.
3. **Q3 — guards on owning-match arms**: two-phase borrow-then-transfer (recommended, §2.6.3) vs. rejecting guards on such arms?
4. **Q4 — equality for borrow-bearing/owning choices**: §9.2's use-site error (recommended) vs. §15.3-style borrowing structural comparison (reference-equality components)? The two recorded precedents conflict; §2.9 argues for the record side.
5. **Q5 — conformance staging**: owning classification to Extended on D.2's move-only row per 0036 (recommended, §6), accepting the per-instantiation Core/Extended boundary for generic choices?
6. **Q6 — `Result` case names**: `Ok`/`Error` (recommended) vs. `Ok`/`Err` vs. `Success`/`Failure`?
7. **Q7 — interface conformance in v1**: admitted with the owning-conversion rejection (recommended, §2.10) vs. deferred entirely on the enum v0.60→v0.61 path? The serialization opt-in spelling (stage 3) needs conformance declarations, so full deferral would force a bespoke opt-in later.
8. **Q8 — `default(C)` declined for all choices** (§2.11): confirm, or admit the first-case-payload-free form with the enum-style warning?
9. **Q9 — the §3.6 reservation amendment** (§2.12): confirm the wording that admits the choice classification as the second scoped exception to the behavior-dispatch reservation, citing rationale §5.15's generalization clause.
10. **Q10 — owning-choice sendability** (§2.13): confirm field-admission-only for v1, with direct move-only sendability recorded as a single future question shared with owning inline arrays.
11. **Q11 — exhaustiveness depth**: confirm one-level coverage counting (nested refutable patterns contribute nothing statically, §2.7), with multi-level usefulness analysis recorded liftable.
12. **Q12 — stdlib combinators**: confirm declining `Map`/`AndThen`/`Unwrap` for v1 with the recorded reasons (§5), or direct a value-payload-only extension-block subset. *(Amended at docs v0.233: the extension-block spelling is not available as recorded — see the §5 amendment note; the value-payload subset landed as compiler-provided members, proposal 0075 Q4.)*

## 13. Edit list (the graduating author's checklist)

Stage numbers per §9. Every bullet is a bounded edit; the new section is the only large one.

**Language spec** (stage 1 unless marked):

- **New top-level section "Choices"** after §10 (number at the editor's discretion): declaration and restrictions (§2.1/§2.3), construction and payload ownership (§2.4), the classification (§2.5's normative text; owning clauses stage 2), match semantics (§2.6, incl. the §2.6.5 traces and §2.6.6 diagnostics; owning match stage 2), equality package (§2.9), boxing/conformance (§2.10), `default`/`with` (§2.11), generics (§2.12), sendability (§2.13; owning field-clause stage 2), FFI exclusion pointer (§2.14).
- **§3.1**: one new kind bullet. **§3.2**: "choices" added to the value-type sentence; owning sub-category clause (stage 2).
- **§3.6**: new classification row; borrow-bearing row's "Classifies" column extended; the reservation paragraph's second scoped exception (§2.12, per Q9).
- **§8.6**: three classification arms added to the boxing rules (owning arm stage 2). **§8.7**: choices added to the no-`IDisposable` list.
- **§9.7**: the borrow-carrying definition's transitivity bullet gains "or a borrow-bearing choice"; the bullet list's "record" occurrences read "record or choice" (a wording sweep, no rule change).
- **§9.8** (stage 3): the choice eligibility clause, the `$case` wire form, the case/payload `serialized` admissions (§2.15).
- **§14.2**: the interface-conformance list gains a choice clause (patterned on the record clause 2 / enum clause 4); a one-sentence `new()` consequence (choices never satisfy the value-type arm — no `default`); the `where T: value` move-compatible-operations note extended to possibly-owning choice substitutions.
- **§15.3** (stage 2): two-kind enumeration; the structure-not-declaration paragraph's choice clause; a pointer to the choice section's drop-on-overwrite strengthening (and a cross-note at the #1385 carve-out).
- **§17.11**: choices added to the `default` compile-error list, with the §2.11 reasoning condensed. **§17.13**: no edit needed (the move-only discard clause already keys on §15.3 membership) — verify at landing, per 0039 §7's "recorded so a landing editor does not go hunting" practice.
- **§19.3**: destructuring sentence extended to choice cases; alternative-member rule gains payload-free case names; the move-only scrutinee paragraph gains the choice-borrowing-match cross-reference.
- **§19.6**: the choice-scrutinee bullet (§2.7); the cross-module subsection's one-sentence extension.
- **§22.3**: no edit — the call-site marker rule already speaks of "`owned` argument"; the payload slot *is* an owned parameter position by the new section's definition. Verify at landing.
- **§28.1**: case-payload position added to the admissible-positions list; the v0.196 type-requirement paragraph's position enumeration gains it.
- **§28.3** (stage 2): owned-consuming positions list gains the owning-match scrutinee forms; the consume-site freeze list gains the marked owning match and owning-choice reassignment; the producers table needs **no new row** (the move-only-call row and local-read rows cover constructions and reads — verify at landing); the unbound-temporaries `case`-scrutinee paragraph gains one sentence noting payload transfer into bindings.
- **§28.11** (stage 2): the element-category table's move-only row gains owning choices.
- **§30.14**: the value-type sendability bullet's parenthetical gains choices (stage 1); transfer-closure rule 2's owning-choice field clause (stage 2).
- **§32.5**: "choices" added to the Not-allowed list, with the §2.14 pointer.
- **Appendix A**: A.2/A.3/A.7 per §3. **Appendix B.1**: `choice` reserved. **Appendix C**: C.1/C.3/C.7 rows per §3. **Appendix D.2** (stage 2): the move-only row's section-list gains the owning-choice clauses; D.3's dependency sentence re-verified.

**Stdlib** (stage 4): the new `System` `Result<T, E>` section (§5); stage 3 adds the `$case` reservation sentence to §2.15's JSON mapping.

**Rationale**: a new "Choices" entry in the §9 family (why a new kind and not enum-widening/classes/structural unions — §10's content, condensed to the record); §5.14 and §5.15 gain forward pointers ("the anticipated extension landed as choices"); §18's sum-types entry is rewritten to point at the landed revision (pinned-history rules: the *changelog* narrates, §18's entry updates as the deferred-list convention already does for shipped items — see the static-interface-members entry's pattern).

**DECLINED.md**: the `Option<T>` entry (§5).

**Guides**: Guide 2 §5 gains the choice-return idiom as the first tool for expected failure with information; §6's table gains the row; Guide 1 gains an owning-match example in its ownership-transfer material. (Guides are docs-workflow items riding stage 4.)

**Issues at landing**: compiler implementation trackers per stage (the house one-issue-per-feature-area practice); the §19.6 example-rename note (§5); a cross-note on #1385 recording that owning choices deliberately do not share the carve-out.

## 14. Spec-derived test surface (first-PR material)

Per the repository's milestone flow (tests first, derived from the spec text with §-citations), the graduating revision's test PR should cover at minimum — grouped by the section that will be cited:

1. **Declaration & grammar**: each `ChoiceDecl` form (payload-free, payload, mixed, generic, with members, `end Name;` closer); rejection of instance fields/constructors/indexers/nested types/inheritance-from-class; case/member name collision; self-containment rejection (direct and through a record field); ≥1-case requirement; the `serialized` case-clause admissions and rejections.
2. **Construction**: qualified forms; payload arity/type errors; `owned` slot with fresh producer (no marker), named binding (marker required; unmarked = error), freeze violation at construction; borrow payload no-temporary-source rejection; generic construction requires explicit type arguments.
3. **Classification**: one test type per classification; transitivity (choice-in-record, record-in-choice, choice-in-choice); the runtime-managed-payload neutrality cases (`string`, function-typed payload); per-instantiation flips for `Result<T, E>` at value/class/interface/move-only substitutions; the `owned`-marker erasure at `T = int`.
4. **Borrowing match**: binding-is-borrow on borrowed/unmarked-owned scrutinees; scrutinee retained; borrow sources propagate (use-after-source-dispose caught in checked mode); prefix omission.
5. **Owning match**: temporary scrutinee (bound payload owned in arm; discarded payload disposed at construct end — ASan-verified per the memory-lane discipline); marked named-binding consume (moved-from after; unmarked read after = error); freeze rejection; guard-declined path (no transfer; no double-dispose); no-match disposal in statement position; `MatchFailureException` path disposal; exception-from-arm path; must-use warning at an undischarged arm binding; drop-on-overwrite disposes (and the reassignment freeze).
6. **Exhaustiveness**: full coverage without `_` (incl. via `|` payload-free alternatives); guarded arms don't count; refutable-slot arms don't count; missing-case warning text names the case; nullable scrutinee needs `null`; generic choice exhaustive with no determination machinery.
7. **Equality/boxing/misc**: auto-equality across cases (same-case equal/unequal, cross-case unequal, hash-container use); use-site error on borrow-bearing/owning `=`; `ToString` forms; boxing per classification incl. owning rejection; `default` rejection; `with` rejection; `where T: new()` rejection; sendability accept/reject at spawn and channel sites; FFI signature rejection.
8. **Serialization** (stage 3): round-trips per case shape; discriminator-first enforcement; unknown/missing/duplicate faults; renames; `skip`/`optional` on payloads; ineligibility errors at the conformance declaration.
9. **Conformance** (stage 2): a Core-mode build rejects owning declarations and owning instantiations with the feature-naming diagnostic (D.1).

## 15. Sizing

| Piece | Size |
|---|---|
| Spec: new section | Large — a full type-kind section, though §2's normative fragments above are most of its text |
| Spec: scoped edits | ~20 sections touched, each a bounded clause or list edit (§13); no existing rule rewritten |
| Grammar | Four productions added, one production and two comments edited; zero ambiguity introduced (`choice` and scrutinee-`owned` are keyword-anchored) |
| Stdlib | One new type (stage 4), one reserved-name sentence (stage 3) |
| Rationale | One new entry + three pointer edits |
| Compiler | The large cost, staged: stage 1 is parser + sema + a tag/payload lowering + exhaustiveness; stage 2 threads the classification through the ownership passes (the §28.3 list edits are small in the spec and real in the checker); stages 3–4 ride existing synthesis and stdlib machinery |
| Migration | None (§8) |
