# Bezel Guide 6: Contracts and Purity

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.270.*

Bezel lets a signature state obligations the compiler enforces: **`requires(...)`** for what the caller must guarantee, **`ensures(...)`** for what the member guarantees back, **`invariant(...);`** for what an object maintains at its public boundary (spec §22.16), and **`pure`** for members that promise no effects at all (spec §22.15). The syntax is small; the *postures* are what this guide teaches. Contracts are grammar, not a debug-mode assertion library: `requires` runs in every build, and a violation is a bug report, not an outcome (§1). Two postures will break habits you may bring along, and each is headed **How Bezel does it**. First: `requires` states only what a *correct caller already guarantees* — never validation of trust-boundary input, which is value-shape territory Guide 2 owns (§2). Second: `pure` is a promise you *declare* and the compiler verifies — never inferred from a body (§5). If you skim everything else, read those two and the table in §9.

**Contents:** §1 contracts are grammar · §2 `requires` is for caller bugs, never input validation · §3 `ensures` and `old` · §4 class invariants: where they are checked · §5 `pure` is declared, not inferred · §6 the pure signature and body · §7 what `pure` buys · §8 `Debug.Assert`: the landed assertion surface · §9 choosing the right tool.

---

## 1. Contracts are grammar

Contract clauses sit **between the signature and the body**. Each clause's parentheses are structural, so a clause is self-delimiting and takes no `;`. The `invariant` member, being a class member, keeps its `;` (spec §22.16):

```bezel
public class Account
  private _balance: decimal := 0m;
  private _limit: decimal := 0m;

  invariant(_balance + _limit >= 0m);

  public constructor(limit: decimal)
    requires(limit >= 0m)

    _limit := limit;
  end constructor;

  public method Withdraw(amount: decimal)
    requires(amount > 0m)
    requires(amount <= _balance + _limit)
    ensures(_balance = old(_balance) - amount)

    _balance := _balance - amount;
  end Withdraw;
end Account;
```

