# Proposal 0134 — `ref` parameters do not narrow

| | |
|---|---|
| **Status** | graduated-v0.320 — superseded — graduated v0.320 (owner-accepted 2026-10-08: option (ii) of #5495, a `ref` parameter is never flow-narrowed; critique round 1 applied, §11; landed with the v0.320 changelog entry; graduation notes, including Q6's reading, in §12) |
| **Tracking issue** | [#5495](https://github.com/s8lap/bezel-lang/issues/5495) (`spec-question`, `needs-owner-decision`; filed by the v0.317 landing) |
| **Target module(s)** | **spec** — §7.4 (the per-local tracking sentence; the *Narrowing applies to locals — and `readonly` fields — only* subsection: its lead sentence and one new paragraph; the v0.317 *Storage inside a narrowed nullable value* paragraph: its carrier and one closing sentence), §12.7 (the "simple variable" definition; one sentence after "Narrowing applies to the original variable"; the box-carrier list), §22.3 (one sentence on the `ref` bullet; one clarifying clause on the `out` bullet's hidden-reference licence, §3.5), Appendix C.1 (one row); **rationale** §6.1 (one paragraph); **code-generation reference** §2 row 17, §4.2's `ref`/`out` bullet, §7.8 (its note and a second example); `DECLINED.md` (three `rejected` rows); **no stdlib change** (the companion declares no `ref` parameter, §4.3); **compiler** (tracked at graduation: `narrowFromNullTest`, `narrowFromTypeTest`, `narrowOnAssignment`, `narrowFromOutPostcond` and the field-fact root admission all accept a `ParameterSymbol` whatever its `modifier`, and a lambda parameter's symbol carries only its *written* modifier, §7; the backend passes a pre-declared `out` recipient's own storage, §7.3) |
| **Written against** | spec **v0.319** (drafted on the working tree at `29277b9`; re-verified by critique round 1 against the landed v0.319 at `7882177`, header v0.319: every `old` below occurs exactly once there, and `src/` is unchanged between the two, so the §7 line numbers hold); stdlib companion v0.319; rationale v0.319 |
| **Origin** | The v0.317 landing (`40aa49a`) ruled #5478 (*Storage inside a narrowed nullable value*) and #5480 (type narrowing shares §7.4's invalidating forms) and, in writing them, found that neither §7.4 nor §22.3 says what a `ref` parameter's narrowing is worth while the caller's other aliases are live; it filed #5495 under *Filed as questions* with three options. The owner accepted option (ii) on 2026-10-08. This document turns that direction into exact text. |

*Reference convention: unprefixed §-references are language-spec references at v0.319 (working tree); rationale, stdlib and proposal references are marked. Quotations are verbatim. ▲ marks proposed text, landed nowhere. The programs below use only members the stdlib companion declares; none invents an API. Line numbers for `src/` are at `29277b9`.*

---

## 0. Thesis

§7.4's narrowing is sound for a local because the local's storage has one name, and every writer through that name is in the body being checked: a direct store, a `ref`/`out` argument, or a lambda that captured it. Each is an invalidating form. A `ref` parameter breaks the premise, not the forms. It aliases a location the caller chose, and the caller's other names for that location — a closure passed as another argument, a second `ref` argument, a field reachable through another argument — stay live for the whole call. A write through any of them reaches the parameter without passing any form §7.4 names, and two of the three need no call at all: `b := null` and `h.F := null` are plain stores inside the callee. A narrowed `ref` parameter is therefore a narrowed mutable field with a different spelling, and §7.4's *Narrowing applies to locals — and `readonly` fields — only* already refuses that.

This proposal says so: **a `ref` parameter is never flow-narrowed** — not by a null test, a type test, an assignment, `??=`, an early exit, or an `out`-recipient postcondition; a `readonly`-field path rooted at it is not trackable; it is read at its declared type at every position, exactly as a mutable field is. The remedy is the field remedy: copy to a local, narrow the copy, and write back with an ordinary store where the caller should see the change. Every other parameter keeps narrowing: a plain, `owned` or `scoped` parameter is the callee's own storage, and an `out` parameter denotes a callee-local slot that no caller alias reaches before copy-out (§22.3), which is also what lets §22.4's `when true` postcondition be checked inside the callee. The rule covers §12.7's type narrowing with the same sentence — `ref a: Animal` narrowed to `Dog` is the same hole, measured as a heap-buffer-overflow in #4851's caller-side instance — and it is a local, body-local rule with no kernel contact. Nothing in `tests/`, `tests/conformance/`, the docs' examples or the stdlib narrows a `ref` parameter today (§4), so the graduation flips no pinned test; the cost is that the straight-line `if a <> null then a.M(); end;` and the guard `if a = null then return; end;` over a `ref` parameter need the copy, the same cost the language already charges at a mutable field.

---

## 1. The problem

### 1.1 The textual root

Three passages are each true and together admit the hole.

- §7.4, *How narrowing is established*: "Narrowing is flow-sensitive: at each program point the compiler tracks, per local, whether its value is known non-null." The section never says whether "local" includes a parameter, and the v0.317 *Storage inside a narrowed nullable value* paragraph says "Over a local or parameter `n`", so parameters are in.
- §7.4, *How narrowing is invalidated*, closes with: "The compiler does not attempt deeper analysis of which methods could reassign which variables transitively; beyond the forms above, nothing invalidates a local's narrowing." The forms are: reassignment from a nullable-typed expression; the variable passed as `ref` or `out`; and, for a mutably captured variable, any operation that can transfer control to user code. All three are about writers *inside the body*.
- §22.3, *`ref` local and field targets alias; accessor targets bracket*: "A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately." The alias is symmetric — the caller's writes are visible through the callee's parameter immediately — and §22.3 does not forbid one location at two `ref` arguments (it defines `Swap(ref a[i], ref a[i])`).

The subsection *Narrowing applies to locals — and `readonly` fields — only* gives the reason narrowing stops at fields: "A field may be reassigned to `null` between a null check and a subsequent use by paths the compiler cannot see: a method call whose body … writes the field, a closure invoked in between, re-entrant code reached from the checked expression itself, or — across an `await` — another continuation". A `ref` parameter whose referent is a field *is* that field; a `ref` parameter whose referent is a captured local has the closure writer. The subsection's premise applies; its text does not reach parameters.

### 1.2 Four traced programs

Each program is accepted by the v0.319 text as written and by the compiler at `29277b9` (the narrowing helpers admit any `ParameterSymbol`, §7). Each dereferences null or stores into an absent value. The caller holds the object in an owned binding and passes a borrow, because §22.3 admits no owned state at a `ref` argument.

**P1 — a closure passed beside the `ref` argument.** The issue's lead case.

```bezel
module Repro;

public class Animal
  public constructor() end constructor;
  public method Speak(): string => "…";
end Animal;

public class Program
  public static method G(ref a: Animal?, f: () -> ()): string
    if a <> null then
      f();                      // §7.4: `a` is not reassigned, not passed ref/out,
      return a.Speak();         //   not captured in G — no form fires; `a` is null here
    end;
    return "none";
  end G;

  public static method Main()
    using var holder := new Animal();
    var m: Animal? := holder;
    Console.WriteLine(G(ref m, lambda() => do m := null; end));
  end Main;
end Program;
```

Trace: `a` aliases `m`. The test `a <> null` is true. `f()` runs the lambda, which stores `null` into `m`, which is `a`. `a.Speak()` dereferences null. In `Main`, `m` is mutably captured and §7.4 would drop *`m`'s* narrowing after the call — but `Main` never narrows `m`; the fact that fails is `G`'s, about `a`, and `G` contains no lambda.

**P2 — the same location at two `ref` arguments.** No call is needed.

```bezel
  public static method G2(ref a: Animal?, ref b: Animal?): string
    if a <> null then
      b := null;                // a plain store to a different parameter
      return a.Speak();         // `a` is `b`; null
    end;
    return "none";
  end G2;
  // caller:  G2(ref m, ref m);
```

§22.3 admits `ref m` twice (the accessor-bracket sentence defines the overlapping case for indexer slots and is silent for locals, which alias). `b := null` is a store to `b`; §7.4's first form invalidates *the variable reassigned*, which is `b`, not `a`.

**P3 — a field passed as `ref` beside its holder.** No call, and the store is a field store inside the callee.

```bezel
public class Holder
  public F: Animal?;
  public constructor() end constructor;
end Holder;

  public static method G3(ref a: Animal?, h: Holder): string
    if a <> null then
      h.F := null;              // a field store; `a` aliases `h.F`
      return a.Speak();         // null
    end;
    return "none";
  end G3;
  // caller:  using var h := new Holder(); h.F := holder; G3(ref h.F, h);
```

§22.3's category list admits "a writable field" at `ref`. §7.4 refuses to narrow `h.F` directly — the callee may not write `if h.F <> null then h.F.Speak()` — and the `ref` spelling reaches the same storage with a name the refusal does not cover.

**P4 — v0.317's store into a narrowed nullable value, over a `ref` parameter.**

```bezel
public record Pt
  public property X: int;
  public constructor(x: int) X := x; end constructor;
end Pt;

  public static method G4(ref n: Pt?, f: () -> ())
    if n <> null then
      f();                      // may run `p := null` in the caller
      n.X := 5;                 // v0.317: "requires `n` narrowed at the store" — it is,
    end;                        //   by the text; the storage does not exist
  end G4;
  // caller:  var p: Pt? := new Pt(1); G4(ref p, lambda() => do p := null; end);
```

v0.317's paragraph requires the narrowing "at the store" and lists what can invalidate it between the right operand and the store (a mutably captured `n`, a `ref n` in the right operand). `f()` is a statement before the store; it is an operation that can transfer control to user code, but `n` is not mutably captured *in `G4`*, so the third form does not fire. The store lands in a `Pt?` holding `null`, which the v0.317 ruling itself declined to define ("defining the write into an absent value's storage, which has no meaning in the language").

### 1.3 Why no invalidating form can be added instead

The owner's choice is between this proposal's rule and option (i), so the comparison is worth making exact. P1 is closed by invalidating at every user-code transfer — v0.316's §22.3 ruling used the same observation for the caller side. P2 and P3 are not: the write is a store in the callee's own straight-line code, to a *different* name. Closing them by invalidation means invalidating a `ref` parameter's narrowing at **every store whose target could alias it** — any store to another `ref`/`out` parameter, any field store on any reachable object, any indexer or property store, since any of them may be the referent. That is option (i)'s second clause, and it leaves a rule that narrows a `ref` parameter only in a block containing no call and no store to anything but locals. Option (iii) closes P2 by rejecting `G2(ref m, ref m)` at the call, needs (i)'s call clause for P1, and does not close P3 at all, since `ref h.F` beside `h` is one location named twice only after resolving what `h` is. Option (ii) removes the fact rather than chasing its writers.

### 1.4 The type-narrowing twin

§12.7's narrowing is the same mechanism with a type instead of a nullability bit, and since v0.317 (#5480) it shares §7.4's invalidating forms by reference. Inside `G5(ref a: Animal, f: () -> ())`, `if a is Dog then f(); a.Bark(); end;` reads a `Dog` member off an `Animal` the closure stored. #4851 measured the caller-side instance of this shape as an ASan heap-buffer-overflow; the callee-side instance is this one. Any rule that answers #5495 for §7.4 and not for §12.7 leaves it open.

---

## 2. Constraints and recorded decisions

Every passage the design has to compose with, with what it fixes.

| Passage | What it says | Consequence for the design |
|---|---|---|
| §7.4 *Narrowing applies to locals — and `readonly` fields — only* | "The compiler narrows local variables and `readonly` fields; it never narrows mutable fields or properties." The reason is unseen writers; the remedy is the copy. | The rule for `ref` parameters is an instance of this subsection's own principle and belongs in it, phrased with the same remedy. |
| §7.4 *How narrowing is established* | Tracks "per local"; lists the establishing forms (null tests, `and`/`or`/`not` composition, early exits, assignment and `??=`, the `out`-postcondition). | "local" must be made to say which parameters it covers. The rule is stated at the carrier, so every establishing form — and every consumer: `if`/`elif`/`while` conditions, `when` guards, the §20 conditional atom, §19.1's nullable-scrutinee reading, §7.11's operator gate, §18.5's hole rule — inherits it with no per-form text. |
| §7.4 `const` locals | "A `const` local's narrowing … is never invalidated". | Unaffected; a `const` local is not a parameter. |
| §7.4 `readonly` fields | "The tracked shape is exactly one field off an admitted root — `this._f`, `TypeName._s`, or `local._f` where `local` is itself narrowing-tracked; a fact rides its root". | A `ref` parameter is not tracked, so `a._f` establishes nothing. One clause says so; nothing else changes. |
| §7.4 *Storage inside a narrowed nullable value* (v0.317) | "Over a local or parameter `n` … requires `n` narrowed at the store". | The carrier becomes "a local or non-`ref` parameter"; one closing sentence gives the copy-out form for a `ref` parameter, which v0.317's own diagnostic already spells (`var p: Pt := n; Inc(ref p.X); n := p;`). |
| §12.7 | "When the test succeeds and the tested expression is a simple variable, the variable is flow-narrowed"; the box-carrier list is "§7.4's throughout — a local variable, or a `readonly` field". Since v0.317 the invalidating forms are §7.4's. | "simple variable" gets its definition; the carrier list names non-`ref` parameters. |
| §22.3 *A bare variable names its declared location* (v0.316, #4851) | A bare `ref a`/`out a` *argument* is typed at its declared type, because "A `ref` callee's alias is live for the whole call, and §7.4 drops a mutably captured variable's narrowing only after the call". | The same timing argument, read from the callee's side, is this proposal. The paragraph stays as it is; one sentence on the `ref` bullet points at §7.4. |
| §22.3 `out` | "within the callee the parameter denotes a callee-local slot, freely readable after assignment; on normal return the slot's value is copied to the caller's recipient"; "An implementation may pass a hidden reference, but a partial write must never be observable on the exception edge." | **Decided: `out` parameters narrow** (§2.1 below). |
| §22.4 *Nullability postcondition* | "on every path of the declaring body that returns the stated constant, the value assigned to the parameter must be provably non-null at the return (§7's narrowing rules apply inside the callee)". | Requires `out` parameters to narrow inside the callee; consistent with §2.1. |
| §22.3 category list | No owned state, no mutable static field or auto-property (§22.12 rule 2), no `using var`, no `readonly` field outside its context at `ref`/`out`. Accessor targets are copy-in/copy-out and "never aliases of each other or of anything else during the call". | The caller-side category is invisible to the callee's single check (0105's body-locality), so the rule cannot be conditional on it (§5.3). |
| §22.14 | "There are no ref returns." No `ref` local exists anywhere in the grammar (A.6 `LocalDecl` carries no modifier). | The only `ref` carriers are parameters: of methods, constructors and lambdas. Operator parameters admit no modifier (§25.2, corpus `s25_2_0006`), indexers forbid `ref`/`out` (§23.5), and property accessors declare no parameter list. A lambda's modifier may be written or inferred from its target type (§27.1 *Lambda parameter modifiers*). |
| §27.1 / §27.3, A.7 `LambdaParam ::= FuncParamMods? Identifier …`, `FuncParamMods ::= "ref" \| "scoped"` | A lambda may declare a `ref` parameter (`lambda(ref x: int) => do x := x + 1; end`). | The rule says "`ref` parameter" and reaches lambda parameters by the same word; §3's text names them so no reader has to infer it. |
| §27.5 | Captured variables are heap-allocated in a closure object; nothing is said about capturing a `ref`/`out` parameter of the enclosing method. The compiler rejects an *escaping* closure over one ("returned closure captures `w` which is a `ref` parameter", `Ownership_Checks.cpp:5348–5362`, #1217) and admits a non-escaping one. | A `ref` parameter mutably captured by a lambda in its own body is already covered by §7.4's third form — moot once the parameter never narrows. The spec's silence on the capture itself is recorded as open question Q4; the rule does not depend on how it is answered. |
| §22.10 primary constructors | "`ref` and `out` are rejected semantically" on a primary-constructor parameter list. | No primary-constructor parameter is a `ref` parameter; nothing to say. |
| §30.1 / §29.3 / §23.5 | `ref`/`out` forbidden on `async` methods, `async` lambdas, iterator methods and indexers. | The `await` writer route of §7.4's field paragraph cannot arise for a `ref` parameter; the three remaining routes (closure, second alias, field) are the ones §1.2 traces. |
| §28.14 `scoped` | A class- or interface-typed `scoped` parameter: "the callee may store the *borrow* in a direct non-owned field of the bounding object; the caller's argument is … frozen until the bound's scope exits". | The parameter itself is by-value callee storage; the freeze is on the caller's argument. A `scoped` parameter narrows like a plain one. |
| Rationale §6.1 | "An **uncaptured** local has none of these writers, so narrowing it is sound; a local mutably captured by a lambda has exactly one". | Gains the parameter case: a `ref` parameter has every writer the field has, through the alias. |
| Proposal 0105 (process §9 line) | Every compile-time rule states its body-locality class. | §6: local. |

### 2.1 `out` parameters: decided, with the reason

An `out` parameter narrows after its assignment today, and this proposal keeps that. The ground is §22.3's copy-out semantics: inside the callee the parameter "denotes a callee-local slot", and the caller's recipient receives the value "on normal return". No caller alias — closure, second argument, field — names the callee's slot during the call, because the slot is not the recipient. The three routes of §1.2 therefore do not exist for `out`, and §7.4's three forms are complete for it exactly as for a local. §22.4 relies on this: the `when true` postcondition is "discharged by this section's flow analysis" inside the declaring body, which is the narrowing of `p` after `p := …` on each `true` path. Making `out` parameters never narrow would break every `Try`-shaped body in the language for no soundness gain. **Declined with reasons:** treating `out` like `ref` (uniformity), because the uniform rule would be stricter than the semantics it polices and would cost §22.4's design its mechanism.

Two consequences follow and are recorded rather than left implicit. First, the decision rests on the *spec's* semantics; a compiler that passes the recipient's own address as the "hidden reference" has let a caller alias into the callee's slot and is non-conforming on §22.3's own terms, before this proposal — see §7.3 for the probe and the issue it files. Second, §7.4's uniform caller-side invalidation at an `out` argument ("even a non-nullable `out T` parameter … invalidates") is untouched; this proposal changes nothing on the caller's side of any call.

### 2.2 What is not reopened

- §22.3's bare-variable paragraph (#4851) stands; it is the caller's half of the same fact.
- v0.317's #5478 paragraph stands; its carrier is narrowed by one word and its diagnostic already spells the copy-out.
- §12.7's `is`-does-not-bind decline (rationale §11.6) stands; the remedy this proposal gives for the type case — `var d := a as Dog; if d <> null then … end;` — is the spelling §12.7 already teaches.

---

## 3. The rule, as exact text

Every edit is `old → new` against the working tree. ▲ marks the new text.

### 3.1 §7.4 — the tracking sentence

*old:* "Narrowing is flow-sensitive: at each program point the compiler tracks, per local, whether its value is known non-null."

*▲ new:* "Narrowing is flow-sensitive: at each program point the compiler tracks, per local variable and per parameter other than a `ref` parameter (*Narrowing applies to locals — and `readonly` fields — only*, below), whether its value is known non-null."

### 3.2 §7.4 — *Storage inside a narrowed nullable value*

*old:* "Over a local or parameter `n` of nullable value type narrowed non-null (`n: Pt?` for a record `Pt`), …"

*▲ new:* "Over a local or non-`ref` parameter `n` of nullable value type narrowed non-null (`n: Pt?` for a record `Pt`), …"

*▲ appended to the paragraph's end:* "A `ref` parameter of nullable value type is never narrowed (below), so no chain from it names storage inside its value: copy to a local, narrow the copy, store into the copy, and write the whole value back — `var c := n; if c <> null then var p: Pt := c; p.X := 5; n := p; end;` — the store `n := p` being an ordinary store to the parameter, which lands in the caller's location whatever that location held meanwhile."

### 3.3 §7.4 — the subsection *Narrowing applies to locals — and `readonly` fields — only*

The heading and its anchor `sec-narrowing-applies-to-locals-only` are unchanged (both are cited from §12.7, §22.3, Appendix C and the code-generation reference).

*old (lead sentence):* "The compiler narrows local variables and `readonly` fields; it never narrows mutable fields or properties."

*▲ new:* "The compiler narrows local variables, parameters other than `ref` parameters, and `readonly` fields; it never narrows mutable fields, properties, or `ref` parameters."

*▲ new paragraph, placed after the **`readonly` fields narrow** paragraph (the end of the subsection):*

> **`ref` parameters do not narrow.** A `ref` parameter ([§22.3](#sec-parameter-modifiers)) — of a method, constructor, or lambda ([§27.3](#sec-lambda-expressions)), its `ref` written or inferred from the target type ([§27.1](#sec-lambda-parameter-modifiers)) — aliases a location the caller chose, and the caller's other names for that location stay live for the whole call: among them a closure the caller passed as another argument, a second `ref` argument naming the same storage (`G(ref m, ref m)`), and a field the callee can reach through another argument, its receiver, or static state (`G(ref h.F, h)`, `h.G(ref h.F)`). A write through any of them is a write to the parameter that none of the invalidating forms above names, and two of the three need no call: `b := null` and `h.F := null` are plain stores in the callee's own straight-line code. The compiler therefore establishes **no** narrowing fact about a `ref` parameter. A null test, type test ([§12.7](#sec-type-tests-is-is-not-as)), assignment, `??=`, early exit, or `out`-recipient postcondition ([§22.4](#sec-out-nullability-postcondition)) on it narrows nothing — in an `if`, `elif`, `while` or `when` condition, a conditional atom, or a `case` alike — and a `readonly`-field path rooted at it (`a._f`) is not trackable, since its root is not. A pattern binding over it is unaffected: `case a of var d: Dog => d.Bark(), …` binds a fresh local ([§19.3](#sec-patterns)) holding the matched value, which no caller alias reaches, and `d` is typed and narrowed as any local is; `as` likewise. A `ref` parameter is read at its **declared** type at every position, exactly as a mutable field is: member access, indexing and calls on a nullable one, and §7.11's operators over a nullable value-typed one, are compile errors, and the remedies are `?.`, `??`, `!`, or the copy. The standard pattern is the field pattern: copy to a local and narrow the copy —
>
> ```bezel
> public static method Greet(ref a: Animal?, f: () -> ()): string
>   var c := a;                 // a borrow of the same object; `c` has one name
>   if c <> null then
>     f();                      // may store null through the caller's alias of `a`
>     return c.Speak();         // `c` still holds the object; narrowing is sound
>   end;
>   return "none";
> end Greet;
> ```
>
> — writing back with an ordinary store, `a := c;`, where the caller should observe the change; the store lands in the caller's location whatever that location held meanwhile, as any store through an alias does. The diagnostic at a rejected use names this remedy. Every other parameter kind is the callee's own storage and narrows under the forms above: a plain, `owned`, or `scoped` parameter holds the callee's copy of the argument, and an `out` parameter denotes a callee-local slot that no caller alias names before its copy-out on normal return ([§22.3](#sec-parameter-modifiers)) — which is what lets [§22.4](#sec-out-nullability-postcondition)'s postcondition be discharged inside the declaring body.

### 3.4 §12.7

*old:* "`is T` tests whether a value is an instance of `T` and produces `bool`. When the test succeeds and the tested expression is a simple variable, the variable is **flow-narrowed** to `T` within the controlled block:"

*▲ new:* "`is T` tests whether a value is an instance of `T` and produces `bool`. When the test succeeds and the tested expression is a simple variable — a local variable, or a parameter other than a `ref` parameter ([§7.4](#sec-narrowing-applies-to-locals-only)) — the variable is **flow-narrowed** to `T` within the controlled block:"

*old:* "Narrowing applies to the original variable. Reassignment within the controlled block invalidates the type narrowing, …"

*▲ new:* "Narrowing applies to the original variable. A `ref` parameter is never type-narrowed, for the reason [§7.4](#sec-narrowing-applies-to-locals-only) gives for its nullability: inside `if a is Dog then … end;` over `ref a: Animal`, `a` is an `Animal` throughout the block, and the value is reached through a copy (`var c := a; if c is Dog then c.Bark(); end;`) or bound with `as` (`var d := a as Dog;`). Reassignment within the controlled block invalidates the type narrowing, …"

*old (the box paragraph):* "The carriers are [§7.4](#sec-narrowing-applies-to-locals-only)'s throughout — a local variable, or a `readonly` field (instance or `static`) off an admitted root: …"

*▲ new:* "The carriers are [§7.4](#sec-narrowing-applies-to-locals-only)'s throughout — a local variable or non-`ref` parameter, or a `readonly` field (instance or `static`) off an admitted root: …"

### 3.5 §22.3 — the `ref` bullet

*old:* "- `ref` — pass by reference. The caller writes `ref` at the call site. The variable must be initialized before the call."

*▲ new:* "- `ref` — pass by reference. The caller writes `ref` at the call site. The variable must be initialized before the call. Inside the callee the parameter is never flow-narrowed ([§7.4](#sec-narrowing-applies-to-locals-only)): its referent keeps the caller's other names for the whole call, so it is read at its declared type and narrowed through a local copy."

The *A bare variable names its declared location* paragraph and the *Storage inside* cross-reference are unchanged.

*(critique round 1, B2)* The `out` bullet's licence sentence, which §2.1's exemption rests on:

*old:* "An implementation may pass a hidden reference, but a partial write must never be observable on the exception edge."

*▲ new:* "An implementation may pass a hidden reference, but only to storage that is the callee's for the call: no write through the caller's names for the recipient — a closure, another argument — reaches the slot during the call, no write to the slot reaches the recipient before normal return, and a partial write is never observable on the exception edge."

This restates copy-out-on-normal-return; it changes no defined behavior and names what the licence may hide. A boundary `out` on an `extern` parameter is untouched: [§32.5](#sec-boundary-out-ref)'s direct address passing is its own normative rule, and a native callee has no Bezel body to narrow in.

### 3.6 Appendix C.1

*old:* "- Nullable field or property access — other than the narrowing `readonly`-field form — without copying to a local is rejected; local variables and `readonly` fields (one field off an admitted root) are narrowed ([§7.4](#sec-member-access-on-nullable-values))."

*▲ new:* "- Nullable field, property, or `ref`-parameter access — other than the narrowing `readonly`-field form — without copying to a local is rejected; local variables, parameters other than `ref` parameters, and `readonly` fields (one field off an admitted root) are narrowed, and a `ref` parameter is never narrowed, to non-null or to a type ([§7.4](#sec-narrowing-applies-to-locals-only), [§12.7](#sec-type-tests-is-is-not-as))."

### 3.7 Rationale §6.1

*▲ new paragraph after "…which is why spec §7.4 (v0.182) invalidates a captured local's narrowing at any call, function-value invocation, or `await` inside the controlled block.":*

> A `ref` parameter (v0.3NN, #5495) has every writer the field has, through the alias: the caller may pass a closure that nulls the referent beside the `ref` argument, name the same storage at a second `ref` argument, or pass the holder of a field it also passed as `ref`. The last two are plain stores inside the callee itself, so no invalidating form could be added short of invalidating at every store to anything that might alias — which is the mutable-field situation under another name. Spec §7.4 therefore never narrows a `ref` parameter and gives it the field remedy, the copy. An `out` parameter is a callee-local slot until copy-out (spec §22.3), which no caller alias names, so it narrows like a local; that is also what discharges §22.4's `when true` postcondition inside the declaring body.

### 3.8 Code-generation reference

- §2 row 17: *old* "Mutable fields and properties **never narrow**. Copy to a local first" → ▲ "Mutable fields, properties and **`ref` parameters never narrow** (an `out` parameter does, after assignment). Copy to a local first".
- §4.2's `ref`/`out` bullet (the line beginning "`ref`/`out` are **forbidden** on `async` methods"): ▲ append "Inside the callee a `ref` parameter **never narrows** — not by `<> null`, `is T`, assignment or an early exit — so `if a <> null then a.M(); end;` over `ref a: Animal?` is rejected: write `var c := a; if c <> null then c.M(); end;` and `a := c;` to write back (§7.4)."
- §7.8's note: *old* "narrowing applies to **locals and `readonly` fields only**" → ▲ "narrowing applies to **locals, parameters other than `ref` parameters, and `readonly` fields only**" (critique round 1, N4).
- §7.8: ▲ a second example block after the field one:

  ```bezel
  public static method Greet(ref a: Animal?): string
    if a <> null then return a.Speak(); end;   // ERROR: a `ref` parameter never narrows
    return "none";
  end Greet;
  ```

  with the note "> §7.4: a `ref` parameter aliases caller storage the caller can still write during the call (a passed closure, a second `ref` argument, a field passed beside its holder). Copy to a local — `var c := a;` — and narrow the copy. `out` parameters are callee-local slots and narrow normally."
- The pin moves to the landing revision.

### 3.9 The diagnostic

At a rejected member access, index, call, or §7.11 operator whose operand is a nullable `ref` parameter, and at a member of the tested type on a type-tested `ref` parameter, the diagnostic names the rule and the remedy: "`a` is a `ref` parameter and is never narrowed (§7.4): its caller-side aliases can write it during the call — copy to a local and narrow the copy (`var c := a; if c <> null then … end;`; write back with `a := c;`)". The existing field diagnostic ("cannot narrow `T?` accessed through a field … copy to a local first (§7.4)", `ExprTypecheck_Calls.cpp:21725–21729`) is the model.

### 3.10 What the rule does not say

It does not change what a `ref` parameter *is* at any other rule: it is still an assignable location (§17.6), still admitted at a nested `ref`/`out` argument (§22.3), still an ordinary read at its declared type, still a borrow for §28.3. It does not change the caller's side of any call. It adds no derived type property (§3.6) and no new diagnostic class.

---

## 4. What the rule newly rejects, and what it flips

### 4.1 The shapes that become compile errors

Over `ref a: T?` (or `ref a: A` with `a is D`):

1. `if a <> null then a.M(); end;` — the straight-line form, which §7.4 admits for a mutably captured *local* ("no call *between* the test and the use") and refuses for a field. The `ref` parameter takes the field's answer: the two store routes of §1.2 are straight-line stores, so "nothing between" cannot be checked without the alias analysis the language declines.
2. `if a = null then return; end; a.M();` — the guard idiom; the surviving path is not narrowed.
3. `while a <> null do … a := a.Next; end;` — the loop condition narrows nothing.
4. `a := new T(…); a.M();` and `a ??= …; a.M();` — assignment establishes nothing (and a fresh `new` into a `ref` location is already an ownership error, §28.3).
5. `if TryGet(out a) then a.M(); end;` — the `out`-recipient postcondition narrows a local or non-`ref` parameter recipient only.
6. `if a <> null then n := a + 1; end;` over `ref a: int?` — §7.11 reads the static type, which stays `int?`.
7. `if a is Dog then a.Bark(); end;` — §12.7.
8. `if a._f <> null then a._f.M(); end;` over a `readonly _f` — the root is not tracked.
9. `n.X := 5` under `if n <> null then` over `ref n: Pt?` — §3.2.

Each has the same remedy: `var c := a;` (a borrow alias of the same object, or a value copy for a value type), narrow `c`, write back with `a := c;` if the caller should see a change. `a!` and `a?.M()` remain available as before.

### 4.2 The sweep of `tests/`, `tests/conformance/`, the docs, and the stdlib

Method: every declaration of a `ref` parameter was listed (`grep -rnE '\bref [a-zA-Z_]+ *: *[A-Za-z_<>\[\]]+\??' tests/ docs/ --include=*.cpp --include=*.bzl --include=*.md`; `grep -nE '(\(|, )ref [a-zA-Z_]+: ' docs/*.md`), and every callee body with a `ref` parameter of nullable, class, interface, `Object`, or type-parameter type was read for a null test, type test, early exit, assignment-then-use, or `out`-recipient use of that parameter. The `int`-typed majority (222 of ~330 declarations) cannot narrow by nullability and were excluded; none tests `is` on an `int`.

| Where | `ref` parameter bodies found | Narrowing of the parameter | Verdict |
|---|---|---|---|
| `tests/sema/narrowed_bare_ref_arg_4806_test.cpp` | `G(ref x: Object)`, `G(ref x: Dog, f)`, `G(ref x: Cat)`, `G(ref x: Dog?)` (`return 0;`), `constructor(ref x: Object)` | none (bodies return constants or read `x` at its declared type) | no flip |
| `tests/sema/ref_out_operand_category_1682_test.cpp`, `tests/lit/ref_out_owned_local_1682.bzl` | `Swallow(ref x: Box) x := new Box();` | none | no flip |
| `tests/lit/sema/sema_null_narrow__ref_arg_invalidates_null_narrowing_1188.bzl` | `Clear(ref u: User?) u := null;` | none (the narrowing under test is the caller's local) | no flip |
| `tests/conformance/rules/cases/s7_4_0008.bzl` | `Reset(ref w: Widget?) => w := null;` | none (the case pins the caller-side invalidation) | no flip; `rule:` header unchanged |
| `tests/conformance/rules/cases/s22_3_0013.bzl`, `s25_2_0006.bzl`, `s32_13_0002.bzl`, `s32_5_0014.bzl`; `tests/conformance/cases/spec_0061.bzl`, `spec_0305.bzl` | `Replace(ref w: Widget)`, `operator +(ref a: Weight, b)`, extern `ref item`/`ref act`/`ref iptr`, `Swap<T>(ref a: T, ref b: T)` | none (extern declarations have no body) | no flip |
| generic bodies: `tests/lit/sema/generic_ref_out_exact_unification_1686__*.bzl`, `generic_concrete_refout_2266__*.bzl`, `tests/codegen/ref_out_generic_expr_method_2588_test.cpp`, `nested_generic_ref_store_caller_t_4982_test.cpp`, `generic_ctor_string_field_retain_4965_test.cpp`, `tests/module/override_omitted_where_chain_5075_test.cpp`, `tests/sema/out_param_expr_body_da_2595_test.cpp` | `Swap<T>`, `SwapR<T>`, `StoreW<T>(value, ref slot: T)`, `Both<T>(ref g: T, ref c: int)`, `Read(ref x: K): K => x`, `Put<U>(ref x: U, v: U)`, `EqV5075G<T>(a: T, ref b: T)` | none (swaps, stores, direct reads) | no flip |
| `tests/ownership/plain_borrow_param_closure_escape_4709_test.cpp`, `escape_deeper_test.cpp`, `tests/sema/definite_assignment_test.cpp`, `readonly_derived_ctor_reassignment_4276_test.cpp`, `tests/codegen/ref_out_accessor_writeback_1302_test.cpp`, `ffi_boundary_out_ref_by_address_1706_test.cpp` | `Make(ref w: Widget): () -> ()` (returns a capturing lambda; rejected by #1217), `Leak(ref o: R) o := r;`, `Inc(ref v: Object) v := v;`, `Touch(ref b: Box)`, `Shift(ref p: Point)`, `Swallow(ref c: Cell)` | none | no flip |
| nullable-`ref` callees added by critique round 1 (N6) | `tests/lit/coalesce_assign_narrow_reject_2104.bzl` `Clobber(ref s: string?)`; `tests/sema/or_composition_false_facts_3877_test.cpp`, `narrowing_restore_intersection_2258_test.cpp` `Clear`/`ClearI(ref s: string?)`; `tests/codegen/nullable_widen_reassign_5005_test.cpp` `SetRef(ref x: long?, k)`; `virtual_property_nullable_set_2927_test.cpp` `BumpQ(ref n: int?)` (`n := (n ?? 0) + 5; return n ?? -1;`); `string_optional_residual_5253_test.cpp` `Peek(ref r: string?)`; `ref_param_write_string_param_5275_test.cpp` `W(ref r: string?, i)`; lit `readonly_field_single_assignment_2206__*`, `sema_null_narrow__ref_capture_in_lambda_*`, `sema_override__ref_value_type_param_widening_*` | none (stores, or reads through `??`); `readonly_field_narrowing_2116__*` roots its path at a plain parameter | no flip |
| spec examples | §14.1 `Swap<T>(ref a: T, ref b: T)` (`var temp := a; a := b; b := temp;`), §22.3's `ref x: Dog`/`ref x: Animal` *signatures*, §27.1's `method Foo(ref n: int)`, §32 `sigaction(…, ref act: SigAction, …)` | none | no doc example changes |
| guides 01–07, code-generation reference | no `ref` parameter declaration with a body (the reference's `lambda(ref x: int) => do x := x + 1; end` does not narrow) | — | no idiom changes; guide pins stay |
| stdlib companion v0.318 | **declares no `ref` parameter** (the only `ref` mentions are prose: span/`ref struct` comparisons, a `ReadGuard` note). `TryGetValue`, `TryPop`, `TryReceive` and the rest of stdlib §1's shape (a) are `out`-shaped and unaffected (§2.1). `Interlocked` and a `Swap` member are not stdlib members. | — | no stdlib change |

**Result: zero tests, lit cases, corpus cases, or doc examples flip.** The sweep is a claim about the repository at `29277b9`, re-run at graduation (process §5 item 3c).

### 4.3 What graduation adds rather than changes

- Rule-corpus cases (new, `tests/conformance/rules/cases/`): §7.4 — null test on a `ref` parameter rejected (`expect: reject`, diagnostic `ref` parameter … never narrowed); the copy remedy runs (`expect: run`); the guard form rejected; an `out` parameter narrowing after assignment runs (pins §2.1). §12.7 — `is` on a `ref a: Animal` rejected at `a.Bark()`; the `as` remedy runs. §7.4 *Storage inside* — `n.X := 5` over `ref n: Pt?` rejected; the copy-out form runs.
- gtest regressions (`tests/sema/ref_param_never_narrows_5495_test.cpp`, `runAot`): P1–P4 of §1.2 rejected with the §3.9 diagnostic; each remedy spelling compiles and runs; a lambda `ref` parameter (`lambda(ref x: Animal?) => do if x <> null then … end; end`) rejected, and the same with the modifier omitted and inferred from the target (`var g: (ref Animal?, () -> ()) -> string := lambda(a, f) => do if a <> null then f(); return a.Speak(); end; return "none"; end;`, critique round 1 B3); an `out` parameter and a `scoped` parameter still narrow. A pre-fix ASan leg of P1 with a reference-typed `Animal` (the issue's probe) documents the fail-without.
- No `DISABLED_`, no weakened oracle; the only test *text* that changes is additions.

---

## 5. Soundness argument

The claim: after this rule, no program accepted by the spec dereferences a `ref` parameter's referent as non-null, or as a `Dog`, when it is not — through the parameter.

### 5.1 The fact is never held

§7.4's consumers (member access, indexing, calls, §7.11's operators, §19.1's nullable-scrutinee reading, §18.5's in-hole expressions, §12.7's member reads at the narrowed type) each read either a tracked fact or the declared type. For a `ref` parameter no establishing form writes a fact (§3.1, §3.3, §3.4), and the `readonly`-field path rule requires a tracked root (§3.3). So every consumer reads the declared type, and a nullable or base-typed `ref` parameter reaches a non-nullable or derived-typed position only through `!` (a checked `NullReferenceException`, §7.8), `?.`/`?(…)`/`??` (which propagate absence), `as` (a nullable result), or a copy. Nothing is left for the three writer routes to invalidate.

### 5.2 The copy is sound

`var c := a;` reads `a` once at its declared type. For a reference type, `c` is a borrow of the object `a` referred to at that instant; §22.3 admits no owned state at a `ref` argument, so the referent location is a borrow location and the object's lifetime is governed by its owner, not by the caller's variable — a later `m := null` through the caller's alias drops a borrow, disposes nothing, and leaves `c`'s object alive for as long as it was going to be (the residual-trust back-reference clause of §28.3 is exactly the clause a plain borrow parameter already lives under). For a value type, `c` is a value copy (§3.2); this assumes a copyable referent, which §22.3's owned-state bound is read to guarantee (both move-only kinds carry ownership, §15.3; Q6 records that the bound's reach over an owning-choice local is not stated in so many words). `c` is a local with one name, so §7.4's three forms are complete for it. The write-back `a := c;` is an ordinary store through the alias; it is last-writer-wins against the caller's other aliases, which is the semantics every aliased store has and which nothing in §7.4 promises otherwise.

### 5.3 Why the rule cannot be narrower

Three refinements were considered and declined with reasons, so the "minimal" claim is a claim about what was weighed.

- *Narrow where the caller's argument cannot alias.* An accessor-mediated argument gives the callee a private slot (§22.3's bracket), and a plain local argument with no closure and no second `ref` argument has no other name. But the callee body is checked once at its declaration, against its signature (0105: "every body checked once at its declaration; cross-declaration checks only through per-body facts"), and the argument's category is a per-call-site fact the signature does not carry. A rule conditional on it would be an invariant amendment for one convenience.
- *Narrow, and invalidate at every store that could alias.* Option (i)'s second clause. It yields a rule that narrows only in a block with no call and no store to a field, indexer, property or other `ref`/`out` parameter, and it has to be re-derived every time a new store form is added. The owner declined it.
- *Keep narrowing, forbid the overlapping call shapes.* Option (iii). Does not close P3 (one location, two spellings, decidable only by alias analysis at the call) and needs (i) for P1. The owner declined it.

### 5.4 Composition with the recorded rules

- With §22.3's bare-variable paragraph (#4851): the caller passes `ref a` at `a`'s declared type; the callee reads `a` at its declared type. Both halves now say the same thing about the same timing.
- With v0.317's #5478 paragraph: the carrier restriction removes the `ref`-parameter instance of "requires `n` narrowed at the store" (P4) — the only instance its own ruling had flagged as unaccounted for ("would also have to account for a `ref`-parameter root").
- With #5480: §12.7 cites §7.4's forms; the forms are moot for a carrier that holds no fact, which is consistent.
- With §22.4: `out` parameters narrow inside the callee (§2.1); the postcondition's discharge is unchanged.
- With #1217: a `ref` parameter captured by a non-escaping lambda in its own body was a mutably-captured carrier under §7.4's third form; it is now a non-carrier. No program that was accepted becomes *more* accepted.

---

## 6. Kernel and body-locality classification

- **Kernel (§28 / §30.5):** no contact. Narrowing is a §7/§12 typing rule; it introduces no ownership state, no producer, no transfer, and no axiom. The copy remedy is an ordinary borrow alias or value copy already classified by §28.3.
- **Body-locality (0105):** **local**. The rule reads one fact — the carrier's own declaration: is it a parameter, and is its modifier `ref` — inside the body being checked. It consumes no per-body fact of any other declaration and adds none to the artifact interface (§2.8.2).

---

## 7. Implementation sketch

All paths under `src/sema/`, at `29277b9`. The compiler currently admits every `ParameterSymbol` as a narrowing carrier, so this is a spec-ahead-of-impl delta to be filed at graduation.

### 7.1 One predicate

`ParameterSymbol::modifier` (`Symbol.hpp:1111`, `"" | "ref" | "out" | "out owned" | "owned"`) is set for method parameters in `SymbolCollection.cpp:169` and for lambda parameters in `ExprTypecheck_Exprs.cpp:11543`. For a lambda that is the **written** modifier only (`ps->modifier = p.modifier;`; the lambda's own function type copies the same spelling at 12147), so a lambda whose `ref` is omitted and inferred from its target (§27.1 *Lambda parameter modifiers*) yields a symbol with an empty modifier — the route critique round 1 (B3) found. The graduation fix stamps the effective modifier on the symbol where the lambda is bound against a target function type (the target's `funcParams[pi].modifier` when `p.modifier` is empty), so a single helper then covers all three:

```cpp
// §7.4 "`ref` parameters do not narrow" (v0.3NN, #5495): a ref parameter is never a
// narrowing carrier — its referent keeps the caller's aliases for the whole call.
static bool isRefParam(const Symbol* s) {
    auto* ps = dynamic_cast<const ParameterSymbol*>(s);
    return ps && ps->modifier == "ref";
}
```

### 7.2 Where facts are written — gate each

| Site | Today | Change |
|---|---|---|
| `ExprTypecheck.cpp:14133–14158` `narrowFromNullTest` | admits `LocalSymbol` or `ParameterSymbol`, inserts into `nullNarrowed_` (14157) | `if (isRefParam(sym)) return false;` after the kind check |
| `ExprTypecheck.cpp:13079–13238` `narrowFromTypeTest` | same kind check at 13098–13100; writes `narrowings_[sym]` at 13237 | same gate |
| `ExprTypecheck.cpp:14002–14028` `narrowOnAssignment` | inserts into `nullNarrowed_`/`initNarrowed_` on a non-null value (14022–14023) | for a `ref` param skip the insert (the erase branch is harmless and may stay) |
| `ExprTypecheck.cpp:14404–14435` `narrowFromOutPostcond`, and `14451–` `narrowFromBuiltinShapeA` | recipient admitted if `LocalSymbol` or `ParameterSymbol` (14425–14429) | gate the recipient |
| `ExprTypecheck.cpp:14234–14296` `readonlyFieldPathOf` (the root admission at 14293 admits a `LocalSymbol` or any `ParameterSymbol`), consumed by `narrowFromFieldNullTest` (14310) and the member-access gate | a `local._f` root is admitted for any parameter | `if (isRefParam(rs)) return std::nullopt;` beside the kind check at 14293 |
| `ExprTypecheck.cpp:20802–20808` (the #4711 arm: an assignment's right operand reads through the *target's* own type narrowing, re-inserted for the RHS visit and erased after) | restores a pre-existing `narrowings_` entry | no change needed — a `ref` parameter never has an entry to restore; noted so the audit is complete |
| `ExprTypecheck.cpp:13302–13310` `isKnownNonNull` | consults the three sets, else the declared type | belt-and-braces: `if (isRefParam(sym)) return dt && !dt->nullable;` before the set lookups, so a fact that slipped in cannot be read |

Invalidation sites (`ExprTypecheck_Exprs.cpp:10538–10541`, `ExprTypecheck.cpp:16255–16290`, the join primitive at 14072) need no change; they only erase.

### 7.3 Diagnostics

- Member access gate, `ExprTypecheck_Calls.cpp:21621–21730` (sub-case (a), the simple-variable arm): when `tsym` is a `ref` parameter of nullable declared type, report the §3.9 text instead of the generic "requires narrowing" line. The index gate at `ExprTypecheck_Exprs.cpp:3500–3549` and the §7.11 operator gates (`ExprTypecheck_Exprs.cpp:6699`, `8010`, `8808`; `ExprTypecheck.cpp:21022`) already read `isNarrowedNonNullRead` → `isKnownNonNull` and will reject on their own; appending the `ref`-parameter hint there is a quality improvement, not a correctness need.
- `out`-slot probe (§2.1's second consequence, §3.5's clarified licence): `LlvmBackend_Calls2.cpp:2594–2600` and `2659–2668` document that the backend passes "a hidden reference" to a caller-side slot for `out var`, and `LlvmBackend_Setup.cpp:28393–28399` (#3304) says outright that a **pre-declared** `out n` recipient is passed as "a WRITE into the recipient's slot" — the recipient's own storage. *HYPOTHESIS (critique round 1; source reading, not yet run):* the shipped compiler therefore aliases a pre-declared recipient during the call. Probe, under `BEZEL_AOT_FORCE_SANITIZE=address`:

  ```bezel
  public static method G(out a: Animal?, src: Animal, f: () -> ()): string
    a := src;
    f();                       // nulls the caller's `m`
    return a.Speak();          // `a` narrowed by the assignment; the slot is the callee's
  end G;
  // caller:  using var holder := new Animal(); var m: Animal? := null;
  //          Console.WriteLine(G(out m, holder, lambda() => do m := null; end));
  ```

  A conforming implementation prints the `Speak()` result (the slot is the callee's; `m` then receives `holder` at copy-out). A null dereference is the §22.3 conformance delta, independent of this proposal's rule but a precondition for its `out` exemption being sound on the shipped compiler; it gets its own issue at graduation (Q2). The original sketch stored `new Animal()` into a plain `out` (an ownership error before it measured anything) and read `a <> null` at run time instead of a narrowed read; both are fixed above.

### 7.4 Tests

As §4.3 lists. The fail-without is mechanical: P1 compiles and runs at `29277b9` and dereferences null (reference-typed `Animal` under `BEZEL_AOT_FORCE_SANITIZE=address`); after the gate it is rejected at compile time with the §3.9 diagnostic.

---

## 8. Graduation edits (the landing's checklist)

Ruling-class landing of an accepted design (process §2, §6).

1. Spec: §3.1–§3.6 above (§3.5 now carries two edits: the `ref` bullet sentence and the `out` licence clause), as `old→new`, each `old` re-verified against HEAD at landing.
2. Rationale §6.1: §3.7.
3. Code-generation reference: §3.8, pin moved.
4. Changelog entry block (one shared entry for the revision): *"§7.4 / §12.7 / §22.3 / Appendix C.1, rationale §6.1 — a `ref` parameter is never flow-narrowed (design change, proposal 0134; #5495). … **Declined:** (i) invalidating a `ref` parameter's narrowing at every user-code transfer and every store that can reach heap storage or another `ref`/`out` parameter; (iii) rejecting overlapping caller storage at two `ref`/`out` arguments plus (i)'s call clause; treating `out` like `ref`. Kernel: no contact. Body-locality: local. **Migration:** newly rejected — a null test, type test, assignment, early exit or `out`-postcondition narrowing of a `ref` parameter (methods, constructors, lambdas — a lambda's `ref` written or inferred), and a chain store into a nullable value-typed `ref` parameter; remedy: copy to a local. Nothing newly defined or warned; §22.3's hidden-reference licence clarified (no behavior change). **Compiler (tracked):** admits every `ParameterSymbol` as a carrier, and a lambda's inferred `ref` is not on its symbol (#NNNN); a pre-declared `out` recipient passed by its own storage, if the §7.3 probe confirms (#NNNN)."*
5. `DECLINED.md`: two `rejected` rows — "narrowing a `ref` parameter with transfer-and-store invalidation (option i)" and "rejecting overlapping `ref`/`out` caller storage at one call (option iii)" — authoritative record this file §5.3 and the changelog entry; reopening bar: an alias analysis the language adopts for another reason. One `rejected` row for "`out` parameters never narrow" (§2.1).
6. Issues: the compiler delta (§7.2); the `out`-slot conformance probe's outcome if negative (§7.3); comment-and-close #5495 with the landed revision.
7. Rule corpus: the new cases of §4.3; re-run the §4.2 sweep at HEAD (`manifest.tsv` `rule:` grep for "narrow", "ref").
8. Guides: no idiom changes; pins stay (§4.2). The code-generation reference is the one companion that moves.
9. Flip this file to `graduated-v0.NNN — superseded`; update the README row.

---

## 9. Open questions

- **Q1 (owner): the diagnostic's shape.** §3.9 gives one message for every rejected position. Should the §7.11 operator rejections carry the `ref`-parameter hint too, or keep their existing "narrow first" text (the rule is the same either way)? *Recommendation:* add the hint; it is the only place a reader learns why narrowing failed.
- **Q2 (owner): the `out` decision's dependence on §22.3's slot semantics** (§2.1, §3.5, §7.3). Critique round 1 pinned the semantics in the text (§3.5's licence clause) and found source evidence that the backend passes a pre-declared recipient's own storage (§7.3, HYPOTHESIS until probed). The proposal stands on the spec either way; the question is whether the owner wants the implementation delta fixed before this rule lands or tracked beside it. *Recommendation:* tracked beside it, with its own issue; the delta is observable without narrowing (a caller closure reading `m` mid-call, and a partial write on the exception edge).
- **Q3 (editorial): the subsection heading.** "Narrowing applies to locals — and `readonly` fields — only" is now also the home of the `ref`-parameter rule and of "non-`ref` parameters narrow". The anchor is cited from four places; renaming the heading text while keeping the anchor is possible but adds nothing normative. *Recommendation:* keep the heading.
- **Q4 (spec gap, not decided here): capture of a `ref`/`out` parameter by a lambda.** §27.5 is silent; the compiler rejects an escaping closure over one (#1217) and admits a non-escaping one. This proposal is independent of the answer (a `ref` parameter is a non-carrier whether or not it is captured). It should be filed as its own question at graduation so the silence is on the record.
- **Q5 (sequencing):** proposal **0135** (*The value receiver is fixed at the call*, #5496) also edits §7.4 *Storage inside a narrowed nullable value*: its §8 item 5 inserts one sentence before "A chain that leaves `n`'s value …". This proposal edits the paragraph's carrier ("Over a local or parameter `n`") and appends at its end. The anchors are disjoint, so the edits compose in either order, and the content agrees: 0135's in-place predicate already classes a `ref` parameter as storage that may be written, and admits a `local._f` root only when the root is narrowing-tracked, which this rule makes exclude `ref` parameters. If both graduate in one revision, process §4's composition gate runs over the union. 0126, 0127 and 0133 do not touch §7.4 or §12.7. The edits touch passages v0.316/v0.317 settled and reopen none.
- **Q6 (settled by the existing text at graduation; impl delta filed): move-only referents at `ref`.** For locals and parameters the text settles it: §28.3's consume table defines an owned local-variable read as one where `x` is "a class-typed owned local, or a local of move-only value type per §15.3", §15.3 makes a plain parameter of move-only type identical to an `owned` one, and both move-only kinds take §28.7's drop-on-overwrite, the machinery §22.3's owned-state bound names as its reason. An owning-choice or owning-inline-array local, or such a parameter, is therefore "an owned local or `owned` parameter" under §22.3 and is not admitted at `ref`; §5.2's copyable-referent assumption holds for them, and no text changes. Not settled: a class's **instance field** of move-only type, which §28.5 bars from the `owned` modifier, so §22.3's list ("an `owned` field") does not name it in so many words; filed as its own question at graduation (§12). The compiler lags the parameter half: `checkRefOutOwnedOperand` (`src/ownership/Ownership_Checks.cpp`) rejects a parameter at `ref`/`out` only by its written `owned`/`out owned` modifier, so a plain move-only parameter is admitted today; that spec-ahead-of-impl delta is filed with the landing. The rule of this proposal does not depend on the answer.

---

## 10. Adversarial self-critique

Run against the draft above in the designer→critic→finalizer shape of process §4, by the author. Each finding is resolved in the text; this section records what was found and how. An independent critic has not yet run — the Status row says so.

**F1 — "The `out` decision is unsound on the shipped compiler."** The backend's own comments say it passes a hidden reference to a slot (`LlvmBackend_Calls2.cpp:2659–2668`), and for a pre-declared recipient that slot may be the recipient itself, in which case P1 with `out` instead of `ref` would be live today. *Resolution:* the design is a spec design and §22.3's callee-local slot is the normative semantics; the proposal now states the dependence explicitly (§2.1, second consequence), gives the measuring probe (§7.3), and files the outcome as a §22.3 conformance issue independent of narrowing. Treating `out` like `ref` was weighed and declined with its cost (§2.1). Status: resolved, with Q2 left for the owner's sequencing choice.

**F2 — "The straight-line form is sound and the rule over-rejects it."** `if a <> null then a.M(); end;` with nothing between has no writer. True for P1's route, false for P2 and P3: `b := null` and `h.F := null` *are* "nothing between" from a call-counting view, and the `ref` parameter cannot tell them apart from innocent stores. §7.4 refuses the same straight-line form at a mutable field for the same reason. *Resolution:* recorded as the deliberate cost in §4.1 item 1 and §5.3's second bullet; the owner's choice of (ii) over (i) is this trade.

**F3 — "The rule does not say whether a lambda's `ref` parameter is covered."** The word "parameter" covers it, but a reader of §7.4 would not think of A.7's `LambdaParam`. *Resolution:* §3.3's text names "a method, constructor, or lambda" (critique round 1 removed "operator" and "accessor", which admit no `ref` parameter, and added the inferred modifier); §4.3 adds a lambda test; §7.1 shows the symbol path is shared (`ExprTypecheck_Exprs.cpp:11543`) and where it misses an inferred modifier.

**F4 — "Zero flips is asserted, not shown."** Process §5 item 3b's bar. *Resolution:* §4.2 lists the search commands, every body found, and the verdict per body, so the claim is re-runnable; the `int`-typed exclusion is stated with its reason; the stdlib's absence of `ref` parameters is a grep result, not a recollection.

**F5 — "The write-back `a := c;` reintroduces the race."** It stores a value the callee computed over a location another alias may have written. *Resolution:* §3.3 and §5.2 say what this is — last-writer-wins through an alias, the semantics every aliased store has — and what it is not: a dereference of an absent value or a read at a wrong type. §7.4's promise is about reads through a narrowing; it never promised that a store through an alias wins.

**F6 — "`a._f` for a `readonly` `_f` is sound even off a `ref` root."** No: the fact rides its root (§7.4), and a caller alias can rebind the root to another object whose `_f` is null. *Resolution:* §3.3 says the path is not trackable; §7.2 gates the root admission.

**F7 — "§12.7's box-write rule needs a `ref` clause."** A `ref o: Object` type-narrowed to a value type would make `o[0] := 9` a box write. *Resolution:* none needed — a `ref` parameter is never type-narrowed, so it is never a box carrier; the carrier list in §3.4 is adjusted so the two sections agree.

**F8 — "The copy remedy may not type-check for a nullable value type."** `var c := n; if c <> null then var p: Pt := c; …` relies on assigning a narrowed `Pt?` to a `Pt`. §7.4's assignment paragraph and the compiler's conversion path (`ExprTypecheck.cpp:5031–5070`) both admit it. *Resolution:* §3.2 spells the remedy in that form; §4.3's corpus case runs it.

**F9 — "The rule changes the caller side."** It does not; §2.1's second consequence and §3.10 say so, and §4.2 shows `s7_4_0008` (the caller-side invalidation pin) is untouched.

**F10 — "A `scoped` class-typed parameter is frozen; is it a `ref`-like alias?"** No: the freeze is on the caller's *argument*; the parameter is by-value callee storage (§28.14). §2's table records it; §4.3 pins that it still narrows.

**F11 — "The proposal cites v0.319 but the header says v0.318."** Superseded: v0.319 has since landed (`7882177`, header v0.319), and critique round 1 re-verified every `old` against it (§11). *Resolution:* the *Written against* row says so; every `old` is re-verified again at landing per process §6.

**F12 — "Does `case a of … Dog …` narrow the scrutinee variable, leaving a route?"** §19.3's type pattern matches without narrowing the scrutinee variable (binding is through `var d: Dog`, a fresh borrow); §19.1's nullable-scrutinee reading consults §7.4's fact, which for a `ref` parameter is absent, so a `null` arm is required as for any un-narrowed nullable scrutinee. No route; §3.3 names `case` among the consumers so no reader has to re-derive it.

No blocking finding survives. Two findings (F1, F2) are recorded costs rather than fixes, with the owner's choice behind each.

---

## 11. Critique (round 1)

An independent critic (not the author) ran on 2026-10-08 against this file as committed in `69fb521` and the spec at `7882177` (v0.319). The full findings are in the run's scratch record; this section records each finding and where the text now resolves it. **The accepted direction is sound:** option (ii) closes all three writer routes by removing the fact instead of chasing the writers, and §12.7's twin is closed by the same sentence. No finding challenges the direction.

**Re-verification.** Every `old` in §3.1–§3.8 occurs exactly once at `7882177`; every anchor the new text links exists (`sec-lambda-parameter-modifiers` and `sec-boundary-out-ref` added by this round included); `src/` is unchanged between `29277b9` and `7882177`, so §7's line numbers hold (spot-checked at each cited site).

### Blocking (all resolved)

- **B1 — the carrier list named operators and accessors.** §25.2: "Operator parameters admit no parameter modifiers" (corpus `s25_2_0006`). Indexers forbid `ref`/`out` (§23.5) and property accessors declare no parameter list. *Resolved:* "of a method, constructor, or lambda" in §2's §22.14 row, §3.3, §8 item 4, F3 and the README row.
- **B2 — the `out` exemption rested on a reading §22.3 does not pin.** §22.3's "An implementation may pass a hidden reference, but a partial write must never be observable on the exception edge" constrains only the exception edge, and the backend reads it as licence to pass a pre-declared recipient's own storage (`LlvmBackend_Setup.cpp:28393–28399`, #3304). Since this proposal makes `out` the one by-reference modifier that narrows, the licence has to say what it may hide. *Resolved:* §3.5 adds the clarifying clause (no behavior change: it restates copy-out); §7.3's probe is rewritten to exercise a narrowed read and to avoid the ownership error the draft's `new`-into-`out` hit; Q2 and §8 item 4 track the delta.
- **B3 — an inferred lambda `ref` was an open route.** §27.1 *Lambda parameter modifiers* lets a lambda omit a modifier its target supplies, so `lambda(a, f) => …` against `(ref Animal?, () -> ()) -> string` has a `ref` parameter with no `ref` written; `ExprTypecheck_Exprs.cpp:11543` puts only the written modifier on the symbol. *Resolved:* §3.3 says "written or inferred from the target type"; §7.1 stamps the effective modifier; §4.3 adds the test.

### Non-blocking (resolved unless marked)

- **N1 — pattern binding.** §3.3 now says a `case` pattern binding (`var d: Dog`) and `as` bind a fresh local that is typed and narrowed as any local, so only the scrutinee `ref` parameter itself stays un-narrowed. (F12's conclusion holds; the text now states it.)
- **N2 — the route list read as closed.** The receiver (`h.G(ref h.F)` writing `this.F`) and static state are added, and the list is introduced with "among them". Soundness was unaffected; the rule is unconditional.
- **N3 — Q5 missed proposal 0135**, which also edits §7.4 *Storage inside*. Rewritten: disjoint anchors, consistent content, composition gate if both land together.
- **N4 — the code-generation reference §7.8 note** ("locals and `readonly` fields only") would go stale. §3.8 edits it.
- **N5 — DECLINED count:** the Target row now says three rows, matching §8 item 5.
- **N6 — the sweep table** omitted several nullable-`ref` callees. A re-run (every `ref` declaration in `tests/` and `docs/`, a 40-line window matched for `<> null`, `= null`, `is`, `case`, `??=`, `out x`, `x._f <> null`, `if`/`while`/`when x`, `x!`) finds no body that narrows its own `ref` parameter. **Zero flips confirmed.** A row lists the omitted callees.
- **N7 — §3.2's appended sentence** ("while it is the carrier") is reworded.
- **N8 — move-only referents.** §5.2 states the copyable-referent assumption, and Q6 records the gap in §22.3's owned-state bound. *Open, not this proposal's question.*
- **N9 — `const` copy.** §7.4 calls the field-copy idiom "airtight when the copy is `const`". The remedy examples could use `const c := a;` where no write-back follows. *Left to the landing's editor; no change.*

### Checked and found sound

`??=` on a `ref` parameter (a legal store that establishes nothing); `a ?? d`, `a?.M()` and `a!` (each reads once); a `readonly` path rooted at a `ref` parameter (the gate at 14293); `scoped` parameters (by-value callee storage; the freeze is on the caller's argument); plain value-typed parameters (a copy, or a by-value move for move-only types, §15.3); extension receivers (always borrows, with no `ref this` form, §26.7; value-kind `this` is 0135's question); `out` under the spec semantics against a closure, `G(out m, ref m)` and `G(out h.F, h)`; FFI-boundary `out` (no Bezel body); definite assignment (unchanged); composition with v0.316's bare-variable paragraph and v0.317's *Storage inside* (P4 is closed by the carrier edit). Kernel: no contact. Body-locality: **local**. After B3 the rule also reads the lambda's own target type, which is in the same body.

---

## 12. Graduation (v0.320)

The owner accepted option (ii) on 2026-10-08. Every `old` of §3 was re-verified against v0.319 (`7882177`) and occurs exactly once; every anchor the new text links resolves to the heading it names (§22.3, §27.3, §27.1 *Lambda parameter modifiers*, §12.7, §22.4 *Nullability postcondition on `out` parameters*, §19.3, §7.11, §32.5 *Boundary `out` and `ref`*). Differences from §3 as landed:

- §12.7's first sentence reads "a simple variable other than a `ref` parameter" instead of defining a simple variable as "a local variable, or a parameter other than a `ref` parameter": the box paragraph names a `readonly` field off an admitted root among the type carriers, and the definition would have contradicted it. Whether §12.7 type-narrows a `readonly` field at all (the compiler's `narrowFromTypeTest` admits only locals and parameters) is filed as its own question.
- §7.4 *Storage inside*'s closing remedy is `var c := n; if c <> null then c.X := 5; n := c; end;`: the copy is a local, so the paragraph's own rule makes `c.X` assignable and the intermediate `var p: Pt := c;` is unnecessary.
- The new paragraph spells the two no-call routes as signatures with their calls, since `b` and `h` are otherwise unbound in the spec, and names the `when` guard beside the conditions; its example and write-back sentence are laid out as a code block and a paragraph.
- §22.3's clarified licence names §32.5's boundary `out` as its own rule, so the clause cannot be read against direct address passing.
- The graduation critique (`scratch/v0320/g0134.critic.md`) made two wording changes: the new paragraph's §7.11 clause reads "over a nullable one" (§7.11 rejects nullable reference operands too, so "value-typed" under-stated the rule), and §22.3's licence clause reads "no write to the recipient's location during the call — through a closure, another argument, the receiver, or static state — reaches the slot" in place of "no write through the caller's names for the recipient — a closure, another argument —", so a write through the callee's own receiver (`h.G(out h.F)` with `this.F := …`) is plainly covered. Neither changes the rule.
- The rationale paragraph records the two declined options and the declined `out` uniformity, so the decline index's rows have a rationale record.
- Q6 is settled by the existing text for locals and parameters (above), with the compiler's plain-move-only-parameter delta filed; the instance-field half is filed as a question. Q4 (capture of a `ref`/`out` parameter, §27.5) and Q2's `out`-slot probe are filed as their own issues; Q1's diagnostic hint is the compiler issue's; Q3 keeps the heading.
- The rule corpus's `s7_4_0005` and `s12_7_0001` quote the two edited sentences; their `rule:` headers and manifest rows are refreshed (process §5 item 3c). Neither verdict changes.
- The §4.2 sweep was re-run at v0.319: no test, lit case, corpus case, doc example or stdlib member narrows a `ref` parameter. No test flips.
