# Future directions: a prioritized program of language changes — 2026-07-18

**Status:** supporting record (owner-requested review; not a numbered proposal — individual items graduate to numbered proposals on owner acceptance)
**Written against:** spec v0.197, stdlib/rationale/guides at the same revision
**Author:** Claude (Fable 5), at the owner's request, synthesizing two reviews: a whole-language design critique and a discussion of what a language should be in a future where AI regenerates implementations cheaply and the durable human artifact is the *contract* the implementation is verified against.
**What this is:** a prioritized list of language changes and additions I believe Bezel should make or seriously consider, each deepened with motivation, existing seeds in the language, a concrete sketch, the key open design questions, and staging. Nothing here is normative; every item that proceeds goes through `spec-change-process.md` as its own proposal with its own critique pass.
**Expansion (2026-07-18, same day):** each P-item below has been expanded into a full numbered proposal — P1→0041, P2→0042, P3→0043, P4→0044, P5→0045, P6→0046, P7→0047, P8→0048, P9→0049, P10→0050, P11→0051, P12→0052, P13→0053, P14→0054 (tracking issues #2028–#2041). The numbered proposals are the deep, implementable versions; this document remains the program-level overview and rationale. Where a numbered proposal diverges from its sketch here, the divergence is recorded in that proposal and the proposal governs.

**What this is not:** a defect list (tracked issues cover that), or a relitigation of recorded syntax decisions (see §16, "Deliberately not proposed").

---

## 0. The thesis, in one paragraph

When producing an implementation costs nearly nothing, the implementation stops being the asset and becomes a build artifact — *if* there is something durable to verify it against. Verification has always been asymmetric (checking is cheap, producing is expensive), and AI collapses the production side, which makes machine-checkable semantics **more** valuable, not less. Bezel is already unusually well positioned: ownership, nullability, and sendability are obligations stated in signatures; meaning is static, local, and deterministic (serial-per-domain); there is no macro or reflection layer to make meaning context-dependent; and the project itself already develops contract-first (a normative spec, AI-regenerated implementation, spec-cited tests). The gap is that Bezel-the-language does not yet give its *users* the machinery Bezel-the-project runs on: a way to state behavioral intent beyond types and ownership, have the toolchain check it at a declared level of rigor, and carry it in the module interface where regeneration must preserve it. The program below closes that gap, repairs a handful of recorded rough edges, and protects the one asset that makes any of it possible — rules simple enough to mechanize.

---

## 1. The priority table

Tier 1 is the strategic program (what makes Bezel future-oriented). Tier 2 is targeted repairs of current rough edges. Tier 3 is toolchain work that multiplies everything else. Within a tier, order is recommended execution order; dependencies are noted.

| # | Item | Tier | Size | Depends on | One-line payoff |
|---|------|------|------|------------|-----------------|
| P1 | `choice` types (closed sums with payloads) | 1 | L | — | Ends the `bool` + `out owned` + `when true` contortion; the natural spelling for expected failure |
| P2 | `pure` methods | 1 | M | — | The verification substrate: contracts, property testing, SMT, and parallel combinators all want it |
| P3 | Behavioral contracts (`requires`/`ensures`/`invariant`), tier-tracked | 1 | XL | P2 | The regeneration acceptance test, in the signature |
| P4 | Contracts and purity in the artifact interface + behavioral diff | 1 | M | P3 | The human-owned seam: module contracts survive; bodies become regenerable |
| P5 | Deterministic record/replay + differential verification | 1 | L | — | The pragmatic oracle: "behaves like yesterday" made sound, not heuristic |
| P6 | Ownership-kernel consolidation revision | 1 | L | — | Restate §28/§30.5 case law as derivations from a small kernel |
| P7 | Mechanized core semantics (ownership + domains) | 1 | L | P6 | The trust anchor; finds the next consume-site-freeze-class hole before shipping |
| P8 | Explicit factory ownership (retire the static-return heuristic) | 2 | S | — | Ownership no longer silently keyed on staticness |
| P9 | Runtime `const` locals (+ two more deferred-list adoptions) | 2 | S | — | Single-assignment locals; narrowing that never re-widens |
| P10 | Labeled `break`/`continue` | 2 | S | — | Multi-level loop exit without helper-method ceremony |
| P11 | Pattern A/B ergonomics: guidance flip now, lifetime-extension study later | 2 | M | — | Unblocks the most common await idiom for the target audience |
| P12 | Machine-grade diagnostics + compiler query API | 3 | M | — | The compiler as the oracle in agent loops (including this project's own lanes) |
| P13 | Property-based specification (extends proposal 0022) | 3 | M | P2, P5 | The cheap middle layer between types and proofs |
| P14 | Traceability conventions (contract-to-code citations) | 3 | S | — | Auditable regeneration: what was rebuilt against what |

---

# Tier 1 — the strategic program

## P1. `choice` types: closed sums with payloads

### Motivation

Bezel's own error-handling guide steers users toward "return the outcome, don't throw" for expected failure — and then the language makes the outcome-return awkward. The current encoding of "value or failure-with-information" is the Try pattern: a `bool` return plus an `out owned conn: Connection? when true` parameter plus `out using var` at the call site (Guide 2 §5) — three interacting features, including a bespoke nullability-postcondition clause (spec §22.4, v0.183), invented largely to express what a sum type expresses directly. Nullable returns (`T?`) cover presence/absence but cannot carry *why*. `sealed abstract` hierarchies (spec §19.6) cover open-ish class sums but are heavyweight for a return type: three class declarations plus heap allocation for what should be a small value.

The rationale defers sum types because "the design space (Swift, Rust, Scala 3) is large and unsettled" (rationale §18). I think that assessment has expired: the space has converged — closed nominal sums, payload-carrying cases, exhaustive matching — and Bezel already implements every hard sub-piece: exhaustiveness analysis over closed sets (§19.6), positional destructuring patterns (§19.3), derived type properties computed from component types (§3.6, §9.7), and even the move-only value-type category (§15.3), whose future extension the rationale explicitly left open ("a future revision may extend the move-only category if user-defined move-only types prove necessary" — rationale §5.14). This is that necessity.

### Sketch

A `choice` is a closed nominal sum: an enum whose constants may carry payloads. It slots into the §3.1 type-kind taxonomy between enums and records.

```bezel
choice FetchOutcome
  Fetched(response: owned Response),     // owned class payload — the value carries a disposal obligation
  NotModified,                           // payload-free case — enum-constant-like
  Failed(status: int, message: string)   // value payloads — freely copyable
end FetchOutcome;
```

Construction is qualified, like enum constants (§10.1): `return FetchOutcome.Failed(504, "upstream timeout");`. Consumption is `case`, with the existing positional-destructure syntax and full exhaustiveness (no `_` needed when every case is covered):

```bezel
case client.Fetch(url) of
  Fetched(var r)          => Render(r),
  NotModified             => RenderCached(),
  Failed(var s, var m)    => ShowError(s, m)
end;
```

Generics make it pay: once the kind exists, the stdlib ships `Result<T, E>` (and nothing else — see "what `choice` is *not* for" below).

### Ownership integration — the part only Bezel has to solve, and the part Bezel is best equipped to solve

The classification of a `choice` type is **derived from its payloads**, exactly as a record's borrow-bearing status is derived from its fields (§9.7) and as `OwningBuffer<T>` dispatches on its type argument's category (rationale §5.15's recorded "type-parameter-category-dependent specialization" precedent):

1. **Value-like** — all payloads are values, `string`s, or runtime-managed: the choice is a copyable value type. `Failed`-style outcomes cost nothing.
2. **Borrow-bearing** — some payload carries a borrow: the choice gets record-style lifetime tracking (§9.7), with the same storage restrictions.
3. **Owning** — some payload is `owned` class-typed: the choice is a **move-only value type** (§15.3's category, extended as §5.14 anticipated). It carries a disposal obligation; disposing it disposes the active case's owned payloads; exactly-once disposal follows from the existing affine machinery with no new runtime concept.

Payload binding follows the borrowing/owning split the language already teaches for iteration (Guide 1 §9): matching a choice you *borrow* gives payload borrows (the §19.3 non-destructive rule, unchanged); matching a choice you *own*, in an owned-consuming position, transfers owned payloads into the bindings — the same announced-consumption logic as owning-iterator yields (§29.2 "Iterator yield ownership") and Pattern B await (§30.5). The adopt idiom (`using var resp := owned r;`) then applies as everywhere else.

### Key design questions (to be settled in the numbered proposal)

- **Representation:** value-typed (inline tag + payload union, my recommendation — outcome returns should not heap-allocate) vs. class-backed for the owning classification. The move-only category makes the value form viable even with owned payloads.
- **Generic payload categories:** `Result<T, E>` with class-typed `T` lands in classification 3 per instantiation; the §5.15 category-dispatch precedent covers this, but the rules must be written for user-visible types rather than three known stdlib types.
- **Consumption form:** the exact rule for when a `case` over an owned choice consumes (my recommendation: scrutinee in an owned-consuming position ⇒ owning match; otherwise borrowing match — mirroring §30.5's operand rules).
- **Keyword:** `choice` (recommended; unused as a stdlib identifier, satisfying B.3) vs. `union` vs. widening `enum`.

### What `choice` is *not* for

`T?` remains the option type — do not ship `Option<T>`; two spellings of absence would be a wound. Open extensible hierarchies remain `sealed abstract` classes. And `choice` should *not* trigger a rework of exceptions: the §31 exception system stays for the unexpected; `choice` is the spelling for expected failure the guides already recommend.

### Payoff inventory

- `Try*` APIs gain direct-return forms; the `out owned … when true` machinery becomes one legacy pattern instead of the load-bearing idiom.
- Parser/validator/protocol code — Bezel's stated domain — gets its natural return shape.
- P3 contracts get better vocabulary: `ensures` over a `choice` return states per-case guarantees.
- P13 property generators compose over choices mechanically.

Size: **L** (type-kind addition: grammar, §3.1 taxonomy, §19 patterns, ownership classification, ABI/layout, stdlib `Result`). Nothing in it requires new theory — every ingredient exists in the language today.

---

## P2. `pure` methods

### Motivation

Ownership is Bezel's resource axis; the missing verification axis is *what can this call touch*. Every downstream item wants it: contract clauses (P3) must not have side effects; property tests (P13) over pure functions can be generated, shrunk, and replayed mechanically; SMT-backed proof works on the pure fragment; `Parallel.Map` bodies (spec §30.14's data-parallel combinators) and `FrozenArray<T>` consumers (stdlib §4.19) are safe precisely when pure. The language already has the compile-time sibling — §17.14's constant expressions define an evaluable subset — and the precedent of *declared, compiler-verified* obligations in signatures is Bezel's entire identity.

### Sketch

```bezel
public pure method Median(xs: Array<int>): int
  ...
end Median;
```

v1 rules (deliberately coarse):

- May read anything reachable (fields, statics, parameters — reads are not effects in v1).
- May not: assign any field, static, or `ref`/`out` target; call any non-`pure` method; perform I/O; construct or interact with tasks, domains, channels, or events; `throw` is permitted (a pure method may fault — purity is about effects, not totality).
- May allocate **records and values**; class allocation is excluded in v1 (constructors and destructors run arbitrary code; admitting them requires pure-constructor analysis — deferred, not declined).
- Constant expressions (§17.14) are pure by construction; `pure` is checked by the same bottom-up call-graph discipline.

Declared, not derived: unlike `sendable` (§3.6), purity on a *method* is an API promise the author opts into — the rationale §12.6 "observations, not obligations" principle applied correctly to the method surface (a derived-purity scheme would make adding a log line to a leaf method a silent API break for every transitive caller).

### The known hard edge

Higher-order purity: `Select(selector: (T) -> R)` cannot claim purity without knowing the lambda's. The slot exists structurally — function types already carry `owned`/`ref`/`out`/`scoped` modifiers as part of type identity (§27.1) — so `(int) pure -> int` is the natural extension, but it doubles function-type vocabulary along a new axis and should ship *after* first-order `pure` proves its worth. v1: a `pure` method may invoke a function-typed value only if its type carries `pure` — which in v1 means, in practice, it usually doesn't take one. Honest and staged.

Size: **M**. Keyword `pure` is contextual-eligible (unused as a stdlib identifier). The main cost is the stdlib annotation sweep (`Math`, `string` operations, comparers), which is mechanical and is itself a purity audit worth having.

---

## P3. Behavioral contracts: `requires` / `ensures` / `invariant`, with tier-tracked enforcement

### Motivation

This is the flagship bet. If regenerating a method body becomes cheap, the durable artifact is the signature *plus its behavioral contract*, and the regeneration loop's acceptance test is "re-verify the body against the contract." Bezel has already prototyped the idea without naming it: the `when true` postcondition on `out` parameters (§22.4) is a contract clause — a machine-checked behavioral statement in the signature that caller-side analysis consumes. `Verify.That` (proposal 0031) chose always-on runtime checking. Checked disposal mode (§28.2) established the pattern of a declared-rigor runtime tier. P3 generalizes all three into one mechanism instead of letting the language grow more one-off postcondition clauses.

The design lineage to follow is **SPARK/Ada**, not Dafny: a pragmatic industrial language with a verifiable subset that grows, where every contract is *at least* a runtime check and *at best* a discharged proof — never a proof burden imposed on code that didn't ask for it. The cautionary lineage is C# Code Contracts, which died of being bolted on: library calls instead of grammar, no compiler ownership, tooling rot. Contracts must be in the grammar, checked by the compiler, and carried in the artifact interface — or they will not survive.

### Sketch

```bezel
public method Transfer(to: Account, amount: int)
  requires(amount > 0)
  requires(amount <= _balance)
  ensures(_balance = old(_balance) - amount)
  ...
end Transfer;

class Ledger
  invariant(_debits.Count = _credits.Count);
  ...
end Ledger;
```

Note how well the existing surface serves this: `=` is already equality (§17.3), so clauses read as mathematics — the Pascal-heritage choice pays an unexpected dividend here. `result` (the return value) and `old(expr)` (prestate) are contextual keywords valid only inside clauses; neither collides with stdlib identifiers (B.3-compliant; stdlib's `SetResult` is PascalCase and unaffected).

**Contract expressions are pure borrows.** A clause may not consume, dispose, move, or mutate — enforced by P2's purity discipline plus the existing ownership classification (the compiler already knows which expressions consume; contracts simply reject those). This is why P2 precedes P3.

**Tiered enforcement — the load-bearing design point.** Every clause lands in exactly one tier, and the tier is *recorded* (visible in the artifact interface, P4):

1. **Proven** — the compiler (later: an SMT-backed checker over the pure fragment — linear integer arithmetic, null tests, enum/choice case analysis) discharges the clause statically. Zero runtime cost.
2. **Checked** — compiled as a runtime check. Recommended defaults: `requires` always on (it replaces the `throw new ArgumentException` idiom and, like `Verify.That`, is never elided — a violated precondition is a caller bug you want caught in production); `ensures`/`invariant` on in checked-contracts mode (the §28.2 pattern), because postconditions run on every return path and hot paths must be able to opt down *visibly*.
3. **Assumed** — stated, not checked (FFI boundaries, clauses over excluded expressions). Legal but loud: recorded in the interface as assumed.

A `static` qualifier (`static ensures result >= 0`) *demands* tier 1: compile error if unprovable. This is the SPARK-style growth path — codebases ratchet clauses from checked to proven without changing their meaning.

**Unifications, so the language contracts (sic) rather than grows:** §22.4's `when true` becomes sugar for an `ensures` clause over the `out` parameter (one mechanism, retroactively); the `requires`-violation exception should *be* `ArgumentException`-family so existing `catch` code is unaffected (open question: a dedicated `ContractViolationException` for `ensures`/`invariant`, which indicate callee bugs, not caller bugs).

**Inheritance:** overrides inherit base contracts and may weaken `requires` / strengthen `ensures` only — behavioral subtyping, the same direction discipline as §12.9 variance. Class invariants hold at public-method entry and exit and at constructor exit; private methods and `this`-reentrant calls are exempt (the Eiffel re-entrancy lesson, to be worked out fully in the proposal — this is the hardest sub-design, and it is severable: `requires`/`ensures` can ship a revision before `invariant`).

### Staging

1. Grammar + `requires`/`ensures` with purity restriction, runtime tier only. (Immediately useful; no prover.)
2. Artifact-interface carriage (P4) + the tier record.
3. The provable fragment + `static` qualifier.
4. `invariant`.

Size: **XL** across all stages, but stage 1 alone is an **M** with standalone value. This is the item where "AI writes the proofs, humans audit the contract" becomes concrete: stage 3's proof burden is exactly the kind of labor that is now cheap, while the clause a human reads stays one line.

---

## P4. Contracts and purity in the artifact interface; behavioral diffing

### Motivation

§2.8.2 already made the module interface a *semantic* requirement — the v0.188 module-identity work (proposal 0034) is the seam this needs, extended one level. If contracts (P3) and purity (P2) are part of the artifact interface, then the interface is a behavioral contract for the module, and two things follow: a regenerated module body must verify against an unchanged interface, and an interface *change* can be mechanically classified.

### Sketch

- The artifact interface (§2.8.2) carries, per public member: signature (today), contract clauses with their tiers (P3), and purity (P2).
- A toolchain verb — `bezel diff-api old new` (riding proposal 0040's driver) — classifies every interface delta:
  - **Behavior-preserving:** clause tiers improved (checked → proven), purity added, `requires` weakened, `ensures` strengthened.
  - **Widening:** new members, new overloads.
  - **Breaking:** everything else — `requires` strengthened, `ensures` weakened, purity removed, ownership annotations changed.
- The classification is the Liskov direction applied at the module boundary, consistent with P3's override rule.

The regeneration story this enables, stated plainly: *humans own module interfaces and their contracts; implementations behind an interface are regenerable; the build verifies the seam.* This is the layered-contract architecture — types and ownership (layer 0, shipped), purity (layer 1, P2), clauses (layer 2, P3), module behavior (layer 3, this item) — with each layer's presence and rigor legible.

Size: **M**, mostly toolchain, once P3 stage 2 defines the carriage format.

---

## P5. Deterministic record/replay and differential verification

### Motivation

Proofs will not be the workhorse verification for regenerated software; *differential testing* will be — run old and new implementations against recorded inputs and compare. Most real software's contract is "behaves like yesterday, plus this one intended change." For most languages that oracle is heuristic, because execution is nondeterministic. Bezel's runtime model makes it potentially *sound*: serial-per-domain execution (§30.13) plus the sendability invariant (§30.14 — no mutable state reachable from two domains) means **all** nondeterminism enters a domain through an enumerable set of edges. Nobody owns this niche. Bezel can.

There is also a same-week internal payoff: the project's own gate fights flaky tests and contention artifacts; sound replay makes test nondeterminism reproducible or impossible.

### Sketch

Enumerate the nondeterminism sources at the domain boundary — the model already confines them: channel receives (order and payload), task settlements (order, results, faults), timer fires, `DateTime.Now`/`UtcNow`, `Random`, environment/args, and FFI call results. Then:

- **Record mode:** the runtime journals each domain's inputs at those edges.
- **Replay mode:** the journal substitutes for the live edges; execution is bit-identical or the runtime reports the first divergence (which is itself the diagnostic).
- **Differential mode:** run implementation A recording, implementation B replaying; compare outputs and journals. This is the regeneration oracle.

Spec/stdlib obligations (the audit this item forces, valuable in itself):

- A normative determinism statement in §30.13: *given identical journal inputs, a domain's execution is deterministic.* Everything that would violate it must be either pinned or journaled.
- Audit `Dictionary`/`Set` iteration order: if implementation-defined today, either pin it or place it behind the journal.
- Seeded hashing (proposal 0008's per-process `HashCode` seed) must be journal-captured.
- `Random` and clock reads become journalable intrinsics — no API change, an interception point.

Size: **L**, almost entirely runtime and stdlib; the language-spec delta is one normative paragraph plus the audit's consequences. No user-facing syntax at all.

---

## P6. The ownership-kernel consolidation revision

### Motivation

The whole-language review's central criticism, promoted here to a work item because the AI-era framing makes it strategic rather than aesthetic. §28 and §30.5 increasingly read as case law: the consume-site freeze, the announced-kill family, the full-expression boundary, the field-restoration rule, three kinds of Pattern A result slot, a nine-bullet admitted-await-positions list, each individually justified, collectively unpredictable — a reader cannot derive the rule they haven't read. Rules that derive from a small kernel can be *mechanized* (P7), *explained* (P12's derivation-based diagnostics), and *safely extended*; case law resists all three. The "learnable in full" promise (rationale §1) is the language's justifying bet, and it needs active defense.

### Sketch

An editorial-plus-formal revision, explicitly **not** intended to change semantics (every divergence found is a bug in one of the two texts, resolved as its own recorded decision):

1. State the kernel once: ownership states and the lattice (§28.3), discharge, borrows-with-sources, frames-as-values (§28.3's deferred-activation story, §5.25), announced kills, the consume-site freeze as a single principle.
2. Re-derive the case lists (§30.5's admitted positions, the §28.8.1 restoration rule, the §28.14 admissions) as corollaries, each tagged *derived* or — where genuinely axiomatic — *axiom, with reason*.
3. Acceptance test: every normative sentence in §28/§30.5 is either a kernel rule, a derivation, or a recorded axiom. Count the axioms; that number is the honest complexity budget, and future rulings must state which axiom they extend.

Size: **L** (it is a full adversarial pass over the two hardest chapters), but it is the prerequisite that makes P3's static tier and P7 tractable, and it is the kind of work this project's process is unusually good at.

---

## P7. Mechanized core semantics

### Motivation

"Verified against the contract" has to bottom out somewhere; for a language, that is a mechanized model of its kernel. This used to be a multi-year academic project; AI labor changes the calculus — and the payoff is not future-signaling, it is *near-term bug-finding*. The record shows it: the §30.14 isolation claim was false until the probe-confirmed race forced the v0.180 consume-site freeze (rationale §5.1); the run logs are a chronicle of soundness holes found empirically by adversarial probing. A model finds the next one before it ships.

### Sketch

- Scope: the P6 kernel — a core calculus with locals/fields/moves/borrows/scopes/exception edges, plus domains, sendability, and channels. Not the surface language.
- Theorems: exactly-once disposal on every path; no silent use-after-consume (checked mode faults deterministically); domain isolation (no mutable state reachable from two domains); frame-residence soundness (§5.25's rules).
- Vehicle: Lean 4 (or equivalent); staged — (1) the calculus on paper in this folder, (2) the mechanization with the theorems, (3) a process rule: a spec change touching §28/§30 updates the model or records why it is out of scope.

Size: **L**, heavily AI-parallelizable, zero user-facing surface. The output is trust — the commodity everything in Tier 1 trades in.

---

# Tier 2 — targeted repairs

## P8. Explicit factory ownership: retire the static-return heuristic

Today a plain (non-`owned`) class-typed return is a **borrow** from an instance method but **treated-owned-with-a-warning** from a static method (§28.3 producers; rationale §5.7). Core ownership semantics silently keyed on staticness has two costs: it is a rule users must memorize rather than derive, and it makes "change an instance method to static" a *silent semantic change at every call site* — a refactoring hazard in exactly the layer that must not have them (and a trap for AI-regenerated code, which refactors constantly). The v0.60 warning was the right bridge; the destination is: **a static method returning a class type must write `owned`** (or restructure). Migration is mechanical — every affected site already carries the warning. One deliberate diagnostic follows: the error message teaches the fix (`add owned to the return type`).

Size: **S** (spec: one producers-table row plus §5.7 amendment; compiler: warning → error under a deprecation revision).

## P9. Runtime `const` locals — plus two more adoptions from your own deferred list

The rationale already names the design (§17.8, §18): extend `const` to runtime-evaluated single-assignment, keeping the compile-time-evaluable subset (§17.14 decides membership) as the only form usable in patterns and default values. No new keyword, source-compatible, and the honest footnote in §17.8 — no-op-disposal `using var` as the "admitted readonly spelling" — stops being needed. One payoff worth stating because it is *checkable*, not aesthetic: a `const` local cannot be mutably captured, so its nullability narrowing is **never invalidated** by §7.4's re-widening rule (v0.182) — the copy-to-a-local idiom the guides teach becomes airtight when the copy is `const`.

Batch two more smalls from §18 into the same revision, since each is one decision plus modest text: **`where T: sendable`** (decide the general derived-property-as-constraint rule once, as §18 itself asks) and **`Channel<T>.ReceiveAll()`** (the `await for` termination shape §18 already sketches — and P5's replay journal wants channel consumption flowing through one structured edge anyway).

Size: **S** each.

## P10. Labeled `break` and `continue`

Multi-level loop exit currently requires extracting a method and using `return` (rationale §17.3) — which, in a class-only language with no block expressions, moves the loop's logic away from its data for control-flow's sake. Labeled break is the middle ground every modern comparator chose (Rust, Kotlin, Java); it is forward-only and structured, sharing none of `goto`'s costs, so §17.3's decline of `goto` proper stands untouched.

The spelling can ride machinery v0.170 already built: a loop gains an optional *name*, and the §1.6 closer rule ("`end` may repeat the construct's name if it has one") then applies to loops with zero new closer rules:

```bezel
scan: for var row in rows do
  for var cell in row do
    if cell.Bad then break scan; end;
  end;
end scan;
```

Size: **S** (grammar: optional label on `for`/`while`; §21.5 text; the closer rule composes as-is).

## P11. Pattern A/B ergonomics: flip the guidance now; study lifetime extension separately

The most common first contact with Bezel async — `var x := await FetchAsync(url);` — is a compile error under Pattern A, and the sanctioned two-line form plus the sequence of admissions relaxing it (v0.182 temporaries, v0.184 plain owned locals, v0.193 borrowed parameters) is evidence the rule fights the dominant idiom. Two moves, separated deliberately:

**Now (no language change):** invert the producer guidance. Rationale §12.2 says "when in doubt, choose Pattern A"; I believe the doubt-default should be **B** for any fresh, single-consumer class result — which is most of them — because B's consumer form (`using var x := await …;`) is the one every newcomer writes unprompted, and B's failure mode (caller must dispose) is precisely what the affine machinery already catches. A's reversibility argument is real but optimizes API evolution over the everyday path. Measure before deciding: classify every await in the test corpus and conformance suite by shape; if the single-consumer shape dominates as I expect, flip §12.2's default and the stdlib's choices where compatible.

**Later (language change, behind evidence):** the one-rule alternative to the growing admission list — an unbound owned task temporary whose awaited borrow is bound by `var` extends to the binding's scope (the C++ const-ref lifetime-extension analog, scoped to this single form). §5.24 records the language's deliberate resistance to general lifetime extension; this would be a second bounded stretch of the same kind as the `case`-scrutinee one. I am *not* recommending adoption — I am recommending the study, because if the admission list grows again, one crisp extension rule may cost less than the next three admissions.

Size: **M** (mostly analysis and doc/stdlib guidance; the study is a design memo, not a commitment).

---

# Tier 3 — the toolchain that multiplies everything

## P12. Machine-grade diagnostics and a compiler query API

In an agent loop the compiler is the teacher and diagnostic quality is the reward signal. Bezel's prose diagnostics are already unusually good; make them machine-consumable: a structured (JSON) diagnostic format with stable codes; ownership rejections carrying their *trace* as data (the leak path; the borrow-source chain; which kernel rule, once P6 exists, was violated — "explain by derivation"); `bezelc --explain <code>` citing the spec section; and a query mode ("ownership state of `x` at line N") for tooling. Latency is part of this item: an agent regenerating a module hundreds of times a day multiplies every check-cycle millisecond. Near-term beneficiary: this project's own producer lanes.

Size: **M**, zero spec surface.

## P13. Property-based specification

Extends proposal 0022 (test framework). A `property` declaration is a boolean method over generated inputs; the runner generates, shrinks, and reports counterexamples. Generator inference falls out of the type system mechanically — records, enums, and choices (P1) compose; `where` constraints bound domains. `pure` properties (P2) additionally admit exhaustive small-domain checking and safe parallel execution; P5's journal supplies deterministic seeds so a failing case replays exactly. Properties are the cheap middle layer of the contract stack: more than an example, less than a proof, and — for a regeneration loop — the highest-value oracle per line of human-written specification.

Size: **M**, stdlib/tooling; no language change beyond what 0022 already contemplates.

## P14. Traceability conventions

The repo's own discipline — `// per §28.3` — is contract-to-code traceability, informally. Formalize the convention without a language change: a documented doc-comment tag (`/// @satisfies <contract-or-requirement-id>`) plus a tool that computes coverage (which contract clauses, which spec sections, which requirements have implementing declarations and citing tests). In a regeneration world this is the audit trail: *what was rebuilt, against what, and what vouches for it.* If an attribute system ever lands (rationale §18's coupled-design position stands), the convention migrates; until then, tags cost nothing and the tool is small.

Size: **S**.

---

## 16. Deliberately not proposed (with reasons)

- **Syntax relitigations** — `:=`/`+=` mixing, the value-first conditional, `when`'s triple duty. All are recorded, re-examined decisions (§3.3/#1373 explicitly). My disagreements are on record in the review conversation; reopening them now buys churn, not capability. The one honest window is a single pre-1.0 syntax-freeze review, if the owner ever wants one — not item-by-item erosion.
- **Block expressions** — rationale §5.13's decline stands. P1 (sum types shrink multi-statement arm pressure) and P10 (labeled break removes an extraction driver) relieve the two real pain sources without reopening it.
- **Reunifying the `Owning*` family** — the doubled type surface is the model's largest visible cost, but the generic-modifier alternative was tried for twenty revisions and abandoned for recorded reasons (rationale §5.11/§5.3, v0.97). No new mechanism in hand — but note the watch item: P1's per-instantiation payload classification is a small, user-visible version of exactly the dispatch v0.97 lacked. Revisit after `choice` ships and its machinery is understood.
- **Macros, member-access reflection, attributes** — the standing declines *age well* in the AI era: each would make program meaning context-dependent or runtime-discovered, which is precisely what cheap verification cannot afford, and the boilerplate they exist to eliminate is now the cheapest thing in the world to generate. The two narrow §18 carve-outs (`obsolete`, inlining hints) remain fine whenever wanted.
- **Checked exceptions** — §31.3 stands. P2's `pure` and (if ever wanted) a `total` qualifier are the verifiable fragments of the idea without the Java failure mode; P1 removes the main pressure to encode expected failure as exceptions at all.

## 17. Relationship to the current roadmap

Nothing here displaces R0 soundness catch-up or the stdlib waves (implementation-roadmap-2026-07). Sequencing reality: P8/P9/P10 are small spec trains that can ride any docs revision; P12 and P5 have near-term *internal* payoff (producer lanes; gate determinism) and can start as tooling work; P1–P3 are owner-gated design programs in the `spec-change-process.md` sense — each needs its own numbered proposal, critique pass, and ruling before a line of normative text changes. P6/P7 can begin as working papers in this folder immediately.

## 18. Closing

The review that preceded this document argued that Bezel's real asset is not any single rule but the discipline that every rule was *chosen* and recorded. This program is that discipline pointed at the next decade: state contracts where types already live (P1–P3), make the module seam behavioral (P4), make "behaves like yesterday" a sound oracle (P5), and keep the rule set small enough to mechanize and mechanized enough to trust (P6–P7) — while paying down the handful of recorded rough edges (P8–P11) and giving the compiler the voice agents will listen to (P12–P14). None of it requires abandoning what the language is. All of it compounds what the language already does better than its comparators: saying what it means, in the signature, checkably.
