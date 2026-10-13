# 0044 - Contracts and purity in the artifact interface; behavioral diffing

**Status:** graduated-v0.204 — superseded — spec deltas graduated v0.204 (carriage, §2.8.3 identity note, §2.8.6 classification, C.13; recorded Q2 divergence: the proven-tier set gains §22.16's constant-fold floor). The `bezel interface`/`bezel diff-api` verbs (Q6/Q8/Q9 surfaces) remain this proposal's tooling lane. Impl: #2054.
**Tracking issue:** [#2031](https://github.com/s8lap/bezel-lang/issues/2031)
**Target:** Spec — [§2.8.2](../Bezel_Language_Spec.md#sec-artifact-interface) (the carriage amendment: per-member contract records, tiers, the purity modifier), one appended sentence in [§2.8.3](../Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) rule 3 (contract records participate in description *agreement*, never in *identity*), a new **§2.8.6 "Interface deltas and their classification"** (placed after [§2.8.5](../Bezel_Language_Spec.md#sec-module-system-non-goals) for citation stability — Q4), and two new Appendix C.13 bullets. Stdlib — none. Rationale — one new section (why clause identity is textual, why contracts stay out of type identity). Tooling — the `bezel diff-api` and `bezel interface` verbs on proposal [0040](0040-build-system.md)'s driver surface ([§2.6](../Bezel_Language_Spec.md#sec-build-phases) keeps the build-tool CLI implementation-defined; only the delta classification the verb reports is spec material).
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

This proposal expands item **P4** of [`future-directions-2026-07-18.md`](papers/future-directions-2026-07-18.md) to graduation depth. Where it sharpens or diverges from the P4 sketch, the divergence is recorded explicitly in §10. **Depends on:** proposal [0042](0042-pure-methods.md) (`pure`, #2029 — the purity modifier this proposal carries; its proposed spec section is cited below as §22.15) and proposal [0043](0043-behavioral-contracts.md) (contracts, #2030 — the clauses and tiers this proposal carries; its proposed section is cited as §22.16; **this proposal is 0043's stage 2**, designed jointly with it). Builds on spec §2.8 (proposal [0034](0034-module-identity.md), landed v0.188 — whose scope decisions and [§2.8.5](../Bezel_Language_Spec.md#sec-module-system-non-goals) non-goals bound this proposal's, §13). Feeds proposal [0040](0040-build-system.md) Stage 1 (the dependency-interface-hash early cutoff, §4).

*Reference convention: an unprefixed §-reference denotes this proposal's own section where one of that number exists (e.g. §6.2); otherwise it is a language-spec reference (e.g. §2.8.2, §22.4). References of the form 0043 §N are to that proposal's sections.*

## 0. Summary

Spec §2.8.2 made the module interface a semantic requirement; 0042 and 0043 give members declared behavioral facts (purity; `requires`/`ensures`/`invariant` clauses with recorded enforcement tiers). This proposal puts those facts **in** the interface — per public member: the purity modifier, and each clause as a *contract record* (kind, `static` qualifier, canonical token text, demand-derived tier) — making the artifact interface a behavioral contract for the module. Two consequences follow and are designed here: a regenerated module body must verify against an unchanged interface (the regeneration seam, §8), and an interface *change* classifies mechanically (§6: **preserving** / **widening** / **breaking**, the Liskov direction applied at the module boundary, with a two-level verdict for tier demotions). A toolchain verb, `bezel diff-api <old> <new>`, reports per-member and whole-artifact verdicts, human and JSON, with exit codes for CI gating (§7). Contracts join the interface hash (so 0040's early cutoff stays sound and clause edits correctly recompile dependents, §4) but never join §2.8.3 type identity (§5).

## 1. Problem and evidence

- **The interface already carries every caller-consumed fact — except the new ones.** [§2.8.2](../Bezel_Language_Spec.md#sec-artifact-interface) clause 1 requires the artifact interface to carry full signatures — ownership modifiers, defaults, variance, constraints — *and* the [§3.6](../Bezel_Language_Spec.md#sec-derived-type-properties) derived-property verdicts, precisely because consumer-side rules dispatch on them. The language already ships one *behavioral* clause with caller-side consumption: the [§22.4 nullability postcondition](../Bezel_Language_Spec.md#sec-out-nullability-postcondition) (`when true`), whose proof the caller's flow analysis consumes to narrow the `out` recipient. It is "part of the method's contract like the other parameter modifiers" (§22.4) — i.e. interface material today, before 0043 even lands. The precedent is the whole argument: **caller-relevant ⇒ interface.** Purity (0042 §5: cross-module calls in pure bodies are admitted by the imported declaration's modifier) and clauses (0043 §7.1: cross-module `static requires` discharge; 0043 §12: proven-ensures consumption) are caller-relevant by construction; both proposals explicitly defer their carriage format here (0042 §14's §2.8.2 row; 0043 §16 stage 2).
- **Without carriage, the facts stop at the module boundary.** A pure-declared imported method would be uncallable from pure bodies; an interface member's clauses would not bind cross-module implementors (0043 §8 rule 1); `static requires` could never be discharged at cross-module call sites; and the [§2.8.3](../Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) rule 3 agreement check — the staleness backstop that turns "compiled against an old contract" into a named build error instead of silent unsoundness (the #1487 lesson, made normative in v0.188) — would not cover the one kind of fact whose stale consumption is *invisible at runtime* (a caller narrowing on a dropped `when` clause dereferences null).
- **Interface changes are today classified by hand.** 0043 §14 already had to write a prose change-taxonomy table ("add `requires` to a virtual member = breaking; checked→proven = behavior-preserving") and 0042 §12 another ("adding `pure` to an interface member is a breaking change") — evidence that the classification exists and is needed; nothing checks it. Proposal 0040's Stage 1 hangs incremental correctness on a dependency-*interface*-hash cutoff; the same interface comparison, classified rather than merely hashed, is the API-evolution and regeneration gate this proposal builds.
- **The regeneration thesis** (future-directions §0, P3/P4): if bodies become cheap to regenerate, the durable human-owned artifact is the module interface plus its contracts, and the build must be able to verify that a regenerated body changed *nothing it doesn't own*. That check is exactly "diff the two interfaces; fail unless identical-or-preserving" — mechanical once the interface carries the behavior and the classification is normative.

## 2. The design at a glance

1. **Carriage** (§3): §2.8.2 clause 1's list gains the purity modifier and, per member, an ordered list of *contract records* — (kind, `static`?, canonical clause text, tier) — plus per-class invariant records (populated at 0043 stage 4). Clause identity is **textual** (canonical token sequence, parameter and type-parameter references positionalized), never semantic — semantic clause equivalence is undecidable and a diff that guessed at it would be unsound in both directions (§3.2, §12 alternatives).
2. **The recorded tier is demand-derived** (§3.3): proven iff the declaration *demands* proof (`static` qualifier, or the §22.4 `when` form); checked otherwise, even where a particular compilation happened to discharge the clause. This keeps the interface a pure function of declarations — the keystone that makes body-only edits leave the interface byte-identical, so 0040's early cutoff survives (§4).
3. **Identity untouched** (§5): contract records join §2.8.3 rule 3's *must-agree* description set; they never join the identity key. Identity is nominal (module, qualified name, arity); a contract is an obligation the interface describes, revisable without the type becoming another type.
4. **The classification** (§6): a normative delta taxonomy — verdicts **identical < preserving < widening < breaking**, per member and whole-artifact, in the Liskov direction consistent with 0043 §8's override rule, with the *slot-audience* refinement (deltas on overridable slots take the stricter of the caller-side and implementor-side verdicts) and the **layered verdict** for tier demotions (preserving at the Core contract level, breaking at the Appendix-D.2 contract-proving level; the §22.4 family — whose proofs Core itself consumes — demotes Core-breaking).
5. **The verb** (§7): `bezel diff-api <old> <new>` — human and `--json` output, exit code = verdict rank (CI-gateable bare), `--fail-on <class>` for the pass/fail shape, `--level core|proof|both`; semver mapping stated as advisory guidance only.
6. **The seam** (§8): the regeneration protocol — capture baseline interface, regenerate the body, rebuild, gate on identical-or-preserving.

## 3. The carriage: what §2.8.2 additionally records

### 3.1 Proposed amendment text

Two edits to [§2.8.2](../Bezel_Language_Spec.md#sec-artifact-interface). First, clause 1's signature list — after "`const` values, and serialization conformances" — gains:

> …, the purity modifier (§22.15), the contract clauses of every member and the invariant members of every class (§22.16) — carried as **contract records** per the paragraph below —,

Second, a new paragraph after the "Definition-site visibility" block:

> **Contract records.** For every member the interface describes, it carries the member's purity modifier and, in declaration order, one **contract record** per contract clause; for every class it describes, one record per `invariant` member, base-first. A contract record is: the clause **kind** (`requires`, `ensures`, or `invariant`), the presence of the **`static` qualifier**, the **canonical clause text**, and the **recorded enforcement tier**. The canonical clause text is the clause expression's token sequence with comments removed and token spellings as written, except that each reference to a parameter of the member is replaced by its zero-based parameter position and each reference to a type parameter of the member or of its enclosing type declarations is replaced by its zero-based position; all other names are recorded as resolved at the declaration (module-qualified). Two clauses are **the same clause** exactly when their kinds, qualifiers, and canonical texts are identical. The recorded tier is derived from the declaration alone: **proven** for a `static`-qualified clause and for the §22.4 nullability postcondition (each *demands* discharge — an artifact that compiled has discharged it); **assumed** where §22.16 admits stated-unverified clauses (reserved; unpopulated in this revision); **checked** otherwise — including for an unqualified clause a compilation happened to discharge (per-site discharge is an as-if optimization (§22.16), not a promise, and recording it would make the interface depend on the member's body). Contract records are part of a type's description for [§2.8.3](../Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) rule 3: every embedded copy must agree, and disagreement is that rule's staleness error.

As everywhere in §2.8.2, the byte format stays implementation-defined; what is normative is the *capability* (a consumer can recover each member's records) and the *equivalence* (what counts as the same clause), which rule 3's agreement check and §2.8.6's diff both need. An implementation may additionally carry each clause's display text (original names) for diagnostics; display text does not participate in identity.

### 3.2 Clause identity is textual — the defense

Semantic equivalence of clauses is undecidable (clauses are boolean expressions over arithmetic and calls; deciding `requires a and b;` ≡ `requires b and a;` in general is deciding formula equivalence), and any decidable approximation misclassifies in one direction or the other. Textual identity is the conservative choice with a stated bias: it **never under-reports** a contract change (soundness for the gate) at the cost of over-reporting stylistic re-spellings (`1_000` → `1000`, added parentheses, re-qualified names) as clause replacements. That cost is small and — for this proposal's central use — *desirable*: under the regeneration protocol the clause text is the human-owned reviewed artifact, and a tool that re-spells contract text is editing what it does not own; the diff should say so. The two admitted canonicalizations (comments/whitespace; parameter and type-parameter positions) are exactly the ones that keep the record stable under edits that are *invisible at every use site*: a `ref`/`out`/`owned` parameter rename is call-site-invisible ([§22.5](../Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments): those arguments are positional-only), a plain-parameter rename is a *signature* event (named arguments) and should surface once, as one — not twice, with a spurious clause delta beside it; type-parameter names are likewise not part of signature identity (arity is, §2.8.3 rule 3).

Worked, on 0043's running example:

```bezel
public method Take(n: int): owned Batch
  requires n > 0;
  requires n <= _pending;
  ensures _pending = old(_pending) - n;
  // ... body ...
end Take;
```

carries (with `%k` as this document's notation for parameter position k — the notation, like the byte format, is implementation-defined):

| kind | static | canonical text | tier |
|---|---|---|---|
| requires | no | `%0 > 0` | checked |
| requires | no | `%0 <= _pending` | checked |
| ensures | no | `_pending = old(_pending) - %0` | checked |

Renaming `n` to `count` changes no record. The stdlib's shape-(a) Try members carry their normative `when` clauses from day one — `TryGetValue(key: K, out value: V? when true): bool` carries (ensures, static, `not result or %1 <> null`, **proven**) per 0043 §12's identity — which is the S0 populated surface before any authored clause exists.

One consequence is stated rather than hidden: a clause over non-public state (legal for checked clauses, 0043 §7.1) carries that state's *names and expression text* into the interface. This is the same disclosure §2.8.2's definition-site clause already makes for internals reached by exported generic bodies, with the same recompile-the-world cost profile (a private-field rename mentioned in a public member's clause invalidates dependents until they recompile — a build-time inconvenience, not a compatibility trap). Redacting non-public clause text was considered and declined (§12): a redacted record cannot be agreement-checked or diffed, which forfeits the staleness backstop exactly where consumption is least visible.

### 3.3 The recorded tier is demand-derived — the cutoff keystone

0043 §7 lets the compiler *opportunistically* discharge unqualified clauses (and elide their checks, as-if). If the interface recorded those discharges, the recorded tier would be a function of the member's **body** — a body edit could flip proven→checked with no declaration change, silently invalidating the interface hash (defeating 0040's cutoff) and, worse, silently invalidating any downstream proof that consumed the tier (a consumer whose own `static` clause discharged against the callee's recorded-proven ensures would break on a dependency's *body-only* edit, with nothing in any declaration to point at). Deriving the recorded tier from demand alone — `static` qualifier or `when` form — makes the tier a declaration fact: stable under body edits, guaranteed by compilation (a demanded proof that fails is a compile error, 0043 §7.3), and therefore safe for downstream consumption. The matching consumption rule is normative in §2.8.6: **downstream static consumption of a clause's proof requires the recorded (demanded) proven tier**; consuming an opportunistic discharge is not conforming — it would be consuming a fact no interface promises. (This is 0042 §2's declared-not-derived argument and 0043 §6.2's "the tier is the contract's guarantee floor, not a per-site optimization fact", applied to the interface: the same one decision, stated once more at the seam it protects.)

### 3.4 What the interface does *not* gain

Effective (inheritance-joined) contracts are not carried: records are attached to declaring members only, and consumers compute effective contracts by 0043 §8's rules from the carried hierarchy — carrying joins would denormalize (two records that must be kept consistent) and would misattribute diffs (an edit at a base member must surface at the base member's row). No new derived type property is introduced ([§3.6](../Bezel_Language_Spec.md#sec-derived-type-properties) is untouched: purity and clauses are declared, not computed — 0042 §2). No behavioral summary of *bodies* is carried beyond what §2.8.2 already requires for generics.

## 4. Hash semantics and 0040's early cutoff

Proposal 0040 Stage 1 keys the per-SCC compile action on, among other things, **dependency interface hashes**, with early cutoff when a rebuilt dependency's interface is byte-identical. This proposal's content is *in* that hash by construction (the records are interface content), which is the required direction: clauses and purity are consumed by dependents' compilations (§1), so a clause edit that did **not** invalidate dependents would leave callers narrowing on dropped `when` clauses, pure bodies calling de-pured members, and discharged `static requires` proofs floating free — the stale-artifact class §2.8.3 rule 3 exists to make impossible. Conversely, §3.3's demand-derived tier is what keeps the hash *quiet* under body edits. The consequence table:

| Change in a dependency | Interface | Dependents |
|---|---|---|
| Body-only edit of a non-generic member (including one that changes what the prover can opportunistically discharge) | unchanged | **early cutoff** — no recompilation |
| Body edit of a public generic member, or of internal machinery reached by carried generic bodies | changed (bodies are carried — §2.8.2's definition-site clause; already so before this proposal, recorded by 0040 §4) | recompile |
| Clause added/removed/edited, tier qualifier changed, purity changed | changed (the new records) | recompile — required for soundness, and now *classified* (§6) rather than merely detected |

Prerequisite unchanged from 0040 Q2: deterministic interface emission. The canonical clause text of §3.1 is itself deterministic by construction (token sequence plus positional substitution — no maps, no pointers), so the records add no new determinism risk.

## 5. Type identity: contracts do not individuate (§2.8.3, one sentence)

Proposed addition at the end of [§2.8.3](../Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) rule 3:

> A type's identity never includes its members' contract records or purity: two descriptions of one type that differ in clauses, tiers, or purity are the *same type described inconsistently* — the staleness error above — not two types.

Why identity must exclude them: §2.8.3's identity is **nominal** — declaring module, qualified name, arity — because identity is the unit of runtime oneness (rule 4: one static state, one runtime type identity; `is`/`catch`/patterns agree process-wide). A contract is an **obligation** the interface describes about a type's members, not a constituent of what the type *is*: contracts are revised across revisions of one module while the type remains the same type (the entire premise of §6's diff — a diff between "two different types" would classify nothing). Folding contracts into the identity key would make a clause edit mint a "new" type: every `is` test, `catch` clause, and pattern over it would bifurcate for values sharing one runtime representation, and the staleness error — the *useful* answer to a contract mismatch, naming the stale artifact and the fix — would become an unrelated-types type error naming nothing. The agreement half is kept and is load-bearing: because records are description content, a dependent compiled against an old clause set presents disagreeing descriptions and the build fails as stale — the backstop that makes cross-module proof consumption (§3.3) and `when`-narrowing sound under partial rebuilds.

## 6. The diff classification (proposed §2.8.6)

### 6.1 Verdicts, member matching, audiences

> **Verdicts.** An interface delta between two interfaces of one module is classified **identical**, **preserving**, **widening**, or **breaking**, in that severity order. *Preserving*: every use and every implementation conforming to the old interface remains conforming, and every fact a consumer could soundly rely on still holds (guarantees may grow). *Widening*: existing uses remain conforming; new surface exists (new members, types, overloads). *Breaking*: some conforming use, implementation, or soundly-consumed fact does not survive. The whole-artifact verdict is the maximum over all per-member and per-type deltas. Verdicts are computed per **conformance level** (§6.3); at the Core level the classification consults only Core-consumable facts.
>
> **Member matching.** Members of the old and new interface are matched by overload identity ([§22.6](../Bezel_Language_Spec.md#sec-overloading)): declaring type, name, arity, parameter types, and `ref`/`out` modifiers. Facts that never distinguish overloads — `owned`, `scoped`, clauses, purity, return type — are compared *within* a match; a change to a matching fact (e.g. a parameter type) reports as removal plus addition.
>
> **Audiences.** A delta on an **overridable slot** — a `virtual`, `abstract`, or interface member — is classified for both audiences, callers and implementors, and takes the **stricter** verdict. (0042 §5 rule 1 and 0043 §8 rule 1 bind implementors through the slot; a change that is caller-preserving can still be implementor-breaking.)

### 6.2 The normative table

Contract deltas (per matched member; "slot" = overridable per §6.1):

| Delta | Verdict | Reason |
|---|---|---|
| `requires` clause removed | preserving; **breaking on a slot** | Liskov: weaker precondition admits every old caller. On a slot, implementor bodies assumed the inherited clause (0043 §8 rule 1) and are now reachable with arguments they excluded. |
| `requires` clause added | **breaking** | Narrows every caller through every face (0043 §14). |
| `ensures` clause added | preserving *(slot note: binds every implementor's next checked run — behavior-revealing, not compile-breaking, 0043 §14)* | Strengthened postcondition; callers were promised no less. |
| `ensures` clause removed | **breaking** | Callers consumed the promise (runtime reliance; proof consumption at the proof level). |
| clause text changed (either kind) | **breaking** | Classified as removal + addition under textual identity (§3.2); at least one direction breaks. |
| clause list reordered, same record set | preserving | Evaluation order changes only which violation is blamed first (0043 §5.1/§5.6) — no conforming execution differs. |
| tier upgraded (checked → proven; `static` added and discharged) | preserving | The ratchet direction (0043 §7.4): strictly more guarantee, zero new obligation. |
| tier demoted (proven → checked; `static` dropped) | **layered** — §6.3 | |
| any tier → assumed (when populated) | **breaking** at every level that consumed the enforcement | Enforcement withdrawn, statement retained; a checked→assumed `requires` stops faulting violating callers in any mode. |
| purity added | preserving; **breaking on a slot** | More admitting contexts for callers; on a slot every override/implementation must restate `pure` — a compile break (0042 §12, verbatim). |
| purity removed | **breaking** | Pure-context callers stop compiling (0042 §12). |
| `invariant` added | preserving *(subclasser note as for ensures-added)* | Strengthened observable guarantee. |
| `invariant` removed or text changed | **breaking** | As ensures-removed, for observers of the class. |

Signature and type-level deltas (the facts §2.8.2 already carries, now classified — a whole-artifact verdict is meaningless without them):

| Delta | Verdict | Reason |
|---|---|---|
| member added (including a new overload) | widening | *Recorded caveat:* an added overload can re-route existing calls under [§22.6](../Bezel_Language_Spec.md#sec-overloading)'s ranking (adding a non-generic beside an applicable generic re-routes by the non-generic preference; the defaulted-arity tie-break was designed so *optional-parameter* overloads never re-route). Re-routed calls stay well-typed; the diff names added overloads so the audit is possible. |
| member removed, or visibility narrowed below the interface | **breaking** | |
| parameter/return type changed; `owned`/`scoped`/`ref`/`out` changed; `when` clause removed | **breaking** | Ownership and modifier surface is contract (§2.8.2); `when`-removed is ensures-removed (0043 §12). `when`-*added*: preserving; **breaking on a slot** (overrides must repeat it exactly, §22.4/§12.1). |
| default value added to a trailing parameter | widening | New legal call shapes; existing calls unchanged ([§22.6](../Bezel_Language_Spec.md#sec-overloading)'s tie-break). |
| default value changed or removed | **breaking** | Omitting call sites change meaning, or stop compiling. |
| generic constraint removed; variance added (`in`/`out` where invariant) | widening | More instantiations / assignments admitted ([§12.9](../Bezel_Language_Spec.md#sec-variance)'s direction discipline). |
| generic constraint added; variance removed or flipped | **breaking** | |
| public type added | widening | Except the closed-world rows below. |
| public type removed; base class changed; implemented interface removed | **breaking** | Interface *added* to a type: widening. |
| derived-property verdict gained (sendable, trivial-dispose, equality-bearing, C-compatible) | preserving | More admitting contexts ([§3.6](../Bezel_Language_Spec.md#sec-derived-type-properties)). |
| derived-property verdict lost; element category changed | **breaking** | §3.6 rule 3, verbatim: "a breaking change for clients relying on it, exactly as a signature change is." |
| enum constant added or removed; constant value changed | **breaking** | [§19.6](../Bezel_Language_Spec.md#sec-exhaustiveness): enum coverage is by declared constants — an addition turns dependents' statically-exhaustive `case` expressions partial (`MatchFailureException` on the new value). The new-member-is-widening intuition inverts under closed-world constructs. |
| new leaf added to a closed (`sealed abstract`) hierarchy | **breaking** | Same §19.6 inversion, hierarchy form. Detected structurally: a new public type whose base chain reaches a carried closed root. |
| public `const` value changed | **breaking** | Consumers fold the value ([§17.14](../Bezel_Language_Spec.md#sec-constant-expressions)); dependent code changes meaning. Added: widening; removed: breaking. |
| carried generic bodies or carried internal machinery changed (contract records unchanged) | preserving — **informational** | The regenerable interior: no declared contract changed. The verdict classifies *declared contracts*, never behavior — behavioral comparison of bodies is proposal [0045](0045-deterministic-replay.md)'s lane (§13 non-goals). |

### 6.3 The tier subtlety and the layered verdict

A proven→checked demotion (dropping `static` from a clause whose text is unchanged) changes **no runtime semantics for any conforming execution**: a proven clause never evaluates; a checked clause evaluates and passes; `requires` stays always-on in both (0043 §6.2); the only executions that differ are violating ones (now caught at runtime instead of rejected at compile time) and the pathological clause-evaluation fault (a checked clause can fault where a proven one never ran — 0043 §5.4; recorded, not classified). But the demotion **withdraws a static fact**: any downstream *proof* that consumed the callee's demanded-proven clause (0043 stage 3: a consumer's `static` clause discharged using it) no longer discharges — those consumers' compilations now fail. A flat verdict is wrong in both directions: flat-breaking cries wolf at every Core-level consumer (nothing they can consume changed — the tool teaches itself to be ignored); flat-preserving mislabels a real break at the proof level. 0043 §14's own row already states the truth in prose — "breaking where callers consumed the proof; flagged, never silent" — but "where" is undecidable for a tool that cannot see callers' code. The decidable refinement is **by conformance level**, because consumption is level-gated:

> **Layered verdict (normative).** A tier demotion of a clause is classified per level: at the **Core** level it is *preserving*, except for a clause whose proof Core machinery itself consumes — in v0.197 exactly the §22.4 nullability-postcondition family, whose demotion disables caller-side narrowing and is therefore *breaking* at Core (dependents' narrowed reads stop compiling). At the **contract-proving** level (the Appendix D.2 row 0043 §13 adds) every demotion is *breaking*. A verdict at level L consults exactly the facts consumable at L; `bezel diff-api` reports both levels (§7).

This is the finest classification that is sound without whole-program knowledge, and it keeps the ratchet culture healthy: promotion is everywhere-preserving, demotion is an honest, level-scoped event rather than either a global alarm or a silent one. (The when-family is detectable from the record alone — kind `ensures`, `static`, the §22.4 canonical shape over an `out` parameter and `result` — no special carriage bit is needed; whether to carry an explicit marker anyway is Q5.)

## 7. The verb: `bezel diff-api <old> <new>`

Tooling, on 0040's driver surface (Stage 1's `bezel` umbrella; 0040 Q1). Inputs: two compiled artifacts, or extracted interface files, or one of each — the companion verb `bezel interface <artifact> -o Name.bzli` extracts the embedded interface into a standalone, deterministic file (format implementation-defined, stability per 0040 Q2) so a *baseline can be checked in without checking in an artifact*. Both inputs must describe the same module name; anything else is a data error (below).

Human output — per-delta lines, worst first, then the whole-artifact verdict per level:

```
$ bezel diff-api contracts/Payments.bzli build/payments.o
module Payments — 4 deltas
  breaking    Account.Withdraw(decimal): requires added — requires amount <= _balance + _limit;
  breaking*   Account.Take(int): tier demoted (proven -> checked) — ensures _pending = old(_pending) - n;
              (* proof level only; preserving at core)
  widening    Account.Close(): member added
  preserving  Account.Deposit(decimal): ensures added — ensures _balance = old(_balance) + amount;
verdict: core = breaking, proof = breaking
```

JSON (`--json`): a stable machine surface — `{"format": 1, "module": …, "old": {"source", "interfaceDigest"}, "new": {…}, "verdict": {"core", "proof"}, "deltas": [{"member", "delta", "clause"?, "core", "proof", "note"?}]}`. Clause fields carry display text where the interface has it, canonical text otherwise.

**Exit codes.** Bare invocation: exit = whole-artifact verdict rank at the selected level — `0` identical, `1` preserving, `2` widening, `3` breaking — so shell gating needs no flags (`bezel diff-api A B; test $? -le 1`). With `--fail-on <preserving|widening|breaking>`: exit `0` below the threshold, `1` at or above it — the conventional CI shape. `--level core|proof|both` selects the level (default `both` = the maximum across levels — the conservative default; a project using no `static` clauses can never differ between levels, so the default costs nobody anything). Tool errors use sysexits-style codes disjoint from verdicts: `64` usage, `65` mismatched or malformed inputs (different module names; disagreeing format), `66` missing input.

**Semver mapping — advisory only.** For projects that adopt semantic versioning: identical → patch; preserving and widening → minor (a strengthened contract is new caller-usable surface exactly as a new member is); breaking → major. This is guidance printed by `--semver-hint`, not machinery: the language has no package manager and no version arbitration (§2.8.5; §13), and the verdicts, not version numbers, are the normative object.

**Degradation.** An input artifact predating contract carriage is *contract-opaque*: the diff runs at signature level, prints a warning, and never fabricates empty contract records (fabrication would classify the first post-carriage rebuild as a wall of clause additions).

## 8. The regeneration protocol (the seam this exists for)

The layered-contract architecture, restated from the mandate: types and ownership (layer 0, shipped) — purity (layer 1, 0042) — clauses (layer 2, 0043) — **module behavior (layer 3, this proposal)**. The protocol that layer 3 enables, stated as the workflow it is:

1. **The contract is captured.** `bezel interface build/payments.o -o contracts/Payments.bzli`; the baseline is checked in beside the source. It — not the body — is the human-owned, reviewed artifact: signatures, ownership, purity, clauses, tiers.
2. **The body is regenerated.** A tool (or a person) rewrites implementation code behind the interface. Contract text in source is not its to touch (§3.2's bias is aligned with this rule: even a re-spelling surfaces).
3. **The build re-verifies the seam.** `bezel build`, then `bezel diff-api contracts/Payments.bzli build/payments.o --fail-on widening` — the gate passes only **identical-or-preserving**: carried-body deltas pass (regenerable interior, §6.2 last row), tier *upgrades* pass (a regenerator that proves more is welcome), a new public member fails (widening — the surface is not the regenerator's to grow), any clause or signature regression fails. The checked/proven clauses themselves are re-verified by ordinary compilation and 0043's runtime tiers; the diff verifies the *interface* half of the acceptance test, compilation the *body* half.
4. **Intended contract changes go the other way.** A human edits the contract, re-captures the baseline, and the PR carries the diff output — the classification is the review's summary line, not its replacement.

0040 integration (recorded for its Stage 1, not designed here): a manifest key naming the baseline file per module makes step 3 automatic in `bezel build` (Q8).

## 9. Grammar delta

None. This proposal adds no syntax: clauses and `pure` are 0042/0043 grammar; carriage, identity, classification, and the verb are interface and tooling surface. (Appendix A is untouched — stated per the house convention rather than omitted silently.)

## 10. Interactions (complete enumeration) and divergences from the P4 sketch

| Surface | Interaction |
|---|---|
| [§2.5](../Bezel_Language_Spec.md#sec-dependency-resolution) / [§2.6](../Bezel_Language_Spec.md#sec-build-phases) | interface embedding and build phases unchanged; the records ride the existing artifact |
| [§2.8.1](../Bezel_Language_Spec.md#sec-module-identity-and-closure) | unchanged; duplicate/self-import rules orthogonal |
| [§2.8.2](../Bezel_Language_Spec.md#sec-artifact-interface) | the §3.1 amendment; the definition-site disclosure precedent extends to clause text over non-public state (§3.2) |
| [§2.8.3](../Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) | records join rule 3's agreement set (staleness backstop for narrowing and proof consumption); identity key untouched (§5); rule 4 runtime oneness is *why* identity must exclude contracts |
| [§2.8.4](../Bezel_Language_Spec.md#sec-program-assembly) | untouched; kind/entry rules orthogonal |
| [§2.8.5](../Bezel_Language_Spec.md#sec-module-system-non-goals) | mirrored, not disturbed — §13 |
| [§3.6](../Bezel_Language_Spec.md#sec-derived-type-properties) | verdict flips classified (§6.2); no new derived property (purity/clauses are declared, 0042 §2) |
| §11.2 / §12.1 / §12.5 | slot binding is what makes the implementor audience real (§6.1); interface members' clauses are the most consequential rows |
| [§12.9](../Bezel_Language_Spec.md#sec-variance) | the direction discipline the classification generalizes; variance deltas classified (§6.2) |
| §13 visibility | interface carries public + protected (subclasser audience); visibility narrowing = breaking |
| §14 generics | type-parameter positionalization (§3.1); constraint deltas classified; carried generic bodies = informational row |
| [§17.14](../Bezel_Language_Spec.md#sec-constant-expressions) | const folding is why const-value changes are breaking; the constant floor makes v1 tiers meaningful |
| [§19.6](../Bezel_Language_Spec.md#sec-exhaustiveness) | the closed-world inversion rows (enum constants; sealed-hierarchy leaves) |
| [§22.4](../Bezel_Language_Spec.md#sec-out-nullability-postcondition) | the precedent (§1) and the Core-consumed family in the layered verdict (§6.3); records populate from the stdlib's Try surface at S0 |
| [§22.5](../Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments) | plain-parameter names are signature identity (named args); positional-only parameter names are not; default-value rows |
| [§22.6](../Bezel_Language_Spec.md#sec-overloading) | member matching key (§6.1); the widening re-route caveat and the tie-breaks that bound it |
| §22.15 (0042) / §22.16 (0043) | the facts carried; 0042 §5's cross-module admission and 0043 §7.1's cross-module `static requires` discharge become possible at S0 |
| §28 / [§30.14](../Bezel_Language_Spec.md#sec-sendable-types) | ownership-modifier deltas breaking; sendability rides the §3.6 rows |
| Appendix C | C.13 gains: *contract records agree across all descriptions of a type (staleness otherwise)*; *the recorded tier derives from the declaration alone* |
| Appendix D | verdict levels bind to conformance surfaces: Core, and 0043 §13's D.2 contract-proving row (§6.3); classification itself needs no prover (tiers are demand-derived), so a Core toolchain emits both levels correctly |
| Proposals | **0042/0043**: the facts (this is 0043 stage 2); **0034**: the seam and the non-goals; **0040**: hash/cutoff (§4), the driver hosting both verbs, the manifest gate (Q8); **0045**: the behavioral complement (replay diffs executions; this diffs declared contracts); **0022**: CI shape of the gate |

**Divergences from the P4 sketch, recorded:**

1. **Slot audience.** The sketch classified "purity added" as flatly preserving; on an overridable slot it is a compile break for every implementor (0042 §12) and is classified breaking there (§6.1/§6.2). The same refinement applies to `requires`-removed and `when`-added.
2. **The layered verdict.** The sketch's "breaking: everything else" folded tier demotion into flat-breaking; §6.3 splits it by conformance level and makes the §22.4 family's Core-breaking demotion explicit. This also *refines 0043 §14's table* (its "remove or weaken any clause = breaking" row and its demotion row are the coarse forms; per-kind, per-level classification supersedes them at graduation — flagged for joint alignment, Q3).
3. **Weakening/strengthening are set-level, not semantic.** The sketch's "requires weakened / ensures strengthened" reads as semantic comparison; under textual identity the detectable forms are clause removal/addition, and a text edit is remove+add (§3.2). Deliberate: decidability over expressiveness.
4. **Demand-derived recorded tier** (§3.3) — the sketch was silent; without it the carriage and 0040's cutoff contradict each other.
5. **Whole-interface scope.** The sketch listed only the behavioral facts; §6.2 classifies the *entire* §2.8.2 surface (signatures, derived properties, consts, enum constants, carried bodies) — a whole-artifact verdict over a partial fact set would be false comfort.
6. **Closed-world inversions.** Enum constants and sealed-hierarchy leaves classify breaking, not widening (§19.6) — the sketch's "new members = widening" needed the exception.

## 11. Migration and compatibility

- **Additive; no source changes.** Carriage is new interface content; no program changes meaning. The interface format grows (implementation-defined versioning); pre-carriage artifacts interoperate as contract-opaque inputs to the diff (§7) and — in a mixed build — simply carry no records, which the agreement check treats as absent-everywhere (consistent), not as empty-set (fabricated).
- **The hash widens once.** The first post-carriage rebuild of a dependency changes its interface bytes; dependents recompile once. Thereafter cutoff behavior is strictly better-informed (§4).
- **Stdlib.** No stdlib clause wave rides this proposal (0043 §14's decision governs); the shape-(a) `when` clauses populate the records at S0 with zero observable change. 0042's first-wave purity annotations appear in the stdlib's interface as they land.
- **The 0043 §14 taxonomy is superseded at graduation** by §2.8.6's table (divergence 2); until then, 0043's prose table stands and agrees with this one everywhere except the refined rows.
- **Conformance.** Carriage and classification are **Core** (interface machinery; tiers are demand-derived so no prover is needed to record or classify them — §6.3's proof-level verdicts are *about* the D.2 row, not gated on implementing it). The verbs are tooling, outside conformance, like the rest of the driver surface.

## 12. Alternatives considered and declined

- **Semantic clause equivalence** (normalization by commutativity, constant folding, or SMT-checked implication — "requires weakened" as `old ⇒ new`). Declined for identity and for the diff: undecidable in general, and any partial decision procedure makes the *verdict* depend on prover strength — two conforming tools would classify one diff differently, which for a CI gate is the worst property available. Implication checking has a home as a future *advisory* layer atop the stage-3 prover (`diff-api --explain` suggesting "the added requires is implied by the removed one"), never as the verdict (recorded, not designed).
- **Contracts in the type-identity key.** Declined at §5: identity is runtime oneness; contracts are revisable obligations; the agreement rule already catches every mismatch as staleness with a better diagnostic.
- **Discharge-derived (opportunistic) recorded tiers.** Declined at §3.3: makes the interface body-dependent — the instability §2.8.2 exists to prevent (0042 §2's argument), defeats 0040's cutoff, and turns dependency body edits into invisible downstream proof breaks.
- **Redacting non-public state from carried clause text.** Declined at §3.2: forfeits agreement checking and diffing exactly where stale consumption is least visible; the disclosure has the definition-site precedent and the recompile-the-world cost model.
- **A flat single-level verdict.** Declined at §6.3 with both failure directions shown; the level split is the decidable refinement of 0043's "breaking where callers consumed the proof".
- **Version numbers or compatibility ranges in artifacts** (the diff as a version-resolution input). Declined: §2.8.5 fences versioning to a future package-manager design; this proposal classifies *changes*, it does not arbitrate *versions* (§13).
- **A separate contract file** (a `.bzli` the compiler *consumes*, header-style, as the source of truth). Declined: Bezel's interface is *emitted from* checked source (§2.8.2), which is what keeps contracts and bodies incapable of drifting; a consumed interface file reintroduces the C/C++ header-drift failure mode. The extracted `.bzli` of §7 is a *snapshot for comparison*, never a compilation input.
- **Diffing artifacts by hash only** (0040's cutoff as the whole story). Declined: a hash detects change but cannot classify it; the regeneration gate needs "identical-or-preserving", which is a classification.

## 13. Non-goals (mirroring §2.8.5)

No package manager, no registry, no **version resolution** — which artifact provides a module, and what version text means, remain the build tool's input and a later design ([§2.8.5](../Bezel_Language_Spec.md#sec-module-system-non-goals)); the semver mapping is advisory prose. No **ABI compatibility checking** — the model stays recompile-the-world; the diff classifies source-level contracts, not binary layouts. No **behavioral equivalence of bodies** — the diff's claims stop at declared contracts; behavioral comparison of implementations is proposal 0045's differential-replay lane. No cross-language surface, no documentation generator (the records make one *possible*; none is designed here).

## 14. Staging

1. **S0 — carriage** (= 0043 stage 2, one train): the §3.1 amendment, the §5 sentence, the C.13 bullets; records populate from `when` clauses, 0042 purity, and 0043 stage-1 clauses; cross-module purity admission and clause inheritance activate. Prerequisite: deterministic interface emission (0040 Q2). Tests: same-input byte-identity; record extraction fixtures per §3.1 rule (rename-invariance, tier derivation, agreement-check staleness triple).
2. **S1 — the diff verb**: `bezel interface` + `bezel diff-api` with the full §6.2 table, both output forms, exit codes; §2.8.6 lands with it (the classification precedes real proof consumption, so its rows are testable immediately). The regeneration gate becomes drivable by hand and in CI.
3. **S2 — tier-aware proof-level verdicts, live**: with 0043 stage 3 (the prover) and cross-module `static requires`/proven-ensures consumption real, the proof-level column stops being vacuously safe and starts gating actual dependents; `--level proof` becomes the recommended CI default for proof-consuming codebases; the 0040 manifest gate (Q8) ships alongside.

## 15. Open questions / owner decisions

1. **Q1 — clause identity**: ratify canonical-token-text equality with exactly the two canonicalizations (comments/whitespace; parameter and type-parameter positions), everything else literal (§3.2) — versus admitting any semantic normalization (declined, §12).
2. **Q2 — demand-derived tiers**: ratify §3.3 (proven = `static`-qualified + §22.4 family only; opportunistic discharges recorded checked) and the matching consumption rule (downstream static consumption requires the recorded proven tier).
3. **Q3 — the layered verdict**: ratify two-level verdicts with the §22.4-family Core-breaking demotion (§6.3), and the graduation-time supersession of 0043 §14's coarse rows (divergence 2) — versus a flat verdict.
4. **Q4 — spec placement**: §2.8.6 appended after the §2.8.5 non-goals (recommended: keeps every existing §2.8.x citation stable; the ordering oddity is acknowledged) — versus a new Appendix E, versus tool-docs-only (not recommended: the regeneration gate needs an implementation-independent classification).
5. **Q5 — when-family detection**: detect by record shape (recommended; no format bit) versus carrying an explicit marker on §22.4-derived records.
6. **Q6 — exit-code contract**: ratify rank-coded bare exits plus `--fail-on`, default `--level both` (§7).
7. **Q7 — contract-opaque degradation**: confirm pre-carriage artifacts degrade with a warning and are never treated as empty-contract (§7, §11).
8. **Q8 — the manifest gate**: does `bezel build` grow a per-module `interface-baseline` key automating §8 step 3 at S1/S2 (recorded for 0040), or does the gate stay a separate CI invocation?
9. **Q9 — semver hint**: confirm advisory-only, behind `--semver-hint`, never spec text (§7, §13).
10. **Q10 — member matching key**: confirm §22.6 overload identity as the match key (§6.1), accepting that parameter-type changes report as remove+add rather than as a fuzzy "signature edited" match.

## 16. Relationship to existing work

Future-directions P4 (the mandate; deepened here, divergences in §10) · proposal 0043 (the clauses and tiers; this is its stage 2; §14 taxonomy superseded at graduation) · proposal 0042 (the purity bit; §5's cross-module admission activates at S0) · proposal 0034 / spec §2.8 (the seam: interface-as-semantic-requirement, agreement-as-staleness, nominal identity — extended one level, non-goals inherited) · proposal 0040 (the hash and cutoff this carriage feeds; the driver hosting both verbs) · spec §22.4 (the shipped precedent: a proven clause in the interface, consumed by callers — the argument of §1 and the special case of §6.3) · proposal 0045 (the behavioral complement: replay compares executions, this proposal compares promises) · proposal 0022 (the CI harness the gate runs under) · #1487/#1513 (the staleness-and-identity evidence bugs behind §2.8.3, whose backstop §5 extends to behavioral facts).
