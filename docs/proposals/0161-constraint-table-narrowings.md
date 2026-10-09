# Proposal 0161 — Three decisions at the constraint table: no `Object` bound, no hashable function type, and the owning family's element rule

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": three narrowings at the constraint table — a bound naming `Object` is a compile error (#5748), a function type is not hashable (§3.6 clause (e); #5756 part 3), and §14.4's underlying-type rule is the owning family's element rule, read nullability-stripped at a concrete argument and at a type parameter (#5749); critique round 1 applied, shared with proposal 0160, which narrowed the owning-family admission to a constraint set that guarantees `class` (a general interface bound admits records) and recorded the `Object`-held function-value hash route as a question; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5748](https://github.com/s8lap/bezel-lang/issues/5748) (`spec-question`: does a bound naming `Object` admit only class types?), the function-value half of [#5756](https://github.com/s8lap/bezel-lang/issues/5756) (`spec-defect`: a function value's `GetHashCode` is undefined though function types are hashable), and [#5749](https://github.com/s8lap/bezel-lang/issues/5749) (`spec-defect`: `where T: class` excludes nullable types, yet `OwningTask<Lock?>` is admitted). Three `spec-tracking` compiler trackers are filed at landing (§7) |
| **Target module(s)** | **spec**: §14.2 (the `where T: SomeClass` row; the argument-containing-a-parameter sentence), §14.4 (the nullable-admission paragraph), §3.6 (the **hashable** row gains clause (e)), §27.7.1 (the advisory sentence becomes the rule), Appendix C.1 (one clause), Appendix C.4 (two bullets). **code-generation reference**: §7.9 (two lines in each half of the pair). **`DECLINED.md`**: three `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | The generics matrix's first run (spec v0.333): root causes RC06 (cells R2–R6, R9, R10 × C4), RC14's function-value half (R6/C5, R6/C12) and RC07 (R7/C8, R7/C13), and `scratch/matrix2/SIMPLIFY.md` items X15 and X14. Each needs a proposal because each changes what compiles: two remove a stated or implied admission (an `Object` bound; a hashable function type), and one admits at a type parameter what §14.2's text rejects (`OwningTask<T?>` under `where T: class`). They share one table and one proposal because each is a one-clause decision about what a constraint admits. |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

**A. No `Object` bound (#5748, X15).** §14.2's table reads `where T: SomeClass` as "Class types only", and §8.1 makes every type derive from `Object`, so `where T: Object` had two readings, one of which (every non-nullable type) falsified §14.2's "`class` is guaranteed … by a class bound" and §22.6's declaration guard. A bound naming `Object` is now a compile error at the declaration; an unconstrained `T`, or `where T: notnull`, says what either reading meant. Zero sites write it.

**B. Function types are not hashable (#5756 part 3, X14).** §27.7.1 defines equality on function values (same method and receiver) and §8.3's only default hash is identity, so two references to one static method were equal and hashed apart; §3.6 counted function types hashable and §27.7.1 merely advised against using them as keys. Clause (e) makes the advice the rule: a function type is not **hashable**; `Set<(int) -> int>`, `Dictionary<(int) -> int, V>`, stdlib §5's hashable block and `where T: hashable` at a function type are rejected at the instantiation site. `Array<(int) -> int>.Contains`, which compares by §27.7.1, is the collection of callbacks. Zero sites write a hash container over a function type.

**C. The owning family's element rule reads the underlying type (#5749).** Every owning generic declares `where T: class`, which §14.2 limits to non-nullable types, while §14.4 and Appendix C.4 admit `OwningTask<Lock?>` by checking "the *underlying* (nullability-stripped) type", and the stdlib signs six such members. §14.4's rule is now stated to be the owning family's element rule, reading the declared `where T: class` nullability-stripped concretely and at a type parameter alike, so `OwningTask<T?>` and `OwningArray<T?>` under `where T: class` are admitted — which the compiler already does, and which one gtest file (`owning_drain_null_element_4796`) already relies on at `IOwningEnumerable<T?> where T: class`.

No new syntax, no new keyword, no new mechanism: three clauses in existing rules.

---

## 1. The problems

### 1.1 #5748: `where T: Object`

§14.2: "`where T: SomeClass` | Non-nullable subtypes of `SomeClass` (the class itself and its descendants). Class types only; interfaces do not extend classes in Bezel." §8.1: "`Object` is the universal root. Every type derives from it" — interfaces, `string`, function types, and value types via boxing. The matrix's counterexample:

```bezel
class C<T> where T: class
end C;
class Box<W> where W: Object
  private _c: C<W>?;            // §14.2: `class` "is guaranteed … by a class bound" — admitted here
