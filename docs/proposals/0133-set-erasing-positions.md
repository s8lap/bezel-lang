# Proposal 0133 — Set-erasing positions: a capture-bearing function value may not be passed where its declared type is not a function type

| | |
|---|---|
| **Status** | graduated-v0.320 — superseded — graduated v0.320 (owner-accepted 2026-10-08 with §7's six recommendations; graduated in the v0.320 train with #5486's local half and #5499 under the composition gate; landed with the v0.320 changelog entry; graduation notes in §10) |
| **Tracking issue** | [#5375](https://github.com/s8lap/bezel-lang/issues/5375) (folds in #5376's function-type-substitution half, per the v0.316 ruling train) |
| **Target module(s)** | **spec** — §28.3 (*Borrow lifetime rules*, the closure bullet: one new clause, *set-erasing positions*; the announcement table's row 5 runtime-managed cell, one qualifying phrase; *Record components of function type*, the type-parameter sentence re-pointed; beside the `Task`-family exclusion, one admissibility paragraph for type parameters in nested parameter slots), §28.14 (the lead paragraph's "among the rest"; *The method-reference formation bar*, widened to set-erasing positions), §27.6 (one cross-reference), §28.1 (clause 3: one cross-reference), Appendix C (one invariant row); **rationale** §4.2 (a dated record); **code-generation reference** §4.4 (the closure bullet) and §7.2 (one row); `DECLINED.md` (three `rejected` rows, one `deferred` row annotated); **no stdlib change**; **compiler** (tracked at graduation) |
| **Written against** | spec **v0.316** (`cef9a1f`, October 2026); stdlib companion v0.316; rationale v0.316 |
| **Origin** | #5303's critique cut a candidate rule and filed #5375; the v0.316 store lane (`scratch/v0316/store.md`, `store.critic.md`) confirmed the question is design-change-sized under process §2 and sketched four repairs; the v0.316 `fnfield` lane keyed #5377 on the *declared* component type precisely so as not to decide this question by the back door. |

*Reference convention: unprefixed §-references are language-spec references at v0.316; K tags are §28.3's kernel ids; rationale and proposal references are marked. Quotations are verbatim. ▲ marks proposed text, landed nowhere. The programs below use only members the stdlib companion declares; none invents an API.*

---

## 0. Thesis

The closure bullet of §28.3 closes every *storage* sink to a function value whose source set is non-empty, on K2.4's ground: a function type announces no source set, so no holder outside the body can re-establish it. It does so by naming the sinks — a field, a collection, an `out`/`ref` location, the method's return, a property or indexer, a record component. It does **not** name the one position that erases the set *before* any sink is reached: a conversion of the function value to a static type that is not a function type. Two such conversions exist. One is to `Object` (§3.1: function types "reach `Object` directly through the inheritance chain"). The other is binding to a parameter, return slot or location whose **declared, pre-substitution type is a bare type parameter** and whose substitution is a function type — an identity conversion in the type system, but one that the generic body, checked once under §28.1 clause 3's class-or-interface assumption, reads as an ordinary object borrow and stores under the borrowed-field rule's K4.3 residual. After either conversion the value is, to every rule, a capture-free thing: a `T`-typed field read "carries the empty set", an `Object` is a class-typed borrow of whatever the caller passed.

This proposal adds one clause to the closure bullet: **a position whose declared type is not a function type is a set-erasing position, and a function value with a non-empty source set is rejected there.** The check is the caller's own, over its own flow analysis and the callee's declared signature — body-local, and a derivation from K2.3 and K2.4 with no new axiom. Two routes reach a set-erasing position without passing through any caller-side conversion (§1.6), so two companion rules close them on precedents the text already has. §28.14's *method-reference formation bar* is widened to a reference whose function-typed parameter slot binds to a set-erasing callee parameter. And beside the `Task`-family exclusion, a member whose parameter type carries a type parameter in a parameter slot of a parameter slot is inadmissible at a function-type substitution of it. Everything else stays as it is: the borrowed-field rule's residual for object borrows, §28.14's asymmetry, 0126 §2's recorded decline of a trust residual for function values, and the compiler's one-check-per-generic-body discipline. The cost is a short list of non-storing calls that become inexpressible with a capture-bearing argument — `Equals(Object?)`, `Console.WriteLine(Object)`, `EventSource<T>.Raise(payload)` at a function-type `T`, a user selector `Pick<T>(a: T, b: T): T`, and a received callback forwarded to a generic or `Object`-typed utility — weighed in §4 and found smaller than the issue's comment feared, because the collection-search members (`Remove`, `Contains`, `IndexOf`) answer *absent* for every capture-bearing argument once the hole is closed, and `Math.Min`/`Max` are not reachable at a function type at all (`where T: IComparable<T>`). One in-tree test flips (§5.4).

---

## 1. The problem

### 1.1 The textual root

Three passages of v0.316 each say something true, and together they admit a dangling store.

- §28.1 *Type requirement* clause 3: "The generic body type-checks under the class-or-interface assumption." A parameter `x: T` is, to the body's single check, a plain class-typed parameter.
- §28.3 *Borrow lifetime rules*, the field-store bullet, clause 2: "borrows sourced from a parameter that is **not `owned`** — a plain or `scoped` parameter — … are admissible", and clause 3: "The residual is a K4.3-recorded trust clause: whether the caller-side source actually outlives the object is the programmer's obligation". So `_v := x` with `x: T` is admitted in every generic body.
- §28.9: "**Function-typed values** — values of any function type … regardless of whether they capture" are runtime-managed, and §28.3 K1.6: such values "carry no classification and no obligation". The announcement table's row 5 reads the same way at the caller: "at value/runtime-managed substitutions, no borrow and no constraint".

The closure bullet meanwhile tracks a function value by its **source set** — "a lambda literal or method reference contributes the sources of the borrows it captures" — and says "a function value read from a field or a collection carries the empty set — those holders admit only capture-free function values, by the same sink rule". The two readings disagree about what a function value *is* at a `T` slot: the body's check sees an object borrow with a residual; the caller's check sees a function value whose set the parameter's plain signature cannot carry. Neither side rejects. The critic's five-reading check (`store.critic.md`, *#5375: is there a reading the current text already settles?*) confirms no current sentence closes it: §28.5's generic-field rule reaches only `owned` fields; §28.14's call bound attaches to parameters *declared* function-typed; the property/indexer bullet keys on an RHS whose static type *is* a function type; §3.6 rule 2 is a pattern rules opt into, not a general re-check; and at the caller `f` passed to `x: T` is an argument, not a store.

### 1.2 Program A — the bare-type-parameter route (the issue's repro, traced)

```bezel
module Repro;

class Item
  public method Value(): int => 7;
end Item;

class Holder<T>
  public _v: T;
  public constructor(x: T)
    _v := x;                                   // (1)
  end constructor;
end Holder;

class C
  public static method Make(): owned Holder<() -> int>
    using var item := new Item();              // (2)
    var f: () -> int := lambda() => item.Value();   // (3)
    return new Holder<() -> int>(f);           // (4)
  end Make;

  public static method Main(): int
    using var h := Make();
    return h._v();                             // (5)
  end Main;
end C;
```

Trace, against v0.316:

1. `_v := x` in `Holder<T>`'s constructor. Checked once under the class-or-interface assumption (§28.1 clause 3). `x` is a plain parameter; the field-store bullet's clause 2 admits a parameter-sourced borrow; clause 3 records the K4.3 residual ("whether the caller-side source actually outlives the object is the programmer's obligation"). **Admitted.**
2. `item` is a `using var` owned local, disposed at `Make`'s scope end (§28.10, K3.14).
3. `f` is a lambda capturing the borrowed read of `item`; its source set is `{item}` (the closure bullet's provenance clause). Binding it to a local in the same block is the local-scope bullet's admission — "an initializer is admitted by construction". **Admitted.**
4. `f` is passed as a plain argument to `x: T` at `T = () -> int`. The closure bullet admits "argument passing" as a consequence of the set rule; the only argument rule it states is for "a function-typed parameter" (call-bounded or `scoped`), and `x: T` is neither. The announcement table's row 1 classifies the `new` as an owned production; nothing reads `f`'s set. **Admitted.** The `owned` return of a fresh `new` is row 1's callee side. **Admitted.**
5. `h._v` is a field read of a function-typed field, which "carries the empty set"; invoking it runs `item.Value()` on an object disposed at the end of (2)'s scope. In the default checked build the call faults as `DisposedAccessException` (§28.2); in an unchecked build it is undefined behavior. **Admitted by every rule, and dangling.**

Nothing in the chain is the recorded residual: the residual's premise is that *the caller* can see and keep alive the source, and here the caller of `Holder`'s constructor is `Make`, whose source `item` cannot outlive `Make` — the exact scope-bound shape the closure bullet rejects at every *named* sink. The `scoped` remedy the field-store bullet names does not apply either: `public constructor(scoped x: T)` is not writable, since §28.14 defines `scoped` on function-typed and on class- or interface-typed parameters only (the issue's pin).

The same route reaches a collection: replace `_v: T` by `private owned _items: Array<T> := new Array<T>();` and `_v := x` by `_items.Add(x);` — §28.11's *Storing a borrow* admits a parameter-sourced element with the same residual, and `h._items[0]()` dangles identically. And it reaches a generic *method* on a non-generic class: `class Keep  public _o: Object?;  public method Put<T>(x: T) … end Keep;` is the next route's shape with `T` in place of `Object`.

### 1.3 Program B — the `Object` route

```bezel
class Keeper
  public _o: Object?;
  public method Keep(o: Object)
    _o := o;                                   // (1) plain-parameter-sourced borrow into a non-owned field: clause 3's residual
  end Keep;
end Keeper;

class C
  public static method Main(): int
    using var k := new Keeper();
    do
      using var item := new Item();
      var f: () -> int := lambda() => item.Value();
      k.Keep(f);                               // (2) `f` converts to `Object` (§3.1); no rule reads its set
    end;                                       // `item` disposed
    return (case k._o of var g: () -> int => g(), _ => 0);   // (3)
  end Main;
end C;
```

(1) is the borrowed-field residual over a declared `Object` parameter, which §9.7's declared-types paragraph confirms is "borrow-carrying … neither is a runtime-managed type". (2) is the implicit widening to `Object`; the closure bullet names no conversion among its sinks. (3) narrows back under §12.7 and §19's type patterns, which state no exclusion for a function-type target (the compiler does not yet lower the narrowing — `tests/codegen/method_own_type_param_escape_release_5127_test.cpp:38` records "#2434/#2491" — so the route is text-reachable and not yet runnable). The dangle is the same as Program A's.

`Object` is the **only** declared type other than a function type or a bare type parameter that a function value converts to: §3.1 lists function types among the reference types that reach `Object` "directly through the inheritance chain", and §14.2's conformance rule enumerates what conforms to an interface — "classes implementing it, records and enums implementing it, interfaces extending it, and primitives conforming through their standard-library declarations or the compiler-provided conformances of §11.8" — and names no function type. So `Object` and a substituted bare type parameter are the only two *types* at which the set is erased. They are not the only *sites*, though: §1.6 traces two routes on which the conversion to one of them happens inside a body no caller checks.

### 1.4 Program C — the return-slot route

The route also runs through a function-typed parameter whose **return slot** is declared at a bare type parameter:

```bezel
class Holder<T>
  public _v: T;
  public constructor(mk: () -> T)
    _v := mk();                                // (1)
  end constructor;
end Holder;

// in Make, with `item` and `f` as in Program A:
    return new Holder<() -> int>(lambda() => f);   // (2)
```

(1) is v0.316's #5376 ruling: "A generic body's `V := g()` with `g: () -> T` is checked the same way under the class-or-interface assumption, with the same residual at a class or interface substitution." Under the assumption, `mk()` is a parameter-sourced object borrow; admitted. (2) passes a capture-bearing lambda to a *function-typed* plain parameter, which §28.14's caller side admits without restriction ("**any** closure — capture-bearing or not"). The lambda's own return path returns `f`, a closure over a binding declared *outside* the lambda, which the closure bullet's lambda-return clause admits ("carries only sources the enclosing lambda's own value already carries"). At `T = () -> int` the stored value is `f`, and `h._v()` dangles as before. The #5376 ruling deliberately wrote nothing at this substitution and folded it here.

### 1.5 Why it matters beyond the three programs

Two load-bearing sentences become false while the hole is open:

- The closure bullet's "a function value read from a field or a collection carries the empty set — those holders admit only capture-free function values". A `T`-typed field at a function-type substitution admits a capture-bearing one.
- §28.3 *Deferred-activation callees*' "A function value read from anywhere else — a field, a collection element, a method call's result — contributes nothing, and soundly so: the closure-escape rules … admit a borrow-capturing closure into none of those locations". Pass `h._v` to a lazy query operator and the frame carries no source for `item`: the dangle rides a frame too, invisibly.

Rationale §5.25 and §4.2's v0.298 record both rest on "borrow-free by construction". The repair must restore that property, not qualify it.

### 1.6 Two routes with no caller-side conversion (added by critique round 1)

Both programs use a generic bag:

```bezel
class Bag<T>
  private owned _items: Array<T> := new Array<T>();
  public method Put(x: T)
    _items.Add(x);                             // parameter-sourced element: §28.11's residual
  end Put;
  public method Fill(reg: ((T) -> ()) -> ())
    reg(lambda(x) => _items.Add(x));           // a {this}-capturing lambda to a call-bounded parameter
  end Fill;
  public method Get(i: int): T => _items[i];
end Bag;
```

**Program E — method-reference formation.**

```bezel
    using var bag := new Bag<() -> int>();
    var put: (() -> int) -> () := bag.Put;     // (1) formed at the substituted signature
    do
      using var item := new Item();
      var f: () -> int := lambda() => item.Value();
      put(f);                                  // (2) f to a function-typed parameter of a function value
    end;
    return bag.Get(0)();                       // (3) dangles
```

At (2), `put`'s parameter is declared `() -> int`, so the call is the closure bullet's argument passing to a function-typed parameter, and §28.14's caller side admits "**any** closure". The value meets `Put`'s `x: T` inside the formed reference, where no caller-side rule looks. The `Object` form needs no generics: `var keep: (() -> int) -> () := k.Keep;` over `Keep(o: Object)`. §27.6's formation "may bridge any [§5.1] implicit conversion", and `tests/codegen/method_ref_conversion_adapter_5208_test.cpp` pins `var b: (int) -> () := H.O;` over `O(x: Object)`. The same form reaches a standard collection, as `var add := xs.Add; add(f)` with `xs: Array<() -> int>`. The closure bullet's collection sink is checked at direct calls, so this one is open at v0.316 independently of this proposal. Value assignability should not be a second bridge. §27.1.1 makes contravariant only parameter slots "of class or interface type", and leaves "everything else" to exact match. This proposal reads a function-typed *target* slot as outside the contravariant case, so a `(Object) -> ()` value is not usable at `(() -> int) -> ()`. The words could be stretched to the value's `Object` slot, so the ▲ formation-bar edit says so in one clause (§3, Option 5). A lambda literal is no bridge, since its parameters take the target's types (§27.4).

**Program D — a type parameter in a parameter slot of a parameter slot.**

```bezel
    using var bag := new Bag<() -> int>();
    do
      using var item := new Item();
      var f: () -> int := lambda() => item.Value();
      bag.Fill(lambda(cb) => cb(f));           // cb: (() -> int) -> () after substitution; cb(f) is call-bounded passing
    end;
    return bag.Get(0)();                       // dangles
```

`reg`'s declared type is a function type whose return slot is `()`, so neither a bare-`T` parameter nor a bare-`T` return slot is involved. In the caller's lambda, `cb`'s parameter is `() -> int` (§27.4), so `cb(f)` is ordinary call-bounded passing. `cb` is the generic body's lambda, which stores its `T`-typed parameter under the class-or-interface assumption. Here `T` sits at positive polarity two parameter slots down, so values flow from the caller into the body there. They flow through invocations *inside* the caller's argument, though, not through the argument's value, so reading the argument's set cannot see them. A capture-free argument carries the hazard too: `lambda(cb) => do using var it := new Item(); cb(lambda() => it.Value()); end`.

---

## 2. Constraints and recorded decisions any fix must respect

1. **K2.4 (§28.3).** "A borrow whose source no caller could re-establish … may not be returned", and a function type "announces no source set". Any admission of a capture-bearing function value into readable storage needs an announcement a plain signature does not have.
2. **K4.3 — recorded trust, or nothing.** "no new admission may be made without recording its clause or its backstop." A trust residual for function values would have to be *recorded*, and (constraint 3) it has already been declined.
3. **Proposal 0126 §2 (graduated v0.298).** "Open the storage and return sinks to a function value whose sources satisfy the object-borrow rules … Declined on K2.4's ground". Rationale §4.2 (v0.298): the sinks "stay closed on K2.4's ground". Per process §2's third rule, reopening this is itself a design change, admissible only on new evidence against the stated reasons. §1.5 is evidence *for* the reasons, not against them.
4. **§28.14's recorded asymmetry.** "a parameter-sourced *object* borrow may be stored into a field under §28.3's borrowed-field rule … but a call-bounded *function* value may not be stored at all … A closure's true bound is its capture set, which a plain signature does not carry, so neither party is positioned to accept the trust".
5. **§28.1 clause 3 and §3.6 rule 2.** The generic body is checked once under the class-or-interface assumption; per-instantiation work is *enforcement* of a pre-substitution-keyed rule, never a re-check of the body.
6. **Proposal 0105's body-locality invariant** (process §9 checklist): "no rule of this specification may require information from inside another declaration's body to check a declaration, except through per-body facts … and no rule may require re-elaborating a body per use." A fix that keys a caller's verdict on whether a callee's body *stores* its parameter is fact-discharged at best (0132's species) and is subject to the records in constraint 7.
7. **The declared-not-derived records.** Rationale §9.7 (`pure` is declared, not derived); proposal 0048's refactoring seam; the v0.314 `DECLINED.md` row *rejecting* a callee-body retention summary at the call (#5360); rationale §5.24's v0.316 addendum (#5374): a body-derived fact "would fire at a statically bound callee and stay silent at a `virtual`, interface or function-value call to the same body"; proposal 0132's **deferral** of the derived-retention fact with a stated reopening bar.
8. **#5377's declared-type keying (v0.316).** *Record components of function type* reads: "a component whose declared type is a type parameter is not reached by this bullet at any instantiation" — written so that record components at `T` are decided *here*, not by that bullet.
9. **§28.3's announcement doctrine.** Every classification is "read off the callee's declared, pre-substitution type"; "a bare type parameter is never re-keyed by what a substitution makes it". A fix that keys on the *substituted* parameter type at the caller would be the first post-substitution keying, which the `Task`-family exclusion paragraph calls "the recorded decline".
10. **The Known v1 limitation (§28.14) and its `deferred` row** (*Unbounded retention tier*, `DECLINED.md`): combinators that compose received callbacks into a returned function value are inexpressible; the missing tier is deferred with a measured bar. A fix must not quietly ship that tier, nor pretend `Pick<T>` over capture-bearing closures is expressible some other way.
11. **Process §7: no history narration in normative text; §5 item 3b: check `src/` and `tests/` before asserting an absence.** §6 below states only the current rule; §1.3's "not yet lowered" claim is sourced to a test comment, not asserted from reading.
12. **The code-generation reference's pin** moves in the same commit as any ownership-rule change (process §9).

---

## 3. Options

Each option is stated with a proposed rule sketch (▲), its soundness argument, what it newly rejects, the expressiveness cost, its body-locality class (0105 §1), its kernel class (process §9), and an implementation sketch against the compiler as read at v0.316 (`src/ownership/Ownership.cpp:465-483` seeds `callBoundedParams_` with the method's *plain function-typed* parameters only; `Ownership_impl.hpp:5774-5860` documents the sink pre-pass; `Ownership_Checks.cpp` `checkBorrowStoreEscape` rejects a field store only when `ownedLocalBorrowSource` finds an owned scope-bound local or `owned` parameter — so Programs A–C are admitted today, matching the text).

### Option 1 — reject at the call: a non-empty-set function value at a bare-`T` or `Object` parameter (#5303's cut, widened to `Object`)

▲ *Sketch.* "A function value with a non-empty source set may not be passed as an argument to a parameter whose declared, pre-substitution type is a bare type parameter or `Object`, nor returned from a lambda into a function-type slot whose declared return is a bare type parameter."

*Soundness.* Closes Programs A and B and the lambda-literal form of C. It does **not** close C's traced-local form: `var mk: () -> (() -> int) := lambda() => f; new Holder<() -> int>(mk)` passes `mk` to a *function-typed* parameter (admitted by §28.14's caller side), and the body's `mk()` is a parameter-sourced borrow stored under the residual. The hazard is the *invocation result's* set, which the closure bullet makes equal to the invoked value's set; Option 1 as cut never looks at it. Unsound as stated.

*Newly rejected.* `new Holder<() -> int>(f)`, `k.Keep(f)`, `new Holder<() -> int>(lambda() => f)` for capture-bearing `f`; also `handlers.Remove(f)`, `.Contains(f)`, `.IndexOf(f)`, `x.Equals(f)`, `source.Raise(f)` at a function-type payload, `Pick(f, g)`, `Console.WriteLine(f)`.

*Cost.* The issue's list, less `Math.Min`/`Max` (not reachable: `where T: IComparable<T>`, stdlib §2.2 lines 144–145; function types conform to no interface). The collection-search trio is vacuous post-repair (§4.2 below). *Body-locality:* local. *Kernel:* derivation (K2.3 + K2.4). *Implementation:* an argument-position check in the caller's ownership pass over the §22.5-bound formal's declared type, reusing `callBoundedSourceOf`'s carrier tracing and the lambda capture analysis the §28.14 admission already runs.

*Verdict.* The right direction, incomplete. Option 5 is its completion. The step between them is short: read the argument's set at the return-slot arm, since the closure bullet makes an invocation result carry the invoked value's set; add the non-argument `Object` positions; and, after critique round 1, add the two companion rules of §1.6. Neither draft closed Programs D and E.

### Option 2 — an instantiation exclusion shaped like the `Task`-family rule

**2a, signature-keyed.** ▲ "A generic type declaring a non-`owned` field, auto-property or record component whose declared type is a bare type parameter `T`, or a generic member declaring a plain parameter of type `T` that it … " — the sentence cannot be finished on the signature alone: a *parameter* of type `T` says nothing about storage. Keyed on **fields**, the rule rejects `Holder<() -> int>` entirely, capture-free uses included (`tests/codegen/generic_holder_function_typed_property_5283_test.cpp`, `tests/sema/box_constructor_function_type_2056_test.cpp`, `tests/codegen/generic_field_direct_call_2068_test.cpp`, `tests/sema/field_initializer_assignability_accept_2926_test.cpp`, the `Wrap<(int) -> int>` cases in `case_type_test_*_erased_{2012,2018}` — all capture-free today and all flipped), does not reach a `Bag<T>` storing into an `Array<T>` field (the field's declared type *mentions* `T` but is not `T`; widening the key to "mentions `T`" rejects every `Array<() -> int>` field too), and does not reach the `Object` route at all. *Body-locality:* carve-out (per-instantiation admissibility on declaration-computable facts). *Kernel:* derivation (K1.6 + K2.4) by analogy with §28.5's generic-field rule. **Declined**: over-rejects capture-free programs the tree relies on, under-rejects the collection and `Object` routes.

**2b, body-keyed.** ▲ "A member is inadmissible at a function-type substitution for `T` when its body stores a `T`-typed parameter into a field or beyond-call collection" — a per-body fact (`stores(M, i)`), carried in the artifact interface (§2.8.2), consumed at the instantiation. Sound where the fact is visible; at an interface, `abstract` or `virtual` member the implementing body is invisible, so the rule must either assume "stores" (which is Option 1 at every dispatched call) or admit unsoundly — the non-uniformity rationale §5.24's v0.316 addendum records against exactly this species. It is 0132's fact widened to collections and non-`readonly` fields, and 0132 §7 **deferred** that fact with a bar this proposal does not meet (no measured count, no implementation probe, no §9.7 class-level ruling). *Body-locality:* fact-discharged. *Kernel:* derivation. **Deferred** with 0132, as `DECLINED.md`'s existing `deferred` rows (#4819's callee-carried storage fact; #5374's warning) already record for the two neighbouring shapes.

### Option 3 — a declaration-site rule for generic bodies

▲ "In a generic body, a store of a value whose static type is a bare type parameter `T` into a field, a collection reachable beyond the call, an `out`/`ref` location or a return is rejected unless `T`'s constraints exclude function types."

*Soundness.* Closes the bare-`T` route at its sink; does nothing for `Object`. *Cost.* §14.2's table: the constraints that exclude function types (`where T: SomeClass`, `where T: IFoo`, `where T: new()`, `where T: value`, `where T: sendable` — function types are not sendable, §30.14) also exclude most class and interface substitutions; the one that admits every class and interface type, `where T: class`, admits "Non-nullable reference types only — classes (including `Array<T>`, `OwningArray<T>`, and other heap collection types), interfaces, `string`, function types" (§14.2's table). So every unconstrained or `class`-constrained container — `Box<T>`, `Holder<T>`, every user collection over `Array<T>` — loses the ability to store its own element type. The standard library's own collections are compiler-known and would be exempt; user code would not. It also reverses #5376's generic sentence. *Body-locality:* local. *Kernel:* derivation (K2.4), but it treats the class-or-interface assumption as a function-type assumption, which §28.1 clause 3 forbids. **Declined**: it rejects the generic container, which is the construct the bare-type-parameter rules exist to admit.

### Option 4 — a K4.3 trust residual for function values

▲ "A function value stored through a bare-`T` or `Object` parameter is admitted under a recorded trust clause: the caller's obligation that every source in its set outlives the holder."

*Soundness.* Not sound as stated. Beyond reopening 0126 §2 (constraint 3) and §28.14's asymmetry (constraint 4), it falsifies the two sentences of §1.5: a `T`-typed field read, a collection element and a method result would have to contribute an *unannounced* or *containing-object* source instead of the empty set, which would make every function value fetched from storage un-storable and un-passable to a deferred-activation callee — the exact regress 0126 §2 refuted at the v0.298 gate round. (Captured value-typed cells are no part of the argument: the closure keeps them alive, and §28.14's v1 bound 1 records that they carry no lifetime hazard.) *Body-locality:* local. *Kernel:* new axiom (a trust clause the kernel cannot derive, with a backstop it cannot name). **Declined** on K2.4 and K4.3, with 0126 §2 as the standing record.

### Option 5 (recommended) — set-erasing positions: the closure bullet names the conversion

The position that erases the set is the *conversion*, not the sink. State it once, in the closure bullet, keyed on declared types exactly as every other classification in §28.3 is.

▲ **Proposed clause** (appended to the closure bullet of §28.3 *Borrow lifetime rules*, after "… and a capture-bearing one is stored only through a `scoped` parameter under §28.14's storage-bounded admission."):

> ▲ A **set-erasing position** is a position whose declared type, nullability stripped, is not a function type and to which a function value nevertheless converts: a position declared `Object` (the only non-function type a function value converts to, §3.1), and a parameter, return slot, `ref` or `out` location, record component, choice payload or inline-array slot whose declared, **pre-substitution** type, nullability stripped (the element type, for an inline array), is a bare type parameter, at an instantiation substituting a function type for it. Such a position is reached by implicit conversion at an argument, an initializer, an assignment, a `return`, a pattern binding, a collection-literal element or a `with` assignment, and — through a function-typed parameter whose declared return slot, reached through return slots only at any depth (`() -> T`, `() -> () -> T`), is a bare type parameter — by the result of every invocation of the argument. A function value as the receiver of a member `Object` declares (`f.ToString()`, `f.GetHashCode()`, an interpolation hole) is not at a set-erasing position: those members are provided for function types, and no declaration overrides them. At every set-erasing position **a function value with a non-empty source set is rejected, and a capture-free one is admitted**: the declared type carries no source set and announces none, so no holder reached through it could re-establish the set (K2.4) and no rule reading the position — the borrowed-field rule's residual, the collection store rule, the bare-type-parameter rule's runtime-managed cell — would see a borrow to bound (K2.3). The check is the caller's, over its own flow analysis and the callee's declared signature, and is enforced per instantiation category (§3.6 rule 2) without re-checking the generic body, which stays under §28.1's class-or-interface assumption. A `scoped` parameter is not a set-erasing position: its declared type is a function type or a class or interface type, never a bare type parameter or `Object`. Remedies: pass a capture-free function value; declare the parameter at a function type (call-bounded, §28.14) and invoke it inside the call; or, where the callee must retain the callback, declare it `scoped`.

▲ **§28.14, *The method-reference formation bar***, widened (added by critique round 1; Program E). After the register sentence, add: "Forming a method reference is likewise a compile error when a parameter slot of the formed function type is a function type, nullability stripped, and binds to a callee parameter that is a set-erasing position ([§28.3](#sec-borrow-lifetime-rules)'s closure bullet): one declared `Object`, or at a bare type parameter that the formation's substitution makes a function type. A call through the formed value would pass a capture-bearing argument to a plain function-typed parameter, which the call-bounded default's caller side admits, and the callee would receive it at a position that announces no set. A function-typed parameter slot of a function value is never retyped to one of `Object` type ([§27.1.1](#sec-function-type-assignability): it is not a class- or interface-typed slot)." The remedy is a lambda, `lambda(g) => xs.Add(g)`, whose body the set-erasing clause then checks. *(The remedy sentence is superseded at v0.330: that lambda is rejected in its body for every argument, its own function-typed parameter carrying itself, and §28.14's remedy is the direct call at each site, `xs.Add(f)`; see the v0.330 changelog.)* §27.6 gains a cross-reference to the widened bar. *Kernel:* derivation (K2.4: the formed type would announce a call bound the callee does not honour). *Body-locality:* local.

▲ **§28.3, beside the `Task`-family exclusion** (added by critique round 1; Program D): "**Function-type substitutions are excluded from type parameters in nested parameter slots (derives: [K1.6](#sec-kernel-k1) + [K2.4](#sec-kernel-k2)).** A member is inadmissible at an instantiation substituting a function type for a type parameter `T` when, within some parameter's declared, pre-substitution type, `T` occurs anywhere within a parameter slot of a function type that itself occupies a parameter slot of that type (nullability stripped throughout): `reg: ((T) -> ()) -> ()`, `reg: ((() -> T) -> ()) -> ()`. A function value of the caller's supplies the arguments of that inner slot by invocation, so no source set the caller reads covers them, while the generic body may store them under the class-or-interface assumption. The use is rejected at the instantiation under [§3.6](#sec-derived-type-properties) rules 2/4's per-instantiation pattern. The diagnostic names the instantiation and the sound spellings: a concrete function type for the slot, or a data type for `T`." At three or more levels of nesting the rule also reaches some negative positions, a deliberate over-reach for one simple key; no declaration in `tests/`, `docs/` or the stdlib writes even the two-level shape. A `T` directly at a parameter, or in a return slot reached through return slots only, is the set-erasing clause's caller-side check. A `T` in a parameter slot one level down (`visit: (T) -> ()`) carries values from the body to the caller, and those are capture-free once the clause holds. *Kernel:* derivation, by analogy with the `Task`-family rule. *Body-locality:* carve-out (per-instantiation admissibility on a declaration-computable fact).

▲ **Row 5's runtime-managed cell**, one qualifying phrase: "at value/runtime-managed substitutions, no borrow and no constraint **(at a function-type substitution the result is a function value carrying the empty set, every argument bound to a source parameter having been admitted at a set-erasing position)**". This makes the cell's existing claim true rather than changing it.

▲ **The #5377 sentence** in *Record components of function type*: "a component whose declared type is a type parameter is not reached by this bullet at any instantiation" → "a component whose declared type is a type parameter is a set-erasing position at a function-type substitution (the closure bullet above), not a field under this bullet".

▲ **§28.14**, lead paragraph: after "or passed as an argument to a function-typed parameter, among the rest" add "(a parameter declared at a bare type parameter or at `Object` is a set-erasing position, §28.3's closure bullet)". *The call-bounded default*'s caller-side sentence is unchanged — it speaks of a parameter "declared *without* `scoped`" and function-typed, which a set-erasing position never is.

▲ **§28.1 clause 3**, one cross-reference at "The generic body type-checks under the class-or-interface assumption": "(a function value entering a bare-type-parameter position is admitted at the caller only capture-free, §28.3's closure bullet, so the assumption is sound at function-type substitutions)".

▲ **Appendix C**, one row: "No function value with a non-empty source set reaches a set-erasing position (§28.3)."

**Soundness argument** (re-stated by critique round 1). Let a function value `v` with source set `S ≠ ∅` reach any holder `H` whose declared type is not a function type. The path from `v`'s formation to `H` passes through at least one conversion of `v` to a non-function static type, since `v`'s formation site types it at a function type and `H` does not. The only non-function types a function value converts to are `Object` and a substituted type parameter (§3.1). Take the first such conversion. It occurs at one of three kinds of site. (a) It is in a body that types `v` at a function type and the position at `Object` or at a bare type parameter: a set-erasing position by definition, rejected by the clause. (b) It is inside a formed method reference, so `v` reached a function-typed parameter slot of the formed value that binds to an `Object` or bare-`T` callee parameter (Program E). The widened formation bar rejects the formation. (c) It is inside a generic body, at a `T`-typed binding whose value came from outside the body at a function type. A value enters a generic body only at its parameters and receiver, at the results of invoking its function-typed parameters, and at the arguments its own function values receive. These are, in turn: the clause's bare-`T` parameter check; the return-slot arm, reading `v`'s set as the invocation result's; and a `T` nested in a parameter slot of a parameter slot, which the new admissibility rule makes inadmissible at a function-type substitution (Program D). A `T` one parameter slot down receives values *from* the body, so it is not an entry. Every case is rejected. This relies on two rulings that make the remaining aggregate holders fields under the closure bullet: #5377 (v0.316: record components declared at a function type) and the v0.317 ruling on #5486 (choice payloads and inline-array slots declared at a function type, local arrays included), which is in flight. Without #5486, an inline-array local `[f]` would carry `f` into `Object` as a value that is not a function value (§9.7: runtime-managed component types do not make an aggregate borrow-bearing). Graduation must therefore follow #5486 or pass process §4's composition gate with it. Conversely, a function value admitted at a set-erasing position has `S = ∅`, so every holder downstream — a `T`-typed field, an `Object`-typed field, a collection of either, a frame carrying either, a row-5 result — holds a capture-free value, which is exactly what the closure bullet's "carries the empty set" and *Deferred-activation callees*' "borrow-free by construction" assert. The return-slot route: an argument `a` to `mk: () -> T` has, by the closure bullet, invocation results carrying `a`'s set; the clause reads the parameter's declared return slot (bare `T`, substituted by a function type) and requires `a`'s set empty, so `mk()` in the body is capture-free whatever the body does with it. The generic body's own check is untouched: under the class-or-interface assumption it admits `_v := x` and `_v := mk()` under the residual, and at a function-type substitution the residual is vacuous because the stored value carries nothing to dangle. §28.1 clause 3 therefore stays sound. The `Object` arm needs no substitution reasoning: the conversion is explicit in the type system and the set is the caller's own.

**What it newly rejects**, with examples (all with `f` capture-bearing, `item` a scope-bound or longer-lived source alike — the rejection is categorical, as the closure bullet's sink rule is):

| Shape | Example | Today | Under 0133 |
|---|---|---|---|
| bare-`T` parameter at a function-type instantiation | `new Holder<() -> int>(f)`, `bag.Add(f)` on a user `Bag<T>`, `s.Apply<() -> int>(f)` | admitted | **rejected** |
| `Object` parameter | `k.Keep(f)`, `Console.WriteLine(f)` (stdlib §2.1), `sb.Append(f)` (stdlib §3.1), `x.Equals(f)` (`Equals(other: Object?)`, §8.1) | admitted | **rejected** |
| received callback forwarded to a generic or `Object` utility | `Mid(g: () -> int)` calling `H.Keep<() -> int>(g)` over `Keep<U>(x: U): int`; `Log(g)` over `Log(o: Object)` | admitted | **rejected** (a plain function-typed parameter carries itself; §5.4's one flipped test) |
| method reference formed with a function-typed slot over a set-erasing parameter | `var put: (() -> int) -> () := bag.Put;`, `var keep: (() -> int) -> () := k.Keep;`, `var add := xs.Add;` at `xs: Array<() -> int>` | admitted | **rejected at formation**, capture-free uses included |
| instantiation at a function type of a member with `T` in a nested parameter slot | `bag.Fill(…)` on `Bag<() -> int>` with `Fill(reg: ((T) -> ()) -> ())` | admitted | **rejected at the instantiation**, capture-free uses included |
| `Object` local, field initializer, return, pattern binding | `var o: Object := f;`, `return f;` under `: Object`, `case f of var o: Object => …` | admitted | **rejected** |
| lambda into a slot whose declared return is bare `T` | `new Holder<() -> int>(lambda() => f)` with `constructor(mk: () -> T)` | admitted | **rejected** |
| traced local into such a slot | `var mk := lambda() => f; new Holder<() -> int>(mk)` | admitted | **rejected** |
| record component / choice payload declared at `T` | `new Box<() -> int>(f)` for `record Box<U>(V: U)`; `Result<() -> int, E>.Ok(f)` | admitted | **rejected** |
| non-storing stdlib bare-`T` queries | `handlers.Remove(f)`, `.Contains(f)`, `.IndexOf(f)` | admitted | **rejected** (vacuous, §4.2) |
| non-storing user selector | `Pick(f, g)` with `static method Pick<T>(a: T, b: T): T` | admitted | **rejected** (§4.3) |
| event payload at a function-type `T` | `source.Raise(f)` on `EventSource<() -> int>` (stdlib §6.5) | admitted | **rejected** (§4.3) |

Not affected: every capture-free function value at every position (`new Holder<() -> int>(lambda() => 7)`, `Util.Wrap<() -> int>(f)` with `f := lambda() => 1`, `new Rec(H.D)` with a static method reference); every function-typed parameter, plain or `scoped`; `Math.Min`/`Max` (unreachable at a function type); `Array<() -> int>.Add(f)` and every stdlib collection store at a direct call (already the closure bullet's "any collection" sink, and the compiler's `builtinCollectionStoreCallee` path); `xs.Select(lambda(x) => lambda() => svc.Run(x))` where `U` infers to a function type and the selector's declared type is `(T) -> U` — see §8 item 4 for why this *is* reached and why that is right.

**Body-locality classification:** **local** for the clause and the widened formation bar; **carve-out** for the nested-slot admissibility rule. The clause and the bar read the callee's declared signature (pre-substitution types and the substitution at the site) and the caller's own flow analysis: no information from any other body, no per-body fact, no re-elaboration. The admissibility rule is per-instantiation admissibility on a declaration-computable fact (the declared parameter type) evaluated against the substitution. That is §3.6 rule 2's confinement (i), the same class as the `Task`-family exclusion.

**Kernel classification:** **derivation** — K2.3 (escape must be bounded; the erased set cannot be) + K2.4 (a type that announces no set admits no holder that could re-establish one), with K1.6 fixing what a function value is. The two companion rules are derivations too: the widened formation bar from K2.4 (a formed type would announce a call bound its callee does not honour), and the nested-slot admissibility rule from K1.6 + K2.4, by analogy with the `Task`-family exclusion. No axiom is extended. A11's forms are untouched, since the formation bar's existing register sentence is unchanged and the new sentence protects the closure bullet, not a register position. No register row.

**Implementation sketch** (`src/ownership/`, from the v0.316 source read; nothing built):

- *Where.* The caller-side ownership pass, at: call and `new` arguments (after §22.5 binding — `verifiedCallBoundedFormalAt` already looks the bound formal up by name, `Ownership_impl.hpp:5857`), choice-case constructions, collection-literal elements typed `Object`, assignments and initializers whose target type is `Object`/`Object?` and whose RHS static type is a function type, `return` under an `Object` return type, pattern bindings of declared type `Object`, `ref`/`out` arguments, and lambda return paths checked against a slot whose declared return is a type parameter (the *Concrete-slot function bodies* check site).
- *Key.* The **declared** formal type from the generic declaration (`ParameterSymbol::declType` kind `TypeParameter`, or resolved `Object`), together with the substituted type being a function type (`TypeRef::Kind::Function`). For the return-slot arm: the declared formal is a function type whose return slot (transitively) is a type parameter substituted by a function type.
- *The set.* The argument's source set is what the compiler already computes in two places: the capture analysis the §28.14 `scoped` admission runs over lambda literals and method references, and `callBoundedSourceOf`'s carrier tracing ("bare identifier, carrier local, capturing lambda, conditional, collection literal, postfix-bang") for traced locals and call-bounded parameters. A non-empty set, or a parameter source (a plain function-typed parameter of the enclosing body contributes itself), or an untraceable provenance (the "unannounced" source) rejects; a lambda with no captured source, a static method reference, a field or collection read, or a method-call result passes.
- *Diagnostic.* "Function value `f` carries captured borrow(s) of `item`; the parameter `x` is declared at `T` (here `() -> int`), a position that cannot carry them. Pass a capture-free function value, declare the parameter at a function type, or declare it `scoped`." Name the clause so the trust-admission report's vocabulary (§28.3 *Diagnostics*) and this rejection read alike.
- *Formation bar (critique round 1).* At method-reference formation (§27.6), after overload selection against the target: for each parameter slot of the target whose type is a function type, look up the bound callee formal's declared type. It is barred if that type is `Object`/`Object?` or a type parameter the formation's substitution makes a function type. The site is the one that already enforces §28.14's register bar.
- *Nested-slot admissibility (critique round 1).* At each instantiation, the per-instantiation check that already enforces the `Task`-family exclusion walks each parameter's declared type for a type parameter inside a parameter slot of a parameter-slot function type, and rejects it when that type parameter is substituted by a function type.
- *Generic body.* Unchanged: `checkBorrowStoreEscape` keeps admitting `_v := x` and `_v := mk()` under the residual.
- *Tests to add at graduation.* Programs A–E as `runAot` rejections (fail-without: all five compile today, by the source read), the capture-free twins of A–C as runs, `Pick(f, g)` and `Raise(f)` rejections, `Console.WriteLine(f)` rejection, and an ASan run of Program A's today-admitted form to pin the dangle the rule closes.

---

## 4. Recommendation

**Adopt Option 5.** Reasons, in order of weight:

1. **It is the only option that is sound over every route, once amended.** With the two companion rules of critique round 1 it closes Programs A–E (§3, soundness argument). Option 1 misses the traced-local return-slot form (§3, Option 1). Options 2a and 3 miss `Object`, and 2a misses collections. Option 4 is unsound without a regress 0126 already refused. As first drafted, no option closed D and E.
2. **It derives from the kernel with nothing new.** K2.4 is the ground the closure bullet already cites "at every position that erases the set"; Option 5 names the positions. No axiom, no trust clause, no register row, no body fact.
3. **It respects every record in §2.** The generic body stays under the class-or-interface assumption (§28.1 clause 3); the borrowed-field residual stays for object borrows; §28.14's asymmetry is restated, not altered; 0126 §2 stands; #5377's declared-type keying is re-pointed, not reversed; the keying is pre-substitution (constraint 9).
4. **Its cost is small and measured** (§4.1–§4.3, §5): one in-tree test flips (§5.4) and no corpus case does; the stdlib sites reached are `Console.Write`/`WriteLine(Object)`, `StringBuilder.Append(Object)`, `Object.Equals(Object?)`, the collection-search trio, and `EventSource<T>.Raise` at a function-type payload, plus method references to those members formed at a function-typed slot.

### 4.1 Why not wait for a storing fact (Option 2b)

Because the hole is a statically certain dangle today, and 2b's precondition is a deferred study whose bar needs evidence this question cannot supply. Option 5 does not foreclose 2b: if 0132 is ever taken up, a `stores(M, i)` fact could *relax* Option 5 at a statically bound callee whose fact is "does not store" — a widening, under its own proposal, against the same non-uniformity objection it faces today.

### 4.2 The collection-search trio is vacuous post-repair

The lane's HYPOTHESIS (store.md, N6) holds once the hole is closed: a borrowing or owning collection admits an element only through `Add`/`Insert`/indexer-set, which the closure bullet reads as a store of a function value and admits only capture-free; the bare-`T` and `Object` routes were the only other way in, and Option 5 closes them. So no collection ever holds a capture-bearing function value, and `Remove(f)`, `Contains(f)`, `IndexOf(f)` with capture-bearing `f` would answer *absent* on every call under §27.7.1's function-value equality. Rejecting the call loses no program that did anything; the diagnostic should say so ("no collection can hold a capture-bearing function value; this query would always find nothing").

### 4.3 The real costs, and their remedies

- **`Pick<T>(a: T, b: T): T` over capture-bearing closures.** The selector returns one of its arguments with the set erased (row 5's runtime-managed cell), so it *must* be rejected: `_f := Pick(f, g)` would otherwise store a capture-bearing closure as a "method call's result [that] carries the empty set". The remedy is the expression form `(if c then f else g)`, whose arms are function values that keep their sets (K7.2's union). A function-typed `Pick(a: () -> int, b: () -> int): () -> int` is not a remedy — it returns a call-bounded parameter, which §28.14 rejects; that is the *Unbounded retention tier* deferral, whose `DECLINED.md` row should be annotated with this instance at graduation (§6).
- **`EventSource<T>.Raise(payload: T)` at a function-type `T`.** A capture-bearing closure as an event payload is rejected. Remedy: raise data and let handlers close over what they need; or raise a capture-free closure. The shape is rare (no test or doc example raises a function-typed payload; the only function-typed `EventSource` instantiations in the tree are `EventSource<()>`), and the alternative — exempting `Raise` by name — would be a per-member allowlist of the kind §28.14's register already confines to `scoped`-marked and stdlib-certified positions.
- **A received callback forwarded to a generic or `Object`-typed utility** (found by critique round 1). A plain function-typed parameter carries itself, a parameter source the signature does not announce, so `Mid(g: () -> int)` cannot pass `g` to `Keep<U>(x: U)` at `U = () -> int`, nor to `Log(o: Object)`, even where those callees store nothing. Remedies: pass what the utility needs (`Log(g())`, a description); or declare the utility at a function type, where §28.14's call-bounded default admits the forward. One test exercises the shape (§5.4).
- **Method references formed at a function-typed slot over `Object` or bare-`T` parameters** (`bag.Put`, `xs.Add`, `Console.WriteLine` at `(() -> int) -> ()`). Rejected at formation, capture-free uses included, because a later call through the formed value cannot be checked. Remedy: a lambda (`lambda(g) => xs.Add(g)`), whose body the clause checks. *(Superseded at v0.330: the lambda is rejected in its body; the remedy is the direct call at each site, `xs.Add(f)`.)* No site in `tests/` or `docs/` by targeted grep.
- **Members with a type parameter in a nested parameter slot, at a function-type instantiation** (`Fill(reg: ((T) -> ()) -> ())` on `Bag<() -> int>`). Inadmissible, capture-free uses included. No stdlib signature, test or doc example has the shape.
- **`Equals(other: Object?)` (§8.1), `Console.WriteLine(value: Object)` (stdlib §2.1), `StringBuilder.Append(value: Object)` (stdlib §3.1).** Rejected for a capture-bearing argument. A function value as the *receiver* (`f.Equals(g)`, `f.ToString()`, `$"{f}"`, whose hole formats via `ToString()`, §18.5) is not reached; only the argument is. Remedies: `f = g` (§27.7.1's operator, whose operands are function-typed); print a description rather than a closure. Cost near zero; recorded for completeness.
- **`var o: Object := f` and `case f of var o: Object`** with capture-bearing `f`. Rejected. No program in the tree does this. A type test with no binding (`f is Object`) is not a position.
- **Not a new cost:** a `Dictionary<string, () -> int>` registry of capture-bearing callbacks is already rejected by the closure bullet's collection sink, and `grep -nE 'Dictionary<[^>]*\(|Array<\(' docs/*.md` finds no registry idiom in the guides or the stdlib.

---

## 5. Migration

### 5.1 Newly rejected

The rows of Option 5's table. Every set-erasing-position row and the forwarding row reject only a function value whose source set is non-empty, and nothing capture-free changes verdict there. The formation-bar and nested-slot rows are categorical: they reject capture-free uses too, because the value that would need checking is not visible at the site. Nothing passed directly to a function-typed parameter changes verdict.

### 5.2 Newly defined

The term *set-erasing position*; row 5's function-type substitution result as a capture-free function value; the record-component sentence re-pointed.

### 5.3 Spec-ahead-of-impl (at graduation, one tracking issue)

Every row of the table is admitted by the compiler today (`checkBorrowStoreEscape` finds no owned-local source; `callBoundedParams_` is seeded with plain *function-typed* formals only; the caller side of a plain parameter is unrestricted). The compiler also does not yet lower the `Object`→function-type narrowing Program B's step (3) uses (test comment at `tests/codegen/method_own_type_param_escape_release_5127_test.cpp:38`, citing #2434/#2491) — so Program B's dangle is text-reachable but not yet runnable, which the issue should state.

### 5.4 Tests (`tests/`) — one flips

**Flips (run → reject), found by critique round 1:** `tests/codegen/method_own_type_param_named_local_release_5129_test.cpp:97`, `codegen_5129.param_arg_to_unreferenced_callee_no_over_release`. `Mid(g: () -> int)` returns `H.Keep<() -> int>(g) + g() - 1` over `Keep<U>(x: U): int`. `g` is a plain function-typed parameter, so its set is non-empty, and `x: U` at `U = () -> int` is a set-erasing position. The row pins a codegen property: a parameter slot that owns no +1, passed to a bare-`U` formal, is not over-released. Under this proposal that shape is unreachable at a function-type `U`. At graduation the row becomes a `reject` pin of the new verdict, recorded in the commit body as a behavior flip; this is not a weakening. Its sibling rows in the 5129, 5216 and 5231 release families pass capture-free lambdas or locals and do not flip.

The draft's sweep grepped explicit function-type arguments only. The release families above instantiate the same way, and inferred instantiations were not swept exhaustively. Graduation re-runs the sweep over inferred instantiations: generic calls whose argument is a function-typed parameter or a capture-bearing local. Otherwise every in-tree instantiation of a user generic at a function type passes a **capture-free** value (search: `grep -rnE '<\([^)]*\) *->' tests/`, 332 lines, read by file):

- `tests/codegen/generic_holder_function_typed_property_5283_test.cpp` — `new Holder<() -> int>(f)` with `f := lambda () => 7`.
- `tests/sema/box_constructor_function_type_2056_test.cpp`, `tests/codegen/generic_field_direct_call_2068_test.cpp` — `new Box<(int) -> int>(f)` with `f := lambda(x) => x + 1`, and `lambda(x) => x + 1` inline.
- `tests/sema/field_initializer_assignability_accept_2926_test.cpp` — `V: U := Box.Noop` (a static method reference).
- `tests/sema/void_generic_method_bare_typeparam_function_arg_5162_test.cpp` — `s.Apply<() -> int>(f)` with `f := lambda () => 42`.
- `tests/codegen/method_own_type_param_escape_release_5127_test.cpp` — `s.Keep<() -> int>(f)`, `Util.Wrap<() -> int>(f)` with `f := lambda () => 42` / `lambda () => 1`.
- `tests/sema/case_type_test_{plain_scrutinee,value_form}_erased_{2012,2018}_test.cpp` — `Wrap<(int) -> int>` scrutinee shapes, capture-free.
- `tests/sema/sorted_dictionary_add_nullable_arg_3399_test.cpp` — `SortedDictionary<int, () -> int>.Add(1, p.Q)`, a property read (empty set).
- `tests/sema/extension_function_type_generic_arg_4168_test.cpp` — `Array<(int) -> int>` literals of capture-free lambdas.
- `Object`-typed parameters: 215 `: Object)` lines in `tests/`; the one line pairing `Object` with a lambda is `tests/sema/formation_fence_function_value_1820_test.cpp:177` (`var g: () -> Object := lambda() => c.Run();` — a function *returning* `Object`, not a function value converted to it). No test passes a lambda to an `Object` parameter.

**Found during the sweep, out of scope, to be filed separately (re-labelled by critique round 1):** `method_own_type_param_escape_release_5127_test.cpp:130` runs `static method Wrap<U>(x: U): () -> U  var w: () -> U := lambda () => x; return w;`. Under the class-or-interface assumption `w` captures the plain parameter `x` and carries `{x}`; the closure bullet rejects returning a function value "while its source set is non-empty" (and at a class `U` the caller would store a closure over its own argument as "a method call's result [that] carries the empty set"). The compiler admits it. The text's verdict is settled: the generic body is checked once under §28.1 clause 3, and *Bare-type-parameter returns* lifts the parameter-sourced prohibition only for a bare `T` return, not for `() -> U`. In this proposal's own usage (§5.3), a compiler that admits what the text rejects is **spec-ahead-of-impl**, independent of #5375. Two more rows pin text-rejected programs of the same family: 5127's `method_own_type_param_wrapped_closure_return_value` (same `Wrap<U>`), and `method_own_type_param_array_add_release_5216_test.cpp`'s `lambda_literal_captured_by_callee_wrapper_no_uaf` (`Cap<U>` adds `lambda () => x`, which captures the plain parameter `x`, to `l: Array<() -> U>`). File one spec-ahead-of-impl issue naming all three. A probe at a class substitution (`Wrap(item)` with `item` a `using var`, the result invoked after the scope) only shows whether the admitted program dangles. The verdict needs no probe.

### 5.5 Conformance corpus (`tests/conformance/`)

No case states a verdict this proposal changes. `tests/conformance/rules/manifest.tsv` was searched by section (28.3, 28.14, 14.2, 9.7) and by construct (bare-type-parameter parameters, `Object` parameters, function-type instantiations): `s28_3_0097`/`0098` pin the v0.314 property/indexer stores (unchanged); `s28_14_0005` pins "any closure may be passed to a call-bounded parameter" — a *function-typed* parameter, unchanged; `s28_3_0026`/`0027` pin the back-reference idiom and the scope-bound rejection for object borrows, unchanged. **Coverage gaps to record at graduation** (not written under landing pressure, per process §5 item 3c): a `reject` for Program A, for Program B (`lib` shape until the narrowing lowers), for Program C in both argument forms; an `accept` for the capture-free twin of each; a `reject` for `Pick(f, g)` with capture-bearing arguments and an `accept` for the `if`-expression remedy; a `reject` for Program E in both forms (`bag.Put`, and `k.Keep` over `Object`) and an `accept` for its lambda remedy; a `reject` for Program D's instantiation.

### 5.6 Documentation examples

`grep -nE 'new [A-Z][A-Za-z0-9]*<\([^)]*\) *->' docs/*.md` finds only the v0.314 changelog line about #5303. No guide, spec or stdlib example instantiates a user generic at a function type with a capture-bearing argument, and no example converts a closure to `Object`. The guides need no revalidation beyond a re-read of Guide 01's closure section; the pin moves only if a sentence is touched.

---

## 6. Edits at graduation (described, not applied)

- **Spec §28.3**, the closure bullet: append the ▲ clause of Option 5. The announcement table, row 5: the ▲ qualifying phrase. *Record components of function type*: the ▲ re-pointed sentence. **§28.14** lead paragraph: the ▲ parenthetical; *The method-reference formation bar*: the ▲ widening, with a cross-reference from §27.6. **§28.3**, beside the `Task`-family exclusion: the ▲ nested-slot admissibility paragraph. **§28.1** clause 3: the ▲ cross-reference. **Appendix C**: the ▲ invariant row. No history narration; the changelog entry carries the story.
- **Changelog** `### v0.NNN changelog`: one bullet in the style of v0.316's #5376 entry — the question, the five routes, the rule and its two companions, the declines (Options 2a, 3, 4 by name; 2b deferred with 0132), the costs of §4.3 with remedies, kernel and body-locality classes, and the compiler paragraph. **Migration**: *Newly rejected* (the table's rows, the one flipped test named), *Newly defined* (§5.2), *Spec-ahead-of-impl* (one issue), *Doc-only* (rationale, code-generation reference, `DECLINED.md`).
- **Rationale §4.2**: a dated paragraph "Function values at set-erasing positions (v0.NNN, #5375)" after the v0.316 record: the two readings that disagreed, why the conversion is the position to name, why the collection-search cost is vacuous, why `Pick<T>` is the unbounded-tier limitation seen again, and the three declines with their reasons.
- **Code-generation reference §4.4**, the closure bullet: after "do **not** store it in a field or a collection (…)" add "or pass it where the parameter is declared `Object` or at a bare type parameter `T` that your call instantiates at a function type — `new Holder<() -> int>(f)`, `k.Keep(f)`, `Console.WriteLine(f)`, `Pick(f, g)` are all rejected while `f` captures anything; a capture-free value goes anywhere; and do not form a method reference such as `xs.Add` or `bag.Put` at a function-typed slot, but write a lambda (§28.3, §28.14, v0.NNN)". **§7.2**, one row: the `Holder<() -> int>` shape with the remedy. Move the pin.
- **`DECLINED.md`**, `rejected` group, three rows: *An instantiation exclusion for generic types with `T`-typed fields at function-type substitutions* (Option 2a; bar: a key that distinguishes storing from non-storing declarations on the signature); *A declaration-site store rejection for bare-`T` values in generic bodies* (Option 3; bar: a constraint form that excludes function types while admitting every class and interface type); *A K4.3 trust residual for function values at bare-`T`/`Object` positions* (Option 4; bar: 0126 §2's — new evidence against K2.4's ground, together with a sound source for storage reads). `deferred` group: annotate the *Unbounded retention tier* row with the `Pick<T>` instance, and annotate the #5374 row (*A caller-side warning … from a body-derived per-parameter storage fact*, proposal 0132) with Option 2b as a second dependent of the same deferred fact. The #4819 row is a `ref`/`out`-slot fact, not this one. Recompute the index-size line once over the revision.
- **Issues**: close #5375 with the citation; file the one spec-ahead-of-impl issue (§5.3); file the §5.4 `Wrap<U>`/`Cap<U>` spec-ahead-of-impl issue; add the §5.5 coverage gaps to the corpus backlog.

---

## 7. Open questions for the owner

1. **`Raise(payload: T)` at a function-type payload.** Accept the rejection with the data-not-closures remedy (recommended), or carve a named stdlib exemption for `EventSource<T>.Raise`/`RaiseAsync` on the ground that they store nothing? The carve would be the first per-member allowlist outside §28.14's register, and a user `IEvent<T>` implementation could not share it.
2. **`Object`-typed stdlib sinks.** Should `Console.Write`/`WriteLine(Object)` and `StringBuilder.Append(Object)` be left as rejections (recommended — they only call `ToString()`, but the rule is uniform and nothing in the tree hits it), or should the stdlib declare `ToString`-only overloads at a function type?
3. **The `scoped x: T` pin.** This proposal leaves `scoped` on a bare-type-parameter parameter *unwritable*, as today, because its two tiers cannot share one field kind: a `scoped` field is never read out as a value (function tier), while a bare-`T` field of a generic container exists to be read (`Get(): T`). Confirm that the pin closes with #5375 as "not the remedy", or direct a separate study of a per-instantiation `scoped T`.
4. **Row 5 at a function-type substitution.** The ▲ phrase states the result is capture-free. An alternative is to make row 5 *announce* the union of the arguments' sets at a function-type substitution (K2.4 re-establishment, so that `Pick(f, g)` would be admitted with set `{f, g}`'s sources) — but a bare-`T` parameter can also be *stored* by the same member, so the arguments must be capture-free anyway, and the announcement would announce only the empty set. Confirm the simpler statement (recommended).
5. **Graduation sequencing.** The soundness argument depends on #5486's ruling (v0.317 train, in flight). Land after it, or in the same train under the composition gate (process §4). The §5.4 `Wrap<U>` issue is independent in text.
6. **The nested-slot rule's form** (critique round 1). The recommended rule makes a member with a type parameter in a nested parameter slot inadmissible at a function-type substitution of it, which is simple and has no measured cost. The alternative admits a lambda-literal argument at such a parameter. It treats the literal's function-typed parameter's erased slots as set-erasing inside its body, and rejects every non-literal argument. That alternative is sound only if the marked binding can never be forwarded to a position that drops the mark, which is a new per-binding annotation. Confirm the exclusion (recommended).

---

## 8. Self-critique (adversarial pass; resolved in the text above)

Counterexamples hunted against the recommendation, each traced:

1. **Generic containers of `Object` — `Array<Object>.Add(f)`, `Dictionary<string, Object>[k] := f`.** The RHS static type is a function type, so the closure bullet's "any collection" sink and the *property and indexer stores* bullet already reject a non-empty set; additionally `Add(item: T)` at `T = Object` is a set-erasing position under the new clause. Doubly rejected; no conflict, same verdict. *No change.*
2. **`Array<T>` at `T` a function type — `xs.Add(f)`, `xs[0] := f`, `[f]` as `Array<() -> int>`.** Already the closure bullet's collection sink (and the compiler's `builtinCollectionStoreCallee`). The new clause also reaches `Add(item: T)` since `T` is a bare type parameter of the stdlib declaration — the same verdict by two rules. The capture-free `[f]` literals in `extension_function_type_generic_arg_4168_test.cpp` are unaffected. *Corrected by critique round 1:* both rules see only a direct call. `var add := xs.Add; add(f)` reached the collection through a formed reference, which the widened formation bar now rejects (§1.6, Program E).
3. **Dictionary values — `d.Add(k, f)`, `d[k] := f` at `V` a function type.** As item 2. `d.TryGetValue(k, out var g)`: `g` is a collection read, empty set — true post-repair. *No change.*
4. **Lazy query producing closures — `xs.Select(lambda(x) => lambda() => svc.Run(x))` at `U = () -> int`.** The selector's declared type is `(T) -> U` with a bare-`U` return slot, so under the ▲ clause the selector must be capture-free, and this one captures `svc`: rejected. Is that an over-rejection? Where `x` is class-typed the inner closure captures the outer lambda's own parameter, which the lambda-return clause already rejects today ("one over a binding declared *inside* the body is the scope-bound rejection"), so the only newly rejected form is a value-typed `x` with an outer borrow (`svc`) captured. That form is sound under the current text: the frame carries `svc` through the selector's capture-set contribution, and an element read out of the frame is an iteration variable, which "carries an **unannounced** source … bounded by the call", so it can neither be stored nor passed to a set-erasing position (an unannounced source satisfies no sink rule, and the ▲ clause rejects it too). Option 5 therefore over-rejects a shape whose results could only ever be invoked inside the enumeration. **Resolution:** keep the rejection in the recommended text for uniformity, record it in §4.3's cost family, and offer the owner the sound refinement: exempt the return-slot arm where the *member's own* declared return is mint-announcing (row 2), because there frame residence carries the argument's set and nothing erases it. Stated as a liftable refinement for the changelog if the owner prefers it.
5. **Async — `async method KeepAsync<T>(x: T): Task` storing `x`, called with `f`.** The caller's argument is at a set-erasing position: rejected for a capture-bearing `f`. Capture-free `f`: admitted; the frame carries no set, correctly. A `Task<T>` result at `T = () -> int` awaited: `var g := await t;` — `g` is a frame result; the closure bullet gives a method-call result the empty set, true because every value that entered the frame's `T` positions was capture-free. *No change.*
6. **Captured `this` — `new Holder<() -> int>(lambda() => this.Count)`.** The lambda's set is `{this}`, non-empty: rejected at the set-erasing position, exactly as `_f := lambda() => this.Count` is rejected at a field today. A `this`-capturing closure may still be passed to a function-typed parameter or registered through `scoped`. *No change.*
7. **Nested lambdas — `new Holder<() -> () -> int>(lambda() => lambda() => item.Value())`.** The outer lambda's captures "include the free bindings of every lambda nested in its body that resolve outside that body": `{item}`. Rejected. *No change.*
8. **Method references — `new Holder<() -> int>(item.Value)`.** A receiver-binding method reference's set is `{item}`: rejected. `new Holder<() -> int>(Util.Seven)` (static): empty, admitted. *No change.*
9. **Interface-typed parameters — `M(x: IFoo)` with a function value.** Function types conform to no interface (§3.1 lists them as reaching `Object` directly; §11 and §14.2's conformance rule name classes, records, enums, primitives and interfaces), so no conversion exists and the position is unreachable. If a future revision adds a conformance, the ▲ clause's "a position … to which a function value nevertheless converts" already covers it. *No change; the clause's wording was chosen for this.*
10. **`where T: class` and `where T: notnull`.** Both admit function types (§14.2's table; `notnull` excludes only nullable types). A generic constrained either way behaves as unconstrained for this rule: its `T` positions are set-erasing at a function-type substitution. `where T: sendable` excludes function values (never sendable, §30.14); `where T: IFoo`/`SomeClass`/`new()`/`value` exclude them too. *No change; stated in Option 3's analysis.*
11. **`where T: U` with `U` unconstrained.** A function-type substitution for `U` admits the same for `T`; both are set-erasing. *No change.*
12. **Inline arrays — `[N]T` field at a function-type `T`; `arr[i] := f`.** The slot store is a store into field storage; the closure bullet's field sink reaches it when the RHS static type is a function type, and the `fnfield` lane filed the inline-array question (`scratch/v0316/fnfield.md` (e) 1) for the declared-function-type case. At a bare `T`, a `[N]T` *parameter*, component or local slot is only covered if the clause reads the element type. **Resolution (applied):** the ▲ clause names "inline-array slot" and reads "the element type, for an inline array".
13. **Choice payloads — `Result<() -> int, E>.Ok(f)`, `Option`-like user choices.** `Ok(value: T)`'s payload is declared at `T`: a set-erasing position; rejected for a capture-bearing `f`. A choice with a payload *declared* at a function type is the `fnfield` lane's filed question, not this one. *Stated in the table.*
14. **`ref`/`out` of `Object` — `M(out o)` with `o: Object?` filled from a function value inside the callee.** The callee's `o := f` is an assignment to an `Object`-typed location: set-erasing, rejected in the callee for a non-empty set. The caller side's `ref`/`out` rule is unchanged. *No change.*
15. **The record-component sentence of #5377.** Re-pointing "not reached by this bullet at any instantiation" to the set-erasing clause changes no verdict #5377 landed (its cases pass capture-free values) and does not reverse its declared-type keying — the component's declared type is still what is read. *Stated in Option 5.*
16. **Does the rule key post-substitution (constraint 9)?** No. It keys on the declared type being a bare type parameter (pre-substitution) and on the substitution being a function type — the same two-part key the `Task`-family exclusion uses ("a function type whose return slot is a bare type parameter … inadmissible at an instantiation substituting a `Task`-family type"). *Stated in Option 5.*
17. **Is the clause a body re-check?** No: the generic body is never consulted. The caller reads the callee's signature and its own flow. *Stated under body-locality.*
18. **Does it reopen 0126 §2 or the #5360/#5374 rows?** No: it stores nothing new and reads no body. It narrows, in the direction 0126 §2 already chose. *Stated in §2 and §4.*
19. **A capture-free value whose provenance the body cannot trace (an iteration variable, a `ref` parameter's value).** It carries an "unannounced" source and is rejected at a set-erasing position, as at every other sink. This is conservative and consistent with the closure bullet; the remedy is to form the value where it is passed. *No change.*
20. **Section citations re-verified against v0.316 headings:** §3.1 *Type kinds* (line 427; the `Object` root at 444 and 1209); §3.6 *Derived type properties* (518); §9.7 *Records and disposal* (1521); §12.7 (2229–2271, no function-type exclusion); §14.2 *Constraints* (2479); §28.1 *Where the `owned` modifier can appear* (6658; clause 3 at 6683); §28.3 (6731) — K1–K9 (6751), the announcement table (7002, row 5 at 7019), *Bare-type-parameter returns* (7048), the `Task`-family exclusion (7072), *Concrete-slot function bodies* (7074), *Borrow lifetime rules* (7125), the field-store bullet (7138–7143), the closure bullet (7145), *Record components of function type* (7149), *Deferred-activation callees* (7211), *Diagnostics* (7216); §28.5 *Owned fields* (7293); §28.9 *Runtime-managed types* (7668); §28.11 *Storing a borrow* (7844); §28.14 (8044; the asymmetry at 8086; the Known v1 limitation at 8088; v1 bound 3 at 8094). Rationale §4.2 records at 255–259; §5.24 addenda at 908–912; §5.25 at 915. Stdlib: `Math.Min/Max` (144–145), `Array<T>` (1594, `Remove`/`Contains`/`IndexOf` 1611–1614), `Console.Write/WriteLine(Object)` (102/105), `StringBuilder.Append(Object)` (1330), `EventSource<T>.Raise` (4066), `Equals(other: Object?)` (spec §8.1, line 1200; corrected by critique round 1 — stdlib line 5395 is `String`'s override, §9.6). Added by critique round 1: §27.1.1 (6328), §27.3 (6363), §27.4 (6410), §27.6 (6459), §18.5 (4006), §30.14's "Function-typed values are never sendable", §28.14 *The method-reference formation bar*.
21. **Method-reference formation (critique round 1, blocking).** `var put: (() -> int) -> () := bag.Put; put(f);` passes `f` to a function-typed parameter of a function value, which is admitted, and the erasure happens inside the formed reference. *Resolved:* the widened formation bar (§3, Option 5; §1.6, Program E).
22. **A type parameter in a nested parameter slot (critique round 1, blocking).** `Fill(reg: ((T) -> ()) -> ())`, where the body passes a storing lambda to `reg` and the caller's `lambda(cb) => cb(f)` invokes it. *Resolved:* the nested-slot admissibility rule (§3, Option 5; §1.6, Program D).
23. **`() -> Task<T>` and `() -> IEnumerable<T>` slots** (critique round 1; reading, to be probed at graduation). `T` sits positively under a frame type, not bare, so the clause's return-slot arm does not read it. The generic body's own check closes both under the class-or-interface assumption. `_v := await work()` stores a borrow of a freshly minted task's result, a temporary. An iteration variable over an interface-typed source is "conservatively the loop's implicit enumerator, a scope-bound local" (*Bare-type-parameter returns*, callee side), and storing it is rejected. *No change; the `Task<T>` case is listed for the graduation probe.*
24. **Static storage** (critique round 1). A capture-free lambda that a generic member returns could reach a static sink only through a mutable static of function, `Object` or class type. §22.12's verdict table makes a mutable function-typed static an error and confines the rest. *No change.*
25. **Aggregates** (critique round 1). The clause is keyed on a function value. An inline-array local or a function-typed choice payload holding one is not borrow-bearing (§9.7). #5377 (records) and the in-flight #5486 (choice payloads, inline-array slots, local arrays included) make those slots fields, which the soundness argument now names as a dependency. *Resolved by sequencing (§7 Q5).*

---

## 9. Critique (round 1)

Independent adversarial critique, 2026-10-07. Findings log: `scratch/v0317/p0133.critic.md`. **The recommendation stands, as amended.** The draft's Option 5 was not sound: two routes (B1, B2) carry a capture-bearing function value into a `T`-typed field without any conversion visible at a caller, so the soundness argument's premise ("the first conversion is a set-erasing position") failed. Both are closed inside Option 5's own frame, on precedents the text already has, without reopening any record in §2.

**Blocking, resolved:**

- **B1 — method-reference formation (Program E, §1.6).** `bag.Put` or `k.Keep` formed at `(() -> int) -> ()`, then called with a capture-bearing `f`. The same form, `var add := xs.Add`, also opened a v0.316 route into stdlib collections. *Resolution:* §28.14's existing method-reference formation bar is widened to a function-typed slot bound to a set-erasing callee parameter. §27.1.1's contravariance is stated not to retype a function-typed slot. Derivation (K2.4); local.
- **B2 — a type parameter in a nested parameter slot (Program D, §1.6).** The generic body hands the caller a storing function value through `reg: ((T) -> ()) -> ()`. *Resolution:* a per-instantiation admissibility rule beside the `Task`-family exclusion. Derivation (K1.6 + K2.4); carve-out; no measured cost. Its alternative (lambda-literal marking) is §7 Q6.
- **B3 — "zero tests flip" was false.** `codegen_5129.param_arg_to_unreferenced_callee_no_over_release` forwards a plain function-typed parameter to `Keep<U>(x: U)` and flips run → reject. It also exposed a cost class the draft missed: a received callback forwarded to a generic or `Object`-typed utility. *Resolution:* §5.4 names the flip and its conversion at graduation, §4.3 and Option 5's table add the cost row, §4 reason 4 is corrected, and the sweep's limits are recorded.

**Non-blocking, applied:**

- N1: the soundness argument names its dependency on #5377 and the in-flight #5486, and §7 Q5 requires the composition gate.
- N2: `T?` parameters are covered ("nullability stripped" moved into the bare-type-parameter test).
- N3: an `Object` member's receiver (`f.ToString()`, `$"{f}"`) is stated not to be a set-erasing position.
- N4: citations corrected: `sb.Append` to stdlib §3.1, `Object.Equals` to spec §8.1, the `where T: class` quotation made verbatim, sendability to §30.14, and the 2b annotation moved to the #5374 `deferred` row.
- N5: §5.4's `Wrap<U>` note re-labelled spec-ahead-of-impl, with two sibling rows added; the text's verdict needs no probe.
- N6: Option 4's value-cell backstop sentence dropped.
- N7: Option 1's dismissal checked and found fair (`mk` traced); its distance from Option 5 stated.
- N8: the return-slot arm reads nested return slots.
- N9: over-rejection completeness (registries, type tests) recorded in §4.3.
- N10 and N11: §8 items 23 and 24.

**Verified and unchanged:** 0126 §2's decline; 0132 §7's deferral bar; 0105's invariant; 0048's refactor hazard; rationale §4.2 (v0.298, v0.316 records), §5.24's #5374 addendum and §5.25 ("borrow-free by construction"); the K2.4 and §28.14 asymmetry quotations; the #5376 and #5377 v0.316 sentences; the §9.7, §3.1 and §14.2 quotations; the `DECLINED.md` #5360 `rejected` row and the *Unbounded retention tier* `deferred` row; the kernel class (derivation) and body-locality (local, plus one carve-out after B2); no conformance case flips. The Bezel programs use only members the stdlib companion declares.

---

## 10. Graduation (v0.320)

The owner accepted Option 5 on 2026-10-08 with every §7 recommendation: (1) `Raise` rejected at a function-type payload, no allowlist; (2) the `Object`-typed stdlib sinks stay rejections; (3) `scoped x: T` stays unwritable, closed as not the remedy; (4) row 5's result is capture-free at a function-type substitution; (5) graduation in the same train as #5486's local half and #5499, under the composition gate; (6) the nested-slot exclusion. The pins were re-verified against v0.319's working text, where row 4 and the field-store bullet's clause 2 gained a generic-body reading of type-parameter-typed arguments; that reading composes, since at a function-type substitution such an argument is capture-free under the clause. Differences from §6 as landed:

- §27.1.1's contravariance sentence, not §28.14's bar, carries the statement that a function-typed target slot is not retyped to a value's `Object` slot, since §27.1.1 is that rule's single normative statement; the compiler already conforms (`functionSlotConvertible` admits contravariance only between two class or interface slots).
- §28.14's caller-side sentence and the closure bullet's extension-method note are qualified for the return-slot arm, which restricts what a function-typed parameter accepts; §6 had said they were unchanged.
- *Choice payloads and inline-array slots of function type* (v0.317) is re-pointed like the record-component sentence.
- The inferred-instantiation sweep §5.4 owed found a second flip, `generic_bare_typeparam_function_value_arg_5178.static_void_method_infers_function_parameter_value` (`Util5178d.Take(f)` with `f` a plain function-typed parameter); the #5499 half flips `call_bounded_2814.store_into_local_collection_clean`, `lambda_own_param_call_bounded_1432.store_into_local_collection_accepted` and `codegen_1073_closure_arg.local_collection_sink_released_once_spec_28_14`.
- §8 item 4's return-slot-arm refinement is recorded as a `deferred` row.
- Folded in by the graduation critique (`scratch/v0320/g0133.critic.md`): a `yield return` of a function value is judged as `return` is (the closure bullet), without which row 5's capture-free result is false for a terminal accessor over an iterator that yields a closure over its own parameter; §27.1.1's return-slot covariance is stated not to retype a function-typed value slot to `Object`, closing `() -> (() -> int)` passed where `() -> Object` is expected; a `scoped` parameter declared `Object` is a set-erasing position (`Object` is a class, §3.1, so §28.14's class-typed `scoped` reaches it); and a lambda's return path is named among the `return` sites.
