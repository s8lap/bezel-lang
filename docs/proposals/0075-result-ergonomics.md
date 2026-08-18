# Proposal 0075 — `Result<T, E>` ergonomics: return-position target-typing, value-payload combinators, and the recorded unlocks

| | |
|---|---|
| **Status** | graduated-v0.233 — **superseded — stages 1–2 graduated v0.233** (owner-ruled 2026-07-29, Q1–Q7 resolved as recommended; recorded in the v0.233 changelog entry; §4 remains the stage-3 deferral register and 0061 §3 the propagation evidence log) |
| **Tracking issue** | [#2354](https://github.com/s8lap/bezel-lang/issues/2354) (stage 1), [#2355](https://github.com/s8lap/bezel-lang/issues/2355) (stage 2) |
| **Target module(s)** | Stage 1: **spec** — [§35.3](../Bezel_Language_Spec.md#sec-choice-construction) (construction), with cross-reference notes at [§21.7](../Bezel_Language_Spec.md#sec-the-return-statement) (`return`), [§22.2](../Bezel_Language_Spec.md#sec-expression-bodied-methods)/[§23.2](../Bezel_Language_Spec.md#sec-expression-bodied-read-only-properties) (expression bodies), and [§27.3](../Bezel_Language_Spec.md#sec-lambda-expressions) (ascribed lambda returns); **no grammar change** (Appendix A untouched — the forms already parse; the edit is name resolution and typing). Stage 2: **stdlib** — [§2.20](../Bezel_Standard_Library.md#sec-result) `Result<T, E>`; plus the one spec edit the chosen enforcement mechanism requires (§3.2 below — this is stage 2's real question). Stage 3: nothing — two recorded deferrals. |
| **Written against** | spec/stdlib v0.228 |
| **Origin** | Owner-directed consolidation (2026-07-28) of the scattered `Result<T, E>` ergonomics story — stdlib §2.20's recorded combinators blocker, proposal 0041's two recorded liftables, proposal 0061 §2's evidence-gated propagation study and §3's evidence log — into one home. The three-stage structure below was established by an adversarially-critiqued design review and is followed here. |

*Reference convention: unprefixed §-references are **language spec** references; stdlib and proposal references are marked.*

---

## 0. Summary and staging

`Result<T, E>` landed at v0.200 (stdlib [§2.20](../Bezel_Standard_Library.md#sec-result); proposal [0041](0041-choice-types.md) stage 4) with a deliberately minimal surface: the two cases, `IsOk()`/`IsError()`, and two recorded declines — construction target-typing ("recorded liftable if the spelling proves heavy in practice", 0041 §2.4) and `Map`/`AndThen` combinators (blocked on ownership-polymorphic function parameters, with "a value-payload-only extension-block subset" recorded as the acceptable interim, 0041 §5/Q12). Proposal [0061](0061-expression-form-studies.md) §2 additionally gated any propagation operator on observed — not predicted — per-call ceremony.

The first real `Result`-dense Bezel program has now been written, and 0061 §3's evidence log records that both gates' conditions fired at once: statement-position `case` per propagating call, and the full generic arguments restated on every construction (`Result<T, RetryError>.Ok(v)` nine times in two combinators) where the declared return type fixes both parameters. This proposal is the staged response:

1. **Stage 1 (proposed for acceptance)** — return-position target-typing of choice-case constructions: `return Ok(v);` where the declared result type is a choice with an `Ok` case. Spec-facing, typing only, zero ownership impact.
2. **Stage 2 (proposed)** — the four value-payload combinators (`Map`, `MapError`, `AndThen`, `UnwrapOr`) on non-owning `Result` instantiations — exactly 0041 Q12's recorded interim. Working out the enforcement mechanism turned up a real finding: **the recorded extension-block route has no clean enforcement in v0.228** (§3.2), so this stage's central question is *which* mechanism the owner prefers, and the recommendation diverges from Q12's recorded spelling with reasons.
3. **Stage 3 (explicitly deferred, recorded)** — the two real unlocks: (a) ownership-polymorphic function parameters, which would extend combinators to owning payloads; (b) any `?`-style propagation operator. Neither is designed here; both are recorded with their dependencies and their re-evaluation conditions.

## 1. Evidence and motivation

### 1.1 The observed call sites

0061 §3's log (2026-07-21 writer-seat sweep, a retry-library program — "the first real `Result`-dense Bezel written") records two frictions. Reconstructed representative shapes — the concrete program is not in this repository; the shapes below are spec-checked against v0.228 and match the log's description (`RetryError` is the program's own value-like record, illustrative here):

```bezel
public record RetryError(Attempt: int, Message: string);

// BEFORE (v0.228): every construction restates both generic arguments
// that the declared return type already fixes — the shape 0061 §3 logged
// nine times across two combinators.
public method NextDelay(attempt: int): Result<TimeSpan, RetryError>
  if attempt > _maxAttempts then
    return Result<TimeSpan, RetryError>.Error(new RetryError(attempt, "gave up"));
  end;
  return Result<TimeSpan, RetryError>.Ok(BaseDelay(attempt));
end NextDelay;
```

and the per-call propagation ceremony 0061 §2 gated on:

```bezel
// BEFORE (v0.228): one statement-position case per propagating call.
public method LoadPort(text: string): Result<int, string>
  var port := 0;
  case ParsePort(text) of                      // ParsePort: stdlib §2.20's example
    Ok(var p)    => port := p;,
    Error(var m) => return Result<int, string>.Error(m);
  end;
  return Result<int, string>.Ok(port);
end LoadPort;
```

After stage 1 the constructions shed the restatement (`return Error(new RetryError(attempt, "gave up"));`, `return Ok(port);`); after stage 2 the single-call propagation shape above folds into a pipeline (§3.5). Stdlib §2.20's own `ParsePort` listing — three constructions, three restatements — is the same shape in the normative document itself.

### 1.2 The honest evidence caveat

**One program has been observed.** The evidence base is a single writer-seat sweep of one retry library, plus the normative documents' own examples. That is exactly the bar the recorded gates set — 0041 §2.4's "if the spelling proves heavy in practice" and 0061 §2's "observed, not predicted" have both fired on their own terms, and the activation path is the established one: 0061 §1's binding atom was activated by explicit owner direction (deferred 2026-07-21, activated 2026-07-22 — changelog v0.216, with no logged evidence at all); this proposal requests the same owner ruling, here with the gates' own recorded evidence conditions additionally met. But one program is one program. Accordingly: **stage 1 is presented as ready to rule on** — its evidence is direct (the nine restatements), its mechanism rides an established precedent family (§2.5), and its blast radius is one resolution rule; **stage 2 is presented as recommended** — the combinator pain is observed (the program was *writing* combinators by hand), but the specific four-member surface is a judgment call, and its enforcement question (§3.2) deserves an explicit ruling rather than a default.

## 2. Stage 1 — return-position target-typing of choice-case constructions *(proposed for acceptance)*

### 2.1 The rule

Proposed normative text (a new paragraph in [§35.3](../Bezel_Language_Spec.md#sec-choice-construction), after the qualified-construction paragraph):

> **Declared-result target-typing.** In a **declared-result position** — the top-level operand of a `return e;` statement in a body with a declared return type ([§21.7](../Bezel_Language_Spec.md#sec-the-return-statement)); the expression body of an expression-bodied member with a declared type ([§22.2](../Bezel_Language_Spec.md#sec-expression-bodied-methods), [§23.2](../Bezel_Language_Spec.md#sec-expression-bodied-read-only-properties)); or a `return e;` / expression body checked against a lambda's **return-type ascription** ([§27.3](../Bezel_Language_Spec.md#sec-lambda-expressions)) — where the declared type — for an `async` body, its declared `Task<T>`'s result type, per §30.1's completion rule (worked in §2.4) — nullability stripped, is a **choice instantiation** `C<…>` (or non-generic choice `C`) that declares a case named `N`: the form `N(args)` is a **case construction** of `C<…>.N(args)`, and the bare identifier `N` is the payload-free case value `C<…>.N`. The declared type supplies the choice type and all its type arguments whole; nothing is inferred from the construction's arguments ([§14.5](../Bezel_Language_Spec.md#sec-type-argument-inference)'s posture is untouched). Payload-argument binding and ownership are exactly the qualified form's ([§35.3](../Bezel_Language_Spec.md#sec-choice-construction)); a declared nullable `C<…>?` target-types through the stripped form and the result converts by [§5.1](../Bezel_Language_Spec.md#sec-implicit-conversions)'s `T → T?` wrapping. When the declared type is not a choice, or is a choice without a case of that name, the form is an ordinary expression under ordinary resolution — nothing changes. Within a declared-result position whose type *is* such a choice, the case reading is **authoritative**: it wins over any same-named member or type in scope, with a shadowing warning when ordinary resolution would also have succeeded (remedy in the diagnostic: qualify the intended call, or bind it to a local first). The qualified construction remains valid everywhere.

For `Result<T, E>` this reads: `return Ok(v);` / `return Error(e);` in a body declared `: Result<int, string>` mean `Result<int, string>.Ok(v)` / `.Error(e)`.

### 2.2 Scope: exactly the declared-result positions, and why the `=>` body is in

The rule is deliberately keyed to positions where a **written declared result type** governs the expression — not to inference in general:

- **`return` statements** — the motivating position; all nine observed restatements are this shape.
- **Expression bodies (`=> expr`)** — *included, decided here*: an expression body is a return. [§27.3](../Bezel_Language_Spec.md#sec-lambda-expressions)'s ascription text already treats the two as one category ("each `return e;` (and an expression body) is checked against it under §5.1"), and the changelog's v0.217 entry describes the ascription as "behaving exactly as a declared method return." Excluding `=>` would make `public method Empty(): Result<int, string> => Result<int, string>.Ok(0);` pay ceremony its four-line spelling escapes — a seam with no principle behind it. The same reasoning admits **ascribed lambda returns** (`lambda(n): Result<int, string> => Ok(n)`, and `return Ok(n);` inside its block body): the ascription is a written declared result type, normatively equated with a method's (§27.3).
- **Not included:** lambdas whose return type arrives only by *target inference* (no ascription written — the type is not declared at the body, and threading it through [§14.5](../Bezel_Language_Spec.md#sec-type-argument-inference) phase 2 / [§22.6](../Bezel_Language_Spec.md#sec-overloading)'s lambda-ranking machinery would entangle case resolution with overload resolution); **argument position** (the [§17.7](../Bezel_Language_Spec.md#sec-the-new-operator) `new()` shorthand draws the same line: "not permitted in general expression contexts where the type would have to be inferred from broader context (overload resolution …)"); **`var` initialization** (no declared type exists); **typed local declarations, field initializers, and default parameter values** — the rest of §17.7's `new()`-shorthand context family. These last three *would* be principled (the same written-declared-type test admits them) and are deliberately not taken: the observed evidence is entirely return-position, and the family can widen later by citing this paragraph (recorded liftable). **Subexpression positions within a return operand are not covered**: in `return (if c then Ok(v) else Error(e));` the branches are operands of the `(if …)` atom ([§20](../Bezel_Language_Spec.md#sec-the-if-expression)), whose branch typing computes a common type from the branches and target-types only the bare-`null` branch; extending declared-result flow into atom branches is a §20 change with its own meet questions, recorded as an open extension (Q3), not smuggled in. The remedy meanwhile is two `return` statements, or the qualified spelling.

### 2.3 The ambiguity rule, decided with reasons

When the declared type is not a choice with the named case, ordinary resolution applies — the form cannot be a required-context error, because `Ok(v)` is lexically indistinguishable from an ordinary call and `return Compute(x);` must keep meaning what it means. The genuine design choice is the other direction: when the declared type **is** such a choice *and* the name also resolves ordinarily (a method named `Error` in scope), who wins?

**Decided: the case reading wins (declared-type-authoritative), with a shadowing warning.** Three reasons:

1. **It is the pattern-side rule, transplanted.** Inside a `case` over a choice scrutinee, "a name resolving to a case is a case pattern, never a type test" ([§35.5](../Bezel_Language_Spec.md#sec-case-patterns)) — context supplies the reading, deterministically. Return position under a declared choice type is the construction-side mirror of the same context.
2. **The alternative is action at a distance.** Scope-first would let an unrelated `import` (or a new helper named `Ok`) silently re-mean an existing `return Ok(v);` — precisely the failure mode [§26.4](../Bezel_Language_Spec.md#sec-generic-extensions) records as the ground for its always-bind rule ("would let an unrelated type declaration elsewhere silently re-mean an extension block"). Declared-type-authoritative keys the meaning to the method's own signature, visible lines above the return.
3. **The warn-on-shadow discipline is established.** §26.4's implicit type-parameter binding binds "always, shadowing any same-named type in scope (the compiler warns on such shadowing)"; the same shape applies here.

The diagnostic set: the shadowing warning (naming the case and the shadowed member, with the qualify-or-bind remedy); and — unchanged — a bare `Ok(v)` in any *other* position keeps today's ordinary-resolution error, to which the implementation should add a note when the enclosing method's return type is a choice with that case ("did you mean `return Ok(v);`, or qualify as `Result<int, string>.Ok(v)`").

Partial qualification does not join the family: `Result.Ok(v)` (generic type, no arguments) remains an error — the admitted spellings are the fully qualified form and the bare case name; a half-inferred middle would reintroduce exactly the argument-inference §14.5/§35.3 forbid.

### 2.4 Nested and generic returns; async bodies

- **Async bodies.** In an `async` method declared `: Task<Result<int, string>>`, the return expression is checked against the task's *result* type: "An async method's `return expression;` completes the task with the value" ([§30.1](../Bezel_Language_Spec.md#sec-async-methods)). The declared-result position's type is therefore `Result<int, string>` — nullability-stripped per the rule — and `return Ok(v);` target-types exactly as in a synchronous body. (`OwningTask<Result<…>>` cannot arise: `OwningTask<T>` requires class `T`, §30.1, and a choice is a value type.)
- **Non-async task forwarding.** A non-`async` method declared `: Task<Result<int, string>>` returns a *task*; its return operand's declared type is the `Task`-family type, not a choice, so the case reading never fires there — ordinary resolution, unchanged.
- **Generic contexts.** The rule reads the declared type *after* the enclosing context's substitutions are applied where they are known (a concrete instantiation), and does not fire on a bare type parameter (`method M<R>(): R` declares no choice). A declared `Result<T, E>` inside a generic body whose `T`/`E` are the body's own parameters is a choice instantiation and fires normally — the case supplies no bindings, the declared type supplies everything.

### 2.5 Engagement with 0041's recorded posture, and the precedent family

0041 §2.4 recorded both sides, and both must be engaged honestly.

**The counter-argument (0041's recorded ground for leaving this out of v1):** target-typing of a type's arguments is "a mechanism the language reserves for literals," and §14.5/§35.3's uniform never-infer posture — "type-level arguments are never inferred from constructor arguments" — exists to prevent a some-sites-infer inconsistency, where readers must know per-form whether spelled-out arguments are required. A first exception invites a second.

**Why the counter-argument does not defeat this form:** stage 1 infers **nothing from arguments**. The declared type arrives *whole* from the signature — the case name selects among its cases; the arguments then check against the selected case's payload types exactly as against a parameter list. That is not the §14.5 mechanism at all; it is the **declared-type-directed** family, which is larger than "literals" and already includes non-literal forms:

- [§17.7](../Bezel_Language_Spec.md#sec-the-new-operator)'s **`new()` shorthand** — the nearest neighbor, and not a literal: the target type supplies the constructed type whole, "permitted in field initializers, typed local-variable declarations, **return statements** (when the method has a declared return type), and default parameter values." `return Ok(v);` is the sum-type twin of `return new(…);` — the target supplies the type, the written name selects within it.
- [§4.2](../Bezel_Language_Spec.md#sec-integer-types)'s **typed-context widening** (v0.159) — "a **`return`** in a method with a declared integer return type" is an enumerated typed context; extended verbatim to ascribed lambda returns at v0.217 (changelog), the same two-position extension this stage makes.
- [§5.1](../Bezel_Language_Spec.md#sec-implicit-conversions)'s **constant narrowing** — declared-target-directed admission at conversion sites, reused by collection-literal elements and default arguments.
- [§15.4](../Bezel_Language_Spec.md#sec-collection-literals) — "`return [1, 2, 3]` is admitted when the enclosing method's return type determines the target type."
- [§14.5](../Bezel_Language_Spec.md#sec-type-argument-inference) **phase 3** — "a `return` statement against a declared return type" is already a target context that *binds generic type arguments* for generic-method calls.

Return position is thus the language's single most established typed context. The uniform posture stands where it was aimed — no inference from arguments, no argument-position or untyped-context target-typing — and the recorded liftable is engaged on its own stated condition: the spelling proved heavy in practice (0061 §3), not in prediction.

### 2.6 Kernel and ownership: typing only

Stage 1 adds **no ownership rule and touches no kernel clause**. The form is resolved to a case construction before classification runs; from that point every existing rule applies to it *as* the qualified construction: payload-slot ownership per §35.3 (an `owned` slot is the K3.3 owned-argument position; the consume-site freeze applies; the no-temporary-source rule applies to borrow slots), the constructed value classifies per [§35.4](../Bezel_Language_Spec.md#sec-choice-classification), an owning construction in return position is the ordinary announced transfer ([§28.3](../Bezel_Language_Spec.md#sec-producers)'s move-only producer row — `return Ok(owned conn);` under a declared `Result<Connection, string>` works today spelled qualified, and works identically bare). No producers-table row, no K-clause, no Appendix C invariant changes.

### 2.7 Edit list and test surface

Spec: the §35.3 paragraph (§2.1 above); one cross-reference sentence each at §21.7, §22.2, §23.2, and §27.3; no grammar edit (`N(args)` and `N` already parse — Appendix A.7 unchanged); the two diagnostics (§2.3). Tests (spec-cited, per `tests/README.md`): the `Result` and domain-choice happy paths in all three position kinds (return / `=>` body / ascribed lambda); the nullable-declared-type wrapping; the async `Task<Result<…>>` body; payload-free bare-name return for a domain choice; the shadowing warning; non-choice declared type falls through to ordinary resolution; `Result.Ok(v)` still rejected; the owning-payload construction (`Ok(owned conn)`) with its freeze and marker errors intact; subexpression positions (if-branch) not covered.

## 3. Stage 2 — value-payload combinators on `Result` *(proposed)*

### 3.1 The members

Exactly the 0041 Q12 interim: value-payload combinators, for non-owning instantiations only. Proposed surface (spelled here as the members land in stdlib §2.20; the *declaration mechanism* is §3.2's question):

```bezel
// PROPOSED — on Result<T, E>, admissible at value-like instantiations only (§3.2/§3.4)
public method Map<U>(f: (T) -> U): Result<U, E>;
      // Ok(v)    -> Ok(f(v));  Error(e) -> Error(e) unchanged
public method MapError<F>(f: (E) -> F): Result<T, F>;
      // Error(e) -> Error(f(e));  Ok(v) -> Ok(v) unchanged
public method AndThen<U>(f: (T) -> Result<U, E>): Result<U, E>;
      // Ok(v)    -> f(v);  Error(e) -> Error(e) unchanged
public method UnwrapOr(fallback: T): T;
      // Ok(v) -> v;  Error(_) -> fallback
```

Contract notes, each spec-checked:

- **`fallback`, not `default`.** The review brief's `UnwrapOr(default: T)` is unspellable: `default` is an **unconditionally reserved keyword** ([Appendix B.1](../Bezel_Language_Spec.md#sec-reserved-keywords)), not contextual. Renamed `fallback`. (Parameter names are caller-visible under [§22.5](../Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments) named arguments, so the name is API surface; recorded.)
- **`fallback` is eagerly evaluated** — an ordinary argument. A lazy `UnwrapOrElse(f: () -> T)` twin is deliberately not proposed (no observed need; one more member per the minimal-surface discipline; recorded so a later sweep cites the decline).
- **Not `pure`.** For `Map`/`MapError`/`AndThen` this is §22.15's hard rule, not a posture: invoking a function-typed value is on the pure body's closed prohibited list ("the value's type carries no effect information"), and §22.15's parameter rule admits function-typed parameters "but not invoked". `UnwrapOr` invokes nothing and *could* be `pure` under §22.15; it stays plain `method` as a deliberate family-uniformity call (the v0.215 posture that keeps the `Contains` family impure — 0067 item 4 restates it).
- **Lambdas are the v0.217 keyword form** in every example: `r.Map(lambda(p) => p * 2)`. `U`/`F` infer at call sites per [§14.5](../Bezel_Language_Spec.md#sec-type-argument-inference) phase 2 (the lambda's inferred return type binds the method-level parameter — the §14.5 worked examples' `Select` shape).
- **No `Unwrap`, still.** Stdlib §2.20's recorded reasoning stands untouched: extraction is consumption, consumption is the owning match's job. `UnwrapOr` is not an extractor with a fault path — it is total, value-in/value-out.

### 3.2 The enforcement finding: the recorded extension-block route has no clean mechanism

0041 Q12 recorded the interim as "a value-payload-only **extension-block** subset" (0041 §5: "as extension blocks on `Result` if the ownership story stays value-only"), and stdlib §2.20 records the blocker the interim routes around. Working the mechanism against v0.228 turns up **three independent gaps** — this is the proposal's most consequential verification result:

1. **Choices cannot be extended at all.** [§26.2](../Bezel_Language_Spec.md#sec-what-can-be-extended)'s extendable list is "Classes (including `Object`). Interfaces. Records. Enums. Built-in primitive types." — choices are absent. The list predates v0.200 and was never amended at the choice graduation (none of §26's text mentions choices). Any extension-block route therefore begins with a §26.2 spec amendment. The amendment itself looks safe — choices sit beside enums everywhere else in the type system, extension receivers are borrows ([§26.7](../Bezel_Language_Spec.md#sec-ownership-in-extensions)), matching [§35.2](../Bezel_Language_Spec.md#sec-choice-members)'s `this`-as-borrow — but it is a language change hiding inside a "stdlib-only" interim, and it was nobody's recorded decision. Flagged first because it re-scopes stage 2 regardless of mechanism.

2. **No constraint names the value-only bound.** An extension block constrains applicability through `where` clauses on its **type parameters** ([§26.4](../Bezel_Language_Spec.md#sec-generic-extensions)); there is no head-level form ("this block applies to value-like instantiations"). And no [§14.2](../Bezel_Language_Spec.md#sec-constraints) constraint captures the classification-neutral payload set (`Result<T, E>` is value-like iff every payload is a non-move-only value, `string`, or runtime-managed — [§35.4](../Bezel_Language_Spec.md#sec-choice-classification) rule 3):
   - `where T: value` **excludes `string`** — knocking out `Result<int, string>`, the flagship instantiation of stdlib §2.20, Guide 2 §5, and this proposal's own examples — **and admits move-only values** (§14.2's `value` row: move-only inline arrays and owning-classified choice instantiations satisfy it), under which `Result<T, E>` is *owning* (§35.4 rule 1) and the combinator bodies do not check.
   - `where T: value, sendable` repairs the move-only leak (a sendable value type is "not borrow-bearing and not move-only", [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)) but still excludes `string`; `where T: sendable` alone admits `string` but also admits **transfer-closed sendable classes** (§30.14's class rule), under which the instantiation is owning again. There is no negated form and no union form (§14.2's derived-property machinery: "there is no negated form").
   - (`hashable`, the remaining §14.2 derived-property constraint, admits equality-bearing class types and fails identically; §14.2 offers no negation or union forms, so the enumeration is closed, not illustrative.) The residual workaround — parallel blocks with concrete `string` heads (`extension Result<T, string> where T: value, sendable`, etc., legal per §26.4's concrete-argument heads) — **collapses at the method level**: `Map<U>` needs the same bound on `U`, `where U: value, sendable` excludes `U = string`, and `r.Map(lambda(p) => $"port {p}")` — mapping *to* a string, a bread-and-butter use — is inexpressible; and even where both spellings could be declared, the call site fails under [§26.5.1](../Bezel_Language_Spec.md#sec-specificity-ordering) — candidates with identical substituted heads never auto-rank (neither a more-concrete-looking head nor a stronger `where` clause makes one more specific), so §26.5 rule 4's ambiguity error fires ([§26.6](../Bezel_Language_Spec.md#sec-multiple-blocks-for-the-same-type)'s duplicate-signature rule bars the same-type spelling besides). The constraint route fails structurally, not aesthetically.

3. **Generic bodies must check once.** [§35.11](../Bezel_Language_Spec.md#sec-choice-generics)'s discipline (bodies type-check once, classification-independent) and [§14.2](../Bezel_Language_Spec.md#sec-constraints)'s constraint-subsumes-interior-checks rule mean the value-only bound must be *expressible at the declaration* for the bodies (`Result<U, E>.Ok(v)` from a copied payload) to check at all. Gap 2 says it is not.

**The options**, with a recommendation:

- **Option A (recommended): compiler-provided per-instantiation members on `Result` itself.** Declare the four members in stdlib §2.20's own listing ([§35.2](../Bezel_Language_Spec.md#sec-choice-members) admits choice methods; `this` is already a borrow), **compiler-provided** and **admissible only at value-like instantiations**: a call at a borrow-bearing or owning instantiation is rejected at the use site under [§3.6](../Bezel_Language_Spec.md#sec-derived-type-properties) rule 2, the diagnostic naming the classification and the deciding payload (rule 4) — exactly the shape §35.8 already uses for the conformance *conversion* and stdlib [§4.14](../Bezel_Standard_Library.md#sec-owned-buffers-and-spans) uses for `OwningBuffer<T>`/`Span<T>`'s genuinely per-category **members** (`IEnumerable<T?>` conformance at class `T` only; `DangerousPointer(): Ptr<T>` at value `T` only; `move buf[i]` "not admitted" for value-typed `T` — spec [§14.4](../Bezel_Language_Spec.md#sec-owned-and-generic-type-arguments) carries the no-`move`-admission half). The *admissibility* half needs no reservation at all — §3.6 rule 2 already rejects property-violating instantiations at the use site generally; only the compiler-provided **bodies** lean on §3.6's behavior-dispatch reservation and the rationale §5.15 precedent 0041 already engaged for the classification itself (§35.11's "second scoped exception"), and designating stdlib `Result` compiler-known for that purpose is itself part of what Option A asks the owner to grant — stdlib §2.20 currently presents it as an ordinary written choice. Costs, stated: one carve at §35.2/§35.11 (these compiler-provided members are per-instantiation-admissible, unlike user choice methods — a sentence, on stdlib §2.19's compiler-provided-conformances precedent, spec-side §11.8); the members are magic in the sense that a user choice cannot replicate them (mitigated: a user choice's *value-like* combinators are writable today as ordinary methods on a concretely-instantiated or value-constrained type; what users cannot write is the unconstrained-generic form, which is exactly the §35.11 wall).
- **Option B: a new §14.2 constraint naming the classification-neutral set** (a `where T: <neutral>` admitting non-move-only value types, `string`, and the runtime-managed types), plus the §26.2 choice amendment; the combinators then land as the recorded extension block with the constraint on `T`, `E`, `U`, `F`. Honest and reusable — user choices get the same power — but it is permanent user-facing constraint vocabulary bought on one program's evidence, and §14.2's discipline ("the admitted set grows only by recorded ruling — one property, one decision") sets the bar deliberately high. Recorded as the liftable follow-on if user-defined choices start wanting combinators.
- **Option C: the multi-block workaround** — rejected above (gap 2's method-level collapse); listed only so the decline is citable.

If the owner prefers B (or a mechanism not listed), stage 2's surface (§3.1) is unchanged — only its declaration form moves. Under every option, **function-typed-payload instantiations** (classification-neutral but degenerate — the `owned` marker erases at runtime-managed substitutions, §28.1) ride or don't per the mechanism's precision; under A they ride (they are value-like), which is harmless — the combinators are total value plumbing. (`Result<Object, E>` is **owning**, not degenerate-neutral: `Object` is a class type, so `Ok(owned value: T)`'s marker keeps its transfer meaning and §35.4 rule 1 fires — it is rejected like any other class payload.)

### 3.3 Ownership story, member by member

Stage 2 is borrow-path value plumbing throughout; no ownership rule is touched. Per [§26.7](../Bezel_Language_Spec.md#sec-ownership-in-extensions)/[§35.2](../Bezel_Language_Spec.md#sec-choice-members), the receiver is a **borrow**; at the admissible (value-like) instantiations, the internal match is [§35.5](../Bezel_Language_Spec.md#sec-choice-matching)'s borrowing match, whose payload bindings are **copies** (value payloads) or **shared references** (`string`/runtime-managed payloads); every result is a fresh value-like choice value — an ordinary copyable value ([§35.3](../Bezel_Language_Spec.md#sec-choice-construction)). The lambda classifications, per [§27.5](../Bezel_Language_Spec.md#sec-closures)/[§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)/[§28.14](../Bezel_Language_Spec.md#sec-scoped-callback-parameters):

- **The parameter `f` is plain (no `scoped`), hence call-bounded** (§28.14): the combinator invokes it during the call and may not retain it — precisely the contract these bodies need; no deferred-activation frame is minted (the combinators are eager), so no frame-residence clause engages.
- **The argument lambda** is a runtime-managed value ([§28.9](../Bezel_Language_Spec.md#sec-runtime-managed-types)); its **captures are borrows** of the enclosing scope (§27.5; §28.3's captured-borrow rows: source = the captured local's source). Because the parameter is call-bounded, *any* capture set is admissible at the call site (§28.14's caller side: every captured source trivially outlives the call).
- **`f`'s invocation result** — `U`, `F`, or `Result<U, E>` at value-like instantiations — is a plain value: §28.3's producers table, "Function call producing an ordinary value type, `string`, or runtime-managed type — Not applicable (§28.9)". No disposal obligation is created anywhere in a combinator call.
- **`UnwrapOr`'s `fallback`** is a value copy in, value out; `Map`/`MapError`/`AndThen` results are ordinary values whose further use (binding, return, argument) follows the standard value rules.

Sendability, equality, and hashing of the instantiation are untouched — the members are behavior, not structure, and [§35.4](../Bezel_Language_Spec.md#sec-choice-classification)/[§35.7](../Bezel_Language_Spec.md#sec-choice-equality)'s derived properties ignore methods by construction.

### 3.4 The value/owning asymmetry, stated honestly

`Result<FileStream, string>` — an owning instantiation ([§35.4](../Bezel_Language_Spec.md#sec-choice-classification) rule 1; `FileStream`: stdlib [§7.2](../Bezel_Standard_Library.md#sec-filestream)) — rides **none** of stage 2. Its `Map` would have to *consume* the payload through a receiver the method model only ever borrows (§35.2's `this`-as-borrow — the same wall that makes `Unwrap` unspellable, stdlib §2.20), and its `f` would need per-instantiation ownership polymorphism (`(owned T) -> owned U` at owning instantiations, `(T) -> U` at value-like ones) — the recorded stage 3(a) territory. The owning user keeps today's idioms: the owning match over a fresh call, `case owned x of …` for a named binding, adopt-with-`using var` in the arm (§35.5; stdlib §2.20's connect-or-explain example). **This asymmetry is the recorded cost of stage 2** — accepted by 0041 Q12's interim wording, re-accepted here with the boundary stated in the stdlib listing so no reader discovers it at a rejected call site: the §3.2 diagnostic names the classification, the deciding payload, and the owning-match remedy. 0041 §5's decline carried a second ground beyond the polymorphism blocker — that value-payload combinators "save one `case` at the cost of opening exactly that door." Engaged, not bypassed: stage 2 opens no *mechanism* door (the members are ordinary borrows over value payloads; per-instantiation slot ownership stays shut behind stage 3(a)'s own future proposal); what it does open is the **expectation** that owning instantiations eventually get combinators, and this section prices that expectation as the recorded asymmetry rather than leaving it implicit.

### 3.5 Worked pipelines (every API verified in v0.228; stage 1 spellings used where they apply)

```bezel
// Lookup-with-fallback in one expression (value payload):
var port := ParsePort(raw).UnwrapOr(8080);

// Shape the success, keep the failure (Map to string — the U = string case
// that §3.2's constraint route could not express):
var label := ParsePort(raw)
    .Map(lambda(p) => $"listening on {p}")
    .UnwrapOr($"bad port: {raw}");

// Re-code the error at a boundary (MapError):
public record ConfigError(Key: string, Message: string);

public method PortSetting(raw: string): Result<int, ConfigError>
  return ParsePort(raw).MapError(lambda(m) => new ConfigError("port", m));
end PortSetting;

// Sequence two fallible steps without an intermediate case (AndThen);
// the ascribed lambda return is a stage-1 declared-result position:
public method ParsePercent(text: string): Result<int, string>
  return ParsePort(text).AndThen(lambda(n): Result<int, string> => do
    if n > 100 then return Error($"not a percentage: {n}"); end;
    return Ok(n);
  end);
end ParsePercent;
```

The `LoadPort` shape of §1.1 — one propagating call — folds entirely; call sites with *several* propagating calls still pay one statement-position `case` per call unless restructured as an `AndThen` chain, which is exactly the residue stage 3(b) waits to measure (§4).

## 4. Stage 3 — explicitly deferred, recorded

### 4.1 (a) Ownership-polymorphic function parameters — the owning-payload unlock

The language feature that would lift §3.4's asymmetry: function parameters whose slot ownership varies with the instantiation, so one `Map` serves `(T) -> U` at value-like and `(owned T) -> owned U` at owning instantiations — stdlib §2.20's recorded blocker, "the retired-and-deferred territory of spec [§14.4](../Bezel_Language_Spec.md#sec-owned-and-generic-type-arguments)" (the v0.78–v0.96 owned-generic-argument mechanisms, removed at v0.97; §14.4's historical-references note). Two recorded observations for the eventual proposal, neither designed here:

- The ground has shifted since v0.97: [§28.1](../Bezel_Language_Spec.md#sec-where-the-owned-modifier-can-appear)'s v0.196 per-instantiation erasure already gives marked **bare-type-parameter** function-type slots exactly this polymorphism ("`(owned T) -> ()` at `T = int` *is* `(int) -> ()`"), so the type-side ingredient partially exists. What does not exist is the consuming *receiver*: §35.2's `this`-as-borrow means owning combinators are likely static/consuming-parameter forms (`owned r: Result<T, E>`) or something newer — its own design space, proposal-sized.
- The dependency direction is recorded: stage 2's surface must be forward-compatible with an owning extension (same names, same shapes, the value-only admission widening rather than a parallel owning family) — a constraint on stage 3(a)'s design, not on stage 2.

### 4.2 (b) A propagation operator — 0061 §2's territory, still gated

The Rust-`?` capability remains genuinely unique under 0061's admission test, and remains control flow hidden in an expression, against the announced-flow personality. 0061's own record, adopted here as the posture (§2: "`map`/`andThen` may relieve the same pain without new control flow"; §3 restates it): **stages 1–2 may dissolve most of the observed propagation pain without new control flow** — stage 1 removes the restatement inside each propagating arm, stage 2's `AndThen` removes the `case` for sequenceable pipelines, and what residue survives (multi-call bodies that do not chain naturally; early returns interleaved with side effects) is unknown until real code is written *against stages 1–2*. The operator therefore stays behind 0061 §2's gate, re-armed with a sharper condition: revisit when `Result`-using code written **with** stages 1–2 available still exhibits per-call propagation ceremony. Any design must still answer 0061 §2's recorded ownership question (an early return is a K3.2 consume of the propagated payload) and weigh against 4.1(a). The evidence log stays in 0061 §3; new observations append there and cite this section.

## 5. Relationship to Guide 2 §6

Guide 2 §6's failure-signal table is **unchanged by every stage**: `Result` keeps exactly its row — "Failure is expected *and carries information* → `choice` return (`Result<T, E>` or a domain choice, spec §35); consume with an exhaustive `case`" — and the neighboring rows (`T?` for plain absence, the Try shape for owned extraction with a boring failure, `requires` for caller bugs) keep theirs. This proposal is **cost reduction inside the third row, not doctrine change**: nothing here makes `Result` correct in a situation where the table routes elsewhere, and nothing weakens the exhaustive-`case` consumption story (`UnwrapOr` is a fallback fold, not an extractor; the match remains the consumption idiom, and the owning world is match-only by design, §3.4). The guides gain the stage-1 spelling and the stage-2 pipelines at their next revalidation pass — a docs-workflow item riding the landings, per house practice, not part of this proposal's edits.

## 6. Migration

**Purely additive; nothing currently legal is rejected.** Stage 1: the qualified construction remains valid everywhere; the bare form was previously an ordinary-resolution error in the covered positions (or, where a same-named member existed, keeps its meaning *outside* declared-choice-result positions and draws the §2.3 warning inside them — the one behavioral edge, affecting only programs that call a scope member named identically to a case of their own declared return type, of which the repository has none). Stage 2: new members beside an unchanged §2.20 surface; no conformance-corpus flips; `IsOk`/`IsError` untouched. Stage 3: no change by definition. Compiler: stage 1 is a resolution-phase change with spec-cited regression tests per `tests/README.md`; stage 2 is stdlib/member machinery per the chosen mechanism, each member with value-like acceptance tests and borrow-bearing/owning rejection tests citing §3.6 rule 2.

## 7. Considered and declined

- **General construction inference** — inferring a choice's type arguments from construction arguments (`Result.Ok(5)`), or admitting the bare-case form in argument/untyped positions. Declined; 0041 §2.4/§35.3's posture stands in full. Stage 1 is deliberately *not* an inference mechanism (§2.5), and the argument-position line is §17.7's own.
- **An `Option<T>` type** to pair with the combinators. Declined without reopening: [`DECLINED.md`](DECLINED.md) D-0004 — `T?` is Bezel's option type; the nullable convenience tier is proposal [0072](0072-nullable-return-convenience-tier.md)'s territory and composes with `Result` as-is.
- **Combinators on owning payloads via linear-use lambdas** — admitting `(owned T) -> owned U` combinator overloads now, restricted to owning instantiations, with single-call lambda obligations. Stage 3(a)'s territory, premature: it commits the consuming-receiver design (§4.1) piecemeal and would ship a second combinator family that 3(a) would then have to unify. Declined here; the dependency is recorded instead.
- **Extending stage 1 to enum constants** (`return Red;` under a declared enum return). The mechanism transplants, but the observed pain does not: enum construction carries no generic-argument restatement and a short prefix. Not proposed; citable if enum-heavy code ever logs the ceremony.
- **`UnwrapOrElse(f: () -> T)`** — declined in §3.1 (no observed need).
- **The `new()`-context family for stage 1** (typed locals, field initializers, default parameter values) — principled but unevidenced; recorded liftable in §2.2, not taken.

## 8. Open questions for the owner

- **Q1 (stage 1, scope):** confirm the `=>` expression-body and ascribed-lambda-return positions ride with `return` statements (§2.2's argument), or cut stage 1 to `return` statements alone?
- **Q2 (stage 1, ambiguity):** confirm the declared-type-authoritative reading with shadowing warning (§2.3), or prefer scope-first-with-fallback (ordinary resolution wins where it succeeds), accepting its action-at-a-distance cost?
- **Q3 (stage 1, branch positions):** should declared-result flow reach `(if …)`/`(case …)` branch results within a return operand (the §20 bare-`null`-branch precedent shows per-branch target flow exists), or stand as scoped (§2.2)? Recorded liftable either way.
- **Q4 (stage 2, the mechanism — the real ruling):** Option A (compiler-provided per-instantiation members on `Result`, recommended), Option B (new §14.2 classification-neutral constraint + §26.2 choice extension admission — the reusable, user-facing route), or defer stage 2 until B's constraint is wanted on its own merits? Note that Q12's recorded extension-block spelling is not available as recorded (§3.2's finding) — whatever is chosen amends that record.
- **Q5 (stage 2, `MapError`):** does `MapError` earn its seat on one observed program? The log's evidence is for construction restatement and propagation ceremony; `MapError`'s error-recoding case (§3.5's `PortSetting`) is inferred from the retry library's shape, not directly logged. Options: land all four; land `Map`/`AndThen`/`UnwrapOr` and record `MapError` as next-evidence; land all four flagged provisional.
- **Q6 (stage 2, naming):** `UnwrapOr` implies an `Unwrap` family that deliberately does not exist (stdlib §2.20's no-`Unwrap` record); `ValueOr` or `OkOr` would avoid the phantom anchor at the cost of the cross-language convention. Also confirm the `fallback` parameter name (§3.1's keyword finding). Owner's pick.
- **Q7 (stage 2, admission breadth under Option A):** confirm that the degenerate-but-value-like instantiations (function-typed payloads — `Result<Object, E>` is owning and already rejected, §3.2's closing note) ride the combinators, or carve them out at the cost of a sharper admission rule?