end Box;
static class Use
  public static method Run()
    var bx: Box<int>? := null;  // reading B: forms C<int>; nothing re-checks `where T: class`
  end Run;
end Use;
```

Under reading B (every non-nullable type) two statements about "a class bound" become false for `Object`: §14.2's guarantee above, and §22.6's declaration guard ("a class bound (`where T: SomeClass`) qualifies — only class types satisfy it"), so an `owned`/plain pair under `where T: Object` is declarable and collapses at `int`. Under reading A (class types only) `Object` is the one spelling of "a class and nothing else" — a constraint no program in the tree writes. The compiler (`TypeResolution_Checks.cpp` `checkOneConstraint`, the `"type"` arm) defers every bound that is not a user-declared named type, so `where T: Object` admits anything today, silently.

### 1.2 #5756 (function values): a hash with no definition

§27.7.1: "A static (or extension-method) reference to `M` equals every other reference to `M`", and "whether the implementation caches or re-allocates the underlying function object is unobservable". §8.3: "`GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`", and the only default hash a function value has is `Object`'s identity hash (§28.3 names `f.GetHashCode()` as `Object`'s member). §3.6's hashable row excepts (a)–(d) and no function type, so `Set<(int) -> int>` forms:

```bezel
static class Demo
  public static method Twice(n: int): int => n * 2;
  public static method Run()
    var f: (int) -> int := Demo.Twice;
    var g: (int) -> int := Demo.Twice;    // §27.7.1: f = g
    using var s := new Set<(int) -> int>();
    s.Add(f);
    s.Add(g);                             // hashed by identity if re-allocated: Count 1 or 2
  end Run;
end Demo;
```

§27.7.1 says "function values should still not be used as dictionary keys or set elements" — advice, not a rule. The compiler's `isEqualityBearing` returns `true` for every non-named type, function types included.

### 1.3 #5749: the owning family at a nullable argument

§14.2's `class` row: "Non-nullable reference types only". Its argument-containing-a-parameter sentence: "`C<W?>` fails `notnull`, `class`, `value` and every type bound". §14.4: "**Nullable class types and interfaces are admitted.** The owning-type instantiation rule checks the *underlying* type, not the nullable-wrapper. `OwningTask<Lock?>` … is admitted". Appendix C.4: "`OwningTask<T>` is a compile error when T's underlying (nullability-stripped) type is neither a class nor an interface type". Stdlib §6.2, §6.3 and §6.11 declare `OwningTask<Lock?>`, `OwningTask<SemaphoreToken?>`, `OwningTask<LockGuard<T>?>`, `OwningTask<ReadGuard<T>?>` and `OwningTask<WriteGuard<T>?>`.

```bezel
class Res
end Res;
class Host
  public async method Acquire(): OwningTask<Res?>                 // §14.4 / C.4: admitted
    return null;                                                  // §14.2: `class` is non-nullable only
  end Acquire;
  public async method Make<T>(): OwningTask<T?> where T: class    // §14.2: `C<W?>` fails `class`
    return null;
  end Make;
