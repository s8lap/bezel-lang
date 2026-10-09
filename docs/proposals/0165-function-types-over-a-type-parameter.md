# Proposal 0165 — Function types over a type parameter: a depth bound, and the function values a generic body hands out

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": option E (SIMPLIFY X19 with the hand-out rules) — a type parameter occurs in a function type at depth one only, in a generic declaration and every type its bodies write; a function value a generic body forms over its type parameters is bound, invoked, captured or passed to a function-typed parameter, never returned (a lambda's `return` included), stored outside a `scoped` field, converted to `Object` or varied away; a member exporting a bare-`T` return slot of a function type is inadmissible at a `Task`- or iterable-family substitution; proposal 0133's nested-slot exclusion is replaced; critique round 1 applied, which dropped the draft's declaration-site ban as redundant, widened the depth bound to body-written types and extended the value rule to a lambda's `return`; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5762](https://github.com/s8lap/bezel-lang/issues/5762) (`spec-defect`: the iterable family has no exclusion for a bare-`T` return slot inside a parameter slot), [#5763](https://github.com/s8lap/bezel-lang/issues/5763) (`spec-defect`: the nested-slot exclusion is stated for parameters only); one `spec-tracking` compiler tracker is filed at landing (§8) |
| **Target module(s)** | **spec**: §28.3 *Bare-type-parameter returns* (the `Task`-family exclusion's closing sentences; the nested-slot exclusion replaced by three paragraphs — the depth bound, the rule for the function values a generic body forms over its type parameters, and the exported-mint exclusion), the announcement table's row 5 (one cell), §28.3 *Set-erasing positions* (the two-routes sentence), §12.9 (one parenthetical), Appendix C.3 (two bullets). **rationale**: §4.2 (one dated paragraph). **code-generation reference**: §4.4 (one sentence replaced), §7.2 (one *Looks right, is rejected* pair). **`DECLINED.md`**: five `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | The generics matrix's first run, root causes RC22 and RC23 (`scratch/matrix2/ROOTS.md`; cells R2/C1, R2/C2, R6/C1, R6/C2, R6/C7 and its ·exported-fn route), the simplification review's candidate X19 (`scratch/matrix2/SIMPLIFY.md`) and the retrospective's R2 (`scratch/matrix2/RETRO.md`, which takes X19 as its one carve-out). The owner directed on 2026-10-08: "Please resolve and decide all known spec related issues. Do proposals for issues that need them", and on 2026-10-09: "Please also resolve all findings autonomously". It needs a proposal because it withdraws proposal 0133's nested-slot exclusion in favour of a declaration-and-body shape rule, and adds a rule on a generic body's own function values that rejects one test program. |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

§28.3 closes two seams of a generic body with per-instantiation exclusions stated over a *parameter's* declared type: a bare-`T` **return slot** inside a parameter's function type at a `Task`-family substitution (the body's lend read by the caller as a mint), and a bare-`T` **parameter slot** inside a parameter slot at a function-type substitution (a caller's closure stored by the body under the class-or-interface assumption). The matrix found the same two hazards outside a parameter's type: in a **field** (`Reg: ((() -> T) -> ())?`, `Reg: (((T) -> ()) -> ())?`), through a **return slot of a function in a parameter slot** (`reg: (() -> ((T) -> ())) -> ()`), for the **iterable family** in the body-to-caller direction (`reg: (() -> T) -> ()` at `T = IEnumerable<int>`, which "needs no per-substitution exclusion" covered the other direction only), and — at depth one — a function value the body **hands out** at a `(T) -> …` return, field or `Object` position, which the caller invokes under the call-bounded default with a capture-bearing closure. Drafting this proposal found the exported twin of #5762: a `() -> T` return at a mint-family substitution, whose invocation the caller keys as a mint while the body lends.

**Decision.** Three clauses replace the nested-slot exclusion; two are a shape rule and a restriction on a generic body's own forms, and one is a per-instantiation exclusion of the kind the `Task`-family rule already is:

1. **Depth bound (SIMPLIFY X19).** In a generic declaration — its signature, its declared types and every type written in its bodies (a local's or lambda's annotation, an `as`, `is` or pattern target, a type argument) — a type parameter, bare, nullable or within a type argument, does not occur inside a function type that is itself inside a function type. Reach: **zero** sites. This closes every depth-two shape in both directions and for both families, the body's `o as ((T) -> ()) -> ()` narrowing included.
2. **Function values a generic body forms over its type parameters.** A function value a generic body forms or retypes at a function type that places one of the body's type parameters, bare or nullable, in a parameter slot, at any depth, is bound to a local, invoked, captured or passed to a function-typed parameter, and nowhere else: not returned (a lambda's `return` included), not stored in a holder other than a `scoped` field, not converted at a set-erasing position (`Object`, or a position another generic declares at a bare type parameter the body instantiates with such a function type); and a variant conversion at a type argument that is such a function type is rejected in the body. Only such a value carries the hazard, so this one rule closes every hand-out route, typed and untyped. Reach: **one** test program (`lambda_generic_capture_1261`'s `Bind(x: T): (T) -> T` returning `lambda(y: T) => y`), zero documents.
3. **Exported mint slots.** A member carrying a function type whose **return slot** (nullability stripped) is a bare type parameter outside a plain, `owned` or `scoped` parameter's declared type (`Make(): () -> T`, `Array<() -> T>` as a field, an `out` slot) is inadmissible at an instantiation substituting a `Task`-family or iterable-family type. Reach: **zero** sites (15 `() -> T` returns in four gtest files, all instantiated at classes or `int`).

The parameter-side `Task`-family exclusion and the iterable family's inhabitant-side closure at a *parameter's* slots are unchanged. Declarations of a `(T) -> …` return, field, property or record component stay admitted (`public OnChange: ((T) -> ())?`). Newly admitted: nothing.

*Review note.* The draft carried a fourth clause, a declaration-site ban on a bare `T` in a parameter slot of any function type outside a parameter's declared type (a return type, a non-`scoped` field or property, a record component, a choice payload, an inline-array element type, a type argument of one), rejecting three test programs. The critic dropped it: every value that can occupy such a declaration is either one clause 2 restricts — which cannot get there — or a value formed outside the generic body and checked there against the substituted type, so the ban carried no soundness and cost a callback-field idiom and two lit programs (`owned_functype_slot_erasure*_1817.bzl`, which return `null`). It is a `declined-at-proposal` row. The same review widened clause 1 to body-written types and made clause 2's `return` cover a lambda's, closing two routes the draft left open (§1.4).

---

## 1. The problem

### 1.1 Depth two, outside a parameter (#5763 shapes 1 and 2; #5762)

The nested-slot exclusion reads: "A member is inadmissible at an instantiation substituting a function type for a type parameter `T` when, within some parameter's declared, pre-substitution type, `T` occurs anywhere within a parameter slot of a function type that itself occupies a parameter slot of that type". A field is no parameter, and "occupies" read as *directly* leaves `(T) -> ()` in a return slot unexcluded:

```bezel
class Item
  public N: int;
end Item;
class Keep<T>
  public Reg: (((T) -> ()) -> ())?;           // a field: the exclusion reads parameters
  private _last: T?;
  public method Arm()
    Reg!(lambda(x) => do _last := x; end);    // stores what the caller's function hands it
  end Arm;
  public method Last(): T? => _last;
end Keep;
static class Use
  public static method Feed(cb: (() -> int) -> ())
    using var it := new Item();
    cb(lambda() => it.N);                      // capture-bearing closure, call-bounded slot: admitted
  end Feed;                                    // `it` disposed; Keep<() -> int>._last still captures it
  public static method Run()
    var k := new Keep<() -> int>();
    k.Reg := Feed;                             // capture-free static reference at a holder
    k.Arm();
    _ := k.Last()!();                          // reads the disposed `it`
  end Run;
end Use;
```

The `Task`-family exclusion is stated over "a member whose declared type or signature contains, anywhere within a parameter's declared type, a function type whose return slot is a bare type parameter", and §28.3 then says "**The iterable family needs no per-substitution exclusion:** … the mint-laundering inhabitants are rejected **inhabitant-side**". That closes caller-to-body (`f: () -> T`). Body-to-caller is open: with `reg: (() -> T) -> ()` the body passes `lambda() => t`, a lend admitted at the pre-substitution bare slot (*Calls mint* clause 7), to a caller's `Drain(cb: () -> IEnumerable<int>)`, which keys `cb()` on its own declared return — row 2, an owned frame — and whose `for` loop disposes the lent collection (#5762's program; `xs.Add(4)` after it is a checked-mode `DisposedAccessException`).

### 1.2 Depth one, handed out (#5763 shape 3; cell R6/C7 and its route)

```bezel
class Item
  public method N(): int => 1;
end Item;
class Sink
  private _o: Object?;
  public method Keep(o: Object?) => _o := o;   // field-store residual
end Sink;
static class Registry
  public static readonly Sink: Sink := new Sink();
  public static method Take(o: Object?) => Registry.Sink.Keep(o);
end Registry;
class Box<T>
  public method Put(): (T) -> ()
    return lambda(t) => Registry.Take(t);   // no conversion written at all
  end Put;
end Box;
static class Demo
  public static method Run(b: Box<() -> int>)
    using var it := new Item();
    var g: () -> int := lambda() => it.N();   // source set {it}
    var p := b.Put();
    p(g);               // plain function-typed slot: call-bounded default admits g
  end Run;              // Registry.Sink holds g past it
end Demo;
```

§28.3's induction ("every T-typed value in a body arrived capture-free") enumerates the entries into a generic body as its parameters, the invocation results of function-typed parameters and the nested slots. A function value the body hands out and the caller invokes is not among them. The typed routes are visible in the signature — a `(T) -> …` return, field, property, record component, or a type argument of one (`IEnumerable<(T) -> ()>`, the frame a `Select` over `lambda(x) => lambda(t: T) => …` builds). Two routes are not: a conversion to `Object` inside the body (`_o := lambda(t: T) => Registry.Take(t);`, capture-free, so admitted at the set-erasing position) narrowed back by the caller and invoked; and a store at a position another generic declares at a bare type parameter the body instantiates with such a function type (`new Keep<(T) -> ()>(lambda(t: T) => …)` converted to `Object` and read back through `Keep`'s surface) — and, for a collection of such values, §12.9's variance (`IEnumerable<(T) -> ()>` to `IEnumerable<Object>`, a function type being a reference type) carries them with no value converted.

The mirror at a mint family: `Get(): (Box<T>) -> T => lambda(b) => b.V;` over `class Box<T> public V: T; end Box;` — capture-free, so the sink rule admits the return, and lending its argument's field at the pre-substitution bare slot — gives the caller, at `T = Task`, `f: (Box<Task>) -> Task`, whose invocation the function-value row keys on the substituted return — a mint — so `using var t := f(box);` adopts the task `box` still holds (a double owner; checked mode faults at the second disposal). (A capturing `lambda() => _t` is already rejected at the `return` by the sink rule; the hazard needs a capture-free lend.) The parameter-side exclusion does not reach a return, and no cell of the first run opened this face.

### 1.3 What the compiler does today

Source-read at `429fd72`; no build was run. No implementation of the nested-slot exclusion or of the `Task`-family exclusion was found in `src/sema/` or `src/ownership/` (`grep -rn -i "nested parameter slot\|Task.*family.*exclu"` finds comments on unrelated record-initializer handling only), so every clause here is spec-ahead-of-impl, as the rules it replaces were. The compiler compiles `Bind(x: T): (T) -> T => lambda(y: T) => y;` and runs it at `T = Payload` (`lambda_generic_capture_1261`), sound at a class substitution and the one program clause 2 rejects.

### 1.4 Two routes found in review

- **A depth-two type written in the body.** The draft's depth bound read signatures and declared types only. A generic body can still name `((T) -> ()) -> ()` by narrowing: `var reg := o as ((T) -> ()) -> ();` over an `Object` a caller filled with a capture-free static method reference `Feed(k: (() -> int) -> ())` that invokes `k` with a closure over its own `using var` local (call-bounded, admitted). The body passes `reg(lambda(t: T) => _last := t);` — passing to a function-typed parameter, which the value rule admits — and at `T = () -> int` the closure lands in `_last`. Clause 1 now covers every type written in a generic body.
- **A selector that returns a body-formed function value.** `return Object(xs.Select(lambda(x: T) => lambda(t: T) => Registry.Take(t)));` builds a frame whose *elements* are body-formed `(T) -> ()` values; the frame leaves through `Object` and the caller narrows it to `IEnumerable<(() -> int) -> ()>`. The draft barred "a `return` of the enclosing member" only. Clause 2 now bars a lambda's `return` of such a value as well, so no frame, task (`async lambda() => h`) or selector result is one.

---

## 2. Options

| | Option | Depth two (fields, return-in-parameter, iterable reverse) | Typed hand-out (`Put(): (T) -> ()`, `IEnumerable<(T) -> ()>`) | Untyped hand-out (`Object`, a bare-`T` slot of another generic, variance) | Exported `() -> T` at a mint family | Text |
|---|---|---|---|---|---|---|
| **A** | Status quo | open | open | open | open | — |
| **B** | Restate the exclusion by **polarity** over the member's whole declared type (#5763's recommendation): `T` inadmissible at a function-type substitution wherever its values flow from a caller-supplied function into the body, fields, returns and `out` slots included; extend the `Task` exclusion to every mint family in body-to-caller polarity (#5762's recommendation) | closed | closed | open (not in any signature) | closed | ~15 lines of polarity definition |
| **C** | Depth bound (X19) + a per-instantiation exclusion for a member exporting a `(T) -> …` slot at function-type substitutions (the mirror of 0133 at one slot, which SIMPLIFY X19's note anticipated) + the mint exclusion | closed | closed at a function-type substitution | open | closed | 3 sentences |
| **D** | Depth bound + the hand-out read as a **source** in the value's set (a lambda with a `T` parameter carries the body's instantiation) + the mint exclusion | closed | closed | closed | closed | 1 provenance clause; but a deferred-activation frame built over such a selector would carry the source, rejecting `return xs.Select(lambda(t: T) => f(t))` in every generic wrapper |
| **E** | **Depth bound (declarations and body-written types) + the value rule for a generic body's own function values + the mint exclusion — adopted** | closed at the declaration or body type | closed in the body (one test site: `Bind`) | closed in the body (zero sites) | closed (typed routes; §10 Q1) | 3 paragraphs replacing 1 |
| **E′** | E plus a declaration-site ban on a bare `T` in a parameter slot of an exported function type (the draft's clause 2) | as E | as E, three test sites | as E | as E | 4 paragraphs |

**E over B.** The polarity definition is a new per-position computation; the depth bound and the value rule serve, and RETRO R2's consolidation of §28.3's function-value text already takes X19 as its one carve-out. B leaves the untyped routes open. **E over C.** C keeps a per-instantiation mechanism where the value rule serves, and leaves the untyped routes open. **E over E′.** The value rule alone carries the hazard: a `(T) -> …` return, field or component is filled either by a value the generic body formed — which clause 2 keeps from it — or by a value formed elsewhere and checked there against the substituted type (a non-generic lambda's function-typed parameter is call-bounded in its own body). E′'s ban rejects the callback-field idiom and three test programs and closes nothing E leaves open. **E over D.** D closes everything by one clause but over-rejects: a selector with a `T` parameter is itself "a function value with `T` in a parameter slot", so a frame built over it would carry the source and could not be returned — the LINQ-wrapper idiom. E reads the restriction off the value's *formation* and names the positions it may not reach, so a frame built over such a selector carries only the selector's captures, and its elements, the selector's results, are never such values (clause 2's lambda-`return` bar). **Clause 4 stays per-instantiation**, not a declaration ban, because `() -> T` returns are the thunk idiom (15 sites) and the hazard exists at the two mint families only.

---

## 3. The rule

### 3.1 §28.3 *Bare-type-parameter returns*: the paragraphs replacing the nested-slot exclusion

The exact text is `W5.json`'s `W5-0165-5763-depth-bound`; in substance:

▲ **A type parameter occurs in a function type at depth one only (derives: K1.6 + K2.4).** In a generic declaration — its signature and declared types (a generic type's fields, properties and member signatures, a generic method's or a generic extension block's member's signature, a generic type alias's expansion read where it is used, §27.8) and every type written in its bodies (a local's or a lambda's annotation, an `as`, `is` or pattern target, a type argument) — a type parameter, bare, nullable or within a type argument, does not occur inside a function type that is itself inside a function type: `reg: ((T) -> ()) -> ()`, `reg: (() -> T) -> ()`, `reg: (() -> ((T) -> ())) -> ()`, `reg: ((Array<T>) -> ()) -> ()`, a field `(((T) -> ()) -> ())?` and a body's `o as ((T) -> ()) -> ()` are compile errors naming this rule. The ground is the pre-substitution keying: at such a shape a caller's function invokes the body's inner function, or the body's inner lambda is invoked by the caller's, with no caller conversion and no slot either side can read. Depth one keeps its rules. The remedies are a function type of depth one or a non-generic interface for the registrar.

▲ **Function values a generic body forms over its type parameters (derives: K2.3 + K2.4).** In a generic body, a function value formed or retyped at a function type that places one of the body's type parameters, bare or nullable, in a parameter slot, at any depth — a lambda literal declaring such a parameter, a method reference formed at such a type, a value converted to one (§27.1.1) — is bound to a local, invoked, captured by a closure, or passed to a function-typed parameter, and reaches no other position: not a `return` or `yield return`, a lambda's included; not a function-typed holder other than a `scoped` field; not a set-erasing position (`Object`, and an argument or store at a position another generic declares at a bare type parameter which the body instantiates with such a function type); and a variant conversion (§12.9) at a type argument that is such a function type is rejected in the body. The restriction is the value's, read off its formation and followed through the locals it is bound to, and adds no source to its set. Declarations are not restricted.

▲ **Exported mint slots (derives: K2.4 + K6.3).** A member whose declared type or signature carries a function type whose return slot (nullability stripped) is a bare type parameter anywhere other than within a plain, `owned` or `scoped` parameter's declared type — a return type (`Make(): () -> T`), a field, a property, a record component, a choice payload, a `ref` or `out` parameter, or a type argument of one (`Array<() -> T>`) — is inadmissible at an instantiation substituting a mint-announcing type (`Task`-family or iterable-family) for that parameter, rejected at the instantiation under §3.6 rules 2/4's per-instantiation pattern. The diagnostic names the sound spellings: a slot declared at the family (`() -> Task<T>`, `() -> IEnumerable<T>`), or the frame exposed through §30.5's or the stored-view property's admitted borrow positions.

▲ The `Task`-family paragraph's closing sentences say that at a **parameter's** slots the iterable family needs no exclusion, and point to the depth bound and *Exported mint slots* for the rest.

### 3.2 The two-routes sentence, row 5, §12.9, Appendix C.3

- *Set-erasing positions*: ▲ "Two routes reach a set-erasing position with no conversion at a caller, and each has its own rule: a method reference formed over one (§28.14's *method-reference formation bar*, which exempts an `out` parameter on the ground above), and a function value a generic body forms at a type with a type parameter in a parameter slot (*Function values a generic body forms over its type parameters*); a type parameter nested two function types deep has no declaration to reach one from (*A type parameter occurs in a function type at depth one only*)."
- Row 5's **Function-type exclusion** cell: ▲ restated as the depth bound, the value rule and the exported-mint exclusion.
- §12.9's parenthetical: ▲ "(at a function-type substitution the body is not re-checked; a function value the body forms or retypes at a type with a type parameter in a parameter slot is bounded by §28.3's *Function values a generic body forms over its type parameters*, so §27.1.1's function-typed-target clause needs no re-check)".
- Appendix C.3: the two bullets restated.

---

## 4. Soundness

**Clause 1.** With no type parameter two function types deep in any type a generic declaration or its bodies write, every function type over `T` the body can see is `(…T…) -> …` or `… -> T` at depth one, and each depth-one slot has a rule: a `T` parameter slot of a *parameter's* function type is the body-to-caller direction (capture-free by the set-erasing induction); a `T` return slot of a parameter's function type is the return-slot arm (set-erasing at a function-type substitution) and the `Task`-family exclusion (mint families); a value the body forms with a `T` parameter slot is clause 2; a `T` return slot of an exported type is clause 3 at the mint families and, at a function-type substitution, the holder and sink rules (a `() -> T` the body returns is a method's function-typed return, capture-free under the sink rule, and its invocation yields a value the body produced — capture-free by induction). The induction's entry list is therefore complete. Body-written types matter: without them a narrowing (`o as ((T) -> ()) -> ()`) re-creates the depth-two shape from a capture-free caller function (§1.4).

**Clause 2.** The hazard needs a function value whose body was checked with a `T` parameter under the class-or-interface assumption to be invoked by code that types that parameter as a concrete function type. Every such value is formed in a generic body (a non-generic lambda's or method's function-typed parameter is call-bounded in its own body). Clause 2 keeps every one inside the forming body's frame: the four admitted uses — a local, an invocation, a capture, a function-typed parameter — export nothing (a plain parameter is call-bounded and a `scoped` one storage-bounded by the callee's signature; a capturing closure whose own type places `T` in a parameter slot is itself such a value, and one whose type does not can only invoke the value with the body's own values); every other position is barred, a lambda's `return` included, so no frame element, task result or selector result is one. Inside the body the value is invoked only with values the body has, capture-free by the induction. A declaration at a `(T) -> …` type therefore never holds one, and needs no rule of its own. Clause 2 is body-local (proposal 0105's classification): it reads the declared type of the lambda, reference or conversion and the body's own flow analysis.

**Clause 3.** The caller cannot see the pre-substitution slot of a function value it obtained from the body; keying it on the substituted type is the only reading the announcement table gives a function-value invocation; so the only sound closure of the typed routes is admissibility, the mechanism the parameter-side exclusion already uses. The untyped route — a capture-free body-formed `() -> T` lend reaching `Object` and narrowed by the caller to `() -> Task` — is not closed by this proposal; §10 Q1 files it.

**Kernel.** Derivations: clause 1 K1.6 + K2.4 (as the exclusion it replaces); clause 2 K2.3 + K2.4 (the positions whose declared type announces no set, and the conversions that reach one); clause 3 K2.4 + K6.3. No new axiom.

---

## 5. Normative edits

The exact `old → new` set is `scratch/v0334/W5.json` (ids `W5-0165-*`): §28.3's `Task`-family paragraph (lead and closing sentences), the nested-slot paragraph (replaced by the three paragraphs of §3.1), row 5's cell, the two-routes sentence, §12.9's parenthetical, Appendix C.3 (two bullets), rationale §4.2 (one dated paragraph), the code-generation reference §4.4 (one sentence) and §7.2 (one pair), `DECLINED.md` (four rows), `README.md` (this proposal's row).

---

## 6. Declined alternatives

| Alternative | Why declined | Reopening bar |
|---|---|---|
| The polarity rule over the member's whole declared type (option B; #5763's and #5762's recommendations) | A new per-position computation where shape rules with three sites serve; it leaves the `Object`, bare-`T` and variance hand-out routes open, which are in the body and not the signature | a measured program that needs a bare type parameter two function types deep |
| A per-instantiation exclusion for a member exporting a `(T) -> …` slot at function-type substitutions (option C; the mirror of 0133's nested-slot exclusion at one slot) | Keeps a per-instantiation mechanism where the value rule serves, and leaves the untyped routes open | —(none stated) |
| A declaration-site ban on a bare type parameter in a parameter slot of a function type outside a parameter's declared type (option E′; the draft's clause 2) | Redundant with the value rule, which alone carries the hazard; it would reject `(T) -> …` callback fields and returns and three test programs | —(none stated) |
| Reading the hand-out as a source in the value's set, carried by every lambda with a `T` parameter (option D) | A deferred-activation frame built over such a selector would carry the source, rejecting `return xs.Select(lambda(t: T) => f(t))` in every generic wrapper | —(none stated) |
| Keeping the nested-slot exclusion beside the depth bound (proposal 0133 §7 Q6's choice) | Redundant once no depth-two shape exists | —(none stated) |

Proposal 0133's rows 2a, 3 and 4 (`DECLINED.md` 206–208) stand: clause 3 is not an instantiation exclusion for `T`-typed fields, not a store ban on bare-`T` values at the declaration, and not a trust residual.

---

## 7. Migration and reach

Measured at `429fd72` with `scratch/v0334/w5work/ft_positions.py` (every function type mentioning a declaration's own type parameter in `tests/` and `docs/`, classified by position), `x19.py`, and `grep` for record components, choice payloads, properties and collection type arguments over such a function type.

- **Newly rejected (clause 1):** zero sites. The critic's scan (`W5crit/ftypes.py`, which spans every function type and tests nesting by span) finds no function type containing a type parameter inside another function type in any gtest program, lit file, either corpus or any document's code (the only hit is proposal 0133's own `Fill(reg: ((T) -> ()) -> ())`); the lane's `x19.py` regex could not see parenthesised parameter lists and is superseded.
- **Newly rejected in a body (clause 2):** one test program, zero documents — `tests/codegen/lambda_generic_capture_1261_test.cpp:152`, `Wrapper<T>.Bind(x: T): (T) -> T` returning `lambda(y: T) => y` (`lambda_generic_funcparam_funcret_spec_27_14_1`, a codegen substitution test at `T = Payload`; its purpose, the function-type return substitution, is kept by returning a `() -> T` thunk, the shape its sibling tests use, and the implementing commit records the flip). The draft's clause 2 also rejected `tests/lit/owned_functype_slot_erasure_1817.bzl:46` and `owned_functype_slot_erasure_control_1817.bzl:24` (`Id(): ((owned T) -> ())?` returning `null`); with that clause dropped both stay admitted. No other generic body returns, stores or converts a lambda or reference with a `T` parameter (the five explicitly `T`-typed lambdas in generic bodies are bound to locals or passed: `generic_method_same_name_tparam_4813`, `xmod_constraint_dispatch_generic_method_5009`), and no lambda in any test returns such a lambda (`=> lambda(` occurs only in non-generic bodies). 379 `(T) -> …` parameters, 2 `scoped` fields (`generic_class_ctor_arg_lambda_3772`) and every declaration at such a type are unaffected (critic's scan `W5crit/ftypes.py`).
- **Newly rejected at an instantiation (clause 3):** zero sites — the 15 `() -> T`/`() -> U` returns (`generic_property_thunk_per_instantiation_5306`, `method_own_type_param_escape_release_5127`, `lambda_generic_capture_1261`, `generic_property_derived_receiver_5316`) and `Cap<U>(l: Array<() -> U>, …)` (`method_own_type_param_array_add_release_5216`) are instantiated at classes and `int`, none at a `Task`- or iterable-family type.
- **Newly admitted:** nothing.
- **Doc-only:** the row-5 cell, the two-routes sentence, §12.9's parenthetical, Appendix C.3.
- **Corpus:** no rule case states the nested-slot exclusion (`manifest.tsv` searched for "nested", "parameter slot of a function type", "((T) -> ())"); `s28_3_0117` (a static reference stored into a bare-`T` field at a function-type instantiation, accept) is unaffected (the caller's store, not the body's). The doc-example corpus is unedited (#1380). Coverage gaps to write after landing: the four depth-two declarations and a body's `o as ((T) -> ()) -> ()` (reject); `Put(): (T) -> ()` returning `lambda(t) => …` (reject at the `return`), the same declaration returning `null` and `record H<T>(F: (T) -> bool)` (accept); `xs.Select(lambda(x: T) => lambda(t: T) => …)` (reject at the inner lambda's return); `_o := lambda(t: T) => …` and `new Keep<(T) -> ()>(lambda(t: T) => …)` (reject at the conversion), a `scoped` field store and a local binding of the same lambda (accept); `Make(): () -> T` named at `Holder<Task>` (reject) and at `Holder<Item>` (accept).
- **Guides:** none teaches a nested function type over a type parameter, a generic registrar or a returned `(T) -> …` closure; no pin moves.

## 8. Spec-ahead-of-impl

One `spec-tracking` issue at landing: (i) clause 1's checks over declarations and body-written types; (ii) clause 2 in the closure analysis (`Ownership_Checks.cpp`'s set-erasing and holder checks, keyed on the lambda's or reference's declared type) and the variance clause in `TypeResolution_Checks.cpp`'s §12.9 check; (iii) the exported-mint instantiation exclusion beside the (also unimplemented) parameter-side `Task` exclusion; (iv) the `lambda_generic_capture_1261` rewrite.

---

## 9. Composition

- W8 (RETRO R2) consolidates §28.3's function-value text and names X19 as its one carve-out; it must take clauses 1–2 and the two-routes sentence as restated here. The anchors this proposal edits (the nested-slot paragraph, the `Task`-family paragraph's lead and close, row 5's cell, the two-routes sentence) are listed in `W5.md` for the composition step.
- W1's option (a)/(a') deletes the holds-borrows ban family; nothing here depends on it.
- Proposal 0164 (this lane) touches §28.14's example only in §28.3's vicinity; no shared anchor.

---

## 10. Questions filed at landing

1. **The untyped route of the exported mint twin.** A generic body's capture-free lambda lending at a bare-`T` return slot (`lambda(b: Box<T>) => b.V`) converted to `Object`, or a collection of such lambdas converted to `Object`, is narrowed by a caller to `(Box<Task>) -> Task` (or a collection of them) and invoked as a mint at `T = Task`. Clause 3 closes the typed routes only; the untyped route predates this proposal (v0.333's inhabitant-side claim) and needs either a value rule like clause 2 for `T` return slots at set-erasing positions or a caller-side rule for a function value narrowed to a mint-announcing return. Filed as a `spec-defect` (title: "§28.3 *Calls mint* (v0.334): a generic body's capture-free bare-`T` lend reaches a caller as a minting `() -> Task` through `Object`").

---
