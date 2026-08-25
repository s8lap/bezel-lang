# Proposal 0117 — A type is nameable everywhere; its extension members are not

| | |
|---|---|
| **Status** | graduated-v0.278 — superseded (owner go, 2026-08-22). Landed as §2.3's auto-import entry and its invariant, §26.5 rule 2's cross-reference, §2.8.3 rule 2's statement of its own exception, stdlib §5's retired *Not auto-imported* paragraph, rationale §2.3, and two `DECLINED.md` rows. **Q2 answered as recommended:** the no-opt-out cost is accepted now, with the provenance tiebreak and the §2.3 opt-out both routed at R2 behind a bar of one real collision. *(prior: `draft` (2026-08-21), authored against **v0.277**.)* Origin is the owner's reading of proposal [0116](0116-conditional-members.md) §5.3.1: a cost recorded there turned out to be a live defect governing the most-used extension surface in the language. |
| **Tracking issue** | [**#3340**](https://github.com/s8lap/bezel-lang/issues/3340). Companions filed with it: [#3341](https://github.com/s8lap/bezel-lang/issues/3341) (the import gate is not applied to the standard library) and [#3342](https://github.com/s8lap/bezel-lang/issues/3342) (`arr.Count(pred)` resolves to the §5 operator against §26.5 rule 1). |
| **Target module(s)** | **spec** — [§2.3](../Bezel_Language_Spec.md#sec-always-available-names)'s auto-import list gains one module and one stated invariant; [§26.5](../Bezel_Language_Spec.md#sec-resolution) rule 2 gains a cross-reference; [§2.8.3](../Bezel_Language_Spec.md#sec-module-identity)'s *use without import* rule gains the sentence that names its own exception. **stdlib** — §5's *Not auto-imported* paragraph is retired; eight spec fences and one guide example become correct without being edited. **rationale** — one decision record. **`DECLINED.md`** — two rows. **Appendix A/C** — untouched: no grammar, no new token, no new compiler-enforced invariant. |
| **Written against** | spec/stdlib/rationale/changelog **v0.277**, HEAD `c7574a8e`. Every quotation re-verified against that tree. Compiler citations were obtained **by reading only** — this author could not build or run `bezelc`, and every claim needing a build is marked and left unasserted. |
| **Currency** | v0.277. **Quoted:** spec §2.3, §2.8.3 rule 2, §2.8.5, §17.10-adjacent rationale, §26.1, §26.5 rules 1–4, §26.6; stdlib §1, §5, §4.1/§4.2; rationale §18. **Re-verify before graduation:** §2.3's auto-import paragraph and §26.5 rule 2 (this proposal rewrites both). |
| **Origin** | Rationale §5.15 (v0.277) records the asymmetry as *"the defect worth its own proposal"*, having found it while costing a rename of `Array<T>.Sort()`. This is that proposal. |

*Reference convention: unprefixed §-references are **language-spec** references at v0.277; **stdlib**, **rationale**, **changelog** and **process** references are marked. ▲ marks proposed normative text, landed nowhere.*

---

## 0. What this proposes, in one page

**The defect.** §2.3 makes 26 standard-library type names visible in every file with no `import`. §26.5 rule 2 scopes extension-member candidacy to *"the current module and all imported modules"*. So a program may hold an `IEnumerable<T>`, and call its instance members, with no import at all — §2.8.3 rule 2 grants exactly that — while `xs.Where(p)` requires `import System.Collections.Linq;`, naming a module the program otherwise never mentions and which is **not the receiver's own module** (`import System.Collections.Generic;` does not help).

**Three facts decide this proposal, and each of them reverses an assumption it is natural to start from.**

1. **The rule was never argued.** The stdlib's *"(v0.195)"* citation dates the *sentence*, not the *rule*: v0.277's own reading of the v0.195 changelog entry finds a stdlib contract sweep containing nothing about extensions or imports. The nearest recorded reason is §2.8.3's decline of *everything-without-import* — argued about **bare names**, which extension members are not. **There is no decision here to defer to.** A proposal must supply the missing argument in whichever direction it goes.
2. **The compiler does not implement the rule for the standard library.** §5's operators are compiler builtins matched by name; the import set is never consulted for them. **34 test files call `.Where(` with no import and pass the real AOT oracle.** The gate *is* implemented, correctly and per-module, for user-declared cross-module extensions — just not at the only place the defect exists.
3. **The documents violate the rule they state.** Eight fenced examples in the spec call §5 operators with no import — **two of them inside §26 itself**; Guide 01 uses `xs.Where(p)` normatively and never mentions the import; and `tests/conformance/cases/spec_0172.bzl` records the un-imported form as **compiling**.

**The recommendation: adopt the rule everything already follows.** Add `System.Collections.Linq` to §2.3's auto-import list (**D1**), and state the invariant that generates it (**D2**): *where the standard library declares an extension over an always-available type, its module is auto-imported* — so a type's full standard surface is reachable wherever the type is nameable. This ratifies the implementation, makes eight spec fences and a guide correct **without editing them**, and vindicates a conformance case already recorded as compiling. Cost: **two** new bare names (`IGrouping`, `IOrderedEnumerable`), **zero** new stdlib ambiguities, and approximately zero implementation work.

**What it costs, stated at full strength (D3).** §2.3 has **no opt-out**. After auto-import, a user module declaring its own `extension IEnumerable<T>` with a member named `Where` gets a §26.5 rule-4 ambiguity whose recorded remedy — *"remove or narrow a conflicting import"* — **does not exist for an auto-import**. And Bezel's extension blocks are **nameless** (§26.1), so unlike C# there is no qualified spelling to fall back on: the only fix is renaming your own member. That cost is real, entailed by recorded rules rather than speculative, and it is this proposal's principal declined-with-reasons item.

**What is declined.** Making extension members over always-available types unconditionally visible (option (b)) — strictly worse, and it reverses a recorded decline. `public import` / re-export — a normative non-goal. **And leaving the rule while fixing only the documents (option (c)) is declined most firmly of all**, because on this evidence it means *enforcing* an unargued restriction: breaking 34 test files, eight spec fences, a guide and a corpus case, to deliver a rule with no recorded benefit that no user has asked for.

---

## 1. The defect

### 1.1 The two rules, side by side

§2.8.3 rule 2 (`Bezel_Language_Spec.md:253`) — the principle the language already committed to:

> **Use without import; naming requires import.** A file that imports module `A` may freely **use** values of any public type reachable through `A`'s public surface — receive them …, **call their public members**, pass them as arguments, return them, and dispose them — without importing the type's declaring module. **Naming** the type in source … requires importing its declaring module.

§26.5 rule 2 (`Bezel_Language_Spec.md:5933`) — the carve-out, stated as a search scope rather than as a prohibition:

> **Extension members.** If the receiver's type declares no instance member of that name, the compiler searches **the current module and all imported modules** for extension blocks on `x`'s type, base types, or implemented interfaces …

There is **no other normative statement of the rule in the spec.** The asymmetry is therefore not announced anywhere: it falls out of a scoping clause in a resolution algorithm, and contradicts a principle stated in plain terms twenty-three chapters earlier.

### 1.2 Why this shape is stranger than "type visible, member invisible"

Because §2.3 grants only *names*, and §2.8.3 rule 2 already permits use without import, a program can obtain an `IEnumerable<T>` and call its instance members with **no import at all**. Add `import System.Collections.Linq;` and the *same receiver expression* gains roughly fifty member names. So the required import is about neither the receiver's type nor the type's module — it names a module that declares nothing the program otherwise mentions.

### 1.3 The footprint, exactly

**Every instance in the shipped library is one module.** `System.Collections.Linq`'s nine `IEnumerable<T>`-headed blocks reach **9 of the 26** non-auto-imported §2.3 names — `IEnumerable<T>`, `IReadOnlyCollection<T>`, `IReadOnlyList<T>`, `ICollection<T>`, `IList<T>`, `IOwningCollection<T>`, `IOwningList<T>` by the §4.1/§4.2 inheritance chain, plus `Array<T>` and `OwningArray<T>` by conformance.

The other **17** — `Task`/`Task<T>`/`OwningTask<T>`, `CancellationToken`, the six event types, the four enumerator interfaces, the four owning-enumeration interfaces — have no stdlib extensions at all. They are **latent** instances: the asymmetry is available to them the moment anyone declares an extension, and costs nothing today. That is what D2's invariant is for.

---

## 2. What the rule was for — and the answer is nothing recorded

The stdlib says (`Bezel_Standard_Library.md:3488`):

> these extensions require `import System.Collections.Linq;` — the extended `IEnumerable<T>` itself is always available (spec §2.3), but extension members resolve only when their declaring module is imported (v0.195).

**The `(v0.195)` citation dates the sentence, not the rule.** The v0.195 changelog entry is the stdlib contract sweep; it contains nothing about extensions or imports. The rule is inherited from §26.5's original resolution algorithm and was never separately argued.

The nearest recorded reason is §2.8.3's decline of *everything-without-import*, which is argued about **bare names** — the cost of a name being ambiguous, and of a reader not knowing where a name comes from. Extension members are not bare names: they are reached through a receiver whose type the reader already has in hand. **That argument does not transfer**, and this proposal does not pretend it was made.

This matters procedurally. Process §2's *"does it contradict a recorded decision?"* test comes back **no**: there is no decision to contradict. The obligation is the harder one — to make the argument, in both directions, for the first time.

---

## 3. What the compiler does, and why it is decisive

**The gate is real for user extensions.** Cross-module extension blocks loaded from artifacts are filtered per-module, per-file, constructively, in `ExprTC::lookupExtensionMember`.

**It is not applied to the standard library at all.** §5's operators are compiler builtins matched by name; the import set is never consulted for them. `grep -rn "System.Collections.Linq" src/` yields three comments and nothing else.

**Thirty-four test files call `.Where(` with no import and pass** — through the AOT oracle, which compiles and links and runs. So the sentence in the stdlib describes a restriction the compiler has never enforced.

That fact does most of the work in §4. A rule that is unargued, unenforced, and contradicted by the documents that state it is not a rule the project decided to have; it is one nobody has looked at. The choice is therefore not *"should we relax this?"* but *"which of the two things we are already doing should we write down?"*

---

## 4. The options

### 4.1 Option (a) — auto-import `System.Collections.Linq` — **recommended**

**Named surface added to every file: exactly two.** `IGrouping<TKey, T>` and `IOrderedEnumerable<T>` are §5's only public named declarations; everything else in the module is a nameless `extension` block (§26.1). Neither collides with anything in `System`, `System.Text`, or §2.3's list.

**New bare-name ambiguities in the stdlib: zero. New §26.5 rule-4 ambiguities in the stdlib: zero** — the only other stdlib extension blocks are over the span/buffer families, with disjoint heads.

**Instance shadowing is unchanged.** §26.5 rule 1 decides instance-vs-extension by name *before* imports matter, so the six names stdlib `:44` records as blocked (`Count`, `Contains`, `First`, `Last`, `Reverse`, `ToArray`) stay exactly as blocked. Auto-import changes none of it.

**Implementation cost: approximately zero**, because the compiler already resolves these calls without the import.

**And it fixes the documentation by making it true.** Eight spec fences, Guide 01's `xs.Where(p)`, and `spec_0172.bzl` all become correct with no edit. An option that discharges a documentation debt by ratifying practice, rather than by editing nine sites to match a rule nobody follows, is the cheaper repair of the two.

### 4.2 Option (b) — extension members over an always-available type are always available — **declined**

Strictly worse than (a) with no compensating benefit: it grants the same visibility by a more complicated rule, extends it to the 17 latent names nobody has asked for, and reverses the §2.8.3 decline in a way (a) does not, because it makes visibility depend on a property of the *extended type* rather than on an explicit module list a reader can consult.

### 4.3 Option (c) — keep the rule, fix only the documents — **declined, and this is the one worth arguing**

Superficially the conservative choice, and on this evidence the most disruptive. It means **enforcing** the rule: 34 test files start failing, eight spec fences and a guide need editing, `spec_0172.bzl` must be reclassified, and the compiler needs new work — to deliver a restriction with no recorded reason that no user has requested.

The conservative-looking option is conservative about the *text* and radical about the *behaviour*. Where those diverge, and the text was never argued, the behaviour is the better guide.

**What survives from (c), and is adopted:** the documentation obligations are owed *regardless* of which option lands, and §5's *Not auto-imported* paragraph must go either way — it is false today in the direction of over-promising a restriction.

### 4.4 Option (d) — `public import` / re-export — **declined**

A normative non-goal at §2.8.5. Its stated reason is about names rather than extensions and does not transfer cleanly, but the evidence bar it carries governs, and this proposal has no evidence to clear it.

---

## 5. D3 — the cost of (a), at full strength

**§2.3 has no opt-out, and Bezel has no qualified spelling.**

After auto-import, a user module declaring `extension IEnumerable<T>` with a member named `Where` gets a §26.5 rule-4 ambiguity in every file of every program that touches the type. The recorded remedy is *"remove or narrow a conflicting import"* (`Bezel_Language_Spec.md:5935` — the compiler builds that exact string). **You cannot remove an auto-import.**

C# has the same collision and two escapes: disable implicit usings, or call the static method explicitly (`MyExtensions.Where(xs, p)`), because a C# extension method lives on a *named* static class. **Bezel's extension blocks are nameless** (§26.1). So there is no qualified spelling, and the only fix is to rename your own member.

That is a genuine loss, and this proposal does not minimise it. Three things bound it:

1. **It is unreachable today.** The compiler does not enforce the import gate for §5, so the escape the remedy describes does not currently work either. (a) does not remove a working escape; it writes down that there isn't one.
2. **The affected population is zero** — no user modules exist, and no in-tree extension block collides with a §5 name.
3. **The failure is a compile error, not a silent wrong answer** — §26.5 rule 4 is an ambiguity diagnostic naming both origins.

**Two general fixes exist and neither is taken here**, because both need evidence this proposal does not have: a **provenance tiebreak** (a user-module extension outranks a compiler-known standard-library one — note this is a provenance rule, not the auto-ranked *specificity* §17.10 deleted, so it does not reopen that), or an **opt-out on §2.3** (an import-suppression form). Both are routed at §9 R2 with a bar: **one real collision**.

---

## 6. Exact edits ▲

### Edit 1 — §2.3's auto-import list gains the module ▲

**OLD:** `All other library modules require explicit `import`.`

**NEW:** ▲ `System.Collections.Linq` is auto-imported (for the [§5] query operators over `IEnumerable<T>`, which [§2.3] makes always available; the module's only named declarations are `IGrouping<TKey, T>` and `IOrderedEnumerable<T>`). All other library modules require explicit `import`.

### Edit 2 — §2.3 states the invariant that generates it ▲ (D2)

▲ Appended to §2.3: *Where the standard library declares an extension over a type this section makes always available, that extension's module is auto-imported. A type's full standard surface is reachable wherever the type is nameable — the alternative is a call that compiles or not depending on an import unrelated to its receiver's type. This currently names one module ([§5]); it is an invariant on the standard library's own shape, not a general rule about user modules, whose extensions remain import-gated ([§26.5] rule 2).*

### Edit 3 — §26.5 rule 2 points at the exception ▲

▲ After *"the current module and all imported modules"*, insert: `— noting that the standard library's extension modules over always-available types are auto-imported ([§2.3](#sec-always-available-names)), so this scope includes them in every file`.

### Edit 4 — §2.8.3 rule 2 names its own exception ▲

▲ Appended to the *use without import* rule: *Extension members are the exception this rule does not otherwise state: they are candidates only from the current module and imported modules ([§26.5] rule 2), which is why the standard library's extension modules over always-available types are auto-imported ([§2.3]).*

### Edit 5 — stdlib §5's *Not auto-imported* paragraph is retired ▲

The paragraph at `Bezel_Standard_Library.md:3488` is replaced by a statement that the module is auto-imported, with the §2.3 citation.

### Edit 6 — rationale, one decision record ▲

Recording: that the rule was unargued and unenforced; that the documents violated it eight times over; that (a) ratifies practice and (c) would have enforced a restriction nobody decided to have; and D3's opt-out cost with its bar.

### Not edited, and why

- **The eight spec fences, Guide 01, and `spec_0172.bzl`** — they become **correct** under Edit 1. That is the point; editing them would be the cost of the option this proposal declines.
- **Appendix A, Appendix C** — no grammar, no token, no new compiler-enforced invariant. The change is to a visibility list.
- **The six v0.259-blocked names** — decided by §26.5 rule 1 before imports matter; unaffected.

---

## 7. Prior art

Every comparable language has converged on making its query/sequence surface available by default, after shipping the opposite first.

- **C#** required `using System.Linq` for two decades, then made it implicit in the default project template (C# 10 implicit usings) — an admission that the per-file opt-in was ceremony rather than protection. Its ambiguity escape survives because extension methods live on *named* static classes.
- **Rust** requires a trait in scope for its methods, but puts the ones everyone needs — `Iterator`, `Into`, `Option`/`Result` — in the **prelude**, imported into every module automatically. §2.3 is Bezel's prelude; this proposal observes that the prelude currently contains the types but not their surface.
- **Kotlin** auto-imports `kotlin.collections.*` in every file.
- **Swift** has no import requirement for extensions within a visible module at all.

The recurring lesson is that a per-file opt-in for a library's *core* surface buys little and costs discoverability. What each language kept the opt-in for is the case Bezel keeps it for too: **third-party** extensions, where ambient injection of member names into someone else's types is a real hazard. D2 draws exactly that line.

---

## 8. Blast radius and migration

- **Newly accepted:** two bare names in every file; roughly fifty member names newly reachable on `IEnumerable<T>` receivers **at the document level only** — the compiler already admits them.
- **Newly rejected: nothing.**
- **Behaviour change in the compiler: none**, which is the unusual property of this proposal and its strongest practical argument.
- **Conformance corpus:** `spec_0172.bzl` (recorded as compiling without the import) becomes correct rather than anomalous; no case is deleted or reclassified. Eight spec fences and one guide example become conforming without edits.
- **§2.8.6 delta class:** widening. A program that compiled continues to; programs that name `IGrouping`/`IOrderedEnumerable` unqualified newly resolve.

---

## 9. Reopening bars

| # | Declined | Reopening bar |
|---|---|---|
| **R1** | Option (c) — keep the import gate for the standard library and enforce it | Evidence that ambient §5 operators cost a real reader real confusion — the §2.8.5 *"real programs hitting real walls"* bar, which today defeats the change in the opposite direction too. Note the asymmetry in what enforcement would cost: 34 test files, eight spec fences, a guide, a corpus case, and new compiler work, against a benefit no record states. |
| **R2** | A provenance tiebreak (user extension outranks a standard-library one) or an opt-out on §2.3 | **One real collision** — a user module wanting a §5 operator name over `IEnumerable<T>` and unable to proceed. Both are answers to D3's cost, both are cheap to add later, and neither is designable without knowing which of the two the collision actually wants. Recorded liftable rather than barred. |

---

## 10. Questions, and what the coordinator must do

### Questions

- **Q1 — D1's scope.** Auto-import `System.Collections.Linq` (recommended), or state D2's invariant and let it name the module implicitly? The proposal does both, belt and braces; the owner may prefer the list alone, with the invariant left in the rationale.
- **Q2 — D3.** Accept the no-opt-out cost now (recommended, on zero affected population), or land R2's provenance tiebreak with it?
- **Q3 — the latent 17.** D2's invariant binds the standard library's *future* shape. Is that a commitment the owner wants stated normatively, or a guideline?

### Issues to file

- **A — this proposal's tracking issue.** Filed: [#3340](https://github.com/s8lap/bezel-lang/issues/3340). **B** is [#3341](https://github.com/s8lap/bezel-lang/issues/3341); **C** is [#3342](https://github.com/s8lap/bezel-lang/issues/3342).
- **B — the compiler/spec divergence**, which is a defect on its own terms whichever option lands: the import gate is not applied to the standard library's extension members, and 34 test files depend on that. Under (a) it becomes correct-by-construction; the issue should record it either way, since today the documents and the implementation disagree.
- **C — `arr.Count(pred)` resolves to the §5 operator on an `Array<T>` receiver**, contradicting stdlib `:44`/`:5301` and §26.5 rule 1, which say the instance `Count` property blocks it. Found while surveying this area; unrelated to this proposal's decision and should not ride it.

### Coordinator actions

- `docs/proposals/README.md` — add the row, `Status: draft`.
- **No `DECLINED.md` rows yet**: that file's ruled scope rule 2 forbids indexing an under-review draft's recommendations. R1 and R2's rows are owed **at graduation**, not now.

---

## 11. Revision record

**Draft 1 (2026-08-21), against v0.277 at `c7574a8e`.** Written after rationale §5.15 recorded the asymmetry as owed its own proposal.

**Three findings inverted the assumption this proposal started from**, and each is recorded because the starting assumption was reasonable:

1. The drafting began expecting to *defend* the import gate on the C#/Rust ambient-injection argument, and to propose a diagnostic obligation as the fix. That argument turns out never to have been made here — the `(v0.195)` citation dates the sentence, not the rule — so there was no decision to defend, and importing another language's reasoning to fill the gap would have been exactly the error this project's process exists to prevent.
2. The compiler does not enforce the rule for the standard library. That converted the question from *"should we relax this?"* to *"which of the two things we already do should we write down?"* — and made option (c), the apparently conservative one, the most disruptive.
3. The documents violate the rule they state, eight times in the spec alone and twice inside §26 itself. A rule the specification cannot follow in its own examples is evidence about the rule.

**What could not be verified.** No build was available. Whether the compiler accepts the un-imported form is asserted from 34 passing test files rather than from a probe run here; whether a *user* extension over `IEnumerable<T>` is correctly import-gated today is read from source, not measured. Both are named in §10's issue B.