end Host;
```

The concrete form is admitted by one rule and rejected by the declared constraint, and at a type parameter §14.2's sentence rejects outright. The compiler (`TypeResolution_Names.cpp:3130–3185`) implements §14.4's underlying-type reading and names the declared constraint "a slight imprecision", as §14.4 itself does; `tests/codegen/owning_drain_null_element_4796_test.cpp` compiles `CountG<T>(src: IOwningEnumerable<T?>): int where T: class` and `new OwningArray<W?>()` and pins the programs running.

---

## 2. Options

**A. `where T: Object`.**

| | Option | `Keep<T>(x: T) where T: Object` at `5`, `"x"`, an interface | `class Box<W> where W: Object` holding `C<W>` (`where T: class`) |
|---|---|---|---|
| A1 | Reading A: class types only (#5748's recommendation, first half) | rejected | admitted; `Box<int>` rejected at the naming (`int` is no class) |
| A2 | Reading B: every non-nullable type | admitted | admitted; `Box<int>` forms `C<int>` unchecked — unsound against the declared constraint |
| **A3** | **`Object` is not admissible as a bound (adopted; #5748's recommendation, second half; X15)** | the declaration is rejected | the declaration is rejected |

**B. Function-type hashing.**

| | Option | `Set<(int) -> int>` | `where T: hashable` at `(int) -> int` | Text |
|---|---|---|---|---|
| B1 | A function-type row in §8.3 defining `GetHashCode` on §27.7.1's (method, receiver) key | admitted; two references to `M` collide and merge | admitted | a hash definition for closures (identity), method references (method × receiver identity hash) and property references |
| **B2** | **Clause (e): a function type is not hashable (adopted; X14)** | rejected at the instantiation | rejected | one clause; §27.7.1's advice becomes one sentence of rule |
| B3 | Status quo | admitted, `Count` unspecified | admitted | the defect |

**C. The owning family's `where T: class`.**

| | Option | `OwningTask<Lock?>` (stdlib) | `OwningTask<T?> where T: class` in a user body |
|---|---|---|---|
| C1 | §14.2's `class` row governs: nullable arguments rejected | rejected; six stdlib signatures change | rejected |
| **C2** | **§14.4's element rule governs, reading the declared constraint nullability-stripped, concretely and at a type parameter (adopted)** | admitted | admitted, decided at `T`'s substitutions |
| C3 | Drop `where T: class` from the owning declarations; §14.4's rule is their only element rule | admitted | admitted, but a body under an unconstrained `T` would also form `OwningArray<T>` at the declaration, deferring the whole check |

---

## 3. Evaluation

**Soundness.** A2 is the unsound reading (§1.1's `Box<int>`); A1 and A3 are both sound, and A3 keeps §14.2's and §22.6's "class bound" statements true without a second reading. B1 and B2 are both sound at the function type itself; B3 leaves §8.3's invariant unmet at a legal key type. B2 does not reach a function value held at `Object`: a capture-free function value is admitted at a set-erasing position (§28.3), so `Set<Object>` holding two evaluations of `Demo.Twice` still has a `Count` of 1 or 2 (identity hash, §27.7.1 equality through §8.4's `Equals`). B1 would close that route too; B2 is adopted on simplicity and zero reach, and the `Object` route is filed as a residual question (`W2.critic.md`) rather than decided here. C2 is the rule the compiler enforces and the stdlib relies on; C1 is sound but breaks six signatures; C3 widens what an unconstrained body may form.

**Simplicity (the standing preference: narrow when reach is negligible, keep and fix where real code uses the feature).** A3 is one clause and removes a question; A1 is one clause and adds a constraint nobody writes. B2 is one clause; B1 is a row with three sub-cases and a key nobody uses. C2 is one paragraph sentence stating which rule the declared constraint is read by; real code (the stdlib, one gtest file, rule case `s14_4_0006`) uses the nullable form, so the feature is kept and the rule fixed.

**Consistency.** A3: `where T: notnull` already spells "every non-nullable type", and an unconstrained `T` spells "everything", so nothing is lost. B2: function types are already inadmissible as auto-hashable components (§9.2's admissible-component rule; lit case `hashable_constraint_2087__valuelike_inadmissible_payload_choice_rejected_35_7` pins a choice with a function payload as not hashable), so (e) makes the direct and the component readings agree; `-=` on event sources and `Array<(T) -> ()>.Contains` keep §27.7.1's first-kind equality and are untouched. C2: the owning family's class-only and runtime-managed exclusions are already "stated instantiation-site rules, carried through chains" (matrix R10/C12), and reading the nullability strip the same way puts the whole element rule in one place.

**Relation to recorded declines.** None of the three contradicts a `DECLINED.md` row or a rationale decline (searched: "Object" bounds, function-type hashing, nullable owning arguments — no rows). `DECLINED.md` row 207 (proposal 0133's Option 3, rejecting bare-type-parameter stores unless the constraints exclude function types) notes that `where T: class` "admits function types", which C2 does not change.

**Migration reach** (measured, §6): zero sites for A3 and B2; for C2 the one gtest file that forms `OwningArray<W?>`/`IOwningEnumerable<T?>` at a type parameter stays admitted (it is the evidence for C2, and under the literal §14.2 sentence it would have been a spec-ahead-of-impl rejection).

---

## 4. Decision

**A3, B2 and C2.** Under the owner's 2026-10-08 delegation and the 2026-10-09 simplification approval these are accepted as a delegated ruling and graduate in v0.334.

- **A3.** A type bound naming `Object` (§8.1) is a compile error at the declaration; the remedies are an unconstrained parameter and `where T: notnull`. §14.2's "`class` is guaranteed … by a class bound" and §22.6's guard are true as written.
- **B2.** A function type is not **hashable** (§3.6 clause (e)): its `GetHashCode` is `Object`'s identity hash and need not agree with §27.7.1's equality. A hash container, stdlib §5's hashable block and `where T: hashable` at a function type are rejected at the instantiation site. `f.GetHashCode()` as a direct call is unchanged (§28.3's receiver sentence), and so is §27.7.1's equality.
- **C2.** §14.4's underlying-type rule is the element rule of the owning types it lists; their declared `where T: class` is read by it, nullability stripped, concretely and at a type parameter. `OwningTask<T?>`/`OwningArray<T?>` in a generic body is admitted at the declaration exactly where `OwningArray<T>` is — under a constraint set that guarantees `class` (§14.2: `where T: class`, a class bound, a bound naming `IDisposable` or an interface extending it) — and decided at `T`'s substitutions (§3.6 rule 2); under any other constraint set, an unconstrained `T` or a general interface bound included, both are rejected at the declaration. (Critic fix: the draft said "or under a class or interface bound", but a general interface bound admits records and does not guarantee `class`, so that wording would have admitted `OwningArray<T?>` where `OwningArray<T>` is rejected.) §14.2's `C<W?>` sentence names the exception.

---

## 5. The exact normative edits (▲)

The machine-actionable set is `scratch/v0334/W2.json` (ids prefixed `W2/0161`).

1. **Spec §14.2**, the `where T: SomeClass` row: ▲ "A bound naming `Object` (§8.1) is a compile error at the declaration: write an unconstrained `T`, or `where T: notnull`."
2. **Spec §3.6**, the **hashable** row (shared edit with proposal 0160's (d)): ▲ "and (e) a function type, whose `GetHashCode` is `Object`'s identity hash and need not agree with its equality (§27.7.1)".
3. **Spec §27.7.1**: "The practical consequence: function values should still not be used as dictionary keys or set elements, but event unsubscription is deterministic for named methods:" becomes ▲ "A function value's `GetHashCode` is `Object`'s identity hash (§8.3), assigned per function object, which need not agree with the equality above, so a function type is not **hashable** (§3.6, clause (e)): a `Set<(int) -> int>`, a `Dictionary<(int) -> int, V>`, stdlib §5's hashable block and `where T: hashable` at a function type are rejected at the instantiation site, and a collection of callbacks is an `Array<(int) -> int>`, whose `Contains` and `Remove` compare by this equality (stdlib §4). Event unsubscription is deterministic for named methods:"
4. **Spec §14.4**, after "`OwningTask<Lock?>` … is admitted because `Lock` is a class type; the nullability of the result is independent of the disposal obligation.": ▲ "This rule, not §14.2's `class` row, is the element rule of the owning types the paragraph above lists: the `where T: class` their declarations carry is read by it, nullability stripped, concretely and at a type parameter alike. So `OwningTask<T?>` or `OwningArray<T?>` formed in a generic body is admitted at the declaration exactly where `OwningArray<T>` is — under a constraint set that guarantees `class` (§14.2: `where T: class`, a class bound, a bound naming `IDisposable` or an interface extending it) — and decided at `T`'s substitutions as every instantiation-site rule is (§3.6 rule 2); under any other constraint set, an unconstrained `T` included, both are rejected at the declaration."
5. **Spec §14.2**, the argument-containing-a-parameter sentence: "`C<W?>` fails `notnull`, `class`, `value` and every type bound" gains ▲ "(the owning family's `where T: class`, which §14.4's element rule reads nullability-stripped, excepted)".
6. **Spec Appendix C.1**, the inline-array hashable bullet: ▲ "; nor is a function type, whose `GetHashCode` is the identity hash (§3.6 clause (e), §27.7.1)."
7. **Spec Appendix C.4**: the type-bound bullet gains ▲ "A bound naming `Object` is rejected at the declaration (§14.2)."; the owning-types bullet gains ▲ ", read nullability-stripped by §14.4's element rule."
8. **Code-generation reference §7.9**: the rejected block gains `var cbs := new Set<(int) -> int>();   // ERROR …` and `public static method Keep<T>(x: T): int where T: Object => 1;   // ERROR …`; the accepted block gains the `Array<(int) -> int>` + `Contains` spelling and `where T: notnull`.
9. **`DECLINED.md`**: three rows (§9).

