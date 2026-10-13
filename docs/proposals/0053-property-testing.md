# 0053 - Property-based specification: generated-input properties over the 0022 test framework

**Status:** under-review — draft
**Tracking issue:** [#2040](https://github.com/s8lap/bezel-lang/issues/2040)
**Target:** Stdlib — the `System.Testing` module proposal [0022](0022-test-framework.md) defines (new `Generator`, `Property`, `PropertyOptions` surface; `TestRunner.AddProperty` overloads; the `FromGenerator` recognition convention; report formats). Spec — **none under 0022's fork (b)**; under fork (a), one clause amending the `test`-modifier shape rule when that rule lands on 0022's own spec track (§3.3 below; no grammar production changes — parameter lists already parse). Tooling — runner execution/reporting/knobs, and the stage-S2 contract-bridge generator riding proposal [0040](0040-build-system.md)'s driver. Rationale — none.
**Written against:** spec v0.197 (stdlib/rationale at the same revision)
**Currency note:** Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-08-11 to v0.246: `hashable` vocabulary (v0.242; formerly *equality-bearing*). Note: 0022's cited constraint mechanism was re-cut 2026-08-12 (currency flag 2 **resolved**, the owner's direction on the recorded flags): `Assert.Equal<T>` now states the ordinary `where T: hashable` clause (§14.2, v0.214) rather than compiler-known enforcement — this proposal's citations conformed in place (§3.4, §4.1, §9, §14 Q2); its own *generatable* classification remains genuinely non-`where`-writable and keeps the compiler-known scoping. **2026-09-17 proposals currency sweep — re-verified at v0.306:** no tier-1 respell (the §A/§B/§D scan is clean over this file; §15.4.1 and stdlib §2.18 / spec §9.2 still resolve, and `Set<T>.TryAdd` and `Array<T>.Sorted(comparer)` are still the declared members §4 and §1 use). One tier-2 flag is recorded in `proposals-currency-2026-09-17.md` §3: §0, §5 and §9 cite `FromCollectionLiteral` as a **name-based recognition** precedent for the `FromGenerator` convention, and that premise was retired at v0.294 — the sequence-literal protocol is now interface-based (`ICollectionLiteralTarget<TSelf, T>` / `IOwningCollectionLiteralTarget<TSelf, T>`, stdlib §2.19, spec §15.4.1), i.e. the promotion §5 calls "forward-compatible" has already happened. Written-against pin unchanged.

This proposal expands item **P13** of [`future-directions-2026-07-18.md`](papers/future-directions-2026-07-18.md) to graduation depth, and extends proposal [0022](0022-test-framework.md) (test framework), which it assumes in full. Where it sharpens or diverges from the P13 sketch, the divergence is recorded explicitly in §12. *Reference convention: an unprefixed §-reference is a language-spec reference; `stdlib §`, `rationale §`, and `0022 §`/`0041 §`/… prefixes are written out; this proposal's own sections are cited as "§N below" or from context.*

## 0. Summary

A **property** is a 0022 test method *with parameters*: the runner generates the arguments, runs the body many times, and reports a failing case as a seed plus a shrunk minimal counterexample. Assertion is the ordinary 0022 `Assert` vocabulary — a property passes for an input exactly when the body completes normally, the same pass criterion every 0022 test already has. Generators are **derived mechanically from the parameter types** (primitives, `string`, enums, positional records, value-like choices per proposal [0041](0041-choice-types.md), nullable liftings, and the three sized borrowing collections `Array<T>`/`Dictionary<K, V>`/`Set<T>`), with mandatory edge-value injection; a type opts into custom generation by declaring one recognized static factory, on the §15.4.1 `FromCollectionLiteral` model. Shrinking is typed and structural — numeric toward zero, collections by removal then element shrink, records field-wise, choices toward earlier-declared cases — and deterministic. `pure` properties (proposal [0042](0042-pure-methods.md)) over small finite domains are checked **exhaustively** instead of sampled. At stage S2, a method carrying [0043](0043-behavioral-contracts.md) contracts gets an auto-derived property — generate arguments satisfying `requires`, call, assert `ensures` — as tool-generated source, not language magic. Failing seeds replay exactly; with a [0045](0045-deterministic-replay.md) journal, whole failing runs do.

The ownership line is drawn deliberately: v1 generates **only values whose disposal runs no user code** — value types, `string`, and trivial-dispose instantiations of the borrowing collections (§28.9) — so the harness never runs user constructors or destructors outside the property body. Owned class generation is a recorded v2 direction, not a v1 feature.

## 1. Problem and evidence

