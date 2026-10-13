# Proposal 0172 — Function-typed slots keep their keying through implementations, containers and `scoped` fields; a body's bare-`T` lend stays out of `Object`

| | |
|---|---|
| **Status** | graduated-v0.335 — superseded — graduated v0.335 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues", the 2026-10-09 direction "resolve all findings autonomously" and the 2026-10-09 "Please proceed as you recommend" after the v0.334 matrix re-runs: four closures of one assumption proposal 0165 made — a method implementing or overriding a generic declaration reads a function-typed parameter that declaration spells over a type parameter at the declaring signature, in its body and at every call; *Exported mint slots*' carve-out is a parameter's own function type only; a `scoped` field is invoked by its declaring type's own bodies only; a function value a generic body forms with a type parameter at a return slot reaches no set-erasing position, no collection or inline-array slot and no variance (#5836, #5798); critique round 1 applied, shared with the #5835 ruling and proposal 0173, which closed the collection and inline-array face of the return-slot value rule, made the pass restriction conditional on the substitutions at which the declaring slot is a route, and corrected the declined visibility rule's ground (a nested type sees `private` members); landed with the v0.335 changelog entry) |
| **Tracking issue** | [#5836](https://github.com/s8lap/bezel-lang/issues/5836) (`spec-defect`: a function value handed to a parameter "exports nothing" only while the code that invokes it reads the pre-substitution slot — routes 1–3), [#5798](https://github.com/s8lap/bezel-lang/issues/5798) (`spec-defect`: a generic body's capture-free bare-`T` lend reaches a caller as a minting function through `Object`); one `spec-tracking` compiler tracker is filed at landing (§8), beside 0165's [#5829](https://github.com/s8lap/bezel-lang/issues/5829) |
| **Target module(s)** | **spec**: §28.3 (the announcement table's inheritance note and row-5 cell; *Function values a generic body forms over its type parameters*, two sentences; *Exported mint slots*, its first sentence; a new paragraph *Implementations and overrides read their function-typed parameters at the declaring signature*; the two-routes sentence of *Set-erasing positions*, shared with the #5835 ruling), §28.14 (the `scoped`-field sentence), §12.9 (one parenthetical), Appendix C.3 (one bullet). **rationale**: §4.2 (one dated paragraph). **code-generation reference**: §7.2 (one *Looks right, is rejected* block, shared with the #5835 ruling). **`DECLINED.md`**: six `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.334** at `fd8a9a2` (main). Section numbers are v0.334's. |
| **Origin** | The generics matrix's re-run against v0.334, roots NR05 (cells R2/C2, routes R2/C2·impl-slot, R6/C2·impl-slot, R6/C7·scoped-field; lanes L1 and L4) and NR06 (routes R8/C2·object-handout, R2/C2·object-handout, cell R8/C7; borrow-flow route R7–R8/C6·objmint; lanes L1, L4, L5, M3), consolidated in `scratch/matrix2/ROOTS2.md`. It needs a proposal because each closure narrows an admission proposal 0165 (v0.334) states in so many words, and one closure flips a test program. |

*Reference convention: unprefixed §-references are language-spec references at v0.334; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

Proposal 0165 bounded a type parameter to depth one in every type a generic declaration writes, and closed the hand-out of a function value a generic body forms with its `T` in a parameter slot by a value rule: such a value is "bound to a local, invoked, captured by a closure, or passed to a function-typed parameter, and reaches no other position", a `scoped` field excepted from its holder ban, "because a function-typed parameter that takes the value is call-bounded or storage-bounded by its own signature, so passing it exports nothing". It also carved a plain parameter's declared type out of *Exported mint slots*, "because the mint-laundering inhabitants are rejected inhabitant-side". Each ground assumes that whoever invokes the value reads its **pre-substitution** slot. Three invokers do not, and one hand-out was left open by 0165 §10:

1. **Dispatch.** `Keep<T>.Arm(s: ISink<T>)` passes `lambda(x) => do _last := x; end` to `ISink<V>.Take(f: (V) -> ())`. `class Feeder: ISink<() -> int>` sees `f: (() -> int) -> ()`, a concrete slot, and invokes it with a closure over its own `using var` — which the generic body stores. The mint twin: `ISink<V>.Put(f: () -> V)` at `ISink<IEnumerable<int>>`; `Drainer.Put` keys `f()` on `() -> IEnumerable<int>` and its `for` disposes the lent array.
2. **A container parameter.** `Box<T>.Export(sink: Array<(Box<T>) -> T>)` adds a capture-free lend; the caller invokes `fs[0](box)` at its own slot type `(Box<IEnumerable<int>>) -> IEnumerable<int>`, a mint, and the loop disposes the caller's array.
3. **A public `scoped` field.** `Hub<T>` registers `lambda(t: T) => Registry.Sink.Keep(t)` into `public scoped Handler: ((T) -> ())?`; a caller at `Hub<() -> int>` invokes `h.Handler?(g)` with a capture-bearing `g`, which the lambda stores.
4. **The untyped hand-out (#5798).** `Box<T>.Hand(): Object => lambda(b: Box<T>) => b.V` is capture-free and has `T` only at its return slot, so it is outside 0165's value rule and admitted at `Object`; the caller narrows it to `(Box<Task>) -> Task` (identity, §12.7) and `using var t := f(box)` adopts the task `box` still holds. The same at `Box<IEnumerable<int>>`, through a bare-`T` slot of another generic, and through §12.9's variance of `Array<(Box<T>) -> T>` to `IEnumerable<Object>`.

The decision closes each at the declaration that already carries the keying, adding no mechanism:

- **(A)** An implementing or overriding method reads a function-typed parameter the declaring signature spells over a type parameter **at the declaring signature**, in its body and at every call, direct or dispatched — the announcement table's inheritance rule and *Bare-type-parameter returns* clause 2, which already read an inherited bare-`T` *return* this way, extended to the member's function-typed parameters.
- **(B)** *Exported mint slots*' carve-out is a parameter's **own** function type only; a bare-`T` return slot inside a type argument of a parameter's declared type is an exported slot.
- **(C)** A `scoped` field is invoked by its declaring type's own bodies only, through `this`.
- **(D)** A function value a generic body forms or retypes at a type with a type parameter in a **return slot** reached through return slots only reaches no set-erasing position and no variant conversion — the mint twin of 0165's parameter-slot rule, restricted to the untyped routes, since the typed ones are *Exported mint slots*' per-instantiation business.

Measured reach: (A) zero sites, (B) zero, (C) one gtest program (`tests/codegen/lambda_test.cpp:413`, `codegen_cg60.function_typed_field_call`, which invokes `t._cb(10)` from `Main`; respelled through a method), (D) zero. Every clause is spec-ahead-of-impl, as 0165's are.

---

## 1. The problem

### 1.1 Dispatch manufactures the depth-two shape (#5836 route 1; generics R2/C2·impl-slot, R6/C2·impl-slot)

```bezel
class Item public method N(): int => 1; end Item;
interface ISink<V>
  method Take(f: (V) -> ());          // depth one over V
  method Put(f: () -> V);
end ISink;
class Keep<T>
  private _last: T?;
  public method Arm(s: ISink<T>)
    s.Take(lambda(x) => do _last := x; end);   // 0165: "passing it exports nothing"
  end Arm;
  public method Last(): T? => _last;
end Keep;
class Feeder: ISink<() -> int>
  public method Take(f: (() -> int) -> ())
    using var it := new Item();
    f(lambda() => it.N());            // a concrete slot: the call-bounded default admits the closure
  end Take;
  public method Put(f: () -> () -> int) end Put;
end Feeder;
// var k := new Keep<() -> int>(); k.Arm(new Feeder()); _ := k.Last()!();   // reads the disposed `it`
```

§28.3 *Set-erasing positions*: "The check is the caller's, over its own flow analysis and the callee's declared signature, enforced per instantiation category (§3.6 rule 2); the generic body is not re-checked". The induction that makes every `T`-typed value in `Keep<T>` capture-free enumerates the body's entries — its parameters, the results of invoking its function-typed parameters, the set-erasing positions. The lambda `Keep<T>.Arm` forms and passes away is invoked by `Feeder.Take`, whose slot is `(() -> int) -> ()`: no position on that list. The depth bound ("a type parameter occurs in a function type at depth one only") removed `reg: ((T) -> ()) -> ()` from every *written* type; dispatch through `ISink<T>` composes `(V) -> ()` with `V := T` and hands the composed shape to a body that types it concretely.

The mint twin, at an iterable head:

```bezel
class Drainer: ISink<IEnumerable<int>>
  public method Take(f: (IEnumerable<int>) -> ()) end Take;
  public method Put(f: () -> IEnumerable<int>)
    for var x in f() do end;          // keyed on the concrete slot: a mint; `using var __source` disposes it
  end Put;
end Drainer;
class Lend<T>
  private _xs: T?;
  public method Arm(s: ISink<T>) => s.Put(lambda() => _xs!);   // a lend at the pre-substitution bare slot
end Lend;
// over `using var xs: Array<int> := [1, 2, 3]; var l := new Lend<IEnumerable<int>>(); … l.Arm(new Drainer());` the loop disposes `xs`
```

*Calls mint* clause 7 keys a lambda's admission "against the target slot's declared, pre-substitution return type"; *Bare-type-parameter returns* clause 2 keys an implementation's own **return** through "the generic declaration's signature"; nothing keys an implementation's function-typed **parameter** that way. The `Task`-family exclusion names "a parameter's declared type" of the *generic* member and does not see `Drainer.Put`'s.

### 1.2 A container parameter (#5836 route 2; generics R2/C2)

```bezel
class Box<T>(v: T)
  public V: T := v;
  public method Export(sink: Array<(Box<T>) -> T>)   // "within a plain … parameter's declared type": carved out
    sink.Add(lambda(b: Box<T>) => b.V);              // a capture-free lend at the bare-T return slot
  end Export;
end Box;
static class Use
  public static method Run()
    using var xs: Array<int> := [1, 2, 3];
    var box := new Box<IEnumerable<int>>(xs);
    using var fs: Array<(Box<IEnumerable<int>>) -> IEnumerable<int>> := [];
    box.Export(fs);
    for var x in fs[0](box) do end;     // the caller's own slot type: a mint; the loop disposes xs
    xs.Add(4);                          // use after dispose
  end Run;
end Use;
```

*Exported mint slots* excludes "a function type whose return slot (nullability stripped) is a bare type parameter anywhere other than within a plain, `owned` or `scoped` parameter's declared type". The carve-out is right for the parameter's **own** function type (`f: () -> T`): every inhabitant of that slot is checked against the declaration (clause 7), and the body invokes it. It is wrong for a function type **inside a type argument** of the parameter: the body does not invoke those slots, it fills them, and the caller invokes them at its own type. At `Box<Task>` the parameter-side `Task`-family exclusion rejects the same member ("anywhere within a parameter's declared type"); the iterable family has no parameter-side exclusion, by design — its inhabitants are closed inhabitant-side — and the body's lend is no inhabitant of the caller's slot.

### 1.3 A public `scoped` field (#5836 route 3; generics route R6/C7·scoped-field)

```bezel
class Sink
  private _o: Object?;
  public method Keep(o: Object?) => _o := o;
end Sink;
static class Registry
  public static readonly Sink: Sink := new Sink();
end Registry;
class Hub<T>
  public scoped Handler: ((T) -> ())?;
  public method Register(scoped h: (T) -> ()) => Handler := h;
  public method Arm() => Register(lambda(t: T) => Registry.Sink.Keep(t));   // T to Object: "not re-checked"
end Hub;
static class Demo
  public static method Run(h: Hub<() -> int>)
    h.Arm();
    using var it := new Item();
    var g: () -> int := lambda() => it.N();
    h.Handler?(g);   // a plain slot, call-bounded: g admitted; the lambda stores it
  end Run;           // it disposed; Registry.Sink._o holds g
end Demo;
```

§28.14: "A `scoped` **field** holds such a callback: it may be invoked, overwritten, or cleared, but never read out as an escaping value" — with nothing about who invokes it. 0165's value rule admits the store ("not a function-typed holder other than a `scoped` field") and the pass ("passed to a function-typed parameter"), and its ground — "Declarations are not restricted … since every value of that type reaching it is either one of these values, which cannot [reach it], or a value checked where it was formed against the substituted type" — is exactly false for a `scoped` field: it is the one holder such a value *can* reach, and a public one is invocable from anywhere.

### 1.4 The untyped hand-out of a bare-`T` lend (#5798; generics R8/C2·object-handout, R2/C2·object-handout, R8/C7; borrow-flow R7–R8/C6·objmint)

```bezel
class Box<T>
  public V: T;
  public method Hand(): Object => lambda(b: Box<T>) => b.V;   // capture-free: passes the set-erasing check
end Box;
class Host
  public method Run(box: Box<Task>)
    var f := box.Hand() as (Box<Task>) -> Task;   // §12.7 tests the function type by identity: the formed type at Box<Task>
    if f <> null then
      using var t := f(box);                      // the function-value column keys a `Task` return as a mint: adopts box.V's task
    end;
  end Run;
end Host;
```

The value has `T` only at its return slot, so 0165's value rule (parameter slots) does not read it; it is capture-free, so the set-erasing check admits it at `Object`; `Hand`'s declared return is `Object`, so *Exported mint slots* does not see it. The same route runs at `Box<IEnumerable<int>>` (the caller iterates the result and the loop disposes its own array), through `new Keep<(Box<T>) -> T>(lambda(b) => b.V)` read back through `Keep<U>.Last(): U?` (a row-5 lend of a function value the caller then invokes as a mint), through `lambda() => s` over a `static readonly s: T`, and through §12.9's variance of a local `Array<(Box<T>) -> T>` to `IEnumerable<Object>`, which converts no value. At `Object`, at a class and at a user interface the narrowed type announces no mint, and the lend is harmless.

### 1.5 What the compiler does today

Source-read at `fd8a9a2`; no build was run. Proposal 0165 §8 records that none of its clauses is implemented (`grep -rn "depth one\|forms over its type parameters\|Exported mint" src/` finds nothing); the announcement table's inheritance rule for returns is implemented for bare-`T` returns (the row-5 classification is pre-substitution in `src/ownership/`), but no keying of an implementation's function-typed parameter exists, `Ownership_Checks.cpp`'s set-erasing checks key on the slot's substituted type, and the `scoped`-field invocation is lowered wherever it is written (`tests/codegen/lambda_test.cpp:413` invokes `t._cb(10)` from `Main` and passes). Every clause below is therefore spec-ahead-of-impl.

---

## 2. Options

| | Option | Route 1 (dispatch) | Route 2 (container parameter) | Route 3 (`scoped` field) | #5798 (untyped hand-out) | Text |
|---|---|---|---|---|---|---|
| **S** | Status quo | open | open | open | open | — |
| **G** | **The general keying**: every invocation *through* a declaration a generic type spells over a type parameter (a field, a property, a record component, an implementing parameter) reads the slots at the declaration | closed | open (the caller's own local slot is no declaration of the generic) | closed | open | one keying sentence, read at every member access of a function-typed declaration |
| **P** | **Per-route closures at the declaration that carries the keying — adopted**: (A) implementations and overrides read the parameter at the declaring signature; (B) the exported-mint carve-out is a parameter's own function type; (C) a `scoped` field is invoked by its declaring type's bodies only; (D) a return-slot value reaches no set-erasing position and no variance | closed (A) | closed (B) | closed (C) | closed (D) | one paragraph, three sentences |
| **P′** | P with (C) replaced by a visibility rule: a `scoped` field whose type places a type parameter of its declaring generic type in a parameter slot is `private` | as P | as P | closed | as P | as P, with a shape-keyed visibility rule |
| **V** | Narrow the value rule's pass instead of (A): a body-formed value is passed only to a function-typed parameter whose declaring signature binds no type parameter to the body's `T` | closed | — | — | — | rejects `_ev.Add(lambda(t: T) => …)` on an `EventSource<T>` field; misses the mint twin |
| **R** | For #5798, a caller-side rule: a function value narrowed from `Object` to a mint-announcing return type is invoked as a lend, or the narrowing is rejected | — | — | — | closed | keys the caller's invocation on provenance it cannot see |
| **I** | For route 2, an iterable per-substitution exclusion at every parameter slot, as the `Task` family has | — | closed | — | — | rejects `xs.Select(lambda(x) => x.Items)` at `U = IEnumerable<Item>` |

**P over G.** G is one principle but over-reaches: a plain `(T) -> ()` field or component is filled by values checked at the substituted type (0165's ground holds for every holder but a `scoped` field), so reading `h.OnChange?(g)` at `Hub<() -> int>` as set-erasing rejects a harmless call; it asks a caller invoking a value read from a container to see a declaration behind a value its own local holds, which route 2 shows it cannot; and it leaves route 2 and #5798 open. P keys each route where the declaration is in view. **P over P′.** P′ is unsound as stated: a nested class sees its enclosing class's `private` members when it holds an instance (§11.6: "A nested class has access to all members (including `private`) of its enclosing class when it holds an instance of the enclosing class"), and since the enclosing type parameters are not in scope in a nested type (§11.6, *Nested types of a generic type*) the instance it holds is a concrete instantiation — `Hub<T>.Poker` taking `h: Hub<() -> int>` invokes `h._handler?(g)` with a capture-bearing `g`. Repaired, P′ becomes P's who-invokes rule restricted to scoped fields whose type places a type parameter in a parameter slot: the same rule plus a shape condition, whose only gain is the one measured site — a non-generic `public scoped _cb: (int) -> int` invoked from `Main` (harmless, and respelled through a method). P states who *invokes* a `scoped` field once, for every `scoped` field, and matches the contract §28.14 already states (the bounding object stores what it bounds and never reads it out). (Critic note, v0.335: the lane's first draft declined P′ on simplicity grounds only; the nested-class route is the decisive ground.) **(A) over V.** V rejects the generic event idiom and does nothing for the mint twin, whose value has `T` only at a return slot. **(D) over R.** R breaks the announcement table's type-announced keying at the function-value column for a reason the caller cannot state. **(B) over I.** The parameter's own slot is sound as read (clause 7); only a slot inside a type argument is invoked at the caller's type.

---

## 3. The rule

### 3.1 (A) Implementations and overrides read their function-typed parameters at the declaring signature

New paragraph in §28.3, between *Exported mint slots* and *Concrete-slot function bodies*; the exact text is `X3.json`'s `X3-0172-28.3-impl-keying`. In substance:

▲ A method implementing or overriding a member of a generic type, or a generic method, whose **declaring** signature places a type parameter in a slot of a function-typed parameter (`Take(f: (V) -> ())`, `Put(f: () -> V)` in `interface ISink<V>`) reads that parameter, in its own body and at every call site, direct or dispatched, at the declaring member's pre-substitution type, as *Bare-type-parameter returns* clause 2 reads an inherited bare-type-parameter return. In the implementing body the parameter is one of the values *Function values a generic body forms over its type parameters* governs, with the declaring member's type parameters as the body's: bound to a local, invoked, captured (a lambda in the body included), or passed to a function-typed parameter whose own declaring signature places a type parameter at the same slots, and reaching no other position — at a substitution that makes the slot a route (a function type for a type parameter in a parameter slot, a mint-announcing type for one in a return slot; elsewhere the parameter is an ordinary function-typed parameter of the implementing body, and a private concrete helper may take it); an argument at a slot declared at a bare type parameter is a set-erasing position at a function-type or `Object` substitution; an invocation whose declared return slot is a bare type parameter is classified by *Bare-type-parameter returns* (row 5), never by the substituted type; at a call, a lambda or reference at the parameter is checked against the declaring slot (*Calls mint* clause 7, *Concrete-slot function bodies*). Where one method satisfies several obligations (§11.2), the slot is read at the obligation that declares it at a type parameter.

The announcement table's inheritance note gains one sentence pointing here, and row 5's cell one clause.

### 3.2 (B) *Exported mint slots*: the carve-out is a parameter's own function type

▲ "anywhere other than **as** a plain, `owned` or `scoped` parameter's **own declared function type**, that type's return slots at every depth included (`f: () -> T`, `f: () -> () -> T`, read at the declaration by every inhabitant, *Calls mint* clause 7) — a return type (`Make(): () -> T`), a field, a property, a record component, a choice payload, a `ref` or `out` parameter, or a **type argument** of any declared type, a parameter's included (`Array<() -> T>` as a field or a return; `sink: Array<(Box<T>) -> T>` as a plain parameter, whose slot the caller invokes at its own type …)"

### 3.3 (C) A `scoped` field is invoked by its declaring type's bodies only

§28.14's sentence becomes ▲ "A `scoped` **field** holds such a callback: it may be **invoked** — by its declaring type's own member bodies only, through `this` (`_cb(x)`, `this._cb(x)`); a derived class's bodies, a nested type's (which see the enclosing class's `private` members, §11.6) and every other body may not invoke it, through `this` or another reference, and a type that wants the invocation reachable exposes a method that performs it (the bounding object alone invokes what it bounds …) — **overwritten**, or **cleared**, but never read out as an escaping value". 0165's value rule names the ground where it excepts the `scoped` field from its holder ban.

### 3.4 (D) A return-slot value reaches no set-erasing position and no variance

Appended to *Function values a generic body forms over its type parameters*: ▲ "A function value the body forms or retypes at a function type that places one of its type parameters, bare or nullable, in a **return slot reached through return slots only** (`(Box<T>) -> T`, `() -> T`, `() -> () -> T`: `lambda(b: Box<T>) => b.V`, `lambda() => s` over a `static readonly s: T`, `Box<T>.Get` over `Get(): T`), which lends at that slot (*Calls mint* clause 7), likewise reaches **no set-erasing position** — not a conversion to `Object`, implicit or by an `as`, and not an argument or store at a position another generic declares at a bare type parameter which the body instantiates with such a function type (`xs.Add(lambda(b) => b.V)` at `xs: Array<(Box<T>) -> T>` included) — no collection slot or inline-array slot (a collection-literal element and an indexer store included: the collection, or the inline array's box, converts to `Object` with no function value converted), and is not varied away at a type argument, rejected in the body as the conversion is; its other positions — a `return`, a field, a property, a record component, a `ref` or `out` location, a `scoped` field — are the declared slots *Exported mint slots* governs at the two mint families." §12.9's parenthetical and Appendix C.3 follow.

### 3.5 The two-routes sentence

*Set-erasing positions*' "Two routes reach a set-erasing position with no conversion at a caller" becomes four: the method-reference formation bar; a body-formed value with `T` in a parameter slot *or in a return slot*; an implementation's function-typed parameter, read at the declaring signature; and a flow-narrowed carrier, read at its declared type (the #5835 ruling of this train, one edit shared with it).

---

## 4. Soundness

**(A).** The hazard needs a value a generic body formed over `T`, or a lend the body makes at a bare-`T` return slot, to be invoked by code that types the slot concretely. 0165 keeps every such value inside the forming body's frame except through "passed to a function-typed parameter", and in a generic body every recipient slot is spelled at a type parameter (the body's `T` converts to no concrete function type), so the only concrete invoker is a body reached by dispatch — an implementation or override whose own signature substitutes the declaring member's parameter. (A) makes that body read the parameter as the generic body would: its arguments at a bare slot are set-erasing (so a capture-bearing closure cannot enter the generic body), its invocation at a bare return slot is a lend (so the body's lend is never minted), and the parameter itself is passed only to a slot that keeps the keying (so no concrete third body invokes it). A store, return or escaping capture of the parameter was already rejected (it is a plain function-typed parameter, provenance clause 3). Call sites read the same slot, so a lambda at a direct call of the implementation takes the bare-`T` admission (a fresh frame rejected at its return path, a lend admitted) — one keying per member however it is reached, the inheritance rule's own posture. At a value or class substitution the reading is vacuous or agrees with the substituted one (rows 4 and 5 coincide), and the pass restriction does not bind there: a concrete helper's invocations at such a type hand the generic body nothing it relies on (at `Object` every argument is capture-free by §28.3's induction; at a class the generic body's store is the field-store residual any concrete body has). The span twin needs nothing new: at `ISink<IReadOnlySpan<int>>` the generic body's store is §9.7's instance-field bullet, read per instantiation. (A) is body-local (proposal 0105): it reads the implementing member's own declaration chain and its own flow analysis.

**(B).** A slot inside a type argument of a parameter is never invoked by the body against the declaration; it is filled by the body and invoked by the caller at the caller's type, which is exactly the two-owner shape *Exported mint slots* already names for a return, a field or an `out` slot. Admissibility at the two mint families is the only sound closure the announcement table offers at a slot the caller keys on the substituted type. Every non-mint substitution is untouched. (Critic note: with (D)'s collection-slot clause the body can place no lend of its own in such a parameter, so for route 2's counterexample (B) is a second, per-instantiation closure; it is kept because it is the signature-level statement — a caller sees the rejection at the instantiation it names — and its reach is zero.)

**(C).** A `scoped` field is the one holder 0165's value rule lets a body-formed `(T) -> …` value reach, and its value is never read out, so its invocation is the only way a caller can feed it. Restricting the invocation to the declaring type's bodies means every argument it ever receives is one of that type's own `T`-typed values — capture-free by the set-erasing induction — or, in a non-generic type, a value the body itself checked. A derived class at a concrete instantiation (`class Sub: Hub<() -> int>`) types the field concretely and is "every other body". The rule changes nothing about storing, overwriting, clearing or reading out.

**(D).** The value lends at its pre-substitution bare return slot (clause 7); any invoker that types the slot as a mint-announcing type keys a mint. The typed routes (a `return`, a field, a component, a type argument of one) are *Exported mint slots*' per-instantiation exclusion; the untyped routes — `Object`, a bare-`T` slot of another generic, variance, and a collection or inline array that the body then converts to `Object` (`var xs: Array<(Box<T>) -> T> := [lambda(b) => b.V]; return xs;` from a member declared `Object`, which a caller narrows back to `Array<(Box<Task>) -> Task>` by the array's formed type and invokes as a mint) — are closed by the same positions 0165 closes for a parameter-slot value, which keeps the two rules one shape. (Critic note: the lane's first draft listed "a holder" among the admitted positions, which left the collection-literal and inline-array faces of this route open; *Exported mint slots* governs only declared member types, and `Array<E>`'s element is a bare type parameter, not a function type.) The rule is the value's, read off its formation, and adds no source to its set; at every non-mint substitution a narrowed invocation is a lend of a capture-free value and loses nothing. A method reference formed in the body at such a type (`Box<T>.Get`) is included; a `() -> T` thunk returned from the body stays the thunk idiom (fifteen sites, 0165 §7), governed at the mint families by *Exported mint slots*.

**Kernel.** (A) K2.4 (the type-announced keying, inherited); (B) K2.4 + K6.3 (as the paragraph it edits); (C) K2.4 (a `scoped` field announces no set outside its bounding object); (D) K2.3 + K2.4 (as 0165's clause 2). No new axiom.

---

## 5. Normative edits

The exact `old → new` set is `scratch/v0335/X3.json` (ids `X3-0172-*`, and the shared `X3-5835-0172-28.3-routes` and `X3-5835-0172-cg-7.2`): §28.3's inheritance note, row 5's cell, *Function values a generic body forms over its type parameters* (two edits), *Exported mint slots* (first sentence), the new *Implementations and overrides* paragraph, the two-routes sentence; §28.14's `scoped`-field sentence; §12.9's parenthetical; Appendix C.3 (one bullet); rationale §4.2 (one dated paragraph); the code-generation reference §7.2 (one block); `DECLINED.md` (six rows); `README.md` (this proposal's row).

---

## 6. Declined alternatives

| Alternative | Why declined | Reopening bar |
|---|---|---|
| The general keying (option G): every invocation through a declaration a generic type spells over a type parameter reads its slots at the declaration | Over-reaches plain `(T) -> …` holders, whose values were checked at the substituted type (0165's ground holds for every holder but a `scoped` field); asks a caller to see a declaration behind a value its own local holds; leaves the container parameter and the `Object` hand-out open | a measured program in which (A) and (C) leave a body-formed value invocable by a caller |
| Narrowing the value rule's pass to parameters whose declaring signature binds no type parameter to the body's `T` (option V; #5836's alternative to its option 1) | Rejects the generic event idiom (`_ev.Add(lambda(t: T) => …)` on an `EventSource<T>` field) and every `Cell<V>.Run(f: (V) -> ())` at `V = T`; misses the mint twin, whose value has `T` only at a return slot | — (none stated) |
| A visibility rule for `scoped` fields over a type parameter in a parameter slot (`private` only, or only in generic types) (option P′; #5836 option 3's second form) | Unsound as stated: a nested class sees the enclosing class's `private` members and holds a concrete instantiation (§11.6), so it invokes the field with a capture-bearing argument; repaired, it is (C) with a shape condition, gaining only the one measured, harmless site | a measured program that invokes another object's `scoped` field and cannot expose a method |
| Dropping the `scoped`-field exemption from the value rule (#5836 option 3's third form) | Rejects the generic registrar idiom (`Register(lambda(t: T) => …)` into the type's own `scoped` field) and closes nothing (C) leaves open | — (none stated) |
| A caller-side lend reading of a function value narrowed from `Object` to a mint-announcing return type, or a rejection of the narrowing (option R; #5798 reading 2) | Keys an invocation on provenance the caller cannot see: any `Object` may hold such a value, so every `o as () -> Task` would be rejected or every mint-typed invocation sometimes a lend — the type-announced keying broken at the function-value column | — (none stated) |
| An iterable-family per-substitution exclusion at every parameter slot, as the `Task` family has (option I) | Rejects `xs.Select(lambda(x) => x.Items)` at `U = IEnumerable<Item>`, which a parameter's own slot reads soundly (clause 7); the hazard is only in a type argument | — (none stated) |

Proposal 0165's rows (`DECLINED.md`, the polarity rule, the per-instantiation exclusion for exported `(T) -> …` slots, the declaration-site ban, the hand-out as a source, the nested-slot exclusion) stand; none is reopened.

---

## 7. Migration and reach

Measured at `fd8a9a2` over `tests/` (gtest C++ strings, lit, both conformance corpora) and `docs/` (every document's code), with `scratch/v0335/x3work/scan_impl.py`, `scan_obj.py` and `grep`:

- **(A), newly rejected:** zero sites. 120 generic members declare a function-typed parameter over their own type parameter (`scan_impl.py`); no class implements or overrides one at a function-type or mint-family substitution (the implementors found are at `int`, records and classes: `subclass_inherited_generic_method_char_enum_interpolation_5094`'s six `Box` derivations, the stdlib event cases' `EventSource<T>`). The stdlib's `IEvent<T>`/`IAsyncEvent<T>` are implemented by its own sources only.
- **(B), newly rejected at an instantiation:** zero sites. The one parameter with a bare-`T` return slot inside a type argument, `Cap<U>(l: Array<() -> U>, x: U)` (`tests/codegen/method_own_type_param_array_add_release_5216_test.cpp:182`), is instantiated at `() -> int`, classes and `int` — no mint family; it stays admitted.
- **(C), newly rejected:** one gtest program, zero documents — `tests/codegen/lambda_test.cpp:413` (`codegen_cg60.function_typed_field_call`), whose `Main` invokes `t._cb(10)` and `t._cb(5)` on another object's `public scoped _cb: (int) -> int`; the implementing commit respells the invocation through a method of `T` and keeps the observable (`25`). The other `scoped` fields in `tests/` (critic re-count: 33 `scoped` field declarations in `tests/` and `docs/`, every `<receiver>.<field>(` over their names with a receiver other than `this` found only at `lambda_test.cpp:420–421`; among them `lambda_own_param_nested_scoped_2342` ×2, `scoped_capture_2814`, `deferred_activation_capture_1818`, `generic_class_ctor_arg_lambda_3772` ×2 — the last two `private` and `(T) -> int`-typed) are stored, read out (rejected) or invoked by their own type; the rule corpus's `s28_14_0017` assigns `r._cb := cb` inside the declaring type (a store, not an invocation; unchanged); Guide 01's `Timer` and the reference's `_job` invoke their own field.
- **(D), newly rejected in a body:** zero sites. No lambda or method reference formed in a generic body is converted to `Object` or varied to `IEnumerable<Object>` (`scan_obj.py`: the only hits are this train's and 0133/0164/0165's own counterexamples in `docs/proposals/`). Re-measured by the critic for the collection and inline-array clause: every collection type spelled with a function-type element whose return slot is a type parameter in `tests/` and `docs/` (outside proposals and research) is `Array<() -> U>` in `codegen_5216.lambda_literal_captured_by_callee_wrapper_no_uaf` (`l.Add(w)` over `w := lambda () => x`, which already captures the plain parameter `x` and is rejected at that holder by the v0.334 sink rule, so it is no new flip) and the §26.4 extension-head tests (no body); zero new sites.
- **Newly admitted:** nothing.
- **Doc-only:** the inheritance note, row 5's cell, the two-routes sentence, §12.9, Appendix C.3.
- **Corpus:** no rule case states the carve-out, the `scoped`-field invocation or the hand-out (`manifest.tsv` searched for "scoped", "exported", "hands out", "implementing", "28.14"); no banked verdict changes. Coverage gaps to write after landing: `Feeder.Take` invoking `f` with a capture-bearing closure and passing `f` to a concrete `k: (() -> int) -> ()` (reject), `Drainer.Put`'s `for var x in f()` (accept, as a borrow) and `d.Put(lambda() => Nums())` (reject at the return path); `Export(sink: Array<(Box<T>) -> T>)` adding a body-formed lend (reject in the body, (D)) and, for (B), a member `Export(sink: Array<(Box<T>) -> T>)` whatever its body at `Box<IEnumerable<int>>` (reject wherever named) and `Box<Item>` (accept); `h.Handler?(g)` from outside (reject) and `Fire(x: T) => Handler?(x)` inside (accept); `Hand(): Object => lambda(b: Box<T>) => b.V` and `Array<(Box<T>) -> T>` to `IEnumerable<Object>` (reject in the body), `Make(): (Box<T>) -> T` at `Box<Item>` (accept).
- **Guides:** Guide 01's `scoped` paragraph invokes the field from its own class; no idiom changes; no pin moves.

---

## 8. Spec-ahead-of-impl

One `spec-tracking` issue at landing, beside 0165's #5829: (i) (A)'s keying of an implementing or overriding method's function-typed parameters — the set-erasing check at its arguments, row-5 classification of its invocations, the pass-only-to-a-keyed-slot restriction, and the pre-substitution slot at its call sites; (ii) (B) in the (unimplemented) exported-mint instantiation exclusion; (iii) (C) in the member-access check for a `scoped` field invocation outside the declaring type's bodies, with the `lambda_test.cpp:413` rewrite; (iv) (D) in the closure analysis beside 0165's clause 2 (`Ownership_Checks.cpp`'s set-erasing checks and `TypeResolution_Checks.cpp`'s §12.9 check).

---

## 9. Composition

- **X4 / #5802** schedules an editorial reduction of §28.3's *Set-erasing positions* paragraph; this proposal edits one sentence of it (the two-routes sentence, shared with the #5835 ruling). If X4 rewrites the paragraph, the coordinator carries the four routes into the rewrite. *(Composed at v0.335: X4's whole-paragraph edit applied first, keeping the `as` sentence and the routes sentence verbatim; the #5835 clause and this proposal's four routes then applied unchanged, so the rewritten paragraph carries both.)*
- **The #5835 ruling (this lane)** adds a narrowing sentence to §12.7, §9.7's borrow-erasing bullet and *Set-erasing positions*' `as` clause; the two-routes sentence and the reference's §7.2 block are shared edits (`X3-5835-0172-*`).
- **Proposal 0173 (this lane)** touches §27.7.1, §3.6, §8.3 and Appendix C.1 only; no shared anchor.
- **README / DECLINED** rows are inserted at the live-queue tail (after row 0132) and at the `declined-at-proposal` group's end, by sequential insertion with the other lanes (`scratch/v0334/comp_work/SEAMS.md`).

---

## 10. Questions filed at landing

1. **A function-typed property or record component of a generic interface implemented concretely.** `interface IHub<V> property Handler: ((V) -> ())?; end` at `IHub<() -> int>`: a holder, which no body-formed value reaches (0165's holder ban), so every value in it was checked at the substituted type and (A) is not needed there. Recorded as considered, not filed.
2. **The generics matrix.** Its next run should add ·impl-slot at the span row (R4/C2: decided by §9.7's per-instantiation reading of the generic body's store) and record R2/C2·object-handout, R8/C2·object-handout and R6/C7·scoped-field as decided by this proposal.