Not edited: stdlib §4 and §6's owning declarations (they keep `where T: class`, now read by §14.4); §14.2's `class` row; §28.3's receiver sentence naming `f.GetHashCode()`.

---

## 6. Measurement

Fresh scans at `429fd72` over `tests/` (gtest, lit, conformance, rule corpus) and `docs/`, cross-checked against `scratch/matrix2/simplify_work/reach_out/x14_set_of_function.txt` and `x15_where_object.txt`.

| Decision | Pattern | Sites | Verdict under the decision |
|---|---|---|---|
| A3 | `where <id>: Object` outside comments | **0** (the six gtest hits are `// … where o: Object` comments; the lit, conformance and doc hits are `where T: class`/`sendable`/`new()` lines whose text happens to continue with `Object`) | nothing to flip |
| B2 | `Set<(`, `Dictionary<(`, `OwningSet<(`, `Counter<(`, `GroupBy`/`ToDictionary` keyed by a function type, `where T: hashable` at a function type | **0** (`SortedSet<() -> int>` in `builtin_collection_store_callee_3696` is an ordered set, keyed on `IComparable`, not hashable — untouched; the lit case with a function payload already expects rejection) | nothing to flip |
| C2 | `Owning*<P?>`/`IOwning*<P?>` at a type parameter `P` | **1 gtest file**, `owning_drain_null_element_4796_test.cpp` (`CountG<T>(src: IOwningEnumerable<T?>) where T: class`; `new OwningArray<W?>()` at a concrete `W`), pinned running | stays admitted (C1 would reject it; the literal §14.2 sentence already did on paper) |
| C2 | concrete `OwningTask<X?>` | 6 stdlib signatures; 15 test sites in 9 files; rule case `s14_4_0006` (pass) | unchanged |

