# Bezel: Design Rationale

**Companion to the Bezel Language Specification, v0.139 — June 2026**

The spec describes the language. This document explains the choices that aren't obvious from reading the spec — places where Bezel diverges from its inheritance, places where two reasonable designs were possible and we picked one, and places where the spec's stated rule has a less-stated rule behind it.

This document is not exhaustive. Decisions that follow Pascal/C#/Rust convention without modification (`:=` for assignment, word keywords for boolean operators, non-virtual by default, `case` for pattern matching) are not discussed; the precedent is the rationale.

<a id="sec-contents"></a>
## Contents

1. [The big picture](#1-the-big-picture)
2. [Modules and files](#2-modules-and-files)
3. [Syntax](#3-syntax)
4. [Memory management](#4-memory-management)
5. [Ownership](#5-ownership)
6. [Nullability](#6-nullability)
7. [Equality and hashing](#7-equality-and-hashing)
8. [Inheritance](#8-inheritance)
9. [Records and classes](#9-records-and-classes)
10. [Generics](#10-generics)
11. [Pattern matching](#11-pattern-matching)
12. [Async](#12-async)
13. [Events](#13-events)
14. [Operators](#14-operators)
15. [Strings](#15-strings)
16. [FFI](#16-ffi)
17. [Things explicitly rejected](#17-things-explicitly-rejected)
18. [Things deferred](#18-things-deferred)
19. [Changelog](#19-changelog)
20. [Closing](#20-closing)

---

<a id="sec-the-big-picture"></a>
## 1. The big picture

Bezel is a systems language that tries to feel high-level. It reaches for three things at once: performance you can predict (no garbage collector, no surprise allocations), modern concurrency (async/await with structured cancellation), and a type system that catches mistakes before the program runs (non-nullable types, exhaustive `case`, generics with variance). Plenty of languages give you any two of these comfortably; Bezel's whole reason for existing is the attempt to hold all three together.

Holding them together is harder than it sounds, and the difficulty always comes back to the same place: how memory is managed. This is where Bezel's most interesting decision lives, and the clearest way to understand it is to look at Rust, the language Bezel is closest to here.

Rust keeps memory safe with its borrow checker — at compile time it allows either many readers of a value or a single writer, but never both at once, and that single rule eliminates entire families of bugs. It is a genuine guarantee, and it asks a genuine price: the rule takes real effort to learn, and it turns away many programs that would have been perfectly correct. Bezel takes a gentler path to the same destination. It keeps what matters most for a systems language — no garbage collector, with ownership written down plainly through `owned` references and `using var` — but it asks less of its compile-time checker than Rust does. The checker still follows who owns what and where borrowed references are allowed to travel ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) of the spec), so most lifetime and leak mistakes are still caught before the program runs; it simply doesn't enforce the reader-or-writer rule. The exchange is deliberate and modest: a small number of errors Rust would have caught at compile time instead show up at run time, and in return the ownership model stays simple enough to keep entirely in your head. Most of this document is that same kind of exchange, considered carefully and made again at smaller scale.

Where Bezel looks familiar, it usually is. A great deal of the syntax and many of the type-system ideas come straight from C#, which is the closest relative for nearly everything *except* the memory model. The chapters that follow are mostly about the places where Bezel leaves that inheritance behind, and the reasons each departure seemed worth making.

<a id="sec-modules-and-files"></a>
## 2. Modules and files

<a id="sec-dotted-module-names-are-one-name"></a>
### 2.1 Dotted module names are one name

`module Foo.Bar.Baz;` declares one module. The dots are punctuation in the name. `Foo.Bar` and `Foo.Bar.Baz` are unrelated modules, with no implicit visibility between them.

The alternative — making nested module names visible to each other, as C# does with namespaces — is a frequent source of confusion. Programmers assume `internal` to `Foo.Bar` is visible to `Foo.Bar.Baz`; assume `import Foo` brings in part of `Foo.Bar`. Neither is true in C#, and people get it wrong reliably. Bezel avoids the question by making the obvious reading the correct one.

The cost: no sub-package privacy. Use `internal` and put related code in the same module.

<a id="sec-no-top-level-functions-or-fields"></a>
### 2.2 No top-level functions or fields

Every method, field, and constant lives inside a class, record, or enum. Classes already group code and own static state; a parallel module-level grouping mechanism doubles the surface area without solving anything. A `public static class` of static methods is the entry point for what other languages call free functions.

Extension blocks are the one exception. They live at module level because their identity is the type they extend, not a surrounding class.

<a id="sec-syntax"></a>
## 3. Syntax

Most syntax follows Pascal/Modula-2 conventions; the precedent is the rationale. Two non-obvious choices are worth noting.

<a id="sec-always-semicolons"></a>
### 3.1 Always semicolons

Pascal famously had places where a semicolon was wrong — typically before `else` — and places where it was required, with the rule depending on what came next. The Ada designers fixed this by making semicolons unconditional: every statement ends with `;`, every block ends with `end;`, no exceptions.

Bezel follows the Ada rule. The rule is "always put a semicolon," which is impossible to get wrong by accident. The cost is occasional visual noise; the benefit is one fewer thing to remember.

<a id="sec-the-discard-_"></a>
### 3.2 The discard `_`

Most of `_` follows C#: wildcard in patterns, discard for `out` parameters. The Bezel-specific addition is the expression-statement form `_ := expression;`, which evaluates `expression` and discards its value.

The motivation is honest opt-out for fire-and-forget patterns and for deliberately-ignored results. Without it, programmers either declare a throwaway local (which then triggers the unused-variable warning) or accept the warning that the result was discarded (which trains them to ignore warnings). The discard makes the deliberate case syntactically distinct from accidentally dropping a value.

`_` is a sink, not a binding. It cannot be read.

<a id="sec-memory-management"></a>
## 4. Memory management

Bezel's ownership model is the part most likely to surprise readers. The high-level shape:

- Class instances are heap-allocated and must be disposed exactly once.
- Records, enums, and primitives need no disposal.
- A small set of types — `string`, boxed value types, function values — are **runtime-managed** and exempt from manual disposal.

The runtime is not specified. Reference counting works. Region allocators work. A small generational collector for the runtime-managed subset works. Implementations choose.

<a id="sec-why-strings-are-runtime-managed"></a>
### 4.1 Why strings are runtime-managed

Marking `string` as owned would mean `using var` on the result of `int.ToString()`, every interpolation, every concatenation. The result would be unreadable.

The alternative — value-type strings with copy-on-write — silently allocates on every concatenation and complicates interop with languages where `string` is a reference type.

The chosen path is to put `string` (and a small set of other deeply-pervasive types: boxed values, function values) in a runtime-managed region and leave manual ownership clean for the 95% of types where it actually matters. Manual ownership would be unworkable for strings; runtime management is unworkable for resources. Each gets the right tool.

<a id="sec-why-function-typed-values-are-runtime-managed"></a>
### 4.2 Why function-typed values are runtime-managed

A non-capturing lambda is just a function pointer; a capturing lambda owns its captured environment; a method reference (`obj.Method`) carries the receiver. These are heterogeneous enough that requiring callers to track ownership of every function-typed value would force `using var` on every callback.

The simple rule — function-typed values are runtime-managed — makes them as easy to use as any other value while keeping the ownership story for resource-bearing types intact.

<a id="sec-why-classes-are-non-nullable-by-default"></a>
### 4.3 Why classes are non-nullable by default

C#'s opt-in nullable reference types proved that most reference fields are non-null in practice. Bezel goes one step further: non-nullability is the default, nullable types are opt-in via `?`. The cost is `?` on a small minority of types; the benefit is that `null` is noteworthy, not the default value of every reference.

<a id="sec-ownership"></a>
## 5. Ownership

<a id="sec-owned-not-borrows"></a>
### 5.1 `owned`, not borrows

Rust's borrow checker enforces single-writer-or-many-readers at compile time. Most working programmers don't need that guarantee; they need "free this once and don't use it after." Bezel targets the second and skips the first.

`owned` marks a field, parameter, or return as on the disposal hook for its containing object. The compiler enforces that owned values reach an owned location and don't escape to non-owned references. Use-after-dispose is not statically prevented; it's caught at runtime via the idempotent `Dispose()` rule.

Trade-off: programmers can write data races and aliasing bugs that Rust would reject. They can also write a working web server in 200 lines without `Arc<Mutex<T>>`.

<a id="sec-dispose-is-not-on-object"></a>
### 5.2 `Dispose` is not on `Object`

Records, enums, and primitives don't need disposal. Putting `Dispose()` on `Object` would mean asking the user "should I dispose this `int`?" — a meaningless question. Disposal lives on classes, where the need actually exists.

Code that takes "any disposable thing" accepts `IDisposable`, not `Object`. This documents intent.

<a id="sec-collections-do-not-own-their-elements"></a>
### 5.3 Collections do not own their elements

The most-debated decision in this area. Three options were considered:

1. Collections always own their elements (Add takes ownership; Dispose disposes everything).
2. Borrowing collections (the original v0.52–v0.87 choice; remains the *default* in v0.97 alongside the explicit owning forms).
3. Collections optionally own via a generic flag.

Option 1 fails for collections of borrowed references, common when a class has a back-reference list. Option 3 doubles the API surface and forces the choice at type-construction time, which is the moment when the programmer least wants to think about it. Option 2 keeps collections universal at the cost of an explicit `DisposeElements()` call when elements should also be disposed; that's one line in a destructor.

`DisposeElements()` is on `ICollection<T>`. Asymmetric ownership (own the values, not the keys) is rare enough to be worth doing by hand.

v0.97's resolution: option 2 remains for the borrowing types (`Array<T>`, `Dictionary<K, V>`, etc.), and option 1 is provided as separately-named owning types (`OwnedArray<T>`, `OwnedDictionary<K, V>`, etc.). Users pick the form that matches their ownership intent at the declaration site. The earlier v0.88-v0.96 attempt at option 3 (an `<owned T>` generic-argument modifier with merged class declarations) was abandoned in v0.97 because the substitution machinery accumulated more complexity each revision; separate named types capture the semantic distinction directly without the substitution rules. The non-owning baseline retains its original justification — it is the right default for collections of borrows, and `DisposeElements()` remains the right escape hatch when the holding class wants to dispose elements explicitly without committing the collection's type to ownership.

<a id="sec-no-shared-ownership"></a>
### 5.4 No shared ownership

There is no `Rc<T>` or `Arc<T>`. Single ownership only.

The cases where shared ownership is genuinely needed are rare in Bezel's target use cases and are served by other patterns: handle-and-table indirection, channels, or the runtime-managed region for shared immutable data.

If shared ownership turns out to be needed, it can be added as a library type without language changes. *(v0.134: it was — `Shared<T>`/`Weak<T>`, scoped, explicit, and domain-local; see [§5.21](#sec-shared-t) and language spec [§28.13](Bezel_Language_Spec.md#sec-shared-ownership). The position of this section otherwise stands: single ownership remains the default and the only implicit regime.)*

<a id="sec-local-variable-ownership-is-inferred-not-annotated"></a>
### 5.5 Local-variable ownership is inferred, not annotated

Fields, parameters, and returns carry an explicit `owned`. Locals do not. The compiler infers each local's ownership state from how it was introduced: `using var` is owned, `var x := new T()` is owned, `var x := someField` is borrowed. The full rules are in [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) of the spec.

The asymmetry is signal-to-noise. Annotations on field, parameter, and return positions communicate API intent across module boundaries — they need to be explicit. Annotations on every local would be redundant noise: the compiler can already see what the local was initialized from.

<a id="sec-the-lattice-direction-at-control-flow-joins"></a>
### 5.6 The lattice direction at control-flow joins

The [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) lattice puts owned ⊑ borrowed: owned is the *stronger* state, borrowed the *weaker*. At a control-flow merge where two branches produced different states for the same variable, the meet would have to be the weaker state — but a meet between owned and borrowed at the same merge is rejected outright, because demoting owned to borrowed silently drops a disposal obligation.

The earlier reading (borrowed ⊑ owned, with the owned branch winning at the join) is wrong: there is no value to dispose on the borrowed branch, so the merged owned slot would carry an obligation it cannot discharge. The compiler instead requires programs to equalize the branches before the merge — either produce an owned value on both branches by allocation or `Clone`-style copy, or arrange for both branches to bind the variable to a borrow whose source outlives the merge. The latter does not mean "borrow the `using var`": an owned value discharged with `using var` in a branch must do its work and be released *within* that branch; the post-merge variable takes its value from a longer-lived source, because a borrow may never outlive the source it points into (the merge inherits, rather than escapes, that rule).

This is the standard lattice-meet rule in linear/affine type systems applied to Bezel's specific kinds. It is more restrictive than what some informal readings of v0.55 suggested, but the prior reading was unsound.

<a id="sec-factory-method-ownership"></a>
### 5.7 Factory method ownership

v0.59 left a gap in the producers table for static factory methods. A method like `File.ReadAllLines(path)` returns a freshly-allocated `Array<string>` — semantically identical to a `new` expression — but the return type carried no `owned` annotation. Under the strict reading of [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows), the return was a "borrow," but from what source? A static method has no receiver.

The v0.60 rule is: when a method returns a class type without `owned` and the compiler cannot identify a borrow source (the method is static, or returns a value documented as freshly allocated), the return is treated as owned. The compiler warns and recommends that the callee add `owned` to its return type.

The `owned` annotation remains the preferred mechanism. The compiler-inferred rule exists as a safety net for two reasons: (1) third-party libraries may not annotate their returns, and (2) adding `owned` to every factory method across the standard library in one step would be a large surface-area change. The v0.60 standard library adds `owned` to the most common factory methods; future revisions will complete the pass.

<a id="sec-collection-literal-ownership"></a>
### 5.8 Collection literal ownership

A collection literal `[1, 2, 3]` targeting `Array<T>` (or another collection class) allocates a heap object. It is semantically a `new` expression with initialization — the fact that the syntax is a literal does not change the allocation behavior. v0.59 did not list collection literals in the producers table, leaving the ownership of `var xs := [1, 2, 3];` unspecified.

v0.60 adds collection literals to the producers table as owned values when targeting a heap collection class. The rationale is straightforward: the literal creates a fresh object, so the binding must own it. The same discharge rules as `new` apply. (Collection literals targeting inline arrays `[N]T` (v0.98+) produce value-typed inline arrays, with no heap ownership; they participate in the move-only category only when the inline-array slots are themselves `owned`.)

<a id="sec-trivial-dispose-classes"></a>
### 5.9 Trivial-dispose classes

Not every class holds resources. `Random` holds only an internal seed. `StringBuilder` holds a buffer that is plain memory. The `Encoding` singletons hold nothing at all. Under v0.60's strict rules, `new Random()` required `using var` or triggered a warning — adding ceremony for a no-op dispose.

The runtime-managed exemption ([§28.9](Bezel_Language_Spec.md#sec-runtime-managed-types) of the spec) covers `string`, boxed values, and function types — types the language itself creates implicitly and pervasively. Extending that exemption to every resource-free class would be unsound: a class that is resource-free today may gain a destructor or owned field tomorrow, and code that assumed no disposal would silently leak.

v0.61 takes a middle path: **trivial-dispose** classes — those with no destructor, no `owned` fields, and no non-trivial base — receive a reduced-severity ownership warning (informational, not standard). `using var` is still accepted and is still the recommended form for code that wants to be robust against future changes to the type. But a bare `var r := new Random()` no longer reads as a probable bug.

The classification is conservative: adding a destructor, an owned field, or an owning-inline-array field ([§15.1.1](Bezel_Language_Spec.md#sec-owning-inline-arrays) of the spec) to a trivial-dispose class in a future revision surfaces previously-suppressed warnings. API authors should treat such additions as source-visible changes.

<a id="sec-the-move-expression-on-owned-fields"></a>
### 5.10 The `move` expression on owned fields

Through v0.86, ownership transfer in Bezel was *single-shot*: a value moved from a `using var` local to an `owned` parameter, from an `owned` parameter to an `owned` field, from an `owned` field to memory at destructor time. Each transition was implicit, named by the syntactic position. There was no transition for "take this owned field's value out and return it to a caller."

The reason was a deliberate conservatism. Field reads always produced borrows ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) of the spec), because fields are *retained*: a class instance exists to be acted upon multiple times, and the natural operation on a field is "read while leaving the value in place." Owning vs. borrowing was a property of the read site's intent, but every field read promised the same thing — the field's value is still there afterward. A take-out operation on a field would have to invalidate the field's contents, and the spec had no machinery for tracking "this field is currently empty."

The cost of this conservatism was visible. The standard pattern "replace a resource and return the old one" (a transactional update on an owned field) had no clean form. The v0.85 spec showed a `Utils.Swap`-via-`ref` workaround that exchanged the field with a freshly-built local; v0.86 documented the gap explicitly when a related case (batch-aggregating `Task<owned T>` results) had no workaround at all. Each acknowledgement made the gap more visible.

v0.87 closes the field-take case with `move x.f`. The expression reads the field as an owned value and leaves the field in *moved-from* state. The compiler tracks the moved-from state across the rest of the method body and rejects any path that exits the method without first reassigning the field. The rule is the same definite-assignment analysis Bezel already performs for non-nullable locals, lifted to the field level.

The design choice is to make the move *explicit*. A bare `var x := _resource;` continues to produce a borrow as it always did; only the explicit `move _resource` form transfers ownership out. Three reasons for the explicitness:

1. **Visibility in code review.** A reader scanning a method body learns the local effect of each line. With explicit `move`, the take-out is announced at the read site; the reader does not have to reason about subsequent lines to discover that the read was actually a take. This matches Bezel's existing preference for syntactic visibility — `using var` declares scope-bound disposal at the binding site, `owned` declares ownership transfer at the type position, `out` declares output at the call site. `move` is the analogous keyword for the take-from-field operation.

2. **Refactoring stability.** A version of the rule that *inferred* take-out from the pattern "read followed by reassignment" would be sensitive to insertions between the two lines. Adding a `Log(...)` call between read and reassignment, or extracting the reassignment into a helper method, would silently change whether the read was a borrow or a take. Programs that compiled would compile differently after small edits. Explicit `move` makes the take-out a user decision; inserting a log call between the move and the reassignment does not change the move's semantics.

3. **The exception-path obligation.** The field-restoration rule has to handle exception edges as well as normal control flow: if a throwable expression appears between the move and the reassignment, the throwing edge is an exit path on which the field has not been restored. The fix is the *build-before-take* discipline — construct the replacement value first, then take, then assign — which is the same discipline RAII code in C++ and transactional code in functional languages already commits to. Making the move explicit lets the spec write down this rule without forcing the user to discover it through experiment.

The `move` form is permitted only on owned instance fields, not on locals (which already have the implicit move-on-read semantics), not on static fields (whose lifetime crosses methods, breaking the field-restoration rule's scoping), and not on collection elements (which are not owned slots in this version). The first two restrictions are by design; the third is open — a future revision that admits `Array<owned T>` ([§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument)) would naturally extend `move` to collection-indexed slots.

The field-restoration rule's analysis reuses the existing definite-assignment machinery. The compiler's flow analysis already tracks "is this non-nullable local definitely assigned on every path that reads it." The same machinery applied to the inverse question — "is this owned field definitely reassigned on every path that exits after a move" — needs no new graph algorithms, only a new state on field nodes. This is why `move` was a feasible v0.87 addition: the cost on the implementation side is small and localized.

v0.87 originally shipped without addressing the collection-of-owned case — `Array<T>` did not own its elements, so `move arr[i]` had no slot to move from. v0.88 closes that gap by extending language spec [§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument)'s opt-in list to admit `Array<owned T>` and the other standard collection types; the next subsection covers the rationale.

<a id="sec-owning-collections"></a>
<a id="sec-owned-element-collections"></a>
### 5.11 Owning collections

For nine revisions (v0.78 through v0.96), Bezel's collection types were uniformly non-owning (`Array<T>`, `Dictionary<K, V>`, etc., storing borrows), with a separate **`<owned T>` instantiation** form admitted for the standard collection types. `Array<owned Connection>` was a distinct type from `Array<Connection>` and owned its elements; the language spec [§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument) of those revisions defined an opt-in list of generic types that could accept the `owned T` argument.

v0.97 replaces the `<owned T>` mechanism with **separate named classes**. `OwnedArray<T>`, `OwnedLinkedList<T>`, `OwnedSet<T>`, `OwnedStack<T>`, `OwnedQueue<T>`, `OwnedDictionary<K, V>`, plus the owning task type `OwnedTask<T>` and the owning iteration interfaces `OwnedEnumerable<T>` / `OwnedEnumerator<T>` (and async variants), are first-class types in the standard library. The borrowing collection types retain their existing names. The `owned` modifier no longer propagates through generic type arguments.

**Why the change.** The `<owned T>` mechanism accumulated language machinery across revisions: an opt-in list (v0.78), conditional inheritance semantics (v0.95), substitution rules and form-conditioning constraints (v0.96), an inheritance-projection rule (also v0.96). Each addition fixed a specific issue and introduced a corner case for the next revision to address. By v0.96 the spec had three substitution rules, two new constraint forms, a special inheritance-projection rule, and ongoing concerns about how future features (LINQ over owning collections, user-defined owning containers) would interact with the mechanism.

The underlying observation: an owning collection's `Add` takes ownership of its argument; a borrowing collection's `Add` takes a borrow. These are *genuinely different operations*, not two specializations of one operation. Trying to express the difference as a parameter modifier substituted through a single generic forced two unrelated semantics through one syntactic vehicle. Separate types capture the semantic distinction directly.

**What was lost.** The `<owned T>` mechanism let one generic algorithm work over both ownership forms. A `Where<T>(items: IReadOnlyList<T>)` would accept both `Array<MyClass>` and `Array<owned MyClass>` via type inference (T inferred as `MyClass` and `owned MyClass` respectively, with the v0.96 inheritance projection stripping owned at iteration points). In v0.97, the generic still accepts both — `Array<T>` and `OwnedArray<T>` both implement `IReadOnlyList<T>` — but T is now always `MyClass`, not parameterized over the ownership form.

For read-only access (the common case), this is no loss: the generic works identically. For *mutating* algorithms over either kind, the user picks one mutating interface (`IList<T>` for borrowing; `IOwnedList<T>` for owning) and parameterizes over that. Code that wants to abstract over both mutating forms isn't expressible in v0.97 — but on reflection, such code is rare and would face the same semantic dilemma the language faced ("does `Add` take ownership?") just at the user's call site rather than at the language layer.

**What was gained.** Substantial simplification:
- The language spec [§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument) opt-in mechanism, substitution rules, and constraint forms are gone.
- The `IEnumerator<owned T>` destructive-Current rule becomes a normal interface declaration (`OwnedEnumerator<T>` has destructive Current by its own definition; `IEnumerator<T>` doesn't).
- User-defined owning containers are now expressible via composition over `OwnedArray<T>` (or another owning primitive), without any opt-in mechanism.
- The Dictionary "four combinations" complication (v0.96 admitted `Dictionary<owned K, V>`, `<K, owned V>`, `<owned K, owned V>`) collapses: `OwnedDictionary<K, V>` covers the one realistic case (V is owned; K is value-like).
- The `Stack.Pop` and `Queue.Dequeue` "return-type form-conditioning" mechanism in v0.96 becomes a simple separate declaration on the owning class.

**User-defined owning containers.** A user who wants their own owning container builds it by composition over an `OwnedArray<T>` (or other owning primitive):

```bezel
public class OwnedCache<T> where T: class
  private owned _items: OwnedArray<T>;

  public constructor() _items := new(); end;

  public method Add(owned item: T) => _items.Add(item);
  public method Drain(): OwnedEnumerable<T> => _items.Drain();
end OwnedCache;
```

This works under existing field-ownership and generic-type rules. The user's class doesn't need any special opt-in or compiler magic — `OwnedArray<T>` is a primitive whose semantics are spec-defined, and the user inherits those semantics by storing one. The "deferred problem" of v0.78-v0.96 (how can user-defined types opt in to owning generics?) is now solved by simplification rather than by adding more language.

**LINQ over owning collections.** Still deferred, but on different grounds. v0.96 deferred it because the design questions (does `Where`'s predicate take a borrow or owned? what does `Select`'s output look like for owning sources?) interacted with the substitution rules in complicated ways. v0.97 simplifies the question: LINQ extension methods can be declared in two families — one over `IEnumerable<T>` (borrowing), one over `OwnedEnumerable<T>` (owning) — with appropriate signatures for each. The work is mechanical: each LINQ method gets two declarations. A future revision will add the owning family when there is a sufficiently complete design.

**Migration.** Code written for v0.78-v0.96 that uses `<owned T>` instantiations needs to be rewritten with the new type names. Every `Array<owned T>` becomes `OwnedArray<T>`. Every `Task<owned T>` becomes `OwnedTask<T>`. Every `IEnumerable<owned T>` becomes `OwnedEnumerable<T>`. The change is mechanical and pervasive but each replacement is local — there are no semantic shifts that require rethinking algorithm design.

<a id="sec-pattern-b-loosening-and-iterator-yield-refinement"></a>
### 5.12 Pattern B loosening and iterator-yield refinement

Two of v0.89's changes refine ownership rules that v0.88 had introduced: the Pattern B consumer rule for `Task<owned T>` (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)), and the iterator-yield-ownership rule for owned-class-typed values (language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for)'s "Iterator yield ownership" subsection). Both are reactions to inconsistencies that surfaced once v0.88's `WhenAllOwned` example had to be written end-to-end.

**Pattern B's "immediate operand of `using var`" rule was over-restrictive.** From v0.74 through v0.88, the rule for awaiting a `Task<owned T>` for class T said the await must be the immediate operand of `using var` or `await using var`. The rule's intent was clear: ensure that the owned result reached an owned location with a definite discharge path. The `using var` form was the canonical such location, so naming it directly was the simplest expression of the rule.

But the `using var` form is one of several owned-consuming contexts the type system already recognizes. An owned-consuming context is any position in the AST where the surrounding expression's type system requires an owned value to be discharged: `using var` is one; an `owned` parameter of a method call is another; an `owned` return expression is a third. In each of these contexts, the type system already enforces that ownership is discharged after the value reaches that position — the same property `using var` provides. The Pattern B rule was distinguishing a single owned-consuming context (`using var`) from the others without a principled reason.

The cost of the over-restrictive rule became visible when v0.88's `WhenAllOwned` example tried to write `results.Add(await task)` — a natural shape, type-correct under the broader rule, rejected under the original rule. The workaround required an intermediate `using var t := await task; results.Add(t);` — three statements where one would do, with the intermediate variable serving no purpose other than satisfying the rule.

v0.89 loosens the rule to admit any owned-consuming context. The safety property is unchanged: every owned-consuming context is one whose type system already requires the value to be discharged, so the rule continues to prevent owned values from being silently dropped. The change is purely about admitting more syntactic positions where the await can appear.

**The iterator-yield-ownership rule needed a split case for `IEnumerable<owned T>`.** v0.78's `IEnumerable<T>` iterator-yield rule said that an iterator yielding owned class-typed values has the enumerator own each yielded value, with the consumer's iteration variable being a borrow valid only for the iteration body. This was the right rule when iterators returning `IEnumerable<owned T>` did not exist — the rule applied uniformly to any iterator that *yielded* owned values, regardless of the iterator's return type.

v0.88 admitted `IEnumerable<owned T>` to language spec [§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument)'s opt-in list. Iterators returning `IEnumerable<owned T>` are now possible and useful — `Drain()` on every owned-element collection is implemented as such. But the v0.78 rule applied to them too, which produced a contradiction: language spec [§28.11](Bezel_Language_Spec.md#sec-collection-element-ownership) said the consumer's iteration variable from `coll.Drain()` is owned, but language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for)'s "Iterator yield ownership" said it is a borrow. Both rules cannot be right.

v0.89 splits the owned-class-typed-yield rule into two cases, distinguished by the iterator's declared return type:

1. **Iterator returns `IEnumerable<T>` (no `owned` modifier on T).** The original v0.78 rule applies: enumerator owns each yielded value, consumer borrows. This is appropriate when the iterator's contract is "let the consumer look at each value briefly" — filtering, formatting, dispatch.

2. **Iterator returns `IEnumerable<owned T>`.** Ownership transfers from enumerator to consumer at each `MoveNext`. The consumer's iteration variable is owned and must be discharged in the loop body. This is the rule that makes `Drain()` work.

The split is determined at compile time by the iterator method's return type. The same physical implementation (a state machine that pops elements from the source one at a time) compiles to different consumer-side semantics depending on which signature it presents.

The split rule's complexity is justified because the two cases address distinct use patterns. A filter iterator that yields owned values (perhaps because it constructs them on the fly) and wants the consumer to borrow each one briefly is one valid pattern; a drain iterator that yields owned values and wants the consumer to take responsibility is another. Forcing both into one rule would require either making every consumer-borrow scenario also accept the discharge obligation (adding ceremony to filtering use cases) or making every consumer-owned scenario accept the borrow restriction (preventing drain use cases entirely). The split lets each pattern be expressed naturally.

The error-message-side complexity is small: an iterator's static signature is known at the call site, and the compiler can produce a clear "this is `IEnumerable<owned T>` — the iteration variable is owned" message when the consumer fails to discharge.

<a id="sec-no-block-expressions"></a>
### 5.13 No block expressions

Bezel does not provide a value-returning `do ... end` form. `do ... end` is a statement-sequencing block with type `()` everywhere it appears — in method bodies, in `case` arms at statement position, in `for` and `while` bodies, and as the body of a multi-statement lambda. Multi-statement logic in expression-position contexts (most visibly, `case` arms inside `var x := case ...`) is written as an extracted helper method or a local lambda, both of which produce values.

Earlier revisions of this spec flagged the absence of block expressions as an "ergonomic gap" and anticipated a future revision adding a value-returning form. v0.93 closes the question: the gap is real but the answer is to leave it. The reasoning, briefly:

**The alternatives all carry real costs.** We considered four shapes for a value-returning block expression:

1. *Rust-style trailing-expression-without-semicolon* (`do { stmts; final-expr } end`). This fights Bezel's mandatory-semicolon rule ([§1.6](Bezel_Language_Spec.md#sec-statements-and-termination)). A trailing expression without a semicolon, in a language otherwise strict about semicolons, would be a foreign element learners would trip over.

2. *Explicit `yield expr;` to produce the block's value.* This reuses `yield`, which is already a keyword for iterator methods (`yield return`, `yield break`). Code inside an iterator method that also contained a block expression would have two scopes the `yield` could target, creating a resolution rule learners would need to track.

3. *A new keyword introducing the block* (`expr ... end`, `compute ... end`, or similar) with `return` exiting the block. This requires `return` to have scope-local semantics — exits the innermost enclosing return-bearing scope — which is one more rule on top of method/lambda return scoping. It also expands the language's keyword surface for a use case the helper-method extraction already covers.

4. *A new keyword for the value-producer statement* (`give expr;` or similar) inside `do ... end`, with the block becoming value-returning iff a `give` appears. This introduces a new keyword and creates a third interpretation of `do ... end` (the value-bearing one), partially undoing the consistency the current design has.

Each option has at least one cost we judge larger than the cost of the helper-method extraction it would replace.

**The helper-method extraction is good design independently.** Pulling a multi-statement arm body into a named method gives the per-arm logic a name. Names are valuable: a reader can scan the `case` and see what each arm does without descending into the body. The extracted helper also localizes any side effects the arm performs, which is the harder thing to track — far harder than the syntactic awkwardness of writing the helper. A spec that requires extraction is steering toward better-structured code.

**The use case is uncommon.** In practice, `case` arms are short — a literal, a method call, an arithmetic expression. Arms with substantial logic are rare; when they occur, they usually benefit from a name. Adding language machinery for a rare case is poor leverage.

**The single-meaning `do ... end` is itself a feature.** Across the language, `do ... end` does one thing: sequence statements. It introduces a fresh scope and has type `()`. This consistency makes the construct easy to learn and easy to reason about — wherever you see `do ... end`, you know what it does. Admitting a value-returning variant, regardless of which option above, would make the construct's meaning context-dependent: sometimes value-less, sometimes value-bearing, with the distinction visible only by reading the body or surrounding context. Single meaning beats two meanings here.

The [§19.2](Bezel_Language_Spec.md#sec-arms) documentation reflects this decision: `do ... end` is described as statement-sequencing, the helper-method extraction is the recommended pattern for multi-statement arm bodies, and no "future revision" promise is carried.

**v0.126 addendum: the bare-statement arm form is not a block expression.** v0.126 added a third arm-body alternative — `=> Statement` — alongside the existing `=> Expression` and `=> do … end` forms, for arms in statement-position `case` and in `catch` arms. This removes the ergonomic friction of writing `=> do _x := y; end` for a one-line assignment-shaped arm body; the new spelling is `=> _x := y;`. The change does *not* relax §5.13. The four declined designs above were all ways of producing a value from a multi-statement block; the v0.126 addition is a syntactic convenience for the single-statement case in arm bodies that already accept `do … end` (which has type `()` and produces no value). No new value-producing construct exists; `do … end` retains its single meaning; the helper-method extraction is still the idiom for multi-statement arms in expression-position `case`. The question §5.13 settled — whether to add value-returning blocks — was re-examined during the v0.126 cycle and the same conclusion was reached: the four candidate designs still carry the same costs, the helper-method extraction still has independent merits, and `do … end`'s single meaning is still worth preserving. The v0.126 change addresses a different question (single-statement ergonomics in arm bodies) which §5.13 did not consider.

<a id="sec-inline-arrays-and-array-kind-split"></a>
<a id="sec-inline-arrays-and-the-array-kind-split"></a>
### 5.14 Inline arrays and the array-kind split

v0.98 introduces inline arrays `[N]T` and retires the previous `T[]` (fixed-size, runtime-managed) form. The change has three motivations and four supporting design decisions.

**Foundational implementability.** Up through v0.97, the standard library's owning collections (`OwnedArray<T>`, `OwnedDictionary<K, V>`, etc.) were compiler intrinsics — their slot-take operations on the backing storage could not be expressed in pure Bezel because `T[]` did not admit per-element ownership tracking. This left the language's most safety-critical primitives in a "trust the compiler" position with no path for users to verify or extend. With v0.98's `[N owned]T?` form, the *per-slot ownership-transfer operations* — `move arr[i]` and the field-restoration discipline that goes with it — became expressible at the user level. For fixed-capacity owning containers, this was sufficient. The remaining intrinsic surface was the *runtime-sized backing storage* used by resizable owning collections like `OwnedArray<T>` and `OwnedDictionary<K, V>`; `[N owned]T?` requires a compile-time N. v0.99 closes that gap by introducing `OwnedBuffer<T>`, a class wrapping runtime-sized owning storage with the same per-slot semantics extended to runtime-determined length; see [§5.15](#sec-owned-buffers-and-spans) for the full story. After v0.99, the intrinsic surface of the entire owning-collection family reduces to one well-scoped primitive (`OwnedBuffer<T>`) plus the inline-array form (`[N owned]T?`); every resizable owning collection composes over these in pure Bezel.

**Disposal-discipline clarity.** `T[]` straddled two categories: it was a reference type (allocated with `new`) but was runtime-managed (no `Dispose`). This was an inconsistency users had to learn — every other heap-allocated reference type in Bezel has a real `Dispose` contract. v0.98 cleans this up: heap arrays (`Array<T>`, `OwnedArray<T>`) are classes with real disposal; inline arrays are value types stored inline. The "runtime-managed" exception for arrays goes away. The remaining runtime-managed types are `string`, function values, and boxed values, which are genuinely interned/runtime-tied at the implementation level in ways `T[]` was not.

**FFI ergonomics.** Inline arrays have C-equivalent layout (packed elements, no header). The previous `T[]` had a Bezel-level header, requiring marshalling cost to convert to C arrays at FFI boundaries. Inline arrays `[N]byte` and similar mean a Bezel `extern method foo(buf: [16]byte)` declaration matches `void foo(char buf[16])` in C with no marshalling. This is a real performance and clarity win for FFI-heavy code, and was repeatedly cited as a gap during v0.97 verification.

The supporting design decisions:

**The array-kind split.** v0.98 commits to two array kinds: inline `[N]T` (value-typed, fixed size at compile time, no allocation) and heap (`Array<T>`, `OwnedArray<T>`, class types, real `Dispose`). Users pick by use case: "I want a value-typed buffer" picks inline; "I want a resizable container that can grow" picks heap. A separate "heap fixed-size" type (à la `FixedArray<T>`) is deliberately not provided — heap arrays support fixed-size discipline by not calling resize methods. The taxonomic simplification keeps two array kinds (one syntactic form per kind) rather than four.

**Move-only value types.** Inline arrays with `[N owned]T?` or `[N owned]T` slots own their slot contents (the per-slot reference is non-null on `[N owned]T`, and may be null or non-null on `[N owned]T?`). Copying such an array would duplicate ownership of those contents, so copying is forbidden — the array is move-only. v0.98 introduces "move-only value types" as a small new type-system category, scoped tightly: only owning inline arrays exhibit it. This is a real new concept but it's foundational (move semantics for owning slots are unavoidable) and the rules are localized to one form. A future revision may extend the move-only category if user-defined move-only types prove necessary; this version does not anticipate that need.

**Collection literals.** The literal `[1, 2, 3]` is now contextually typed: it produces whatever target type the context names (`[3]int`, `Array<int>`, etc.). A literal in a position with no target type is a compile error, which makes the construction site explicit about what's being built. The contextual typing reuses the same machinery as `null` (untyped null) and is the right cost-benefit tradeoff for keeping the literal syntax both flexible and unambiguous.

**Migration.** The change requires every `T[]` reference in user code to be rewritten as either `Array<T>` (the common case — most fixed-size T[] uses were just "I want a sequence of T"), `OwnedArray<T>` (when the element type was owned), or `[N]T` (when truly fixed-size and inline storage is wanted). The standard library was swept once for v0.98; user code makes the corresponding edits on its own schedule. The mechanical conversion is supported by the compiler's diagnostics, which flag `T[]` references with explicit guidance on the replacement form.

<a id="sec-owned-buffers-and-spans"></a>
### 5.15 Owned buffers and spans

**The current shape (as of v0.109).** Three families of types provide runtime-sized buffer storage and views over it:

| Type | Role |
|---|---|
| `OwnedBuffer<T>` ([§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)) | Owning fixed-at-construction storage. Class T: slots are owned `T?`. Value T: slots are `T` directly. |
| `Span<T>`, `ReadOnlySpan<T>` ([§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)) | Views over `OwnedBuffer<T>`. Read-write and read-only respectively. |
| `ArraySpan<T>`, `ReadOnlyArraySpan<T>` ([§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)) | Views over heap `Array<T>` (borrowing collection). Uniform mutation API across class/value T because slots are borrows. |
| `ReadOnlyStringSpan` ([§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)) | Read-only view over a contiguous range of code points in a `string`. |
| `ReadOnlyAsciiByteSpan` ([§3.4](Bezel_Standard_Library.md#sec-readonlyasciibytespan)) | Read-only byte view over an ASCII string (UTF-8 byte access without allocation). |
| `IReadOnlySpan<TElement>` ([§4.16](Bezel_Standard_Library.md#sec-ireadonlyspan)) | Interface unifying the read-only surface across all six concrete span types. Implemented by all of them. |

All spans are borrow-bearing records carrying a borrow of their source (`OwnedBuffer<T>`, `Array<T>`, or `string`); the borrow-bearing-record rule of language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) gives them lifetime tracking through existing machinery. Indexer access is bounds-checked against the span's `Length` (as of v0.105). Span construction is non-allocating record construction; the standard entry points are `AsSpan` / `AsReadOnlySpan` methods on the source (`string.AsSpan()`, `Array<T>.AsSpan()`, `OwnedBuffer<T>.AsSpan()`, etc.) and the explicit constructors.

The remainder of this section explains how each piece was added and the trade-offs that produced the current shape. Readers wanting the operational details should follow the §-references into the standard library document.

---

v0.99 introduces two stdlib types: `OwnedBuffer<T>` (a class wrapping runtime-sized owning storage) and `Span<T>` / `ReadOnlySpan<T>` (records carrying views into a buffer; `Span<T>` is read-write as of v0.100, `ReadOnlySpan<T>` is read-only). v0.102 generalizes all three to admit value-typed T in addition to class-typed T. Together they complete what v0.98 started — making the owning-collection family expressible in pure Bezel — and add a safe, no-`unsafe`-keyword escape hatch for working with raw storage of either category.

**The gap v0.98 left.** v0.98's `[N owned]T?` admitted per-slot ownership semantics at the user level for fixed compile-time N. Resizable owning collections — `OwnedArray<T>`, `OwnedDictionary<K, V>`, `OwnedSet<T>`, etc. — need runtime-sized owning storage. Until v0.99, the standard library's resizable owning collections remained compiler intrinsics: the per-slot operations *could* be expressed in user Bezel (via the [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) move-and-null pattern), but the *underlying runtime-sized buffer* could not — no language syntax expressed "a class field of N owning slots where N is a constructor parameter."

**Why a class, not new language syntax.** A natural alternative would have been to extend inline arrays to admit a runtime-sized form, perhaps `[?owned]T?` with `?` denoting "runtime-determined." That would have been one additional language construct. Adding a stdlib class is the smaller intervention: the class is one new type, its surface is the standard indexer-and-move pattern that already applies to owning collections, and the language adds nothing new. The intrinsic boundary stays inside the standard library rather than expanding into the type-system primitives.

**The class/value category split (v0.102).** v0.99 restricted `OwnedBuffer<T>`, `Span<T>`, and `ReadOnlySpan<T>` to class-typed T. The restriction kept the v0.99 design tight: slots are `T?` (nullable references), the buffer owns each non-null slot's referent, `move buf[i]` is admitted with field-restoration, the destructor cascades. v0.102 extends to value-typed T (primitives, records, enums, non-move-only inline arrays). For value T the semantics simplify substantially: slots are `T` directly (no nullability wrapper); the setter just overwrites; `move buf[i]` is not admitted (values have no ownership to transfer); the destructor performs no per-slot work, only releasing the buffer's allocation. The two presentations are different specializations of the same generic class — the compiler resolves T's category at instantiation and emits the appropriate semantics.

The motivating use case was binary buffers: `OwnedBuffer<byte>` for file-format parsing, network protocols, image headers. Under the v0.99–v0.101 design these did not exist — `byte` is value-typed and the class constraint excluded it. Workarounds (use `Array<byte>`, copy data, etc.) all imposed costs the design should not have required. The honest fix is to admit value-typed T at the language-design level rather than continuing to ship a class-only buffer family.

The cost of the generalization is a small amount of additional design machinery: the slot type and several method signatures dispatch on T's category. The compiler must distinguish at instantiation time between "T is a class type" (existing semantics) and "T is a value type" (new semantics). This is *type-parameter-category-dependent specialization* — a new kind of generic specialization that previously did not exist in Bezel. v0.102 admits it as a compiler-known behavior specific to these three types (`OwnedBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>`); it is not a general user-available mechanism. Future revisions may generalize the mechanism if other types benefit, but that is not committed work.

**Why move-only value types are not yet admitted.** The owning inline arrays `[N owned]T` and `[N owned]T?` are value-typed but move-only (rationale [§5.14](#sec-inline-arrays-and-array-kind-split) and language spec [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types)). A buffer slot containing a move-only value type would itself be move-only — the slot setter would need to be `set(owned)`-equivalent for the inline-array contents, with the previous slot's owned slots being disposed before the new is set. This is implementable but adds a third category to the slot-semantics dispatch (class T, non-move-only value T, move-only value T) without unlocking a clearly-motivated use case. Deferred. Users who need a runtime-sized array of fixed-size owning arrays today can express it manually with `OwnedArray<MyOwningBoxClass>` or similar wrappers.

**Why `OwnedBuffer<T>`'s slots are nullable for class T.** The class-T buffer's slot type is `T?` rather than `T`. The slot-take pattern `move buf[i]` produces an owned `T?` and leaves the slot moved-from; the standard restoration is `buf[i] := null`. Non-nullable slots would require restoration via "assign a fresh owned T," which is inconvenient when the slot's intended state is "empty after take." Resizable owning collections need precisely the "empty after take" semantic — `OwnedArray<T>` shrinks its used range without disposing the underlying buffer, `OwnedDictionary<K, V>`'s buckets routinely hold null between rehashings. The nullable form serves these naturally. For value T the question doesn't arise — `move` isn't admitted, so there's no "empty after take" state to express, and slots are `T` directly.

**Why `Span<T>` and `ReadOnlySpan<T>` are records, not a `ref struct`.** C#'s `Span<T>` is a `ref struct` — stack-only, cannot escape to the heap. This gives it the strongest possible lifetime guarantees but introduces a new type kind that pervades the language (rules for where ref structs can appear, how they interact with generics, etc.). Bezel's spans are records carrying a borrow of an `OwnedBuffer<T>`; the existing borrow-bearing-record rule ([§9.7](Bezel_Language_Spec.md#sec-records-and-disposal) of the spec) gives them lifetime tracking through machinery the language already has. The trade-off: Bezel spans participate in the standard borrow-lifetime tracking — they can be locals, parameters, return values, and (where the lifetime tracker can statically prove the constraint holds) fields of longer-lived classes; they can also cross `await` points in async methods provided the underlying buffer's lifetime extends past the suspension (typically by holding the buffer as an `owned` field of the async method's enclosing class or as a `using var` whose scope outlives the awaits). Programs whose buffer lifetimes cannot be proven sufficient are rejected by the borrow tracker. C# `Span<T>` cannot appear in classes or async state machines at all; Bezel `Span<T>` can, with the lifetime tracker enforcing the constraint. This trade follows the same logic v0.98 applied to inline arrays — prefer existing mechanisms (borrow-bearing records, lifetime tracking) over new type-system primitives.

**Why no `unsafe` keyword.** C# pairs `Span<T>` with `unsafe` blocks for the cases where the developer needs raw-pointer access. Bezel's path is different: the user-facing surface never exposes raw pointers, pointer arithmetic, or untyped memory access. `OwnedBuffer<T>` allocates through `new` (a normal class constructor), indexes through a bounds-checked indexer, and disposes through the standard destructor cascade. `Span<T>` indexes through the buffer's indexer. Operations that would need `unsafe` in C# — direct memory writes, byte-swapping, type punning — go through FFI: declare a C function that does the work and call it. The FFI surface ([§32](Bezel_Language_Spec.md#sec-foreign-function-interface) of the spec) already accepts inline arrays as FFI-boundary types, so the C interop is direct. The cost is one extra named function per low-level operation, versus a free-for-all `unsafe` block; the benefit is that Bezel's user-facing language remains fully checked.

**What this does not solve.** Two deliberate gaps remain in the current span family:

1. *Stack-allocated runtime-sized buffers* (C# `stackalloc`). Bezel's inline arrays handle fixed-size cases; runtime-sized stack allocation would require new language machinery and a story for how the stack-bound lifetime interacts with closures. Deferred — currently low priority.
2. *`Span<T>` over inline arrays `[N]T`*. Would need a new borrow-source kind tying the span's lifetime to a stack frame, with rules for closure capture, exception unwinding, and recursive calls. Substantial design work for a narrow performance gain. Deferred indefinitely; revisit if a specific need arises.

Earlier gaps that have since been closed: value-typed T (v0.102), byte-buffer accessors (v0.103), string-source spans (v0.104, v0.106), heap `Array<T>`-source spans (v0.107). See the chronological narrative below.

**The read/write split.** `Span<T>` is the read-write view; `ReadOnlySpan<T>` is the read-only view. The split was deferred briefly in v0.99 (which shipped both types as read-only) and resolved in v0.100 by giving `Span<T>` an indexer setter that delegates to the underlying buffer's owned-setter. The mechanism is the standard one: the span carries a borrow of the buffer, and calling a setter through a borrowed reference is the same operation as calling any instance method on a borrowed reference. No new lifetime rules are needed. `ReadOnlySpan<T>` remains the read-only variant; its absence of a setter is what makes it usable as an API-boundary contract for "this code will not mutate the underlying buffer through the span."

**API surface expansion.** v0.101 thickens the span surface to support common use cases without going through the underlying buffer: `IsEmpty`, `IndexOf`, `Contains`, `SequenceEqual` (search and equality on both span types); for-loop integration (a direct-indexed-loop desugaring rule in language spec [§29.2](Bezel_Language_Spec.md#sec-desugaring-of-for), parallel to the inline-array rule). For class-typed `Span<T>`: `Clear`, `MoveFrom`, `Reverse` (bulk mutation by ownership transfer; no `Fill`/`CopyFrom` because class references cannot be implicitly cloned). For value-typed `Span<T>` (added in v0.102): `Fill`, `CopyFrom`, `Reverse` (bulk mutation by value copy; no `Clear`/`MoveFrom` because values have no "empty" state and no ownership transfer). The category-dependent mutation API is a direct consequence of the ownership-vs-value-semantics distinction.

**Byte-buffer accessors (v0.103).** When T is `byte`, the spans are commonly used for binary I/O — file formats, network protocols, image headers, serialization. v0.103 adds endianness-aware reads and writes for the 16/32/64-bit integer types (signed and unsigned) and IEEE 754 floats on `Span<byte>` and `ReadOnlySpan<byte>`, via extension methods (stdlib [§4.14](Bezel_Standard_Library.md#sec-byte-buffer-accessors)). The methods name the endianness — `ReadInt32LittleEndian`, `WriteUInt64BigEndian`, etc. — rather than taking an endianness parameter or relying on a platform default; both alternatives invite the class of bugs where someone forgets that a particular format uses a specific byte order. Each method has a `Try*` variant that returns `false` instead of throwing on insufficient input, for incremental parsers that treat short input as end-of-stream rather than as an error. No offset parameter is provided; callers slice first (`header.Slice(8).ReadInt32LittleEndian()`), keeping the access model uniform with the rest of the span API. The implementation is pure byte-shuffling for integer types and routes through `BitConverter` ([§2.12](Bezel_Standard_Library.md#sec-bitconverter)) for floats. The choice to put `BitConverter` in `System` rather than gating it behind FFI keeps the user-facing language free of bit-level intrinsics while still admitting the operations that serialization, hashing, and exact float comparison need.

**String spans (v0.104).** The `ReadOnlyStringSpan` type (stdlib [§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)) is a read-only view over a contiguous range of code points in a `string`, parallel to `ReadOnlySpan<char>` but with a string source instead of an `OwnedBuffer<char>` source. The natural companion to `string.AsSpan()` for zero-allocation substring operations — tokenization, parsing, prefix/suffix checks — without materializing intermediate strings. The use case is the same one that motivates C#'s `ReadOnlySpan<char>` over `string`.

The design choice that took time was whether to extend `ReadOnlySpan<char>` to admit a second source type (alongside `OwnedBuffer<char>`) or introduce a separate type. Extending the existing type would either change its record shape (breaking v0.99-v0.103 callers) or require a general source-polymorphism mechanism (an `IBufferSource<T>` interface or similar) that v0.104 should not be introducing on its own. The separate type — `ReadOnlyStringSpan` — leaves `ReadOnlySpan<char>` unchanged, makes the source visible at API boundaries, and uses the existing borrow-bearing-record machinery for lifetime tracking (the span's `Source` field carries a borrow of the string).

The cost is that two type names cover the "read-only view of char sequence" space — `ReadOnlySpan<char>` for buffer-backed views, `ReadOnlyStringSpan` for string-backed views. Methods that want either need overloads or to be written against the more common type (typically `ReadOnlyStringSpan`, since most char views in practice come from strings). C# avoids this through implicit conversion from `string` to `ReadOnlySpan<char>`; Bezel's nominal record typing makes the conversion explicit. The friction is real but bounded — most parsing code is written against one type or the other, not both.

The asymmetric ASCII-byte view (a read-only byte view over an ASCII string) was not addressed in v0.104. It would require a similar borrow-of-string mechanism as `ReadOnlyStringSpan` but applies only to ASCII strings and needs a runtime check at construction. The deferred work was delivered in v0.106 as `ReadOnlyAsciiByteSpan` — see the v0.106 paragraph below.

**Span-level bounds checking (v0.105).** v0.99-v0.104 declared the `Span<T>` and `ReadOnlySpan<T>` indexers with a delegating expression body — `public property [i: int]: T? => Buffer[Start + i]` — which checked the index only against the underlying buffer's length, not against the span's `Length`. If a `Span<int>` with `Length = 5` was constructed over a 100-element buffer, `span[10]` would silently succeed: `Buffer[Start + 10]` was within the buffer's bounds, so no exception fired. This was a latent correctness bug — the whole point of having a separate `Span<T>` type is to enforce the range restriction the span advertises. v0.105 fixes it by giving each indexer an explicit block-form body with a span-level bounds check:

```bezel
public property [i: int]: T?
  get => do
    if i < 0 or i >= Length then
      throw new IndexOutOfRangeException();
    end;
    return Buffer[Start + i];
  end;
  set(owned) => do
    if i < 0 or i >= Length then
      throw new IndexOutOfRangeException();
    end;
    Buffer[Start + i] := value;
  end;
end;
```

The same fix applies to `Span<T>`'s setter (class T and value T), `ReadOnlySpan<T>`'s getter (class T and value T), and the `move span[i]` form (which now bounds-checks against `span.Length` before delegating to the underlying buffer). The bounds-check semantics align with C#'s `Span<T>`/`ReadOnlySpan<T>` and with v0.104's `ReadOnlyStringSpan` (which used the explicit form from the start).

This is technically a behavior change for code that compiled against v0.99-v0.104. Programs that depended on the bug — reading or writing outside the span's declared range while the underlying buffer still admitted the access — will now throw `IndexOutOfRangeException` instead of silently succeeding. Such programs were almost certainly already buggy; the fix converts a silent failure into a clear error. No source-syntax change is required; correct programs continue to work without modification.

**ASCII byte spans (v0.106).** The `ReadOnlyAsciiByteSpan` type (stdlib [§3.4](Bezel_Standard_Library.md#sec-readonlyasciibytespan)) is a read-only byte view over an ASCII string, parallel to `ReadOnlyStringSpan` (v0.104) but with `byte` element type instead of `char`. The natural companion to `string.AsAsciiByteSpan()` for FFI and I/O paths that need UTF-8 bytes from an ASCII string without a per-call allocation. ASCII strings are bit-identical to their UTF-8 byte sequence (every code point in `0..127` encodes as a single byte in `0..127`), so a byte view that delegates to the string's indexer-with-narrowing is correct by construction.

Construction validates the ASCII invariant — `AsAsciiByteSpan()` throws on non-ASCII input; `TryAsAsciiByteSpan(out value): bool` returns `false`. Once constructed, the indexer narrows char to byte unconditionally; the ASCII check at construction makes the narrowing safe. `Slice` preserves the invariant automatically (the source string has not changed).

The design choice that took time was whether to fold this into `ReadOnlyStringSpan` (e.g., add an `AsBytes()` view) or introduce a separate type. The separate type wins for the same reasons that motivated `ReadOnlyStringSpan` itself in v0.104: the element type differs, the construction APIs differ, the lifetime and source story is similar but the bounds and semantics are distinct. A user method that takes "any read-only byte view from a string" would need overloads anyway because the byte and char element types are different at the language level. Keeping the two types parallel and explicit makes the boundary visible.

The closely-related deferred items are not delivered in v0.106 — `ReadOnlyAsciiByteSpan` is the v0.106 piece of the broader "spans over additional view sources" deferred set. `Span<T>` over inline arrays `[N]T` and `Span<T>` over heap `Array<T>` remain deferred (see [§18](#sec-things-deferred)).

**No byte-buffer accessors on `ReadOnlyAsciiByteSpan`.** The v0.103 byte-buffer accessors (`ReadInt32LittleEndian`, etc.) are declared on `ReadOnlySpan<byte>` and `Span<byte>` for parsing binary protocols. They are *not* duplicated on `ReadOnlyAsciiByteSpan` because the motivating use cases differ — ASCII text views are for I/O (writing bytes to a stream, passing to FFI), not for binary number parsing. The two types coexist with different surface APIs; cross-source `SequenceEqual` is the one bridge provided. Should demand for binary parsing on ASCII byte spans emerge, the 32-method extension can be duplicated in a later revision; it is mechanical work.

**Per-access cost honesty.** The "zero-copy" claim depends on the string's internal storage representation. ASCII strings stored as 1-byte-per-code-point (the typical case under the adaptive-storage model) admit direct memory reads through the byte cast; the access is genuinely free. ASCII strings stored in wider representation (2 or 4 bytes per code point, which an implementation is allowed but not required to avoid) incur a width-dispatched read and a narrowing operation per access. The view is still allocation-free in both cases. Implementations are expected to use 1-byte storage for ASCII strings when practical, but the language does not require it.

**Array spans (v0.107).** The `ArraySpan<T>` and `ReadOnlyArraySpan<T>` types (stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)) are span views over an `Array<T>` source, parallel to `Span<T>` and `ReadOnlySpan<T>` (over `OwnedBuffer<T>`) but with a borrowing-collection source instead of an owning one. The natural companion to `Array<T>.AsSpan()` for passing a sub-range of an array to a method without copying.

The mechanics diverge from `Span<T>`/`ReadOnlySpan<T>` in one substantive way: `Array<T>` slots store borrows, not owned values. This changes the slot semantics in three places: (a) the indexer return type is `T` directly (the array's element type), not `T?` (no nullability wrapping — the array stores what it was instantiated with); (b) the setter takes the value with the default parameter convention rather than `set(owned)` (no ownership transfer; no dispose-previous); (c) `move arrSpan[i]` is *not* admitted for any T (borrowed slots have no ownership to transfer).

The mutation API simplifies as a consequence. `Span<T>` splits its bulk-mutation surface by T's category — `Clear`/`MoveFrom` for class T (which has owned slots), `Fill`/`CopyFrom` for value T (where copying is well-defined). `ArraySpan<T>` provides a single uniform surface — `Fill`/`CopyFrom`/`Reverse` — for both categories, because borrowed slots have no ownership constraints regardless of T. The category-dependent split was a consequence of ownership semantics that `Array<T>` doesn't have.

The choice of a separate type (vs extending `Span<T>`/`ReadOnlySpan<T>` to accept an `Array<T>` source) is the same one made in v0.104 and v0.106: extending the existing types would require source polymorphism, which would either break their record shape or require a general source-polymorphism mechanism this revision should not introduce on its own. The separate-type pattern is now the established Bezel approach for span family expansion. With v0.107, the span family contains six types: `Span<T>` and `ReadOnlySpan<T>` (`OwnedBuffer<T>`-backed, v0.99-v0.102), `ReadOnlyStringSpan` (`string`-backed, v0.104), `ReadOnlyAsciiByteSpan` (ASCII-`string`-backed, v0.106), and `ArraySpan<T>` and `ReadOnlyArraySpan<T>` (`Array<T>`-backed, v0.107). Methods that want to operate on "any span" need overloads — Bezel's nominal record typing doesn't admit implicit conversions between them. The friction is real but bounded; most code is written against a specific source type.

The `Array<T>` class itself ([§4.4](Bezel_Standard_Library.md#sec-arrayt)) gets eight new methods in v0.107: four `AsSpan` overloads (no-arg, `(start)`, `(start, length)`, `(r: Range<int>)`) returning `ArraySpan<T>`, and four matching `AsReadOnlySpan` overloads returning `ReadOnlyArraySpan<T>`. `AsSpan` returns the writable form by default (matching `Array<T>`'s mutable nature); callers wanting read-only either call `AsReadOnlySpan` directly or call `.AsReadOnly()` on the writable form.

A structural difference from the `OwnedBuffer<T>`-backed spans is worth noting: `Array<T>`'s `Count` can change during the array's lifetime, while `OwnedBuffer<T>`'s `Length` is fixed. A span's range is set at construction and does not auto-update if the underlying array resizes. The intended usage pattern is short-lived spans — pass the span to a method that reads or writes through it without resizing the source. Programs that need a stable view across mutations should copy the relevant range into a fresh container. The stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant) prose flags this with a "Caveat" paragraph; the v0.107 design accepts this footgun rather than designing around it (the alternative — making spans observe array resizes, or rejecting span construction over resizable sources — would either require new language machinery or eliminate the use case entirely).

**Unifying read-only spans through an interface (v0.108).** With v0.107, the span family contained six concrete types. Methods that wanted to accept "any read-only span" had no way to express that — they had to declare per-source overloads, accept one form and require callers to convert, or duplicate their implementation. v0.108 introduces `IReadOnlySpan<TElement>` (stdlib [§4.16](Bezel_Standard_Library.md#sec-ireadonlyspan)), an interface implemented by all six concrete span types, that unifies the read-only access surface.

The mechanism is straightforward and uses existing language machinery. Spans are records, records may implement interfaces (language spec [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal)), and the borrow-bearing-record boxing rule (language spec [§9.7.1](Bezel_Language_Spec.md#sec-boxing-of-borrow-bearing-records)) tracks the borrow's lifetime through the boxed value. A span passed as an `IReadOnlySpan<TElement>` parameter boxes; the boxed value's lifetime is bounded by the source of the underlying borrow; the parameter cannot be stored in a long-lived location, returned from a method without lifetime evidence, or captured by an escaping closure. These are the standard borrow-bearing-record constraints, not new rules.

The element-type parameterization is exact and explicit. Each concrete span implements `IReadOnlySpan<TElement>` with `TElement` set to whatever the concrete indexer actually returns: `Span<MyClass>` (which has `[i: int]: MyClass?`) implements `IReadOnlySpan<MyClass?>`; `ArraySpan<MyClass>` (which has `[i: int]: MyClass`) implements `IReadOnlySpan<MyClass>`; `ReadOnlyStringSpan` implements `IReadOnlySpan<char>`; and so on. There is no implicit conversion across nullability or element types — the interface parameter must match exactly. Authors who want full breadth provide two overloads (one for `IReadOnlySpan<T>`, one for `IReadOnlySpan<T?>`); the friction is real but localized at API boundaries.

The interface's surface is deliberately minimal: indexer-get, `Length`, `IsEmpty`, `IndexOf`, `Contains`, and `SequenceEqual`. `Slice` is intentionally omitted — putting it on the interface would force the return type to be `IReadOnlySpan<TElement>`, propagating the interface-typed handle (and its virtual-dispatch cost) through the slicing chain. Callers who need to slice through the interface can recover the concrete type via a runtime check (`if s is ReadOnlySpan<T> then ...`) or read the relevant elements into a fresh container. `SequenceEqual` *is* on the interface; the convenience of cross-source equality through the interface outweighs the cost of dispatching two indexer reads per element.

The performance cost is opt-in and per-call. Code that takes a concrete span type continues to compile to direct-dispatch machine code identical to v0.107. Code that takes `IReadOnlySpan<TElement>` accepts virtual dispatch at the indexer and at every other interface-routed call — vtable lookup, indirect call, no inlining. For tight inner loops over many elements, the cost is significant (typically 5-10x slowdown depending on what the loop body does). For utility code that doesn't iterate hot, the cost is negligible. The recommended pattern is: in performance-sensitive APIs, take concrete span types (possibly via overloads); in utility or convenience APIs, take the interface. The choice is the method author's, made by signature.

No writable interface (`IMutableSpan` or similar) is introduced in v0.108. The writable side has substantive semantic differences across span families — `Span<T>` for class T has a disposing `set(owned)` setter; `ArraySpan<T>` has a non-disposing `set` setter taking a borrow; value-T setters are plain copies. Unifying these under a single writable interface would either pick one model and exclude the others, or fork the contract by source kind, defeating the unification. The read-only side is genuinely uniform: every span supports indexer-get returning the slot's element. That's enough for one interface, and one interface is what v0.108 adds. A partial writable interface (covering only the non-owned-slot writable spans) may be added in a later revision if a real use case emerges.

**Ergonomics pass (v0.109).** v0.109 is a pure-ergonomics release that fills two gaps in the span family surface: symmetric CopyTo/MoveTo methods, and `AsSpan` / `AsReadOnlySpan` factories on `OwnedBuffer<T>`.

The CopyTo/MoveTo additions parallel the existing CopyFrom/MoveFrom methods. Each concrete span type's mutable form gets a symmetric method:

| Source | New method |
|---|---|
| `Span<T>` (value T) | `CopyTo(destination: Span<T>)` |
| `Span<T>` (class T) | `MoveTo(destination: Span<T>)` |
| `ReadOnlySpan<T>` (value T) | `CopyTo(destination: Span<T>)` |
| `ArraySpan<T>` | `CopyTo(destination: ArraySpan<T>)` |
| `ReadOnlyArraySpan<T>` | `CopyTo(destination: ArraySpan<T>)` |
| `ReadOnlyStringSpan` | `CopyTo(destination: Span<char>)` |
| `ReadOnlyAsciiByteSpan` | `CopyTo(destination: Span<byte>)` |

Each is equivalent to the corresponding CopyFrom/MoveFrom on the receiving side — `source.CopyTo(dest)` is `dest.CopyFrom(source)`. Both directions are provided because the natural call form depends on which side the caller holds. Tight processing loops typically write `dest.CopyFrom(source)` when the destination is the new local being filled; pipeline-style code typically writes `source.CopyTo(dest)` when chaining transformations left-to-right. Neither form is strictly necessary; both reduce friction.

The string-family CopyTo destinations are `Span<char>` and `Span<byte>` respectively — the OwnedBuffer-backed writable forms, since there is no writable string-backed span (strings are immutable). The ASCII-byte form keeps the v0.106 invariant: the source has been validated ASCII at construction; copying the bytes out preserves that property in the destination.

`ReadOnlySpan<T>` for class T does not get CopyTo. Reading from a read-only class-T span produces a borrow; writing into a Span<T> for class T requires ownership transfer (the `set(owned)` setter); the two are incompatible. Code that needs this pattern moves through the underlying buffer or uses a different ownership model entirely.

CopyTo is deliberately *not* added to the `IReadOnlySpan<TElement>` interface (v0.108). Two reasons. First, the destination type varies by source family — `ReadOnlyArraySpan<T>.CopyTo` takes `ArraySpan<T>`; `ReadOnlySpan<T>.CopyTo` takes `Span<T>`; the string-family forms take `Span<char>` and `Span<byte>` respectively. A single interface method would have to pick one destination shape, fragmenting the implementations. Second, the class-T-borrow vs class-T-owned incompatibility (above) means CopyTo cannot be uniformly defined across all TElement instantiations of `IReadOnlySpan<TElement>` — there is no class-T destination Span that accepts borrowed-source content via copy. Code that wants "copy from any read-only span" writes an extension method against the concrete span types, accepting one destination shape per source family.

The `OwnedBuffer<T>.AsSpan()` / `AsReadOnlySpan()` family adds eight methods (four overloads each, parallel to v0.107's `Array<T>.AsSpan()` and v0.104's `string.AsSpan()`). The methods replace `new Span<T>(buf, 0, buf.Length)` boilerplate at every span call site, completing the three-way symmetry across the standard collection sources (string for `ReadOnlyStringSpan`/`ReadOnlyAsciiByteSpan`; Array for `ArraySpan<T>`/`ReadOnlyArraySpan<T>`; OwnedBuffer for `Span<T>`/`ReadOnlySpan<T>`).

`OwnedArray<T>.AsSpan()` was deliberately omitted. The class would need either a new span type with owned-slot semantics matching its element-ownership model, or exposure of its internal buffer through a public method. Neither is justified by current demand. Users with an `OwnedArray<T>` who want span access can copy the relevant range into a fresh `OwnedBuffer<T>` and call `AsSpan` on that — at the cost of one allocation. If real demand for a direct OwnedArray span emerges in user code, a future revision can add the missing type.

**Migration.** No user code needs to change for v0.102 through v0.109. v0.101's spans (class-T-only) are a strict subset of v0.102's. v0.103, v0.104, v0.106, v0.107, v0.108, and v0.109 are pure stdlib additions (byte-buffer accessors and `BitConverter` in v0.103; `ReadOnlyStringSpan` and `string.AsSpan()` overloads in v0.104; `ReadOnlyAsciiByteSpan` and `string.AsAsciiByteSpan` / `TryAsAsciiByteSpan` in v0.106; `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, and `Array<T>.AsSpan` / `AsReadOnlySpan` overloads in v0.107; `IReadOnlySpan<TElement>` interface and six `SequenceEqual(other: IReadOnlySpan<TElement>): bool` overloads on the concrete span types in v0.108; symmetric CopyTo/MoveTo methods on the span family and `OwnedBuffer<T>.AsSpan` / `AsReadOnlySpan` overloads in v0.109). v0.105 is the bounds-check fix described above — only programs that read or wrote outside their declared span range will see a change (those will now throw). Programs that compiled against v0.101 compile against v0.109 without source modification.

<a id="sec-collection-literals-for-complex-collections"></a>
### 5.16 Collection literals for complex collections

Bezel's collection-literal form `[e1, e2, ...]` is contextually typed: the literal has no standalone type and is well-typed only where a target type is determinable (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)). v0.110 broadens the set of target types and pins down the previously-underspecified dictionary case.

**Why the existing colon-pair dictionary syntax.** The dictionary form `[k: v, ...]` was present in the spec before v0.110 but documented in a single sentence with no examples, no ordering semantics, and no statement of edge-case behavior. v0.110 keeps the syntax — it was the right choice — and fills in the specification. The syntax was weighed against three alternatives. The arrow form `[k => v]` would overload `=>`, which Bezel uses for case arms and expression-bodied accessors; reusing it for data would make `[x => y]` ambiguous to a reader scanning for control flow. The brace form `{k: v}` would introduce a fourth bracket kind (`()`, `[]`, `<>`, `{}`) competing for visual distinctness, where `[]` already carries collection-literal meaning and `{}` appears only inside string interpolation; the cost is a permanent extra syntactic category for a single use. The tuple-list form `[(k, v), ...]` would require tuple types, which Bezel does not provide (deferred per [§18](#sec-things-deferred)); the named-record workaround `[Pair(k, v), ...]` is verbose and reads as a list of pairs, not a mapping. The colon-pair form is compact, distinct from the bare sequence literal, familiar from JSON/Python/Swift/Ruby, and overloads no token in a conflicting role. The one parsing subtlety — distinguishing `[a: b]` (dictionary) from a hypothetical type-annotated expression — does not arise because Bezel has no inline expression type-annotation syntax; the grammar disambiguates purely on the presence of a top-level colon in a literal element.

**Why broaden to LinkedList / Stack / Queue.** Before v0.110 the supported target list named only inline arrays, `Array`/`OwnedArray`, `Set`/`OwnedSet`, and `Dictionary`/`OwnedDictionary`. `LinkedList<T>`, `Stack<T>`, `Queue<T>` and their owning variants are sequence-shaped — a literal `[1, 2, 3]` has an obvious meaning for each — and excluding them was an omission, not a deliberate restriction. The only design question was element-order convention. For lists and queues the convention is unsurprising (textual order is head-to-tail / front-to-back). For stacks there were two defensible readings: left-to-right as push order (first element ends at the bottom) or left-to-right as pop order (first element on top). v0.110 picks push order — `[1, 2, 3]` means "push 1, push 2, push 3," so 3 is on top — because it lets a reader trace the literal as a sequence of construction operations, consistent with how the list and queue literals read. The pop-order alternative would have made the literal read "as the stack looks from the top," which is convenient for some mental models but breaks the uniform "textual order = construction order" rule the other sequence types follow.

**The user-defined sequence-literal protocol (v0.111).** v0.110 hard-coded the supported target types; v0.111 opens sequence-literal construction to user-defined types via a named static factory, `FromCollectionLiteral` (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)). The design was guided by a comparison with C#'s `[CollectionBuilder]` mechanism, which solves the same problem and surfaced three lessons.

*Mechanism: named static factory, not an attribute.* C# opts a type in via `[CollectionBuilder(typeof(BuilderType), "Create")]` — an attribute naming a separate non-generic builder class that holds the static generic `Create<T>(ReadOnlySpan<T>)`. The indirection through a separate builder type exists because C#'s attribute argument needs a `typeof` that cannot cleanly name a generic instantiation, and because static methods interact awkwardly with the generic type itself in their model. Bezel has no general attribute system, so the natural "explicit opt-in" equivalent is *declaring a specifically-named static method*. And Bezel static methods on a generic type may freely reference the type's own parameters (the standard library is full of them — `Array<byte>.NewBuffer`, etc.), so the factory goes *directly on the collection type* with no builder-type indirection. The result is simpler than C#'s: one method on the type, no attribute, no separate builder class.

*Element type: read from the factory signature, not inferred.* C#'s own design documents flag an unresolved wart: with `[CollectionBuilder]` alone the compiler cannot reliably determine the collection's element type, so C# additionally requires the type to define its iteration type (implement `IEnumerable<T>` or expose a `GetEnumerator`). Bezel's named-factory approach has no such problem — the element type *is* the `ReadOnlySpan<T>` (or `OwnedArray<T>`) parameter's element type, declared right there in the signature the compiler is already looking up. There is no secondary requirement and no inference. This is a case where examining C#'s difficulty produced a cleaner Bezel design rather than a copy.

*Ownership: two factory shapes, a Bezel-specific concern.* C# has no ownership system, so its single `ReadOnlySpan<T>` factory shape suffices. Bezel's borrowing/owning collection duality requires two shapes: the borrowing form takes `ReadOnlySpan<T>` (the compiler synthesizes a temporary span over the borrowed element values); the owning form takes `owned OwnedArray<T>` (the compiler builds a temporary owned array, transferring each owned element expression into it, and hands ownership to the factory, which drains it). Exactly one shape per type; declaring both is rejected because the literal's element-expression ownership would not unambiguously pick a target. The owning form's partial-failure behavior (an element expression throwing mid-construction) folds into the existing `OwnedArray<T>`-literal exception-path discharge — no new rule.

*One opt-in path, not two.* C# also supports a duck-typed fallback (implement `IEnumerable<T>` + an `Add` method + a parameterless constructor, no attribute). Bezel deliberately does not replicate this. The specification is careful to keep exactly one structural-dispatch rule (`Contains` for `in`); a second "your type became literal-constructible because it happens to have an `Add` method" path would be exactly the kind of accidental enrollment that rule's deliberate scarcity guards against. C# carries the dual path largely for backward compatibility with pre-existing collection-initializer types; Bezel has no such legacy and chooses the single explicit path.

*A new recognition category, named honestly.* Bezel now has three ways the compiler recognizes a type's participation in a language construct: interface-based (the iteration protocol, [§29.1](Bezel_Language_Spec.md#sec-the-protocol)); the single structural-by-shape rule (`Contains`/`in`); and now name-based (`FromCollectionLiteral`). Name-based recognition — "declare this exact `public static` method with a recognized signature" — is the closest available analogue to C#'s explicit attribute opt-in given that Bezel has neither attributes nor (yet) static interface members. The spec at [§16](Bezel_Language_Spec.md#sec-range-expressions) already anticipates static interface members for a future `ISteppable<T>`; if they arrive, `FromCollectionLiteral` can be promoted to an `ICollectionLiteralTarget<T>` interface method with the same name and signature, shifting recognition from name-based to interface-based without breaking any user code. The name and signature were chosen now to be interface-promotable later.

*Dictionaries stay out, matching C#'s staged path.* C# deliberately excluded dictionaries from collection expressions and is still, two years on, designing dictionary expressions as a separate feature. The pair-shaped literal needs its own factory design (a paired-span or key/value-span shape) and the design space is unsettled even in a mature language. v0.111 covers user-defined *sequence* collections only; user-extensible dictionary literals remain deferred, for the same reason C# deferred them.

**What is still deferred.** User-extensible *dictionary*-literal construction (the `[k: v, ...]` form for user-defined map-like types) remains deferred — see [§18](#sec-things-deferred). The built-in `Dictionary`/`OwnedDictionary` keep the dictionary-literal form; user map-like types must use ordinary constructors until a future pass designs the paired-factory protocol.

<a id="sec-why-no-class-deconstruction"></a>
### 5.17 Why no class deconstruction

A recurring request is positional destructuring of class objects — `var (a, b) := someClass;` — paralleling the record-destructuring of [§19.3](Bezel_Language_Spec.md#sec-patterns). It was evaluated in full and declined. This section records the analysis so the decision is not silently re-litigated.

The motivating case is the multi-owned-value return: a method that produces several owned resources at once. This is already fully expressible. A class may own multiple fields ([§28.5](Bezel_Language_Spec.md#sec-owned-fields)); a method may return `owned C`; a single `using var` on the result discharges the whole aggregate, the destructor cascade disposing every owned field in reverse declaration order. The `OwnedDictionary<K, V>.Drain()` case (stdlib [§4.9](Bezel_Standard_Library.md#sec-owneddictionaryk-v)) is exactly this pattern: `DrainedEntry<K, V>` is a class that owns its `Value`, iterated through `OwnedEnumerable<T>`. No language feature beyond what already exists is needed for the capability.

What class deconstruction would add is therefore *only* notation: referring to `conn` instead of `pair.Connection`. Three considerations made that notation not worth its cost.

First, the naive form — "destructuring disposes the class and yields its fields" — is unsound. A class destructor runs arbitrary user code and, per [§28.5](Bezel_Language_Spec.md#sec-owned-fields), runs *before* the owned-field auto-dispose cascade; destructor bodies routinely use the very fields a deconstruction would try to extract. "Dispose the container but keep its fields alive" inverts the dependency disposal embodies: disposal *is* the teardown of those fields. Extraction (move the fields out, transferring ownership) and disposal (the container tears the fields down) are opposite operations on the same slots; they cannot both happen.

Second, the sound form is a separate, opt-in `Deconstruct`-style method that *moves* each owned field out (transferring ownership) and consumes the receiver, leaving it moved-from and exempt from the field-restoration rule of [§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression) — a new ownership-system rule ("a deconstruct method consumes its receiver, waiving restoration for fully-moved-out fields"). The language already waives restoration in one place — inside a destructor body ([§28.8.1](Bezel_Language_Spec.md#sec-the-move-expression), "the field-restoration rule does not require reassignment inside a destructor") — but that waiver rests on the destructor being *the* designated teardown site: the object's lifetime provably ends there and no later code can observe it. A `Deconstruct` method has neither property; it is an ordinary method, the husk survives the call, and other references to the object may still be live. The destructor carve-out therefore does not extend to it, which is exactly why a genuinely new rule (not a reuse of the existing one) would be required. To keep the borrow checker unchanged, the consumption would have to be syntactically visible at the use site (an explicit `move` at the destructuring position, so the existing move-invalidation machinery applies rather than a new borrow concept). This is implementable and coherent, but it adds a rule to the ownership system — Bezel's most cost-sensitive subsystem, where the whole value proposition is that the rules are few and composable. Every rule there raises the baseline cost of the feature that is supposed to be invisible when it works.

Third, the benefit is sugar on a solved problem at low frequency. Named owning classes and `out owned` parameters cover the capability; the recurring instance (`Drain()`) is closed by a pure standard-library addition. Weighing a new ownership rule against "saves writing `pair.` twice" does not clear the bar a change to the ownership system should meet. Bezel's design philosophy already argues, for records-versus-tuples, that a name and a documented shape is a feature rather than a limitation; `pair.Connection` tells a reader what they hold, where a positional `conn` tells them less. The same argument applies to owned aggregates.

The conservative resolution is taken: the capability stays (owning classes), the sugar is declined (no class deconstruction, no tuples as a prerequisite), and the one real friction point (`OwnedDictionary` draining) is removed at the library level rather than the language level. If a future revision finds the notation genuinely necessary — driven by evidence from real code, not design aesthetics — the `Deconstruct`-with-explicit-`move` design recorded here is the starting point.

<a id="sec-type-identity-not-reflection"></a>
### 5.18 Type identity, not reflection

Bezel provides `typeof(T)`, `value.GetType()`, the `is`/`as` operators, and a `Type` token carrying name, kind predicates, and lattice queries (stdlib [§2.13](Bezel_Standard_Library.md#sec-the-type-class)). It deliberately does *not* provide member-access reflection: no enumeration of fields, properties, or methods; no by-name member access; no attribute reading; no dynamic instantiation. This section records why the boundary sits exactly there, so the question is settled rather than perennially reopened.

**The boundary is principled, not merely unimplemented.** Member-access reflection is in direct tension with the language's two load-bearing commitments.

*Ownership has no reflective site.* Every ownership contract in Bezel attaches to a static program point: a parameter declared `owned`, a field declared `owned`, a `move` expression, a `using var`. The borrow checker reasons about these points. A reflective field read — `someType.GetField("conn").GetValue(obj)` — has no such point. Is the returned `Connection` a borrow of the still-living object, or a transfer that leaves the field moved-from? The call site is `GetValue`, whose signature is identical regardless of the field's ownership; there is nowhere to write the contract and nowhere for the checker to enforce it. C#-style reflection works precisely because the GC makes the question moot — every reference is equivalent and lifetimes are not tracked. Bezel's whole value proposition is that they *are* tracked, at explicit sites. Reflective member access would be a hole in exactly the mechanism the language exists to provide. This is not a difficulty to be engineered around; it is a contradiction.

*It is the maximal "hidden magic."* The rationale invokes the no-hidden-magic principle repeatedly — every value has a static shape, every dispatch is either statically resolved or an explicit interface call, the one structural-dispatch rule (`Contains`/`in`) is singular and called out as exceptional. Member reflection is the largest conceivable violation: arbitrary code reaching arbitrary members of arbitrary types with no static trace. Admitting it would not bend the principle; it would delete it.

**The line is drawn at lattice metadata because that costs nothing.** Everything the `Type` token exposes — name, kind, base, interface conformance, enum members — is information the compiler already has and the runtime already carries so that `is`, `as`, and `GetType` can function at all. Surfacing it adds no new capability and no new tension; it only stops forcing users to thread `typeof(T)` through their own hand-built side tables. The genuine, ownership-neutral use cases are well served: type-keyed registries and caches, kind-based diagnostic formatting, interface-conformance checks on bare `Type` tokens, and enum reflection (closed, stateless, ownership-free — the safest introspection that exists, and the single most requested). None of these touch instance state, so none of them raises the ownership question.

**What was deliberately excluded from even the metadata surface.** Generic-instantiation decomposition — "is this some `Array<X>`, and what is `X`?" via a `GenericArguments` / `GenericDefinition` pair — is genuinely useful for mapping libraries, but it cannot be expressed without also adding an *unbound-generic* `typeof` (a way to write `typeof(Array<>)` to compare against a `GenericDefinition`), which the `typeof` grammar does not admit (language spec [§17.10](Bezel_Language_Spec.md#sec-the-typeof-operator) accepts only closed instantiations). That is a language change wearing a standard-library costume, and bundling it into a metadata revision would hide its true scope. It is therefore deferred as its own explicitly-scoped question ([§18](#sec-things-deferred)), to be decided together with the unbound-`typeof` syntax, not smuggled in.

**Attributes are the realistic future pressure valve, separately.** Several use cases that look like "reflection" (serialization hints, dependency-injection registration) are really *attribute* use cases. Bezel has no attribute system today. If reflection pressure ever genuinely materializes from real programs, the principled response is a minimal attribute system plus attribute-only `Type` queries — never member-access reflection. That, too, is its own scoped question ([§18](#sec-things-deferred)); it is named here so the eventual direction is on record and is not mistaken for a step toward member reflection.

The disposition mirrors [§5.17](#sec-why-no-class-deconstruction): deliver the part that fits the language cleanly (lattice metadata), decline the part that fights its foundations (member-access reflection) *with the reasoning written down*, and keep the genuinely-open adjacent questions (generic decomposition, attributes) explicitly scoped rather than vaguely "deferred." An explicitly reasoned "no" is more useful to users than an open "later": it tells them to architect with compile-time generics and explicit registration now, rather than waiting for a reflective capability that is not coming.

<a id="sec-serialization-without-reflection"></a>
### 5.19 Serialization without reflection

[§5.18](#sec-type-identity-not-reflection) closes the door on reflective serialization. This section records the design that replaces it, the staged plan, and why the v0.114 vocabulary (stdlib [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary)) is shaped the way it is.

**Every reflection-free language converged on the same architecture.** Rust's serde generates `Serialize`/`Deserialize` implementations at compile time via a derive macro and routes them through a format-agnostic `Serializer`/`Deserializer` pair, so a type knows nothing about JSON or any other format. Swift's `Codable` has the *compiler itself* synthesize conformance for types whose stored properties are all `Codable`, routed through a format-agnostic `Encoder`/`Decoder` pair. Even C#, which has runtime reflection, moved its modern JSON path to *build-time source generators* for performance and ahead-of-time-compilation reasons — evidence that compile-time codegen is not a Bezel compromise forced by the lack of reflection but the destination reflection-capable languages migrate *to*. The shared invariant across all three: separate *what a type's shape is* (static, the type's concern) from *how a format encodes it* (the format's concern). Reflection conflates them by discovering shape at runtime; the reflection-free approach keeps them separate and resolves shape at compile time.

**Bezel already does opt-in compile-time codegen — it is called auto-equality.** A body record with only auto-properties automatically receives a synthesized `=`, `GetHashCode`, and `ToString` ([§9.2](Bezel_Language_Spec.md#sec-body-records)); a record with a disqualifying member instead produces a compile error at the use site, naming the obstruction. This is precisely opt-in-by-structure compile-time generation with a recursive eligibility rule and a disciplined error story — and it needs no attribute system, no macro system, and no general source-generator plugin mechanism. Serialization synthesis is not a new *kind* of feature for Bezel; it is auto-equality generalized from "compare the fields" to "read and write the fields against a format." Choosing the Swift model (a dedicated compiler feature keyed off a declared conformance) over the C# model (an open-ended source-generator subsystem) is therefore the choice that fits machinery the language already has, rather than the choice that requires building the deferred macro system first.

**The naming follows the established interface convention.** Bezel's interface set splits cleanly into capability adjectives a type declares about itself (`IDisposable`, `IEquatable<T>`, `IComparable<T>` — the `-able` family) and agent nouns that act on other values (`IEnumerator<T>`, `IEqualityComparer<T>`). Serialization has one of each: a *type* declares it can serialize itself (`ISerializable`/`IDeserializable`, the `-able` family, by direct analogy with `IDisposable`); a *format* is an agent that serializes values (`ISerializer`/`IDeserializer`, the agent-noun family, by analogy with `IEqualityComparer<T>`). The type-side interfaces are deliberately *parameterless* (not `ISerializable<TFormat>`): a value's shape is intrinsic and format-independent, so format polymorphism lives in the `ISerializer` parameter to the method, never in the type-side interface — the same place serde and Codable put it, and the same shape as the parameterless `IDisposable`. Splitting serialize and deserialize into two interfaces rather than one combined interface follows the existing `IDisposable`/`IAsyncDisposable` and `IEnumerable`/`IAsyncEnumerable` precedent of separating paired concerns, and is additionally forced here by a structural asymmetry: serialization is an instance operation (an existing value writes itself), deserialization a static one (a reader produces a value where none exists yet).

**That asymmetry is why `IDeserializable` is, for now, a marker.** A static construction entry point cannot be an interface *method* until static interface members exist (separately deferred). Rather than block deserialization on that keystone, v0.114 recognizes the entry point *by name* — a static `Deserialize(d: IDeserializer): T` (plain by-value for the record/enum common case; `owned T` only when the declaring type is a class, since `owned` on a non-class return is semantically rejected) whose presence is opted into by declaring `: IDeserializable` — exactly the mechanism, and exactly the forward-compatibility discipline, established for `FromCollectionLiteral` in [§5.16](#sec-collection-literals-for-complex-collections) and language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol). The name and signature are chosen now so that when static interface members arrive, `Deserialize` promotes to a real interface method with no source change. This reuse of an already-justified pattern is deliberate: it avoids inventing a second recognition mechanism for the same shape of problem, and it means deserialization is not gated on the static-interface-members keystone — a materially better staging than making serialization wait.

**The synthesis design (delivered in v0.116).** A record or enum that declares `: ISerializable` (and/or `: IDeserializable`) and satisfies the synthesis-eligibility rule has the bodies generated by the compiler (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)), governed by a recursive structural-eligibility rule that mirrors auto-equality's: a type is synthesis-eligible when every serialized component is itself serializable; a non-serializable component is a compile error at the conformance declaration, naming the offending member, with the same error discipline auto-equality uses for disqualifying members (reported at the declaration rather than the use site, because the conformance declaration is the explicit opt-in). Records (shape known from [§9.1](Bezel_Language_Spec.md#sec-positional-records)/[§9.2](Bezel_Language_Spec.md#sec-body-records)) and enums (closed, members known) are the eligible set; classes are not (no inheritance/identity/owned-field complications, exactly the closed-shape property that makes auto-equality sound for the same set). The ownership concern that made *reflection* unsound does not arise: the synthesized `Deserialize` body is a static program point that calls the type's constructor with read values, so field types, nullability, and definite-assignment are checked normally — exactly where the language wants the contract, and exactly the site reflection lacked. Synthesis is therefore sound for the same structural reason auto-equality is. Hand-writing either direction suppresses synthesis of that direction (as a manual `Equals` suppresses auto-equality); the two directions are independent. The specific wire choices are normative (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)) so independent implementations agree: enums serialize by member *name* not underlying integer (stable against renumbering, self-describing in text), components in declaration order (matching auto-`ToString`), dictionaries as key/value-pair arrays (admitting non-string keys uniformly rather than forcing string coercion as JSON-object-keyed encodings do).

**Field-name customization (delivered in v0.117): a compiler-recognized modifier, not attributes, improving on `CodingKeys`.** Renaming a serialized field, excluding one, or tolerating a missing one has no auto-equality analog (equality needs no external names), so it required a new mechanism. Two off-the-shelf models were rejected. *Attributes* (`[JsonPropertyName("…")]`) were rejected because field-name customization is precisely the canonical case that, in C#, metastasizes into a dozen stacked serialization attributes per member — exactly the proliferation, stringly-typed-magic, and semantics/metadata-blur the attribute-system deferral ([§18](#sec-things-deferred)) is on record against; solving this one narrow need by importing a general attribute system would pay that entire cost to buy one feature, and would prejudice the still-open general-attribute question. *Swift's `CodingKeys`* (a nested `enum: String, CodingKey` listing every coded property) was rejected because it is a footgun by construction: it conflates "which members are coded" with "what they are renamed to" into one exhaustive list, so adding a member and forgetting its case **silently drops it** from serialization, and a reader cannot distinguish intentional exclusion from an omission — the same "blur" indictment, relocated into an enum. The chosen design is the narrow-modifier escape hatch [§18](#sec-things-deferred) already sanctions for `[Obsolete]`/`[MethodImpl]`-style needs: a closed `serialized` clause expressing three effects — rename (`serialized "name"`), exclude (`serialized skip`), tolerate (`serialized optional`), where rename and tolerate combine (`serialized "name" optional`) and exclude is mutually exclusive with both — attached to the member it affects (language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)). It improves on `CodingKeys` in the specific way the design question demanded: it is *per-member and total* — a member with no clause is coded under its own name, always, so there is no separate list to desynchronize and the silent-drop failure is structurally impossible; the clause is *local* — its effect is visible at the member, with one defined meaning, no reflective indirection, answering [§18](#sec-things-deferred)'s "a reader cannot tell what an annotation does" objection directly; and it is *closed* — a fixed effect vocabulary, no parameters interpreted by external machinery, no user extensibility, so it cannot proliferate (a new effect would be a language change with its own rationale, exactly the friction that prevents the C# explosion). It is forward-compatible: if a general attribute system later arrives, `serialized "x"` re-expresses mechanically as `[Serialized("x")]` with identical semantics, the same promotion discipline used for `FromCollectionLiteral` and recognized-`Deserialize`. The honest cost: this is the first serialization stage that adds grammar — unavoidable, since customization inherently needs somewhere to write the custom name, and the modifier is the *smallest* such addition (two closed productions, no new top-level construct, no attribute-target or retention rules, no reflection dependency) where attributes would have been the largest.

**Unknown-field tolerance is an operation policy, not a type annotation (v0.118).** The strict default — an input field matching no declared component raises (established with synthesis, v0.116) — is correct because silently dropping unrecognized input masks producer/consumer schema drift and field-name typos. But making it *configurable* raised a placement question worth recording, because the answer generalizes to future serialization-policy questions. The natural-seeming move was a type-level `serialized tolerant` clause, parallel to v0.117's member-level `serialized` modifier. That was rejected on a category distinction: the v0.117 effects (rename, skip, optional) are *intrinsic to the type* — `FirstName` *is* `"first_name"` in this type's wire form for every caller, every format, every call site; that is part of the type's serialization contract and belongs on the type. Unknown-field tolerance is not a fact about the type at all: the same type is legitimately read strictly when consuming a trusted internal message (drift *should* error) and leniently when consuming a third-party producer that adds fields freely. That is a property of the *read* — the caller has it, with the input, in a context — not of the type. A type-level annotation would force one answer on every call site, would be strictly less expressive than a per-operation control, and would reintroduce exactly the "inert-looking but load-bearing, and you cannot tell from the call site what it does" objection [§18](#sec-things-deferred) levels at attributes — the same objection the v0.117 design was careful to answer. So the knob lives on the deserialization operation: `JsonOptions.IgnoreUnknownFields` and the binary deserializer's constructor parameter (stdlib [§2.15](Bezel_Standard_Library.md#sec-concrete-serialization-formats)), defaulting to strict. Mechanically, the synthesized `Deserialize` is identical regardless of policy — on an unrecognized field it calls `IDeserializer.SkipValue` and continues; whether *reaching* an unknown field is an error is the deserializer's configured behavior. This is the v0.114 shape/format split extended to shape/policy: the synthesized code expresses structure, the deserializer expresses policy, and they compose without either knowing the other's choices. The general principle, worth stating once for future policy questions: a serialization knob belongs on the type when it is part of the type's wire contract, and on the operation when it is a property of a particular read or write; "which spelling is most consistent with an existing mechanism" does not override "where does this policy actually live."

**Tree-shaped class synthesis (v0.119): the part of the class-graph tier that was never actually blocked.** The class-graph tier was deferred wholesale through v0.118, but it decomposes into sub-problems of very different difficulty, and the easiest is not hard at all. A class whose serialized object graph is a *tree* — no two serialized edges alias the same object, no cycles — is, for synthesis purposes, a record plus heap allocation and ownership transfer: the synthesized `Deserialize` constructs each owned child and transfers it into the parent's constructor, and the ownership contract lives at that constructor signature, a static program point, exactly as for records. This is the case [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis)'s original soundness argument already covered ("an `owned`-field class's ownership contract lives at its constructor signature — exactly where the language wants it"); it was bundled with the genuinely hard sub-problems and deferred with them, but it does not need the reference-graph machinery the hard sub-problems do. v0.119 unbundles it.

The one real design question was that *tree-shape is not statically verifiable* — whether two `owned` subtrees alias is not a static property of the type. Three responses were possible: infer-and-hope (serde's choice: silently duplicate, the documented footgun where deserializing shared reference-counted pointers yields distinct objects with no diagnostic); require a static proof (impossible without an aliasing analysis the language does not have and does not want); or **assert-and-enforce**. Bezel takes the third: the programmer asserts tree-shape with an explicit `serialized tree` class-header clause, and the synthesized deserializer *dynamically enforces* it — a repeated object identity in the input raises `SerializationException` rather than silently duplicating. This is the disposition the project takes everywhere: do not let a generic mechanism paper over a property it cannot guarantee; make the assumption explicit and make its violation a *detected error*, never a silent corruption. Silent duplication is precisely the behavior other ecosystems ship and warn about; refusing it is worth a runtime check.

`serialized tree` is *type-scoped* — placed on the class header, not on a deserialization call — and this is consistent with, not a contradiction of, the v0.118 placement principle. v0.118 established: a knob belongs on the type when it is part of the type's wire contract, and on the operation when it is a property of a particular read. Tree-shape is squarely the former — a class either serializes as a tree for every caller, every format, every call site, or it does not; it is an invariant of the type's serialized form, exactly like a field's renamed wire name and unlike unknown-field tolerance (which legitimately varies per read). The v0.118 principle is what *tells* us `serialized tree` is type-scoped while `IgnoreUnknownFields` is operation-scoped; the two placements are the same rule applied to two genuinely different kinds of property.

**The generalization ceiling, stated honestly.** With tree-shaped class synthesis delivered (v0.119), the class-graph tier is narrowed to its genuinely hard residue, deferred with its reason. Three sub-problems remain, and they are really one: *shared references* (a DAG where two serialized edges alias one object), *cycles* (which in an exclusive-ownership model are necessarily an owning spanning tree plus borrowing back-edges, i.e. the shared-reference problem wearing a scarier hat), and *polymorphic dispatch* (a base-typed field holding a derived instance, needing a wire type-discriminator and an explicit construction registry — orthogonal to identity, and consistent only as *explicit registration*, never reflective discovery, per [§5.18](#sec-type-identity-not-reflection)). The crux for the first two is specific and unresolved: Bezel's `owned`/borrow annotations *already encode* which edges are owning (the canonical-owner information that defeats serde and every garbage-collected language is, uniquely, statically present here) — but reconstructing a back-borrow requires binding a borrowing field *after* its owning target is constructed, which collides with definite-assignment, the guarantee that is itself load-bearing. That tension (deferred borrow binding vs definite-assignment) is the single question that gates everything past trees, and it deserves its own rationale working-out *before* any spec text, exactly as every hard question in this design has been resolved reasoning-first. *Concrete formats* are delivered (v0.115, `System.Serialization`, stdlib [§2.15](Bezel_Standard_Library.md#sec-concrete-serialization-formats)). The staging held and continues: vocabulary (v0.114) → formats (v0.115) → record/enum synthesis (v0.116) → field-name customization (v0.117) → operation-scoped unknown-field tolerance (v0.118) → tree-shaped class synthesis (v0.119), each an independent non-breaking addition; the remaining work is the shared/cyclic/polymorphic residue, gated on the deferred-binding question.

**Working out the deferred-borrow-binding vs definite-assignment tension (v0.120, rationale only).** This subsection is the promised reasoning-first working-out. It commits no spec text; its purpose is to determine whether identity-preserving class-graph deserialization is *coherent* in Bezel before any syntax or semantics is proposed, because if the central tension has no sound resolution the honest outcome is a reasoned decline, not an open "later."

*The tension, stated exactly.* A shared-reference graph, after the v0.119 observation, is an owning spanning tree (the `owned` edges) plus a set of non-owning back-edges (borrowing fields that point at objects owned elsewhere in the tree). Deserialization must reconstruct the owning tree — that part v0.119 already does soundly — and then bind each borrowing field to the already-constructed object it references. The collision is with [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields): a non-nullable reference field must be assigned in every constructor before that constructor returns, with no default value in existence for it. A borrowing field whose target appears *later* in the construction order cannot be given its value in its owner's constructor — the target does not exist yet. So either the field is left unassigned at construction (violating [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields)), or construction is reordered so every borrow target precedes every borrower (impossible in general — a cycle of borrows has no such order), or the field is made nullable (changing the *type* to serve the deserializer, which is unacceptable: the type's contract must not be deformed by how it is read, the principle [§5.19](#sec-serialization-without-reflection) established when rejecting type-level tolerance).

*Why the three obvious escapes each fail.* (1) *Relax [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) for synthesized deserialization.* Rejected: [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) is not a local convenience, it is the guarantee that a non-nullable reference is never observed unset; a synthesized path that can produce a half-constructed object with an unset non-nullable field reintroduces exactly the null-dereference class the language spent its nullability design eliminating, and it would do so invisibly inside generated code where no reader can audit it. A guarantee with a synthesized exception is not a guarantee. (2) *Topologically order construction so borrowers follow targets.* Rejected as a general solution: it handles a DAG but not a cycle (mutually borrowing objects have no valid order), and even for a DAG it forces construction order to be a function of the borrow graph rather than the ownership tree, which the ownership model — not the serializer — is entitled to define. (3) *Make borrowing fields nullable for deserialization.* Rejected on the [§5.19](#sec-serialization-without-reflection) principle above, and independently because it would make every *reader* of such a type handle a `null` that the type's own invariants forbid — pushing the deserializer's difficulty onto every downstream use site, the worst possible place for it.

*The candidate that survives scrutiny: a bounded two-phase construction window, expressed in the ownership model rather than bolted onto it.* The serde comparison is instructive precisely because Rust hit the same wall and its answer (`Rc::new_cyclic`, which hands the closure a `Weak` to the not-yet-initialized allocation) is a *controlled* deferral: a narrowly-scoped window in which a back-reference may be wired up after the target's storage exists but before the object is considered fully constructed. The principled Bezel analog is *not* `Weak` (Bezel has no reference counting and should not grow one for serialization), but the structural idea transfers: deserialization of a shared/cyclic component group is a single operation with two phases — *allocate-and-own* (construct every object in the group through its owning constructor, establishing the owning tree, with borrowing back-edge fields in a defined not-yet-bound state that **is not observable outside the deserialization operation**), then *bind-backs* (assign every borrowing field to its now-existent target, after which the group is fully constructed and released to the caller). The [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) guarantee is preserved *as a guarantee* if and only if the not-yet-bound state is provably unobservable: no user code, no constructor body, no method runs against an object in the group between phase one and phase two. That is the whole crux reduced to a single checkable property — **unobservability of the intermediate state** — rather than a vague "two-phase init."

*What that reduction buys, and what it still costs.* It converts an open-ended "how do we deserialize graphs" into one precise design obligation: define a construction-group operation whose intermediate state is statically guaranteed unobservable, and prove the guarantee. This is tractable-looking because Bezel already has the ingredients — `owned`/borrow annotations supply the tree/back-edge partition for free (the information serde must guess), and the synthesized `Deserialize` is already a closed compiler-generated region with no user code interleaved. The residual hard part is honest and nameable: the unobservability proof must survive *constructor side effects* (a constructor in the group that calls a method that could reach a not-yet-bound borrow), which means either the group's constructors must be restricted to a statically-checkable "no escape during construction" shape, or the back-edges must be bound before any constructor body runs (constructor-parameter injection of the borrow, only possible if the target precedes — back to ordering for the acyclic case, and genuinely open for the cyclic case). Polymorphism remains orthogonal and is *not* gated on this: it is the explicit-registry sub-problem ([§5.18](#sec-type-identity-not-reflection)'s explicit-registration-not-reflection stance applies unchanged) and can be designed independently whenever wanted.

*Conclusion of the working-out.* The tension is **not** a dead end: the shared-reference and acyclic-back-edge case has a credible resolution (bounded two-phase construction with a statically-enforced unobservable intermediate state, the back-edge partition taken from the ownership annotations the language already requires). The **truly cyclic** case — mutual borrows with no construction order and constructors with potentially-escaping bodies — remains genuinely open and is the single honest blocker; it is *not* refuted here, but it is not resolved here either, and it should not receive spec text until the unobservability proof obligation is discharged for it specifically. The disposition matches the rest of the design: the sub-problem that fits (DAG-shaped shared references) is now understood well enough to specify in a future revision; the sub-problem that fights the foundations (arbitrary cycles under effectful constructors) stays explicitly scoped with its exact obstruction named — the unobservability-under-constructor-side-effects proof — rather than vaguely deferred. This is the gate cleared for *shared-reference* spec work and held, with a precise reason, for *cyclic* spec work.

**Shared-reference DAG synthesis (delivered in v0.121): the discharged proof, made spec.** v0.121 specifies the case the working-out cleared, and it is worth recording exactly *which* of the working-out's two candidate discharges the spec took, because the choice matters. The working-out named two ways to make the intermediate state unobservable: a bounded two-phase window (allocate-and-own, then bind-backs, with a hard unobservability proof that must survive constructor side effects), or — for the acyclic case only — binding the back-edges *before any constructor body runs*, via constructor-parameter injection, "only possible if the target precedes." A directed acyclic graph is *exactly* the condition under which the target can always precede: a DAG admits a topological (owner-before-borrower) construction order, so every borrowing back-edge's target is already constructed when the borrower's constructor is called, and the borrow is passed *as a constructor argument*. This takes the **easier** of the two discharges — the one with no separate mutable phase and therefore no unobservability-under-side-effects proof obligation at all, because there is no intermediate state to be observed: each object is fully constructed, back-edges included, by its single constructor call. [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) is satisfied *unmodified* — not relaxed, not excepted for synthesized code, not worked around with nullability — which was the non-negotiable requirement the working-out set. The harder two-phase discharge is precisely what the *cyclic* case would need (a cycle has no owner-before-borrower order, so constructor-parameter injection is impossible and a mutable post-construction bind is unavoidable), and it remains deferred with its proof obligation intact: v0.121 deliberately ships only the sub-problem whose discharge needs no new proof, exactly the project's standing disposition. The `serialized graph` clause is type-scoped for the same reason `serialized tree` is — graph shape is an intrinsic property of the type's serialization contract, the [§5.19](#sec-serialization-without-reflection) placement criterion — and the deserializer's dynamic cycle-detection (a reference whose target is still under construction) is the same assert-and-enforce discipline as the tree case: the assertion the compiler cannot check is checked at read, and its violation is a *detected error*, never silent corruption.

**Polymorphic dispatch (delivered in v0.122): explicit registration, vindicated by the ecosystem's own trajectory.** The polymorphism design was never the hard part — it was deferred only because it was orthogonal and could wait, not because it was unresolved. v0.122 specifies it, and the specific shape is worth recording because the wider ecosystem independently converged on it after learning the alternative the hard way. The mechanism is a closed registry, written explicitly in the `polymorphic` clause, mapping a wire discriminator string to each permitted concrete derived type; the synthesized serializer writes the discriminator first, the synthesized deserializer reads it first and dispatches through the registry, and any discriminator or run-time type outside the registry is a `SerializationException`. There is no reflective subtype discovery. This is exactly the [§5.18](#sec-type-identity-not-reflection) explicit-registration-never-reflection principle, and it is not an arbitrary preference: reflective type-discriminator binders (the historical Newtonsoft `$type` model, which resolved an attacker-controlled string to *any* constructible type) became a notorious remote-code-execution vector, and the modern mainstream (System.Text.Json from .NET 7) deliberately moved to *explicitly opted-in, closed* derived-type registration with undeclared types raising at runtime. Bezel's design lands at the same place by principle that the ecosystem reached by incident — the registry is closed because an open one is a security defect, not merely because closed is tidier. Two further design points were settled by the same external evidence: the discriminator is consumed *before* component binding (a serializer that treats it as an ordinary field hits the documented problem that it cannot then be a required data property and cannot participate in normal field machinery — so Bezel makes it structural, never a data field, never subject to field-name customization or the unknown-field policy); and polymorphism is *type-scoped*, the permitted-subtype set being an intrinsic property of the base type's serialization contract exactly as graph shape is, by the v0.118 placement principle. Polymorphic dispatch composes with `serialized tree`/`serialized graph` — the registry selects *which* concrete type, the graph-shape clause governs *how that type's edges reconstruct* — and is wholly independent of the cyclic deferral: nothing about choosing a concrete type touches the unobservability-under-constructor-side-effects obligation, so v0.122 ships without disturbing the one genuinely open problem.

**The generalization ceiling, reached (v0.124).** Cyclic class synthesis is delivered (v0.124): the spec makes normative exactly what the v0.123 working-out discharged — the self-contained-constructor eligibility rule and the two-phase construction whose intermediate state is provably unobservable, with [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) preserved unmodified. **The serialization stack is complete.** Every class object graph — tree, directed acyclic, or cyclic — is synthesizable; polymorphic edges resolve through an explicit closed registry; field names, missing-field tolerance, and unknown-field policy are all covered. The single remaining reason to hand-write `Serialize`/`Deserialize` is a class whose constructor cannot be made self-contained (construction logic genuinely entangled with its references) — and that is a *recorded, statically-detected, principled exclusion*, the honest residue every prior stage's reasoning anticipated, not an open problem. There is no undischarged obligation and no deferred shape. Stated plainly: serialization began (v0.114) as a format-abstraction vocabulary with the object-graph tier flagged as the genuinely hard part, and ends (v0.124) with that tier fully delivered, each hard question resolved reasoning-first before its spec text, the one irreducible limit (reference-entangled constructors) named and checked rather than hit at runtime. The external corroboration stands as the closing note: Java, with full GC and unrestricted mutable identity, cannot preserve a cycle through an immutable record's constructor for precisely the reason Bezel's two-phase-plus-self-contained-constructor design is engineered around — and Bezel turns that limitation into a compile-time restriction with a stated remedy rather than a silent failure.

**Working out the cyclic case (v0.123, rationale only).** This subsection attempts the discharge the v0.120 working-out named as the single outstanding obligation. It commits no spec text; its purpose is to determine whether the unobservability-under-constructor-side-effects proof can be carried, and to be honest if it cannot be carried in full. The v0.120 analysis left exactly two ways to make the two-phase intermediate state unobservable: (a) restrict the cyclic group's constructors to a statically-checkable "no escape during construction" shape, or (b) bind back-edges before any constructor body runs — and (b) is impossible for a cycle (a cycle has no owner-before-borrower order, so some back-edge's target cannot precede its borrower). The cyclic case is therefore forced onto path (a), and the whole question reduces to: *can "no escape during construction" be defined precisely enough to be a sound static check, and is it sufficient?*

*What "escape during construction" must forbid.* The two-phase deserializer constructs every object in the cyclic group with its back-edge fields unbound, then binds them. The intermediate state is unobservable iff, between the first allocation and the last bind, no code can read a back-edge field of any group object. The only code that runs in that window is the group's own constructor bodies — the synthesized deserializer interleaves no user code (it is a closed compiler-generated region, the property the v0.120 working-out already established). So the proof obligation is exactly: *no constructor body in the group, transitively, observes a not-yet-bound back-edge.* A constructor observes a back-edge if it (i) reads the back-edge field directly, (ii) calls a method on `this` that does, (iii) passes `this` to any code that could, or (iv) calls a virtual method an eligible subtype could override to do so. (iii) and (iv) are the "escape": once `this` leaves the constructor, unobservability of this object's fields is no longer locally checkable.

*The candidate static rule.* A class is **cyclic-serialization-eligible** only if its *construction* is *self-contained* — where "construction" means all code that runs while an instance is built: the constructor body, every constructor reached transitively through `this(...)`/`base(...)` chaining, and every field and property initializer that runs during that construction. All of it, taken together, may assign the instance's own fields and read its own value/owned fields and constructor parameters, but it may not (i) read any field that is a back-edge in the serialized graph, (ii) call any instance method on `this`, (iii) let `this` escape — pass it as an argument, store it in any location, capture it in a closure, or return it, or (iv) call a virtual/abstract method. (The transitive scope is essential, not incidental — but it decomposes into two parts of unequal weight. The field/property-initializer part is *already discharged by the language*: language spec [§22.8](Bezel_Language_Spec.md#sec-field-initializers) universally forbids any class's field initializers from referencing `this`, instance members, `base`, virtual methods, or any `this`-observing expression, so an initializer structurally cannot read a back-edge or call a virtual/instance method in the first place — the proof can simply cite that invariant rather than re-derive it. The load-bearing part is the **constructor chain**: a chained or `base(...)` constructor's *body* is construction-time code that [§22.8](Bezel_Language_Spec.md#sec-field-initializers) does not restrict — it may freely read its own fields and call virtual methods — so restricting attention to the most-derived constructor body would leave a real hole through which a base constructor's virtual call or own-field read could observe the unbound state. The rule closes exactly that hole, and the proof quantifies over the transitive chain for that reason.) This is a strong restriction, but it is *exactly* the restriction the proof needs, and it is statically checkable by the same escape analysis the borrow checker already performs (it is a strict subset of the "does this reference escape this scope" question the language answers everywhere else), run over the transitive construction-time code rather than one body. Under this rule no construction-time code can observe any back-edge — its own or another group member's — because it touches no back-edge field, calls no method that could, and never lets `this` reach code that could. The intermediate state is then provably unobservable: the only code in the window is self-contained construction, none of which can read a back-edge.

*Is it sufficient — and what does it cost?* It is sufficient: the proof goes through. It is also honestly costly, and the cost must be stated, not hidden. A self-contained constructor cannot do initialization work that depends on its references (no "validate the referenced peer," no "cache a value computed from a referenced object"). For the *common* cyclic shapes — parent/child trees with up-pointers, doubly-linked structures, observer graphs — constructors are typically pure field assignment and the rule bites lightly. For classes whose construction logic is entangled with their references, it bites hard: such a class is not cyclic-serialization-eligible and must hand-write `Deserialize` (always available). This is the project's standing disposition — synthesis covers the shape that fits soundly; the shape that fights the foundation is explicitly excluded with the reason recorded — applied one level deeper. It is *not* a relaxation of [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields): the two-phase window binds every back-edge before the object is released and before any observing code runs, so no non-nullable field is ever observed unset; definite assignment holds exactly as for the DAG case, by a different but equally airtight argument (DAG: bound through the constructor; cyclic: bound in a provably-unobservable window before any observer exists).

*The residual honest gap.* One sub-case resists even this: a class whose construction *genuinely requires* observing a mutually-referenced peer (an invariant establishable only once the peer exists) cannot be made self-contained, because the language's constructor-centred initialization model is precisely what makes such logic want to live in the constructor. For these classes the synthesized path is correctly refused; the rule does not solve them, it *excludes* them with a stated reason. So the discharge is real but bounded: cyclic graphs whose participating classes have self-contained constructors are provably synthesizable; cyclic graphs whose construction is reference-entangled remain hand-written, by a recorded principled exclusion rather than an open question. That is a genuine resolution of the obligation — the proof is carried for a precisely delimited, statically-checkable class of programs, and the remainder is a declared boundary, not an unresolved tension.

*Conclusion of the working-out.* The unobservability-under-constructor-side-effects proof **is dischargeable**, via a statically-checkable self-contained-constructor rule the existing escape analysis already has the machinery to enforce, and it discharges *without* weakening [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields). The cyclic case is therefore no longer "the single open problem"; it is a *specifiable* feature with a known eligibility restriction, gated now only on ordinary spec-design work: the wire mechanism for cycle-closing references (the identity/`$ref` table the DAG case already established, extended so a reference may target an object still under construction) and the `serialized` clause spelling. This subsection is the gate-clearing the project's discipline requires before that spec work — the next revision may specify cyclic synthesis for self-contained-constructor classes, exactly as v0.121 specified the DAG case after v0.120 cleared its gate. What is *not* claimed: this does not make every cyclic graph synthesizable; it discharges the proof for the statically-delimited eligible class and converts the remainder from "unresolved" to "explicitly excluded with reason" — the honest and complete form of the answer. *(v0.124: this discharge was made normative — language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) specifies the `serialized cyclic` clause, the self-contained-constructor eligibility rule as a compile-time check, and the two-phase construction, exactly as worked out here and with [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) preserved unmodified. The serialization stack is complete.)*

**Why v0.114 ships only the vocabulary.** The format abstraction is the one piece that cannot be retrofitted: if the type-side interface ever names a format, or the format ever inspects type shape, the separation is lost and every later tier is built on sand. It also costs no language feature and the writing direction is usable by hand the day it ships. Shipping it now, alone, fixes the architectural keystone at zero language cost and lets the synthesis feature, the concrete formats, and the customization mechanism land later as independent, non-breaking additions — each forward-compatible because the vocabulary was designed, like `FromCollectionLiteral` before it, to be promotable rather than replaced. The disposition is the same one applied in [§5.17](#sec-why-no-class-deconstruction) and [§5.18](#sec-type-identity-not-reflection): ship what fits the language now, record the staged plan for what does not yet, keep every open question explicitly scoped.

<a id="sec-one-element-policy"></a>
### 5.20 One element policy, not twelve

v0.132 adds the element slot policy (language spec [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)): a single table — collection family × element category → slot kind, slot kind → the semantics of every element operation — from which all twelve standard collections derive their element behavior. The change is declarative, not behavioral: each collection's documented operations are unchanged; what changes is their *status*, from twelve independently-stated rule sets to twelve presentations of one rule.

**Why.** The implementation experience made the cost of the per-collection statement concrete. The compiler hand-wired each collection's element handling separately, and the defect history clustered exactly there: a long series of bugs in which one collection's handling of one element category diverged from its siblings' — string elements compared by `memcmp` in one list operation but structurally in another, dictionary value decoding correct for one value category and garbage for the next, disposal order right in one container and wrong in its peer (#556, #558–#560, #562–#563, #565–#566, #570–#571, #598 among others). Every one of those bugs is, in the table's terms, a cell filled in inconsistently with its column. A normative table makes the column the authority: an implementation can implement the column once, and a divergence between a per-collection clause and the table is now *defined* to be a spec defect rather than a judgment call. This is the specification-side half of the direction recorded in #545 (implement the element policy once, type-keyed, instead of per collection); the spec now says the twelve behaviors *are* one behavior, so the implementation is licensed to build them that way.

**What was already in place.** The owning column's storage embodiment, `OwnedBuffer<T>` (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans), [§5.15](#sec-owned-buffers-and-spans) above), existed since v0.99/v0.102, and its category-dispatched slot semantics were the prototype for the table; the owning family's rejection of runtime-managed element types was likewise already normative ([§14.4](Bezel_Language_Spec.md#sec-owned-as-a-generic-argument)). v0.132 did not have to invent the policy — it had to *name* it, state it once, and subordinate the per-collection prose to it.

**The deferred half: a borrow-storing primitive.** The table's borrowing column has no user-expressible storage embodiment, and v0.132 deliberately does not add one. The blocker is principled: outside the compiler-known borrowing collections, storing a borrow in a field is rejected ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)) — that rule is what makes borrow lifetimes statically trackable. A user-expressible `Buffer<T>`-of-borrows would need its own lifetime discipline (the collection-as-transit rules of [§28.11](Bezel_Language_Spec.md#sec-collection-element-ownership), generalized and made checkable for arbitrary user containers), which is real design work, not transcription. Until that work is done, borrowing collections remain compiler intrinsics specified by the table. **Declined** rather than attempted now: a quick dispensation ("classes marked somehow may store borrows") would re-open the soundness of the borrow tracker for a convenience the standard library does not itself need — the same shape of reasoning that declined `<owned T>` generic parameterization in v0.97 ([§5.11](#sec-owning-collections)).

<a id="sec-shared-t"></a>
### 5.21 `Shared<T>`: scoped shared ownership

v0.134 adds `Shared<T>`/`Weak<T>` (language spec [§28.13](Bezel_Language_Spec.md#sec-shared-ownership), stdlib [§2.16](Bezel_Standard_Library.md#sec-shared-and-weak)) — the library type [§5.4](#sec-no-shared-ownership) predicted. The addition keeps every commitment that section made: single ownership remains the default and the only *implicit* regime; sharing is opt-in, and its cost is visible at every site (`Clone()` is an explicit call — there is no implicit handle duplication, consistent with the no-implicit-conversions discipline of [§14](#sec-operators)). What changed since §5.4 was written is that the gap stopped being hypothetical: caches handing out entries, registries whose entries outlive any single owner, and the multi-consumer channel question (§12.6) all reduce to "N holders, last one cleans up," and the workarounds (handle tables) push the same reference counting into user code where the compiler cannot see it.

**Why compiler-known rather than pure library.** Faithful shared ownership cannot be built from unique ownership in user Bezel: N handles must keep one control block alive, and "kept alive by N owners" *is* the feature. The precedent is the collection library — compiler-known types with ownership semantics user code cannot express (§28.11) — and the runtime already maintains invisible reference counts for strings and closures (§28.9); `Shared<T>` reuses that machinery with user-visible semantics.

**Why the handles are ordinary owned objects.** The design's entire surface area is the *inside* of the type: count, last-one-disposes, and `Get()`'s borrow-from-handle sourcing (the same dispensation owning-collection indexer reads already have). Everything else — `using var`, owned fields, `move`, branch joins, exception-path discharge, storage in owning collections — applies to handles by virtue of their being class instances, with zero new checker rules. A design where sharing was a new *reference kind* (a third state beside owned and borrowed) was **declined**: it would have touched every join rule, every producer table entry, and every feature that dispatches on ownership state, for no expressiveness the handle form lacks.

**Why domain-local.** The count is unsynchronized; making `Shared<T>` sendable would either race the count or impose atomic operations on every single-domain user. A synchronized `SharedAtomic<T>` (or sendable-`Shared` mode) is the known follow-on — it is also what multi-consumer channels need (§12.6) — and is deferred until a use case forces the cost question; adding it later is non-breaking. Also **declined**: refcounting arbitrary classes via an annotation (hides cost, breaks the structural-facts discipline of [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)-style reasoning), and compiler-inferred sharing via escape analysis (implicit magic; the language's position is that ownership transitions are visible in source).

**The accepted hazard.** Reference cycles through `Shared<T>` leak, and the compiler does not detect them. This is the standard refcount trade, accepted with the standard mitigation (`Weak<T>`, with `TryUpgrade` in the Try-pattern shape the language already has). Cycle *detection* would be a tracing collector by another name — exactly what the language promises not to have.

<a id="sec-nullability"></a>
## 6. Nullability

<a id="sec-field-narrowing-is-not-safe-across-threads"></a>
### 6.1 Field narrowing is not safe across threads

`if user <> null then user.Method() end;` works for locals because the compiler knows nothing else can reassign them. For fields, the same code is a compile error: another thread can write `null` to the field between the check and the use.

The compiler does not narrow fields. The recommended pattern is to copy the field to a local before checking:

```bezel
var u := _user;
if u <> null then u.DoStuff(); end;
```

Some languages (Kotlin) try to narrow fields and produce subtle TOCTOU bugs. Bezel declines to.

<a id="sec-is-postfix-only"></a>
### 6.2 `!` is postfix only

`name!.Length` reads left-to-right; prefix `!name.Length` would have to be parenthesized. There is also a semantic argument: `!` asserts something about the value to its left, and putting the assertion to the left of the asserted value reads backwards.

There is no prefix `!` for boolean negation either; `not` is the boolean operator.

<a id="sec-equality-and-hashing"></a>
## 7. Equality and hashing

<a id="sec-=-invokes-equals"></a>
### 7.1 `=` invokes `Equals`

The most common bug in C# was reference-equality on classes that should have had value equality. Bezel chooses the other way: `=` invokes `Equals`. For records, `Equals` is value equality by default. For classes, `Equals` is reference equality by default but overrideable.

The common case does the right thing. For genuine reference comparison, `Object.ReferenceEquals(a, b)` is available.

<a id="sec-=-cannot-be-overloaded-on-classes"></a>
### 7.2 `=` cannot be overloaded on classes

Operator overloading of `=` on classes invites bugs: the override defines value equality, but the rest of the language treats classes as reference types in many places (collections that hash on identity, `is` checks). Classes customize equality through `Equals`; records and enums may overload `=` and `<>` freely.

<a id="sec-overriding-equals-requires-overriding-gethashcode"></a>
### 7.3 Overriding `Equals` requires overriding `GetHashCode`

The "if you override one, override both" invariant is a source of bugs in other languages because it's a documentation rule, not a language rule. Bezel enforces it at compile time. The cost is a `GetHashCode` on types that may never be hashed; the benefit is that no Bezel type can produce silent dictionary-lookup bugs years later when someone refactors.

<a id="sec-float-equality-is-reflexive"></a>
### 7.4 Float equality is reflexive

`NaN.Equals(NaN)` is `true`. `(-0.0).Equals(0.0)` is `true`. The relational operators `<`, `<=`, `>`, `>=` keep IEEE semantics — any comparison involving `NaN` is false.

The asymmetry is intentional. Equality is what hash tables, sets, and `case` patterns use; an irreflexive `=` makes `Set<double>` misbehave when NaN is inserted. Relational operators retain IEEE semantics so numeric algorithms work as published. Strict IEEE equality is available as `Double.IeeeEquals`; bit-pattern equality is `Double.BitwiseEquals`.

<a id="sec-inheritance"></a>
## 8. Inheritance

<a id="sec-member-hiding-is-forbidden"></a>
### 8.1 Member hiding is forbidden

C# allows a derived member with the same name as a base member to *shadow* (not override) it, with dispatch depending on the static type of the receiver. Almost no one understands the semantics correctly, and the rare cases where it's the right tool are better solved by renaming.

Bezel rejects same-name members in derived classes outright. To override a virtual base member, write `override`. To add something new, choose a different name.

<a id="sec-class-hierarchy-exhaustiveness-in-case"></a>
### 8.2 Class-hierarchy exhaustiveness in `case`

A sealed hierarchy can be matched exhaustively without a `_` arm: if the base class is `sealed` (or only has `sealed` subclasses, recursively forming a closed set) and every concrete leaf type appears as a pattern, the match is provably total.

This is the partial form of sum types — useful where it applies, no language additions required. Full sum types remain deferred ([§18](#sec-things-deferred)); the hierarchy form covers most of the use cases.

<a id="sec-records-and-classes"></a>
## 9. Records and classes

<a id="sec-why-records-cannot-have-destructors-or-owned-fields"></a>
### 9.1 Why records cannot have destructors or `owned` fields

A record is copied on assignment. If a record had a destructor, every copy would need to track whether it owned or borrowed the underlying resource — C++ value semantics, with their attendant complications.

The clean rule: records hold values and borrowed references. They never own resources. If you need ownership, use a class.

<a id="sec-why-records-are-sealed"></a>
### 9.2 Why records are sealed

Inheritance and value-equality conflict. The classic problem: does `Square(5, 5) = Rectangle(5, 5)`? Either answer is wrong in some context. Records side-step this by being sealed. They may implement interfaces, which gives polymorphism without the equality problem.

<a id="sec-auto-equality-on-body-only-records"></a>
### 9.3 Auto-equality on body-only records

A record whose body contains only auto-properties and methods (no explicit fields, no computed properties with side effects) gets auto-generated `=`, `<>`, and `GetHashCode` over those auto-properties. This is what makes `DateTime`, `TimeSpan`, `Guid`, and similar standard types work as equality-bearing without per-type overloads.

The previous rule ("body-only records get no auto-equality") was simpler but forced every standard library record to write `=`/`<>` overloads explicitly, with no benefit. The refined rule keeps the simple cases simple.

<a id="sec-enum-interface-implementation"></a>
### 9.4 Enum interface implementation

v0.60 prohibited enums from implementing interfaces, even though enums could already declare methods and properties with the same signatures. This meant a user enum with a `CompareTo` method could not declare `: IComparable<MyEnum>`, making it invisible to generic algorithms constrained on interfaces. The restriction was artificial — there is no semantic obstacle to an enum implementing an interface, since the interface methods are simply dispatched to the enum's declared methods.

v0.61 lifts the restriction. Enums may now declare interface conformance. The interfaces appear after the underlying type in the inheritance list: `enum Priority: int, IComparable<Priority>`. The enum must provide matching public members for every interface member, just as classes and records do.

<a id="sec-sealed-abstract-classes"></a>
### 9.5 `sealed abstract` classes

The sum-type pattern — a closed set of alternatives matched exhaustively — is common in language design and appears frequently in Bezel's own `case` matching. v0.60 supported this via `abstract` base classes whose subclasses are all `sealed`, but the base class itself could not be both `abstract` and `sealed`, so external code could always subclass it (breaking exhaustiveness guarantees across module boundaries).

v0.61 permits `sealed abstract` on a class declaration. The meaning is: the class cannot be instantiated (abstract) and cannot be subclassed outside its declaring module (sealed). Within the declaring module, subclassing is permitted. This gives the language a mechanism for closed hierarchies that is enforced by the compiler rather than by convention.

The `sealed abstract` combination was previously rejected because `sealed` and `abstract` seemed contradictory. The resolution is that `sealed` on a class means "no external subclasses" rather than "no subclasses at all" — the module boundary is the seal.

<a id="sec-generics"></a>
## 10. Generics

Two extreme implementations exist. Erasure (Java) is fast to compile but produces poor codegen and disallows reflection on type parameters. Monomorphization (C++ templates, Rust) produces excellent codegen but slow compile times and binary bloat.

Bezel's intended implementation specializes for value-type substitutions (where boxing would hurt) and shares for reference-type substitutions (where uniform layout is achievable). This gives most of the codegen benefit of monomorphization where it matters, and most of the binary-size benefit of erasure where it doesn't.

`typeof(T)` and `value.GetType()` work because the runtime carries enough type information to resolve them, regardless of whether a particular generic was specialized or shared.

<a id="sec-pattern-matching"></a>
## 11. Pattern matching

`case` is the multi-way conditional. Most of its design follows ML/Scala/F# precedent. Two Bezel-specific points:

<a id="sec-the-scrutinee-less-form"></a>
### 11.1 The scrutinee-less form

A common need is "test totally different conditions." Forcing those into a scrutinee-having `case` would require gymnastics. Bezel's `case` permits omitting the scrutinee, with each arm being a full boolean test:

```bezel
var grade := case
  score >= 90 => "A",
  score >= 80 => "B",
  score >= 70 => "C",
  _ => "F"
end;
```

This subsumes most `if-else-if` chains.

<a id="sec-naked-binding-inside-positional-patterns"></a>
### 11.2 Naked binding inside positional patterns

Inside `Point(x, y)`, `x` and `y` are bindings, not type tests. The explicit `Point(var x, var y)` is also accepted. Outside positional destructuring (at the top level of a pattern), an identifier is a type test, so a binding requires the `var` form.

The naked-identifier shorthand is allowed only where positional bindings are unambiguous, which is the common case and the natural reading.

<a id="sec-catch-is-case-for-exceptions"></a>
### 11.3 `catch` is `case` for exceptions

Bezel's `catch` is a multi-way pattern dispatcher: it takes a thrown exception and selects a handler based on type, optionally with a guard, optionally binding the value to a name. That is exactly what `case` does for ordinary values. v0.63 makes the syntax reflect this — `catch` arms now mirror `case` arms in every detail that matters.

The shape:

```bezel
try
  DoWork();
catch
  var ex: TimeoutException => RetryWithBackoff(ex),
  var ex: IOException, NetworkException => Retry(ex),
  InvalidOperationException => RecordInvalidState(),
  var ex: Exception => throw                 // re-raise unrecognized
end;
```

Arms are comma-separated. Bindings use `var` exactly as in `case`. Guards use `when` exactly as in `case`. Multi-type alternatives use comma exactly as in `case` constant alternatives. Arm bodies are expressions or `do ... end` blocks, exactly as in `case`.

The previous syntax — repeated `catch ... then ... end` clauses — was inherited from C# without examination. C# inherited it from Java, which inherited it from C++. Each step preserved the surrounding shape because changing it would have broken existing code; Bezel has no such constraint and can pick the form that fits the rest of its design.

The same logic applied to C#'s `switch` statement and Bezel's `case`: C# has the heavy `case Type: ... break;` shape from C, while Bezel's `case` is arm-shaped from the start. The argument was structural — pattern dispatch reads more clearly as arms than as a sequence of labeled clauses. The argument applies to exception handlers too; v0.63 finally acts on it.

<a id="sec-why-the-change-was-deferred"></a>
#### Why the change was deferred

v0.62 explicitly deferred this change to give it dedicated attention. Three concerns held it back, all of which had answers but each of which deserved being thought through.

The first was the `=>` token's reading. In `case` expressions, `=> body` reads as "produces this value." In `catch`, the body is statement-position; nothing is produced. Resolved by noting that statement-position `case` already establishes that `=>` means "body for this match" without producing a value — the `case` and `catch` arms are both statement-position constructs in this respect.

The second was exhaustiveness. `case` warns when it can't prove exhaustiveness. `catch` should not — a `try` block usually catches a few specific exceptions and lets others propagate. Resolved by noting that exhaustiveness is a `case`-specific behavior, not a property of arm-shaped syntax. `catch` borrows the syntax without inheriting the exhaustiveness rule, just as statement-position `case` borrows the syntax without inheriting the value-producing rule.

The third was the cost on the dominant case. Most `catch` clauses today catch a single exception type and run a few statements. The new shape requires `do ... end` around any multi-statement body, where the old syntax accepted a bare statement sequence. This is real overhead for the most common form. The tradeoff: three extra tokens (`do`, `end`, comma) on the common case, in exchange for structural consistency with `case` and visibly cleaner multi-handler code. The empirical analysis was a near-tie on density; the structural argument tipped the decision.

<a id="sec-why-bindings-use-var"></a>
#### Why bindings use `var`

`for var i in coll`, `case ... var n: int => ...`, `out var name`. Every site in Bezel where a name is being introduced as a fresh local binding uses `var`. Catch bindings are no different — they introduce a name for the caught exception. Marking them `var` is consistency with the rest of the language, not new ceremony.

The cost is three letters per binding-form arm. The benefit is that a reader scanning code sees `var` and knows "binding being introduced," with no exception case to remember.

<a id="sec-multi-type-alternatives"></a>
#### Multi-type alternatives

The old syntax forced each exception type into its own `catch` clause. To handle two exception types identically, programmers either duplicated the body or used the `when ex is A or ex is B` workaround (documented in v0.62 [§31.2.1](Bezel_Language_Spec.md#sec-arm-patterns)). The new syntax expresses multi-type catches directly:

```bezel
catch
  var ex: IOException, TimeoutException => Retry()
end;
```

This matches Java's `catch (A | B)` and Python's `except (A, B):` in expressive power; the syntax is comma-separated rather than `|`-separated to match Bezel's `case` constant-alternative pattern. Inside the arm body, `ex` has the static type of the nearest common base — usually `Exception`. For type-specific handling within a multi-type arm, use a nested `case ex` or `is` test, but the common case (treating multiple types uniformly) doesn't need that.

<a id="sec-bare-throw-as-an-expression"></a>
#### Bare `throw` as an expression

The transition to arm-form `catch` surfaced an asymmetry in v0.62: `throw expr` was an expression (with bottom type, valid in any expression position), but bare `throw;` was statement-only. In the old `catch ... then statements end` form, this didn't matter — both fit naturally in a statement sequence. In the new arm form, where bodies are `=> expression` or `=> do statements end`, the asymmetry would have forced bare re-raises into the heavier `do ... end` form even when the re-raise was the entire body:

```bezel
// Without the change:
catch
  var ex: TransientException => Retry(),
  var ex: Exception => do throw; end       // do/end just for one statement
end;
```

v0.63 resolves this by promoting bare `throw` to an expression form too. The operand becomes optional: `throw expr` raises the supplied exception, `throw` (no operand) re-raises the current exception. Both have bottom type. The bare form is restricted to catch arm bodies — the same restriction as the statement form `throw;`. The natural arm shape becomes:

```bezel
catch
  var ex: TransientException => Retry(),
  var ex: Exception => throw
end;
```

The statement form `throw;` continues to work and means the same thing as the expression form followed by `;`. Inside `do ... end` arm bodies that perform multiple steps before re-raising, the statement form is still natural:

```bezel
catch
  var ex: SqlException => do
    metric.RecordSqlError(ex);
    throw;                                  // statement form, in a sequence
  end
end;
```

Both forms coexist; the choice between them is whether the re-raise is the whole arm body (use the expression form) or one step of several (use the statement form). This is the same expression-vs-statement choice that programmers already make for method calls, `case` arms, and other expressions that can also stand alone as statements.

The change is purely additive — every program that compiled in v0.62 continues to compile in v0.63 — but it materially improves the arm-form's ergonomics. Without it, the most common idiom in catch arms that re-raise (handle some types specifically, re-raise everything else) would have been heavier than the form it replaced. With it, the idiom reads cleanly:

```bezel
catch
  var ex: TransientException => Retry(),
  Exception => throw
end;
```

<a id="sec-what-catch-and-case-still-dont-share"></a>
#### What `catch` and `case` still don't share

The unification is structural, not total. Exception dispatch and value dispatch differ in two ways the syntax doesn't try to hide.

The first is the source of the value being matched. `case` matches an explicit scrutinee (or evaluates boolean conditions in scrutinee-less form). `catch` matches the implicit "currently-thrown exception" — there is no scrutinee to name, and the arms can only test types. A non-type `catch` arm pattern (a literal, a range, a record-destructuring) makes no sense and is a syntax error.

The second is the behavior on no match. `case` in expression position raises `MatchFailureException` on no match; `case` in statement position falls through silently. `catch` does neither — unmatched exceptions propagate to the enclosing handler. This is the right behavior for exception dispatch, and it is documented as a third position in [§19.6](Bezel_Language_Spec.md#sec-exhaustiveness) (alongside expression-position and statement-position `case`).

These differences aren't gaps; they're the parts of exception handling that don't reduce to value pattern matching. The syntax unifies the parts that are the same and leaves the parts that differ to the semantics.

<a id="sec-async"></a>
## 12. Async

<a id="sec-pattern-a-vs-pattern-b-is-type-directed"></a>
### 12.1 Pattern A vs Pattern B is type-directed

The interaction between async and ownership is the hardest design problem in the language. A class-typed task result must end up somewhere with defined disposal — but that somewhere is sometimes "the task itself, with multiple readers borrowing through it" (a fan-out cache, a future shared between subscribers) and sometimes "a fresh single owner" (a connection from `ConnectAsync`, a token from `AcquireAsync`).

The producer's return type carries the choice:

- `Task<T>` for class T means fan-out. Every awaiter borrows. The task must be held in `using var` for the borrow to be valid.
- `Task<owned T>` for class T means single-consumer. One awaiter takes ownership; the task is consumed by `using var x := await t;`. A second await raises `InvalidOperationException`.

The rule reduces to "whatever the type says." Producers commit in their signature; consumers' obligations follow from that signature. There is no syntactic peek at the await context.

The motivating case is `Semaphore.AcquireAsync(): Task<owned SemaphoreToken>` — an operation whose entire purpose is to produce a fresh, single-consumer token. The consumer writes the only syntactic shape that compiles:

```bezel
using var token := await sem.AcquireAsync();
DoCriticalWork();
```

Producers who want both shapes declare two methods with distinct names.

<a id="sec-choosing-between-taskt-and-taskowned-t"></a>
### 12.2 Choosing between `Task<T>` and `Task<owned T>`

The choice between Pattern A and Pattern B is the producer's — it goes in the method signature and determines what every caller must do. The decision tree below is from the producer's perspective.

**Step 1: Is the result type a value type, `string`, or runtime-managed type?**

If yes, neither pattern applies. Return `Task<T>` and stop here. Value types are copied out; `string` and function values are runtime-managed. No ownership crosses the await boundary.

```bezel
public async method CountAsync(): Task<int>          // value type — no pattern
public async method FetchNameAsync(): Task<string>   // string — no pattern
```

**Step 2: The result is a class type. Does the operation produce a fresh resource the caller should own and dispose?**

The hallmark of a resource-acquisition operation: the caller asked the system to create, open, connect, or acquire something, and the caller is the one who must close, release, or dispose it when done. The operation's purpose is to hand over a resource.

If yes → **Pattern B: `Task<owned T>`**

```bezel
public async method ConnectAsync(url: string): Task<owned Connection>
public async method AcquireAsync(): Task<owned SemaphoreToken>
public async method OpenAsync(path: string): Task<owned FileStream>
```

The caller writes:

```bezel
using var conn := await ConnectAsync(url);
// conn is owned; disposed at scope end.
```

**Step 3: The result is a class type, but the operation is not producing a fresh resource. Could multiple consumers need the same result?**

This covers caches, shared lookups, fan-out patterns — any case where the result lives longer than a single consumer, or where multiple awaiters read from the same task.

If yes → **Pattern A: `Task<T>`**

```bezel
public async method FetchConfigAsync(): Task<Config>
public async method LoadProfileAsync(id: int): Task<UserProfile>
```

The caller writes:

```bezel
using var task := FetchConfigAsync();
var config := await task;              // config is borrowed from the task
UseConfig(config);
// task disposed at scope end; config disposed with it.
```

Multiple awaiters may share the task:

```bezel
using var task := FetchConfigAsync();
var c1 := await task;                  // borrow
var c2 := await task;                  // same borrow, same underlying object
```

**Step 4: The result is a class type produced by a query or lookup. The caller doesn't own it — something else does (a repository, a cache, a parent object).**

This is still Pattern A. The task borrows from whatever produced the result; the caller borrows through the task.

```bezel
public async method FindUserAsync(id: int): Task<User>
public async method GetCachedItemAsync(key: string): Task<CacheEntry>
```

**Summary**

| Question | Answer | Pattern | Return type |
|---|---|---|---|
| Result is value type / string / runtime-managed? | Yes | Neither | `Task<T>` |
| Operation produces a fresh resource the caller must dispose? | Yes | B | `Task<owned T>` |
| Multiple consumers may read the same result? | Yes | A | `Task<T>` |
| Result is borrowed from an existing owner? | Yes | A | `Task<T>` |

**When in doubt, choose Pattern A.** It is the conservative default: the task owns the result, callers borrow, and no one can accidentally leak. Pattern B is the explicit opt-in for "I am handing you a resource; you are now responsible for it." If the producer isn't sure whether the caller needs ownership, Pattern A keeps the decision reversible — the producer can widen to Pattern B later without breaking existing callers (they'll just need to add `using var`). Going the other direction — narrowing from Pattern B to Pattern A — is a breaking change because callers already wrote `using var x := await t;` and relied on owning the result.

**The one question that settles it:** *"If the caller forgets to dispose the result, is that a resource leak?"* If yes, Pattern B — force the caller to take ownership. If no (the result is just data, or someone else owns it), Pattern A.

<a id="sec-no-configureawait"></a>
### 12.3 No `ConfigureAwait`

C#'s `ConfigureAwait(false)` exists because the default is to capture the synchronization context, and library code needs to opt out. Bezel's runtime has no synchronization contexts in this version, so no opt-out is needed. If UI-thread affinity is added later, this will need revisiting.

<a id="sec-cooperative-cancellation"></a>
### 12.4 Cooperative cancellation

Forcible cancellation (Thread.Abort) is unsafe — it can interrupt code holding a lock, corrupt invariants, or skip cleanup. Bezel uses cooperative cancellation: a `CancellationToken` is passed in and the operation checks it at safe points. This is the same model as C# and Rust async.

<a id="sec-execution-domains"></a>
### 12.5 Execution domains: why the async model is serial per domain

Through v0.130 the spec said async continuations "may resume on any thread-pool thread" and that shared mutable state "requires explicit synchronization," while the rationale (§5.1) accepted that programmers can write data races. Stated together, those two positions made the ownership chapter unsound in any program that actually exercised the stated freedom: the borrow checker's lifetime reasoning assumes nothing else runs between two points of a continuation with no suspension between them, yet the runtime model permitted another continuation — on another thread — to dispose a borrow's source mid-use. The language was protecting against the *smaller* hazard while permitting the larger one: §6.1 refuses to narrow nullable fields because another thread could write null between check and use, but a concurrent thread was permitted to *dispose* the object a borrow points into. v0.131 resolves the contradiction in favor of the ownership model: a domain is a unit of serial, run-to-completion execution, and every program in this revision is a single-domain program. The wording change costs existing programs nothing — no construct could observe genuine continuation parallelism portably, since any program doing so had data races whose behavior was already undefined — and it converts the §28 soundness assumptions from accidents of the current implementation into normative guarantees.

Three alternatives were considered and **declined**. *Send/Sync-style thread-safety typing* (Rust's approach) statically tracks which types may cross threads; it is the right tool for a language committed to shared-memory parallelism, but it infects every generic signature, roughly doubles the constraint vocabulary, and buys safety for a parallelism model Bezel does not actually offer — the cost lands on every user, the benefit on few. *Documenting the status quo* (keep "any thread, synchronize it yourself") was declined because it leaves the flagship feature unsound and outsources the hardest correctness problem to convention — the precise failure C++ shows daily. *Locks as discipline* (rely on `Mutex` use) was declined because nothing checks it; a forgotten lock is exactly as silent as the race it permits. The serial-domain model, by contrast, requires no new constructs, no new annotations, and no migration.

The decided direction for *parallelism* — multiple domains communicating by ownership transfer over channels, with sendability derived from existing type properties (owned transfer for classes, the §9.7 borrow-bearing computation for value types) — is recorded in the project tracker (#600) and deferred to a later revision. The one capability genuinely given up in the interim is CPU-bound parallelism inside one process; I/O-bound concurrency, the dominant async workload, is interleaving at suspension points and is unaffected by serialization of continuations. `Mutex`, `Semaphore`, and `Atomic<T>` (§30.12) are retained: foreign threads can still enter through the FFI, and the types are forward-compatible with multi-domain programs. *(v0.133: the deferred direction was delivered — language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)–[§30.15](Bezel_Language_Spec.md#sec-channels); the reasoning is §12.6 below.)*

<a id="sec-sendability-spawning-channels"></a>
### 12.6 Sendability, spawning, and channels

v0.133 delivers the parallelism half of the domain model: `Domain.Spawn` creates domains, `Channel<T>` moves data between them, and the **sendable** derived type property ([§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) instance, defined in [§30.14](Bezel_Language_Spec.md#sec-sendable-types)) is the entire safety story. The design's one invariant — *no mutable state is reachable from two domains* — is what makes the v0.131 single-domain guarantees survive parallelism unchanged: every crossing value is moved (the origin domain provably loses it, by the same machinery as `move` and owned parameters) or immutable (`string`, value copies), and the channel core is the single compiler-known synchronized exception. Data-race freedom is purchased with the ownership system the language already had, not with a new type system. The decisions, and what was declined:

**Sendable classes must be `sealed` (or `sealed abstract` over a fully-sendable closed hierarchy).** The subtyping hole forced this: sendability is checked on static types, but an owned field typed at a sendable open class could hold, at runtime, a subclass instance carrying a back-reference into the origin domain — a data race smuggled past the static check. Three closures of the hole were considered. *Sealing* (chosen): zero runtime cost, compile-time diagnosis, statically airtight — at the price of excluding open classes from crossing domains. *A runtime send-check* (walk the transferred ownership tree at the send site, verifying each concrete class is transfer-closed): sound and more permissive, **declined for now** because it hides a per-send cost proportional to the transferred graph and converts a compile-time error into a runtime one; it remains the natural widening if sealing proves too narrow, and admitting it later is non-breaking. *Subclass-must-be-sendable inheritance rules* (deriving from a sendable class may not break self-containment): declined because it inverts the dependency — a base author's coincidental sendability would constrain every subclass author, violating the principle that derived type properties are observations, not obligations.

**Spawn entries are capture-free; data enters through explicit arguments.** The alternative — *move-capturing lambdas* (a closure form that takes ownership of its captures) — was declined: §28.9's capture-is-borrow rule is a single uniform story for every closure in the language, and carving out a second capture mode for one call site would ripple into closure typing, disposal of captured-but-never-run closures, and the §28.3 escape analysis. An explicit `owned arg` parameter is semantically identical, visibly costed at the call site, and required no new machinery. Multiple values aggregate into a sendable class, an `OwnedArray`, or flow through a channel.

**Faults cross as `DomainFaultedException`, not as the original exception object.** Exception classes are not in general sendable (they are open classes, often carrying arbitrary state), and special-casing "exception objects may cross anyway" would reopen the invariant exactly where programs are least disciplined. Copying the message and stack-trace text (strings — sendable) preserves the diagnostic value; programs needing structured fault data return it as a sendable result or send it over a channel. Declined: a sendability requirement on throwable types (intrusive, retroactive) and runtime graph-checking of exception objects (the runtime-check costs above, on the unhappy path).

**Channels are MPSC, with the `Channel<T>` object as the receiving end and `NewSender()` minting sender endpoints.** Declined alternatives: *a `Create` returning two endpoints via `out owned` parameters* — more ceremony at every creation site and a third type, for no expressiveness gain over minting senders on demand; *multi-consumer channels* (receiver cloning) — cloning a receiving end is shared ownership of the consume capability, which is #603's territory and is deferred until `Shared<T>` exists; *bounded channels* — back-pressure wants an asynchronous `Send` that awaits capacity, a useful but separable design, deferred. The unbounded MPSC form covers the dominant fan-in/pipeline shapes and every deferral is an additive extension.

**`CancellationToken` does not cross domains.** Not by a new rule — it falls out: the token is a borrow-bearing record, hence not sendable. The cross-domain cancellation pattern (a dedicated channel carrying a cancel message) is documented rather than mechanized; a sendable cancellation primitive can be added later without disturbing anything in this revision.

<a id="sec-events"></a>
## 13. Events

<a id="sec-raise-instead-of-invoke"></a>
### 13.1 `Raise` instead of `?.Invoke`

The C# idiom `event?.Invoke(this, args)` carries two operators of boilerplate. The `?.` exists because an empty event is a null delegate. The `Invoke` exists because delegates need explicit invocation. Forgetting the `?.` raises `NullReferenceException` for an empty event.

Bezel uses `event.Raise(payload)` (sync) and `await event.RaiseAsync(payload)` (async). Three properties:

- **No null check.** Zero subscribers is just zero subscribers; `Raise` is a no-op. Whether the implementation backs the event with a null sentinel is invisible at the call site.
- **No `Invoke` indirection.** The verb `Raise` means "call all subscribed handlers." That replaces both `?.` and `Invoke`.
- **Symmetric with `Subscribe`.** Outside the declaring class, an event has type `IEvent<T>` (only `Subscribe`). Inside, `EventSource<T>` (both `Subscribe` and `Raise`). The asymmetry — outside callers can't `Raise`, inside callers can — is enforced by the static type, not by a special access rule.

Direct invocation `event(args)` was considered. It's cleaner at the call site but conflicts with the asymmetric inside/outside types — "why is `event(args)` legal here but not there?" is harder to explain than "the type changes."

<a id="sec-payload-types-not-handler-types"></a>
### 13.2 Payload types, not handler types

v0.53 declared events with a handler function type: `event Clicked: (Button, ClickEventArgs) -> ()`. The generic parameter `TArgs` in `EventSource<TArgs>` was defined as "the handler function type." This created a type-system incoherence: `Subscribe(handler: TArgs)` was correct (handler *is* a function), but `Raise(args: TArgs)` would mean `Raise` takes a function, not the event's data. The call site worked around this with pseudo-tuple syntax that had no foundation in the type system.

v0.54 fixes this by making events name a **payload type**: `event Clicked: ClickedEvent`. The generic parameter `T` in `EventSource<T>` is the payload, and `Raise(payload: T)` is a well-typed generic method that means exactly what it says. The handler type `(T) -> ()` is derived mechanically from the payload type and the sync/async modifier.

Three options were considered:

1. **Compiler-synthesized `Raise`.** Keep the function-type declaration; the compiler generates a per-event `Raise` whose parameters mirror the handler's. This would be the first compiler-generated method not expressible in the generic type — a crack in the "no hidden magic" principle.
2. **Payload types (chosen).** Events name a payload record. `Raise(payload: T)` is a normal generic method. The handler type is derived.
3. **Tuple types.** Add language-level tuples to make the handler's parameter list a first-class type. Tuples are a large feature commitment for a single use case.

Option 2 wins because it requires no new language concepts, keeps `EventSource<T>` fully expressible as a generic class, and aligns with the existing recommendation ([§24.2](Bezel_Language_Spec.md#sec-payload-records)) that event arguments be records. The cost — one record declaration per event — is modest and produces better-documented APIs than multi-parameter handlers.

The subscribe-side trade-off is that handlers take a single payload parameter instead of multiple: `(e) => e.X` instead of `(btn, args) => args.X`. This could be partially recovered by lambda-parameter destructuring (`(ClickedEvent(_, x, y, _)) => ...`) in a future revision, but the single-parameter form is clear enough on its own.

<a id="sec-method-reference-subscriptions"></a>
### 13.3 Method-reference subscriptions

Once payload records are the convention, method-reference subscription becomes the natural idiom for handlers that have a name worth keeping:

```bezel
public method UpdateStatus(e: ClickedEvent)
  // ...
end UpdateStatus;

button.Clicked += UpdateStatus;     // method reference; no lambda wrapping needed
```

The method's signature `(ClickedEvent) -> ()` matches the handler type derived from the event's payload. The compiler resolves the `+=` form to `button.Clicked.Add(UpdateStatus)` via the desugaring described in spec [§24.4](Bezel_Language_Spec.md#sec-subscribing). Lambdas remain available for ad-hoc handlers; named methods are preferred when the handler has a stable identity that supports later `-=` removal.

<a id="sec-events-on-interfaces"></a>
### 13.4 Events on interfaces

v0.61 prohibited event declarations in interfaces, deferring the feature pending design work on the interaction between interface-level events and the `IEvent<T>`/`EventSource<T>` type split. v0.62 lifts the restriction.

The resolution is straightforward given the type split that already existed. From outside the declaring class, every event has type `IEvent<T>` (sync) or `IAsyncEvent<T>` (async). That's exactly what an interface event member should expose: the public, subscribe-only surface of an event. The implementing class declares the event normally; its compiler-generated `EventSource<T>` backing satisfies the interface contract because `EventSource<T>` implements `IEvent<T>`. No new mechanism is required — the existing types compose to give interface events for free.

The motivating use cases are conventional: `INotifyPropertyChanged` and analogous observable contracts, UI component abstractions where multiple concrete types share an event-bearing interface, and plugin extension points where the host subscribes to events declared by the plugin contract. Without interface events, each of these patterns required either an abstract base class (which consumes the single-inheritance slot) or an awkward `GetEventX(): IEvent<X>` method that worked around the missing language feature. The cost of supporting the feature is one new grammar production (`IfaceEvent` in Appendix A.4) and a paragraph of semantics; the benefit is that observable contracts become idiomatic.

C# has supported interface events since C# 1.0 (2002); the precedent is well-established. The deferral in v0.61 was conservative; v0.62's resolution does not require any changes to the existing event system.

<a id="sec-handler-exception-policy-first-error-wins-not-aggregate"></a>
### 13.5 Handler exception policy: first error wins, not aggregate

When a synchronous event handler throws, subsequent handlers in the subscription list are not invoked, and the exception propagates out of `Raise` to the raiser. The same rule applies to async event handlers: if one handler's task faults or is cancelled, subsequent handlers are not invoked and the exception propagates from the awaited `RaiseAsync`. This is "first error wins": the publisher sees one exception and that's the end of the dispatch.

The alternative — running every handler regardless of failures and aggregating the exceptions — was considered. C# went this route for `Task.WhenAll` (which produces an `AggregateException`) and some event-like patterns. Bezel chose first-wins for three reasons.

The first is that aggregation introduces a special exception type that callers must handle differently. `AggregateException` in C# has a complex API (`Flatten`, `InnerExceptions`, `Handle`) and exists primarily because async-with-many-tasks produces multi-failure shapes that ordinary stack-walking exceptions cannot represent. Events are a different shape: a publisher fires once, multiple handlers respond, and the publisher usually does not care about all of them — it cares that *something* failed. Promoting "something failed" into a multi-exception object asks every event consumer to know about an aggregation type.

The second is that handlers that need to run independently can opt in trivially:

```bezel
button.Clicked += async (e) => do
  try
    await ProcessAsync(e);
  catch
    var ex: Exception => Log(ex)        // swallow per-handler failures
  end;
end;
```

The handler catches its own exceptions, returns a successful task, and `Raise`/`RaiseAsync` continues to the next subscriber. Each handler decides whether its failure should poison the rest of the dispatch. This puts the policy where the failure semantics are known — in the handler, which knows whether its work is best-effort or critical — rather than in the publisher, which usually doesn't.

The third is that aggregation across handler boundaries can mask bugs. If a handler throws because it has a real defect (a null-deref, a state-machine violation), the publisher silently aggregating and continuing means the bug surfaces later, in a different code path, with the original stack trace already obscured by the aggregation wrapper. First-wins keeps the original exception visible at the point it was thrown.

The cost is real: a publisher that wants every handler to run regardless of failures must implement that policy itself, typically by catching inside each handler at subscribe time. That's the trade we're making — a small amount of subscriber-side ceremony for a simpler default and clearer failure semantics.

The published policy is normative (language spec [§24.5](Bezel_Language_Spec.md#sec-raising-events)). Future revisions could add an opt-in aggregating raise method (e.g. `RaiseAggregated(payload: T)`) without breaking existing code, but the default would not change.

<a id="sec-operators"></a>
## 14. Operators

<a id="sec-++-and-are-statements"></a>
### 14.1 `++` and `--` are statements

Statement-only, no expression form. `i++` vs `++i`, `arr[i++]`, sequence-point confusion, undefined behavior — none of those exist in Bezel because `++` produces no value. The cases where the old or new value is genuinely needed are clearer when written explicitly.

<a id="sec-mandatory-parentheses-on-mixed-precedence"></a>
### 14.2 Mandatory parentheses on mixed precedence

Mixing bitwise (`&`, `|`, `^`) with equality, relational, or logical operators in the same expression is a syntax error. `a & 1 = 0` doesn't compile; write `(a & 1) = 0`.

The rule costs nothing on programs that mean what they say and catches the most common precedence pitfalls inherited from C. Shift mixed with arithmetic is the same: `a << 2 + 1` is rejected; parenthesize.

<a id="sec-comparison-operators-must-be-defined-together"></a>
### 14.3 Comparison operators must be defined together

`<`, `<=`, `>`, `>=` together. `=` and `<>` together. Languages that derive `>=` from `not (a < b)` get the NaN cases wrong. Forcing each to be defined explicitly keeps the semantics explicit.

<a id="sec-not-is-not-user-overloadable"></a>
### 14.4 `not` is not user-overloadable

`not` is the boolean negation operator. It is not user-overloadable. v0.61 listed `not` among the user-overloadable operators ([§25.1](Bezel_Language_Spec.md#sec-overloadable-operators)) and contradicted the [§6.5](Bezel_Language_Spec.md#sec-bitwise-operations) statement that `not` is "valid only on `bool`." v0.62 resolves this by prohibiting `not` overloading entirely.

The reasoning: `not` reads as a boolean operation. A reader seeing `not x` expects `x` to be (or to behave like) a `bool`. Overloading `not` on arbitrary types would invite confusion — three-valued logic, set-complement semantics, "negate" as in "produce additive inverse" all have superficial appeal, but each interpretation departs from the boolean reading in ways that are easy to misread. Domain-specific inversion is better expressed by a named method (e.g. `.Negate()`, `.Invert()`, `.Complement()`), which makes intent explicit at every call site.

The bitwise complement operator `~` remains user-overloadable. Types that have a meaningful bit-pattern inversion or set-complement operation can overload `~`; types with logical-style negation should provide a named method.

<a id="sec-no-implicit-conversion-operators"></a>
### 14.5 No implicit conversion operators

Implicit conversions make overload resolution unpredictable, hide allocation costs, and let inappropriate values silently flow into APIs. Bezel has no `implicit` keyword; explicit conversion is the only user-defined conversion mechanism.

**Overload resolution tie-breaking (v0.128).** v0.127's §22.6 said overloads are distinguishable by number, type, and `ref`/`out` modifiers, but was silent on how to choose when two applicable overloads are each reachable through a different built-in implicit conversion (§5.1). §5.1 catalogs the implicit conversions; it did not say how they rank during overload resolution. v0.128 closes the gap conservatively: an exact match is preferred over any implicit conversion, but built-in implicit conversions are not ranked against one another, so a call that reaches two overloads only through (equally-ranked) conversions is ambiguous and is a compile error citing the candidates. The programmer disambiguates with an explicit cast.

We considered importing C#'s finer "better conversion target" ordering (narrower target preferred, so `int→long` would beat `int→float`). We declined it: §5.1 deliberately admits an implicit conversion only when no programmer-visible information is lost (or, for wide-integer-to-float, when an explicit cast would be unjustified noise), and it places every numeric widening in one undifferentiated row — including the precision-losing `int→float`. Ranking those conversions would be net-new semantics layered on top of §5.1, not a clarification of it. The conservative rule keeps faith with Bezel's "explicit over implicit" stance (the same principle that omits user-defined conversion operators, §25.4) and with the existing treatment of overloaded method references (§27.6), which already makes the absence of a unique winner a compile error citing the candidates. The cost is that a few calls that a C#-style rule would silently resolve are instead flagged for an explicit cast; that is the intended trade.

<a id="sec-strings"></a>
## 15. Strings

Strings appear in nearly every program. The choice of representation affects memory footprint and indexing performance, and the obvious choices each have a real cost.

Three options were on the table:

- **UTF-32, fixed width.** Simplest implementation, O(1) indexing trivially, but pays 4× memory for ASCII text.
- **UTF-8 with non-integer indexing.** What Rust, Go, and Swift do. Smallest memory, efficient sequential operations, but indexing the i-th code point becomes O(i), and the API needs cursor types and view-based abstractions to stay usable.
- **Adaptive width (PEP 393).** Per-string storage of 1, 2, or 4 bytes per code point, chosen at construction. O(1) indexing always; ASCII pays one byte; the representation is invisible to programmers.

Bezel picks PEP 393.

The Swift route was attractive on technical merit but requires a substantial design package — substring views, cursor types, multiple abstraction levels — that Bezel doesn't have time to do well in this revision. Without that package, plain UTF-8 with O(i) indexing puts too much friction on basic string code.

A two-types route ("normal `string` is UTF-32; `Utf8String` is the memory-efficient form") fragments the ecosystem. Every API author chooses a side and every consumer pays for conversions across the boundary.

PEP 393 hides the choice in the runtime. The user model is "string is a sequence of code points indexed in O(1)." The implementation picks 1, 2, or 4 bytes per string. ASCII text pays one byte; mixed-BMP text pays two; only strings with code points above the BMP pay four. Indexing is one branch on the kind, reliably predicted. CPython has thirteen years of experience showing this works.

`char` remains 32 bits — the right type for a code point. `s.IsAscii` gives FFI and IO a runtime predicate to skip encoding work for the dominant case. The char-level non-allocating substring view landed in v0.104 as `ReadOnlyStringSpan` (stdlib [§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)) — a record carrying a borrow of the string. The byte-level view from ASCII strings landed in v0.106 as `ReadOnlyAsciiByteSpan` (stdlib [§3.4](Bezel_Standard_Library.md#sec-readonlyasciibytespan)) — a parallel record using the same borrow-of-string mechanism, with construction validated by an ASCII check. ASCII strings are now first-class for both char-level parsing and byte-level FFI/IO without per-call allocation. See rationale [§5.15](#sec-owned-buffers-and-spans) for the full design story.

<a id="sec-ffi"></a>
## 16. FFI

<a id="sec-hand-written-declarations-not-bindgen"></a>
### 16.1 Hand-written declarations, not bindgen

Auto-generated bindings (Rust's `bindgen`, Python's `ctypes`) parse C headers and produce language-side declarations automatically. Bezel's FFI is manual: the programmer writes the Bezel-side signature, and the compiler trusts it.

Auto-generated bindings tend to expose every quirk of the C API; hand-written declarations let the programmer expose only what's needed, with the names they want. The cost is some typing per imported function. A bindgen tool could generate `extern` block source code in the future; nothing precludes that.

<a id="sec-distinct-types-for-cstring-wstring-rawbytes"></a>
### 16.2 Distinct types for `CString`, `WString`, `RawBytes`

C strings, UTF-16 strings, and arbitrary byte buffers have different ownership stories, different encoding assumptions, and different valid operations. Conflating them — as `void*` does — pushes the burden of remembering which is which onto the programmer at every call site. Distinct types let the compiler refuse calls that mix them up.

<a id="sec-things-explicitly-rejected"></a>
## 17. Things explicitly rejected

<a id="sec-truthiness-in-conditions"></a>
### 17.1 Truthiness in conditions

`if (user)` is forbidden. Conditions must be `bool`. Truthiness collapses zero, empty string, empty collection, and null.

<a id="sec-string-concatenation-with-non-strings"></a>
### 17.2 String concatenation with non-strings

`"x = " + 42` doesn't compile. Use interpolation: `$"x = {42}"`. Implicit `.ToString()` calls hide allocation behavior.

<a id="sec-goto"></a>
### 17.3 `goto`

Multi-level loop exit is done by extracting a method and using `return`.

<a id="sec-default-interface-methods"></a>
### 17.4 Default interface methods

Interfaces have only signatures. Default interface methods (C# 8+) blur the line between interfaces and abstract classes and create surprising behavior around explicit implementation. For shared logic, use an abstract class.

<a id="sec-member-hiding"></a>
### 17.5 Member hiding

See [§8.1](#sec-member-hiding-is-forbidden).

<a id="sec-inline-fixed-size-value-type-array-fields"></a>
### 17.6 Inline fixed-size value-type array fields

*Previously rejected; provided in v0.98.* Inline arrays `[N]T` are now a first-class type. They can appear as locals, as class fields (including with owned slots via `[N owned]T`), and as record fields when the slots are not `owned` (records hold only copyable value types, so move-only `[N owned]…` inline arrays are not admitted as record fields). A class can have an inline-array field whose storage is part of the class's layout, equivalent to C#'s `fixed` or Rust's `[T; N]`. See language spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays) for the type form and [§15.3](Bezel_Language_Spec.md#sec-move-only-value-types) for the move-only category that supports inline arrays with owned slots. The v0.40-v0.97 rejection rationale was that the use case is rare; experience and the v0.97 verification rounds suggested that inline arrays' value extends beyond fixed-size buffers into FFI marshalling, packed records, and as the foundational storage primitive over which owning collections compose.

<a id="sec-combined-visibility-modifiers-protected-internal"></a>
### 17.7 Combined visibility modifiers (`protected internal`)

C#'s `protected internal` (union: visible to derived classes *or* same-module code) and `private protected` (intersection: visible to derived classes *only within* the same module) are not provided. Most C# developers cannot state the semantics of either without looking it up. Bezel's four levels — `public`, `internal`, `protected`, `private` — are individually clear and cover the majority of needs. The intersection case (framework-internal override points) is a real gap, but the workaround is acceptable: use `internal` and rely on code review for the "only subclasses should call this" convention. If implementation experience shows the pain is worse than expected, `protected internal` meaning the intersection can be added as a purely additive change in a future revision.

<a id="sec-readonly-local-bindings-let-val-readonly-var"></a>
### 17.8 Readonly local bindings (`let`, `val`, `readonly var`)

Many languages provide a way to declare a local variable that cannot be reassigned after initialization: `let` (Swift, Rust, JavaScript), `val` (Kotlin, Scala), `final` (Java). Bezel deliberately does not provide this in the current version.

`var` is always reassignable. `const` requires a compile-time constant. `using var` is non-reassignable but carries disposal semantics. There is no `let`, `val`, or `readonly var` for runtime-computed but non-reassignable locals.

The omission is a cost-benefit judgment. The bug class prevented by readonly locals — accidental reassignment of a local that should have been stable — is real but small relative to the complexity budget. Most locals in well-written code are single-assignment regardless of enforcement, and the ownership system ([§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)) already tracks local-variable state far more aggressively than mutability alone. Adding a fourth local-declaration form (`let`, `readonly var`, or similar) would increase the decision burden on every local declaration without a proportionate safety payoff.

C# has lived without readonly locals for 25 years. Java added `final` locals but few teams enforce their use. The practical evidence is that this is a nice-to-have, not a must-have.

If future experience demonstrates stronger demand, the leading candidate is extending `const` to accept runtime-evaluated expressions — making `const` mean "single assignment" rather than "compile-time evaluable." This would be a source-compatible widening (every current `const` is also a single-assignment value) and would avoid introducing a new keyword. The design would need to distinguish compile-time constants (usable in `case` patterns, default parameter values) from runtime constants (not usable in those positions), following the `constexpr`/`const` split familiar from C++. This is deferred, not rejected.

<a id="sec-variadic-parameters-params"></a>
### 17.9 Variadic parameters (`params`)

There is no `params` keyword. A method that accepts a variable number of arguments uses `Array<T>` and the caller writes a collection literal:

```bezel
public method Log(messages: Array<string>) ...
Log(["hello", "world"]);
```

The brackets at the call site are syntactic friction but they make the allocation visible. A `params` mechanism that silently constructs an array from bare arguments hides allocation behavior — exactly the kind of implicit cost that Bezel's design philosophy aims to surface. The brackets also make the call site unambiguous when reading: `Log(["a", "b"])` is clearly one argument of type `Array<string>`, while `Log("a", "b")` under a `params` rule could be a two-argument overload or a params call, depending on what's declared.

<a id="sec-things-deferred"></a>
## 18. Things deferred

Not in this version; the design is open to them in future revisions.

- **Sum types / discriminated unions.** The current version narrows the gap with `sealed abstract` class hierarchies for `case` exhaustiveness. Full sum types are bigger; the design space (Swift, Rust, Scala 3) is large and unsettled.
- **Static interface members.** Required for a generic numeric constraint, factory-pattern interfaces, and parsing-method abstractions.
- **Trait/typeclass-style polymorphism.** Interfaces handle most of what traits do; the gap is associated types.
- **Const generics.** Generic parameters that are values, not types.
- **Reflection beyond `typeof`/`GetType`.** This splits into three pieces with different dispositions. (1) *Type-lattice metadata* — kind predicates, `BaseType`, `Implements`/`IsAssignableFrom`, enum introspection — **delivered in v0.113** on the `Type` token (stdlib [§2.13](Bezel_Standard_Library.md#sec-the-type-class)). (2) *Member-access reflection* — field/method enumeration, by-name member access, dynamic instantiation — is **declined, not deferred**: it contradicts the ownership model (a reflective `owned`-member access has no static site to carry its contract) and is the maximal violation of the no-hidden-magic principle; see rationale [§5.18](#sec-type-identity-not-reflection). It is recorded as declined so the question is settled rather than perennially reopened. (3) *Generic-instantiation decomposition* (`GenericArguments`/`GenericDefinition`) remains genuinely **deferred** — it is useful but cannot be expressed without also adding an unbound-generic `typeof` (`typeof(Array<>)`), which is a language change; it is to be decided together with that syntax, not bundled into a metadata revision. Attribute-only reflection, if reflection pressure ever materializes, is the realistic future direction and is itself a separately-scoped question (the attribute-system entry below).
- **Macros / metaprogramming.** Source generators, compile-time evaluation, syntax-tree macros.
- **Serialization: complete (object-graph tier fully delivered).** The serialization stack landed in nine planned non-breaking stages: the format-abstraction vocabulary (v0.114, stdlib [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary) — `ISerializer`/`IDeserializer` agents, `ISerializable`/`IDeserializable` capabilities); the concrete `Json`/`Binary` format implementations (v0.115, `System.Serialization`, stdlib [§2.15](Bezel_Standard_Library.md#sec-concrete-serialization-formats)); compiler-synthesized conformance for records and enums (v0.116, language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) — modeled on the existing auto-equality synthesis, a contained dedicated compiler feature, deliberately not the open-ended C# source-generator model and not gated on the macro system above); field-name customization (v0.117, the `serialized` clause of language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) — a closed compiler-recognized modifier with three effects (rename, exclude, tolerate; rename and tolerate combinable), deliberately *not* an attribute system and improving on Swift's `CodingKeys` by being per-member and total); operation-scoped unknown-field tolerance (v0.118 — `IgnoreUnknownFields` on the deserializer, *not* a type annotation, because tolerance is a property of a particular read rather than of the type's wire contract); tree-shaped class synthesis (v0.119, the `serialized tree` class-header clause of language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) — synthesis for classes whose serialized graph is a tree, the assertion enforced dynamically so a non-tree graph is a *detected error*, never silent duplication); the deferred-borrow-binding vs definite-assignment working-out (v0.120, rationale only, no spec text — determining the tension had a sound resolution before any syntax); shared-reference DAG synthesis (v0.121, the `serialized graph` class-header clause of language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) — identity-preserving deserialization of aliasing acyclic graphs, the owning tree reconstructed owner-before-borrower and every back-edge bound *through the constructor* so [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) holds unmodified); and polymorphic dispatch (v0.122, the `polymorphic` registry of language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) — a closed, explicitly-registered discriminator→derived-type map, no reflective subtype discovery, the discriminator consumed before component binding; explicit-registration-never-reflection per [§5.18](#sec-type-identity-not-reflection), the model the mainstream reached only after reflective `$type` became a remote-code-execution vector; rationale [§5.19](#sec-serialization-without-reflection)). One piece remained through v0.123 — *cyclic* class synthesis — and it is **delivered in v0.124**: language spec [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis) specifies the `serialized cyclic` clause, the statically-checked self-contained-constructor eligibility rule, and the two-phase construction whose intermediate state is provably unobservable, with [§7.9](Bezel_Language_Spec.md#sec-definite-assignment-for-non-nullable-fields) preserved unmodified — the v0.123 working-out's discharge made normative. **The serialization stack is now complete**: every class object graph (tree, DAG, cyclic) is synthesizable, polymorphic edges resolve through an explicit closed registry, and the only remaining reason to hand-write `Serialize`/`Deserialize` is a class whose constructor cannot be made self-contained — a recorded, compile-time-detected, principled exclusion, not a deferred feature. The staging was deliberate throughout: the vocabulary (v0.114) is the one piece that cannot be retrofitted, and each later stage was an independent non-breaking addition forward-compatible with the next, every hard question (the deferred-binding tension, v0.120; the cyclic unobservability proof, v0.123) resolved reasoning-first before its spec text.
- **Custom collection-literal protocols.** As of v0.111, user-defined types opt into *sequence*-literal construction by declaring the `FromCollectionLiteral` static factory (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)); both borrowing (`ReadOnlySpan<T>` parameter) and owning (`owned OwnedArray<T>` parameter) shapes are supported. What remains deferred: a user-extensible *dictionary*-literal protocol — the `[k: v, ...]` paired form for user-defined map-like types. The built-in `Dictionary`/`OwnedDictionary` keep the dictionary-literal form; user map-like types use ordinary constructors until the paired-factory protocol is designed. See rationale [§5.16](#sec-collection-literals-for-complex-collections).
- **Synchronization contexts for async.** Needed if Bezel targets desktop UI frameworks.
- **Module-private types.** A `module` modifier restricting visibility to a sub-module rather than the whole assembly.
- **Ref-returning indexers.** Would let `dict[key]++` be one operation instead of two. The language currently has no ref returns ([§22.14](Bezel_Language_Spec.md#sec-no-params-no-ref-returns) of the spec); adding them is deferred.
- **Asymmetric record auto-equality customization.** A way to exclude one field (cached hash, debug timestamp) from auto-equality. Currently it's all-or-nothing.
- **Stack-allocated runtime-sized buffers (`stackalloc` analog).** v0.99 introduced `Span<T>` and `ReadOnlySpan<T>` as records carrying borrows of an `OwnedBuffer<T>`; v0.100 added the write capability to `Span<T>`; v0.101 thickened the API surface with iteration, search, equality, and bulk-mutation methods; v0.102 generalized to admit value-typed T; v0.103 added byte-buffer accessors on `Span<byte>` and `ReadOnlySpan<byte>`. What remains deferred: a stack-allocated runtime-sized buffer (C# `stackalloc` analog) that would let spans view into stack memory rather than only heap memory. The use case is performance — avoiding heap allocation in hot paths like parsers. Bezel's existing inline arrays (`[N]T`, language spec [§15.1](Bezel_Language_Spec.md#sec-inline-arrays)) cover the fixed-size case; a runtime-sized stack form would extend the same lifetime story to runtime-N. The design must address how stack-bound lifetimes interact with closures, exception unwinding, and recursive calls. Low priority — the heap-backed `OwnedBuffer<T>` is sufficient for almost all use cases; the gap is real but narrow.
- **Spans over additional view sources.** v0.99-v0.103 spans (`Span<T>` and `ReadOnlySpan<T>`) view only into `OwnedBuffer<T>`. v0.104 added `ReadOnlyStringSpan` (stdlib [§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)) for the string-source case. v0.106 added `ReadOnlyAsciiByteSpan` (stdlib [§3.4](Bezel_Standard_Library.md#sec-readonlyasciibytespan)) for the byte view over ASCII strings. v0.107 added `ArraySpan<T>` and `ReadOnlyArraySpan<T>` (stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)) for sub-range views over `Array<T>`. v0.108 added the `IReadOnlySpan<TElement>` interface (stdlib [§4.16](Bezel_Standard_Library.md#sec-ireadonlyspan)) unifying the read-only surface across all concrete span types. What remains deferred: `Span<T>` over an inline array `[N]T` — would need a new borrow-source kind that ties the span's lifetime to a stack frame, with rules for closure capture, exception unwinding, and recursive calls. Substantial design work for a narrow performance gain over the existing `OwnedBuffer<T>`-backed form. Deferred — the six concrete span types (OwnedBuffer-backed pair, ReadOnlyStringSpan, ReadOnlyAsciiByteSpan, and ArraySpan-pair) plus the read-only interface cover the dominant use cases.
- **Move-only value types in `OwnedBuffer<T>` / `Span<T>` / `ReadOnlySpan<T>`.** v0.102 admits value-typed T but excludes the move-only value types (owning inline arrays `[N owned]T` and `[N owned]T?`). A buffer of move-only values would itself have move-only slot semantics — admissible in principle but requires a third category in the slot-semantics dispatch (class T, non-move-only value T, move-only value T) without an obvious motivating use case. Users who need a runtime-sized container of fixed-size owning arrays today wrap them in a class. Deferred until a motivating use case emerges.
- **Lambda-parameter destructuring.** Destructuring a positional record in a lambda parameter position: `(ClickedEvent(_, x, y, _)) => ...`. Would improve event-handler ergonomics under the payload convention.
- **Tuples and tuple destructuring.** A built-in anonymous-product type — `(int, string)` as a type, `(42, "hi")` as a value — with positional access and `for var (k, v) in dict do ...` destructuring in patterns and loops. This is an **ergonomic convenience, not a capability gap**: every use case is already expressible. Bezel's model uses positional records (language spec [§9.1](Bezel_Language_Spec.md#sec-positional-records)) for value-only products, and *owning classes with multiple `owned` fields* for products that carry owned resources (a class may own its fields per [§28.5](Bezel_Language_Spec.md#sec-owned-fields); a method may return `owned C`; one `using var` discharges the whole aggregate, cascading to every owned field). The `OwnedDictionary<K, V>.Drain()` case — once thought to need tuples — is handled by exactly this pattern via the `DrainedEntry<K, V>` owning class (stdlib [§4.9](Bezel_Standard_Library.md#sec-owneddictionaryk-v)), with no language change. What tuples would add is purely notational: positional `(a, b)` destructuring sugar and one-shot anonymity, versus the named, documented shape that records and classes provide. Bezel's design philosophy already argues (records-vs-tuples) that a name and a documented shape is a feature, not a limitation; the same argument applies to the owned case. Class deconstruction (a `Deconstruct`-style positional surface for classes) was evaluated and declined for the same reason — see [§5.17](#sec-why-no-class-deconstruction). Tuples remain available as a low-priority future convenience; the structural-vs-nominal identity question and the named-vs-positional-fields question would need resolving, but neither blocks any program from being written today.
- **Runtime-evaluated `const` for locals.** Extending `const` to accept runtime-evaluated expressions, making it mean "single assignment" rather than "compile-time evaluable." Would provide readonly local bindings without a new keyword. The design must distinguish compile-time constants (usable in patterns and default values) from runtime constants (not usable in those positions). See [§17.8](#sec-readonly-local-bindings-let-val-readonly-var) for the rationale behind the deferral.
- **Post-condition loops (`do-while` / `repeat-until`).** A loop that executes the body at least once before checking its termination condition. The current workaround — `while true do ...; if not cond then break; end; end;` — is straightforward but heavier than the dedicated form would be. The open design questions are syntactic: a literal `do ... while (cond);` form clashes with [§21.6](Bezel_Language_Spec.md#sec-do-end-blocks)'s `do ... end` blocks, so the addition would either need to disambiguate by the presence of `while`/`until` (which parses but reads ambiguously) or introduce a new keyword (`repeat ... until cond;` is the Pascal/Modula-2 form and would fit Bezel's overall vocabulary). The decision is not whether the construct is useful but which spelling minimizes confusion with existing blocks; deferring keeps the option open without committing to a syntax.
- **`owned` as a generic argument on user-defined types.** v0.75 through v0.96 admitted `owned T` as a generic-argument modifier (`Array<owned T>`, `Task<owned T>`, etc.) via a hard-coded opt-in list. v0.97 retired this mechanism in favor of separate named classes (`OwnedArray<T>`, `OwnedTask<T>`, `OwnedDictionary<K, V>`, etc.). What remains deferred is whether and how user-defined generic types should be able to express owning-element semantics: a mechanism by which a user-written `Pool<T>` could declare an "owning T" variant and define the corresponding per-operation semantics (which methods accept owned arguments, which dispose on removal, which transfer ownership out on drain). v0.98's inline-array primitive (`[N owned]T?`) gives user-defined owning containers a foundation to compose over (see rationale [§5.14](#sec-inline-arrays-and-array-kind-split)), but expressing this with full generic-argument-level support is deferred.
- **LINQ over owned-element collections.** The standard library's LINQ extensions (`Where`, `Select`, `OrderBy`, `ToArray`, etc.) are declared on `IEnumerable<T>` with non-owning element semantics. Applying them to an `OwnedArray<T>` and other `OwnedX<T>` collections works today through the borrowing-iteration upcast — owning collections implement `IEnumerable<T>` and produce borrows on iteration — so `ownedArr.Where(predicate)` produces a borrowing pipeline. What is deferred is a parallel LINQ surface declared on `OwnedEnumerable<T>` that preserves ownership through the pipeline (e.g., `Where(predicate: (T) -> bool): OwnedEnumerable<T>`). The design question is whether to provide one method per operation that infers from receiver type, or to add a parallel set of named methods on `OwnedEnumerable<T>`. Code that needs to drain-and-transform today writes the drain loop explicitly.
- **Attribute / annotation system.** A C#-style `[Attribute]` declaration syntax, with attribute classes derived from a base type and reflective query at runtime. Useful for serialization metadata, framework integration, AOT hints, deprecation markers, validation rules, dependency-injection registrations, and other annotations whose effect is interpreted by tools rather than the language itself. Not in this version because most attribute *uses* depend on machinery that is itself deferred — reflection beyond `typeof`/`GetType`, serialization frameworks, dependency injection, test frameworks. Designing attributes before designing how they are consumed would lock in syntax for an unbuilt feature, and that syntax would likely need to change once the consuming machinery exists. The corollary: any future attribute design will be tightly coupled to the reflection and serialization designs, and developing them together is the better order than attempting to retrofit one to the other.

  Two narrowly-useful capabilities can be added before the full system, if user demand emerges:
  - **`[Obsolete]`-style compile-time deprecation.** Achievable as a compiler-recognized declaration modifier (an `obsolete` keyword preceding a declaration, or a similar narrow syntax) without needing runtime reflection. The compiler emits a warning at every use site of an obsolete declaration; no runtime machinery required.
  - **Method-implementation hints** (`[MethodImpl]`-style: aggressive inlining, no inlining, AOT-only). Achievable as a small fixed set of compiler-recognized declaration modifiers. No runtime introspection required.

  These two narrow additions don't depend on reflection at runtime and could land independently of the broader attribute design. They are not committed for any particular revision; the deferred-list entry exists to mark the open question rather than to schedule the work.

  C# attributes have known costs that influence the deferral: attributes proliferate in real-world C# codebases (validation rules, serialization hints, ORM mappings, DI registrations stacking on a single class), they invite stringly-typed magic (attribute parameters often interpreted by reflection-based machinery with shallow type checking), and they blur the boundary between language semantics, framework machinery, and inert metadata (a reader cannot tell from an attribute alone what affects compilation, what affects runtime, and what affects nothing). Bezel has time to design around these costs rather than inheriting them.

<a id="sec-changelog"></a>
## 19. Changelog

Per-revision migration notes. Normative for migration; not part of the language definition. Each subsection records the source-breaking and behavioural changes between two adjacent versions.

<a id="sec-v0-52-to-v0-53-changelog"></a>
### v0.52 to v0.53 changelog

Normative for migration; not part of the language definition.

<a id="sec-source-breaking"></a>
### Source-breaking

- **Event raising.** `event?.Invoke(args)` is removed. Inside the declaring class, write `event.Raise(args)` (sync) or `await event.RaiseAsync(args)` (async).
- **`List<T>` removed.** v0.52's `List<T>` (a doubly-linked list) is renamed to `LinkedList<T>`. The name `List<T>` is reserved and unused; reach for `Array<T>` for a dynamic array.
- **`ToList()` on LINQ removed.** Use `ToResizableArray()` (returns `Array<T>`) or `ToArray()` (returns `T[]`).
- **LINQ `Map` removed.** Use `Select`.
- **Mixed-precedence parenthesization.** `a & 1 = 0` and similar expressions mixing bitwise with equality, relational, or logical operators now require explicit parentheses.
- **Stdlib primitive class names.** `class int`, `class string`, etc. are renamed `class Int32`, `class String`, etc. The lowercase forms remain the keywords; only the standard library's class declarations changed name. Most user code is unaffected.

<a id="sec-source-compatible"></a>
### Source-compatible

- `Task<owned T>` is now legal, expressing single-consumer producers.
- `when` guards are now permitted on the short form of `catch`.
- `where T: new()` accepts positional records whose fields all have well-defined defaults.
- Sealed class hierarchies are exhaustive in `case` without requiring a `_` arm.
- The discard `_` is valid in three positions (pattern, out-argument, expression-statement).
- `out owned` is a legal parameter modifier combination.
- Default `CancellationToken := CancellationToken.None` added to standard library async signatures.
- `IEvent<TArgs>` and `EventSource<TArgs>` are explicit standard-library types.
- Static `owned` fields are defined to be disposed at program exit in reverse static-initialization order.
- Float equality is reflexive: `NaN.Equals(NaN)` is `true`, `(-0.0).Equals(0.0)` is `true`. Relational operators retain IEEE semantics. `Double.IeeeEquals` and `Double.BitwiseEquals` (and `Float` analogs) provide alternatives.

<a id="sec-specification-only"></a>
### Specification-only

- [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) added: a normative specification of local-variable ownership and borrow lifetimes. This was load-bearing throughout v0.52 but never written down.
- [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) rewritten: Pattern A vs Pattern B is type-directed (`Task<T>` vs `Task<owned T>`) instead of syntax-directed.
- [§4.6](Bezel_Language_Spec_0_98.md#sec-string) rewritten: string representation is adaptive (PEP 393-style) instead of UTF-32. The user-visible surface is unchanged.
- [§9.2](#sec-why-records-are-sealed) refined: body-only records with auto-properties only get auto-generated equality.
- [§24](#sec-v0-57-to-v0-58-changelog) rewritten: events use `Raise`/`RaiseAsync` instead of `?.Invoke`.
- [§17.1](#sec-truthiness-in-conditions) reorganized: precedence levels for `??` and bitwise operators reshuffled. v0.52 expressions that didn't mix the affected operators are unaffected.
- [§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order) expanded: static `owned` field disposal at program exit is specified.

<a id="sec-standard-library"></a>
### Standard library

- `Task<owned T>` is legal; `Semaphore.AcquireAsync` etc. expose it.
- `Mutex.TryAcquire` and `Semaphore.TryAcquire` declare `out owned` instead of the previously-invalid `out using var`.
- `IEvent<TArgs>`, `IAsyncEvent<TArgs>`, `EventSource<TArgs>`, `AsyncEventSource<TArgs>` are added.
- `String.IsAscii` added; `String.operator +` declared explicitly.
- `Double.IeeeEquals`, `Double.BitwiseEquals`, and `Float` analogs added.
- `EnumerateRowMajor` and `EnumerateColumnMajor` extensions added on multi-dim arrays.
- `Subscribe`, `Acquire`, `Register`, and other resource-producing methods declare their returns as `owned` consistently.
- All standard async methods accepting `CancellationToken` default it to `CancellationToken.None`.

There is no automated migration tool; find-and-replace plus a compile-and-fix pass is the recommended path.

---

<a id="sec-v0-53-to-v0-54-changelog"></a>
### v0.53 to v0.54 changelog

Normative for migration; not part of the language definition.

<a id="sec-v0-53-to-v0-54-changelog-source-breaking"></a>
<a id="sec-changelog-source-breaking"></a>
### Source-breaking

- **Event declarations use payload types.** v0.53's `event Clicked: (Button, ClickEventArgs) -> ()` becomes `event Clicked: ClickedEvent` — the right-hand side is now the payload type, not the handler function type. All event declarations, subscribe sites, and raise sites must be updated. The handler type `(T) -> ()` (sync) or `(T) -> Task` (async) is derived from the payload type automatically.
- **Event `Raise` signature changed.** `Raise(args: TArgs)` (where TArgs was the function type) becomes `Raise(payload: T)` (where T is the payload record). Call sites change from pseudo-tuple syntax to record construction: `Clicked.Raise(new ClickedEvent(this, x, y, button))`.
- **Subscribe handlers take one parameter.** `(btn, args) => ...` becomes `(e) => e.X`. The handler receives the payload record as a single argument.
- **No-payload events omit the type.** `event E: () -> ()` becomes `event E` (implicit `()` payload). Raise sites change from `E.Raise(())` to `E.Raise()`.

<a id="sec-v0-53-to-v0-54-changelog-source-compatible"></a>
<a id="sec-changelog-source-compatible"></a>
### Source-compatible

- `Range<T>` now implements `IEnumerable<T>` and composes with LINQ: `(1..100).Where(...)` works.
- `string` now implements `IEnumerable<char>`; `for var c in "hello"` is valid.
- No-selector `Min()`, `Max()`, `Sum()`, and `Average()` LINQ overloads added for comparable and numeric types.
- `MinOrNull()` and `MaxOrNull()` added for comparable types.

<a id="sec-specification-only-no-behavioral-change"></a>
### Specification-only (no behavioral change)

- **[§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) discard examples corrected.** Examples now use `:=` (matching grammar and prose); v0.53 examples erroneously used `=`.
- **[§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness) exhaustiveness example corrected.** Base class changed from `sealed` (contradictory) to `abstract`.
- **Appendix A constructor grammar fixed.** `base()`/`this()` is now correctly shown as the first body statement, not after a colon between params and body.
- **Appendix A grammar expanded.** `GenericConstraints*` added to `ClassDecl`, `InterfaceDecl`, `RecordDecl`. `LValue` now includes `this`. `ArmBody` production allows `do...end` in case arms. `AutoAccessorVis` production supports `private set` on auto-properties. Signed alignment in interpolated strings. `ClassBody` is now required (not optional). `EventDecl` uses `Type?` instead of `FunctionType`.
- **Appendix B reserved words.** `step` and `convention` added to [§B.1](Bezel_Language_Spec_0_98.md#sec-reserved-keywords); introductory text reworded to distinguish fully-reserved from contextual keywords.
- **[§8.1](#sec-member-hiding-is-forbidden) Object declaration.** Clarified as compiler intrinsic with runtime-provided method bodies.
- **[§11.5](Bezel_Language_Spec_0_98.md#sec-modifier-order) modifier order.** `sealed override` combination shown explicitly.
- **[§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order) static fields.** New subsection clarifying non-`owned` static class-typed fields; `await` explicitly forbidden in static initializers and static constructors.
- **[§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) range optimization.** Note added that compiler may optimize range iteration into counted loops.
- **[§29.5](Bezel_Language_Spec_0_98.md#sec-return-not-allowed-in-iterator-methods) async iterators.** Clarified that `return` is forbidden in async iterators (`IAsyncEnumerable<T>`) just as in sync iterators.
- **[§30.3](Bezel_Language_Spec_0_98.md#sec-the-task-and-taskt-types) `end Task<T>;`** corrected to `end Task;`.
- **[§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling) CString example** corrected to use `using var` (ownership compliance) and removed redundant `!`.
- **Appendix C.6.** Added: `await` does not appear in static field initializers or static constructor bodies.

<a id="sec-v0-53-to-v0-54-changelog-standard-library"></a>
<a id="sec-changelog-standard-library"></a>
### Standard library

- `IEvent<T>`, `IAsyncEvent<T>`, `EventSource<T>`, `AsyncEventSource<T>` rewritten: `T` is now the payload type; handler types are derived. `Raise(payload: T)` replaces `Raise(args: TArgs)`.
- `EventSource<()>` provides a convenience `Raise()` overload for no-payload events.
- `Range<T>` added ([§9.8](Bezel_Standard_Library_0_98.md#sec-ranget)): compiler-intrinsic record implementing `IEnumerable<T>`, with `Contains`, `GetEnumerator`, `IsOpenEnded`, `IsEmpty`.
- `String` now declares `IEnumerable<char>` and `GetEnumerator(): IEnumerator<char>`.
- No-selector `Min()`, `Max()`, `MinOrNull()`, `MaxOrNull()` on `IEnumerable<T> where T: IComparable<T>`.
- No-selector `Sum()` and `Average()` as per-type extensions on `IEnumerable<int>`, `IEnumerable<long>`, `IEnumerable<double>`, `IEnumerable<decimal>`, and analogous for `uint`, `ulong`, `float`.

<a id="sec-migration-guide"></a>
### Migration guide

The event redesign is the only source-breaking change. Migration steps:

1. For each event declaration `event E: (P1, P2, ...) -> ()`, define a payload record `record EPayload(p1: P1, p2: P2, ...)` and change the declaration to `event E: EPayload`.
2. For each `Raise` call, change from pseudo-tuple syntax to record construction: `E.Raise(new EPayload(a, b, ...))`.
3. For each handler lambda, change from multi-parameter `(p1, p2, ...) => ...` to single-parameter `(e) => ... e.p1 ... e.p2 ...`.
4. For no-payload events, change `event E: () -> ()` to `event E` and `E.Raise(())` to `E.Raise()`.

---

<a id="sec-v0-54-to-v0-55-changelog"></a>
### v0.54 to v0.55 changelog

Normative for migration; not part of the language definition.

<a id="sec-v0-54-to-v0-55-changelog-source-compatible"></a>
<a id="sec-source-compatible-2"></a>
### Source-compatible

- **Property references widened.** [§27.6](Bezel_Language_Spec_0_98.md#sec-method-references): method references now work on all properties, not only read-only ones. The reference always produces the getter; whether a setter exists is irrelevant. Setter references are not supported. Existing code that used read-only property references continues to work unchanged.

<a id="sec-v0-54-to-v0-55-changelog-specification-only"></a>
<a id="sec-changelog-specification-only"></a>
### Specification-only

- **[§30.8](Bezel_Language_Spec_0_98.md#sec-cancellation) CancellationToken declaration expanded.** Now shows `None`, `IsCancellationRequested` (on both token and source), and `Register`, matching the standard library's [§6.1](Bezel_Standard_Library_0_98.md#sec-cancellationtoken-and-cancellationtokensource) surface.
- **[§14.1](#sec-++-and-are-statements) `Swap<T>` example** wrapped in `static class Utils`, consistent with [§2.2](#sec-no-top-level-functions-or-fields)'s "no top-level functions" rule.
- **[§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) discard** now includes a worked example distinguishing `_ := expr;` (discard) from `var _ := expr;` (binding).
- **[§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types) catchability** clarified: all standard exception types are catchable except `StackOverflowException`.
- **[§17.6](#sec-inline-fixed-size-value-type-array-fields) indexer increment** now includes a side-by-side source-vs-desugaring example for readability.

<a id="sec-design-rationale"></a>
### Design rationale

- **[§12.2](#sec-choosing-between-taskt-and-taskowned-t) added: choosing between `Task<T>` and `Task<owned T>`.** A decision tree for API designers, covering value-type results (neither pattern), resource-acquisition (Pattern B), shared/borrowed results (Pattern A), and the "when in doubt" default. Includes a summary table and a note on forward-compatibility of the choice.

---

<a id="sec-v0-55-to-v0-56-changelog"></a>
### v0.55 to v0.56 changelog

Normative for migration; not part of the language definition.

This revision is primarily a correctness pass. v0.55 had several places where the spec text contradicted itself, where examples were ungrammatical under the spec's own grammar, and where the standard library used syntactic forms that the spec section on parameters did not permit. Most of v0.56's changes restore consistency. The two deeper changes — the lattice-direction fix and the method-reference borrow-capture rule — close real soundness gaps that affected the ownership system's guarantees. There is one source-breaking standard library change (the primitive-wrapper redeclaration); everything else is a clarification or an additive widening.

<a id="sec-v0-55-to-v0-56-changelog-source-breaking"></a>
<a id="sec-source-breaking-2"></a>
### Source-breaking

- **Primitive type wrappers are records, not static classes.** [§9.1](#sec-why-records-cannot-have-destructors-or-owned-fields)–9.5 of the standard library: `Int32`, `Double`, `Decimal`, `Boolean`, `Char`, and the `Float`/integer-width siblings are now declared as `public record` with the `compiler-intrinsic` annotation, instead of `public static class`. The previous declaration was self-contradictory (a `static class` cannot have instance methods, and the wrappers had `ToString`, `CompareTo`, etc.) and disagreed with the language spec's [§3.1](Bezel_Language_Spec_0_98.md#sec-type-kinds) classification of integers, floats, decimal, char, and bool as primitive value types. User code that referenced these types by name (`int.MaxValue`, `Int32.Parse`, etc.) is unaffected — the static members remain accessible through the same identifiers. Code that explicitly wrote `static class Int32` or assumed the type was a class (e.g., `if x is Int32 then ...`) needs to be updated; in practice this is rare.
- **Event `operator +=` and `operator -=` declarations removed.** [§24.3](Bezel_Language_Spec_0_98.md#sec-the-type-of-an-event-member) / std lib [§6.5](Bezel_Standard_Library_0_98.md#sec-events-ievent-eventsource): `EventSource<T>` and `AsyncEventSource<T>` no longer expose `+=`/`-=` as overloaded operators. The `+=` and `-=` *syntactic forms* remain available at the source level on event members and now desugar to internal `Add`/`Remove` method calls. User code that wrote `button.Clicked += handler` or `button.Clicked -= handler` is unchanged — the desugaring is identical at the call site. User code that referenced these operators *as operators* (taking their address, declaring matching overloads on user types) is rejected. The motivation is to resolve the contradiction between [§25.1](Bezel_Language_Spec_0_98.md#sec-overloadable-operators)'s "assignment operators are not overloadable" and the previous appearance of `operator +=` on the event source classes.
- **Reserved-keyword parameter names renamed in the standard library.** Five method signatures used `base` or `type` as parameter names — both unconditionally reserved keywords (Appendix B.1) and therefore not legal identifiers. `Math.Log(x, base)` → `Math.Log(x, base_)` (a reluctant underscore suffix; alternatives like `radixOrBase` were considered but `Log` takes a logarithmic *base*, not a numeric base, so `radix` is misleading); `Convert.ToString(value, base: int)`, `Int32.Parse(s, base: int)`, `Int32.ToString(base: int)` → all `radix: int`; `MessageBoxW(..., type: uint)` → `style: uint`. Code that passed these arguments by name needs to update the parameter name; positional calls are unaffected.
- **`KeyNotFoundException` moved to `System.Collections.Generic`.** [§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types): `KeyNotFoundException` is no longer language-built-in. Its only producer is `Dictionary<K, V>.[key]`, which already requires `import System.Collections.Generic`; the exception now lives in the same module. Code that catches `KeyNotFoundException` and was already importing `System.Collections.Generic` for the dictionary type is unaffected. Code that caught the exception without importing the module (rare; `Dictionary` was always required for it to be raised) needs the import.

<a id="sec-v0-55-to-v0-56-changelog-source-compatible"></a>
<a id="sec-source-compatible-3"></a>
### Source-compatible

- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) lattice direction corrected.** v0.55 said the join lattice was `borrowed ⊑ owned` and that an owned-on-one-branch / borrowed-on-the-other merge produced an owned variable at the join. This is unsound: the borrowed branch has no value to dispose, so the merged owned slot would carry an obligation it cannot meet. v0.56 specifies the correct direction (owned ⊑ borrowed; owned is the stronger state) and rejects mixed merges, requiring the program to equalize branches before the merge. Two patterns for equalization are shown in the spec. The previous reading was inconsistent with the rest of language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management)'s rules; programs that compiled under a strict v0.55 implementation would also compile under v0.56.
- **Method references that capture borrowed receivers are subject to the closure-escape rule.** [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows): a method reference `obj.Method` is treated, for borrow-lifetime purposes, as a closure capturing `obj`. Function-typed values remain runtime-managed ([§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)) — that part is unchanged — but the captured environment's borrows are tracked separately. Code that passed `borrowedReceiver.Method` to an API that stored the function value beyond the borrow's source lifetime would have compiled in v0.55 (the rule was not stated) and now fails to compile in v0.56. We believe this case is rare in practice but is exactly the kind of soundness hole that justifies the new rule.
- **`for var x in source` over an owned `using var` source.** [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for): the desugaring's `using var __source := source` is replaced by a borrow-pinning form when `source` is itself an owned local (including a `using var`). Code that wrote `for var line in stream do ... end` where `stream` was a `using var` would have failed to compile under a strict v0.55 reading (move out of `using var` is forbidden); now it compiles correctly without taking ownership of `stream`.
- **Async iterators may return `IAsyncEnumerator<T>`.** [§29.3](Bezel_Language_Spec_0_98.md#sec-iterator-methods) and [§30.1](Bezel_Language_Spec_0_98.md#sec-async-methods): previously only `IAsyncEnumerable<T>` was listed. The single-use form is now permitted, parallel to synchronous iterators. The omission was accidental.
- **`DateTime` now has a reverse `+` overload.** Std lib [§2.5](Bezel_Standard_Library_0_98.md#sec-datetime-timespan-dateonly-timeonly): `operator +(t: TimeSpan, d: DateTime): DateTime` added alongside the existing `operator +(d: DateTime, t: TimeSpan): DateTime`. Source-compatible widening.

<a id="sec-v0-55-to-v0-56-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-changelog-specification-only-no-behavioral-change"></a>
### Specification-only (no behavioral change)

- **[§13.3](#sec-method-reference-subscriptions) visibility rule** clarified to acknowledge that `static constructor` and `destructor` are exempt from the "every member declares visibility explicitly" rule. The grammar already encoded this; the prose now agrees.
- **[§3.4](Bezel_Language_Spec_0_98.md#sec-keyword-aliases-for-built-in-types)** explicitly notes that `Object` has no lowercase keyword alias, by design.
- **[§9.2](#sec-why-records-are-sealed) auto-equality rule refined.** Pure expression-bodied properties (functions of stored auto-properties) no longer disqualify a record from auto-equality; they simply do not contribute to the comparison. This is what `DateTime`, `TimeSpan`, `Guid`, and other standard records implicitly assumed.
- **[§9.7](Bezel_Language_Spec_0_98.md#sec-records-and-disposal)** allows borrow-bearing records to have static fields when every borrow field is null (the **null-source** form). This permits sentinels like `CancellationToken.None`.
- **[§16.5](Bezel_Language_Spec_0_98.md#sec-iteration)** broken `if/else` example fixed (the previous example had two `else` clauses, which is ungrammatical).
- **[§17.5](#sec-member-hiding)** clarifies that the `in` operator's `Contains` lookup is structural — the one structural-dispatch rule in the language — and gives the reason.
- **[§17.6](#sec-inline-fixed-size-value-type-array-fields)** documents the leak risk of `++`/`--` on owned class-typed fields.
- **[§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_)** adds a worked example clarifying that `var _ := expr;` declares a local named `_` that can be read, while `_ := expr;` discards.
- **[§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness)** notes that exhaustiveness is established at compile time and is brittle across module boundaries when a sealed hierarchy gains a new leaf.
- **[§22.7](Bezel_Language_Spec_0_98.md#sec-constructors)** clarifies that `try`/`catch` inside a static constructor body catches exceptions normally — the failed-state rule fires only on uncaught exceptions.
- **[§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors)** explains why convenience constructors must chain to `this(...)` rather than `base(...)`.
- **[§24.5](Bezel_Language_Spec_0_98.md#sec-raising-events)** documents that an async event handler's task being cancelled produces the same propagation behavior as a fault.
- **[§25.1](Bezel_Language_Spec_0_98.md#sec-overloadable-operators)** explicitly lists assignment and compound-assignment operators as non-overloadable, and notes that the event `+=`/`-=` syntax is a fixed compiler desugaring rather than a user-defined operator.
- **[§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer)** explicitly exempts implicit boxing from the `new`-discharge rule.
- **[§28.10](Bezel_Language_Spec_0_98.md#sec-using-var)** has a new subsection on `using var` initialized from a borrow, which formerly was documented only inside [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for)'s desugaring.
- **[§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal)** rewritten to clearly distinguish "logical disposal" (destructor or `DisposeAsync`, mutually exclusive) from "physical reclamation" (owned-field disposal and heap release, always after logical disposal).
- **[§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal)** specifies what happens when a `Task<owned T>` is disposed before completion: a continuation disposes the produced result whenever it appears; the dispose call returns immediately.
- **[§32.5](Bezel_Language_Spec_0_98.md#sec-ffi-compatible-types)** adds the rationale for excluding `char` from the FFI surface and for fixing `bool` at 1 byte.
- **[§33.5](Bezel_Language_Spec_0_98.md#sec-program-entry-point-resolution)** disambiguation: a single class declaring multiple qualifying `Main` overloads is rejected at compile time, with no flag-based override.

<a id="sec-v0-55-to-v0-56-changelog-standard-library"></a>
<a id="sec-standard-library-2"></a>
### Standard library

- **`out var` removed from twelve method signatures.** The `out var name: T` form is a call-site shorthand for declaring a fresh local; it is not a parameter-declaration form. The signatures of `TryParse` (across `Convert`, `DateTime`, `Guid`, primitive wrappers, `Boolean`), `Dictionary.TryGetValue`, `Stack.TryPop`/`TryPeek`, `Queue.TryDequeue`/`TryPeek` are corrected to plain `out result: T`. Call sites that wrote `parser.TryParse("42", out var n)` are unchanged; the call-site form is the same.
- **`Sort()` constraint moved to extension.** `Array<T>.Sort()` (the no-comparer form) cannot express `T: IComparable<T>` on the class-level T parameter, so it is moved to a generic extension `extension Array<T> where T: IComparable<T>`. `Array<T>.Sort(comparer)` (the comparer form) remains an unconstrained instance method.
- **`Atomic<T>.Value` is a block-form property** to make explicit that the read and write are compiler-intrinsic atomic operations rather than auto-property storage.
- **`String` now declares its full interface set** (`IEnumerable<char>`, `IComparable<string>`, `IEquatable<string>`) and shows its `Object` overrides (`Equals(Object?)`, `GetHashCode()`) explicitly, matching the language spec's "string is a reference type with content equality" claim.
- **Destructor declarations** added to `Lock`, `SemaphoreToken`, `Subscription`, and `CancellationTokenSource` to make the disposal contract visible in the class body rather than only in adjacent prose. `Random` is documented as having a no-op `Dispose`.

<a id="sec-v0-55-to-v0-56-changelog-migration-guide"></a>
<a id="sec-changelog-migration-guide"></a>
### Migration guide

For most codebases, the only required changes are:

1. Update parameter names in calls that pass arguments by name to `Math.Log`, `Convert.ToString(value, base:)`, `Int32.Parse(s, base:)`, or `Int32.ToString(base:)`. Replace `base:` with `radix:` (or `base_:` for `Math.Log`).
2. Add `import System.Collections.Generic;` to any file catching `KeyNotFoundException` that didn't already have it. (Almost certainly already present.)
3. Code that declared user types with names like `static class MyInt` mimicking the v0.55 stdlib pattern should re-examine whether `record` is the correct kind. (User code rarely did this; the pattern was stdlib-specific.)

There are no migration steps for the lattice-direction or method-reference-capture changes — programs that compiled correctly under a sound reading of v0.55 continue to compile under v0.56.

---

<a id="sec-v0-56-to-v0-57-changelog"></a>
### v0.56 to v0.57 changelog

Normative for migration; not part of the language definition.

This revision is a consistency and completeness pass. v0.56 had several places where the spec text contradicted itself, where the standard library used syntactic forms the spec prohibited, where specification gaps left important concepts undefined, and where the grammar disagreed with the normative prose. There is one source-breaking change (destructor visibility).

<a id="sec-v0-56-to-v0-57-changelog-source-breaking"></a>
<a id="sec-source-breaking-3"></a>
### Source-breaking

- **Destructors no longer carry the `public` keyword.** A destructor is an implementation hook called by the compiler-generated `Dispose()` method; it is not directly callable by user code. Writing `public destructor` is now a compile error; write `destructor` instead. This parallels static constructors, which also carry no visibility keyword. The grammar, [§13.3](#sec-method-reference-subscriptions), [§28.4](Bezel_Language_Spec_0_98.md#sec-the-destructor), and all standard library destructor declarations are updated. Migration: delete `public` from every destructor declaration.

<a id="sec-v0-56-to-v0-57-changelog-source-compatible"></a>
<a id="sec-source-compatible-4"></a>
### Source-compatible

- **`static const` is now permitted.** [§22.12](Bezel_Language_Spec_0_98.md#sec-static-members): `const` is implicitly static; writing `static const` is redundant but no longer a compile error. This legalizes the form already used throughout the standard library (`public static const Pi: double := ...`). The grammar now includes `"static"?` before `"const"` in `ConstDecl`.
- **`Dictionary<K, V>.Contains(key: K): bool` added.** An alias for `ContainsKey` that enables the `in` operator: `key in dict` now compiles. The method is a source-compatible addition to the standard library.
- **`DateTime.operator -(d: DateTime, t: TimeSpan): DateTime` added.** The missing subtraction overload. `date - duration` now compiles alongside the existing `date + duration`.
- **`Task.FireAndForget(task: Task)` added.** [§30.10](Bezel_Language_Spec_0_98.md#sec-standard-library-task-helpers) and std lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers): a supervised fire-and-forget helper. Registers a continuation that logs exceptions and disposes the task. The dangling cross-reference in [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) is now resolved.

<a id="sec-v0-56-to-v0-57-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-2"></a>
### Specification-only (no behavioral change)

- **[§4](#sec-memory-management) renamed from "Built-in Primitive Types" to "Built-in Types."** `string` is a reference type ([§3.2](#sec-the-discard-_)), not a primitive value type ([§3.1](#sec-always-semicolons)). The section covers all built-in types regardless of kind; the heading now reflects that.
- **[§8.7](Bezel_Language_Spec_0_98.md#sec-dispose-is-not-on-object) and [§11.7](Bezel_Language_Spec_0_98.md#sec-static-classes): static classes defined.** A class declared `static` may contain only static members, cannot be instantiated, and does not implement `IDisposable`. The `abstract`, `sealed`, and `static` modifiers are mutually exclusive on class declarations.
- **[§17.5](#sec-member-hiding) `in` operator and extension methods.** Clarified that the structural `Contains` lookup includes extension methods ([§26](#sec-v0-59-to-v0-60-changelog)), not only instance members.
- **[§19.8](Bezel_Language_Spec_0_98.md#sec-inside-enum-scrutinees) deduplicated.** The section on enum prefix omission in `case` patterns was duplicated between [§10.6](Bezel_Language_Spec_0_98.md#sec-case-matching) and [§19.8](Bezel_Language_Spec_0_98.md#sec-inside-enum-scrutinees). [§19.8](Bezel_Language_Spec_0_98.md#sec-inside-enum-scrutinees) is now a cross-reference to [§10.6](Bezel_Language_Spec_0_98.md#sec-case-matching).
- **[§22.8](Bezel_Language_Spec_0_98.md#sec-field-initializers) `readonly` instance fields specified.** A field declared `readonly` may be assigned only in a field initializer or in a constructor body. This was implied by the grammar and by `readonly` auto-property semantics but never stated for fields.
- **[§18.8](Bezel_Language_Spec_0_98.md#sec-escape-sequences-non-verbatim) `\x` escape greedy matching documented.** `\x` greedily consumes the maximum number of valid hex digits, up to 4. A clarifying note with an example resolves the ambiguity.
- **[§28.6](Bezel_Language_Spec_0_98.md#sec-leak-prevention) methods returning `this`.** A method that returns `this` returns a borrow of the receiver. The ownership semantics of method chaining (e.g., `sb.Append("a").Append("b")`) are now specified.
- **[§24.1](Bezel_Language_Spec_0_98.md#sec-declaration) event backing field ownership specified.** The compiler generates a private `owned` backing field of type `EventSource<T>` (or `AsyncEventSource<T>`) for each event declaration. The backing field is disposed with the containing object.
- **[§24.1](Bezel_Language_Spec_0_98.md#sec-declaration) events-in-interfaces gap acknowledged.** A note explains the limitation and signals intent to resolve it in a future revision.
- **[§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) record inheritance restricted to interfaces.** Records may not inherit from other records or classes (other than the implicit `Object` root); only interface implementations appear in the inheritance clause.
- **[§30.1](Bezel_Language_Spec_0_98.md#sec-async-methods) async method execution corrected.** The previous text ("does not run its body synchronously") contradicted [§30.13](Bezel_Language_Spec_0_98.md#sec-runtime-model) ("begins execution synchronously until the first await"). [§30.1](Bezel_Language_Spec_0_98.md#sec-async-methods) now agrees with [§30.13](Bezel_Language_Spec_0_98.md#sec-runtime-model).
- **[§30.12](Bezel_Language_Spec_0_98.md#sec-thread-synchronization) `Mutex` example corrected.** The `_mutex` field is now declared `owned`, consistent with [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer)'s rule that `new` results must reach owned locations.
- **Appendix A.4 `VirtualKind` grammar fixed.** The grammar now matches [§11.5](Bezel_Language_Spec_0_98.md#sec-modifier-order)'s modifier order: `"sealed"? "override"` instead of `"override" "sealed"?`.
- **Appendix C.10 updated.** Reflects the permissive `static const` rule.
- **[§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types) exception list expanded.** `TypeInitializationException` (producer: type loading, [§22.7](Bezel_Language_Spec_0_98.md#sec-constructors)) and `FormatException` (producer: `Parse` methods on auto-imported primitives) added to the built-in set.
- **Forward-looking version numbers removed.** References like "v0.56 does not yet provide" are replaced with "this version does not yet provide" throughout all three documents. Specific version numbers now appear only in changelogs when referring to past revisions. This eliminates a class of busywork that produced stale references every revision.

<a id="sec-v0-56-to-v0-57-changelog-standard-library"></a>
<a id="sec-standard-library-3"></a>
### Standard library

- `Dictionary<K, V>.Contains(key: K): bool` added as an alias for `ContainsKey`, enabling `key in dict`.
- `DateTime.operator -(d: DateTime, t: TimeSpan): DateTime` added.
- `Task.FireAndForget(task: Task)` added.
- `String` indexer corrected from `public property [i: int]: char` to `public readonly property [i: int]: char`, matching the immutability documented in language spec [§4.6](Bezel_Language_Spec_0_98.md#sec-string).
- All destructor declarations changed from `public destructor;` to `destructor;`.
- [§1](#sec-the-big-picture) Conventions: a note clarifies that compiler-intrinsic types may declare `const` members whose values are compiler-provided, exempting them from the source-level initializer requirement. A second note updates the destructor convention to reflect the removed `public` keyword.

<a id="sec-v0-56-to-v0-57-changelog-design-rationale"></a>
<a id="sec-changelog-design-rationale"></a>
### Design rationale

- **Two entries added under [§17](#sec-things-explicitly-rejected) ("Things explicitly rejected"):** post-condition loops (`do-while`), explaining the deliberate omission and the interaction with `do ... end` block syntax; and combined visibility modifiers (`protected internal`), explaining why the gap is accepted and noting that the intersection form can be added later as a non-breaking change. (The post-condition-loops entry was subsequently moved into [§18 Things deferred](#sec-things-deferred); the combined-visibility entry remains in [§17](#sec-things-explicitly-rejected) and is reachable at its slug, [`sec-combined-visibility-modifiers-protected-internal`](#sec-combined-visibility-modifiers-protected-internal).)

<a id="sec-v0-56-to-v0-57-changelog-migration-guide"></a>
<a id="sec-migration-guide-2"></a>
### Migration guide

The only source-breaking change is the removal of `public` from destructor declarations. Migration: delete the word `public` before every `destructor` keyword. All other changes are source-compatible additions or specification clarifications.

---

<a id="sec-v0-57-to-v0-58-changelog"></a>
### v0.57 to v0.58 changelog

Normative for migration; not part of the language definition.

This revision is a correctness pass. v0.57's changelog contained a self-contradictory note, several cross-references pointed to wrong sections, the grammar included an undefined production, and a number of specification gaps left important behaviors unspecified. There are no source-breaking changes.

<a id="sec-v0-57-to-v0-58-changelog-source-breaking"></a>
<a id="sec-source-breaking-4"></a>
### Source-breaking

None.

<a id="sec-v0-57-to-v0-58-changelog-source-compatible"></a>
<a id="sec-source-compatible-5"></a>
### Source-compatible

- **`Task.FireAndForget` parameter is now `owned`.** The signature changes from `FireAndForget(task: Task)` to `FireAndForget(owned task: Task)`, matching the documented semantics ("the calling code relinquishes ownership"). Call sites that were already discarding the task after the call are unaffected; call sites that continued to use the task after the call were already violating the intent and will now be caught by the compiler.

<a id="sec-v0-57-to-v0-58-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-3"></a>
### Specification-only (no behavioral change)

- **[§3.1](#sec-always-semicolons) cross-reference corrected.** The `string` type-kind entry referenced [§4.7](Bezel_Language_Spec_0_98.md#sec-the-unit-type) (the unit type); it now correctly references [§4.6](Bezel_Language_Spec_0_98.md#sec-string).
- **[§8.6](Bezel_Language_Spec_0_98.md#sec-boxing) nullable value-type boxing specified.** Boxing a `null` `T?` produces a null `Object` reference; boxing a non-null `T?` produces a boxed `T` (the nullable wrapper is stripped).
- **[§11.2](#sec-naked-binding-inside-positional-patterns) read-write satisfies read-only.** A read-write member in an implementing class satisfies a read-only interface member of the same name and type.
- **[§12.8](Bezel_Language_Spec_0_98.md#sec-constructor-chaining) constructor chaining terminology corrected.** The base/this chain is described as a distinct syntactic element before the constructor body, not as "the first statement."
- **[§14.2](#sec-mandatory-parentheses-on-mixed-precedence) `IDisposable` constraint equivalence dependency noted.** The equivalence between `where T: IDisposable` and `where T: class` depends on the [§9.7](Bezel_Language_Spec_0_98.md#sec-records-and-disposal) rule that records may not implement `IDisposable`.
- **Spec [§17.10](Bezel_Language_Spec_0_98.md#sec-the-typeof-operator) `Type` identity for generics specified.** Each distinct generic instantiation has a distinct `Type` instance; `typeof(T)` inside a generic body returns the substituted type.
- **[§2.3](Bezel_Language_Spec_0_98.md#sec-always-available-names) `Subscription` auto-import clarified.** `Subscription` is declared in `System.Threading` but auto-imported as a language primitive; a note explains why.
- **[§10.4](Bezel_Language_Spec_0_98.md#sec-methods-properties-static-members) enum static readonly initialization order documented.** Enum constants are compile-time values available before static readonly initializers run.
- **[§24.4](Bezel_Language_Spec_0_98.md#sec-subscribing) event `+=`/`-=` scope clarified.** A note makes explicit that `+=`/`-=` syntax is available only inside the declaring class (and derived classes for protected events); external code uses `.Subscribe()`.
- **[§30.8](Bezel_Language_Spec_0_98.md#sec-cancellation) `CancellationToken` thread safety specified.** Reading `IsCancellationRequested` through a borrowed token is guaranteed atomic and visible across threads without additional synchronization.
- **[§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types) `FormatException` classification fixed.** `FormatException` appeared in both the auto-imported built-in list and the import-required domain-specific list; it now appears only in the auto-imported list.
- **[§32.5](Bezel_Language_Spec_0_98.md#sec-ffi-compatible-types) `bool` FFI representation specified.** `false` is byte 0, `true` is byte 1; any non-zero byte received from native code is treated as `true`.
- **Appendix A.6 `UsingStmt` removed.** The `Statement` production listed `UsingStmt`, which had no definition. The `using var` forms are handled by `LocalDecl`; the stale alternative has been deleted.
- **Appendix A.4 `ExpressionPropertyDecl` note added.** A grammar comment clarifies that `readonly` is redundant on expression-bodied properties (they have no setter) but permitted for consistency.
- **[§15.3](Bezel_Language_Spec_0_98.md#sec-collection-literals) collection literal description expanded.** The prose now mentions the nested-literal form used for multi-dimensional array initialization.
- **v0.57 changelog self-contradiction removed.** The "specification-only" note about [§13.3](#sec-method-reference-subscriptions) destructor visibility contradicted the source-breaking change in the same changelog; the incorrect note is deleted.

<a id="sec-v0-57-to-v0-58-changelog-standard-library"></a>
<a id="sec-standard-library-4"></a>
### Standard library

- `Task.FireAndForget(owned task: Task)` — parameter is now `owned`, matching its documented ownership-transfer semantics.
- `Stream.ReadAll(): byte[]` and `Stream.ReadAllAsync(token): Task<byte[]>` added as convenience methods, resolving dangling references in language spec examples.
- `Dictionary<K, V>` — a note acknowledges the inherited `Add(DictionaryEntry<K, V>)`, `Remove(DictionaryEntry<K, V>)`, and `Contains(DictionaryEntry<K, V>)` from `ICollection`.
- `TextReader` and `TextWriter` surface declarations added ([§7.4](#sec-float-equality-is-reflexive)), since `Console.In` and `Console.Out` return these types.
- `Encoding` — a note clarifies that the concrete subclasses behind the static properties (`Utf8`, `Utf16`, etc.) are implementation-provided and not part of the public API.
- `DateTime.Now`, `DateTime.UtcNow`, `DateTime.Today` — a comment clarifies that these are computed properties returning the current time at each access; `readonly` means no setter, not constant value.

<a id="sec-v0-57-to-v0-58-changelog-design-rationale"></a>
<a id="sec-design-rationale-2"></a>
### Design rationale

No new rationale sections. This changelog is the only addition.

<a id="sec-v0-57-to-v0-58-changelog-migration-guide"></a>
<a id="sec-migration-guide-3"></a>
### Migration guide

The only change affecting existing code is the `owned` parameter on `Task.FireAndForget`. Call sites that passed a task and then continued to use it afterward were already violating the documented contract; the compiler now enforces it. All other changes are specification clarifications or standard library additions.

---

<a id="sec-v0-58-to-v0-59-changelog"></a>
### v0.58 to v0.59 changelog

Normative for migration; not part of the language definition.

This revision is a consistency and correctness pass focused on internal contradictions, specification gaps, and missing guidance. There are no source-breaking changes.

<a id="sec-v0-58-to-v0-59-changelog-source-breaking"></a>
<a id="sec-source-breaking-5"></a>
### Source-breaking

None.

<a id="sec-v0-58-to-v0-59-changelog-source-compatible"></a>
<a id="sec-source-compatible-6"></a>
### Source-compatible

None.

<a id="sec-v0-58-to-v0-59-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-4"></a>
### Specification-only (no behavioral change)

- **[§3.4](Bezel_Language_Spec_0_98.md#sec-keyword-aliases-for-built-in-types) renamed from "Standard library names for primitives" to "Keyword aliases for built-in types."** `string` is a reference type ([§3.2](#sec-the-discard-_)), not a primitive value type ([§3.1](#sec-always-semicolons)). The table now notes the inclusion of `string` alongside primitives.
- **[§9.2](#sec-why-records-are-sealed) body-record non-auto-equality mechanism clarified.** The compile error on `=`/`<>` for non-auto-equality-bearing body records is specific to the operator; calling inherited `.Equals(Object?)` directly is permitted but warned against, since it falls back to boxed-reference comparison on a value type.
- **[§9.5](#sec-sealed-abstract-classes) `with` and classes contradiction resolved.** The sentence "`with` does not apply to classes" is replaced with a precise statement: `with` does not apply to existing class instances for non-destructive update; the `with ... end` syntax on a `new` class expression is an object initializer block ([§22.11](Bezel_Language_Spec_0_98.md#sec-object-initializer-blocks)) with mutating semantics.
- **[§9.5](#sec-sealed-abstract-classes) `with` and constructor validation noted.** A new paragraph acknowledges that `with` copies field values directly without re-running the record's constructor; records with cross-field invariants should provide named methods for constrained updates.
- **[§11.5](Bezel_Language_Spec_0_98.md#sec-modifier-order) modifier order corrected.** `sealed` alone is removed from the member-modifier list; it applies only to class declarations ([§12.4](#sec-cooperative-cancellation)). The member-level forms are `override` and `sealed override`. The grammar's `VirtualKind` production is updated to match.
- **[§12.7](Bezel_Language_Spec_0_98.md#sec-type-tests-is-is-not-as) `is not T` else-branch narrowing specified.** The else branch of `is not T` now narrows the variable to `T`, mirroring the true-branch narrowing of `is T`. The full narrowing table for all four forms (`is T`, `is not T`, `is null`, `is not null`) is now stated explicitly for both branches.
- **[§15](#sec-strings) `T[]` vs `Array<T>` guidance added.** A short paragraph after the introduction advises when to use fixed-size arrays versus resizable arrays.
- **[§17.1](#sec-truthiness-in-conditions) `++` and `--` removed from precedence table.** These are statement-only constructs ([§17.6](#sec-inline-fixed-size-value-type-array-fields)) that do not participate in expressions and have no meaningful precedence. A note in [§17.2](#sec-string-concatenation-with-non-strings) explains the removal.
- **[§17.11](Bezel_Language_Spec_0_98.md#sec-the-default-operator) `default(T)` recursive record check specified.** The non-nullable-reference-field check for records is explicitly stated to be recursive through nested record-typed fields.
- **[§19.5](Bezel_Language_Spec_0_98.md#sec-type-pattern-matching-never-matches-null) typed binding patterns and null clarified.** `var n: T` (a typed binding pattern) follows type-pattern semantics and does not match `null`. Only untyped `var x` patterns and `_` match null.
- **[§21.9](Bezel_Language_Spec_0_98.md#sec-statement-form-of-expressions) non-`()` expression statements specified.** An expression statement whose result is a non-`()` type is permitted but emits a warning about discarded results. The discard form `_ := expr;` suppresses the warning.
- **[§22.4](Bezel_Language_Spec_0_98.md#sec-inline-out-declarations) `out owned` call-site semantics documented.** The meaning of `out owned`, `out var`, and `out using var` at the call site of an `out owned` parameter is now stated.
- **[§24.6](Bezel_Language_Spec_0_98.md#sec-thread-safety) event thread-safety guidance expanded.** A note explains that async continuations may cause concurrent event access even in seemingly single-threaded code, and recommends `Mutex` protection or single-thread confinement.
- **[§27.7.1](Bezel_Language_Spec_0_98.md#sec-equality-of-function-values) function-type equality semantics added.** Function values use reference equality. Two separately-constructed lambdas with identical source text are not equal. The practical consequences for event `-=` removal are noted.
- **language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) summary table added.** A table at the top of the resource management section summarizes disposal obligations by type kind: classes (yes), records (no), string (no-op), boxed values (no-op), function values (no-op), arrays (yes).
- **[§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) enumerator allocation optimization note added.** The compiler may elide the `IEnumerable`/`IEnumerator` protocol for arrays, strings, and range literals, ensuring that common `for` patterns incur no allocation or disposal overhead.
- **[§4.6](Bezel_Language_Spec_0_98.md#sec-string) string interning specified.** Whether the runtime interns strings is implementation-defined. String literals may be interned at compile time. The `=` operator always performs content comparison; implementations may short-circuit via reference equality as an invisible optimization.
- **Appendix A.4 `VirtualKind` grammar updated.** `"sealed"` as a standalone alternative is removed; only `"sealed"? "override"` remains, matching [§11.5](Bezel_Language_Spec_0_98.md#sec-modifier-order).
- **Appendix A.6 `ConstDecl` in `Statement` replaced with `LocalConstDecl`.** The statement-level production now uses `LocalConstDecl` (no visibility or `static` modifier), fixing the mismatch with the member-level `ConstDecl` that requires visibility.
- **Appendix B.1 keyword list reorganized.** Unconditionally reserved keywords and contextual keywords (`convention`, `step`, `value`) are now listed separately for clarity.

<a id="sec-v0-58-to-v0-59-changelog-standard-library"></a>
<a id="sec-standard-library-5"></a>
### Standard library

No changes.

<a id="sec-v0-58-to-v0-59-changelog-design-rationale"></a>
<a id="sec-design-rationale-3"></a>
### Design rationale

- **[§17.8](#sec-readonly-local-bindings-let-val-readonly-var) added: readonly local bindings.** Explains why `let`, `val`, and `readonly var` are not provided in this version. Notes that extending `const` to runtime-evaluated expressions is the leading candidate for a future revision.
- **[§17.9](#sec-variadic-parameters-params) added: variadic parameters (`params`).** Explains why the `params` mechanism is not provided: the collection-literal call-site syntax makes allocation visible and keeps overload resolution unambiguous.
- **[§18](#sec-things-deferred) deferred list updated.** "Runtime-evaluated `const` for locals" added as a deferred feature, cross-referencing [§17.8](#sec-readonly-local-bindings-let-val-readonly-var).

<a id="sec-v0-58-to-v0-59-changelog-migration-guide"></a>
<a id="sec-migration-guide-4"></a>
### Migration guide

No source-breaking changes. No migration steps required.

---

<a id="sec-v0-59-to-v0-60-changelog"></a>
### v0.59 to v0.60 changelog

Normative for migration; not part of the language definition.

This revision is primarily a correctness and completeness pass. v0.59 had a significant gap in the ownership model for factory methods and collection literals, an inconsistency between [§22.7](Bezel_Language_Spec_0_98.md#sec-constructors) and [§12.8](Bezel_Language_Spec_0_98.md#sec-constructor-chaining) on constructor chaining syntax, unclear wording around `with` on positional records, and missing specifications for borrow-bearing record boxing, `Dispose()` thread safety, and the exception inheritance hierarchy. The `await for` construct gained a `with` clause for cancellation token propagation. The standard library received `owned` annotations on factory methods that return freshly-allocated class instances.

<a id="sec-v0-59-to-v0-60-changelog-source-compatible"></a>
<a id="sec-source-compatible-7"></a>
### Source-compatible

- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) factory method ownership rule added.** Static methods (and other methods without an identifiable borrow source) returning a class type without `owned` are treated as producing an owned value at the call site. The compiler warns and recommends that the callee add `owned` to its return type. Code that was already disposing such results or storing them in `owned` fields is unaffected. Code that was silently dropping factory-method results will now see a warning (not an error) recommending explicit disposal or discard.
- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) collection literal ownership specified.** Collection literals targeting reference types (`T[]`, `Array<T>`) produce owned values, subject to the same discharge rules as `new` expressions. Code that was already disposing collection-literal results is unaffected.
- **`await for ... with token` added.** [§30.7](Bezel_Language_Spec_0_98.md#sec-asynchronous-programming-async-iterators): `await for` now accepts an optional `with CancellationToken` clause that passes the token to `GetAsyncEnumerator`. When omitted, `CancellationToken.None` is passed. Existing `await for` code without `with` is unaffected; the default behavior is identical to v0.59. Grammar A.6 updated.
- **[§28.2](Bezel_Language_Spec_0_98.md#sec-the-idisposable-contract) `Dispose()` thread safety specified.** The compiler-generated `Dispose()` is now specified as thread-safe with respect to idempotency: concurrent calls execute disposal exactly once.

<a id="sec-v0-59-to-v0-60-changelog-specification-only"></a>
<a id="sec-specification-only-2"></a>
### Specification-only

- **[§22.7](Bezel_Language_Spec_0_98.md#sec-constructors) constructor chaining wording corrected.** The phrase "as the first statement" is replaced with "in the chaining position before the body," consistent with [§12.8](Bezel_Language_Spec_0_98.md#sec-constructor-chaining)'s description of `base(…)` / `this(…)` as syntactically distinct from the body's statements.
- **[§9.5](#sec-sealed-abstract-classes) `with` on records rewritten for clarity.** The target rule now leads with positional-field properties ("regardless of their read-only status") and clarifies that `with` constructs the copy directly from storage without invoking setters. The previous phrasing — listing "public settable properties" first, then adding positional fields — was confusing since positional properties are read-only.
- **[§5.1](#sec-owned-not-borrows) signed/unsigned implicit conversion rule reworded.** The old rule "Signed to unsigned, in either direction" was ambiguous because `byte → short` (unsigned to wider signed) is permitted. The new wording explicitly says "between signed and unsigned types of the same or narrower width."
- **[§8.6](Bezel_Language_Spec_0_98.md#sec-boxing) boxing of borrow-bearing records specified.** Boxing a borrow-bearing record does not extend the lifetimes of its borrowed references. The boxed value is subject to the same borrow-lifetime constraints as the unboxed form. Appendix C.1 updated.
- **[§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types) exception hierarchy specified.** The inheritance relationships between built-in exceptions are now stated: `ArgumentNullException` derives from `ArgumentException`; `OverflowException` and `DivideByZeroException` derive from `ArithmeticException`. A note on catch-ordering implications is included.
- **[§2.3](Bezel_Language_Spec_0_98.md#sec-always-available-names) auto-imported names expanded.** `IComparable<T>` and `IEquatable<T>` are now listed in the auto-imported set from `System`, matching their actual availability.
- **[§4.6](Bezel_Language_Spec_0_98.md#sec-string) `in` operator on strings noted.** The `c in s` form is now explicitly listed in the string operations section, cross-referencing [§17.5](#sec-member-hiding).
- **[§17.11](Bezel_Language_Spec_0_98.md#sec-the-default-operator) `default(T)` for body-only records clarified.** The eligibility rule now specifies that auto-property backing storage is considered, and expression-bodied properties have no backing storage and do not affect `default` eligibility.
- **[§12.4](#sec-cooperative-cancellation), [§12.5](Bezel_Language_Spec_0_98.md#sec-abstract-classes-and-members), [§12.6](Bezel_Language_Spec_0_98.md#sec-async-and-abstract) examples fixed.** Missing visibility modifiers (`public`) added to class declarations in the sealing, abstract class, and abstract-method examples.
- **Grammar A.7 `Pattern` updated.** `"null"` added as an explicit alternative, matching the prose in [§19.5](Bezel_Language_Spec_0_98.md#sec-type-pattern-matching-never-matches-null).
- **Appendix C updated.** New entries: collection literal ownership (C.1), boxing of borrow-bearing records (C.1), static factory method ownership (C.3), `Dispose()` thread-safe idempotency (C.3), `await for with` clause type check (C.6).

<a id="sec-v0-59-to-v0-60-changelog-standard-library"></a>
<a id="sec-standard-library-6"></a>
### Standard library

- **`owned` annotations added to factory methods.** Static methods and instance methods that return freshly-allocated class instances now annotate the return type with `owned`. Affected methods include: `File.ReadAllBytes`, `File.ReadAllLines`, `Stream.ReadAll`, `Stream.ReadAllAsync`, `Stopwatch.StartNew`, `Guid.ToByteArray`, `String.Split` (all overloads), `String.ToCharArray`, `Array<T>.ToArray`, `Encoding.GetBytes`, and the LINQ materialization methods (`ToArray`, `ToResizableArray`, `ToDictionary`, `ToSet`). Methods returning `string` or other runtime-managed types are unchanged.
- **[§1](#sec-the-big-picture) conventions updated.** A new bullet documents the `owned` return-type convention for factory methods.
- **[§3.2](#sec-the-discard-_) `Encoding` singleton ownership clarified.** The static singleton properties (`Utf8`, `Utf16`, etc.) are documented as program-lifetime objects with no-op `Dispose()`, exempt from ownership discharge.

<a id="sec-v0-59-to-v0-60-changelog-design-rationale"></a>
<a id="sec-design-rationale-4"></a>
### Design rationale

- **[§5.7](#sec-factory-method-ownership) added: factory method ownership.** Explains the rationale for the sourceless-return rule: static factory methods have no receiver to borrow from, so the caller must own the result. The `owned` annotation is preferred; the compiler-inferred rule is a safety net.
- **[§5.8](#sec-collection-literal-ownership) added: collection literal ownership.** Notes that a collection literal targeting a reference type is semantically a `new` expression and should follow the same ownership rules.

<a id="sec-v0-59-to-v0-60-changelog-migration-guide"></a>
<a id="sec-migration-guide-5"></a>
### Migration guide

No source-breaking changes. The new ownership rules for factory methods and collection literals produce warnings (not errors) on code that was previously undiagnosed. The recommended migration is to add `using var` or `owned` field storage to call sites of factory methods returning class instances, or to use `_ := expr;` for deliberate discards.

---

<a id="sec-v0-60-to-v0-61-changelog"></a>
### v0.60 to v0.61 changelog

Normative for migration; not part of the language definition.

This revision is a correctness, consistency, and completeness pass. v0.60 had a contradiction between the spec and design rationale on field narrowing, a missing keyword (`owned`) in the reserved-words list, an unspecified primary-constructor syntax for classes that the grammar accepted but the prose never described, and several gaps in the standard library (no linked-list node type, no secondary-sort LINQ methods, no dictionary/set collection literals). The enum interface-implementation restriction is lifted. A `sealed abstract` class modifier is introduced for closed hierarchies. The ownership model gains a "trivial-dispose" classification to reduce friction for resource-free classes. `CancellationToken` is auto-imported.

<a id="sec-v0-60-to-v0-61-changelog-source-compatible"></a>
<a id="sec-source-compatible-8"></a>
### Source-compatible

- **Primary constructors for classes ([§11.1](#sec-the-scrutinee-less-form)).** Classes may now declare parameters in parentheses after the class name, analogous to records. The compiler generates a primary constructor; parameters are in scope in field initializers but not in method bodies. Existing code is unaffected.
- **`sealed abstract` class modifier ([§11.7](Bezel_Language_Spec_0_98.md#sec-static-classes)).** The combination `sealed abstract` is now permitted, creating a closed abstract class: cannot be instantiated, cannot be subclassed outside the declaring module. This strengthens exhaustiveness checking for `case` matches ([§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness)). Existing `abstract`-only hierarchies continue to work.
- **Enum interface implementation ([§10.5](Bezel_Language_Spec_0_98.md#sec-restrictions)).** Enums may now implement interfaces. The interface names appear after the underlying type in the declaration. Existing enums without interfaces are unaffected.
- **Collection literals for `Set<T>` and `Dictionary<K, V>` ([§15.3](Bezel_Language_Spec_0_98.md#sec-collection-literals)).** `Set<T>` accepts bracket-literal syntax (`var s: Set<int> := [1, 2, 3]`). `Dictionary<K, V>` accepts key-value literal syntax (`var d: Dictionary<string, int> := ["a": 1, "b": 2]`). Grammar A.8 updated with `DictEntry` production. Existing `T[]` and `Array<T>` literals are unaffected.
- **`CancellationToken` auto-imported ([§2.3](Bezel_Language_Spec_0_98.md#sec-always-available-names)).** `CancellationToken` is now a language primitive, available without `import System.Threading`. `CancellationTokenSource`, `Mutex`, and other threading types still require the import. Existing code with the explicit import is unaffected.
- **Trivial-dispose class classification ([§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)).** Classes with no destructor, no `owned` fields, and no non-trivial base class are classified as trivial-dispose. Ownership warnings on `new` expressions of trivial-dispose types are reduced to informational severity. Existing `using var` usage on such types remains valid.
- **`ThenBy` / `ThenByDescending` added to LINQ (stdlib [§5](Bezel_Standard_Library_0_98.md#sec-system-collections-linq)).** `OrderBy` and `OrderByDescending` now return `IOrderedEnumerable<T>`, which supports secondary sorting via `ThenBy` and `ThenByDescending`. The return-type change from `IEnumerable<T>` to `IOrderedEnumerable<T>` is source-compatible because `IOrderedEnumerable<T>` extends `IEnumerable<T>`.
- **`LinkedListNode<T>` added (stdlib [§4.3](Bezel_Standard_Library_0_98.md#sec-linkedlistt)).** `LinkedList<T>` gains node-based operations: `AddBefore`, `AddAfter`, `RemoveNode`, `Find`. The `Add` methods now return the created node. `FirstNode` and `LastNode` properties expose node references.
- **`Stream.ReadAllToArray` / `ReadAllToArrayAsync` added (stdlib [§7.1](Bezel_Standard_Library_0_98.md#sec-stream)).** Convenience methods returning `owned Array<byte>` for resizable-array consumers.

<a id="sec-v0-60-to-v0-61-changelog-specification-only"></a>
<a id="sec-specification-only-3"></a>
### Specification-only

- **[§7.4](#sec-float-equality-is-reflexive) field narrowing removed.** The compiler no longer narrows nullable fields; only local variables are narrowed. The design rationale ([§6.1](#sec-field-narrowing-is-not-safe-across-threads)) already stated this, but the spec text was contradictory. Field narrowing is now a compile error; the copy-to-local pattern is required. This is a specification correction, not a behavioral change — the rationale always intended this behavior.
- **Appendix B: `owned` added to reserved keywords.** `owned` was used throughout the spec as a modifier but was missing from the keyword list.
- **[§30.3](Bezel_Language_Spec_0_98.md#sec-the-task-and-taskt-types) `Task<T>.Result` behavior specified.** Reading `Result` blocks until the task completes; reading on a faulted task re-raises the exception; reading on a cancelled task raises `OperationCanceledException`.
- **[§8.3](Bezel_Language_Spec_0_98.md#sec-equals-and-gethashcode) `GetHashCode` combining specified.** Auto-generated `GetHashCode` combines per-field hashes in declaration order; the combining is order-dependent.
- **[§17.11](Bezel_Language_Spec_0_98.md#sec-the-default-operator) `default(T)` enum warning added.** The compiler warns when `default(T)` is used on an enum type with no declared zero-value constant.
- **[§9.5](#sec-sealed-abstract-classes) `with` on body-only records clarified.** Auto-properties declared in a record body are valid `with` targets.
- **[§26.5](Bezel_Language_Spec_0_98.md#sec-resolution) extension type parameter inference specified.** Block-level type parameters on generic extension blocks are inferred from the receiver's type through interface implementation.
- **[§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal) `IAsyncDisposable` fallback clarified.** A class implementing `IAsyncDisposable` without a destructor receives a compiler warning; synchronous `Dispose()` skips async-only cleanup.
- **[§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness) exhaustiveness updated for `sealed abstract`.** Closed hierarchies using `sealed abstract` provide cross-module exhaustiveness guarantees.
- **[§34](#sec-v0-67-to-v0-68-changelog) `BankAccount` example: missing `public` added.**
- **[§4.6](Bezel_Language_Spec_0_98.md#sec-string) `s.IsAscii` made normative.** Changed from "implementations should expose" to a required property.
- **[§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) `Point.CompareTo` example fixed.** Subtraction-based comparator replaced with explicit comparisons to avoid overflow.
- **[§15.3](Bezel_Language_Spec_0_98.md#sec-collection-literals) multi-dimensional literal sentence corrected.** Removed reference to runtime-sized mismatch that cannot occur under current syntax.

<a id="sec-v0-60-to-v0-61-changelog-standard-library"></a>
<a id="sec-standard-library-7"></a>
### Standard library

- `ICollection<T>` description corrected from "mutable collections" to "collections," matching its `IsReadOnly` contract.
- `LinkedList<T>` gains `LinkedListNode<T>`, node-returning methods, and `FirstNode`/`LastNode` properties.
- `IOrderedEnumerable<T>` interface added. `OrderBy`/`OrderByDescending` return `IOrderedEnumerable<T>`. `ThenBy`/`ThenByDescending` extension methods added.
- `Dictionary<K, V>` iteration order specified as insertion order.
- `Atomic<T>` restriction error message specified for non-primitive value types.
- `Encoding` subclassing semantics clarified.
- `Stream` gains `ReadAllToArray` and `ReadAllToArrayAsync` convenience methods.
- `String` comparison methods annotated as ordinal-only; culture-aware comparison deferred to `System.Globalization`.

<a id="sec-v0-60-to-v0-61-changelog-design-rationale"></a>
<a id="sec-design-rationale-5"></a>
### Design rationale

- **[§6.1](#sec-field-narrowing-is-not-safe-across-threads) field narrowing** updated to state the compiler does not narrow fields (matching corrected spec).
- **[§9.4](#sec-enum-interface-implementation) added: enum interface implementation.** Explains why the restriction is lifted: enums with methods but no interface conformance are second-class in generic algorithms.
- **[§9.5](#sec-sealed-abstract-classes) added: `sealed abstract` classes.** Explains the motivation for closed hierarchies and the interaction with exhaustiveness.
- **[§5.9](#sec-trivial-dispose-classes) added: trivial-dispose classes.** Explains why resource-free classes need relaxed ownership warnings.
- **[§18](#sec-things-deferred) deferred list updated.** "Enum interface implementation" removed (now implemented). "Custom collection-literal protocols" narrowed (Set and Dictionary now supported). "Non-owning views (`Span<T>`)" and other items remain.

<a id="sec-v0-60-to-v0-61-changelog-migration-guide"></a>
<a id="sec-migration-guide-6"></a>
### Migration guide

No source-breaking changes. Field narrowing was already prohibited by the design rationale; code that relied on it (if any existed) should copy fields to locals before null-checking. The new warnings (trivial-dispose informational, enum `default` without zero constant, `IAsyncDisposable` without destructor) are non-blocking.

---

<a id="sec-v0-61-to-v0-62-changelog"></a>
### v0.61 to v0.62 changelog

Normative for migration; not part of the language definition.

This revision is a correctness, consistency, and feature pass. v0.61 had a contradiction between [§11.1](#sec-the-scrutinee-less-form) and [§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors) on primary constructor parameter scoping, an unsound claim that `not` is non-overloadable while [§25.1](Bezel_Language_Spec_0_98.md#sec-overloadable-operators) listed it among overloadable operators, an incomplete list of async return types in Appendix C, several missing auto-imports for event interfaces, and a documentation gap for FFI-boundary types in the standard library. New features in this revision: events on interfaces, generic type aliases, and a documented multi-catch idiom (full multi-catch syntax deferred to v0.63).

<a id="sec-v0-61-to-v0-62-changelog-source-compatible"></a>
<a id="sec-source-compatible-9"></a>
### Source-compatible

- **Primary constructor parameter scoping clarified ([§11.1](#sec-the-scrutinee-less-form)).** Parameters are in scope everywhere in the class body, including method, property, constructor, and destructor bodies. The compiler captures parameters referenced in body positions into private fields. This matches C# 12 semantics and resolves the contradiction with [§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors). Existing code using primary-constructor parameters only in initializers is unaffected.
- **Events on interfaces ([§11.2](#sec-naked-binding-inside-positional-patterns), [§24.7](Bezel_Language_Spec_0_98.md#sec-events-on-interfaces), stdlib [§6.5](Bezel_Standard_Library_0_98.md#sec-events-ievent-eventsource)).** Interfaces may declare event members. The interface event has type `IEvent<T>` (sync) or `IAsyncEvent<T>` (async) externally; implementing classes provide the standard `event` declaration whose `EventSource<T>` backing satisfies the interface contract automatically. Existing code without interface events is unaffected.
- **Generic type aliases ([§27.8](Bezel_Language_Spec_0_98.md#sec-type-aliases)).** `type Result<T> = ...` is now permitted. Aliases may carry constraints. Each instantiation is interchangeable with the corresponding instantiation of the aliased type. Grammar A.2 updated. Existing non-generic aliases are unaffected.
- **`IEvent<T>` and `IAsyncEvent<T>` auto-imported ([§2.3](Bezel_Language_Spec_0_98.md#sec-always-available-names)).** External event consumers no longer need to `import System.Threading` to declare `IEvent<T>`-typed locals or parameters. Existing code with the explicit import is unaffected.
- **`String.From(CString)` and `String.From(WString)` overloads (stdlib [§9.6](Bezel_Standard_Library_0_98.md#sec-string)).** Documented FFI-string conversion methods that the language spec [§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling) already used in examples.
- **`CString`, `WString`, `RawBytes` documented (stdlib [§10](Bezel_Standard_Library_0_98.md#sec-ffi-boundary-types), originally introduced as [§11](#sec-pattern-matching) and renumbered in v0.64).** Previously referenced but not specified; now have full API surface in the standard library.

<a id="sec-v0-61-to-v0-62-changelog-specification-only"></a>
<a id="sec-specification-only-4"></a>
### Specification-only

- **[§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) corrected.** `not` is not user-overloadable. The rule is now consistent with the resolved [§25.1](Bezel_Language_Spec_0_98.md#sec-overloadable-operators).
- **[§25.1](Bezel_Language_Spec_0_98.md#sec-overloadable-operators) grammar updated.** `not` removed from `OpToken` in Appendix A.4. The `TriBool` example in [§25.3](Bezel_Language_Spec_0_98.md#sec-examples) was retained but rewritten in v0.62 to demonstrate `~` overloading instead.
- **[§28.4](Bezel_Language_Spec_0_98.md#sec-the-destructor) disposal wording softened.** The fourth step of `Dispose()` ("releases the heap block") was a runtime-implementation commitment that contradicted the Design Rationale's "runtime is unspecified" stance. The wording now says memory reclamation is implementation-defined and not part of the user-observable contract.
- **[§31.2](Bezel_Language_Spec_0_98.md#sec-try-catch-finally) example fixed.** `FileNotFoundException` (which requires `import System.IO`) replaced with `InvalidOperationException` (built-in).
- **[§31.2.1](Bezel_Language_Spec_0_98.md#sec-arm-patterns) added.** Multi-catch idiom documented (`catch ex: Exception when ex is A or ex is B then`).
- **[§19.3](Bezel_Language_Spec_0_98.md#sec-patterns) comma disambiguation noted.** The arm separator and pattern alternative separator are both `,`; the parser disambiguates by `=>`.
- **[§9.5](#sec-sealed-abstract-classes) `with` cross-references added.** Notes the three uses of `with` and their syntactic distinguishability.
- **[§25.2](Bezel_Language_Spec_0_98.md#sec-operator-overloading-restrictions) `=` overload semantics on records clarified.** Overloaded `=` and `<>` are invoked directly, not through `Equals`; authors should keep the two consistent.
- **[§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) `out _: T` rejected explicitly.** Mirrors [§22.4](Bezel_Language_Spec_0_98.md#sec-inline-out-declarations)'s rule for `out var name`.
- **[§4.8](Bezel_Language_Spec_0_98.md#sec-ffi-only-primitive-types) cross-reference to stdlib [§11](Bezel_Standard_Library_0_98.md#sec-modules-deferred-to-future-revisions) added (renumbered to [§10](#sec-generics) in v0.64).** FFI-boundary types now have a documentation pointer.
- **[§12.4](#sec-cooperative-cancellation) typo fixed.** "Bay!" → "Woof!".
- **Appendix C.5 updated.** Added `IAsyncEnumerable<T>` and `IAsyncEnumerator<T>` to the list of valid async return types.
- **Appendix C.9 updated.** Clarified that `~` is overloadable but `not` is not.
- **Appendix A.2 updated.** `TypeAliasDecl` grammar extended for generic aliases.
- **Appendix A.4 updated.** `IfaceEvent` production added to `InterfaceMember`.
- **Stdlib [§4.3](Bezel_Standard_Library_0_98.md#sec-linkedlistt) ownership note for `LinkedListNode<T>`.** Nodes are borrows of the list; references become invalid after node removal.
- **Stdlib [§4.4](Bezel_Standard_Library_0_98.md#sec-dictionaryk-v) dictionary key constraint note.** Documents the runtime expectation that the type system cannot enforce.
- **Stdlib [§2.4](Bezel_Standard_Library_0_98.md#sec-environment) import note for `Environment.GetEnvironmentVariables()`.**

<a id="sec-v0-61-to-v0-62-changelog-standard-library"></a>
<a id="sec-standard-library-8"></a>
### Standard library

- `String.From(CString)` and `String.From(WString)` overloads added.
- `CString`, `WString`, and `RawBytes` types fully documented in new [§11](#sec-pattern-matching).
- The deferred-modules section renumbered [§10](#sec-generics) → [§12](#sec-async).

<a id="sec-v0-61-to-v0-62-changelog-design-rationale"></a>
<a id="sec-design-rationale-6"></a>
### Design rationale

- **[§1](#sec-the-big-picture) borrow-checker characterization corrected.** Bezel has a simplified borrow-lifetime tracker, not zero borrow checking. The previous wording understated the analysis the spec already requires.
- **language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) added: this changelog.**

<a id="sec-v0-61-to-v0-62-changelog-migration-guide"></a>
<a id="sec-migration-guide-7"></a>
### Migration guide

No source-breaking changes. The new features (interface events, generic type aliases, expanded auto-imports) are additive. Specification-only fixes resolve contradictions but do not change implemented behavior — implementations following the spec's intent in v0.61 (rather than its literal contradictory text) are already correct under v0.62.

The arm-form `catch` syntax is **not** included in this revision; it will be introduced in v0.63 with dedicated migration documentation given the breaking change.

---

<a id="sec-v0-62-to-v0-63-changelog"></a>
### v0.62 to v0.63 changelog

Normative for migration; not part of the language definition.

This revision has two related changes: arm-form `catch` syntax and the promotion of bare `throw` to an expression. The first is source-breaking; the second is purely additive. Together they restructure exception handling to mirror `case` arms throughout, eliminating the structural asymmetry between value and exception pattern dispatch.

<a id="sec-v0-62-to-v0-63-changelog-source-breaking"></a>
<a id="sec-source-breaking-6"></a>
### Source-breaking

- **`catch` syntax restructured ([§31.2](Bezel_Language_Spec_0_98.md#sec-try-catch-finally)).** The old form `catch <pattern> then <statements>;` (one or more clauses) is replaced by a single `catch` block containing comma-separated arms: `catch <arm>, <arm>, ... finally ...`. Each arm has the form `<pattern> (when <expr>)? => <body>`, mirroring `case` arms. Bindings use `var` (e.g. `var ex: IOException`); the no-binding form omits `var` (e.g. `IOException`). Multi-type alternatives use comma: `var ex: IOException, TimeoutException => ...`. Bodies are expressions or `do ... end` blocks. The `then` keyword is no longer used in `catch`; it remains the `if`/`elif` body separator.

  Migration is mechanical:
  - Each `catch <pattern> then <statements>;` becomes an arm `<pattern> => do <statements> end,` (with `var` added to bindings).
  - The closing `;` after the body of each old clause becomes a comma; the comma after the last arm is optional.
  - Single-statement bodies can use the expression form `=> <expr>` instead of `=> do <expr>; end`.
  - Arms whose body is just a bare re-raise can use the new bare `throw` expression: `=> throw` instead of `=> do throw; end`.

- **Multi-type catches now first-class.** v0.62's recommended idiom was `catch ex: Exception when ex is A or ex is B then ...`. v0.63 expresses this directly: `var ex: A, B => ...`. The bound variable's static type is the nearest common base of the listed types (typically `Exception` if the types are unrelated). The `when ... is` workaround still works but is no longer the recommended idiom.

<a id="sec-v0-62-to-v0-63-changelog-source-compatible"></a>
<a id="sec-source-compatible-10"></a>
### Source-compatible

- **Bare `throw` is now an expression ([§17.12](Bezel_Language_Spec_0_98.md#sec-the-throw-expression)).** The operand on `throw` is optional in expression position. `throw expr` raises the supplied exception (unchanged from v0.62); bare `throw` (no operand) re-raises the currently-handled exception. Both have bottom type. Bare `throw` is valid only inside a catch arm body — the same restriction as the statement form `throw;`, which continues to work unchanged.

  This change is purely additive: existing code using `throw;` as a statement is unaffected. The new expression form enables `=> throw` as an arm body in the new `catch` syntax, eliminating the `do ... end` overhead that would otherwise be required for bare re-raises.

<a id="sec-v0-62-to-v0-63-changelog-specification-only"></a>
<a id="sec-specification-only-5"></a>
### Specification-only

- [§31.2](Bezel_Language_Spec_0_98.md#sec-try-catch-finally) entirely rewritten to describe arm-form syntax. Subsections 31.2.1 through 31.2.5 added: arm patterns, arm bodies, ordering and exhaustiveness, bare re-raise (covering both expression and statement forms), deliberate suppression.
- [§31.2.1](Bezel_Language_Spec_0_98.md#sec-arm-patterns) (multi-catch idiom) from v0.62 removed. Multi-type catches are now first-class; the `when ex is A or ex is B` idiom is no longer the recommended approach (though it continues to work).
- [§17.12](Bezel_Language_Spec_0_98.md#sec-the-throw-expression) rewritten to define `throw` as an expression with an optional operand; both forms have bottom type.
- [§21.8](Bezel_Language_Spec_0_98.md#sec-throw) updated to note the relationship between expression-form `throw` and statement-form `throw;`.
- [§24.5](Bezel_Language_Spec_0_98.md#sec-raising-events) event handler exception example rewritten in arm form.
- [§29.6](Bezel_Language_Spec_0_98.md#sec-restrictions-inside-try-catch-finally) iterator try/catch restrictions reworded.
- [§30.9](Bezel_Language_Spec_0_98.md#sec-exception-handling-in-async-methods) async exception example rewritten in arm form.
- Appendix A.6 grammar: `TryStmt` updated; `CatchClause` removed; `CatchBlock`, `CatchArm`, `CatchPattern` productions added.
- Appendix A.7 grammar: `ThrowExpr` operand made optional.
- Appendix C invariant added: bare `throw` (and `throw;`) appears only inside a catch arm body.
- Appendix C.5 yield-in-try restriction wording updated.

<a id="sec-v0-62-to-v0-63-changelog-standard-library"></a>
<a id="sec-standard-library-9"></a>
### Standard library

No changes. The exception types and their relationships are unchanged.

<a id="sec-v0-62-to-v0-63-changelog-design-rationale"></a>
<a id="sec-design-rationale-7"></a>
### Design rationale

- [§11.3](#sec-catch-is-case-for-exceptions) added: rationale for `catch`-as-`case`, including a subsection on bare `throw` as an expression and why the asymmetry needed resolving.
- [§18](#sec-things-deferred) deferred list: arm-form `catch` removed (now implemented).
- [§29](#sec-v0-62-to-v0-63-changelog) added: this changelog.

<a id="sec-v0-62-to-v0-63-changelog-migration-guide"></a>
<a id="sec-migration-guide-8"></a>
### Migration guide

The `catch` syntax change is mechanical but pervasive. A find-and-replace tool can handle most of the conversion:

For each `catch <name>: <Type> when <guard> then <statements>;`:
- Convert to `var <name>: <Type> when <guard> => do <statements> end,`
- If `<statements>` is a single expression, simplify to `var <name>: <Type> when <guard> => <expr>`
- If `<statements>` is just `throw;`, simplify to `var <name>: <Type> when <guard> => throw`

For each `catch <Type> then <statements>;` (no binding):
- Convert to `<Type> => do <statements> end,`
- If `<statements>` is a single expression, simplify to `<Type> => <expr>`
- If `<statements>` is just `throw;`, simplify to `<Type> => throw`

Multiple consecutive `catch` clauses become arms separated by commas in a single `catch` block. The trailing comma on the last arm is optional. The `finally` block (if present) is unchanged. The closing `end;` is unchanged.

Tightening the new syntax — using multi-type arms instead of duplicated arms, replacing `when ex is A or ex is B` workarounds with first-class multi-type alternatives, replacing `=> do throw; end` with `=> throw` — is a manual cleanup pass after the mechanical conversion, not part of the migration.

The bare `throw` expression promotion requires no migration. Existing `throw;` statements continue to work. Programs may opt into the expression form where it improves readability (typically in single-arm bodies that just re-raise).

---

<a id="sec-v0-63-to-v0-64-changelog"></a>
### v0.63 to v0.64 changelog

Normative for migration; not part of the language definition.

This revision is a correctness and consistency pass. It resolves a number of internal contradictions (the `IList<T>` declaration mismatch between spec and standard library, the redundant `: IDisposable` on `Task`, the loose `where T: IDisposable` ≈ `where T: class` claim), fills several specification gaps (records with zero auto-properties, `++`/`--` on class-typed indexer slots, the unit type as an FFI return), tightens FFI semantics (lambdas of any kind are now rejected as callbacks), and produces a clearer presentation of the standard exception hierarchy. The standard library renumbers two sections to close a gap left by an earlier revision. There are no source-breaking changes to user code; one standard-library API (`CancellationTokenSource`'s side-effect-bearing constructor) is replaced by a static factory.

<a id="sec-v0-63-to-v0-64-changelog-source-breaking"></a>
<a id="sec-source-breaking-7"></a>
### Source-breaking

- **`CancellationTokenSource(delay: TimeSpan)` constructor removed; replaced by `CancellationTokenSource.WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource`.** Constructors that schedule callbacks were inconsistent with the language's preference for initialization-only constructors. The factory form makes the side effect explicit at the call site. Migration: `new CancellationTokenSource(delay)` → `CancellationTokenSource.WithCancelAfter(delay)`. The parameterless constructor is unchanged.

<a id="sec-v0-63-to-v0-64-changelog-source-compatible"></a>
<a id="sec-source-compatible-11"></a>
### Source-compatible

- **[§14.2](#sec-mandatory-parentheses-on-mixed-precedence) `where T: IDisposable` constraint clarified.** The constraint admits the same set of class substitutions as `where T: class`, but differs on interface substitutions: `where T: class` accepts any non-nullable interface, while `where T: IDisposable` accepts only `IDisposable`-extending interfaces. Existing code using either constraint with a class-typed argument is unaffected.
- **`()` is FFI-compatible as a return type ([§32.5](Bezel_Language_Spec_0_98.md#sec-ffi-compatible-types)).** An `extern` method with no `:` clause (or with explicit `: ()`) maps to C `void`. Existing FFI declarations are unaffected; this clarifies a previously-implicit case.
- **[§32.8](Bezel_Language_Spec_0_98.md#sec-callbacks) lambdas in FFI callbacks: now uniformly rejected.** Previously the spec rejected only capturing lambdas; non-capturing lambdas were ambiguous. The rule is now uniform: callbacks must be method references. Code that was passing capturing lambdas was already a compile error; code passing non-capturing lambdas (probably rare, given the rest of the FFI surface) needs to extract the body to a named static method.
- **`Task.Delay(duration: TimeSpan): Task` and the cancellable overload added (stdlib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers)).** The integer-milliseconds overload caps practical use at ~24.8 days; the `TimeSpan` form removes the cap and reads more naturally for long delays. Existing code using the integer form is unaffected.
- **`Dictionary<K, V>` constructor `(capacity: int, comparer: IEqualityComparer<K>)` added (stdlib [§4.4](Bezel_Standard_Library_0_98.md#sec-dictionaryk-v)).** Closes a gap in the constructor set; existing code using the parameterless, capacity-only, or comparer-only forms is unaffected.

<a id="sec-v0-63-to-v0-64-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-5"></a>
### Specification-only (no behavioral change)

- **[§3.2](#sec-the-discard-_)** clarifies that interface-typed variables are reference-typed; an interface itself is a contract, but a variable whose declared type is an interface holds a reference. This was assumed throughout the spec but never stated.
- **[§4.7](Bezel_Language_Spec_0_98.md#sec-the-unit-type)** notes that `()?` is a legal but rarely useful type.
- **[§9.2](#sec-why-records-are-sealed)** specifies that body-only records with zero auto-properties are auto-equality-bearing: `=` returns `true` for every pair of values, `GetHashCode` returns a fixed implementation-defined constant, `with` is rejected (no fields to replace), and `default(T)` produces the singleton value. Previously the case was undecided.
- **[§11.5](Bezel_Language_Spec_0_98.md#sec-modifier-order)** "exactly one may appear" → "at most one may appear" for the `abstract`/`virtual`/`override`/`sealed override` group; a non-virtual member carries none of them. The earlier wording read as if a virtual modifier was required.
- **[§11.2](#sec-naked-binding-inside-positional-patterns) and [§23.4](Bezel_Language_Spec_0_98.md#sec-indexers)** `IList<T>` interface example updated to match the standard library's declaration (`IList<T>: IReadOnlyList<T>, ICollection<T>` — `Add` and friends inherited from `ICollection`). The two declarations had drifted apart in earlier revisions.
- **[§17.6](#sec-inline-fixed-size-value-type-array-fields)** documents that `arr[i]++` (and `--`, and the compound assignments) on a non-owning collection of class-typed elements is rejected when the element type overloads the operator. The interaction with [§28.6](Bezel_Language_Spec_0_98.md#sec-leak-prevention)'s leak-prevention rule was unstated.
- **[§22.7](Bezel_Language_Spec_0_98.md#sec-constructors)** clarifies the interaction of static-init failed state with non-nullable reference fields: the failed-state machinery intercepts every access before any field read, so the absence of a default value is never observed and definite-assignment analysis is not violated.
- **[§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order)** specifies intra-type ordering for static `owned` field disposal at program exit: reverse declaration order within a type, mirroring the instance-field disposal rule.
- **[§27.7.1](Bezel_Language_Spec_0_98.md#sec-equality-of-function-values)** now includes a worked example of the reliable `+=`/`-=` pattern (store the function value once and reuse it for both calls) and the anti-pattern that silently fails to unsubscribe.
- **[§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal)** wording on `DisposeAsync` and the `async` modifier rewritten to be clearer: `async` is an implementation-side modifier and never appears on interface declarations.
- **[§30.3](Bezel_Language_Spec_0_98.md#sec-the-task-and-taskt-types)** removes the redundant `: IDisposable` from `Task`'s declaration; every class implicitly implements `IDisposable` per [§28.2](Bezel_Language_Spec_0_98.md#sec-the-idisposable-contract).
- **[§30.10](Bezel_Language_Spec_0_98.md#sec-standard-library-task-helpers)** documents both forms of `Task.Delay` (milliseconds and `TimeSpan`).
- **[§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types)** standard exception list rewritten as a tree, making the inheritance relationships visible at a glance.
- **[§32.5](Bezel_Language_Spec_0_98.md#sec-ffi-compatible-types)** lists the unit type `()` as FFI-compatible (return position only).
- **[§32.8](Bezel_Language_Spec_0_98.md#sec-callbacks)** rules out all lambdas, capturing or not, as FFI callbacks.
- **Appendix C.1** adds an explicit note that calling `.Equals(Object?)` on a non-auto-equality-bearing body record is permitted but resolves to boxed-reference comparison, with no compiler warning.

<a id="sec-v0-63-to-v0-64-changelog-standard-library"></a>
<a id="sec-standard-library-10"></a>
### Standard library

- **[§1](#sec-the-big-picture) conventions** clarify that the `destructor;` shorthand used throughout the document is documentation-only; user code requires a destructor body.
- **FFI-boundary types renumbered [§11](#sec-pattern-matching) → [§10](#sec-generics); deferred-modules renumbered [§12](#sec-async) → [§11](#sec-pattern-matching).** Closes the gap left by the [§10](#sec-generics)-removal in v0.62. Cross-references in the language spec ([§4.8](Bezel_Language_Spec_0_98.md#sec-ffi-only-primitive-types), [§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal)) updated accordingly.
- **`CancellationTokenSource(delay)` → `CancellationTokenSource.WithCancelAfter(delay)`.** See source-breaking note above.
- **`Dictionary<K, V>(capacity, comparer)` constructor added.**
- **`Task.Delay(TimeSpan)` and `Task.Delay(TimeSpan, CancellationToken)` added.**
- **[§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations)** explains why `internal` `Add`/`Remove` on `EventSource<T>` work correctly across module boundaries — the compiler-recognized `+=`/`-=` desugaring is special-cased to bypass the visibility check, but only when the static type warrants it.

<a id="sec-v0-63-to-v0-64-changelog-design-rationale"></a>
<a id="sec-design-rationale-8"></a>
### Design rationale

- [§13.5](#sec-handler-exception-policy-first-error-wins-not-aggregate) added: handler exception policy. Explains the deliberate choice of "first error wins" over `AggregateException`-style aggregation for synchronous and async event handlers.
- [§18](#sec-things-deferred) deferred list updated. No new deferrals; no items completed.

<a id="sec-v0-63-to-v0-64-changelog-migration-guide"></a>
<a id="sec-migration-guide-9"></a>
### Migration guide

The only required change for user code is replacing `new CancellationTokenSource(delay)` with `CancellationTokenSource.WithCancelAfter(delay)`. Find-and-replace handles this case directly. All other changes are specification clarifications, additive widenings, or documentation improvements.

---

<a id="sec-v0-64-to-v0-65-changelog"></a>
### v0.64 to v0.65 changelog

Normative for migration; not part of the language definition.

This revision continues the correctness-and-consistency pass begun in v0.64. It tightens the specification of `mod` (now explicitly integer-and-decimal only, with `Math.Fmod` and `Math.IEEERemainder` covering the floating-point cases), splits `Atomic<T>` into `Atomic<T>` and `AtomicInteger<T>` so that integer-only operations are visible in the type system rather than enforced by intrinsic restriction, declares `IDisposable` and `IAsyncDisposable` in the standard library (closing a long-standing reference-without-declaration gap), clarifies `Range<T>.Step` semantics for narrow `T`, and acknowledges the `do ... end` expression-position gap pointing at block expressions as a candidate future feature. There is one source-breaking standard-library change: `Atomic<int>.Increment()` and friends are now on `AtomicInteger<int>`.

<a id="sec-v0-64-to-v0-65-changelog-source-breaking"></a>
<a id="sec-source-breaking-8"></a>
### Source-breaking

- **`Atomic<T>` split into `Atomic<T>` and `AtomicInteger<T>` (stdlib [§6.4](Bezel_Standard_Library_0_98.md#sec-atomict-and-atomicintegert)).** The arithmetic methods `Increment`, `Decrement`, and `Add` move to `AtomicInteger<T>`. `Atomic<T>` retains `Value`, `Exchange`, and `CompareExchange`. `Atomic<T>`'s eligible-`T` set widens to include `char`, `IntPtr`, and `UIntPtr`. Migration: `new Atomic<int>(0)` → `new AtomicInteger<int>(0)` if any of `Increment`, `Decrement`, or `Add` is called on the instance; `new Atomic<bool>(false)` is unchanged. The compiler error on the old usage is a clear name-resolution failure rather than the previous opaque intrinsic-restriction message.
- **`mod` on `float` and `double` was always undefined; it is now an explicit compile error.** Previously the spec said `mod` was integer-only by omission (the `mod` row of [§6.1](#sec-field-narrowing-is-not-safe-across-threads) covered only integer arithmetic). Code that wrote `x mod y` with floating-point operands compiled in some implementations and not others; it now uniformly fails to compile. Migration: `x mod y` where `x: double` → `Math.Fmod(x, y)` (C-style) or `Math.IEEERemainder(x, y)` (IEEE 754).

<a id="sec-v0-64-to-v0-65-changelog-source-compatible"></a>
<a id="sec-source-compatible-12"></a>
### Source-compatible

- **`mod` on `decimal` is now explicitly defined ([§6.1](#sec-field-narrowing-is-not-safe-across-threads)).** Same dividend-sign rule as for integers; raises `DivideByZeroException` on a zero divisor. Existing decimal code using `mod` continues to work with the same semantics.
- **`Math.Fmod` and `Math.IEEERemainder` added (stdlib [§2.2](Bezel_Standard_Library_0_98.md#sec-math)).** Floating-point remainder operations covering both the C-style (same-sign-as-dividend) and IEEE 754 (nearest-even) forms.
- **`IDisposable` and `IAsyncDisposable` are now declared in stdlib [§2.10](Bezel_Standard_Library_0_98.md#sec-idisposable-and-iasyncdisposable).** Both were referenced from language spec [§28.2](Bezel_Language_Spec_0_98.md#sec-the-idisposable-contract) / [§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal) and from `where T: IDisposable` constraint discussions, but only the inline declarations in the spec described them. The stdlib declaration matches the inline one verbatim. Existing code is unaffected; the addition gives generic constraints and explicit interface variables a documented home.

<a id="sec-v0-64-to-v0-65-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-6"></a>
### Specification-only (no behavioral change)

- **[§6.1](#sec-field-narrowing-is-not-safe-across-threads)** explicitly lists the types on which `mod` is defined (integers and `decimal`) and explicitly rejects floating-point operands. The reasoning — that the choice of remainder semantics for floats is a deliberate decision, not a syntactic afterthought — is stated.
- **[§6.3](Bezel_Language_Spec_0_98.md#sec-floating-point-arithmetic)** cross-references the `Math.Fmod` / `Math.IEEERemainder` alternatives.
- **[§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type)** documents that `Range<T>.Step` is independent of `T`'s representable range; bounds are enforced at iteration time, not at construction. Worked examples for narrow `T`.
- **[§19.2](Bezel_Language_Spec_0_98.md#sec-arms)** acknowledges the `do ... end` expression-position gap with a forward note pointing at block expressions as a candidate future feature, and shows the recommended helper-method workaround pattern.
- **Appendix C.1** adds an explicit note that definite-assignment analysis applies uniformly to instance and static non-nullable reference fields, including across `try`/`catch` paths in static constructor bodies.

<a id="sec-v0-64-to-v0-65-changelog-standard-library"></a>
<a id="sec-standard-library-11"></a>
### Standard library

- **[§2.2](#sec-no-top-level-functions-or-fields) `Math`** gains `Fmod` and `IEEERemainder`.
- **[§2.10](Bezel_Standard_Library_0_98.md#sec-idisposable-and-iasyncdisposable) added: `IDisposable` and `IAsyncDisposable`.** Both interfaces declared in `System` (auto-imported).
- **[§6.4](Bezel_Language_Spec_0_98.md#sec-decimal-arithmetic) `Atomic<T>` and `AtomicInteger<T>`.** Two-class split. `Atomic<T>`'s eligible types widened to include `char`, `IntPtr`, `UIntPtr`. `AtomicInteger<T>` covers only the eight primitive integer types.

<a id="sec-v0-64-to-v0-65-changelog-design-rationale"></a>
<a id="sec-design-rationale-9"></a>
### Design rationale

- [§31](#sec-v0-64-to-v0-65-changelog) (this changelog) added.
- No new deferrals; no items completed beyond what the changelog covers.

<a id="sec-v0-64-to-v0-65-changelog-migration-guide"></a>
<a id="sec-migration-guide-10"></a>
### Migration guide

The changes that affect existing user code are:

1. Any `Atomic<int>` (or other integer `T`) that calls `Increment`, `Decrement`, or `Add` becomes `AtomicInteger<int>`. Find-and-replace on the type name, and the body remains identical.
2. Any `x mod y` with `x: double` or `y: double` (or `float`) becomes `Math.Fmod(x, y)` for C-style semantics, or `Math.IEEERemainder(x, y)` for IEEE 754 semantics. The choice depends on the algorithm; for most application code `Fmod` is the right pick.

No other user-facing changes.

---

<a id="sec-v0-65-to-v0-66-changelog"></a>
### v0.65 to v0.66 changelog

Normative for migration; not part of the language definition.

This revision resolves the long-standing tension between the no-GC promise and the discard form `_ := classExpr;`. Earlier revisions said simultaneously that (1) Bezel has no garbage collector, (2) every class instance must be disposed exactly once, and (3) the runtime "eventually disposes" discarded class instances. Those three statements cannot all hold: the third describes a tracing-with-finalization mechanism, which is what (1) rules out. v0.66 resolves the contradiction by tightening the discard form: it is now legal only when the class is **trivial-dispose** (no destructor, no owned fields, no non-trivial base), and rejected at compile time for everything else. For tasks specifically, `Task.FireAndForget` is the only supported discard pathway.

The change makes the no-GC promise honest: trivial-dispose classes have no destructors to run, so the runtime's memory-reclamation mechanism (refcount, region, or other) for them runs no user code at any observable time. Non-trivial-dispose classes never get to abandon their destructor invocations — every such instance must reach `using var`, an `owned` location, an explicit `.Dispose()`, or (for tasks) `Task.FireAndForget`.

<a id="sec-the-design-space-we-considered"></a>
### The design space we considered

Three options were on the table.

**A) Embrace runtime-managed class instances.** Specify a refcount-or-equivalent mechanism for *all* class instances and let `_ := expr;` mean "the runtime disposes when convenient." Cost: the marketing claim "no GC pauses, no surprise allocations" becomes false in spirit, since something does have to track refcounts and run destructors. Even if it's predictable refcount-based, it's GC-shaped.

**B) Tighten the discard.** Make `_ := classExpr;` legal only for classes whose disposal is a no-op. Cost: a small number of currently-legal patterns become compile errors and need migration.

**C) Status quo with disclaimer.** Document discard-of-class as implementation-defined. Cost: programs become non-portable in their resource-handling behavior.

We chose B. The rationale: a language whose ownership story is "destructors run at known points" cannot afford a discard that hands the value to a refcount-driven cleanup. The destructor would run *somewhere*, on *some* thread, possibly much later — exactly the surprise that `using var` exists to prevent. Better to make the discard mean something narrow and honest.

<a id="sec-what-changed"></a>
### What changed

**Source-breaking:**

- **`_ := classExpr;` is rejected at compile time when the class type is not trivial-dispose.** The compiler error is specific, naming the destructor or owned field that disqualifies the type, and listing the three legal alternatives: `using var _ := …`, an explicit `var name := …; name.Dispose();`, or `Task.FireAndForget(…)` for tasks.
- **`_ := taskExpr;` is rejected.** `Task` has a destructor (handles continuation cleanup, exception propagation), so it is non-trivial-dispose. Programs that previously wrote `_ := DoAsync();` must migrate to `Task.FireAndForget(DoAsync());`. The migration is mechanical; an IDE quick-fix can perform it. `Task.FireAndForget` is now the *only* supported way to discard a Task.
- **`_ := interfaceExpr;` is rejected when the interface extends `IDisposable`.** This includes `IDisposable` itself, `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`, and any user-defined interface that explicitly extends them. For other interfaces, discard is permitted only when the compiler can statically prove the underlying concrete type is trivial-dispose; in practice, this almost never holds, so most interface-typed discards become compile errors.

**Source-compatible:**

- **`_ := newTrivialClass();` continues to work.** Trivial-dispose classes (no destructor, no owned fields, no non-trivial base) accept discard; the standard library types `Random`, `StringBuilder`, and the singleton `Encoding` instances are trivial-dispose.
- **`_ := valueExpr;` continues to work.** Value types, primitives, `string`, and the other runtime-managed types are not affected.
- **`Task.FireAndForget` is unchanged in API surface.** Only its prominence rises: it is now the unique entry point for "discard a task."

<a id="sec-what-this-buys"></a>
### What this buys

- The "no GC" promise is honest. Memory reclamation for trivial-dispose classes happens by a small, predictable mechanism that runs no user code. Memory for non-trivial-dispose classes is reclaimed at the explicit `Dispose()` site, which is the only place destructors can run.
- The model becomes consistent across language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management): classes with non-trivial dispose must reach an owned location, always. Classes with trivial dispose may be discarded freely. Runtime-managed types (`string`, boxed values, function values) are unchanged.
- `Task.FireAndForget` graduates from "recommended pattern" to "the only supported pattern." This removes a footgun: raw `_ := taskExpr;` previously suppressed warnings without supervising exceptions, so a faulted task could fail silently. The new rule forces an explicit choice — supervised (`FireAndForget`) or synchronous wait (`using var`).

<a id="sec-trivial-dispose-status-as-api-contract"></a>
### Trivial-dispose status as API contract

Adding a destructor or `owned` field to a previously-trivial class is now a **source-breaking change**: dependent code that used `_ := new ThatClass();` will fail to compile after the upgrade. API authors should:

- Document trivial-dispose status as part of the published contract.
- Treat adding a destructor as a major-version change (analogous to changing a method signature).
- When the addition is necessary, list affected discard sites and direct callers to `using var`, explicit dispose, or `FireAndForget`.

This is the same migration discipline as v0.64's `CancellationTokenSource(delay)` → `WithCancelAfter(delay)`: a small breaking change that buys a substantially clearer model.

<a id="sec-what-did-not-change"></a>
### What did NOT change

- The runtime is not augmented with reference counting for arbitrary class instances. Only trivial-dispose classes — which run no user code at reclamation — get the implementation-defined memory mechanism.
- `using var`, `owned` fields, `owned` parameters, and `owned` returns are unchanged. The rules for non-discard ownership remain exactly as in v0.65.
- `Task.FireAndForget`'s signature, behavior, and exception-handling semantics are unchanged.

<a id="sec-v0-65-to-v0-66-changelog-migration-guide"></a>
<a id="sec-migration-guide-11"></a>
### Migration guide

User code requires three categories of fix:

1. **`_ := taskExpr;` → `Task.FireAndForget(taskExpr);`** for any `_ := someAsync(...);` or `_ := someTaskExpr;`. This is the most common case. Mechanical replace.

2. **`_ := classExpr;` → `using var _ := classExpr;`** for any non-task class discard where the result was a non-trivial-dispose class (e.g. `_ := new FileStream(path);`). Confirms scope-bounded disposal at the discard site. Some sites may want explicit `.Dispose()` instead of `using var _ :=` if disposal should happen earlier than scope end.

3. **For interface-typed discards extending `IDisposable`,** the same `using var _ := …` workaround applies. The compiler error message names the offending interface explicitly.

Discard sites that targeted trivial-dispose classes (`_ := new Random();`, `_ := new StringBuilder();`, etc.) are unaffected and continue to compile.

<a id="sec-specification-changes"></a>
### Specification changes

- **[§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_)** rewritten. The discard form's legality is now a function of static type: value/runtime-managed/trivial-dispose → permitted; non-trivial-dispose / Task / IDisposable-extending interface → compile error with specific guidance.
- **language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management)'s preamble** describes the implementation-defined memory mechanism for trivial-dispose classes and explicitly states that it is not a garbage collector in the sense Bezel avoids: no scanning, no thread pauses, no user-code invocation.
- **[§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)** promotes the trivial-dispose classification from a warning-severity policy to a hard typing rule. Adds an explicit note that trivial-dispose status is part of an API's public contract.
- **[§30.10](Bezel_Language_Spec_0_98.md#sec-standard-library-task-helpers)** marks `Task.FireAndForget` as the only supported task-discard pathway and lists the two legal patterns explicitly.
- **Appendix C.3** updated with the new discard-rejection rules.

---

<a id="sec-v0-66-to-v0-67-changelog"></a>
### v0.66 to v0.67 changelog

Normative for migration; not part of the language definition.

This revision closes two of the three Tier-4 design questions identified during the v0.63 review. The first — the `Task.FromResult` / `TaskCompletionSource.SetResult` ownership hole for class T — was the highest-impact remaining unresolved issue, since it affected the shape of every async-result-producing API. The second — variance for `IEvent<T>` and `IAsyncEvent<T>` — is small and additive but worth doing alongside the larger change because the design rationale reads more cleanly with both decisions stated together.

<a id="sec-the-task-fromresult-taskcompletionsource-ownership-hole"></a>
### The `Task.FromResult` / `TaskCompletionSource` ownership hole

[§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) specifies that a `Task<T>` for class T owns its result (Pattern A): the task holds a single owned reference, awaiters receive borrows, and disposing the task disposes the result. This is unambiguous when the task is produced by an async method — the `return value;` inside the method body transfers ownership. But for the two factories that construct already-completed tasks — `Task.FromResult<T>(value: T)` and `TaskCompletionSource<T>.SetResult(result: T)` — the parameter shape was a borrow, not an ownership transfer. For class T, this was incoherent: the task is supposed to own the result, but the factory has only a borrow.

The three options on the table were:

**A) Owned overloads.** Add `Task.FromResult<T>(owned value: T): Task<T> where T: class` as an overload of `FromResult`, with the compiler picking based on T's kind. Cost: Bezel doesn't generally do ownership-based overload resolution, and the constraint shape would have to differ between overloads.

**B) Separate methods.** Provide `Task.FromOwnedResult<T>(owned value: T): Task<T>` as a sibling of `FromResult`. Each method has a clear, unambiguous signature; the compiler doesn't need to resolve ownership from context. Cost: one extra method name.

**C) Borrow-with-caller-keeps-alive.** Document that for class T, `FromResult` takes a borrow and the caller is responsible for keeping the value alive while the task is being awaited. Cost: breaks the [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) "task owns the result" model and creates a class-T-only escape hatch from the ownership system.

We chose **B**. The naming pair `FromResult` / `FromOwnedResult` mirrors the language's existing `owned` vocabulary, makes ownership-transfer visible at the call site, and avoids the kind-dependent overload resolution that A would require. C was rejected for the same reason as the v0.66 discard fix: the ownership model is the spec's central commitment, and every ad-hoc exception to it accumulates inconsistency.

The same split applies to `TaskCompletionSource<T>`. We took the consistency-with-v0.65-Atomic route: split into two classes, `TaskCompletionSource<T>` for non-class T and `OwnedTaskCompletionSource<T> where T: class` for class T. The class-level split rather than method-level (one TCS class with two `SetResult` methods) lets the constraint be expressed as a `where` clause, which is how the rest of Bezel's generics work.

<a id="sec-ieventt-and-iasynceventt-covariance"></a>
### `IEvent<T>` and `IAsyncEvent<T>` covariance

The interfaces declared `T` as invariant in v0.66. Variance was deferred from earlier revisions because the interaction between function-parameter contravariance and method-parameter contravariance — composing through `Subscribe(handler: (T) -> ())` — looked like it might force `T` into mixed positions.

It does not. T appears once, inside `(T) -> ()`'s parameter position (contravariant), which is itself a method parameter (input, contravariant). Composition gives covariant. So `IEvent<out T>` and `IAsyncEvent<out T>` are sound, and that is the right declaration: `IEvent<Dog>` becomes a subtype of `IEvent<Animal>`, exactly as `IEnumerable<Dog>` is a subtype of `IEnumerable<Animal>`. This widens what consumers can subscribe to without changing what producers can declare.

`EventSource<T>` and `AsyncEventSource<T>` (the producer-side classes) remain invariant. They expose `Raise(payload: T)` which places T in input position — incompatible with `out`. Since these are visible only inside the declaring class, the user-facing variance comes from `IEvent<out T>` exclusively.

<a id="sec-v0-66-to-v0-67-changelog-what-changed"></a>
<a id="sec-changelog-what-changed"></a>
### What changed

**Source-breaking:**

- **`Task.FromResult<MyClass>(value)` for class T is rejected.** Use `Task.FromOwnedResult<MyClass>(value)` and pass the value with `owned`. Compiler error names the alternative.
- **`new TaskCompletionSource<MyClass>()` for class T is rejected.** Use `new OwnedTaskCompletionSource<MyClass>()`. The `Task` exposed by the new source is `Task<MyClass>` (Pattern A); `SetResult` takes `owned result: MyClass`.

Both changes were unsound under v0.66's letter (the spec said tasks own class-typed results, but the factories couldn't transfer ownership), so any existing code using these forms was relying on undefined behavior. Migration is mechanical.

**Source-compatible:**

- **`Task.FromResult<int>(...)`, `Task.FromResult<string>(...)`, `TaskCompletionSource<int>`, etc. unchanged.** Non-class T users see no difference.
- **`IEvent<T>` and `IAsyncEvent<T>` now declared `out T`.** This widens type relationships (any `IEvent<Dog>` is now usable where `IEvent<Animal>` is expected); existing code is unaffected. New code can take advantage of the variance.

<a id="sec-v0-66-to-v0-67-changelog-specification-changes"></a>
<a id="sec-changelog-specification-changes"></a>
### Specification changes

- **[§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal)** new subsection on constructing completed `Task<T>` for class T, pointing at `FromOwnedResult` and `OwnedTaskCompletionSource`.
- **[§30.10](Bezel_Language_Spec_0_98.md#sec-standard-library-task-helpers)** updates `Task.FromResult` description and adds `Task.FromOwnedResult`.
- **[§30.11](Bezel_Language_Spec_0_98.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)** rewritten: split TCS section covering both `TaskCompletionSource<T>` (non-class T) and `OwnedTaskCompletionSource<T>` (class T), with a worked example for each.
- **[§12](#sec-async)** adds an explanatory paragraph on variance composition with a worked example using `IEvent<out T>`. The composition rule (contravariant-of-contravariant = covariant) was implicit in v0.66's `IEnumerable<out T>` example but never spelled out; this revision makes it explicit so that `out T` on `IEvent` is not a surprise.
- **[§1](#sec-the-big-picture)'s primitive list** updates `IEvent<T>` and `IAsyncEvent<T>` to `IEvent<out T>` and `IAsyncEvent<out T>`.

<a id="sec-v0-66-to-v0-67-changelog-standard-library"></a>
<a id="sec-standard-library-12"></a>
### Standard library

- **[§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) `IEvent<T>` and `IAsyncEvent<T>` now declared `out T`.** Worked explanation of why covariance is sound (and why `EventSource<T>` remains invariant) added.
- **[§6.6](Bezel_Language_Spec_0_98.md#sec-compound-assignments)** split into `TaskCompletionSource<T>` (non-class T) and `OwnedTaskCompletionSource<T>` (class T).
- **[§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) `Task` helpers:** `FromResult<T>(value: T): Task<T>` restricted to non-class T; `FromOwnedResult<T>(owned value: T): Task<T> where T: class` added.

<a id="sec-v0-66-to-v0-67-changelog-migration-guide"></a>
<a id="sec-migration-guide-12"></a>
### Migration guide

Two categories of fix for user code:

1. **`Task.FromResult<MyClass>(value)` → `Task.FromOwnedResult<MyClass>(value)`** wherever a class-typed task is being constructed from an already-known instance. The argument expression must transfer ownership (be an `owned`-returning expression like a `new`, or a value held in an `owned` location that the caller is willing to give up).

2. **`new TaskCompletionSource<MyClass>()` → `new OwnedTaskCompletionSource<MyClass>()`.** Find-and-replace handles the type name. The `SetResult` and `TrySetResult` methods on the new class take `owned result: MyClass` rather than a borrow, so the argument expression must transfer ownership (be a `new` expression, an `owned`-returning call, or a value held in an `owned` location that the caller is willing to give up). The compiler enforces the `owned` argument requirement at every call site.

No source-compatible code requires changes. The variance change is purely a widening.

<a id="sec-what-remains-open"></a>
### What remains open

After v0.67, the remaining material from the original v0.63 review is:

- **Interface vs class-constraint type-kind cleanup** (Tier 5). Partially addressed in v0.64's [§3.2](#sec-the-discard-_) and [§14.2](#sec-mandatory-parentheses-on-mixed-precedence), but the underlying type-kind framework — whether interfaces are a separate kind or simply reference types with extra structure — merits a careful pass.

This is the only structurally significant item remaining. Everything else in the original review has been closed.

---

<a id="sec-v0-67-to-v0-68-changelog"></a>
### v0.67 to v0.68 changelog

Normative for migration; not part of the language definition.

This revision closes the last open item from the original v0.63 review: the interface-vs-class-constraint type-kind cleanup. There are no behavioral changes; the work is making explicit what the rest of the spec already implied, removing one direct contradiction, and generalizing a worked example into a stated rule.

<a id="sec-the-contradiction-we-removed"></a>
### The contradiction we removed

[§3.1](#sec-always-semicolons) listed nine type kinds, including "Interfaces — user-defined contracts implemented by classes and records." [§3.2](#sec-the-discard-_)'s v0.64 wording then said "An interface itself is a contract rather than a separate type kind." These cannot both be true. The [§3.2](#sec-the-discard-_) wording was a v0.64 hedge — added to document that interfaces are reference-typed at the variable level — but it overshot and contradicted [§3.1](#sec-always-semicolons).

The resolution is that interfaces play two roles, and both are real:

- **Interfaces are a type kind.** They are one of the nine forms a type declaration can take; they are listed alongside classes, records, enums, etc. in [§3.1](#sec-always-semicolons).
- **Interface-typed variables are reference-typed.** A variable whose declared type is an interface holds a heap reference, with the standard reference-type assignment semantics.

The two are not in tension. Many type-system designs that include interfaces, traits, or protocols treat them as both a structural concept (what operations does the type provide?) and a value category (what does a variable holding this type contain?). v0.68 makes this explicit in the spec: interfaces are kind-and-reference-type, not kind-vs-reference-type.

<a id="sec-the-general-rule-we-stated"></a>
### The general rule we stated

[§14.2](#sec-mandatory-parentheses-on-mixed-precedence) in v0.66 contained a paragraph specific to `IDisposable`: it described how `where T: IDisposable` admits every class but only `IDisposable`-extending interfaces. The reason is that `IDisposable` has the unusual property of being implemented by every class implicitly. The mechanism described in that paragraph, however, is fully general: for *any* interface `I`, the constraint `where T: I` admits class implementations, record implementations, and interface extensions of `I`.

v0.68 promotes this from "a thing the IDisposable paragraph happens to describe" to a stated **interface conformance rule** with three clauses (one for classes, one for records, one for interfaces). The IDisposable paragraph is preserved as a worked example, alongside a second worked example (`where T: IComparable<int>`) that exercises all three clauses.

<a id="sec-v0-67-to-v0-68-changelog-what-changed"></a>
<a id="sec-what-changed-2"></a>
### What changed

**Specification-only:**

- **[§3.2](#sec-the-discard-_) rewritten.** Each type is exactly one of two storage classes: value type or reference type. The classification is total and disjoint. Interfaces appear in the reference-type list (alongside classes, `string`, arrays, function types). The contradiction with [§3.1](#sec-always-semicolons) is removed: interfaces are both a kind (per [§3.1](#sec-always-semicolons)) and a reference type (per [§3.2](#sec-the-discard-_)). A new paragraph describes the consequences of the partition for assignment, ownership, and generic constraints.
- **[§7.10](Bezel_Language_Spec_0_98.md#sec-generics-and-nullability) updated** to align constraint descriptions with [§14.2](#sec-mandatory-parentheses-on-mixed-precedence)'s expanded story. Adds explicit notes that `where T: SomeClass` admits class types only (no interfaces, since interfaces do not extend classes in Bezel) and that `where T: IFoo` admits class, record, and interface conformances per the general rule.
- **[§14.2](#sec-mandatory-parentheses-on-mixed-precedence) expanded.** The constraint table's entries now spell out which kinds each constraint admits. A new "Interface conformance" subsection states the three-clause rule. The pre-existing IDisposable paragraph is rewritten as a worked example illustrating the rule. A second worked example (`where T: IComparable<int>`) exercises all three clauses with class, record, and interface substitutions.
- **Appendix C.4 expanded** with the full admission rules per constraint kind.
- **Standard library [§2.10](Bezel_Standard_Library_0_98.md#sec-idisposable-and-iasyncdisposable)** points at [§14.2](#sec-mandatory-parentheses-on-mixed-precedence)'s conformance rule for the full description of `where T: IDisposable` admission.

**Source-compatible.** No behavioral change. Existing code that compiled under v0.67 compiles unchanged under v0.68. The work is documentation precision.

<a id="sec-what-we-considered-and-didnt-do"></a>
### What we considered and didn't do

Three alternatives were considered and rejected.

**Demoting interfaces to "non-kind reference types."** This would have removed interfaces from [§3.1](#sec-always-semicolons)'s kind list, treating them only as the contract aspect of types declared with `interface`. The cost: [§3.1](#sec-always-semicolons) then becomes incomplete (you can't tell from the kind list what an `interface` declaration produces), and the structural role of interfaces in the type system goes unstated. Rejected.

**Promoting interfaces to a structurally separate kind, distinct from reference types.** This would have introduced a new kind tier — "interface types" — alongside value and reference types. The type-vs-value classification becomes ternary. The cost: every constraint, every variance rule, every assignment rule has to handle three categories. This is the type-theory-clean answer, but it costs more than it earns; reference-typed interfaces compose cleanly with the reference-type rules already in the spec. Rejected.

**A new constraint kind `where T: interface`.** Some designs separate `where T: class` (class types only) from `where T: interface` (interface types only). The cost: extra surface area in the constraint vocabulary, and every place that currently writes `where T: class` to mean "non-nullable reference type" would have to choose between the two. Rejected; the current `where T: class` covering all reference types matches C# convention and works fine in practice.

The chosen design — interfaces are a kind *and* a reference type, with a stated three-clause conformance rule — preserves the existing constraint vocabulary, removes the [§3.1](#sec-always-semicolons)/[§3.2](#sec-the-discard-_) contradiction, and writes down the general rule that v0.66's IDisposable paragraph had hinted at.

<a id="sec-status-of-the-v0-63-review"></a>
### Status of the v0.63 review

This revision closes the last item from the original v0.63 review.

| Tier | Item | Resolved in |
|---|---|---|
| 1 | Mechanical errata | v0.64 |
| 2 | Local cleanups | v0.64 |
| 3 | mod on float, Atomic split, do…end gap, Range step, IAsyncDisposable | v0.65 |
| 4 | Discard pattern under no-GC | v0.66 |
| 4 | Task.FromResult ownership; IEvent variance | v0.67 |
| 5 | Interface vs class-constraint type-kind cleanup | v0.68 |

The spec is now internally consistent on every issue surfaced in the original review. Further revisions will be driven by implementation experience or by features deferred to future revisions (block expressions, `do`/while postcondition loops, tuples, the deferred standard-library modules, static interface members, full reflection).

---

<a id="sec-v0-68-to-v0-69-changelog"></a>
### v0.68 to v0.69 changelog

Normative for migration; not part of the language definition.

This revision is a fresh-eyes review pass. v0.68 closed every item from the original v0.63 review; v0.69 surfaces what a careful re-read of the now-stable v0.68 spec turned up. The largest item is a precedence change for the range operator (`0..n+1` now parses naturally instead of as a type error); the rest are documentation precision, missing cross-references, and small specification gaps that emerged once the substantive design questions were settled.

<a id="sec-v0-68-to-v0-69-changelog-source-breaking"></a>
<a id="sec-source-breaking-9"></a>
### Source-breaking

- **Range operator precedence lowered.** In v0.68 and earlier, `..` and `..<` bound at level 2 (just below Primary), which made `0..n+1` parse as `(0..n) + 1` — almost always a type error. v0.69 lowers Range to level 14 (between `or` and `if`/`else`), so `0..n+1` parses as `0..(n+1)` and other arithmetic-in-range-bound expressions compose naturally. Programs that were written under the tight-binding rule and relied on `..` binding tighter than arithmetic must add explicit parentheses; in practice no such programs exist that aren't already written defensively, since the tight-binding form rejected the natural arithmetic shapes. Migration: any range with arithmetic in a bound that *was* parenthesized continues to work; any unparenthesized form that previously failed to compile now compiles correctly.

<a id="sec-v0-68-to-v0-69-changelog-source-compatible"></a>
<a id="sec-source-compatible-13"></a>
### Source-compatible

- **`TimeoutException` added** to `System.Threading` (stdlib [§6.1](Bezel_Standard_Library_0_98.md#sec-cancellationtoken-and-cancellationtokensource)). Previously referenced in spec catch examples ([§31.2.2](Bezel_Language_Spec_0_98.md#sec-arm-bodies), [§31.2.3](Bezel_Language_Spec_0_98.md#sec-catch-ordering-and-exhaustiveness)) but never declared. Distinct from `OperationCanceledException`: that one signals cancellation through a `CancellationToken`; `TimeoutException` signals time-budget exhaustion where no token is involved.
- **`owned`, `ref`, and `out` are now permitted in function types and lambda parameter lists** ([§27.1](Bezel_Language_Spec_0_98.md#sec-function-types), [§27.3](Bezel_Language_Spec_0_98.md#sec-lambda-expressions)). The [§30.11](Bezel_Language_Spec_0_98.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet) `OwnedTaskCompletionSource` example required this for its callback parameter; the grammar in v0.68 didn't formally allow it. v0.69 adds the modifiers to the function-type table and the lambda-parameter grammar, with explicit rules: modifiers are part of function-type identity (`(T) -> ()` and `(owned T) -> ()` are distinct), and lambdas may have modifiers inferred from their target type.
- **`IntPtr` and `UIntPtr` formally declared in stdlib [§10.0](Bezel_Standard_Library_0_98.md#sec-intptr-and-uintptr).** Previously referenced as compiler intrinsics (language spec [§4.8](Bezel_Language_Spec_0_98.md#sec-ffi-only-primitive-types), [§32.7](Bezel_Language_Spec_0_98.md#sec-pointers-and-intptr)) without a stdlib home; now have a documented declaration alongside the other FFI-boundary types.

<a id="sec-v0-68-to-v0-69-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-7"></a>
### Specification-only (no behavioral change)

- **[§1.3](Bezel_Language_Spec_0_98.md#sec-identifiers)** corrected: the `String` (PascalCase) identifier is `System.String`, not "hypothetical." The disambiguation between `string` and `String` is by reservation status, not by casing convention.
- **[§2.3](Bezel_Language_Spec_0_98.md#sec-always-available-names)** auto-imported `System` parenthetical updated to include `IDisposable` and `IAsyncDisposable`. (They were added to `System` in v0.65 but the listing was not updated.)
- **[§16.6](Bezel_Language_Spec_0_98.md#sec-empty-ranges-and-equal-bound-ranges)** specifies behavior for equal-bound ranges (inclusive yields exactly the bound regardless of step; half-open is empty regardless of step).
- **[§17.2](#sec-string-concatenation-with-non-strings)** documents the Range-precedence change with worked examples and adds a small note explaining the `mod=` asymmetry (no compound form, by design).
- **[§17.4](#sec-default-interface-methods)** now has substantive content covering the precedence of `is`/`is not`/`as`, the result type of `as` on value types (`T?`), and the ownership-preservation rule for `as`. Previously a near-empty cross-reference.
- **[§22.6](Bezel_Language_Spec_0_98.md#sec-overloading)** documents that `owned` does **not** participate in overload resolution; two overloads differing only in `owned` on a parameter are a compile error. This was implicit in the v0.67 design rationale's discussion of `FromResult` / `FromOwnedResult` but not stated normatively.
- **[§22.7](Bezel_Language_Spec_0_98.md#sec-constructors)** cross-references the [§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors) primary-constructor restriction on `base(...)`.
- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows)** producer table extended: `as` preserves ownership state; closures capture borrows of owned locals (without ownership transfer).
- **[§28.7](Bezel_Language_Spec_0_98.md#sec-reassigning-owned-fields)** acknowledges the exception-unsafe nature of `field.Dispose(); field := new T(...);` and states the convention that disposers should not throw.
- **[§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)** adds an explicit rule: capturing an owned class-typed local in a closure does not transfer ownership; the capture is a borrow whose source is the local's enclosing scope. This is what makes function-typed values runtime-managed without contradicting the no-GC promise.
- **[§28.10](Bezel_Language_Spec_0_98.md#sec-using-var)** clarifies that `using var` accepts value-type and runtime-managed right-hand sides as a no-op, supporting the for-loop desugaring in [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) and similar generic uses.
- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows)** factory-rule warning carved out for iterator methods: they are factories by construction, and adding `owned` to their natural return type would be redundant noise.
- **[§30.11](Bezel_Language_Spec_0_98.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet) / stdlib [§6.6](Bezel_Standard_Library_0_98.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)** specifies what `TrySetResult` does with the `owned result` when it returns `false`: the implementation disposes it.
- **Appendix C.5** corrected: a constructor's chain call is in the chain position before the body, not the first statement of the body.
- **Appendix A.7** grammar updated to reflect the new range precedence: `RangeExpr` moved from between `Multiplicative` and `UnaryExpr` to between `ConditionalExpr` and `NullCoalesce`, and its operands are now `NullCoalesce` expressions rather than `UnaryExpr`.

<a id="sec-v0-68-to-v0-69-changelog-standard-library"></a>
<a id="sec-standard-library-13"></a>
### Standard library

- **`TimeoutException`** added (stdlib [§6.1](Bezel_Standard_Library_0_98.md#sec-cancellationtoken-and-cancellationtokensource)).
- **`IntPtr` and `UIntPtr` formal declarations** added (stdlib [§10.0](Bezel_Standard_Library_0_98.md#sec-intptr-and-uintptr)).
- **`OwnedTaskCompletionSource<T>.TrySetResult`** documentation expanded with the failure-path disposal rule.

<a id="sec-v0-68-to-v0-69-changelog-migration-guide"></a>
<a id="sec-migration-guide-13"></a>
### Migration guide

The only change with potential source impact is the range precedence, and it is a strict widening: every program that compiled under v0.68 continues to compile under v0.69, and additional programs (those using arithmetic in range bounds without explicit parentheses) now compile correctly. No mechanical migration is needed.

The other changes are additive (new declarations) or documentation-only.

<a id="sec-status"></a>
### Status

v0.69 is a polish revision following v0.68's closure of the original review. The spec has now been through a fresh-eyes pass on top of the substantive design work; future revisions will be driven primarily by implementation experience rather than by review-surfaced issues, until a compiler exists to surface new ones.

---

<a id="sec-v0-69-to-v0-70-changelog"></a>
### v0.69 to v0.70 changelog

Normative for migration; not part of the language definition.

This revision is a second fresh-eyes review pass. v0.69 surfaced a batch of issues from a careful re-read of v0.68; v0.70 surfaces another batch from re-reading v0.69. The pattern matches the v0.68 → v0.69 work: real contradictions (a stale [§24](#sec-v0-57-to-v0-58-changelog) paragraph, an inconsistent variance declaration, a wrongly-listed overloadable operator) plus grammar bugs (a typo, a too-strict enum form, a kleene-star-vs-question-mark inconsistency) plus a few documentation precision issues. There are no behavioral changes; every fix is either a documentation correction or a grammar production aligning with the prose specification.

<a id="sec-v0-69-to-v0-70-changelog-source-compatible"></a>
<a id="sec-source-compatible-14"></a>
### Source-compatible

- **Stale [§24](#sec-v0-57-to-v0-58-changelog) paragraph removed.** The line "Events cannot be declared in interfaces in this version. This limitation is acknowledged..." was leftover from before v0.62 added the feature and contradicted [§11.2](#sec-naked-binding-inside-positional-patterns), [§24.7](Bezel_Language_Spec_0_98.md#sec-events-on-interfaces), the example throughout [§11.2](#sec-naked-binding-inside-positional-patterns), and the grammar's `IfaceEvent` production. The replacement points readers at [§24.7](Bezel_Language_Spec_0_98.md#sec-events-on-interfaces).
- **[§24.3](Bezel_Language_Spec_0_98.md#sec-the-type-of-an-event-member) inline declarations of `IEvent<T>` and `IAsyncEvent<T>`** updated to `out T` to match the standard library's declaration ([§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations)) and v0.67's variance-introducing changelog. The [§24.3](Bezel_Language_Spec_0_98.md#sec-the-type-of-an-event-member) prose was correctly describing covariant interfaces; only the inline declarations were stale.
- **[§25.1](Bezel_Language_Spec_0_98.md#sec-overloadable-operators) list of overloadable operators** corrected to remove `not`. [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) has always said `not` is not user-overloadable; [§25.3](Bezel_Language_Spec_0_98.md#sec-examples)'s worked example demonstrates the recommended workaround (`Negate()`); the grammar's `OpToken` does not include `not`. Only [§25.1](Bezel_Language_Spec_0_98.md#sec-overloadable-operators)'s prose listing was inconsistent. The non-overloadable list is also expanded to explicitly include `??`, `is`, `is not`, and `as` — these were unspecified in v0.69 but follow from the language's structural decisions.
- **Appendix A.7 typo** in `TypeAliasDecl`: `";""` corrected to `";"`.
- **Appendix A.3 `EnumDecl` grammar** corrected: the v0.69 form required an integer type to follow the `:` if any inheritance clause was present, but [§10.5](Bezel_Language_Spec_0_98.md#sec-restrictions) explicitly permits `enum E: IComparable<E>` (interfaces only, default underlying type). The corrected grammar accepts either form.
- **Appendix A.3 `ExtensionDecl`** changed from `GenericConstraints?` to `GenericConstraints*`, matching every other declaration in the grammar. An extension over a multi-parameter generic with constraints on each parameter (`extension Foo<A, B> where A: ... where B: ...`) was rejected by the v0.69 grammar; this fix admits it.
- **`ParamModifier` grammar (introduced in v0.69 for function types and lambda parameters)** replaced by `FuncParamMods`, which mirrors the method-declaration `ParamModifiers` exactly — including the `out owned` combination. v0.69's narrower form would have rejected a function type for a method like `method M(out owned r: T)`. The [§27.1](Bezel_Language_Spec_0_98.md#sec-function-types) table and prose are updated accordingly.
- **[§22.11](Bezel_Language_Spec_0_98.md#sec-object-initializer-blocks)** wording on records and object initializer blocks softened to acknowledge that the *syntax* `new RecordType(...) with ... end` is accepted (per [§9.5](#sec-sealed-abstract-classes)); only the *semantics* (mutation) does not apply, with the type-checker resolving such expressions to record-`with` (copy-with-overrides). The v0.69 wording read as if the syntax was rejected on records, which contradicted [§9.5](#sec-sealed-abstract-classes).
- **[§17.12](Bezel_Language_Spec_0_98.md#sec-the-throw-expression)** `throw` example uses `Retry(ex)` rather than `Retry()`, matching other examples in [§31.2](Bezel_Language_Spec_0_98.md#sec-try-catch-finally).

<a id="sec-v0-69-to-v0-70-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-8"></a>
### Specification-only (no behavioral change)

Every item above is a specification-only change. There is no source-breaking impact, no migration required.

<a id="sec-v0-69-to-v0-70-changelog-status"></a>
<a id="sec-changelog-status"></a>
### Status

Two consecutive review-pass revisions (v0.69 and v0.70) on top of the original-review-closing work in v0.68. Each pass surfaced a batch of issues; the v0.70 batch is smaller than v0.69's, suggesting the rate of issue discovery is decreasing. Future revisions will be driven primarily by implementation experience.

---

<a id="sec-v0-70-to-v0-71-changelog"></a>
### v0.70 to v0.71 changelog

Normative for migration; not part of the language definition.

This revision is the third fresh-eyes review pass. It corrects a real regression introduced in v0.69 (the Range-precedence change broke `score in 60..100`), fills a serious grammar gap (`await` had no expression production despite being used throughout [§30](#sec-v0-63-to-v0-64-changelog)), adds four missing entries to the Appendix B keyword list, and clarifies four specification gaps. There are no source-breaking user-facing changes.

<a id="sec-the-range-precedence-rebalance-v0-71s-banner-change"></a>
### The Range-precedence rebalance (v0.71's banner change)

v0.69 lowered Range from level 2 to level 14 — the lowest binary-operator level — to make `0..n+1` parse naturally as `0..(n+1)` rather than as `(0..n)+1`. The change worked for the arithmetic-in-bounds case it targeted, but in retrospect it broke `score in 60..100` and similar `in`-with-range idioms: with `..` looser than `in`, `score in 60..100` parses as `(score in 60) .. 100`, a type error.

v0.71 settles Range at level 6, **between Shift (level 5, tighter) and Bitwise AND (level 7, looser)**. The placement is chosen to satisfy both motivating cases:

- `0..n+1` parses as `0..(n+1)` — Range looser than Additive (level 4) ✓
- `1<<2..1<<10` parses as `(1<<2)..(1<<10)` — Range looser than Shift (level 5) ✓
- `score in 60..100` parses as `score in (60..100)` — Range tighter than Relational (level 10) ✓
- `r = 0..10` parses as `r = (0..10)` — Range tighter than Equality (level 11) ✓
- `0..10 if cond else 20..30` — Range tighter than Conditional (level 15) ✓

The bitwise band (levels 6–9) was the natural home: bitwise-with-range mixes are almost certainly bugs (`mask & 0..10` makes no sense), so the mandatory-parenthesization rules ([§17.2](#sec-string-concatenation-with-non-strings)) reject them rather than letting either parse silently. Range now belongs to that band, with mandatory parens required for any bitwise-range mix.

The grammar in Appendix A.7 is restructured accordingly: `RangeExpr` moves into the chain between `BitwiseAnd` and `Shift`, with its operands parsed as `Shift` expressions.

This is a **specification-only** change — programs that used parenthesized ranges (the only safe form under v0.69 for the broken cases) continue to compile under v0.71. Programs that used `score in 60..100` were rejected by v0.69 anyway (they hit the type error), so no migration is needed; v0.71 just makes them work.

<a id="sec-await-added-to-the-grammar"></a>
### `await` added to the grammar

The grammar in v0.70 had no production for `await Expression`. The expression form was used pervasively in [§30](#sec-v0-63-to-v0-64-changelog) (`var x := await someTask;`) but the formal grammar didn't admit it. v0.71 adds `await` to `UnaryExpr` at unary precedence:

```
UnaryExpr ::= ("+" | "-" | "not" | "~")? PrimaryExpr
            | "await" UnaryExpr
```

The right-recursive form admits `await await x` (rare but valid for `Task<Task<T>>`). `await` is also added to the [§17.1](#sec-truthiness-in-conditions) precedence table at level 2.

<a id="sec-missing-keywords"></a>
### Missing keywords

Four keywords missing from Appendix B's reserved-word list:

- `operator` — used in operator overload declarations (`public static method operator + ...`). Contextual: only meaningful after `method`.
- `get`, `set` — used in property accessor declarations and visibility shorthand. Contextual: only inside property bodies.
- `notnull` — used in generic constraints (`where T: notnull`). Contextual: only after a `where` constraint colon.

All four are added to the contextual-keywords section of Appendix B.

<a id="sec-smaller-fixes"></a>
### Smaller fixes

- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) producer-table entry** for "Local-variable read x where x is owned" clarified: the move-out only happens in owned-consuming positions; in borrow-consuming positions, the read is a borrow and `x` retains ownership.
- **[§29.8](Bezel_Language_Spec_0_98.md#sec-iterators-with-owned-parameters) iterators with `owned` parameters** — clarified the cleanup behavior. Auto-dispose by the iterator state machine runs only for parameters whose ownership has not been moved out by the body. When the body re-binds with `using var`, the local takes over the disposal obligation and the auto-dispose does not run.
- **[§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling) FFI string marshalling** — the v0.70 example used `var resultBytes: CString? := getenv(key);` without disposing, which violates v0.66's discharge rules for non-trivial-dispose classes. The fix introduces a clear distinction at the FFI declaration site: `CString` returns are caller-owned (require `using var`), `IntPtr` returns are borrowed pointers (read through `string.FromCStringPointer(p: IntPtr): string` and not disposed). Native libraries declare each function with whichever return type matches its documented contract. The standard library gains `string.FromCStringPointer` and `string.FromWStringPointer` (stdlib [§10.4](Bezel_Standard_Library_0_98.md#sec-conversions-to-bezel-types)).
- **[§27.6](Bezel_Language_Spec_0_98.md#sec-method-references) method references to overloaded methods** — the v0.70 example `numbers.OrderBy(Math.Abs)` left the overload-resolution rule unspecified. v0.71 adds a subsection: the target function type drives the resolution, with an error and disambiguation guidance when no target type is available.
- **[§31.1](Bezel_Language_Spec_0_98.md#sec-throwing) exception ownership** — added a paragraph on exception lifetime: the runtime owns the in-flight exception; catch arms borrow it for the duration of the arm body; preserving an exception past the arm requires copying out the relevant data rather than retaining the instance.

<a id="sec-v0-70-to-v0-71-changelog-standard-library"></a>
<a id="sec-standard-library-14"></a>
### Standard library

- **`string.FromCStringPointer(p: IntPtr): string` and `string.FromWStringPointer(p: IntPtr): string` added** to [§10.4](Bezel_Standard_Library_0_98.md#sec-conversions-to-bezel-types) of the standard library, supporting the [§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling) FFI distinction between caller-owned strings and borrowed pointers.

<a id="sec-v0-70-to-v0-71-changelog-status"></a>
<a id="sec-status-2"></a>
### Status

After three fresh-eyes review passes (v0.69 → v0.71), the rate of new issue discovery has slowed: v0.69 had 19 items, v0.70 had 11, v0.71 has 11. The remaining issues are increasingly subtle interactions and specification gaps rather than direct contradictions. Future review passes are expected to surface fewer items still; the spec is approaching a fixed point. The next major change driver will be implementation experience.

---

<a id="sec-v0-71-to-v0-72-changelog"></a>
### v0.71 to v0.72 changelog

Normative for migration; not part of the language definition.

This revision is the fourth fresh-eyes review pass. It corrects two factual errors (an Int32 misclassification, a misleading FireAndForget comment), tightens the [§3.1](#sec-always-semicolons) type-kinds section, fills in three specification gaps (boxing of unit, yield ownership for non-class values, enum-cast edge cases), and notes one pre-existing grammar bug that was incidentally fixed by v0.71. There are no source-breaking changes.

<a id="sec-factual-corrections"></a>
### Factual corrections

- **[§14.2](#sec-mandatory-parentheses-on-mixed-precedence) worked example for `where T: IComparable<int>`:** the previous text identified `Int32` as a class, placing it under conformance rule 1. `Int32` has been a record since v0.62 (the primitive-wrapper unification); it conforms under rule 2. The example is updated to list `Int32` correctly and to clarify the rule-by-rule breakdown.
- **Stdlib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) (Task helpers):** the comment on `using var _ := DoTelemetryAsync();` previously read "synchronous wait at scope end," which is incorrect — disposing an unconsumed `Task` does not block ([§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) of the language spec). The revised comment correctly describes the form as "unsupervised; disposed at scope end" and adds a paragraph distinguishing supervised (`FireAndForget`) from unsupervised (`using var`) discard with guidance on when each is appropriate.

<a id="sec-3-1-type-kinds-tightening"></a>
### [§3.1](#sec-always-semicolons) type-kinds tightening

- The "Interfaces" entry previously read "user-defined contracts implemented by classes and records," omitting primitives and the unit type, both of which can underlie an interface variable via boxing ([§3.2](#sec-the-discard-_) already covered this). The entry now mentions all four kinds that can be the underlying value of an interface reference.
- The "Every type derives from `Object`" sentence previously named only primitives, records, enums, and arrays, omitting interfaces and the unit type. The revised wording groups the kinds by how they reach `Object` (via boxing for value-typed instances; directly through the inheritance chain for reference types) and explicitly covers all kinds.

<a id="sec-specification-gaps-filled"></a>
### Specification gaps filled

- **[§8.6](Bezel_Language_Spec_0_98.md#sec-boxing) boxing of the unit value:** added a paragraph noting that all boxings of `()` may share a single heap object, that implementations may but need not intern boxed unit, and that programs should not rely on either interpretation.
- **[§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) (Iterator yield ownership):** the v0.71 text described yield as transferring ownership, which is accurate only for owned class-typed yields. The revised text breaks down yield behavior by the static type of the yielded expression: owned class transfers ownership, borrowed class re-borrows (with the lifetime constraint propagated to the enumerator), and value/string/runtime-managed yields involve no ownership at all. The subsection's organizational placement (it sits at the end of [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) but pertains to [§29.3](Bezel_Language_Spec_0_98.md#sec-iterator-methods)'s `yield` construct) is acknowledged with a parenthetical lead noting the cross-section interaction; a full move would have required renumbering [§29.4](Bezel_Language_Spec_0_98.md#sec-lazy-evaluation)–[§29.13](Bezel_Language_Spec_0_98.md#sec-async-iterators) and was judged not worth the churn.
- **[§22.3](Bezel_Language_Spec_0_98.md#sec-parameter-modifiers) (Parameter modifiers):** added the `out owned` combination, which had been valid in the grammar (`ParamModifiers ::= "ref" | "out" "owned"? | "owned"`) and in function types since v0.70 but was not listed in the prose-level enumeration of modifiers.
- **[§5.4](#sec-no-shared-ownership) (Enum casts):** added two subsections covering cases the previous wording was silent on. The first explains that `int(p)` on an enum with a non-`int` underlying type is treated as a two-step conversion (enum-to-underlying, then underlying-to-`int`), with the integer-conversion's overflow rules applying. The second covers integer-to-enum casts: out-of-range constants are compile errors, out-of-range runtime values raise `OverflowException`, and within-range values that don't correspond to a declared constant produce raw enum values that the consumer must validate against the declared constants.
- **[§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order) (Static initialization):** the previous mention of "Task.Wait()" as a synchronous wrapper was imprecise — `Wait()` returns nothing, so the actual pattern is `task.Wait(); ... task.Result`. The wording now spells out the two-step sequence.
- **Stdlib [§10.4](Bezel_Standard_Library_0_98.md#sec-conversions-to-bezel-types) (`FromCStringPointer` / `FromWStringPointer`):** added a "Behavior at the boundaries" paragraph specifying that null-pointer inputs raise `ArgumentNullException` and that buffers without proper null termination produce undefined behavior. This makes the contract explicit at the FFI boundary.

<a id="sec-incidentally-fixed-in-v0-71"></a>
### Incidentally fixed in v0.71

- **`??` precedence relative to `and` / `or`:** the v0.70 grammar had `RangeExpr → NullCoalesce → LogicalOr → LogicalAnd → BitwiseOr → ...`, which made `??` *looser* than `and`/`or`, contradicting the precedence table (`??` at 11, `and` at 12, `or` at 13) and the [§17.2](#sec-string-concatenation-with-non-strings) note ("`a ?? b or c` parses as `(a ?? b) or c`"). v0.71's grammar restructure replaced this chain with `LogicalOr → LogicalAnd → NullCoalesce → Equality → ...`, which correctly makes `??` tighter than `and`/`or` — agreeing with both the precedence table and the [§17.2](#sec-string-concatenation-with-non-strings) note. The fix was a side effect of the Range-precedence rebalance and was not explicitly noted in v0.71's changelog; this entry records it for completeness.

<a id="sec-acknowledged-but-not-changed"></a>
### Acknowledged but not changed

- The grammar for `RangeExpr` requires a leading expression (`RangeExpr ::= Shift ((".." | "..<") Shift ("step" Shift)?)?`), which means open-ended range patterns documented in [§16.2](#sec-distinct-types-for-cstring-wstring-rawbytes) and [§19.3](Bezel_Language_Spec_0_98.md#sec-patterns) (`..<5`, `5..`) are not admitted by the grammar. This is a long-standing gap from before v0.69 and would require either a dedicated `OpenRangePattern` production in the pattern grammar or a more substantial rework. It is left for a future revision.
- Exception ownership ([§31.1](Bezel_Language_Spec_0_98.md#sec-throwing)) was clarified in v0.71 to specify that catch-arm-bound exceptions are borrows of the in-flight exception. The corollary — that storing an exception past the catch arm requires copying out its data fields — is documented but the design forecloses certain patterns (typed-aggregator, exception-history tracking) that other languages express with shared exception ownership. This is by design under no-GC and is not a defect; it is noted here as a known limitation.

<a id="sec-v0-71-to-v0-72-changelog-status"></a>
<a id="sec-status-3"></a>
### Status

After four fresh-eyes review passes (v0.69 through v0.72), the rate of issue discovery has plateaued at roughly a dozen items per pass, with most items now in the "documentation precision" category rather than substantive defects. The remaining issues are increasingly rare contradictions and edge cases rather than missing core semantics. Future revisions are expected to be driven by implementation experience.

---

<a id="sec-v0-72-to-v0-73-changelog"></a>
### v0.72 to v0.73 changelog

Normative for migration; not part of the language definition.

This revision is the fifth fresh-eyes review pass. The main substantive change is the constructor-order normalization (base before field initializers); the rest are documentation precision, missing cross-references, and one specification gap on what "matching" means for interface implementation.

<a id="sec-substantive-change-base-before-field-initializers"></a>
### Substantive change: base-before-field-initializers

The construction order in [§22.8](Bezel_Language_Spec_0_98.md#sec-field-initializers) has been changed from "field initializers, then base call, then body" to "base call, then field initializers, then body." This brings convention-constructor inheritance into alignment with primary-constructor inheritance ([§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors) has always specified base-first) and with the order used by C#, Java, and C++.

The motivation:

- **Consistency.** [§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors)'s primary-constructor inheritance rule is "base call before field initializers." Convention constructors used the opposite order. Two ways for the same class to construct itself with different orderings is a needless asymmetry.
- **Exception safety simplicity.** Under the old order, a base-call exception would leave the derived instance with already-initialized owned fields but no constructed base subobject; the runtime had to dispose the derived owned fields without invoking the base destructor (which couldn't run, since base wasn't constructed). Under the new order, by the time any derived state exists the base subobject is fully constructed, so unwinding can dispose derived owned fields and then dispose the base subobject cleanly.
- **Programmer expectation.** Every other class-based language with explicit base calls runs base first. Departing from this needs a strong reason; Bezel's previous order had no rationale entry and no apparent design motivation.

The change is essentially source-compatible. [§22.8](Bezel_Language_Spec_0_98.md#sec-field-initializers)'s existing restrictions on field initializers (no `this`, no instance members, no `base`, no virtual methods) mean the only programs whose observable behavior changes are those whose `base(...)` arguments depended on the side effects of a derived field initializer running first — a rare and unidiomatic pattern. The [§22.9](Bezel_Language_Spec_0_98.md#sec-owned-fields-and-exception-safety) exception-safety section is rewritten to describe the new unwinding sequence in terms of the three construction phases.

<a id="sec-other-corrections"></a>
### Other corrections

- **[§30.9](Bezel_Language_Spec_0_98.md#sec-exception-handling-in-async-methods) example:** the `var ex: TransientException => Retry()` example bound `ex` but called `Retry()` with no arguments, inconsistent with the three other `Retry(ex)` uses in the spec. Changed to `Retry(ex)`.
- **[§11.5](Bezel_Language_Spec_0_98.md#sec-modifier-order) modifier order:** `owned` was missing from the order specification. The grammar (`FieldDecl ::= Visibility "static"? "owned"? "readonly"?`) and [§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order)'s `static owned readonly` example require `owned` to sit between the virtual-modifier slot and `readonly`. The [§11.5](Bezel_Language_Spec_0_98.md#sec-modifier-order) enumeration is updated accordingly.
- **[§12.9](Bezel_Language_Spec_0_98.md#sec-variance) and Appendix C.1 variance wording:** both said "Substituting a value type for a variant generic parameter is rejected," which is misleading — what is rejected is the variant *assignment*, not the type itself. `IEnumerable<int>` is a perfectly valid type; it just doesn't covariantly convert to `IEnumerable<Object>`. Both spots are reworded to clarify which side of the rule does the rejecting.
- **[§9.7](Bezel_Language_Spec_0_98.md#sec-records-and-disposal) borrow-bearing record definition:** the previous wording defined "borrow-bearing record" as "a record type that holds at least one reference-typed field," then later said records whose only reference fields are runtime-managed are *not* borrow-bearing. The first definition is wrong if read literally. Combined into a single accurate definition: "at least one reference-typed field of a non-runtime-managed type."
- **[§1.3](Bezel_Language_Spec_0_98.md#sec-identifiers) lexical:** added a cross-reference to [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) noting that the bare identifier `_` has discard meaning in three positions.

<a id="sec-specification-gap-filled-what-matching-means-for-interface-implementation"></a>
### Specification gap filled: what "matching" means for interface implementation

[§11.2](#sec-naked-binding-inside-positional-patterns) said classes implement an interface "by declaring matching public members" without defining "matching." The gap was visible in stdlib [§9.6](Bezel_Standard_Library_0_98.md#sec-string)'s `String: IEquatable<string>` declaration, whose `Equals(other: string?): bool` doesn't strictly match the interface's `Equals(other: T): bool` with T = string (non-nullable).

v0.73 specifies the rule: a member matches an interface member when name, arity, parameter modifiers (`ref`, `out`, `owned`), parameter types, and return type are exactly equal under the interface's substitution, with one allowed widening: the implementation may declare a parameter as `T?` where the interface requires `T`. The reverse direction is rejected. Return types must match exactly (no covariance in this version), and parameter modifiers must match exactly.

The asymmetry mirrors standard contravariant-parameter reasoning — implementations may accept more than they were asked to. The narrow allowance covers the common case (e.g. `Equals(string?)` satisfying `IEquatable<string>`) without opening the door to arbitrary parameter-type widening.

Stdlib [§9.6](Bezel_Standard_Library_0_98.md#sec-string)'s `String.Equals(other: string?)` is annotated to make the rule's application explicit.

<a id="sec-documentation-precision"></a>
### Documentation precision

- **Stdlib [§9.1](Bezel_Standard_Library_0_98.md#sec-integer-types):** primitive-wrapper records like `Int32` declared `IEquatable<int>` but didn't show the `Equals(int): bool` method. Added a comment noting that the method is auto-generated per language spec [§9.2](Bezel_Language_Spec_0_98.md#sec-body-records) and applies to every primitive wrapper.
- **Stdlib [§9.2](Bezel_Standard_Library_0_98.md#sec-double-and-float):** the `Float` analogue was previously described in one sentence ("Float has the analogous shape"). Expanded to enumerate the corresponding Float members so readers don't need to extrapolate from `Double`.
- **Stdlib [§2.5](Bezel_Standard_Library_0_98.md#sec-datetime-timespan-dateonly-timeonly) DateTime:** the comment on the static properties grouped `Now`/`UtcNow`/`Today` (computed each call) with `MinValue`/`MaxValue` (constants) under one explanation that didn't apply to both. Reworded to distinguish the two groups.

<a id="sec-v0-72-to-v0-73-changelog-status"></a>
<a id="sec-status-4"></a>
### Status

After five fresh-eyes review passes, the rate of issue discovery is converging — most remaining items are documentation precision rather than substantive defects. The base-first constructor order is the only substantive change in v0.73 and its source-impact is minimal. The spec is in a stable state suitable for a first implementation pass to begin generating implementation-driven feedback.

---

<a id="sec-v0-73-to-v0-74-changelog"></a>
### v0.73 to v0.74 changelog

Normative for migration; not part of the language definition.

This revision is a holistic Appendix A grammar audit. It corrects six grammar defects and adds clarifying comments to six places where the grammar admits more than the type system accepts. There are no language-level behavioral changes; every program that parsed and type-checked under v0.73 continues to do so under v0.74. Several programs that did *not* parse under v0.73 now do — open-ended range slicing and generic type arguments to `is`/`as` were broken in the v0.73 grammar even though both were documented as supported features.

<a id="sec-six-grammar-defects-fixed"></a>
### Six grammar defects fixed

1. **Generic type arguments to `is` and `as` did not parse.** The v0.73 grammar had `is` and `as` as members of `RelOp`, with `Relational ::= BitwiseOr (RelOp BitwiseOr)*` — meaning the right operand was a BitwiseOr expression. So `someValue as Array<int>` parsed as `someValue as Array < int >` (the `<` becoming a less-than operator on two QualifiedName expressions). v0.74 splits the relational operators into two groups: ordinary relops (`<`, `>`, `<=`, `>=`, `in`) take expression operands as before; type-test relops (`is`, `is not`, `as`) take a `TypeOperand` (a Type, or `null` for the `is null` / `is not null` forms). Programs that used non-generic type tests (`x is Dog`) parse identically under both grammars; programs that used generic type tests (`x is Array<int>`) now parse correctly.

2. **`with` expressions were grammatically ambiguous.** v0.73 had `WithExpr ::= PrimaryExpr "with" ...` as a `PrimaryAtom` alternative. Since `PrimaryAtom` is the base of `PrimaryExpr`, the `WithExpr` production was left-recursive in a way that made `expr.field with X := 5 end` ambiguous between two parse trees. v0.74 moves `with` to a `PrimarySuffix`, alongside `.`, `?.`, `[]`, `()`, and `!`. The form `someExpr with X := 5 end` is now unambiguous: `someExpr` parses as a `PrimaryAtom` followed by zero or more `PrimarySuffix`es, the last of which is the `with` suffix. The standalone `WithExpr` production and the `ObjectInit?` attached to `NewExpr` are removed; `new T() with X := 5 end` now parses as a `NewExpr` followed by a `with` suffix, and the [§9.5](#sec-sealed-abstract-classes) rule that "the difference between record-with and class-new-with is semantic, not syntactic" is now reflected in the grammar itself.

3. **`new()` constraint was a single quoted literal.** v0.73 had `Constraint ::= ... | "new()" | Type`, treating `new()` as one quoted token. Lexically it's three tokens (`new`, `(`, `)`), and the grammar should reflect that. v0.74 splits it into `"new" "(" ")"`. The change permits `new ()` (with whitespace) as well as `new()` per the lexer's whitespace-permissive rules; both are equivalent.

4. **`()` (unit value) vs `"(" ")"` inconsistency.** v0.73 wrote the unit value as `"()"` in `PrimaryAtom` but the unit type as `"(" ")"` in `TypeNoArray`, with the same token sequence written two different ways. v0.74 normalizes both to `"(" ")"` for consistency.

5. **Open-ended range expressions did not parse.** v0.73's `RangeExpr ::= Shift ((".." | "..<") Shift ("step" Shift)?)?` required a leading Shift, so the open-ended forms `..5`, `5..`, `..<5`, and `..` did not parse anywhere — even though [§16.2](#sec-distinct-types-for-cstring-wstring-rawbytes) documented `arr[..5]`, `arr[10..]`, and `arr[..]` as legal slicing forms, and [§19.3](Bezel_Language_Spec_0_98.md#sec-patterns) documented `..<0` and `31..` as pattern-matching forms. v0.74 expands `RangeExpr` to admit five alternatives: an unranged Shift; a closed range (with optional step); a low-only `Shift ".."`; a high-only `(".." | "..<") Shift`; and the fully-open `".."`. `step` is admitted only on the closed form; open-ended ranges have no enumerable step semantics. The new productions integrate cleanly with the existing precedence chain — open-ended ranges work in slicing, in patterns, and as standalone expressions wherever the type system makes sense of them.

6. **`Expression → AssignableExpr` was a misnomer alias.** v0.73 had `Expression ::= AssignableExpr` and `AssignableExpr ::= ConditionalExpr`, an indirection named for an assignment context that doesn't exist in Bezel (assignment is a statement, not an expression). v0.74 drops the alias: `Expression ::= ConditionalExpr` directly.

<a id="sec-six-clarifying-comments-added"></a>
### Six clarifying comments added

The grammar admits more than the type system accepts in several places — this is normal practice for a grammar that prefers readability over expressing every constraint directly. v0.74 adds explanatory comments at the relevant productions so a reader knows that the rejection happens in the type checker rather than at parse time:

- **`Constraint`'s `"new" "(" ")"`** carries a comment noting that the three tokens admit whitespace between them.
- **`MethodBody`'s `";"` body** is annotated as exclusively for abstract methods (interface methods use `IfaceMethod`); the requirement that `VirtualKind = "abstract"` accompanies a body-less `MethodDecl` is enforced semantically.
- **`IndexerDecl`'s block-form `end Identifier?`** is annotated to note that for indexers the only valid form is plain `end;` — the optional Identifier is for property-declarations to repeat the property name.
- **`ClassDecl`** carries a comment noting that the grammar admits modifier combinations (e.g., `static class C(args)`) that the type system rejects.
- **`ArgExpression`'s `_` alternative** is annotated to clarify that `_` is grammatically permitted in any argument position but semantically valid only in `out` positions.
- **`Pattern`'s Type/Constant ambiguity** (a single-Identifier `Dog` could be a type test or a constant pattern) is documented: the grammar does not distinguish them syntactically; resolution is by name lookup.

A separate comment on **`LValue`** notes the pre-existing restriction that LValues cannot have a method-call base — `getter().field := value` does not parse, and the workaround (extract to a local first) is given inline.

<a id="sec-v0-73-to-v0-74-changelog-status"></a>
<a id="sec-status-5"></a>
### Status

Six grammar defects in one pass after five fresh-eyes reviews is a useful reminder that grammar audits are different from feature audits: the latter scans for missing or contradictory rules; the former checks whether the formal productions actually accept the language the prose describes. v0.74 brings the two into alignment.

The grammar is now consistent with the prose in every spot the audit examined. As with all earlier revisions, future implementation work may surface further gaps the audit missed.

---

<a id="sec-v0-74-to-v0-75-changelog"></a>
### v0.74 to v0.75 changelog

Normative for migration; not part of the language definition.

This revision applies four targeted fixes identified in a v0.74 review pass. Three of the four are mechanical and source-compatible; the fourth (`Task<owned T>` becoming a real type) is source-breaking for code that calls `Task.FromOwnedResult` or reads `OwnedTaskCompletionSource<T>.Task`. Migration is straightforward and is described below.

<a id="sec-v0-74-to-v0-75-changelog-source-breaking"></a>
<a id="sec-source-breaking-10"></a>
### Source-breaking

- **`Task.FromOwnedResult<T>` returns `Task<owned T>` instead of `Task<T>`.** [§30.10](Bezel_Language_Spec_0_98.md#sec-standard-library-task-helpers). The factory was always intended to produce a single-consumer (Pattern B) task — its name and the `owned` parameter say so — but its declared return type in v0.74 was `Task<T>`, which under [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal)'s rules made it Pattern A (multi-consumer, awaiters borrow). v0.75 corrects the return type to match the intent.

  Migration: a v0.74 caller that wrote
  ```bezel
  using var task := Task.FromOwnedResult(myInstance);
  var x := await task;          // borrow under Pattern A
  UseX(x);
  ```
  should rewrite to the Pattern B form:
  ```bezel
  using var x := await Task.FromOwnedResult(myInstance);   // ownership moves to x
  UseX(x);
  ```
  The new form is shorter and the variable name corresponds to the actual class instance rather than to the wrapping task. Compile-time errors at the old call sites point to the new shape.

- **`OwnedTaskCompletionSource<T>.Task` is `Task<owned T>` instead of `Task<T>`.** [§30.11](Bezel_Language_Spec_0_98.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet). Same correction as above, for the same reason. The class name "OwnedTCS" already implies an owned-result task; the `Task` property's declared return type now matches.

  Migration consequence: the v0.74 example wrapping a callback API into an async method had a subtle type incoherence — `LoadResourceAsync` declared `Task<Resource>` (Pattern A) but obtained its result from `OwnedTaskCompletionSource<Resource>` (which v0.74 treated as Pattern A but described as taking owned input). Under v0.74's rules the example actually didn't typecheck cleanly: `await tcs.Task` produced a borrow of a value that `using var tcs` was about to dispose. v0.75's example declares `Task<owned Resource>` end to end, the `await` performs a Pattern B move out of the inner task, and the outer `return` moves the same value into the outer task's result slot.

  Migration: a v0.74 method that produced `Task<MyClass>` via `OwnedTaskCompletionSource<MyClass>` and `return await tcs.Task;` should change its return type to `Task<owned MyClass>` and update its caller to the Pattern B `using var x := await ...` shape.

<a id="sec-v0-74-to-v0-75-changelog-source-compatible"></a>
<a id="sec-source-compatible-15"></a>
### Source-compatible

- **`Task<owned T>` is now a real type with grammar support.** language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) (new), Appendix A.5. The v0.74 spec described `Task<owned T>` as a return type and built [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal)'s Pattern A vs Pattern B distinction around it, but the grammar's `GenericArgs` production admitted only plain types — `Task<owned SemaphoreToken>` was parseable only by readers, not by a conforming parser. v0.75 adds `GenericArg ::= "owned"? Type`, with explicit rules:

  - `owned T` and `T` are distinct types as generic arguments.
  - `owned T` is permitted only on opted-in generic parameters; in v0.75, only `Task<T>` opts in.
  - The argument type must be a class type; `Task<owned int>`, `Task<owned MyRecord>`, etc. are compile errors.
  - The receiving type parameter must be invariant.

  These rules are stated in the new [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) of the language spec. The opt-in is per-type because `owned T` changes the receiving type's interaction with values flowing through that slot, and other generic types (collections, streams) have not been designed for owned-element semantics — see the deferred-list expansion below.

  The change is source-compatible with respect to existing type expressions: any v0.74 type that did not contain `owned` inside `<...>` parses identically. v0.74 source that referenced `Task<owned T>` (e.g. `Semaphore.AcquireAsync`'s declared return) was already conforming to a documented spec, so it parses too — for the first time.

- **[§19.3](Bezel_Language_Spec_0_98.md#sec-patterns) destructuring clarified as records-only.** [§19.3](Bezel_Language_Spec_0_98.md#sec-patterns), [§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness). v0.74's prose said "Record destructuring" but used `Circle(r)` as an example, where Circle was a class in the [§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness) exhaustiveness example. v0.75 makes the records-only restriction explicit and reworks [§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness)'s exhaustiveness example to use type-test patterns (`var c: Circle => ...`) instead of destructuring. The [§11.7](Bezel_Language_Spec_0_98.md#sec-static-classes) Shape hierarchy is updated correspondingly: each subclass declares an explicit public property (`Radius`, `Side`, `Base`, `Height`) initialized from its primary-constructor parameter, so the type-test patterns can access fields through the bound name.

  No existing program changes behavior under this clarification — destructuring on classes was never specified to work, and the v0.74 example's `Circle(r)` form would have been rejected by any conforming compiler.

- **[§11.7](Bezel_Language_Spec_0_98.md#sec-static-classes) and [§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness) `decimal` → `double` for geometric examples.** v0.74's Shape hierarchy declared `Area(): decimal` and `Math.Pi * Radius * Radius` — but `Math.Pi` is `double` (std lib [§2.2](Bezel_Standard_Library_0_98.md#sec-math)), and [§6.2](#sec-is-postfix-only) forbids implicit conversion between `decimal` and `double`. The example as written did not type-check. v0.75 changes the geometric types to `double` throughout. The [§9.2](#sec-why-records-are-sealed) Rectangle example, which uses `decimal * decimal` arithmetic without `Math` calls, retains its `decimal` typing — that example was always self-consistent.

<a id="sec-v0-74-to-v0-75-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-9"></a>
### Specification-only (no behavioral change)

- **language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) added: `owned` as a generic argument.** A new subsection of [§14](#sec-operators) codifies the rules for `owned` on generic arguments — opt-in per type, class-only, invariant-only — and explains the per-type opt-in design.
- **Appendix A.5 grammar updated.** `GenericArgs` now references a `GenericArg` production that admits `"owned"? Type`. A clarifying comment names the constraints enforced semantically (opt-in, class-only, invariance).
- **Appendix C.4 and C.6 updated.** New invariants: the `owned`-argument rules in language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) are a generics invariant, and the `Task<owned T>` non-class-T compile error is an async invariant.
- **[§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) "Constructing a completed task with a class-typed result" rewritten.** The subsection's old framing claimed Pattern A applied to `FromOwnedResult` results; v0.75's rewrite states the Pattern B reality and explains why no factory exists for imperative-completion Pattern A class-typed tasks (the case is rare and can be expressed via an async wrapper around `OwnedTaskCompletionSource`).
- **[§30.11](Bezel_Language_Spec_0_98.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet) example rewritten.** The `LoadResourceAsync` example is updated to use `Task<owned Resource>` end to end. The annotation explains how the `await tcs.Task` move composes with the outer `return` to deliver an owned value to the caller.

<a id="sec-specification-only-consistency-fixes-no-behavioral-change"></a>
### Specification-only consistency fixes (no behavioral change)

A second pass after the v0.74 review caught a set of small inconsistencies — wording mismatches, a wrong precedence-level number, two cases where std-lib declarations used invalid Bezel syntax — that don't affect the language design but did make the v0.74 spec internally inconsistent. v0.75 cleans them up:

- **[§17.4](#sec-default-interface-methods): relational precedence level corrected from 9 to 10.** The [§17.1](#sec-truthiness-in-conditions) precedence table lists relational at level 10; [§17.4](#sec-default-interface-methods)'s prose said level 9. The table was always correct; the prose now matches.
- **[§4.6](Bezel_Language_Spec_0_98.md#sec-string): `Range<int>` slicing on strings now documented in the operations list.** v0.74's std lib [§9.6](Bezel_Standard_Library_0_98.md#sec-string) declared `String[r: Range<int>]` as an indexer, but the language spec [§4.6](Bezel_Language_Spec_0_98.md#sec-string) only mentioned `s[i]` for an integer index. v0.75 adds `s[r]` to the [§4.6](Bezel_Language_Spec_0_98.md#sec-string) operations list with a full description of endpoint semantics; std lib [§9.6](Bezel_Standard_Library_0_98.md#sec-string) also corrects the indexer's declaration from `method` to `readonly property`.
- **Std lib [§2.4](Bezel_Standard_Library_0_98.md#sec-environment): closing code fence repositioned.** A prose paragraph about `GetEnvironmentVariables()` was inadvertently inside a ```bezel code block in v0.74. The fence now closes before the prose.
- **Std lib [§10.0](Bezel_Standard_Library_0_98.md#sec-intptr-and-uintptr): `IntPtr` and `UIntPtr` declarations made parseable.** v0.74's declarations used `op_Equality`, `op_Inequality`, and `op_Explicit` — names borrowed from CLR metadata that don't correspond to any Bezel syntax. v0.75 declares the `=` and `<>` operators using the operator-overload syntax ([§25](#sec-v0-58-to-v0-59-changelog)), declares `Zero` as `static const` (matching the convention [§1](#sec-the-big-picture) establishes for compiler-intrinsic constants), and adds prose noting that explicit conversions to and from `long`/`ulong` are compiler intrinsics (language spec [§32.7](Bezel_Language_Spec_0_98.md#sec-pointers-and-intptr)) rather than user-overridable operator declarations. The change is to the declarations' surface only; the runtime semantics described in language spec [§32.7](Bezel_Language_Spec_0_98.md#sec-pointers-and-intptr) are unchanged.
- **Std lib [§9.6](Bezel_Standard_Library_0_98.md#sec-string): `String.FromCStringPointer` and `String.FromWStringPointer` listed.** The std lib [§10.4](Bezel_Standard_Library_0_98.md#sec-conversions-to-bezel-types) (FFI) referenced these factory methods, but the [§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) String declaration didn't list them. The methods are now listed alongside the existing `From(value: CString)` and `From(value: WString)` factories, with a comment distinguishing the owned-instance source from the raw-pointer source.
- **[§29.3](Bezel_Language_Spec_0_98.md#sec-iterator-methods): async iterator definition tightened.** v0.74 said an async iterator "uses both `yield` and `await`," but `await` is permitted-but-not-required: any `async` method declared with an iterator return type and a `yield` is an async iterator, regardless of whether it actually uses `await`. The wording is corrected.
- **Grammar A.7: `step` in pattern position called out.** v0.74's `Pattern ::= ... | RangeExpr` admitted ranges including the `step` modifier in patterns. [§16.4](Bezel_Language_Spec_0_98.md#sec-stride) forbids `step` in patterns semantically. A clarifying comment in the grammar names the rule.
- **[§7.4](#sec-float-equality-is-reflexive): ref-narrowing-invalidation rule tightened.** v0.74 said narrowing was invalidated "if a method that may reassign it through `ref` is called" — too vague (which methods?). v0.75 narrows the rule to "if the variable is itself passed as a `ref` argument inside the controlled block," which is what implementations can actually check.
- **[§1.3](Bezel_Language_Spec_0_98.md#sec-identifiers): `string` keyword vs `String` type wording clarified.** v0.74 said the two were "different tokens to the lexer" but didn't specify which positions admit which form. v0.75 clarifies that the two are interchangeable in source (both name the same type) and that the same dual-form rule applies to other primitive-keyword aliases listed in [§3.4](Bezel_Language_Spec_0_98.md#sec-keyword-aliases-for-built-in-types).
- **[§17.2](#sec-string-concatenation-with-non-strings): `await` precedence given worked examples.** v0.74 had a single sentence about `await` being unary-precedence; v0.75 expands this with five worked examples (`await x.y`, `await x + 1`, `await -x`, `(await x) + 1`, `await await x`) covering the cases that come up in practice.
- **Grammar A.6: `await using var` annotated as async-context-only.** The grammar admits `await using var` at any LocalDecl position; the restriction to `async` methods and lambdas is enforced semantically ([§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal), Appendix C.6). A clarifying comment names the restriction.
- **Appendix B.2: `not` vs `notnull` keyword status clarified.** A new subsection explains that `not` (boolean negation) is unconditionally reserved while `notnull` (the generic constraint) is contextual, and that the `is not` two-token sequence is parsed as a single relational operator without making `not` contextual elsewhere.

<a id="sec-v0-74-to-v0-75-changelog-standard-library"></a>
<a id="sec-standard-library-15"></a>
### Standard library

- **`OwnedTaskCompletionSource<T>.Task: Task<owned T>`.** [§6.6](Bezel_Language_Spec_0_98.md#sec-compound-assignments).
- **`Task.FromOwnedResult<T>(owned value: T): Task<owned T> where T: class`.** [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers).
- **[§10.0](Bezel_Standard_Library_0_98.md#sec-intptr-and-uintptr) `IntPtr` and `UIntPtr` declarations rewritten** to use Bezel operator-overload syntax (`operator =`, `operator <>`) instead of the CLR-metadata names; `Zero` is now `static const` per [§1](#sec-the-big-picture)'s compiler-intrinsic convention; explicit conversions to `long`/`ulong` are documented as compiler intrinsics rather than declared.
- **[§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) `String.FromCStringPointer` and `String.FromWStringPointer` added** to the String class declaration, alongside the existing `From(CString)` and `From(WString)` factories. The pointer-taking forms were referenced in [§10.4](Bezel_Language_Spec_0_98.md#sec-methods-properties-static-members) (FFI) but not listed in [§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) in v0.74.
- **[§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) `String[Range<int>]` indexer corrected** from `method` to `readonly property`.
- **[§2.4](Bezel_Language_Spec_0_98.md#sec-compilation-unit) markdown fix:** the closing code fence for the `Environment` declaration was repositioned so the `GetEnvironmentVariables` prose paragraph renders as prose rather than as code.
- No other library types accept `owned` arguments in v0.75; the std lib is unchanged elsewhere.

<a id="sec-v0-74-to-v0-75-changelog-design-rationale"></a>
<a id="sec-design-rationale-10"></a>
### Design rationale

- **[§12.1](#sec-pattern-a-vs-pattern-b-is-type-directed) / [§12.2](#sec-choosing-between-taskt-and-taskowned-t)** were already written for the type-directed Pattern A vs Pattern B distinction; no changes were needed beyond the language being able to actually express what those sections describe.
- **[§18](#sec-things-deferred) deferred list expanded** with three related cases: `IAsyncEnumerable<owned T>`, `Task.WhenAll` over owned-result tasks, and owned-element collections more broadly. All three sit at the same opt-in surface that v0.75 introduces; adding any of them is a future-revision change to the language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) opt-in list.

<a id="sec-v0-74-to-v0-75-changelog-migration-guide"></a>
<a id="sec-migration-guide-14"></a>
### Migration guide

For most codebases, the only required changes are at call sites of `Task.FromOwnedResult` and at consumers of `OwnedTaskCompletionSource<T>.Task`:

1. **Audit callers of `Task.FromOwnedResult`.** A bare assignment `var t := Task.FromOwnedResult(...)` inferred type `Task<MyClass>` in v0.74 and now infers `Task<owned MyClass>`. The static type changes; consumers that wrote the Pattern A shape (`using var task := ...; var x := await task`) need to switch to Pattern B (`using var x := await ...`).

2. **Audit producers that returned `Task<MyClass>` and used `OwnedTaskCompletionSource<MyClass>` internally.** The producer's return type should change to `Task<owned MyClass>`. Each caller of the producer then switches from the Pattern A consumer shape to the Pattern B shape — same one-line change.

3. **Audit `Task<owned T>` declarations for non-class T.** v0.74 documented this as a compile error but did not enforce it (the type didn't parse anyway). v0.75 enforces it per the language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) rule. If any code declared `Task<owned int>` or similar, it must change.

The geometric-example change in [§11.7](Bezel_Language_Spec_0_98.md#sec-static-classes) / [§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness) affects no real code — those are spec examples, not standard library APIs. The destructuring-on-classes clarification in [§19.3](Bezel_Language_Spec_0_98.md#sec-patterns) / [§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness) likewise affects no conforming code, since the v0.74 form was never specified to work.

---

<a id="sec-v0-75-to-v0-76-changelog"></a>
### v0.75 to v0.76 changelog

Normative for migration; not part of the language definition.

This revision applies twelve targeted fixes from a v0.75 design review. Most are clarifications or pure design choices with no source-level impact; three are mildly source-breaking (the discard rule on interface types, the move of `Subscription` between modules, and the new compile error on `.Equals`/`GetHashCode` calls for non-auto-equality records); none touch the core type system.

The recurring theme is **picking the C# behavior where C# settles a question well, and diverging where Bezel's stricter contracts (no nulls, every class is `IDisposable`, records expect content equality) make C#'s behavior unsafe.** The diverging cases are flagged below.

<a id="sec-v0-75-to-v0-76-changelog-source-breaking"></a>
<a id="sec-source-breaking-11"></a>
### Source-breaking

- **Interface-typed discard `_ := expr;` is now always a compile error.** [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_). v0.75 permitted discard on an interface-typed expression when the compiler could prove the underlying concrete type was trivial-dispose. v0.76 makes the rule unconditional regardless of any visible implementation, on the principle that an implementation in another module — or an implementation added in a future version of any module — may carry a non-trivial destructor. The cheap fix at any rejected discard site is `using var _ := expr;`.

  Migration: search for `_ := expr;` where `expr`'s static type is an interface, and add the `using var ` prefix. The error message at each rejected site names the alternative.

- **`.Equals(Object?)` and `.GetHashCode()` are compile errors on records without auto-equality.** [§9.2](#sec-why-records-are-sealed). v0.75 made `=` and `<>` compile errors for non-auto-equality-bearing body records but left `.Equals(Object?)` (and the `IEquatable<T>.Equals` and `.GetHashCode()` paths) as silent fallthroughs to `Object`-level reference equality. The silent path was a footgun: records are value-shaped, users expect content equality, and the reference-equality fallthrough is virtually never the intended semantics. v0.76 makes all four call sites — `=`, `<>`, `.Equals`, `.GetHashCode` — compile errors on the same set of records.

  Migration: any code calling `.Equals` or `.GetHashCode` on a non-auto-equality-bearing record now fails to compile. The fix is one of: (a) declare custom `=`, `<>`, `Equals`, `GetHashCode` overloads; or (b) simplify the record to an auto-equality-bearing form (auto-properties only, no non-trivial constructor body, no side-effecting computed properties). The error message points to both options.

  Calls through an `Object`-typed receiver (`var o: Object := someRecord; o.Equals(...)`) bypass the static-type check and reach `Object.Equals` at runtime, which gives reference equality on the boxed receiver. This escape hatch is unavoidable but rare in practice.

- **`Subscription` moved from `System.Threading` to `System`.** Std lib [§2.11](Bezel_Standard_Library_0_98.md#sec-subscription) (added), [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) (Subscription declaration removed). The type was always auto-imported (language spec [§2.3](Bezel_Language_Spec_0_98.md#sec-always-available-names) listed it explicitly), but lived in a non-auto-imported declaring module. v0.76 collocates the declaration with the auto-import: `Subscription` is declared directly in `System`. The `import System.Threading;` directive is no longer required to mention `Subscription` outside of the threading-module-internal `EventSource<T>` machinery.

  Migration: code that wrote a `using System.Threading;` solely to access `Subscription` can drop the import (most other System.Threading types — `CancellationTokenSource`, `Mutex`, `EventSource<T>` — still require the import). Code that wrote `System.Threading.Subscription` as a fully-qualified type name must change to `System.Subscription`. The auto-imported short name `Subscription` continues to work.

<a id="sec-v0-75-to-v0-76-changelog-source-compatible"></a>
<a id="sec-source-compatible-16"></a>
### Source-compatible

- **[§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) added: operators on primitive types are compiler intrinsics.** This documents what was already true but unstated: `+`, `-`, `*`, `/`, `mod`, `=`, `<>`, `<`, `<=`, `>`, `>=`, `&`, `|`, `^`, `~`, `<<`, `>>`, `and`, `or`, `not` on the primitive types do not correspond to user-visible `operator` method declarations on the primitive-wrapper records. The language reference now distinguishes intrinsic operators (on primitives) from declared operators (on records and classes via [§25](#sec-v0-58-to-v0-59-changelog)). String's `operator +` remains a real declaration since it was always documented as one — the constant-folding for string-literal operands is a separate compiler optimization.

  No code changes; the [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) addition makes a previously-implicit rule explicit.

- **[§7.9](Bezel_Language_Spec_0_98.md#sec-definite-assignment-for-non-nullable-fields) default-initialization rule clarified.** v0.75 said "fields default to zero" and "non-nullable fields must be assigned in every constructor or have an initializer," which appeared to conflict for non-nullable value-typed fields. v0.76 spells out the three-way split: value-typed fields are implicitly zero-initialized (the must-initialize rule is satisfied vacuously); nullable reference fields are implicitly null-initialized (vacuous again); only non-nullable reference fields require an explicit initializer or constructor assignment. The same rule extends to local-variable declarations.

  No behavioral change — every program that compiled under v0.75 continues to compile, and every program that failed to compile continues to fail. The fix is to the spec's wording.

- **[§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order) forward references in static field initializers are now a compile error.** v0.75 said forward references "observe the field's default value during static initialization," which is C#'s rule. The rule has two problems in Bezel: (a) for non-nullable reference-typed `static readonly` fields, no default value exists, leaving the C# rule undefined; (b) even where a default exists, the C# rule is a footgun — the more common cause of a forward reference is a declaration-order mistake, not a deliberate "I want the default for now" pattern.

  v0.76 rejects forward references at compile time. Cyclic or order-independent static initialization moves into the static constructor, where ordering is explicit. This is a divergence from C# in the strict direction.

  Migration: code that depended on the v0.75 default-value rule is unlikely (the pattern is a footgun more than an idiom). If the compile error fires, refactor either by reordering field declarations or by moving the cyclic logic into a static constructor.

- **[§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order) disposal-under-abnormal-termination clarified.** A new subsection documents that the static-`owned` disposal-at-exit guarantees apply only to graceful shutdown (`Main` returning, `Environment.Exit()`, uncaught exception caught by the runtime). External signals (`kill -9`), runtime panics, native `abort()` from FFI, and hardware faults bypass the disposal sequence. Programs whose correctness depends on disposal-time effects (writing a final log, releasing an external lock recorded outside the process) must use external recovery mechanisms. The same caveat applies to C# `IDisposable` and Java finalizers; documenting it explicitly avoids surprise.

  No behavioral change; spec text only.

- **[§22.13](Bezel_Language_Spec_0_98.md#sec-static-initialization-order) `static readonly` of non-trivial-dispose class type: warning, not error.** The warning was already present in v0.75; v0.76 elaborates the rationale (the singleton-with-resources pattern is widespread and idiomatic; promoting the warning to an error would force ceremony for `static readonly HttpClient`-style cases) and details the two remedies the warning text should suggest.

  No behavioral change.

- **[§17.2](#sec-string-concatenation-with-non-strings) (within [§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type)) `Range<T>` step overflow now covers type-range overflow explicitly.** v0.75 documented that overflow at the *range's upper endpoint* causes silent iteration termination. v0.76 extends the rule to cover overflow of `T`'s representable range — for example, a range over `T = ulong` whose stepping would push past `ulong.MaxValue` terminates silently at the last value that fit. The rule is unconditional silent termination on either condition, whichever happens first. No `OverflowException` is raised in either case. This matches the existing rule's intent.

  No behavioral change for code that didn't already exercise the corner case.

- **[§22.5](Bezel_Language_Spec_0_98.md#sec-optional-parameters-and-named-arguments) default parameter values: call-site evaluation specified.** Bezel chooses **call-site evaluation** for default parameter values: the default expression is re-evaluated each time a call site omits the parameter. This diverges from C# (which requires defaults to be compile-time constants) in the more-expressive direction: defaults like `timeout: TimeSpan = TimeSpan.FromSeconds(30)` and `when: DateTime = DateTime.Now` are well-typed in Bezel and produce fresh values per call. The Python-style "shared-mutable-default" footgun does not arise because Bezel does not support reference-shared default values: owned-class defaults construct fresh per call, and value-type defaults are copied.

  Restrictions: a default-value expression may not reference instance state (`this`, instance fields), local variables, or contain `await`. It may reference earlier parameters of the same parameter list, static methods, and constants. The full rule is in [§22.5](Bezel_Language_Spec_0_98.md#sec-optional-parameters-and-named-arguments).

  v0.75 did not specify the timing; v0.76 commits to call-site. No code changes (no v0.75 program could observe the difference, since default values were undefined in timing).

- **[§9.2](#sec-why-records-are-sealed) record-equality compile error message extended.** The error already pointed to the `=` operator's options; v0.76 extends it to cover the new `.Equals` / `.GetHashCode` compile errors with the same two remedies. Consistent diagnostics across the four entry points.

<a id="sec-v0-75-to-v0-76-changelog-specification-only-no-behavioral-change"></a>
<a id="sec-specification-only-no-behavioral-change-10"></a>
### Specification-only (no behavioral change)

- **[§3.1](#sec-always-semicolons) boxed-unit mention removed from interface description.** v0.75 listed "boxed unit value" as one of the things an interface variable could hold, alongside class instances, boxed records, and boxed primitives. The boxed-unit case is technically possible but rarely meaningful; the interface description shouldn't surface it. v0.76 drops the mention from [§3.1](#sec-always-semicolons) and adds a brief technical note in [§4.7](Bezel_Language_Spec_0_98.md#sec-the-unit-type) (where the unit type is defined) noting that boxing is permitted but the result is rarely useful.

- **Std lib [§6.5](Bezel_Standard_Library_0_98.md#sec-events-ievent-eventsource) `EventSource<T>.Add`/`Remove` declared `public` instead of `internal`.** The visibility restriction is now enforced by the **type fence**: an `EventSource<T>` reference is obtainable only inside the declaring class (the public surface is the `IEvent<T>` interface, which has only `Subscribe`). Making `Add` and `Remove` public on the class surface eliminates the v0.75 compiler-magic rule that special-cased the `+=`/`-=` desugaring to bypass an internal-visibility check. The new design is cleaner: pure type-system enforcement, no compiler magic for visibility.

  User code that smuggles an `EventSource<T>` reference out of the declaring class — by exposing the field through a public method or by reflection — gains the ability to call `Raise` directly. This is a deliberate breach of the event-pattern contract that the language does not attempt to defend against; the conventional design is to never expose the `EventSource<T>` itself.

  No behavioral change for conforming code: `+=` and `-=` work identically, and the external surface (only `Subscribe`) is unchanged.

<a id="sec-v0-75-to-v0-76-changelog-standard-library"></a>
<a id="sec-standard-library-16"></a>
### Standard library

- **[§2.11](Bezel_Standard_Library_0_98.md#sec-subscription) added: `Subscription`.** Type moved from `System.Threading` per the source-breaking section above. The declaration is unchanged; only the module placement differs.
- **[§6.1](#sec-field-narrowing-is-not-safe-across-threads) `CancellationToken.Register` documents synchronous-fire-on-already-cancelled.** Registering a callback on a token that is already in the cancelled state invokes the callback synchronously on the calling thread before `Register` returns; the returned `Subscription`'s `Dispose()` is a no-op. This matches C#'s `CancellationToken.Register`. The rule is the only race-free choice — any policy that schedules-for-later or ignores-the-registration creates a window in which the caller could observe an inconsistent state.
- **[§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) `Subscription` cross-reference added** pointing to [§2.11](Bezel_Standard_Library_0_98.md#sec-subscription).
- **[§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) `Add`/`Remove` declared `public`** per the source-compatible section above.

<a id="sec-v0-75-to-v0-76-changelog-migration-guide"></a>
<a id="sec-migration-guide-15"></a>
### Migration guide

For most codebases, the source-level impact is small:

1. **Audit interface-typed discards.** Search for `_ := expr;` where `expr`'s static type is an interface. Each such site needs `using var _ := expr;` instead.

2. **Audit `.Equals` / `.GetHashCode` calls on records.** Compile errors will surface every site where the record is non-auto-equality-bearing. Each site needs either custom equality declared on the record, or simplification of the record to an auto-equality-bearing form.

3. **Drop or update `import System.Threading` for `Subscription`.** The import is no longer required for `Subscription`; if it was the only reason for the import, drop it. Fully-qualified `System.Threading.Subscription` references must change to `System.Subscription`.

4. **Audit static field initializers for forward references.** Any compile error here points to a declaration ordering issue or an intent to use the static constructor; the fix is mechanical.

5. **No changes needed for** the operator-intrinsic clarification, the default-init clarification, the boxed-unit removal, the abnormal-termination doc, the static-readonly warning rationale, the range-overflow extension, or the EventSource visibility change — these are all documentation or internal-design clarifications that don't surface at user code's call sites.

<a id="sec-v0-75-to-v0-76-changelog-status"></a>
<a id="sec-status-6"></a>
### Status

v0.76 is a consolidation revision. The big design questions (`Task<owned T>` as a real type in v0.75; `owned` as a parameter modifier in earlier revisions) are settled; this revision tightens edges and removes ambiguities. The next revision is expected to address the Batch 2 items from the v0.75 design review (`out` vs `out owned` overload distinguishability; `IEnumerable<T>.GetEnumerator()` returning `owned`; `Mutex.TryAcquire`'s out-parameter contract; user-defined explicit conversions; `owned` on interface return types).

---

<a id="sec-v0-76-to-v0-77-changelog"></a>
### v0.76 to v0.77 changelog

Normative for migration; not part of the language definition.

This revision applies the Batch 2 items from the v0.75 design review — five fixes whose effects propagate through multiple parts of the spec or std lib. Three of them tighten the `owned` system in places where v0.76 left ambiguity; two settle the design of standard `IEnumerable<T>` ownership and the contract of `TryAcquire`-style methods.

The recurring theme of v0.77: **make `owned` annotations as visible to the type system as ordinary type information.** v0.76 had `owned` as a property of values and parameters; v0.77 finalizes how `owned` flows through generic arguments, `out` parameters, interface implementation, and ownership-transferring iterator returns.

<a id="sec-v0-76-to-v0-77-changelog-source-breaking"></a>
<a id="sec-source-breaking-12"></a>
### Source-breaking

- **`IEnumerable<T>.GetEnumerator()` returns `owned IEnumerator<T>`.** Std lib [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic) (interfaces), [§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) (String.GetEnumerator), [§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) (Range.GetEnumerator); language spec [§29.1](Bezel_Language_Spec_0_98.md#sec-the-protocol), [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) (desugaring), [§12.9](Bezel_Language_Spec_0_98.md#sec-variance) (variance example). The same change applies to `IAsyncEnumerable<T>.GetAsyncEnumerator(...)`. v0.76 had these return plain `IEnumerator<T>` / `IAsyncEnumerator<T>`, with the disposal contract carried by the `IDisposable` / `IAsyncDisposable` extension and enforced by the `for` / `await for` desugaring's implicit `using var`. The honor system around manual `GetEnumerator` calls is removed: callers must now write `using var enum := source.GetEnumerator();` or fail to compile. This makes every static type carry the disposal obligation explicitly, matching the rest of the language's "ownership is in the type" model.

  Migration: any user code that calls `GetEnumerator()` outside a `for` loop must wrap the result in `using var`. User-defined classes that implement `IEnumerable<T>` (or that expose a `GetEnumerator()` method matching the duck-typed `for` pattern) must change their declared return type from `IEnumerator<T>` to `owned IEnumerator<T>`. The compile error at each violating site names the fix.

  The `for` and `await for` desugarings already wrap the result in `using var` / `await using var`, so loop forms continue to work without changes.

- **`Mutex.TryAcquire` and `Semaphore.TryAcquire` return `owned T?` instead of taking `out owned T`.** Std lib [§6.2](Bezel_Standard_Library_0_98.md#sec-mutex-and-lock), [§6.3](Bezel_Language_Spec_0_98.md#sec-floating-point-arithmetic). v0.76 declared `TryAcquire(timeout: TimeSpan, out owned guard: Lock): bool`, but the failure path of `TryAcquire` (returning `false`) had no value to assign through the `out owned` parameter. The [§22.3](Bezel_Language_Spec_0_98.md#sec-parameter-modifiers) must-assign rule was unsatisfiable on the failure path. v0.77 replaces the bool-with-out-parameter form with a nullable-result form: `TryAcquire(timeout: TimeSpan): owned Lock?`. The `null` value is the failure signal; the call site narrows by null-test under `using var`.

  Migration: rewrite call sites:

  v0.76:
  ```bezel
  if mutex.TryAcquire(TimeSpan.FromSeconds(5), out using var guard) then
    DoCriticalWork();
  end;
  ```

  v0.77:
  ```bezel
  using var guard := mutex.TryAcquire(TimeSpan.FromSeconds(5));
  if guard <> null then
    DoCriticalWork();
  end;
  ```

  The async variants (`TryAcquireAsync`) follow the same pattern, returning `Task<owned Lock?>`. The Pattern B move ([§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) of the language spec) composes with the nullable result: `using var guard := await mutex.TryAcquireAsync(...);` followed by null-narrowing.

  This change does not propagate to the other `TryX` methods in the std lib (`TryGetValue` on `Dictionary`, `TryPop` on `Stack`, `TryDequeue` on `Queue`, `TryPeek` on `Stack`/`Queue`). Those methods' `out` parameters are not `out owned` — they handle borrows or value types — and their failure-path semantics are well-defined for value types and runtime-managed references. The narrower question of "what should those methods return for non-nullable class T instantiations?" is intentionally left for a future revision; the v0.76 signatures continue to work.

- **`out T` and `out owned T` are distinct overloads.** [§22.6](Bezel_Language_Spec_0_98.md#sec-overloading). v0.76 had a blanket "owned doesn't participate in overload resolution" rule that rejected even `M(out T)` vs `M(out owned T)`. v0.77 carves out the `out` case: because the call-site syntax visibly distinguishes the two forms (`out var x` for plain `out`, `out using var x` for `out owned`), resolution can proceed without ambiguity. Two overloads differing only in the `owned` modifier on an `out` parameter are now permitted.

  Migration: this is source-compatible for any v0.76 program that compiled, since v0.76 outright rejected the overload pair this rule now permits. Code that previously had to use distinct method names (`TryGet` and `TryGetOwned`) can collapse to one method name with two overloads. The rename is optional; the v0.76 distinct-name pattern continues to work.

  The non-`out` form of the rule is unchanged: `M(t: T)` and `M(owned t: T)` remain a compile error, because the call-site form `M(value)` does not visibly distinguish them.

<a id="sec-v0-76-to-v0-77-changelog-source-compatible"></a>
<a id="sec-source-compatible-17"></a>
### Source-compatible

- **[§25.4](Bezel_Language_Spec_0_98.md#sec-no-user-defined-conversion-operators) added: no user-defined conversion operators.** The language has never had syntax for declaring user-defined `implicit` or `explicit` conversion operators on records and classes; v0.77 makes the absence explicit and recommends factory methods (`Money.FromDecimal(d)`) or projection methods (`userDto.ToUser()`) instead. The recommendation matches the rest of Bezel's "make conversions visible" stance — implicit conversions are restricted to lossless built-in widenings ([§5.1](#sec-owned-not-borrows)), and [§6.2](#sec-is-postfix-only) forbids implicit `decimal`/binary-float mixing. C# supports user-defined explicit and implicit conversion operators; Bezel's omission is deliberate.

  No code changes; the [§25.4](Bezel_Language_Spec_0_98.md#sec-no-user-defined-conversion-operators) addition documents what was already true.

- **[§11.2](#sec-naked-binding-inside-positional-patterns) strengthened: `owned` matches exactly across interface implementation.** v0.76 said parameter modifiers `ref`, `out`, `owned` must match exactly between an interface declaration and an implementing method, but didn't address `owned` in other positions (return types, generic arguments, function-type parameters). v0.77 makes the rule uniform: `owned` annotations must match exactly in every position where they may appear. Loosening from `owned T` to `T` in a return type leaks ownership; tightening from `T` to `owned T` in a parameter creates undischarged ownership. Both directions are unsound, and the symmetric "exact match" rule is the only safe one.

  No code changes for conforming v0.76 code: any v0.76 program that compiled cleanly already had matching `owned` annotations (the alternative would have been silently accepting unsound implementations). The strengthened rule documents the fix that v0.76 implementations were already required to perform; it makes the rule explicit in the spec and adds it to the implementation invariants in Appendix C.

<a id="sec-v0-76-to-v0-77-changelog-standard-library"></a>
<a id="sec-standard-library-17"></a>
### Standard library

- **[§4](#sec-memory-management) (Collections) interfaces:** `IEnumerable<T>.GetEnumerator(): owned IEnumerator<T>`; `IAsyncEnumerable<T>.GetAsyncEnumerator(...): owned IAsyncEnumerator<T>`. The language-spec declarations of these interfaces ([§12.9](Bezel_Language_Spec_0_98.md#sec-variance), [§29.1](Bezel_Language_Spec_0_98.md#sec-the-protocol), [§30.7](Bezel_Language_Spec_0_98.md#sec-asynchronous-programming-async-iterators)) are also updated.
- **[§9.6](Bezel_Language_Spec_0_98.md#sec-records-and-inheritance) `String.GetEnumerator(): owned IEnumerator<char>`.** Mirrors the interface change.
- **[§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) (in language spec) `Range<T>.GetEnumerator(): owned IEnumerator<T>`.** Same.
- **[§6.2](#sec-is-postfix-only) `Mutex` redesigned** with `Acquire(): owned Lock`, `TryAcquire(timeout): owned Lock?`, `AcquireAsync(...): Task<owned Lock>`, `TryAcquireAsync(timeout, ...): Task<owned Lock?>`.
- **[§6.3](Bezel_Language_Spec_0_98.md#sec-floating-point-arithmetic) `Semaphore` redesigned** with the same shape applied to `SemaphoreToken`.

<a id="sec-v0-76-to-v0-77-changelog-migration-guide"></a>
<a id="sec-migration-guide-16"></a>
### Migration guide

The most invasive change is the `IEnumerable<T>.GetEnumerator()` ownership shift, which surfaces source-breaking changes wherever user code calls `GetEnumerator()` outside a loop. The two patterns are:

1. **Manual enumeration in user code.** Add `using var` to the call:
   ```bezel
   using var enum := someCollection.GetEnumerator();
   while enum.MoveNext() do ... end;
   ```
2. **User-defined `IEnumerable<T>` implementations.** Update the declared return type:
   ```bezel
   public class MyCollection<T>: IEnumerable<T>
     public method GetEnumerator(): owned IEnumerator<T> => ...
   end MyCollection;
   ```

The `Mutex.TryAcquire` / `Semaphore.TryAcquire` redesigns surface compile errors at every call site. The mechanical fix at each site is the bool-out-parameter-to-nullable-result rewrite shown above.

The interface-implementation `owned`-matching rule ([§11.2](#sec-naked-binding-inside-positional-patterns)) doesn't break correctly-implemented v0.76 code; it tightens the spec and adds the rule to Appendix C.

<a id="sec-v0-76-to-v0-77-changelog-status"></a>
<a id="sec-status-7"></a>
### Status

v0.77 closes the Batch 2 items. The Batch 3 items from the v0.75 design review (range step semantics on `char`; field layout determined by body-position parameter use; trivial-dispose status of generic class instantiations; attribute system as a deferred-list entry) remain for a future revision. None of them are blocking the existing language design; they are clarifications of corner cases that have not yet caused observable inconsistency.

---

<a id="sec-v0-77-to-v0-78-changelog"></a>
### v0.77 to v0.78 changelog

Normative for migration; not part of the language definition.

This revision applies the Batch 3 items from the v0.75 design review. Three are spec clarifications of existing intent — corner cases where v0.77 did not state a rule but the right answer was clear; v0.78 writes the rule down. The fourth (attribute system) joins the deferred list with its rationale spelled out.

The recurring theme of v0.78: **make implicit rules explicit.** Each of the three applied fixes documents what a conforming implementation was already required to do; v0.78 adds the prose that a reader needs to verify the implementation is correct.

<a id="sec-v0-77-to-v0-78-changelog-source-compatible"></a>
<a id="sec-source-compatible-18"></a>
### Source-compatible

- **[§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) surrogate-boundary semantics for `Range<char>`.** v0.77 had a general silent-termination rule for `Range<T>` iteration when the next computed value would overflow `T`'s representable range, but the rule referred only to numeric overflow. For `T = char`, `T`'s representable range is non-contiguous: code points U+D800 through U+DFFF are surrogates and not valid `char` values ([§4.5](Bezel_Language_Spec_0_98.md#sec-char)). v0.78 extends the silent-termination rule to cover the surrogate boundary: iteration of `Range<char>` terminates when the next computed code point would be a surrogate, exactly as it terminates on numeric overflow.

  Concretely, `for var c in '\u{0000}'..'\u{10FFFF}' do ... end` yields code points U+0000 through U+D7FF and stops; iteration does not skip across the surrogate gap. Code that needs the full Unicode code point space iterates two ranges (the lower and upper segments). The `Contains` operation continues to use the integer-membership rule ([§17.5](#sec-member-hiding)) — `'\u{D800}' in r` may return `true` if the range's integer extent covers it — but iteration of `r` will not yield surrogate values. The asymmetry is deliberate: `Contains` answers a mathematical extent question; iteration produces a sequence of valid `char` values.

  No code changes for any program that didn't already exercise the corner case (which was undefined under v0.77). Programs that did rely on the corner case were doing so without a normative basis; the v0.78 rule brings the spec in line with what a conforming implementation must do.

- **[§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors) capture rule and layout stability documented.** v0.77's primary-constructor rule said primary-constructor parameters are captured as private fields when used in body positions, but did not address the layout-stability consequence: adding or removing a body reference to a parameter changes whether it is captured. v0.78 documents this explicitly with the **capture-iff-used** rule, calls out the consequence for ABI stability and reflection, and provides three remedies for code that needs stable field layout (explicit field declarations, public properties, non-primary constructors).

  Bezel's capture-iff-used rule matches C# 12's primary-constructor design. Both languages treat primary constructors as ergonomic shorthand for short-lived data-carrier classes where ABI stability is a non-concern. Code writing a primary constructor implicitly opts into "the compiler chooses my field layout based on body content"; code needing layout guarantees uses explicit declarations.

  No behavioral change. The v0.78 prose documents the rule that conforming implementations were already required to apply; programs are unaffected. The text adds the layout-stability caveat that v0.77 readers might have been surprised to discover.

- **[§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) per-instantiation trivial-dispose for generics.** v0.77 defined "trivial-dispose" for non-generic classes but left the generic-instantiation case to inference. v0.78 makes the rule explicit: for a generic class `C<T>`, the trivial-dispose check is performed on the **instantiation** rather than on the generic declaration. The compiler substitutes the actual type arguments and applies the rules to the resulting concrete class.

  The practical consequence: standard collection types (`Array<T>`, `Dictionary<K, V>`, `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`) are trivial-dispose for every choice of type argument, because they do not own their elements ([§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements)). `Array<MyClass>` is trivial-dispose even when `MyClass` is not. By contrast, a user-defined `OwnedBox<T> where T: class` with an `owned T` field is non-trivial-dispose for any T, regardless of T's own properties.

  The rule clarifies what the discard rule ([§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_)) and the relaxed-discharge rules ([§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)) check: they check the instantiation's trivial-dispose status, not the generic declaration's. v0.77 implementations would have computed this correctly anyway; v0.78 writes the rule down.

  No source-level change for any program that compiled under v0.77.

<a id="sec-deferred"></a>
### Deferred

- **Attribute / annotation system added to [§18](#sec-things-deferred) deferred list.** A C#-style `[Attribute]` declaration syntax with attribute classes derived from a base type and reflective query at runtime. Useful for serialization metadata, framework integration, AOT hints, deprecation markers, validation rules, and dependency-injection registrations. Not in this version because most attribute *uses* depend on machinery that is itself deferred — reflection beyond `typeof`/`GetType`, serialization frameworks, dependency injection, test frameworks. Designing attributes before designing how they are consumed would lock in syntax for unbuilt features.

  The deferred-list entry calls out two narrowly-useful capabilities that could land before the full attribute system if user demand emerges: an `[Obsolete]`-style compile-time deprecation modifier (no runtime reflection required) and method-implementation hints (`[MethodImpl]`-style). Both are achievable as compiler-recognized declaration modifiers without a full attribute system.

  The entry also documents the costs of C#'s attribute system that motivate Bezel's careful deferral: attribute proliferation in real-world codebases, stringly-typed parameter handling in reflection-based machinery, and the blurred line between language semantics, framework machinery, and inert metadata.

<a id="sec-v0-77-to-v0-78-changelog-migration-guide"></a>
<a id="sec-migration-guide-17"></a>
### Migration guide

There is no migration. All three applied fixes are clarifications of behavior that conforming v0.77 implementations were already required to provide; programs that compiled under v0.77 continue to compile under v0.78 with identical semantics. The deferred attribute-system entry adds no syntax and removes none.

The most useful change for code authors is **[§22.10](Bezel_Language_Spec_0_98.md#sec-methods-and-constructors-primary-constructors)'s layout-stability note**: programs that need stable primary-constructor field layout should switch to explicit field declarations now, before reflective access mechanisms exist that would expose the implicit rule.

<a id="sec-v0-77-to-v0-78-changelog-status"></a>
<a id="sec-status-8"></a>
### Status

v0.78 closes Batch 3, which was the last open batch from the v0.75 design review. The language design is now stable through the major decisions catalogued in that review:

- v0.75 made `Task<owned T>` a real type and fixed the related factory return types.
- v0.76 applied 12 mechanical clarifications and small-design choices (Batch 1).
- v0.77 propagated `owned` through interface implementation, generic-argument positions, and the standard `IEnumerable`/`Mutex` interfaces (Batch 2).
- v0.78 documented the remaining corner-case rules and added the attribute-system deferral (Batch 3).

The next revision will likely focus on items that surface during early implementation work (the language has no implementation yet) or on the deferred-list items as user demand for them emerges.

---

<a id="sec-v0-78-to-v0-79-changelog"></a>
### v0.78 to v0.79 changelog

Normative for migration; not part of the language definition.

This revision is a fresh-look pass over v0.78. Seven issues surfaced; six were applied as fixes and one as a clarifying paragraph. Several of the bugs predate the recent batches — notably the grammar's missing `owned` modifier on method return types, which had been quietly inconsistent with std-lib usage for many revisions.

The recurring theme of v0.79: **make pervasive usage match grammar.** Two of the six fixes (A1 default-parameter syntax, A2 `owned` return-type modifier) brought the spec's grammar productions into line with what user code and the std lib have been writing all along. The other four are smaller — a stale version reference, a broken cross-reference, a syntax bug in a v0.78 example, and a reconciliation of [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types)'s framing with [§25.2](Bezel_Language_Spec_0_98.md#sec-operator-overloading-restrictions)'s class-equality rule.

<a id="sec-v0-78-to-v0-79-changelog-source-compatible"></a>
<a id="sec-source-compatible-19"></a>
### Source-compatible

All six fixes are source-compatible. The grammar fixes (A1, A2) document existing usage; the others are localized clarifications.

- **[§22.5](Bezel_Language_Spec_0_98.md#sec-optional-parameters-and-named-arguments) default-parameter syntax: `=` → `:=`.** v0.74 introduced default parameter values with prose using `=` (`level: string = "INFO"`), but Appendix A's grammar has always required `:=` (`Param ::= ... (":=" Expression)?`). The std lib writes `:=` consistently. v0.79 fixes the [§22.5](Bezel_Language_Spec_0_98.md#sec-optional-parameters-and-named-arguments) prose and examples to match the grammar. No code-affecting change — implementations have been parsing `:=` all along, and the spec's `=` examples were a typo.

- **Grammar: `MethodDecl`, `IfaceMethod`, `ExternDecl`, `OperatorDecl` admit `owned` on the return type.** The std lib uses `owned T` as a return type in 35+ places (`Acquire(): owned Lock`, `Subscribe(): owned Subscription`, `ToArray(): owned T[]`, `From(...): owned CString`); the spec uses it in 14 places. But v0.78's grammar productions had `(":" Type)?` — no `"owned"?` modifier. v0.79 adds `"owned"?` to each of the four method-form productions, with explanatory comments noting that `owned` applies to class-typed returns and is rejected semantically for other types.

  No behavioral change. Implementations have been parsing `owned T` as a return-type modifier all along; the grammar productions now reflect the syntax that has been used consistently.

- **language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument): stale "v0.75" reference replaced.** A sentence in language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) said "none of these accept `owned` arguments in v0.75" — a direct dating from when the rule was introduced. v0.79 replaces "v0.75" with "this version," matching the version-neutral phrasing used elsewhere in the spec.

- **[§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_): broken §28.6.4 cross-reference fixed.** The discard-rule prose in [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) referenced §28.6.4, but [§28.7](Bezel_Language_Spec_0_98.md#sec-reassigning-owned-fields) is "Reassigning owned fields" with no §28.6.4 subsection. The intended target was [§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)'s "Trivial-dispose classification is part of the API surface" subsection, which discusses adding a destructor as a source-breaking change. v0.79 redirects the reference accordingly.

- **[§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types): OwnedBox example field syntax corrected.** The OwnedBox example added in v0.78 used C-style field-declaration syntax (`public T Value;`) instead of Bezel's `name: type` form. v0.79 rewrites the example to use the proper syntax (`public Value: T;`, `public owned Value: T;`) per `FieldDecl ::= Visibility "static"? "owned"? "readonly"? Identifier ":" Type ...`.

- **[§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types): framing of `string` operators reconciled with [§25.2](Bezel_Language_Spec_0_98.md#sec-operator-overloading-restrictions).** v0.76's [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) introduced compiler-intrinsic operators on primitives, listing `string`'s comparison operators (`=`, `<>`, `<`, `<=`, `>`, `>=`) among them but then claiming "operators on non-primitive types (records, classes) are regular `operator` method declarations" — and `string` is non-primitive per [§3.1](#sec-always-semicolons). v0.79 adds a paragraph reconciling the two: `string` straddles the categories because [§25.2](Bezel_Language_Spec_0_98.md#sec-operator-overloading-restrictions) forbids classes from overloading `=`/`<>` (so the compiler must implement them intrinsically for any class with content equality, including `string`), while `string`'s `+` is a regular declared overload. The asymmetry follows from [§25.2](Bezel_Language_Spec_0_98.md#sec-operator-overloading-restrictions), not from a categorical claim about `string` being "primitive."

<a id="sec-areas-reviewed-but-unchanged"></a>
### Areas reviewed but unchanged

The fresh-look review covered [§17](#sec-things-explicitly-rejected) expression precedence, [§29](#sec-v0-62-to-v0-63-changelog) iteration, [§30](#sec-v0-63-to-v0-64-changelog) async, [§31](#sec-v0-64-to-v0-65-changelog) exception handling, [§32](#sec-v0-65-to-v0-66-changelog) FFI, and the std lib's collection types. No issues warranting v0.79 fixes were found. Two narrower observations were noted for future revisions but not applied:

- **`Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>` documentation gap.** Dictionary's std-lib declaration shows the `Add(key: K, value: V)` form but not the `Add(item: DictionaryEntry<K, V>)` form required by the interface contract. The accompanying prose says these "inherit from the interface" — wording that conflates interface declaration with implementation. A future std-lib revision should either declare the entry-typed members explicitly or reword the prose to say "the std lib provides them implicitly."

- **§28.2.5 numbering oddity.** language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management)'s subsections run 28.1, 28.1.5, 28.2, 28.3, ..., 28.10. The 28.1.5 numbering is unusual — most of the spec uses simple X.Y numbering — and reads as a leftover from a mid-revision insertion. It is referenced 11 times, so a renumbering would be modestly invasive; left for a future cleanup pass.

<a id="sec-v0-78-to-v0-79-changelog-migration-guide"></a>
<a id="sec-migration-guide-18"></a>
### Migration guide

There is no migration. Six of the six fixes are spec/std-lib adjustments that bring documentation into line with the language as already implemented (or as already used by the std lib). User code that compiled under v0.78 continues to compile under v0.79 with identical semantics.

The most useful change for a code reader is the **A2 grammar fix**: a reader following the v0.78 grammar would have concluded that `Acquire(): owned Lock` is illegal syntax, contradicting decades of std-lib examples. v0.79's grammar matches what the std lib does, and a reader can now check the grammar against any std-lib declaration without surprises.

<a id="sec-v0-78-to-v0-79-changelog-status"></a>
<a id="sec-status-9"></a>
### Status

v0.79 is a consolidation revision. The design is stable through every major decision catalogued in the original review and through the v0.78 closing of Batch 3. Remaining open items are the deferred-list entries in [§18](#sec-things-deferred) (the attribute system, owned generic arguments beyond `Task<T>`, non-owning views, etc.) and any items that surface during eventual implementation work.

---

<a id="sec-v0-79-to-v0-80-changelog"></a>
### v0.79 to v0.80 changelog

Normative for migration; not part of the language definition.

This revision closes the two follow-up items identified at the end of v0.79 — both organizational rather than design-level, neither affecting language semantics. The std lib gains a new convention paragraph clarifying how interface-required members are documented; language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management)'s subsection numbering is normalized.

<a id="sec-v0-79-to-v0-80-changelog-source-compatible"></a>
<a id="sec-source-compatible-20"></a>
### Source-compatible

- **language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management)'s subsections renumbered.** v0.79 left language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) with an unusual numbering scheme — `§28.2`, `§28.2.5`, `§28.3`, `§28.4`, ..., `§28.11` — that read as a leftover from a mid-revision insertion of what is now the borrows-and-ownership section. v0.80 normalizes the numbering: the section formerly known as §28.2.5 becomes [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows), and [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) through [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements) each shift up by one, becoming [§28.4](Bezel_Language_Spec_0_98.md#sec-the-destructor) through [§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal).

  The mapping:

  | v0.79 number | v0.80 number | Section title |
  |---|---|---|
  | [§28.2](Bezel_Language_Spec_0_98.md#sec-the-idisposable-contract) | [§28.2](Bezel_Language_Spec_0_98.md#sec-the-idisposable-contract) | The `IDisposable` contract |
  | §28.2.5 | [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) | Local variables, ownership, and borrows |
  | [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) | [§28.4](Bezel_Language_Spec_0_98.md#sec-the-destructor) | The `destructor` |
  | [§28.4](Bezel_Language_Spec_0_98.md#sec-the-destructor) | [§28.5](Bezel_Language_Spec_0_98.md#sec-owned-fields) | Owned fields |
  | [§28.5](Bezel_Language_Spec_0_98.md#sec-owned-fields) | [§28.6](Bezel_Language_Spec_0_98.md#sec-leak-prevention) | Leak prevention |
  | [§28.6](Bezel_Language_Spec_0_98.md#sec-leak-prevention) | [§28.7](Bezel_Language_Spec_0_98.md#sec-reassigning-owned-fields) | Reassigning owned fields |
  | [§28.7](Bezel_Language_Spec_0_98.md#sec-reassigning-owned-fields) | [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) | Ownership transfer |
  | [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) | [§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) | Runtime-managed types |
  | [§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) | [§28.10](Bezel_Language_Spec_0_98.md#sec-using-var) | `using var` |
  | [§28.10](Bezel_Language_Spec_0_98.md#sec-using-var) | [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements) | Collections do not own elements |
  | [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements) | [§28.12](Bezel_Language_Spec_0_98.md#sec-asynchronous-disposal) | Asynchronous disposal |

  Every cross-reference in the language spec, the standard library, and the design rationale was rewritten by an automated pass against the mapping above. The changelog entries from earlier revisions also reference language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) sections, and those references were rewritten too — the convention used throughout the rationale's changelogs is "all references use current numbering," so a reader of v0.80 sees consistent numbers regardless of which historical change they're reading about.

  The single intentional exception: the v0.79 changelog's bullet describing the §28.2.5 numbering oddity preserves the literal historical reference (`§28.2.5`) so the bullet remains accurate as a record of what v0.79 observed.

  No code changes. The renumbering is documentation-only; the rules and their semantics are unchanged.

- **Std lib [§1](Bezel_Standard_Library_0_98.md#sec-conventions): convention added for elided interface-required members.** The std lib has long used two patterns for documenting interface conformance: some classes (like `Array<T>`) show every interface-required member explicitly; others (like `Set<T>` and `LinkedList<T>`) elide redundant declarations with a comment such as `// plus standard ICollection<T> operations`. v0.79's review noted that the elision convention was implicit and that `Dictionary<K, V>`'s prose conflated "implements" with "inherits."

  v0.80 adds a normative bullet to [§1](Bezel_Standard_Library_0_98.md#sec-conventions) of the std lib stating the convention: when a class declares an interface, the members required by that interface are part of the public contract whether the declaration shows them explicitly or elides them. The convention covers the asymmetric case where the interface's substituted member name collides with another method on the class — `Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>` is the leading example, where `Add(item: DictionaryEntry<K, V>)` is required by the interface and coexists with `Dictionary`'s own `Add(key: K, value: V)`.

- **Std lib [§4.4](Bezel_Standard_Library_0_98.md#sec-dictionaryk-v) `Dictionary` prose corrected.** The phrase "it also inherits `Add(item: DictionaryEntry<K, V>)`, `Remove(item: DictionaryEntry<K, V>): bool`, and `Contains(item: DictionaryEntry<K, V>): bool` from the interface" was misleading — Bezel interfaces have no implementations, so a class doesn't "inherit" from them. v0.80 reworks the sentence to say that Dictionary "provides" these members "to satisfy the interface contract" and cross-references the new [§1](#sec-the-big-picture) convention. The behavioral content is unchanged; the wording now matches [§11.2](#sec-naked-binding-inside-positional-patterns)'s terminology.

<a id="sec-v0-79-to-v0-80-changelog-migration-guide"></a>
<a id="sec-migration-guide-19"></a>
### Migration guide

There is no migration. The renumbering rewrites cross-references in the documents themselves; user code that referenced section numbers in comments or documentation should be updated to the new numbers, but the compiler does not see section numbers and there is no build-time impact.

For projects that rely on the std lib's documented surface, the Dictionary prose change clarifies what was already true under v0.77's [§11.2](#sec-naked-binding-inside-positional-patterns) rule: Dictionary's interface-required entry-typed members are part of the public contract. The new [§1](#sec-the-big-picture) convention removes ambiguity for any future std-lib type that uses elision.

<a id="sec-v0-79-to-v0-80-changelog-status"></a>
<a id="sec-status-10"></a>
### Status

v0.80 closes the two follow-up items from v0.79's review. The next revision is expected to be the cross-reference reorganization mentioned at the end of the v0.79 review — adding stable anchor labels to the most-referenced rules and a small link-checker — but that is an organizational change, not a design or content change, and the timing depends on when the document set's growth makes the prose-only references brittle enough to justify the migration.

---

<a id="sec-v0-80-to-v0-81-changelog"></a>
### v0.80 to v0.81 changelog

Normative for migration; not part of the language definition.

This revision is the cross-reference reorganization promised at the end of v0.80. It is a tooling-and-format change with no design or content modifications.

<a id="sec-v0-80-to-v0-81-changelog-what-changed"></a>
<a id="sec-what-changed-3"></a>
### What changed

Every numbered section heading in the three documents now has an HTML anchor tag immediately preceding it, and every prose §X.Y reference is now a Markdown link pointing to that anchor. The change is mechanical and uniform:

- **Anchors** added before each `## N.`, `### N.M`, `#### N.M.K`, `### A.X`, and `## Appendix X` heading. Format: HTML anchor tags with slugs of the form `sec-X-Y` for numeric subsections, `sec-appendix-A` for appendix top-levels, `sec-contents` for the table of contents. Total: 313 anchors in the spec, 60 in the std lib, 106 in the design rationale.

- **References** of the form §X.Y are now Markdown links wrapping the same `§X.Y` text. The link text preserves the section number so a reader sees the same prose as before; only the underlying anchor target changed. Internal references link to a fragment in the same file; cross-document references link to a fragment in the appropriate companion document by filename. Total: 512 internal links and 507 cross-document links across the three documents.

- **Disambiguation** of cross-document references uses both forward and reverse phrasings:
  - Forward form like "language spec §X.Y" links to spec
  - Reverse form like "§X.Y of the language spec" links to spec
  - Chained variants like "§A and §B of the language spec" correctly link both A and B to spec
  - Same logic for "standard library" and "design rationale" aliases
  - Unprefixed references with no matching section in the source document fall back to spec (the dominant case for the std lib's references to language rules and for the design rationale's references to spec sections)

- **A few historical literal references stay as plain text** because they describe text that was once in the document but no longer corresponds to a section. The v0.79 changelog's mention of "broken §28.6.4 cross-reference" is preserved as text — §28.6.4 never existed and shouldn't link anywhere. Similarly, §28.2.5 references in the v0.79 and v0.80 changelogs that describe pre-renumbering history are preserved as text.

<a id="sec-what-didnt-change"></a>
### What didn't change

- **Section numbers in link text.** This revision keeps section numbers visible in the rendered prose (a link displays as "[§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer)"). A future revision could replace the numeric link text with semantic descriptions, but doing that mechanically requires choosing per-section descriptions that read naturally in every context the section is referenced from. Step 1 (this revision) and step 2 (semantic link text) are separated so the link plumbing can be verified independently of the prose changes.

- **Slug format.** Slugs are derived from current section numbers (`sec-28-7` for [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer)). A future renumbering would invalidate them. Step 2 of the cross-reference work would also rename slugs to semantic identifiers (`sec-trivial-dispose-runtime-types`) so they survive renumbering. For now, slugs and section numbers are coupled — but the renumbering script for language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) used in v0.80 demonstrates that mass updates remain mechanically tractable.

- **TOC entries.** The table of contents in each document still uses GitHub-style auto-generated heading anchors (`#28-resource-management` rather than `#sec-28`). Both anchor systems coexist; the auto-anchors continue to work because they're derived from the heading text at render time. Updating the TOC entries to use the explicit `sec-N` slugs is straightforward but was not done in this revision — leaving the TOC as-is keeps it visually identical to v0.80.

<a id="sec-tooling"></a>
### Tooling

Two scripts were written in support of this change and are preserved:

- `linkify_refs.py` — adds anchors to all section headings and converts prose §X.Y references into Markdown links. Idempotent: running it twice produces identical output. The disambiguation logic uses the union of section anchors across all three documents to decide where each unprefixed reference should resolve.

- `check_links.py` — walks every Markdown link with a fragment target and verifies the anchor exists in the target document. Reports broken links with their source location. The current revision passes the checker with all 1093 links resolving correctly.

The link-checker is the more important of the two going forward: it catches the failure mode that surfaced in the original v0.78 fresh-look review (the broken §28.6.4 cross-reference). Running it before each release is cheap and makes the cross-reference graph machine-checkable rather than human-memory-checkable.

<a id="sec-v0-80-to-v0-81-changelog-migration-guide"></a>
<a id="sec-migration-guide-20"></a>
### Migration guide

There is no source-level migration. The change is to the document set's internal cross-references; user code is unaffected.

For a reader of the rendered documentation, the visible change is that §X.Y references are now clickable links. The text of every reference is unchanged from v0.80.

For maintainers of the document set: when adding a new prose §X.Y reference, write it the same way as before; the next run of `linkify_refs.py` will turn it into a link. When adding a new section, the next run of `linkify_refs.py` will add an anchor automatically.

When bumping the version number, the cross-document filenames embedded in cross-doc links must be updated. A simple `sed` pass handles this; alternatively, the file naming convention could be changed to drop version numbers from filenames, in which case cross-doc links would become version-stable.

<a id="sec-v0-80-to-v0-81-changelog-status"></a>
<a id="sec-status-11"></a>
### Status

v0.81 completes step 1 of the cross-reference reorganization. Step 2 (semantic slugs and link text) remains for a future revision. The infrastructure (anchors, link-checker) is now in place; future revisions can lean on the link-checker to catch broken references at the moment they are introduced rather than during fresh-look reviews.

---

<a id="sec-v0-81-to-v0-82-changelog"></a>
### v0.81 to v0.82 changelog

Normative for migration; not part of the language definition.

This revision is step 2 of the cross-reference reorganization started in v0.81. It replaces the numeric anchor slugs with semantic ones derived from heading titles. The change is purely structural — link text is unchanged, only the URL fragments differ.

<a id="sec-v0-81-to-v0-82-changelog-what-changed"></a>
<a id="sec-what-changed-4"></a>
### What changed

Every section anchor now uses a slug derived from the heading title rather than the section number. For example:

| Section | v0.81 slug | v0.82 slug |
|---|---|---|
| [§28.2](Bezel_Language_Spec_0_98.md#sec-the-idisposable-contract) The `IDisposable` contract | `sec-28-1` | `sec-the-idisposable-contract` |
| [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) Local variables, ownership, and borrows | `sec-28-2` | `sec-local-variables-ownership-and-borrows` |
| [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) Ownership transfer | `sec-28-7` | `sec-ownership-transfer` |
| [§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) Runtime-managed types | `sec-28-8` | `sec-runtime-managed-types` |
| [§11.2](#sec-naked-binding-inside-positional-patterns) Interface declaration | `sec-11-2` | `sec-interface-declaration` |
| [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) The discard `_` | `sec-17-13` | `sec-the-discard-_` |
| Appendix C: Compiler-Enforced Invariants | `sec-appendix-c` | `sec-appendix-c` (unchanged) |

Link text stays as the section number (e.g., `[§28.8](...)` displays as "[§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer)"). Only the URL fragment after the `#` changed.

The new slugs survive renumbering. If language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) were renumbered to [§29](#sec-v0-62-to-v0-63-changelog) in a future revision, the slug `sec-runtime-managed-types` would still resolve to the same heading; only the displayed link text would need updating. By contrast, the v0.81 slug `sec-28-7` would have been broken by the same renumbering.

<a id="sec-collision-handling"></a>
### Collision handling

When two sections in the same file share a title, the second occurrence (and onward) gets a slug prefixed with its parent section's slug. Examples found in the spec:

| Section pair | Resolved slugs |
|---|---|
| [§3](#sec-syntax) Type System / [§C.1](Bezel_Language_Spec_0_98.md#sec-appendix-c-type-system) Type system | `sec-type-system` / `sec-appendix-c-type-system` |
| [§13](#sec-events) Visibility / [§C.2](Bezel_Language_Spec_0_98.md#sec-appendix-c-visibility) Visibility | `sec-visibility` / `sec-appendix-c-visibility` |
| [§29](#sec-v0-62-to-v0-63-changelog) Iteration / [§16.5](Bezel_Language_Spec_0_98.md#sec-iteration) Iteration | `sec-iteration` / `sec-range-expressions-iteration` |
| [§29.13](Bezel_Language_Spec_0_98.md#sec-async-iterators) Async iterators / [§30.7](Bezel_Language_Spec_0_98.md#sec-asynchronous-programming-async-iterators) Async iterators | `sec-iteration-async-iterators` / `sec-asynchronous-programming-async-iterators` |
| Multiple "Restrictions" sections ([§10.5](Bezel_Language_Spec_0_98.md#sec-restrictions), [§23.5](Bezel_Language_Spec_0_98.md#sec-properties-and-indexers-restrictions), [§25.2](Bezel_Language_Spec_0_98.md#sec-operator-overloading-restrictions), [§26.3](Bezel_Language_Spec_0_98.md#sec-extension-blocks-restrictions)) | First gets `sec-restrictions`; others prefixed by parent (e.g., `sec-records-restrictions`, `sec-properties-and-indexers-restrictions`, `sec-operator-overloading-restrictions`, `sec-extension-blocks-restrictions`) |

Appendix headings get a short prefix (`appendix-c` rather than the full title slug `appendix-c-compiler-enforced-invariants`) so that combined slugs stay readable.

<a id="sec-v0-81-to-v0-82-changelog-tooling"></a>
<a id="sec-changelog-tooling"></a>
### Tooling

Two scripts are involved:

- `linkify_refs.py` — updated in v0.82 to derive semantic slugs natively. Building the slug map from heading titles, with collision detection, replaces the trivial number-based slug generation. The script now scans more headings than v0.81 did — including named subsubsections like the rules inside [§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) — and adds anchors to all of them. Total anchors after v0.82: 378 in the spec (up from 313), 63 in the std lib (up from 60), 262 in the design rationale (up from 107).

- `rename_slugs.py` — written for the one-time v0.81→v0.82 migration. It builds a mapping from old numeric slugs to new semantic slugs and applies the rewrite to anchor tags, internal link fragments, and cross-document link fragments. Idempotent. Preserved for documentation purposes; future revisions don't need it because `linkify_refs.py` produces semantic slugs natively.

The link-checker (`check_links.py`) requires no changes — it's slug-format-agnostic. It still passes with all links resolving correctly (1094 links across the three documents).

<a id="sec-v0-81-to-v0-82-changelog-migration-guide"></a>
<a id="sec-migration-guide-21"></a>
### Migration guide

There is no source-level migration. The change is to the URL fragments embedded in the document set's cross-references; user code is unaffected.

For external sites or notes that referenced the v0.81 numeric slugs (e.g., a bookmark or external link to `#sec-28-7`), those links will break. The translation table at the top of this changelog lists the most-referenced sections with their old and new slugs.

For maintainers: the workflow is unchanged. Write `§X.Y` references in prose, run `linkify_refs.py`, run `check_links.py`. The script now uses semantic slugs throughout. New sections automatically get a slug derived from their heading title.

<a id="sec-future-proofing"></a>
### Future-proofing

The combination of semantic slugs plus link-checker means renumbering operations like the v0.80 language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) cleanup can be done in a single mechanical pass: rename the section number in the heading and update displayed link text via search-and-replace; the URL fragments don't need updating because they don't depend on section numbers. The v0.80 language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) renumbering had to update both heading numbers and URL fragments because v0.81's slugs were numeric. v0.82's slugs decouple the two concerns.

<a id="sec-v0-81-to-v0-82-changelog-status"></a>
<a id="sec-status-12"></a>
### Status

v0.82 closes the cross-reference reorganization started in v0.79's review. The infrastructure is now:

- Anchor tags on every section heading (level 2-4), with stable semantic slugs.
- Markdown links wrapping every prose §X.Y reference, pointing to the appropriate slug.
- A linkify script that adds anchors and links to new content.
- A link checker that catches broken references.

Future revisions can add new sections or rename existing ones without breaking the link graph; the link checker catches any that get out of sync.

---

<a id="sec-v0-82-to-v0-83-changelog"></a>
### v0.82 to v0.83 changelog

Normative for migration; not part of the language definition.

This revision applies the four issues found in v0.82's fresh-look review. Three are bug fixes in the FFI examples (top-level statements that violate [§2.2](#sec-no-top-level-functions-or-fields)'s "no top-level functions or fields" rule); one is a clarifying paragraph plus std-lib documentation on the Task helpers' interaction with `Task<owned T>`; one is a clarifying paragraph on variance in interface inheritance.

<a id="sec-v0-82-to-v0-83-changelog-source-compatible"></a>
<a id="sec-source-compatible-21"></a>
### Source-compatible

- **[§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling), [§32.7](Bezel_Language_Spec_0_98.md#sec-pointers-and-intptr), [§32.8](Bezel_Language_Spec_0_98.md#sec-callbacks), [§32.9](Bezel_Language_Spec_0_98.md#sec-error-handling) FFI examples wrapped in `static class` containers.** v0.82's FFI examples mixed `extern` blocks (legal at module level per [§32.1](Bezel_Language_Spec_0_98.md#sec-the-extern-block)) with executable statements and method declarations (which need to be inside a class or method body per [§2.2](#sec-no-top-level-functions-or-fields)'s "no top-level functions or fields" rule). v0.83 wraps the executable parts in `public static class` containers with named methods, matching the [§14.1](#sec-++-and-are-statements) `Swap<T>` example pattern.

  Specific fixes:
  - **[§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling)** — Two examples wrapped: `FileOps.OpenForReading(path)` for the `CString.From` pattern, and `Env.GetVariable(name)` for the `getenv`-with-`IntPtr`-return pattern. The latter also gains a proper return type (`string?`) and null-narrowing on the result, which fits the `IntPtr.Zero` test naturally.
  - **[§32.7](Bezel_Language_Spec_0_98.md#sec-pointers-and-intptr)** — The `dlopen`/`dlclose` example wrapped in `DynamicLoader.LoadAndClose(name)`.
  - **[§32.8](Bezel_Language_Spec_0_98.md#sec-callbacks)** — The `qsort` callback example wrapped: `IntSorter.CompareInts` becomes a static method on a containing class, and the `qsort(...)` call becomes the body of a `SortInPlace` method. The callback reference at the call site is `IntSorter.CompareInts`, matching the language spec's "method reference — a named instance method, a static method, or a property reference" rule for FFI callbacks.
  - **[§32.9](Bezel_Language_Spec_0_98.md#sec-error-handling)** — The error-handling example wrapped in `FileOps.OpenChecked(path)`.

  No behavioral change. The compiled output of any equivalent v0.82 program (one that worked around the spec's rule by mentally hoisting the examples into a method) is identical.

- **Std lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) Task helpers documented for class T.** v0.82's `Task` static helpers had a quiet hole: `FromException<T>`, `FromCancelled<T>`, and `WhenAll<T>` did not address what happens when T is a class type, leaving readers to wonder whether parallel `FromOwnedException`, `FromOwnedCancelled`, `WhenAllOwned` companions were needed. v0.83 adds two prose subsections that close the question:

  - `FromException<T>` and `FromCancelled<T>` are correctly typed as `Task<T>` regardless of whether T is a class. A faulted or cancelled task never produces a result value — there is no value to transfer ownership of, only an exception to re-raise. The Pattern A vs Pattern B distinction does not apply. Code that needs an immediately-faulted task in a context expecting `Task<owned T>` constructs one through `OwnedTaskCompletionSource<T>`.

  - `WhenAll<T>` does not aggregate ownership-transferring tasks. Because `Array<owned T>` is not a valid type (language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) admits `owned` only as an argument to `Task<...>` in this version), neither the input nor the output of a hypothetical `WhenAll` over `Task<owned T>` is expressible. Code that needs to gather ownership-transferring tasks iterates and awaits explicitly, transferring each result into an owned collection element. A worked example is added showing the pattern.

  The std-lib declaration of `Task` gains comment annotations on `FromException`, `FromCancelled`, and `WhenAll<T>` pointing to the prose subsections.

  No behavioral change. The signatures are unchanged; the prose explains what the signatures already meant.

- **[§12.9](Bezel_Language_Spec_0_98.md#sec-variance) variance and interface inheritance.** v0.82's variance section described how `out` and `in` work for individual interface declarations but did not address how a generic interface may inherit from a generic base whose variance is different from its own. The standard library has the leading example: `IList<T>` is invariant in T (it has mutating methods) and extends `IReadOnlyList<out T>` (which is covariant). v0.83 adds a "Variance and interface inheritance" subsection to [§12.9](Bezel_Language_Spec_0_98.md#sec-variance) with the worked example.

  The rule: the base's variance determines what assignments are admissible *through the base type*; the derived interface's variance determines what assignments are admissible *through the derived type*. An instance implementing `IList<Dog>` is assignable to `IReadOnlyList<Animal>` (covariant chain through the read-only view) but not to `IList<Animal>` (the derived interface is invariant). The derived interface's body is checked against its own variance constraints; the inheritance relation only fixes which type argument is substituted into the base.

  No behavioral change. The rule documents what conforming v0.82 implementations were already required to allow; v0.83 writes it down.

<a id="sec-v0-82-to-v0-83-changelog-migration-guide"></a>
<a id="sec-migration-guide-22"></a>
### Migration guide

There is no source-level migration. The FFI fixes are documentation changes; the Task-helper notes document existing behavior; the variance subsection clarifies a rule that was already implicit. Programs that compiled under v0.82 continue to compile under v0.83 with identical semantics.

For maintainers writing new FFI examples: wrap executable statements in a `public static class` with named static methods, matching the existing `IntSorter.SortInPlace` and `FileOps.OpenChecked` patterns. The compiler enforces the rule; the spec now consistently illustrates it.

<a id="sec-v0-82-to-v0-83-changelog-status"></a>
<a id="sec-status-13"></a>
### Status

v0.83 closes the four items from v0.82's fresh-look review. No new design questions are open at this revision.

---

<a id="sec-v0-83-to-v0-84-changelog"></a>
### v0.83 to v0.84 changelog

Normative for migration; not part of the language definition.

This revision applies the four issues found in v0.83's fresh-look review. Three are bug fixes — a contradiction between two sections of the spec about which operators apply to `bool`, an example that uses an invalid method-body syntax, and a stale cross-reference in Appendix A. The fourth is a clarifying paragraph documenting the design choice behind `Main(args: Array<string>)` rather than `string[]`.

<a id="sec-v0-83-to-v0-84-changelog-source-compatible"></a>
<a id="sec-source-compatible-22"></a>
### Source-compatible

- **[§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) contradiction with [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) about bitwise operators on `bool` resolved.** v0.83's [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) listed `bool` among the types supporting `&`, `|`, `^` (bitwise operators), but [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) said these operators are "not valid on `bool`, `char`, or floating-point types," and the std lib's `Boolean` record declared no `&`/`|`/`^` operators. The [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) wording is the actual rule: `bool` uses `and`, `or`, `not` (logical, with short-circuit semantics) and does not support the bitwise operators. v0.84 removes `bool` from [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types)'s bitwise list and adds an explicit cross-reference to [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) with a one-line explanation that boolean operations use `and`/`or`/`not`.

  No behavioral change. Implementations have been required to reject `true & false` all along; v0.84 brings [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types)'s prose in line with the spec's actual rule.

- **[§19.2](Bezel_Language_Spec_0_98.md#sec-arms) `Classify` example uses correct method-body syntax.** v0.83's example showed `private static method Classify(score: int): string => do ... end;` — but the grammar production `MethodBody ::= "=>" Expression ";" | Statement* "end" Identifier? ";"` does not admit `=> do ... end` for methods. The `=> do ... end` form is valid only for lambdas, property accessors, and case-arm bodies; method bodies use either `=> Expression` or the block-statement form. v0.84 rewrites the example to use the correct block form: `private static method Classify(score: int): string / Log(...); / return ...; / end Classify;`.

  No behavioral change. The example was illustrative and the rule it was illustrating ("multi-step logic in arm body extracts to a helper method") is unchanged; only the helper-method's syntax is corrected.

- **Appendix A.1 stale `§1.5` reference removed.** A.1's prose for the `CharContent`/`StringContent`/`VerbatimContent`/`FormatSpec` lexical productions referenced "[§1.5](Bezel_Language_Spec_0_98.md#sec-comments), [§18.3](Bezel_Language_Spec_0_98.md#sec-character-literals), [§18.4](Bezel_Language_Spec_0_98.md#sec-string-literals), [§18.5](Bezel_Language_Spec_0_98.md#sec-interpolated-strings), [§18.6](Bezel_Language_Spec_0_98.md#sec-verbatim-strings), and [§18.8](Bezel_Language_Spec_0_98.md#sec-escape-sequences-non-verbatim)" — but [§1.5](Bezel_Language_Spec_0_98.md#sec-comments) is "Comments" and has nothing to do with character or string content. The reference was stale (likely a leftover from an earlier section reorganization). v0.84 removes the [§1.5](Bezel_Language_Spec_0_98.md#sec-comments) reference; the listed [§18](#sec-things-deferred).x sections cover the productions completely.

- **[§33.4](Bezel_Language_Spec_0_98.md#sec-command-line-arguments) documents the `Main(args: Array<string>)` design choice.** v0.83 used `Array<string>` (resizable) for the `Main` args parameter without explaining why — C# uses `string[]` (fixed-size), and a reader following from C# might wonder. v0.84 adds a paragraph explaining the choice: `Array<string>` exposes the standard collection surface (`Count`, `Contains`, indexing, iteration) consistently with the rest of the std lib, where collection-typed parameters are uniformly `Array<T>` or an `IList<T>`/`IEnumerable<T>` interface. The arity of the args set is fixed for the program's lifetime, so the resizability is unused but harmless. The `string[]` form would also work and is marginally lighter; the consistency benefit outweighs the per-process allocation savings. The paragraph also notes the conversion path (`args.ToArray()`) for code that needs the fixed-size form.

  No behavioral change. The accepted Main signatures are unchanged; the new prose explains the existing choice.

<a id="sec-v0-83-to-v0-84-changelog-migration-guide"></a>
<a id="sec-migration-guide-23"></a>
### Migration guide

There is no source-level migration. The [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) bitwise fix removes a false claim in the prose; conforming implementations were already rejecting bitwise operators on `bool`. The [§19.2](Bezel_Language_Spec_0_98.md#sec-arms) example fix is a documentation correction; user code unaffected. The Appendix A.1 fix is a stale-reference cleanup. The [§33.4](Bezel_Language_Spec_0_98.md#sec-command-line-arguments) addition is purely explanatory.

For code authors: nothing to change. For readers of the spec, the [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) → [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) cross-reference makes the boolean-operator rule easier to find when reading the operator list for the first time.

<a id="sec-v0-83-to-v0-84-changelog-status"></a>
<a id="sec-status-14"></a>
### Status

v0.84 closes the four items from v0.83's fresh-look review. No new design questions are open at this revision.

---

<a id="sec-v0-84-to-v0-85-changelog"></a>
### v0.84 to v0.85 changelog

Normative for migration; not part of the language definition.

This revision applies the four issues found in v0.84's fresh-look review. Two are documentation corrections (the wrong escape syntax in [§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) and the invalid `=> ...;` body shape in [§26.5.1](Bezel_Language_Spec_0_98.md#sec-specificity-ordering)), and two are example rewrites in [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) where the prior code violated the ownership rules it was supposed to illustrate.

<a id="sec-v0-84-to-v0-85-changelog-source-compatible"></a>
<a id="sec-source-compatible-23"></a>
### Source-compatible

- **[§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) char range escapes use the correct fixed-width form.** v0.84's [§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) (Range<char> iteration) used brace-delimited Unicode escapes — `'\u{0000}'`, `'\u{D7FF}'`, `'\u{E000}'`, `'\u{10FFFF}'`, `'\u{D7F0}'`, `'\u{E010}'` — but language spec [§18.8](Bezel_Language_Spec_0_98.md#sec-escape-sequences-non-verbatim)'s normative escape table defines only fixed-width forms (`\uXXXX` for the BMP, `\UXXXXXXXX` for the full code point range). v0.85 rewrites the [§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) examples in the fixed-width form: `'\u0000'..'\uD7FF'` and `'\uE000'..'\U0010FFFF'`, with the literal U+10FFFF written `'\U0010FFFF'` since it falls outside the BMP.

  The [§16.3](Bezel_Language_Spec_0_98.md#sec-the-ranget-type) prose previously contained the example `'\u{D800}' in r` to illustrate that surrogate code points can appear in a range's integer extent without being yielded by iteration. But surrogate escapes are themselves compile errors per language spec [§18.3](Bezel_Language_Spec_0_98.md#sec-character-literals), so neither `'\uD800'` nor the brace form `'\u{D800}'` would actually compile, and `char(0xD800)` raises `OverflowException` per language spec [§5.2](Bezel_Language_Spec_0_98.md#sec-explicit-conversions). v0.85 rewrites the surrounding prose to describe the surrogate-membership question without showing a literal that cannot exist, and adds a clarifying note that surrogate code points cannot be written as `char` literals.

  No behavioral change. The rule about iteration terminating at the surrogate boundary is unchanged; only the example syntax is corrected.

- **[§26.5.1](Bezel_Language_Spec_0_98.md#sec-specificity-ordering) elided method body uses correct block syntax.** v0.84's [§26.5.1](Bezel_Language_Spec_0_98.md#sec-specificity-ordering) showed `public method Sum(): T => ...;` to elide the body of an example extension method, but `...` is not a Bezel expression, and the form `=> ...;` is therefore not valid syntax. v0.85 replaces this with the standard block-bodied form with a comment in place of the body:
  ```
  public method Sum(): T
    // implementation elided
  end Sum;
  ```

  This matches the convention used elsewhere in the spec (such as the std-lib doc-shorthand pattern with `// ...` for elided method bodies) and parses correctly.

  No behavioral change. The example illustrates extension specificity ordering; the elision form is the only thing changed.

- **[§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) examples rewritten to match the ownership rules.** v0.84's [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) ownership-transfer examples — `Attach(owned handler: EventHandler)` adding to a non-owning array, and `DetachAll()` returning the field's contents as `owned` — both violated rules introduced earlier in language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management). The `Attach` body called `_handlers.Add(handler)`, but `Array<T>.Add(item: T)` takes `item: T` (no `owned`) per language spec [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements)'s "collections do not own elements" rule, leaving the owned `handler` parameter undischarged at scope end. The `DetachAll` body read the owned field `_handlers` into a local — which produces a borrow per the language spec [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) producers table — then attempted to return the borrow as `owned`, a type error; it also overwrote `_handlers` without first disposing the previous value, a language spec [§28.7](Bezel_Language_Spec_0_98.md#sec-reassigning-owned-fields) violation.

  v0.85 replaces both examples with versions that compose with the ownership rules:

  - `TakeOwnership(owned resource: Stream)` stores the owned parameter directly into an owned field. This is the canonical "owned param → owned field" transfer.
  - `NewBuffer(size: int): owned Array<byte>` returns a fresh `new` value as owned. This is the canonical "factory returns owned" pattern.

  v0.85 also adds explanatory prose covering two cases the original examples conflated:

  - **Replacing an owned field.** When a class needs to replace a previously-set owned field, the replacement method first disposes the existing value, then assigns. The [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) prose now shows this pattern (`ReplaceResource`) explicitly.
  - **Moving an owned field's contents out.** This was the intent of the original `DetachAll`. Bezel's design (field reads produce borrows; collections don't own elements) means there is no direct "take this field's contents and return them as owned" form — a swap helper using `ref T` parameters is required. The new [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) prose shows the pattern (`ReplaceAndReturn`) using `Utils.Swap` from [§14.1](#sec-++-and-are-statements).

  The original examples were intended to illustrate ownership transfer but illustrated patterns that didn't compile under Bezel's rules. The replacements cover the same conceptual ground (owned param, owned return, field replacement, value-out-of-field via swap) with code that actually conforms to the rules elsewhere in language spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management).

<a id="sec-v0-84-to-v0-85-changelog-migration-guide"></a>
<a id="sec-migration-guide-24"></a>
### Migration guide

There is no source-level migration. All four fixes are documentation corrections; no language semantics change. Programs that compiled under v0.84 continue to compile under v0.85 with identical behavior.

For maintainers writing new examples involving `char` ranges with surrogate boundaries: use prose ("the code point U+D800") to refer to surrogate values, since they cannot be written as `char` literals or constructed via `char(int)` conversion. For maintainers writing examples that elide a method body: use the block form with a `// ...` or `// implementation elided` comment, not `=> ...;`. For maintainers writing examples involving owned fields: be explicit about whether the operation is "set-once," "replace-with-disposal," or "swap-out-via-helper" — the three patterns have different code shapes.

<a id="sec-v0-84-to-v0-85-changelog-status"></a>
<a id="sec-status-15"></a>
### Status

v0.85 closes the four items from v0.84's fresh-look review. The [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) rewrite documents a real ergonomic gap in Bezel's ownership design — there is no built-in "take from owned field" idiom, only the swap-via-`ref` workaround — but the gap is now visible in the spec rather than hidden by examples that pretended the gap didn't exist. A future revision could add a dedicated take-from-field operator (a hypothetical `move x.f` form, or similar) if usage shows the swap idiom is too ceremonious in practice. For now, the workaround is documented and the examples compile.

---

<a id="sec-v0-85-to-v0-86-changelog"></a>
### v0.85 to v0.86 changelog

Normative for migration; not part of the language definition.

This revision applies the four issues found in v0.85's fresh-look review. Three are documentation corrections (two stale cross-references and one imprecise wording on primary-constructor capture), and one is a std-lib example rewrite where the prior code was a workaround for an acknowledged design gap but didn't actually compile under the rules it was supposed to be working around.

<a id="sec-v0-85-to-v0-86-changelog-source-compatible"></a>
<a id="sec-source-compatible-24"></a>
### Source-compatible

- **[§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) cross-reference for the unused-result warning corrected.** v0.85's [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) said the discard `_ := expr;` "suppresses the [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) *unused result* warning." But [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) is about owned-discharge warnings on `new` results, not about the unused-result warning on expression statements. The actual warning is defined in language spec [§21.9](Bezel_Language_Spec_0_98.md#sec-statement-form-of-expressions) (Statement form of expressions): "An expression statement whose result is a non-`()` type is permitted but the compiler emits a warning..." v0.86 corrects the cross-reference to point to [§21.9](Bezel_Language_Spec_0_98.md#sec-statement-form-of-expressions).

  No behavioral change. The warning behavior is unchanged; only the cross-reference is corrected.

- **[§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) cross-reference for short-circuit semantics corrected.** v0.85's [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) said "Logical on `bool`: `and`, `or`, `not` (with short-circuit evaluation, [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations))." But [§6.5](Bezel_Language_Spec_0_98.md#sec-bitwise-operations) is "Bitwise operations" and does not discuss short-circuit. Short-circuit semantics are documented in language spec [§17.2](Bezel_Language_Spec_0_98.md#sec-notes-on-operators) (Notes on operators): "`and` and `or` short-circuit: the right operand is not evaluated if the result is determined by the left." v0.86 corrects the cross-reference to point to [§17.2](#sec-string-concatenation-with-non-strings).

  No behavioral change. The short-circuit rule is unchanged; only the cross-reference is corrected.

- **[§22.8](Bezel_Language_Spec_0_98.md#sec-field-initializers) step 0 wording matches [§11.1](#sec-the-scrutinee-less-form)'s iff-used capture rule.** v0.85's [§22.8](Bezel_Language_Spec_0_98.md#sec-field-initializers) step 0 (the construction-sequence list) said "Primary-constructor parameters ... are captured into their compiler-generated private fields." This omitted the qualification [§11.1](#sec-the-scrutinee-less-form) carries: "Parameters referenced only in initializers do not generate backing fields; parameters referenced in method, property, constructor, or destructor bodies are captured." v0.86 tightens step 0 to match — the captured parameters are those referenced in body positions, and parameters referenced only in initializer positions are not captured.

  No behavioral change. The capture-iff-used rule is the actual rule [§11.1](#sec-the-scrutinee-less-form) has documented all along; the [§22.8](Bezel_Language_Spec_0_98.md#sec-field-initializers) wording was less precise but the implementations were already implementing the [§11.1](#sec-the-scrutinee-less-form) rule.

- **Std lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) `GatherFiles` example replaced with prose acknowledging the gap.** v0.85's std lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) contained `GatherFiles` as the recommended workaround for "no `WhenAll` for `Task<owned T>`." But the workaround was itself broken: `using var results := new Array<FileStream>()` followed by `results.Add(fs)` (where `fs` is `using var`) leaves the array holding a borrow that becomes stale when `fs` disposes at the end of each loop iteration (language spec [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements) — collections do not own their elements; `Add` accepts only a borrow); and the trailing `return results` violates language spec [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) — a `using var` cannot be moved out of.

  v0.86 replaces the broken example with prose that honestly describes the expressiveness gap: there is no batch-aggregation form for `Task<owned T>` in this version, because no collection type currently owns its elements and `using var` locals cannot be returned. The prose then shows the patterns that *do* compile: sequential consumption inside the iteration body (each `await` produces an owned value that is consumed before the next iteration), and an async-iterator producer (`yield return await OpenFileAsync(path)`) for incremental producer-consumer pipelines. The async-iterator path uses the existing rule from language spec [§29.8](Bezel_Language_Spec_0_98.md#sec-iterators-with-owned-parameters): a `yield return` of an owned class-typed expression transfers ownership to the enumerator, and the consumer's iteration variable is a borrow valid only within the loop body.

  The fix acknowledges what the original example was reaching for — a batch-collected, owned-aggregated result — and honestly documents that this shape is not currently expressible. A future revision is expected to extend language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s opt-in list to admit `Array<owned T>`, at which point `WhenAllOwned<T>` and a true batch-collect form become possible. For now, the sequential and async-iterator patterns above are the supported alternatives.

<a id="sec-v0-85-to-v0-86-changelog-migration-guide"></a>
<a id="sec-migration-guide-25"></a>
### Migration guide

There is no source-level migration. All three of the spec-side fixes are documentation corrections that don't change semantics. Programs that compiled under v0.85 continue to compile under v0.86 with identical behavior.

For std-lib doc readers: the `GatherFiles` example previously presented as a working workaround is gone. Code that copied `GatherFiles` from the v0.85 std-lib doc would have failed to compile (the example was always broken — v0.86 just makes the limitation visible rather than hidden). Migrate to the sequential pattern (consume each `await` before the next) or to the async-iterator pattern (`yield return await ...`) shown in the new prose.

<a id="sec-v0-85-to-v0-86-changelog-status"></a>
<a id="sec-status-16"></a>
### Status

v0.86 closes the four items from v0.85's fresh-look review.

The "take from owned field" gap that v0.85 first acknowledged (and that v0.86 now extends to "no batch-aggregation of `Task<owned T>` results") is the one substantive design hole this series of revisions has surfaced. Both the field-out-take case and the batch-aggregation case stem from the same root: Bezel has no way to extract an owned value from a slot that doesn't already commit to single-shot consumption. `using var` is single-shot at scope end. `owned` parameters are single-shot at the call. `owned` return values are single-shot at the return. But owned-as-a-field is *retained* — fields exist to be read repeatedly — and the natural "take and replace" operation has no syntax. Likewise, owned-as-a-collection-element doesn't exist at all because language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s opt-in list only admits `Task<owned T>`.

A future revision is expected to address one or both of these gaps. The two leading candidates are:

1. **A `move` (or `take`) operator on field/element access.** `move x.f` reads `x.f` as an owned value and leaves the field in moved-from state, which the assigning caller must overwrite before the next read. This is the Rust pattern, suitably adapted. It is the smallest surface-area change — no new collection types, no new generic-argument admittance — and composes with the existing rules. The cost is a new compile-time state-tracking obligation on field accesses (the field becomes a tracked-state slot, akin to a local).

2. **Extending language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s opt-in list to admit `Array<owned T>` and friends.** This is more invasive but more expressive — once `Array<owned T>` is a real type, batch-aggregation falls out naturally, and the `OwnedTaskCompletionSource<T>` / `Task<owned T>` machinery extends to `WhenAll<owned T>`. The cost is a substantial extension of the ownership-tracking system into collection internals.

These are not exclusive. A future revision could adopt (1) as the smaller-surface fix for the field case, and (2) later for the collection case; or (2) alone could subsume (1) by letting users wrap a single owned value in `OwnedCell<T>`. The decision belongs to a substantive design pass; for v0.86, the gap is honestly documented and the workable patterns are shown.

---

<a id="sec-v0-86-to-v0-87-changelog"></a>
### v0.86 to v0.87 changelog

Normative for migration; not part of the language definition.

This revision is the first since v0.85's review-and-fix cadence began that introduces a substantive language addition rather than a documentation correction. v0.87 adds the `move` expression form, which closes the "take from owned field" gap that v0.85 first surfaced and that v0.86 expanded the visibility of. The addition is source-compatible (no existing program changes meaning) but adds a new keyword and a new compile-time analysis.

<a id="sec-new-language-feature"></a>
### New language feature

- **The `move x.f` expression form.** Read an owned instance field as an owned value, leaving the field in *moved-from* state. The field must be reassigned before the surrounding method exits, on every control-flow path including exception edges. The compiler enforces this statically.

  Syntactic form: `move PrimaryExpr`, where `PrimaryExpr` resolves to an owned-field access. Semantic restrictions: the field must be an owned instance field (not a static field, not a local, not a collection element, not a non-owned field). Examples in the spec at language spec [§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression).

  Reserved word: `move` is added to the unconditionally reserved keyword list (Appendix B.1). User-defined identifiers named `move` are now compile errors. The keyword choice follows the C++ / Rust / Swift convention for ownership-transfer operations and reads naturally at use sites.

  The new form composes with the existing single-shot transitions: `using var u := move _resource;` consumes scope-bound; `Helper(move _resource)` passes ownership to a helper expecting `owned`; `return move _resource;` transfers ownership to the caller under an `owned` return type. In each case the field-restoration rule still applies — the surrounding method must reassign the field before exit.

  The grammar adds a `MoveExpr` production at PrimaryAtom level (language spec Appendix A). The new production is non-overlapping with the existing grammar — `move` was previously a free identifier and is now a reserved keyword in primary-expression context.

- **The field-restoration rule.** Within a method body that performs `move x.f`, every control-flow path from the move site to the method's exit must execute an assignment `x.f := value;` between the move and the exit. The exits considered include normal return, fall-through, throw, and `yield break`; exception edges from intervening throwable expressions are exits and must be satisfied as well.

  The rule's enforcement is the same definite-assignment analysis Bezel already performs for non-nullable locals (language spec [§7.9](Bezel_Language_Spec_0_98.md#sec-definite-assignment-for-non-nullable-fields)), lifted to the field level. Implementations that already track local-variable definite-assignment have the machinery in place; the addition is a new state ("moved-from") on owned-field nodes in the per-method flow graph and a new check at every exit edge.

  Failure to satisfy the rule is a compile error. The error message names the move site, the unrestored exit path, and the standard remedies: assign on the offending path; move the move into a branch where reassignment is reachable; or, if the field should sometimes hold no value, declare it nullable and assign `null` after the move. Language spec [§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression)'s "field-restoration rule" subsection gives the canonical examples.

<a id="sec-code-shape-implications"></a>
### Code-shape implications

The "build before take" discipline now has a statically-enforced expression. The pattern:

```bezel
public method ReplaceAndReturn(): owned Stream
  var fresh := DangerousFactory();   // build first (may throw — but no move yet)
  var old := move _resource;          // take (cannot throw)
  _resource := fresh;                  // restore (cannot throw)
  return old;                          // return the taken value
end ReplaceAndReturn;
```

is the form the spec recommends and the form the rule rewards. The reverse order — move, then build, then assign — is rejected at compile time because the build's exception edge is an unrestored exit.

The `Utils.Swap`-via-`ref T` workaround that v0.85 introduced is no longer needed for the field-take case. v0.87 leaves `Utils.Swap` in the std lib (it remains a valid value-swap helper for non-owned-field uses) but the language spec's [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) prose now uses `move` as the primary form. Existing programs that used the swap idiom continue to compile; they can migrate to `move` opportunistically.

<a id="sec-updates-to-existing-prose"></a>
### Updates to existing prose

- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) producers table.** New row: `move obj.f` produces an Owned value, with the field-restoration rule deferred to [§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression).
- **[§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) ownership transfer.** The "moving an owned field's contents *out*" subsection no longer presents the swap idiom as the primary form; it now defers to [§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression) (`move`) and provides one worked example, with [§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression) carrying the full rule and the failure-mode catalog.
- **[§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression) (new).** The full specification of the `move` expression: scope of applicability, the field-restoration rule, exception-path obligations, loop-and-conditional control-flow rules, constructor and destructor interactions, interaction with `using var`, the nullable-field detach pattern, and interaction with overload resolution.
- **[§22.6](Bezel_Language_Spec_0_98.md#sec-overloading) practical pattern.** The `TryGetCached(out owned)` overload now has a worked-out body using `move` on a single-slot owned field, replacing the earlier "no-implementation-shown" presentation. The example also documents the multi-slot case as not yet expressible (the collection-element case is still gated on language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) admitting `Array<owned T>`).
- **Appendix A grammar.** `MoveExpr` production added to `PrimaryAtom`.
- **Appendix B reserved keywords.** `move` added to the unconditionally reserved list.
- **Appendix C resource management.** New invariant entry for the field-restoration rule.

<a id="sec-v0-86-to-v0-87-changelog-migration-guide"></a>
<a id="sec-migration-guide-26"></a>
### Migration guide

Existing programs do not change meaning. There is no source-level migration required, with one exception:

- **Programs that used `move` as a user-defined identifier** (a method name, a parameter name, a local variable name, a field name) must rename the identifier. The compiler reports the use site with a "reserved keyword" error directly. In practice this is rare — `move` was not a common identifier in the standard library or in any code published to date — but maintainers should grep for `\bmove\b` outside of comments before upgrading.

Code that previously used the `Utils.Swap`-via-`ref T` workaround (the v0.85 [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) ReplaceAndReturn pattern) continues to compile. To migrate to `move`, replace:

```bezel
var fresh := new Stream();
Utils.Swap(ref _resource, ref fresh);
return fresh;
```

with:

```bezel
var fresh := new Stream();
var old := move _resource;
_resource := fresh;
return old;
```

The new form is one statement longer but is what the spec now recommends and what the compiler now understands directly. The swap form's appeal was that it was the only expressible form; now that there is a direct form, the swap is correctly understood as an idiom for non-take cases.

<a id="sec-what-is-*not*-in-v0-87"></a>
### What is *not* in v0.87

The other half of the gap that v0.85 surfaced — collections of owned values — is still not expressible. `Array<owned T>` and similar types remain rejected (language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) admits `owned` as a generic argument only for `Task<...>`). Batch-aggregation of `Task<owned T>` results still requires the sequential and async-iterator workarounds documented in std lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers).

The reasons to ship `move` separately from collection-of-owned support:

1. **Surface area.** `move` is one keyword and one analysis pass. Collection-of-owned is a substantial extension of the type system (every collection type's signatures change; the std lib gains second variants) and a substantial extension of ownership tracking (per-element state in collections). Combining the two would have produced a v0.87 with several times the spec delta and several times the implementation work.

2. **Maturation order.** The field-restoration rule's flow analysis is the prototype for the per-element flow analysis a `Array<owned T>` design would need. Shipping the field case first lets the analysis crystallize — error messages, edge cases, performance characteristics — before being applied to the larger surface.

3. **Independent value.** Even without collection-of-owned, `move` covers the most common pain points: the `OwnedTaskCompletionSource`-style class, the resource-replacement method, the `TryGetCached(out owned)` pattern. These are common; the collection case is narrower.

A future revision is expected to extend language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s opt-in list to admit `Array<owned T>` and similar collection-of-owned types. The `move` keyword's rules already accommodate that extension — `move arr[i]` would be admitted under the same restoration rule, with the array element's slot replacing the field as the moved-from location.

<a id="sec-v0-86-to-v0-87-changelog-status"></a>
<a id="sec-status-17"></a>
### Status

v0.87 introduces the first new language feature in this revision series. The field-take gap that has been visible since v0.85 is closed for the field case; the collection case remains open and is the next likely target.

---

<a id="sec-v0-87-to-v0-88-changelog"></a>
### v0.87 to v0.88 changelog

Normative for migration; not part of the language definition.

This revision is the second substantive language addition since the review-and-fix cadence began. v0.87 added the `move` expression, closing the take-from-owned-field gap. v0.88 closes the parallel take-from-owned-collection-element gap, by extending language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s opt-in list to admit owned-modified element types on the standard library's resizable collections. The addition is source-compatible (no existing program changes meaning) and unblocks several patterns that were previously inexpressible — most prominently, `WhenAllOwned<T>` for batch-awaiting `Task<owned T>` results.

<a id="sec-v0-87-to-v0-88-changelog-new-language-feature"></a>
<a id="sec-changelog-new-language-feature"></a>
### New language feature

- **Owned-element collections.** `Array<owned T>`, `LinkedList<owned T>`, `Set<owned T>`, `Stack<owned T>`, `Queue<owned T>`, `Dictionary<K, owned V>` (and `Dictionary<owned K, V>`, `Dictionary<owned K, owned V>`) are now valid types when T (resp. K, V) is a class type. The instantiations are distinct from the unmodified-argument forms; there is no implicit conversion between `Array<T>` and `Array<owned T>`.

  An owned-element collection owns its elements. The disposal contract cascades: `coll.Dispose()` disposes every element, then frees the backing storage. `Clear()` disposes every element. The indexer-set disposes the previous value before installing the new one. `Remove` and `RemoveAt` dispose the removed element. `Add(owned item)` accepts ownership transfer.

  The opt-in list is hard-coded at the language level; the std-lib types listed above are admitted, plus the iteration interfaces (`IEnumerable<owned T>`, `IEnumerator<owned T>`, `IList<owned T>`, `IReadOnlyList<owned T>`, `ICollection<owned T>`, `IAsyncEnumerable<owned T>`, `IAsyncEnumerator<owned T>`). User-defined generic types do not opt in in this version; that is deferred to a future revision (rationale [§5.11](#sec-owned-element-collections)). Fixed-size arrays `T[]` are also not in the opt-in list; the rationale notes that adding owned-element semantics to a fixed-size array would not extend the language's expressive power beyond what `move arr[i]` on a resizable owned-element array already provides.

- **Drain methods on owned-element collections.** Each owned-element collection provides:
  - `PopAt(index): owned T` (and `PopFirst`, `PopLast`) for sequence types — removes and returns ownership.
  - `Pop(item): owned T` for `Set<owned T>` — removes and returns ownership of the matching element.
  - `PopValue(key): owned V` for `Dictionary<K, owned V>` — removes and returns ownership of the value at the given key.
  - `Drain(): IEnumerable<owned T>` — iteration drains the source as it consumes; after full iteration, the collection is empty.
  
  Default iteration `for var x in coll do ... end` continues to produce *borrows* of each element, the same as for non-owned collections. To consume, use `Drain()`. The opt-in distinction makes the consuming case explicit at the call site.

- **`move coll[i]` for owned-element collection slots.** The `move` expression form ([§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression)) is extended from owned fields to owned-element collection slots. `move coll[i]` reads the i-th element as owned, leaving the slot in moved-from state; the slot must be reassigned before the surrounding method exits. The field-restoration rule applies, with constant indices tracked per-slot and non-constant indices tracked conservatively at the whole-collection level.

- **`Task.WhenAllOwned<T>`.** New std-lib helper that aggregates `Task<owned T>` results into an owned `Array<owned T>`. Signature: `Task.WhenAllOwned<T>(owned tasks: Array<owned Task<owned T>>): Task<owned Array<owned T>> where T: class`. The caller transfers ownership of the input array; the result is a Pattern B task whose result is an owned array of owned elements, consumed under `using var`. Closes the gap that std-lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) had been documenting since v0.86.

<a id="sec-v0-87-to-v0-88-changelog-updates-to-existing-prose"></a>
<a id="sec-changelog-updates-to-existing-prose"></a>
### Updates to existing prose

- **language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) opt-in list expanded.** The list now names the std-lib types explicitly and notes the deferred user-type opt-in. The "currently only `Task<T>` accepts an owned argument" wording is replaced with the full list.
- **[§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) rewritten.** Previously titled "Collections do not own elements" (a categorical claim that is no longer true), now titled "Collection element ownership" with two subsections: "Non-owning collections" (the prior content, still valid for the unmodified element-type case) and "Owned-element collections" (the new content). Both forms continue to be supported.
- **[§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression) extended.** The `move` expression now applies to collection slots in addition to fields. The "form is permitted only on owned fields" wording is replaced with "owned fields and owned-element collection slots."
- **[§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) producers table.** New row for `move coll[i]` producing Owned (parallel to the existing `move obj.f` row).
- **[§17.6](#sec-inline-fixed-size-value-type-array-fields) indexer-increment rule.** The `arr[i]++` rejection for owned-element types of class T is updated: `Array<owned MyClass>` admits `arr[i]++` because the slot owns its content; `Array<MyClass>` continues to reject it.
- **[§22.6](Bezel_Language_Spec_0_98.md#sec-overloading) TryGetCached example.** The single-slot owned field workaround is replaced with a `Dictionary<K, owned V>.PopValue` form, which is what users would naturally write today.
- **[§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) trivial-dispose for generic instantiations.** Owned-element collection instantiations (`Array<owned T>`, etc.) are explicitly noted as **not** trivial-dispose — they own their elements and disposal cascades.
- **[§30.10](Bezel_Language_Spec_0_98.md#sec-standard-library-task-helpers) task helpers list.** Adds `Task.WhenAllOwned<T>` with a brief description and a pointer to the std-lib worked example.
- **Std lib [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic) (collections).** Each collection type (`Array<T>`, `LinkedList<T>`, `Dictionary<K, V>`, `Set<T>`, `Stack<T>`, `Queue<T>`) gains an "owned-element variant" subsection with the signature changes spelled out.
- **Std lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) task helpers.** The "no batch-aggregation expressible" prose is replaced with the worked `WhenAllOwned` example. The deferred-future-revision language is removed.
- **Std lib [§4.6](Bezel_Standard_Library_0_98.md#sec-stackt-and-queuet) Stack and Queue.** The borrow-source story for `Pop()`, `Peek()`, and `Dequeue()` on non-owned collections is clarified — these methods produce borrows whose source is the original owner of the value, not the stack/queue itself, since the stack/queue is a transit point. For the owned-element variants, `Pop(): owned T` and `Dequeue(): owned T` transfer ownership cleanly.
- **Appendix C resource-management invariants.** New entries for owned-element collection rules and `move coll[i]`.

<a id="sec-v0-87-to-v0-88-changelog-migration-guide"></a>
<a id="sec-migration-guide-27"></a>
### Migration guide

Existing programs do not change meaning. There is no source-level migration required.

For new code: types like `Array<Connection>` (where Connection is a class) continue to be non-owning, with the existing `DisposeElements()` pattern available for cases where the holding class wants to dispose elements explicitly. The owned-element form `Array<owned Connection>` is opt-in; users choose it when they want the collection to manage element disposal automatically.

**When to choose owned-element vs. non-owning:**

- Use `Array<owned T>` (or the corresponding owned-element form for other collections) when the collection *is* the owner of its elements. Common cases: a class holds a collection of resources whose lifetime matches the class; a method aggregates results from `Task<owned T>` calls; a builder accumulates owned partial results before committing.
- Use `Array<T>` (the non-owning form) when elements are owned by some other source — typically the caller. Common cases: a function takes a collection of pre-allocated items and just operates on them; a collection is built by sharing references to long-lived objects; the collection's `Dispose()` should not affect element lifetimes.
- When a class semantically owns a collection-of-resources but wants explicit control over when element disposal happens, the v0.85 pattern of `private owned _coll: Array<T>` plus a destructor calling `_coll.DisposeElements()` is still valid. The owned-element form (`Array<owned T>`) automates this; the explicit form remains for cases where automatic disposal is wrong (perhaps because not every element should be disposed, or because element disposal is interleaved with other work).

For code that previously documented the gap with workarounds, the workarounds can be migrated:

- The `using var oneSlot: Connection` indirection for a "pool" can become `Dictionary<string, owned Connection>`.
- The "WhenAll doesn't work for class T; here's the sequential workaround" pattern can become `await Task.WhenAllOwned(taskArray)` directly.
- `OwnedTaskCompletionSource<T>`-style classes that hold one owned result can continue to do so, but classes that need to hold *several* owned results no longer need one field per slot.

<a id="sec-what-is-*not*-in-v0-88"></a>
### What is *not* in v0.88

- **LINQ over owned-element collections.** `Where`, `Select`, `Sum`, and the rest of the LINQ surface are not extended to `IEnumerable<owned T>` in this version. Users who need LINQ-style operations on owned-element collections must iterate explicitly with `for var x in coll.Drain() do ...` and build a new collection from the consumed elements. This is rationale [§5.11](#sec-owned-element-collections)'s "deferred LINQ" point; the LINQ extension is anticipated for v0.89 or later.

- **User-defined collections.** A class declared `class MyContainer<T>` cannot be instantiated as `MyContainer<owned Connection>`. The opt-in mechanism for user types — what kind of declaration says "this class handles owned-T correctly" — is deferred. Users who need an owned-element user type today must use the std-lib types as building blocks, not declare their own.

- **`T[]` (fixed-size arrays) of owned elements.** Not admitted. The owned-element form for `T[]` would be limited to `move arr[i]` plus assignment-back, which is the same surface that `move x.f` already provides for fields and that resizable owned-element collections already provide. Adding `owned T[]` would not extend expressive power and is deferred.

- **Single-method `Drain()` on `Dictionary<K, owned V>`.** A natural drain that yields `(key, owned value)` pairs is not expressible because records cannot have owned fields ([§28.5](Bezel_Language_Spec_0_98.md#sec-owned-fields)). The recommended pattern is to collect keys first, then `PopValue` each. A future revision adding tuple deconstruction in `for` loops would unblock the single-method form.

<a id="sec-v0-87-to-v0-88-changelog-status"></a>
<a id="sec-status-18"></a>
### Status

v0.88 closes the collection-of-owned gap that has been visible since v0.85's review surfaced it and that v0.86 expanded the documentation of. The `move` expression added in v0.87 now extends to collection slots, completing the take-out story for both fields and elements.

The remaining ownership-related design items are LINQ-over-owned and user-type opt-in; both are deferred to future revisions. For the field-take and element-take cases that triggered this work, v0.88 is the closing chapter.

---

<a id="sec-v0-88-to-v0-89-changelog"></a>
### v0.88 to v0.89 changelog

Normative for migration; not part of the language definition.

This revision applies the nine items found in v0.88's fresh-look review. Two are substantive design refinements that loosen overly-restrictive rules introduced in earlier versions; the remaining seven are documentation corrections, signature renames, and prose cleanups. All changes are source-compatible — no existing program changes meaning, and one previously-rejected pattern (`coll.Add(await task)`) is now admitted.

<a id="sec-substantive-rule-changes"></a>
### Substantive rule changes

- **Pattern B's "immediate operand of `using var`" rule loosened.** The language spec [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) rule for awaiting a `Task<owned T>` for class T previously required the await to be the immediate operand of `using var` or `await using var`. v0.89 admits any owned-consuming context — `using var`, `await using var`, an `owned` parameter argument, an `owned` return expression, or any other position the type system requires the value to be discharged. The rule's safety property is unchanged; the change is purely about admitting more syntactic positions where the await can appear.

  This unblocks the natural shape `coll.Add(await task)` (where `Add` takes `owned`), which v0.88's `WhenAllOwned` example tried to use but had to work around. The std-lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) example is updated to use the cleaner form. See rationale [§5.12](#sec-pattern-b-loosening-and-iterator-yield-refinement) for the design discussion.

- **Iterator yield-ownership rule split into two cases.** The language spec [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) "Iterator yield ownership" subsection previously had a single rule for owned-class-typed yields: enumerator owns each yielded value, consumer borrows. v0.89 splits the rule by the iterator method's declared return type:

  1. Iterator returns `IEnumerable<T>` (no `owned` modifier on T) and yields owned class-typed values: enumerator owns; consumer borrows. (The original rule, unchanged.)
  2. Iterator returns `IEnumerable<owned T>`: ownership transfers from enumerator to consumer at each `MoveNext`; consumer's iteration variable is owned and must be discharged in the body.

  The split is necessary because v0.88 admitted `IEnumerable<owned T>` to the language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) opt-in list and `Drain()` was specified as yielding owned values to the consumer; the v0.78 single-rule form contradicted v0.88's `Drain` semantics. v0.89's split makes both use patterns expressible: filter-style iterators that briefly let the consumer look at owned values, and drain-style iterators that hand off ownership.

  The split applies the existing definite-assignment machinery: when the consumer's iteration variable is owned (case 2), the loop body must discharge it on every path, and falling off without discharging is a compile error. See rationale [§5.12](#sec-pattern-b-loosening-and-iterator-yield-refinement) for the design discussion.

<a id="sec-source-compatible-signature-changes"></a>
### Source-compatible signature changes

- **`Set<owned T>.Pop` and `Set<owned T>.TryPop` renamed to `Take` and `TryTake`.** The non-owned `Set<T>` does not have a `Pop` method, and the owned variant's `Pop(item: T)` was a search-by-equality operation that confusingly shared the name with the Stack-style `Pop()`. v0.89 renames the methods to `Take(query: T)` and `TryTake(query: T, out owned removed: T?)`, with prose explaining the search-by-equality semantics. The non-owned `Set<T>` continues to have no analogous method; the owned variant's `Take` is unique to it.

  `TryTake`'s out parameter is `T?` (nullable) so that the false-return path can write `null`. This matches the broader correction (next item) of using nullable out parameters when an `out owned` value cannot meaningfully exist on the false path.

- **`TryGetCached` example uses nullable out parameter.** The language spec [§22.6](Bezel_Language_Spec_0_98.md#sec-overloading) practical pattern for `TryGetCached(out owned conn)` had previously declared `conn: Connection` (non-nullable) and used `conn := default;` on the cache-miss path. `default(Connection)` for a non-nullable reference type is a compile error per [§17.11](Bezel_Language_Spec_0_98.md#sec-the-default-operator). v0.89 declares both overloads as `out owned conn: Connection?` (nullable), with `conn := null;` on the miss path and the caller using `c!` (the null-forgiving operator) after the bool guard.

  This is the correct pattern for any try-method with an `out owned` parameter: the parameter must be nullable because the false-return path cannot produce an owned value. Earlier versions of the spec elided this nuance; v0.89 makes it explicit in the canonical example and adds prose documenting the convention.

<a id="sec-std-lib-signature-corrections"></a>
### Std-lib signature corrections

- **`Drain()` return type harmonized.** The std-lib declarations of `Drain` for owned-element collections previously had the return type `owned IEnumerable<owned T>`, while the spec's [§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) declared `Drain(): IEnumerable<owned T>` (without the leading `owned`). The spec form is consistent with how other iterator-returning methods (e.g., LINQ's `Where`, `Select`) are typed; the iterator-method exemption ([§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) "Factory methods and sourceless returns") covers the discharge obligation without requiring the explicit `owned`. v0.89 updates the std-lib declarations to match: `Drain(): IEnumerable<owned T>` in five places (`Array<owned T>`, `LinkedList<owned T>`, `Set<owned T>`, `Stack<owned T>`, `Queue<owned T>`).

- **Stale `Dictionary<K, owned V>.Drain()` reference removed.** v0.88's prose mentioned `dict.Drain()` yielding `KeyAndOwnedValue<K, V>`, but `Dictionary<K, owned V>` does not have a `Drain()` method (the entry-record form is not expressible because records cannot have owned fields per [§28.5](Bezel_Language_Spec_0_98.md#sec-owned-fields)). The prose is corrected to point to the documented two-step "capture keys, then `PopValue` each" pattern.

- **`Queue<owned T>.Peek()` comment added.** Cosmetic: `Stack<owned T>.Peek()` carried a `// borrow of front element` comment; `Queue<owned T>.Peek()` did not. v0.89 adds the matching comment.

<a id="sec-spec-prose-corrections"></a>
### Spec prose corrections

- **[§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) indexer-set asymmetry explained.** v0.88's [§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) said the owned-element collection's indexer-set "disposes the previous value before installing the new one," while [§28.7](Bezel_Language_Spec_0_98.md#sec-reassigning-owned-fields) says owned-field assignment requires the programmer to dispose the previous value explicitly. The two rules are different by design, but v0.88 did not explain the asymmetry — both sections appeared to describe the "same" reassignment with different semantics. v0.89 adds a paragraph to [§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) explaining: the indexer setter is a method (the compiler-generated `set_Item`) whose body can include disposal logic; field assignment is a primitive operation with no method body. The two forms produce equivalent behavior; only the syntactic location of the disposal differs.

- **`move coll[i]` clarified as Array/Dictionary only.** v0.88's "move on a collection slot" subsection talked about `move coll[i]` as if it worked on any owned-element collection. Only `Array` and `Dictionary` support indexer syntax; `Stack`, `Queue`, `LinkedList`, and `Set` do not (`stack[i]` does not parse). v0.89 adds a paragraph noting the syntactic restriction, with a pointer to the dedicated take methods on each non-indexed collection (`Pop`, `Dequeue`, `Take`, etc.).

<a id="sec-rationale-corrections"></a>
### Rationale corrections

- **Stale `[§14.4](#sec-not-is-not-user-overloadable)` cross-references replaced.** v0.88's rationale [§5.10](#sec-the-move-expression-on-owned-fields) and [§5.11](#sec-owned-element-collections) contained four references of the form `[§14.4](#sec-not-is-not-user-overloadable)`, which resolved to the rationale's own [§14.4](#sec-not-is-not-user-overloadable) ("`not` is not user-overloadable"), not to the language spec's [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) ("`owned` as a generic argument"). v0.89 changes these to the prose form "language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)," which the linkify tool resolves to the spec's [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument). The same pattern appears in the historical changelogs ([§47](#sec-v0-80-to-v0-81-changelog), [§52](#sec-v0-85-to-v0-86-changelog), [§53](#sec-v0-86-to-v0-87-changelog)) for v0.75/v0.79/v0.86 era prose; those are also updated for consistency, though they document past states and are not normative for current code.

- **[§5.12](#sec-pattern-b-loosening-and-iterator-yield-refinement) added.** New rationale subsection covering the v0.89 design refinements (Pattern B loosening, iterator-yield split). Documents why both rules were over-restrictive in their earlier forms and what the relaxed rules accomplish.

<a id="sec-v0-88-to-v0-89-changelog-migration-guide"></a>
<a id="sec-migration-guide-28"></a>
### Migration guide

There is no source-level migration required.

For new code that uses `Task<owned T>`: the loosened Pattern B rule means `coll.Add(await task)` is now legal directly (where `Add` takes `owned`). Previously this required an intermediate `using var t := await task; coll.Add(t);`. The new form is cleaner; the old form remains valid.

For new code that consumes `IEnumerable<owned T>` (typically from a `.Drain()` call): the iteration variable is now owned and must be discharged in the loop body. Previously this would have been ambiguous between the borrow rule (per the old [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for)) and the owned rule (per the new [§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership)); v0.89's split rule resolves the ambiguity in favor of the owned semantics for `IEnumerable<owned T>` specifically.

For code that used `Set<owned T>.Pop` or `Set<owned T>.TryPop`: rename to `Take` and `TryTake`. The signatures otherwise are unchanged (modulo the nullable out parameter for `TryTake`).

For code that called the v0.88 `TryGetCached(out owned conn: Connection)` form: change the parameter type to `Connection?` and add a null check (or `!`) on the caller side. This was a documentation issue — the v0.88 form did not actually compile because of the `default` on the false path — so no real-world code is likely affected.

<a id="sec-v0-88-to-v0-89-changelog-status"></a>
<a id="sec-status-19"></a>
### Status

v0.89 closes the items that v0.88's fresh-look review surfaced. The substantive design refinements (loosened Pattern B, split iterator-yield rule) make v0.88's owned-element collection feature work end-to-end without the workarounds that the v0.88 examples would otherwise require. The remaining ownership-related deferred items — LINQ over owned-element collections, user-defined collection opt-in — are unchanged and remain future-revision work.

---

<a id="sec-v0-89-to-v0-90-changelog"></a>
### v0.89 to v0.90 changelog

Normative for migration; not part of the language definition.

This revision applies the seven items found in v0.89's fresh-look review. All are documentation cleanups, prose corrections, or small spec rule additions clarifying behavior that v0.89 left implicit. None are substantive design changes; no existing program changes meaning, and no new patterns are admitted that v0.89 rejected.

The point of v0.90 is to leave the v0.85→v0.89 cluster of revisions (which closed the resource-management gap that v0.85's review surfaced) in a coherent, self-consistent state before any larger work begins on the deferred items (LINQ over owned-element collections, user-defined collection opt-in).

<a id="sec-spec-text-corrections"></a>
### Spec text corrections

- **Pattern B owned-consuming-context list simplified.** v0.89's [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) rule listed five owned-consuming contexts in which an `await` of a `Task<owned T>` could appear, with the fifth bullet — "the right-hand side of `var x := await ...` where the surrounding context immediately consumes `x` as owned" — adding a non-local rule that did not fit the others. v0.90 drops the non-local bullet and recognizes that `var x := <owned expression>` is itself an owned-consuming context: it produces an owned local that the standard owned-local discharge rules then handle. The list of contexts shrinks to four bullets that all describe local properties of the AST around the `await`.

  No new patterns are admitted, and the v0.89 patterns that compiled continue to compile. The change is purely about how the rule is expressed — the standard owned-local discharge rules now do the work for the `var x := await ...` case rather than the Pattern B rule special-casing it.

- **`Drain()` implementation sketch corrected.** v0.89's [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) "Iterator yield ownership" prose described `Drain()` as implemented with `move coll[i]` per slot. That implementation would violate the field-restoration rule ([§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression)): each `move` leaves the slot in moved-from state, and the iterator's body never reassigns the slot before exiting. v0.90 corrects the prose to describe the actual implementation: `Drain()` uses the collection's `Pop`-style operations (`PopFirst`, `PopAt`, etc.), which remove the slot entirely rather than emptying it, sidestepping the field-restoration rule. The rationale [§5.12](#sec-pattern-b-loosening-and-iterator-yield-refinement) paragraph that made the same incorrect claim is also corrected.

- **`IEnumerator<owned T>.Current` semantics specified.** v0.89's [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) "Iterator yield ownership" rule said that for an iterator returning `IEnumerable<owned T>`, the consumer's iteration variable is owned and ownership transfers from the enumerator on each `MoveNext`. The mechanism by which the transfer happens is `Current`'s getter, but v0.89 didn't describe how the standard property-getter semantics (which return borrows) would yield owned values. v0.90 adds a [§29.1 subsection](Bezel_Language_Spec_0_98.md#sec-current-on-owned-element-enumerators) "`Current` on owned-element enumerators" specifying that the getter is **destructive**: the first read after a successful `MoveNext` transfers ownership and leaves the enumerator's internal slot empty; a second read without an intervening `MoveNext` is undefined behavior (concrete enumerators typically throw `InvalidOperationException`).

  The for-loop desugaring at [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) is unchanged — it reads `__enumerator.Current` exactly once per iteration, which matches the destructive-read semantics. The destructive-read is invisible to user code that uses `for` loops; only manual `MoveNext`/`Current` callers (rare) need to be aware of it.

- **`await` discharges the awaited task.** v0.89's [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) noted that "the task is disposed implicitly without disposing its result" but did not formalize this as a discharge rule for the type system. With v0.88's owned-element collections and v0.89's loosened Pattern B, code now appears in the standard library and rationale that depends on the await consuming an owned task value (the iteration variable from `taskArray.Drain()`, for instance). v0.90 adds a paragraph to [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) making the rule explicit: the await operator on a `Task<owned T>` for class T is owned-consuming with respect to its operand, and the implicit task-disposal that the runtime performs is the same operation that satisfies the type system's discharge rule.

  Two worked examples follow: the iteration-variable case (`for var task in taskArray.Drain() do results.Add(await task); end;`) and the local-variable case (`var theTask := SomeFactory(); using var result := await theTask;`). In neither case does the user need a separate `using var` to discharge the task itself — the await's implicit disposal *is* the discharge.

  The borrowed-task case (where the operand is borrowed, not owned) is also clarified: the await still disposes the task implicitly per the runtime semantics, but the discharge rule does not apply (nobody owed a discharge for it). The `using var task := ...; await task;` pattern continues to work via idempotent disposal — the await disposes first, the using-var's disposal at scope end is a no-op.

- **Owned-iteration-variable rule covers `break`, `continue`, `return`, exceptions explicitly.** v0.89's [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) iterator-yield rule (case 2) said "Falling off the body without discharging is a compile error" but did not enumerate the other exit edges. The standard owned-local discharge rules ([§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows)) already cover `break`, `continue`, `return`, `yield break`, `throw`, and throwable expressions, but the implicit application to the iteration variable was easy to miss.

  v0.90 makes the connection explicit and adds a worked example showing the standard remediation: bind the iteration variable to a `using var` immediately on entry to the loop body, converting "must discharge on every exit edge" into the simpler "scope-bound disposal." The pattern `for var conn in pool.Drain() do using var c := conn; ... end;` is the canonical form when the body has conditional control flow.

<a id="sec-indexer-set-wording-correction"></a>
### Indexer-set wording correction

- **[§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) "elides the disposal call" reworded.** The asymmetry-explanation paragraph added in v0.89 said "the compiler tracks the moved-from state and elides the disposal call" for the indexer-set on a moved-from slot. The framing implied a compile-time optimization, but the actual mechanism is runtime: the indexer setter detects the empty slot and skips the disposal step before installing the new value. v0.90 changes "compiler tracks ... elides" to "indexer setter detects the empty slot at runtime and skips the disposal step" to match the implementation.

<a id="sec-v0-89-to-v0-90-changelog-rationale-corrections"></a>
<a id="sec-changelog-rationale-corrections"></a>
### Rationale corrections

- **Stale `[§14.4](#sec-not-is-not-user-overloadable)` cross-reference fixed in [§5.12](#sec-pattern-b-loosening-and-iterator-yield-refinement).** v0.89's review caught most instances of this stale cross-reference (which resolves to the rationale's own [§14.4](#sec-not-is-not-user-overloadable) about operator overloading rather than the language spec's [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) about owned generic arguments), but missed one in [§5.12](#sec-pattern-b-loosening-and-iterator-yield-refinement) (line 326 of v0.89). v0.90 changes it to the prose form "language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)."

<a id="sec-v0-89-to-v0-90-changelog-migration-guide"></a>
<a id="sec-migration-guide-29"></a>
### Migration guide

There is no source-level migration required. All changes are documentation cleanups or rule clarifications that match the behavior v0.89 already implied; no v0.89-compiling program changes meaning under v0.90.

Users who write iterator methods returning `IEnumerable<owned T>` should now have a clear contract: `Current`'s getter is destructive, callers consume each value via `MoveNext`/`Current` exactly once per iteration. Users who write `for var x in coll.Drain() do ... end;` with conditional `break` or early `return` in the body should bind `x` to a `using var` on entry to ensure all exit edges discharge `x`; this was already the right pattern, and v0.90 documents it explicitly.

Users who depend on the awaited-task discharge in patterns like `for var task in taskArray.Drain() do results.Add(await task); end;` now have a spec-text rule to point to (rather than relying on the implicit disposal-after-await).

<a id="sec-what-is-*not*-in-v0-90"></a>
### What is *not* in v0.90

The deferred items from v0.88 — LINQ over owned-element collections, user-defined collection opt-in — remain deferred. v0.90 is a cleanup revision; the next substantive design work would be one of these.

<a id="sec-v0-89-to-v0-90-changelog-status"></a>
<a id="sec-status-20"></a>
### Status

v0.85 surfaced the resource-management gap (take-from-owned-field, take-from-owned-element). v0.86, v0.87, v0.88 closed it incrementally (`OwnedTaskCompletionSource`, `move` on fields, owned-element collections). v0.89 cleaned up the inconsistencies that the v0.88 work surfaced. v0.90 cleans up the inconsistencies that the v0.89 work surfaced.

The cluster is now self-consistent. Any future review-and-fix cycles will be working on a settled foundation.

---

<a id="sec-v0-90-to-v0-91-changelog"></a>
### v0.90 to v0.91 changelog

Normative for migration; not part of the language definition.

This revision applies the five items found in v0.90's fresh-look review. All trace to the same root cause: v0.90's iteration-rule cleanups covered the synchronous case but stopped at the sync/async boundary, leaving `IAsyncEnumerable<owned T>` and `IAsyncEnumerator<owned T>` either unmentioned or contradicted in the prose. v0.91 extends the rules uniformly to cover both forms of iteration. No design changes; no source-level migration; no v0.90-compiling program changes meaning.

<a id="sec-spec-text-generalizations"></a>
### Spec text generalizations

- **[§29.1](Bezel_Language_Spec_0_98.md#sec-the-protocol) "`Current` on owned-element enumerators" extended to cover the async case.** v0.90's new subsection was titled "`Current` on `IEnumerator<owned T>`" and covered only the synchronous interface. v0.91 renames it to "`Current` on owned-element enumerators" and generalizes the rule to cover `IAsyncEnumerator<owned T>` alongside `IEnumerator<owned T>`. The destructive-read semantics is the same: first read after a successful advance (whether `MoveNext` or `MoveNextAsync`) transfers ownership, second read without an intervening advance is undefined behavior. Both `for` and `await for` desugarings read `Current` exactly once per iteration, so the destructive-read semantics matches the loop's natural shape in both forms.

  Implementations of `IAsyncEnumerator<owned T>` may use the same two storage strategies described for the synchronous case: a private owned field that is moved out by `Current`'s getter, or transient state that exists only between the advance call and the next `Current` read.

- **[§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) iterator-yield-ownership rule case 2 extended.** v0.90's case 2 listed only "`IEnumerable<owned T>` or `IEnumerator<owned T>`" as the iterator return types that produce owned iteration variables. v0.91 extends the list to include `IAsyncEnumerable<owned T>` and `IAsyncEnumerator<owned T>`. The rule's content is otherwise unchanged: ownership transfers from enumerator to consumer at each advance, the consumer's iteration variable is owned, the standard discharge rules apply on every exit edge from the loop body (including `await for` bodies).

  The discharge-on-exit paragraph is updated to mention that the rule applies inside `await for` bodies as well as `for` bodies. The standard `using var f := <iteration variable>;` pattern on entry to the body is the canonical way to convert "must discharge on every edge" into "scope-bound disposal" in both the sync and async forms.

<a id="sec-std-lib-6-7-prose-correction"></a>
### Std-lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) prose correction

- **`OpenFiles` async-iterator example prose rewritten.** v0.90 introduced an async-iterator example (`public async method OpenFiles(paths: Array<string>): IAsyncEnumerable<owned FileStream>`) with introductory prose that described the consumer's iteration variable as borrowing for the duration of the loop body. The example's *producer* side is correct under both the v0.78 and v0.91 rules, but the *consumer* side of the prose described the v0.78 rule, contradicting the v0.91 case 2 rule (the iteration variable is owned for `IAsyncEnumerable<owned T>`).

  v0.91 rewrites the introductory prose to say the consumer takes ownership of each yielded value, and adds a consumer-side example (`public async method ProcessAll(paths: Array<string>)`) showing the canonical `using var f := file;` pattern inside the `await for` body. The producer example is unchanged.

  The cross-reference at the same location pointing at "language spec [§29.8](Bezel_Language_Spec_0_98.md#sec-iterators-with-owned-parameters)" (which is about iterators with `owned` *parameters* — a separate topic) is corrected to point at [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for)'s "Iterator yield ownership" subsection.

<a id="sec-v0-90-to-v0-91-changelog-spec-prose-corrections"></a>
<a id="sec-changelog-spec-prose-corrections"></a>
### Spec prose corrections

- **[§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) borrowed-task framing trimmed.** v0.90 introduced a paragraph describing what happens when an `await`'s operand is a borrowed `Task<owned T>` (rather than an owned one). The paragraph framed the borrowed case as "a `Task<owned T>` obtained from a pool or a shared source," inviting the reader to consider a scenario that isn't really sensible — Pattern B tasks are single-consumer by type, and sharing or pooling them is a design smell. v0.91 trims the paragraph to a single sentence noting that Pattern B tasks are single-consumer by type, the discharge rule applies whenever the operand is owned (the typical case), and the `using var task := ...; await task;` form continues to work via idempotent disposal.

- **Appendix C.3 iterator invariant extended.** The C.3 invariant added in v0.89 listed only `IEnumerable<owned T>` as the return type that produces owned iteration variables. v0.91 extends the list to include `IEnumerator<owned T>`, `IAsyncEnumerable<owned T>`, and `IAsyncEnumerator<owned T>`, matching the [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) case 2 rule. The phrase "`MoveNext`" is extended to "`MoveNext` (or `MoveNextAsync`)" in the same place.

<a id="sec-v0-90-to-v0-91-changelog-migration-guide"></a>
<a id="sec-migration-guide-30"></a>
### Migration guide

There is no source-level migration required. All changes are documentation extensions or prose corrections that match the behavior v0.90 already implied for synchronous iteration; v0.91 simply extends that to the async case as the rules clearly require.

Users who write async iterators returning `IAsyncEnumerable<owned T>` should now have a clear contract: the consumer's iteration variable is owned, must be discharged in the loop body, and the canonical pattern is `using var x := <iteration variable>;` on entry.

Users who write `await for var x in src do ... end;` over an `IAsyncEnumerable<owned T>` source should bind `x` to a `using var` immediately on entry to discharge every exit edge — same pattern as for synchronous owned-element iteration.

Users who consumed v0.90's std-lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) prose at face value (and concluded that the consumer's iteration variable from an `IAsyncEnumerable<owned T>` is a borrow) now have correct documentation: the iteration variable is owned, and a consumer-side example shows the canonical disposal pattern.

<a id="sec-what-is-*not*-in-v0-91"></a>
### What is *not* in v0.91

The deferred items from v0.88 — LINQ over owned-element collections, user-defined collection opt-in — remain deferred. v0.91 is the second cleanup-only revision in the v0.85→v0.91 cluster; the next substantive design work would be one of these.

<a id="sec-v0-90-to-v0-91-changelog-status"></a>
<a id="sec-status-21"></a>
### Status

v0.85→v0.91 is now a closed cluster: v0.85 surfaced the resource-management gap, v0.86–v0.88 filled it (`move`, owned-element collections, `WhenAllOwned`), v0.89–v0.91 cleaned up the inconsistencies the new work surfaced. The sync and async iteration rules are now uniform — every place a rule names the sync interfaces, it also names the async interfaces; every place a worked example shows the sync pattern, the async equivalent is either explicit or derivable from the parallel rule.

The cluster is self-consistent and stable. Any future review-and-fix cycles will be working on a settled foundation.

---

<a id="sec-v0-91-to-v0-92-changelog"></a>
### v0.91 to v0.92 changelog

Normative for migration; not part of the language definition.

This revision applies the seven items found in v0.91's fresh-look review. Two of them (A1, B5) trace to bugs in the linkify tool that has been generating cross-document references since the early v0.5x revisions; the rest are documentation cleanups of the same kind that the v0.85→v0.91 cluster produced. v0.92 also fixes the tool itself, retroactively correcting the bad links the old tool produced across all earlier changelogs.

No design changes; no source-level migration; no v0.91-compiling program changes meaning.

<a id="sec-linkify-tool-improvements"></a>
### Linkify tool improvements

- **Possessive cross-doc phrases now route correctly.** The previous tool recognized forward forms like "language spec §X.Y" and routed them to the spec's anchor, but it did not handle the possessive form "the language spec's §X.Y" — the possessive `'s` between the phrase and the section symbol caused the forward pattern to miss, and the bare `§X.Y` fell through to internal resolution, often producing the wrong anchor target.

  v0.92 extends the forward pattern to also match a possessive marker (`'s` or `\u2019s`, the typographic apostrophe) between the phrase and the section symbol. Forms like "the language spec's [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)," "the rationale's [§5.11](#sec-owned-element-collections)," and "design rationale's [§A.2](Bezel_Language_Spec_0_98.md#sec-source-file)" now route to the correct document's anchor. The bare and possessive forms produce equivalent links.

- **Phrase list extended with short forms.** The phrase list previously required the full phrase "language spec," "design rationale," or "standard library." v0.92 adds short forms ("spec," "rationale") so that "the spec's §X.Y" and "the rationale's §X.Y" — which appear in several changelogs as natural prose — route correctly. The longer forms remain the canonical phrasing; the short forms are accepted as a courtesy.

- **Slug generator handles markdown links in headings.** The previous slugifier left link-fragment characters (`#`, `_`, `.`) in the generated slug when a heading contained a markdown link. v0.91's "Std-lib [§6.7](...) prose correction" heading produced the anchor `sec-std-lib-[§6](#sec-nullability)-7bezel_standard_library_0_91-md#sec-task-static-helpers-prose-correction` — gibberish that referenced an unrelated rationale section and contained markdown-link syntax inside its own slug. v0.92's slugifier strips markdown link syntax first (`[text](url)` → `text`) before applying the other transformations, so the same heading now produces `sec-std-lib-6-7-prose-correction`. The v0.91 changelog heading is also rewritten to not include a markdown link (using plain "[§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers)" instead of "[§6.7](url)").

<a id="sec-retroactive-corrections-from-tool-fixes"></a>
### Retroactive corrections from tool fixes

The improved tool was run against the v0.92 documents, replacing previously-mis-routed possessive-form links with correct ones. The most visible corrections:

- **Rationale [§47](#sec-v0-80-to-v0-81-changelog)** ("v0.80 to v0.81 changelog"). A v0.55-era prose mention "the language spec's [§3.1](Bezel_Language_Spec_0_98.md#sec-type-kinds)" pointed to the rationale's [§3.1](#sec-always-semicolons) ("Always semicolons" — about syntax) rather than the spec's [§3.1](Bezel_Language_Spec_0_98.md#sec-type-kinds) ("Type kinds" — about primitive value types vs class types). The rationale prose was specifically about type classification, which is the spec's [§3.1](Bezel_Language_Spec_0_98.md#sec-type-kinds). The corrected link points to the spec.

- **Rationale [§55](#sec-v0-88-to-v0-89-changelog)** (v0.88 to v0.89 changelog). Three prose mentions of "the language spec's [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)" pointed to the rationale's [§14.4](#sec-not-is-not-user-overloadable) ("`not` is not user-overloadable" — about operator overloading) rather than the spec's [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) ("`owned` as a generic argument"). The corrected links point to the spec. The historical mention "the rationale's own [§14.4](#sec-not-is-not-user-overloadable)" continues to point to the rationale, which is correct.

- **Rationale [§56](#sec-v0-89-to-v0-90-changelog)** (v0.89 to v0.90 changelog). Two prose mentions of "the language spec's [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)" had the same problem. Corrected to point to the spec.

- **Rationale [§49](#sec-v0-82-to-v0-83-changelog)** (v0.82 to v0.83 changelog). One prose mention of "the language spec's [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer)" — already correct because [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer) is unambiguous (the rationale has no [§28.8](Bezel_Language_Spec_0_98.md#sec-ownership-transfer)), but the link target was a different slug than the corrected version. Now uses the canonical spec slug.

- **Rationale [§44](#sec-v0-77-to-v0-78-changelog)** (v0.77 to v0.78 changelog). One prose mention of "the language spec's [§32.8](Bezel_Language_Spec_0_98.md#sec-callbacks)" was correct in target (the spec's [§32.8](Bezel_Language_Spec_0_98.md#sec-callbacks)) but went through the internal-fallback path, producing an internal-style link. Now uses the cross-document form.

In total, seven previously-mis-routed possessive-form links were corrected. None of the corrections affect normative content — all were inside historical changelogs documenting past work — but the corrections improve navigability for readers following the design's evolution.

<a id="sec-v0-91-to-v0-92-changelog-spec-text-corrections"></a>
<a id="sec-changelog-spec-text-corrections"></a>
### Spec text corrections

- **Spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) stale cross-reference.** A v0.78-era reference reading "see [§14.4](#sec-not-is-not-user-overloadable) of the design rationale for the proposed direction" had two independent problems: the label said "[§14.4](#sec-not-is-not-user-overloadable)" but the rationale's [§14.4](#sec-not-is-not-user-overloadable) is unrelated to user-type opt-in (it covers "`not` is not user-overloadable"); and the target slug was `sec-the-move-expression-on-owned-fields` (the rationale's [§5.10](#sec-the-move-expression-on-owned-fields) about move on owned fields), also not the intended target. The intended target is rationale [§5.11](#sec-owned-element-collections)'s discussion of user-defined types not being able to opt in to owned-element semantics yet. v0.92 changes the reference to rationale [§5.11](#sec-owned-element-collections) directly, with a parenthetical pointing to the specific subsection.

- **[§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) owned-consuming-context list extended with `yield return`.** The Pattern B owned-consuming-context list at [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) enumerated five contexts (`using var`, `var`, indexer-set/field-assign, `owned` parameter, `return`) but did not include `yield return` in an iterator method whose return type is `IEnumerable<owned T>`, `IEnumerator<owned T>`, `IAsyncEnumerable<owned T>`, or `IAsyncEnumerator<owned T>`. The std-lib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers) `OpenFiles` example uses exactly this pattern (`yield return await OpenFileAsync(path);` from a method returning `IAsyncEnumerable<owned FileStream>`), so the list's omission made the example non-conforming under a literal reading. v0.92 adds the `yield return` bullet to the list with a cross-reference to [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for)'s "Iterator yield ownership" subsection that defines the ownership transfer.

- **[§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) follow-up paragraph extended for all four interfaces.** v0.91 extended case 2 of the iterator-yield rule to cover the four interfaces but only updated the bullet itself, not the paragraph that follows. The paragraph mentioned only `IEnumerable<owned T>` in the "for iterators returning..." clause. v0.92 extends the paragraph to list all four interfaces, matching case 2's coverage. The `MoveNext` reference becomes "`MoveNext` (or `MoveNextAsync`)" in the same paragraph.

- **[§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) enumerator-disposal prose mentions async.** A sentence about disposing the enumerator on `break` or exhaustion mentioned only `for` loops and `Dispose()`. v0.92 extends the parenthetical to `for` or `await for` loops and `Dispose()`/`DisposeAsync()` on a manually-held enumerator.

- **[§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) borrowed-task framing trimmed further.** v0.91's already-trimmed paragraph still framed the borrowed-Pattern-B-task case with "the rare case the operand is borrowed (perhaps obtained from a method that returns a borrow of a task from internal storage)." The speculative parenthetical invited readers to consider an unusual scenario that isn't an idiomatic Pattern B pattern. v0.92 drops the parenthetical, stating the rule abstractly: "When the operand is borrowed, the await still disposes the task implicitly but no discharge is owed."

<a id="sec-v0-91-to-v0-92-changelog-migration-guide"></a>
<a id="sec-migration-guide-31"></a>
### Migration guide

There is no source-level migration required.

For authors writing prose: cross-document references in possessive form (`the language spec's §X.Y`, `the rationale's §5.11`) now route correctly without needing to be rewritten to bare form (`language spec §X.Y`). The bare form is still preferred as canonical phrasing; the possessive form is admitted for natural prose flow.

For authors writing headings: avoid embedding markdown links inside heading text. The improved slug generator handles them correctly, but the resulting slug excludes the link's content — which may not match the heading's visible text. Plain prose in headings is the cleanest convention.

For readers following changelog history: several wrong-target links in v0.55–v0.91 changelogs have been retroactively corrected by the v0.92 tool. The corrections are silent (no error message previously; no error message now); they affect only navigation, not normative content.

<a id="sec-what-is-*not*-in-v0-92"></a>
### What is *not* in v0.92

The deferred items from v0.88 — LINQ over owned-element collections, user-defined collection opt-in — remain deferred. v0.92 is the third cleanup-only revision in the v0.85→v0.92 cluster; the next substantive design work would be one of these.

<a id="sec-v0-91-to-v0-92-changelog-status"></a>
<a id="sec-status-22"></a>
### Status

v0.85→v0.92 is now a closed cluster, with v0.92 as the final cleanup pass. The cluster's narrative:

- v0.85 surfaced the resource-management gap.
- v0.86 closed std-lib gaps (`OwnedTaskCompletionSource`).
- v0.87 added the `move` expression on owned fields.
- v0.88 admitted owned-element collections to language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s opt-in list.
- v0.89 loosened Pattern B and split the iterator-yield rule.
- v0.90 specified destructive `Current`, made discharge requirements explicit on every exit edge.
- v0.91 extended the new rules to the async iteration interfaces.
- v0.92 cleaned up tool bugs that were silently mis-routing cross-document links, plus residual sync/async parity gaps in supporting prose.

All rules apply uniformly to sync and async iteration. All cross-document references route to their intended targets. The cluster is self-consistent.

The remaining work in the resource-management area (LINQ-over-owned, user-type opt-in) is design work — not cleanup. Either is appropriate for the next substantive revision.

---

<a id="sec-v0-92-to-v0-93-changelog"></a>
### v0.92 to v0.93 changelog

Normative for migration; not part of the language definition.

This revision applies the six items found in v0.92's broader fresh-look review — a pass that examined sections outside the resource-management cluster (slicing, fixed-size arrays, FFI examples, `case` arm ergonomics) and surfaced issues that had been accumulating in adjacent territory. v0.93 closes them.

The largest of these is the decision on block expressions — an "ergonomic gap" the spec had been carrying since the early revisions, anticipating a future revision that never came. v0.93 closes the question by deciding not to add the construct; the prose in [§19.2](Bezel_Language_Spec_0_98.md#sec-arms) is reframed accordingly, and rationale [§5.13](#sec-no-block-expressions) documents the reasoning.

No design changes to existing rules; one new spec-level restriction (slicing on owned-element collections is rejected); the rest are documentation corrections.

<a id="sec-spec-text-additions-and-corrections"></a>
### Spec text additions and corrections

- **Slicing on owned-element collections is rejected.** [§15.5](Bezel_Language_Spec_0_98.md#sec-slicing) previously described slicing's copy-returning semantics without addressing how the rule interacts with owned-element collections. v0.93 adds an explicit paragraph: `arr[a..b]` on an `Array<owned T>` is a compile error. The copy-returning semantics would duplicate references to owned values into a fresh array, manufacturing a second owner for each element. Users who want a range-bounded ownership transfer use `Drain()` plus filtering or a sequence of `PopAt` calls; users who want a borrowing view iterate the index range explicitly. A forward reference notes that the future `Span<T>` work is expected to admit `Span<T>` over `Array<owned T>` as a borrowing view.

- **`T[]` reclamation semantics clarified.** Three sections of v0.92 described `T[]` differently: [§8.7](Bezel_Language_Spec_0_98.md#sec-dispose-is-not-on-object) said `Dispose()` is class-only (so `T[]` has none); [§28](Bezel_Language_Spec_0_98.md#sec-resource-management)'s disposal table grouped `T[]` and `Array<T>` together as "class types" with Dispose releasing storage; [§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) listed runtime-managed types but did not include `T[]`. The result: `T[]` was reference-typed, had no `Dispose()`, and wasn't runtime-managed — three positions that couldn't all be right.

  v0.93 places `T[]` explicitly in the runtime-managed list ([§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)), with a paragraph explaining the implementation-defined reclamation mechanism (the same as the one used for trivial-dispose class instances; no user code runs at reclamation). The [§28](#sec-v0-61-to-v0-62-changelog) disposal table is split: `T[]` gets its own row ("No — runtime-managed; no `Dispose()`; implementation-defined reclamation"); `Array<T>` is in a separate row covering it and the other resizable collection types.

  The `owned T[]` annotation that appears on some std-lib return types (e.g., `IEnumerable<T>.ToArray(): owned T[]`) is also addressed: when `owned` applies to a `T[]` return, it documents that the array is a fresh allocation (not a borrowed view), but the compile-time discharge obligation that `owned` carries on class-typed returns is degenerate for `T[]`. The convention is documentation, not enforcement. This matches the std lib's existing practice without introducing new rules.

- **[§22.4](Bezel_Language_Spec_0_98.md#sec-inline-out-declarations) / [§22.6](Bezel_Language_Spec_0_98.md#sec-overloading) contradiction on out-parameter call-site matching fixed.** v0.92's [§22.4](Bezel_Language_Spec_0_98.md#sec-inline-out-declarations) said "`out var` at the call site of an `out owned` parameter produces an owned local," but [§22.6](Bezel_Language_Spec_0_98.md#sec-overloading) said the call-site syntax must match the parameter modifier — `out var` is rejected at the call site of an `out owned` parameter because "the resulting value's ownership would have nowhere to go." [§22.6](Bezel_Language_Spec_0_98.md#sec-overloading)'s stricter rule is the right one. v0.93 rewrites the offending [§22.4](Bezel_Language_Spec_0_98.md#sec-inline-out-declarations) sentence to match [§22.6](Bezel_Language_Spec_0_98.md#sec-overloading): the accepted forms at the call site of an `out owned` parameter are `out using var x` and `out owned var x`; plain `out var x` is not accepted.

- **FFI `getenv` example replaced with `strdup`.** v0.92's [§32.1](Bezel_Language_Spec_0_98.md#sec-the-extern-block) example declared `getenv(name: CString): CString?`, implying the returned `CString` is caller-owned. But `getenv` semantically returns a pointer to the process environment that the caller must not free — Bezel code using the declaration and binding the result to `using var` would attempt to free the environment string, which is wrong. (The same function was correctly declared with `IntPtr` return at [§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling), showing the borrowed-pointer pattern.) v0.93 replaces the [§32.1](Bezel_Language_Spec_0_98.md#sec-the-extern-block) example's `getenv` with `strdup`, which genuinely returns a caller-owned allocation. The [§32.6](Bezel_Language_Spec_0_98.md#sec-string-marshalling) example is unchanged.

<a id="sec-reframing-the-block-expression-decision"></a>
### Reframing the block-expression decision

- **[§19.2](Bezel_Language_Spec_0_98.md#sec-arms) "ergonomic gap" prose reframed.** v0.92's [§19.2](Bezel_Language_Spec_0_98.md#sec-arms) framed the lack of a value-returning `do ... end` form as a "known ergonomic gap" and anticipated a future revision adding **block expressions**. v0.93 closes the question: the helper-method extraction is *the* idiom for multi-statement arm bodies in expression-position `case`, and the language does not provide a value-returning block form. The [§19.2](Bezel_Language_Spec_0_98.md#sec-arms) prose is rewritten accordingly — the "known gap" / "future revision" language is removed, and the helper extraction is framed as good design with its own merits (gives the logic a name, localizes side effects).

  A new rationale subsection [§5.13](#sec-no-block-expressions) ("No block expressions") documents the reasoning. Four candidate designs were considered — Rust-style trailing-expression-without-semicolon, `yield expr;`, a new `expr ... end` keyword with scope-local `return`, and a `give expr;` statement inside `do ... end`. Each option had a cost we judged larger than the cost of the helper-method extraction it would replace: the Rust form fights the mandatory-semicolon rule; `yield` is taken by iterator methods and reusing it creates resolution ambiguity; a new keyword expands the language for a use case extraction already covers; a `give` statement creates a third interpretation of `do ... end`, partially undoing the consistency the current design has. The decision is to leave `do ... end` with its single meaning (statement-sequencing block, type `()`).

<a id="sec-v0-92-to-v0-93-changelog-migration-guide"></a>
<a id="sec-migration-guide-32"></a>
### Migration guide

There is no source-level migration required for any of the spec text changes; v0.92-compiling programs continue to compile and behave the same way under v0.93.

The one substantive new rule — slicing on owned-element collections is rejected — affects programs that attempt `arr[a..b]` on an `Array<owned T>`. Such programs did not compile under v0.92 either, because the spec did not specify the rule and the std-lib implementation of `Array<owned T>.[Slice]` does not exist. v0.93 promotes the implicit-behavior-via-omission to an explicit compile error with a diagnostic.

For programs reading the spec to understand `T[]`'s status: v0.93 makes `T[]` explicitly runtime-managed. Programs already treat `T[]` this way in practice (no `using var`, no `owned` field tracking, no explicit `Dispose()` calls); v0.93 documents the behavior that v0.92 implementations were necessarily following.

For programs that took the v0.92 [§19.2](Bezel_Language_Spec_0_98.md#sec-arms) "future revision" prose seriously and were waiting for block expressions: there will not be a block-expression construct in Bezel. The helper-method or local-lambda extraction is the long-term pattern.

<a id="sec-what-is-*not*-in-v0-93"></a>
### What is *not* in v0.93

The deferred items from v0.88 — LINQ over owned-element collections, user-defined collection opt-in — remain deferred. v0.93 is a cleanup revision adjacent to the v0.85→v0.92 cluster; the next substantive design work would still be one of these.

<a id="sec-v0-92-to-v0-93-changelog-status"></a>
<a id="sec-status-23"></a>
### Status

v0.85→v0.92 closed the resource-management gap. v0.93 closes a small set of issues adjacent to that cluster — slicing's interaction with owned arrays, `T[]`'s classification within the new disposal taxonomy, a method-call-site documentation contradiction, an FFI example using the wrong function name — plus the long-standing block-expression question.

The cluster v0.85→v0.93 is now stable and self-consistent. Future revisions will be working on a settled foundation.

<a id="sec-v0-93-mid-revision-refinements"></a>
### v0.93 mid-revision refinements

A subsequent review of v0.93 surfaced six items, mostly stale prose that the v0.85→v0.92 implementation work had outpaced. All are documentation corrections; no design changes, no source-level migration required.

- **[§18](#sec-things-deferred) deferred list refreshed.** The entry titled "`owned` as a generic argument on types beyond `Task<T>`" claimed three sub-items as deferred — `IAsyncEnumerable<owned T>`, `Task.WhenAll` over `Array<Task<owned T>>`, and owned-element collections more broadly — all of which v0.88–v0.91 actually shipped. The bullet is replaced with the work that *is* still deferred: a declaration-level opt-in for user-defined generic types (the std-lib types are admitted; user types are not). A separate **LINQ over owned-element collections** bullet is added alongside, since the v0.88–v0.92 status sections repeatedly flag it as the remaining ownership-area design item.

- **[§18](#sec-things-deferred) tuples entry added.** Tuples were referenced ≥4 times across the spec, stdlib, and rationale (rationale [§5.11](#sec-owned-element-collections), stdlib [§4.4](Bezel_Standard_Library_0_98.md#sec-dictionaryk-v) "owned-value form," and elsewhere) as a future-revision feature that would unblock specific dictionary-drain and multi-return cases, but never appeared in the deferred list itself. Added as an explicit entry naming the open design questions (structural vs. nominal identity; positional vs. named fields; interaction with the owned-element record gap).

- **[§5.3](#sec-collections-do-not-own-their-elements) reframed.** The original "Collections never own elements (chosen)" framing predated v0.88's owned-element collections and read as a categorical statement that v0.88 directly contradicts. The body is extended with a closing paragraph noting that v0.88 reopened option 3 in a narrower form — non-owning remains the default and existing code is unchanged, but `Array<owned T>` etc. now coexist alongside — with a forward pointer to [§5.11](#sec-owned-element-collections). The section title is preserved (the original wording remains accurate for the *default*) so that inbound cross-references continue to resolve.

- **Post-condition loops (`do-while`) moved from [§17](#sec-things-explicitly-rejected) rejected to [§18](#sec-things-deferred) deferred.** The construct's open design questions are syntactic, not categorical: a literal `do ... while (cond);` form clashes with [§21.6](Bezel_Language_Spec_0_98.md#sec-do-end-blocks)'s `do ... end` blocks, but a Pascal/Modula-2 `repeat ... until cond;` form would fit Bezel's overall vocabulary. The decision is not whether the construct is useful but which spelling minimizes confusion; deferring keeps the option open without committing to a syntax. With the move, the remaining [§17](#sec-things-explicitly-rejected) entries collapse from ten subsections to nine — combined visibility, readonly local bindings, and variadic parameters shift up by one numerical position. Historical changelog entries that referenced the prior numbers were rewritten to identify their targets by slug rather than by number, so renumbering does not invalidate the historical record.

- **Spec [§30.3](Bezel_Language_Spec_0_98.md#sec-the-task-and-taskt-types) `Task` snippet given a `destructor;` line.** [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) says `Task` is non-trivial-dispose (which is why `_ := someAsyncCall();` is rejected), but [§30.3](Bezel_Language_Spec_0_98.md#sec-the-task-and-taskt-types)'s class declaration showed no destructor — a reader of [§30.3](Bezel_Language_Spec_0_98.md#sec-the-task-and-taskt-types) alone would conclude `Task` is trivial-dispose per [§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)'s rule. Added a `destructor;` documentation shorthand (per stdlib [§1](Bezel_Standard_Library_0_98.md#sec-conventions)'s convention) with an inline comment pointing at [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_); a follow-up paragraph explains that the destructor body is provided by the runtime.

- **Spec [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) disposal-obligations table extended with an interface-typed row.** [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) discusses why discard on an interface-typed expression is always rejected (a future implementation may carry a non-trivial destructor, even when every visible implementation today is trivial-dispose), but [§28](Bezel_Language_Spec_0_98.md#sec-resource-management)'s summary table omitted interface types entirely. Added a row matching the [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_) rule.

- **One pre-existing wrong-target link in the v0.57 historical changelog corrected.** A bullet describing "`Type` identity for generics specified" linked to the rationale's `sec-variadic-parameters-params` (variadic parameters) when it should have linked to spec [§17.10](Bezel_Language_Spec_0_98.md#sec-the-typeof-operator) (the `typeof` operator). The label/target mismatch predated this review; fixed at the same time.

- **Linkifier filenames updated from v0.92 to v0.93.** The `DOCS` dictionary in `linkify_refs.py` referenced the prior version's filenames; updated to the current ones.

- **Linkifier historical-suppression rule added.** A separate tool-side improvement, motivated by the do-while move above: the linkifier previously converted every bare `§X.Y` reference in prose into a markdown link pointing at whatever currently sits at that section number. When the surrounding prose discussed a *historical* number (one that has since been renumbered or removed) — for example, parenthetical notes like "(was §17.10)" or "originally numbered §17.8" — the auto-link produced a label/target mismatch, routing the reader to the new occupant of that slot. The tool now scans the 60 characters immediately preceding each bare `§X.Y` reference for tight historical-context markers — `originally`, `previously`, `formerly` (with optional intermediate words: `originally-titled`, `previously numbered`), `renumbered from`, `(was`, `, was`, `old`, `former`, `prior` — and leaves the reference as plain text when any is present. Range continuations are recognized too: `old §17.7–§17.10` suppresses both endpoints rather than just the first. Verbs whose meaning is ambiguous (`moved`, `renumbered to`) are deliberately excluded — they can legitimately precede a *current* reference (e.g., "moved to [§18](#sec-things-deferred)"). For edge cases the heuristic misses, authors can write `&sect;17.8` (the HTML entity), which renders identically to `§17.8` but does not match the regex. The rule is idempotent: a suppressed reference stays plain on every subsequent run.

- **Spec [§24.3](Bezel_Language_Spec_0_98.md#sec-the-type-of-an-event-member) `Add`/`Remove` visibility corrected from `internal` to `public`.** The class declarations for `EventSource<T>` and `AsyncEventSource<T>` showed `internal method Add` and `internal method Remove`, contradicting both (a) the stdlib [§6.5](Bezel_Standard_Library_0_98.md#sec-events-ievent-eventsource) declarations (which have always been `public`, with a "type fence" justification — the `EventSource<T>` reference is unreachable from outside the declaring class because the field type from outside is the interface), and (b) [§24.3](Bezel_Language_Spec_0_98.md#sec-the-type-of-an-event-member)'s own surrounding text stating "the split is type-driven, not a special access-control rule." The `internal` form was also functionally inconsistent with the rewriting rule in the same section: `event += handler` is rewritten by the compiler to `event.Add(handler)` in *user code inside the event's declaring class* — but user-declared events live outside `System.Threading`, so an `internal Add` would be inaccessible there. The fix aligns the spec with the stdlib's existing `public` declarations.

- **Spec Appendix [§C.3](Bezel_Language_Spec_0_98.md#sec-appendix-c-resource-management) interface-discard rule generalized to match [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_).** The Appendix said the `_ := expression;` discard is "rejected for non-trivial-dispose class types, for `Task` and `Task<T>`, and for interface types that extend `IDisposable`" — narrower than [§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_)'s actual rule, which rejects discard on *all* interface types unconditionally. ([§17.13](Bezel_Language_Spec_0_98.md#sec-the-discard-_)'s reasoning: a conforming implementation added later in another module may carry a non-trivial destructor, so accepting "this interface doesn't extend `IDisposable` today" creates a source-breaking-change waiting to happen. The [§28](Bezel_Language_Spec_0_98.md#sec-resource-management) disposal-obligations table added in an earlier refinement already used "rejected unconditionally" for interface-typed locations; Appendix [§C.3](Bezel_Language_Spec_0_98.md#sec-appendix-c-resource-management) was the lone holdout.) Appendix [§C.3](Bezel_Language_Spec_0_98.md#sec-appendix-c-resource-management) now states the rule the same way as the two normative sections that already cover it.

- **Spec [§3.1](Bezel_Language_Spec_0_98.md#sec-type-kinds) and [§3.2](Bezel_Language_Spec_0_98.md#sec-reference-vs-value-types) interface-substitution descriptions extended to mention boxed enums.** Both sections listed the possible runtime values of an interface-typed variable as "a class instance, a boxed record, or a boxed primitive" — omitting enums even though enums are value types ([§3.1](Bezel_Language_Spec_0_98.md#sec-type-kinds)), can implement interfaces ([§10.5](Bezel_Language_Spec_0_98.md#sec-restrictions)), and are boxed when used in interface-typed positions per the same rule as records and primitives ([§8](Bezel_Language_Spec_0_98.md#sec-the-object-class)'s preamble already listed "records, enums, primitives, and the unit type" as the boxable categories). Added "boxed enum" to both list-form descriptions.

These refinements leave the document set internally consistent: every cross-document `sec-*` anchor reference (168 unique in the rationale, 39 in the stdlib, 15 in the spec) resolves; every internal `sec-*` link resolves; the linkifier is idempotent on a second run.

---

<a id="sec-v0-93-to-v0-94-changelog"></a>
### v0.93 to v0.94 changelog

Normative for migration; not part of the language definition.

This revision applies the findings from v0.93's fresh-look review — a 36-finding pass that examined every section of the spec, stdlib, and rationale and surfaced a coherent set of issues. The largest piece of work is the owned-element collections cluster: four interrelated findings (interface conformance for `Array<owned T>` and its siblings; a missing `IReadOnlyCollection<T>` declaration; the Dictionary-entry-conformance contradiction) all trace to the same root cause and are resolved together. The remaining items are smaller — clarifications, rule-generalizations, stale-wording fixes, and grammar polish.

<a id="sec-owned-element-collections-cluster"></a>
### Owned-element collections cluster

Four findings about the interaction between owned-element collections and the interface hierarchy are resolved together. They share a root cause: the v0.93 declarations conferred mutating-interface conformance (`IList<T>`, `ICollection<T>`) on `Array<owned T>` and its siblings, contradicting language spec [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements)'s explicit "borrowing-iteration interfaces only" rule and rationale [§5.11](#sec-owned-element-collections)'s design intent.

- **`IReadOnlyCollection<T>` added to the standard library.** The interface was referenced by language spec [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements) ("the borrowing-iteration interfaces ... `IReadOnlyCollection<T>`") and by stdlib [§4.4](Bezel_Standard_Library_0_98.md#sec-dictionaryk-v)'s `Dictionary.Keys`/`Dictionary.Values` return types, but never declared. v0.94 declares it in stdlib [§4.7](Bezel_Standard_Library_0_98.md#sec-ireadonlycollectiont-ireadonlylistt-and-ilistt):

  ```bezel
  public interface IReadOnlyCollection<out T>: IEnumerable<T>
    readonly property Count: int;
  end IReadOnlyCollection;
  ```

  The interface is admitted to language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s opt-in list, so `IReadOnlyCollection<owned T>` is well-typed (consistent with the existing `IReadOnlyList<owned T>` admission).

- **`IReadOnlyList<T>` reshaped to extend `IReadOnlyCollection<T>`.** Previously `IReadOnlyList<out T>: IEnumerable<T>` declared `Count` and the indexer; now `IReadOnlyList<out T>: IReadOnlyCollection<T>` inherits `Count` and declares only the indexer. The change matches C#'s analogous hierarchy and tightens Bezel's interface structure: the read-only interfaces now form a clean chain `IReadOnlyList<T>: IReadOnlyCollection<T>: IEnumerable<T>`, parallel to the mutating chain `IList<T>: ICollection<T>: ...`. Existing code referring to `IReadOnlyList<T>.Count` continues to work; the property is inherited rather than declared.

- **`ICollection<T>` reshaped to extend `IReadOnlyCollection<T>`.** Same move on the mutating side. Previously `ICollection<T>: IEnumerable<T>` declared `Count`; now `ICollection<T>: IReadOnlyCollection<T>` inherits `Count`. `IList<T>: IReadOnlyList<T>, ICollection<T>` now receives `Count` through two inheritance paths (each ending at `IReadOnlyCollection<T>`), satisfying the diamond-resolution rule with a single property declaration. Language spec [§11.2](Bezel_Language_Spec_0_98.md#sec-interface-declaration)'s diamond-resolution discussion is updated to mention this case.

- **Owned-element collections declare only borrowing-iteration interfaces.** This is the central fix. The v0.93 declarations had the wrong inheritance:

  | v0.93 declaration | v0.94 declaration |
  |---|---|
  | `Array<owned T>: IList<T>, IReadOnlyList<T>` | `Array<owned T>: IReadOnlyList<T>` |
  | `LinkedList<owned T>: ICollection<T>` | `LinkedList<owned T>: IReadOnlyCollection<T>` |
  | `Set<owned T>: ICollection<T>` | `Set<owned T>: IReadOnlyCollection<T>` |
  | `Stack<owned T>: ICollection<T>` | `Stack<owned T>: IReadOnlyCollection<T>` |
  | `Queue<owned T>: ICollection<T>` | `Queue<owned T>: IReadOnlyCollection<T>` |
  | `Dictionary<K, owned V>: ICollection<DictionaryEntry<K, V>>` | `Dictionary<K, owned V>: IReadOnlyCollection<DictionaryEntry<K, V>>` |

  The new declarations match language spec [§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements)'s authoritative rule. The mutating-interface members (`IList<T>.Add(item: T)`, `ICollection<T>.Add(item: T)`, etc.) took borrows of T, which an owned-element collection cannot satisfy without breaking the ownership-transfer contract. The previous declarations attempted to handwave this with prose ("the implementation rejects them by inheriting them in a way that requires owned-call form"), but no language-level rule supports such partial conformance — interface members must match exactly per [§11.2](Bezel_Language_Spec_0_98.md#sec-interface-declaration). Restricting the inheritance to the read-only interfaces makes the contract honest: when a consumer holds an `Array<owned T>` as `IReadOnlyList<T>`, only reading is reachable; mutation requires the concrete class type, which exposes the owned-aware mutation surface.

  The Dictionary case is the same fix scaled down: `Dictionary<K, owned V>`'s inherited `Add(item: DictionaryEntry<K, V>)` would have been an entry-typed mutating-add with a borrowed value, incoherent with the owned-value contract. The borrowing-only `IReadOnlyCollection<DictionaryEntry<K, V>>` view preserves iteration (entries with borrowed values) without manufacturing an unsatisfiable Add.

  Code that relied on the spurious mutating conformance — `someOwnedArray as IList<T>`, `someOwnedDict as ICollection<DictionaryEntry<K, V>>`, or assignment of owned-element collections to mutating-interface-typed variables — is rejected in v0.94 with a type-conversion error. The migration is to use the concrete type for mutation, or one of the borrowing interfaces (`IEnumerable<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`) for read-only iteration.

  The implementations of `Array<owned T>` etc. continue to behave as before; only the declared interface conformance changes. The Drain pattern, the `move coll[i]` form, and every other owned-element-specific operation remain on the concrete class.

<a id="sec-moves-throwable-expression-list-spec-28-8-1-corrected"></a>
### `move`'s throwable-expression list (spec [§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression)) corrected

The list of throwable expressions used by the field-restoration analysis ([§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression)) had three problems:

- **`as` was listed as throwable** ("`as` (which throws `InvalidCastException` on mismatch)"), contradicting language spec [§12.7](Bezel_Language_Spec_0_98.md#sec-type-tests-is-is-not-as) and [§17.4](Bezel_Language_Spec_0_98.md#sec-the-is-is-not-and-as-operators), which both state that a failed `as` cast returns `null` rather than raising. The analysis was over-conservative: a program using `move x.f` followed by `something as T` between the move and the field-restoration point was forced to restructure unnecessarily. v0.94 removes `as` from the throwable list, with an explicit follow-up sentence stating that `as` never throws.

- **Explicit casts in function-call form were omitted.** `short(x)` and `int(d)` can throw `OverflowException` for numeric narrowing ([§5.2](Bezel_Language_Spec_0_98.md#sec-explicit-conversions)); `T(n)` on a `T?` value where `n` is null throws `NullReferenceException` ([§5.3](Bezel_Language_Spec_0_98.md#sec-nullable-casts)); integer-to-enum casts throw `OverflowException` for out-of-range values ([§5.4](Bezel_Language_Spec_0_98.md#sec-enum-casts)). All three forms can throw, and all three were missing from the analysis's throwable list. v0.94 adds them explicitly, with cross-references to the originating sections.

- **"Indexer accesses on collection types" was ambiguous about fixed-size arrays.** Fixed-size arrays `T[]` are intrinsic ([§15.1](Bezel_Language_Spec_0_98.md#sec-fixed-size-arrays)), not "collection types" in the class-type sense, but their indexer can throw `IndexOutOfRangeException`. The v0.93 phrasing left the reader to guess whether `T[]` indexing was covered. v0.94 names `T[]` explicitly alongside the resizable collection classes and user-declared indexers.

The corrected list disambiguates each case; the safety property of the analysis is unchanged for code that doesn't use `as` (still conservative), and slightly relaxed for code that does (no longer treats `as` as a synthetic exception edge).

<a id="sec-appendix-c-6-pattern-a-rule-aligned-with-30-5"></a>
### Appendix [§C.6](Bezel_Language_Spec_0_98.md#sec-async) Pattern A rule aligned with [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal)

The summary in Appendix C.6 had retained pre-v0.90 wording: "A bare `var x := await someTask;` whose result is class-typed and whose task is not held in `using var` is rejected; one of Pattern A or Pattern B is required." Per v0.90, [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) loosened the rule: `var x := await someTask;` compiles for Pattern B (`x` is then an owned local subject to standard discharge rules), and Pattern B's call site need only be an owned-consuming context (not necessarily `using var` specifically). The Appendix's bullet contradicted the normative section.

v0.94 rewrites the bullet to describe Pattern A's specific requirement (the task must be held in `using var`) and defers to [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) for the admitted Pattern A shapes. The next bullet about Pattern B was already correct (it lists "any owned-consuming context") and is unchanged.

<a id="sec-pattern-as-admitted-task-expression-shapes-enumerated-30-5"></a>
### Pattern A's admitted task-expression shapes enumerated ([§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal))

v0.93's Pattern A rule said only "the task must be held in a `using var` (or `await using var`)" and rejected "a bare `var x := await someTask;` ... not held in a `using var`." Two cases were left underspecified:

- **Expression-position awaits like `Process(await Producer())`** — is this admissible? The task is a temporary; there's no `using var`. The rule didn't say.
- **Return-position awaits like `return await Producer()`** in an async method whose return type is also Pattern A — same question.

v0.94 closes the gap by enumerating Pattern A's admitted task-expression positions, parallel to Pattern B's owned-consuming-context list. The admitted positions are: a `using var` (or `await using var`)-bound local, an `owned` parameter, an `owned` field of `this`, or a producer call directly inside `return` of an async method whose return type is Pattern A `Task<U>` for class U. Any other awaited-task expression is a compile error.

The principle is that the awaited task's lifetime must be syntactically visible at the await site so the borrow lifetime rules of [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) can bound the borrowed result. Expression-position temporaries (`Process(await Producer())`) are rejected because the temporary's lifetime ends with the enclosing expression, but the borrow may live longer; the fix is to introduce a `using var` for the task.

The enumeration also describes where the awaited *result* (the borrow) may flow — non-owned locals, non-owned arguments, matching Pattern A returns — and where it may not: a `using var` binding (using-var requires owned, not a borrow), an `owned` field, an `owned` parameter, or an `owned T` return position. The new wording is normatively complete; the diagnostic surface improves correspondingly.

<a id="sec-as-and-ownership-clarified-17-4-28-3"></a>
### `as` and ownership clarified ([§17.4](Bezel_Language_Spec_0_98.md#sec-the-is-is-not-and-as-operators), [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows))

v0.93's [§17.4](Bezel_Language_Spec_0_98.md#sec-the-is-is-not-and-as-operators) said `x as T` "preserves the ownership state of `x`: if `x` is owned, the result is owned." But the result type of `as` is always nullable, and `null` cannot carry ownership — what happens to ownership when the cast fails was left implicit. The natural reading (the cast destructively moves from `x` on success, but not on failure) would require a conditional ownership-state-of-`x` rule that the type system isn't equipped to express.

v0.94 picks the cleaner design: `as` is **non-destructive on its operand**, matching `is`. The result is always a borrow with the same source as `x`, regardless of whether `x` is owned or borrowed; `x` retains its original ownership state after the cast. To take ownership across a downcast, the user moves from `x` into a target-typed owned location after narrowing — this is the natural pattern and aligns with how `is`-based narrowing already works.

The producer table in [§28.3](Bezel_Language_Spec_0_98.md#sec-local-variables-ownership-and-borrows) is updated correspondingly: the `x as T` row reads "borrowed" unconditionally, with a pointer to [§17.4](Bezel_Language_Spec_0_98.md#sec-the-is-is-not-and-as-operators) for the rationale. The change matches C#'s analogous semantics for `as` (C# `as` is also non-destructive; it does not mutate the operand reference).

<a id="sec-spec-31-4-invalidcastexception-entry-removed"></a>
### Spec [§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types) `InvalidCastException` entry removed

The built-in exception hierarchy listed `InvalidCastException — invalid type cast`, but after the v0.94 correction to [§28.8.1](Bezel_Language_Spec_0_98.md#sec-the-move-expression) (which removed `as` from the throwable list per its actual semantics in [§12.7](Bezel_Language_Spec_0_98.md#sec-type-tests-is-is-not-as) / [§17.4](Bezel_Language_Spec_0_98.md#sec-the-is-is-not-and-as-operators)), no built-in operation produces this exception:

- `as` returns null on cast failure, never raising.
- Explicit numeric narrowing casts raise `OverflowException`.
- Nullable-to-non-nullable casts raise `NullReferenceException`.
- Integer-to-enum casts raise `OverflowException`.

The entry was vestigial. v0.94 removes it from the [§31.4](Bezel_Language_Spec_0_98.md#sec-standard-exception-types) hierarchy. If a future revision adds a throwing-cast form (a reflection-based downcast that raises rather than returns null, for example), the exception can be re-added at that time. Until then, the hierarchy reflects only the exception types built-in operations actually produce.

<a id="sec-stdlib-6-3-stale-pattern-b-wording-fixed"></a>
### Stdlib [§6.3](Bezel_Standard_Library_0_98.md#sec-semaphore) stale Pattern B wording fixed

Stdlib [§6.3](Bezel_Standard_Library_0_98.md#sec-semaphore)'s description of `AcquireAsync` said "the await must be the immediate operand of `using var` or `await using var`" — the pre-v0.89 wording. The rule was loosened in v0.89 to "must appear in an owned-consuming context" (rationale [§5.12](#sec-pattern-b-loosening-and-iterator-yield-refinement)) and further refined in v0.90; the stdlib paragraph had not been updated. v0.94 replaces the stale sentence with the current, correct phrasing and a pointer to [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal).

<a id="sec-spec-13-7-visibility-and-inheritance-rule-generalized"></a>
### Spec [§13.7](Bezel_Language_Spec_0_98.md#sec-visibility-and-inheritance) visibility-and-inheritance rule generalized

v0.93's [§13.7](Bezel_Language_Spec_0_98.md#sec-visibility-and-inheritance) said "a `public` field, parameter, return type, or property type must itself be at least as visible as the member that uses it." The wording had two limitations:

- It applied only to `public` members. An `internal` field whose type was a `private` nested class was technically unconstrained by the rule, even though the obvious intent was to reject such a declaration. The rule should generalize: a member's signature types must be at least as visible as the member at every visibility level except `private` (which is the most restrictive level and cannot be tightened further).
- The list "field, parameter, return type, or property type" mixed categories: fields and properties have types, but "parameter" and "return type" are not visibility-bearing entities on their own. The intent was "every type appearing in a member's signature."

v0.94 rewrites the bullet as: "Every type appearing in a member's signature must be at least as visible as the member" — covering field types, parameter types, return types, property types, indexer parameter and result types, event payload types, and types named in `where` constraints on public/protected members. Generic-method type parameters whose substitution at the call site could narrow visibility are not constrained — the caller's substitution is the caller's choice. Appendix [§C.2](Bezel_Language_Spec_0_98.md#sec-appendix-c-visibility) is updated to match. A worked example shows the rule rejecting an `internal` field whose type is a `private` nested class. The rule's safety property is unchanged where it previously applied; the change extends its reach to the non-`public` cases.

<a id="sec-grammar-a-5-comment-aligned-with-14-4"></a>
### Grammar [§A.5](Bezel_Language_Spec_0_98.md#sec-types) comment aligned with [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)

The `GenericArg` production carried a comment from v0.78 stating that "only `Task<T>` accepts `owned T` as an argument." The opt-in list had since grown to cover the resizable collections (v0.88), the iteration interfaces (v0.88), the async iteration interfaces (v0.91), and now `IReadOnlyCollection<T>` (v0.94). The stale comment had survived three expansions. v0.94 rewrites the comment to defer to [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) as the authoritative list, eliminating the duplication that drove the staleness.

<a id="sec-grammar-a-1-floatliteral-admits-digit-separators"></a>
### Grammar [§A.1](Bezel_Language_Spec_0_98.md#sec-appendix-a) `FloatLiteral` admits digit separators

[§18.1](Bezel_Language_Spec_0_98.md#sec-numeric-literals) states "digit separators allowed except at start/end" as a general rule. The integer-literal productions (`DecLiteral`, `HexLiteral`, `BinLiteral`) admitted `_` separators; the `FloatLiteral` production did not. The asymmetry was a grammar oversight, not a design choice: every comparable language (C# 7+, Rust, Python 3.6+) admits digit separators uniformly across integer and float literals. v0.94 introduces a `DigitGroup ::= Digit (Digit | "_")*` non-terminal and uses it in the FloatLiteral productions for the integer part, the fractional part, and the exponent. Existing programs continue to compile; the change is strictly additive at the lexer.

<a id="sec-spec-4-2-integer-suffix-prose-aligned-with-grammar"></a>
### Spec [§4.2](Bezel_Language_Spec_0_98.md#sec-integer-arithmetic) integer-suffix prose aligned with grammar

The grammar's `IntSuffix ::= "L" | "U" | "UL" | "LU"` admits both orderings of the ulong suffix; the [§4.2](Bezel_Language_Spec_0_98.md#sec-integer-arithmetic) prose listed only `UL`. v0.94 updates the prose to mention both. C# admits both orderings too; reading uniformly across the two locations is the goal.

<a id="sec-patterns-relational-operator-subset-clarified-a-7"></a>
### Pattern's relational-operator subset clarified ([§A.7](Bezel_Language_Spec_0_98.md#sec-expressions))

The Pattern grammar previously had `Pattern ::= ... | RelOp Expression`, where `RelOp` was the same set used in expression-position relationals (`<`, `>`, `<=`, `>=`, `in`). Two problems followed:

- `in` was reachable as a pattern relop, but [§19.3](Bezel_Language_Spec_0_98.md#sec-patterns) didn't list `in pattern` as a valid pattern form, and the equivalent semantics (membership in a range or collection) is already expressible by range patterns (`0..10`) and constant alternatives (`1, 2, 3`).
- The `Expression` right-hand side admitted forms like `60..100` (a range), giving syntactically-parseable but semantically-incoherent patterns like `< 60..100`.

v0.94 introduces a separate `RelOpPattern ::= "<" | ">" | "<=" | ">="` production for the pattern form. `in` is no longer reachable in pattern position. A comment in the grammar notes that the Expression operand must be a value of the scrutinee's type (or implicitly convertible to it); the type check is semantic. This matches C# 9+'s relational-pattern design, which restricts pattern operators to the four ordering operators.

<a id="sec-spec-10-5-explicitly-rejects-virtual-abstract-override-on-enum-methods"></a>
### Spec [§10.5](Bezel_Language_Spec_0_98.md#sec-restrictions) explicitly rejects `virtual`/`abstract`/`override` on enum methods

The grammar's `EnumMember ::= MethodDecl | ...` and `MethodDecl ::= Visibility "static"? "async"? VirtualKind?` admitted `virtual`, `abstract`, `override`, and `sealed override` on enum methods syntactically, but enums are implicitly sealed and have no virtual-dispatch surface, so these modifiers carry no meaning. [§10.5](Bezel_Language_Spec_0_98.md#sec-restrictions) listed several enum restrictions but not this one. v0.94 adds an explicit bullet: "Enum methods may not be declared `virtual`, `abstract`, `override`, or `sealed override`." Restricting the grammar production would be the alternative; the prose-rejection form is consistent with [§10.5](Bezel_Language_Spec_0_98.md#sec-restrictions)'s other enumerated restrictions and is the simpler fix.

<a id="sec-spec-1-5-doc-tag-list-defers-to-34"></a>
### Spec [§1.5](Bezel_Language_Spec_0_98.md#sec-comments) doc-tag list defers to [§34](Bezel_Language_Spec_0_98.md#sec-comments-and-documentation)

[§1.5](Bezel_Language_Spec_0_98.md#sec-comments) listed six recognized documentation-comment tags; [§34](Bezel_Language_Spec_0_98.md#sec-comments-and-documentation) listed eight (the same six plus `<see>` and `<seealso>`). The inconsistency was a duplication that drifted. v0.94 changes [§1.5](Bezel_Language_Spec_0_98.md#sec-comments) to defer to [§34](Bezel_Language_Spec_0_98.md#sec-comments-and-documentation) for the recognized tag set, eliminating the duplication.

<a id="sec-spec-8-5-and-9-2-—-auto-tostring-for-auto-equality-bearing-body-records"></a>
### Spec [§8.5](Bezel_Language_Spec_0_98.md#sec-tostring) and [§9.2](Bezel_Language_Spec_0_98.md#sec-body-records) — auto-`ToString` for auto-equality-bearing body records

[§8.5](Bezel_Language_Spec_0_98.md#sec-tostring)'s default-`ToString` table had:

| Type kind | Default `ToString` |
|---|---|
| Class | type's qualified name |
| Positional record | `"TypeName(F1: v1, F2: v2)"` |
| Body record | type's qualified name |

But a body record that satisfies the auto-equality conditions ([§9.2](Bezel_Language_Spec_0_98.md#sec-body-records)) has the same structural shape as a positional record — only auto-properties, no manually-declared fields or equality — and the asymmetry meant `Console.WriteLine(person)` on an auto-equality-bearing body record produced only the type name, while the same shape declared positionally produced the property list. v0.94 changes the rule: auto-equality-bearing body records receive auto-`ToString` along with auto-`Equals` and auto-`GetHashCode`, formatted identically to the positional form. The three auto-generated members move as a package. Body records that do not satisfy auto-equality conditions, and enums, continue to receive only the type name. C#'s record types behave the same way (auto-`ToString` accompanies auto-equality).

<a id="sec-stdlib-9-6-trim-split-accept-ienumerablechar-instead-of-char"></a>
### Stdlib [§9.6](Bezel_Standard_Library_0_98.md#sec-string) `Trim`/`Split` accept `IEnumerable<char>` instead of `char[]`

`string.Trim(chars: char[])` and `string.Split(separators: char[])` previously required a fixed-size array, inconsistent with elsewhere in the stdlib where the more general `IEnumerable<T>` is used (e.g., `string.Join(separator, items: IEnumerable<string>)`, `Array<T>.AddRange(items: IEnumerable<T>)`, `File.WriteAllLines(path, lines: IEnumerable<string>)`). The asymmetry forced callers with a non-array source (a `LinkedList<char>`, an iterator) to materialize an array before the call.

v0.94 changes both signatures to take `IEnumerable<char>`. The call-site forms `s.Trim([' ', '\t'])` and `s.Split([' ', ','])` continue to work — a collection literal infers its target type from the parameter, so `IEnumerable<char>` works as readily as `char[]`. C# uses `params char[]` for these methods; Bezel has no `params` (rationale [§17.9](#sec-variadic-parameters-params)), so the equivalent generality comes from `IEnumerable<T>` plus collection literals at the call site.

<a id="sec-stdlib-9-6-equalsother-ignorecase-semantics-clarified"></a>
### Stdlib [§9.6](Bezel_Standard_Library_0_98.md#sec-string) `Equals(other, ignoreCase)` semantics clarified

The case-insensitive `Equals` overload's semantics weren't directly described; the reader had to infer them from the nearby "All comparison methods in this version are ordinal" comment, whose scope was ambiguous between just the comparison methods above it or also the case-insensitive equality below it. v0.94 moves the ordinal-only note to the top of the comparison-and-equality block and explicitly mentions that the `ignoreCase` overloads use simple Unicode case folding without locale tailoring. The semantics are unchanged; the documentation is sharper.

<a id="sec-stdlib-9-2-float-declaration-shown-in-full"></a>
### Stdlib [§9.2](Bezel_Standard_Library_0_98.md#sec-double-and-float) `Float` declaration shown in full

[§9.2](Bezel_Standard_Library_0_98.md#sec-double-and-float) had a complete declaration block for `Double` followed by prose describing `Float` as "the analogous shape." The prose form was less complete than the block form — it didn't pin down the IEEE 754 standard the float-typed constants follow, and the cost-vs-clarity tradeoff was tilted the wrong way for a primitive type's reference declaration. v0.94 replaces the prose with an explicit `Float` block mirroring `Double`'s, with constants noted as IEEE 754 binary32. (The integer-types section [§9.1](Bezel_Standard_Library_0_98.md#sec-integer-types) retains its "show `Int32`; the others are analogous" form because the duplication cost of eight separate blocks would outweigh the clarity gain. The float case has only two types; explicit declaration is cheap.)

<a id="sec-stdlib-10-0-intptr-uintptr-operator-declarations-removed"></a>
### Stdlib [§10.0](Bezel_Standard_Library_0_98.md#sec-intptr-and-uintptr) `IntPtr`/`UIntPtr` operator declarations removed

The `IntPtr` and `UIntPtr` declarations explicitly listed `public static method operator =` and `operator <>` overloads, but the other primitive-wrapper records in [§9.1](Bezel_Standard_Library_0_98.md#sec-integer-types)–[§9.5](Bezel_Standard_Library_0_98.md#sec-char) — `Int32`, `Double`, `Float`, `Decimal`, `Boolean`, `Char` — did not. They rely on the language-intrinsic comparison rule for value types (language spec [§3.4](Bezel_Language_Spec_0_98.md#sec-keyword-aliases-for-built-in-types)) and the auto-equality machinery for records ([§9.2](Bezel_Language_Spec_0_98.md#sec-body-records) of the language spec). v0.94 removes the explicit operator declarations from `IntPtr`/`UIntPtr` for consistency. The prose around the declarations is updated to explain that the comparison is provided by the same intrinsic rule as the other primitive records.

<a id="sec-stdlib-10-1-cstring-destructor-comment-matches-conditional-release-behavior"></a>
### Stdlib [§10.1](Bezel_Standard_Library_0_98.md#sec-cstring) `CString` destructor comment matches conditional-release behavior

`CString`'s `destructor;` declaration carried the comment `// releases the underlying buffer`, suggesting unconditional release. But the prose in [§10.1](Bezel_Standard_Library_0_98.md#sec-cstring) explains that release happens only when the `CString` allocated the buffer itself (via `CString.From` or `FromBytes`); native-wrapped instances are governed by the FFI declaration's contract. v0.94 expands the destructor comment to mention the conditional behavior and point at the prose, eliminating the contradiction between the declaration line and the surrounding text.

<a id="sec-clarifications-and-observations"></a>
### Clarifications and observations

Several smaller findings from the v0.93 review surfaced ambiguities or asymmetries that don't change behavior but warranted explicit text:

- **`owned T[]` and `T[]` are the same type for matching** ([§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types)). The `owned` modifier on a `T[]` return is documentation only; its compile-time discharge effect is degenerate. v0.94 states explicitly that the two forms are interchangeable for interface conformance, function-type compatibility, override matching, and generic substitution. This is the one exception to [§27.1](Bezel_Language_Spec_0_98.md#sec-function-types)'s general "modifier is part of the type" rule, justified by the absence of any observable ownership obligation on `T[]` itself. For class-typed returns the modifier remains part of the type.

- **`IEnumerable<owned T>`'s class-only restriction restated as a general consequence of [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)** ([§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for)). The v0.93 wording made the class-only restriction look like a property specific to `IEnumerable<owned T>`; in fact every `owned T` instantiation requires T to be a class type per [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument)'s general rule. v0.94 says so explicitly to avoid implying the iteration interfaces are special.

- **"Pinning" in [§28.10](Bezel_Language_Spec_0_98.md#sec-using-var-initialized-from-a-borrow) defined precisely.** v0.93's "the using-var still pins the borrow's source for the duration of the scope" left "pin" as a term of art. v0.94 defines it: a compile-time constraint that extends the source's required-liveness range to cover the using-var's scope. The same word is used in [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) for the for-loop desugaring with the same meaning.

- **[§26.5.1](Bezel_Language_Spec_0_98.md#sec-specificity-ordering) specificity example replaced.** The v0.93 example showed `extension IEnumerable<T> ... method Sum(): T`, but the generic `Sum` cannot actually be implemented in v0.94 (it would require static interface members for `T: Add` / `T: Zero`, deferred to a future revision per rationale [§18](#sec-things-deferred)). The example illustrated specificity ordering with code that doesn't compile. v0.94 replaces it with `First()`, which is implementable for any T and shows the specificity ordering equally well.

- **Spec [§15.3](Bezel_Language_Spec_0_98.md#sec-collection-literals) defers the deferred-protocol status to rationale [§18](#sec-things-deferred).** The line "A general protocol for user-defined collection types is reserved for a future revision" duplicated what rationale [§18](#sec-things-deferred) already said. v0.94 keeps a one-line pointer to rationale [§18](#sec-things-deferred); the deferred-items list is maintained in one place.

- **Spec [§10.4](Bezel_Language_Spec_0_98.md#sec-methods-properties-static-members) enum example now notes `MatchFailureException` for out-of-range values.** The `Name` property's `case this` lists only the declared enum constants and is not exhaustive over the integer value space; an enum value cast from an out-of-range integer ([§5.4](Bezel_Language_Spec_0_98.md#sec-enum-casts)) reaches the property and triggers `MatchFailureException`. The behavior is correct per the existing non-exhaustive-`case` rule ([§19.6](Bezel_Language_Spec_0_98.md#sec-exhaustiveness)); v0.94 adds a prose note so readers don't have to derive the consequence.

- **Stdlib [§6.1](Bezel_Standard_Library_0_98.md#sec-cancellationtoken-and-cancellationtokensource): `CancellationTokenSource.IsCancellationRequested` documented as a convenience equivalent to `Token.IsCancellationRequested`.** The duplication on the API surface matches C#'s and exists for ergonomics; v0.94 adds a sentence confirming the two never diverge.

- **Owned-field property setters do not auto-dispose** ([§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership)). The discussion of why owned-element-collection indexer setters auto-dispose (the setter is a method that can host the disposal logic) had a corollary worth stating explicitly: user-written property setters over an owned field also have method bodies but do **not** receive auto-disposal — the rule is specific to compiler-synthesized indexer setters on owned-element collections. v0.94 adds the disclaimer so readers don't infer a general "owned-targeting setter" auto-disposal feature.

- **Static-initialization scenarios summarized as a table** ([§22.7](Bezel_Language_Spec_0_98.md#sec-constructors)). The two scenarios (initializer throws and escapes the type → failed state; initializer catches the exception → normal completion) were described in dense prose. v0.94 adds a two-row summary table for readability; the normative content is unchanged.

---

<a id="sec-v0-94-to-v0-95-changelog"></a>
### v0.94 to v0.95 changelog

Normative for migration; not part of the language definition.

This revision applies the findings from v0.94's fresh-look review — an 8-finding pass focused on the v0.94 changes themselves, since v0.94 was a recent fix-up revision. Two of the findings are **regressions** introduced by v0.94 changes (`IntPtr`/`UIntPtr` equality removal, `Split` overload ambiguity); the other findings are clarifications and one foundational gap (instantiation-conditional inheritance for `Array<owned T>` and its siblings, which has been compiler magic in every revision and had never been explicitly described).

<a id="sec-stdlib-10-0-intptr-uintptr-operator-declarations-restored-v0-94-regression"></a>
### Stdlib [§10.0](Bezel_Standard_Library_0_98.md#sec-intptr-and-uintptr) `IntPtr`/`UIntPtr` operator declarations restored (v0.94 regression)

v0.94 removed the explicit `operator =` and `operator <>` declarations from `IntPtr` and `UIntPtr` on the reasoning that "the [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) intrinsic-equality rule covers these like the other primitive-wrapper records." That reasoning was wrong:

- Language spec [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types)'s intrinsic-equality rule applies to **primitive types** (`int`, `long`, `double`, etc., plus `string`), not to general record types. `Int32`, `Double`, etc. inherit equality from their underlying primitive (the *keyword-aliased* value type), not from a rule about records.
- `IntPtr` and `UIntPtr` are records with no underlying primitive keyword — there is no `intptr` keyword analogous to `int`. The [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) rule has no hook for them.
- As records with zero auto-properties (only `static const Zero`), they fall under [§9.2](Bezel_Language_Spec_0_98.md#sec-body-records)'s "zero auto-properties is auto-equality-bearing" rule, which produces an auto-`=` that returns `true` for every pair of values — semantically wrong for pointer comparison.

v0.95 reverts the v0.94 change: the explicit `operator =` and `operator <>` declarations are restored on `IntPtr` and `UIntPtr`. The stdlib paragraph explaining the design is rewritten to make the rationale explicit: `IntPtr`/`UIntPtr` are records *without* a corresponding primitive keyword, cannot piggyback on the [§3.5](Bezel_Language_Spec_0_98.md#sec-operators-on-primitive-types) intrinsic rule, and require explicit operator declarations to express pointer-comparison semantics. The implementations remain compiler intrinsics (no user-overridable bodies), but they are declared as overloads in the standard library to make the surface visible.

This is the most consequential v0.94 → v0.95 change: it restores correct equality semantics for the FFI-boundary pointer types.

<a id="sec-stdlib-9-6-splitseparators-char-restored-v0-94-regression"></a>
### Stdlib [§9.6](Bezel_Standard_Library_0_98.md#sec-string) `Split(separators: char[])` restored (v0.94 regression)

v0.94 changed `string.Split(separators: char[])` to `Split(separators: IEnumerable<char>)` for consistency with the v0.94 `Trim(chars: IEnumerable<char>)` change. The Trim change was fine — there is no competing `Trim(string)` overload. But the Split change created an overload ambiguity: `string` itself implements `IEnumerable<char>` (the standard library declares `class String: IEnumerable<char>`), so the call `s.Split("ab")` matched both `Split(separator: string)` (split at substring "ab") and `Split(separators: IEnumerable<char>)` (split at any of 'a', 'b') with no principled way to choose between them.

C# avoids the analogous issue because `char[]` is not assignable to `string`; the two overloads are distinguishable by argument type. v0.95 reverts Bezel's Split to the same shape: `Split(separators: char[])`. Callers wanting to split at any of a dynamically-supplied set of characters from a non-array source must materialize an array first (`s.Split(charSource.ToArray())`); this is the only valid pattern that resolves the ambiguity.

The Trim change is **not** reverted — `Trim(chars: IEnumerable<char>)` does not have a competing string-typed overload, so the generality remains a net improvement. The asymmetry between Trim and Split is justified by the overload-resolution constraint: when an overload set already contains a string-typed signature, an `IEnumerable<char>` signature in the same set creates ambiguity; when it doesn't, the more general type is preferable.

<a id="sec-spec-30-5-pattern-a-enumeration-reframed-as-principle"></a>
### Spec [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) Pattern A: enumeration reframed as principle

v0.94 enumerated Pattern A's admitted task-expression positions as a closed list of four cases: using-var-bound local, owned parameter, owned field of `this`, and producer call in a Pattern A return. The list was a strict subset of the natural set of positions: indexer access into owned-element collections (`tasks[i]` where `tasks` is `Array<owned Task<MyClass>>`) and iteration variables (`task` in `for var task in tasks do ... end`) are equally valid — they produce borrows whose source is `tasks`, a syntactically-visible owning location — but were not enumerated.

v0.95 reframes Pattern A's consumer rule as a **principle** with examples:

> The awaited task expression must be either (a) a reference to an owning location whose lifetime is visible at the await site, or (b) a producer call whose result is directly consumed by a Pattern A return in the enclosing async method.

The principle is followed by a non-exhaustive examples list (the four v0.94 cases plus indexer access on owned-element collections and iteration variables). The list illustrates the principle without claiming completeness — future syntactic forms producing borrows of owned tasks will fall under the principle automatically. The diagnostic surface improves: errors can point to the principle ("the awaited task expression has no syntactically-visible owning location") rather than listing the admitted positions and leaving the user to guess why their case doesn't appear.

<a id="sec-spec-12-7-is-non-destructive-rule-stated-explicitly"></a>
### Spec [§12.7](Bezel_Language_Spec_0_98.md#sec-type-tests-is-is-not-as) `is` non-destructive rule stated explicitly

v0.94 added an explicit non-destructive rule for `as` but not for `is`. The asymmetry was conspicuous: `as` and `is` are both type-tests that inspect the runtime type tag, neither moves from its operand, and the rule for one should mirror the rule for the other. v0.94's [§17.4](Bezel_Language_Spec_0_98.md#sec-the-is-is-not-and-as-operators) for `as` even said "This design matches the analogous rule for `is`" — referring to an analogous rule that wasn't actually written.

v0.95 adds a paragraph at the end of [§12.7](Bezel_Language_Spec_0_98.md#sec-type-tests-is-is-not-as) stating that both `is` and `as` are non-destructive: the operand retains its ownership state across the test, and narrowing-into-a-block (for `is`) or the borrowed result (for `as`) does not transfer ownership. The paragraph appears in the section covering both operators, eliminating the need to find the rule in two places.

<a id="sec-spec-9-2-auto-tostring-for-zero-property-records-specified"></a>
### Spec [§9.2](Bezel_Language_Spec_0_98.md#sec-body-records) auto-`ToString` for zero-property records specified

v0.94 added auto-`ToString` to the auto-equality package for body records (F27 of the v0.93 review). For non-zero-property records, the generated `ToString` produces `"TypeName(Prop1: v1, Prop2: v2)"`. For zero-property records (the singleton-by-value case mentioned at the end of [§9.2](Bezel_Language_Spec_0_98.md#sec-body-records)), the format was unspecified — three reasonable possibilities exist (`"TypeName()"`, `"TypeName"`, or implementation-defined).

v0.95 specifies the format: `"TypeName()"` — the type name followed by empty parens, the natural specialization of the property-listing template for zero properties. The choice mirrors C#'s record format (`"TypeName { }"` in C# becomes `"TypeName()"` in Bezel's parenthesized style). The decision is documented in the zero-property paragraph of [§9.2](Bezel_Language_Spec_0_98.md#sec-body-records).

<a id="sec-spec-2-3-auto-imports-the-iteration-interface-family"></a>
### Spec [§2.3](Bezel_Language_Spec_0_98.md#sec-always-available-names) auto-imports the iteration-interface family

The v0.94 always-available list included `IEnumerable<out T>`, `IEnumerator<out T>`, `IAsyncEnumerable<out T>`, and `IAsyncEnumerator<out T>` but omitted `IReadOnlyCollection<out T>`, `IReadOnlyList<out T>`, `ICollection<T>`, and `IList<T>`. The omission was inconsistent — these interfaces participate in standard-library type inheritance (`Array<T>: IList<T>`, `Array<owned T>: IReadOnlyList<T>`) and in pattern-frequent variable type ascriptions; requiring `import System.Collections.Generic` for them while `IEnumerable<T>` is auto-imported produced an arbitrary boundary in the interface family.

v0.95 extends the always-available list to cover the full iteration-interface family. The explanatory paragraph notes that these interfaces participate in core language constructs (the `for` and `await for` desugarings; standard-library type inheritance) — the same justification used for `IEnumerable<T>`'s presence on the list. Users no longer need an explicit import to reference `IList<T>`, `IReadOnlyList<T>`, etc.; the stdlib continues to document the full declarations in [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic).

<a id="sec-spec-14-4-instantiation-conditional-specialization-documented"></a>
### Spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument): instantiation-conditional specialization documented

Throughout the language's history — from v0.78's `Task<owned T>` admission onward — the standard library has presented opted-in generic types with *two* declarations: one for the unmodified-argument form and one for the owned-argument form, with different inheritance clauses and different member signatures. For example:

```bezel
public class Array<T>: IList<T>
  public method Add(item: T);
  // ...
end Array;

public class Array<owned T>: IReadOnlyList<T>
  public method Add(owned item: T);
  // ...
end Array;
```

Rationale [§5.11](#sec-owned-element-collections) explains that these are presentations of one generic class, not two separate classes — but the spec did not describe the mechanism by which a single class declaration produces two different per-instantiation behaviors. A reader looking at the stdlib could reasonably ask: *how do I write a class with that pattern?* The answer (until v0.95) was nowhere in the spec.

v0.95 adds a subsection to [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument), "Instantiation-conditional specialization," which:

- States explicitly that the dual presentation in stdlib [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic) is a documentation device showing the compile-time forms of one compiler-specialized class.
- Identifies the specialization as compiler magic available only to the opted-in types listed in [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument).
- Notes that user-defined generic types **cannot** declare instantiation-conditional inheritance or signatures in this version; a declaration-level opt-in is expected in a future revision (rationale [§5.11](#sec-owned-element-collections) subsection "User-defined types can't opt in yet" has the proposed direction).
- Lists the concrete differences the compiler produces for each opted-in type — `Add`/`Insert` parameter modifiers, indexer-setter semantics, drain methods, inheritance clause.

The change is documentation-only — no language behavior changes — but closes the largest hidden contract in the spec's description of owned-element collections. Future readers of the stdlib's owned-collection declarations now have an explicit explanation of why two declarations appear for one class.

---

<a id="sec-v0-95-to-v0-96-changelog"></a>
### v0.95 to v0.96 changelog

Normative for migration; not part of the language definition.

v0.96 makes the merged-declaration story real. v0.95 had documented the "instantiation-conditional specialization" of owned-element collections as compiler magic available only to opted-in types; v0.96 replaces the magic with explicit, user-visible language mechanism. The standard library's owned-element collections move from dual-class presentation to a single declaration per collection, with the per-instantiation differences expressed by two new constraint forms and one new substitution rule.

The three additions to the language:

1. **Substitution rule (R3').** When a generic argument carries `owned` and is substituted into a position that produces a borrow-flow value (an indexer getter, a property getter, a method return without explicit `owned`, a function-type internal, a record field), the `owned` modifier is stripped. The stripped position uses the unmodified type. This is the explicit form of the implicit projection v0.95's dual-class presentation had performed silently.

2. **Constraint form `where T is owned`.** A method-level constraint that admits the method only on instantiations where T's argument carries `owned`. Methods that exist only on owned-element forms (the drain family — `PopAt`, `Drain`, etc.) carry this constraint.

3. **Constraint form `where T is not owned`.** The complement: a method exists only when T's argument does not carry `owned`. Methods that exist only on non-owned forms (`ToArray()`, `DisposeElements()`) carry this constraint.

With these, single-class declarations for `Array<T>`, `LinkedList<T>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `Dictionary<K, V>`, and the iteration interface family replace v0.95's dual presentation. The methods that have form-dependent parameter modifiers (Add, Insert, indexer setters, AddRange, GetOrAdd) are declared dually inside the single class, with `where` constraints distinguishing the forms; the bulk of each collection's API merges. The compiler-magic story in v0.95 language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) ("instantiation-conditional specialization") is replaced by these explicit mechanisms; the section is rewritten accordingly.

<a id="sec-spec-14-4-substitution-rules-made-explicit"></a>
### Spec [§14.4](Bezel_Language_Spec_0_98.md#sec-substitution-of-owned-bearing-type-arguments): substitution rules made explicit

v0.95 (and every earlier revision) had treated the substitution of `owned T` as "compiler magic" — particularly in the way iteration interfaces were projected from owned-element collections. The dual-class presentation `Array<T>: IList<T>` versus `Array<owned T>: IReadOnlyList<T>` worked because the compiler hard-coded the per-instantiation differences; the spec didn't describe substitution as a uniform rule.

v0.96 specifies three substitution rules:

**Rule 1 (idempotence).** A position that declares `owned T` substitutes to `owned C` when T = `owned C`; the double-`owned` collapses. This applies to parameter positions with explicit `owned`, return positions with explicit `owned`, owned-T inside opt-in generic arguments (`IEnumerable<owned T>` substituting to `IEnumerable<owned C>`), and `out owned p: T`.

**Rule 2 (admissible position without explicit `owned`).** A position that does not carry `owned` declares a borrow-flow slot; substitution strips the modifier so the substituted position uses unmodified C. This applies to parameter positions like `Contains(item: T)` (borrow-taking → `Contains(item: C)`), return positions like the indexer-getter `[i: int]: T` (returns borrow of C), and function-type internals like `Sort(comparer: (T, T) -> int)` (substitutes to `(C, C) -> int`).

**Rule 3 (non-admissible position).** A position that cannot carry `owned` regardless of declaration intent strips the modifier. This applies to record fields (no owned record fields per [§28.5](Bezel_Language_Spec_0_98.md#sec-owned-fields) — `DictionaryEntry<K, V>.Value` strips when V is owned), the element type of `T[]` (not in language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) opt-in list — `T[]` substitutes to `C[]`), and non-opted-in generic type arguments.

The unifying principle: **the explicit presence or absence of `owned` in a declaration position determines whether the position is owned-taking or borrow-taking, and substitution preserves this declaration intent.** A bare `T` in a function-type internal position is always a borrow-taking parameter, regardless of T's modifier. A bare `T` as a return type is always a borrow-flow return.

This rule resolves a subtle issue that v0.95's dual-presentation had hidden: `Sort(comparer: (T, T) -> int)` on the owned form needs the lambda to take borrows of T, not owned T (otherwise the lambda would dispose its parameters on exit). The dual-class presentation handled this by projecting the owned form to `Array<owned T>: IReadOnlyList<T>` (note the *unmodified* T in the inheritance), implicitly stripping. v0.96 makes the stripping explicit and uniform across all positions.

The new substitution rules are documented in spec [§14.4](Bezel_Language_Spec_0_98.md#sec-substitution-of-owned-bearing-type-arguments) under "Substitution of owned-bearing type arguments." The "Admissibility of `owned` in declaration positions" subsection enumerates where `owned` is admissible (parameter declarations, return declarations, opt-in generic arguments, fields, locals through `using var`) and where it is not (function-type internals, record fields, `T[]`, non-opt-in arguments). The substitution rules reference admissibility to determine which rule applies.

<a id="sec-spec-14-4-and-grammar-a-4-where-t-is-owned-and-where-t-is-not-owned"></a>
### Spec [§14.4](Bezel_Language_Spec_0_98.md#sec-conditioning-method-existence-on-ts-modifier) and grammar [§A.4](Bezel_Language_Spec_0_98.md#sec-members): `where T is owned` and `where T is not owned`

v0.96 introduces two new constraint forms for conditioning method/property/operator existence on T's modifier:

- `where T is owned` — the declaration is present only on instantiations where T's argument carries `owned`.
- `where T is not owned` — the declaration is present only on instantiations where T's argument does not carry `owned`.

The constraints sit alongside the existing `where T: SomeType` and `where T: class`/`value`/`notnull`/`new()` constraints from spec [§14.1](Bezel_Language_Spec_0_98.md#sec-generic-declarations). They use the `is` keyword rather than `:` to make the modifier-predicate-vs-type-bound distinction syntactically visible. Each `is`-form constraint is a complete `where` clause; for multi-parameter conditioning, declarations carry multiple `where` clauses:

```bezel
public method Add(owned key: K, owned value: V)
  where K is owned where V is owned;
```

The grammar is extended (Appendix A.4):

```
GenericConstraints ::= "where" Identifier ":" Constraint ("," Constraint)*
                     | "where" Identifier "is" ModifierConstraint
ModifierConstraint ::= "owned" | "not" "owned"
```

The compiler evaluates the constraint at instantiation time and exposes only the matching declarations per per-instantiation member set. A call to a constrained method on a non-matching instantiation is rejected with a diagnostic that identifies the constraint.

User-defined generic types can declare members with these constraints, but until language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) condition 1 admits user-defined types to the owned-argument opt-in list, `where T is owned` on a user-defined `Box<T>` is never satisfiable. The constraints are useful in v0.96 primarily for standard-library declarations.

<a id="sec-stdlib-4-merged-collection-declarations"></a>
### Stdlib [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic): merged collection declarations

All six owned-element collections (`Array`, `LinkedList`, `Set`, `Stack`, `Queue`, `Dictionary`) and the iteration-interface family (`ICollection`, `IList`, `IReadOnlyCollection`, `IReadOnlyList`) move to single-declaration form in v0.96. The bulk of each declaration is uniform — `Count`, `Contains`, `Remove`, `Clear`, comparison-style methods, the indexer-getter — and the form-dependent members are declared in pairs or singletons guarded by the new `where` clauses.

For each collection, the form-dependent members are:

- **`Array<T>`**: `Add`, `Insert` (split by parameter modifier); `AddRange` (split by signature shape); indexer setter (split by parameter modifier); `ToArray` and `DisposeElements` (non-owned only); `PopAt`/`PopFirst`/`PopLast`/`TryPopFirst`/`TryPopLast`/`Drain` (owned only).
- **`LinkedList<T>`**: `AddFirst`/`AddLast`/`AddBefore`/`AddAfter` (split by parameter modifier); `Pop*`/`TryPop*`/`Drain` (owned only); the `source: IEnumerable<T>` constructor is non-owned only.
- **`Set<T>`**: `Add` (split); `UnionWith` (split by signature shape); `Take`/`TryTake`/`Drain` (owned only).
- **`Stack<T>` / `Queue<T>`**: `Push`/`Enqueue` (split by parameter modifier); `Pop`/`Dequeue` (split by return modifier); `TryPop`/`TryDequeue` (split by out-parameter modifier); `Drain` (owned only).
- **`Dictionary<K, V>`**: `Add` (split four-way on K and V independently); indexer setter (split on V); `GetOrAdd` (split on V); `PopValue`/`TryPopValue` (V-owned only); `DisposeElements` (both-non-owned only).

The inheritance clause becomes uniform: `Array<T>: IList<T>`, `LinkedList<T>: ICollection<T>`, etc. The substitution behavior is form-dependent in a structured way:
- The **mutating-interface** inheritance preserves T's modifier: `Array<owned Connection>: IList<owned Connection>`, `: ICollection<owned Connection>`. The interface itself uses the merged-declaration pattern (its `Add` and indexer setter are conditioned, matching the class's conditioning), so the substituted interface has the owned-form storage methods.
- The **read-only iteration-interface** inheritance projects with T's modifier *stripped*: `Array<owned Connection>` transitively implements `IEnumerable<Connection>`, `IReadOnlyList<Connection>`, `IReadOnlyCollection<Connection>` — the *unmodified* forms. This is the inheritance-projection rule in language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-substitution-of-owned-bearing-type-arguments), preserving v0.95's behavior that default iteration over an owned-element collection yields borrows (rather than triggering destructive `Current` on `IEnumerator<owned T>`).

This split is a behavioral departure from v0.95, where `Array<owned T>` deliberately did *not* implement `IList<T>` at all (only the read-only interfaces). In v0.96, the owned form implements *both* a mutating interface (`IList<owned C>`, the merged-form's owned-bearing version) *and* the read-only interfaces (`IReadOnlyList<C>`, the unmodified-T versions). The mutating-interface restoration is what gives generic mutating-collection code (e.g., a function accepting `IList<owned Connection>`) something to work with; the read-only projection is what keeps default iteration borrowing.

The interface declarations (`ICollection<T>`, `IList<T>`) acquire their own conditional members:

```bezel
public interface ICollection<T>: IReadOnlyCollection<T>
  method Add(item: T) where T is not owned;
  method Add(owned item: T) where T is owned;
  method Clear();
  method Contains(item: T): bool;
  method Remove(item: T): bool;
  method DisposeElements() where T is not owned;
end ICollection;
```

This is the structural foundation that lets `Array<T>: IList<T>` work for both instantiations: the interface's per-form Add matches the class's per-form Add.

<a id="sec-spec-28-11-cross-form-iteration-preserved-via-inheritance-projection-rule"></a>
### Spec [§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership): cross-form iteration preserved via inheritance-projection rule

The v0.95 [§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) example showed `ReportEach(items: IEnumerable<Connection>)` accepting an `Array<owned Connection>` directly. The explanation was that `Array<owned T>: IEnumerable<T>` via the v0.95 dual-class declaration's projection — the inheritance stripped the `owned` modifier so the iteration interface used unmodified T.

v0.96 needs to preserve this property. Without it, default iteration over an owned-element collection would desugar to `IEnumerator<owned T>` whose `Current` is destructive (per [§29.1](Bezel_Language_Spec_0_98.md#sec-current-on-owned-element-enumerators)) — turning every `for var x in pool do ... end` over an owned-element pool into an *unintended drain*. That would be a serious regression and would contradict the design intent in [§28.11](Bezel_Language_Spec_0_98.md#sec-collection-element-ownership) that "default iteration produces borrows."

v0.96 preserves the property via an **inheritance-projection rule** added to spec [§14.4](Bezel_Language_Spec_0_98.md#sec-substitution-of-owned-bearing-type-arguments): when an opted-in collection class (`Array`, `LinkedList`, `Set`, `Stack`, `Queue`, `Dictionary`) inherits an iteration interface (`IEnumerable<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`, etc.) with type argument T, and the collection's T is instantiated as `owned C`, the inheritance projects to the interface with unmodified C. So `Array<owned Connection>: IList<owned Connection>` (the mutating-interface form preserves T's modifier) but transitively implements `IEnumerable<Connection>` (unmodified, via the projection rule applied to `IEnumerable<T>`).

This means:
- `ReportEach(items: IEnumerable<Connection>)` accepts `Array<owned Connection>` directly — exactly as in v0.95.
- Default iteration `for var x in pool do ... end` desugars to use `IEnumerator<Connection>` (unmodified, non-destructive Current) — borrows, as intended.
- `Drain()` (which explicitly returns `IEnumerable<owned T>` per its declaration) preserves the owned-bearing form for its enumerator's destructive-Current semantics; the projection rule doesn't apply to drain because the drain method's return type explicitly carries `owned T` (Rule 1, not the projection rule).

The rule is "special" in the sense that it only applies to inheritance edges from opted-in collection classes to read-only iteration interfaces, not to substitution in general. Mutating interfaces (`IList<T>`, `ICollection<T>`) do *not* get the projection — they preserve T's modifier, so `Array<owned Connection>` implements `IList<owned Connection>` (the owned-mutating form) and exposes the owned-form `Add(owned item: T)` through the interface. This split is intentional: read-only iteration is form-agnostic (borrowing both ways), while mutation is form-dependent (owned-taking on owned, borrow-taking on non-owned).

The migration impact from v0.95: code that used `IEnumerable<Connection>` to iterate an `Array<owned Connection>` continues to work (the projection rule preserves the v0.95 behavior). Code that used `ICollection<Connection>` or `IList<Connection>` (the v0.93 buggy form, which v0.94 rejected) is still rejected — the owned-form's mutating interface is `IList<owned Connection>`, distinct from `IList<Connection>`. Migration to the merged form is minimal for read-only iteration; for code that wants to mutate via the interface, the static type changes from `IList<Connection>` to `IList<owned Connection>`.

<a id="sec-spec-14-4-form-dependent-return-types-and-overload-pair-semantics"></a>
### Spec [§14.4](Bezel_Language_Spec_0_98.md#sec-merged-declarations-and-form-dependent-members): form-dependent return types and overload-pair semantics

Several stdlib methods need return-type forms that differ by T's modifier. `Stack<T>.Pop` is the canonical example: on `Stack<MyClass>` it returns a borrow of `MyClass` (the borrow has the source of whatever was pushed); on `Stack<owned MyClass>` it returns owned `MyClass` (ownership transfers out of the stack). The v0.95 dual-class presentation declared these with different signatures in different classes; v0.96 declares them as two conditioned declarations in the merged class:

```bezel
public method Pop(): T where T is not owned;
public method Pop(): owned T where T is owned;
```

The two declarations differ in return type, which would normally fall under Bezel's prohibition on return-type-only overloading. v0.96 clarifies in [§14.4](Bezel_Language_Spec_0_98.md#sec-merged-declarations-and-form-dependent-members): when two declarations of the same method are conditioned on mutually-exclusive `where T is owned` / `where T is not owned` clauses, only one is present on the per-instantiation member set; the two never coexist in a single instantiation. The prohibition on return-type-only overloading does not apply because there is no overload set within an instantiation — there is just the one selected declaration.

This pattern appears in `Stack<T>.Pop`, `Queue<T>.Dequeue`, and the `TryPop`/`TryDequeue` variants (the out-parameter modifier differs between forms). The pattern is restricted to per-form overload pairs — return-type-only overloads remain prohibited within a single instantiation as before.

<a id="sec-stdlib-4-v0-95-dual-presentation-phrases-retired"></a>
### Stdlib [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic): v0.95 dual-presentation phrases retired

The v0.95 stdlib contained several phrases that were specific to the dual-class presentation:

- "Array<owned T> — owned-element variant" subsections under each collection — gone; the merged declaration covers both forms.
- "The inheritance is limited to `IReadOnlyList<T>` (which transitively brings in `IReadOnlyCollection<T>` and `IEnumerable<T>`). The mutating interfaces ... are deliberately omitted ..." — replaced by the merged-declaration story; the mutating interfaces are admitted on the owned form via per-form conditioning.
- "Code that previously wrote `someOwnedArray as ICollection<T>` ... is rejected in v0.94" — the v0.94 rejection is reversed in v0.96; the cast now succeeds and yields `ICollection<owned T>` (the merged form's mutating surface).

These are real behavior changes from v0.94→v0.95→v0.96. Code that worked in v0.93 (with the buggy mutating-interface conformance) and was broken in v0.94/v0.95 (with the explicit borrowing-only conformance) again works in v0.96 (with merged-form conformance) — though the *interface type* is now `IList<owned T>` rather than `IList<T>`, since the merged form preserves T's modifier.

The migration from v0.95 to v0.96 for code that did *not* use the mutating interfaces on owned-element collections is minimal: most code continues to work unchanged. Code that iterated owned-element collections via the borrowing interfaces (`IEnumerable<MyClass>`, `IReadOnlyList<MyClass>`) continues to work — the new inheritance-projection rule preserves the v0.95 behavior of stripping `owned` for the read-only iteration interfaces. Code that used the mutating interfaces (`IList<MyClass>`, `ICollection<MyClass>`) on owned-element collections was rejected in v0.94/v0.95 and is now admissible via `IList<owned MyClass>` / `ICollection<owned MyClass>` (the merged-form's mutating-interface types, which preserve T's modifier).

---

<a id="sec-v0-96-to-v0-97-changelog"></a>
### v0.96 to v0.97 changelog

Normative for migration; not part of the language definition.

v0.97 is the largest design change since v0.78 introduced `owned` as a generic argument. It removes the `<owned T>` instantiation mechanism in its entirety and replaces every owned-bearing generic instantiation with a separate, explicitly named class or interface. The motivation: v0.96's verification (and the design history of v0.78 through v0.96) revealed that `<owned T>` as a parameterization mechanism required progressively more machinery — substitution rules, constraint forms, inheritance projection — each of which fixed one issue and introduced another. The owned/non-owned distinction is genuinely *semantic* (an owning collection's Add takes ownership; a borrowing collection's Add takes a borrow), and trying to express it as a parameter modifier was forcing two different abstractions through one syntactic vehicle.

In v0.97, owning collections are first-class named types: `OwnedArray<T>`, `OwnedLinkedList<T>`, `OwnedSet<T>`, `OwnedStack<T>`, `OwnedQueue<T>`, `OwnedDictionary<K, V>` (V is the owned value; K is value-like). The owning enumeration interfaces are `OwnedEnumerable<T>`, `OwnedEnumerator<T>`, `OwnedAsyncEnumerable<T>`, `OwnedAsyncEnumerator<T>`. The owning task type is `OwnedTask<T>`. The owning mutating-collection interfaces are `IOwnedCollection<T>` and `IOwnedList<T>`. The `where T: class` constraint on each makes the underlying type's classification explicit.

The language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) opt-in mechanism is removed. There are no substitution rules for `owned`-bearing type arguments, no `where T is owned` / `where T is not owned` constraints, no inheritance-projection rule for opted-in collections. The `owned` modifier survives in declaration positions (parameter, return, field, local via `using var`, `out`) — its original v0.77 role. Generic type arguments no longer carry the modifier.

The migration cost is substantial: every existing piece of code using `<owned T>` instantiations needs to be rewritten to use the named types. The spec, stdlib, and rationale are all heavily affected. This is recognized as a major break and is the reason v0.97 is a substantial version bump.

<a id="sec-spec-14-4-rewritten-as-brief-disallowance"></a>
### Spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument): rewritten as brief disallowance

The v0.96 language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) was the longest section in the spec — roughly 21KB describing the opt-in mechanism, three substitution rules, two new constraint forms, an inheritance-projection rule, the admissibility-of-`owned`-in-positions table, the merged-declaration mechanism, and the various interactions among them.

The v0.97 language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) is approximately 2KB and says, in essence: `owned` is a modifier on declaration positions, not a generic-argument form. The v0.96 forms (`Array<owned T>`, `Task<owned T>`, etc.) are not valid types in v0.97; the v0.97 equivalents are separate named classes (`OwnedArray<T>`, `OwnedTask<T>`, etc.) declared in the standard library. A brief table at the end of language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) enumerates the v0.96-to-v0.97 type renaming.

The new language spec [§28.1](Bezel_Language_Spec_0_98.md#sec-where-the-owned-modifier-can-appear) "Where the `owned` modifier can appear" inherits the only durable content from v0.96 language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument): the enumeration of admissible positions (parameter declarations, return types, field declarations, locals through `using var`, `out` parameters, function-type parameter and return slots, lambda parameters) and non-admissible positions (record fields, fixed-size-array element types, generic type arguments).

<a id="sec-stdlib-4-collections-split-into-borrowing-and-owning-families"></a>
### Stdlib [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic): collections split into borrowing and owning families

The stdlib [§4](Bezel_Standard_Library_0_98.md#sec-system-collections-generic) is restructured around the borrowing/owning distinction:

- [§4.1](Bezel_Standard_Library_0_98.md#sec-borrowing-collection-interfaces) — Borrowing collection interfaces (`ICollection<T>`, `IList<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`).
- [§4.2](Bezel_Standard_Library_0_98.md#sec-owning-collection-interfaces) — Owning collection interfaces (`IOwnedCollection<T>`, `IOwnedList<T>`).
- [§4.3](Bezel_Standard_Library_0_98.md#sec-borrowing-and-owning-collections-relate-via-iiterator-interfaces) — How the two hierarchies relate.
- [§4.4](Bezel_Standard_Library_0_98.md#sec-arrayt) — `Array<T>` (borrowing).
- [§4.5](Bezel_Standard_Library_0_98.md#sec-ownedarrayt) — `OwnedArray<T>` (owning).
- [§4.6](Bezel_Standard_Library_0_98.md#sec-linkedlistt) — `LinkedList<T>` and [§4.7](Bezel_Standard_Library_0_98.md#sec-ownedlinkedlistt) `OwnedLinkedList<T>`.
- [§4.8](Bezel_Standard_Library_0_98.md#sec-dictionaryk-v) — `Dictionary<K, V>` and [§4.9](Bezel_Standard_Library_0_98.md#sec-owneddictionaryk-v) `OwnedDictionary<K, V>`.
- [§4.10](Bezel_Standard_Library_0_98.md#sec-sett) — `Set<T>` and `OwnedSet<T>`.
- [§4.11](Bezel_Standard_Library_0_98.md#sec-stackt-and-queuet) — `Stack<T>`, `OwnedStack<T>`, `Queue<T>`, `OwnedQueue<T>`.
- [§4.12](Bezel_Standard_Library_0_98.md#sec-borrowing-enumeration-interfaces) — Borrowing enumeration interfaces.
- [§4.13](Bezel_Standard_Library_0_98.md#sec-owning-enumeration-interfaces) — Owning enumeration interfaces.

Each owning collection requires `where T: class` (and `where V: class` for `OwnedDictionary`). The v0.96 four-way Dictionary instantiation (`Dictionary<K, V>`, `<owned K, V>`, `<K, owned V>`, `<owned K, owned V>`) collapses to two: `Dictionary<K, V>` and `OwnedDictionary<K, V>`. The asymmetric `Dictionary<owned K, V>` case was rare to nonexistent in practice and is not supported.

<a id="sec-spec-29-1-and-stdlib-4-12-4-13-iteration-interfaces-simplified"></a>
### Spec [§29.1](Bezel_Language_Spec_0_98.md#sec-iterator-protocol) and stdlib [§4.12-§4.13](Bezel_Standard_Library_0_98.md#sec-borrowing-enumeration-interfaces): iteration interfaces simplified

The v0.96 destructive-Current rule for `IEnumerator<owned T>` becomes a normal declaration on `OwnedEnumerator<T>`. `IEnumerator<T>` always has non-destructive `Current` — no special case needed. The for-loop desugaring at [§29.2](Bezel_Language_Spec_0_98.md#sec-desugaring-of-for) now has two cases: sources typed `IEnumerable<T>` use `GetEnumerator()`/non-destructive Current; sources typed `OwnedEnumerable<T>` use `GetOwnedEnumerator()`/destructive Current.

A type can implement both interfaces. `OwnedArray<T>` does: default iteration (`for var x in arr do ... end`) uses `IEnumerable<T>` and yields borrows; drain iteration (`for var x in arr.Drain() do ... end`) uses `OwnedEnumerable<T>` and yields owned values that the consumer must discharge. The selection is made by the static type of the source expression, which is determined at the call site.

<a id="sec-stdlib-6-7-taskowned-t-becomes-ownedtaskt"></a>
### Stdlib [§6.7](Bezel_Standard_Library_0_98.md#sec-task-static-helpers): `Task<owned T>` becomes `OwnedTask<T>`

The async result-bearing task type splits into two:

- `Task<T>` remains the borrowing form (await yields a borrow of T). Unchanged from v0.96.
- `OwnedTask<T>` is the new owning form (await yields owned T). Replaces v0.96's `Task<owned T>`.

The owning task is single-consumer; the language spec [§30.5](Bezel_Language_Spec_0_98.md#sec-result-lifetime-and-task-disposal) Pattern B rules apply to `OwnedTask<T>` directly (no longer keyed off "`Task<owned T>` for class T"). The Pattern A rules for borrowing-task consumption apply to `Task<T>` for class T.

`Task.WhenAllOwned` and related static helpers now accept `OwnedArray<OwnedTask<T>>` rather than `Array<owned Task<owned T>>`. The names of the helpers are unchanged.

<a id="sec-grammar-appendix-a-4-setowned-survives-where-t-is-owned-removed"></a>
### Grammar Appendix A.4: `set(owned)` survives; `where T is owned` removed

Two grammar changes from v0.97:

- **Removed:** the `where T is owned` and `where T is not owned` constraint forms introduced in v0.96. Their motivation (conditioning method existence on T's modifier inside a merged class declaration) no longer applies, since v0.97 has no merged class declarations.
- **Retained:** the `set(owned)` setter parameter spec introduced in v0.96. The setter for `OwnedArray<T>.[i: int]: T` declares `set(owned)` to make the indexer setter owned-taking; this is the same syntax as in v0.96, with the same semantics. The mechanism is now used by direct class declarations (rather than by merged-form conditioning), but the syntax itself is unchanged.

<a id="sec-rationale-5-11-user-defined-owning-containers-via-composition"></a>
### Rationale [§5.11](#sec-owned-element-collections): user-defined owning containers via composition

The "user-defined types can't opt in" subsection of v0.95 and v0.96 anticipated a future declaration-level opt-in mechanism for user-defined generic types that want to be owning. v0.97 solves this problem differently: user code builds owning containers by *composition* over the standard-library owning primitives.

```bezel
public class OwnedCache<T> where T: class
  private owned _items: OwnedArray<T>;

  public constructor() _items := new(); end;

  public method Add(owned item: T) => _items.Add(item);
  public method Drain(): OwnedEnumerable<T> => _items.Drain();
end OwnedCache;
```

No special language mechanism is needed. The user's `OwnedCache<T>` is a normal generic class whose owned-storage semantics come from its `OwnedArray<T>` field. The `where T: class` constraint on the user class ensures `OwnedArray<T>` is itself well-formed. This pattern composes cleanly: a user can build owning trees over owning maps over owning arrays without any opt-in machinery.

<a id="sec-migration-mechanical-renaming"></a>
### Migration: mechanical renaming

The v0.96 → v0.97 migration is mechanical but pervasive. The renaming table:

| v0.96 | v0.97 |
|---|---|
| `Array<owned T>` | `OwnedArray<T>` |
| `LinkedList<owned T>` | `OwnedLinkedList<T>` |
| `Set<owned T>` | `OwnedSet<T>` |
| `Stack<owned T>` | `OwnedStack<T>` |
| `Queue<owned T>` | `OwnedQueue<T>` |
| `Dictionary<K, owned V>` | `OwnedDictionary<K, V>` |
| `Dictionary<owned K, V>`, `<owned K, owned V>` | not supported (see [§5.11](#sec-owning-collections)) |
| `Task<owned T>` | `OwnedTask<T>` |
| `IEnumerable<owned T>` | `OwnedEnumerable<T>` |
| `IEnumerator<owned T>` | `OwnedEnumerator<T>` |
| `IAsyncEnumerable<owned T>` | `OwnedAsyncEnumerable<T>` |
| `IAsyncEnumerator<owned T>` | `OwnedAsyncEnumerator<T>` |
| `IList<owned T>` | `IOwnedList<T>` |
| `ICollection<owned T>` | `IOwnedCollection<T>` |
| `IReadOnlyList<owned T>`, `IReadOnlyCollection<owned T>` | `IReadOnlyList<T>`, `IReadOnlyCollection<T>` (these always produced borrows; the owned-bearing form was redundant) |

Method signatures within the renamed types are largely unchanged: `Add(owned item: T)` is still `Add(owned item: T)` after the type rename. The `where T: class` constraints on the owning types make explicit what was implicit in v0.96 (the language spec [§14.4](Bezel_Language_Spec_0_98.md#sec-owned-as-a-generic-argument) condition that `owned T` requires T to be a class type).

Code that used `where T is owned` or `where T is not owned` constraints (introduced in v0.96) needs to remove those constraints; if the code was inside a v0.96 merged class declaration, it should be split into separate borrowing and owning class declarations.

The `set(owned)` indexer-setter parameter spec is unchanged — code using this syntax for owning collections' indexer setters continues to work.

---

<a id="sec-v0-97-to-v0-98-changelog"></a>
### v0.97 to v0.98 changelog

v0.98 introduces a foundational change to the array system and resolves the long-standing question of how the standard library's owning collections should be implementable rather than purely intrinsic. The change spans three documents and retires one venerable type (`T[]`), replacing it with two cleanly-separated kinds: inline arrays `[N]T` (value-typed, no allocation) and the existing heap arrays `Array<T>` / `OwnedArray<T>` (class-typed, allocated, real disposal).

The driving consideration was an aesthetic objection to "compiler intrinsics whose visible actions cannot be expressed by the language." The v0.97 standard library's owning collections were intrinsic at the slot-level: `OwnedArray<T>.PopAt(i)`'s take-then-shrink semantics could not be written in pure Bezel because `T[]` did not admit per-element ownership tracking. v0.98 removes this intrinsic by extending the language with a small set of features — inline arrays, the move-only value-type category, and per-slot ownership tracking on inline arrays held in owned fields — that together let the owning collections be expressed honestly. The intrinsic boundary shrinks from "all owning collections" to "the inline-array primitive itself."

The change is large in surface area (every `T[]` reference in the spec and stdlib is rewritten) but small in user-facing behavior change: most user code that used `T[]` was using it as "a fixed-size sequence of T," and that use case migrates cleanly to `Array<T>` with fixed-size discipline. FFI-shaped code that genuinely needed inline-array semantics now has a proper inline-array type to use.

<a id="sec-language-changes"></a>
### Language changes

- **Inline arrays `[N]T` added** ([§15.1](Bezel_Language_Spec_0_98.md#sec-inline-arrays)). Value-typed, fixed-size set at compile time, stored inline in their enclosing context. N must be a compile-time integer expression greater than zero. Element type may be value-typed, class-typed (stored as inline references), or runtime-managed.

- **Owning inline arrays `[N owned]T?`** ([§15.1.1](Bezel_Language_Spec_0_98.md#sec-owning-inline-arrays)). The `owned` modifier inside the brackets marks the array's slots as owned. The array is a move-only value type ([§15.3](Bezel_Language_Spec_0_98.md#sec-move-only-value-types)). For nullable element types (`T?`), slots default to null; for non-nullable element types, an initializer is required at declaration.

- **Move-only value types** ([§15.3](Bezel_Language_Spec_0_98.md#sec-move-only-value-types)). A new small type-system category: value types whose instances cannot be duplicated. Assignment transfers the value; copying is forbidden. The single member of this category in v0.98 is owning inline arrays. The category exists to support inline arrays carrying owned slots without contradicting the no-shared-ownership rule.

- **`T[]` retired.** All previous uses of `T[]` in the standard library are rewritten to `Array<T>` (the resizable heap form, used in fixed-size discipline where appropriate). The `T[]` syntax is no longer admitted by the grammar.

- **Collection literals are contextually typed** ([§15.4](Bezel_Language_Spec_0_98.md#sec-collection-literals)). A literal `[1, 2, 3]` has no standalone type; it is well-typed only in a context that provides the target type, either via type annotation or method parameter. A literal in a position with no target type is a compile error.

- **Move on inline-array slots** ([§28.11](Bezel_Language_Spec_0_98.md#sec-collections-do-not-own-elements)). `move arr[i]` is admitted on both `[N owned]T?` and `[N owned]T` slots. For nullable forms, the field-restoration rule is satisfied by assignment of `null`; for non-nullable forms, restoration requires assigning a fresh owned T value (the same pattern as for non-nullable `OwnedArray<T>` slots).

- **Multi-dimensional arrays deferred.** The previous `T[,]`, `T[,,]` etc. forms are not retained in v0.98. Multi-dimensional inline arrays (`[N, M]T`) are deferred to a future revision. Users who need multi-dimensional data structures compose `Array<Array<T>>` or similar in v0.98.

- **Slicing scoped to `Array<T>`** ([§15.6](Bezel_Language_Spec_0_98.md#sec-slicing)). Slicing is admitted on heap-allocated `Array<T>`. Inline arrays do not support slicing in v0.98 (a span/view type is anticipated for a future revision). `OwnedArray<T>` slicing is forbidden as before (would duplicate ownership of slot contents).

- **[§28.1](Bezel_Language_Spec_0_98.md#sec-where-the-owned-modifier-can-appear) admissibility extended.** The `owned` modifier admits a new position: the inline-array slot marker `[N owned]…`. The "documentation-only `owned T[]` returns" exception is removed (T[] is gone, the exception is obsolete).

- **[§28.5](Bezel_Language_Spec_0_98.md#sec-owned-fields) owned-field rules updated.** The previous "fixed-size-array-typed (`T[]`) fields may not be `owned`" restriction is rephrased to distinguish two new cases: inline-array fields without the `owned` slot marker (`[N]T`, `[N]T?`) may not carry the `owned` field modifier (no per-slot ownership to manage), and inline-array fields with the `owned` slot marker (`[N owned]T`, `[N owned]T?`) are admissible as fields without an additional field-level `owned` modifier (the slot marker conveys the obligation). Records still cannot have `owned` fields and cannot have inline-array fields with the `owned` slot marker.

- **[§28.9](Bezel_Language_Spec_0_98.md#sec-runtime-managed-types) runtime-managed types list updated.** `T[]` removed from the list. The remaining runtime-managed types are `string`, function values, and boxed values.

<a id="sec-standard-library-changes"></a>
### Standard library changes

- **`Array<T>.ToArray()`** now returns `owned Array<T>` (was `owned T[]`).
- **`IEnumerable<T>.ToArray()`** now returns `owned Array<T>` (was `owned T[]`). The separate `ToResizableArray()` method is removed — `ToArray()` is the single materialization-to-array entry point.
- **Stream methods** (`Read`, `Write`, `ReadAsync`, `WriteAsync`, `ReadAll`, `ReadAllAsync`) take and return `Array<byte>` (was `byte[]`). The v0.95-introduced `ReadAllToArray` / `ReadAllToArrayAsync` convenience methods are removed: with `ReadAll` now returning `Array<byte>` directly, the separate methods became duplicates of the same signature.
- **File methods** (`ReadAllBytes`, `WriteAllBytes`) take and return `Array<byte>` (was `byte[]`).
- **Encoding methods** (`GetBytes`, `GetString`) take and return `Array<byte>` (was `byte[]`).
- **string methods** (`From(buffer: ...)`, `Split(separators: ...)`, `ToCharArray()`) take and return `Array<char>` (was `char[]`).
- **FFI types** (`CString.FromBytes`, `CString.ToBytes`, `RawBytes.From`, `RawBytes.CopyTo`) take and return `Array<byte>` (was `byte[]`). The `RawBytes.FromArray` overload, previously redundant with `From(byte[])`, is folded into `From(Array<byte>)`.
- **Multi-dimensional `T[,]` extension methods** (`EnumerateRowMajor`, `EnumerateColumnMajor`) removed; multi-dim is deferred to a future revision.
- **`Random.NextBytes(buffer: ...)`** takes `Array<byte>` (was `byte[]`).
- **`Random.ToByteArray()`** returns `owned Array<byte>` (was `owned byte[]`).

<a id="sec-rationale-additions"></a>
### Rationale additions

- **New rationale [§5.14](#sec-inline-arrays-and-array-kind-split)** explains the inline-array design, the array-kind split, the move-only value-type category, and the migration story.
- **Rationale [§17.6](#sec-inline-fixed-size-value-type-array-fields)** "Inline fixed-size value-type array fields" updated from "rejected" to "provided in v0.98," with a brief note on why the rejection rationale didn't survive.

<a id="sec-migration"></a>
### Migration

Programs that compiled under v0.97 need the following changes:

- **`T[]` references in code** rewrite to `Array<T>` in most cases. The mechanical replacement is correct when the array is allocated with `new` and consumed at a single point. For uses where the fixed-size discipline was important (e.g., FFI buffer marshalling), consider rewriting to inline `[N]T` where N is known at compile time.
- **`new T[N]` expressions** rewrite to one of: `new Array<T>(capacity := N)` (if T is value-typed and the array is mutated), an annotated declaration `using var arr: Array<T> := [...]` initialized from a collection literal, or a declaration `var arr: [N]T` for inline usage when N is a compile-time constant.
- **Collection literals without target type** — `var x := [1, 2, 3]` — now require an annotation: `var x: Array<int> := [1, 2, 3]` or `var x: [3]int := [1, 2, 3]`. This is a source-breaking change but flags genuinely ambiguous code.
- **LINQ `ToResizableArray()` calls** rewrite to `ToArray()` (which now returns `Array<T>`).

<a id="sec-v0-97-to-v0-98-changelog-what-did-not-change"></a>
<a id="sec-changelog-what-did-not-change"></a>
### What did not change

- Heap arrays (`Array<T>`, `OwnedArray<T>`) retain their identity and behavior, including resizability.
- The ownership system's core rules (`owned` modifier, using-var, field-restoration, exception-path discharge) are unchanged.
- Records remain unable to own (records cannot have `owned` fields and cannot have `[N owned]…` inline-array fields).
- The runtime-management exemption for `string`, function values, and boxed values is preserved.
- FFI marshalling for value-typed elements is unchanged; the new inline-array form is an additional option for FFI buffers.

---

<a id="sec-v0-98-to-v0-99-changelog"></a>
### v0.98 to v0.99 changelog

v0.99 is a focused addition. It introduces two stdlib types and a small amount of supporting spec wording. No source-breaking changes to v0.98 user code; the existing collection APIs (`OwnedArray<T>`, `OwnedDictionary<K, V>`, etc.) are unchanged in surface and now have documented reference implementations in pure Bezel.

<a id="sec-v0-98-to-v0-99-summary"></a>
<a id="sec-summary"></a>
### Summary

The v0.98 rationale claimed that inline arrays with the `[N owned]T?` form made the owning-collection family expressible in pure Bezel. This was only partly true: the per-slot ownership-transfer operations were expressible, but the *runtime-sized backing storage* used by resizable owning collections remained compiler-intrinsic, because `[N owned]T?` requires a compile-time N. v0.99 closes this gap by introducing `OwnedBuffer<T>` — a class wrapping runtime-sized owning storage with the same per-slot semantics extended to runtime-determined N — and uses it as the storage primitive for `OwnedArray<T>`, `OwnedDictionary<K, V>`, and the rest of the resizable owning collection family.

The same primitive serves a second purpose: it gives user code a safe, no-`unsafe`-keyword escape hatch for working with raw owning storage. The companion types `Span<T>` and `ReadOnlySpan<T>` are records carrying a borrow of an `OwnedBuffer<T>`, viewing into a contiguous range of slots; they are purely user-level Bezel (records with a borrow-bearing reference field) and use the existing borrow-lifetime tracking to ensure spans cannot outlive their buffers.

<a id="sec-v0-98-to-v0-99-stdlib-changes"></a>
<a id="sec-v0-98-to-v0-99-changelog-standard-library-changes"></a>
<a id="sec-changelog-standard-library-changes"></a>
### Standard library changes

- **New [§4.14](Bezel_Standard_Library_0_99.md#sec-owned-buffers-and-spans)** — `OwnedBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>`. `OwnedBuffer<T>` is a compiler-known class (the new intrinsic surface for owning storage); `Span<T>` and `ReadOnlySpan<T>` are pure-Bezel records using the existing borrow-bearing-record machinery.
- **[§4.5](Bezel_Standard_Library_0_99.md#sec-ownedarrayt) `OwnedArray<T>`** — public API unchanged. A new "Reference implementation" subsection shows the type composed over `OwnedBuffer<T>` in pure Bezel. The standard library uses this implementation directly.
- **[§4.9](Bezel_Standard_Library_0_99.md#sec-owneddictionaryk-v) `OwnedDictionary<K, V>`** — public API unchanged. A new "Reference implementation" subsection shows the type composed over `OwnedBuffer<Entry<K, V>>` with chained hashing, using the standard `move`-and-null pattern for bucket and chain mutations.

The other resizable owning collections (`OwnedSet<T>`, `OwnedLinkedList<T>`, `OwnedStack<T>`, `OwnedQueue<T>`) compose over `OwnedBuffer<T>` or other owning-collection primitives in the same way; their reference implementations follow the patterns of `OwnedArray<T>` and `OwnedDictionary<K, V>` and are not separately documented.

<a id="sec-v0-98-to-v0-99-spec-changes"></a>
<a id="sec-language-spec-changes"></a>
### Language spec changes

The spec edits split into one direct change for the new types and several clarifications that surfaced during verification of the v0.99 reference implementations.

Direct changes for the new types:

- **[§28.11](Bezel_Language_Spec_0_99.md#sec-collection-element-ownership)** "What `move coll[i]` is admitted on" list extended to include `OwnedBuffer<T>`. The mechanism is the same as for other owning heap collections; the rule simply names the new type.
- **[§28.3](Bezel_Language_Spec_0_99.md#sec-local-variables-ownership-and-borrows) producer table** correspondingly updated to include `OwnedBuffer<T>` in the `move coll[i]` row.
- **[§14.4](Bezel_Language_Spec_0_99.md#sec-owned-and-generic-type-arguments)** class-only restriction enumeration extended to include `OwnedBuffer<T>`.
- **[§28.9](Bezel_Language_Spec_0_99.md#sec-runtime-managed-types)** non-trivial-dispose enumeration extended to include `OwnedBuffer<T>`.

Clarifications surfaced by writing the reference implementations as actual Bezel code (none change the language's behavior, only make existing rules more precise):

- **[§28.3](Bezel_Language_Spec_0_99.md#sec-local-variables-ownership-and-borrows) producer table** now includes a `null` literal row. The literal is context-typed; for nullable-class locals initialized to null, the local's ownership state is determined by the next non-null assignment, with a polymorphic-null state preceding it. Needed for the `var rebuilt: Entry<K, V>? := null;` pattern in the OwnedDictionary reference implementation.
- **[§28.3](Bezel_Language_Spec_0_99.md#sec-local-variables-ownership-and-borrows) exception-path discharge** rule widened to cover owned parameters in addition to owned locals. Needed for the `Add(key: K, owned value: V)` pattern that throws `ArgumentException` on duplicate keys; the owned `value` parameter must be auto-disposed on the exception edge.
- **[§28.7](Bezel_Language_Spec_0_99.md#sec-reassigning-owned-locals)** new subsection "Reassigning owned locals" added, paralleling the existing rule for owned fields. Needed for the `chain := rest;` pattern after `chain.Dispose();` in the OwnedDictionary reference implementation.
- **[§28.11](Bezel_Language_Spec_0_99.md#sec-collection-element-ownership) indexer setter on a moved-from slot** documented: writing through the indexer setter to a slot already in moved-from state does not trigger the owned-setter's "dispose previous value" semantics. Needed for the `_storage[i] := null;` restoration after `move _storage[i]`.
- **[§A.5](Bezel_Language_Spec_0_99.md#sec-types) `FunctionType` grammar** extended to admit `"owned"?` before the return type. The form `() -> owned Connection` appeared in prose throughout the spec but was missing from the grammar.
- **[§A.7](Bezel_Language_Spec_0_99.md#sec-expressions) `MoveExpr` grammar comment** updated: the comment said the operand must be a field access, but `move coll[i]` indexer-access form was already admitted by [§28.11](Bezel_Language_Spec_0_99.md#sec-collection-element-ownership). The comment now reflects both forms.

No new keywords, no new operators. The type-system rules are unchanged in substance.

<a id="sec-v0-98-to-v0-99-rationale-changes"></a>
<a id="sec-rationale-changes"></a>
### Rationale changes

- **[§5.14](#sec-inline-arrays-and-array-kind-split)** "Foundational implementability" paragraph rewritten to honestly describe what v0.98 achieved and what v0.99 completes. The earlier wording overclaimed; the new wording distinguishes "per-slot operations" (v0.98) from "runtime-sized backing storage" (v0.99).
- **New [§5.15](#sec-owned-buffers-and-spans)** "Owned buffers and spans" introduces the v0.99 additions in detail: the gap v0.98 left, why a stdlib class rather than new syntax, why the buffer's slots are nullable, why spans are records rather than C#-style `ref struct`s, why no `unsafe` keyword, and what remains deferred (stack-allocated runtime-sized buffers, mutable spans, untyped bit-level reads).

<a id="sec-v0-98-to-v0-99-migration"></a>
<a id="sec-v0-98-to-v0-99-changelog-migration"></a>
<a id="sec-changelog-migration"></a>
### Migration

None required. v0.99 is fully backward-compatible with v0.98 user code. The compiler-intrinsic implementations of the resizable owning collections that v0.98 documented are now expressible at the user level; the public APIs are unchanged.

User code that wanted to write its own resizable owning container in v0.98 (and was forced to either fake it with `Array<T>` plus per-element bookkeeping or wait) can now use `OwnedBuffer<T>` directly as the storage primitive.

<a id="sec-v0-98-to-v0-99-what-did-not-change"></a>
<a id="sec-v0-98-to-v0-99-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-2"></a>
### What did not change

- Inline arrays (`[N]T` and `[N owned]T?`) retain their v0.98 form. They remain the primitive for fixed-compile-time-size storage.
- The ownership system's core rules (`owned` modifier, `using var`, field-restoration, move-only value types, exception-path discharge) are unchanged.
- The borrow-lifetime tracking machinery is unchanged. `Span<T>` uses the existing borrow-bearing-record rule (`[§9.7](Bezel_Language_Spec_0_99.md#sec-records-and-disposal)` of the spec).
- The FFI surface is unchanged. `OwnedBuffer<T>` is not FFI-compatible (it is a class wrapping a header-bearing allocation); FFI-side buffers continue to use inline arrays `[N]byte` or `RawBytes`.
- No new keywords, no new operators, no new type-system primitives.

---

<a id="sec-v0-99-to-v0-100-changelog"></a>
### v0.99 to v0.100 changelog

v0.100 is a small, focused revision that closes one of the deferred items from v0.99: the read/write capability split between `Span<T>` and `ReadOnlySpan<T>`. No source-breaking changes; the v0.99 surface is a strict subset of v0.100's.

<a id="sec-v0-99-to-v0-100-changelog-summary"></a>
<a id="sec-changelog-summary"></a>
### Summary

v0.99 shipped `Span<T>` and `ReadOnlySpan<T>` as mechanically-equivalent read-only types, with the v0.99 rationale stating that a mutable variant was deferred. On review, that design was incoherent: two type names with identical semantics, distinguished only by an *anticipated* future capability, asked callers to encode forward-looking promises in present-day code without any concrete benefit. v0.100 resolves this by making `Span<T>` actually read-write — its indexer gains a setter that delegates to the underlying `OwnedBuffer<T>`'s owned-setter. `ReadOnlySpan<T>` stays as it is. The two types now have a real, present-day semantic difference that justifies their separate existence.

<a id="sec-v0-99-to-v0-100-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-2"></a>
### Standard library changes

- **[§4.14](Bezel_Standard_Library_0_100.md#sec-owned-buffers-and-spans) `Span<T>`** — indexer gains a setter `set(owned)` that delegates to `Buffer[Start + i] := value`. The setter takes ownership of the assigned value and stores it in the underlying buffer slot; the buffer's owned-setter disposes whatever the slot previously held. Writing through a span is mechanically the same as writing through the buffer directly — the span is a parameterized view, not a separate ownership root. `move span[i]` is also admitted, delegating to `move span.Buffer[span.Start + i]` and leaving the buffer slot in moved-from state.
- **[§4.14](Bezel_Standard_Library_0_100.md#sec-owned-buffers-and-spans) `ReadOnlySpan<T>`** — unchanged. The type's read-only-ness is now a real, present-day contract rather than a placeholder for a future split.
- **[§4.14](Bezel_Standard_Library_0_100.md#sec-owned-buffers-and-spans) framing** — the "both span types are read-only by design choice" prose is replaced by the clean read/write split. The rationale that the C# name precedent justifies the split is now substantive: `Span<T>` and `ReadOnlySpan<T>` map to the corresponding C# types in capability as well as in name.

<a id="sec-v0-99-to-v0-100-changelog-language-spec-changes"></a>
<a id="sec-changelog-language-spec-changes"></a>
### Language spec changes

- **[§28.11](Bezel_Language_Spec_0_100.md#sec-collection-element-ownership)** "What `move coll[i]` is admitted on" list extended to include `Span<T>` (via delegation to the underlying buffer). `ReadOnlySpan<T>` is explicitly excluded.
- **[§28.3](Bezel_Language_Spec_0_100.md#sec-local-variables-ownership-and-borrows) producer table** correspondingly updated to mention `Span<T>` in the `move coll[i]` row.

No new keywords, no new operators, no new type-system rules. The setter on `Span<T>`'s indexer uses the existing borrow-bearing-record machinery (calling a setter on a borrowed reference) without any extension.

<a id="sec-v0-99-to-v0-100-changelog-rationale-changes"></a>
<a id="sec-changelog-rationale-changes"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** "What this does not solve" list updated: the *mutable spans* item is removed (delivered in v0.100), and the remaining items (stackalloc analog, untyped bit reads) carry forward. A new "The read/write split" paragraph explains the v0.100 resolution.
- **[§18](#sec-things-deferred)** "Things deferred" entry for spans split into three: (a) stackalloc analog (low priority, deferred); (b) spans over additional view sources — inline arrays, strings, heap `Array<T>` (deferred, narrower scope); (c) untyped bit-level reads on byte buffers (deferred, stdlib expansion).

<a id="sec-v0-99-to-v0-100-changelog-migration"></a>
<a id="sec-migration-2"></a>
### Migration

No user code needs to change for v0.100. The v0.99 form (both span types read-only) is a strict subset of the v0.100 form. Programs that wanted to write through a span in v0.99 (and worked around the limitation by passing `owned OwnedBuffer<T>` directly to the writer) can switch to passing `Span<T>` instead, which is cleaner. The workaround remains valid; no migration is forced.

<a id="sec-v0-99-to-v0-100-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-3"></a>
### What did not change

- `OwnedBuffer<T>` is unchanged.
- The owning-collection reference implementations (`OwnedArray<T>`, `OwnedDictionary<K, V>`) are unchanged.
- All v0.99 spec clarifications (null-literal producer rule, exception-path discharge for owned parameters, owned-local reassignment subsection, indexer-setter-on-moved-from-slot rule, FunctionType grammar, MoveExpr grammar comment) carry forward unchanged.
- The borrow-lifetime tracking machinery is unchanged. `Span<T>`'s new setter exercises existing rules — calling a setter through a borrowed reference works the same way calling any instance method through a borrowed reference does.
- No FFI changes; no async changes; no changes to the type system, ownership model, or class hierarchy.

---

<a id="sec-v0-100-to-v0-101-changelog"></a>
### v0.100 to v0.101 changelog

v0.101 is a focused API-surface expansion for `Span<T>` and `ReadOnlySpan<T>`. v0.100 established the read/write capability split (`Span<T>` is read-write; `ReadOnlySpan<T>` is read-only); v0.101 thickens both types' method surface to support common operations directly. No source-breaking changes; v0.100 user code compiles unchanged against v0.101.

<a id="sec-v0-100-to-v0-101-changelog-summary"></a>
<a id="sec-summary-2"></a>
### Summary

v0.100 left both span types with a minimal surface: indexer, `Slice`, and (on Span<T>) `AsReadOnly`. Common operations — iteration, search, equality, bulk mutation — required users to write index-loops by hand. v0.101 adds the methods that close this gap without expanding the type system. The choice of methods is conservative: every operation has unambiguous owned-class-slot semantics, and operations that don't (such as `Fill(value)` or `CopyFrom(source)`, which would require implicit cloning of class references) are explicitly omitted with rationale.

<a id="sec-v0-100-to-v0-101-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-3"></a>
### Standard library changes

- **[§4.14](Bezel_Standard_Library_0_101.md#sec-spant) `Span<T>`** — adds `IsEmpty: bool`, `IndexOf(value: T?): int`, `Contains(value: T?): bool`, `SequenceEqual(other: Span<T>): bool`, `Clear()`, `MoveFrom(source: Span<T>)`, `Reverse()`. The new prose explains each method's semantics and the rationale for omitting `Fill` and `CopyFrom`.
- **[§4.14](Bezel_Standard_Library_0_101.md#sec-readonlyspant) `ReadOnlySpan<T>`** — adds `IsEmpty: bool`, `IndexOf(value: T?): int`, `Contains(value: T?): bool`, `SequenceEqual(other: ReadOnlySpan<T>): bool`. No mutation methods (consistent with the type's read-only contract).

<a id="sec-v0-100-to-v0-101-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-2"></a>
### Language spec changes

- **[§29.2](Bezel_Language_Spec_0_101.md#sec-desugaring-of-for)** — a paragraph added stating that `for var x in span do ... end` over a `Span<T>` or `ReadOnlySpan<T>` desugars to a direct indexed loop, parallel to the existing inline-array rule. Spans do not implement `IEnumerable<T?>` (a borrow-bearing record can't be boxed to a heap-bound interface reference without losing the borrow source), so the enumerator path does not apply.

No new keywords, no new operators, no new type-system rules. The for-loop extension follows the existing pattern for type-directed desugaring (inline arrays, ranges).

<a id="sec-v0-100-to-v0-101-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-2"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — adds an "API surface expansion" paragraph explaining what v0.101 added and why `Fill`/`CopyFrom` were deliberately omitted.
- **[§18](#sec-things-deferred)** — the "Stack-allocated runtime-sized buffers" and "Spans over additional view sources" entries are updated to reflect v0.101's status. The list of remaining deferred items is unchanged.

<a id="sec-v0-100-to-v0-101-changelog-migration"></a>
<a id="sec-migration-3"></a>
### Migration

No user code needs to change for v0.101. The v0.100 form is a strict subset: every v0.100 program compiles against v0.101 without modification. Programs that wrote their own index-loops for iteration, search, equality, or bulk mutation in v0.100 can now use the new methods; the explicit loops remain valid.

<a id="sec-v0-100-to-v0-101-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-4"></a>
### What did not change

- `OwnedBuffer<T>` is unchanged.
- The owning-collection reference implementations (`OwnedArray<T>`, `OwnedDictionary<K, V>`) are unchanged.
- The borrow-lifetime tracking machinery is unchanged. The new span methods exercise existing rules (calling methods through a borrow, returning value-typed results).
- No FFI changes; no async changes; no changes to the type system, ownership model, or class hierarchy.

---

<a id="sec-v0-101-to-v0-102-changelog"></a>
### v0.101 to v0.102 changelog

v0.102 generalizes `OwnedBuffer<T>`, `Span<T>`, and `ReadOnlySpan<T>` to admit value-typed T in addition to class-typed T. v0.99–v0.101 restricted T to class types; v0.102 removes that restriction. No source-breaking changes; v0.101 user code compiles unchanged against v0.102.

<a id="sec-v0-101-to-v0-102-changelog-summary"></a>
<a id="sec-summary-3"></a>
### Summary

The v0.99 design restricted the buffer/span family to class-typed T to make ownership semantics clean: slots are `T?`, owned, with disposal cascade and `move` admission. The restriction excluded an important use case — `OwnedBuffer<byte>` and friends — because primitives are value-typed. v0.102 lifts the restriction by admitting two slot-semantics modes:

- For class-typed T: the v0.99–v0.101 semantics carry forward unchanged.
- For value-typed T (primitives, records, enums, non-move-only inline arrays): slots are `T` directly, no ownership, no Dispose cascade, `move` not admitted.

The two modes share the indexer, `Slice`, `AsReadOnly` (on `Span<T>`), `IsEmpty`, `IndexOf`, `Contains`, `SequenceEqual`, iteration (via the [§29.2](Bezel_Language_Spec_0_102.md#sec-desugaring-of-for) desugaring rule), and `Reverse` (on `Span<T>`). They differ in the slot type (`T?` vs `T`), the indexer setter modifier (`owned` vs none), the admissibility of `move`, and the bulk-mutation API (class T has `Clear`/`MoveFrom`; value T has `Fill`/`CopyFrom`).

The dispatch is *type-parameter-category-dependent specialization* — a new kind of generic specialization specific to these three types. The compiler resolves T's category at instantiation and emits the appropriate semantics. The mechanism is not exposed as a general user-available feature in v0.102; future revisions may generalize.

<a id="sec-v0-101-to-v0-102-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-4"></a>
### Standard library changes

- **[§4.14](Bezel_Standard_Library_0_102.md#sec-ownedbuffert) `OwnedBuffer<T>`** — `where T: class` constraint removed. The class declaration now admits both class-typed T (v0.99 semantics, unchanged) and value-typed T (new in v0.102, value semantics). The intro paragraph describes the slot-semantics dispatch. The class declaration is presented in two forms (class-T and value-T) to make the dispatch concrete. For value T, the single-argument constructor requires T to have a default value; the two-argument constructor's init function returns T (not `owned T`). The class-T `IEnumerable<T?>` conformance is preserved; value-T variants do not implement IEnumerable (the type-parameter-conditional interface implementation is not a general Bezel mechanism).
- **[§4.14](Bezel_Standard_Library_0_102.md#sec-spant) `Span<T>`** — `where T: class` constraint removed. For class T, all v0.101 surface preserved (indexer with `set(owned)`, `move span[i]`, `Clear`, `MoveFrom`, `Reverse`). For value T, the indexer takes plain `set`, `move span[i]` is not admitted, and the bulk-mutation API is `Fill(value: T)` and `CopyFrom(source: ReadOnlySpan<T>)` plus `Reverse`. New prose paragraphs explain the category dispatch and the rationale for the different bulk-mutation surfaces.
- **[§4.14](Bezel_Standard_Library_0_102.md#sec-readonlyspant) `ReadOnlySpan<T>`** — `where T: class` constraint removed. The read-only surface adapts to T's category (indexer return type, `IndexOf`/`Contains` parameter types). No mutation surface (consistent with the type's contract).

<a id="sec-v0-101-to-v0-102-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-3"></a>
### Language spec changes

- **[§14.4](Bezel_Language_Spec_0_102.md#sec-owned-and-generic-type-arguments)** — class-only restriction list updated. `OwnedBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>` are now noted as exceptions: they admit either class-typed or value-typed T (excluding runtime-managed reference types and move-only value types).
- **[§28.3](Bezel_Language_Spec_0_102.md#sec-local-variables-ownership-and-borrows) producer table** — `move coll[i]` row updated to note that for `OwnedBuffer<T>` / `Span<T>` with value-typed T, `move` is not admitted (values have no ownership to transfer).
- **[§28.9](Bezel_Language_Spec_0_102.md#sec-runtime-managed-types)** — non-trivial-dispose paragraph updated to note that `OwnedBuffer<T>` is non-trivial-dispose for both class and value T (the buffer's allocation requires release in either case).
- **[§28.11](Bezel_Language_Spec_0_102.md#sec-collection-element-ownership)** — `move coll[i]` admitted-on list updated to clarify the class-T requirement for buffer/span moves. The "indexer setter on a moved-from slot" paragraph is unchanged (it concerns class-T semantics, which are unchanged). The "restoration on `move coll[i]`" enumeration updated to clarify the class-T scope.
- **[§29.2](Bezel_Language_Spec_0_102.md#sec-desugaring-of-for)** — for-loop desugaring extended: for `OwnedBuffer<T>` with value-typed T, the desugaring is a direct indexed loop (parallel to spans and inline arrays). The iteration variable's type follows the slot type — `T?` for class T (existing), `T` for value T (new).
- **[§A.7](Bezel_Language_Spec_0_102.md#sec-expressions) `MoveExpr` grammar comment** updated to mention the class-T constraint for buffer/span move admission.

No new keywords, no new operators. The class/value category dispatch is compiler-known behavior specific to these three types; it does not generalize to a user-available mechanism.

<a id="sec-v0-101-to-v0-102-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-3"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — the "Why slots are nullable" paragraph clarified to apply only to class T (for value T, the question doesn't arise). A new "The class/value category split" paragraph explains the v0.102 generalization, its motivation (binary buffers), and the design machinery cost (a new kind of specialization, scoped to these three types). A new "Why move-only value types are not yet admitted" paragraph documents the deferral. The "What this does not solve" list reduced to two items (the "untyped bit-level reads" item now notes that v0.102's value-T support is the prerequisite for the deferred byte-accessor methods).
- **[§18](#sec-things-deferred)** — the spans-related entries updated: "Stack-allocated runtime-sized buffers" now mentions v0.102; "Spans over additional view sources" updated; a new entry "Move-only value types in OwnedBuffer/Span" added; "Untyped bit-level reads on byte buffers" updated to note that v0.102 makes the prerequisite types exist.

<a id="sec-v0-101-to-v0-102-changelog-migration"></a>
<a id="sec-migration-4"></a>
### Migration

No user code needs to change for v0.102. The v0.101 form (class-T-only buffers and spans) is a strict subset of the v0.102 form. Any program that compiled against v0.101 compiles against v0.102 without modification.

Programs that wanted byte buffers or other value-typed buffers in v0.101 (and worked around the limitation with `Array<byte>`, manual byte-handling, or FFI) can switch to `OwnedBuffer<byte>` if the owning-allocation semantics fit better. The owning collections (`OwnedArray<T>`, `OwnedDictionary<K, V>`, etc.) retain their `where T: class` constraints — they were not generalized; their semantics fundamentally require owned elements.

<a id="sec-v0-101-to-v0-102-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-5"></a>
### What did not change

- The owning collections (`OwnedArray<T>`, `OwnedLinkedList<T>`, `OwnedSet<T>`, `OwnedStack<T>`, `OwnedQueue<T>`, `OwnedDictionary<K, V>`) keep their `where T: class` constraints. These types' Add/Remove/Pop semantics fundamentally use owned arguments; generalizing them would change every method signature in a non-trivial way and the motivation (value-typed owning collections) is unclear in practice. Borrowing collections (`Array<T>`, `Dictionary<K, V>`, etc.) already admit any T.
- The reference implementations of `OwnedArray<T>` and `OwnedDictionary<K, V>` are unchanged — they use class-typed `OwnedBuffer<T>` and `OwnedBuffer<Entry<K, V>>` respectively, both of which retain v0.99 semantics.
- The borrow-lifetime tracking machinery is unchanged. The new value-typed buffer/span semantics use existing rules (records carrying class borrows, indexer dispatch, etc.).
- No new keywords, no new operators, no new type-system rules.
- No FFI changes; no async changes; no changes to the class hierarchy.

---

<a id="sec-v0-102-to-v0-103-changelog"></a>
### v0.102 to v0.103 changelog

v0.103 is a pure stdlib expansion: byte-buffer accessors on `Span<byte>` and `ReadOnlySpan<byte>`, plus a small `BitConverter` utility class. No language changes; no source-breaking changes; v0.102 user code compiles unchanged against v0.103.

<a id="sec-v0-102-to-v0-103-changelog-summary"></a>
<a id="sec-summary-4"></a>
### Summary

v0.102 generalized `OwnedBuffer<T>`, `Span<T>`, and `ReadOnlySpan<T>` to admit value-typed T — which made `Span<byte>` and `ReadOnlySpan<byte>` exist for the first time. v0.103 cashes in that generalization by adding the endianness-aware reads and writes that binary I/O code needs. The methods come as extension blocks on the two byte-span types, declared in the standard library so user code does not repeat the bit-shuffling for every parser.

Float bit handling is routed through a new `BitConverter` utility class (`System.BitConverter`) with `FloatToInt32Bits`, `Int32BitsToFloat`, `DoubleToInt64Bits`, `Int64BitsToDouble`. This is the only general bit-reinterpretation surface in the standard library — the byte-span accessors are its first consumer, but user code with custom bit-twiddling needs (custom encodings, hashing, exact float comparison) can use it directly.

<a id="sec-v0-102-to-v0-103-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-5"></a>
### Standard library changes

- **[§2.12](Bezel_Standard_Library_0_103.md#sec-bitconverter)** new — `BitConverter` static class with four methods: `FloatToInt32Bits`, `Int32BitsToFloat`, `DoubleToInt64Bits`, `Int64BitsToDouble`. Each is a pure bit-reinterpretation between a float and its IEEE 754 bit pattern. NaN bit patterns are preserved.
- **[§4.14](Bezel_Standard_Library_0_103.md#sec-byte-buffer-accessors)** new subsection — "Byte-buffer accessors" — declares two extension blocks. `extension ReadOnlySpan<byte>` provides 32 read methods: 16 throwing (`ReadInt16LittleEndian`, `ReadInt16BigEndian`, `ReadUInt16LittleEndian`, `ReadUInt16BigEndian`, and the same set for Int32/UInt32/Int64/UInt64/Float/Double) and 16 corresponding `Try*` variants. `extension Span<byte>` provides the same 32 read methods (each delegating to the corresponding `this.AsReadOnly()` method) plus 32 write methods (16 throwing `Write*` and 16 `TryWrite*`). The rationale prose explains the endianness-in-name choice, the slice-then-read access model, the throw-vs-Try distinction for both reads and writes, and the routing through `BitConverter` for floats.

<a id="sec-v0-102-to-v0-103-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-4"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — "What this does not solve" list shrinks to one item (stack-allocated runtime-sized buffers). A new "Byte-buffer accessors (v0.103)" paragraph explains the surface and the design choices.
- **[§18](#sec-things-deferred)** — the byte-accessor deferred entry is removed (now implemented). The stack-allocated-buffer entry is updated to mention v0.103.

<a id="sec-v0-102-to-v0-103-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-4"></a>
### Language spec changes

None. v0.103 is purely a stdlib addition; the byte accessors and `BitConverter` use existing language mechanisms (extension blocks, static classes, primitive types, explicit conversions, bitwise operators).

<a id="sec-v0-102-to-v0-103-changelog-migration"></a>
<a id="sec-migration-5"></a>
### Migration

No user code needs to change for v0.103. The v0.102 form is a strict subset: every v0.102 program compiles against v0.103 without modification. Programs that wrote their own endianness-aware byte handling in v0.102 (typically via inline bit-shifting or FFI to a C function) can now use the standard methods; the hand-written forms remain valid.

<a id="sec-v0-102-to-v0-103-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-6"></a>
### What did not change

- `OwnedBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>` themselves are unchanged. Only extension blocks and a new utility class were added.
- The borrow-lifetime tracking machinery is unchanged. Extension methods use existing rules (statically dispatched on the receiver's type).
- No FFI changes (though `BitConverter` may use FFI in its implementation — the user-visible surface stays in pure Bezel).
- No new keywords, no new operators, no new type-system rules.

---

<a id="sec-v0-103-to-v0-104-changelog"></a>
### v0.103 to v0.104 changelog

v0.104 introduces `ReadOnlyStringSpan`, a read-only view over a contiguous range of code points in a `string`, plus four `AsSpan()` overloads on `string`. No language changes; no source-breaking changes; v0.103 user code compiles unchanged against v0.104.

<a id="sec-v0-103-to-v0-104-changelog-summary"></a>
<a id="sec-summary-5"></a>
### Summary

v0.99-v0.103 spans (`Span<T>` and `ReadOnlySpan<T>`) view only into `OwnedBuffer<T>`. The string-source case — a read-only view over a contiguous range of a `string` — has been deferred since v0.99. v0.104 lands it as a separate parallel type, `ReadOnlyStringSpan`, rather than extending the existing `ReadOnlySpan<T>` to accept a second source type. The reasoning is in rationale [§5.15](#sec-owned-buffers-and-spans): extending the existing type would either change its record shape or require a general source-polymorphism mechanism that v0.104 should not introduce on its own. The separate type keeps the existing Span<T>/ReadOnlySpan<T> family unchanged and makes the source visible at API boundaries.

The use case is zero-allocation substring parsing — tokenization, prefix/suffix checks, format parsing — that previously required materializing intermediate strings. With `ReadOnlyStringSpan`, those operations work on logical views without allocation.

<a id="sec-v0-103-to-v0-104-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-6"></a>
### Standard library changes

- **[§3.3](Bezel_Standard_Library_0_104.md#sec-readonlystringspan)** new — `ReadOnlyStringSpan` record. Surface: indexer (getter only, returning `char`, with span-level bounds checking), `Length`, `IsEmpty`, `Slice`, `IndexOf(char)`, `Contains(char)`, `IndexOf(ReadOnlyStringSpan)`, `IndexOf(string)`, `Contains(ReadOnlyStringSpan)`, `Contains(string)`, `StartsWith(ReadOnlyStringSpan)`, `StartsWith(string)`, `EndsWith(ReadOnlyStringSpan)`, `EndsWith(string)`, `SequenceEqual(ReadOnlyStringSpan)`, `SequenceEqual(string)`, `ToString` (materializes if not full source). String overloads are convenience wrappers that delegate via `value.AsSpan()` — no allocation, just record construction. Carries a borrow of the source string.
- **[§9.6](Bezel_Standard_Library_0_104.md#sec-string) `String`** — four new methods: `AsSpan(): ReadOnlyStringSpan`, `AsSpan(start: int): ReadOnlyStringSpan`, `AsSpan(start: int, length: int): ReadOnlyStringSpan`, `AsSpan(r: Range<int>): ReadOnlyStringSpan` (the last paralleling the existing `[r: Range<int>]: string` slicing form). The standard entry points for constructing a `ReadOnlyStringSpan` over a string. The prose note about "future zero-copy view" is updated to reflect that the char-level view now exists; the byte-level view remains deferred.

<a id="sec-v0-103-to-v0-104-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-5"></a>
### Language spec changes

- **[§29.2](Bezel_Language_Spec_0_104.md#sec-desugaring-of-for)** — a paragraph added stating that `for var c in stringSpan do ... end` over a `ReadOnlyStringSpan` desugars to a direct indexed loop, parallel to the OwnedBuffer-backed span case. `ReadOnlyStringSpan` does not implement `IEnumerable<char>` (it is a borrow-bearing record carrying a string borrow). The iteration variable's type is `char` and the loop body sees a copy of each code point.

No new keywords, no new operators, no new type-system rules. The new type uses existing language mechanisms (borrow-bearing records, indexer dispatch, the standard for-loop desugaring pattern).

<a id="sec-v0-103-to-v0-104-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-5"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — a new "String spans (v0.104)" paragraph explains the new type, the choice of a separate type over extending `ReadOnlySpan<char>`, and the cost of having two parallel type names for "read-only view of char sequence."
- **[§15](#sec-strings)** — the paragraph about deferred zero-copy views is updated: char-level views now exist as `ReadOnlyStringSpan`; the byte-level view from ASCII strings remains deferred.
- **[§18](#sec-things-deferred)** — the "Spans over additional view sources" entry is updated: the `ReadOnlySpan<char>` over `string` sub-item is closed (delivered as `ReadOnlyStringSpan`); the remaining items (inline-array spans, heap-`Array<T>` spans, ASCII byte view) stay deferred.

<a id="sec-v0-103-to-v0-104-changelog-migration"></a>
<a id="sec-migration-6"></a>
### Migration

No user code needs to change for v0.104. The v0.103 form is a strict subset: every v0.103 program compiles against v0.104 without modification. Programs that previously materialized intermediate strings for substring parsing (via `Substring`, `[Range<int>]` slicing, or hand-written substring methods) can now use `AsSpan()` and `ReadOnlyStringSpan` operations to avoid allocation. The materialized-substring forms remain valid.

<a id="sec-v0-103-to-v0-104-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-7"></a>
### What did not change

- `OwnedBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>` are unchanged. `ReadOnlyStringSpan` is a separate, parallel type.
- `string`'s existing surface is unchanged. Only `AsSpan()` overloads are added.
- The borrow-lifetime tracking machinery is unchanged. `ReadOnlyStringSpan` carries a borrow of a `string`, which is a class type — the borrow tracker handles it identically to other class borrows.
- No FFI changes; no async changes; no type-system rules.

---

<a id="sec-v0-104-to-v0-105-changelog"></a>
### v0.104 to v0.105 changelog

v0.105 is a bug-fix release: span-level bounds checking on `Span<T>` and `ReadOnlySpan<T>` indexers (a latent correctness bug since v0.99). No language changes; no surface-syntax changes; behavior change only for code that read or wrote outside its declared span range (which is the bug being fixed).

<a id="sec-v0-104-to-v0-105-changelog-summary"></a>
<a id="sec-summary-6"></a>
### Summary

v0.99-v0.104 declared the span indexers with a delegating expression body — `public property [i: int]: T? => Buffer[Start + i]` — which bounds-checked the index only against the underlying buffer's length, not against the span's `Length`. Span `[i]` for `i >= span.Length` silently succeeded if `Start + i` was within the buffer's bounds. v0.105 fixes the four affected indexers (`Span<T>` for class T and value T; `ReadOnlySpan<T>` for class T and value T) and the `move span[i]` form on class-typed `Span<T>` to bounds-check against the span's `Length` before delegating.

The semantics now align with C#'s `Span<T>`/`ReadOnlySpan<T>` and with v0.104's `ReadOnlyStringSpan` (which used the explicit form from the start).

<a id="sec-v0-104-to-v0-105-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-7"></a>
### Standard library changes

- **[§4.14](Bezel_Standard_Library_0_105.md#sec-owned-buffers-and-spans) `Span<T>`** — class-T and value-T indexers updated. Getter and setter each have an explicit block-form body with a span-level bounds check. If `i < 0` or `i >= Length`, `IndexOutOfRangeException` is thrown before any access to the underlying buffer.
- **[§4.14](Bezel_Standard_Library_0_105.md#sec-owned-buffers-and-spans) `ReadOnlySpan<T>`** — class-T and value-T indexers updated the same way (getter only, no setter on the read-only form).
- The "Read access" and "Write access" prose paragraphs are updated to specify the bounds-check semantics and note that the rule is new in v0.105.

<a id="sec-v0-104-to-v0-105-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-6"></a>
### Language spec changes

- **[§28.11](Bezel_Language_Spec_0_105.md#sec-collection-element-ownership)** — the `move coll[i]` admitted-on list's `Span<T>` bullet is clarified: `move span[i]` bounds-checks against `span.Length` before delegating to the underlying buffer's slot move. If the check fails, `IndexOutOfRangeException` is thrown and no slot is moved-from.

No new keywords, no new operators, no new type-system rules.

<a id="sec-v0-104-to-v0-105-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-6"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — a new "Span-level bounds checking (v0.105)" paragraph explains the bug, the fix (with a code example of the new indexer form), and the source-compatibility implications. The "Migration" paragraph at the end of [§5.15](#sec-owned-buffers-and-spans) is updated to cover v0.105.

<a id="sec-v0-104-to-v0-105-changelog-migration"></a>
<a id="sec-migration-7"></a>
### Migration

No syntactic change is needed. Correct programs (those that never accessed outside their declared span range) compile and behave identically. Programs that depended on the bug — reading or writing through `span[i]` where `i >= span.Length` while `Start + i` was still within the underlying buffer — will now throw `IndexOutOfRangeException`. Such programs were almost certainly already buggy at the source-author intent level; the fix surfaces the bug as an exception rather than allowing it to silently corrupt assumptions about span ranges.

<a id="sec-v0-104-to-v0-105-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-8"></a>
### What did not change

- `OwnedBuffer<T>`'s indexer is unchanged. It already bounds-checks against the buffer's `Length` (which is the correct invariant for the underlying storage).
- `ReadOnlyStringSpan` (v0.104) is unchanged — it used the explicit bounds-checked form from the start.
- The borrow-lifetime tracking machinery is unchanged.
- The `Slice`, `AsReadOnly`, `IndexOf`, `Contains`, `SequenceEqual`, `Clear`, `MoveFrom`, `Reverse`, `Fill`, `CopyFrom` methods on spans are unchanged — they were not affected by the bug (they iterate over `0..<Length`, which is within both the span's range and the underlying buffer's range when the span was correctly constructed).
- No FFI changes; no async changes; no class-hierarchy changes.

---

<a id="sec-v0-105-to-v0-106-changelog"></a>
### v0.105 to v0.106 changelog

v0.106 adds `ReadOnlyAsciiByteSpan`, a read-only byte view over the contiguous code points of an ASCII string, plus two new `String` methods for construction (`AsAsciiByteSpan` and `TryAsAsciiByteSpan`). Closes the long-deferred "zero-copy byte view from ASCII strings" gap (mentioned in rationale [§15](#sec-strings) since v0.99). No language changes; no source-breaking changes; v0.105 user code compiles unchanged against v0.106.

<a id="sec-v0-105-to-v0-106-changelog-summary"></a>
<a id="sec-summary-7"></a>
### Summary

ASCII strings are bit-identical to their UTF-8 byte sequence (every code point in `0..127` encodes as one byte in `0..127`). FFI and I/O paths that need UTF-8 bytes from an ASCII string previously had to go through `Encoding.UTF8.GetBytes`, which allocates a fresh `Array<byte>` on each call. v0.106 provides a non-allocating alternative: `ReadOnlyAsciiByteSpan`, a record carrying a borrow of the string and a (Start, Length) range, with construction validated by an ASCII check and the indexer narrowing each char to byte unconditionally (safe by the construction invariant).

The new type is parallel to v0.104's `ReadOnlyStringSpan` — same borrow-of-string mechanism, same separate-type design choice (vs polymorphic source on existing types). The element type and construction semantics differ: `ReadOnlyStringSpan` views chars from any string; `ReadOnlyAsciiByteSpan` views bytes from an ASCII string only.

<a id="sec-v0-105-to-v0-106-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-8"></a>
### Standard library changes

- **[§3.4](Bezel_Standard_Library_0_106.md#sec-readonlyasciibytespan)** new — `ReadOnlyAsciiByteSpan` record. Surface: indexer `[i: int]: byte` (getter only, with span-level bounds check and ASCII-safe narrowing cast), `Length`, `IsEmpty`, `Slice`, `IndexOf(byte)`, `Contains(byte)`, `SequenceEqual(ReadOnlyAsciiByteSpan)`, `SequenceEqual(ReadOnlySpan<byte>)` (cross-source). Carries a borrow of the source string. Construction via `String.AsAsciiByteSpan` / `String.TryAsAsciiByteSpan` is the supported entry point; the direct positional constructor is admitted but performs no ASCII check itself.
- **[§9.6](Bezel_Standard_Library_0_106.md#sec-string) `String`** — two new methods: `AsAsciiByteSpan(): ReadOnlyAsciiByteSpan` (throws `InvalidOperationException` if `not IsAscii`), `TryAsAsciiByteSpan(out value: ReadOnlyAsciiByteSpan): bool` (returns false with default out value if `not IsAscii`). The "zero-copy view" paragraph in [§9.6](Bezel_Language_Spec_0_106.md#sec-records-and-inheritance) is updated to describe both v0.104's `AsSpan()` and v0.106's `AsAsciiByteSpan()` / `TryAsAsciiByteSpan()`, and to redirect the per-call-allocation case to `Encoding.UTF8.GetBytes`.

<a id="sec-v0-105-to-v0-106-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-7"></a>
### Language spec changes

- **[§29.2](Bezel_Language_Spec_0_106.md#sec-desugaring-of-for)** — a paragraph added stating that `for var b in asciiBytes do ... end` over a `ReadOnlyAsciiByteSpan` desugars to a direct indexed loop, parallel to the `ReadOnlyStringSpan` case. The iteration variable's type is `byte` and the loop body sees a copy of each byte.

No new keywords, no new operators, no new type-system rules. The new type uses existing language mechanisms (borrow-bearing records, indexer dispatch, the standard for-loop desugaring pattern), and reuses v0.104's borrow-of-string lifetime story.

<a id="sec-v0-105-to-v0-106-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-7"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — a new "ASCII byte spans (v0.106)" paragraph explains the new type, the design choice (separate type vs `ReadOnlyStringSpan` extension), the per-access-cost dependency on string storage representation, and the deliberate omission of byte-buffer accessors on the new type.
- **[§15](#sec-strings)** — the deferred "zero-copy byte view from ASCII strings" paragraph is updated to note that the byte view landed in v0.106 as `ReadOnlyAsciiByteSpan`; the gap that has been mentioned in this section since v0.99 is now closed.
- **[§18](#sec-things-deferred)** — the "Spans over additional view sources" entry is updated: item (c) (ASCII byte view) is closed; items (a) inline-array spans and (b) heap-`Array<T>` spans remain deferred.

<a id="sec-v0-105-to-v0-106-changelog-migration"></a>
<a id="sec-migration-8"></a>
### Migration

No user code needs to change for v0.106. The v0.105 form is a strict subset: every v0.105 program compiles against v0.106 without modification. Programs that previously used `Encoding.UTF8.GetBytes(s)` for ASCII strings (and tolerated the per-call allocation) can switch to `s.AsAsciiByteSpan()` for zero-allocation byte access when the source is known to be ASCII. The encoding-based form remains valid.

<a id="sec-v0-105-to-v0-106-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-9"></a>
### What did not change

- `ReadOnlySpan<byte>`, `Span<byte>`, and their byte-buffer accessor extensions (v0.103) are unchanged. `ReadOnlyAsciiByteSpan` is a separate, parallel type with its own minimal surface; no byte-buffer accessors are added to it in v0.106. ASCII text views are for I/O and FFI, not for binary protocol parsing.
- `ReadOnlyStringSpan` (v0.104) is unchanged. Its surface and construction APIs are independent of the v0.106 work.
- The bounds-check semantics introduced in v0.105 apply automatically to `ReadOnlyAsciiByteSpan` — its indexer uses the same explicit block-form body with `i < 0 or i >= Length` check.
- The borrow-lifetime tracking machinery is unchanged. `ReadOnlyAsciiByteSpan` carries a borrow of a `string`, handled identically to `ReadOnlyStringSpan`'s borrow.
- No FFI changes; no async changes; no class-hierarchy changes.

---

<a id="sec-v0-106-to-v0-107-changelog"></a>
### v0.106 to v0.107 changelog

v0.107 adds `ArraySpan<T>` and `ReadOnlyArraySpan<T>`, span views over a heap `Array<T>` source, plus eight new methods on `Array<T>` (`AsSpan` × 4 overloads + `AsReadOnlySpan` × 4 overloads). Closes deferred item (b) under "Spans over additional view sources" from [§18](#sec-things-deferred). No language changes; no source-breaking changes; v0.106 user code compiles unchanged against v0.107.

<a id="sec-v0-106-to-v0-107-changelog-summary"></a>
<a id="sec-summary-8"></a>
### Summary

v0.99-v0.106's spans (`Span<T>` / `ReadOnlySpan<T>`) view only into `OwnedBuffer<T>`. v0.104's `ReadOnlyStringSpan` and v0.106's `ReadOnlyAsciiByteSpan` added string-backed forms. v0.107 adds the remaining commonly-wanted source: a heap `Array<T>`. The motivating use case is "give me a sub-range of this array" — passing a slice of an array to a method without copying or allocating.

The mechanical departure from `Span<T>`/`ReadOnlySpan<T>` is the slot model: `Array<T>` slots store borrows (not owned values), so the new types use a **borrowed-slot** model. Three consequences: indexer return type is `T` directly (no `T?` wrapping); setter takes the value with the default parameter convention (no `set(owned)`); `move arrSpan[i]` is *not* admitted for any T. As a side-effect of the simpler ownership story, the bulk-mutation API collapses into a single uniform surface — `Fill` / `CopyFrom` / `Reverse` for all T — without the class/value category split that `Span<T>` requires.

<a id="sec-v0-106-to-v0-107-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-9"></a>
### Standard library changes

- **[§4.4](Bezel_Standard_Library_0_107.md#sec-arrayt) `Array<T>`** — eight new methods. Four `AsSpan` overloads (no-arg, `(start)`, `(start, length)`, `(r: Range<int>)`) returning `ArraySpan<T>`, and four matching `AsReadOnlySpan` overloads returning `ReadOnlyArraySpan<T>`. A prose note added describing the views and pointing to [§4.15](Bezel_Standard_Library_0_107.md#sec-arrayspant-readonlyarrayspant). `AsSpan` returns the writable form by default (matching `Array<T>`'s mutable nature); callers wanting read-only call `AsReadOnlySpan` directly or `.AsReadOnly()` on the writable form.
- **[§4.15](Bezel_Standard_Library_0_107.md#sec-arrayspant-readonlyarrayspant)** new — `ArraySpan<T>` and `ReadOnlyArraySpan<T>` records. Surface for `ArraySpan<T>`: indexer (get/set, span-level bounds check), `Length`, `IsEmpty`, `Slice`, `AsReadOnly`, `IndexOf`, `Contains`, `SequenceEqual` (own type and cross-access-mode), `Fill`, `CopyFrom`, `Reverse`. Surface for `ReadOnlyArraySpan<T>`: indexer (get only), `Length`, `IsEmpty`, `Slice`, `IndexOf`, `Contains`, `SequenceEqual` (own type and cross-access-mode against `ArraySpan<T>`). Both carry a borrow of the source `Array<T>`. Includes a "Caveat" paragraph noting that the span's range is fixed at construction while the underlying `Array<T>`'s `Count` can change; accesses through a span over a since-shrunk array will throw via the array's bounds check.

<a id="sec-v0-106-to-v0-107-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-8"></a>
### Language spec changes

- **[§28.3](Bezel_Language_Spec_0_107.md#sec-local-variables-ownership-and-borrows)** producer table — the `move coll[i]` row clarified to note that `ArraySpan<T>` and `ReadOnlyArraySpan<T>` do not admit `move` for any T (borrowed slots have no ownership to transfer).
- **[§28.11](Bezel_Language_Spec_0_107.md#sec-collection-element-ownership)** — a paragraph added stating that `Array<T>`, `Dictionary<K, V>`, and other borrowing collections do not admit `move coll[i]`, and the same applies to `ArraySpan<T>` and `ReadOnlyArraySpan<T>`.
- **[§29.2](Bezel_Language_Spec_0_107.md#sec-desugaring-of-for)** — a paragraph added stating that `for var x in arrSpan do ... end` over an `ArraySpan<T>` or `ReadOnlyArraySpan<T>` desugars to a direct indexed loop, parallel to the `Span<T>`/`ReadOnlySpan<T>` case. The iteration variable's type is T (no `T?` wrapping); the borrow source is the underlying `Array<T>`.

No new keywords, no new operators, no new type-system rules. The new types use existing language mechanisms (borrow-bearing records, indexer dispatch, the standard for-loop desugaring pattern). The borrowed-slot model uses Bezel's existing borrow-tracking — `Array<T>` slots have always stored borrows, and the new span types delegate to the array's indexer for both reads and writes.

<a id="sec-v0-106-to-v0-107-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-8"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — a new "Array spans (v0.107)" paragraph explains the new types, the borrowed-slot model, why the mutation API simplifies (single uniform surface, no class/value split), and the running cost of the separate-type approach (six span family types now). The "Migration" paragraph at the end of [§5.15](#sec-owned-buffers-and-spans) is updated to cover v0.107.
- **[§18](#sec-things-deferred)** — the "Spans over additional view sources" entry is updated: item (b) (heap `Array<T>` spans) is closed; only item (a) (inline-array spans, needs stack-frame-bounded borrows) remains deferred.

<a id="sec-v0-106-to-v0-107-changelog-migration"></a>
<a id="sec-migration-9"></a>
### Migration

No user code needs to change for v0.107. The v0.106 form is a strict subset: every v0.106 program compiles against v0.107 without modification. Programs that previously passed sub-array ranges via index parameters or by copying the relevant portion can now use `array.AsSpan(start, length)` to pass a non-allocating view. The copying forms remain valid.

<a id="sec-v0-106-to-v0-107-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-10"></a>
### What did not change

- `Span<T>`, `ReadOnlySpan<T>`, `OwnedBuffer<T>` are unchanged. `ArraySpan<T>` and `ReadOnlyArraySpan<T>` are separate, parallel types with their own minimal surface.
- `ReadOnlyStringSpan` (v0.104) and `ReadOnlyAsciiByteSpan` (v0.106) are unchanged.
- The bounds-check semantics introduced in v0.105 apply automatically to `ArraySpan<T>` and `ReadOnlyArraySpan<T>` — their indexers use the same explicit block-form body with `i < 0 or i >= Length` check.
- The borrow-lifetime tracking machinery is unchanged. The new types carry borrows of `Array<T>`, handled identically to other class borrows.
- `Array<T>`'s existing surface (Add, Remove, Insert, Sort, etc.) is unchanged. Only the `AsSpan` / `AsReadOnlySpan` methods are added.
- No new keywords, no new operators, no new type-system rules.
- No FFI changes; no async changes; no class-hierarchy changes.

---

<a id="sec-v0-107-to-v0-108-changelog"></a>
### v0.107 to v0.108 changelog

v0.108 adds `IReadOnlySpan<TElement>`, an interface implemented by all six concrete span types, that unifies the read-only access surface across the span family. Plus one new `SequenceEqual` overload (taking the interface type) on each of the six concrete spans. No language changes; no source-breaking changes; v0.107 user code compiles unchanged against v0.108.

<a id="sec-v0-107-to-v0-108-changelog-summary"></a>
<a id="sec-summary-9"></a>
### Summary

With v0.107, the span family contained six concrete types — `Span<T>`, `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, `ReadOnlyStringSpan`, `ReadOnlyAsciiByteSpan` — none of which shared an interface. Methods that wanted to accept "any read-only span" had to declare per-source overloads, accept one form and require callers to convert, or duplicate their implementation. v0.108 introduces `IReadOnlySpan<TElement>` so the breadth case has a single signature.

The interface uses existing language machinery: records implementing interfaces (language spec [§9.7](Bezel_Language_Spec_0_108.md#sec-records-and-disposal)) and borrow-bearing-record boxing (language spec [§9.7.1](Bezel_Language_Spec_0_108.md#sec-boxing-of-borrow-bearing-records)). The borrow tracker handles the boxed-borrow case via its existing rule; no new lifetime annotations are introduced.

Each concrete span implements the interface with `TElement` matching its indexer's actual return type — `Span<MyClass>` implements `IReadOnlySpan<MyClass?>` (OwnedBuffer slots are nullable), `ArraySpan<MyClass>` implements `IReadOnlySpan<MyClass>` (Array slots track T directly), `ReadOnlyStringSpan` implements `IReadOnlySpan<char>`, etc. The element type is invariant in the interface parameter; authors wanting full nullability breadth provide two overloads.

<a id="sec-v0-107-to-v0-108-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-10"></a>
### Standard library changes

- **[§4.16](Bezel_Standard_Library_0_108.md#sec-ireadonlyspan)** new — `IReadOnlySpan<TElement>` interface. Surface: `Length`, `IsEmpty`, indexer-get, `IndexOf`, `Contains`, `SequenceEqual(other: IReadOnlySpan<TElement>): bool`. `Slice` is deliberately omitted (would propagate interface-typed handles through slicing chains).
- **Span declarations** updated to implement the interface:
  - `Span<T>` for class T → `IReadOnlySpan<T?>` ([§4.14](Bezel_Standard_Library_0_108.md#sec-owned-buffers-and-spans))
  - `Span<T>` for value T → `IReadOnlySpan<T>` ([§4.14](Bezel_Standard_Library_0_108.md#sec-owned-buffers-and-spans))
  - `ReadOnlySpan<T>` for class T → `IReadOnlySpan<T?>` ([§4.14](Bezel_Standard_Library_0_108.md#sec-owned-buffers-and-spans))
  - `ReadOnlySpan<T>` for value T → `IReadOnlySpan<T>` ([§4.14](Bezel_Standard_Library_0_108.md#sec-owned-buffers-and-spans))
  - `ArraySpan<T>` → `IReadOnlySpan<T>` ([§4.15](Bezel_Standard_Library_0_108.md#sec-arrayspant-readonlyarrayspant))
  - `ReadOnlyArraySpan<T>` → `IReadOnlySpan<T>` ([§4.15](Bezel_Standard_Library_0_108.md#sec-arrayspant-readonlyarrayspant))
  - `ReadOnlyStringSpan` → `IReadOnlySpan<char>` ([§3.3](Bezel_Standard_Library_0_108.md#sec-readonlystringspan))
  - `ReadOnlyAsciiByteSpan` → `IReadOnlySpan<byte>` ([§3.4](Bezel_Standard_Library_0_108.md#sec-readonlyasciibytespan))
- Each of the six concrete span types gets one new `SequenceEqual(other: IReadOnlySpan<TElement>): bool` overload, satisfying the interface contract. The existing concrete-typed `SequenceEqual` overloads are unchanged; direct calls between concrete spans continue to dispatch directly without going through the interface.

<a id="sec-v0-107-to-v0-108-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-9"></a>
### Language spec changes

- **[§29.2](Bezel_Language_Spec_0_108.md#sec-desugaring-of-for)** — a paragraph added stating that `for var x in iSpan do ... end` over an `IReadOnlySpan<TElement>` desugars to a direct indexed loop over `0..<Length` using the interface's indexer-get. Access through the interface is virtual dispatch; the recommended pattern for hot loops is to iterate over a concrete span type instead.

No new keywords, no new operators, no new type-system rules. The interface uses existing language mechanisms (record-implements-interface, borrow-bearing-record boxing, virtual dispatch).

<a id="sec-v0-107-to-v0-108-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-9"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — a new "Unifying read-only spans through an interface (v0.108)" paragraph explains the design, the boxing mechanism, the element-type parameterization, the deliberate Slice omission, and the performance cost. The "Migration" paragraph at the end of [§5.15](#sec-owned-buffers-and-spans) is updated to cover v0.108.

<a id="sec-v0-107-to-v0-108-changelog-migration"></a>
<a id="sec-migration-10"></a>
### Migration

No user code needs to change for v0.108. The v0.107 form is a strict subset: every v0.107 program compiles against v0.108 without modification. Methods that previously declared per-source-type overloads can be simplified to a single `IReadOnlySpan<TElement>` parameter if the performance cost of virtual dispatch is acceptable; the original concrete-typed signatures remain valid and continue to be the right choice for performance-sensitive code.

<a id="sec-v0-107-to-v0-108-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-11"></a>
### What did not change

- The concrete span types' surfaces are unchanged except for the one new `SequenceEqual` overload per type. All existing methods on `Span<T>`, `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, `ReadOnlyStringSpan`, and `ReadOnlyAsciiByteSpan` retain their v0.107 signatures and semantics.
- The bounds-check semantics introduced in v0.105 apply transparently when accessing through the interface — the dispatched indexer is the concrete type's bounds-checked indexer.
- No writable interface is introduced. The writable side has substantive semantic differences across span families that prevent a single uniform contract.
- The borrow-lifetime tracking machinery is unchanged. The boxing rule for borrow-bearing records (language spec [§9.7.1](Bezel_Language_Spec_0_108.md#sec-boxing-of-borrow-bearing-records)) already covers interface-typed parameters; no new tracking is needed.
- No FFI changes; no async changes; no class-hierarchy changes.

---

<a id="sec-v0-108-to-v0-109-changelog"></a>
### v0.108 to v0.109 changelog

v0.109 is a pure-ergonomics release: symmetric CopyTo/MoveTo methods on the span family, plus `AsSpan` / `AsReadOnlySpan` factories on `OwnedBuffer<T>`. No language changes; no source-breaking changes; v0.108 user code compiles unchanged against v0.109.

<a id="sec-v0-108-to-v0-109-changelog-summary"></a>
<a id="sec-summary-10"></a>
### Summary

Two complementary additions:

1. **CopyTo/MoveTo symmetry.** Every span type that has a `CopyFrom` or `MoveFrom` method gets the symmetric form: `CopyTo(destination)` or `MoveTo(destination)`. The two forms are operationally equivalent (`source.CopyTo(dest)` ≡ `dest.CopyFrom(source)`); both are provided because the natural call form depends on which side the caller holds.
2. **`OwnedBuffer<T>` AsSpan factories.** Eight new methods (four `AsSpan` overloads returning `Span<T>`; four `AsReadOnlySpan` overloads returning `ReadOnlySpan<T>`). Replaces `new Span<T>(buf, 0, buf.Length)` boilerplate. Completes the three-way symmetry with v0.104's `string.AsSpan()` and v0.107's `Array<T>.AsSpan()`.

<a id="sec-v0-108-to-v0-109-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-11"></a>
### Standard library changes

- **[§3.3](Bezel_Standard_Library_0_109.md#sec-readonlystringspan) `ReadOnlyStringSpan`** — new `CopyTo(destination: Span<char>)` method. Copies chars to a writable char buffer; lengths must match.
- **[§3.4](Bezel_Standard_Library_0_109.md#sec-readonlyasciibytespan) `ReadOnlyAsciiByteSpan`** — new `CopyTo(destination: Span<byte>)` method. Copies the validated-ASCII bytes to a writable byte buffer; lengths must match.
- **[§4.14](Bezel_Standard_Library_0_109.md#sec-owned-buffers-and-spans) `OwnedBuffer<T>`** — eight new methods. `AsSpan()`, `AsSpan(start)`, `AsSpan(start, length)`, `AsSpan(r: Range<int>)` returning `Span<T>`; `AsReadOnlySpan()` × four matching overloads returning `ReadOnlySpan<T>`. Available for both class-T and value-T forms. Replaces explicit `new Span<T>(buf, ...)` constructions; reads more naturally and matches established stdlib patterns.
- **[§4.14](Bezel_Standard_Library_0_109.md#sec-owned-buffers-and-spans) `Span<T>`** — new methods. For class T: `MoveTo(destination: Span<T>)` (symmetric with existing `MoveFrom`). For value T: `CopyTo(destination: Span<T>)` (symmetric with existing `CopyFrom`).
- **[§4.14](Bezel_Standard_Library_0_109.md#sec-owned-buffers-and-spans) `ReadOnlySpan<T>` (value T only)** — new `CopyTo(destination: Span<T>)` method. Source side is read-only; destination side is writable.
- **[§4.15](Bezel_Standard_Library_0_109.md#sec-arrayspant-readonlyarrayspant) `ArraySpan<T>`** — new `CopyTo(destination: ArraySpan<T>)` method (symmetric with existing `CopyFrom`).
- **[§4.15](Bezel_Standard_Library_0_109.md#sec-arrayspant-readonlyarrayspant) `ReadOnlyArraySpan<T>`** — new `CopyTo(destination: ArraySpan<T>)` method.

In total: 15 new methods (7 CopyTo/MoveTo + 8 AsSpan/AsReadOnlySpan overloads). All have direct equivalents already expressible via existing surface; none introduce new semantics.

<a id="sec-v0-108-to-v0-109-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-10"></a>
### Language spec changes

None. v0.109 is a pure-stdlib release.

<a id="sec-v0-108-to-v0-109-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-10"></a>
### Rationale changes

- **[§5.15](#sec-owned-buffers-and-spans)** — a new "Ergonomics pass (v0.109)" paragraph explains the CopyTo/MoveTo additions (with a summary table), the OwnedBuffer factories, and the deliberate omission of `ReadOnlySpan<T>` for class T from CopyTo (read-only borrow-based access incompatible with owned-setter writes). The "Migration" paragraph at the end of [§5.15](#sec-owned-buffers-and-spans) is updated to cover v0.109.

<a id="sec-v0-108-to-v0-109-changelog-migration"></a>
<a id="sec-migration-11"></a>
### Migration

No user code needs to change. The v0.108 form is a strict subset: every v0.108 program compiles against v0.109 without modification. Code that previously wrote `new Span<T>(buf, 0, buf.Length)` can be simplified to `buf.AsSpan()`; code that wrote `dest.CopyFrom(source)` can equivalently be written `source.CopyTo(dest)` if that fits the surrounding control flow better. Neither change is required.

<a id="sec-v0-108-to-v0-109-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-12"></a>
### What did not change

- `OwnedArray<T>.AsSpan()` is not added. The class would need either a new span type with owned-slot semantics or exposure of its internal buffer. Deferred until real demand emerges; users can copy to a fresh `OwnedBuffer<T>` and call `AsSpan` on that.
- `Span<T>` for class T does not get `CopyTo` (only `MoveTo`). For class-T owned slots, copying without ownership transfer is ill-defined (you cannot duplicate ownership). `MoveTo` is the right primitive; users wanting a copy can `Clone` the source or use a different ownership model.
- `ReadOnlySpan<T>` for class T does not get `CopyTo`. Read-only access to class T produces borrows; writing into class-T `Span<T>` requires owned values; the two are incompatible at the slot semantics level.
- No new interfaces (`IMutableSpan` or similar). The reasoning of v0.108 still holds: the writable side has substantive semantic differences across span families.
- All existing surface (methods, properties, indexers) of every span type is unchanged in signature and semantics.
- No FFI changes; no async changes; no class-hierarchy changes; no new keywords, operators, or type-system rules.

---

<a id="sec-v0-109-to-v0-110-changelog"></a>
### v0.109 to v0.110 changelog

v0.110 broadens collection-literal target types and fully specifies the dictionary-literal form (previously documented in a single underspecified sentence). No language changes; no source-breaking changes; v0.109 user code compiles unchanged against v0.110. The broadening is purely additive — literals that did not previously type-check against `LinkedList`/`Stack`/`Queue` targets now do.

<a id="sec-v0-109-to-v0-110-changelog-summary"></a>
<a id="sec-summary-11"></a>
### Summary

Two complementary pieces:

1. **Broadened target set.** `LinkedList<T>`/`OwnedLinkedList<T>`, `Stack<T>`/`OwnedStack<T>`, and `Queue<T>`/`OwnedQueue<T>` are now collection-literal targets, joining the previously-supported inline arrays, `Array`/`OwnedArray`, `Set`/`OwnedSet`, and `Dictionary`/`OwnedDictionary`. The sequence literal `[e1, e2, ...]` targets all sequence-shaped types; the dictionary literal `[k: v, ...]` remains dictionary-only.
2. **Dictionary-literal specification.** The `[k: v, ...]` form, present but underspecified before v0.110, now has a complete specification: element-order maps to insertion order, duplicate-key handling (compile error for all-constant duplicates; last-wins otherwise, with displaced `OwnedDictionary` values disposed), empty-literal behavior, nested-literal examples, and the syntactic distinction from the sequence form.

<a id="sec-v0-109-to-v0-110-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-11"></a>
### Language spec changes

- **[§15.4](Bezel_Language_Spec_0_110.md#sec-collection-literals)** — substantially expanded. The supported-target list now includes the linked-list, stack, and queue families. New prose specifies per-target element-order semantics (notably: stack literals list bottom-to-top, so the last element is on top; queue literals list front-to-back). Worked examples added for sequence literals over the new types, dictionary literals, nested dictionary-of-array and dictionary-of-dictionary, record-typed keys, and owning collections. Empty-literal behavior is specified for every target. A rationale-pointer paragraph explains the colon-pair syntax choice. The grammar (`CollectionLiteral`, `DictEntry`) was already correct and is unchanged.

No new keywords, operators, or type-system rules. The grammar is unchanged. The broadening is a target-typing matter handled by existing contextual-typing machinery.

<a id="sec-v0-109-to-v0-110-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-12"></a>
### Standard library changes

- **[§4.6](Bezel_Standard_Library_0_110.md#sec-linkedlistt) `LinkedList<T>`** — prose note added: sequence-literal construction, head-to-tail order, owning-form behavior.
- **[§4.8](Bezel_Standard_Library_0_110.md#sec-dictionaryk-v) `Dictionary<K, V>`** — prose + example added: dictionary-literal construction, insertion-order observability, duplicate-key rule, empty literal.
- **[§4.9](Bezel_Standard_Library_0_110.md#sec-owneddictionaryk-v) `OwnedDictionary<K, V>`** — prose + example added: owned-value transfer, displaced-value disposal on duplicate, empty literal.
- **[§4.10](Bezel_Standard_Library_0_110.md#sec-sett-and-ownedsett) `Set<T>` / `OwnedSet<T>`** — prose note added: duplicate collapse with first-occurrence retention, owned duplicate disposal, empty set.
- **[§4.11](Bezel_Standard_Library_0_110.md#sec-stackt-ownedstackt-queuet-and-ownedqueuet) `Stack`/`OwnedStack`/`Queue`/`OwnedQueue`** — prose added specifying the stack bottom-to-top and queue front-to-back literal-ordering conventions, with examples.

<a id="sec-v0-109-to-v0-110-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-11"></a>
### Rationale changes

- **[§5.16](#sec-collection-literals-for-complex-collections)** new — explains why the colon-pair dictionary syntax was kept (weighed against arrow, brace, and tuple-list alternatives), why `LinkedList`/`Stack`/`Queue` were added (sequence-shaped; prior omission not deliberate), and the stack-ordering convention choice (push-order so the literal reads as a construction sequence).
- **[§18](#sec-things-deferred)** — the custom-collection-literal-protocol entry updated to reflect the v0.110 built-in target set and to scope precisely what remains deferred (user-defined types).

<a id="sec-v0-109-to-v0-110-changelog-migration"></a>
<a id="sec-migration-12"></a>
### Migration

No user code needs to change. The v0.109 form is a strict subset. Code that previously could not construct a `LinkedList`/`Stack`/`Queue` from a literal (and used explicit `Add`/`Push`/`Enqueue` loops or the `IEnumerable<T>` constructor) can now use the literal; the explicit forms remain valid.

<a id="sec-v0-109-to-v0-110-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-13"></a>
### What did not change

- The collection-literal grammar (`CollectionLiteral`, `DictEntry`) is unchanged — it already admitted both the sequence and dictionary forms.
- The dictionary `[k: v, ...]` syntax is unchanged; v0.110 specifies its semantics, it does not alter them.
- No change to inline-array, `Array`/`OwnedArray`, or `Set`/`OwnedSet` literal behavior beyond added documentation.
- The user-defined-collection-literal protocol remains deferred.
- No FFI changes; no async changes; no class-hierarchy changes; no new keywords, operators, or type-system rules.

---

<a id="sec-v0-110-to-v0-111-changelog"></a>
### v0.110 to v0.111 changelog

v0.111 adds the user-defined sequence-literal protocol: a type opts into collection-literal construction by declaring a `FromCollectionLiteral` static factory. No syntax changes; no source-breaking changes; v0.110 user code compiles unchanged against v0.111. The change is purely additive — types that previously could not be literal targets now can if they declare the factory.

<a id="sec-v0-110-to-v0-111-changelog-summary"></a>
<a id="sec-summary-12"></a>
### Summary

Before v0.111, the set of collection-literal target types was hard-coded in the language spec (inline arrays plus the built-in collections). v0.111 makes the sequence-literal form `[e1, ..., en]` open to any user-defined type that declares a recognized static factory named `FromCollectionLiteral`. Two factory shapes are recognized — a borrowing form taking `ReadOnlySpan<T>` and an owning form taking `owned OwnedArray<T>` — mirroring Bezel's borrowing/owning collection duality. The element type is read directly from the factory's parameter. The dictionary-literal form `[k: v, ...]` remains built-in-only; a user-extensible dictionary protocol stays deferred.

The design followed a comparison with C#'s `[CollectionBuilder]` mechanism. Bezel's named-factory-on-the-type is simpler than C#'s attribute-plus-separate-builder-class (Bezel static methods on generic types can name the type's own parameters, so no builder-type indirection is needed), and avoids the element-type-inference wart that C#'s design documents flag (the element type is the factory parameter's element type, not something the compiler must infer from a secondary iteration type).

<a id="sec-v0-110-to-v0-111-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-12"></a>
### Language spec changes

- **[§15.4](Bezel_Language_Spec_0_111.md#sec-collection-literals)** — the target-types list and the sequence-vs-dictionary prose updated to include user-defined types declaring `FromCollectionLiteral`.
- **[§15.4.1](Bezel_Language_Spec_0_111.md#sec-the-user-defined-sequence-literal-protocol)** new — the user-defined sequence-literal protocol: the two recognized factory signatures (borrowing / owning), declaration requirements (public, static, declared directly on the type, returns `owned C<T>`), lowering for both shapes, empty-literal handling, result ownership, the recognition-category statement, and a worked `RingBuffer<T>` example.
- **[§28.3](Bezel_Language_Spec_0_111.md#sec-local-variables-ownership-and-borrows)** ownership rule made explicit that a literal targeting a user-defined type produces the factory's `owned C<T>` return.

No grammar changes. The collection-literal grammar already admitted the sequence form; the factory is an ordinary static method declaration. No new keywords, operators, or type-system rules — the protocol uses spans, owned arrays, and static methods, all already in the language.

<a id="sec-v0-110-to-v0-111-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-12"></a>
### Rationale changes

- **[§5.16](#sec-collection-literals-for-complex-collections)** — a new subsection-length passage explains the protocol design with the C# `[CollectionBuilder]` comparison: why a named factory rather than an attribute, why the factory goes directly on the generic type (no builder-type indirection), why element-type-from-signature beats C#'s inference wart, the two-shape ownership split, the single-opt-in-path choice (no duck-typed fallback), the new name-based recognition category, the forward-compatibility path to an interface form, and why dictionaries stay deferred (matching C#'s staged path).
- **[§18](#sec-things-deferred)** — the custom-collection-literal entry updated: sequence-literal protocol now delivered; user-extensible dictionary-literal protocol remains the deferred remainder.

<a id="sec-v0-110-to-v0-111-changelog-migration"></a>
<a id="sec-migration-13"></a>
### Migration

No user code changes. The v0.110 form is a strict subset. A user type that previously required explicit construction (`var rb := new RingBuffer<int>(); rb.Push(1); ...`) can now declare `FromCollectionLiteral` and be constructed with `[1, 2, 3]`; the explicit form remains valid. No built-in type's behavior changes.

<a id="sec-v0-110-to-v0-111-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-14"></a>
### What did not change

- The collection-literal grammar is unchanged.
- The dictionary `[k: v, ...]` form is unchanged and remains built-in-only.
- Built-in collection types' literal behavior is unchanged (they remain compiler-intrinsic; the protocol is purely additive for user types and built-ins were not retrofitted onto it).
- No duck-typed fallback opt-in (unlike C#'s secondary path) — the single explicit `FromCollectionLiteral` declaration is the only opt-in.
- No FFI changes; no async changes; no class-hierarchy changes; no new keywords, operators, or type-system rules.

---

<a id="sec-v0-111-to-v0-112-changelog"></a>
### v0.111 to v0.112 changelog

v0.112 is a standard-library-only revision. It closes a long-standing admitted defect in `OwnedDictionary<K, V>` — the absence of a single-call draining iteration — and corrects a misleading note that claimed the language lacked the syntax to support it. No language changes; no source-breaking changes; v0.111 user code compiles unchanged against v0.112.

<a id="sec-v0-111-to-v0-112-changelog-summary"></a>
<a id="sec-summary-13"></a>
### Summary

`OwnedDictionary<K, V>` previously offered no ownership-transferring iteration: draining required a two-step key-collection-then-pop pattern, and a stdlib note asserted that a single-call drain was something "the language does not yet have the syntax to support cleanly." That assertion was false. An owning *class* may own its fields ([§28.5](Bezel_Language_Spec_0_112.md#sec-owned-fields) of the language spec); a class iterated through `OwnedEnumerable<T>` transfers ownership per element. The single-call drain needs no language feature — only a `DrainedEntry<K, V>` owning class and a `Drain()` method. v0.112 adds both and rewrites the note.

The fix also serves as a worked demonstration, in the standard library itself, of the multi-owned-value aggregate pattern: a class that owns several resources, returned/yielded as `owned`, discharged with one `using var`. This is the pattern that makes tuples and class deconstruction unnecessary for the owned-multi-value case (rationale [§5.17](#sec-why-no-class-deconstruction), [§18](#sec-things-deferred)).

<a id="sec-v0-111-to-v0-112-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-13"></a>
### Standard library changes

- **[§4.9](Bezel_Standard_Library_0_112.md#sec-owneddictionaryk-v) `OwnedDictionary<K, V>`** — new `Drain(): OwnedEnumerable<DrainedEntry<K, V>>` method; iteration drains the dictionary, each yielded entry owning its value. New `DrainedEntry<K, V>` class (`where V: class`): a constructor taking `(key: K, owned value: V)`, a `Key` readonly property, and an `owned Value: V` field. It is a *class* (not a record) precisely so it may own `Value` per [§28.5](Bezel_Language_Spec_0_112.md#sec-owned-fields). The misleading "the language does not yet have the syntax" paragraph is replaced with an accurate description and a worked `Drain()` example alongside the existing key-iteration example.

<a id="sec-v0-111-to-v0-112-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-13"></a>
### Language spec changes

None. v0.112 is a pure-stdlib revision.

<a id="sec-v0-111-to-v0-112-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-13"></a>
### Rationale changes

- **[§5.17](#sec-why-no-class-deconstruction)** new — records the full analysis behind declining class deconstruction (and, by extension, treating tuples as unnecessary for the owned-aggregate case): the naive "destructuring disposes the class" form is unsound (disposal is the field teardown; extraction and disposal are opposite operations on the same slots); the sound `Deconstruct`-with-explicit-`move` form is implementable but adds a rule to the cost-sensitive ownership system for sugar on a solved problem; the conservative resolution keeps the capability (owning classes) and declines the sugar.
- **[§18](#sec-things-deferred)** — the tuples entry downgraded from "foundational gap entangled with owned-record-fields" to "ergonomic convenience, not a capability gap," with a pointer to the owning-class pattern and to [§5.17](#sec-why-no-class-deconstruction).

<a id="sec-v0-111-to-v0-112-changelog-migration"></a>
<a id="sec-migration-14"></a>
### Migration

No user code changes. Existing two-step key-iteration drains continue to work unchanged; `Drain()` is an additional, more concise option. The `DrainedEntry<K, V>` name is newly introduced in `System.Collections.Generic`; the only conflict risk is user code that already declared a type of that exact name in a conflicting import scope, resolved by the standard import-aliasing mechanism.

<a id="sec-v0-111-to-v0-112-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-15"></a>
### What did not change

- No language changes whatsoever.
- The borrowing `Dictionary<K, V>` and its `DictionaryEntry<K, V>` record are unchanged.
- The two-step key-iteration drain pattern remains valid and documented.
- No FFI, async, or class-hierarchy changes; no new keywords, operators, or type-system rules.

---

<a id="sec-v0-112-to-v0-113-changelog"></a>
### v0.112 to v0.113 changelog

v0.113 fills in the `Type` token's surface with type-lattice metadata, records the deliberate boundary between type identity and member-access reflection, and reclassifies the reflection deferred-item accordingly. The only language-spec change is documentation: the inline `Type` sketch in spec [§17.10](Bezel_Language_Spec_0_113.md#sec-the-typeof-operator) is expanded and pointed at the new canonical stdlib definition. No new keywords, operators, or type-system rules; v0.112 user code compiles unchanged against v0.113.

<a id="sec-v0-112-to-v0-113-changelog-summary"></a>
<a id="sec-summary-14"></a>
### Summary

`Type` was previously described only by an inline two-member sketch in spec [§17.10](Bezel_Language_Spec_0_113.md#sec-the-typeof-operator) (`Name`, `FullName`) and was **not defined in the standard library at all** — a latent documentation defect independent of any reflection question. v0.113 (1) gives `Type` a canonical stdlib definition, (2) adds the ownership-neutral lattice-metadata surface (kind predicates, `BaseType`, `Implements`/`IsAssignableFrom`, enum introspection), and (3) writes down the principled boundary: Bezel provides type *identity and lattice metadata*, deliberately not member-access reflection. Member-access reflection is reclassified from "deferred" to **declined-with-reasons**; generic-instantiation decomposition is split out as its own explicitly-scoped deferred question (it requires an unbound-generic `typeof`, a language change).

<a id="sec-v0-112-to-v0-113-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-14"></a>
### Standard library changes

- **[§2.13](Bezel_Standard_Library_0_113.md#sec-the-type-class)** new — the canonical `Type` definition. Members: `Name`, `FullName`; kind predicates `IsClass`/`IsInterface`/`IsRecord`/`IsEnum`/`IsPrimitive`/`IsInlineArray`/`IsFunctionType` (exactly one true for every nameable type, with `string` and heap arrays flattened to `IsClass` and the unit type the documented exception) plus the derived `IsValueType`; lattice queries `BaseType`, `Implements`, `IsAssignableFrom`; enum metadata `EnumNames`/`EnumValues` (raising `InvalidOperationException` on non-enum types). Worked enum-introspection example. Explicit statement of what `Type` does not provide and why.

<a id="sec-v0-112-to-v0-113-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-14"></a>
### Language spec changes

- **[§17.10](Bezel_Language_Spec_0_113.md#sec-the-typeof-operator)** — the inline `Type` sketch expanded from two members to the commonly-used subset, with a pointer to the canonical stdlib [§2.13](Bezel_Standard_Library_0_113.md#sec-the-type-class) and a one-paragraph statement of the identity-not-reflection boundary. No semantic change; `typeof` behavior is unchanged.

<a id="sec-v0-112-to-v0-113-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-14"></a>
### Rationale changes

- **[§5.18](#sec-type-identity-not-reflection)** new — records why the boundary sits at lattice metadata: member-access reflection contradicts the ownership model (no static site carries a reflectively-accessed `owned` member's contract) and is the maximal no-hidden-magic violation; lattice metadata costs nothing because the compiler and runtime already carry it for `is`/`as`/`GetType`; generic decomposition is excluded from even the metadata surface because it needs an unbound-`typeof` language change; attributes are the realistic separate future pressure valve. Mirrors the [§5.17](#sec-why-no-class-deconstruction) disposition: deliver what fits, decline what fights the foundations with the reasoning on record, keep adjacent open questions explicitly scoped.
- **[§18](#sec-things-deferred)** — the "reflection beyond `typeof`/`GetType`" entry split three ways: lattice metadata *delivered*; member-access reflection *declined* (pointer to [§5.18](#sec-type-identity-not-reflection)); generic-instantiation decomposition *deferred* as its own scoped question coupled to unbound-`typeof`.

<a id="sec-v0-112-to-v0-113-changelog-migration"></a>
<a id="sec-migration-15"></a>
### Migration

No user code changes. `typeof(T)` and `GetType()` behave exactly as before; the `Type` they return now has more readable members. Code that previously maintained hand-built type→metadata side tables (because `Type` exposed only `Name`/`FullName`) can use the new members but is not required to. The `Type` name was already in scope (auto-imported `System`); no new name is introduced.

<a id="sec-v0-112-to-v0-113-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-16"></a>
### What did not change

- `typeof` / `GetType` / `is` / `as` semantics are unchanged.
- No member-access reflection was added (deliberately — see [§5.18](#sec-type-identity-not-reflection)).
- No generic-instantiation decomposition (deferred, needs unbound-`typeof`).
- No attribute system (separately deferred).
- No FFI, async, or class-hierarchy changes; no new keywords, operators, or type-system rules.

---

<a id="sec-v0-113-to-v0-114-changelog"></a>
### v0.113 to v0.114 changelog

v0.114 establishes the serialization vocabulary: the format-abstraction interfaces and the type-side capability interfaces. It is almost entirely a standard-library and rationale revision; the only language-spec change is adding the four new interfaces to the auto-imported `System` list. No new keywords, operators, or type-system rules; v0.113 user code compiles unchanged against v0.114.

<a id="sec-v0-113-to-v0-114-changelog-summary"></a>
<a id="sec-summary-15"></a>
### Summary

[§5.18](#sec-type-identity-not-reflection) closed reflective serialization. v0.114 supplies the reflection-free replacement's foundation. The architectural keystone — separating *type shape* (the type's concern) from *format encoding* (the format's concern), the invariant every reflection-free language converged on — is fixed now, in the standard library, at zero language cost. The writing direction is usable by hand immediately; the reading direction reuses the v0.111 `FromCollectionLiteral` name-based static-factory recognition so it is not gated on the deferred static-interface-members keystone. Compiler-synthesized conformance, concrete formats, and customization are deferred with their reasoning recorded, modeled explicitly on the existing auto-equality synthesis rather than on an open-ended source-generator subsystem.

<a id="sec-v0-113-to-v0-114-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-15"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_114.md#sec-serialization-vocabulary)** new — four auto-imported interfaces. `ISerializer`/`IDeserializer` are the format abstraction (agent nouns, like `IEqualityComparer<T>`): primitive and structural read/write tokens, naming no format. `ISerializable`/`IDeserializable` are the type-side capabilities (the `-able` family, like `IDisposable`): parameterless, format-independent. `ISerializable.Serialize` is an ordinary instance method usable by hand today. `IDeserializable` is a marker in this version (a static construction entry point cannot be an interface method until static interface members exist); the deserialization entry point is a recognized static `Deserialize(d: IDeserializer): T` factory (plain by-value for the record/enum common case; `owned T` only for class targets), the `FromCollectionLiteral` mechanism ([§15.4.1](Bezel_Language_Spec_0_114.md#sec-the-user-defined-sequence-literal-protocol)) reused, promotable to a real interface method without source change when static interface members land. Worked hand-written `Point` examples for both directions.

<a id="sec-v0-113-to-v0-114-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-15"></a>
### Language spec changes

- **[§2.3](Bezel_Language_Spec_0_114.md#sec-always-available-names)** auto-imported-`System` list extended with `ISerializer`, `IDeserializer`, `ISerializable`, `IDeserializable`. No semantic change; no grammar change.

<a id="sec-v0-113-to-v0-114-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-15"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** new — records the design and staged plan: every reflection-free language (serde, Codable, even C#'s modern source-generator path) converged on the shape/format split; Bezel already does opt-in compile-time codegen (auto-equality), so serialization synthesis is that generalized, not a new kind of feature, and follows the Swift dedicated-compiler-feature model rather than the C# open-ended-source-generator model; the naming follows the established capability-adjective vs agent-noun convention; the instance/static asymmetry is why `IDeserializable` is a marker reusing the `FromCollectionLiteral` recognition; the synthesis design, its structural-eligibility rule, and the generalization ceiling (customization, reference/cyclic/polymorphic graphs) are recorded. Same disposition as [§5.17](#sec-why-no-class-deconstruction)/[§5.18](#sec-type-identity-not-reflection): ship what fits now, record the staged plan, keep open questions scoped.
- **[§18](#sec-things-deferred)** — new entry splitting deferred serialization work three ways (synthesis / concrete formats / customization + hard graphs), each with its reason and an explicit note that synthesis is not gated on the macro system.

<a id="sec-v0-113-to-v0-114-changelog-migration"></a>
<a id="sec-migration-16"></a>
### Migration

No user code changes. The four interface names are newly introduced in auto-imported `System`; the only conflict risk is user code that already declared a conflicting top-level name, resolved by the standard import-aliasing mechanism. Types wanting serialization today implement `ISerializable.Serialize` (and optionally a recognized static `Deserialize`) by hand; this code remains valid unchanged when synthesis is later added (synthesis only removes the obligation to write the bodies; it does not change their shape).

<a id="sec-v0-113-to-v0-114-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-17"></a>
### What did not change

- No language feature was added — the vocabulary is pure stdlib; the spec change is one auto-import list entry.
- No compiler synthesis yet (deferred, with the design recorded in [§5.19](#sec-serialization-without-reflection)).
- No concrete `JsonSerializer`/`BinarySerializer` yet (deferred, ordinary stdlib work).
- No static-interface-members dependency was introduced (the `FromCollectionLiteral` recognition was reused precisely to avoid it).
- No FFI, async, or class-hierarchy changes; no new keywords, operators, or type-system rules.

---

<a id="sec-v0-114-to-v0-115-changelog"></a>
### v0.114 to v0.115 changelog

v0.115 delivers the concrete serialization formats: `JsonSerializer`/`JsonDeserializer` and a compact `BinarySerializer`/`BinaryDeserializer` pair, in a new `System.Serialization` module. This is pure standard-library work — the v0.114 vocabulary was deliberately designed so the formats are ordinary classes implementing the existing agent interfaces. No language-spec change of any kind; v0.114 user code compiles unchanged against v0.115.

<a id="sec-v0-114-to-v0-115-changelog-summary"></a>
<a id="sec-summary-16"></a>
### Summary

v0.114 shipped the serialization *vocabulary* (the four interfaces) but nothing concrete to serialize *to*; a user could hand-write `Serialize`/`Deserialize` but had no format. v0.115 closes that: the by-hand serialization path is now end-to-end usable. The formats live in `System.Serialization` and are *not* auto-imported — naming a concrete format is an explicit choice, while the format-agnostic `ISerializable` conformance never forces the import. This is "Tier A" of the staged serialization plan (rationale [§5.19](#sec-serialization-without-reflection)); compiler-synthesized conformance ("Tier B," the per-type boilerplate eliminator) and customization remain deferred.

<a id="sec-v0-114-to-v0-115-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-16"></a>
### Standard library changes

- **[§2.15](Bezel_Standard_Library_0_115.md#sec-concrete-serialization-formats)** new — `JsonSerializer`/`JsonDeserializer`, `BinarySerializer`/`BinaryDeserializer`, the `JsonOptions` record, and `SerializationException`, all in `System.Serialization`. JSON maps the [§2.14](Bezel_Standard_Library_0_115.md#sec-serialization-vocabulary) token stream to/from JSON text (advisory counts, `-1` on read since JSON is not length-prefixed, non-finite floats rejected); the binary format is length-prefixed, same-version-only, exact for non-finite floats via `BitConverter`. All four classes are `IDisposable`; serializers expose `ToBytes(): owned OwnedBuffer<byte>`. Worked `SavePoint`/`LoadPoint` round-trip example. Errors raise `SerializationException` with byte-offset/line-column location.
- **[§11](Bezel_Standard_Library_0_115.md#sec-modules-deferred-to-future-revisions)** — the deferred-modules `System.Text.Json` entry clarified to distinguish a future general JSON DOM/streaming API from the now-provided serialization JSON *format agent* in `System.Serialization`; they are complementary, not the same thing.

<a id="sec-v0-114-to-v0-115-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-16"></a>
### Language spec changes

None. v0.115 is a pure standard-library revision.

<a id="sec-v0-114-to-v0-115-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-16"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — the "generalization ceiling" updated from three deferred items to two: concrete formats are no longer pending (delivered in v0.115); synthesis and customization/hard-graphs remain.
- **[§18](#sec-things-deferred)** — the serialization entry updated to record formats as delivered and restate the remaining deferrals (synthesis, customization) with the staging rationale (vocabulary → formats → synthesis, each an independent non-breaking addition).

<a id="sec-v0-114-to-v0-115-changelog-migration"></a>
<a id="sec-migration-17"></a>
### Migration

No user code changes. Code that hand-wrote `ISerializable.Serialize` against the v0.114 vocabulary now has concrete formats to pass: `using var s := new JsonSerializer(); value.Serialize(s); ... s.ToBytes()`. The new names live in `System.Serialization` (explicit import); the only conflict risk is user code already declaring a conflicting name in a scope that imports the module, resolved by standard import aliasing.

<a id="sec-v0-114-to-v0-115-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-18"></a>
### What did not change

- No language feature, no spec change — the formats are ordinary classes implementing the v0.114 interfaces.
- The vocabulary interfaces (`ISerializer`/`IDeserializer`/`ISerializable`/`IDeserializable`) are unchanged and remain auto-imported.
- No compiler synthesis yet (Tier B, deferred, design recorded in [§5.19](#sec-serialization-without-reflection)).
- No FFI, async, or class-hierarchy changes; no new keywords, operators, or type-system rules.

---

<a id="sec-v0-115-to-v0-116-changelog"></a>
### v0.115 to v0.116 changelog

v0.116 delivers compiler-synthesized serialization conformance — "Tier B" of the staged serialization plan, and the one piece that required genuine language-spec writing. A record or enum that declares `: ISerializable` and/or `: IDeserializable` and is synthesis-eligible now has the member bodies generated by the compiler, eliminating the per-type hand-written boilerplate. The design is modeled directly on the existing auto-equality synthesis; it adds no grammar and no new keyword. v0.115 user code compiles unchanged against v0.116 (hand-written `Serialize`/`Deserialize` suppress synthesis, exactly as a manual `Equals` suppresses auto-equality, so existing hand-written conformances keep working).

<a id="sec-v0-115-to-v0-116-changelog-summary"></a>
<a id="sec-summary-17"></a>
### Summary

v0.114 shipped the vocabulary, v0.115 the concrete formats; both left the user hand-writing `Serialize`/`Deserialize` per type. v0.116 closes that: declaring the conformance on an eligible record or enum is now sufficient. The eligibility rule is recursive and structural — a type is eligible when every serialized component is itself serializable — and the compile-error discipline mirrors auto-equality's, reported at the conformance declaration (the explicit opt-in site). Classes remain outside the eligible set (their reference/identity/owned-field complications are the deferred hard tier); they participate by hand-writing the members. This is the language-spec-heavy stage; with it, serialization is "good support" rather than merely "usable."

<a id="sec-v0-115-to-v0-116-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-17"></a>
### Language spec changes

- **[§9.8](Bezel_Language_Spec_0_116.md#sec-serialization-synthesis)** new — *Serialization synthesis*. Defines the synthesis-eligibility rule (records, enums, recursive component check; classes excluded), the synthesized `Serialize` body shape, the synthesized recognized-static-`Deserialize` body shape and value-type return, missing/duplicate/unknown-field error behavior, suppression by hand-writing either direction, and composition with auto-equality. The wire shapes are stated normatively (enum-name-not-integer, declaration order, key/value-pair dictionaries) so independent implementations agree. No grammar change — `: ISerializable, IDeserializable` is ordinary interface-conformance syntax; the section adds only the eligibility rule and generated-body semantics.

<a id="sec-v0-115-to-v0-116-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-17"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_116.md#sec-serialization-vocabulary)** — the "what is deferred" paragraph rewritten: synthesis is delivered (with the one-line `Point` example showing the body-less declaration); the suppression rule and the remaining deferrals (field-name customization, class graphs) stated; the three-stage staging recap (vocabulary → formats → synthesis) recorded.

<a id="sec-v0-115-to-v0-116-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-17"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — the "synthesis design" paragraph moved from *recorded, not yet implemented* to *delivered in v0.116*, with the normative wire-choice rationale (enum names, declaration order, key/value-pair dictionaries) and the soundness argument (static construction site, unlike reflection) retained and tied to language spec [§9.8](Bezel_Language_Spec_0_116.md#sec-serialization-synthesis). The "generalization ceiling" updated from two open items to one (customization; class graphs), noting the planned staging held exactly.
- **[§18](#sec-things-deferred)** — the serialization entry rewritten to record all three stages as delivered and restate the two remaining deferrals (field-name/tolerant-mode customization; reference/cyclic/polymorphic class graphs) with their reasons.

<a id="sec-v0-115-to-v0-116-changelog-migration"></a>
<a id="sec-migration-18"></a>
### Migration

No user code changes. A type that hand-wrote `ISerializable.Serialize` (and/or a recognized static `Deserialize`) against v0.114/v0.115 keeps compiling unchanged: a present hand-written body suppresses synthesis of that direction, so the existing contract is preserved. A type that previously *could not* be serialized without writing the boilerplate can now declare `: ISerializable, IDeserializable` with no body if it is synthesis-eligible. No name, grammar, or import change.

<a id="sec-v0-115-to-v0-116-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-19"></a>
### What did not change

- No grammar, no new keyword, no new operator — synthesis is keyed off ordinary interface-conformance syntax, exactly as auto-equality is keyed off record structure.
- The vocabulary interfaces and concrete formats are unchanged.
- Hand-written conformances are unchanged in meaning and still take precedence (suppression rule).
- Classes are still not synthesis-eligible (deferred hard tier); no change to how they participate.
- No FFI, async, or class-hierarchy changes; no new type-system rules beyond the [§9.8](Bezel_Language_Spec_0_116.md#sec-serialization-synthesis) eligibility rule, which is structural and parallels the existing [§9.2](#sec-why-records-are-sealed) auto-equality rule.

---

<a id="sec-v0-116-to-v0-117-changelog"></a>
### v0.116 to v0.117 changelog

v0.117 delivers field-name customization for serialization synthesis: a compiler-recognized `serialized` clause on positional fields, auto-properties, and enum constants. This is the first serialization stage that adds grammar — one closed production — and the design is deliberately *not* an attribute system and deliberately improves on Swift's `CodingKeys`. v0.116 user code compiles unchanged against v0.117 (the clause is purely additive; a member with no clause behaves exactly as before, and `serialized` is a contextual keyword so existing identifiers named `serialized` are unaffected).

<a id="sec-v0-116-to-v0-117-changelog-summary"></a>
<a id="sec-summary-18"></a>
### Summary

v0.116 made `: ISerializable, IDeserializable` synthesize bodies but always under the members' declared names. v0.117 adds a closed `serialized` clause with three effects — rename (`serialized "<name>"`), exclude (`serialized skip`), tolerate (`serialized optional`, omit-if-null on write / default-if-missing on read), with rename and tolerate combinable (`serialized "<name>" optional`) and exclude mutually exclusive with both — attached to the member it affects. The design was chosen over both an attribute system (which would pay the entire deferred-attribute cost — proliferation, stringly-typed magic, semantics/metadata blur — to buy one narrow feature) and Swift's `CodingKeys` (whose exhaustive separate list silently drops members added without a corresponding case). The clause is per-member and total: a member with no clause is coded under its own name, always, so the silent-drop failure is structurally impossible.

<a id="sec-v0-116-to-v0-117-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-18"></a>
### Language spec changes

- **[Appendix A](Bezel_Language_Spec_0_117.md#sec-appendix-a)** — new `SerializedClause` production; `PositionalField`, the auto-property `PropertyBody` alternative, and `EnumConstant` each gain an optional `SerializedClause`. Grammar notes state the closed effect vocabulary (rename/exclude/tolerate, rename+tolerate combinable, exclude exclusive), the contextual-word status of `skip`/`optional`, and that `skip`/`optional` are not admitted on enum constants.
- **[§9.8](Bezel_Language_Spec_0_117.md#sec-serialization-synthesis)** — new "Field-name customization" subsection defining the three forms' semantics, the uniqueness/default/eligibility-interaction rules, the compile errors (collision, no-default `skip`, clause on a non-synthesizing or hand-written type), and the explicit framing as a compiler-recognized modifier rather than an attribute system. The prior "Synthesis adds no grammar" claim is corrected to "one closed grammar production."
- **[Appendix B](Bezel_Language_Spec_0_117.md#sec-appendix-b)** — `serialized` added to the contextual-keyword list ([§B.1](Bezel_Language_Spec_0_117.md#sec-reserved-keywords)) and the [§B.2](Bezel_Language_Spec_0_117.md#sec-contextual-keywords) table; `skip`/`optional` noted as contextual within the clause. None is unconditionally reserved, so existing identifiers are unaffected.

<a id="sec-v0-116-to-v0-117-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-18"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_117.md#sec-serialization-vocabulary)** — the synthesis paragraph updated with a worked `User` example showing all three clause forms; the deferred list reduced to one item (class graphs); the staging recap extended to four stages.

<a id="sec-v0-116-to-v0-117-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-18"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — new paragraph recording the design: why attributes were rejected here (imports the entire [§18](#sec-things-deferred)-deferred cost for one feature), why `CodingKeys` was rejected (silent-drop footgun, semantics/metadata blur relocated into an enum), and how the chosen modifier improves on it (per-member, total, local, closed, forward-compatible to attributes if those ever arrive). The "generalization ceiling" reduced from one open item to the class-graph tier plus a noted small adjacent question (whole-type unknown-field tolerance).
- **[§18](#sec-things-deferred)** — the serialization entry updated: four stages delivered, only reference/cyclic/polymorphic class graphs remain, whole-type unknown-field tolerance noted as a small adjacent question.

<a id="sec-v0-116-to-v0-117-changelog-migration"></a>
<a id="sec-migration-19"></a>
### Migration

No user code changes. The `serialized` clause is optional and additive; records and enums without it synthesize exactly as in v0.116. `serialized` is a contextual keyword (Appendix B), so any existing identifier named `serialized` continues to work outside the clause position. A hand-written `Serialize`/`Deserialize` still suppresses synthesis; a `serialized` clause on a member of a suppressed-synthesis type is a compile error (the clause would be inert), which surfaces rather than silently ignores a now-meaningless annotation.

<a id="sec-v0-116-to-v0-117-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-20"></a>
### What did not change

- The vocabulary interfaces, concrete formats, and synthesis eligibility rule are unchanged; v0.117 only adds the customization clause as an input to synthesis.
- No unconditionally reserved word added; no existing identifier can break.
- Classes are still not synthesis-eligible (deferred hard tier).
- No FFI, async, or class-hierarchy changes; no new type-system rules — the clause is a syntactic input to the existing [§9.8](Bezel_Language_Spec_0_117.md#sec-serialization-synthesis) synthesis, not a new semantic subsystem.

---

<a id="sec-v0-117-to-v0-118-changelog"></a>
### v0.117 to v0.118 changelog

v0.118 makes unknown-field tolerance configurable as a property of the deserialization *operation*, not an annotation on the type. It is almost entirely a standard-library revision plus one spec contract clarification; there is no grammar change and no new keyword. v0.117 user code compiles unchanged against v0.118, and behavior is unchanged unless the new option is explicitly set (strict remains the default).

<a id="sec-v0-117-to-v0-118-changelog-summary"></a>
<a id="sec-summary-19"></a>
### Summary

The strict-by-default unknown-field behavior (an input field matching no declared component raises `SerializationException`, established with synthesis in v0.116) is correct as a default but was the one remaining serialization policy that could not be varied. v0.118 makes it configurable. The design question — type-level annotation vs operation property — was resolved in favor of the operation: unknown-field tolerance is not part of a type's wire contract (the type is legitimately read strictly in one context and leniently in another), so it belongs to the caller performing the read, not to the type. A type-level `serialized tolerant` clause was considered and rejected for this reason; it would have been less expressive than a per-operation control and would have reintroduced the very "you cannot tell from the call site what it does" objection the v0.117 design was careful to avoid.

<a id="sec-v0-117-to-v0-118-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-19"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_118.md#sec-serialization-vocabulary)** — `IDeserializer` gains `SkipValue()`: consume and discard the current value (including nested object/array structure). The synthesized `Deserialize` uses it for unrecognized fields; strict deserializers raise before it is reached, tolerant ones skip.
- **[§2.15](Bezel_Standard_Library_0_118.md#sec-concrete-serialization-formats)** — `JsonOptions` gains `IgnoreUnknownFields: bool` (default `false`); `BinaryDeserializer` gains an `(input, ignoreUnknownFields: bool)` constructor overload. New "Unknown-field policy" paragraph specifying the strict default, the operation-scoped configuration, the `SkipValue` mechanism, and orthogonality with the per-member `serialized optional`.

<a id="sec-v0-117-to-v0-118-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-19"></a>
### Language spec changes

- **[§9.8](Bezel_Language_Spec_0_118.md#sec-serialization-synthesis)** — the synthesized-`Deserialize` contract revised: an unrecognized field is delegated to `IDeserializer.SkipValue` and whether reaching it is an error is the *deserializer's* configured policy, not baked into the synthesized code. The prior text ("an unknown field name … raises `SerializationException`") becomes "raises by default; skipped when the deserializer is configured to ignore unknown fields." Missing/duplicate/type-mismatch remain unconditional errors. This is a clarification and correctness improvement to the synthesis contract, not a new language feature; no grammar change. The v0.117 `serialized optional` description updated to note its orthogonality with the (now-delivered, operation-scoped) unknown-field policy.

<a id="sec-v0-117-to-v0-118-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-19"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — new paragraph recording why unknown-field tolerance is an operation policy not a type annotation (the intrinsic-to-type vs property-of-the-read distinction), the rejected type-level `serialized tolerant`, and the general principle for future serialization-policy placement decisions. The "generalization ceiling" updated: the "small adjacent open question" is resolved; only the class-graph tier remains.
- **[§18](#sec-things-deferred)** — serialization entry updated to five delivered stages; no small adjacent questions remain open; only reference/cyclic/polymorphic class graphs remain deferred.

<a id="sec-v0-117-to-v0-118-changelog-migration"></a>
<a id="sec-migration-20"></a>
### Migration

No user code changes and no behavior change by default. Deserialization remains strict unless `JsonOptions.IgnoreUnknownFields` is set to `true` or the `BinaryDeserializer` tolerant constructor is used. Existing hand-written `IDeserializer` implementations gain one method to implement, `SkipValue`; standard-library deserializers implement it as part of this revision. Code using only the standard formats is unaffected.

<a id="sec-v0-117-to-v0-118-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-21"></a>
### What did not change

- No grammar, no new keyword, no spec syntax change — the only spec edit is the synthesized-`Deserialize` contract wording.
- Strict-by-default is unchanged; tolerance is opt-in, now at the operation.
- The v0.117 `serialized` clause and all prior serialization stages are unchanged.
- Classes are still not synthesis-eligible (the one remaining deferred tier).
- No FFI, async, or class-hierarchy changes; no new type-system rules.

---

<a id="sec-v0-118-to-v0-119-changelog"></a>
### v0.118 to v0.119 changelog

v0.119 extends serialization synthesis to **tree-shaped classes** via an explicit `serialized tree` class-header clause. It is the first stage of the previously-deferred class-graph tier — specifically the sub-problem that was bundled with the hard ones but was never actually blocked. v0.118 user code compiles unchanged against v0.119; the feature is purely additive (a class with no `serialized tree` clause behaves exactly as before — outside synthesis, hand-written only).

<a id="sec-v0-118-to-v0-119-changelog-summary"></a>
<a id="sec-summary-20"></a>
### Summary

The class-graph tier decomposes into sub-problems of very different difficulty. A class whose serialized object graph is a *tree* (no aliasing among serialized edges, no cycles) is, for synthesis, a record plus heap allocation and ownership transfer: the synthesized `Deserialize` constructs each owned child and transfers it through the constructor, where the ownership contract already lives at a static program point — exactly the soundness argument [§9.8](Bezel_Language_Spec_0_119.md#sec-serialization-synthesis) made for records. The one real design question — tree-shape is not statically verifiable — is resolved by *assert-and-enforce*: the programmer asserts tree-shape with `serialized tree`, and the synthesized deserializer enforces it dynamically (a repeated object identity raises `SerializationException` rather than silently duplicating, refusing the documented footgun of reference-unaware serializers). `serialized tree` is type-scoped, consistent with the v0.118 placement principle (tree-shape is an invariant of the type's wire contract, not a property of a particular read).

<a id="sec-v0-118-to-v0-119-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-20"></a>
### Language spec changes

- **[Appendix A](Bezel_Language_Spec_0_119.md#sec-appendix-a)** — `ClassDecl` gains `SerializedClassClause?` (after `PrimaryCtorParams?`, before `Inheritance?`); `SerializedClassClause ::= "serialized" "tree"`. `tree` is a contextual word, not reserved.
- **[§9.8](Bezel_Language_Spec_0_119.md#sec-serialization-synthesis)** — component-eligibility extended to admit serialization-eligible tree-shaped classes. New "Tree-shaped class synthesis" subsection: the `serialized tree` opt-in and its assertion; serialized components are the owned/value fields and auto-properties in declaration order; synthesized `Deserialize` returns `owned T` and transfers owned children through the constructor; dynamic tree-shape enforcement; type-scoped per the rationale [§5.19](#sec-serialization-without-reflection) placement principle; explicit non-coverage of shared/cyclic/polymorphic graphs. The prior blanket "classes are not serialization-eligible" paragraph is replaced.
- **[Appendix B](Bezel_Language_Spec_0_119.md#sec-appendix-b)** — the `serialized` contextual-keyword entry updated to note the class-header `serialized tree` clause and `tree` as a contextual sub-word.

<a id="sec-v0-118-to-v0-119-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-20"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_119.md#sec-serialization-vocabulary)** — the synthesis paragraph updated with a worked `serialized tree` class example (`Order` with `owned Customer` and `owned OwnedArray<owned LineItem>`), the dynamic-enforcement note, and the narrowed remaining deferral (shared/cyclic/polymorphic). Staging recap extended to six stages.

<a id="sec-v0-118-to-v0-119-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-20"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — new paragraph: why tree-shaped class synthesis was never actually blocked (the decomposition argument), the assert-and-enforce design choice (vs serde's infer-and-hope and an impossible static proof), and why `serialized tree` is type-scoped under the v0.118 principle. The "generalization ceiling" rewritten: trees delivered; the residue is shared/cyclic/polymorphic (really one problem); the crux is the deferred-borrow-binding-vs-definite-assignment tension, which gates everything past trees and must be worked out in the rationale before any spec text.
- **[§18](#sec-things-deferred)** — serialization entry updated to six delivered stages; the remaining deferral narrowed to the shared/cyclic/polymorphic residue with its crux stated.

<a id="sec-v0-118-to-v0-119-changelog-migration"></a>
<a id="sec-migration-21"></a>
### Migration

No user code changes. `serialized tree` is opt-in and additive; classes without it are unaffected (still outside synthesis, hand-written only). `serialized`/`tree` are contextual, so existing identifiers are unaffected. A class that hand-writes `Serialize`/`Deserialize` is unaffected and its hand-written forms still take precedence (suppression is unchanged).

<a id="sec-v0-118-to-v0-119-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-22"></a>
### What did not change

- Records and enums synthesize exactly as in v0.118; the eligibility rule for value-typed components is unchanged.
- Classes without `serialized tree` remain hand-written-only.
- Shared-reference, cyclic, and polymorphic class graphs remain deferred (now the sole remaining serialization work), gated on the deferred-binding question.
- No new unconditionally reserved word; no FFI, async, or class-hierarchy semantic changes; the only new type-system content is the `serialized tree` eligibility extension, structural and parallel to the existing record rule.

---

<a id="sec-v0-119-to-v0-120-changelog"></a>
### v0.119 to v0.120 changelog

v0.120 is a **rationale-only** revision. It adds no language feature, no standard-library API, and no normative spec text. Its single purpose is to work out, reasoning-first, the deferred-borrow-binding vs definite-assignment tension that gates all serialization work past tree-shaped class synthesis (v0.119) — the same reasoning-before-spec discipline every hard question in this design has followed. v0.119 user code is entirely unaffected; there is nothing to migrate.

<a id="sec-v0-119-to-v0-120-changelog-summary"></a>
<a id="sec-summary-21"></a>
### Summary

The remaining serialization tier (shared-reference, cyclic, polymorphic class graphs) was deferred through v0.119 with its crux named but not resolved: reconstructing a borrowing back-edge requires binding a non-nullable reference field after its owning target is constructed, colliding with [§7.9](Bezel_Language_Spec_0_120.md#sec-definite-assignment-for-non-nullable-fields). v0.120 determines whether that tension has a sound resolution at all, so that the eventual outcome is either a specifiable design or a *reasoned* decline — not an indefinite open question.

The working-out concludes: the shared-reference **DAG** case has a credible resolution — a bounded two-phase construction (allocate-and-own, then bind-backs) whose intermediate state is *statically guaranteed unobservable*, with the owning-tree/back-edge partition taken for free from the `owned`/borrow annotations the language already requires (the information serde must guess and gets wrong). The three obvious escapes (relax [§7.9](Bezel_Language_Spec_0_120.md#sec-definite-assignment-for-non-nullable-fields) for synthesized code; topologically order construction; make borrow fields nullable) are each examined and rejected with reasons. The **truly cyclic** case remains the single honest blocker, but its obstruction is now named precisely — discharging an unobservability-under-constructor-side-effects proof — rather than left vague. Polymorphism is confirmed orthogonal and independently designable as explicit registration.

<a id="sec-v0-119-to-v0-120-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-21"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — new subsection "Working out the deferred-borrow-binding vs definite-assignment tension," placed after the generalization-ceiling paragraph. States the tension exactly against [§7.9](Bezel_Language_Spec_0_120.md#sec-definite-assignment-for-non-nullable-fields); rejects the three obvious escapes with reasons; identifies the surviving candidate (bounded two-phase construction with provably-unobservable intermediate state); reduces the whole problem to one checkable property (unobservability of the intermediate state) and names the residual hard part (unobservability under constructor side effects); concludes that the DAG case is gate-cleared for future spec work and the cyclic case is held with a precise stated obstruction.
- **[§18](#sec-things-deferred)** — the serialization deferred-entry updated: the tension is now worked out (v0.120, rationale only); the DAG case is understood well enough to specify in a future revision; the cyclic case's exact obstruction is named; polymorphism noted as independently designable.

<a id="sec-v0-119-to-v0-120-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-23"></a>
### What did not change

- No spec text, no grammar, no standard-library API, no keyword. v0.120 is purely the rationale working-out.
- No serialization feature shipped; the shared-reference/cyclic/polymorphic tier remains deferred. v0.120 changes the *quality* of the deferral (from "crux unresolved" to "DAG case resolved in principle, cyclic case blocked on a named proof obligation"), not its delivered surface.
- Tree-shaped class synthesis (v0.119) and all prior stages are unchanged.
- No FFI, async, type-system, or class-hierarchy changes of any kind.

---

<a id="sec-v0-120-to-v0-121-changelog"></a>
### v0.120 to v0.121 changelog

v0.121 specifies **shared-reference DAG serialization** — the case the v0.120 rationale-only working-out cleared. A class whose serialized object graph aliases (the same object reachable by more than one serialized path) but does not cycle declares `serialized graph` on its header and is synthesized. v0.120 user code compiles unchanged against v0.121; the feature is purely additive (a new `SerializedClassClause` alternative; classes with neither clause are unaffected).

<a id="sec-v0-120-to-v0-121-changelog-summary"></a>
<a id="sec-summary-22"></a>
### Summary

The v0.120 working-out named two ways to make the two-phase intermediate state unobservable; v0.121 takes the *easier* one, available precisely because the graph is acyclic: a directed acyclic graph admits an owner-before-borrower construction order, so every borrowing back-edge's target is already constructed when the borrower's constructor runs, and the back-edge is passed **as a constructor argument**. There is consequently no mutable post-construction phase and no intermediate state at all — each object is fully constructed, back-edges included, by its single constructor call. [§7.9](Bezel_Language_Spec_0_121.md#sec-definite-assignment-for-non-nullable-fields) is satisfied *unmodified*: no nullable relaxation, no synthesized-code exception, which was the working-out's non-negotiable requirement. The harder two-phase discharge (with the unobservability-under-side-effects proof) is exactly what the still-deferred *cyclic* case needs; v0.121 ships only the sub-problem whose discharge requires no new proof.

<a id="sec-v0-120-to-v0-121-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-21"></a>
### Language spec changes

- **[Appendix A](Bezel_Language_Spec_0_121.md#sec-appendix-a)** — `SerializedClassClause ::= "serialized" ( "tree" | "graph" )`; `graph` added as a contextual word. Grammar note states the two assertions and their dynamic enforcement.
- **[§9.8](Bezel_Language_Spec_0_121.md#sec-serialization-synthesis)** — the class-synthesis subsection retitled and extended: serialized components now include borrowing back-edge fields; the owning-tree/back-edge partition is read from the `owned`/borrow annotations; synthesized `Serialize` emits identity tags and references; synthesized `Deserialize` reconstructs owner-before-borrower with back-edges bound through the constructor (the discharged proof); dynamic enforcement distinguishes `tree` (raise on any repeat) from `graph` (bind repeats, raise on cycle). The "what is not covered" set narrowed to cyclic + polymorphic. Component-eligibility extended to admit `serialized graph` classes.
- **[Appendix B](Bezel_Language_Spec_0_121.md#sec-appendix-b)** — the `serialized` contextual-keyword entry updated to include `serialized graph` and `graph` as a contextual sub-word.

<a id="sec-v0-120-to-v0-121-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-21"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_121.md#sec-serialization-vocabulary)** — a worked `serialized graph` example (`Invoice` with two `owned Company` edges and a borrowing `AccountManager` back-edge); the constructor-argument back-edge binding and the [§7.9](Bezel_Language_Spec_0_121.md#sec-definite-assignment-for-non-nullable-fields)-holds-unmodified consequence stated; dynamic cycle-detection described; staging recap extended to seven stages.

<a id="sec-v0-120-to-v0-121-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-22"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — new paragraph recording *which* of the working-out's two discharges the spec took and why (the acyclic case enables constructor-parameter injection, the easier discharge with no intermediate state and so no unobservability proof obligation), and that the harder discharge is exactly the deferred cyclic case's requirement. The generalization-ceiling paragraph rewritten: tree and DAG done; cyclic is the one open problem with its single named proof obligation; polymorphism is independent and ungated.
- **[§18](#sec-things-deferred)** — serialization entry updated to seven delivered stages; remaining deferral narrowed to cyclic (with its precise obstruction) and polymorphic (independent, ungated).

<a id="sec-v0-120-to-v0-121-changelog-migration"></a>
<a id="sec-migration-22"></a>
### Migration

No user code changes. `serialized graph` is opt-in and additive; classes without a graph-shape clause are unaffected (still hand-written-only), `serialized tree` classes are unchanged. `serialized`/`tree`/`graph` are contextual, so existing identifiers are unaffected. Hand-written `Serialize`/`Deserialize` still suppresses synthesis and takes precedence.

<a id="sec-v0-120-to-v0-121-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-24"></a>
### What did not change

- `serialized tree` semantics are unchanged; `serialized graph` is a strictly weaker assertion added alongside it.
- [§7.9](Bezel_Language_Spec_0_121.md#sec-definite-assignment-for-non-nullable-fields) is untouched — the whole point of taking the constructor-injection discharge is that definite assignment needs no modification.
- Cyclic and polymorphic class graphs remain deferred (now the only remaining serialization work), the cyclic case with its exact proof obligation stated.
- No new unconditionally reserved word; no FFI, async, or class-hierarchy semantic changes; the only new type-system content is the `serialized graph` eligibility extension, structural and parallel to `serialized tree`.

---

<a id="sec-v0-121-to-v0-122-changelog"></a>
### v0.121 to v0.122 changelog

v0.122 specifies **polymorphic dispatch** for class serialization synthesis: a base class declares a closed `polymorphic` registry mapping wire discriminator strings to permitted concrete derived types. v0.121 user code compiles unchanged against v0.122; the feature is purely additive (an optional `PolymorphicClause` on the existing graph-shape clause; classes without it are unaffected).

<a id="sec-v0-121-to-v0-122-changelog-summary"></a>
<a id="sec-summary-23"></a>
### Summary

The polymorphism design was orthogonal and ungated, deferred only because it could wait. v0.122 specifies it as explicit, closed registration — no reflective subtype discovery — exactly the [§5.18](#sec-type-identity-not-reflection) explicit-registration-never-reflection principle. This is the model the mainstream (System.Text.Json, .NET 7+) reached only after the reflective-`$type` approach became a remote-code-execution vector; Bezel lands there by principle rather than by incident. The discriminator is consumed *before* component binding (structural, never a data field, never subject to field-name customization or the unknown-field policy — settled by the documented .NET sharp edge where a discriminator treated as an ordinary required property breaks). Polymorphism composes with `serialized tree`/`serialized graph` (registry selects the concrete type; the graph-shape clause governs that type's edge reconstruction) and is independent of the cyclic deferral.

<a id="sec-v0-121-to-v0-122-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-22"></a>
### Language spec changes

- **[Appendix A](Bezel_Language_Spec_0_122.md#sec-appendix-a)** — `SerializedClassClause` gains an optional `PolymorphicClause`; `PolymorphicClause ::= "polymorphic" "(" PolyCase ("," PolyCase)* ")"`, `PolyCase ::= StringLiteral ":" TypeName`. `polymorphic` added as a contextual word. Grammar note states closed explicit registration, no reflective discovery, the eligibility constraints on registered types.
- **[§9.8](Bezel_Language_Spec_0_122.md#sec-serialization-synthesis)** — new "Polymorphic dispatch" subsection: the registry's closed semantics; discriminator written first / read first and consumed by dispatch; unregistered discriminator (read) or run-time type (write) raises `SerializationException`; abstract vs concrete base; composition with graph-shape clauses; independence from the cyclic deferral; type-scoped justification. The "what these clauses do not cover" paragraph narrowed: polymorphism removed from the deferred set, only cyclic remains; a base-typed component without a registry is still a synthesis-eligibility compile error.
- **[Appendix B](Bezel_Language_Spec_0_122.md#sec-appendix-b)** — the `serialized` contextual-keyword entry updated to include the `polymorphic` registry and `polymorphic` as a contextual sub-word.

<a id="sec-v0-121-to-v0-122-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-22"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_122.md#sec-serialization-vocabulary)** — a worked `Shape`/`Circle`/`Square` polymorphic example; the explicit-registration/no-reflection rationale and the security context stated; discriminator-first read/write described; staging recap extended to eight stages; the deferred set narrowed to cyclic only.

<a id="sec-v0-121-to-v0-122-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-23"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — new paragraph recording the polymorphism design: closed explicit registration vindicated by the ecosystem's own trajectory (reflective `$type` → RCE → mainstream move to closed opt-in), the discriminator-before-binding decision and its external evidence, type-scoped justification, composition with graph shape, independence from the cyclic obligation. The generalization-ceiling paragraph rewritten: exactly one sub-problem (truly cyclic) remains, with its single named proof obligation.
- **[§18](#sec-things-deferred)** — serialization entry updated to eight delivered stages; remaining deferral narrowed to cyclic alone, with its precise obstruction and external corroboration.

<a id="sec-v0-121-to-v0-122-changelog-migration"></a>
<a id="sec-migration-23"></a>
### Migration

No user code changes. The `polymorphic` registry is opt-in and additive; classes without it are unaffected, `serialized tree`/`serialized graph` semantics are unchanged. `serialized`/`tree`/`graph`/`polymorphic` are contextual, so existing identifiers are unaffected. Hand-written `Serialize`/`Deserialize` still suppresses synthesis and takes precedence.

<a id="sec-v0-121-to-v0-122-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-25"></a>
### What did not change

- `serialized tree` and `serialized graph` semantics are unchanged; `PolymorphicClause` is an optional addition that composes with them.
- The discriminator is structural, not a data field — field-name customization, `serialized optional`, and the unknown-field policy are untouched and do not apply to it.
- Cyclic class graphs remain the sole deferred serialization piece, with the exact proof obligation stated and externally corroborated.
- No new unconditionally reserved word; no FFI, async, or class-hierarchy semantic changes; the only new type-system content is the `polymorphic` registry's eligibility/assignability checks, structural and parallel to the existing graph-shape rules.

---

<a id="sec-v0-122-to-v0-123-changelog"></a>
### v0.122 to v0.123 changelog

v0.123 is a **rationale-only** revision. It adds no language feature, no standard-library API, and no normative spec text. Its single purpose is to discharge — reasoning-first — the unobservability-under-constructor-side-effects proof obligation that the v0.120 working-out named as the one outstanding blocker for cyclic class synthesis. v0.122 user code is entirely unaffected; there is nothing to migrate. This mirrors v0.120 exactly: the hard question is resolved in the rationale before any syntax, and the subsequent revision may then specify the feature.

<a id="sec-v0-122-to-v0-123-changelog-summary"></a>
<a id="sec-summary-24"></a>
### Summary

The cyclic case was forced onto the v0.120 working-out's path (a) — a statically-checkable "no escape during construction" constructor restriction — because path (b), binding back-edges before any constructor body runs, is impossible for a cycle. v0.123 determines that path (a) is dischargeable: a class is *cyclic-serialization-eligible* only if its constructor is *self-contained* (assigns its own fields; reads no back-edge field; calls no instance method on `this`; never lets `this` escape; calls no virtual/abstract method). Under this rule the only code running in the two-phase construction window is self-contained constructors, none of which can observe a not-yet-bound back-edge, so the intermediate state is provably unobservable and [§7.9](Bezel_Language_Spec_0_123.md#sec-definite-assignment-for-non-nullable-fields) holds unmodified. The discharge is honestly bounded: classes whose construction logic is reference-entangled are *excluded* (hand-write `Deserialize`) by a recorded principled boundary, not solved. This is a genuine resolution — the proof is carried for a precisely delimited, statically-checkable class of programs; the remainder is a declared exclusion rather than an unresolved tension.

<a id="sec-v0-122-to-v0-123-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-24"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — new subsection "Working out the cyclic case," after the generalization-ceiling paragraph. Reduces the obligation to a single checkable property, defines the self-contained-constructor rule, proves sufficiency, states the cost honestly, identifies the residual excluded sub-case, and concludes the proof is dischargeable without weakening definite assignment. The generalization-ceiling paragraph rewritten: no undischarged obligation remains; cyclic synthesis is now in the same gate-cleared position the DAG case held between v0.120 and v0.121.
- **[§18](#sec-things-deferred)** — the serialization entry updated: the cyclic proof obligation is discharged (v0.123, rationale only); what remains is ordinary spec-design work plus a stated eligibility restriction, not an open problem.

<a id="sec-v0-122-to-v0-123-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-26"></a>
### What did not change

- No spec text, no grammar, no standard-library API, no keyword. v0.123 is purely the rationale working-out, exactly as v0.120 was.
- No serialization feature shipped; cyclic synthesis is not yet specified. v0.123 changes the *quality* of the deferral (from "undischarged proof obligation" to "proof discharged; ordinary spec work and a stated eligibility restriction remain"), not the delivered surface.
- Polymorphic dispatch (v0.122) and all prior stages are unchanged.
- No FFI, async, type-system, or class-hierarchy changes of any kind.

---

<a id="sec-v0-123-to-v0-124-changelog"></a>
### v0.123 to v0.124 changelog

v0.124 specifies **cyclic class synthesis**, completing the serialization stack. It makes normative exactly what the v0.123 rationale-only working-out discharged: the `serialized cyclic` clause, the statically-checked self-contained-constructor eligibility rule, and the two-phase construction whose intermediate state is provably unobservable. v0.123 user code compiles unchanged against v0.124; the feature is purely additive (a third graph-shape alternative; classes without it are unaffected).

<a id="sec-v0-123-to-v0-124-changelog-summary"></a>
<a id="sec-summary-25"></a>
### Summary

`serialized cyclic` permits a serialized graph containing cycles. It deserializes by constructing every object in the group first (back-edges unbound), then binding all back-edges through the identity/`$ref` table — the two-phase construction. Soundness rests entirely on the **self-contained-construction rule**: a participating class's construction — its constructor body, every constructor reached transitively via `this(...)`/`base(...)`, and every field/property initializer that runs during construction — may assign its own fields but must not read a back-edge, call an instance method on `this`, let `this` escape, or call a virtual method. This is a strict subset of the existing borrow-checker escape analysis; a class that violates it is a *compile error*, never a runtime fault, and hand-writes `Deserialize`. [§7.9](Bezel_Language_Spec_0_124.md#sec-definite-assignment-for-non-nullable-fields) is preserved unmodified — a non-nullable back-edge is never observed unset because the only code running between the two phases is self-contained construction that provably cannot read one. This is the v0.123 proof made normative; the serialization stack is now complete.

<a id="sec-v0-123-to-v0-124-changelog-language-spec-changes"></a>
<a id="sec-language-spec-changes-23"></a>
### Language spec changes

- **[Appendix A](Bezel_Language_Spec_0_124.md#sec-appendix-a)** — `SerializedClassClause` gains a third alternative: `"serialized" ( "tree" | "graph" | "cyclic" )`. `cyclic` added as a contextual word; grammar note states the two-phase construction and the self-contained-constructor compile-time requirement.
- **[§9.8](Bezel_Language_Spec_0_124.md#sec-serialization-synthesis)** — the "what these clauses do not cover" paragraph replaced by "the graph-shape clauses are exhaustive as of v0.124." New "Cyclic class synthesis" subsection: the `$ref` extension for forward references; the self-contained-constructor eligibility rule (four prohibitions) as a compile error at the conformance declaration; the two-phase construction; the [§7.9](Bezel_Language_Spec_0_124.md#sec-definite-assignment-for-non-nullable-fields)-preserved argument; type-scoped justification; composition with the `polymorphic` registry. Component-eligibility extended to admit `serialized cyclic` classes.
- **[Appendix B](Bezel_Language_Spec_0_124.md#sec-appendix-b)** — the `serialized` contextual-keyword entry updated to include `serialized cyclic` and `cyclic` as a contextual sub-word.

<a id="sec-v0-123-to-v0-124-changelog-standard-library-changes"></a>
<a id="sec-standard-library-changes-23"></a>
### Standard library changes

- **[§2.14](Bezel_Standard_Library_0_124.md#sec-serialization-vocabulary)** — a worked `serialized cyclic` example (`Node` with an owned forward edge and a cycle-closing `Parent` back-edge); the two-phase construction, self-contained-constructor rule, and [§7.9](Bezel_Language_Spec_0_124.md#sec-definite-assignment-for-non-nullable-fields)-preserved consequence stated; the stale "independent of the cyclic deferral" phrase in the polymorphism paragraph corrected; staging recap extended to the complete nine-stage stack; the "serialization stack complete" statement added.

<a id="sec-v0-123-to-v0-124-changelog-rationale-changes"></a>
<a id="sec-rationale-changes-25"></a>
### Rationale changes

- **[§5.19](#sec-serialization-without-reflection)** — the working-out's conclusion annotated with the v0.124 delivery (the discharge made normative). The generalization-ceiling paragraph rewritten as "the generalization ceiling, reached": the serialization stack is complete; the sole remaining hand-write reason (non-self-contained constructor) is a recorded, statically-detected, principled exclusion, not an open problem.
- **[§18](#sec-things-deferred)** — the serialization entry retitled "complete (object-graph tier fully delivered)"; cyclic synthesis delivered in v0.124; the stack described as nine feature stages with both hard questions resolved reasoning-first before their spec text.

<a id="sec-v0-123-to-v0-124-changelog-migration"></a>
<a id="sec-migration-24"></a>
### Migration

No user code changes. `serialized cyclic` is opt-in and additive; classes without it are unaffected, `serialized tree`/`serialized graph`/`polymorphic` semantics are unchanged. `serialized`/`tree`/`graph`/`cyclic`/`polymorphic` are contextual, so existing identifiers are unaffected. Hand-written `Serialize`/`Deserialize` still suppresses synthesis and takes precedence.

<a id="sec-v0-123-to-v0-124-changelog-what-did-not-change"></a>
<a id="sec-what-did-not-change-27"></a>
### What did not change

- `serialized tree`, `serialized graph`, and the `polymorphic` registry are unchanged; `serialized cyclic` is an additional alternative composing with them.
- [§7.9](Bezel_Language_Spec_0_124.md#sec-definite-assignment-for-non-nullable-fields) is untouched — the two-phase construction is engineered specifically so definite assignment needs no modification, the same non-negotiable the DAG case honored.
- No new unconditionally reserved word; no FFI, async, or class-hierarchy semantic changes; the only new type-system content is the `serialized cyclic` eligibility/self-contained-constructor check, structural and a strict subset of the existing escape analysis.

---

### v0.124 to v0.125 changelog

v0.125 is a clarifying revision: it states a rule the language has always intended and never spelled out. No grammar changes, no semantic changes, no migration. The opportunity was identified by reading the optional-`end`-label productions in Appendix A alongside the one-sentence statement in [§1.6](Bezel_Language_Spec_0_125.md#sec-statements-and-termination) and noticing that the rule "the label, when supplied, must match" was nowhere stated normatively — a reader implementing the parser from Appendix A alone would not know to enforce it.

<a id="sec-v0-124-to-v0-125-changelog-summary"></a>
### Summary

[§1.6](Bezel_Language_Spec_0_125.md#sec-statements-and-termination) is expanded from one sentence to a complete normative paragraph categorizing the three forms of optional end-label (identifier for named constructs, construct keyword for keyword-labelled constructs, no label for plain-end constructs) and stating the match-when-supplied rule. The match rule is added to Appendix C.1 as a compiler-enforced invariant. No grammar production changes — the grammar already admits the labelled form everywhere it was intended.

<a id="sec-v0-124-to-v0-125-changelog-language-spec-changes"></a>
### Language spec changes

- **[§1.6](Bezel_Language_Spec_0_125.md#sec-statements-and-termination)** — expanded from one sentence to a full normative paragraph. Three categories articulated: *named constructs* (class, interface, record, enum bodies; method bodies; property accessor blocks) admit an optional identifier label that must match the construct's declared name; *keyword-labelled constructs* (constructors, destructors, operator overloads) admit an optional construct-keyword label (with operator token where applicable) that must match the construct kind; *plain-end constructs* (statement-level blocks, extension and extern blocks, arm/lambda `do…end`) admit no label. The unlabelled form `end;` is always valid.
- **[Appendix C.1](Bezel_Language_Spec_0_125.md#sec-appendix-c-type-system)** — new invariant: a supplied end-label must match the construct it closes; a mismatch is a compile error.

### Standard library changes

None.

### Rationale changes

None beyond this changelog entry.

### Migration

No user code changes. The grammar is unchanged; the language has always admitted the labelled form for the categorized constructs, and the matching rule has always been the obvious one. Existing programs that omit labels remain valid; existing programs that supply matching labels remain valid; existing programs that supplied *mismatching* labels were always working only by accident of an underspecified rule and would have been rejected by any reasonable parser — v0.125 makes that rejection normative.

<a id="sec-v0-124-to-v0-125-changelog-what-did-not-change"></a>
### What did not change

- Appendix A grammar productions are byte-identical to v0.124. The optionality of the label in each production was already correctly expressed; the missing piece was prose, not grammar.
- The semantics of every block construct are unchanged.
- The set of constructs that admit labels is unchanged. Adding labels to constructs that currently take none (e.g., `end while;`, `end if;`, `end try;`) was considered and declined: those constructs have no user-chosen name and no construct-keyword distinction worth labelling, the indentation already disambiguates them, and adding the option would expand the grammar surface without adding informational content.

### Editorial note

The `linkify_refs.py` toolchain was not re-run for this revision. The substantive edits (one expanded paragraph and one bullet, both in the spec) introduce a small number of cross-references that were verified manually against existing anchors. Future revisions should run the linkifier as the per-revision workflow specifies; any anchor drift this revision missed will be caught by the next run.

---


### v0.125 to v0.126 changelog

v0.126 adds a purely-additive ergonomic convenience: arm bodies in statement-position `case` and in `catch` arms may now be a single bare `Statement`, in addition to the existing `Expression` and `do … end` forms. The change removes the friction of writing `=> do _x := y; end` for a one-line assignment-shaped arm body; the new spelling is `=> _x := y;`.

<a id="sec-v0-125-to-v0-126-changelog-summary"></a>
### Summary

`ArmBody` (the right-hand side of `=>` in a `case` or `catch` arm) gains a third grammatical alternative: a single `Statement`. The new form is restricted by the same rule that already restricts the `do … end` arm-body form — valid only in statement-position `case` or in a `catch` arm, never in expression-position `case`. Existing programs continue to compile unchanged. The change does *not* introduce block expressions (rationale [§5.13](Bezel_Design_Rationale.md#sec-no-block-expressions) stands and was re-examined during the v0.126 cycle).

<a id="sec-v0-125-to-v0-126-changelog-language-spec-changes"></a>
### Language spec changes

- **[Appendix A](Bezel_Language_Spec.md#sec-appendix-a)** — `ArmBody` gains a `Statement` alternative. The existing `do … end` alternative's grammar comment is corrected to note that it applies to catch arms as well as statement-position case arms (the prose in §31.2.2 already said so; the grammar comment had missed it).
- **[§19.2](Bezel_Language_Spec.md#sec-arms)** — the "an arm has the form `pattern => expression`" introductory paragraph is rewritten to enumerate the three arm-body shapes and the position rule for each. The helper-extraction example and the §5.13 cross-reference are unchanged.
- **[§31.2.2](Bezel_Language_Spec.md#sec-arm-bodies)** — the bulleted list of catch arm-body forms gains the single-statement entry; the existing entries are kept.

### Standard library changes

None.

### Rationale changes

- **[§5.13](Bezel_Design_Rationale.md#sec-no-block-expressions)** — addendum at the end of the section explaining that the v0.126 arm-body addition is *not* a block expression, that §5.13's decision against value-returning blocks stands and was re-examined during this cycle with the same conclusion, and that the change addresses a different question (single-statement ergonomics in arm bodies) which §5.13 did not consider.

### Migration

No source-level migration. The change is purely additive. Programs using the v0.125 `=> do stmt; end` form continue to compile and behave identically; the new `=> stmt;` form is a shorter spelling of the same construct. Style guides may prefer the shorter form for single-statement arms; this is taste, not specification.

The accompanying guides (Bezel Guide 2: Errors and Nullability) are updated to use the new bare-statement form where the previous `=> do … end` for single-statement arm bodies appeared, since the new form is the idiomatic one going forward.

### What did not change

- The semantics of arm bodies, including disposal, control flow, and exception propagation, are unchanged. The new form is a syntactic shortcut for the `do … end` form with one statement inside.
- The position rule is unchanged: arm bodies that have type `()` (the new `Statement` form and the existing `do … end` form) remain valid only in statement-position `case` and `catch`; expression-position `case` still requires an Expression arm body.
- `do … end` retains its single meaning of statement-sequencing block of type `()`. The v0.126 change adds a sibling form in arm-body position; it does not give `do … end` a new interpretation.
- The four declined block-expression designs ([§5.13](Bezel_Design_Rationale.md#sec-no-block-expressions)) remain declined for the same reasons. The helper-method extraction is still the idiom for multi-statement expression-position arms.
- Statements and expressions remain distinct categories. Assignment is still a statement; `if-then-else` block is still a statement; `value if cond else value` is still an expression. The v0.126 change does not blur these.

### Editorial note

The `linkify_refs.py` toolchain was again not re-run for this revision, for the same reason noted in the v0.125 changelog. The substantive edits are small (one grammar alternative, two prose paragraphs, one rationale addendum, one changelog entry) and the new cross-references were verified manually against existing anchors. Future revisions should re-run the linkifier once it is recovered.

---


### v0.126 to v0.127 changelog

v0.127 resolves a long-standing inconsistency between the §18.1 prose ("digit separators allowed except at start/end") and the Appendix A grammar productions for numeric literals, which permitted a trailing underscore. The grammar now matches the prose: a numeric literal's digit-run cannot end with `_`.

<a id="sec-v0-126-to-v0-127-changelog-summary"></a>
### Summary

In Appendix A, the four productions for numeric literal digit-runs — `DecLiteral`, `HexLiteral`, `BinLiteral`, and `DigitGroup` — are tightened from the form `Digit (Digit | "_")*` to `Digit ("_"* Digit)*`. The literal's digit-run must end with a digit; underscores still appear freely between digits, including consecutively. The §18.1 prose is unchanged — it already stated the rule that this revision now enforces in the grammar.

The discrepancy was noticed during implementation of the lexer's numeric-literal scanner: the prose called the trailing-`_` form an error, but the grammar admitted it as a valid literal. The project discipline is that an apparent contradiction is resolved in the spec rather than silently in the implementation; this revision is that resolution.

<a id="sec-v0-126-to-v0-127-changelog-language-spec-changes"></a>
### Language spec changes

- **[Appendix A](Bezel_Language_Spec.md#sec-lexical)** — productions `DecLiteral`, `HexLiteral`, `BinLiteral`, and `DigitGroup` are rewritten so that the digit-run ends with a digit (no trailing `_`). Underscores between digits — including consecutive underscores — are unchanged.

### Standard library changes

None.

### Rationale changes

None.

### Migration

The set of valid literal lexemes shrinks: forms ending with `_` (e.g. `42_`, `0xFF_`, `0b1_`, `1_.0`) are no longer accepted by the grammar. Programs that contained such literals were already non-conforming under the v0.126 §18.1 prose; no program that conformed to v0.126 is broken by this change.

### What did not change

- The §18.1 prose is unchanged; it already stated the rule.
- Underscores between digits remain unrestricted, including consecutive underscores (e.g. `1__2` is still legal).
- The leading-`_` restriction (a numeric literal cannot begin with `_`) was already enforced by the grammar and remains so; an identifier-shaped token starting with `_` is still an identifier, not a literal.
- All numeric suffix forms (`L`/`U`/`UL`/`LU`/`f`/`F`/`m`/`M`) are unchanged.
- The float and exponent productions outside the digit-run forms are unchanged.

### Editorial note

The `linkify_refs.py` toolchain was again not re-run for this revision. The substantive edits are small (four grammar productions in one appendix, one changelog entry), and the inter-document filename references were updated mechanically with the version bump. The new cross-references were verified manually against existing anchors.

---

### v0.127 to v0.128 changelog

v0.128 specifies how overload resolution chooses among applicable candidates when an argument matches more than one overload through different built-in implicit conversions (§5.1). v0.127's §22.6 named the factors that make overloads distinguishable but did not state the tie-breaking rule for this case; the gap is now closed.

<a id="sec-v0-127-to-v0-128-changelog-summary"></a>
### Summary

§22.6 gains a new subsection, "Implicit conversions and tie-breaking" (anchor `sec-implicit-conversion-ranking`), specifying that an exact match is preferred over any implicit conversion, that built-in implicit conversions are not ranked against one another, and that a call with no unambiguously-better candidate is an ambiguous compile error citing the candidates. The rule applies after arity and named-argument binding (§22.5) and to constructor overload sets (§22.7). A corresponding compiler-enforced invariant is added to Appendix C.1.

This is the conservative resolution of an under-specification, chosen to match §27.6 (overloaded method references) and §25.4 (no user-defined conversions); the alternative of a finer C#-style "better conversion target" ordering was considered and declined because §5.1 treats all admitted implicit conversions as co-equal widenings.

<a id="sec-v0-127-to-v0-128-changelog-language-spec-changes"></a>
### Language spec changes

- **[§22.6](Bezel_Language_Spec.md#sec-implicit-conversion-ranking)** — new subsection "Implicit conversions and tie-breaking": exact match preferred over conversion; built-in implicit conversions co-equal; no unambiguously-better candidate ⇒ ambiguous compile error citing candidates; applies after §22.5 arity/named-argument filtering and to constructor overload sets.
- **[Appendix C.1](Bezel_Language_Spec.md#sec-appendix-c-type-system)** — new invariant restating the overload tie-breaking rule.

### Standard library changes

None.

### Rationale changes

- New note "Overload resolution tie-breaking (v0.128)" recording why the conservative exact-match-or-ambiguous rule was adopted and the C#-style conversion-target ordering declined.

### Migration

§22.6 was silent on this case in v0.127, so the behavior of such calls was unspecified rather than defined; v0.128 defines it. No conforming v0.127 program relied on a specified outcome here. Under the now-defined rule, a call that reaches two overloads only through different implicit conversions is an ambiguous compile error; where such a call appears, add an explicit cast (§5.2) to make exactly one overload an exact match — e.g. `M(long(x))` or `M(float(x))`. Calls that resolved by exact match, or where only one overload was ever applicable, are unaffected.

### What did not change

- §5.1's set of implicit conversions is unchanged; no conversion is added, removed, or re-ranked.
- §27.6 (overloaded method references) is unchanged; the new §22.6 rule is consistent with it.
- §22.5 (optional/named arguments) and the `owned`-on-`out` overload carve-out (§22.6) are unchanged; tie-breaking operates only on candidates already applicable under those rules.

### Editorial note

The `linkify_refs.py` toolchain was again not re-run for this revision. The substantive edits are small (one new §22.6 subsection, one Appendix C.1 invariant, one rationale note, one changelog entry), and the inter-document filename references were updated mechanically with the version bump. The new cross-references were verified manually against existing anchors.

---

### v0.128 to v0.129 changelog

v0.129 writes down two compiler-enforced rules the spec had left unstated, and resolves one under-specification in §14.2. None of the three changes the language as implemented; each closes a gap between what the compiler already does and what the spec says it does, in keeping with the project discipline that an apparent gap is closed in the spec rather than left implicit in the implementation.

<a id="sec-v0-128-to-v0-129-changelog-summary"></a>
### Summary

Three normative additions:

1. **Inheritance cycles are a compile error.** A new §12.10 states that no type may, directly or transitively, list itself among its base classes or extended interfaces, and that the compiler detects and rejects such cycles naming the participating types. The rule was always enforced; it is now written.

2. **Indistinguishable within-type members are a compile error.** A new paragraph in §11.3 states that a type may not declare two members the type system cannot tell apart — two fields, properties, events, or nested types of the same name, or two methods/constructors/operator overloads of the same name that are not distinguishable as overloads (§22.6). Legal overloads are unaffected; the rule rejects only the duplicates overloading does not admit.

3. **`where T: value` requires a *non-nullable* value type.** §14.2's `value` constraint was silent on whether a nullable value type (e.g. `int?`) satisfies it. v0.129 states that it does not: the `value` constraint admits only the non-nullable value type, and `where T: notnull` likewise excludes every nullable type (it already did, now stated explicitly). Inside a `where T: value` body, T is the non-nullable value type and `T?` is its nullable companion.

Each addition is mirrored by an Appendix C invariant: two new bullets in C.1 (inheritance cycles, indistinguishable members) and the amendment of the C.4 `notnull`/`value` bullets to state the nullable exclusion.

<a id="sec-v0-128-to-v0-129-changelog-decision"></a>
### The §14.2 `value` decision

The choice that a nullable value type does **not** satisfy `where T: value` is the conservative reading and matches C#, whose `struct` constraint is not satisfied by `Nullable<T>` — `where T : struct` rejects `int?`. The reasoning is the same one §14.2 already gives for type-bound constraints: a nullable value type is a distinct type from its non-nullable form (§7.1) with a different representation (it carries the additional `null` state), so admitting it under a constraint named `value` would conflate two types the rest of the type system keeps apart. The alternative — admitting nullable value types and treating `T` as nullable inside the body — was declined because it would make `where T: value` the only constraint under which `T` is nullable, contradicting the uniform rule that `notnull` and every type-bound and `value`/`class` constraint make `T` non-nullable inside the body (§7.10). A generic that wants to accept a nullable value type writes the constraint against the underlying value type and uses `T?` in the body.

<a id="sec-v0-128-to-v0-129-changelog-language-spec-changes"></a>
### Language spec changes

- **[§11.3](Bezel_Language_Spec.md#sec-member-body)** — new paragraph: a type may not declare two indistinguishable members; clashes are a compile error at the second declaration; legal overloads (§22.6) are unaffected; the rule applies to interface members too.
- **[§12.10](Bezel_Language_Spec.md#sec-inheritance-cycles)** — new subsection "Inheritance cycles": a type may not directly or transitively list itself among its base classes or extended interfaces; the compiler detects and reports the cycle.
- **[§14.2](Bezel_Language_Spec.md#sec-constraints)** — `value` row reworded to "non-nullable value types" with an explicit note that `int?` and other nullable value types do not satisfy it; `notnull` row reworded to note it excludes nullable types of either kind; a new prose paragraph states the rule and its reasoning. The parallel §7.10 prose is aligned to match.
- **[Appendix C.1](Bezel_Language_Spec.md#sec-appendix-c-type-system)** — two new invariants (inheritance-cycle rejection; indistinguishable-member rejection).
- **[Appendix C.4](Bezel_Language_Spec.md#sec-appendix-c-generics)** — `notnull` and `value` invariants amended to state the nullable exclusion.

### Standard library changes

None.

### Rationale changes

- This changelog entry, including the recorded §14.2 decision and the C# `struct` precedent.

### Migration

All three additions describe behavior the compiler already enforces, so no conforming program changes meaning. A program that declared an inheritance cycle or two indistinguishable members was already rejected; v0.129 only documents the rejection. For the `value` constraint, a program that instantiated `where T: value` with a nullable value type was likewise already rejected; code that needs to operate on a nullable value type constrains against the underlying value type and uses `T?` in the body.

### What did not change

- The set of value types is unchanged; the `value` constraint still admits records, enums, primitives, inline arrays, and the unit type — only the (already-rejected) nullable forms are now named as excluded.
- Overload distinguishability (§22.6) is unchanged; the new §11.3 rule rejects exactly the same-name members that §22.6 does not already admit as overloads.
- Single-inheritance and the at-most-one-base-class rule (§11.1) are unchanged; §12.10 only adds the cycle prohibition over the inheritance relation those rules define.

### Editorial note

The `linkify_refs.py` toolchain was again not re-run for this revision. The substantive edits are small (one new §12.10 subsection, one §11.3 paragraph, three §14.2 edits plus the §7.10 alignment, four Appendix C bullets, one changelog entry), and the inter-document filename references were updated mechanically with the version bump. The new cross-references were verified manually against existing anchors.

---

<a id="sec-v0-129-to-v0-130-changelog"></a>
### v0.129 to v0.130 changelog

v0.130 folds four decided amendments. Three are spec/stdlib additions that complete partially-stated surfaces (lifted operators, `Array<T>` value copy-forms, generic arguments in expression position); one corrects the source-file extension to match the compiler (`.bzl`). None changes the meaning of an existing conforming program: the operator-lifting and copy-form additions widen what is *accepted*, the grammar addition writes down forms the parser already accepts, and the extension fix is cosmetic to the language definition.

<a id="sec-v0-129-to-v0-130-changelog-summary"></a>
#### Summary

Four changes:

1. **`Array<T>` value copy-forms (#472).** The standard library's `Array<T>` gains `Reversed(): owned Array<T>` and `Sorted(comparer: (T, T) -> int): owned Array<T>`, plus a parameterless `Sorted(): owned Array<T>` in the `where T: IComparable<T>` extension — the return-a-copy counterparts of the existing in-place `Reverse()`/`Sort()`. `OwnedArray<T>` deliberately receives no such forms.

2. **Lifted operators (#486).** A new §7.11 states that the built-in primitive operators lift over nullable value types `T?`: arithmetic/bitwise/shift yield `T?` (null if any operand is null), relational yield `bool` (false if either operand is null), equality is unchanged (already null-safe per §8.4).

3. **`.bzl` source extension (#512).** §1.1's source-file extension changes from `.bezel` to `.bzl`, matching the compiler.

4. **Generic arguments in expression position (#520).** Appendix A.7's expression grammar gains the productions for generic type arguments on a name in receiver position (`Registry<int>.Register()`) and explicit method type arguments at a call site (`obj.Method<int>(args)`), with a note in §22.12.

<a id="sec-v0-129-to-v0-130-changelog-472"></a>
#### The #472 `Array` copy-form decision

The naming follows a uniform convention the library can apply wherever both an in-place and a copy-returning form make sense: the imperative-verb spelling (`Reverse`, `Sort`) mutates in place, and the past-participle spelling (`Reversed`, `Sorted`) returns a fresh `owned Array<T>` and leaves the receiver untouched. Two alternatives were considered and **declined**. The first was a C#-style `AsEnumerable()` bridge — let callers drop into the LINQ surface (`arr.AsEnumerable().OrderBy(...).ToArray()`) rather than provide first-class copy methods; this was declined as indirect for so common an operation and as obscuring the ownership of the result behind a lazy-sequence layer. The second was *return-context-directed resolution* — a single `Sort`/`Reverse` whose in-place-vs-copy behaviour is selected by whether the call site uses the result; this was declined because it makes a mutation's effect depend on its surrounding expression, exactly the kind of action-at-a-distance the rest of the language avoids, and because it interacts badly with `owned` result tracking. The verb/participle split keeps the two behaviours lexically distinct at every call site. `OwnedArray<T>` is left **asymmetric** on purpose: it has neither the copy-forms nor `ToArray`, because its elements are uniquely owned and any value copy would either double-own each element or drain the source; reordering an owning array uses the in-place forms, and moving its elements into a new owning array uses `Drain()`.

<a id="sec-v0-129-to-v0-130-changelog-486"></a>
#### The #486 lifted-operator decision

The model is C#'s operator lifting, adopted because it is the consistent completion of a lifting the language already performs: §8.4 already lifts equality (`=`/`<>`) to be null-safe over nullable operands, and leaving the arithmetic, bitwise, shift, and relational operators *un*-lifted would make equality the lone special case. Lifting is also safe under the existing nullability discipline: a lifted arithmetic result stays typed `T?`, so §7.4 narrowing is still required before any non-null use — lifting never manufactures a non-nullable value out of nullable operands. Three boundaries are drawn explicitly. Relational operators yield plain `bool` (false on a null operand) rather than `bool?`, so an ordering comparison never itself needs narrowing. Three-valued (`bool?`) logical operators are **excluded**: `and`/`or`/`not` stay boolean-only and a `bool?` operand to them is an error, so the language gains no Kleene logic by the back door; code combining `bool?` values handles null explicitly. Lifting of user-defined operators (§25) is **deferred**, scoped to a later revision. This addition **supersedes** an earlier rejected approach that would have *diagnosed* (rejected) the use of a built-in operator on a nullable operand outright; that approach was declined because it forces a narrowing or `!` at every nullable-arithmetic site even where propagating null is exactly what the author wants, and because it is inconsistent with the equality operators, which already accept nullable operands.

<a id="sec-v0-129-to-v0-130-changelog-512"></a>
#### The #512 `.bzl` decision

The compiler already uses `.bzl`; the spec said `.bezel`. The spec is corrected to match the implementation. `.bzl` is the shorter, conventional three-letter form and avoids the visual collision between the language name and the file extension.

<a id="sec-v0-129-to-v0-130-changelog-520"></a>
#### The #520 grammar-gap decision

Two forms in the "generic arguments in an expression" family were accepted by the parser but absent from the grammar: a generic type name in receiver position so a generic class's static members can be reached with the type arguments supplied (`Registry<int>.Register()`), and explicit method type arguments at a call site (`obj.Method<int>(args)`, implemented for #509). v0.130 writes both into Appendix A.7 — `Identifier GenericArgs?` at `PrimaryAtom` and `GenericArgs?` on the `.`/`?.` member-access suffix — with a grammar note recording how the parser resolves the generics-vs-less-than ambiguity (admit the `GenericArgs` reading only when the `<…>` is balanced and followed by a token that continues a primary expression). This is gap-filling: it documents existing behaviour rather than adding a new construct.

<a id="sec-v0-129-to-v0-130-changelog-language-spec-changes"></a>
#### Language spec changes

- **[§1.1](Bezel_Language_Spec.md#sec-source-files)** — source-file extension `.bezel` → `.bzl`; the §18.6 verbatim-string example is updated to match.
- **[§7.11](Bezel_Language_Spec.md#sec-lifted-operators)** — new subsection "Lifted operators": arithmetic/bitwise/shift lift to `T?`, relational lift to `bool`, equality unchanged (§8.4); no `bool?` three-valued logic; user-defined-operator lifting deferred; lifted results remain subject to §7.4 narrowing.
- **[Appendix A.7](Bezel_Language_Spec.md#sec-expressions)** — `PrimaryAtom` gains `Identifier GenericArgs?`; the `.`/`?.` `PrimarySuffix` forms gain a trailing `GenericArgs?`; both carry a disambiguation note.
- **[§22.12](Bezel_Language_Spec.md#sec-static-members)** — note that a generic class's static members are accessed by supplying the type arguments on the type name, cross-referencing the Appendix A.7 grammar.

<a id="sec-v0-129-to-v0-130-changelog-standard-library-changes"></a>
#### Standard library changes

- **[`Array<T>`](Bezel_Standard_Library.md#sec-arrayt)** — `Reversed(): owned Array<T>` and `Sorted(comparer): owned Array<T>` added; the `where T: IComparable<T>` extension gains parameterless `Sorted(): owned Array<T>`. A prose note records the verb/participle convention and the `OwnedArray<T>` ([§4.5](Bezel_Standard_Library.md#sec-ownedarrayt)) asymmetry. In-place `Reverse()`/`Sort()` are unchanged.

<a id="sec-v0-129-to-v0-130-changelog-rationale-changes"></a>
#### Rationale changes

- This changelog entry, recording the four decisions with their declined alternatives.

<a id="sec-v0-129-to-v0-130-changelog-migration"></a>
#### Migration

No conforming program changes meaning. The copy-forms and operator lifting only widen what compiles; the grammar additions describe forms the parser already accepted; the `.bzl` change affects the file extension a toolchain expects, not program text. Programs that relied on a nullable operand to a built-in arithmetic/relational operator being rejected (there were none, since the operators were previously unspecified on nullable operands) would observe the new lifting semantics instead.

#### Editorial note

The `linkify_refs.py` toolchain was again not re-run for this revision. The substantive edits are small (one new §7.11 subsection, two §1.1/§18.6 extension fixes, two Appendix A.7 grammar productions plus a §22.12 note, three `Array<T>` stdlib signatures plus a prose note, one changelog entry), and the inter-document filename references were updated mechanically with the version bump. The new cross-references were verified manually against existing anchors.

---

<a id="sec-v0-130-to-v0-131-changelog"></a>
### v0.130 to v0.131 changelog

v0.131 folds two decided amendments — the soundness batch. Both *define behavior the spec previously left undefined or under-defined*; neither adds a language construct, and no conforming program changes meaning. The first (#600, phase 1) replaces the "continuations may resume on any thread-pool thread, synchronize shared state yourself" runtime model with **execution domains** — serial, run-to-completion units of execution — making the ownership chapter's soundness assumptions normative instead of accidental. The second (#601) replaces "use after `Dispose()` is undefined behavior" with a two-mode definition in which a **checked disposal mode** raises a deterministic exception.

<a id="sec-v0-130-to-v0-131-changelog-summary"></a>
#### Summary

Two changes:

1. **Execution domains (#600, phase 1).** [§30.13](Bezel_Language_Spec.md#sec-runtime-model) is rewritten around the *execution domain*: at most one of a domain's continuations runs at any instant; a continuation runs to its next suspension point without interruption from the domain's other work; a program begins in, and in this revision consists entirely of, a single domain. Three guarantees become normative: per-domain memory visibility (each continuation happens-before the next), absence of intra-domain data races, and the soundness of [§28](Bezel_Language_Spec.md#sec-resource-management)'s ownership and borrow analysis. Consequential rewordings: the [§30 intro](Bezel_Language_Spec.md#sec-asynchronous-programming), §30.2's continuation-queueing bullet, §30.4's multiple-awaiters note, the closing paragraph of [§30.12](Bezel_Language_Spec.md#sec-thread-synchronization), the [§24 thread-safety note](Bezel_Language_Spec.md#sec-thread-safety) (events need no synchronization within a domain), and a parenthetical in [§28.2](Bezel_Language_Spec.md#sec-the-idisposable-contract). Foreign threads entering through the FFI remain outside the domain model and keep the synchronization obligations.

2. **Checked disposal mode (#601).** [§28.2](Bezel_Language_Spec.md#sec-checked-disposal) gains the *disposal-checking mode*, a per-build choice: **unchecked** (default) preserves today's undefined behavior; **checked** raises the new built-in `DisposedAccessException` ([§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), under `InvalidOperationException`) on any instance member access through a reference to a logically-disposed instance. Correct programs behave identically in both modes. An implementation *should* also warn at compile time when a local is used after a dominating explicit `Dispose()` — the statically-decidable subset.

<a id="sec-v0-130-to-v0-131-changelog-600"></a>
#### The #600 execution-domains decision

The contradiction being resolved: the spec simultaneously claimed compile-time lifetime safety (§28) and permitted concurrently-running continuations to dispose a borrow's source mid-use (§30.13 v0.130, rationale §5.1's accepted data races). The language refused to narrow nullable *fields* because another thread could write null between check and use (§6.1) while permitting another thread to *dispose* the object a borrow points into — guarding the smaller hazard and not the larger. v0.131 decides in favor of the ownership model. The full reasoning, including the three **declined** alternatives — Send/Sync-style thread-safety typing (cost lands on every generic signature, benefit on a parallelism model Bezel does not offer), documenting the unsound status quo, and locks-as-discipline (unverifiable) — is recorded as new rationale [§12.5](#sec-execution-domains). The decided *direction* for real parallelism — multiple domains communicating by ownership transfer over channels, with sendability derived from existing type properties — is deliberately **not** in this revision; it is tracked as #600 phases 2–3. The capability given up in the interim is CPU-bound parallelism within one process; I/O-bound concurrency is interleaving at suspension points and is unaffected.

<a id="sec-v0-130-to-v0-131-changelog-601"></a>
#### The #601 checked-disposal decision

Use-after-dispose was the one error class in the disposal story that was neither prevented at compile time (like leaks and double-disposal) nor defined at runtime (like `NullReferenceException` from `!`). The asymmetry was an artifact, not a choice: the machinery a defined fault needs was already mandated — idempotent `Dispose()` (§28.2) implies a per-instance disposed flag, and the logical-disposal/physical-reclamation split (§28.12) already decouples the destructor from memory release, so an implementation may defer reclamation and keep the flag readable. One alternative was considered and **declined**: making the checked semantics unconditional (always raise, no UB mode). Declined because the flag test on every member entry is a real cost on hot paths, because release builds of correct programs would pay it for no observable difference, and because mandating reclamation deferral in all builds constrains allocator design. A second alternative — compile-time prevention via a Rust-style exclusive-access rule — remains declined for the reasons recorded in §5.1; checked mode is the proportionate mitigation. The mode split follows the precedent of contract-checking modes in other systems languages: identical semantics for correct programs, deterministic diagnosis for incorrect ones where the build can afford it.

<a id="sec-v0-130-to-v0-131-changelog-language-spec-changes"></a>
#### Language spec changes

- **[§30.13](Bezel_Language_Spec.md#sec-runtime-model)** — rewritten: new "Execution domains" subsection (normative serial-execution, memory-visibility, no-intra-domain-race, and ownership-soundness guarantees; single-domain in this revision) and a "Scheduling" subsection (domain queueing; thread migration permitted but never two threads at once; FFI threads outside the model).
- **[§30 intro](Bezel_Language_Spec.md#sec-asynchronous-programming), §30.2, §30.4** — "thread pool" wording replaced with domain wording; §30.4 now states continuations never run concurrently within a domain.
- **[§30.12](Bezel_Language_Spec.md#sec-thread-synchronization)** — closing paragraph: single-domain programs need no synchronization for their own state; the types remain for FFI threads and are forward-compatible with multi-domain parallelism.
- **[§24 thread safety](Bezel_Language_Spec.md#sec-thread-safety)** — events need no external synchronization within a domain; the `Mutex` recommendation now applies only where event operations are reachable from foreign threads.
- **[§28.2](Bezel_Language_Spec.md#sec-checked-disposal)** — new "Use after disposal; checked disposal mode" subsection: the two disposal-checking modes, their equivalence for correct programs, the implementation strategy, and the should-level static warning.
- **[§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)** — `DisposedAccessException` added under `InvalidOperationException`, with a prose note that it is checked-mode-only and not for control flow.

<a id="sec-v0-130-to-v0-131-changelog-standard-library-changes"></a>
#### Standard library changes

None. `Mutex`, `Lock`, `Semaphore`, and `Atomic<T>` are unchanged and remain accurate under the domain model (their remaining roles: FFI threads, forward compatibility).

<a id="sec-v0-130-to-v0-131-changelog-rationale-changes"></a>
#### Rationale changes

- New [§12.5](#sec-execution-domains) recording the execution-domains decision and its declined alternatives.
- This changelog entry.

<a id="sec-v0-130-to-v0-131-changelog-migration"></a>
#### Migration

No conforming program changes meaning. A program could only observe the difference between the v0.130 and v0.131 runtime models through a data race, whose behavior was already undefined; such programs were not portable under v0.130 and become well-defined (serialized) under v0.131. `DisposedAccessException` is raised only in checked-mode builds and only by programs whose behavior was previously undefined. Implementations: a runtime that already runs continuations on a single thread is a conforming single-domain implementation as-is; a runtime that migrated continuations across threads without per-domain serialization must add it.

#### Editorial note

The `linkify_refs.py` toolchain was not re-run for this revision. The substantive edits are localized (one §30.13 rewrite, five consequential rewordings, one new §28.2 subsection, one §31.4 tree entry plus a prose note, one new rationale §12.5, this changelog entry), and the inter-document filename references were updated mechanically with the version bump. The new cross-references were verified manually against existing anchors.

---

<a id="sec-v0-131-to-v0-132-changelog"></a>
### v0.131 to v0.132 changelog

v0.132 folds the three legs of the foundation amendment (#602). All three are *declarative* — they name, unify, and partition machinery the specification already had; none changes the meaning of any conforming program, and none adds a language construct. (This revision is also the first under stable document filenames: the version-in-filename convention was retired immediately after v0.131, with the revision number living in each document's header and in this changelog.)

<a id="sec-v0-131-to-v0-132-changelog-summary"></a>
#### Summary

Three changes:

1. **Derived type properties (#602, leg A).** New [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) defines once the mechanism the spec had been using four times over: a compiler-computed structural predicate on types, with uniform rules — computed never declared, evaluated per generic instantiation with instantiation-site diagnostics, part of the public contract, named in diagnostics. The existing four (trivial-dispose, borrow-bearing, auto-equality-bearing, serialization-eligible) plus the element category are tabulated as instances and their defining sections backlink to §3.6. New spec rules needing a structural classification must define it under §3.6 rather than inventing parallel machinery.

2. **The element slot policy (#602, leg B).** New subsection in [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy): one table mapping collection family × element category to slot kind, and slot kind to the semantics of every element operation (store, remove, clear, dispose, get/iterate, ownership-out, `DisposeElements`). The per-collection clauses in spec §28.11 and stdlib §4 are demoted to presentations of the table; a divergence is now by definition a spec defect. The owning column's storage embodiment is the existing `OwnedBuffer<T>`; a user-expressible borrow-storing primitive is **deferred with reasons** (new rationale [§5.20](#sec-one-element-policy)).

3. **Conformance levels (#602, leg C).** New [Appendix D](Bezel_Language_Spec.md#appendix-d-conformance-levels): a **Core** level (the language minus the listed Extended features, with mandatory clean rejection of unimplemented Extended constructs) and a **Full** level, plus "Core + named features" claims. Extended features: Events, Asynchronous programming (incl. §28.12 and the async-iterator forms), Serialization synthesis, FFI, Buffers and spans, Sequence operations. Dependencies and the Core-soundness note (§30.13's domain guarantees hold trivially in a sync-only Core) are stated in D.3/D.4.

<a id="sec-v0-131-to-v0-132-changelog-602a"></a>
#### The leg-A decision

The spec had evolved the same mechanism four times — trivial-dispose (v-early), borrow-bearing (§9.7), auto-equality-bearing (§9.2), serialization-eligibility (§9.8) — and v0.102's buffer work had additionally introduced "type-parameter-category-dependent specialization" as a one-off compiler-known behavior ([§5.15](#sec-owned-buffers-and-spans)). Each was correct; each was specified from scratch; their generic-instantiation behavior and diagnostic conventions agreed only by imitation. Naming the mechanism makes the agreement normative, gives the next property (sendability, #600 phases 2–3, is the known next customer) a definition to cite instead of a pattern to imitate, and gives diagnostics a uniform vocabulary. One alternative was **declined**: user-declared marker traits (opt-in interfaces or attributes asserting a property). Declined because a declared property can lie — the entire value of trivial-dispose, borrow-bearing, and element-category dispatch is that they are *facts about structure* the compiler derives and cannot be miss-asserted; an assertion mechanism would convert structural soundness into programmer promises. Behavior-dispatch on these properties remains reserved to compiler-known standard-library types, unchanged from v0.102's scoping.

<a id="sec-v0-131-to-v0-132-changelog-602b"></a>
#### The leg-B decision

Recorded in full as rationale [§5.20](#sec-one-element-policy): the implementation defect history (a dozen-plus bugs, each a single collection × element-category cell diverging from its column) motivated stating the policy once; `OwnedBuffer<T>` had already embodied the owning column since v0.99/v0.102, so the amendment names and subordinates rather than invents; the borrow-storing user primitive is deferred because it needs its own borrow-lifetime design (storing borrows in fields is otherwise rejected by [§28.3](Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)), and a quick dispensation was declined on v0.97-lesson grounds. The table also closes one small gap discovered during drafting: dictionary key slots are now explicitly classified per the borrowing column (owning dictionaries own values, never keys — consistent with stdlib §4.9's existing prose).

<a id="sec-v0-131-to-v0-132-changelog-602c"></a>
#### The leg-C decision

The driver is the feature-interaction matrix: the language's subsystems are individually sound but their pairwise interactions are where both specification complexity and implementation defects concentrate. A defined inner language — Core — gives implementations, the test suite, and teaching a boundary that must be airtight before the outer ring composes with it. The partition criterion was *severability*: a feature is Extended if a program-rejecting implementation lacking it can still claim a coherent, useful language (events, async, serialization synthesis, FFI, buffers/spans, sequence operations all sever cleanly; nullability, ownership, generics, exceptions do not). Two alternatives were **declined**. *Fully à-la-carte feature flags* (every section independently optional): declined because most sections are not severable and the combinatorics of claims would be meaningless. *No levels* (the status quo): declined because the implementation already stages features de facto — the appendix makes the de-facto staging claimable and testable instead of informal. Mandatory clean rejection (not silent misbehavior) of unimplemented Extended constructs is the load-bearing clause; it is what makes a Core claim trustworthy.

<a id="sec-v0-131-to-v0-132-changelog-language-spec-changes"></a>
#### Language spec changes

- **[§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)** — new section "Derived type properties": the mechanism, the table of the five current properties, the four common rules, the reservation of behavior-dispatch to compiler-known stdlib types.
- **[§9.2](Bezel_Language_Spec.md#sec-body-records), [§9.7](Bezel_Language_Spec.md#sec-records-and-disposal), [§9.8](Bezel_Language_Spec.md#sec-serialization-synthesis), [§28](Bezel_Language_Spec.md#sec-resource-management) intro** — one-line backlinks marking each defining site as a §3.6 instance.
- **[§28.11](Bezel_Language_Spec.md#sec-element-slot-policy)** — new subsection "The element slot policy": the family × category classification, the operation table, the derivation statement, the dictionary key/value clause, the deferred borrow-storing primitive note.
- **[Appendix D](Bezel_Language_Spec.md#appendix-d-conformance-levels)** — new appendix "Conformance Levels" (D.1–D.4); Contents updated; one pointer sentence added to the document intro.

<a id="sec-v0-131-to-v0-132-changelog-standard-library-changes"></a>
#### Standard library changes

- **[§4.3](Bezel_Standard_Library.md#sec-how-the-borrowing-and-owning-hierarchies-relate)** — one paragraph stating that the per-collection sections present, and do not define, element semantics (deriving from spec §28.11's slot policy), and naming `OwnedBuffer<T>` as the owning column's embodiment.

<a id="sec-v0-131-to-v0-132-changelog-rationale-changes"></a>
#### Rationale changes

- New [§5.20](#sec-one-element-policy) "One element policy, not twelve", recording the leg-B reasoning, the defect-history evidence, and the deferred-with-reasons borrow-storing primitive.
- This changelog entry.

<a id="sec-v0-131-to-v0-132-changelog-migration"></a>
#### Migration

No conforming program changes meaning. Leg A and leg C are purely declarative. Leg B restates existing per-collection behavior as one rule; the only normative tightening is definitional (a future per-collection divergence is a spec defect, not an alternative reading). Implementations gain license, not obligations: the compiler may now implement element handling once, keyed by the slot table, and may stage features along the Appendix D partition — the current implementation's de-facto staging becomes claimable.

#### Editorial note

The `linkify_refs.py` toolchain was not re-run for this revision. The substantive edits are localized (one new §3.6, four one-line backlinks, one new §28.11 subsection, one new appendix plus Contents/intro pointers, one stdlib §4.3 paragraph, one new rationale §5.20, this changelog entry). No filename references changed (stable filenames as of this revision). The new cross-references were verified against existing anchors, including the §29 anchor disambiguation (`sec-iteration-2` for the chapter; `sec-iteration` is §16.5's range-iteration anchor).

---

<a id="sec-v0-132-to-v0-133-changelog"></a>
### v0.132 to v0.133 changelog

v0.133 delivers #600 phases 2–3 — parallelism by ownership transfer. v0.131 made the single-domain language sound; v0.133 makes it parallel without weakening anything: new domains are spawned explicitly, and the **sendable** derived type property guarantees that no mutable state is ever reachable from two domains. Every addition is within the *Asynchronous programming* Extended feature (Appendix D); a Core implementation is unaffected, and single-domain async programs are unaffected.

<a id="sec-v0-132-to-v0-133-changelog-summary"></a>
#### Summary

Three additions:

1. **Sendable types ([§30.14](Bezel_Language_Spec.md#sec-sendable-types)).** The sixth derived type property ([§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)): value types sendable iff not borrow-bearing and not move-only; `string` sendable (shared-immutable); function values never sendable; class types sendable iff **transfer-closed** — `sealed` (or a fully-sendable `sealed abstract` closed hierarchy), every field a sendable value / `string` / `owned` field of sendable class type / owning-inline-array of sendable element class, and no function-typed, boxed, interface-typed, or non-`owned` class-typed fields. Compiler-known stdlib types state their sendability per their structure.

2. **`Domain.Spawn` ([§30.14](Bezel_Language_Spec.md#sec-spawning-domains), stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)).** Spawns a domain running a **capture-free** entry (non-capturing lambda or static method reference) with sendable arguments (class-typed ones moved as `owned`) and a sendable result. The join handle is an ordinary `OwnedTask<R>` / `Task` / `Task<R>` — Pattern B's single-consumer ownership transfer ([§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)) *is* the cross-domain join, unchanged. Entry faults surface as `DomainFaultedException` carrying copied message/stack-trace strings.

3. **Channels ([§30.15](Bezel_Language_Spec.md#sec-channels), stdlib [§6.9](Bezel_Standard_Library.md#sec-channel)).** Unbounded MPSC `Channel<T>` (the receiving end) and `ChannelSender<T>` endpoints minted by `NewSender()`; `Send(owned item)` consumes in the sender's domain, awaiting `Receive()` (an `OwnedTask<T>` for class T) produces owned in the receiver's; last-sender disposal closes (drain, then `ChannelClosedException`); channel disposal disposes undelivered class-typed items in reverse insertion order per the [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy) owned column. Both endpoint types are sendable; they are the only standard types two domains may reach simultaneously, and they are compiler-known and internally synchronized.

<a id="sec-v0-132-to-v0-133-changelog-600"></a>
#### The #600 phases 2–3 decisions

Recorded in full as rationale [§12.6](#sec-sendability-spawning-channels): the no-shared-mutable-state invariant and why sendability needs no new type system (ownership transfer is the existing `move` machinery; the borrow-bearing computation supplies value-type transitivity); the `sealed` requirement that closes the static-types-vs-runtime-objects subtyping hole, with the runtime send-check recorded as the declined-for-now widening; capture-free entries over move-capturing lambdas; `DomainFaultedException` over cross-domain exception objects; MPSC with minted senders over two-endpoint `Create` and over multi-consumer forms (deferred to #603's shared ownership); bounded channels deferred; cancellation falling out as domain-local because `CancellationToken` is borrow-bearing.

<a id="sec-v0-132-to-v0-133-changelog-language-spec-changes"></a>
#### Language spec changes

- **[§30.14](Bezel_Language_Spec.md#sec-multiple-domains)** — new section "Multiple domains and sendable types": the no-shared-mutable-state discipline, the sendable property (value/string/boxed/function/class/interface/stdlib rules, coinductive evaluation, §3.6 instantiation-site checking), `Domain.Spawn` call-site rules (capture-free entry, sendable arguments/results), join semantics, faults, domain completion.
- **[§30.15](Bezel_Language_Spec.md#sec-channels)** — new section "Channels": the MPSC model, endpoint ownership and sendability, send/receive ownership transfer, closing and disposal semantics (slot-policy owned column), ordering.
- **[§30.13](Bezel_Language_Spec.md#sec-runtime-model)** — the single-domain-only paragraph replaced with the spawn/channel pointers and the "domains share no mutable state" statement.
- **[§30.12](Bezel_Language_Spec.md#sec-thread-synchronization)** — closing paragraph updated: multi-domain programs create no synchronization obligations; the synchronization types remain only for FFI threads.
- **[§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)** — sendable added to the property table.
- **[Appendix D](Bezel_Language_Spec.md#appendix-d-conformance-levels)** — the Asynchronous-programming feature row now names domains, spawning, and channels (§30.13–§30.15) and the `Domain`/`Channel` stdlib types.

<a id="sec-v0-132-to-v0-133-changelog-standard-library-changes"></a>
#### Standard library changes

- **[§6.8](Bezel_Standard_Library.md#sec-domain)** — new: `Domain` (the `Spawn` overload set over argument/result categories) and `DomainFaultedException`.
- **[§6.9](Bezel_Standard_Library.md#sec-channel)** — new: `Channel<T>`, `ChannelSender<T>` (category-dispatched signatures per the `OwnedBuffer<T>` precedent), and `ChannelClosedException`.

<a id="sec-v0-132-to-v0-133-changelog-rationale-changes"></a>
#### Rationale changes

- New [§12.6](#sec-sendability-spawning-channels) recording the decisions and declined alternatives; a delivery note appended to [§12.5](#sec-execution-domains).
- This changelog entry.

<a id="sec-v0-132-to-v0-133-changelog-migration"></a>
#### Migration

Purely additive. No existing program changes meaning: programs that never call `Domain.Spawn` remain single-domain programs with v0.131's guarantees verbatim. The new names (`Domain`, `Channel<T>`, `ChannelSender<T>`, `DomainFaultedException`, `ChannelClosedException`) live in `System.Threading` and require its import. Core implementations (Appendix D) reject the new constructs as part of the Asynchronous-programming feature.

#### Editorial note

The `linkify_refs.py` toolchain was not re-run for this revision. The substantive edits are localized (two new spec sections §30.14–§30.15, two §30.12/§30.13 reword paragraphs, one §3.6 table row, one Appendix D row, two new stdlib sections §6.8–§6.9, one new rationale §12.6 plus the §12.5 note, this changelog entry). The new cross-references were verified against existing anchors.

---

<a id="sec-v0-133-to-v0-134-changelog"></a>
### v0.133 to v0.134 changelog

v0.134 adds **scoped shared ownership** (#603): `Shared<T>` and `Weak<T>`, the library type [§5.4](#sec-no-shared-ownership) said could be added without language changes — and it was: no new syntax, no new ownership state, no checker changes. Purely additive; a new Extended feature ("Shared ownership", Appendix D), so Core implementations are unaffected.

<a id="sec-v0-133-to-v0-134-changelog-summary"></a>
#### Summary

One addition:

1. **`Shared<T>` / `Weak<T>` ([§28.13](Bezel_Language_Spec.md#sec-shared-ownership), stdlib [§2.16](Bezel_Standard_Library.md#sec-shared-and-weak)).** Compiler-known classes: N *handles* jointly own one underlying class-typed value; `new Shared<T>(owned value)` consumes the value, `Clone()` explicitly mints another owned handle, `Get()` borrows the value with the handle as source (the owning-collection indexer-get dispensation), `Downgrade()`/`TryUpgrade(out owned …)` give the weak/upgrade pair in the existing Try-pattern shape. The last handle's disposal disposes the value — the exactly-once invariant (Appendix C) is preserved. Handles are *ordinary owned class instances*: every §28 rule applies to them unchanged; the only novel semantics are inside the compiler-known type. `Shared<T>`/`Weak<T>` are domain-local (not sendable; the count is unsynchronized). Cycles through `Shared<T>` leak (standard refcount hazard, documented; `Weak<T>` is the mitigation; no cycle detection — that would be tracing collection by another name).

<a id="sec-v0-133-to-v0-134-changelog-603"></a>
#### The #603 decision

Recorded in full as rationale [§5.21](#sec-shared-t): why the gap stopped being hypothetical (caches, registries, the multi-consumer-channel question of §12.6); why compiler-known rather than pure library (N-holders-keep-alive *is* the feature and cannot be built from unique ownership; precedent: the compiler-known collections, and the runtime's existing invisible refcounts for strings/closures); why handles-as-ordinary-objects rather than a third reference kind (**declined**: a new ownership state would touch every join rule and producer-table entry for no added expressiveness); why domain-local (**deferred**: a synchronized `SharedAtomic<T>` — also the prerequisite for multi-consumer channels — until a use case forces the atomic-cost question; adding it later is non-breaking). Also **declined**: annotation-driven refcounting of arbitrary classes (hides cost) and compiler-inferred sharing (implicit magic). [§5.4](#sec-no-shared-ownership)'s position stands with a delivery note: single ownership remains the default and the only implicit regime.

<a id="sec-v0-133-to-v0-134-changelog-language-spec-changes"></a>
#### Language spec changes

- **[§28.13](Bezel_Language_Spec.md#sec-shared-ownership)** — new section "Shared ownership: `Shared<T>` and `Weak<T>`": semantics stated against the existing model (handles as ordinary owned instances; constructor consumes; explicit `Clone`; `Get` borrow-from-handle; last-one-disposes; cycle hazard; aliasing note; not sendable).
- **[§30.14](Bezel_Language_Spec.md#sec-sendable-types)** — `Shared<T>`/`Weak<T>` added to the not-sendable stdlib list.
- **[Appendix D](Bezel_Language_Spec.md#appendix-d-conformance-levels)** — new Extended feature row "Shared ownership"; the Core enumeration's §28 carve-out now reads "except §28.12 and §28.13"; D.3 notes the feature depends only on Core.

<a id="sec-v0-133-to-v0-134-changelog-standard-library-changes"></a>
#### Standard library changes

- **[§2.16](Bezel_Standard_Library.md#sec-shared-and-weak)** — new: `Shared<T>` and `Weak<T>` declarations, usage notes, and the cache example. Auto-imported with `System`.

<a id="sec-v0-133-to-v0-134-changelog-rationale-changes"></a>
#### Rationale changes

- New [§5.21](#sec-shared-t) recording the decision and declined alternatives; a delivery note appended to [§5.4](#sec-no-shared-ownership).
- This changelog entry.

<a id="sec-v0-133-to-v0-134-changelog-migration"></a>
#### Migration

Purely additive. No existing program changes meaning. The new names are auto-imported with `System`; programs that already declare types named `Shared` or `Weak` shadow the library types per the ordinary name-resolution rules. Core implementations (Appendix D) reject the types as part of the Shared-ownership feature.

#### Editorial note

The `linkify_refs.py` toolchain was not re-run for this revision. The substantive edits are localized (one new spec §28.13, one §30.14 list edit, two Appendix D edits, one new stdlib §2.16, one new rationale §5.21 plus the §5.4 note, this changelog entry). The new cross-references were verified against existing anchors.

---

<a id="sec-v0-134-to-v0-135-changelog"></a>
### v0.135 changelog (v0.134 → v0.135)

v0.135 folds two small decided amendments — a documentation-defect fix and a scoping correction. Neither changes the meaning of a conforming program; the second *widens* what is accepted (it licenses an implementation feature the prior text over-restricted).

<a id="sec-v0-134-to-v0-135-changelog-summary"></a>
#### Summary

1. **`TypeInitializationException` surface documented (#555).** The type appeared in [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)'s tree and §22.13's semantics, but its `InnerException: Exception?` property was stated only in prose and the standard-library document never mentioned the type. §31.4 now declares the property explicitly, and a new stdlib [§2.17](Bezel_Standard_Library.md#sec-built-in-exceptions-cross-reference) records the built-in/import-required split (built-ins are normative in spec §31.4; producers' exceptions live with their producers) and the one built-in with extra surface.

2. **Borrowing-`Dictionary` entry iteration scoped correctly (#548).** The spec's `for` chapter has always shown `for var entry in dict` as a working example, and stdlib §4.8 stated that iteration yields entries in insertion order — but a blanket sentence in stdlib §4.9 ("default `for var entry in dict do` iteration is not provided") read as covering both dictionary kinds, and the implementation's #530 diagnostic enforced it that way. The ownership argument behind the restriction (a record entry cannot *own* a value) only bites the owning dictionary. Amended: stdlib [§4.8](Bezel_Standard_Library.md#sec-dictionaryk-v) now states the entry-iteration form explicitly — single-pass, `DictionaryEntry<K, V>` elements, class-typed `Value` reads are borrows per the [§28.11](Bezel_Language_Spec.md#sec-element-slot-policy) borrowing column; stdlib [§4.9](Bezel_Standard_Library.md#sec-owneddictionaryk-v)'s restriction is now explicitly scoped to `OwnedDictionary<K, V>` (whose look-don't-take entry view remains a deliberately untaken separate decision). This licenses the implementation to narrow the #530 diagnostic to the owning dictionary and implement borrowing entry iteration (#548).

<a id="sec-v0-134-to-v0-135-changelog-changes"></a>
#### Changes

- **Spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)** — one paragraph declaring `TypeInitializationException.InnerException`.
- **Stdlib [§2.17](Bezel_Standard_Library.md#sec-built-in-exceptions-cross-reference)** — new cross-reference subsection.
- **Stdlib [§4.8](Bezel_Standard_Library.md#sec-dictionaryk-v)** — entry-iteration form stated explicitly with element type and borrow semantics.
- **Stdlib [§4.9](Bezel_Standard_Library.md#sec-owneddictionaryk-v)** — the not-provided sentence scoped to `OwnedDictionary<K, V>` with a cross-reference to §4.8.
- **Rationale** — this changelog entry.

#### Migration

None. Amendment 2 only widens acceptance: programs previously rejected by the over-broad diagnostic become legal once the implementation lands; no existing program changes meaning.

#### Editorial note

The `linkify_refs.py` toolchain was not re-run; the four edits are localized and the new cross-references were verified against existing anchors.

---

### v0.136 changelog (v0.135 → v0.136)

Five amendments: two stdlib additions that unblock tracked implementation work (#553, #547), one async-dispatch clarification (#451's open question), and two §9.8 clarifications surfaced by the serialization tier-2 test derivation (#659).

#### Amendment 1 — Encoding semantics for `Utf16`, `Utf32`, `Ascii` (stdlib §3.2; closes the spec side of #553)

The four standard encodings were declared in §3.2 with no normative semantics. §3.2 now specifies, per encoding: byte order (UTF-16/32 are little-endian only), BOM policy (never emitted; one leading little-endian BOM consumed on decode), and the error model. **Decision: strict defined-fault error model** — `FormatException` on ill-formed input bytes (decode) and unrepresentable code points (encode, `Ascii` only); no replacement characters. *Declined: lenient decode-with-replacement (U+FFFD).* Silent substitution is data corruption by another name, and Bezel's design rule is that faults are defined and loud (cf. checked disposal, §28.2; serialization contract violations, §9.8). A program that wants replacement decoding writes it explicitly over the byte span. *Declined: big-endian variants* — no in-scope producer; byte-swapping is one explicit loop, and the singleton surface stays minimal.

#### Amendment 2 — `SortedDictionary<K, V>` and `SortedSet<T>` (stdlib §4.17/§4.18, new; spec side of #547)

Comparison-ordered associative collections with `K: IComparable<K>` constraints, O(log n) point operations, ascending in-order single-pass borrowing iteration (§4.8 form), `First`/`Last` accessors. **Decision: ordering comes from the key type's `CompareTo` only — no comparer parameter.** Keeping equality, ordering, and hashing on the type (one identity per type) avoids the comparer-mismatch class of bugs (a set built with one comparer queried under another); a different order is a wrapper record away. **Decision: no `OwnedSortedDictionary`/`OwnedSortedSet` in this revision** — owning sorted collections ride the self-hosted-collections direction (#545), where the element-slot policy (§28.11) already centralizes the finalization machinery they would need.

#### Amendment 3 — Catch guards are synchronous (spec §30.9; settles #451's deferred question)

§30.9 stated `await` has no placement restrictions in `try`/`catch`/`finally`; that sentence is now scoped to *bodies*, and a new paragraph states that a `when` guard on a catch arm may not contain `await` (compile error). Exception dispatch runs to completion without suspension. *Declined: asynchronous guards* — a suspension point inside dispatch leaves the exception neither caught nor propagating across the suspension; re-entering dispatch after resumption would require materializing dispatch state in the state machine for a construct whose use cases are served by awaiting before the `try` or re-raising from the arm body. The implementation has diagnosed this since #451; the spec now says so.

#### Amendment 4 — §9.8 constructor selection clarified (#659 silence 3)

The synthesized `Deserialize` text said "its constructor," silent on classes with several. Now: the unique constructor binding exactly the serialized components and borrowing fields by declared name; zero or several matches are a compile error at the conformance declaration naming the candidates. Compile-time decidable, consistent with the eligibility-error discipline.

#### Amendment 5 — §9.8 records × class-typed components, and `sealed` (#659 silences 4 and 2)

A class-typed component of a *record* participates only as an owning edge (`owned` field of a clause-carrying class); a borrowing class-typed component makes the record ineligible (compile error naming the field) — a record declares no owning spanning tree against which a back-edge could resolve. One parenthetical states `sealed` is irrelevant to serialization. (#659 silence 1 — polymorphic components — needed no amendment: §9.8 has specified the `polymorphic` registry and the no-registry compile error since v0.122.)

#### Touched

- **Spec §30.9** — bodies scoping + catch-guard paragraph (cross-refs §19.4, §21.8).
- **Spec §9.8** — constructor-selection paragraph; record/owning-edge paragraph; `sealed` parenthetical.
- **Stdlib §3.2** — encoding semantics block.
- **Stdlib §4.17/§4.18** — new sections (anchors `sec-sorted-dictionary`, `sec-sorted-set`).
- **Rationale** — this changelog entry.

#### Migration

None. Amendments 1–2 specify previously-unspecified surface; 3–5 codify the implementation's existing (or compile-error) behavior. No existing well-formed program changes meaning.

#### Editorial note

The `linkify_refs.py` toolchain was not re-run; edits are localized and all new cross-references were verified against existing anchors (`sec-guards`, `sec-throw`, `sec-dictionaryk-v`, `sec-sett-and-ownedsett`).

---

### v0.137 changelog (v0.136 → v0.137)

Two small rulings, both settling questions the implementation surfaced (#670, #655).

#### Amendment 1 — `Decimal.Parse` rejects surrounding whitespace (stdlib §9.3; closes #670)

§9.3 was silent; the implementation rejected. Now specified: optional sign, digits, at most one
`.`, **no surrounding whitespace** (`FormatException`), out-of-range → `OverflowException`,
`TryParse` returns `false` wherever `Parse` raises. *Declined: leading/trailing trim* — the
strict-fault model adopted for `Encoding` in v0.136 applies for the same reason: silently
tolerated padding hides upstream data problems, and an explicit `Trim()` is one call.

#### Amendment 2 — `await` is a compile error inside an async iterator's `finally` (spec §30.7; settles #655)

When a consumer abandons iteration, pending `finally` blocks run synchronously inside
`Dispose` (§29.7) — no async driver exists at that point, so a suspension there has nowhere
to resume. The implementation's observed behavior (continuation silently discarded, #655) was
a bug, not a semantic. *Declined: an async disposal driver (DisposeAsync-style protocol)* —
it adds a second disposal contract to §28 for one construct; revisit only if real programs
need awaited cleanup that cannot hoist before the first `yield`. *Declined: defined
"continuation discarded" semantics* — silent partial execution of cleanup code is the exact
failure mode the defined-fault philosophy exists to prevent. The rule mirrors v0.136's
catch-guard ruling: positions evaluated during synchronous unwinding or dispatch are
synchronous. Ordinary `async` methods are unaffected (§30.9).

#### Touched

- **Spec §30.7** — the `await`-in-`finally` paragraph (cross-refs §29.6, §29.7, §30.9).
- **Stdlib §9.3** — the `Parse`/`TryParse` semantics paragraph.
- **Rationale** — this changelog entry.

#### Migration

Programs that awaited inside an async iterator's `finally` were already broken (silently
skipped cleanup on early disposal); they now fail to compile with a remedy in the message.
No other program changes meaning.

---

### v0.138 changelog (v0.137 → v0.138)

One ruling: **`Mutex` is sendable; `Lock` is not** (stdlib §6.2; settles the question flagged in the #647 design note).

A mutex exists to be reached from multiple domains — it joins `Channel<T>`/`ChannelSender<T>` as the third internally-synchronized standard type two domains may touch simultaneously (§6.9's "only" sentence amended accordingly). A held `Lock` stays on its acquiring domain: acquisition and release must pair on one domain for the `using var` guard discipline to stay sound; *declined: sendable Lock* — moving a held guard would let domain B release what domain A acquired, reintroducing exactly the cross-thread release hazards the guard idiom exists to prevent.

#### Touched
- **Stdlib §6.2** — sendability paragraph. **Stdlib §6.9** — "only standard types" sentence now includes Mutex. **Rationale** — this entry.

#### Migration
None; this only widens acceptance (Mutex was conservatively rejected at Spawn boundaries pending the ruling).

---

### v0.139 changelog (v0.138 → v0.139)

One amendment: **the JSON wire form for graph-identity tokens** (stdlib §2.15). v0.138's JSON
mapping covered the value tokens but not §9.8's identity operations; the implementation
truncated output at the first tag (found by the 06-13 verification probe). Now: `"$id": n` as
a reserved first field at the tagged object's first occurrence; `{"$ref": n}` for
back-references; `$`-initial field names reserved (a `serialized "$..."` rename is a compile
error). *Declined: a side-table section* (all identities collected in a separate JSON member)
— it separates an object from its identity, breaks streaming reads, and has no precedent in
the §9.8 design, which interleaves tags with the owning-tree walk. The `$id`/`$ref` spelling
follows the vocabulary §9.8's prose already uses.

#### Touched
Stdlib §2.15 (JSON-mapping paragraph); rationale (this entry).

#### Migration
None: graph-mode JSON serialization previously produced truncated output (an implementation
defect, now also tracked as a compiler issue); tree-mode JSON and all binary forms are
unchanged.

---

## 20. Closing

If the opening framed Bezel by what it borrows, the body has shown what it actually contributes: not the features — those have ancestors — but the discipline applied to combining them. The recurring move in this document is the same one every time a hard question arrived. Ship the part that fits the foundations cleanly. Decline the part that fights them, with the reason written down so the question stays settled rather than perennially reopened. Scope whatever genuinely remains open by naming its precise blocker, not by deferring it vaguely. Member-access reflection is declined, not pending. The cyclic-serialization soundness obligation was discharged in prose before any syntax was written. Tuples and post-condition loops are conveniences with recorded costs, not gaps. The deferred list is a list of decisions deliberately not yet made — each with its reasoning — never a list of unsolved problems.

That is the claim this document makes for the language: not that it is novel, but that every line in it was chosen rather than defaulted into, and that the choices cohere. The design is, by intent, complete. What remains is implementation — the language exists on paper, and a compiler will surface concrete questions the spec did not anticipate. Those questions will be answered the same way every question here was: by determining what the foundations already imply before adding anything new.
