# Null lifting in Bezel: investigation and decision

**Date:** 2026-08-20 · **Tracking issue:** [#3294](https://github.com/s8lap/bezel-lang/issues/3294) · **Written against:** spec/stdlib/rationale **v0.266**

**Commissioned by the owner**, who asked whether §7.11's propagation semantics are right and stated a lean toward rejecting the form — compile error or runtime fault. Three independent lanes ran in parallel: one mapping §7.11's surface and consequences inside Bezel, one gathering cross-language evidence, one designing and pricing each option. Each was briefed **not to confirm the lean**, and to report if propagation won. This document records what they found, where they disagreed, and the decision.

---

## 1. The decision

**Reject lifting for arithmetic, bitwise, shift, and relational operators over nullable value types.** Retain §8.4's null-safe equality. Retain `??`, `?.`, `?(args)`, and `??=` unchanged.

Concretely: `a + b`, `a & b`, `a << b`, `a < b` and their compound and `++`/`--` forms become **compile errors** when an operand's static type is a nullable value type, with the diagnostic naming the remedy the language already uses — unwrap with `!` or `??`, or narrow per §7.4.

The rationale, absent since §7.11 was written, is recorded with it. The design lands as **proposal 0114**; this document is the evidence behind it.

Three sub-decisions, in the order the evidence supports them:

| | Verdict | Confidence |
|---|---|---|
| **Relational** (`<` `>` `<=` `>=`) | **Reject** | Decisive — three independent refutations |
| **Compound assignment** on a nullable target | **Reject**, as a consequence of rejecting the underlying operator | High |
| **Arithmetic / bitwise / shift** generally | **Reject** | High, but the weakest of the three |
| **Equality** (`=` `<>`, §8.4) | **Retain** | Deliberate — see §6 |
| **Runtime fault** (the owner's alternative) | **Decline** | Decisive — incoherent, see §5 |

---

## 2. What §7.11 actually does, and why the usual defence fails

The defence of propagation — the one I gave the owner before this investigation, and the one #3294 recorded — is that Bezel is not SQL: §7.4 requires narrowing before a nullable reaches a non-nullable position, so a null cannot silently become a plausible wrong number.

**That defence is false as stated.** It holds for *numbers* — member access, indexing, `for` sources, `Range<T>` bounds, `Console.WriteLine(Object)`, `i += n` into a non-nullable target, and `and`/`or`/`not` were each checked and each holds the line. But the null escapes into `bool` through two **total** exits that require no narrowing at all:

- **L1 — relational lifting yields `bool`, not `bool?`.** §7.11: *"lift to yield **`bool`** — not `bool?` … yields `false` if either operand is `null`."*
- **L2 — §8.4 equality** is null-safe and total.

Composed, they produce a fail-open guard:

```bezel
if (flags & ADMIN) <> 0 then GrantAdmin(); end;   // taken when flags: uint? is null
```

Lifted `&` yields null; `null <> 0` is true per §8.4. The authorization test **passes** on missing input.

Related shapes, all verified against current text:

- `requires(not (retries > Max))` **passes** on null, while the positive form `requires(retries <= Max)` faults — the same contract, opposite outcomes, chosen by spelling.
- `while spent < budget do … end;` never executes when `spent` is null.
- A comparator built from `<` violates stdlib §4.4's total-order requirement, whose stated consequence is *"unspecified permutation"*.

**The spec contradicts itself twice on this.** §7.11's *Safety* paragraph claims *"Lifting never silently produces a non-nullable value from nullable operands"* — refuted by its own *Relational* paragraph twenty lines above. And §18.5 states that in ordering *"a null participant has no defensible result"*, which is why §4.6 makes `string? < string?` an error, while §7.11 says the defensible result is `false`.

**§7.11 also already rejects lifting for `and`/`or`/`not` on `bool?`** — with reasoning that applies verbatim to arithmetic — and records no reason why the two differ.

---

## 3. The cross-language evidence

Eight of eight languages that enforce nullability by **flow analysis** reject lifted arithmetic and lifted ordering. The only lifters are SQL, C#, VB.NET, and F#'s LINQ corner.

**The hypothesis I started with was falsified, and its replacement is sharper.** I expected the predictor to be *designed-in vs. retrofitted* nullability. TypeScript refutes that: it retrofitted null-safety and still rejects lifted arithmetic and ordering (`checkNonNullType` in the relational branch; diagnostics 2531/18047), while permitting `==`. The predictor that survives is **mechanism**:

> Flow analysis ⇒ reject. Runtime value wrapper ⇒ lift.

C# lifted in 2005 because it had `Nullable<T>` as a runtime wrapper and no flow analysis. **Bezel has C#'s representation but not C#'s excuse** — it is squarely in the flow-analysis group and behaves like the wrapper group.

Four findings carry most of the weight:

1. **C#'s own designers declined to repeat it.** csharplang #1865 records Mads Torgersen rejecting lifting for C# 8's nullable reference types: *"Doesn't really make good use of null tracking; just propagates nulls"*, and it would *"flag the problem 'at the end' rather than on the first undesired null"*. That is #3294's sticky accumulator, named by the designers of the language Bezel inherited lifting from, as a reason **not** to lift.
2. **Swift shipped lifted ordering and removed it in a breaking release** — SE-0121, Swift 3 — having concluded it was easy to write bugs with. Swift's semantics were *strictly better* than Bezel's (`.none < .some` is a genuine total order, where Bezel collapses to `false`), so the evidence applies a fortiori. SE-0121 also names implicit `T → T?` promotion as the accelerant; **Bezel has that promotion** (§5.1, blessed by §7.11's mixed-operand rule).
3. **Bezel's `<` is worse than SQL's, not merely as bad.** With a null operand SQL's `NOT (x < 5)` stays `unknown` and the row drops; Bezel's `not (x < y)` collapses to `false` and negates to a definite **`true`**, so the guarded branch fires. SQL's three-valued logic is precisely what protects SQL there. Bezel took C#'s two-valued collapse, which removes the protection while keeping the propagation.
4. **SQL itself declines propagation at accumulation** — *"All these functions ignore null values in their aggregated input"*. The canonical propagating language carves out exactly the weak point #3294 identified. Rust makes propagating-sum an opt-in type annotation. Neither defaults to poisoning.

---

## 4. Where the lanes disagreed, and how it was settled

The surface lane and the design lane **disagreed on relational lifting**: reject outright, or retain at `bool`.

The retain case was not unreasonable. Its argument: §19.3 relational patterns and §16.6 range patterns ride on §7.11's relational lifting, so dropping it forces a second ruling in §19.3; and the headline fail-open example is closed anyway by rejecting bitwise lifting, since `flags & ADMIN` never produces the null.

That second point is correct and worth recording — rejecting arithmetic and bitwise **does** close the `(flags & ADMIN) <> 0` shape at the `&` step. But it does not reach the residue: `while spent < budget`, `requires(not (retries > Max))`, and the comparator case involve no arithmetic at all and survive untouched.

The cross-language lane settled it. Relational lifting is where the evidence is **strongest**, not weakest: it is the construct a peer language shipped and then **removed in a breaking release**, with better semantics than Bezel's, and it is the construct where Bezel is worse than the cautionary tale. The §19.3/§16.6 dependency is a **cost to pay**, not a reason to keep it, and proposal 0114 carries that companion ruling explicitly rather than discovering it later.

**Decision: reject relational lifting.**

---

## 5. Why the owner's runtime-fault option is declined

The owner offered a runtime null-value fault as an alternative to a compile error. It is declined, and the reason is coherence rather than cost — it would need **no new exception type**, `NullReferenceException` fitting §31.4's admission principle.

**A faulting lifted operation can never yield null.** If `a + b` faults when either operand is null, its result type is `T`, not `T?` — which makes option 4 **option 3 with the diagnosis moved from compile time to production**. Every program it accepts, option 3 accepts or rejects at build; every program it faults on, option 3 rejects with a compiler message.

It also contradicts three landed postures: Appendix C.1 (*"A nullable value may not be dereferenced without a null check or `!`"*), §7.8's explicit-postfix-`!` design (option 4 is an unspelled `a! + b!`), and the pattern §5.4 and §6.5 both follow of taking static rejection **wherever available** and faulting only on the residual. No surveyed language faults at an operator; Ada faults only at a dereference, where static rejection is impossible, and spent Ada 2005 moving that check *earlier*.

**The narrow option 2 is also dead**, and it was the one I judged most plausible before the investigation. It contradicts a recorded decision — rationale §11.6 (v0.254): *"Rejecting sugar whose normative expansion is legal would carve §6.6's equivalence for no semantic gain."* And its premise fails independently: the sticky accumulator is reproduced verbatim by `total := total + p;`, which option 2 leaves legal. A speed bump in front of an unlocked door.

---

## 6. What is retained, and why

**§8.4 equality (`=`, `<>`) stays.** Equality over a nullable has a defensible total answer — two nulls are equal, null and a value are not — which is exactly what §18.5 denies for *ordering*. Every flow-analysis language in the survey that rejects lifted ordering still permits `==` (TypeScript and Kotlin explicitly). Equality is how a program *tests* for absence; removing it would leave `??`/`?.`/narrowing as the only spellings and make null-checking harder, not safer.

**`??`, `?.`, `?(args)`, `??=` stay unchanged.** They are the sanctioned propagation forms, and they are *marked*: the `?` in the token tells the reader propagation is happening.

That observation is the sharpest framing the investigation produced, and it reframes the whole question. **Every propagating construct in Bezel already carries `?` in its spelling** — `?.`, `?(`, `??`, `??=`. Lifted `+` and lifted `<` are the **sole unmarked exceptions**. So the principle being enforced is not *"propagation is wrong"* — it is *"unmarked propagation is wrong"*, and rejecting the two unmarked cases makes the language's own convention exceptionless.

(F# offers the fifth option nobody listed: keep the semantics but spell them `?+?` and `?<?`. It is a real design and it fits the convention above. It is declined for surface cost — two operator families to serve a case the survey shows is better written with `??` — but it is recorded in 0114 as the alternative with a reopening bar, not dismissed.)

---

## 7. Cost

**Migration cost is zero across every artifact that isn't the compiler's own test suite.** Counted, not estimated: zero sites in the spec's examples (§7.11 has no code block; §6.6's has no nullable targets), zero in the stdlib, zero in all six guides, and zero in all 571 conformance cases. Twelve corpus files declare a nullable value type; none performs lifted arithmetic, lifted comparison, or compound assignment on one.

Two feared blast radii turned out empty:

- **Generics.** `a + b` on a type parameter resolves only through static-interface constraints (§25.2), and a nullable value type satisfies no type-bound constraint (§7.10) — so lifted arithmetic in a generic body is **already** a compile error at declaration. No §3.6 rule-2 per-instantiation carve-out is needed; the rule is body-local.
- **Contracts.** `requires(port in 0..65535)` over a nullable is **already** rejected, because `Range<T>.Contains(value: T)` takes a non-nullable `T`. Rejection costs v0.263's purity win nothing.

Costs that are real: ~14 lit acceptance files and a bounded set of gtest blocks, plus the §19.3/§16.6 companion ruling. Codegen is a **net deletion**.

**The decisive datum on cost is that Bezel already pays it.** `src/sema/ExprTypecheck_Exprs.cpp:4443–4482` already rejects lifting for **user-defined** operators, with the diagnostic *"unwrap the operand(s) with `!` or `??`"*. The rule, the diagnostic, and the migration idiom are all shipped. Today `total += price` silently poisons for `decimal?` and is a compile error for `Money?` — and nothing records why the two differ.

This is also the last moment the change is free. Bezel has no external users; the only migration is to the project's own artifacts, which is zero.

---

## 8. What would change this decision

Recorded so the decision is falsifiable rather than merely argued:

- **Evidence that Swift regretted SE-0121.** The rejection rationale for SE-0123 and the 2022 "Optional comparison revisited" thread are both on `forums.swift.org`, which this session's egress proxy blocked. If Swift reversed itself, finding 2 weakens substantially. **This is the one gap worth closing before 0114 is accepted.**
- A demonstration that the `??`-and-narrow idiom is materially worse than lifting for a real Bezel program — the survey found the explicit forms push code toward a better shape, but that was assessed on other languages' corpora, not Bezel's.
- A consumer for whom the §19.3/§16.6 companion ruling proves genuinely expensive.

Also unsourced, and listed honestly: C# 2.0's LDM notes are not public, so C#'s *original* lifting rationale is inferred from the mechanism rather than quoted; VB.NET, Scala's `Ordering.Option`, and SQL `CHECK`/`UNKNOWN` polarity were not independently verified.

---

## 9. Method

Three Opus lanes, run in parallel, each read-only and each briefed to attack the owner's lean rather than confirm it. Sources were pinned to primary artifacts (ECMA-334 draft-v8, swift-evolution SE-0121/SE-0123, dart-lang/language NNBD spec, rust-lang `option.rs`, PostgreSQL SGML sources, Kotlin spec, ziglang langref, dotnet/fsharp `Nullable.fs`, microsoft/TypeScript `checker.ts`); where the proxy blocked a host, GitHub-hosted copies of the same artifacts were quoted instead, and every substitution is recorded in the cross-language memo.

No claim here rests on the compiler having been run: **this container has no usable LLVM**, so every implementation statement is a source read with a file and line.

The two lanes that disagreed did so on the record, and the disagreement is reported in §4 rather than smoothed away — the retain case was argued from a real cost, and it lost to evidence rather than to a vote.