**Newly rejected: none measured** (A3, B2). **Newly admitted: none that was not already compiled** (C2 admits on paper what the compiler and one test already do). No banked baseline verdict moves.

---

## 7. Compiler facts and the trackers

- **A3.** `TypeResolution_Checks.cpp` `checkOneConstraint`, `"type"` arm: a bound that is not a user-declared named type is deferred ("Built-in bounds/args carry no base-type chain here; defer"), so `where T: Object` admits every argument today. The #4689 declaration-level pass (the same file, ≈2029) already rejects record, enum and choice bounds at the declaration; **tracker**: reject a bound resolving to `Object` there, naming the two remedies. Regression row: `Keep<T>(x: T) where T: Object` rejected at the declaration.
- **B2.** `SymbolCollection.cpp` `isEqualityBearing` returns `true` for every non-named type; **tracker**: return `false` for a function type, and give `hashableFailureComponentPath` a reason clause naming §27.7.1 and the `Array<…>.Contains` spelling. Regression rows: `new Set<(int) -> int>()`, `new Dictionary<(int) -> int, int>()`, `Hold<(int) -> int>()` under `where T: hashable`, each rejected; `Array<(int) -> int>.Contains(Demo.Twice)` admitted and `True`.
- **C2.** `TypeResolution_Names.cpp:3130–3185`: the owning family's purpose-built check reads the underlying type (implemented); the generic-body face is exercised by `owning_drain_null_element_4796`. **Tracker** (spec-ahead-of-impl is nil here; file as a conformance note or fold into the rule-corpus sweep): rule-corpus cases below.