A clause's expression is `bool` and is checked under the pure-body discipline of §22.15, so it can call only members that are `pure`-declared or pure-classified (§7). It can neither write state, consume a value, nor suspend. And unlike a pure *body*, a clause may not `throw`: a clause states a proposition, and the mechanism and blame of a violation belong to §22.16's own machinery. (The throw carve-out stays available to a clause's pure *callees*.) A clause that evaluates `false` raises **`ContractViolationException`** (spec §31.4). It is catchable, and its `Message` names the clause kind, the member, and the clause's source text — but it is never a control-flow tool. It reports a **bug**: a caller bug for `requires`, an implementation bug for `ensures`/`invariant`.

**Contracts are not compiled out.** The habit from environments where assertion machinery vanishes in release builds does not transfer. A `requires` clause is **always-on**: evaluated on every invocation, in every build mode. The only elision is elision-by-*proof* — a statically discharged clause compiles to nothing. `requires` replaces the always-on `throw new ArgumentException(...)` prologue, so it keeps that idiom's posture; the remedy for a hot path is the proven tier (§3), not a knob. `ensures` and `invariant` run under **checked-contracts mode**, off by default in release. But that relaxation is *defined* non-evaluation: because clauses are pure (spec §22.16), it is unobservable for any program whose clauses hold and never fault (§3).

Clauses are part of the member's contract in the fullest sense. They are carried by the artifact interface (spec §2.8.2), so a caller compiling against your module sees them — API documentation the compiler enforces. And they are **slot-bound** (spec §22.16): a clause on a virtual, abstract, or interface member binds every override and implementation, inherited verbatim and never restated. An override may *add* `ensures` clauses — strengthening its guarantee, which is always sound for callers. It may **not** declare `requires` clauses: strengthening a precondition would break every caller dispatching through the base contract.

Two practical notes. First, write **one condition per clause**: the clause is the unit of blame, evaluation order (first `false` wins), and interface identity, so `requires(a) requires(b)` gives better diagnostics than `requires(a and b)`. Second, know the edges of the scope (spec §22.16). An `async` method takes `requires` only, evaluated synchronously before the first suspension — a violation surfaces on the caller's own stack, not through the task. Iterator methods, accessors, operators, and lambdas take no clauses in this revision. One edge is not a rejection but a reclassification: a clause on a **record's** constructor — `requires`, `ensures`, either visibility, any tier — makes that record **constructor-only** (spec §9.9, v0.268), a §3.6 derived type property whose two consequences are static and hold in every build mode: the record is not defaultable (`default(R)`, an uninitialized `R` slot, `where T: new()` at `R`) and `with` over it is a compile error. A record declaring **positional** fields may not declare such a constructor at all — a compile error at the record declaration, whose diagnostic names the two remedies: move the storage to auto-properties in a body record, or state the condition on a static factory method instead. And because §3.6 rule 3 makes a derived type property part of the public contract, adding *or removing* that clause is a breaking delta in both directions (spec §2.8.6) — the one case where a clause is more than the member's own contract.

---

## 2. How Bezel does it: `requires` is for caller bugs — never trust-boundary input

This is the boundary that matters most, and it mirrors Guide 2 §7's table exactly. The failure-signal rule (stdlib §1) puts every fault condition in one band. Band 1 is the **caller bug** — a condition a correct caller already guarantees. That is `requires` territory, with defect exceptions like `ArgumentException` as the pre-contract residual. Bands 2–3 are **expected outcomes**: absence, bad input, a failure the caller branches on. Those are returned as values — `T?`, a Try shape, `Result<T, E>`, a domain choice. A contract on an expected outcome misclassifies it as a bug.

A good `requires` — an index precondition. No correct caller ever passes an out-of-range index, so an out-of-range index is a bug, and the clause says so in the signature. (Where the bound is a literal range rather than a live `Count`, the `in` spelling is clause material too: `Range<T>.Contains`, `IsEmpty`, and `IsOpenEnded` carry `pure` as of v0.263, so `requires(port in 0..65535)` compiles — stdlib §9.8, spec §16.3.)

```bezel
public class SampleWindow
  private owned _samples: Array<double> := new Array<double>();

  public method Push(value: double)
    _samples.Add(value);
  end Push;

  public method At(index: int): double
    requires(index >= 0)
    requires(index < _samples.Count)

    return _samples[index];
  end At;
end SampleWindow;
```

A bad `requires` — the same syntax, on user input. This *compiles*: `int.ParseOrNull` is `pure`, so it is legal clause material. That is exactly why the trap is worth naming — the compiler enforces the clause's purity, not your trust-boundary judgment:

```bezel
// WRONG: rawPort arrives from outside the program. Its invalidity is an
// expected outcome, not a caller bug — no caller of Listen can guarantee it.
public method Listen(rawPort: string)
  requires(int.ParseOrNull(rawPort) <> null)   // misclassified: band 2 dressed as band 1

  Bind(int.ParseOrNull(rawPort)!);
end Listen;
```

A violated `requires` says "fix the calling code" — but there is no calling code to fix. The input was always going to be wrong sometimes. The first rewrite makes the outcome a value the *caller* branches on — and renames the method to say so: a `Try` name announces that the declined path is expected and unexceptional (stdlib §1's `Try` convention, the bool-only shape; Guide 2 §2–§3):

```bezel
public method TryListen(rawPort: string): bool
  var port := int.ParseOrNull(rawPort);
  if port = null then
    return false;                 // expected outcome, reported as a value
  end;
  Bind(port);                     // narrowed by the early exit
  return true;
end TryListen;
```

`TryListen` follows one of the stdlib's own bool-only specimens, `TryAdd` (stdlib §4.10): where the owning set's `Add(owned item)` silently disposes a duplicate, `TryAdd(owned item): bool` reports the declined path as a value to act on:

```bezel
if not registry.TryAdd(move session) then    // declined: an equal element is already
  _rejected := _rejected + 1;                //   present; session is consumed either way,
end;                                         //   and on this path also disposed (stdlib §4.10)
```

When no caller will branch — the consumer sits above the method, at a *supervision boundary*: a catch site that logs, retries, or converts failures into protocol answers — the sanctioned signal is a typed exception (Guide 2 §4; the failure-signal rule's band 4, stdlib §1). It is still never a caller bug: the input remains an expected outcome, and only the signal changes from the misclassified `requires`. Here the plain name keeps its plain contract — `Listen` returns having bound, or it throws:

```bezel
public method Listen(rawPort: string)
  var port := int.ParseOrNull(rawPort);
  if port = null then
    // Boundary-consumed expected failure: a typed exception, still never
    // a requires. FormatException is the built-in format fault (spec §31.4).
    throw new FormatException($"not a port: {rawPort}");
  end;
  Bind(port);                                     // narrowed by the early exit
end Listen;
```

Why not just call the throwing `int.Parse(rawPort)` and let its exception fly, if you were going to throw anyway? Two reasons. The throwing form is the *assert-shaped* tier — stdlib §1's words are that it "treats absence as a bug" — so calling it on boundary input re-asserts, in exception spelling, the same misclassification the bad `requires` made. And it splits the condition: `int.Parse` raises `FormatException` for a malformed numeral but `OverflowException` for an out-of-range one (stdlib §9.1, §2.3), while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words. The same reasoning rules out `ArgumentException`: the `Argument*` family is part of band 1's pre-contract defect residual (stdlib §1) — the exception that blames the *calling code* — and no caller is at fault here. Throwing it would plant a defect report in the boundary's log — telling whoever triages that log that the calling code needs fixing, when it does not; `FormatException` blames the input's shape, which is what actually failed. (The consuming side of this signal is Guide 2 §4's catch example, which handles exactly this fault.)

The two-question test before writing `requires(E)`: *could a correct caller always guarantee E?* and *is a false E a bug in the calling code?* Two yeses: contract. Either no: a value shape — or, at a boundary consumed by logging/retry, a typed exception, as in the `Listen` rewrite (Guide 2 §4).

When it does run, `requires` evaluates after argument binding and before anything else — before a constructor's chain, before field initializers, before the first body statement. Clauses run in declaration order, stopping at the first `false` (spec §22.16). A constructor's `requires` may not observe `this`, since no field is assigned yet; it reads parameters and statics only.

---

## 3. `ensures` and `old`: naming what you guarantee back

An `ensures` clause evaluates on every **normal-completion** path, and never on a path that exits by `throw` (spec §22.16). It runs after the return value and every `out` slot are bound, and before the frame's scope-end disposals — so every borrow is still live for the clause. Inside it, `result` names the returned value, read as a borrow; `old(E)` names the value `E` had at entry:

```bezel
// private owned _items: Array<string>;
public method Discard(count: int)
  requires(count >= 0)
  requires(count <= _items.Count)
  ensures(_items.Count = old(_items.Count) - count)

  for var i in 0..<count do
    _items.RemoveAt(_items.Count - 1);
  end;
end Discard;
```

**`old` is shallow.** A class-typed capture holds the entry *reference*, so `old(_items).Count` reads the count *now* — almost never what you meant. `old(_items.Count)` captures the entry count. The idiom is value-typed projections inside `old`, and implementations are directed to warn on the dereferenced-class-capture form (spec §22.16). One operand kind is rejected outright: a **move-only value** (an owning inline array, an owning choice — spec §15.3) cannot be captured at all — a copy is forbidden and a borrow would observe post-state — so capture a copyable projection instead: given an owning-inline-array field `private _pool: [16 owned]Connection?;`, write `old(_pool[slot] <> null)`, or capture a `pure` choice method's result (spec §22.16, v0.253).

Contracts compose. Here the class invariant from §1 is what makes the postcondition true:

```bezel
public pure method Headroom(): decimal
  ensures(result >= 0m)

  return _balance + _limit;     // the §1 invariant guarantees non-negativity
end Headroom;
```

Two more rules worth knowing. First, a clause that *faults* — overflow, an indexer fault, a pure callee's own validation throw — propagates that exception as itself, never converted to `ContractViolationException`. The clause did not evaluate `false`; it failed to evaluate, which is a defect in the contract, and the original exception is its diagnosis. Second, every clause occupies one recorded **tier**: **proven** (discharged statically, zero cost), **checked** (a runtime evaluation), or **assumed** (reserved, unpopulated). The `static` qualifier demands the proven tier: an undischargeable `static` clause is a compile error, never a silent runtime check. The shipped prover floor is deliberately small — constant folds, plus the `when true`/`when false` `out`-nullability postconditions (spec §22.4 — the Try shapes' familiar carrier), whose meaning is a `static ensures` clause (the `when` clause is their only spelling, spec §22.16). So essentially every clause you author today is checked. The extended fragment (linear integer arithmetic, null-test flow, closed-set case analysis) is fixed in Appendix D.2 so `static`'s meaning never drifts (spec §22.16).

---

## 4. Class invariants: where they are checked

An `invariant(E);` member declares a predicate over instance state; the class invariant is the conjunction of the class's and its bases' invariant members. It is an *observable-boundary* property: it must hold whenever the instance is observable from outside itself, and may be freely false while the instance operates on itself. The instrumentation set is exact (spec §22.16), and worth memorizing before relying on it:

- **Construction exit** — the normal completion of the *most-derived* constructor (base and mid-chain completions are interior).
- **Entry and normal exit of every `public` instance method**, and of every **`public` setter**. Order at entry: `requires`, then the invariant; at exit: `ensures`, then the invariant.
- **Getters are exempt** — the recommended getter is a `pure` one, which cannot mutate, closing the gap.
- **Never checked**: non-public members, static members, destructors, and `Dispose` (disposal legitimately tears state down); throw exits check nothing.
- **`this`-receiver calls are exempt** — a public method calling another public method on `this` enters its unchecked face, so interior algorithms may pass through broken intermediate states without tripping their own alarm. Calls on *other* instances of the same class check normally.

What the invariant buys a reader is locality. `invariant(_balance + _limit >= 0m);` is one line that every public entry and exit re-establishes, so no method body needs to re-derive it — §3's `Headroom` leaned on it directly. Like `ensures`, invariants evaluate under checked-contracts mode (§1).

---

## 5. How Bezel does it: `pure` is declared, not inferred

Elsewhere you may be used to a compiler or analyzer *deducing* that a function has no side effects. Bezel refuses to: `pure` is a **declared, compiler-verified part of the signature**, carried by the artifact interface like `owned` on a return (spec §22.15, §2.8.2). The body is checked against the promise; the promise is never read off the body.

The reason is stability (rationale §9.7). Inferred purity would be an observation callers silently convert into an obligation: adding one log line to a leaf method would flip the inferred purity of every transitive caller and break every contract clause that called them, with no declaration anywhere recording that a promise existed. Declared purity puts the promise, and any future breakage, at an explicit keyword whose removal is a visible signature change.

Be equally clear about what `pure` is *not*: it is **not determinism**. Purity promises no writes, no I/O, no disposal-time user code, no task or synchronization traffic. But a pure member may read *mutable* state, so two calls with identical arguments may return different values (spec §22.15). Do not memoize on the strength of `pure` alone.

Dispatch keeps the promise honest. An override of a `pure` member must itself be `pure`. An override may *add* purity where the base lacks it, usable only through the derived static type; once a chain member is pure, every further override is (spec §22.15). Admission at a call site keys on the static type's declaration — which is what lets the checker admit virtual calls in pure bodies without seeing any body.

---

## 6. The pure signature and body

**The signature rules** exist so that nothing observable enters or escapes the call. A pure member declares no `ref` or `out` parameter — both are caller-visible write channels. It declares no `owned` or `scoped` parameter, and no move-only value parameter (an owning-choice or owning-inline-array value — spelled plainly, it still carries a disposal obligation whose scope-end discharge runs user code). It cannot return an `owned` class instance or a move-only value — no pure body could produce one. (The move-only fences are the newest rules here: the return side landed v0.230, the parameter side v0.234.) What remains: value and record parameters arrive as frame-local copies; class and interface parameters arrive as borrows, readable but never consumed; and function-typed parameters may be held and returned but not invoked (spec §22.15).

The stdlib teaches the cost of an `out` parameter with one example (stdlib §2.2): `Math.DivRem(a, b, out remainder)` is **not** `pure`, solely because a pure member declares no `out`. When you need a pure multi-result, return a record:

```bezel
public record QuotRem(Quot: int, Rem: int);

public static pure method DivMod(a: int, b: int): QuotRem
  requires(b <> 0)

  return new QuotRem(a / b, a mod b);
end DivMod;
```

Record construction is fine in a pure body (the synthesized positional constructor is pure-classified), and the record return carries both results without opening a write channel.

**The body rules** are a closed list (spec §22.15); in teaching terms:

- **Reads are free — even of mutable state.** Locals, parameters, fields through `this` or any borrow, statics, `typeof`, `is`, `nameof` — reads are not effects.
- **Frame-local mutation is fine.** Assign, compound-assign, `++`/`--` — provided the target's **root is a local or by-value parameter of this body** and the path from that root traverses only value-typed storage. The first class-typed dereference on the path leaves the frame and makes the store prohibited: mutating a record *local* is frame-private; storing through a field of `this` is not. The one exception is a `pure` **constructor**: its stores to `this`'s own fields are admitted — the birth-store rule — because the instance is not yet reachable from any holder (spec §22.15).
- **Building values is fine**: primitives, enums, strings (concatenation and interpolation included), records, choices whose classification is not owning, ranges. Defining lambdas is fine — the body runs later, at the caller's discipline. But *invoking* a function value is prohibited, because its type carries no effect information.
- **All control flow is admitted**, `throw` included. `new E(args)` is admitted *only* as the immediate operand of a `throw`, with a `pure` constructor on a trivial-dispose class — the built-in exception types' `(message)` constructors qualify (spec §31.4) — so the validation idiom stays writable in the pure fragment.
- **Calls resolve to `pure` declarations only.** This single rule fences off all I/O, tasks, channels, and synchronization — none of those entry points is or can be pure-declared.

The **`for`-loop rule** draws the fragment's line at enumerator construction, and the common loops fall on the admitted side: a `for` over a primitive `Range` is the counted loop; `for` loops over inline arrays, spans, `ReadOnlyStringSpan`, and value-typed `OwningBuffer<T>` are already direct indexed loops; and a `for` over a source **concretely typed** `Array<T>`, `OwningArray<T>`, class-typed `OwningBuffer<T>`, or `string` takes the direct-indexed elision as its normative pure-body semantics (spec §22.15 rule 5) — a bounds read and per-element reads, no enumerator built. So the everyday loop compiles in a pure body as written:

```bezel
public static pure method CountOf(text: string, needle: char): int
  var n := 0;                       // frame-local mutation: root is a local
  for var c in text do              // concrete string source — direct-indexed,
    if c = needle then              //   admitted; no enumerator is built
      n := n + 1;
    end;
  end;
  return n;
end CountOf;
```

What stays rejected is a source the checker cannot see concretely: an **interface-typed** source (`IEnumerable<T>` included) or a **generic type parameter** source — even one every caller instantiates at a concrete array type — because there the desugaring would construct an enumerator class instance, and pure-body checking never depends on the substitution (spec §22.15). For those sources the remedy is getting the source to a concrete sequence type: take the parameter as `Array<T>`, or bind the value at a concrete type before the loop. An index loop is *not* the workaround here — inside a pure body it hits the same wall (`IEnumerable<T>` has no `Count` or indexer, and an interface indexer read resolves to a declaration that is not `pure`-declared, so the element access is rejected too; spec §22.15 rule 5).

And the two traps the checker will catch for you, both compile errors with the operation named:

```bezel
public pure method Describe(): string
  Console.WriteLine("describing…");   // ERROR: callee is not pure
  _hits := _hits + 1;                 // ERROR: store whose root is a field of this
  return $"{_count} items";           // fine: reads and string building
end Describe;
```

The remedies are standard: hoist the effect to the (impure) caller, or drop the `pure` keyword — visibly, as a signature change — if the member genuinely needs the effect.

---

## 7. What `pure` buys

The immediate payoff is **contract material**: a clause expression may call only pure members (§1), so every predicate that is `pure` — marked, or pure by classification, as a stored-slot auto-property getter is (a marker there is the redundant spelling §22.15 rejects) — becomes usable in `requires`, `ensures`, and `invariant` — including on other members of the same type:

```bezel
public pure method Accepts(state: int): bool
  return state >= 0 and state < _stateCount;
end Accepts;

public method Transition(state: int)
  requires(Accepts(state))

  _current := state;
end Transition;
```

Purity buys more beyond clause admission. Invariant evaluation runs as interior code, so an invariant clause may call a `pure` public method of the same instance without recursing into its own checks (spec §22.16). A `pure` getter is the recommended form, because getters are exempt from invariant instrumentation and purity closes the mutation loophole (§4). Purity travels in the artifact interface, so cross-module callers — and their clauses — can rely on it (spec §2.8.2). And purity is what makes the whole tier system coherent: evaluating, eliding, or proving an effect-free clause are indistinguishable for every non-violating program.

What it does **not** buy: pure calls are not compile-time evaluated — a pure call is not a constant expression — and, §5 again, `pure` is not determinism.

---

## 8. `Debug.Assert`: the landed assertion surface

`System.Diagnostics` requires an explicit `import System.Diagnostics;`. Its surface, unchanged from v0.247 through v0.270, is `Stopwatch` (stdlib §8.1) and the static `Debug` class (stdlib §8.2) — `Assert(condition)`, `Assert(condition, message)`, and `WriteLine(message)`. That is the whole in-body assertion vocabulary the library ships today. Anything richer is not landed, and this guide teaches only what exists.

```bezel
import System.Diagnostics;

public method Rebalance()
  // … move entries between _hot and _cold …
  Debug.Assert(_hot.Count + _cold.Count = _total, "rebalance lost entries");
end Rebalance;
```

`Debug.Assert` differs from a contract on every axis, and the differences are the choosing rule:

- **Elision**: `Debug.*` calls may be elided in release builds, depending on the compiler's `--debug` flag. Side effects in `Assert` arguments are therefore unspecified; keep the arguments effect-free. A `requires` is never elided (§1).
- **Failure**: a failed assertion reports the condition's source location and message and **terminates the process** — it never throws (stdlib §8.2; the failure-signal rule's band 5, *fatal*). A violated contract raises the catchable `ContractViolationException`, so a supervision boundary can log it and shed the request.
- **Placement**: a contract is a signature-level obligation, visible to callers and carried by the interface; `Debug.Assert` is a mid-body check on your own algorithm's intermediate state — a claim no caller could see or rely on.

So: obligations between caller and callee go in clauses; sanity checks inside an algorithm — states you believe unreachable, bookkeeping you want verified during development — go in `Debug.Assert`.

---

## 9. Choosing the right tool

This table classifies the *condition*; Guide 2 §7's table — the home for failure signals — classifies the *outcome shapes*. They meet at rows 1, 4, 5 and 7 — the last of which Guide 2 states in the same words.

| The condition is… | Reach for |
|---|---|
| Something a correct caller already guarantees (argument range, index bounds, protocol order) | `requires(...)` — always-on, faults `ContractViolationException` (spec §22.16; stdlib §1 band 1) |
| A guarantee your implementation makes back to its callers | `ensures(...)` — checked-contracts mode, or proven where the prover reaches (§3) |
| A consistency property of a **class** instance's state at its public boundary | `invariant(...);` — checked at construction exit and public entry/exit only (§4); the member is class-only (spec §22.16 *Scope*) |
| The same property on a **record** | a clause on the record's constructor — which makes the record constructor-only: not defaultable, no `with` (spec §9.9) — or, where that property is unwanted, the condition on a static factory method |
| An expected outcome at a trust boundary — absence, malformed input, a branchable failure | never a contract: `T?`, a Try shape, `Result<T, E>`, or a domain choice (Guide 2 §2–§3, §7) — or, when the failure is consumed at a boundary rather than branched on, a typed exception (stdlib §1 band 4; §2's `Listen` rewrite) |
| A boundary fault — I/O, environment, anything logged or retried rather than branched on | a typed exception (Guide 2 §4) |
| A mid-body sanity check on your own algorithm's intermediate state | `Debug.Assert` — maybe elided, terminates on failure, never throws (stdlib §8.2) |
| An invalid *argument* at a pre-contract boundary (interop, code without clauses) — a caller bug that no clause can state | `throw new ArgumentException(...)` — the residual idiom `requires` replaces where a contract is possible |

A few habits carry most of it. Before writing `requires`, ask whether a correct caller could always guarantee the condition — if not, it is an outcome, and Guide 2's shapes apply. Put the obligations callers must know in clauses, where the compiler carries and enforces them; keep `Debug.Assert` for interior claims. Mark every effect-free predicate and getter `pure` at declaration time — purity is the admission ticket to clause-land, and it cannot be retrofitted by inference. And when the purity checker or a contract violation pushes back, treat it as §1 treats it: not friction, but a misclassified condition or a real bug, found early.