- **0022 stops at examples.** Proposal 0022 §9 explicitly leaves parameterized/data-driven tests out of v1, answering with "an ordinary `for` loop over cases inside one test body." That answer covers *enumerated* cases; it cannot search, has no seed discipline, no shrinking, and no reporting beyond the first `Assert` failure. The principled form of the deferred feature is the property, and it needs design, not just an attribute.
- **The hand-rolled alternative exists and is bad.** Everything needed to fake a property exists today — `Random` (stdlib §2.7), a `for` loop, `Assert` — and every hand-rolled instance reinvents, badly, the parts that make the technique trustworthy: printing the seed, injecting the edge values, shrinking the counterexample, and distinguishing "passed 100 arbitrary cases" from "checked the whole domain." A default-constructed `Random` is system-time seeded (stdlib §2.7), so the naive loop is an unreproducible flake generator.
- **The language is unusually well-shaped for derivation.** Value-type structure is closed and self-containment-free (records §9.1–§9.3; enums §10; choices per 0041 §2.1), so type-directed generation provably terminates (§4.6 below). Per-instantiation structural classification is an established discipline (§3.6 rule 2; the hashable enforcement at `Dictionary<K, V>` and 0022's `Assert.Equal`). Reflexive float equality (`NaN = NaN` is true, §4.3.1) makes edge-injected NaN cases *assertable* — in IEEE-equality languages every round-trip property spuriously fails at NaN; in Bezel `Assert.Equal(x, f(x))` is meaningful at every injected edge.
- **The verification stack has a hole exactly here** (future-directions P13): types state shape; 0043 contracts state clauses; proofs discharge them. Properties are the cheap middle layer — more than an example, less than a proof — and for a regeneration loop they are the highest-value oracle per line of human-written specification. 0043 §20 already points at this proposal for its oracles; this proposal supplies the machine.

## 2. The design at a glance

```bezel
import System.Testing;

public class ParseProps
  // Fork (a) spelling: a `test` method with parameters IS a property.
  public test method ParseRoundtrips(n: int)
    Assert.Equal(n, int.Parse(n.ToString()));
  end ParseRoundtrips;

  public test method SortIsIdempotent(xs: Array<int>)     // xs: generated, harness-owned, borrowed in
    using var once := xs.Sorted(lambda(a, b) => a.CompareTo(b));
    using var twice := once.Sorted(lambda(a, b) => a.CompareTo(b));
    Assert.Equal(once.Count, twice.Count);
    for var i in 0..<once.Count do
      Assert.Equal(once[i], twice[i]);
    end;
  end SortIsIdempotent;
end ParseProps;
```

```bezel
// Fork (b) spelling: registration, zero language change — works against v0.197 + the 0022 runner.
// (Under (b) the methods above are plain `public method`s — 0022 §7's dual-spelling comment.)
runner.AddProperty("ParseRoundtrips",   ParseProps.ParseRoundtrips);   // (int) -> () method reference
runner.AddProperty("SortIsIdempotent",  ParseProps.SortIsIdempotent);  // (Array<int>) -> ()
```

The pillars, each decided below: **(1)** a property is a parameterized 0022 test, asserting via `Assert`, passing when the body completes (§3); **(2)** generators derive from parameter types by closed per-kind rules with mandatory edge injection, bounded by the ownership line (§4); **(3)** custom generation is one recognized static factory, `FromGenerator` (§5); **(4)** shrinking is typed, structural, deterministic (§6); **(5)** execution reports seed + original + minimal counterexample, `--seed` replays, 0045 journals close the impure gap, and pure finite-domain properties run exhaustively (§7); **(6)** 0043 contracts auto-derive bridge properties at S2 (§8).

## 3. The declaration form: a property is a parameterized test

### 3.1 Shape

A property is a method that would be a 0022 test but for its parameter list: unit-returning, non-`virtual`, non-generic, synchronous, with **one to four parameters** of generatable type (§4.1), none carrying `ref`/`out`/`out owned`/`owned`/`scoped` modifiers or default values. More than four dimensions are bundled into a positional record parameter — which is itself generatable by field composition (§4.2), so the arity cap costs no expressiveness and buys a named shape for the case. Value, record, enum, choice, and `string` arguments arrive as ordinary copies/shared references; collection arguments arrive as **borrows of harness-owned instances** (plain class-typed parameters are borrows, §28.3) that the harness disposes after the case — the 0022 §7 ownership-as-fixture story extends to per-case inputs with zero new mechanism, and per-case fixtures remain `using var` in the body.

### 3.2 Assertion via `Assert`; pass = normal completion (no bool-return convention)

The P13 sketch describes "a boolean method over generated inputs." This proposal deliberately does not adopt the bool-return convention (divergence D2, §12). A property asserts with the 0022 `Assert` vocabulary and **passes for an input iff the body completes normally** — exactly 0022 §8.2's scoring rule, unchanged. The defense:

- **One failure channel.** 0022 §11 (decline 4) already rejected return-code assertions because "forgetting to check an assertion [is] a silent pass"; a bool-returning property re-admits the same defect one level up — a body that computes a verdict and forgets to return it (or returns `true` on a path that meant to check something) silently passes. Thrown `AssertionException` cannot be dropped.
- **Reporting.** A `false` return says *that* the property failed; an `Assert.Equal` failure says *where and why*, with rendered expected/actual (0022 §5.3). The shrink driver (§6) re-runs the body hundreds of times; the final report's quality is the assertion's quality.
- **Consistency.** A property body reads exactly like a test body; every `Assert` member, every fixture idiom, and the failure classification (assertion failure vs. `[error]`, 0022 §8.2) carry over verbatim. Multiple assertions per property need no conjunction plumbing.
- **Uniformity under both forks.** The runner's pass criterion stays "completed normally" for every entry it runs, tests and properties alike — one scoring rule in the engine.

### 3.3 Discovery under fork (a) — the `test` modifier

If the owner approves 0022's recommended endpoint, the `test`-modifier shape rule (0022 ruling 5: "static, parameterless, `()`-or-`Task`-returning") is amended by one clause when it lands on the spec track:

> A `test` method may declare parameters. A `test` method with parameters is a **property**: the runner invokes it repeatedly with generated arguments per stdlib `System.Testing`'s generation rules. Every parameter type must be **generatable** (stdlib `System.Testing`; violations are a compile error at the declaration naming the parameter and the structural reason, in the [§3.6] rule-4 style); parameters carry no modifiers and no default values; a parameterized `test` method returns `()` (not `Task`, §3.5 below).

No grammar production changes: `MethodDecl`'s parameter list already parses; the v0.197 grammar delta of this proposal is empty. Discovery is 0022's compiler-collected registry, with parameter types recorded per entry; the registry feeds the same engine as fork (b) (0022 §8.4's fork-independence).

### 3.4 Discovery under fork (b) — registration

`TestRunner` (0022 §8) gains overloads, shipping with no ruling needed:

```bezel
public class TestRunner
  // 0022 §8 members unchanged, plus:
  public method AddProperty<T1>(name: string, scoped body: (T1) -> (), cases: int := 0);
  public method AddProperty<T1, T2>(name: string, scoped body: (T1, T2) -> (), cases: int := 0);
  public method AddProperty<T1, T2, T3>(name: string, scoped body: (T1, T2, T3) -> (), cases: int := 0);
  public method AddProperty<T1, T2, T3, T4>(name: string, scoped body: (T1, T2, T3, T4) -> (), cases: int := 0);
end TestRunner;
```

Each `Ti` must be generatable, enforced **per instantiation at the `AddProperty` call site**: `AddProperty` is a compiler-known member carrying a structural constraint that is genuinely not a user-writable `where` clause (§4.1) — the scoping `HashCode.Add<T>`'s eligibility rule established (0008 §5.4; the `Dictionary<K, V>` structural precedent, stdlib §4.8). (`Assert.Equal<T>`, this clause's original citation, left that family under 0022's 2026-08-12 delegated ruling — its hashable bound is now the ordinary `where T: hashable` clause; *generatable* has no such clause to write, so the compiler-known scoping stays.) `cases := 0` means "runner default" (§7.1); the optional parameter rides §22.5. `scoped` storage and the `using var runner` discipline are 0022 §8.1's, unchanged. Under fork (b), per-property configuration has a natural home (the `cases` argument); under fork (a) it has none until an attribute system exists — a recorded asymmetry (§14 Q4), mitigated by the runner-level knobs (§7.5).

### 3.5 Synchronous only in v1

An async property (a `Task`-returning parameterized test) is **declined for v1**. The feature's spine is the reproduction contract — seed ⇒ same verdict — and an async body's verdict can depend on scheduling edges the seed does not control (§30.13). Worse, shrinking (§6) needs hundreds of *fresh* executions at *different* inputs; a 0045 journal can replay one recorded execution exactly, but cannot drive fresh executions at new inputs, so shrink verdicts over scheduling-sensitive bodies are unstable in a way no current machinery repairs. Sync-impure bodies share a milder form of this (§7.3's strata); async crosses the line from "degraded guarantee" to "no guarantee." Recorded as v2 with "shrinking under nondeterminism" named as the open problem (§14 Q10).

### 3.6 Discarding a case: `Property.Discard()`

```bezel
public static class Property
  public static method Discard();    // raises CaseDiscardedException; the runner scores the case
                                     //   as discarded (neither pass nor fail) and draws a fresh one
end Property;
```

A property over constrained inputs rejects a non-conforming case by calling `Property.Discard()`. The mechanism is a typed exception (`CaseDiscardedException: Exception`, `System.Testing`-resident per §31.4's built-in-iff-built-in-producer principle) — deliberately the *same* signaling mechanism as `AssertionException` itself: the runner is the sole intended catcher, and a non-local verdict from a unit-returning body has exactly one spelling in this language. Discard accounting feeds the exhaustion verdict (§8.3). A plain `return;` is *not* a discard — it is a pass, exactly as in any test; the distinction is why the explicit form exists.

## 4. Generator derivation

### 4.1 The generatable classification, and the ownership line

> A type is **generatable** when it is: `bool`; an integer type; `double` or `float`; `decimal`; `char`; `string`; an enum; a record whose storage components are exactly its positional fields ([§9.1]/[§9.3] — no body storage), each of generatable type; a value-like choice (0041 §2.5) whose every payload field is of generatable type *(proposed-dependency: this clause activates when 0041 lands)*; `T?` where `T` is generatable; `Array<T>`, `Set<T>`, or `Dictionary<K, V>` where the element/key/value types are generatable (the existing hashable instantiation requirements of stdlib §4.8/§4.10 apply unchanged and are what make generated keys well-behaved); or any record, enum, or choice declaring the `FromGenerator` factory (§5). The classification is computed from structure, never declared, and evaluated per instantiation — [§3.6] rules 1–2 in style — but it is **not** a [§3.6] table row: it is enforced only at the compiler-known `System.Testing` members that consume it (`AddProperty`'s type parameters; fork (a)'s parameter rule; `Generator.Next<T>`), the scoping `HashCode.Add<T>`'s eligibility rule established (0008 §5.4; `Assert.Equal<T>`'s hashable bound is, since 0022's 2026-08-12 delegated ruling, an ordinary `where T: hashable` clause — compiler-known scoping remains only where the classification is not `where`-writable, exactly as *generatable* is not). *(Owner decision Q2.)*

Everything else is **not generatable**, and the reason is ownership before it is anything else. Generating a class or interface instance means electing a constructor and fabricating its arguments — arbitrary user code running at harness time — and the produced value is owned (§28.3): a disposal obligation the harness would discharge by running a user destructor *between* cases, where a fault can be attributed to no case and a lifetime belongs to no test. v1 therefore generates **only values whose disposal provably runs no user code**: value types, `string`, and the borrowing collections at trivial-dispose instantiations (§28.9 — `Array<T>`/`Set<T>`/`Dictionary<K, V>` over generatable element types are exactly that, since every generatable type is value-shaped or `string`; nested collection elements are fresh trivial-dispose producers, admissible everywhere per §15.4's literal rule). Harness-side cleanup is thereby invisible by construction. Owning collections, spans (borrow-bearing, no source to borrow from), function types (behavior cannot be invented), inline arrays (mechanically derivable; deferred to keep v1's surface minimal, recorded liftable), and bare type parameters (properties are non-generic, §3.1) are all outside v1.

**The v2 direction, recorded and not designed:** user-registered factory generators producing *owned* values — `FromGenerator`-shaped factories returning `owned T` for class `T` — with the harness assuming the disposal obligation per case and a defined fault-attribution rule for generator-time and disposal-time exceptions. That design owes answers on exception paths, on borrow-bearing constructor arguments, and on 0045 interaction (generator-time world edges), and is deliberately deferred until the value-type layer has proven the workflow.

### 4.2 Per-kind derivation rules (normative-intent)

For each kind: the **distribution** (sampled mode), the **edge set** (§4.3), and the **canonical enumeration order** (exhaustive mode, §7.4). All entropy flows from the per-case `Generator` state (§4.5); all numeric constants below are fixed per runtime revision (§7.2's determinism contract).

- **`bool`** — uniform. Edges: both values. Order: `false`, `true`.
- **Integer types** (stdlib §9.1) — a mixed distribution: half the draws uniform in a small band (|v| ≤ 100, clipped to the type's range), the rest uniform over the full range. Edges: `0`, `1`, `-1` (signed only), `MinValue`, `MaxValue`. Order: ascending from `MinValue`.
- **`double` / `float`** (stdlib §9.2) — a mixed distribution over small rationals, full-range finite values, and (at low fixed weight) the non-finite values. Edges: `0.0`, `-0.0`, `1.0`, `-1.0`, `Epsilon`, `MinValue`, `MaxValue`, `PositiveInfinity`, `NegativeInfinity`, `NaN`. The reflexive `=` of [§4.3.1] is what makes the NaN edge assertable in round-trip properties; properties wanting IEEE semantics test via `Double.IeeeEquals` explicitly. Never enumerated exhaustively (domain size 2^64).
- **`decimal`** (stdlib §9.3) — small values with few fractional digits dominate; full-range occasionally. Edges: `0m`, `1m`, `-1m`, `MinValue`, `MaxValue`. Never exhaustive.
- **`char`** ([§4.5]) — ASCII-weighted: printable ASCII most of the time, then BMP non-ASCII, then supplementary-plane scalars; surrogate code points cannot arise (excluded by the type). Edges: `'\u0000'`, `'a'`, `' '`, one BMP non-ASCII (`'é'`), one supplementary (`'\U0001F600'`), the maximum scalar `'\U0010FFFF'`. Domain size 1,112,064 — finite, but above any plausible threshold.
- **`string`** — a generated length per §4.4, then per-`char` draws with the `char` distribution; non-ASCII content therefore occurs by construction. Edges: `""`, `" "`, `"a"`, a BMP non-ASCII string, a supplementary-plane string. Never exhaustive.
- **Enums** ([§10]) — uniform over the **distinct declared values** ([§10.2]: aliases denote one value and collapse). Edges: every distinct value. Order: declaration order of first-declaring constants. Domain size = distinct-value count.
- **Positional records** — field composition: each storage field generated by its own type's rule, in declaration order; constructed via the synthesized positional constructor ([§9.1]), which is total over field values (it cannot impose invariants), which is precisely why body-storage records are excluded from derivation — their authored constructors ([§9.2]) may encode invariants the deriver cannot know, and `FromGenerator` (§5) is their door. Edges: field-wise (§4.3). Order: lexicographic by field order. Domain size = product of field domain sizes.
- **Choices** (0041; value-like classification only) — draw a case uniformly over the declared cases, then generate its payload fields recursively. Owning and borrow-bearing choices are excluded by the §4.1 rule (class payloads), which the classification computes already. Edges: every declared case, constructed at payload edges (§4.3) — for a `Result`-shaped type both `Ok` and `Error` are exercised by construction. Order: case declaration order, then payload-lexicographic. Domain size = sum over cases of payload products.
- **`T?`** ([§7]) — null-weighted: `null` with fixed weight 1/8, else a `T` draw. Edges: `null` plus `T`'s edges lifted. Order: `null` first, then `T`'s order. Domain size = 1 + |T|.
- **`Array<T>`** — a length per §4.4, then per-element draws. `Set<T>` — draw a length n, `TryAdd` n drawn elements (duplicates collapse; resulting `Count` ≤ n, and small-domain elements make small sets — accepted, not corrected). `Dictionary<K, V>` — draw n, insert n drawn pairs via the indexer (last write wins, stdlib §4.8). Generated keys/elements are value-shaped or `string`, hence immutable — the stability-while-stored obligation of stdlib §4.8 is satisfied by construction. Edges: the empty collection, and a singleton holding an element edge. Never exhaustive.

### 4.3 Edge-value injection is mandatory and deterministic

> The first **E** executed cases of a property, where E is the largest edge-list length among the parameters, draw each parameter from its edge list at index (case index mod that parameter's edge-list length), in a fixed order per kind; composite kinds (records, choices, collections, nullables) surface their components' edges through the composite constructions named in §4.2. Subsequent cases are distribution draws, in which edge values additionally occur at a small fixed weight. Consequence, normative: when the case budget is at least E, **every edge value of every parameter appears in at least one executed case**, and the schedule is a pure function of the seed and the parameter types.

This buys the guarantee that matters (each edge value is exercised) without combinatorial explosion (the cross product of edge tuples is not promised; the random phase samples it). Zero, empties, `null`, `NaN`, and both infinities are not left to luck — the difference between a property suite and a fuzzer.

### 4.4 Sizing

Collection lengths and string lengths draw from a size-governed distribution: the size budget starts small and grows linearly over the run to `MaxSize` (default 64), so early cases are small (fast, and failures found early are already near-minimal) and later cases probe deeper. Lengths 0 and 1 have dedicated weight beyond the edge phase. Nested collections divide the budget by nesting depth; generation recursion follows the *static type's* structure, which has finite depth, so no runtime depth guard is needed (§4.6).

### 4.5 The `Generator` context

```bezel
public class Generator                      // System.Testing; not user-constructible —
                                            //   the runner creates one per case from (seed, case index)
  public readonly property Size: int;                                // current size budget (§4.4)
  public method Next<T>(): T;                                        // compiler-known; T generatable (§4.1)
  public method NextInt(minInclusive: int, maxExclusive: int): int;
  public method NextLong(minInclusive: long, maxExclusive: long): long;
  public method NextDouble(): double;                                // [0.0, 1.0)
  public method NextBool(): bool;
  public method NextLength(): int;                                   // size-governed, for custom collections
end Generator;
```

`Generator` wraps a deterministically seeded PRNG (the stdlib `Random` machinery, stdlib §2.7, seeded — never the system-time default constructor) plus the size and edge schedule. `Next<T>()` is the compiler-known entry point through which all derivation runs; its per-instantiation body is compiler-synthesized from the §4.2 rules — the same synthesis category as [§9.8] serialization (mechanically derived from declared structure, per type, no reflection). `Generator` is a class, domain-local, harness-owned, and passed to custom factories as a borrow.

### 4.6 Termination, stated as a theorem

Generation recurses only through component structure: record fields, choice payloads, nullable wrappers, and collection elements. Records and choices cannot contain themselves through value paths ([§9]-family layout rules; 0041 §2.1's no-self-containment), class-typed components are not generated at all (§4.1), and collection nesting follows the static type, which is finitely deep. Therefore derivation-driven generation **always terminates, with no depth heuristics** — a property the reference QuickCheck lineage cannot state (recursive sum generation there needs manual size damping). The graduating stdlib text should carry this argument; it is the reason the derivation rules can be closed.

## 5. Custom generators: the `FromGenerator` static factory

One mechanism, on the model of [§15.4.1]'s `FromCollectionLiteral` (name-based recognition of a specifically-named public static with a recognized signature; rationale §5.16 records why that mechanism over attributes):

> A record, enum, or choice type `T` opts into custom generation by declaring
>
> ```bezel
> public static method FromGenerator(g: Generator): T;
> ```
>
> — a `public static` method named exactly `FromGenerator`, with exactly one parameter of type `Generator` and return type `T`. Where declared, it **replaces** structural derivation for `T` entirely (the author knows the invariant; the deriver defers), and it makes an otherwise-underivable value type (a body record; a positional record with an invariant the field types cannot express) generatable. It is rejected on class and interface types in v1 (the diagnostic names the §4.1 ownership line and the recorded v2 direction). The factory must draw **all** of its entropy from `g`: this is a stated obligation, not v1-checkable — a factory reading the clock or a fresh `Random` breaks seed replay, and a 0045 Mode W recording makes the violation *visible* (the stray world read appears as a journal edge where none should be). Values produced by `FromGenerator` have **no shrink candidates in v1** (§6.4).

```bezel
public record Latitude(Degrees: double)
  public static method FromGenerator(g: Generator): Latitude
    return new Latitude(g.NextDouble() * 180.0 - 90.0);    // invariant: -90 <= Degrees < 90
  end FromGenerator;
end Latitude;
```

Composition is ordinary code: a factory calls `g.Next<Field>()` for components it wants derived. Like `FromCollectionLiteral`, the convention is forward-compatible with promotion to a static interface member ([§11.8]) if the recognition set ever grows — the identical zero-source-change promotion path the stdlib already records for `FromCollectionLiteral` and `Deserialize`. *(Owner decision Q3.)*

## 6. Shrinking

### 6.1 The candidate relation, per kind

Shrinking is defined by a per-kind **candidate function**: for a failing value, an ordered, finite list of strictly-simpler values to try. Simplicity is a per-kind well-founded measure, so descent terminates structurally (and a step budget bounds it operationally, §7.5).

- **Integers** — candidates for v ≠ 0, in order: `0`; the positive counterpart |v| (if v < 0); v with its magnitude halved toward zero (truncating); v stepped one toward zero. A candidate unrepresentable in the type — |`MinValue`| of a signed type — is skipped (candidates are computed in the type's domain; spec §6.1's checked overflow never fires in the driver). Measure: (|v|, sign).
- **`double`/`float`** — `0.0`; the truncation of v to an integral value; v halved. NaN's sole candidate is `0.0`; each infinity's candidates are `0.0` and `MaxValue`/`MinValue`. **`decimal`** — `0m`; the integral truncation; v halved.
- **`char`** — treat cp(v) − cp('a') as a signed integer shrunk toward 0 by the integer rule: `'a'` is the designated minimum, letters simplify toward it, and one rule is reused instead of a bespoke order.
- **`bool`** — `true` shrinks to `false`.
- **Enums** — earlier-declared distinct values, in declaration order (toward the first).
- **`string`** — as a `char` sequence under the collection rule below (chunk removal, then per-`char` shrink).
- **Collections** — **element removal first**: drop the first half, the second half, then quarters, then single elements front-to-back (the ddmin-shaped sequence, deterministic); **then element shrink**: each element's candidates tried with the others held, front-to-back. `Dictionary` shrinks by entry removal, then by *value* shrink; keys shrink only via removal (a shrunk key can collide and silently merge entries — legal but attribution-hostile; recorded refinable). `Set` element-shrink candidates that collide with an existing element are skipped.
- **Records** — field-wise, declaration order: each field's candidates with the other fields held.
- **Choices** — first, **earlier-declared cases at their minimum values**, in declaration order — where every generatable type's *minimum* is the designated bottom of its measure (0 / `0.0` / `false` / `'a'` / `""` / the empty collection / `null` for `T?` / first distinct enum value / field-wise minima for records / first-declared case at payload minima for choices); then payload-wise shrink within the current case.
- **`T?`** — `null` first, then the inner value's candidates lifted.

### 6.2 The driver

Greedy first-failure descent: starting from the original counterexample, compute the candidate list; run the property on each in order; the first candidate that **fails** (an `AssertionException` or `[error]` outcome — a discard, and in bridge properties a filtered-out candidate (§8), counts as *not failing*) becomes the current value and the descent restarts from it; when no candidate fails, the current value is **locally minimal** and is reported. The candidate order is normative, the property's verdict is deterministic for pure bodies (and for impure bodies to the §7.3 strata), and no step draws entropy — so **the entire shrink trace is a pure function of the seed and the property**, replayable by `--seed` alone.

### 6.3 Both ends are reported

The failure report (§7.2) carries the **original** failing case and the **minimal** one (plus the step count). For non-pure bodies whose verdict wobbles under re-execution, the shrunk value is best-effort — the original case and seed are the ground truth and are never displaced. Reporting both is the honesty valve that lets shrinking stay aggressive.

### 6.4 Custom-generated values

`FromGenerator` values have no candidates (the harness cannot invent invariant-respecting simplifications), so they pass through shrinking unchanged; components *around* them (other record fields, other collection elements) shrink normally. A recognized companion `ShrinkCandidates` static is the recorded v2 hook, deliberately not designed here (one mechanism per proposal stage).

## 7. Execution, reporting, determinism

### 7.1 Budgets and seeds

Default **100 cases** per property (`cases` at `AddProperty`, runner options, or driver flag override; 0 = default). The **run seed** is 64-bit: supplied by `--seed`, else drawn fresh through the stdlib entropy surface and always printed in the run header. Per-property streams derive as a pinned mix of (run seed, property *name*) — name-keyed so one property's cases are independent of suite composition, registration order, and filtering: `--filter X --seed S` reproduces X's cases exactly as the full run produced them. Per-case state derives from (property stream, case index). The mixing function and all distribution constants are **pinned per runtime revision** (the 0045 §9.1 posture: a revision that changes them is a new determinism domain; seeds do not promise cross-revision stability). The 0008 `HashCode` per-process seed is deliberately *not* used anywhere in this pipeline — it is per-process random by owner ruling, which is exactly what a replayable stream must not be built on.

### 7.2 The failure report

0022 §8.3's format, extended with a `[property]` tag:

```
FAIL SortIsIdempotent [property] (seed 3735928559, case 42, 17 shrink steps)
  counterexample: xs := [0, 0, -1]
  original:       xs := [312, -87, 0, 44, -1, 312]
  Assert.Equal failed.
    Expected: 3
    Actual:   2
  rerun: --filter SortIsIdempotent --seed 3735928559
```

Counterexamples render structurally for derived kinds (element lists for collections; stdlib §2.18 rendering for values; the spec §9.2 auto-`ToString` for records), so the report is paste-able as Bezel literals wherever the type has a literal form — the mechanical path from counterexample to a permanent 0022 example test ("promote the counterexample") is copy from the report. `RunAsync`'s failure count treats a falsified property as one failure regardless of case count.

### 7.3 The determinism contract, stratified — and 0045

> Given the runtime revision, the suite artifact, the run seed, and the budget knobs, the generated argument sequence, the edge schedule, and the shrink candidate sequence are **identical across runs and machines**. Verdicts are additionally identical when the body is `pure` (0042 — no edges exist to vary, 0045 §5.7's own observation) or when the run executes under a 0045 Mode W replay (the body's edges are journal-substituted).

Degradation is graceful and explicit: an impure sync body (file reads, clock) reproduces its *inputs* from the seed alone, and its *verdict* only insofar as the world cooperates; that residue is precisely what 0045 exists for. The runner draws its fresh seed **through the stdlib entropy surface deliberately, so that it is a B2 journal edge** (0045 §3.3): `bezel test --record` (0045 §3.11) captures the seed with no plumbing, and a recorded failing property replays — generation, body edges, verdict — with no `--seed` at all. Without a journal, `--seed` is the contract; with one, the journal is.

### 7.4 Exhaustive small-domain mode (pure-gated)

> When a property is declared `pure` (0042) and every parameter's **domain size** (per §4.2; unbounded kinds count as infinite) has a finite product ≤ the **exhaustive limit** (default **4096**, a knob), the runner enumerates the full cross product in the canonical per-kind orders instead of sampling: every domain value runs exactly once; edge injection and sizing are vacuous; shrinking still applies to a found counterexample (canonical order is not the simplicity order). The verdict line names the mode: `PASS SignRespectsNegation [property: exhaustive, 512 cases]` — a *domain-total* statement: for this build, the property holds at every value of the domain.

`bool` (2), enums (distinct-value count), `byte`/`sbyte` (256), nullable liftings, and small records/choices over these qualify; `short` (65,536) and `char` (1,112,064) exceed the default limit and sample — "small ranges" beyond the structural kinds arrive with the contract bridge's constraint-derived domains, recorded at §8.4. The **pure gate** is load-bearing three ways: an impure body may accumulate state across cases, making a sweep's verdict order-dependent — the total claim would be unsound as a per-case statement; a sweep auto-engaging up to thousands of cases must not be able to do I/O; and a domain-total verdict is defensible exactly when each case's verdict is a function of its arguments and un-interfered state, which effect-freedom (though not full determinism, 0042 §9) delivers in the runner's serial domain. Impure finite-domain properties sample, always. Parallel case execution — which purity also admits in principle — stays deferred with 0022 §6's serial-in-one-domain decision; recorded, not designed.

**An honest tension, recorded.** Under 0042's v1 rules, `Assert.Equal<T>` cannot be `pure`-declared (its failure path renders operands via constraint/`Object`-surface `ToString` dispatch — abstracted-position calls, 0042 §5/§11), and `Assert.Throws` never can (it invokes a function value). The pure-declarable v1 assertion surface is `Assert.True`/`False`/`Fail`/`Null`/`NotNull` plus `pure constructor`s on `AssertionException` — enough for real properties (`Assert.True(cond, $"…{x}")` with primitive-rendered interpolands is fully pure under 0042 §13's wave), but narrower than the full vocabulary. Remedies in order of arrival: 0042's v2 constraint purity unlocks `Equal<T>` for value-shaped `T`; until then, a pure property states its comparison as a `bool` condition. This proposal adds these `System.Testing` members to 0042's annotation wave at its own graduation (§14 Q6) and does **not** relax the gate — an exhaustive verdict earned by weakening purity would be the silent degradation this repo's postures exist to prevent. Note the gate prices only the *exhaustive/parallel* upgrades: ordinary sampled properties require no purity at all.

### 7.5 CI budget knobs, and the quarantine question

```bezel
public record PropertyOptions(
  Cases: int,               // per-property case budget            (default 100)
  ExhaustiveLimit: int,     // §7.4 domain-product threshold       (default 4096)
  ShrinkLimit: int,         // max shrink steps per failure        (default 1000)
  DiscardLimit: int,        // max attempts, as a multiple of Cases (default 10; §8.3)
  MaxSize: int);            // collection/string size ceiling      (default 64)
```

`TestRunner.RunAsync` gains an overload taking `options: PropertyOptions? := null, seed: long? := null` alongside 0022's `filter`; proposal 0040's driver surfaces the same knobs as `bezel test --cases N --seed S --shrink-limit N …`. Budgets bound *time*, never *soundness*: a shrink-limit hit reports the best-so-far counterexample plus the original; a lowered case budget still runs the edge schedule first (§4.3).

**Flaky-quarantine posture.** 0022 has no quarantine mechanism (even `Skip` is deferred, 0022 §9), and this proposal deliberately adds none: a failing property is definitionally reproducible — the seed is in the report — so "flaky property" decomposes into exactly two honest cases. (1) A fresh seed found a real counterexample yesterday's seeds missed: that is the feature working; the response is fix the code or fix the property, pinning the counterexample as a promoted example test meanwhile — never suppression. (2) The failure does not reproduce under its own seed: the *body* is world-dependent, the defect class is 0045's, and `bezel test --record` (0045 §1.2's gate story) is the tool. The seed policy that makes (1) a policy choice rather than an accident: fresh-per-run seeds are **discovery mode** (recommended default — matching this repo's deflake-at-source discipline), a pinned `--seed` in CI is **reproducibility mode**; the owner picks the CI default (§14 Q5).

## 8. The contract bridge (stage S2)

The killer integration: a method that already carries 0043 contracts has already written its property — this stage makes the runner read it.

### 8.1 What is derived, and how it ships

For every **static `pure` method** whose parameter types are all generatable and which carries at least one `requires` or `ensures` clause (0043), the bridge derives one property: *generate arguments satisfying the preconditions; call; assert the postconditions.* It ships as **tool-generated Bezel source** — a generation step on 0040's driver (`bezel test --contracts`) emitting ordinary properties into the test target, discovered like any hand-written one — **not** as language or runner magic. The generated text is inspectable, diffable, and debuggable; the clause expressions are copied from the declaration (visible in source today; carried by the artifact interface at 0043's stage 2 for cross-module targets). Cite-chain: 0043 §7.4 records tiers in the interface; this bridge is their first mechanical consumer.

```bezel
// Source (0042 + 0043):
public static pure method Clamp(x: int, low: int, high: int): int
  requires(low <= high)
  ensures(result >= low)
  ensures(result <= high)
  return (if x < low then low else (if x > high then high else x));
end Clamp;

// Tool-emitted bridge property (fork (a) spelling):
public test pure method Contract_Clamp(x: int, low: int, high: int)
  if not (low <= high) then
    Property.Discard();
  end;
  var result := Clamp(x, low, high);
  Assert.True(result >= low,  "ensures result >= low");
  Assert.True(result <= high, "ensures result <= high");
end Contract_Clamp;
```

### 8.2 Semantics, decided

- **Rejection sampling before the call.** The `requires` conjunction is evaluated as the discard filter *before* invoking the target, so the target's own always-on `requires` check (0043 §6.2) never fires on a generated case. If it fires anyway, the filter and the clause have drifted (a stale generated file) — reported as a distinct harness error naming regeneration, never as a falsification.
- **`old(E)`** in an `ensures` clause is handled by pre-call capture (`var __old1 := E;`) in the generated body — and for a *pure static* target this is provably equivalent to post-call evaluation (a pure body writes nothing, and the runner's serial domain interleaves nothing mid-case), so the capture is form, not necessity. Instance methods — where `old` is load-bearing — are exactly what v1 excludes (§8.5).
- **Shrinking respects the filter**: a shrink candidate failing the `requires` filter is not a counterexample and counts as not-failing in the §6.2 descent, so minimization stays inside the legal domain.
- **Contract modes compose.** Under 0043's checked-contracts mode the target's own `ensures` machinery fires first with the same predicate — same verdict, doubled evaluation, harmless. The bridge's independent value is in *release-mode* builds (where `ensures` is not evaluated, 0043 §6.3) and in the *search* itself: contracts check the inputs that occur; the bridge goes looking.

### 8.3 The exhaustion verdict

Rejection sampling has a failure mode — a narrow `requires` (an equality, a conjunction of tight bounds) rejects almost everything — and it must not become a silent pass. Attempts are budgeted at `DiscardLimit × Cases`; if the accepted count falls short of the case budget when attempts run out, the property's verdict is **`[exhausted]`: a failure**, reporting the acceptance rate and the remedies (a `FromGenerator` factory on a wrapper record encoding the domain; excluding the member from the bridge; widening the budget). Failing by default is the 0043 §7.3 never-silently-degrade posture applied here: "could not exercise the contract" must look different from "the contract held." *(Owner decision Q8 confirms fail-by-default.)*

### 8.4 Recorded refinement: constraint-derived domains

When the `requires` fragment falls inside 0043's stage-3 prover fragment (linear integer arithmetic, null tests, closed-hierarchy case analysis — 0043 §7.3), the bridge *may* derive bounded domains directly — generating `low <= high` pairs by construction, or enumerating `requires(n >= 0 and n < 16)` exhaustively (§7.4's "small ranges", arriving here) — replacing rejection with construction. Recorded as the S2+ refinement it is; v1 of the bridge is rejection-only, honest and simple.

### 8.5 v1 bridge scope

Static + `pure` + generatable parameters only. Instance methods need a receiver — the class-generation problem (§4.1) plus `invariant` interplay (0043 §9); impure statics need effect containment the harness cannot promise. Both are the same v2 frontier as owned-value generation, and the bridge inherits that staging rather than forcing it.

## 9. Interactions (the honest enumeration)

| Surface | Interaction |
|---|---|
| 0022 test framework | Extended, not forked: same `Assert`, same scoring, same runner, same fixture story (§3.1); works under fork (a) via one shape-rule clause (§3.3) and under fork (b) via `AddProperty` (§3.4); reporting extends §8.3's format; `RunAsync` gains an options overload (§7.5) |
| 0041 choices | Proposed-dependency: the choice generator/shrink/enumeration clauses (§4.2, §6.1, §7.4) activate when 0041 lands; value-like classification is the admission gate; nothing else here waits on it |
| 0042 `pure` | Gates exhaustive mode and (future) parallel cases (§7.4); pure properties get the full determinism stratum (§7.3); the Assert-purity tension recorded with remedies (§7.4); the bridge requires target purity (§8) |
| 0043 contracts | The bridge (§8): requires→filter, ensures→assertions, `[exhausted]` verdict; 0043's `ContractViolationException` from a filtered call is a harness error, not a falsification |
| 0045 replay | Seed alone reproduces generation; the run seed is drawn through the journaled entropy edge deliberately (§7.3); recorded failing runs replay whole; `FromGenerator` world-read violations become visible as journal edges (§5) |
| 0040 build driver | Hosts the knobs (`bezel test --seed/--cases/…`, §7.5) and the bridge generation step (§8.1); nothing here blocks on it (the runner's own options suffice) |
| 0028 Random extensions | Convenient for `Generator`'s internals when it lands; no dependency — stdlib §2.7's seeded surface suffices |
| §3.6 derived properties | The generatable classification is §3.6-*style* (computed, per-instantiation) but scoped to compiler-known `System.Testing` members, the `HashCode.Add<T>` precedent (0008 §5.4; `Assert.Equal<T>` moved to the written `where T: hashable` clause, 0022's 2026-08-12 ruling) — not a §3.6 table row (Q2) |
| §9 / §10 records, enums | Positional-storage records compose field-wise; body-storage records require `FromGenerator` (§4.2); enums generate over distinct values, [§10.2]'s aliasing respected |
| §7 nullability | `T?` null-weighted with `null` as an edge; narrowing inside property bodies is untouched |
| §4.3.1 float equality | Reflexive `=` makes NaN-edge round-trip assertions meaningful; IEEE-strict properties spell `Double.IeeeEquals` |
| §14 generics | Properties are non-generic (§3.1); generatable is checked per concrete instantiation at `AddProperty`; no constraint-directed generation in v1 (divergence D3) |
| §15.4.1 / rationale §5.16 | The `FromGenerator` recognition mechanism and its promotion path are `FromCollectionLiteral`'s, transplanted (§5) |
| §28 ownership | The v1 generation set is exactly the no-user-code-on-disposal fragment (§4.1); generated collections are trivial-dispose instantiations; parameters receive borrows; nested fresh trivial-dispose elements are §15.4-admissible |
| §29 / §30 | Sync-only v1 (§3.5); cases run serially in the runner's domain (0022 §6); parallel pure cases recorded-deferred (§7.4) |
| stdlib §2.7 / §4.4 / §4.8 / §4.10 | The entropy substrate (seeded, algorithm pinned per revision §7.1); the three generated collections with their existing hashable instantiation rules; stability-while-stored satisfied by value-shaped keys (§4.2) |
| stdlib §2.18 / spec §9.2 | Counterexample rendering for values (stdlib §2.18) and records (spec §9.2 auto-`ToString`); structural rendering for collections (§7.2) |

## 10. Migration and compatibility

Purely additive. No existing program, test, or 0022 surface changes meaning: `AddProperty`, `Generator`, `Property`, `PropertyOptions`, `CaseDiscardedException`, and the `FromGenerator` recognition are new names in a non-auto-imported module; a `FromGenerator` static existing today on a user type is impossible to collide with (the recognition activates only at generation sites that do not yet exist). Under fork (b) there is no spec delta at all; under fork (a) the shape-rule clause (§3.3) lands with 0022's own `test`-modifier train, and a parameterless suite is untouched by it. Removing a `FromGenerator` factory reverts a type to derivation (or to non-generatable, surfacing as the ordinary admission error at its use sites) — a source-visible, per-type event. Conformance: no Appendix D row — `System.Testing` is stdlib surface, the generation machinery is library+compiler-known synthesis in the [§9.8] category, and a Core toolchain that has not implemented the synthesis rejects `AddProperty`/`Next<T>` instantiations with the feature-naming diagnostic rather than mis-running them.

## 11. Staging

- **S0 — generators + runner, value types.** `Generator` with the §4.2 derivation set minus choices (0041-gated) and minus shrinking; edge injection (§4.3); sizing (§4.4); `FromGenerator` recognition; `AddProperty` (fork (b)); seeds, reporting with `original:` only, `--seed` rerun; `PropertyOptions`. Exit: the determinism contract (§7.3, generation half) demonstrated cross-machine on this repo's suite; every §4.2 kind has spec-cited accept/generate/report tests that fail without the feature per `tests/README.md`.
- **S1 — shrinking + exhaustive mode.** The §6 relation and driver; both-ends reporting; the §7.4 pure-gated exhaustive mode with the canonical orders; choice generation/shrinking if 0041 has landed. Exit: shrink determinism (identical traces for identical seeds) tested; a seeded known-bug property produces the known minimal counterexample; an exhaustive verdict on a byte-domain property is order-stable.
- **S2 — the contract bridge.** The 0040-hosted generator (§8.1), filter/capture semantics (§8.2), filter-respecting shrink, the `[exhausted]` verdict (§8.3). Exit: bridge properties for a demo module catch a seeded `ensures` violation in a release-mode (contracts-unchecked) build; a tight-`requires` method reports `[exhausted]`, not `PASS`. The §8.4 constraint-derived-domain refinement and any fork-(a)-specific spelling ride behind their own dependencies (0043 stage 3; 0022's (a) ruling).

Each stage stands alone; nothing in S0/S1 waits on 0041/0042/0043 (their clauses activate when their proposals land).

## 12. Recorded divergences from the P13 sketch

- **D1 — no `property` declaration form.** P13 says "a `property` declaration is a boolean method." This proposal adds no declaration kind and no keyword: a property is a parameterized 0022 test under *both* discovery forks. Reasons: 0022's discovery fork is an open owner ruling, and a new declaration form would prejudge it while adding a second discovery surface; the parameterized-test shape ships today under (b) and re-expresses mechanically under (a) — the same promotion discipline 0022 §3.1 leans on throughout.
- **D2 — no boolean return.** Assertion is the `Assert` vocabulary; pass is normal completion (§3.2's four reasons: one failure channel, report quality, body consistency, engine uniformity). The expansion mandate directed this divergence; it is recorded here because P13's own text says "boolean method."
- **D3 — "`where` constraints bound domains" is not delivered in v1.** Properties are non-generic and constraint-directed generation is undesigned; the delivered forms of domain bounding are `Property.Discard`, `FromGenerator`, and — at S2 — the contract bridge's `requires` filtering with the §8.4 constraint-derived refinement recorded. The sketch's phrase is aspiration this proposal narrows to a staged path.
- **D4 — the P5 seed dependency is sharpened, not consumed.** P13 says "P5's journal supplies deterministic seeds." In this design the seed is self-sufficient for generation and shrinking with no journal at all (§7.1–§7.3); what the 0045 journal supplies is the *body's* determinism (edges) and free seed capture (the runner draws its seed through a journaled edge on purpose). Degradation without 0045 is graceful and stated, per the mandate.

## 13. Alternatives considered and declined

1. **Bool-returning properties** — §3.2; also 0022 §11's decline-4 lineage (unchecked verdicts silently pass).
2. **A `property` declaration kind / contextual keyword** — D1's reasons, plus grammar cost for zero semantic gain and rationale §18 attribute-adjacency (a single-purpose declaration marker beside `test` where one marker suffices).
3. **First-class combinator generators as the primary surface** (a QuickCheck-style `Gen<T>` value algebra with map/filter/bind). Declined as *primary*: it front-loads higher-order machinery (stored function values, scoped-capture plumbing through generator objects, unverifiable factories under 0042 v1) to serve the 10% case, while derivation serves the 90% case with zero configuration; `FromGenerator` plus `Generator`'s helpers cover the custom cases in plain code. Recorded liftable — a combinator layer composes *over* `Generator` later without touching this design.
4. **Reflection-based derivation** — no reflection exists and the decline is recorded (rationale §18); derivation is compiler-known synthesis in [§9.8]'s category instead.
5. **Internal shrinking** (Hypothesis-style: shrink the recorded entropy stream, not the value). Genuinely attractive — custom generators shrink for free — but declined: shrink *quality* then depends on generation layout being shrink-aligned, a property that cannot be stated normatively or diagnosed when absent; the typed structural relation is specifiable, deterministic, and testable rule-by-rule. The cost — no custom-type shrinking in v1 — is taken openly (§6.4).
6. **Constructor-election generation for classes** (e.g., via `where T: new()`): declined with §4.1's ownership/user-code reasons; the recorded v2 owned-factory direction is the honest path.
7. **Attribute-carried per-property configuration** (`[MaxCases(200)]`) — attributes are deferred (rationale §18); configuration is the `cases` argument and runner options, promotable to attribute spellings if that system ever lands, like every other narrow marker.
8. **Exhaustive mode for impure properties** (auto or opt-in) — §7.4's order/interference/budget argument; an impure sweep's "total" verdict is unsound as a per-case claim, and this proposal refuses to print one.
9. **A flaky-property quarantine** — §7.5: seed-reproducibility makes quarantine either suppression of a real counterexample or a mislabel of a 0045-class body problem; both responses are worse than the two honest ones.
10. **Seeding generation from the 0008 `HashCode` machinery** — per-process randomized by owner ruling; a replayable stream must not touch it (§7.1). A pinned mix is specified instead.

## 14. Open questions / owner decisions

1. **Q1 — the declaration ruling.** Ratify "a property is a parameterized test" under both 0022 forks, including the §3.3 shape-rule clause riding fork (a)'s spec train if approved. This amends 0022 ruling 5's "parameterless" — the one place this proposal edits a recorded 0022 decision.
2. **Q2 — generatable's residence.** Compiler-known-member enforcement only (recommended; the `HashCode.Add<T>` scoping — `Assert.Equal`'s former scoping, before its 2026-08-12 `where T: hashable` re-cut) vs. a full §3.6 table row (heavier: makes a testing concern part of the language's classification vocabulary).
3. **Q3 — `FromGenerator`.** Confirm the name, the one-mechanism recognition (§5), and the no-shrink-in-v1 consequence; or direct the static-interface-member spelling ([§11.8]) immediately instead of as the recorded promotion.
4. **Q4 — budgets.** Confirm defaults: 100 cases, exhaustive limit 4096, shrink limit 1000, discard limit 10×, max size 64; and the fork-(a) asymmetry (no per-property `cases` spelling until attributes) as acceptable.
5. **Q5 — CI seed policy.** Fresh-per-run (discovery; recommended, with the report's rerun line as the reproduction contract) vs. pinned (fully quiet CI) as the recommended default posture in the docs.
6. **Q6 — the pure-assertion wave.** Confirm adding `Assert.True`/`False`/`Fail`/`Null`/`NotNull`, `Property.Discard` (its body is the [0042 §4.5] carve-out shape), and the `AssertionException`/`CaseDiscardedException` constructors to 0042's stdlib annotation wave (the §7.4 tension's near remedy, and what lets a bridge property be declared `pure` at all, §8.1), owned jointly by the two proposals at graduation.
7. **Q7 — distribution constants.** Confirm the fixed weights/edges of §4.2–§4.4 (null 1/8, small-band ½, the per-kind edge lists) as pinned-per-revision implementation constants named in stdlib text, vs. normatively fixed numbers in the spec-facing text.
8. **Q8 — `[exhausted]` fails by default** (§8.3): confirm, or direct a warn-only default with a strict flag.
9. **Q9 — `Property.Discard` via typed exception** (§3.6): confirm the mechanism (the `AssertionException` symmetry) or direct an alternative signaling design.
10. **Q10 — async properties deferred** (§3.5): confirm, with "shrinking under nondeterminism" recorded as the reopening question.

## 15. Relationship to existing work

Future-directions P13 (the mandate; deepened here, divergences §12) · proposal 0022 (the host framework: `Assert`, runner, fixtures, both discovery forks — extended by §3, §7.2, §7.5) · proposal 0041 (choice generation/shrinking/enumeration — proposed-dependency, §4.2/§6.1) · proposal 0042 (`pure`: the exhaustive/parallel gate §7.4, the bridge's target class §8, the assertion-purity tension and Q6) · proposal 0043 (contracts: the bridge §8; "contracts give property tests their oracles" made mechanical) · proposal 0045 (seeds as journaled edges, whole-run replay, the determinism strata §7.3) · proposal 0040 (driver knobs and the bridge generation step) · proposal 0008 (the seed source deliberately *not* used, §7.1) · proposal 0028 (optional internals) · spec [§9.8] (the synthesis category `Next<T>` joins) · [§15.4.1]/rationale §5.16 (the recognition mechanism) · [§4.3.1] (reflexive equality making edge cases assertable) · [§28.9]/[§15.4] (the trivial-dispose line that bounds v1 generation) · rationale §18 (the attribute deferral this design respects at every configuration point).