---

## 8. Corpus cases to write (after landing)

- `s14_2_*` reject: `where T: Object` at a method and at a class declaration.
- `s3_6_*` reject: `Set<(int) -> int>`; `s27_7_1_*` run: `Array<(int) -> int>` with `Contains(Demo.Twice)` → `True`.
- `s14_2_*` reject: `where T: hashable` instantiated at a function type.
- `s14_4_*` accept/run: `Make<T>(): OwningTask<T?> where T: class` at a class `T`; reject: the same under an unconstrained `T` and under a general interface bound `where T: IShape` (a record may implement it); reject: `Make<string>` (runtime-managed, carried through the chain).

---

## 9. Declined alternatives (one `declined-at-proposal` row each)

1. **A1/A2, a type bound naming `Object` read as class-only or as every non-nullable type.** A1 would make `Object` the only spelling of "class types, no interface, `string` or function type", a constraint no program writes; A2 falsifies §14.2's class-bound guarantee and §22.6's guard. *Reopening bar:* a measured program that needs a class-only bound; a proposal naming the spelling.
2. **B1, a function-type row in §8.3.** It defines a hash for a key nobody uses, a lambda's hash would still be identity (so a set of lambdas would be legal and useless), and §27.7.1 already advised against the use. *Reopening bar:* a measured program keying a hash container on method references.
3. **C1, the strict reading of the owning family's `where T: class`.** Six stdlib signatures, fifteen test sites and a rule case write the nullable form, and §14.4 already states the underlying-type rule. *Reopening bar:* none stated.

---

## 10. Self-critique (adversarial pass)

1. **Why three decisions in one proposal?** Each is a one-clause decision about what a constraint admits, each has negligible reach, and three proposal files for ≈150 words of normative text would be ceremony. A critic who wants them separable can split the JSON by id prefix (`X15`, `X14`, `RC07`): the edits are disjoint.
2. **Does A3 break `where T: U` with an unconstrained `U`?** No: that is a type-parameter bound, which "contributes only `Object`'s surface" by §14.2's own paragraph; the rejection is of a bound that *names* `Object`.
3. **Does B2 reach events?** `EventSource<T>.Remove` and `-=` match by §27.7.1's equality, not by hashing (§27.7.1, §17.2); untouched. `OwningBuffer<(int) -> int>` was already rejected (§14.4). A record with a function-typed component was already not auto-hashable (§9.2).
4. **Is C2 a design change or a ruling?** Two texts contradict (§14.2 vs §14.4/C.4) and the decision picks one — a ruling's shape; but at a type parameter it admits what §14.2's sentence literally rejects, so under process §2's stricter-gate rule it is drafted as a design change. It changes no compiled verdict (§6).
5. **Does C2 leak `string?` into the owning family at a type parameter?** `where T: class` admits `string`; `OwningTask<string?>` is rejected concretely by the runtime-managed exclusion, which is a stated instantiation-site rule carried through chains (matrix R10/C12), so `Make<string>` is rejected at that naming, as `OwningArray<T>` at `string` already is.
6. **Composition.** §14.2's "A type parameter as the type argument" paragraph is W5's territory (C9, X19); this proposal edits one parenthetical inside its `C<W?>` sentence and names that in the lane md. §3.6's hashable row is shared with 0160 and RC12 (one JSON edit carries (d) and (e)).
