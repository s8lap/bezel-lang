# Proposal 0022 (RFC) — the test framework: an `Assert` library, an ownership-as-fixture lifecycle, and the discovery fork

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Tracking issue** | [#1453](https://github.com/s8lap/bezel-lang/issues/1453) |
| **Target module(s)** | A new `System.Testing` module — **explicitly imported, deliberately *not* auto-imported** (§5.6). Builds on `System` (`Exception`/`ExceptionInfo` §31.4, `Type` stdlib §2.13, `Task` §30) |
| **Written against** | Language spec **v0.177**; stdlib companion v0.165 |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-08-11 to v0.246: `hashable` vocabulary (v0.242; formerly *equality-bearing*), the stdlib §4.8 quotation re-derived from the landed text. §5.1/§10's "not a user-writable `where` clause" premise (expired at v0.214) is **resolved**: `Assert.Equal<T>`'s eligibility is now the ordinary `where T: hashable` clause, acceptance ruling 2 superseded under the owner's 2026-08-12 direction on the recorded flags — see "Delegated ruling 2026-08-12" below. Refreshed 2026-09-17 to v0.306: §5.5's auto-import parenthetical conformed to §2.3's current list (`System.Collections.Linq` auto-imported since v0.278, `System.Collections.Generic` since v0.289; `System.Testing`'s explicit-import argument is unaffected). The §28.14 call-bounded/`scoped` quotations and §9.2's `Object`-receiver quotation re-verified against the landed text. No tier-2 flags. Written-against pin unchanged. Corrected 2026-10-07 at v0.317 ([#5483](https://github.com/s8lap/bezel-lang/issues/5483)): §5.1's and §11 item 2's ground for declining `Equal(Object, Object)` restated against spec §9.2 as of v0.316. The identity fallthrough belongs to records and choices that are not auto-hashable, not to every record; the decline and its conclusion are unchanged. |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #24 (P1, diagnostics), wave D3 of the [stdlib gap DESIGN plan](stdlib-gap-design-plan-2026-07.md) — the one wave-D3 item flagged "numbered proposal (RFC-track) … the `test` modifier is a language change — needs the RFC discipline, owner ruling." |
| **Related** | [#1398](https://github.com/s8lap/bezel-lang/issues/1398) (the future build driver — the natural test *runner* host); [#1419](https://github.com/s8lap/bezel-lang/issues/1419)/§28.14 (call-bounded closures — `Assert.Throws`'s shape); gap item #23 (`Verify`/`Unreachable`/`Debug.Fail` — the *sibling* diagnostics item, and the release-elidable `Debug.Assert` §8.2 this proposal is careful **not** to be) |

## Rulings recorded at acceptance

Resolving §10's open questions under delegated ruling authority (2026-07-07; owner review pending). The recommendations were the proposal's own; the reasoning is one line each.

1. **Discovery fork (Q1) → approve (a) the `test` modifier as the discovery *endpoint*; ship the `Assert` library + (b) registration runner immediately; decline (c).** (b)'s silent-no-run is the CodingKeys footgun rationale §5.19 already ruled decisive, and (a) makes it structurally impossible in the identical per-declaration/total/closed/attribute-forward-compatible way `serialized` does — §18 sanctions exactly this narrow-modifier shape. The `Assert` library + runner (§5–§8) carry no language change and graduate now through the ordinary docs workflow; (a)'s grammar delta (§3.1) follows on the spec-amendment track, gated on the §3.4 evidence trigger. (c) is declined outright (stringly-typed, blocked on #1398, reintroduces the intentional-vs-accidental blur).
2. **`Assert.Equal` eligibility (Q2) → compiler-known, carrying the hashable derived-type-property constraint** — full §8.3 coverage and the actionable §9.2 use-site diagnostic, at parity with `Dictionary<K,V>`'s `K` and `HashCode.Add<T>`'s `T`; the pure-library `where T: IEquatable<T>` fallback is declined for excluding enums and un-declared auto-equality records. *(Superseded 2026-08-12 — the ruling's stated premise expired at v0.214; the surface is now the ordinary `where T: hashable` clause, the goal delivered unchanged. See "Delegated ruling 2026-08-12" below; the `IEquatable` decline stands.)*
3. **`Throws` return (Q4) → `ExceptionInfo` snapshot**, consistent with §31.1 (the live caught instance cannot escape the arm), letting a test go on to assert on the message. (The exact/subtype discrimination additionally depends on the catch-generic-type-parameter ruling and #1505 — see §5.4.)
4. **Module name (Q5) → `System.Testing`, explicit-import** — the shorter name; keeps the production namespace clean and marks test files.
5. **`test` method shape (Q3) → settled when (a) is implemented on the spec track; this proposal's lean stands: static, parameterless, `()`-or-`Task`-returning, non-`virtual`, fixtures via inner `using var` (§7).** A `test skip` sub-clause is deferred until (a) itself lands.

## Delegated rulings 2026-08-11 (stdlib-wave readiness; owner review in repo)

Under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo), three additions to the acceptance record above — the fork ruling itself is unchanged and stands as FINAL ((a) approved as the endpoint, (b) ships now, (c) declined).

1. **The (a)-promotion evidence gate is sharpened into an exact bar**, so the fork can never re-open as a vague question and no implementation lane stalls wondering whether the trigger has fired. The §3.1 grammar delta lands when **either** of these occurs, whichever is first — both recorded on [#1453](https://github.com/s8lap/bezel-lang/issues/1453) when observed:
   - **(i) The first documented silent-no-run incident** in a real Bezel suite: a test method present in source but absent from the (b) registration list, discovered to have masked a defect or found stale in review. One incident suffices — the fork ruling already deemed this footgun decisive in principle; the gate exists only to demand one observation before paying the grammar change.
   - **(ii) Registration outgrows review**: a first-party `.bzl` suite whose `Add` list a reviewer can no longer verify against the class surface in one sitting (as a working figure, on the order of a hundred registrations in one suite) — the point where the mitigation the (b)-permanent world relies on ("one `Add` per test, reviewed") stops being real.
   Until then, (b) is the shipped surface and the `Assert` library graduates independently, exactly as ruled at acceptance.
2. **The §5.4 spec dependency is discharged.** The catch-generic-type-parameter question this proposal recorded as in-flight has **landed**: the current spec (v0.247) states that in a generic body a type parameter may be used as a pattern type, testing against the substituted argument per instantiation, "the same rule as catch arms (§31.2.1)" — so `Throws<TException>`'s `catch … var ex: TException` arms and the exact/subtype split are design-unblocked. [#1505](https://github.com/s8lap/bezel-lang/issues/1505) (wrong-arm dispatch) remains implementation catch-up only and gates nothing at the design level.
3. **Cross-reference, deliberately not resolved here:** the Q2 eligibility mechanism's stated premise ("not a user-writable `where` clause", §5.1/§10) expired at v0.214 — `where T: hashable` is §14.2-admitted — and the resulting re-cut of the `Assert.Equal` mechanism is **owner-queued** as the [2026-08-11 currency paper](proposals-currency-2026-08-11.md) §3 flag 2. Nothing in this section depends on which way that goes: the fork ruling, the (b) runner, and the gate above are constraint-mechanism-independent, and 0053's citation follows the owner's re-cut when it lands. *(Resolved 2026-08-12 — the re-cut landed; see the delegated ruling below.)*

## Delegated ruling 2026-08-12 (currency flag 2 — the `Assert.Equal` eligibility re-cut)

Recorded under the owner's 2026-08-12 direction on the recorded flags ([2026-08-11 currency paper](proposals-currency-2026-08-11.md) §3 flag 2; the third option as recommended). **Acceptance ruling 2 is superseded — its premise expired, not its judgment overturned.** The 2026-07-07 ruling chose between (i) compiler-known enforcement and (ii) a pure-library `where T: IEquatable<T>`, and stood on the stated premise that the equality-bearing constraint was "not a user-writable `where` clause" (§14.2 at the v0.177 pin). That premise was true when recorded and was **falsified at v0.214**, when `where T: hashable` joined §14.2's derived-property constraints (v0.242 then renamed the property to match the keyword). A third option therefore exists that is strictly better than both horns, and it is the ruled surface:

```bezel
public static method Equal<T>(expected: T, actual: T) where T: hashable;
public static method NotEqual<T>(expected: T, actual: T) where T: hashable;
```

— **ordinary generic code, no compiler-known machinery**: full §8.3 coverage (everything the 2026-07-07 ruling wanted from horn (i)), the §14.2 instantiation-site diagnostic naming the constraint, the offending type argument, and the structural reason as a component path, and the exact spelling the stdlib already ships on `Counter<K>` (`where K: hashable`, stdlib §4.20, v0.215). What the supersession **preserves**: the ruling's goal — hashable-property coverage with the actionable diagnostic — is delivered unchanged, and the `where T: IEquatable<T>` decline (§5.1, §11 item 3) stands on its unchanged grounds (excludes enums and un-declared auto-equality records). What it **retires**: `Assert` leaves the compiler-known-member roster — the parity argument now runs through the *constraint vocabulary* rather than through compiler-known machinery (`Dictionary`/`Set` keep their structural §3.6 rule-2 key checks; `Counter<K>` is the written-clause precedent; `HashCode.Add<T>`'s eligibility rule, 0008 §5.4, remains genuinely non-`where`-writable and keeps its compiler-known scoping — as does 0053's *generatable*, whose citation of this mechanism is conformed in that proposal). §5.1's signature sketch and §10 Q2 are conformed in place; the original two-horn text stands in the acceptance record as history.

## 1. What is missing, and the shape of the answer

Bezel has no way to write a test. There is `Debug.Assert(condition)` (stdlib §8.2), but that is a *release-elidable runtime invariant check* — "calls to `Debug.*` may be elided in release builds … side effects in `Debug.Assert` arguments are unspecified." A test assertion is the opposite: it must **never** be elided, must produce a structured pass/fail record, and must render *why* it failed. The conformance harness (`tests/conformance/run_conformance.sh`) tests the *compiler* by compiling `.bzl` cases and diffing output; it is not a way for a Bezel *program* to test itself. Nothing in the language or stdlib lets a `.bzl` library ship its own unit tests.

This proposal has **two cleanly separable parts**, and the separation is the most important structural fact about it:

1. **An `Assert` library and a library test-runner (§5–§8) — no language change, ships regardless of anything below.** It is buildable *today* on machinery the language already has: `=`/§9.2 auto-equality (the `Assert.Equal` foundation), §28.14/v0.177 **call-bounded closures** (`Assert.Throws(lambda() => …)`), §27.6 method references (the runner's registration), §31 typed exceptions (assertion failures as typed faults), and — the design's centerpiece — §28.10 `using var` and §28.4 destructors (fixture setup/teardown *as ownership*, with no `SetUp`/`TearDown` and no attributes).

2. **The discovery fork (§3) — an RFC.** *How the runner finds the tests* is the one question with a language-change option on the table (a `test` modifier). This is reserved for an owner ruling; the proposal presents all three candidates and recommends one, arguing entirely from the project's own recorded reasoning.

The load-bearing observation that makes the split clean: **the runner (part 1) is a prerequisite of every fork in part 2.** A `test` modifier still needs something to *run* the collected methods; a registration API *is* that runner; a naming convention needs it too. So part 1 is built now under any outcome, and the fork ruling only decides how tests reach the runner's input.

## 2. Trace against the spec — what already works

Before proposing anything, confirm the substrate exists (CLAUDE.md rule 3, trace-before-writing):

- **Equality.** `Assert.Equal(a, b)` needs value equality. §9.2 gives body records auto-`=` over their auto-properties; §8.3 defines the **hashable** derived type property; primitives/enums/`string` compare intrinsically. A record that is *not* auto-hashable makes `=`/`.Equals` a **compile error at the use site** (§9.2) — that error is the design, not a gap (§5.1).
- **Typed faults.** §31.4 gives the `Exception` surface (`Message`, `StackTrace`, `CauseChain`) and shows user subclasses chaining through `base(msg)`. An `AssertionException: Exception` is an ordinary user-style subclass (§5.4).
- **Call-bounded closures.** §28.14 (v0.177, #1419): "A function-typed parameter declared *without* `scoped` is **call-bounded** … the caller side is correspondingly unrestricted: **any** closure — capture-bearing or not — may be passed." `Assert.Throws(lambda() => account.Withdraw(over))` is the canonical call-bounded shape: the callee invokes the closure during the call and never retains it, so the caller's captures are trivially in-lifetime.
- **Method references.** §27.6: `greeter.Greet` (bound instance), `int.Parse` (static), `MyTests.ParsesEmpty` (a static reference). The registration runner takes exactly these.
- **Ownership as lifecycle.** §28.10 `using var` disposes "on every exit path: normal completion, `break`, `return`, or exception"; §28.4 destructors run deterministically; §22.9 makes owned fields exception-safe. This is teardown-that-cannot-be-forgotten (§7).
- **Domains.** §30.13: a domain is serial; continuations interleave only at `await`. A serial awaited sequence of async tests in one domain is deterministic (§6).

Everything the `Assert` library and runner stand on is present. Only the discovery mechanism (§3) has a piece the language lacks.

## 3. The discovery fork (RFC — owner ruling required)

*How does the runner learn which methods are tests?* Three candidates; the recommendation and its reasoning follow.

### 3.1 (a) A `test` modifier — a language change

```bezel
public class ParserTests
  public test method ParsesEmptyInput()
    using var p := new Parser("");
    Assert.True(p.Parse().IsEmpty);
  end ParsesEmptyInput;
end ParserTests;
```

The compiler recognizes `test` and **collects** every `test` method into a runtime registry the runner enumerates. Zero per-test boilerplate; adding a `test` method *is* adding a test — forgetting to register one is structurally impossible.

**Cost — grammar + spec.** `MethodDecl` today is `Visibility "static"? "async"? VirtualKind? "method" …` (Appendix A). `test` is a **new closed production** on that rule — a *contextual keyword* (like `serialized`, so existing identifiers named `test` are unaffected), plus normative rules: a `test` method is parameterless, returns `()` or `Task` (§6), is not `abstract`/`virtual`, and is collected into the registry. This is the same **"one closed grammar production"** footprint the `serialized` clause cost (rationale §5.19, stdlib §9.8), no more.

**Cost — it prejudges the attribute question.** A `test` modifier is a single-purpose annotation on a declaration. That is *precisely* the territory rationale §18 defers ("test frameworks" is named there, verbatim, among the attribute use-cases). Adding a `test` modifier commits to a narrow modifier where a general `[Test]` attribute would one day be the uniform answer.

**The honest precedent — `serialized` (§18 sanctions narrow modifiers).** §18 does not forbid narrow compiler-recognized modifiers; it sanctions them where an attribute system "would be imported wholesale" for one need. The `serialized` clause is the live example: rationale §5.19 chose "the narrow-modifier escape hatch §18 already sanctions for `[Obsolete]`/`[MethodImpl]`-style needs" over both an attribute system and Swift's `CodingKeys`. A `test` modifier is structurally the same kind of object: **per-declaration, total, closed** (no parameters, no reflection), and **forward-compatible** — `test method` re-expresses mechanically as `[Test]` if a general attribute system ever lands, the identical promotion discipline §5.19 uses for `serialized "x"` → `[Serialized("x")]` and `FromCollectionLiteral` → an interface method.

But the precedent must be cited *honestly*, and the honest reading cuts one way against (a): `serialized` earned its grammar because field-name customization **had no zero-language alternative** — "renaming a serialized field … has no auto-equality analog, so it required a new mechanism" (§5.19). Test discovery is different: option (b) below **is** a complete zero-language mechanism that works today. So the `serialized` precedent establishes that (a) is *admissible*, not that it is *forced*.

### 3.2 (b) A registration API — zero language change, works today

```bezel
// The test main — explicit, no compiler support.
public static method Main(): Task<int>
  using var runner := new TestRunner();   // `using var` so a capture-bearing body may be Add'd (§28.14 bounding object)
  runner.Add("ParsesEmptyInput", ParserTests.ParsesEmptyInput);   // §27.6 method reference (capture-free — admitted regardless)
  runner.Add("ParsesWhitespace",  ParserTests.ParsesWhitespace);
  return runner.RunAsync();
end Main;
```

`runner.Add(name, ParserTests.ParsesEmptyInput)` passes a §27.6 method reference (a `() -> ()` or `() -> Task` function value). This compiles against v0.177 *unchanged*; it is the runner every other fork also needs.

**Cost — the silent no-run footgun.** Boilerplate scales linearly with the test count, and — decisively — **a test you write but forget to `Add` is silently never run.** That is *exactly* the CodingKeys footgun the project already ruled decisive. Rationale §5.19 indicts Swift's separate-list `CodingKeys` because "adding a member and forgetting its case **silently drops it** … and a reader cannot distinguish intentional exclusion from an omission." A registration list is structurally the same separate list: a method present in the class but absent from the `Add` calls is silently excluded, indistinguishable from a deliberately-disabled test. By the project's own recorded reasoning, this is the failure mode the `serialized` design was built to make *structurally impossible* — and (a)'s per-method `test` modifier reproduces that structural impossibility, while (b) reintroduces the footgun.

### 3.3 (c) Naming convention + build-driver discovery — a tooling change

Methods named `Test*` (or in `*Tests` classes) discovered by the future build driver (#1398). No boilerplate, no language change — but three defects: it is **stringly-typed** (discovery keys off a name, with no compile-time check that a `Test*` method has the right shape — the exact compile-time-to-runtime regression DECLINED D-0001 ground 2 argues against, "a constructor parameter fails at compile time"); it **reintroduces the intentional-vs-accidental blur** (a `Test*`-named helper is accidentally enrolled; a mistyped `Tst*` is silently skipped — §5.19's blur, relocated to a naming rule); and it is **blocked on a tool that does not exist** — #1398 is an unimplemented build-driver layer, and (c) cannot ship before it.

### 3.4 Recommendation

**Build (b) now; recommend the owner approve (a) as the discovery endpoint; decline (c).**

The recommendation is a *sequenced pair*, not a contested fork, because (b) is a hard prerequisite of (a) — the `test` modifier is inert without a runner, and (b) *is* that runner. So:

- **v1, no ruling needed: ship the `Assert` library (§5) + the (b) registration runner (§8).** These graduate through the ordinary docs workflow. Every fork consumes this; none of it is wasted under any outcome.
- **Recommended endpoint, pending owner ruling: (a) the `test` modifier.** The load-bearing argument is the project's own: (b)'s **silent no-run** is the CodingKeys footgun §5.19 calls decisive, and (a) makes it structurally impossible in the identical way the `serialized` modifier does — per-declaration, total, closed, attribute-forward-compatible; §18 sanctions exactly this narrow-modifier shape.
- **The evidence bar is respected, not waved past.** Because (b) is a *working* zero-language alternative (unlike the `serialized` case, where none existed), rationale §18's evidence bar for a language change applies: **adopt (b), and promote to (a) once the silent-no-run cost is observed in real suites** — the same "evidence from real code, not re-asking" bar DECLINED D-0002 sets for tuples/anonymous types. This keeps the language change decline-with-reasons-compatible: it is recommended *with a stated trigger*, not asserted by taste.
- **Decline (c)** outright: stringly-typed (no compile-time shape check), blocked on #1398, and — even after #1398 lands — it reintroduces the intentional-vs-accidental blur that both (a) and (b)-with-discipline avoid.

If the owner rules **for (a)**, §3.1's grammar delta and collection semantics are the spec change; the (b) runner stays as the execution engine (its `Add` calls become compiler-emitted from the registry — see §8.4). If the owner rules **against (a)**, (b) is the permanent surface and the footgun is mitigated by convention (one `Add` per test, reviewed) rather than by the compiler.

## 4. The `Assert` library ships regardless — the rest of this proposal

Everything from here (§5–§8) is part 1: **no language change, no ruling.** It is the concrete design the gap review's "Assert library" names.

## 5. The `Assert` static class (design question 1)

`Assert` is a static class in `System.Testing`. Failures are a typed `AssertionException` (§5.4). Assertions **never** elide (unlike `Debug.Assert` §8.2 — §5.5 draws the line).

### 5.1 `Equal` — built on §9.2 equality, with the §9.2 eligibility error

`Assert.Equal` compares by value. Its `T` must be **hashable** (§3.6/§8.3) — and a type *without* value equality is a **compile error**, exactly as the gap-review brief anticipated ("what happens for types WITHOUT auto-equality: compile error like `HashCode`'s eligibility, cite 0008's precedent").

**The precedent is already in the stdlib, twice over.** `Dictionary<K, V>` requires `K` **hashable**, "enforced at the instantiation site per §3.6 rule 2: a `Dictionary<K, V>` (or `OwningDictionary<K, V>`) whose `K` is a record that is neither auto-hashable nor declares user equality is rejected with a diagnostic naming the property" (stdlib §4.8). `HashCode.Add<T>` (proposal [0008 §5.4](0008-integer-toolkit.md)) requires `T` "hashable under §8.3 … the *same compile error* §9.2 already raises." `Assert.Equal<T>` is the third instance of the identical rule — **`T` hashable, checked per instantiation, a non-auto-equality record rejected with the diagnostic pointing to the same fix** (declare custom equality, or simplify to an auto-equality form) — and, per the 2026-08-12 delegated ruling, states it as the written `where T: hashable` clause (§14.2, v0.214; `Counter<K>`'s spelling, stdlib §4.20) rather than as compiler-known machinery.

```bezel
public static class Assert
  // T hashable: the ordinary §14.2 derived-property constraint (v0.214; Counter<K>'s spelling,
  // stdlib §4.20) — a non-hashable type argument is rejected at the instantiation site naming
  // the constraint, the offending type, and the structural reason (delegated ruling 2026-08-12).
  public static method Equal<T>(expected: T, actual: T) where T: hashable;
  public static method NotEqual<T>(expected: T, actual: T) where T: hashable;
  // ... (True/False/Null/NotNull/Throws/Fail below)
end Assert;
```

On mismatch, `Equal` throws `AssertionException` carrying the **rendered** expected and actual (§5.3).

**Why not `where T: IEquatable<T>`, and why not `Object`.** Two spellings that need no compiler support were considered and **declined** (§11):

- `where T: IEquatable<T>` — the pure-library form, but it **excludes** enums (whose `IEquatable` conformance is *not* established by §2.9 — the same exclusion 0008 §5.4 records) and auto-equality records that do not *explicitly* declare `: IEquatable<T>`, and it cannot phrase the nullable/inline-array components §9.2 admits. It is strictly narrower than the hashable property and would reject the most common test target — a plain auto-equality record — with a confusing "does not implement `IEquatable`" error instead of §9.2's actionable one.
- `Equal(expected: Object, actual: Object)` — **declined outright as a footgun**: boxing erases the static check spec §9.2 places on equality. An auto-hashable record's box dispatches its generated `Equals(Object?)`, but a record that is not auto-hashable and declares no `Equals(Object?)` reaches the inherited `Object.Equals`, which is **reference equality of the boxes** (§9.2, v0.316), and a non-auto-hashable choice's box does the same (§35.7). A boxing `Assert.Equal` would therefore compare exactly the types with no defined equality by identity, silently, passing or failing for the wrong reason, where the generic `where T: hashable` form rejects them at the instantiation site with the constraint failure (spec §14.2). It must be generic-`T`-with-the-hashable-constraint.

**Owner question (§10) — RESOLVED 2026-08-12 by supersession.** As written at the v0.177 pin, this question posed a two-horn fork — (i) **compiler-known** enforcement vs (ii) pure-library `where T: IEquatable<T>` — on the premise that the equality-bearing constraint was "not a user-writable `where` clause" (§14.2 then had `value`/`class`/`notnull`/type-bound only). The premise expired at v0.214 (`where T: hashable` joined §14.2's derived-property constraints), dissolving the fork: `Assert.Equal<T>` declares the constraint as ordinary generic code — see the delegated ruling above. The two-horn text stands here and in the acceptance record as history.

### 5.2 `True`/`False`, `Null`/`NotNull`, `Fail`

```bezel
  public static method True(condition: bool, message: string := "");
  public static method False(condition: bool, message: string := "");
  public static method Null<T>(value: T?);        // value type or class; fails if non-null
  public static method NotNull<T>(value: T?);     // fails if null
  public static method Fail(message: string);     // unconditional AssertionException
```

`Null`/`NotNull` take `T?` and need no equality (a null check, not a value compare) — so they carry **no** hashable constraint. `Fail` always throws; callers wanting bottom-typed control flow write `throw` themselves (`Fail` is an ordinary unit-returning method, not the bottom type — the compiler cannot prove it never returns, so code after a `Fail` in a value-returning position still needs its own `throw`/`return`). The `message` defaults to `""` (§18.5 interpolation makes a good message cheap: `Assert.True(n > 0, $"expected positive, got {n}")`).

### 5.3 The rendering rule — snapshot to strings via §2.18/auto-`ToString`

An `AssertionException` **cannot hold the live `expected`/`actual` values**: `T` may be a borrow-bearing record or an owned class, and §31.1 forbids exceptions from owning or borrowing (the same reason `CauseChain` is a *snapshot*, §31.4). So the rule is: **at throw time, `Equal` renders both operands to `string` via `ToString()` and stores the two strings in the exception.** Rendering is §2.18-governed — records get the auto-`ToString` `"TypeName(Prop1: v1, …)"` (§9.2; a `ToString` override replaces only the formatting, §9.2 v0.158, and equality is intact), primitives/temporals format intrinsically (stdlib §2.18), classes render their identity `ToString`. This is the exact "copy the relevant data out … into separate owned values" discipline §31.1 prescribes.

```bezel
public class AssertionException: Exception
  public readonly property Expected: string;   // rendered snapshot ("" for non-comparison asserts)
  public readonly property Actual: string;
  public constructor(message: string) base(message);   // CtorChain ends with `;` (grammar A.4)
    Expected := "";
    Actual := "";
  end constructor;
  public constructor(message: string, expected: string, actual: string) base(message);
    Expected := expected;
    Actual := actual;
  end constructor;
end AssertionException;
```

`Equal`'s message composes them (the runner prints it verbatim, §8.3):

```
Assert.Equal failed.
  Expected: Money(Amount: 5.00, Currency: USD)
  Actual:   Money(Amount: 5.00, Currency: EUR)
```

### 5.4 `Throws` — the canonical call-bounded closure, returning a snapshot

`Assert.Throws<TException>(action)` takes a **call-bounded** `() -> ()` (§28.14 v0.177) — the callee runs `action` inside a `try` and never retains it, so any capture is admitted with no `scoped` ceremony. It **cannot return the live exception** (§31.1 — a caught exception is a borrow whose lifetime is the arm body). It returns an `ExceptionInfo` **snapshot** (§31.4's `record ExceptionInfo(TypeName, Message, StackTrace)`) so a test can go on to assert on the message:

```bezel
  // Exact dynamic-type match. Returns a snapshot (§31.1 — the live instance cannot escape the arm).
  public static method Throws<TException>(action: () -> ()): ExceptionInfo
    where TException: Exception;
  // Subtype match — any TException or derived.
  public static method ThrowsAny<TException>(action: () -> ()): ExceptionInfo
    where TException: Exception;
```

Sketch of `Throws` (all real v0.177 syntax; `Type` compares with `=`, stdlib §2.13 "reference equality and `=` coincide"):

```bezel
public static method Throws<TException>(action: () -> ()): ExceptionInfo
  where TException: Exception
  try
    action();
  catch
    var ex: TException when ex.GetType() = typeof(TException) =>
      return new ExceptionInfo(ex.GetType().Name, ex.Message, ex.StackTrace),   // §31.1 snapshot
    var ex: TException =>                        // right hierarchy, wrong exact type
      throw new AssertionException(
        $"Assert.Throws<{typeof(TException).Name}> failed: threw {ex.GetType().Name} "
        + "(a subtype). Use Assert.ThrowsAny for subtype matching."),
    var ex: Exception =>
      throw new AssertionException(
        $"Assert.Throws<{typeof(TException).Name}> failed: threw {ex.GetType().Name}.")
  end;
  throw new AssertionException($"Assert.Throws<{typeof(TException).Name}> failed: no exception thrown.");
end Throws;
```

**Dependency — catching a method type parameter.** The `catch … var ex: TException` arm above matches the caught exception against the method's *type parameter* `TException`. Whether a type-pattern arm may bind a method type parameter is **spec-silent** (§31.2.1 defines the bare-type match only for concrete named types), and the compiler currently mis-dispatches this shape (wrong-arm selection — tracked as compiler bug **#1505**). `Assert.Throws`/`ThrowsAny` therefore depend on the **catch-generic-type-parameter ruling** (lands with the v0.183 train, in flight) and the #1505 fix. Until both land, the two methods degrade to `ThrowsAny`-only semantics expressed with a concrete `catch var ex: Exception` arm plus an `is TException` runtime test (no exact-type discrimination); the exact/subtype split above is the intended surface once the ruling settles. Recorded as a dependency, not a blocker on the rest of the `Assert` library.

**Exact vs subtype — decided: two named methods, exact is the default `Throws`.** `Throws<T>` requires the **exact** dynamic type (`ex.GetType() = typeof(T)` — `typeof` per spec §17.10, `Type` identity comparison per stdlib §2.13 "reference equality and `=` coincide"); `ThrowsAny<T>` allows subtypes (the bare-type match §31.2.1 already performs). Reasoning: the exact form catches the "expected `ArgumentNullException`, got its base `ArgumentException`" bug — the same specificity xUnit's `Throws` (exact) vs `ThrowsAny` (assignable) split exists to give — and the catch machinery already *is* the subtype form, so `ThrowsAny` is the thin one. Exact-by-default makes the stricter, bug-catching choice the one you reach for without thinking.

### 5.5 `Assert` is not `Debug.Assert` — the line, drawn explicitly

The stdlib already has `Debug.Assert(condition)` / `Debug.Assert(condition, message)` (§8.2, `System.Diagnostics` per stdlib §8). This proposal's `Assert` is a **different tool in a different module**, and the distinction is normative, not cosmetic:

| | `Debug.Assert` (§8.2, exists) | `Assert` (this proposal, `System.Testing`) |
|---|---|---|
| **Purpose** | runtime invariant check | test oracle |
| **Elision** | may be elided in release builds | **never elided** |
| **On failure** | implementation-defined (side effects unspecified) | throws typed `AssertionException` with rendered expected/actual |
| **Import** | explicit `import System.Diagnostics` (not auto-imported — spec §2.3's auto-import list is `System`, `System.Text`, `System.Collections.Generic` (v0.289) and `System.Collections.Linq` (v0.278); `System.Diagnostics` is not on it) | **explicit** `import System.Testing` |

No name collision arises: `Debug.Assert` is a method on class `Debug`; `Assert.Equal` is a method on class `Assert` — distinct qualifiers, and because `System.Testing` is not auto-imported, the bare name `Assert` never leaks into production code. (Gap item #23 — `Verify`/`Unreachable`/`Debug.Fail` — is the *invariant* side of the same diagnostics domain and is designed separately; it belongs with `Debug`, not here.)

### 5.6 Why `System.Testing` is not auto-imported

Auto-imported modules (§2 header) are the vocabulary every program uses. Test scaffolding is not that — it is the vocabulary of a program's *tests*, which are separate compilation targets. Making `Assert`/`TestRunner` require an explicit `import System.Testing` keeps the production namespace clean and states, at the import, that a file is test code. This mirrors the stdlib's existing posture that a producer requiring `import` keeps its exceptions non-built-in (§31.4's built-in-iff-built-in-producer principle): `AssertionException`'s only producer is the imported `Assert`, so it correctly lives in the module, not among the language built-ins.

## 6. Async tests (design question 2)

A test method returns `()` **or** `Task`; the runner awaits the `Task` form to completion before scoring it. (Not `owned Task`/`Task<T>`: a test yields pass/fail, not a value; a returned `owned Task` would impose a discharge obligation on the runner for no benefit — `Task` (Pattern A, fire-and-await) is the right shape.)

```bezel
public class LedgerTests
  public async method PostsConcurrentlyAsync()      // returns Task — runner awaits it
    using var ledger := new TestLedger();
    await ledger.PostAsync(entry);
    Assert.Equal(1, ledger.Count);
  end PostsConcurrentlyAsync;
end LedgerTests;
```

**Ordering/parallelism — decided: serial, in one domain, for v1.** The runner awaits each test's `Task` to completion before starting the next. Because a domain is **serial** (§30.13 — continuations "interleave only at suspension points and never overlap"), a serially-awaited sequence of async tests in the runner's own domain is fully deterministic and needs no synchronization; a shared `using var` fixture (§7) is reachable by every test without a sendability question. **Parallelism is a deferred runner *option*, not v1** — running each test in its own `Domain.Spawn` (§30.14) forces the test body to be capture-free and its fixtures sendable (§30.14's transfer-closed rule), which most tests and most fixtures are not; a shared borrowed fixture cannot cross a domain boundary at all. So per-domain parallelism buys throughput only for the narrow slice of tests that are already sendable-closed, at the cost of the ownership-as-fixture story (§7) that is this design's whole point. v1 is serial-in-one-domain; a `parallel := true` runner knob is recorded as a later addition for the sendable-fixture case (§9).

## 7. Lifecycle without attributes — `using var` fixtures are the answer (design question 3)

**This is the design's centerpiece.** Every attribute-based framework (JUnit `@Before`/`@After`, xUnit constructor/`Dispose`, NUnit `[SetUp]`/`[TearDown]`) splits fixture *setup* and *teardown* into separate lifecycle callbacks the framework invokes by reflection. Bezel needs **none of it**, because it already has a better mechanism for "acquire a resource, use it, release it deterministically even on failure": **ownership.**

```bezel
public test method WritesThenReadsBack()          // or: public method, registered via (b)
  using var db := new TestDatabase();             // SETUP — constructor
  using var conn := db.Connect();                 // nested fixture, disposed first
  conn.Insert(row);
  Assert.Equal(row, conn.SelectOne(row.Id));
end WritesThenReadsBack;                           // TEARDOWN — destructors, reverse order,
                                                   // on EVERY exit incl. the Assert-throws path
```

The fixture's lifetime **is** the test's scope. `using var` disposes "on every exit path: normal completion, `break`, `return`, or exception" (§28.10) — so teardown runs when an `Assert` throws, with no `try`/`finally` and no `@After`. Destructors run in reverse construction order (§28.10) and owned fields are exception-safe (§22.9). The argument, stated plainly:

- **One mechanism, not two.** Setup is a constructor; teardown is a destructor; the language already guarantees the pairing. There is no `SetUp` that can be written without a matching `TearDown`, because there is no `TearDown` — disposal is automatic.
- **Teardown cannot be forgotten.** The xUnit footgun (a `Dispose` you forget to implement, an `@After` you forget to write) is *structurally impossible*: an owned fixture's destructor is called by the language, not by a framework callback the author must remember to supply.
- **Teardown runs on failure.** The precise thing frameworks add `try`/`finally` and lifecycle ordering to guarantee — cleanup after a *failing* test — is §28.10's plain behavior. A leaked test DB after a red test is not possible.
- **Shared/expensive fixtures compose without a second concept.** A per-*suite* fixture (JUnit `@BeforeClass`, xUnit `IClassFixture`) is a `using var` in the test `Main` (§8) whose lifetime *outlives* the `RunAsync` call, **borrowed** into each test — the runner passes the borrow, and the fixture's single destructor runs when `Main` returns. Per-test vs per-suite is not two annotations; it is *where you write the `using var`* — inner scope or `Main`. The ownership model already expresses the whole lifetime taxonomy.

This is the same disposition DECLINED D-0001 reached for DI ("the lifetime taxonomy maps without a framework"): the framework's job is already done by `owned`/borrow, more legibly, at compile time.

## 8. The runner (design question 4)

v1 is a **library `Main`** — option (b)'s runner, needed under every fork.

```bezel
public class TestRunner
  public constructor();
  public method Add(name: string, scoped body: () -> ());       // sync test  (scoped: the runner stores the body until RunAsync, §28.14)
  public method Add(name: string, scoped body: () -> Task);     // async test (§6)
  public method RunAsync(filter: string? := null): Task<int>;   // returns the failure count
end TestRunner;
```

### 8.1 Execution and isolation

`RunAsync` runs the added tests **serially in its own domain** (§6): for each, it invokes `body` (awaiting the `Task` form), scoring pass on normal completion and fail on a thrown exception. Each test's `using var` fixtures (§7) are disposed at that test's scope exit, so no state leaks between tests except a fixture *deliberately* shared by borrow from `Main` (§7). The `body` parameters are **`scoped`** (storage-bounded) function-typed parameters (§28.14): `Add` *stores* each body in the runner's own scoped collection from registration until `RunAsync` invokes it, which a plain call-bounded parameter forbids ("may **not** be stored in a field of any object"). The bounding object is the runner receiver, so a **capture-bearing** test closure is admitted iff the runner is a `using var`/`await using var` local and every captured source is declared before it (§28.14) — hence the `using var runner` in §3.2/§8; the runner drains and never invokes a stored body after its own disposal (quiescence, §28.14). **Capture-free method references** (the canonical `ClassTests.MethodName` registration) are admitted to the `scoped` parameter unconditionally, so the common path needs nothing extra.

### 8.2 Failure classification

Two failure kinds, mirroring the conformance runner's `[reason]` tags:

- **`AssertionException`** → `FAIL name` with the exception's composed message (expected/actual, §5.3).
- **Any other `Exception`** → `FAIL name [error]` with a **snapshot** of the type and message (`ex.GetType().Name`, `ex.Message` — §31.1, the runner cannot retain the live instance). An unexpected throw is a test error, distinct from an assertion failure, exactly as the conformance runner distinguishes `[compile-error]` from `[wrong-result]`.

### 8.3 Reporting — the conformance runner's PASS/FAIL precedent

The reporting format is the existing `tests/conformance/run_conformance.sh` style, which already prints "`PASS`/`FAIL <reason>` per case and a final tally" and exits non-zero iff any failed:

```
PASS ParsesEmptyInput
FAIL WritesThenReadsBack: Assert.Equal failed.
  Expected: Row(Id: 7, Name: alice)
  Actual:   Row(Id: 7, Name: alcie)
PASS PostsConcurrentlyAsync
FAIL DividesByZero [error]: DivideByZeroException: attempted to divide by zero

2 passed, 2 failed
```

`RunAsync` returns the failure count; `Main` returns it as the process exit code (`0` iff all green) — CI-consumable with no extra convention, the same exit-code contract the conformance runner uses ("Exit code: 0 if all non-skip pass, 1 otherwise").

### 8.4 Filtering

`RunAsync(filter)` runs only tests whose `name` contains `filter` (a substring match — the smallest useful selector; a glob/regex form waits for the Regex proposal, gap #19). Under fork (a), the compiler-collected registry feeds the same `Add`/`RunAsync` engine — the runner is fork-independent; only its *input* (hand-written `Add` calls vs a compiler-emitted registry enumeration) changes.

## 9. What v1 deliberately leaves out

- **Per-domain parallel execution** — deferred runner option; the sendable-fixture constraint makes it niche (§6).
- **Parameterized/data-driven tests** (`[Theory]`/`[InlineData]`) — these are the attribute-shaped feature §18 most directly defers; v1's answer is an ordinary `for` loop over cases inside one test body (`for var c in cases do Assert.Equal(c.want, Compute(c.input)); end`), which needs nothing new.
- **`Assert` collection helpers** (`Assert.Contains`, `Assert.Empty`, sequence-equality) — additive; v1 covers them via `Assert.True(coll.Contains(x))`. A later batch adds the ergonomic forms.
- **Mocking / test doubles** — out of scope; Bezel's answer is hand-written fakes implementing the interface under test (the same explicit-over-reflective posture as DECLINED D-0001), never a reflective mock framework.
- **A `[Skip]`/ignore mechanism** — under (b), comment out the `Add`; under (a), an owner question (§10) — likely a `test skip` sub-clause, deferred until (a) itself is ruled.

## 10. Open questions for the owner

1. **The fork ruling (§3).** Approve **(a) the `test` modifier** as the eventual discovery mechanism (recommended, gated on the §3.4 evidence trigger), or hold at **(b)** permanently? This is the one language-change decision; everything else ships without it.
2. **`Assert.Equal` eligibility mechanism (§5.1).** Make `Assert` **compiler-known** with the hashable derived-type-property constraint (recommended — full §8.3 coverage, the §9.2 diagnostic, parity with `Dictionary<K,V>`/`HashCode.Add`), or ship it **pure-library** with `where T: IEquatable<T>` (no compiler support, but excludes enums and un-declared auto-equality records)? *(Superseded 2026-08-12 — the fork's premise expired at v0.214; the surface is the ordinary `where T: hashable` clause. Delegated ruling above.)*
3. **`test` method shape, *if* (a) is approved (§3.1).** Static-only, or instance methods on a class with a parameterless constructor the runner calls? (This proposal leans **static** `test` methods + `using var` fixtures inside — it keeps the fixture story on ownership, §7, and avoids a constructor-as-`SetUp` reintroduction.) Also: confirm the parameterless / `()`-or-`Task`-return / non-`virtual` restrictions, and whether a `test skip` sub-clause is in scope.
4. **`Throws` return (§5.4).** Confirm `ExceptionInfo` snapshot (recommended, §31.1-consistent) over returning nothing.
5. **Module name.** `System.Testing` (proposed) vs `System.Diagnostics.Testing` (nearer the `Debug`/`Verify` siblings)? This proposal recommends the shorter `System.Testing`, explicit-import.

## 11. Declined alternatives (register)

1. **(c) naming-convention discovery** — stringly-typed, no compile-time shape check (DECLINED D-0001 ground 2), blocked on #1398, reintroduces the intentional-vs-accidental blur (§3.3).
2. **`Assert.Equal(Object, Object)`** — boxes its arguments, so a record or choice that is not auto-hashable and declares no equality of its own compares by box identity through the §9.2 `Object`-receiver escape hatch, silently (§5.1); auto-hashable records keep their generated `Equals(Object?)`. Must be generic-`T` hashable.
3. **`where T: IEquatable<T>` on `Equal`** — excludes enums (§2.9) and un-declared auto-equality records; narrower than the hashable property; recorded as the pure-library fallback, not the recommendation (§5.1). The decline stands under the 2026-08-12 re-cut — the ruled surface is the same library-only shape *without* `IEquatable`'s holes (`where T: hashable`).
4. **Assertion failure as a return code / out-param** rather than a thrown `AssertionException` — declined: it makes forgetting to check an assertion a silent pass, and does not compose with the `using var` teardown-on-throw that makes §7 work. A thrown typed fault is the shape §31 exists for.
5. **A `[Test]`/`[Fact]` attribute (the general system) now** — declined per rationale §18: designing the attribute *before* its consuming machinery (reflection, the runner) "would lock in syntax for an unbuilt feature." The narrow `test` modifier is the §18-sanctioned interim, promotable to `[Test]` later (§3.1).
6. **`SetUp`/`TearDown` lifecycle methods** (with or without attributes) — declined: `using var` fixtures (§7) express the same lifetime with one mechanism, cannot-forget teardown, and teardown-on-failure for free.
7. **`Assert` in the auto-imported `System`** — declined: test scaffolding is not production vocabulary; explicit `import System.Testing` keeps the namespace clean and marks test files (§5.6).
8. **Per-domain parallel tests in v1** — declined for v1: the sendability constraint on bodies and fixtures makes it niche and forfeits the ownership-as-fixture story; a later runner option (§6, §9).

## 12. Relationship to existing work

§9.2 (auto-equality — the `Assert.Equal` foundation and its use-site error) · §8.3/§3.6 (the hashable derived type property, enforced identically for `Dictionary<K,V>` K and `HashCode.Add<T>` — proposal [0008 §5.4](0008-integer-toolkit.md)) · §28.14/v0.177/#1419 (call-bounded closures — `Assert.Throws`) · §27.6 (method references — the (b) runner) · §31/§31.4 (typed exceptions, `ExceptionInfo` snapshots, `AssertionException` as a user subclass, the built-in-producer principle) · §2.18 (rendering expected/actual) · §28.4/§28.10/§22.9 (`using var` + destructors — the fixture lifecycle) · §30.13/§30.14 (domains — serial async tests, the deferred parallel option) · stdlib §2.13 (`Type` identity — exact-`Throws`) · stdlib §8.2 (`Debug.Assert` — the release-elidable tool this is deliberately *not*) · rationale §18 + §5.19 (the attribute deferral, the `serialized`-modifier precedent, the CodingKeys silent-drop footgun — the load-bearing arguments of §3) · DECLINED D-0001/D-0002 (the compile-time-over-reflection and evidence-from-real-code posture §3.4/§7 argue from) · gap review #1403 item #24 (this proposal), item #23 (the sibling `Verify`/`Debug.Fail`), item #19 (Regex — the eventual `RunAsync` filter) · [#1398](https://github.com/s8lap/bezel-lang/issues/1398) (the build driver — fork (c)'s host) · wave D3 of the [gap DESIGN plan](stdlib-gap-design-plan-2026-07.md).
