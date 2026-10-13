# Proposal 0043 — Behavioral contracts: `requires` / `ensures` / `invariant`, with tier-tracked enforcement

| | |
|---|---|
| **Status** | graduated-v0.203 — superseded — graduated v0.203 (stages 1 + 4; stage 2 rides 0044, stage 3 the prover remains) |
| **Tracking issue** | [#2030](https://github.com/s8lap/bezel-lang/issues/2030) |
| **Target** | Spec: new §22.16 (contract clauses; numbered after proposal 0042's §22.15, which this proposal depends on), amendments to §22.4 (retro-unification), §11.2/§12.1/§12.5 (clause binding at interface/override/abstract boundaries), §22.7 (constructor clauses and the invariant's construction story), §30.1 (async requires), §31.4 (`ContractViolationException`), Appendix A.4, Appendix B.1/B.2, new Appendix C.15, new Appendix D.2 row (contract proving); stdlib: §2.17 cross-reference only — **no stdlib clause wave in v1** (§14); rationale: one new section (tiers, and why contracts are grammar). |
| **Written against** | spec v0.197 (stdlib/rationale at the same revision) |
| **Origin** | [Future directions 2026-07-18](papers/future-directions-2026-07-18.md) item P3 (Tier 1, the flagship bet). This proposal deepens P3 to graduation quality; divergences from the P3 sketch are recorded in §15. |
| **Depends on** | Proposal **0042** (`pure` methods, #2029): clause admissibility *is* 0042's §4 rule set applied to expressions (§4 below). If 0042's v1 rules move at graduation, this proposal's clause rules move with them — one purity definition, stated once. |
| **Design influences** | SPARK/Ada (every contract at least a runtime check, at best a discharged proof; the checked→proven ratchet; never a proof burden on code that didn't ask) — *not* Dafny (§17). Eiffel (invariant boundary and re-entrancy lessons — adopted; `require else`/`ensure then` refinement syntax and the assertion-suppression flag — declined, §17). C# Code Contracts as the cautionary lineage: library calls instead of grammar, a bolt-on rewriter, no compiler ownership, tooling rot — the failure mode this proposal is shaped to avoid. Bezel's own prototypes: §22.4 `when true` (a shipped proven-tier contract clause), proposal 0031 `Verify.That` (always-on checking), §28.2 checked disposal mode (the declared-rigor build mode). |

*Reference convention: an unprefixed §-reference denotes this proposal's own section when one of that number exists (e.g. §5.3, §9); otherwise it is a language-spec reference (e.g. §28.3, §31.4). The `spec §` / `stdlib §` / `rationale §` prefixes are written out wherever the unprefixed form would collide. References of the form 0042 §N are to proposal 0042's sections.*

## 1. Problem and evidence

The signature is Bezel's unit of promise. Today it can state who owns a value (spec §28), what may cross a domain (§30.14), what a callback may retain (§28.14), and — after 0042 — what a call may touch. It cannot state what a call *does*: that `Withdraw` requires a positive amount, that `Transfer` leaves the balance decremented, that a `Ledger`'s books balance between operations. Those facts live in prose, in hand-written `throw new ArgumentException(…)` prologues, and in tests — three homes with three failure modes (prose drifts, prologues are unverifiable convention, tests live far from the declaration).

Current-state evidence that the language is already growing this feature one clause at a time:

- **§22.4 is a contract clause.** The `when true` nullability postcondition on `out` parameters (v0.183) is a declared, compiler-*proven* behavioral statement in a signature, consumed by caller-side analysis. It is exactly an `ensures` clause with a special-purpose prover — §12 makes that identity normative. Left alone, the language will grow more such one-off clauses (a nonnegativity clause, an emptiness clause), each with its own spelling and matching rules.
- **The validation idiom is unverified convention.** The stdlib's own contracts are documentation plus explicit throws: `Math.Clamp` raises `ArgumentException` on `low > high` (stdlib §2.2), `StringBuilder.Replace` on an empty `oldValue` (stdlib §3.1's contracts paragraph, v0.195). Nothing checks that the prologue matches the prose, and nothing lets a caller or a tool see the condition.
- **0031 chose always-on and stopped at the body.** `Verify.That` (proposal 0031) established that production invariant checks are never elided — but it is a statement in a body: invisible in the signature, inert for overrides, unusable by callers or provers.
- **§28.2 established the declared-rigor mode.** Checked disposal mode is the precedent for a per-build enforcement tier whose two modes agree on every non-violating program. Contracts need the same shape for postconditions (§6.3).
- **The regeneration thesis** (future-directions §0, P3): if bodies become cheap to regenerate, the durable human-owned artifact is the signature plus its behavioral contract, and re-verification against the contract is the acceptance test. That requires contracts to be grammar, compiler-checked, and carried in the artifact interface (§2.8.2, the seam of proposal 0044 — drafted in parallel; not yet landed) — none of which a library or attribute encoding delivers (§17).

The claim this proposal makes normative: *a member's `requires`/`ensures` clauses and a class's `invariant` members are typed, pure, borrow-only boolean predicates with fixed evaluation points, a single violation exception, and a per-clause recorded enforcement tier (proven / checked / assumed) that can ratchet without changing the program's meaning.*

## 2. The design at a glance

```bezel
public class Account
  private _balance: decimal := 0m;
  private _limit: decimal := 0m;

  invariant _balance + _limit >= 0m;            // class invariant (§9; ships at stage 4)

  public constructor(limit: decimal)
    requires limit >= 0m;
    _limit := limit;
  end constructor;

  public method Withdraw(amount: decimal)
    requires amount > 0m;
    requires amount <= _balance + _limit;
    ensures _balance = old(_balance) - amount;
    _balance := _balance - amount;
  end Withdraw;
end Account;
```

The moving parts, each decided in the sections that follow:

- **Grammar, not attributes.** `requires E;` / `ensures E;` clauses sit between a method's or constructor's signature and its body; `invariant E;` is a class-body member. Every clause ends with `;` (rationale §3.1's always-semicolons rule; the P3 sketch's semicolon-free `static ensures result >= 0` is corrected here). `old(E)` and `result` are clause-scoped contextual keywords. (§3)
- **Clauses are pure borrows.** A clause is a `bool` expression admitted under 0042's pure-body rules: it may read anything reachable, call only pure members, and can neither consume, dispose, move, store, nor `await`. Bezel's statement/expression split does half the work — assignment and `++`/`--` are statements and cannot be spelled in an expression at all. (§4)
- **Fixed evaluation points.** `requires` after argument binding, before everything else; `ensures` on every normal return edge, after result binding, before scope-end disposals, reading `result` as a borrow; `throw` exits skip `ensures`; `old(E)` evaluates once at entry. (§5)
- **One violation exception.** A new built-in `ContractViolationException` (§31.4), catchable. `requires` is always-on, like 0031's `Verify.That`; `ensures`/`invariant` run under a **checked-contracts mode** specified in §28.2's two-mode shape. The `ArgumentException`-compatible alternative is presented and declined with reasons. (§6)
- **Tier-tracked enforcement.** Every clause is **proven**, **checked**, or **assumed**; the tier is static, per-clause, and (stage 2) recorded in the artifact interface. The v1 prover is the honest floor: §17.14 constant folding plus the §22.4 nullability-flow analysis. `static requires`/`static ensures` *demands* the proven tier, with the discharge site fixed per clause kind. (§7)
- **Behavioral subtyping.** Overrides and implementations inherit clauses and may add `ensures`; they may not add `requires`. Interface-declared contracts bind implementors; abstract members carry clauses. (§8)
- **Invariants at the public boundary** with a `this`-reentrancy exemption — the Eiffel lessons adopted, the Eiffel machinery (suppression flags, refinement keywords) not. (§9)
- **Scope discipline.** Async methods: `requires` only in v1, with the ensures-at-settlement shape recorded; iterators deferred; accessors, operators, lambdas, extern: no clauses in v1, each with reasons. (§10)
- **§22.4 becomes an instance, not a sibling.** `when true` is normatively a `static ensures` clause; zero migration. (§12)

Why grammar residence is non-negotiable, restated once: Bezel deliberately has no attribute system (rationale §18, with the proliferation and stringly-typed-magic costs recorded there), and §22.4 already chose the compiler-recognized-clause route over attributes for exactly this kind of declaration. A contract that the compiler does not own is documentation with extra steps — the C# Code Contracts outcome. Every subsequent section assumes clauses are first-class syntax the compiler type-checks, inherits, evaluates, and records.

### 2.1 Proposed opening text for the new spec §22.16

> A method or constructor declaration may carry **contract clauses** between its signature and its body — precondition clauses **`requires E;`** and postcondition clauses **`ensures E;`** — and a class body may declare **invariant members** (**`invariant E;`**). Each clause's expression is of type `bool`, is admitted under the pure-body discipline of §22.15 with the amendments of this section, and can neither write state, consume or dispose a value, nor suspend. A `requires` clause is evaluated on every invocation, after argument binding and before any execution of the member; a clause evaluating `false` raises `ContractViolationException` ([§31.4](#sec-standard-exception-types)), in every build mode. An `ensures` clause is evaluated on every normal-completion path, after the return value and every `out` slot are bound and before the frame's scope-end disposals; within it the contextual keyword `result` denotes the returned value, read as a borrow, and `old(E)` denotes the value `E` had at entry; a path that exits by `throw` evaluates no `ensures` clause. The class invariant — the conjunction of a class's and its base classes' invariant members — is evaluated at the boundaries this section fixes: construction exit and the public entry and exit points, with `this`-receiver calls exempt. Every clause occupies exactly one recorded enforcement tier — **proven**, **checked**, or **assumed** — and the `static` clause qualifier demands the proven tier: a `static` clause the implementation cannot discharge statically is a compile error. Clauses bind overrides and implementations ([§12.1](#sec-members-are-non-virtual-by-default), [§11.2](#sec-interface-declaration)): they are inherited, never restated; an override or implementation may add `ensures` clauses and may not declare `requires` clauses. Contract clauses are part of the member's contract, carried by the artifact interface ([§2.8.2](#sec-artifact-interface)); they never distinguish overloads ([§22.6](#sec-overloading)).

## 3. Grammar delta

### 3.1 Clause syntax and placement (Appendix A.4)

One new production, threaded into `MethodDecl` and `ConstructorDecl` between signature and body:

```
ContractClause  ::= "static"? ("requires" | "ensures") Expression ";"

MethodDecl      ::= Visibility "static"? "async"? VirtualKind?
                    "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)?
                    GenericConstraints* ContractClause*
                    MethodBody

ConstructorDecl ::= Visibility "constructor" "(" ParamList? ")"
                    ContractClause* CtorChain? Statement* "end" "constructor"? ";"
                  | "static" "constructor" "(" ")"
                    Statement* "end" "constructor"? ";"
                              /* the static-constructor alternative admits no
                                 clauses (§10.4) */
```

Textual order mirrors evaluation order: on a constructor, `requires` clauses precede the `base(...)`/`this(...)` chain because they *run* before it (§5.1) — the arguments are validated before the base constructor consumes them. `ensures` clauses are written in the same leading region although they evaluate at exit; clauses are declaration-region items, not statements, and grouping the whole contract ahead of the body is what makes it readable as the member's specification (the SPARK/Dafny/Eiffel convention, and the shape the P3 sketch already used). `ExtensionMember` reuses `MethodDecl` (Appendix A.4), so extension methods inherit the clause region with no further grammar (§10.5). *(Composition note: the productions above are shown against the v0.197 baseline; proposal 0042 amends the same productions, widening `"async"?` to `("async" | "pure")?` on `MethodDecl`/`ConstructorDecl` and adding the `"pure"?` slot to the interface-member rules. A landing editor applies both deltas merged — neither supersedes the other.)*

**Parse disambiguation (normative).** After a signature's `GenericConstraints*` (and, in a constructor, after the parameter list), the tokens `requires`, `ensures`, and `static` immediately followed by either, open the clause region; the region is the maximal run of `ContractClause`s. In that position the keyword reading always wins — the Appendix B.2 `env` precedent. The only reachable collision is a *first body statement* that begins with a bare identifier named `requires` or `ensures` (necessarily a parameter or static so named — no local is in scope yet): parenthesize (`(requires)(x);`) or rename. No statement begins with `static` (locals cannot be static), so the qualified form is unambiguous. `MethodBody`'s block form needs no opening token: the clause region ends at the first token that is not a clause opener, exactly as the region began.

### 3.2 Body-less members: abstract and interface declarations

Abstract methods and interface members may carry clauses (§8 makes them the *natural* place for clauses — the slot's contract). Their current body slot is a lone `;`; with clauses, the final clause's semicolon closes the declaration and the lone `;` is omitted:

```
IfaceMethod       ::= "method" Identifier GenericParams?
                      "(" ParamList? ")" (":" "owned"? Type)?
                      ( ";" | ContractClause+ )
IfaceStaticMethod ::= "static" "method" Identifier GenericParams?
                      "(" ParamList? ")" (":" "owned"? Type)?
                      ( ";" | ContractClause+ )
```

and, for `MethodDecl`, the `";"` alternative of `MethodBody` is reachable only with zero clauses; a clause-bearing abstract method ends at its last clause (writing an extra `;` after it is an error — there is exactly one way to spell each form):

```bezel
public abstract class Shape
  public pure abstract method Area(): decimal;            // 0042; clause-free: lone ";"

  public abstract method Scaled(factor: decimal): owned Shape
    requires factor > 0m;                                 // clause closes the declaration
end Shape;
```

The parser knows the member is body-less when it reaches the clause region (`abstract` was parsed in `VirtualKind`; interface members are body-less by production), so no lookahead beyond the existing token is needed. The considered alternatives — a required trailing `;` after the clause list (`requires factor > 0m;;`), or promoting clause-bearing abstract members to `end`-closed blocks — are recorded and declined: the first is a double-semicolon with no reading, the second makes clause addition restructure the declaration. `IfaceProperty`, `IfaceIndexer`, `PropertyDecl`, `IndexerDecl`, `OperatorDecl`, `DestructorDecl`, and lambdas are unchanged — no clauses in v1 (§10).

### 3.3 The class-body `invariant` member (Appendix A.4)

```
Member          ::= FieldDecl | MethodDecl | PropertyDecl | IndexerDecl
                  | ConstructorDecl | DestructorDecl
                  | OperatorDecl | NestedTypeDecl | ConstDecl
                  | PolymorphicMember | InvariantDecl

InvariantDecl   ::= "static"? "invariant" Expression ";"
```

An invariant member takes no visibility (the runtime and the declaring class are its only consumers — the `PolymorphicMember` precedent), may appear any number of times, anywhere among the members. Parsing is unambiguous: every field, method, property, and nested-type member begins with a required visibility keyword (§13.3), so a member-leading `invariant` token can only open an `InvariantDecl`; `static invariant` versus `static constructor` is two-token lookahead, which the grammar already spends elsewhere. `RecordBody` reuses `Member`, so an invariant *parses* in a record and is rejected semantically (v1 is classes only, §9.6) — the house over-admit-and-reject pattern (Appendix A.4's own commentary). The `static` qualifier demands the proven tier, as on clauses (§7); it does not make the invariant a static-state predicate (§9.6 rejects invariants in static classes).

The grammar ships in two pieces: `ContractClause` at stage 1, `InvariantDecl` at stage 4 with its semantics (§16) — a parsed-but-unenforced invariant would be a silent lie, the one thing a contract must never be.

### 3.4 Keyword status (Appendix B) and the collision audit

Five words join the **contextual keyword** machinery — B.1's contextual list and B.2's table. Verified against v0.197 as follows; none is a reserved word today.

| Word | Reserved context (B.2 row) | Collision audit (v0.197) |
|---|---|---|
| `requires` | Opening a `ContractClause` in the clause region of a method or constructor declaration | Prose uses only ("requires `i.IsInterface`", stdlib §2.13); no stdlib identifier — member names are PascalCase, and no parameter or type is so named. The clause-region marker-wins rule (§3.1) covers the pathological body-leading identifier. |
| `ensures` | Same | No occurrence as an identifier anywhere in the normative docs. |
| `invariant` | Member-leading position in a class body (optionally after `static`) | Prose only ("invariant-culture", stdlib §2.18); fields cannot occupy the colliding position (visibility keyword required first, §13.3). |
| `old` | Inside `ensures` clauses, where followed by `(` | No stdlib parameter, member, or type named `old` (`oldValue`/`OldValue` are distinct identifiers and unaffected). Within a clause the reading is *total*: clause purity prohibits invoking function-typed values (0042 §4.4 item 4), so `old(` can never be a legal call of a value named `old` — the keyword reading forecloses nothing expressible. Outside `ensures` clauses, `old` stays an ordinary identifier everywhere. |
| `result` | Throughout `ensures` clauses | **The one real collision, resolved by rename-ability.** The stdlib names parameters `result`: the shape-(c) Try family's `out result: T` (stdlib §1; `Convert.TryTo*`, `DateTime.TryParse`, …) and the completion sources' plain `SetResult(result: T)`/`TrySetResult(result: T)` (stdlib §6.6). Inside an `ensures` clause the keyword reading always wins (the B.2 `env` rule): a parameter named `result` is not denotable there — rename it to use it. For `out` parameters the rename is **call-site-invisible**: `ref`/`out`/`owned` arguments are positional-only (§22.5, v0.160), so no caller names them. The plain `SetResult(result:)`/`TrySetResult(result:)` parameters *are* nameable at call sites; there the resolution is the same keyword-wins rule — rename the parameter to denote it in an `ensures` clause. (`SetResult` is unit-returning, so `result` the keyword is an error in its clauses anyway per §5.2; `TrySetResult` returns `bool`, so in its `ensures` clauses `result` denotes that `bool` and the parameter is shadowed.) Their natural clauses are `requires`, where the parameter reading holds. Outside `ensures` clauses, `result` stays an ordinary identifier. |

`requires`/`ensures`/`invariant`/`old`/`result` are all lowercase, per B.1's rule that keywords are; none touches B.3's `event` reservation. No existing program changes meaning: every reserved context above is a syntactic position that does not exist in v0.197 programs.

## 4. Clause typing: `bool`, pure, borrow-only, no `await`

The following is proposed normative-quality text for the core admission rule of the new spec §22.16.

> A contract clause's expression — and each operand of `old` within it — is type-checked in the context of the declaring member: the member's parameters (all modifiers), `this` where the member has one, and the enclosing scopes' static and type members are in scope; the member's body locals are not. The expression of a `requires`, `ensures`, or `invariant` clause must be of type `bool` ([§4.1](Bezel_Language_Spec.md#sec-bool)'s conditions-are-`bool` discipline; no truthiness, rationale §17.1). The expression is admitted under the **pure-body rules of §22.15** (0042 §4.3/§4.4), applied expression-wise, with three clause-specific amendments:
>
> 1. **`throw` is rejected.** A clause states a proposition; the mechanism, message, and blame of a violation belong to this section's machinery (§6), not to the clause text. The pure fragment's throw carve-out (0042 §4.5) remains available to *callees* of the clause. A `throw` expression — including inside `??` or an `if` expression — is a compile error in a clause.
> 2. **`await` is rejected**, on every member including `async` ones: a clause is not part of an async body's statement sequence and has no suspension semantics (Appendix C.6 gains the bullet). §10.1 records the settlement-time design this preserves room for.
> 3. **`old` and `result`** are admitted only as §5.2/§5.3 define; anywhere else each is an ordinary identifier (§3.4).

The borrow-only guarantee — a clause may not consume, dispose, move, or reassign — is not a new checker; it is the sum of existing classifications, each prohibition tied to the machinery that already detects it:

- **No stores.** Assignment, compound assignment, and `++`/`--` are *statements* in Bezel (§22.2: "an assignment produces no value"; rationale §14.1) — a clause is an expression, so mutation cannot even be spelled. Stores reachable *through calls* are excluded because callees must be pure (0042 §4.4 item 1).
- **No owned-consuming positions can arise.** Walk §28.3's owned-consuming list against a clause expression: `owned` arguments cannot occur (pure callees declare no `owned` parameters, 0042 §4.2); `move` (§28.8.1) is an owned-producing form a pure expression prohibits (0042 §4.4 item 2); the `throw` operand is rejected above; the discard `_ := …` (§17.13) and the marked-transfer binding forms are statements; `set(owned)` stores are stores. A read of a *named owned binding* in a clause is therefore always §28.3's **unmarked borrow read** — the aliasing read that "never silently consumes" — on every path.
- **No disposal.** `Dispose()`/`DisposeAsync()` are never pure-declared (0042 §13, stdlib policy), so the pure call rule excludes them; no `using var` binding exists in an expression.
- **Ownership-state composition at return edges.** An `ensures` clause reading an `owned` parameter (or any owned binding visible to it) is checked against §28.3's per-path ownership state *at each return edge*: on a path where the binding was moved out, the clause's read is a moved-from read and a compile error naming the consuming site. The honest idiom for contracts over consumed inputs is `old(…)` (§5.3), which captures before the body runs.

Purity is what makes the whole tier system coherent (§7): because a clause has no effects, evaluating it, eliding it, or proving it are indistinguishable to every non-violating program — the property none of the ancestor systems (Eiffel's side-effecting assertions, C# Contracts' arbitrary expressions) had, and the reason 0042 precedes this proposal.

Flow-sensitivity inside a clause is the ordinary expression machinery, unchanged: `requires x <> null and x.Count > 0;` narrows `x` across the `and` per spec §7.4; `is` tests narrow per §12.7; both are reads.

### 4.1 Admission gallery (accept and reject, each tied to its rule)

```bezel
public class Cache
  private owned _conn: Connection;
  private _hits: int := 0;

  public method Touch(key: string): int
    requires key.Length > 0;             // OK — pure call chain (string.Length: 0042 §13 wave 1)
    ensures result >= 0;                 // OK — result read (§5.2)
    ensures _hits = old(_hits) + 1;      // OK — value-typed old capture (§5.3)
    _hits := _hits + 1;
    return _conn.Ping(key);
  end Touch;

  public method Bad(f: () -> bool): bool
    requires _hits := 1;                 // does not even parse: assignment is a statement (§4)
    requires Warm();                     // error: Warm is not pure-declared (0042 §4.3 rule 6)
    requires f();                        // error: invoking a function-typed value (0042 §4.4 item 4)
    requires result > 0;                 // error: `result` is ensures-only (§5.2)
    ensures Ready() or throw new Exception("not ready");
                                         // error: `throw` in a clause (§4 amendment 1) —
                                         //   bottom-typing would admit it; the rule rejects it
    ensures Take(move _conn) <> null;    // error: `move` cannot appear in a clause (§4);
                                         //   and no pure callee could receive it (0042 §4.2)
    return true;
  end Bad;
end Cache;
```

(`Connection.Ping`, `Warm`, `Ready`, `Take` stand for user members; the annotations name which rule rejects each clause, which is also the shape of the stage-1 reject-test matrix.)

## 5. Evaluation semantics

### 5.0 The boundary order, in one table

| Point | Order of operations |
|---|---|
| Member entry | bind arguments — supplied in call-site textual order, then defaults in declaration order (§22.5) → `requires` clauses, declaration order, stop at first `false` (§6) → `old` operands, textual order (§5.3) → *constructors:* `base(...)`/`this(...)` chain → field initializers (§22.8) → body |
| Normal return edge | compute and bind the return value; every `out` callee-local slot holds its final value (§22.3) → `ensures` clauses, declaration order → *instrumented boundaries (stage 4):* the class invariant (§9.2) → scope-end disposals and auto-drops (§28.3) → `out` copy-out and return (§22.3) |
| Throw exit | no clause evaluation of any kind; the ordinary §28.3 exception-path discharge unwinds the frame |

### 5.1 `requires`

> **`requires` clauses** are evaluated on every invocation of the member, after argument binding completes — supplied arguments in §22.5's call-site textual order, then defaults in declaration order, exactly as for any call — and before any of the member's own execution: before a constructor's `base(...)`/`this(...)` chain (§12.8), before field initializers (§22.8), before the first body statement. Clauses evaluate in declaration order; evaluation stops at the first clause producing `false`, and the invocation faults with `ContractViolationException` (§6) raised *from the member* (the callee's frame exists; its parameters were bound; no body effect has occurred).
>
> A `requires` clause of an **instance method** may observe `this` (a borrow, §28.3) and its fields. A `requires` clause of a **constructor** may not observe `this` in any of §22.7's observation senses — at clause time not one field of the declaring class is assigned, the same reason §22.7 rejects observation in the chaining position; the clause reads parameters and statics only. An `out` parameter is unassigned at entry and unreadable in `requires` clauses — §7.9's definite-assignment rules, applied verbatim, no new rule.
>
> On a `this(...)`-chaining constructor, the chained-to constructor's own `requires` clauses evaluate when the chain invokes it — each constructor validates its own arguments; nothing is inherited across the chain.

A rejected call evaluates no `old` operand and runs no body statement: the object and all arguments are exactly as the caller left them (clauses are pure), which is what makes `requires` the declarative replacement for the throw-prologue idiom — same timing, same observability, checked shape. The boundary between `requires` and *expected failure* is guide 2 §5/§6's rule, unchanged and load-bearing: input whose invalidity is a normal outcome at a trust boundary (user text, wire data) is handled with Try shapes and nullable returns, never with `requires`; a `requires` states what a *correct caller* already guarantees, and its violation is a bug report, not a result.

### 5.2 `ensures` and `result`

> **`ensures` clauses** are evaluated on every **normal-completion (return) path** of the member — a `return` statement, a unit-typed method falling off its end, an arrow body's completion, a constructor body's completion — and never on a path that exits by `throw`: the same normal-completion boundary as §22.3's `out`-assignment obligation (v0.184) and §22.4's stated-constant paths. The evaluation point on a return edge is precisely: **after** the return value (if any) is computed and bound and after every `out` parameter's callee-local slot holds its final value, and **before** the frame's scope-end work — before `using var` disposals, owned-local auto-drops (§28.3), and `out` copy-out (§22.3). Clauses evaluate in declaration order; a `false` clause raises `ContractViolationException` from the return site. Because the clause runs before the unwind, every borrow the frame holds is still live for it.
>
> Within an `ensures` clause of a member with a declared return type, the contextual keyword **`result`** denotes the value being returned, typed as the declared return type. `result` is a **read-only borrow position** (§28.3): for a class- or interface-typed return — `owned` or plain — the clause observes the referent without consuming, disposing, or extending it, and clause purity guarantees the borrow cannot escape (there is no store to escape into); the ownership transfer the return performs is unaffected and completes after the clause. For value, record, enum, and `string` returns, `result` denotes the returned value. `result` is a compile error in a unit-typed member's clauses, in a constructor's clauses, and in any `requires` or `invariant` clause.
>
> An `ensures` clause may read `out` parameters — it reads the callee-local slot of §22.3's copy-out model, definitely assigned on every normal path by §22.3's own rule — and `this`, fields, statics, and plain parameters as borrows. On a constructor, `ensures` evaluates at body completion, where every §7.9-required field is assigned; observing `this` there satisfies §22.7's observation gate by construction.

Reading `result` as a borrow *before* scope-end disposal is load-bearing in both directions: a plain (borrowed) class return whose source is a frame binding is still valid at the clause; and an `owned` fresh return is observed exactly once, in flight, with no second owner ever arising — no interaction with Appendix C.3's disposed-exactly-once invariant.

A full trace, numbered against §5.0's table:

```bezel
public method Take(n: int): owned Batch
  requires n > 0;
  requires n <= _pending;
  ensures result.Count = n;
  ensures _pending = old(_pending) - n;
  using var w := _source.OpenWindow();
  var b := w.CopyOut(n);           // CopyOut returns owned Batch
  _pending := _pending - n;
  return b;
end Take;
```

On `repo.Take(3)`: (1) `n := 3` binds; (2) `n > 0`, then `n <= _pending` evaluate — a `false` here raises before `w` ever exists; (3) `old(_pending)` captures the `int` by copy; (4) the body runs; (5) at `return b;` the owned local `b` is consumed into the result slot (§28.3's owned-consuming return position); (6) `result.Count = n` reads the in-flight result as a borrow (`Batch.Count` an auto-property getter — implicitly pure, 0042 §4.7, no annotation needed), then `_pending = old(_pending) - n` compares the field against the entry capture; (7) — stage 4 — the invariant, if `Take`'s class declares one and `Take` is public; (8) `w` is disposed at scope end; (9) ownership of the result transfers to the caller. A violation at step 6 raises with `w` still live; the raise then unwinds through the ordinary exception path, disposing `w` and discharging the in-flight owned result (§28.3's exception-path discharge — no special rule).

### 5.3 `old(E)` — the prestate operator

> Within an `ensures` clause of a **method**, **`old(E)`** denotes the value `E` had at the member's entry. Semantics are defined by desugaring: for each `old` operand across the member's `ensures` clauses, in textual order, the compiler introduces a hidden entry binding equivalent to `var __oldN := E;`, evaluated exactly once per invocation, immediately after the last `requires` clause passes (a rejected call evaluates none), and each `old(E)` in a clause reads its binding at the return edge. Textually identical operands may share one binding — sharing is unobservable because operands are pure (the as-if rule). Every rule that governs a user-written binding governs the hidden one; `old` adds **no new ownership machinery**:
>
> - **Value, record, enum, and `string` operands are captured by copy.** A record copy carries its fields; a **borrow-bearing record** operand (§9.7 — a span) is admitted with its carried borrows subject to §9.7's ordinary rules, the hidden binding's scope being the member body.
> - **Class- and interface-typed operands are captured as a reference — a frame-resident borrow** whose source is whatever sourced `E` at entry (§28.3's producer table). The lifetime story is the standard one: the borrow lives to the return edge, so its sources must outlive the body. For local and parameter sources the **consume-site freeze** (§28.3) rejects a marked transfer of the source while the capture is live, and an explicit `Dispose()` of the source is the announced kill with the §28.2 checked-mode backstop; for field-path sources the capture is in the same **residual-trust regime** as every field-held borrow — structural checks static, temporal guarantee the programmer's, violation a deterministic `DisposedAccessException` in checked disposal mode.
> - **`old` is shallow — normatively stated.** A class-typed capture holds the entry *reference*; member reads through it in the clause observe the referent's **post-state**. `old(_items).Count` reads the count *now* through the entry reference — almost never what is meant; `old(_items.Count)` captures the entry count. The recommended idiom is value-typed projections; a compiler warning on a class-typed `old` capture that is dereferenced for a member in the clause is cheap and proposed on by default (diagnostic-identity latitude per Appendix D).
>
> `old` is a compile error: in `requires` clauses (vacuously the current state), in `invariant` members (single-state predicates), in constructor clauses (there is no prestate — fields sit at §22.7's null watermark), and outside clauses entirely (§3.4). Nesting `old(old(E))` is a compile error.

The stricter alternative — rejecting class-typed `old` operands outright, forcing projections — was considered; §18 Q6 records it. It buys freedom from the shallow trap and the residual at the price of every reference-identity and object-valued postcondition; the desugaring-based semantics above costs no new machinery, so admitting with the warning is recommended.

### 5.4 Faults during clause evaluation

> A clause expression may fault — an indexer raising `IndexOutOfRangeException`, arithmetic overflow (spec §6.1), a pure callee's own validation throw (0042 §4.5), or a nested contract violation in a pure callee (which propagates like any other exception). The fault propagates from the clause's evaluation point **as itself**: it is never converted to `ContractViolationException`, because the clause did not evaluate to `false` — it failed to evaluate, which is a defect in the *contract*, and the original exception is its diagnosis. At an `ensures` point the fault turns the completion exceptional: later clauses are unevaluated, and the ordinary exception-path machinery (§28.3) unwinds the frame — an in-flight owned result is discharged by the standard exception-path discharge, no new rule.

### 5.5 No nested-check suppression

Clause evaluation calls pure members; those members carry their own clauses, which check normally during the outer clause's evaluation — checks compose as ordinary calls. Eiffel's global assertion-evaluation flag (nested assertions silently unchecked) is **declined**: a hidden per-thread mode that changes call semantics is exactly the invisible magic the language avoids, and the two real recursion sources are already closed — invariant evaluation runs on the unchecked face (§9.3), and clause purity keeps evaluation effect-free so re-entry cannot corrupt state. A genuinely cyclic `requires` chain (`P`'s clause calls `Q`, whose clause calls `P`) is a program bug that manifests as `StackOverflowException` (§31.4), the same way any other unbounded recursion does.

### 5.6 Clause granularity is semantic

`requires a; requires b;` is not a style variant of `requires a and b;`. The clause is the unit of four things: **tier** — each clause is tiered independently (§7), so the provable half of a condition ratchets to `static` while the other half stays checked; **blame** — a violation names its clause, so the split form points at the exact failed condition where the conjunction reports the whole; **order** — clauses evaluate in declaration order with stop-at-first-failure (§5.1); and, at stage 2, **diff identity** — adding a clause is a classified interface event, while editing a conjunction inside one clause is a clause replacement. Recommended style, non-normative: one condition per clause. (Runtime behavior of the two spellings is otherwise identical — `and` short-circuits inside a clause exactly as the clause sequence short-circuits across them.)

## 6. Failure semantics: `ContractViolationException` and the enforcement modes

### 6.1 The exception (decided)

A new **built-in** exception type joins the §31.4 hierarchy, deriving directly from `Exception`:

```
Exception
├─ …
├─ ContractViolationException      — a requires/ensures/invariant clause evaluated false.
```

```bezel
public class ContractViolationException: Exception
  public constructor(message: string);
end ContractViolationException;
```

Built-in placement is the §31.4 principle applied: "an exception is built-in iff its only producer is a built-in operation" — clause evaluation is a language operation, like the indexer's `IndexOutOfRangeException` and the `case` construct's `MatchFailureException`. The type is available without import in every conformance level (a Core implementation runs the checked tier, §13), and stdlib §2.17's built-in cross-reference table gains the one row — the whole stdlib delta of stage 1. The v1 surface is minimal — the `Message` carries the clause kind (`requires`/`ensures`/`invariant`), the member, and the clause's source text; a structured surface (a clause-kind enum, the declaring type) is deferred to stage 2, where the artifact interface fixes a machine-readable clause identity to point at. The exception is **catchable** (everything but `StackOverflowException` is): test frameworks must be able to assert violations (`Assert.Throws`-shaped, proposal 0022), and a supervisory boundary must be able to log-and-contain a callee's contract failure. Programs must not use it for control flow — it reports a bug, not an outcome; guide 2 §5's expected-failure rule (return a value, don't throw) applies with full force *before* any `requires` is written (§5.1).

### 6.2 `requires` violations are always-on

> An unproven `requires` clause is compiled as a runtime check in **every** build mode; it is never elided. (A **proven** clause — §7 — is discharged statically and compiles to nothing; elision-by-proof is the only elision.)

This is 0031's `Verify.That` posture promoted to the boundary, for 0031's reasons: a violated precondition is a caller bug you want caught in production, and the clause *replaces* the always-on `throw new ArgumentException(…)` prologue — an elidable replacement would be a silent behavioral downgrade of the code it replaces, the `Debug.Assert` failure mode the gap review recorded. The cost on hot paths is real and has a designed remedy that is not a knob: ratchet the clause to proven (`static requires`, §7.1 — discharged per call site), or restructure so the boundary is crossed less often. §17 declines the build-knob alternative.

An implementation **may** elide the runtime check at a *particular call site* it can statically discharge, even for an unqualified clause — unobservable by purity, the as-if rule — but the clause's recorded tier stays **checked**: the tier is the contract's guarantee floor, not a per-site optimization fact.

### 6.3 `ensures` and `invariant` run under checked-contracts mode

> How postcondition and invariant clauses are enforced is the *contract-checking mode*, a per-build choice between two conformance modes, independent of §28.2's disposal-checking mode:
>
> - **Unchecked (release) mode** — unproven `ensures` and `invariant` clauses are **not evaluated**. This is the default.
> - **Checked-contracts mode** — unproven `ensures` and `invariant` clauses are evaluated at the points §5.2 and §9.2 fix; a clause evaluating `false` raises `ContractViolationException`.
>
> The two modes give identical observable behavior for every program whose checked clauses never evaluate `false` and never fault: clauses are pure (§4), so not evaluating one is unobservable by construction. This is the deliberate contrast with §28.2's shape: there the release relaxation is *undefined behavior*, because the fault being relaxed is a memory error the program cannot outrun; here the relaxation is *non-evaluation*, and a violating program's behavior remains fully defined — it is whatever the body's ordinary semantics produce, with the postcondition silently unmet. `requires` clauses are outside the mode entirely (§6.2).

Why postconditions get a mode when preconditions do not: a `requires` guards *callers* and fires at boundaries the caller already pays for; `ensures` and `invariant` run on **every normal return edge** and at **every public boundary** respectively — cost proportional to the callee's own hot loop, paid to catch the *implementation's* bugs, which is precisely the class of checking a production build may reasonably retire once the implementation is trusted — *visibly*, as a recorded build-mode choice, never clause-by-clause silently. The mode's granularity is per-build (the §28.2 precedent); per-module or per-clause knobs are declined as mode proliferation (§17).

### 6.4 The `ArgumentException`-compatible alternative, presented and declined

The P3 sketch leaned toward raising the `ArgumentException` family for `requires` violations "so existing `catch` code is unaffected," with a dedicated type only for `ensures`/`invariant`. The alternative is real and has one genuine advantage — migration transparency: a method whose prologue `if amount <= 0m then throw new ArgumentException(…); end;` becomes `requires amount > 0m;` would keep faulting with the type its callers may already catch. It is declined, for four reasons:

1. **The blame mapping does not exist.** `ArgumentException` names a parameter (`ParamName`); a clause names a *condition* — `requires amount <= _balance + _limit;` blames a parameter and two fields jointly. Synthesizing a `paramName` would be wrong more often than right; omitting it yields an `ArgumentException` that violates its own surface's expectations.
2. **Two types for one mechanism.** Splitting requires→`ArgumentException`, ensures→`ContractViolationException` makes the *same* clause syntax fault differently by kind, and makes the §7 tier story asymmetric for no checking benefit. One mechanism, one exception; the kind lives in the message (and in stage 2's structured record).
3. **Catch semantics should distinguish convention from contract.** Existing `catch var e: ArgumentException` arms were written for hand-validation of *expected* bad input at trust boundaries. A contract violation is a *bug* — caller bug for `requires`, callee bug for `ensures`/`invariant` — and silently routing bugs into recovery arms written for input validation inverts guide 2 §6's tool-choice table. A new type makes the escalation visible at exactly the sites that must decide.
4. **The honest cost is bounded and recorded.** Migrating a specific member's prologue to `requires` changes the exception type its violating callers observe. That is a per-member, opt-in, source-visible change — the §3.6-rule-3-style documentation discipline applies — and it is the reason v1 ships **no stdlib clause conversions** (§14): the stdlib's documented `ArgumentException` contracts stand until an owner decision (Q7) converts them deliberately.

Also declined: per-kind exception triples (`RequiresViolationException`/…) — taxonomy bloat, reason 2 verbatim; and deriving `ContractViolationException` from `ArgumentException` — inherits reason 1's false surface and mis-taxonomizes `ensures`.

## 7. The tier system: proven / checked / assumed

> Every contract clause of every member occupies exactly one **tier**, determined statically per clause:
>
> 1. **Proven** — discharged statically at the clause's discharge site (§7.1); never evaluated at runtime; zero cost in every build mode. A proven clause cannot be violated by any execution (prover soundness is a conformance obligation).
> 2. **Checked** — compiled to a runtime evaluation at this proposal's points, governed by §6's mode rules (`requires`: always; `ensures`/`invariant`: checked-contracts mode).
> 3. **Assumed** — stated and carried, neither proven nor evaluated. **Unpopulated in v1**: no v1 construct produces an assumed clause. The tier exists in the model — and, at stage 2, in the artifact-interface format — for the recorded extension that needs it: contract clauses on `extern` declarations (§10.4), where the same trust question as 0042's Q8 (extern purity) recurs and the two are to be designed jointly. Reserving the tier now costs one enum value and saves a format revision.

### 7.1 Discharge sites differ by clause kind (decided)

"Proven" means something different per kind, and the difference is normative — the terse "the compiler discharges the clause" hides it:

- An **`ensures`** or **`invariant`** clause is discharged at the **declaring body** (bodies, for inherited clauses): the prover shows every normal-completion path establishes the postcondition, every instrumented boundary preserves the invariant. `static ensures`/`static invariant` therefore makes an unprovable *body* a compile error at the declaration — exactly §22.4's existing rule ("a violating path is a compile error in the declaring body").
- A **`requires`** clause has nothing to prove at the declaration — it is the body's *assumption*. Its discharge site is **every call site**: the caller's compiler substitutes the actual argument expressions (and the caller's own known facts) into the clause and discharges the instance. `static requires` therefore makes an undischargeable **call** a compile error at that call, the diagnostic naming the clause and the failing condition; the callee compiles no runtime check because every admitted call carries a proof. Cross-module, the artifact interface transports the clause (stage 2) and each importing compilation discharges its own calls — which is why carriage precedes the prover in the staging (§16).

Consequence stated plainly: `static requires` is a *strong API demand* — it rejects callers, not bodies — and in v1 (the constant floor below) it is discharged only where the substituted clause becomes a §17.14 constant folding `true` (`Take(3)` against `static requires n > 0;` folds `3 > 0`; `Take(k)` does not fold and is rejected). Its practical life, like `static ensures`', begins at stage 3.

One visibility consequence is itself normative: a **`static requires` clause may mention only state its call sites can see** — the member's parameters, and members visible to every caller (for an exported member, `public` state). A `static requires` over non-public state is a compile error *at the declaration*: no call site could ever discharge a condition it cannot name, so the demand is unsatisfiable by construction. **Checked** `requires` clauses are unrestricted (they evaluate callee-side, where the state is visible), as are `ensures` and `invariant` clauses of every tier (discharged declaring-side; caller-side *consumption* of an `ensures` is possible only for the caller-visible part, which is exactly the §22.4 case — `result` and the `out` recipient are the caller's own).

### 7.2 The v1 floor

> The v1 prover is deliberately minimal, and exactly two things inhabit the proven tier:
>
> - A clause whose expression — after call-site substitution, for `requires` — is a **constant expression** (§17.14) folding to `true` is proven. Folding to `false` is a **compile error at the clause** (a statically-violated contract; for a `requires`, at the call). A constant clause whose folding faults is a compile error per §17.14's own fault rule.
> - The `when true`/`when false` postconditions of §22.4 — which §12 defines as `static ensures` clauses — are proven by the existing spec-§7 nullability flow analysis over the declaring body, exactly as §22.4 already mandates. This is not new machinery being claimed; it is shipped machinery being *named* as the prover it is.
>
> Since real clauses mention parameters and fields, essentially every authored v1 clause is **checked**. That is the deliberate floor: the runtime tier is independently useful (stage 1 stands alone), and the prover grows under a fixed meaning rather than the meaning growing under a fixed prover.

Illustration of where clauses land:

| Clause | v1 tier | Stage-3 tier |
|---|---|---|
| `requires 1 <= 2;` (degenerate) | proven (folds) | proven |
| `out p: T? when true` (≡ `static ensures not result or p <> null;`) | proven (spec-§7 flow) | proven |
| `requires n > 0;` | checked (proven per call site where the argument folds) | checked; dischargeable per call site by linear arithmetic |
| `static requires n > 0;` | error except at folding call sites | admitted where callers' facts discharge it |
| `ensures _pending = old(_pending) - n;` | checked | dischargeable where the body stays in the linear fragment |
| `static ensures result >= 0;` on a body with branches | error (unprovable at floor) | admitted where the fragment discharges each path |
| `invariant _balance + _limit >= 0m;` | checked (stage 4) | fragment-dependent (`decimal` arithmetic is outside the linear-integer fragment as fixed below — recorded) |

### 7.3 The `static` qualifier and the stage-3 fragment

> The **`static` clause qualifier demands the proven tier**: a `static` clause the active prover cannot discharge at its §7.1 site is a compile error; a discharged clause compiles to no runtime check. The stage-3 prover fragment is fixed now so the qualifier's meaning never drifts: **linear integer arithmetic** over parameters, fields, `result`, and `old` captures (under spec §6.1's checked semantics — overflow is part of the theory, not ignored by it); **null tests** composed with spec §7's flow narrowing; and **case analysis over enums and closed hierarchies** — `sealed abstract` hierarchies per §19.6's exhaustiveness algebra, and `choice` payloads when proposal 0041 lands. Everything outside the fragment stays checked; `static` on an out-of-fragment clause stays an error rather than silently downgrading — a demanded proof that quietly became a runtime check would invert the qualifier's meaning (the same never-silently-degrade posture as Appendix D.1's rejection rule, which §13 applies to conformance).

### 7.4 Tiers are recorded

Stage 2 carries, for every public member, each clause with its tier in the artifact interface (§2.8.2) — the seam for proposal 0044 (drafted in parallel; not yet landed). Tier changes then classify mechanically at the module boundary: checked→proven is behavior-preserving; clause addition/strengthening/weakening classifies under §8's direction rules, which are the Liskov directions 0044 applies to diffs. The ratchet, stated as the workflow it enables: a codebase writes `requires`/`ensures` freely (checked tier, immediate runtime value); as the stage-3 prover lands, clauses inside the fragment are promoted by prefixing one word, whose only observable effect is cost — because purity made evaluation and elision indistinguishable (§4). "AI writes the proofs, humans audit the contract" (future-directions P3) lands exactly here: obligation discharge is machine labor; the clause a human reviews stays one line.

## 8. Inheritance, interfaces, and dispatch

Contracts are **slot-bound**: a clause declared on a virtual or abstract member, or on an interface member, is part of that slot's contract and binds every override and implementation. The rules, in the same never-drop / may-strengthen shape as 0042 §5 and the §12.9 direction discipline:

1. **Clauses are inherited verbatim, never restated.** An override or implementing member does not repeat the base or interface clauses; its invocations are checked against them wherever the member is reached — directly or by dispatch. (Restating invites drift; the declaration site is the single source. This also means adding a clause to a base or interface member is a *declaration-site* edit that binds all implementors — see §14 for the compatibility classification.)
2. **An override or implementation may add `ensures` clauses.** Added clauses are conjoined with the inherited ones at that member's return edges — strengthening the postcondition, always sound for callers (they were promised no less). A caller through the base static type cannot *name* the added guarantee, but every execution honors it; through the derived static type it is part of the visible contract (the same derived-surface-strengthening shape as 0042 §5 rule 2).
3. **An override or implementation may not declare `requires` clauses.** Strengthening a precondition breaks every caller that dispatches through the base contract — the Liskov direction — and v1 provides **no weakening syntax** either (Eiffel's `require else` is declined for now, §17): the override's effective precondition is the inherited one, verbatim. This is stricter than the P3 sketch's "may weaken"; §15 records the divergence and the reason (weakening is rare, needs new surface syntax, and the multi-root rule below already handles the one place weakening is forced).
4. **Multiple contract roots combine soundly.** A member reachable through several contract-bearing declarations — a class member implementing two interfaces that both declare clauses for it (§11.2's single-implementation rule), or an override whose base chain and interface both carry contracts — has: effective `requires` = the **disjunction over roots** of each root's clause conjunction (a caller reached the member through *some* static type and established *that* root's precondition; demanding all roots would reject legal callers — the Eiffel inherited-precondition rule, arrived at for the same reason); effective `ensures` = the **conjunction of all roots'** clauses plus the member's own additions. Evaluation order: roots in inheritance-declaration order, clauses in declaration order within a root; `requires` evaluation short-circuits at the first satisfied root; if none is satisfied the violation names every root's failed clause set. A diamond (§11.2's `Count` case) contributes one root — both paths substitute to the same declaration.
5. **Monotone down the chain.** An override inherits from its *nearest* overridden declaration transitively (§12.1's matching direction), so ensures additions accumulate and nothing is ever dropped mid-chain.
6. **Clause context is the declaring type.** An inherited clause evaluates as declaring-type code: it may read members visible at its declaration (including the base's `private` fields — it was checked there), and `result` in an inherited `ensures` binds to the actual invocation's returned value, which is type-identical by §12.1's exact-return-match rule (no covariance in this version — an enabler this design leans on and records).
7. **Abstract members carry clauses** (§12.5, §3.2's grammar) — this is the *intended* home: the slot's contract written where the slot is declared, every concrete subclass bound. `abstract` + clauses composes with §12.6's async-abstract rule unchanged (a `Task`-returning abstract member's clauses bind its `async` overrides under §10.1's rules).

Both directions in one example — inherited requires joined by disjunction, ensures added on the implementation:

```bezel
public abstract class Repo
  public abstract method Load(id: int): owned Doc
    requires id > 0;
end Repo;

public interface IAudited
  method Load(id: int): owned Doc
    requires id >= 100;                 // audited stores reserve the low ids
end IAudited;

public class AuditedRepo: Repo, IAudited
  public override method Load(id: int): owned Doc
    // effective requires: (id > 0) or (id >= 100)   — root disjunction, rule 4
    // may not declare requires clauses here          — rule 3
    ensures result.Id = id;             // added ensures — rule 2 (Doc.Id: an
                                        //   auto-property, implicitly pure, 0042 §4.7)
    // ... body ...
  end Load;
end AuditedRepo;
```

A call `repo.Load(5)` through a `Repo`-typed reference satisfies root 1 and is admitted; the same call through `IAudited` *also* runs (the member is one body — the disjunction is the only reading under which both static types' legal callers pass), which is exactly the behavioral-subtyping account: the implementation honors the weakest precondition any of its faces advertises.

**Overloading and signature identity.** Clauses never distinguish overloads and are not part of §22.6 resolution — like `owned` and `scoped`, they constrain the contract, not selection; two members differing only in clauses are the same signature and clash (§11.3). Clause presence is, however, part of the member's *published contract* for §2.8.2/stage-2 purposes, exactly like the `when` clause it subsumes.

## 9. Class invariants

The hardest sub-design, severable by construction (stage 4). All rules below are decided, not sketched.

### 9.1 Declaration and meaning

> An **invariant member** (`invariant E;`, §3.3) declares a predicate over the instance's state, checked under §4's clause rules in the context of the declaring class (its `private` fields readable; `old` and `result` rejected). The **class invariant** of C is the conjunction of every invariant member of C and of every base class, evaluated base-first, declaration order within each class. The invariant is an *observable-boundary* property: it must hold whenever the instance is observable from outside itself, and it may be freely false while the instance is operating on itself.

### 9.2 The instrumentation set (where the invariant is checked)

> The invariant is evaluated at exactly these points, in checked-contracts mode (§6.3):
>
> - **Construction exit.** The normal completion of the **most-derived constructor** of a construction — the constructor `new` invoked, of any visibility, after its body and its `ensures` clauses. A base constructor completing as part of a derived construction is **not** an instrumentation point (the §22.8 sequence is interior — derived initializers have not run, and a base invariant evaluated mid-construction could dispatch a pure virtual member into derived code over the §22.7 null watermark); likewise a `this(...)`-chained-to constructor completing mid-chain. Every fresh instance therefore satisfies its full dynamic class invariant before it is observable, whatever the visibility of the constructor that built it — construction is birth, not a call on an existing object, so the public-boundary logic below does not apply to it. A violation raises `ContractViolationException` and the partially-constructed instance unwinds under §22.9's ordinary machinery — the instance is never observable.
> - **Entry and normal exit** of every `public` instance **method**. Order at entry: `requires` clauses, then the invariant (a caller's argument error is the more actionable diagnosis when both would fire); at exit: `ensures` clauses, then the invariant (the local promise before the global consistency).
> - **Entry and normal exit** of every `public` **setter** — block-form, auto-property, indexer, `set(owned)` alike. A public setter is a public mutation; exempting it would let one assignment break the invariant undetected until the next method call. (Auto-property assignments *inside constructors* are backing-field stores, not accessor invocations — §22.7's observation-gate wording — so construction does not double-check.)
> - **Getters are exempt** in v1, recorded with the residual: a mutating block-form getter escapes invariant checking. The recommended getter is a 0042 `pure` one, which cannot mutate; instrumenting the commonest access path to police the rarest abuse fails the cost test.
> - **Never instrumented:** non-public methods and setters, static members, destructors, and `Dispose` — disposal legitimately tears the state down (§28.4), and post-disposal access is already §28.2's checked-mode territory. Throw exits check nothing (the §5.2 boundary; the object may legitimately be mid-mutation when an exception unwinds it — the exception, not the invariant machinery, is the report).

### 9.3 The visibility boundary, the `this`-reentrancy exemption, and dispatch

> **Public-only** is the boundary (the Eiffel lesson, adopted): non-public calls are the implementation's interior, where an algorithm may pass through broken intermediate states; checking `private`/`protected`/`internal` calls forces artificial re-establishment mid-algorithm and teaches authors to launder interior steps through worse structures. `internal` is interior by this rule — module-mates are co-implementors, not clients.
>
> **`this`-reentrancy exemption.** A call whose receiver is `this` — explicit or implicit — from inside any instance member of the class or a subclass enters the member's **unchecked face**: no invariant evaluation at that call's entry or exit. Mechanism: every instrumented public member compiles to a checked face (entry checks → unchecked face → exit checks) and an unchecked face (the body); call sites with syntactic receiver `this` bind to the unchecked face — statically decidable, including for virtual dispatch (a `this`-receiver dispatching call reaches the dynamic target's *unchecked* face). Calls on *other* instances of the same class check normally (`other.M()` is an observation from outside `other`). Invariant evaluation itself runs as interior code — its own `this`-calls take unchecked faces — so a clause may call a `pure` public method of the same instance without recursion (§5.5).
>
> **Dispatch checks the dynamic class's invariant.** An external call through a base-typed reference enters the dynamic target's checked face, which evaluates the **full dynamic class invariant** — base and derived conjunction (§9.1) — not merely the static type's part: the object under observation is the derived object, and its consistency is what the boundary asserts.

### 9.4 Construction, destruction, and the checked-mode composition

The construction-exit point composes with §22.7 exactly: at the most-derived constructor's normal completion every §7.9-required field of every class in the chain is definitely assigned, so evaluating the invariant (an observation of `this`) is admitted by the observation gate by construction, and no check runs earlier — during construction the invariant may be freely false. The recorded residual is inherited, not enlarged: a during-construction `this`-escape that re-enters a public method *from outside the instance* (§22.7's residual list) hits a checked face and may evaluate a derived class's invariant over fields still at the null watermark — which faults as `NullReferenceException` in checked disposal mode per §22.7's stated bound. Invariants add no new hazard class; they add observers inside an already-recorded residual.

Concurrency composes the same way: invariant evaluation is a boundary-time *read* of instance state in the calling domain, so it introduces no new cross-domain access — an instance legitimately reachable from several domains (through `Shared<T>` of a §28.13 synchronized type, or under §22.12 rule 4's confine-or-synchronize obligation) is checked under exactly the discipline that admitted the boundary call itself, and a `Locked<T>` interior (stdlib §6.11) is checked only inside its guard's critical section, where its methods run. No rule of §30.13/§30.14 changes.

### 9.5 Worked example

```bezel
public class Ledger
  private owned _entries: OwningArray<Entry>;
  private _total: decimal := 0m;

  invariant _total >= 0m;

  public constructor()
    _entries := new OwningArray<Entry>();
  end constructor;                       // most-derived construction exit: invariant checked

  public method Post(amount: decimal)
    requires amount > 0m;                // entry: requires, then invariant
    _total := _total + amount;           // interior: invariant may be momentarily false
    _entries.Add(new Entry(amount));     // fresh producer into owned param — no marker (§22.3)
    Audit();                             // this-call: unchecked face, no re-check
  end Post;                              // exit: (no ensures), then invariant

  public method Audit()
    // ...
  end Audit;
end Ledger;
```

### 9.6 Out of scope for v1 invariants, with reasons

- **Records**: a record invariant collapses to constructor-exit checking plus a rule for `with`-update (spec §9.5's synthesized copy would need an instrumentation point) — a coherent later design, not attempted here.
- **Interface-declared invariants**: would instrument implementors across module boundaries; deferred until stage-2 carriage exists to transport them.
- **Static classes / static-state invariants**: an `invariant` in a static class is a compile error (no instance boundary exists to check at); a per-type static invariant checked at §22.12/§22.13's boundaries is recorded as a possible extension, not designed.
- **`static invariant`** (the proven-tier demand) parses from day one but is useful only at stage 3, like the clause qualifier.

## 10. Scope decisions recorded

### 10.1 Async methods: `requires` in v1; ensures-at-settlement staged, shape recorded

**`requires` on an `async` method is in scope for v1** and evaluates in the **synchronous prefix**: after argument binding, before any body statement and therefore before the first suspension (§30.1 — execution is synchronous until the first `await`). A violation therefore surfaces **synchronously at the call expression**, not through the task: the caller that passed the bad argument gets the exception on its own stack, the diagnosis §30.9's faulted-task channel would blur. This composes with §12.6's pattern unchanged: a clause on a `Task`-returning abstract member binds `async` overrides, checked in each override's prefix.

```bezel
public abstract class DataSource
  public abstract method LoadAsync(id: int): Task<string>
    requires id > 0;
end DataSource;

public class FileSource: DataSource
  public async override method LoadAsync(id: int): Task<string>
    // inherits `requires id > 0` (§8 rule 1) — evaluated in this body's
    // synchronous prefix: a violating caller faults at the call expression,
    // on its own stack, before any suspension and before a task exists to fault.
    return await File.ReadAllTextAsync(PathFor(id));
  end LoadAsync;

  private method PathFor(id: int): string => $"/data/{id}.txt";
end FileSource;
```

**`ensures` on an `async` method is rejected in v1**, with the settlement design recorded so graduation can adopt it without redesign: clauses would evaluate at **successful settlement** — in the completing frame, after the body's result is bound, before the result is delivered to any awaiter and before the frame's terminal-state disposals (§30.1's owned-parameter teardown) — reading `result` as a borrow of the settling value (Pattern A's task-owned result or Pattern B's in-flight owned value, §30.5); a violation would fault the task (§30.9's channel — the awaiters' exception, since no synchronous caller exists at settlement); faulted and cancelled settlements check nothing (the §5.2 boundary transposed); `old` captures would evaluate in the synchronous prefix (after `requires`) and live in the state-machine frame across suspensions — value captures ride free, but a **class-typed `old` capture is a frame-held borrow of a possibly caller-side source**, exactly §30.1's held-until-settlement category, inheriting its settlement obligation and its residual. That last interaction — contract captures entangled with §30.1's most delicate machinery — is *why* this is staged rather than shipped: it deserves its own verification pass, not a rider.

### 10.2 Iterators: deferred whole

Clauses on iterator methods (§29.3) are rejected in v1. Calling an iterator executes none of its body (§29.4), so `ensures` has no coherent point (per-yield? at exhaustion? both are new semantics), and even `requires` needs a decision — checked eagerly at the call, it would replace the hand-written eager-validation-wrapper idiom, which is genuinely attractive and is recorded as the natural first slice when iterators are revisited; checked lazily at first `MoveNext`, it would fire far from the fault. Deferred as a package, the 0042 §4.1 posture.

### 10.3 Accessors, operators, lambdas: no clauses in v1

- **Properties and indexers**: no `requires`/`ensures` on accessors in v1. The design surface is real — §23's four forms × per-accessor clauses × inheritance through §11.2's readonly-satisfaction rule — and the near-term need is covered from both sides: invariants instrument public setters (§9.2), and 0042's `pure` getters state the read side's strongest useful fact. Recorded v2, to be designed with accessor purity's leftovers.
- **Operators**: rejected in v1 (recorded liftable). An operator contract is expressible today by refactoring to a named method; operator declarations are `public static` only and would inherit the method rules mechanically when lifted.
- **Lambdas**: no clause surface exists or is planned at the literal — a lambda's contract belongs to its *function type*, and clause-bearing function types are the same v2 bucket as 0042 §11's function-type purity (both are contracts in abstracted positions; both double the type vocabulary; both wait for first-order value to be proven).

### 10.4 `extern`, destructors, static constructors

- **`extern` declarations** (§32): no clauses in v1. Checkable thunks are designable (evaluate `requires`, call the symbol, evaluate `ensures`), but the clause expressions would range over FFI-typed values and the enforcement story is trust-tiered by nature — this is the **assumed tier's intended client** (§7), to be designed jointly with 0042's Q8 extern-purity question, where the same trust decision recurs.
- **Destructors**: no clauses (disposal is the context contracts fence off; no call site consumes the fact — 0042 §4.1 verbatim).
- **Static constructors**: no clauses (§22.13's machinery is the only caller; a static initialization contract is the static-invariant question of §9.6, not a clause on the initializer).

### 10.5 In scope, stated positively

Instance methods, static methods, extension methods (MethodDecl-reused; the receiver is a borrow, §26.7, readable in clauses), constructors, abstract methods, interface methods and static interface methods (§11.8's abstract-only members carry clauses like any abstract member), and generic members — a generic member's clauses type-check once at the declaration under its constraints (0042 §5's generics posture; constraint-typed receivers offer no pure members in v1, so clauses over bare-`T` values are near-inexpressible — recorded honestly, lifted by v2 constraint purity). The generic boundary, shown:

```bezel
public static method Pick<T>(xs: Array<T>, i: int): T
  requires i >= 0 and i < xs.Count;     // OK — the clause ranges over concretely-typed
  return xs[i];                         //   state; Array<T>.Count is a 0042 §13 enabler
end Pick;

public static method Best<T>(a: T, b: T): T where T: IComparable<T>
  // requires a.CompareTo(b) <> 0;      // rejected in v1: constraint interface members
  // ...                                //   are not pure-declared (0042 §5, §13) — the
  return a;                             //   recorded bare-T clause poverty
end Best;
```

**Record and enum methods** are in scope too — `RecordBody` and `EnumMember` reuse `MethodDecl`, the receiver is a value, and every §5 rule applies unchanged (`old` over receiver fields captures by the value rules of §5.3); only the *invariant* member is class-only (§9.6) — method clauses and type invariants are separate axes, deliberately.

`Main` (§33) is a method like any other; an uncaught violation terminates per §33.3. Non-`async` methods returning `Task`-family types take ordinary §5 clauses — `ensures` there ranges over the *task object* (a coherent but rarely useful contract; the useful one is §10.1's settlement form, recorded).

## 11. Worked consumption: what callers get in v1

Stage 1 already changes the caller's experience in three concrete ways, none requiring the prover:

1. **Diagnosis at the boundary.** A bad argument faults at the callee's entry with the clause text in the message — before any partial effect, before the chain runs on a constructor — instead of deep inside the body at whatever operation first trips.
2. **Contracts follow dispatch.** A `requires` on an abstract or interface member holds for every implementation forever (§8 rule 1); today's equivalent — each implementor remembering to copy the validation prologue — silently decays.
3. **The contract is readable at the declaration** — and, from stage 2, machine-readable in the artifact interface, which is what 0044's behavioral diff and the regeneration loop consume.

## 12. The §22.4 retro-unification

> §22.4's nullability postcondition is redefined — surface-preservingly — as a proven-tier contract clause. Normatively, for a `bool`-returning method:
>
> `method M(…, out p: T? when true, …): bool` **is** `method M(…, out p: T?, …): bool` carrying the clause `static ensures not result or p <> null;`
>
> and `when false` correspondingly carries `static ensures result or p <> null;`.
>
> Everything §22.4 mandates falls out of this proposal's general rules: the declaring-body obligation (every path returning the stated constant leaves `p` provably non-null) is the spec-§7 flow prover discharging a `static ensures` at its §7.1 site — a violating path is a compile error because an undischargeable demanded proof is one; the evaluation point and normal-completion boundary are §5.2's; the call-site narrowing (in regions where the call's result is statically known to equal the stated constant, the recipient is treated non-null) is caller-side **consumption of a proven ensures** — the first shipped instance of the consumption pattern stage 3 generalizes; and the clause's §2.8.2 interface presence is stage 2's carriage.

Worked, on the stdlib's canonical shape-(a) member (stdlib §4.8, clause normative per stdlib §1):

```bezel
// Surface spelling (unchanged everywhere):
public method TryGetValue(key: K, out value: V? when true): bool;

// Normative identity under this proposal:
public method TryGetValue(key: K, out value: V?): bool
  static ensures not result or value <> null;

// Caller — narrowing as today, now explained as proven-ensures consumption:
if users.TryGetValue(name, out var u) then
  Console.WriteLine(u.Email);       // u non-null here: the region knows the call
end;                                //   returned true, so the proven clause yields
                                    //   u <> null — no `!` needed
```

**Compatibility: total.** The `when true`/`when false` spelling **remains the surface form** — no source changes anywhere, and stdlib §1's Try-convention elision note is untouched (every shape-(a) member's clause remains normative). Writing the desugared `static ensures` form directly is equally legal and is the same declaration. One existing rule is deliberately retained rather than generalized: §12.1 requires an override to repeat the `when` clause **exactly** — neither add nor drop — while §8 rule 2 would permit *adding* a postcondition. Retained because it is the zero-migration reading; the relaxation (an override may add a `when` clause, consumable through the derived static type) is recorded as a graduation-time option (Q5). Dropping stays an error under both readings — a postcondition is never weakened.

This section is the proposal's proof-by-instance: the language already shipped one contract clause with a special-purpose prover and caller-side consumption, and it worked. §22.16 is that mechanism with the special-purposeness removed.

## 13. Conformance

**Core:** clause grammar and typing, the purity admission, evaluation semantics, `ContractViolationException`, always-on `requires`, checked-contracts mode, inheritance/interface binding, the constant-floor prover (§17.14 is Core machinery), the `when`-clause prover (shipped, Core today), and — at stage 4 — invariants. Runtime checking needs nothing beyond expression evaluation and a §31.4 raise: well inside Core's "complete and sound inner language."

**Extended:** a new Appendix D.2 row, **Contract proving** — the stage-3 prover fragment, which is what gives the `static` qualifier reach beyond the floor. A Core implementation **rejects** a `static`-qualified clause it cannot discharge, with a diagnostic naming the feature (Appendix D.1's clean-rejection posture); it never downgrades a demanded proof to a runtime check, which would silently invert the qualifier's meaning (§7.3). Defense of the split: an SMT-backed engine is exactly the kind of outer feature Appendix D exists to stage — heavy, separable, and absent from the inner language's soundness story — while keeping the floor Core means every conforming implementation agrees on what constant clauses and `when` clauses mean, so the tier vocabulary never forks.

**Appendix C.15 (new), the checklist form:** (1) a contract clause's expression is `bool`, satisfies the §22.15 pure-body rules, and contains no `throw`, no `await`, no store, and no owned-consuming or disposal-running operation; (2) `requires` evaluates after argument binding and before the chain/initializers/body, in declaration order, and is never elided unless proven; (3) `ensures` evaluates on every normal-completion path after result and `out`-slot binding and before scope-end disposals, and never on a throw path; (4) `result` appears only in `ensures` of a member with a declared return type and is a borrow position; (5) each `old` operand evaluates exactly once at entry after `requires`, by copy for value/record/enum/`string` and as a frame-resident borrow for class/interface types, under the ordinary §28.3 binding rules; (6) an override or implementation declares no `requires` and may add `ensures`; inherited clauses are never restated; multi-root `requires` combines by disjunction, `ensures` by conjunction; (7) a `static`-qualified clause the active prover cannot discharge at its discharge site (declaring body for `ensures`/`invariant`; each call site for `requires`) is a compile error; a `static requires` mentions only caller-visible state; a constant clause folding `false` is a compile error; (8) the class invariant is evaluated at most-derived-construction exit, public-method entry/exit, and public-setter entry/exit, never on non-public members, destructors, `Dispose`, throw paths, or `this`-receiver calls; (9) clauses never distinguish overloads; (10) `invariant` members appear only in classes; `old`/`result` appear only where rules 4/5 admit them.

## 14. Migration and compatibility

- **Additive.** No v0.197 program changes meaning or parses differently: every new reserved context is a position that does not exist today (§3.4's audit), and all five words are contextual.
- **Adding clauses to an existing non-virtual, non-interface member** is compile-compatible for all callers; at runtime it is behavior-*revealing*, not behavior-changing — calls that honored the contract are untouched (clauses are pure), calls that violated it now fault deterministically instead of proceeding wrongly. It is nevertheless a contract event: the §3.6-rule-3-style documentation discipline applies, and the one observable migration cost — a hand-validation prologue replaced by `requires` changes the caught exception type — is recorded at §6.4 and borne per-member, deliberately.
- **Adding a clause to a virtual or interface member** binds every override/implementation (§8 rule 1) with no source change at those sites — new `ensures` obligates existing bodies (their next checked run may reveal violations: intended), new `requires` narrows every caller: a **breaking change** in the same class as strengthening any contract, to be flagged like one. Stage 2's diff classification makes exactly this mechanical: requires-added = breaking; ensures-added = strengthening; tier-raised = behavior-preserving.
- **Adding an `invariant` to an existing class** (stage 4) is the same event class as adding `ensures` to every public member at once — behavior-revealing under checked-contracts mode, silent in release. Flag accordingly.
- **The stdlib ships no clause wave in v1** (deliberate divergence from 0042's §13 shape): converting documented `ArgumentException` contracts to `requires` changes observable exception types (§6.4 reason 4) and is deferred to an owner decision (Q7). The already-normative `when` clauses of every shape-(a) Try member become `static ensures` by definition (§12) with zero observable change — that *is* the v1 stdlib contract surface. One mechanical preparation is recommended now: renaming the shape-(c) family's `out result:` parameters (call-site-invisible, §3.4) so future authored `ensures` on them is unobstructed.
- **Removing a clause** is weakening — breaking for consumers of the fact (stage 2 makes them visible); the same discipline as removing `owned` from a return.

The change taxonomy in one table — the input 0044's behavioral diff consumes at stage 2:

| Contract change | Classification |
|---|---|
| Add a clause to a non-virtual, non-interface member | Behavior-revealing; a documented contract event, not a break |
| Add `ensures` to a virtual/interface member | Strengthening — binds every implementor's next checked run |
| Add `requires` to a virtual/interface member | **Breaking** — narrows every caller through every face |
| Add an `invariant` to a class | Behavior-revealing across the class's whole public surface |
| Remove or weaken any clause | **Breaking** for consumers of the fact |
| Checked → proven (add `static`, prover discharges) | Behavior-preserving; the ratchet direction |
| Proven → checked (drop `static`) | Guarantee demotion — **breaking** where callers consumed the proof (the §22.4 narrowing is exactly such a consumer); flagged, never silent |

## 15. Interactions (complete enumeration) and divergences from the P3 sketch

| Spec surface | Interaction |
|---|---|
| §4.1 / §17.3 / rationale §17.1 | clauses are `bool`; `=` is equality, so clauses read as mathematics (the Pascal-heritage dividend the P3 sketch noted) — no truthiness |
| spec §7 nullability | narrowing works inside clause expressions (`and`-chains, `is` tests) per spec §7.4/§12.7; the §22.4 prover is spec §7's flow analysis (§12); `out` recipients' caller-side narrowing is proven-ensures consumption |
| spec §9 records | `old` copies records; borrow-bearing operands under §9.7's rules (§5.3); record invariants deferred (§9.6) |
| §11.2 / §11.8 | interface members carry clauses (grammar §3.2); implementations inherit, never restate; multi-interface joins per §8 rule 4; static interface members carry clauses like any abstract member |
| §11.3 / §22.6 | clauses never distinguish overloads and cannot create clashes; same-signature members clash as today |
| §12.1 / §12.5 / §12.9 | overrides inherit clauses; may add `ensures`, may not add `requires` (§8); exact-return-match is the enabler for inherited `result` (§8 rule 6); abstract members are the intended clause home; the direction discipline is §12.9's applied to contracts; the `when`-clause exact-match rule retained (§12, Q5) |
| §12.6 / §30 | async: `requires` in the synchronous prefix (v1); ensures-at-settlement recorded and staged (§10.1); clause `await` rejected (C.6); non-`async` `Task`-returning methods take ordinary §5 clauses over the task reference (coherent, rarely useful — §10.5) |
| spec §13 visibility | invariants: public boundary only (§9.3); clause context is the declaring type, so inherited clauses may read base-private state (§8 rule 6); `static requires` restricted to caller-visible state (§7.1); accessor-visibility interactions deferred with accessor clauses (§10.3) |
| spec §14 generics | clauses type-check once at declaration under constraints; no per-instantiation re-evaluation; bare-`T` clause poverty in v1 recorded (§10.5) |
| §17.12 / §17.13 | `throw` rejected in clauses (§4); discard and marked transfers are statements and cannot occur; clause faults propagate as themselves (§5.4) |
| §17.14 | the constant floor of the proven tier; fold-to-`false` is a compile error (§7.2) |
| spec §19 / §20 | `case`-expressions and `if`-expressions admitted in clauses under the pure rules; §19.6's exhaustiveness algebra is the stage-3 case-analysis fragment's foundation |
| §22.3 / §22.4 / §22.5 | `out` slots readable in `ensures` (copy-out model), unreadable in `requires` (§5.1/§5.2); §22.4 subsumed (§12); defaults evaluate before `requires` per §22.5's order; `ref` parameters readable as reads |
| §22.7 / §22.8 / §22.9 | constructor clauses: `requires` before the chain, no `this` observation; `ensures` at exit where the observation gate holds; invariant at most-derived-construction exit composes with §22.9's unwind and §22.7's recorded residual (§9.2, §9.4) |
| §22.12 / §22.13 | statics readable in clauses (reads); no clauses on static constructors; static-state invariants deferred (§9.6/§10.4) |
| §23 | no accessor clauses v1 (§10.3); invariants instrument public setters including `set(owned)` (§9.2); getter exemption recorded |
| §25 / §26 / §27 | operators: no clauses v1 (liftable); extension methods: in scope, receiver a borrow (§26.7); lambdas/function types: the v2 bucket shared with 0042 §11 |
| §28.2 | checked-contracts mode is a *sibling* of checked disposal mode — same two-mode shape, different relaxation (non-evaluation vs UB, §6.3); the modes are independent; checked disposal backstops `old`-capture residuals (§5.3) |
| §28.3 / §28.8.1 / §28.9 | the borrow-only guarantee is assembled from existing classifications (§4); `result` is a borrow position (§5.2); `old` desugars to hidden entry bindings under the ordinary rules — freeze, moved-from, announced-kill backstop all compose (§5.3); moved-out owned parameters unreadable in `ensures` on the moving path |
| §28.13 / §30.13–§30.14 | invariant evaluation is a boundary-time read in the calling domain — no new cross-domain access; checked under whatever discipline admitted the call (§9.4); domain serialism and sendability rules unchanged |
| §28.14 | scoped parameters readable in clauses as reads; no capture or storage can occur (no stores) |
| §29 | iterators: clauses rejected v1, requires-at-call recorded as the revisit slice (§10.2) |
| §31 | `ContractViolationException` joins §31.4's built-ins (§6.1); throw paths skip `ensures` (§5.2); clause faults propagate unconverted (§5.4); catch arms and supersession unchanged |
| §32 | extern: no clauses v1; the assumed tier's intended client, joint design with 0042 Q8 (§10.4) |
| §2.8.2 | stage 2 carries (clause, tier) per public member — the seam for proposal 0044 (drafted in parallel; not yet landed); tier/direction diff classification (§7.4, §14) |
| §33 | no entry-point special-casing (§10.5) |
| Appendix A/B/C/D | A.4 productions (§3); B.1/B.2 rows and the collision audit (§3.4); new C.15 (§13); new D.2 row (§13) |
| Proposals | **0042**: the dependency (§4) — one purity definition; **0031**: `Verify.That` stays as the body-internal always-on check, contracts are its boundary complement (§17); **0022**: `Assert.Throws`-shaped tests can target `ContractViolationException`; contracts give property tests (P13) their oracles; **0041**: `choice` case analysis in the stage-3 fragment, `ensures` over choice returns per-case; **0044** (drafted in parallel; not yet landed): carriage and diffing (stage 2); **0040**: the build driver hosts the mode switches |

**Divergences from the P3 sketch, recorded:**

1. **The violation exception.** The sketch leaned `ArgumentException`-family for `requires` (with the dedicated type an open question); this proposal recommends a single built-in `ContractViolationException` for all three clause kinds — at the mandate's own direction, with the compat cost named and the alternative presented (§6.4).
2. **Override weakening.** The sketch's "may weaken `requires`" is narrowed: v1 overrides may not declare `requires` at all — no weakening syntax ships; the multi-root disjunction rule (§8 rule 4) covers the place weakening is structurally forced, and `require else`-style refinement is recorded as a declined-for-now alternative (§17).
3. **Clause punctuation.** The sketch's `static ensures result >= 0` (no semicolon) is corrected: every clause ends with `;` (rationale §3.1).
4. **The assumed tier is unpopulated in v1.** The sketch listed FFI boundaries as assumed-tier inhabitants; v1 rejects extern clauses entirely and reserves the tier for their staged arrival (§7, §10.4).
5. **No stdlib clause conversions in v1** — a consequence of divergence 1 (§14); the sketch implied the `ArgumentException` idiom would be *replaced* by `requires` broadly.
6. **Invariant instrumentation includes public setters and every most-derived construction** — the sketch said only "public-method entry and exit and constructor exit"; §9.2 adds setters (a public mutation is a public boundary), refines the constructor point to most-derived-completion of any visibility (birth is not a boundary call), and records the getter exemption.
7. **`requires` discharge is per-call-site** under `static` (§7.1) — the sketch's single sentence ("compile error if unprovable") did not fix the discharge site; this proposal does, per clause kind.

## 16. Staging

1. **Stage 1 — clauses, runtime tier (this proposal's §3–§8, §10, §12).** Grammar (`ContractClause` only), purity admission via 0042, evaluation semantics, `ContractViolationException`, always-on `requires` + checked-contracts mode, inheritance/interface binding, async `requires`, the §22.4 unification, the constant-floor prover, and the `static` qualifier at floor reach. Doc artifacts: the new §22.16, the §22.4/§11.2/§12.1/§12.5/§30.1/§31.4 amendments, A.4, the five B.1/B.2 rows, C.15, and stdlib §2.17's one row (§6.1). Ships with a regression suite keyed to each C.15 bullet (accept + reject + runtime-violation triples, spec-cited per `tests/README.md`; each runtime rule needs a fails-without/passes-with pair in both contract modes, and the §22.4 unification gets a no-behavior-change regression pinning the narrowing). **M**, standalone value — the P3 sizing confirmed.
2. **Stage 2 — carriage (with proposal 0044, drafted in parallel; not yet landed).** (clause, tier) per public member in the artifact interface (§2.8.2); the structured violation surface (§6.1); behavioral diff classification (§14); cross-module `static requires` discharge becomes possible (§7.1).
3. **Stage 3 — the prover.** The fixed SMT fragment (§7.3): linear integer arithmetic, null tests, enum/closed-hierarchy/`choice` case analysis; the `static` ratchet becomes real at both discharge sites; the Extended D.2 row activates. Iterator `requires`-at-call is the natural rider if iterators are revisited here (§10.2).
4. **Stage 4 — `invariant`.** §9 entire: `InvariantDecl` grammar, the instrumentation set, checked/unchecked faces, the construction composition. Severable by design; nothing in stages 1–3 depends on it.
5. **Recorded beyond the numbered stages:** async ensures-at-settlement (shape in §10.1); accessor clauses (§10.3); extern clauses + assumed-tier population, jointly with 0042 Q8 (§10.4); function-type clauses with 0042's v2 (§10.3); precondition-weakening syntax if demand materializes (§8 rule 3); record and static invariants (§9.6).

## 17. Alternatives considered and declined

- **Attribute-shaped contracts** (`[Requires("amount > 0")]` or typed equivalents). Declined: Bezel has no attribute system and the deferral is reasoned (rationale §18 — attributes wait for their consumers, and proliferation/stringly-typed-magic costs are recorded); worse, this is the C# Code Contracts architecture — conditions outside the language proper, a rewriter the compiler doesn't own — whose tooling rot is the mandate's named cautionary tale. §22.4 already chose grammar over attributes for precisely this kind of clause; contracts follow.
- **Library-only contracts** (a `Contract.Requires(cond)` static, or extending 0031's `Verify`). Declined: a body statement cannot appear in the signature or artifact interface, cannot bind overrides and implementors, cannot be consumed by callers (narrowing, proving) or diffed, cannot restrict itself to purity, and has no access to `result`/`old` or to the every-return-edge evaluation point — each a load-bearing feature above. **The relationship to 0031 is complementarity, not replacement**: `Verify.That` remains the body-*internal* always-on check ("this can't happen mid-algorithm"), and its `VerificationException` stays distinct — an interior invariant failure and a boundary contract violation are different diagnoses.
- **Full Dafny-style verification** (every clause a static proof obligation; unproven = rejected). Declined: it imposes a proof burden on code that didn't ask, gates adoption on prover maturity, and inverts this language's incremental discipline. The SPARK posture — every clause at least a runtime check, `static` the opt-in demand, the tier recorded — keeps unproven contracts *useful* and proof a ratchet, not a toll.
- **`ArgumentException`-family `requires` violations** — presented and declined at §6.4 with the four reasons and the recorded cost; per-kind exception triples and `ArgumentException`-derivation declined there too.
- **Elidable `requires` (a build knob).** Declined: 0031's always-on argument governs — the clause replaces an always-on idiom, and an elidable precondition is `Debug.Assert` again, the gap the feature exists to close. Hot paths ratchet to proven instead (§6.2). Per-module/per-clause mode knobs for `ensures` likewise declined — one per-build mode, like §28.2, or the modes themselves become configuration surface.
- **Eiffel's `require else` / `ensure then` refinement syntax.** Declined for v1: inherit-verbatim plus add-`ensures` covers the dominant cases with zero new syntax; the multi-root disjunction rule (§8 rule 4) supplies the soundness that refinement syntax exists to supply, without user-facing surface. Revisit on demonstrated demand (§16 item 5).
- **Assertion-evaluation suppression during clause evaluation** (Eiffel's flag). Declined at §5.5: a hidden mode changing call semantics; purity plus the unchecked-face rule close the real recursion sources.
- **Invariant checking at all visibilities.** Declined at §9.3 — the Eiffel lesson: interior algorithms legitimately traverse broken states; checking them forces artificial re-establishment and teaches evasion. Public-only matches the invariant's meaning (observable-state consistency).
- **Inferring contracts from bodies** (deriving `ensures` from the implementation). Declined without a section: an inferred postcondition is a description, not a promise — it changes when the body changes, which is the exact instability the artifact interface exists to prevent (0042 §2's declared-not-derived argument, applying a fortiori: a contract's entire job is to outlive the body).

## 18. Open questions / owner decisions

1. **Q1 — the violation exception** (§6): ratify a single built-in `ContractViolationException` (recommended) versus the `ArgumentException`-compatible alternative, accepting the recorded migration cost (a converted prologue changes the caught type).
2. **Q2 — always-on `requires`** (§6.2): ratify no-knob always-on (recommended), versus admitting a build-mode escape for preconditions.
3. **Q3 — the invariant boundary** (§9): ratify the instrumentation set — public methods entry/exit, most-derived-construction exit (any constructor visibility), public setters entry/exit, getters exempt — the public-only visibility rule, and the `this`-reentrancy exemption via checked/unchecked faces.
4. **Q4 — multi-root `requires` disjunction** (§8 rule 4): ratify (recommended), versus rejecting multi-root contract-bearing joins outright (simpler, but makes adding a clause to an interface a new break class for diamond implementors).
5. **Q5 — the `when`-clause override rule at unification** (§12): retain §12.1's exact-match (recommended for zero migration), or relax to the general add-`ensures` rule now.
6. **Q6 — class-typed `old` captures** (§5.3): admit with the shallow-capture warning and recorded residual (recommended), versus restricting v1 `old` to value/record/enum/`string` operands.
7. **Q7 — stdlib conversion policy** (§14): confirm v1 ships no stdlib clause conversions, and whether the `out result:` parameter renames ride the graduation revision as preparation.
8. **Q8 — the assumed tier's reservation** (§7): confirm the stage-2 format reserves the tier value now, with population deferred to the joint extern design (this proposal's answer to 0042's Q8: yes, designed jointly, at the extern stage).
9. **Q9 — grammar shape** (§3): ratify the clause-region marker-wins rule and the final-clause-closes-body-less-member rule (the alternatives — a required trailing `;`, or `end`-closed abstract members — are recorded in §3.2 and declined there).
10. **Q10 — extension methods in v1 scope** (§10.5): confirm (recommended; they reuse `MethodDecl` and the receiver-borrow story is settled by §26.7).
11. **Q11 — `static requires` discharge site** (§7.1): ratify per-call-site discharge (recommended; the SPARK reading, and the only one under which "zero runtime cost" is sound), versus reserving the qualifier to `ensures`/`invariant` until stage 3 decides.

## 19. Implementation sketch and diagnostics (non-normative)

For the eventual stage-1 lanes; no new runtime machinery beyond one exception-type registration.

- **Parser:** the clause region after `GenericConstraints*` (methods) / the parameter list (constructors) — a loop consuming `("static"? ("requires"|"ensures") Expression ";")`, ended by any other token; the body-less closure rule keys on the already-parsed `abstract`/interface context; `InvariantDecl` (stage 4) keys on member-leading `invariant` / `static invariant` two-token lookahead.
- **Sema:** clause checking is 0042's pure-body checker invoked expression-wise with a clause symbol context (parameters + `this` + statics; no body locals; `result`/`old` symbols injected per clause kind and member shape); `old`-operand collection per member; `ensures` reads validated against the existing per-path §28.3 ownership state at each return edge; inheritance attaches clause sets to slots where §12.1/§11.2 matching already runs, computing the root sets of §8 rule 4 at override/implementation binding.
- **Lowering:** `requires` → a prologue block after parameter binding, before the chain; `old` captures → hidden frame slots through the existing borrow-slot machinery (the §5.3 desugaring is the implementation); `ensures` → a single-exit epilogue over a result temporary (the temp *is* the `result` borrow), placed before the §28.3 unwind block; the violation raise is an ordinary §31 throw of a runtime-constructed instance whose message is a compile-time-formatted string (member, clause kind, clause source text, site).
- **Invariants (stage 4):** checked face = outlined wrapper `entry checks → unchecked face → exit checks`; `this`-receiver call sites bind the unchecked symbol (one bit in call lowering); virtual dispatch needs the unchecked face dispatchable too — a parallel slot per instrumented virtual member, present only in checked-contracts builds (release builds compile a single face and zero cost).
- **Diagnostics:** message shape `contract violation: requires 'amount > 0m' — Account.Withdraw (Account.bzl:12)`. An *inherited* clause's violation names both the declaring slot and the executing member (`requires 'id > 0' of Repo.Load — violated calling AuditedRepo.Load`); a multi-root `requires` failure (§8 rule 4) lists each root's failed clause set; an invariant violation names the declaring class of the failed invariant member (base invariants blame the base) and the boundary member that tripped it. Appendix D's diagnostic-identity latitude applies (no code registry; classification-only conformance).
- **Tests:** per `tests/README.md` — accept/reject pairs per C.15 bullet; runtime triples (violating caller faults / honoring caller unaffected / release-mode `ensures` unevaluated) as spec-cited gtests that fail without the feature and pass with it; a §22.4 regression pinning that unification changed no narrowing behavior.

## 20. Relationship to existing work

Future-directions P3 (the mandate; deepened here) · proposal 0042 (the purity substrate — the dependency, §4; its Q8 answered jointly at §10.4/Q8) · §22.4 (the shipped prototype, now an instance — §12) · proposal 0031 (the always-on precedent; complementary body-internal checks — §17) · §28.2 (the two-mode precedent — §6.3) · proposal 0044 (drafted in parallel; carriage and behavioral diff — stage 2) · proposal 0041 (`choice` case analysis in the stage-3 fragment; per-case `ensures` vocabulary) · proposal 0022 (test framework: violation-asserting tests; P13 property specs sit between clauses and proofs) · proposal 0040 (the driver that hosts mode switches and, stage 3, the prover invocation) · rationale §18 (the attributes deferral this design respects by being grammar).
