# 0049 — Runtime `const` locals, `where T: sendable`, `Channel<T>.ReceiveAll` — three separable amendment trains

**Status:** graduated-v0.201 — superseded — graduated v0.201 (trains A+B; train C was a currency finding, annotated)
**Tracking issue:** [#2036](https://github.com/s8lap/bezel-lang/issues/2036)
**Target:** Train A — spec §21.1 (new subsection), §17.14 (two edits), §22.12 (one sentence split), §7.4 (one paragraph), §28.10 (cross-reference), Appendix A.6/A.8 (comment-level), rationale §17.8 rewrite + §18 entry + one decision record. Train B — spec §14.2 (table row + new subsection), §3.6 (one sentence), §30.14 (cross-reference), Appendix A `Constraint` production, Appendix B.2, Appendix C.4 (one bullet), stdlib §6.8/§6.9 signatures, rationale §18 entry + one decision record. Train C — **docs-currency only**: rationale §18 entry annotation, two optional clarification sentences in spec §30.15; no grammar, no stdlib surface change.
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

## 0. Summary

This proposal expands item **P9** of [future-directions-2026-07-18.md](papers/future-directions-2026-07-18.md): adopt three items from the rationale's own deferred list (§18) as three **separable amendment trains**, each small, each additive, each able to ride any docs revision. They are marked **A**, **B**, **C** throughout and are independently acceptable; if landed together, the [spec-change-process.md](spec-change-process.md) §4 **composition gate** applies to the combined edit set (the known seams are enumerated in §6).

- **Train A — runtime `const` locals.** `const` in local position accepts any initializer; an initializer that is a §17.14 constant expression (with the existing primitive/`string` type restriction) keeps today's compile-time-constant semantics, and everything else becomes a **runtime const**: a single-assignment local binding, not usable in patterns or other compile-time positions. The central design point is ownership: a runtime `const` **rejects** any owned-producing right-hand side, keeping a clean division — `using var` is the owned, scope-pinned, non-reassignable binding; `const` is the non-owned one. The checkable payoff is a §7.4 amendment: a `const` local's nullability narrowing is **never invalidated**.
- **Train B — `where T: sendable`.** Decide once, as rationale §18 asks, the general rule for derived-type-property constraints (admitted individually, by recorded ruling; nothing propagates unless written), and admit **sendable** as the first. `Domain.Spawn` and `Channel<T>` then state in their signatures what stdlib §6.8 today concedes "is not expressible as a `where` constraint," and generic user libraries move the not-sendable diagnostic from their internals to the user's instantiation site. Explicitly *not* Send/Sync inference — rationale §12.5's decline stands untouched.
- **Train C — `Channel<T>.ReceiveAll()`.** **The mandated design already landed in v0.186** (spec §30.15; stdlib §6.9) — a fact both the P9 brief and its rationale-§18 starting point missed, because the §18 entry was never annotated at landing. Train C is therefore a **currency train**, not a design train: annotate the stale deferred-list entry, confirm the implementation tracker, and land two clarification-class sentences the v0.186 text left implicit. The divergence from the P9 mandate is recorded in §4.2 with the verification trail.

## 1. Problem and evidence

### 1.A — no readonly local binding (verified current state)

- Spec §21.1: locals are `var` (always reassignable) or `const` with a §17.14 compile-time-constant initializer of primitive/`string` type (§17.14 "Type restriction on `const`", v0.184; §22.12 restates it for locals via grammar A.6). Grammar A.6: `LocalConstDecl ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"`.
- Rationale §17.8 records the decline of `let`/`val`/`readonly var` — and carries the honest footnote (review finding #1360): a `using var` whose RHS is value-typed, runtime-managed, or a borrow has a no-op disposal (§28.10), making it an *admitted but unendorsed* readonly-local spelling. §17.8's own closing names the remedy this train delivers: "the leading candidate is extending `const` to accept runtime-evaluated expressions."
- Rationale §18 ("Runtime-evaluated `const` for locals") defers exactly this, naming the one design obligation: distinguish compile-time constants (patterns, default values) from runtime constants.
- The checkable cost of the gap: §7.4 (v0.182) invalidates a local's narrowing when the local is *mutably captured* and the block performs any call/await. The sanctioned remedy — "copy to an uncaptured local" — produces a `var` that is itself re-capturable and reassignable; nothing in the language lets the author *say* the copy is stable. Guide 02's very first idiom (`var local := _field;  // copy`) is the shape a readonly binding would harden.
- At member level the readonly story is complete (`readonly` fields §22.8, `static readonly` §22.12); locals are the one binding position with no non-owned single-assignment form. `using var` covers the *owned* half only.

### 1.B — sendability is a signature-invisible requirement (verified current state)

- Spec §30.14 defines **sendable** as a derived type property (§3.6 row), checked per instantiation (§3.6 rule 2). §14.2's constraint vocabulary has no way to name it: the `Constraint` production is `"class" | "value" | "notnull" | "new" "(" ")" | Type`.
- Stdlib §6.8 (Domain.Spawn), verbatim concession: "The `where` clauses above are the declared constraints; sendability is the stronger compiler-checked requirement and is diagnosed at the call site naming the failing rule (**it is not expressible as a `where` constraint — it is a derived type property**)." The declared rows carry comments like `where A: class; // A sendable (compiler-checked)` — requirements living in comments.
- Stdlib §6.9: `Channel<T>`'s element restriction ("T must be sendable; the instantiation is rejected otherwise") is likewise prose plus §3.6 machinery, not a clause on the declaration.
- A generic user library wrapping a channel or a spawn cannot state the requirement at all: the rejection fires when the *internal* `Channel<T>`/spawn use is reached at some instantiation, so the requirement is undocumented in the signature and the diagnostic path runs through library internals.
- Rationale §18 ("`sendable` as a `where` constraint") defers this with the machinery already priced ("the checking machinery — per-instantiation §3.6 evaluation — already exists") and one condition: "the general rule — *which* derived properties may appear as constraints — should be decided once, not per property." Rationale §12.5 records the decline this must not reopen: Send/Sync marker typing that "infects every generic signature."

### 1.C — the ReceiveAll mandate is already normative (currency finding)

Verified against v0.197:

- Spec §30.15 carries a full normative **`ReceiveAll()` (v0.186)** bullet: `IOwningAsyncEnumerable<T>` for class-typed T (`IAsyncEnumerable<T>` otherwise), `Receive`'s transfer semantics per item, suspends while empty-and-open, completes **normally** at drained-and-closed, at-most-one-consumption with `InvalidOperationException` on violations (a second `ReceiveAll`, a `ReceiveAll` over a pending `Receive`, interleaved `Receive`/`TryReceive`), affine per §29.11, abandonment deregisters like an abandoned `Receive` with no item consumed or lost.
- Stdlib §6.9 declares `public method ReceiveAll(): IOwningAsyncEnumerable<T>;` with a matching semantics bullet, and the docs already use it as the canonical drain idiom — the §6.5 TickBus example and the §6.9 worker-pool example (`await for var job in c.Inbox.ReceiveAll() do … end;`), whose shutdown trace leans on normal completion.
- Changelog v0.186: "Its rider lands now: **`Channel<T>.ReceiveAll()`**…"; migration: "`ReceiveAll` is the sanctioned drain idiom (the `catch ChannelClosedException` loop remains legal)"; and "Spec-ahead-of-impl trackers filed at landing: … §6.9/§30.15 `ReceiveAll`".
- The **rationale §18 entry is stale**: "Channel iteration (`ReceiveAll`)" still ends "Deferred, not designed here; it rides no other feature and can land additively" — written before v0.186 and never annotated at landing, unlike the §18 entries that carry shipped markers ("*Shipped in v0.176 (#807, spec §11.8)*", "*Shipped in v0.183 (#1408, spec §27.4)*").
- Compiler: zero `ReceiveAll` hits in `src/` and `tests/` — spec-ahead-of-impl, consistent with the filed tracker.

The stale entry is not cosmetic: it caused two downstream documents (the P9 brief and this proposal's own mandate) to schedule design work for a shipped feature. That is precisely the failure the shipped-marker convention exists to prevent.

## 2. The design — Train A: runtime `const` locals

### 2.1 The split rule (classification)

Proposed §21.1 subsection (normative sketch):

> **Runtime `const` locals (v0.19x).** A local `const` declaration admits any initializer expression. The declaration is classified by the initializer's *form* and the declaration's type:
>
> - **Compile-time constant** — the initializer is a constant expression by [§17.14]'s inductive definition *and* the declared or inferred type is a primitive value type or `string`. Semantics unchanged from v0.197: the name denotes a compile-time value, admissible in `case` patterns (A.8 `Constant`), in other constant expressions ([§17.14]), in inline-array sizes (A.5 `ConstIntExpr`), and in constant narrowing ([§5.1]). An evaluation fault in a form-constant initializer remains a **compile error at the declaration** ([§17.14]) — a faulting constant expression is never reclassified as a runtime const (`const B := 2147483647 + 1;` stays rejected where it is written).
> - **Runtime const** — every other admissible initializer. The declaration is a **single-assignment local binding**: initialized exactly once, at its declaration, by ordinary runtime evaluation. A reference to it is *not* a constant expression; a use in any compile-time position above is a compile error naming the declaration and its first non-constant subexpression.
>
> A `const` local of either classification is not an assignable location: assignment, compound assignment, `++`/`--`, and binding to a `ref` or `out` parameter are compile errors naming the declaration. A lambda may capture a `const` local by read; the mutable-capture forms are excluded by the same rule. The initializer is mandatory (grammar A.6) — there is no declare-then-assign form.

The classification is **computed, never declared** — deliberately mirroring §3.6 rule 1's posture. The recorded cost: an initializer edit can silently flip a compile-time const to runtime, which is loud at any pattern/size/default use site ("not a §17.14 constant expression") and silent otherwise. §9 offers the marker alternative and declines it.

Class-level `const` (including `static const`, §22.12) is **unchanged**: at member level `static readonly` and instance `readonly` already provide runtime single-assignment; the gap this train fills exists only for locals.

### 2.2 The ownership rule (the central decision)

> **Ownership.** The initializer of a runtime `const` must not be an **owned production** ([§28.3] producers): the binding never takes a disposal obligation and never holds an affine (consumable) value. Admitted: value-typed expressions (records included — a borrow-bearing record value carries [§9.7]'s ordinary source obligations), `string` and other runtime-managed values ([§28.9]), `null` (the binding is then permanently null; the [§28.3] dead-null warning applies), and **borrows** — the binding is then a borrow carrying the initializer's source-liveness constraints, exactly as a plain unmarked `var` alias ([§28.3]): the consume-site freeze sees it as any other live local borrow, and its constraint on the source ends at its last use (it does **not** scope-pin the source; pinning remains [§28.10]'s job). Rejected, with a diagnostic naming this rule and the `using var` remedy: a fresh `new`, an `owned`-returning call, a `Task`-family call ([§28.3], v0.190), a deferred-activation producer (a lazy query, `Drain()`, `ReceiveAll()` — affine values, [§29.11]), a `move`, a sourceless static factory result (the [§28.3] treated-owned case), a marked transfer (`const y := owned x;` is not a form), and a move-only value ([§15.3]).

Why reject rather than admit-and-auto-dispose: an owned, never-reassigned, scope-disposed local **already exists** — `using var` is implicitly non-reassignable (§28.10) and is the declared resource idiom. A permissive owned `const` would be `using var` with the disposal *undeclared*: two spellings for one semantics, differing only in whether the reader is told a disposal runs at scope end. Worse, every resolution of the duplicate is bad: if owned `const` disposes at scope end it duplicates `using var`; if it admits move-out it is not constant (consumption is a binding-state change: bound → moved-from); if it neither disposes nor moves it leaks. The rejection also settles the affine cases for free — a lazy query or task bound `const` could never be consumed, so admitting it would create a binding whose only future is the must-use warning. One rule covers all of it: *`const` bindings never change state — not by assignment, not by consumption, not by disposal.* The clean division the diagnostic teaches:

| | reassignable | ownership | disposal |
|---|---|---|---|
| `var` | yes | inferred from RHS | auto-drop if owned (§28.3) |
| `using var` | no | owned (or borrow-pin) | declared, scope end (§28.10) |
| `const` (runtime) | no | **never owned** | none, ever |

```bezel
const n := items.Count + 1;          // runtime const: int
const name := user.Name;             // runtime const: string (runtime-managed)
const conn := pool.Primary;          // runtime const: a borrow, source pool (§28.3 rules unchanged)

const cfg := LoadConfig();           // error: `LoadConfig(): owned Config` is an owned
                                     //   production; a `const` binding takes no disposal
                                     //   obligation — bind owned values with
                                     //   `using var cfg := LoadConfig();` (§28.10)
```

In a generic body, an RHS whose ownership depends on the substitution (a bare-`T` production, e.g. `Take(): owned T` at value vs class instantiations under §28.1's v0.196 erasure) is enforced **per instantiation category** — the §3.6 rule 2 pattern §28.5's generic-owned-field check already uses — with the diagnostic naming the instantiation. A body that needs uniform admissibility across kinds writes `using var`, which §28.10 keeps uniformly admissible for exactly this purpose.

### 2.3 The narrowing payoff (§7.4 amendment)

Proposed addition to §7.4's invalidation paragraph:

> A `const` local's narrowing, once established, is **never invalidated**: no invalidating form can apply to it — it cannot be reassigned, cannot be passed as `ref` or `out`, and cannot be mutably captured (assignment to a `const` is a compile error everywhere, lambda bodies included). A null test on a `const` local therefore narrows it for the remainder of its scope, across calls and `await`s. The field-copy idiom of this section is airtight when the copy is `const`.

This is checkable, not aesthetic: the v0.182 invalidation triggers are syntactic (reassignment; `ref`/`out`; mutable capture plus any call/await), and each is already a compile error on a `const`, so the analysis needs **no new machinery** — the conditions are vacuously false. Contrast:

```bezel
public async method DrainAsync(): Task
  var u := _user;                            // u: User?  (copy-to-local idiom, §7.4)
  var reset := () => do u := null; end;      // u is now mutably captured (§27.5)
  if u <> null then
    await _queue.FlushAsync();               // §7.4 (v0.182): first call/await after the test —
    Send(u.Email);                           //   compile error: u's narrowing is invalidated
  end;
  reset();
end DrainAsync;

public async method DrainAsync2(): Task
  const u := _user;                          // runtime const: single-assignment copy
  // `() => do u := null; end` is a compile error at the assignment,
  // so no mutable capture of u can exist anywhere in this body.
  if u <> null then
    await _queue.FlushAsync();               // narrowing survives: no §7.4 form applies to u
    Send(u.Email);                           // well-typed
  end;
end DrainAsync2;
```

The rationale §17.8 footnote (no-op-disposal `using var` as the "admitted readonly spelling") stops being needed: `const` becomes the endorsed readonly-local idiom, and `using var`'s value-typed admission reverts to what it was tolerated for — generic and desugaring uniformity.

### 2.4 Companion edits

- **§17.14**: the constant-expression atom "a reference to a `const` member — class-level, local, …" is conditioned: a *local* reference qualifies only when the declaration is compile-time-classified (§21.1). The "Type restriction on `const`" paragraph is re-scoped: the two conditions (constant-expression initializer, primitive/`string` type) remain the definition of the **compile-time classification** and remain mandatory for class-level `const`; a local failing either is a runtime const rather than an error.
- **§22.12**: the sentence "the same requirement covers local `const` declarations (grammar A.6)" is replaced by a pointer to §21.1's classification. §22.13's const forward-reference note is unaffected (it already concerns compile-time consts only; runtime consts are ordinary locals with ordinary use-before-declaration rules).
- **§28.10**: one sentence naming `const` as the non-owned single-assignment counterpart, so the two diagnostics can point at each other.
- **§5.1, §19.3/A.8**: no rule change — both key on "compile-time constant (§17.14)"; the pattern-position diagnostic gains the "this `const` local has a runtime initializer" wording.

## 3. The design — Train B: `where T: sendable`

### 3.1 The general rule, decided once

Proposed §14.2 subsection (normative sketch), answering rationale §18's decide-once condition:

> **Derived-property constraints (v0.19x).** A `where` clause may name a derived type property ([§3.6]) from the **admitted set** below. Satisfaction is decided per instantiation, after substitution, by the property's defining computation — the same evaluation [§3.6] rule 2 already performs; the constraint adds no second definition of the property and can therefore never disagree with the property itself. Nothing is inferred and nothing propagates: a generic declaration that requires the property of a parameter writes the constraint; an unconstrained parameter never acquires it; a generic *type's own* properties remain computed from its structure at each instantiation ([§3.6] rule 1 — the constraint is an admission filter on substitutions, never a declaration of the property). Inside the body, `T` counts as having the property wherever a rule of this specification demands it — the per-instantiation re-check for those interior uses is subsumed by the constraint, and one diagnostic (the constraint failure, at the instantiation site) replaces the interior one. Overrides and interface implementations inherit the constraint under the unchanged [§14.2] inheritance rule. The admitted set grows only by recorded ruling — one property, one decision, registered here and in the rationale; there is no negated form and no user-extensible mechanism.
>
> Admitted set of this revision: **`sendable`** ([§30.14]).
>
> | Constraint | Accepts |
> |---|---|
> | `where T: sendable` | Types whose **sendable** derived type property ([§3.6], [§30.14]) holds, decided by the [§30.14] computation unchanged. Composes with the other constraint forms (`where T: sendable, class` admits sendable class types). |
>
> A failed derived-property constraint is diagnosed at the instantiation site naming the constraint, the offending type argument, and the structural reason **as a component path** — [§3.6] rule 4, sharpened: for `sendable`, the failing [§30.14] rule and the field/element chain that violates it (e.g. `Batcher<Session>: Session is not sendable — Session._sock: Socket is a non-owned class-typed field (§30.14 class rule 3)`).

Which properties were considered for the initial set and *not* admitted (each needs its own future ruling; recorded so the register starts honest): **equality-bearing** (the natural second candidate — hash-container keys — but its instantiation-site rejection already names the property and no stdlib concession line exists; undemonstrated demand), **serialization-eligible** (plausible for generic serializers; same posture), **trivial-dispose** and **C-compatible** (no current rule would consume them from a constraint: nothing generic keys on trivial-dispose admission, and §32 has no generic externs), **borrow-bearing** (useful only negated; negation is excluded), **element category** (a classification, not a predicate — not constraint-shaped).

### 3.2 The stdlib surface

Stdlib §6.8's rows gain the constraint and lose the comment-and-concession apparatus (representative; the stdlib section holds the full set):

```bezel
Domain.Spawn<R>(entry: () -> owned R): owned OwningTask<R>
    where R: sendable, class;
Domain.Spawn<A, R>(entry: (owned A) -> owned R, owned arg: A): owned OwningTask<R>
    where A: sendable, class, R: sendable, class;
Domain.Spawn<R>(entry: () -> R): owned Task<R>
    where R: sendable, notnull;          // sendable value type or string
```

```bezel
public class Channel<T> where T: sendable      // §6.9; the T-category signature dispatch is unchanged
```

The §6.8 concession sentence ("it is not expressible as a `where` constraint — it is a derived type property") is replaced by: "the `where` clauses state the full requirement; `sendable` is checked per instantiation by the §30.14 computation (§14.2), diagnosed at the caller's site naming the failing component." The existing kind constraints (`class` / `notnull`) are deliberately **kept alongside** `sendable`: they select the row's ownership shape (owned-transfer vs value forms), which sendability alone does not determine, and keeping them means the amendment changes no row's admitted set — only where and how the requirement is stated.

For user libraries, the payoff is the requirement becoming part of the signature:

```bezel
public class Batcher<T> where T: sendable, class
  private owned _out: ChannelSender<T>;
  public constructor(owned sink: ChannelSender<T>)
    _out := sink;
  end constructor;
  public method Flush(owned item: T)
    _out.Send(owned item);
  end Flush;
end Batcher;
```

Today `Batcher<T>` can only be written *without* the clause, and `new Batcher<Session>(…)` for non-sendable `Session` fails when the interior `ChannelSender<Session>` instantiation is checked — a diagnostic through the library's internals, from a requirement no signature states. With the clause, `Batcher<Session>` fails at the user's instantiation naming the declared constraint and the component path; a generic caller of `Batcher<U>` must itself write `where U: sendable, class`, making the requirement chain **visible at every level** — explicit propagation by spelling, never by inference.

### 3.3 Not Send/Sync — the §12.5 boundary, restated

Rationale §12.5 declined Rust-style Send/Sync typing because it "infects every generic signature" via inference and pervasiveness. This train stays on the declined posture's right side, and the rationale decision record will say so in these terms: the constraint is **opt-in per declaration**; an unconstrained generic is exactly as checkable as today (interior uses re-check per instantiation, §3.6 rule 2 — nothing new is required of code that does not cross domains); no auto-trait inference exists, no constraint is synthesized, and the vocabulary grows by exactly one word admitted by ruling. What §12.5 priced as the failure mode — every signature paying for a parallelism model few use — cannot arise from a constraint that only appears where a signature already traffics in domain crossing.

## 4. The design — Train C: `Channel<T>.ReceiveAll` — adoption audit and residue

### 4.1 What landed in v0.186 (verified, against the P9 mandate point by point)

Every element the P9 brief mandates is already normative: the exact signature (`ReceiveAll(): IOwningAsyncEnumerable<T>`, value-T form `IAsyncEnumerable<T>`); affine single consumption (§29.11 named in both spec §30.15 and stdlib §6.9); owned yields with `Receive`'s transfer semantics; normal completion at drained-and-closed, with the `ChannelClosedException` path retained for bare `Receive` (changelog v0.186: "the `catch ChannelClosedException` loop remains legal"); the consumer-role claim with decided details (`InvalidOperationException` on a second `ReceiveAll`, on `ReceiveAll` over a pending `Receive`, and on `Receive`/`TryReceive` during an active enumeration); disposal-of-enumerator behavior (abandonment deregisters, item mid-handoff given back, nothing lost); and the worked consume-loop rewrite — the stdlib's own §6.9 worker pool and §6.5 TickBus examples already consume via `await for … ReceiveAll()`. Cancellation composes by construction: the return type is the §30.7 interface, so `await for var x in inbox.ReceiveAll() with token do … end;` routes the token to `GetOwningAsyncEnumerator(token)` (A.6 `ForStmt`'s `("with" Expression)?` clause), and a requested cancellation faults the in-flight `MoveNextAsync` per §30.7.

The before/after this train would otherwise have proposed, for the record (both forms are legal today; the second is the sanctioned idiom):

```bezel
// Exception-terminated drain (pre-v0.186 shape; still legal):
try
  while true do
    using var job := await inbox.Receive();   // raises ChannelClosedException at drained-and-closed
    Process(job);
  end;
catch
  ChannelClosedException => Log("drained")
end;

// The v0.186 drain form (stdlib §6.9's idiom):
await for var job in inbox.ReceiveAll() do    // completes normally at drained-and-closed
  using var j := owned job;                   // owned iteration variable discharged (§30.7)
  Process(j);
end;
```

### 4.2 The divergence from the P9 brief, recorded

P9 directs: "batch … `Channel<T>.ReceiveAll()` (the `await for` termination shape §18 already sketches)". This proposal **does not re-design or re-land the feature**, because it exists at the mandated shape since v0.186; re-deriving it here would produce a second normative source for settled text (the exact defect the spec-change process exists to prevent). The root cause of the mismatch is identified and fixed by this train: the rationale §18 entry was never annotated at the v0.186 landing, so every reader of the deferred list — including the P9 author — was told the feature was still open. Train C therefore delivers the *intent* of the mandate (the deferred-list item is closed out) by the correct mechanism: currency edits, not design.

### 4.3 The residue

1. **Annotate the stale §18 entry** (the shipped-marker precedent, matching "Static interface members" and "Lambda-parameter destructuring"): prefix the entry's deferral sentence with "*Shipped in v0.186 — spec §30.15, stdlib §6.9; landed as sketched here (affine per §29.11, normal completion at drained-and-closed, abandonment deregisters)*". Deleting the entry outright is declined: §18's convention keeps shipped entries with markers, preserving the design trail.
2. **Implementation** rides the existing spec-ahead-of-impl tracker filed at the v0.186 landing (changelog v0.186 migration notes: "§6.9/§30.15 `ReceiveAll`"); `grep -r ReceiveAll src tests` is empty at v0.197, so the tracker is still open work. No new issue; conformance cases for the §30.15 bullet land with that tracker, not this proposal.
3. **Two clarification-class sentences** for spec §30.15 (owner-optional, C-Q2 — both make explicit what the v0.186 text implies, changing no behavior):
   - *Cancellation names the same no-loss guarantee:* a `ReceiveAll` enumeration ended by a cancellation fault (§30.7's token routing) deregisters exactly as an abandoned enumeration — an item mid-handoff at that instant is returned to the channel's head; items already yielded to the body are the consumer's.
   - *Consumer-role release:* "at most one consumption in progress" releases when an enumeration ends by any edge — normal completion, fault, or enumerator disposal; a subsequent `Receive`, `TryReceive`, or fresh `ReceiveAll()` (a new affine enumerable) is then admissible.

## 5. Grammar delta (Appendix A)

- **Train A — none at token level, one comment.** A.8 already defines `ConstExpr ::= Expression` restricted *semantically* to §17.14, so `LocalConstDecl ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"` parses every runtime-const initializer today; the change is the semantic restriction moving into §21.1's classification. Edit: the A.6 `LocalConstDecl` comment gains "initializer classified per §21.1: §17.14-constant (of primitive/`string` type) ⇒ compile-time constant; otherwise runtime const"; the A.8 comment notes that `Constant`'s named-const references reach compile-time-classified locals only. Class-member `ConstDecl` (A.4) and `StaticConstDecl` keep the §17.14 restriction unchanged. No new production, no new token.
- **Train B — one alternative, one appendix row.** `Constraint ::= "class" | "value" | "notnull" | "sendable" | "new" "(" ")" | Type`. `sendable` joins Appendix B.2's **contextual keywords** (keyword meaning in constraint position only — the `notnull`/`value` treatment; a user type named `sendable` was already un-idiomatic and remains nameable outside constraint position). Appendix C.4 gains one bullet in the existing pattern: "`where T: sendable` substitutions are types whose sendable derived type property (§30.14) holds; checked per instantiation after substitution (§14.2)."
- **Train C — none.**

## 6. Interactions

Enumerated per the composition gate (spec-change-process §4); "none" entries are stated so absence is checked, not assumed.

- **A × §28.3 (producers/freeze):** a runtime-const borrow enters the existing machinery as an ordinary live local borrow — it freezes consumption of its source while live (consume-site freeze), carries union source constraints from arm-meets (`const x := a if c else b;` is admissible when the meet is borrowed or value; a mixed or owned meet is rejected by the meet rule and by this train's ownership rule respectively). No new producer row: the const *binding* is not a new expression form, and a read of it is classified by the existing borrowed-local row.
- **A × §28.10:** disjoint by construction (owned RHS ⇒ `using var`; non-owned single-assignment ⇒ `const`; a borrow under `using var` remains available when scope-*pinning* of the source is wanted — `const` deliberately does not pin).
- **A × §30 (async):** the RHS may `await` in async bodies; Pattern B awaits and `Task`-family calls are owned productions and rejected (consistent with v0.190's calls-mint rule); a Pattern A `await t` whose result is a borrow is admitted, with the §30.5 borrow bounds unchanged. Narrowing persistence across `await` (§2.3) is the intended interaction.
- **A × §29/§30.7 (iterators):** a const local in an iterator or async body is an ordinary frame local. Binding a lazy query or `ReceiveAll()` enumerable `const` is rejected (affine values; §2.2) — which also keeps Train A from creating an un-consumable claim on Train C's surface.
- **A × generics:** per-instantiation enforcement of the ownership rule for substitution-dependent RHS (§2.2); classification (compile-time vs runtime) is substitution-independent — §17.14's forms never involve type parameters.
- **A × §7.9/§22.13:** none — initializer mandatory, so definite assignment is trivial; runtime consts have no static-initialization-order interaction (locals).
- **B × §3.6:** rule 1 ("computed, never declared") is preserved and the §3.6 sentence added says so: a constraint *requires* the property of substitutions; it never asserts it. Rule 3 (property flips are breaking changes) becomes *more* visible: a dependency's type losing sendability now breaks a constrained instantiation with a named constraint instead of an interior rejection — same break, better blame.
- **B × §30.14:** the property's definition and the set of sendable types are untouched; the constraint is a pure re-expression, so no program changes meaning — verified by construction, since satisfaction is defined as the §30.14 computation itself. Open-class/interface/function-type exclusions all flow through unchanged.
- **B × §12.9 variance / §14.3:** constraints are not variance positions; no interaction. **B × §14.5/§22.6 (inference and overloading):** `sendable` behaves as any other constraint — checked after inference on the completed substitution, never an inference source (§14.5, Appendix C.1/C.4) — and restating it on an override must match the inherited set order-insensitively (§14.2).
- **B × C (the one shared surface):** both trains touch stdlib §6.9 — B edits the `Channel<T>` header, C's residue leaves §6.9 alone (its edits are rationale + spec §30.15). At a combined landing, the §6.9 editor applies B's header clause and verifies the T-category signature dispatch prose still reads correctly under it. This is the seam the composition gate must sight-check.
- **A × B:** none beyond generics-general: `const` locals of constrained `T` follow §2.2's per-instantiation rule regardless of which constraints are in force.
- **C × §30.5/§30.15 internals:** the two clarification sentences restate the give-back and single-consumer rules; neither adds a state or transition.

## 7. Migration and compatibility

All three trains are **additive**; no currently-well-formed program changes meaning or stops compiling.

- **Train A:** every v0.197 local `const` satisfies the compile-time classification (identical conditions), so its semantics are bit-for-bit unchanged, including pattern eligibility. Newly *accepted*: runtime-const declarations (previously "not a constant expression" / type-restriction errors). Newly *rejected*: nothing that compiles today. Conformance: new spec-derived cases for the classification split, the ownership rejection (each rejected producer kind), the pattern-position diagnostic, and the §7.4 persistence (narrowed const used after an await — the case that fails for `var` with a mutable capture and passes for `const`). Rides the conformance level const locals sit at today; no level change.
- **Train B:** grammar addition is backward-compatible (`sendable` contextual). Stdlib signature edits change no admitted set (§3.2 — kind constraints kept; sendability was already enforced at the same sites by §3.6). One conformance-visible refinement: where an instantiation previously produced the interior §3.6 rejection it now produces the constraint diagnostic — same site, richer content; implementations must emit **one** diagnostic, not both. Conformance: satisfaction/failure cases per §30.14 category (sendable value, string, sealed transfer-closed class, open class, interface, function type), the component-path diagnostic, constraint inheritance on an override.
- **Train C:** docs-currency only; zero program-visible change. The v0.186 migration note already covers the idiom transition ("the `catch ChannelClosedException` loop remains legal").

## 8. Staging

Each train is a self-contained revision-ready unit; recommended order **B → A → C-docs**, though any order works:

1. **Train B** first (smallest normative surface; unblocks stdlib honesty and gives Train A's generic examples a vocabulary if they ever need it). Spec + stdlib + rationale in one ruling-class landing; impl tracker filed at landing (constraint parsing, satisfaction via the existing sendability computation, diagnostic path).
2. **Train A** second (ruling-class; the ownership rule is the piece wanting the adversarial critique pass). Impl tracker: classification, single-assignment enforcement, ownership rejection, §7.4 persistence (deleting invalidation edges for consts is the entire analysis change).
3. **Train C** any time, including batched into either landing's editorial pass (clarification/currency class; the two §30.15 sentences are owner-optional).
4. If landed together: one composition-gate pass over the combined edit set (§6's seam list is the checklist).

## 9. Alternatives considered and declined

**A1 — a new keyword (`let`/`val`/`readonly var`).** Declined; rationale §17.8's recorded reasons stand (a fourth declaration form, decision burden on every local), and §17.8 itself names const-extension as the preferred path. This train is that path.

**A2 — permissive owned `const` (auto-dispose at scope end).** Declined with reasons in §2.2: semantic duplicate of `using var` distinguishable only by an undeclared disposal; forces a bad trilemma on move-out; erases the "const bindings never change state" invariant that makes the §7.4 payoff a two-line spec edit.

**A3 — declare-then-assign-once (Java `final` flow form).** Declined: requires a definite-single-assignment flow analysis for a convenience the expression forms already cover (`const x := a if c else b;`, expression-position `case`); keeps A.6's mandatory initializer, which is also what keeps §7.9 untouched.

**A4 — an explicit marker separating the two classifications (C++ `constexpr`-style).** Declined: the classification is computable, misuse errors loudly at every compile-time position, and a marker taxes every declaration to disambiguate a rare silent flip. Recorded cost accepted in §2.1; revisit only if the flip proves a real defect source.

**A5 — extend runtime `const` to class members.** Declined: `readonly`/`static readonly` already are the member-level runtime single-assignment forms; a member runtime `const` would duplicate them and blur §22.13's const-have-no-initialization-order property. (Also out: `for const` loop variables — the loop variable is freshly bound per iteration already, §21.4.)

**B1 — an open family (any §3.6 property usable as a constraint).** Declined: several properties are not predicate-shaped (element category) or have no consuming rule (trivial-dispose, C-compatible), and an open family is the first step toward the pervasive-marker world §12.5 declined. Individually-admitted-by-ruling keeps each addition a decision.

**B2 — `sendable` as a marker interface (`where T: ISendable`).** Declined: sendability is computed, never declared (§3.6 rule 1); an interface is declared conformance and would let a type assert what its structure denies — or force the compiler to special-case one "interface" as structural, a worse carve-out than one contextual keyword.

**B3 — inference/propagation (auto-trait style).** Declined explicitly; §12.5's decline is load-bearing and this proposal's §3.3 restates it as the boundary. Nothing is constrained unless written.

**B4 — a negated form (`where T: not sendable` or similar).** Declined: no consuming rule wants it, and negations invert the additive-widening property that makes constraint growth non-breaking.

**C1 — re-deriving/re-landing ReceiveAll per the P9 brief.** Declined: §4.2 — the feature is normative since v0.186 at the mandated shape; a second derivation would fork settled text.

**C2 — deleting the stale §18 entry instead of annotating.** Declined: the shipped-marker convention preserves the design trail (two precedents in §18 today), and deletion would hide that the deferral existed — the wrong lesson from a staleness defect whose fix is *more* annotation discipline, not less text.

**C3 — widening Train C to multi-consumer channels.** Declined: out of scope; §18's MPMC entry stands on its recorded remaining blocker (§28.13 domain-locality for the consume capability), which this proposal does not engage.

## 10. Open questions / owner decisions

1. **A-Q1 (the ruling):** ratify the ownership rule — runtime `const` rejects every owned-producing RHS (§2.2)? Recommended: **yes**; the permissive alternative is priced in §9 A2.
2. **A-Q2:** per-instantiation enforcement for substitution-dependent RHS in generic bodies (recommended, §3.6-rule-2 pattern) vs conservative declaration-site rejection?
3. **A-Q3:** confirm no classification marker (computed split, silent-flip cost accepted, §2.1 / §9 A4).
4. **A-Q4:** should a permanently-null runtime const (`const x: T? := null;`) carry §28.3's dead-null-local warning? Recommended: yes (same posture, no new rule).
5. **B-Q1 (the ruling):** ratify the general rule — derived-property constraints admitted individually by recorded ruling, `sendable` first (§3.1)?
6. **B-Q2:** confirm `sendable` as an Appendix B.2 contextual keyword (vs unconditional reservation — declined in §5 as needless breakage surface).
7. **B-Q3:** stdlib spelling — keep the kind constraints alongside `sendable` (`where R: sendable, class`; recommended, zero admission drift, §3.2) vs `sendable`-only rows (would require deciding nullable-type sendability composition first)?
8. **B-Q4:** confirm single-diagnostic subsumption (constraint failure replaces the interior §3.6 rejection at constrained instantiations, §3.1/§7).
9. **C-Q1:** annotate the stale rationale §18 entry with the shipped marker (recommended) — and, procedurally, adopt the implied rule that a §18 entry must be annotated in the same revision that ships it (a one-line addition to spec-change-process's landing checklist; this proposal's §1.C is the evidence for why).
10. **C-Q2:** land the two §30.15 clarification sentences (§4.3 item 3)? Both are behavior-neutral; recommended as riders on whichever train lands first.
11. **C-Q3:** confirm Train C needs no numbered-proposal machinery of its own beyond this audit (it is currency/clarification class per spec-change-process §2, not a ruling).

## Recommendation

**Adopt A and B as drafted; adopt C as the currency train it turned out to be.** Train A is the highest-leverage small change: one keyword's meaning widened along the path the rationale pre-committed to, buying the language's first airtight narrowing guarantee for two paragraphs of spec text. Train B closes a stated expressiveness concession with machinery that already exists, and settles the decide-once question §18 asked. Train C's lesson is procedural and worth the owner's attention independent of the feature: deferred-list entries that outlive their features mislead the project's own planning documents — C-Q1's checklist rule is the durable fix.

**Confidence:** high on A's split rule and B's constraint semantics (both are re-expressions of existing computations); high on the C audit (every claim carries a v0.197 citation verified for this proposal); medium-high on A's ownership rule — the rejection is argued from the state-change invariant and the `using var` duplicate, but reasonable owners could prefer the permissive form, which is why A-Q1 leads the decision list.
