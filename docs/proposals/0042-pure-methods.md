# Proposal 0042 — `pure` methods

| | |
|---|---|
| **Status** | graduated-v0.202 — superseded — graduated v0.202 |
| **Tracking issue** | [#2029](https://github.com/s8lap/bezel-lang/issues/2029) |
| **Target** | Spec: new §22.15 (pure members), §11.5 (modifier order), §12.1/§11.2 (override/implementation matching), §23 (accessor forms), §26 (extension members), Appendix A.4, Appendix B.1/B.2, new Appendix C.14; stdlib: first-wave annotations in §2.2, §9.1–§9.6, §3.3, §4.4, §2.17/spec §31.4 exception constructors; rationale: one new section (declared-not-derived). No Appendix D change (Core). |
| **Written against** | spec v0.197 (stdlib/rationale at the same revision) |
| **Origin** | [Future directions 2026-07-18](papers/future-directions-2026-07-18.md) item P2 (Tier 1, the verification substrate). This proposal deepens P2 to graduation quality; divergences from the P2 sketch are recorded in §14. |
| **Design influences** | §17.14 constant expressions (the compile-time sibling: a closed evaluable sublanguage, defined once, referenced everywhere); `sendable`'s lesson *in reverse* (rationale §12.6: derived properties are observations, not obligations — a method-surface promise must be declared); D's `pure` (declared, compiler-checked, staged from strict to widened — and its cautionary tale of late widening churn); SPARK's "at least checkable, never imposed" posture. |

*Reference convention: an unprefixed §-reference denotes this proposal's own section when one of that number exists (e.g. §4.5, §13); otherwise it is a language-spec reference (e.g. §28.9, §31.1). The `spec §` / `stdlib §` / `rationale §` prefixes are written out wherever the unprefixed form would collide.*

## 1. Problem and evidence

Ownership is Bezel's resource axis. The language can state, in a signature, who disposes a value (`owned`, spec §28), what may cross a domain (`sendable`, §30.14), and what a callback may retain (`scoped`, §28.14). It cannot state the question every verification tool asks first: **does this call mutate anything?**

Current-state evidence that the gap is real and adjacent features are already paying for it:

- **Contracts need it first.** P3's `requires`/`ensures` clauses must not have side effects — evaluating a precondition twice, or eliding a proven one, must be unobservable. Without a purity discipline, clause admission has no principled boundary. This is why P2 precedes P3 in the future-directions ordering.
- **The parallel combinators check the wrong half.** `Parallel.Map`'s spawn-position bodies (spec §30.14 "Data-parallel combinators", stdlib §6.12) are checked for *capture-freedom and sendability* — who the body can reach — but nothing checks what it *does*: a capture-free static method that writes a static or performs I/O is admitted today, and the race discipline holds only because §22.12 separately restricts mutable statics. Effect-freedom is the natural admission the API wants to state.
- **`FrozenArray<T>` consumers** (stdlib §4.19) are safe to run concurrently precisely when their bodies are read-only — a fact currently held in documentation.
- **The compile-time sibling exists and stops short.** §17.14 defines a closed constant-expression sublanguage — an effect-free, evaluable fragment — but it is literal-rooted and excludes *all calls*. There is no way to write a named, reusable, effect-free function and have the compiler hold it to that.
- **Property-based testing and SMT** (P13, and the future-directions Tier-1 program generally) operate on the pure fragment: generated inputs, replayable runs, logic over heap-free semantics. Today that fragment is not demarcated.

The claim this proposal makes normative: *a call to a `pure` member reads program state but never writes it, performs no I/O, runs no user code at disposal time, and interacts with no concurrency machinery.* Purity is deliberately **not** determinism (§9) and **not** compile-time evaluation (§10).

## 2. The design at a glance

```bezel
public static pure method Mean(xs: Array<int>): double
  if xs.Count = 0 then
    throw new ArgumentException("xs must be non-empty", nameof(xs));
  end;
  var sum := 0;
  for var i in 0..<xs.Count do
    sum := sum + xs[i];
  end;
  return double(sum) / double(xs.Count);
end Mean;
```

`pure` is a member modifier: part of the member's signature, declared by the author, verified by the compiler against a closed rule set (§4), and enforced transitively through a declared call graph (§5). It is **declared, not derived** — deliberately the opposite mechanism from §3.6's derived type properties, and the choice needs defending because §3.6's closing paragraph pushes new classifications toward derivation:

- §3.6 governs *structural classifications of types* ("computed, never declared"). Purity is not a classification of a type's structure; it is a behavioral contract of a *member*, the same grammatical species as `owned` on a return or `scoped` on a parameter — all declared, all signature-carried, none derivable from structure.
- Rationale §12.6 records the governing principle from the sendable-inheritance decision: derived properties are *observations, not obligations* — a base author's coincidental property must not constrain others. Method-side purity inverts the risk: if purity were **inferred** from bodies, it would be an observation that callers silently *convert into* an obligation — adding one log line to a leaf method would flip the inferred purity of every transitive caller, breaking every pure-context use of them, with no declaration anywhere recording that a promise existed. Declared purity puts the promise, and therefore the breakage, at an explicit `pure` keyword whose removal is a visible signature change (§12, migration).
- The declared form is also what the artifact interface needs: §2.8.2 makes the module interface a semantic requirement, and P4 extends it with per-member behavioral facts. A declared modifier is already interface material; an inferred fact would make module interfaces change when bodies change — the exact instability §2.8.2 exists to prevent.

## 3. Grammar delta and modifier placement

**Modifier order (§11.5).** `pure` occupies the same slot as `async`; the two are mutually exclusive (§4.1):

```
visibility static (async | pure) (abstract | virtual | override | sealed override) owned readonly member-kind Name ...
```

**Appendix A.4 edits** (each `"pure"?` addition shown against the v0.197 production; everything else unchanged):

```
MethodDecl        ::= Visibility "static"? ("async" | "pure")? VirtualKind?
                      "method" Identifier GenericParams?
                      "(" ParamList? ")" (":" "owned"? Type)?
                      GenericConstraints* MethodBody

IfaceMethod       ::= "pure"? "method" Identifier GenericParams?
                      "(" ParamList? ")" (":" "owned"? Type)? ";"
IfaceStaticMethod ::= "static" "pure"? "method" Identifier GenericParams?
                      "(" ParamList? ")" (":" "owned"? Type)? ";"

ConstructorDecl   ::= Visibility "pure"? "constructor" "(" ParamList? ")"
                      CtorChain? Statement* "end" "constructor"? ";"
                      /* the static-constructor alternative does not admit "pure" */

PropertyDecl      ::= Visibility "static"? "pure"? VirtualKind?
                      "readonly"? "property" Identifier ":" Type PropertyBody
IndexerDecl       ::= Visibility "static"? "pure"? VirtualKind?
                      "readonly"? "property" "[" ParamList "]" ":" Type PropertyBody
IfaceProperty     ::= "pure"? "readonly"? "property" Identifier ":" Type
                      ( ";" | Accessor* "end" Identifier? ";" )
IfaceIndexer      ::= "pure"? "readonly"? "property" "[" ParamList "]" ":" Type
                      ( ";" | Accessor* "end" ";" )

Accessor          ::= Visibility? "pure"? "get" AccessorBody
                    | Visibility? "set" SetterParamSpec? AccessorBody
                      /* "pure" is admitted on "get" only; a pure "set" is
                         rejected semantically (§6) */

OperatorDecl      ::= "public" "static" "pure"? "method" "operator" OpToken
                      "(" ParamList ")" ":" "owned"? Type OperatorBody
```

`DestructorDecl` and `FieldDecl` are unchanged: destructors and fields do not carry `pure` (§4.1). Extension members reuse `MethodDecl`/`ExpressionPropertyDecl` and inherit the slot.

**Keyword status (Appendix B).** `pure` becomes a **contextual keyword**: reserved only in the modifier slots above, an ordinary identifier everywhere else. It joins the B.1 contextual list and gains a B.2 table row. Verified against v0.197: `pure` appears as an identifier nowhere in the stdlib or spec surface (prose uses only), so no declaration anywhere changes meaning.

**No function-type grammar change in v1.** The A.5 `FunctionType` production is untouched; the deferred `(int) pure -> int` form is sketched in §11 and deliberately not adopted.

## 4. The v1 rule set

The following is proposed normative text quality for the new spec §22.15. "Pure body" means the body of any member declared `pure` (method, constructor, operator, accessor) and every expression the section names as checked under these rules (field initializers of pure-constructed classes, §4.6; default-value expressions of pure members' parameters, §7).

Proposed opening text for §22.15, the contract in one paragraph:

> A member declared **`pure`** promises that evaluating a call to it — including every member it transitively calls — writes no storage outside the call's own frames, performs no I/O, runs no user code through disposal, and creates or touches no task, domain, channel, event, or synchronization object. A pure call may read any reachable state, may allocate runtime-managed values ([§28.9](#sec-runtime-managed-types)) and values of value, record, and enum type, and may fault by `throw` under the operand rule below. Purity is a declared, compiler-verified part of the member's signature — carried by the artifact interface ([§2.8.2](#sec-artifact-interface)) — not a derived type property ([§3.6](#sec-derived-type-properties)) and not a determinism or termination guarantee. The compiler rejects a `pure` member whose body violates the rules of this section, and rejects, inside a pure body, any call whose statically resolved declaration is not pure.

### 4.1 Where `pure` may appear

`pure` is admitted on: instance and static methods; extension methods and expression-bodied extension properties; operator declarations; constructors (§4.6); properties, indexers, and `get` accessors (§6); interface methods, static interface methods, interface properties and indexers; abstract and virtual members and overrides (§5).

`pure` is rejected, each with a diagnostic naming this section, on:

- **`async` members** (and async lambdas): an `async` body's execution interleaves with the domain's continuation queue and traffics in tasks — the §4.4 prohibited family. `pure` implies non-`async`; `await` therefore cannot appear in a pure body (C.6's `await`-placement rule needs no amendment).
- **Iterator methods** (`yield`): calling an iterator method executes none of its body (§29.4) — the body runs during the consumer's `MoveNext` calls, so "the call is effect-free" would be vacuously true and misleading; and the produced enumerator is a class instance whose disposal (§29.7) runs cleanup at consumer-determined times. Deliberately *open for v2* as "pure iterator = body pure + trivially disposable frame"; not designed here.
- **Destructors**: disposal is the effect context purity fences off; a pure destructor has no call sites that could use the fact.
- **`extern` declarations** (spec §32): a foreign body cannot be checked and v1 defines no trust tier (§4.4; open question Q8 declines even an "assumed-pure" annotation for now).
- **Fields and auto-properties as declarations** (the auto-accessor classification is implicit, §6; writing `pure` on an auto-property is rejected as redundant — there is nothing the author's promise adds to what the declaration form already fixes).
- **Static constructors**: they run at type-initialization time under §22.13's machinery; no call site exists to consume the fact.

### 4.2 Parameters and result

A pure member declares **no `ref`, `out`, `out owned`, `owned`, or `scoped` parameters**:

- `ref` is a writable alias of caller storage — prohibited-target machinery (§4.4) with nothing left to permit;
- `out` obliges the body to assign on every normal return; v0.184's copy-out semantics arguably make it a second return channel rather than a caller-storage write, and the relaxation is **recorded as liftable** (open question Q4 — lifting it would make every stdlib `TryParse` annotatable), but v1 stays coarse and uniform: none of the destructive-mode modifiers appear on pure members;
- `owned` brings a disposal obligation into the frame: every exit path must discharge it, and discharge runs `Dispose` — user code at disposal time, the exact thing purity promises not to run;
- `scoped` grants a storage permission (§28.14) that a pure body can never exercise (no field stores) — admitting the modifier would be noise.

Plain parameters remain: value types and records arrive as frame-local copies; class- and interface-typed parameters arrive as **borrows** (readable, never consumed); function-typed parameters arrive as values that may be held and returned but not invoked (§4.4). Nullability is orthogonal and unchanged.

The return type is unrestricted except: an **`owned` class-typed return is rejected in v1** — no pure body can produce an owned class instance (§4.4), so the declaration would merely defer the error into the body. This rejection is automatically lifted by the v1.5 widening (§8). Returning strings, values, records (including borrow-bearing records like `ReadOnlySpan<T>`, subject to the ordinary spec §9.7 lifetime rules; `ReadOnlyStringSpan` is an ordinary record, not borrow-bearing — stdlib §3.3 — and returns without restriction), and function values is admitted.

### 4.3 Permitted operations

A pure body may contain exactly the following; the list is closed, and everything outside it is rejected with a diagnostic naming the operation and this section.

1. **Reads of anything reachable.** Local and parameter reads; instance-field reads through `this` or any borrow; static field, `static readonly`, and `const` reads; enum constants; `typeof(T)` and the type tests `is` / `is not` / `as`; `nameof(…)`; `default(T)`; boxing a value (the box is runtime-managed, §28.9). Reads are not effects in v1 — including reads of *mutable* state; see §9. (Reads spelled as calls — property getters, indexers on library types — are calls, governed by rule 6.)
2. **Locals and frame-local mutation.** Variable declarations of every §21.1 form; assignment, compound assignment, and `++`/`--` where the target's **root is a local variable or by-value parameter of this body and the access path from that root traverses only value-typed storage** (record fields, inline-array slots). The first class-typed dereference on a path leaves the frame and makes the target prohibited (§4.4). Mutating a record local, a record parameter copy, or an inline-array local is frame-private and admitted.
3. **Value, record, enum, and string construction and operations.** All built-in operators of §17.1 on primitives, enums, and `string` (concatenation included); interpolated strings (each interpolated value's implied `ToString`/`Format` call is governed by rule 6); explicit conversions (spec §5.2) and implicit conversions; range construction (spec §16); record construction and `with` update, provided the invoked constructor is compiler-synthesized or declared `pure` (§4.6); inline-array construction and literals of value/record element type.
4. **Defining lambdas and forming method references.** Closure allocation is runtime-managed (§28.9) — permitted like string concatenation. A lambda *body* defined inside a pure body is **not** checked under these rules: it is a separate function that provably cannot run during the pure call (invocation of function-typed values is prohibited, §4.4), and its later invocation happens at the caller's effect discipline, not this body's. Captures follow §28.3/§28.9 unchanged.
5. **All control flow.** `if`/`elif`/`else`, `case`/`when`, `while`, `for`, `do … end`, `break`/`continue`/`return`, `try`/`catch`/`finally`, and `throw` — including the bare re-raise inside a catch arm; a caught exception binding is a borrow (§31.1) and is readable under rule 1. A `for` loop is admitted when its desugaring does not construct an enumerator: over `Range<T>` of primitive steppable type the direct counted-loop form of §16.5/§29.2 — today an admitted elision — becomes **the normative semantics inside a pure body**; over inline arrays, spans (`Span<T>`/`ReadOnlySpan<T>`), `ReadOnlyStringSpan`, and value-typed `OwningBuffer<T>` the desugaring is already the direct indexed loop (§29.2). A `for` over an `IEnumerable<T>`-typed source (including `Array<T>` and `string`) is **rejected in v1**: the desugaring constructs an enumerator class instance and calls interface members; the remedy is an index loop (`for var i in 0..<xs.Count do … xs[i] … end;`).
6. **Calls to pure members.** A call — method, operator, accessor (property read, indexer read), constructor in the positions §4.5/§4.6 admit — is permitted iff the member **declared on the receiver's static type** (or the statically resolved member, for statics/extensions) is `pure`-declared or `pure`-classified (§4.7). Arguments are passed by the rules of §4.2's mirror image: no `owned`/`ref`/`out` argument positions can arise, because pure members declare none.
7. **`throw` with the §4.5 carve-out**, and re-raise.

### 4.4 Prohibited operations

Stated for implementers; each item is the contrapositive of §4.3 with its reason.

1. **Any store outside the frame**: assignment (simple, compound, `++`/`--`) whose target root is an instance field, a static field, an indexer slot of a class instance, or whose access path dereferences a class-typed reference — including all stores through borrows, and stores to `this`'s fields (outside pure constructors' birth stores, §4.6).
2. **Every owned-consuming or disposal-running operation**: explicit `Dispose()`/`DisposeAsync()`; `using var`/`await using` binding of an owned producer (none can arise in v1, so this is belt-and-braces); the discard `_ := expr;` of a non-trivial-dispose owned value (cannot arise); `move`; `owned` arguments; `set(owned)` indexer/property stores; `throw` of anything but the carve-out (the operand must be owned, and the carve-out is the only owned producer).
3. **Class construction** — `new C(…)` for any class type, collection literals of class collection types (`Array<T>`, `Dictionary<K, V>`, … literals construct class instances, §15.4), and calls returning `owned` class instances — **except** the §4.5 throw operand. Reasons recorded: constructors and destructors run arbitrary user code; an owned instance in the frame drags in the whole discharge machinery of item 2. This is the honest cost of v1 (§8 records the widening).
4. **Invoking function-typed values** — lambdas (including one defined in the same body), method references, function-typed parameters, `?(…)` conditional calls: the value's type does not carry effect information in v1, so the call's effects are unknown. §11 sketches the v2 repair.
5. **`await`, tasks, domains, channels, events, synchronization primitives, I/O, and `extern` calls** — all excluded *by rule 6 of §4.3, not by enumeration*: none of their entry points is or can be `pure`-declared (stdlib policy, §13; `extern`, §4.1). The spec text needs no per-API blacklist, which is what keeps the rule set closed under stdlib growth.
6. **Iterator constructs** (`yield`) — §4.1.

### 4.5 The throw carve-out (decided)

Inside a pure body, `new E(args)` is admitted **only** as the immediate operand of a `throw` statement or `throw` expression (§17.12; both forms), and only when:

- **(a)** the invoked constructor of `E` is `pure`-declared (§4.6) — covering, transitively, its chained constructors and `E`'s field initializers; and
- **(b)** `E` is a **trivial-dispose** class (§28.9, an existing §3.6 derived type property; the diagnostic names it).

Soundness, worked through. The construction's two recorded hazards are the constructor's code and the destructor's code. (a) fences the constructor with the same discipline as every other call. For the destructor: ownership of the operand transfers to the runtime's exception machinery at the throw (§31.1 "Exception ownership") — the frame never disposes it. Disposal happens after the matching catch arm. If that arm is in an *impure* caller, disposal runs outside the pure region and is unconstrained, like any other post-return effect. If the arm is inside the pure region (a pure body may `try`/`catch` its own faults, §4.3 rule 5), dispose-after-arm runs *during* the pure call — but every exception in flight within a pure region originates either from a carve-out `throw` in the region's own call subtree (pure members call only pure members, so the subtree is closed) or from a runtime-raised built-in (`IndexOutOfRangeException`, `OverflowException`, `DivideByZeroException`, `NullReferenceException`, `MatchFailureException`, `FormatException`, `DisposedAccessException` — §31.4, all trivial-dispose: string/record/`Array`-of-record state, no destructors). Condition (b) closes the only remaining door: with it, *every* dispose-after-arm inside a pure region is provably a no-op — no user code runs. The condition reuses an existing derived property rather than minting new analysis, the same admission move §15.4 (v0.184) makes for fresh trivial-dispose producers in borrowing literals.

The carve-out follows §31.1's operand rule otherwise: any class, not only `Exception`-derived (#771 stands); deriving from `Exception` remains the recommended default for the reporting surface.

**The strict alternative, presented and declined.** Without the carve-out, a pure body can obtain no owned class instance at all, so `throw expr` is unwritable and a pure method can fault only through built-in operations. That kills the language's own validation idiom inside the entire pure fragment: `Math.Clamp` (raises `ArgumentException` on `low > high`, stdlib §2.2 v0.195), `string.Substring`/`s[i]` (explicit `IndexOutOfRangeException` sites), `int.Parse` (`FormatException`), `Decimal.Round` (`ArgumentOutOfRangeException`) — essentially the whole first wave either faults by explicit `throw` today or is documented to. Purity that excludes argument validation would teach programmers to launder faults through provoked built-in exceptions (`_ := 1 / 0;`) — strictly worse than admitting the construction whose disposal provably runs nothing. Declined with these reasons; the strict form remains a trivial subset an implementation can stage through.

Worked example:

```bezel
public static pure method Checked(x: int, low: int, high: int): int
  if low > high then
    throw new ArgumentException("low must not exceed high", nameof(low));
  end;
  return low if x < low else (high if x > high else x);
end Checked;
```

### 4.6 Pure constructors

A constructor may be declared `pure`. Its body is checked under §4.3/§4.4 with one addition, the **birth-store rule**: assignments whose target root is a field of `this` (including `readonly` auto-property backing stores) are permitted — the instance under construction is fresh and unreachable from any other holder until the constructor returns, so its initialization is not an observable effect. The rule extends to the class's **field initializers** (they execute as part of construction, §22.8): a class with a pure constructor must have field initializers that satisfy the pure body rules. A `pure` constructor's `base(…)`/`this(…)` chain target must itself be pure. Records: the compiler-synthesized positional constructor is pure-classified (§4.7); an authored record constructor may be declared `pure` under the same rules, and record construction inside a pure body requires it (§4.3 rule 3).

In v1, pure class constructors are *consumable* only at the §4.5 throw operand (class construction is otherwise prohibited); they are specified generally anyway because v1.5 (§8) reuses them unchanged, and because the stdlib exception constructors need them now.

### 4.7 Members classified by rule (synthesized bodies)

Compiler-synthesized member bodies have no author to make a promise; the compiler classifies them by checking its own generated code under §4.3/§4.4:

- an **auto-property getter** is pure (a backing-field read);
- an **auto-property setter** is never pure (a field store) and is never classifiable;
- a **record's synthesized members** — positional constructor, `Equals`/`GetHashCode` (§9.2 auto-equality, including the implicit `IEquatable<T>` conformance), `ToString`, `with`-copy — are pure iff the synthesized body's constituent operations are pure under the ordinary rules: for equality/hash, iff every field's equality dispatch target is pure (primitives, `string`, enums, and records that themselves qualify; a class-typed field's `Equals` is virtual user code and disqualifies); analogous per-field reasoning for `ToString`.

This is the one deliberately derived-looking corner, and it does not contradict §2's declared-not-derived argument: the classified body is *language-authored* — its content can change only when the declaration's form changes (auto → block form; a field's type changes), which is already a visible, §3.6-rule-3-style contract event. Authored bodies (block-form accessors, expression-bodied properties, methods) always require the explicit keyword.

## 5. Checking and dispatch soundness

**Per-body, syntactic, declared call graph.** Each pure body is checked in isolation against §4.3/§4.4; every call site is admitted by the *declaration* of the statically named member, never by inspecting its body. There is no inference, no interprocedural fixpoint, and recursion (direct or mutual) needs no special case — the declarations are the induction hypothesis. Cross-module, the imported declaration carries the modifier (the artifact interface, §2.8.2, is extended by one bit per member — the P4 hook); its body was checked when its module compiled.

**Virtual and interface dispatch.** A call in a pure body through a receiver of static type `S` to member `M` is admitted iff `M` *as declared on `S`* is pure. Soundness then needs every dynamic target to be pure, which the following matching rules guarantee:

1. **Purity is never dropped.** An `override` of a pure member must itself be declared `pure` (restated explicitly, like every other matched modifier — §12.1 does not infer across the boundary). A class member implementing a pure interface member must be declared `pure` (§11.2's matching rule gains the purity clause). Omission is a compile error at the override/implementation.
2. **Purity may be strengthened.** An override or implementation *may* declare `pure` where the overridden/interface member is not. The strengthened fact is usable only through the derived or concrete static type — dispatch admission keys on the static type's declaration, so a non-pure base slot admits nothing, whatever the runtime type. (This is the "may not *add* pure to the slot" rule: the slot's contract is fixed by its declaring type; a derived declaration adds a fact about the derived surface only.)
3. **Monotone down the chain.** An override matches against the *nearest* overridden declaration (§12.1's existing rule, as with nullability widening): once a chain member is pure, every further override is pure.

The argument, spelled out: admission site sees `S.M` pure ⇒ by (1) and (3), every override of that slot in every subclass is pure-declared ⇒ by the per-body check, every such body satisfies §4.3/§4.4 ⇒ the dispatched execution, whatever the dynamic type, is a pure body calling only pure members — induction closes.

The three rules in one picture — the base slot's purity binds the chain, the derived-only purity stays derived-surface-local, and auto-accessors participate through §4.7:

```bezel
public abstract class Shape
  public pure abstract method Area(): decimal;      // the slot's contract (§11.5 order: pure
end Shape;                                          //   sits in the async slot, before abstract)

public sealed class Circle: Shape
  public readonly property Radius: decimal;         // auto-getter: implicitly pure (§4.7)
  public constructor(radius: decimal)
    Radius := radius;
  end constructor;

  public pure override method Area(): decimal       // must restate pure (rule 1)
    => Radius * Radius * 3.14159265358979m;
end Circle;

public pure method TotalArea(a: Shape, b: Shape): decimal
  return a.Area() + b.Area();                       // admitted: Shape.Area is pure-declared;
end TotalArea;                                      //   sound for every subclass by rule 1
```

Rule 2 is load-bearing for the stdlib: `string.CompareTo` implements `IComparable<string>.CompareTo` and `string.Equals(Object?)` overrides `Object.Equals` — both slots stay unannotated in v1 (annotating a widely-implemented interface or `Object` member forces `pure` onto every user override: a breaking change, §12), yet the concrete members must be annotatable for `string`-typed receivers. The strict exact-match alternative (purity must match in both directions, like the §22.4 `when`-clause rule) was considered and **declined**: it would sever every interface-implementing stdlib member from purity, gutting the first wave; the recorded cost of asymmetry — one member exposing different contracts through different static types — is already the language's situation for visibility-filtered lookup (§12.2's residual) and is mitigated by the delegation idiom (declare a separate pure method; let the non-pure slot's implementation call it).

**Overloading.** `pure` is not an overload discriminator: §22.6 resolution is by parameter types, and two same-signature members differing only in `pure` cannot coexist (they are the same signature). Like `async`, purity constrains the body and the contract, not selection.

**Generics.** A generic method or member of a generic type may be `pure`; the body is checked once, at the declaration, under its constraints. Calls on constraint-typed values resolve to the constraint interface's declared members — which, in v1, are never pure (no interface annotations ship, §13) — so v1 generic pure bodies handle type-parameter-typed values *opaquely*: copy, store in locals, return, compare with `=` only where the substitution-independent rules admit it. This is exactly why `Math.Min<T>`/`Max<T>`/`Clamp<T>` (constraint-dispatched through `IComparable<T>.CompareTo`) are excluded from the first wave (§13) — the honest finding that **constraint purity** (`where T: IComparable<T>` with a pure obligation) belongs to the same v2 bucket as function-type purity: both are purity in abstracted positions. No per-instantiation purity re-evaluation exists in v1; nothing purity-relevant depends on the substitution.

## 6. Properties, indexers, accessors (decided)

Purity attaches to the *getter* — a setter's purpose is a store, so `pure` on `set` is rejected always. The forms:

- **Auto-properties**: getter implicitly pure, setter never; no marking admitted (§4.1, §4.7).
- **Expression-bodied properties** (§23.2) and **read-only block-form properties**: property-level `pure` (the §3 grammar slot), marking the single getter. Authored bodies, so explicit opt-in — the §2 declared-not-derived argument applies with full force to accessor bodies, which are exactly where "just a quick cache write" effects creep in (a memoizing getter is *not* pure under these rules, and that is the point: the caller of a pure getter may assume re-reads are free of writes).
- **Read-write block-form properties**: per-accessor marking — `pure get => …; set => …;`. The property remains read-write; only the getter joins the pure surface.
- **Indexers**: same as properties (property-level `pure` for `readonly` indexers; `pure get` in accessor blocks). `string`'s two indexers are first-wave annotations.
- **Interfaces**: `pure readonly property P: T;`, `pure method …;`, and accessor-block forms with `pure get;` — implementations must restate purity (§5 rule 1).

```bezel
public class Account
  private _balance: decimal := 0m;

  public property Balance: decimal
    pure get => _balance;
    set => _balance := value;
  end Balance;

  public pure property IsOverdrawn: bool => _balance < 0m;
end Account;
```

## 7. Small print: defaults, `const`, statics

Default-value expressions of a pure member's parameters are checked under the pure body rules — they evaluate at call sites (§22.5), and a pure *caller* omitting the argument must not thereby execute effects. Local `const` declarations are §17.14 material and always admitted. Reads of statics — including rule-2-admitted mutable statics — are reads (§4.3 rule 1, §9); *stores* to any static are prohibited stores; the synchronized-handle statics (`Atomic<T>`, `Mutex`, …) self-exclude operationally because their members are never pure-declared.

## 8. The `Array<T>` tension, and the v1.5 widening (recorded either/or)

The no-class-construction rule excludes the largest block of pure-*feeling* stdlib members: everything returning a fresh `Array<T>` — `string.Split` (×3), `ToCharArray`, `Encoding.GetBytes(s)` (the brief's named exclusion), `Array<T>.Sorted`/`Reversed`/`ToArray`/`Slice`, extension `Words()` — plus `StringBuilder` accumulation. These are observably effect-free from the caller's seat; excluding them is the visible cost of v1's coarseness, and both widening shapes are recorded now so graduation can adopt one without redesign:

- **Variant A — owned-return flow** (the future-directions sketch's direction): admit `new C(…)` (pure constructor required) when the instance provably flows to an `owned` return on every path, undisposed in-body. The caller assumes the obligation; the body never runs a destructor. Cost: a genuine flow analysis, and a hard interaction with the exception edges — an owned local live at a `throw` (carve-out included) is discharged by §28.3's exception-path machinery, which runs `Dispose`; the analysis must therefore also prove no throw can occur while the instance is frame-held, or forbid binding it at all (producer-to-return only). That is a new, subtle checker.
- **Variant B — trivial-dispose admission** (recommended): admit construction of **trivial-dispose** classes (§28.9) with pure constructors, unrestricted in-body — binding, dropping, discarding, and returning (plain or `owned`) all admitted, because disposal of a trivial-dispose instance provably runs no user code on *any* path, exception edges included. No flow analysis: the same derived-property admission as the §4.5 carve-out condition (b) and §15.4's literal rule. Collection literals of trivial-dispose instantiations become admissible the same way. Coverage: `Array<T>` and the borrowing collections are trivial-dispose for every non-owned element type (§28.9), so *every member on the pressure list above except `StringBuilder`* (destructor-backed storage, honestly still excluded — pure string building stays concat/interpolation) becomes annotatable, including the `Exception(message, cause)` snapshot constructor (builds a fresh `Array<ExceptionInfo>`), currently excluded from §13.

Recommendation: **Variant B**, staged as v1.5 after one implementation cycle of v1 (the stdlib sweep's exclusion list is itself the pressure evidence); Variant A remains the recorded later step for the *non*-trivial case (pure factories of owning aggregates) if demand materializes — the two compose, B being A's easy stratum. This is a deliberate divergence from the P2-derived sketch, which reached for A first; reasons above (soundness-per-machinery ratio).

## 9. Purity is not determinism

A pure member may read mutable state: two calls with identical arguments may return different values (a config field mutated between calls; `Environment`-style state read through some future pure surface). The v1 contract is exactly *no writes, no I/O, no disposal-time user code, no concurrency interaction* — chosen because it is checkable by the closed syntactic rule set and because it is what the near-term consumers need (P3's clauses must be effect-free, not deterministic; a `requires` over current state is the norm). The stronger tiers — **`det`** (result depends only on arguments: no reads of mutable state) and **`total`** (det + provably terminating, no faults) — are the natural future refinements and are **deliberately out of scope**: they are separate promises, with separate checking machinery (reachability of mutable state; termination), and nothing in this design blocks their later addition as further modifiers or contract-tier annotations. Recorded so the v1 keyword is not later re-interpreted: `pure` means effect-free, permanently.

## 10. Relationship to §17.14 constant expressions

Containment, stated as the design invariant: **every §17.14 constant expression is valid pure-body material** — literals, `const` and enum references (reads), `nameof`, the listed operators on primitives/`string`, the `if` expression, parenthesization: each is admitted by §4.3 rules 1/3/5. The converse direction does not hold and must not: `pure` is a *runtime* discipline — pure bodies contain locals, loops, borrows of heap objects, `throw` — and a call to a pure method is **not** a constant expression (§17.14 excludes calls of any kind; this proposal does not change that). Compile-time *evaluation* of pure calls (constexpr-style folding) is a separate, later question: it would require the det tier at least (§9), and is listed under non-goals. The two features stay siblings: §17.14 defines what the *compiler* evaluates; `pure` defines what a *call* may touch.

## 11. Higher-order purity: the deferred function-type form

The v1 rule "never invoke a function-typed value" is the honest coarse cut: a function value's effects are invisible in its type. The structural slot for the repair already exists — §27.1 makes `owned`/`ref`/`out`/`scoped` part of function-type identity, so purity extends the same axis:

```bezel
// v2 sketch — NOT adopted by this proposal:
var f: (int) pure -> int := (x) => x * 2;      // lambda checked as a pure body
public pure method Select(xs: Array<int>, f: (int) pure -> int): owned Array<int>
  // may invoke f: its type carries the promise
end Select;
```

Identity and assignability would follow the §27.1.1 pattern (a pure-typed value usable where an unannotated type is expected — safe forgetting; never the reverse), method-reference formation from a pure method would mint a pure-typed value, and the §13 exclusions of `Sort(comparer)`, LINQ (stdlib §5), `Trim(chars)`/`Join(items)` (iteration of caller-supplied `IEnumerable` — interface dispatch, the same abstracted-position problem) would unwind. Not adopted now, with reasons: it doubles the function-type vocabulary along a new axis (every higher-order stdlib signature must then choose), it drags constraint purity with it (§5, generics), and first-order purity should prove its consumer value (P3 gating) before the type system pays that rent. One v1 consequence recorded plainly: forming a method reference *from* a pure method loses the purity fact at the formation boundary — retrieval is v2's job.

## 12. Migration and compatibility

- **Additive.** No existing program changes meaning or compiles differently: `pure` appears nowhere today (§3), no rule of this proposal fires on unannotated code, and `pure` is contextual so `pure` as an identifier keeps working.
- **Adding `pure` to an existing non-virtual, non-interface member is non-breaking** for callers (strictly more contexts admit the call) and non-breaking for the member's author only in the forward sense: the body is now checked. The stdlib first wave (§13) confines itself to such members.
- **Adding `pure` to a virtual or interface member is a breaking change** — every override/implementation must add it (§5 rule 1). This is deliberate and is the reason `IComparable<T>`, `IEquatable<T>`, and the `Object` roots are not annotated in v1; doing so later is a major-version event with a mechanical migration (add the keyword or lose the conformance).
- **Removing `pure` is a breaking change** exactly like removing `owned` from a return: pure-context callers stop compiling. Purity is part of the published contract; the §3.6 rule-3 documentation discipline applies verbatim.
- **Conformance:** purity is **Core**. The checker is static, needs no runtime support, and Extended features interact only as prohibited content (§4.4). No Appendix D row; Core implementations reject `pure` programs they do not implement under the ordinary unknown-construct rule until the feature lands (pre-landing, the keyword simply does not parse).

## 13. The stdlib first wave (verified), and the exclusions

Annotations proposed for the graduation revision — every member below verified present in stdlib v0.197 at the cited section. Non-virtual/static/concrete members only (§12).

**`Math` (stdlib §2.2):** `Abs` (int/long/double/decimal), `Sqrt`, `Pow`, `Exp`, `Log` (both), `Log10`, `Log2`, `Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Atan2`, `Sinh`, `Cosh`, `Tanh`, `Floor`, `Ceiling`, `Round` (both), `Truncate`, `Sign` (int/long/double), `Fmod`, `IEEERemainder`. **Excluded:** `Min`/`Max`/`Clamp` — constraint-dispatched through unannotated `IComparable<T>.CompareTo` (§5 generics; v2 constraint purity). `Clamp`'s `ArgumentException` is carve-out-compatible when it returns.

**`string` (spec §4.6, stdlib §9.6):** `Length`, `IsAscii`, both indexers (`[i: int]: char`, `[r: Range<int>]: string`), `Contains` (string/char), `StartsWith`, `EndsWith`, `IndexOf` (char/string), `LastIndexOf` (char/string), `Substring` (both), `Trim()`, `TrimStart`, `TrimEnd`, `ToUpper`, `ToLower`, `Replace` (both), `PadLeft` (both), `PadRight` (both), `Insert`, `Remove` (both), `CompareTo`, `Compare` (both statics), `Equals(string?)` (both), the `Object` overrides (`Equals`/`GetHashCode`/`ToString`), `operator +`, `From(char)`, `From(buffer: Array<char>, start, count)` (reads via the `Array<char>` enablers below), `Repeat`, `IsNullOrEmpty`, `IsNullOrWhiteSpace`, `AsSpan` (all four). **Excluded with reasons:** `Split` (×3), `ToCharArray`, `GetEnumerator` — construct class instances (v1.5 Variant B lifts the first two); `Trim(chars)`, `Join` — iterate a caller-supplied `IEnumerable` (abstracted-position calls, §11); `From(CString/WString)`, `FromCStringPointer`, `FromWStringPointer` — FFI boundary.

**Primitives (stdlib §9.1–§9.5):** per type — `Parse(s)` and radix/format variants, `ToString` variants, `CompareTo`; `Double`/`Float` classification predicates (`IsNaN`, `IsInfinity`, `IsPositiveInfinity`, `IsNegativeInfinity`, `IsFinite`) and `IeeeEquals`/`BitwiseEquals`; `Decimal.Round(digits)`; `Char.IsDigit`/`IsLetter`/`IsLetterOrDigit`/`IsWhiteSpace`/`IsUpper`/`IsLower`/`IsControl`/`IsPunctuation`/`ToUpper`/`ToLower`; `Boolean.Parse`/`ToString`. The synthesized `Equals(other: T)` conformances classify implicitly (§4.7). **Excluded:** every `TryParse` — `out` parameter (§4.2; lifted by Q4 if taken).

**Read-surface enablers:** `Array<T>.Count`, `Capacity`, indexer **getter** (stdlib §4.4) — required for the index-loop idiom (§4.3 rule 5); `ReadOnlyStringSpan` read members — `Length`, `IsEmpty`, `[i]`, `Slice` (both), `IndexOf(char)`, `Contains(char)`, `StartsWith`/`EndsWith` (span and string forms), `SequenceEqual` (span and string forms), `ToString` (stdlib §3.3). **Excluded:** `Array<T>` mutators and `Sorted`/`Reversed`/`ToArray`/`Slice` (stores; fresh arrays — v1.5), `Contains`/`IndexOf` on `Array<T>` (element-equality dispatch), `CopyTo` (writes destination), the whole `Dictionary` read family (`TryGetValue`: `out`; `ContainsKey`/indexer: key-equality dispatch on unannotated `Equals`/`GetHashCode`) — recorded, not designed around.

**Exception constructors (spec §31.4, stdlib §2.17):** `pure constructor` on `Exception()`, `Exception(message)`, `ArgumentException` (both), and the `(message)` constructors of the built-in hierarchy — the carve-out enablers. **Excluded:** `Exception(message, cause)` — its snapshot flattening builds a fresh `Array<ExceptionInfo>` (v1.5 Variant B lifts it; until then, pure code cannot chain causes).

**Interfaces deliberately not annotated in v1:** `IComparable<T>`, `IEquatable<T>`, `IDisposable` (absurd anyway), `IFormattable`, the `INumber<T>` family, `Object`'s members. Each would be a breaking change to every implementor (§12).

The sweep is itself the audit the P2 sketch promised: performing it produced the `Min/Max/Clamp`, `TryParse`, `Split`, and `Dictionary` findings above — each now a recorded fact with a staged remedy rather than a surprise at graduation.

## 14. Interactions (complete enumeration) and divergences from the P2 sketch

| Spec surface | Interaction |
|---|---|
| spec §9 records | construction/`with` admitted with synthesized-or-pure constructors; synthesized members classify (§4.7); record mutation is frame-local under the root rule (§4.3 rule 2) |
| spec §10 enums | constants are reads; authored enum methods annotatable |
| spec §11.5 / §11.8 | modifier slot (§3); static interface members may be `pure` (abstract-only; conformance requires restating) |
| spec §12 | override matching gains the never-drop/may-strengthen/monotone rules (§5); `sealed override` composes with `pure`; abstract pure members bind all concrete subclasses |
| spec §13 | visibility orthogonal; accessor-visibility (§13.6) composes with `pure get` |
| spec §14 | declaration-site checking under constraints; constraint calls non-pure in v1 (§5); no instantiation-dependent purity |
| spec §17.14 | containment invariant (§10); no change to the constant sublanguage |
| spec §19–§21 | all control flow admitted (§4.3 rule 5); `case` arms/guards are expressions checked by the same rules |
| spec §22.2–§22.5 | arrow bodies admitted (assignment arrow only under the root rule); parameter modifiers excluded (§4.2); defaults checked pure (§7); named arguments unaffected |
| spec §22.6 | not an overload discriminator (§5) |
| spec §22.7–§22.13 | pure constructors + birth stores (§4.6); static reads admitted, static stores prohibited; static constructors never pure (§4.1) |
| spec §23 | accessor design (§6); `set(owned)` positions are owned-consuming, prohibited in pure bodies |
| spec §25 | operator declarations annotatable (`string.operator +` in wave 1); operator *use* in pure bodies resolves through the call rule |
| spec §26 | extension methods/properties annotatable; receiver is a borrow (§26.7) — reads compose; statically dispatched, so no matching rules needed |
| spec §27 / §28.9 | define-don't-invoke (§4.3 rule 4 / §4.4 item 4); closures runtime-managed; captures unchanged; method-reference formation loses purity at the boundary until v2 (§11) |
| spec §28 | no owned params/returns (v1), no consuming ops, no disposal; borrows readable; §28.14 `scoped` excluded; ownership rules otherwise untouched — purity adds no new ownership states |
| spec §29 | iterator methods rejected (§4.1); `for` admitted per desugaring form, enumerator path rejected, Range counted-loop made normative in pure bodies (§4.3 rule 5) |
| spec §30 | `pure` excludes `async`/`await`; pure methods freely callable *from* async code; task/domain/channel surfaces self-exclude via the call rule |
| spec §31 | throw carve-out (§4.5); catch binding is a readable borrow; bare re-raise admitted; exception constructors annotated (§13) |
| spec §32 | `extern` never pure (§4.1); no trust tier in v1 |
| spec §2.8.2 | purity joins the artifact interface (one bit per public member) — the P4 seam |
| Appendix C | new **C.14 Purity**: (1) `pure` combines with neither `async` nor `yield`; (2) a pure member declares no `ref`/`out`/`out owned`/`owned`/`scoped` parameter and, in v1, no `owned` class-typed return; (3) a pure body contains no store whose target root leaves the frame, no disposal-running or owned-consuming operation, no class construction outside the throw carve-out, no invocation of a function-typed value, and no call to a member not pure-declared or pure-classified; (4) the throw-carve-out operand's class is trivial-dispose and its constructor pure; (5) overrides and implementations of pure members restate `pure`; purity never narrows down an override chain; (6) `extern` members carry no `pure`; (7) field initializers of a pure-constructed class and default-value expressions of pure members' parameters satisfy the pure body rules |

**Divergences from the P2 sketch, recorded:** (1) the **throw carve-out** is new — P2's "throw is permitted" was unimplementable without it (§4.5's strict-alternative analysis); the carve-out's two conditions are this proposal's addition. (2) P2's higher-order line ("may invoke a function value only if its type carries pure") is **collapsed to a flat prohibition** in v1 — no pure-carrying function type exists until v2, so the P2 wording described an empty set; stating the prohibition directly is the same rule made honest. (3) `Math.Min/Max/Clamp` fall **out** of P2's "annotate `Math`" — the constraint-dispatch finding (§5, §13). (4) The `for`-over-`IEnumerable` rejection is a precisification P2's own `Median(xs: Array<int>)` example silently needed (the index-loop idiom plus `Array<T>` enabler annotations keep the example writable). (5) The v1.5 widening is recommended in the **trivial-dispose** shape rather than the sketched owned-return-flow shape, both recorded (§8).

## 15. Staging

1. **v1 (this proposal):** first-order declared purity — grammar, §4 rule set, §5 checking, §6 accessors, C.14, the §13 stdlib wave. Core conformance. Ships with a regression suite keyed to each C.14 bullet (accept + reject pairs per rule, spec-cited per `tests/README.md`).
2. **v1.5 (recorded option, §8):** trivial-dispose construction admission (Variant B) + the `owned`-return unblocking + the exclusions it lifts (`Split`, `ToCharArray`, `GetBytes`, `Sorted`/`Reversed`/`ToArray`, `Exception(message, cause)`). Trigger: the v1 annotation wave's exclusion list confirmed against real pure-fragment usage, one implementation cycle after v1.
3. **v2:** function-type purity (§11: type identity, assignability, formation, lambda checking) together with constraint purity (`where`-clause purity obligations) — designed as one train, since both are purity-in-abstracted-positions; iterator purity revisited; the Q4 `out` relaxation if not taken earlier.
4. **Out of scope permanently for this line:** `det`/`total` tiers (§9) and compile-time evaluation of pure calls (§10) — separate proposals if wanted.

## 16. Alternatives considered and declined

- **Derived (inferred) purity** — the checker computes each method's effects bottom-up; no keyword. Declined: converts observations into silent obligations (§2); makes the artifact interface body-dependent; every leaf-edit becomes a potential transitive API break with no declaration to point at. The whole-program-analysis variant additionally dies at module boundaries.
- **Effect polymorphism / effect variables** (Koka-style row polymorphism, or an `impure`-default with inference inside modules). Declined for v1: the consumer set (P3 clauses, combinator admissions) needs a boolean, not an algebra; the staging path (§15) keeps the algebra reachable if the v2 higher-order work ever demands it.
- **Purity as an attribute/comment convention** pending the deferred attribute system (rationale §18). Declined: unchecked purity is documentation, which is the status quo; the entire value is the compiler holding the promise, which requires grammar residence (the C#-Code-Contracts lesson cited in the P3 mandate).
- **Strict no-carve-out throw rule** — declined with the §4.5 analysis (validation idiom unwritable; fault-laundering incentive).
- **Exact-match purity at override/implementation boundaries** — declined with the §5 analysis (severs stdlib interface implementations from purity; delegation idiom covers the residual).
- **Admitting `pure` on auto-properties as an optional redundancy** (the `static const` precedent, §22.12). Declined: the redundancy there disambiguates nothing either, but is grandfathered; here it would train authors to believe unmarked auto-properties are impure, which is false.
- **A compiler-known "pure builtin" list instead of stdlib annotations** — declined: name-gated special-casing is the pattern the project is retiring (proposal 0005); annotations keep the stdlib self-hosting-honest (a member is annotated only if its body could check, which is how the §13 exclusions were found).

## 17. Open questions / owner decisions

1. **Q1 — ratify the throw carve-out** (§4.5) with both conditions (pure constructor; trivial-dispose class), versus the strict alternative. Recommendation: ratify.
2. **Q2 — v1.5 shape and trigger** (§8): Variant B (trivial-dispose admission, recommended) vs Variant A (owned-return flow), and whether v1.5 lands with graduation or one cycle later. Recommendation: B, one cycle later.
3. **Q3 — implicit classification of synthesized bodies** (§4.7): confirm the auto-accessor/record-synthesis carve-out from declared-not-derived, or require explicit `pure` everywhere (which would demand a marking syntax for compiler-generated members that have no declaration site — the reason the implicit form is recommended).
4. **Q4 — the `out` relaxation** (§4.2): admit plain `out` on pure members under v0.184 copy-out semantics (makes every `TryParse` annotatable), in v1 or deferred. Recommendation: defer to v2 with the P1 `choice`-sum alternative weighed against it (a `Parse` returning a sum may obsolete the `out` idiom entirely).
5. **Q5 — ratify strengthening-only override/implementation matching** (§5) against the exact-match alternative.
6. **Q6 — keyword and contextuality**: confirm `pure` as a contextual keyword (B.2) rather than a reserved word; no competing spelling is proposed.
7. **Q7 — stdlib wave scope** (§13): confirm the enabler additions (`Array<T>`/`ReadOnlyStringSpan` read surfaces) ride the same graduation as the mandated Math/string/primitives set, since the index-loop idiom needs them for the feature to be usable at all.
8. **Q8 — extern trust tier**: v1 declines even an "assumed pure" extern annotation; confirm, or direct a P3-style `assumed` tier to be designed jointly with contracts (where the same trust question recurs).

## 18. Relationship to existing work

Future-directions P2 (this proposal's mandate) · P3 contracts (consumer #1; "contract expressions are pure borrows" presupposes this rule set) · P4 behavioral artifact interface (§2.8.2 seam; purity is interface bit #1) · P13 property testing (pure properties get exhaustive/parallel checking) · proposal 0022 (test framework — pure assertions) · stdlib §6.12 `Parallel`/§4.19 `FrozenArray` (future admission tightening, not changed here) · proposal 0005 (self-hosting keeps the annotate-only-what-checks discipline honest) · §17.14 (the compile-time sibling, §10).
