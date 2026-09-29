# Self-Hosting Bezel Standard Library Collections — Design Proposal

**Status:** under-review — draft (refreshed 2026-09-17 to v0.306 as a decision document; owner acceptance pending)  
**Written against:** language spec / stdlib / rationale **v0.306**; source anchors re-verified at `main` @ `29410e29`.  
**Scope:** Moving the C++ runtime implementations of the stdlib collections into
`.bzl` source that the Bezel compiler compiles, so the compiler's hardcoded
"name == Array" gates progressively atrophy.  
**Context:** Issue #545 direction; rationale §5.11 (v0.97), §5.15 and §5.20 (v0.132).  
**Provenance:** Drafted mid-June 2026; committed 2026-07-04 as proposal 0005 (it had
sat untracked at the repo root — the custody gap the coverage audit flagged). Written
pre-v0.168 Owning-rename and pre-#1414-stage-3 registry; the two refreshes recorded in
the provenance notes below have since brought its names and anchors current.

---

> **Provenance note — currency + decision refresh performed 2026-09-17 (coordinator-directed,
> bug-fixing mode).** Brought from stdlib v0.231 to **v0.306** and re-pointed at `main`
> @ `29410e29`. What changed:
>
> - **Every `src/` anchor was re-verified by `grep` and re-cited.** The tree has moved
>   substantially since the 2026-07-30 pass — the construction dispatch, the method-call
>   dispatch, the disposal walk, the registry and the module loader have all moved by
>   thousands of lines. Each anchor below is either updated or marked *moved* / *no longer
>   exists*. No stale anchor was left standing. The one **content** change found, not just a
>   line move: `tests/conformance/cases/stdlib_0039.bzl` is **no longer** the `OwningArray<T>`
>   reference implementation (that case is now a `Result<T, E>` connect-or-explain example);
>   the reference implementation is `tests/conformance/cases/stdlib_0051.bzl`.
> - **New §0** — the evidence section this refresh exists to add: measured counts, the
>   duplication census, the defect-campaign families, and the open-issue population whose
>   shared root cause is *the compiler special-cases this collection by name*. §1.3's
>   quantification was re-run with its own recipe: **~385 → 634 sites**.
> - **Currency to v0.306.** v0.258's calls-mint unification (proposal 0096) retires the
>   owning-head declaration-site rejection and the `Drain()` §29.11 affine carve the D3 ruling
>   was written against; v0.290's stdlib regularization (proposal 0123) is checked for
>   collection impact; v0.298 makes a closure a borrow-carrying value, which is what ties
>   SH-4 to defect-campaign family A; v0.302–v0.306 are editorial/restructuring passes that
>   left every §4.x collection heading in place (re-verified section by section). The §5 POC
>   body is respelled against the **v0.306 §4.11** surface.
> - **§6 re-sequenced** against the 2026-09 defect campaign, with a per-slice statement of
>   what it *removes*, which campaign family it touches, and what rows it owes.
> - **The owner decision is stated** at §0.6 as a single recommendation with its reasons.
>
> The architecture is not changed. Three design claims are **superseded in place with a
> note** where the current tree or the current spec refutes them: §3.1's "there is no `.bzl`
> prelude" (there is one now, and it is source text — §0.5), §3.1's "all codegen predicates
> check `rt->declaration == nullptr`" (the predicate now also admits a `builtinClassSym`
> declaration — §3.1), and SH-4's "the borrowing variants use `Array<T>` … as backing, which
> avoids the owned-element complexity" (rationale §5.20's declined borrow-storing primitive
> is the reason that is not the cheap slice it reads as — §6 SH-4).

> **Provenance note — mechanical refresh performed 2026-07-30 (owner-directed
> 2026-07-29).** The pre-v0.168 type names this draft was written with
> (`OwnedStack` / `OwnedBuffer` / `OwnedArray` / `OwnedQueue` / `IOwnedCollection`, …)
> were renamed to the current `Owning*` / `IOwning*` family (stdlib v0.231; spec
> §14.4); every `src/` file:line anchor was re-verified against the current tree
> and updated (several cited mechanisms have moved or been superseded — notably
> the collection-name predicates now read the #1414 stage-3 registry in
> `src/common/BuiltinRegistry.cpp`, and the former monolithic
> `bezel_runtime.cpp` was split into per-domain `bezel_runtime_*.cpp` files);
> and the §5 POC's conformance claim was corrected for the v0.222
> `OwningStack`/`OwningQueue` demotion (#1811). The architecture and slice plan
> were not changed.

## 0. Why this matters now — the evidence, and the decision asked

*Added 2026-09-17. Everything in this section is a measurement at `main` @ `29410e29` or a
quotation from a named record; each number carries its source.*

### 0.6 The decision this document asks for

**Recommendation: HOLD — do not schedule SH-1..SH-3 during the 2026-09 defect campaign;
re-decide at the campaign's Phase 2 boundary.** Not *decline*: the direction is sound, the
evidence below is strong, and the cost of the name-gated architecture is measurable and
**growing** (634 sites, up from ~385 in seven weeks — §0.1). But the campaign's own
sequencing argument (`defect-campaign-2026-09.md` §1, fact 2, evidenced by #4438: "two
correct narrow fixes to one family cancelled each other") applies with more force to a
slice that *moves* a lowering path than to one that fixes it, and three of the four
campaign families self-hosting would shrink (**E**, **G**, **K**) are Phase-3 families whose
shared sites SH-3 and SH-5 would be editing underneath the lane that owns them. The
prerequisite is also not what §3 says it is any more: the real gate is no longer "a prelude
loader does not exist" — one exists and is used in production (§0.5) — it is **defect-campaign
family A (#2718, the refcounted capture cell)** for the *borrowing* half and **oracle repair
(campaign Phase 0)** for all of it, because a POC whose acceptance criterion is "the
self-hosted path produces the same results as the intrinsic path" is worthless measured
against oracles #4452 shows passing for the wrong reasons at 477 + 1,551 + 129 call sites.

Two things should happen *now*, and neither is a lane:

1. **Record that 0005 is the named home for four already-deferred obligations** (§0.4), so
   the campaign stops routing around it silently. Stdlib §4.17 (v0.306) already says in
   normative text: *"There is no owning `OwningSortedDictionary` in this revision — owning
   sorted collections ride the self-hosted-collections work."* The campaign plan, by contrast,
   lists #545 under §6 "Design and study proposals … not defects" — both are defensible, and
   together they are a gap: the stdlib defers *to* this work while the defect plan excludes it.
2. **Treat SH-2 as campaign-compatible and schedule only it** (§6). It is test-writing against
   an existing intrinsic path, it removes nothing, it touches no family's shared site, and it
   is the falsification instrument for the whole proposal: if `OwningBuffer<T>` as a generic
   class's owned field does *not* round-trip cleanly under ASan, SH-3 is not a POC, it is a
   defect hunt, and the owner should know that before committing a lane.

**Alternatives declined, with reasons.** *Accept-and-schedule-POC now* — declined: it spends
the campaign's scarcest resource (lanes able to reason about `LlvmBackend_Setup.cpp`'s
literal lowering and the element-disposer stamp) on a slice whose payoff is measured in
removed lines rather than closed crash rows, while #4438's interaction evidence says
concurrent edits to one family cancel. *Decline-with-reasons* — declined: the cost being
avoided is not hypothetical (§0.1–§0.3), the direction is cited **by the normative stdlib**
(§4.17) and by the rationale (§5.20: "the implementation is licensed to build them that
way"), and #547 already records what happens when a collection is added natively against
this proposal's advice — `SortedSet`/`SortedDictionary` shipped natively and produced
#3490 / #3489 / #3491 / #2647 (§0.3), exactly the per-collection hand-wiring #547 predicted.

**Design choices this document makes that the spec does not specify** (CLAUDE.md rule 4,
recorded here rather than left silent): (D1) prelude location and build model — §3.2, **now
partly answered by the tree**, see §0.5; (D2) `OwningBuffer<T>` stays the permanent intrinsic
floor — §4.2; (D3) `Drain()` defers out of SH-3 — ruled 2026-08-11, §6 SH-3, and **re-validated
against v0.258** in §6; (D4) whether `LinkedList` / `Dictionary` stay C++ long-term — §6 SH-6;
and, new at this refresh, **(D5)** whether the borrowing family is in scope at all, given that
rationale §5.20 declines the borrow-storing primitive a self-hosted `Array<T>` would need —
this document's answer is *no*: the borrowing floor is `Array<T>` itself and SH-4 can only
self-host *wrappers* over it (§6 SH-4).

### 0.1 The name-gated surface, re-measured with §1.3's own recipe

§1.3's recipe: grep `src/**/*.cpp` (headers and `tests/` excluded) for the fourteen quoted
collection names `"Array"`, `"OwningArray"`, `"Dictionary"`, `"OwningDictionary"`, `"Stack"`,
`"OwningStack"`, `"Queue"`, `"OwningQueue"`, `"Set"`, `"OwningSet"`, `"LinkedList"`,
`"OwningLinkedList"`, `"OwningBuffer"`, `"Buffer"`.

| Subsystem | 2026-07-30 | 2026-09-17 (`29410e29`) | Δ |
|---|---|---|---|
| `src/sema/` | ~194 | **318** | +64% |
| `src/codegen/` | ~137 | **217** | +58% |
| `src/ownership/` | ~38 | **38** | 0 |
| `src/common/` (the #1414 stage-3 registry) | ~15 | **61** | +307% |
| **total** | **~385** | **634** | **+65%** |

Two further counts, measured the same way and new at this refresh:

- **58 hand-spelled collection-name disjunctions** (`grep -rn '== "Array"' src/ --include=*.cpp`
  in its five spellings — `cn ==` / `n ==` / `head ==` / `colName ==` / `name ==`), spread
  across **18 files**. These are the sites the registry was supposed to retire and did not.
- **Per-name reach**, all `src/` including headers: `SortedSet` **78**, `OwningStack` **56**,
  `Stack` **54**, `OwningQueue` **52**, `Queue` **49**, `OwningBuffer` **41**, `FrozenArray`
  **36**. A single new collection name costs on the order of **40–80 sites**, which is the
  number SH-3's subtraction is measured against.

**Five of those disjunctions live in one file and disagree with each other** —
`src/ownership/Ownership_Checks.cpp`:

| Site | Question it answers | Names |
|---|---|---|
| `:7968–7971` (`checkCollectionBorrowStoreEscape`) | §15.4 fresh-store, borrowing family | 8 — incl. `SortedSet`, `SortedDictionary` |
| `:8338–8341` (`sec154HeadHasOwningTwin`) | does the head have an owning twin | 6 — deliberately excludes the sorted pair |
| `:8510–8513` (`checkIndexerSetBorrowStoreEscape`) | the indexer-set half of the same sink | 8 — kept in step by a comment, not by a shared table |
| `:12620–12623` | is this field type a collection | 14 — **includes `"List"`**, a name removed from the language at v0.53 |
| `:18743–18748` | is this head a collection | 14 — no `"List"`, no `"OwningBuffer"` |

A sixth, `src/sema/PureChecker.cpp:562–567` (§22.15 rule 5, enumerator-constructing sources),
spells **16** names including **`"List"` and `"OwningList"`** — neither of which is a builtin
type name at all (`grep '"List"' src/sema/TypeResolution_Names.cpp` → no match). Two dead
entries in a predicate that decides whether a `for` source constructs an enumerator.

This is the mechanism `next-run-queue-2026-08-13.md` §B names **"the run's dominant defect
generator"**: *"The same question answered in more than one place; the copies drift silently
and the failure is always silent (wrong value / leak / double free), never loud."* Its damage
table records the measured cost of four such predicates — owns-frame (**2 copies, a lane added
a wrong 3rd, review found a 4th** → *double free*), positional/auto-bearing (**≥4** → leak +
two rendering divergences), await-site (**a hand-kept list of 12 vs the AST's 17** → awaits in
`const` initialisers and `case` arms never §30.5-checked) — against the one counter-example
that proves the rule, "is this Exception-derived?", **1 copy, by design (#3046)**, whose lane
*"passed review clean."*

### 0.2 The duplication census: this is a known, measured hotspot class

`docs/proposals/duplication-census-2026-08-30.md` §4, measured 2026-08-30:

- **Hotspot 3 — LINQ operator name list (#3589, extends open #3444).** *"10 hand-spellings,
  6 of them 37–44 names. Behaviour DISAGREES: measured the 44-name set
  (`ExprTypecheck_Calls.cpp:5263`) against the 37-name set (`:16970`) — **9 names only in the
  44-set, 2 only in the 37-set** (`First`/`Last` — real LINQ operators, missing from the
  44-set). Two independently-recorded `MinOrNull`/`MaxOrNull` transcription gaps already
  found in this family."* This is the same failure mode as §0.1's five-way disagreement,
  one layer up: the *operators over* collections, name-listed, drifting.
- **Hotspot 2 — §28.9 trivial-dispose classification (#3588).** *"4 copies, ~470 lines …
  **The fourth copy's own commit message says a fix to the shared copy did NOT propagate** —
  measured, not assumed (fix #3191's comment)."* Separability is rated **hard**; §28.9
  v0.237's third disqualifier (owning inline arrays) *"has already drifted once (#2666)
  between these four."* Self-hosting does not fix this predicate, but it removes its hardest
  consumer class: a self-hosted collection is an ordinary class and its dispose triviality is
  read off its own declaration, not off a name list.

**§5 of the census, "Do NOT touch — deliberately load-bearing duplication", constrains this
proposal directly and is honoured here.** Its third entry is about *these exact predicates*:
`isArrayBackedCollectionName` (`BuiltinRegistry.hpp:133`) versus `isArrayEnumeratorWrapSourceName`
(`:107–124`) *"are two different questions over overlapping name sets — collapsing them would
either wrap a hash-table receiver as if it were a dense array (**crash**) or silently unorder a
`SortedSet` iteration (**spec violation**)."* **This proposal therefore does not propose
merging those two predicates**, and SH-5's de-hardcoding is explicitly scoped to *removing
names from a set as a type leaves the intrinsic path*, never to merging two sets. The census's
other two entries (the `GetAsyncEnumerator` self-alias guard; `LlvmBackend_Calls2.cpp`'s
decl-cast soundness gate, #2821) are untouched by every slice here.

### 0.3 Open issues whose root cause is "the compiler special-cases this collection by name"

Read at `29410e29`; every quotation is from the issue body.

| Issue | Measured finding | The name-gate |
|---|---|---|
| **#4299** | *"Three generations of the same by-name list, one session apart"* — `fix #4235` (6 names, 2 call sites), `fix #4245` (a 7th), #4294 (an 8th), with a 9th candidate. A **third** consumer (`ExprTC::groundSatisfiesNew`) *"has no awareness of any of these names at all."* | The null-declaration builtin population: *"there is no TypeSymbol to introspect"* — the exact property a self-hosted type does not have. |
| **#4318** | **6** null-declaration builtins (`Atomic`, `AtomicInteger`, `Channel`, `ChannelSender`, `SortedDictionary`, `SortedSet`) *"still skip their OWN where-constraint check"*; `checkGenericConstraints` *"is gated on a real `TypeSymbol` that these builtins never get."* Measured: `AtomicInteger<ClsB>` violating `where T: value` → **compile exit=0, no diagnostic**. Same for all six. | A prelude-declared type **has** a `TypeSymbol`, so its own `where` clause is checked by the ordinary path. Two of the six are collections. |
| **#4327** | `fix #4309`'s 25-name closed set is *"anti-drift, not anti-omission … a 26th such interface would get **zero signal**."* Cause: *"`Resolver::builtinTypeArity` … is a long chain of `if (n == "X" ‖ …) return arity;` statements, not a `constexpr` array — there is nothing to iterate."* Suggested structural direction: *"give `builtinTypeArity`'s entries an explicit `Kind` tag … the natural completion of issue #4299's consolidation."* | Same table this proposal's SH-3/SH-5 remove entries from (§1.3, `TypeResolution_Names.cpp:113`). |
| **#3896** | **8** collection types (`Set`, `OwningSet`, `SortedSet`, `FrozenArray`, `Dictionary`, `OwningDictionary`, `LinkedList`, `OwningLinkedList`) still emit a placeholder reject for `ToString`/`GetHashCode`/`Equals`/`CompareTo` *"pointing at a CLOSED issue, with no live tracker"* (#2457). The gate is `isRegisteredCollection` — `src/sema/ExprTypecheck_Calls.cpp:20852–20857`, literally the disjunction of the three registry predicates. | §8.1's *universal* Object contract is unimplemented for 8 types **because** they are name-gated builtins. A self-hosted class inherits it. |
| **#3490 / #3489 / #3491** | `SortedSet` with a reference element. #3489: *"There is no `CompareTo`-through-the-vtable comparator anywhere in the compiler"*; the hash backing *"cannot express §4.18's membership rule for references however the comparator is wired"*; and *"the `SortedDictionary<ClassK, V>` path that takes exactly this route today iterates in insertion order and SEGVs on `FirstKey`."* #3491, measured on `0c5ecd1`: `countAfterDupAdd=3` where §4.10 requires 2, plus a `TryAdd` that *"does not lower a value"* (#2434 zero-init degrade). #3490: `new SortedSet<int?>()` degrades, and *"nothing checks stdlib §4.18's `where T: IComparable<T>` constraint at all for the builtin collections"*. | Each is one intrinsic backing chosen by name, whose behaviour then contradicts the type's own spec text. |
| **#2647** | `SortedSet` literal takes the `bz_array_alloc` path while `SortedSet` *"is documented elsewhere in the codebase as sharing the `bz_dict_t*` representation."* *"Those two cannot both be right."* | A representation chosen per call site from a name set. |
| **#2645 / #2648** | The owning element-disposer stamp. #2645: `new OwningBuffer<T>(n)` allocates via `bz_array_alloc_count_n` and *"`return`s the buffer … BEFORE control reaches the `tagOwningElems` wrapper"* → **NULL header disposer**. #2648: *"`lowerNew` and the literal sites key on different heads"* — raw `tnode->name->parts[0]` vs alias-transparent `headName()` — so *"an **aliased** `OwningArray` is stamped when constructed through a literal but not through `new`"*; and an **imported** owning literal is never stamped at all. | Both are "which head string did this site read", i.e. the name gate itself. |
| **#3576** | The §5 element-access terminals *"do not lower on **any** member of the dictionary family"*; the local *"compiles, links, runs, and yields a zero-filled entry. Silently wrong, not rejected."* Measured: `dictFirst=0`, `dictLast=0`, `dictElemAt1=0` against expected `330`, `110`, `110`. | Sema types it correctly; codegen has no arm **for that name set**. |
| **#2296** | `Set<C>.Add` with a class element does not deduplicate: *"the dedup-on-insert handler … is gated on `isOwnedSetRecv && arrElemIsClass && arrIsOwning` — i.e. **`OwningSet` only**."* `s.Count` → **2**, §4.10 requires **1**. And the literal form of the same set *"correctly yields 1"* — so *"`Set<C>.Add` is now the odd one out and disagrees with the literal built from the same elements."* | The canonical instance: one behaviour, two name-gated call sites, disagreeing on one receiver. |
| **#545** | The tracking issue. *"Codegen pattern-matches method names (`arr.Add` → `bz_array_add`); there are ZERO Bezel-source (`.bzl`) stdlib files. Element memory management … is hand-wired **per collection** in codegen. Consequences: duplication, latent gaps … and every escaping-refcount sub-slice of #429 has to re-derive the retain/release pattern for each collection."* | — |
| **#547** | *"Implement these as **self-hosted Bezel collections (#545), NOT native C** … **adding it natively now would actively worsen the #429 burden.**"* | **Not followed.** `SortedSet` and `SortedDictionary` shipped natively, and #3490 / #3489 / #3491 / #2647 above are the bill. This is the proposal's strongest single piece of evidence, because it is a prediction that was made, ignored, and came true. |

Two more found during this refresh, same root cause, not previously linked to 0005:

- **`src/ownership/Ownership_Checks.cpp:12620` and `src/sema/PureChecker.cpp:563`** each carry
  a dead `"List"` (and PureChecker a dead `"OwningList"`) — names the language removed at
  v0.53 and reserved unused. Nothing detects them, for the reason #4327 gives: the population
  is not enumerable.
- **#3489's item 4 is now stale in the *fixed* direction and is worth recording as such**: it
  reports `SortedSet` absent from *both* §15.4 fresh-store whitelists; at `29410e29` both
  (`:7968–7971`, `:8510–8513`) list it, with a comment saying it *"joins the whitelist in the
  same commit that makes it reachable, per #3428's explicit request."* The third and fourth
  lists in the same file still do not. That is the drift shape, caught mid-drift.

### 0.4 The defect-campaign families self-hosting would shrink

`docs/proposals/defect-campaign-2026-09.md` (on `main`). Which members, and why:

| Family | Members self-hosting reaches | Why |
|---|---|---|
| **E** — collection and inline-array literal initializers (§15.1, §15.4); **13 open** | #2648 and #2645 (the disposer stamp), #2327 (owned `Array` field from a fresh literal miscompiles to `Count=0`), #2762 (literal on a borrowing `Array` field never disposed), #3465 | The family's own mechanism statement is a name-gate: *"the element-disposer stamp is keyed at `lowerNew` on a different head than at the literal sites (#2648)."* A self-hosted collection's constructor **is** the store, so there is no second head to key on. The other 8 members (#4096, #4427, #4351, #3393, #4332, #2676, #2672, #2685) are *inline-array* and target-typing defects that self-hosting does **not** touch — stated so the claim is not inflated. |
| **G** — iterator and enumerable heads (§29.2, §29.11); **21 open** | #2307, #3478/#3476/#3479 (the owning-head twins), #2297, #1839, #3522/#1776 (`for` over a user class) | *"The runtime representation behind an `IEnumerable<T>` … head … is chosen from the **declared head** at one **face**."* A self-hosted collection has exactly one representation — its own class — so the face×head matrix collapses for those types. **Caveat, stated honestly:** the family's fix is *"one bridge … keyed on the **runtime** representation not the declared head"*, and that bridge is needed whether or not collections self-host. Self-hosting shrinks the family's *input set*; it is not an alternative to the bridge. #3522/#1776 (`for` over a user class) is the one that cuts the other way: self-hosted collections make that path **hotter**, so it must be fixed first. |
| **K** — silent degrade, the `internal:` Warning sites (§22.3, §28.3); **21 open** | #3576 (dictionary terminals → zero-filled entry), #2296 (`Set.Add` no dedup), #4207, #2221 (`array.Sort` on a class `Array` is a heap-use-after-free with **no diagnostic**) | The degrade happens precisely where *"codegen has no arm"* for a name×method pair. A self-hosted method either compiles or does not; there is no name-keyed arm to be missing. The family's own ratchet argument applies here too: `grep -c 'internal:' src/codegen/*.cpp` at `00fb0ea0` returns **99 lines** against #2924's **52 sites on `c7404bf`** — *"the population has roughly doubled."* |
| **M** — release-eligibility predicates that answer a different question (§28.3, §28.9, §9.7); **17 open** | #2636 (*"erased `IReadOnlyList` owned-field free assumes an Array backing"*), #3632, #4301 | The family's mechanism is *"predicates that test **declaration kind** rather than **content**"* — the name gate in its purest form. #2636 is literally "the compiler guessed the backing from the interface name." |

**Family A (#2718, the refcounted capture cell) — checked, and the answer is conditional.**
Spec §28.11's element-category table (v0.306) classifies **function types** as
*runtime-managed references*: they are **admitted in the borrowing column** (as shared
references) and **rejected at instantiation in the owning column**. So:

- For **SH-3** (`OwningStack<T> where T: class`, the owning family) family A is **not** a
  prerequisite — an owning collection can never hold a closure.
- For **SH-4** (borrowing `Stack<T>` / `Queue<T>`) it **is**: a self-hosted borrowing
  collection with a function-typed element puts closure storage into an ordinary user class
  field, and v0.298 makes a closure *a borrow-carrying value admitted wherever such a value is
  admitted*, so the §28.3 source-set rule and family A's cell machinery both become live on a
  path they do not cover today. Family A's shared surface is `grep -c` over `src/` for its five
  decision-machinery names → **219 lines** (campaign §2, family A) — that is the machinery
  #2718 retires and that SH-4 would otherwise have to satisfy as it stands.

That is the precise form of the coordinator's question, checked rather than asserted.

### 0.5 The prerequisite has moved: a `.bzl` prelude now exists

§3.1 below says *"There is **no `.bzl` prelude**"* and describes `injectStdlibPrelude` as
injecting *"one synthesized AST declaration, `Result<T, E>` (#2047), not a compiled `.bzl`
file."* **That is superseded.** At `29410e29`, `src/driver/Driver.cpp`:

- `kResultPreludeSource` (`:62–74`) and `kIoPreludeSource` (`:89–101`) are **verbatim Bezel
  source text**, including a *generic choice with `owned` payloads* (`Result<T, E>` with
  `Ok(owned value: T)`) and three enums.
- `injectOnePrelude` (`:110–153`) runs the **real** `Lexer` and `Parser` over that text into a
  private `DiagnosticEngine`, applies user-shadowing (*"an auto-imported prelude type is
  SHADOWED by a user top-level declaration of the same name"*, pinned by
  `tests/codegen/result_stdlib_2047_test.cpp`), marks each decl `isSynthesizedPrelude` so
  codegen emits **internal linkage** (`LlvmBackend_Setup.cpp:2106`, `:2466`) and the metadata
  builder skips it (`ModuleMetadataBuilder.cpp:2562`), and prepends the decls to the unit.
- `injectStdlibPrelude` (`:156–161`) is called at **three** pipeline sites (`:190`, `:484`,
  `:681`), and the concatenated prelude text is **an input to the AOT object cache key**
  (`:242–261`: *"the same user source compiled against two different preludes can produce two
  different objects"*).

**What this changes for SH-1.** The slice as written (compile the prelude to a `.bezel.meta`
object and load it through `module::loadDependency`) is now the *harder* of two available
designs, and the simpler one is already in production. The residual questions are real but
narrower than §3.2 states, and they are the ones a scheduled SH-1 must answer:

1. **Does the source-text prelude carry a `class`, not just a `choice` and an `enum`?** The
   mechanism is decl-kind-agnostic (`dynamic_cast<ast::TypeDecl*>`), but no class has ridden
   it. Unverified by reading alone.
2. **Internal linkage means one private copy per module.** For `Result<T, E>` that is stated
   to be safe (*"Choices are not embedded in module metadata, so the injected `public` decl
   never collides"*). For a **collection** it is a live design question: is an
   `OwningStack<T>` created in module A the same *type* as one in module B, and what happens
   when one crosses an `--import` boundary? Today's intrinsics have one identity by
   construction. **This is the real D1**, and it is sharper than "where does the file live."
3. **Code size.** Every module gets a private copy of every prelude collection it instantiates.
   Not measurable without a build (#4451).

So: SH-1's *mechanism* risk has dropped substantially; its *identity* risk has surfaced and is
now the slice's dominant unknown.

### 0.5.1 What did NOT land (checked)

`grep -rn prelude|bootstrap src/`, `ls src/stdlib`, `find . -name '*.bzl' -path '*/src/*'`:
**no `src/stdlib/` directory exists, and there is not one `.bzl` file anywhere under `src/`.**
The only Bezel source the compiler compiles is the two `const char[]` prelude strings above.
#545's *"there are ZERO Bezel-source (`.bzl`) stdlib files"* remains true for the collections.

---

## 1. Current State

### 1.1 C++ Runtime Implementations

Every stdlib collection is implemented in
`src/runtime/bezel_runtime_collections.cpp` (**3,286 lines** at `29410e29`; was
~2,980 at the 2026-07-30 refresh — the former monolithic `bezel_runtime.cpp` has
been split into per-domain `bezel_runtime_*.cpp` files)
as `extern "C"` functions the LLVM backend calls directly.

**`bz_array_t`** (struct — *moved: was `bezel_runtime_internal.hpp:94–108`, now at
`:149`*) backs `Array<T>`,
`OwningArray<T>`, `Stack<T>`, `OwningStack<T>`, `Queue<T>`, `OwningQueue<T>`,
`Set<T>`, `OwningSet<T>`, `SortedSet<T>`, `FrozenArray<T>`, and `OwningBuffer<T>`
(*the last two names were added to the array-backed family after this draft — see
§1.3*).  Key functions (*moved: was "lines 10–222; the LIFO drain helper at 1191"*):

| Function | `bezel_runtime_collections.cpp` | Serves |
|---|---|---|
| `bz_array_alloc` | `:80` | `new Array<T>()`, `new Stack<T>()`, etc. |
| `bz_array_alloc_count_n` | `:108` | `new OwningBuffer<T>(n)` |
| `bz_array_add` | `:143` | `xs.Add(x)`, `xs.Push(x)`, `xs.Enqueue(x)` |
| `bz_array_get` / `bz_array_set` | `:151` / `:156` | `xs[i]` get/set (value elements) |
| `bz_array_set_disposing` | `:165` | `xs[i] := v` (class elements, OwningArray) |
| `bz_array_clear_disposing` | `:218` | `OwningArray.Clear()` |
| `bz_array_pop_at` | `:251` | `OwningArray.PopAt(i)` — transfer ownership without disposal |
| `bz_array_free` | `:284` | scope-end `Dispose()`, class elements reversed (§28.11) |
| `bz_array_mark_disposed` | `:311` | checked-disposal quarantine (spec §28.2; issue #621) |
| `bz_array_drain_next_lifo` | `:1347` | Stack drain (LIFO order) |

**`bz_dict_*`** (*moved: was "lines 320–1163"; `bz_dict_alloc` is now at `:585` and
the family runs to roughly `:1560`*) backs `Dictionary<K,V>`,
`OwningDictionary<K,V>` and `SortedDictionary<K,V>`.  Includes separate string-key
(`bz_dict_*_str`) and generic-key (`bz_dict_*_gen`) variants because Dictionary key
hashing is runtime-polymorphic. `Set`/`OwningSet`/`SortedSet` also reach
`bz_dict_alloc_generic` for eligible element types while being registered in the
*array-backed* name set — the representation split #2647 and #3489 are about.

**`bz_llist_*`** (*moved: was "lines 1330–1601"; `bz_llist_alloc` is now at `:1569`,
`bz_llist_free` at `:1733`*) backs `LinkedList<T>` and
`OwningLinkedList<T>`.  Uses a separate doubly-linked node struct.

**`bz_buffer_*`** (*moved: was "lines 2931–2978", now `:3255–3286`*) backs `Buffer<T>`
(value-element fixed-capacity primitive, issue #545).  Three functions only:
`bz_buffer_new` (`:3255`), `bz_buffer_get_ptr` (`:3273`), `bz_buffer_free` (`:3281`).

**`OwningBuffer<T>`** with class-typed T is *not* `bz_buffer_*`; it reuses
`bz_array_t` (see `bz_array_alloc_count_n`, *moved: was
`bezel_runtime_collections.cpp:31`, now `:108`*; confirmed in
`LlvmBackend_Exprs2.cpp` — *moved: was `:2743–2760`, now two arms at `:5596` and
`:5708`, both declaring `bz_array_alloc_count_n`*).

### 1.2 Codegen Lowering — How `new Array<T>()`, `xs.Add(x)`, `xs[i]` are Lowered

**Construction** — `LlvmBackend_Exprs2.cpp` (13,252 lines; the dispatch moved
out of `LlvmBackend_Exprs.cpp`). *Every line below moved at this refresh.* A
name-switch on `colName` — bound at **`:4981`** (*was `:2613`*) — dispatches each
collection type to the appropriate runtime allocator, over roughly `:4981–:6400`
(*was "lines 2611–3010"*):

| Target | `bz_*` entry point | Now at |
|---|---|---|
| `Array`/`OwningArray`/`Stack`/`OwningStack`/`Queue`/`OwningQueue`/`Set`/`OwningSet` | `bz_array_alloc` | `:5760`, `:5902` (*was `:2918`*) |
| `OwningBuffer<T>(n)` | `bz_array_alloc_count_n` | `:5598`, `:5710` (*was `:2756`*) |
| `LinkedList`/`OwningLinkedList` | `bz_llist_alloc` | `:5406` (*was `:2730`*) |
| `Dictionary`/`OwningDictionary`/`SortedDictionary` | `bz_dict_alloc` / `bz_dict_alloc_generic` | `:5890` / `:5266`, `:5885` (*was "lines 2985–2990"*) |
| `Buffer<T>(n)` | `bz_buffer_new` | `:6329` (*was `:3289–3335`; see §2.2*) |
| `Set`/`OwningSet`/`SortedSet` comparer ctors | *rejected* — `rejectUnimplementedComparerCtor` | `:4645`, dispatched at `:5201` |

**Method calls** — `LlvmBackend_Calls.cpp` `tryLowerCollectionInstanceCall`
(*moved: entry was `:681`, now **`:1813`***). The type-name predicates are no longer
called at the function's head (a static-call re-target for `Array.Sort(a)`,
fix #3345, runs first); the first `isCollectionTypeName` call inside the function is
at `:1882` (*was "lines 701–702"*), and the three predicate **definitions** are
`isCollectionTypeName` `:821`, `isLinkedListTypeName` `:844`, `isDictTypeName` `:859`
(*was `:397–427`*). Method-name string dispatch follows: `"Add"`/`"Push"`/`"Enqueue"`
at **`:5810`** (*was `:3954`*); `"Pop"`/`"Dequeue"` at **`:6371`** (*was `:4507`*).

**Indexer get/set** — `LlvmBackend_Assign.cpp` (14,468 lines):
`lowerCollectionIndexerGet` (*moved: was `:164`, now **`:326`***; it superseded the
former `tryLowerIndexExpr` in `LlvmBackend_Exprs.cpp`) and
`lowerCollectionIndexerSet` (*moved: was `:655`, now **`:936`***) dispatch on the same
type-name predicates — `isCollectionTypeName` is consulted from this file at `:514`,
`:770`, `:1069`, `:4377` and `:4798`.

### 1.3 Quantified "Hardcoded Collection Name" Surface

A grep for `"Array"`, `"OwningArray"`, `"Dictionary"`, `"OwningDictionary"`,
`"Stack"`, `"OwningStack"`, `"Queue"`, `"OwningQueue"`, `"Set"`, `"OwningSet"`,
`"LinkedList"`, `"OwningLinkedList"`, `"OwningBuffer"`, `"Buffer"` across
`src/` (excluding `.h`/`.hpp` headers and test/ directories) yielded **~385 sites**
at the 2026-07-30 refresh. **Re-run 2026-09-17 at `29410e29` with the same recipe: 634.**

| Subsystem | 2026-07-30 | 2026-09-17 |
|---|---|---|
| `src/sema/` | ~194 | **318** |
| `src/codegen/` | ~137 | **217** |
| `src/ownership/` | ~38 | **38** |
| `src/common/` (the #1414 stage-3 registry) | ~15 | **61** |
| **total** | **~385** | **634** |

The registry consolidated the *sets*; it did not stop the *sites* growing. §0.1
decomposes the growth and adds two counts the original did not take (58 hand-spelled
disjunctions across 18 files; per-name reach of 36-78 sites).

Representative clusters (the ones a self-host migration would have to displace) —
*every anchor re-verified and updated 2026-09-17*:

- **`TypeResolution_Names.cpp`** — `Resolver::builtinTypeArity`, the master list of
  which names are built-in generics. *Moved: was `:44–296`; the function now begins at
  **`:113`** and returns `kNotBuiltin` at **`:450`***. Collection entries are no longer
  contiguous: `Array`/`OwningArray` `:209`; `Stack`/`OwningStack`/`Queue`/`OwningQueue`/
  `Set`/`OwningSet`/`LinkedList`/`OwningLinkedList` `:264–267`; `OwningBuffer` `:276`;
  `FrozenArray` `:286`; `Dictionary`/`OwningDictionary` `:306`; `SortedSet` `:312`;
  `Buffer` `:380`. Removing a name from this list requires it resolve as an ordinary
  `TypeSymbol` instead. **#4327 is about this function**: it is *"a long chain of
  `if (n == "X" || ...) return arity;` statements, not a `constexpr` array — there is
  nothing to iterate at compile time or test time."*
- **`LlvmBackend_Calls.cpp`** — `isCollectionTypeName`, `isLinkedListTypeName`,
  `isDictTypeName`, the predicates that gate every method-call lowering path.
  *Moved: was `:397–427`; now **`:821`**, **`:844`**, **`:859`***. The name sets live in
  the #1414 stage-3 registry — *moved: was `src/common/BuiltinRegistry.cpp:69–87`, now
  **`:199`** (`kArrayBackedCollections`, **11** names), **`:214`** (`kDictionaryNames`,
  **3**), **`:219`** (`kLinkedListNames`, **2**), with the predicate wrappers at `:922`,
  `:926`, `:930`*. The array-backed set has grown by two names since the last refresh:
  `SortedSet` (#1426) and `FrozenArray` (#3249).
- **`LlvmBackend_Exprs2.cpp`** — the `new Coll<T>()` dispatch switch. *Moved: was
  `:2611–3010`, now `:4981` to roughly `:6400` — see §1.2's table.*
- **`ExprTypecheck_Exprs.cpp`** (11,377 lines) — sema checks keyed on literal
  collection name strings. *The three anchors `:914–919, 1487–1519, 1996–2001` no
  longer point at collection code; the equivalents are*: **`:1848–1854`** (the
  auto-imported stdlib class-name list — `Array`/`OwningArray`/`Dictionary`/
  `OwningDictionary`/`Stack`/`OwningStack`/`Queue`/`OwningQueue`/`Set`/`OwningSet`/
  `LinkedList`/`OwningLinkedList` spelled out beside `Console`/`Math`/…),
  **`:2763–2776`** (the indexer element-type gate: `Buffer<T>[i]` at `:2763`, then the
  `isArrLike` disjunction at `:2768`), and **`:4488–4500`** (the `Buffer<T>`
  value-element restriction; see §2.2).
- **`src/sema/ExprTypecheck_Calls.cpp:20852–20857`** — *new at this refresh*:
  `isRegisteredCollection`, literally the disjunction of the three registry predicates,
  gating the §8.1 Object-contract placeholder reject. **This is #3896's site**: eight
  collection types cannot answer `ToString`/`GetHashCode`/`Equals`/`CompareTo` because
  they are name-gated builtins.
- **`LlvmBackend_Disposal.cpp`** — `emitOwnedFieldDisposals` (moved out of
  `LlvmBackend_Setup.cpp`): the owned-field disposal loop that identifies a field
  as a "collection" (hence requiring `bz_array_free` / `bz_llist_free`) via
  `ownedCollectionFieldType`, which applies
  `isCollectionTypeName`/`isLinkedListTypeName`/`isDictTypeName`.
  *Moved: `emitOwnedFieldDisposals` was `:415`; its **definition** is now **`:958`**, it
  is **called** from the disposer emitter at `:429`, and its collection arm — the
  `ownedCollectionFieldType` test — is at **`:1084`**. `ownedCollectionFieldType` was
  `LlvmBackend_impl.hpp:5498`, now **`:10855`**.*
  **This is the deepest coupling**: a self-hosted collection FIELD in another
  class must still trigger the correct destructor sequence at the field owner's
  Dispose time. If the collection is a user class, the standard vtable-dispatch
  path handles this automatically — this is actually a simplification.
- **`src/ownership/Ownership_Checks.cpp`** — *new at this refresh*: five independent
  hand-spelled collection-name disjunctions with five different memberships, tabulated
  in §0.1. The ownership pass's *count* did not grow (38, unchanged) but its
  *disagreement* is now measured, and two of its entries (`"List"`, and
  `"OwningList"` in the sibling `src/sema/PureChecker.cpp:563`) name types the
  language removed at v0.53.

---

## 2. Foundation Already in Place (#545)

### 2.1 Element-Policy Unification

`src/codegen/LlvmBackend_ElementPolicy.cpp` (*was 280 lines; now **455***) implements
`Emitter::elementPolicyFor(elemTy, owningFamily)` — the §28.11 table
translated into code (**`:72`**, unchanged — the one anchor in this document that did
not move). §28.11's table is unchanged in substance at v0.306; its element-category
rows are re-read in §0.4 for the function-type row that ties SH-4 to campaign family A.
This factory returns an `ElementPolicy` struct
whose `disposerThunk` field is the correct LLVM thunk for any element category
(ClassOwned, ClassBorrow, RuntimeManaged, Value), derived from the element type
alone rather than from per-collection name switches.

This unification (issues #602, #619, #545 batch 4) means that any new collection
type — whether intrinsic or self-hosted — automatically gets the right per-element
lifecycle if it calls `elementPolicyFor` or if its disposal flows through the
vtable (which standard user classes do).

### 2.2 `Buffer<T>` Primitive (Value-Element Fixed-Capacity)

`TypeResolution_Names.cpp` (*moved: was `:233`, now **`:380`***): `Buffer<T>`
registered with arity 1.
`src/runtime/bezel_runtime_collections.cpp` (*moved: was "lines 2931–2978", now
**`:3255–3286`***): three C functions
(`bz_buffer_new` `:3255`, `bz_buffer_get_ptr` `:3273`, `bz_buffer_free` `:3281`).
`LlvmBackend_Exprs2.cpp` (*moved: was "lines 3289–3335", now **`:6291–6335`***):
`new Buffer<T>(n)` lowered to `bz_buffer_new(len, elem_size)` (declared at `:6329`).

`Buffer<T>` is restricted to primitive value types (int/byte/short/long/double/
decimal/char/bool) per the `TypeResolution_Names.cpp` comment (*moved: was
`:230–232`, now **`:377–379`***) and the sema enforcement at
`ExprTypecheck_Exprs.cpp` (*moved: was `:2380–2405`, now **`:4488–4500`***, which
superseded the comment-only note formerly cited in `LlvmBackend_impl.hpp`).
Class-element `Buffer<T>` is the §5.20 deferred design pass — **still deferred at
v0.306**; rationale §5.20's closing paragraph ("The deferred half: a borrow-storing
primitive") is unchanged and is re-read in §6 SH-4, where it is the reason the
borrowing family cannot be self-hosted below the wrapper level.

### 2.3 `OwningBuffer<T>` with Class T

Per stdlib §4.14 and the `OwningArray<T>` reference implementation — ***no longer
exists at the cited path**: `tests/conformance/cases/stdlib_0039.bzl` is now a
`Result<T, E>` connect-or-explain case; the reference implementation is
**`tests/conformance/cases/stdlib_0051.bzl`***, whose opening is
`public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>,
IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class` with
`private owned _storage: OwningBuffer<T>;` — `OwningBuffer<T>` with class T is the
intended storage primitive for every self-hosted owning collection. The same source
is quoted in the normative stdlib at §4.5 (*"`OwningArray<T>` composes over
`OwningBuffer<T>` (§4.14) in pure Bezel; the standard library provides this
implementation directly, and user code is welcome to inspect or extend it"*).
Two sibling cases carry earlier declaration-only forms: `stdlib_0049.bzl`,
`stdlib_0050.bzl`.

The compiler currently lowers `OwningBuffer<T>` (class T case) as a `bz_array_t*`
with `bz_array_alloc_count_n` (construction), `bz_array_get/set/set_disposing`
(indexer), and `bz_array_free` with the element disposer from `elementPolicyFor`
(disposal). The `move buf[i]` / field-restoration pattern that the reference
implementation uses is exercised by `generic_owned_field_disposal_test.cpp` (the
`Box<T>` test — *moved: was "lines 145–166", the `kBoxOfClassDisposedOnce` fixture
is now at **`:151–170`***) and the move-interface-slot tests in
`tests/ownership/`. ⚠ **Known defect on this exact path**: #2645 measures that
`new OwningBuffer<T>(n)` *"returns the buffer … BEFORE control reaches the
`tagOwningElems` wrapper"*, so a class-element `OwningBuffer` gets a **NULL header
disposer**. Leak-direction only, and the issue itself records that it is *"not
reproduced by probe"* — but it is on the storage primitive this whole proposal
composes over, which is why SH-2 (§6) is the slice that earns its keep first.

**Conclusion:** `OwningBuffer<T>` for class T is implemented and tested at
the intrinsic level. The stdlib reference implementation of `OwningArray<T>`
(stdlib §4.5 and `tests/conformance/cases/stdlib_0051.bzl`) is purely
expressed in Bezel source and is the template for self-hosting. The compiler
already understands every operation that source uses — *with the #2645 caveat
above, and subject to SH-2's round-trip proof rather than to reading.*

---

## 3. The Prelude-Bootstrap Gap

### 3.1 How Stdlib Types Are Registered Today

> **Superseded in place, 2026-09-17.** The paragraph below said *"There is **no
> `.bzl` prelude**"* and described the `Driver.cpp` hook as injecting *"one
> synthesized AST declaration … not a compiled `.bzl` file."* **Both halves are now
> false.** At `29410e29` the driver holds two *verbatim Bezel source* preludes
> (`kResultPreludeSource` `:62–74`, a **generic choice with `owned` payloads**;
> `kIoPreludeSource` `:89–101`, three enums), runs the real `Lexer`/`Parser` over
> them (`injectOnePrelude` `:110–153`), applies user-shadowing, marks the decls
> `isSynthesizedPrelude` for internal linkage, and prepends them — at three pipeline
> sites, with the prelude text keyed into the AOT object cache. **§0.5 is the current
> statement of this prerequisite**, including the two residual questions (does a
> `class` ride the mechanism; what is cross-module *type identity* under internal
> linkage) that replace "does a loader exist" as SH-1's real risk. The rest of §3 is
> kept because its *name-resolution* argument is unchanged and still correct.

There is **no `.bzl` prelude** *(false as of §0.5)*. Every stdlib type is registered
as a builtin by
name inside `builtinTypeArity` (*moved: was `TypeResolution_Names.cpp:44–296`, now
`:113–450`*). A type not in
that function gets `kNotBuiltin` (*moved: was "line 296", now `:450`*) and causes an
"unresolved type name"
diagnostic. (The `injectStdlibPrelude` hook — *moved: was `Driver.cpp:80`, now
`:156`* — is a real source-text prelude; see §0.5.)

The `TypeRef` for a builtin resolves with `Kind::Named`, `declaration = nullptr`
(no `TypeSymbol`), and an `astNode` pointing at the AST type reference node.

> **Superseded in place, 2026-09-17 (mechanism changed, conclusion holds).** The
> next sentence said every codegen predicate checks `rt->declaration == nullptr`
> **plus** the string name. `isCollectionTypeName` (`LlvmBackend_Calls.cpp:821–843`)
> now reads: *"Reject a USER-CLASS declaration … `rt->declaration` can be null …
> **OR a `builtinClassSym`** (the declaration stamped for a static-factory result
> like `FrozenArray<T>.From(...)`); both are accepted"*, and the actual test is
> `if (rt->declaration && (rt->declaration->decl || !rt->declaration->moduleName.empty()))
> return false;`. **The conclusion is unaffected**: a prelude-loaded `TypeSymbol` has
> `decl` set (and, if module-scoped, a non-empty `moduleName`), so the predicate still
> returns false and the type still falls through to the standard class dispatch. But
> the *reason* is now "has a real declaration or a module", not "has any declaration",
> and SH-3 must assert against the current form, not the old one.

All
codegen predicates (`isCollectionTypeName`, `isLinkedListTypeName`, etc.) check
`rt->declaration == nullptr` **plus** the string name before routing to the
runtime call path *(respelled above)*. A user-defined class would have
`rt->declaration != nullptr` *with `decl` set*,
causing all those predicates to return false and the code to fall through to the
standard class dispatch.

### 3.2 Designing the Prelude-Bootstrap Loader

**Goal:** Before compiling user code, parse and typecheck a stdlib prelude
(one or more `.bzl` files containing the collection class definitions), register
their `TypeSymbol*` into `SemaContext`, and let codegen handle them via the
standard user-class path.

**Hook point:** `Driver::compileFile` (*moved: was "`src/driver/Driver.cpp` lines
165–339; thin forwarding overloads at 153/159". The file is now 795 lines; the
four-argument `compileFile` is at **`:371`**, with thin forwarders at **`:346`** and
**`:352`**, and the front end proper has been extracted into
`compileExecutableObject` + `linkExecutableObject` — `compileFile` is now "exactly
the composition of its two halves"*). The pipeline today is:

```
readFile → Lexer → Parser → injectStdlibPrelude → collectSymbols
  → (loadDependencies) → resolveTypes → typecheckExpressions → checkPureBodies
  → checkDefiniteAssignment → checkStaticInitCycles → checkOwnership → codegen
```

The prelude loader would run between `SemaContext semaCtx(diagnostics)`
(*moved: was "line 203", now `:488`*) and `collectSymbols(*unit, semaCtx)`
(*moved: was "line 204", now `:489`*) — or equivalently, as
the first step of `collectSymbols`, loading the prelude's symbols before the user
unit's symbols so user code can reference them. ***In the shipping mechanism the
injection is earlier still*** — `injectStdlibPrelude(*unit)` at `:484`, i.e. before
`collectSymbols` sees the unit at all, because the prelude's decls are prepended to
the AST rather than registered as imports.

**Mechanism (option A — the metadata route this draft assumed):** Reuse the existing
cross-module loader infrastructure
(`module::loadDependency` / the batched `loadDependencies` — *moved: was
`ModuleLoader.cpp:1523/1546`, now **`:5222`** and **`:5293`** in a 5,459-line file*).
The stdlib prelude is compiled
once to an object + metadata (the `.bezel.meta` section) and loaded as a special
"always-present" dependency. The `SemaContext::importedByModule_` table
(*moved: was `SemaContext.hpp:382` with "registration/lookup at lines 201–239"; the
member is now declared at **`:1028`**, its accessor at **`:634`**, and
registration/lookup are at **`:483`** and **`:544`***) would map a
`"System"` or `"System.Collections"` module
name to the prelude's exported `TypeSymbol*` objects.

**Mechanism (option B — the route that already exists, added 2026-09-17).** Extend
`injectStdlibPrelude` with a third source string (or a third source *file*, embedded).
Costs nothing to build: no CMake ordering, no `.bezel.meta`, no new loader. Costs
**one private copy of every prelude collection per module** (internal linkage,
`LlvmBackend_Setup.cpp:2106`/`:2466`) and puts cross-module **type identity** in
question — see §0.5, question 2. `Result<T, E>` gets away with it because *"choices
are not embedded in module metadata"*; a collection crossing an `--import` boundary
is a different question, and it is the one **D1 should now be asked about**.

**What changes in name resolution:** `TypeResolution_Names.cpp`'s
`builtinTypeArity` currently returns a nonzero arity for collection names so they
don't fail "unresolved type." Once a type's `TypeSymbol*` exists in the sema
context (from the prelude), `resolveTypes` finds it via the normal symbol-lookup
path, `builtinTypeArity` is not consulted, and `rt->declaration != nullptr` makes
every `isCollectionTypeName`/etc. predicate return false. The collection then
routes through the standard user-class codegen path.

**Incremental approach:** Remove each type from `builtinTypeArity` only after
its prelude `.bzl` file is loaded. This lets the existing and the self-hosted
paths coexist during migration, type by type.

**Owner decision required (D1):** Where does the prelude `.bzl` live in the
repository? Options: (a) `src/stdlib/` compiled as a separate target and linked
in, (b) embedded as a binary blob in the compiler binary (reproducible but
verbose), (c) shipped alongside the compiler executable and loaded from disk.
Option (a) is the cleanest; the cross-module loader already handles this workflow.

> **D1 restated, 2026-09-17.** The location question is now the *easy* half and (b)
> is already the shipping answer for `Result`/`System.IO`. **The decision D1 should
> actually carry is: does a prelude collection have ONE type identity across modules,
> or one per module?** Internal linkage gives one per module. That is invisible for a
> choice and an enum; for `OwningStack<T>` it decides whether an
> `OwningStack<Cell>` constructed in module A can be passed to, stored by, or disposed
> by module B. Today's intrinsics have one identity by construction, so this is a
> capability the migration could silently *lose*. Recorded rather than assumed, per
> CLAUDE.md rule 4: **this document does not choose**; it asks.

---

## 4. The Runtime-Sized Owning-Buffer Gap

### 4.1 The Problem

A user-written Bezel class **cannot today hold runtime-sized owning storage
as a field** without `OwningBuffer<T>`. But `OwningBuffer<T>` is itself a
compiler intrinsic — it has no user `TypeSymbol`; its construction, indexer,
`move buf[i]`, and disposal are all hardcoded in the backend.

The reference implementation of `OwningArray<T>` (stdlib §4.5) uses
`OwningBuffer<T>` as its backing field. A self-hosted `OwningStack<T>` would do the
same: `private owned _storage: OwningBuffer<T>`. That is **already valid Bezel
source** because:

1. `OwningBuffer<T>` is a compiler-known type — `builtinTypeArity` returns 1
   for it (*moved: was "line 183", now **`TypeResolution_Names.cpp:276`***).
2. Its owned field in a user class triggers `ownedCollectionFieldType` in
   `LlvmBackend_impl.hpp` (*moved: was `:5498`, now **`:10855`***), which returns the
   TypeRef so
   `emitOwnedFieldDisposals` (*moved: was `LlvmBackend_Disposal.cpp:415`, now
   **`:958`**, with its collection arm at `:1084`*; moved out of
   Setup.cpp) calls `bz_array_free` with
   the correct element disposer from `elementPolicyFor` — **modulo #2645**, which
   reports the `alloc_count_n` construction arm never stamping that disposer (§2.3).
3. The `move buf[i]` and `buf[i] := value` indexer operations on an
   `OwningBuffer<T>` field are lowered by the existing intrinsic paths in
   `LlvmBackend_Assign.cpp` (indexer get/set/move) and `LlvmBackend_Exprs2.cpp`
   (construction).

**`OwningBuffer<T>` as the owned-storage primitive is NOT a gap.** The spec
already designates it as the intrinsic boundary (rationale §5.14, §5.15,
§5.20 — *all three headings re-verified at v0.306; §5.14 `:512`, §5.15 `:533`,
§5.20 `:805` of `docs/Bezel_Design_Rationale.md`*). After v0.99, *"the intrinsic
surface of the entire owning-collection family reduces to one well-scoped primitive
(`OwningBuffer<T>`) **plus the inline-array form (`[N owned]T?`)**; every resizable
owning collection composes over these in pure Bezel"* (rationale §5.14). *The
clause in bold was elided by the earlier draft and is restored here: the intrinsic
floor is two forms, not one.*

### 4.2 Options for Any Remaining Gap

The **only residual gap** is that `OwningBuffer<T>` itself remains a compiler
intrinsic — users cannot rewrite it. The spec explicitly accepts this (stdlib
§4.14: "Why this is a compiler-known primitive"). If the goal is to self-host the
*collections that compose over* `OwningBuffer<T>`, no additional primitive is
needed.

If in the future even `OwningBuffer<T>` must be expressible in Bezel source, the
options are:

- **(a) Expose `OwningBuffer<T>` as a full user TypeSymbol** with a compiler-
  synthesized body for its one intrinsic operation (runtime-sized heap allocation).
  Compiler knows the allocation; everything else (indexer, move, disposal) is
  already standard user-class machinery. Risk: moderate — requires extending the
  prelude bootstrap to handle the one constructor that needs the intrinsic
  allocation call.
- **(b) A new `HeapArray<T>` primitive** at the FFI layer (`extern class`). Risk:
  requires spec amendment (out of scope for implementation PRs per CLAUDE.md).
- **(c) Defer to the §5.20 design pass** — the spec explicitly defers class-
  element `Buffer<T>` and the borrow-storing primitive to future design work.
  Accept `OwningBuffer<T>` as the permanent intrinsic floor.

**Recommendation:** Option (c). Keep `OwningBuffer<T>` as the intrinsic floor
and self-host only the collections that compose over it. This matches the spec's
own stated intent and avoids a spec amendment.

**Owner decision required (D2):** Confirm that `OwningBuffer<T>` remains the
permanent intrinsic floor for this run, and that the self-hosting goal is
"collections composed over `OwningBuffer<T>`" rather than `OwningBuffer<T>` itself.

---

## 5. Proof-of-Concept Target: `OwningStack<T>`

### 5.1 Why `OwningStack<T>` First

- **Smallest API surface.** *Re-counted against stdlib **§4.11, v0.306**: **10**
  declared members, not 8* — `FromCollectionLiteral` (static, the §2.19 witness),
  `constructor()`, `Push`, `Contains`, `Pop`, `Peek`, `TryPop`, `TryPeek`, `Clear`,
  `Drain`. The explicit `Contains` was added at v0.222 (#1811); the
  `FromCollectionLiteral` witness and the
  `IOwningCollectionLiteralTarget<OwningStack<T>, T>` conformance it discharges are
  **new since this draft** and are the one genuinely awkward member for a self-hosted
  class (a static factory returning `owned OwningStack<T>` from an
  `owned elements: OwningArray<T>`). `Peek` is declared **`pure`**, so a self-hosted
  body must satisfy spec §22's purity checker — an obligation an intrinsic does not
  have. No resize-with-insert semantics.
- **Composes trivially** over `OwningArray<T>` (whose reference implementation
  in Bezel is already written — *moved: was `tests/conformance/cases/stdlib_0039.bzl`,
  now **`stdlib_0051.bzl`**; see §2.3*).
  In fact, `OwningStack<T>` IS just an `OwningArray<T>` where `Push = Add` and
  `Pop = PopLast`. The self-hosted implementation is approximately 40 lines
  (*~60 with `FromCollectionLiteral` and `Contains`*).
- **No dictionary/hash machinery** (unlike `Set<T>` or `Dictionary<K,V>`).
- **No linked-node pointer chasing** (unlike `LinkedList<T>`).
- **No closures.** Spec §28.11's element-category table rejects runtime-managed
  element types (including function types) in the owning column, so an
  `OwningStack<T> where T: class` can never hold a closure — which is why defect
  family A (#2718) is **not** a prerequisite for SH-3, though it is for SH-4 (§0.4).
- **Has an existing working codegen test** at
  `tests/codegen/collection_test.cpp` (*re-verified: the `Stack<T>`/`Queue<T>` block
  begins at **`:79`**, the value-element `Stack<int>` program at **`:84`***), and the
  owned-element variant is exercised in AOT round-trips by
  `owned_collection_pop_dispose_746_test.cpp` and
  `owned_collection_accessors_test.cpp` — *re-verified 2026-09-17: both contain **zero**
  `runJit(` call sites, so both are already on the strict AOT oracle*. (CLAUDE.md's
  wording is "deprecated, not retired"; the count is ratcheted by
  `scripts/check_deprecated_jit_oracle.py`. The earlier draft's "was retired" is
  respelled here accordingly.) `OwningArray` element disposal is thoroughly tested.

### 5.2 Bezel Source for the POC

The self-hosted `OwningStack<T>` composes directly over the `OwningArray<T>`
reference implementation, or equivalently over `OwningBuffer<T>` directly:

```bezel
module System.Collections;

public class OwningStack<T>: IReadOnlyCollection<T>,
                             IOwningCollectionLiteralTarget<OwningStack<T>, T>
                             where T: class
  // conformance per v0.222 (#1811): OwningStack was demoted to
  // IReadOnlyCollection<T>; the IOwningCollection<T> conformance this draft
  // originally declared was removed at that revision. The
  // IOwningCollectionLiteralTarget<OwningStack<T>, T> conformance and its
  // static FromCollectionLiteral witness are v0.306 stdlib §4.11 surface and
  // were added to this sketch at the 2026-09-17 refresh (see the editorial
  // note after the body for what is still owed).
  private owned _buf: OwningBuffer<T>;
  private _count: int := 0;

  public constructor()
    _buf := new OwningBuffer<T>(8);
  end constructor;

  public method Push(owned item: T)
    if _count = _buf.Length then
      var fresh := new OwningBuffer<T>(_buf.Length * 2);
      for var i in 0..<_count do
        var m := move _buf[i];
        _buf[i] := null;
        fresh[i] := m;
      end;
      _buf.Dispose();
      _buf := fresh;
    end;
    _buf[_count] := item;
    _count := _count + 1;
  end Push;

  public method Pop(): owned T
    if _count = 0 then throw new InvalidOperationException(); end;
    _count := _count - 1;
    var item := move _buf[_count];
    _buf[_count] := null;
    return item!;
  end Pop;

  public pure method Peek(): T          -- `pure` per stdlib §4.11 (v0.306)
    if _count = 0 then throw new InvalidOperationException(); end;
    return _buf[_count - 1]!;
  end Peek;

  public method TryPop(out owned item: T?): bool     -- v0.171 Try convention
    if _count = 0 then item := null; return false; end;
    item := Pop();
    return true;
  end TryPop;

  public method TryPeek(out item: T?): bool          -- out is a borrow
    if _count = 0 then item := null; return false; end;
    item := _buf[_count - 1]!;
    return true;
  end TryPeek;

  public method Contains(item: T): bool               -- v0.222 (#1811)
    for var i in 0..<_count do
      if _buf[i]!.Equals(item) then return true; end;
    end;
    return false;
  end Contains;

  public method Clear()
    -- ⚠ 2026-09-17: this walks FORWARD. Spec §28.11 (v0.306, after the #4442
    -- ruling) requires "reverse storage order … pop order for OwningStack",
    -- i.e. descending index. Left as written so the defect is visible rather
    -- than silently patched; fix with the other §5.2 owed items (see §8).
    for var i in 0..<_count do
      var m := move _buf[i];
      _buf[i] := null;
      m!.Dispose();
    end;
    _count := 0;
  end Clear;

  // Drain: see the IOwningEnumerable pattern (§4.13); deferred to later slice
end OwningStack;
```

[Editorial note, 2026-07-30 refresh, **discharged 2026-09-17**: the drifts the
mechanical refresh flagged but did not rewrite — nullable `Try` outs under the
v0.171 convention, and v0.222's explicit `Contains` — are now written into the body
above, together with `Peek`'s `pure` and the
`IOwningCollectionLiteralTarget`/`FromCollectionLiteral` pair the v0.306 §4.11
surface requires.]

[**Still owed when SH-3 is scheduled (2026-09-17).** Three members of the §4.11
surface are *named* above but not *written*, deliberately, because each is a design
question rather than a transcription:

1. **`FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningStack<T>`**
   — a static factory taking an owned `OwningArray<T>` and draining it into a fresh
   stack. It is the §2.19 witness that discharges the
   `IOwningCollectionLiteralTarget` conformance, and it is the member that makes
   `using var s: OwningStack<Cell> := [a, b, c];` compile. Writing it requires
   `OwningArray<T>.Drain()` or `PopAt`, so it is coupled to whichever of those the
   POC is allowed to use.
2. **`Drain(): IOwningEnumerable<T>`** — deferred by ruling D3 (§6 SH-3).
3. **`Contains`'s equality.** The body above uses `item.Equals(...)`, which for a
   class element is §8.3 reference identity. That matches how a `Stack` should
   behave, but it is asserted here, not derived from §4.11 text, and #3896 records
   that `Equals` on these very types currently *"is declared via the universal Object
   contract (§8.1) but is not yet implemented by this compiler for this receiver"* —
   so the self-hosted version would be the first working one. Confirm against §4.11
   and §8.3 before writing the test, not after.]

All operations in this source body are already working in the compiler:
- `new OwningBuffer<T>(n)` — lowered via `bz_array_alloc_count_n`
- `_buf[i]` indexer get — lowered by intrinsic OwningBuffer indexer path
- `move _buf[i]` — ownership-checker proven by `generic_owned_field_disposal_test.cpp`
- `_buf[i] := null` — field-restoration rule enforced by ownership checker
- `m!.Dispose()` — vtable dispatch to element's Dispose
- `_buf.Dispose()` — OwningBuffer intrinsic disposal (bz_array_free with disposer)
- `owned item: T` parameter and `return item!` owned-return — proven by
  `generic_owned_field_disposal_test.cpp` (Box pattern at line 156–166)
- `where T: class` constraint — sema registered and codegen monomorphization
  proven by `generic_spec_cache_test.cpp`

### 5.3 What the Compiler Must Do

When `OwningStack<T>` is compiled from the prelude (a self-hosted `.bzl` file):

1. **Sema:** resolve `OwningBuffer<T>` as the intrinsic builtin it already is;
   resolve `move _buf[i]`, `_buf[i] := null`, `item!.Dispose()` via the
   normal expression-typecheck paths for class fields and owned returns.

2. **Codegen:** lower the class through the standard `emitClass` path. The
   `Push` method's `_buf[_count] := item` lowers via the standard
   `lowerCollectionIndexerSet` **because `_buf` is still an OwningBuffer
   intrinsic field** — codegen recognizes OwningBuffer fields and emits
   `bz_array_set_disposing`. The Clear loop's `m!.Dispose()` emits vtable
   dispatch.

3. **Disposal:** the compiler-generated `Dispose` for `OwningStack<T>` sees
   `private owned _buf: OwningBuffer<T>`. `ownedCollectionFieldType` (*moved: was
   `LlvmBackend_impl.hpp:5498`, now **`:10855`***) recognizes OwningBuffer as a
   collection field
   (via `isCollectionTypeName(rt)` — `OwningBuffer` is a member of
   `kArrayBackedCollections`, `BuiltinRegistry.cpp:199` — or, for the layout
   question, via the `n == "OwningBuffer"` branch *moved: was
   `LlvmBackend_Layout.cpp:649`, now **`:1090`***).
   The emitted disposal calls `bz_array_free` with the element disposer derived
   from `elementPolicyFor`. This is **unchanged from the current fully-intrinsic
   path** — the self-hosted class's Dispose works identically to today's. *Caveat
   added 2026-09-17: #2645 reports that disposer is never stamped on an
   `alloc_count_n`-constructed buffer, so "identically to today's" may mean
   "identically leaky". SH-2 is the slice that settles it.*

4. **Prelude bootstrap:** after loading the prelude, `TypeSymbol*` for
   `OwningStack` exists in `SemaContext` (via the imported-type path under option A,
   or via the prepended-AST path under option B — §3.2). `builtinTypeArity` returns
   `kNotBuiltin` for `OwningStack`
   (it has been removed from the list at `TypeResolution_Names.cpp:264`).
   `isCollectionTypeName` returns `false` — *respelled 2026-09-17*: because the
   symbol carries `decl` (and, under option A, a `moduleName`), which is what the
   predicate's current reject clause at `LlvmBackend_Calls.cpp:828–830` tests; the
   older "`rt->declaration != nullptr`" phrasing no longer describes the code
   (§3.1). Method calls on `OwningStack` go
   through `tryLowerBuiltinClassCall` (*re-verified: `LlvmBackend_Calls.cpp:9361`*)
   → standard vtable dispatch path.

The net result: user code `using var s := new OwningStack<MyClass>();` compiles
through the same path as any user-defined generic class, with no special-casing
in the backend for `OwningStack`.

---

## 6. Sequencing and Risk

The work divides into six slices. Each slice is a candidate for one or more PRs
within a dedicated self-hosting run.

### 6.0 Re-sequencing against the 2026-09 defect campaign (added 2026-09-17)

The campaign (`defect-campaign-2026-09.md`) runs in four phases: **0** oracle repair,
**1** architectural anchors (A, D, I1, J, K, L), **2** surviving instances, **3** the
remaining families (B, C, E, F, G, H, M, N) and the isolated crashes. Its §6 currently
excludes #545 from scope — *"Design and study proposals … #545 … not defects."* The
table below is what a scheduler needs to decide whether that exclusion should stand.

| Slice | Compiler paths touched | Campaign family (Phase) | What it **removes** (the benefit is subtraction) | Regression rows owed |
|---|---|---|---|---|
| **SH-1** prelude loader | `src/driver/Driver.cpp` (or a new `src/stdlib/` + CMake), `src/module/ModuleLoader.cpp`, `src/sema/SemaContext` | **none** — it is additive infrastructure, no lowering path edited | **nothing yet.** Enables every later subtraction; on its own it is pure addition. | One row: a prelude-declared `class StdHello`, instantiated from user code, resolves and links. Under **option B** add a *second* module importing the first, asserting the type-identity answer D1 gives — pass or documented reject, never a silent pun. |
| **SH-2** `OwningBuffer<T>` as a generic class's owned field | **none edited** — tests only | **E** (Phase 3), read-only: it *measures* #2645 | **nothing.** Its output is evidence. | A Box-like `class Foo<T> where T: class` holding `OwningBuffer<T>`, exercising construct / indexer-get / `move buf[i]` / `buf[i] := null` / `buf[i] := v` / class-level Dispose, asserting an **element destructor count** under ASan. Plus a deliberate #2645 row: a class-element `OwningBuffer` reaching a header-disposer-consulting free. |
| **SH-3** self-host `OwningStack<T>` | `TypeResolution_Names.cpp:264`, `BuiltinRegistry.cpp:199`, the `colName` dispatch (`LlvmBackend_Exprs2.cpp:4981`ff), `tryLowerCollectionInstanceCall` (`LlvmBackend_Calls.cpp:1813`, `:5810`, `:6371`) | **E** and **G** (Phase 3) — *edits their shared sites* | 1 name from `builtinTypeArity`; 1 from `kArrayBackedCollections` (11 → 10); the `OwningStack` arms of the construction and method-call dispatches. Measured reach of the name today: **56 sites** in `src/`. | Every existing `OwningStack` row re-run on the self-hosted path with byte-identical results (`owned_collection_pop_dispose_746_test.cpp`, `owned_collection_accessors_test.cpp`, `owned_collection_trypop_831_test.cpp`); one new element-disposal row through the self-hosted destructor; one row per §4.11 member that is *not* deferred; one **negative** row proving `isCollectionTypeName` now answers false for the head. |
| **SH-4** borrowing `Stack`/`Queue`, `OwningQueue` | same as SH-3, plus `Ownership_Checks.cpp`'s five lists and `PureChecker.cpp:563` | **A** (Phase 1) — **blocking**, see below — and **E**, **G** | `Stack` **54**, `Queue` **49**, `OwningQueue` **52** sites; 3 more names from both tables | SH-3's row set ×3, plus a **function-typed element** row for each borrowing type (the family-A exposure) and a §15.4 fresh-store row proving the ownership pass still warns after the name leaves its lists. |
| **SH-5** incremental de-hardcoding | all of the above, plus `ExprTypecheck_Exprs.cpp:1848/2768`, `ExprTypecheck_Calls.cpp:20852`, `LlvmBackend_Disposal.cpp:1084`, `LlvmBackend_impl.hpp:10855` | **E**, **G**, **K**, **M** (all Phase 3) | the residue of the 634 sites for each migrated name | For **each** name removed from **each** predicate: the previously-passing rows for that name, re-run. Gate each removal on its own green run — this is the slice where a silent regression is cheapest to introduce. |
| **SH-6** remaining collections | everything | **E**, **G**, **K**, **M**, plus `bz_dict_*`/`bz_llist_*` | the remaining ~10 names | multi-run; out of scope for a campaign-era decision |

**The honest cost/benefit, POC during the campaign vs after it.**

*What the owner buys by scheduling SH-1..SH-3 **during** the campaign:* one name
(`OwningStack`, 56 sites) leaves the intrinsic surface, and — more valuable — the
architecture is either validated or falsified while the evidence is fresh. If SH-3
passes, SH-4..SH-6 become mechanical and the 634-site surface acquires a downward
trajectory for the first time. If SH-3 fails, it fails on a 40-line class with the
smallest owning surface in the stdlib, which is the cheapest possible place to learn it.

*What it costs:* (a) SH-3 and SH-5 edit the shared sites of campaign families **E**
and **G** while those families' lanes are live, which is exactly the interaction
#4438 measured — *"lane-36's release discipline fixes four capture-cell leaks
standalone, and cherry-picked onto a tree carrying lane-31's deferred-cell-free path
it fixes **none** of them, with byte-identical failures to baseline."* (b) SH-3's
acceptance criterion is a **differential** ("same results as the intrinsic path"),
and campaign Phase 0 exists because the oracles that would decide it are known to
pass for the wrong reasons — #4452's three measured holes: *"a compile timeout
satisfies **477** negative assertions, a missing toolchain silently disarms the ASan
tier at **1,551** call sites, and a bare `EXPECT_FALSE(r.ran)` cannot tell a
rejection from a SIGSEGV at **129** rows."* A POC graded by those oracles proves
nothing. (c) There is **no working build in this repository** (#4451: the compiler
needs LLVM 21, only clang 18 is present), so nothing above can be probed today.

*After the campaign:* (a) and (b) both go away — E and G are closed or their bridges
landed, and the oracles are ratcheted. The cost of waiting is that the surface keeps
growing at roughly the rate §1.3 measured (+65% in seven weeks), and every collection
added natively in the meantime repeats #547's mistake.

**Prerequisites, restated.** §7's *"the keystone prerequisite is SH-1 … that loader
does not exist today"* is **no longer the binding constraint** (§0.5: a source-text
prelude exists and is in production). The binding constraints are now:

| Gate | Status | Evidence |
|---|---|---|
| A prelude mechanism exists | **MET** (option B), for choices and enums | `Driver.cpp:62–161`; §0.5 |
| It carries a `class` | **unverified** — no class has ridden it; mechanism is decl-kind-agnostic | `injectOnePrelude`'s `dynamic_cast<ast::TypeDecl*>`, `Driver.cpp:134` |
| Cross-module type identity is decided | **OPEN — this is the new D1** | internal linkage, `LlvmBackend_Setup.cpp:2106`/`:2466`; §0.5 q2 |
| Oracles can grade a differential POC | **NOT MET** — campaign Phase 0 | #4452 |
| Family A (capture cells) for the borrowing half | **NOT MET** — #2718 is a Phase 1 anchor | §0.4 |
| `OwningBuffer<T>` field round-trip proven under ASan | **NOT MET** — that is SH-2, and #2645 is open on it | §2.3 |
| A working build | **NOT MET** in this tree | #4451 |
| Borrow-storing primitive (for a self-hosted `Array<T>`) | **DECLINED**, rationale §5.20 | §6 SH-4 |

### Slice SH-1: Prelude Bootstrap Loader (Prerequisite)

**What:** Add a mechanism to compile a stdlib `.bzl` prelude before user code
and register its `TypeSymbol*` into `SemaContext`. Hook into `Driver.cpp` at the
`compileFile` entry (*moved: was "line 165", now `:371`; the front end is
`compileExecutableObject`, and the existing prelude injection point is `:484`*).
Reuse `module::loadDependency` or implement a
dedicated `loadStdlibPrelude(path, semaCtx)` that compiles the prelude to a
`.bezel.meta` object and loads it. *Revised 2026-09-17: **option B** (extend
`injectStdlibPrelude`'s source-text list, §3.2) is now the cheaper starting point
and needs no CMake work at all; the metadata route becomes the answer only if D1
rules that a prelude collection must have one cross-module identity.*

**Scope:** Driver.cpp + a new `src/stdlib/` directory + CMakeLists.txt entry.
No sema or codegen changes yet.

**Test:** A minimal prelude with one class (`class StdHello`); a test that
instantiates `new StdHello()` in user code and verifies the sema does not
report "unresolved type name."

**Size:** Medium (3–5 days). Touches CMake build, driver pipeline, and the
existing module loader. Does not change any collection behavior. *Revised: under
option B, small (1–2 days) — no CMake, no loader.*

**Risk:** Low for the mechanism; medium for CMake (dependency on build system
order — the prelude must be compiled before any user program using it). *Revised:
the mechanism risk is now **low** either way (§0.5); the dominant risk has moved to
cross-module **type identity** under internal linkage, which option B does not
solve and option A does.*

**Owner decision required (D1):** Prelude file location and build-system model.
*Restated 2026-09-17 (§3.2): the location is largely settled by the shipping
mechanism; the live question is one identity or one per module.*

### Slice SH-2: Expose `OwningBuffer<T>` as Holdable Owned Field for Generic Classes

**What:** This is mostly already done. Verify and test that a user-written generic
class `class Foo<T> where T: class` can declare `private owned _buf: OwningBuffer<T>`
and that:
- Construction (`_buf := new OwningBuffer<T>(n)`) compiles.
- Indexer get (`_buf[i]!`) and owned-move (`move _buf[i]`) compile.
- Indexed set (`_buf[i] := value`) and null-restore (`_buf[i] := null`) compile.
- Class-level Dispose cascades through the OwningBuffer field correctly.

If any of these are untested or broken, fix them before SH-3.

**Tests:** Write an AOT round-trip test (`tests/codegen/owning_buffer_generic_field_test.cpp`)
with a Box-like class that holds `OwningBuffer<T>`, fills some slots, and verifies
the element destructor count at Dispose time.

**Size:** Small-medium (2–3 days). Primarily test writing; may expose 1–2 bugs.

**Risk:** Low. The conformance file (*moved: was `stdlib_0039.bzl`, now
**`stdlib_0051.bzl`***, the `OwningArray<T>` Bezel
source) already exercises all these operations — the risk is in translating from
conformance-check form to AOT round-trip proof.

> **Re-scoped 2026-09-17: SH-2 is the one slice that is campaign-compatible, and it
> should be scheduled regardless of the SH-1/SH-3 decision.** It edits no lowering
> path, removes nothing, and touches no family's shared site — it only *measures*. It
> also has a named open defect to aim at: **#2645** (`new OwningBuffer<T>(n)` returns
> before `tagOwningElems`, leaving a NULL header disposer), which the issue itself
> records as *"not reproduced by probe"*. SH-2's ASan element-destructor-count row is
> exactly the probe #2645 asks for, and it decides whether the storage primitive this
> whole proposal composes over is sound. If it is not, SH-3 is not a proof-of-concept;
> it is a defect hunt wearing one, and the owner should learn that from a 2-day test
> slice rather than from a 5-day migration.

### Slice SH-3: Self-Host `OwningStack<T>` (Proof of Concept)

**What:** Write `src/stdlib/owning_stack.bzl` containing the `OwningStack<T>`
source above. Wire it into the prelude loader from SH-1. Remove `"OwningStack"`
from `builtinTypeArity` (*moved: was "TypeResolution_Names.cpp line 171", now
**`:264`***) and from the
`isCollectionTypeName` name set (the #1414 stage-3 registry's
`kArrayBackedCollections` — *moved: was `src/common/BuiltinRegistry.cpp:69`, now
**`:199`**, an 11-name array going to 10; the predicate itself — *moved: was
`LlvmBackend_Calls.cpp:397`, now **`:821`*** — reads that registry, and the
registry-side wrapper `isArrayBackedCollectionName` is at `BuiltinRegistry.cpp:922`).
Add or
migrate the existing `OwningStack<T>` codegen tests to confirm the self-hosted
path produces the same results as the previous intrinsic path. *Note 2026-09-17:
`"OwningStack"` reaches **56** sites in `src/` (headers included); the two table
entries above are the ones that **gate**, and SH-5 sweeps the rest.*

**What does NOT change:** `OwningBuffer<T>` intrinsic handling stays. The
`Push`/`Pop`/`Peek` operations on `OwningStack` now reach vtable dispatch instead
of `bz_array_*` calls. The Drain() implementation is deferred.

**Tests:** Migrate the existing OwningStack codegen tests
(`tests/codegen/owned_collection_pop_dispose_746_test.cpp`,
`owned_collection_accessors_test.cpp`; `owned_collection_ops_test.cpp` now covers
only `OwningArray` Remove/Insert); add a new test verifying element disposal via
the self-hosted destructor.

**Size:** Medium (3–5 days). This is the highest-value proof: if it passes, the
architecture is validated.

**Risk:** Medium. The biggest unknown is whether the prelude-loaded TypeSymbol
and its generic instantiation interact correctly with the codegen monomorphization
cache (`generic_spec_cache_test.cpp` patterns). If generic instantiation for an
imported class has bugs, they will surface here. The `#428` decline-with-reasons
memory recorded concerns about cross-module generic construction — this slice
probes exactly that boundary, but with a simpler case (no multi-module separation
of definition site from instantiation site; the prelude is always co-loaded).

**Owner decision required (D3):** Accept that `Drain()` is deferred to a
subsequent slice (it requires `IOwningEnumerable<T>` machinery — stdlib §4.13 —
which is a larger codegen feature).

> **Delegated ruling 2026-08-11 (stdlib-wave readiness; owner review in repo) —
> D3: ACCEPTED.** `Drain()` defers to its own slice. The `IOwningEnumerable<T>`
> protocol machinery (stdlib §4.13) is a separable codegen feature with a fully
> specified stdlib target, so the deferral loses no design information; folding
> it into SH-3 would couple the architecture-validating slice to the largest
> unproven codegen surface and blur the pass/fail signal the slice exists to
> produce. Declined: requiring `Drain()` in SH-3 (couples the proof slice to
> unrelated risk); dropping `Drain()` from the plan (it is required stdlib
> surface — deferral, not removal). Consequence: SH-3's acceptance criteria
> exclude `Drain()`; the follow-up slice picks it up with the §4.13 pattern as
> its spec-shaped test target. (Housekeeping, recorded with the ruling: the
> 2026-07-28 sweep's standing flag 1 — the wholesale pre-v0.168 staleness of
> this document — was discharged by the mechanical refresh performed 2026-07-30,
> per the provenance note above; no refresh gate remains ahead of a 0005 lane.)
>
> **D3 re-validated 2026-09-17 against v0.258 — the ruling stands, one premise
> changed.** The ruling was written at stdlib v0.231, before proposal **0096**'s
> iterable-announcement unification graduated at **v0.258**. That revision makes a
> call whose declared return type is *mint-announcing* — including *"the eight §29.3/
> §30.7 iterable and enumerator interface heads"* — produce a caller-owned frame at
> every callee kind, and it **retires** *"the v0.223 owning-head declaration-site
> rejection and its `Drain()`/`ReceiveAll()` §29.11 affine carve"* (changelog v0.258).
> So `Drain()`'s deferral is no longer partly justified by a special affine carve
> that no longer exists; it is justified purely by the remaining
> `IOwningEnumerable<T>` **codegen** surface, which defect-campaign family **G**
> owns (21 open members) and which v0.258 did not implement. The deferral is
> therefore *more* clearly correct than when it was ruled: SH-3 must not be the
> first consumer of a protocol whose 21 open defects are somebody else's Phase-3
> lane. Anyone reading the 2026-08-11 ruling should read *"§29.11 affine carve"* out
> of their model of why.

### Slice SH-4: Self-Host Borrowing `Stack<T>` and `Queue<T>` / `OwningQueue<T>`

**What:** Write `src/stdlib/stack.bzl`, `src/stdlib/queue.bzl`,
`src/stdlib/owning_queue.bzl`. Borrowing `Stack<T>` and `Queue<T>` are simpler
(no owned-element disposal). Remove their entries from `builtinTypeArity` and
`isCollectionTypeName`.

**Size:** Small-medium per type (1–2 days each).

**Risk:** Low after SH-3 is proven. The borrowing variants use `Array<T>` or a
value `OwningBuffer<int>` as backing, which avoids the owned-element complexity.

> **Superseded in place, 2026-09-17 — SH-4 is the *hardest* remaining slice, not the
> easy one.** Three findings, none of which were available to the earlier drafts:
>
> 1. **The borrowing family has no user-expressible storage, by an explicit spec
>    decline.** Rationale §5.20's closing paragraph ("The deferred half: a
>    borrow-storing primitive", unchanged at v0.306): *"The table's borrowing column
>    has no user-expressible storage embodiment, and v0.132 deliberately does not add
>    one. The blocker is principled: outside the compiler-known borrowing
>    collections, storing a borrow in a field is rejected (§28.3) — that rule is what
>    makes borrow lifetimes statically trackable."* It records the alternative as
>    **Declined**: *"a quick dispensation ('classes marked somehow may store
>    borrows') would re-open the soundness of the borrow tracker for a convenience
>    the standard library does not itself need."* **Consequence:** a self-hosted
>    borrowing `Stack<T>` can only be a **wrapper over the still-intrinsic
>    `Array<T>`** — which is a real subtraction (`Stack` 54 sites, `Queue` 49,
>    `OwningQueue` 52) but is *not* the "compose over a primitive" story SH-3 has,
>    and it means **`Array<T>` itself can never be self-hosted** without a spec
>    amendment. That is recorded here as **D5** (§0.6).
> 2. **Closures become storable.** §28.11's element-category table admits
>    *runtime-managed references (string, **function types**, Type)* in the borrowing
>    column as *shared references*. A self-hosted borrowing `Stack<Func>` therefore
>    puts closure storage in an ordinary user-class field, and **v0.298** makes a
>    closure *a borrow-carrying value admitted wherever such a value is admitted*
>    (§28.3's closed three-form capture list replaced by the source-set rule,
>    proposal 0126, #4154). That lands SH-4 squarely on **defect-campaign family A**
>    (#2718, 33 open members, 219 lines of decision machinery). **Family A is a
>    prerequisite for SH-4.** It is *not* one for SH-3, because the owning column
>    rejects runtime-managed element types at instantiation.
> 3. **The ownership pass's five name lists** (§0.1) must all be updated coherently,
>    and they currently disagree with each other on five different memberships —
>    including one list that still names `"List"`, removed from the language at
>    v0.53. Removing `Stack`/`Queue` from some but not all of them is precisely the
>    silent-drift failure `next-run-queue` §B calls the run's dominant defect
>    generator.
>
> **Revised size/risk:** large, and **gated on family A**. It should be re-planned,
> not just re-estimated, after SH-3 reports.

### Slice SH-5: Incremental Codegen De-Hardcoding

**What:** For each type migrated off the intrinsic path, audit the name-string
sites in `src/sema/` and `src/codegen/` (*was "~385"; **634** at `29410e29` — §1.3*)
and remove or conditionalize the
entries that are now dead. Focus on the high-impact clusters:
- `isCollectionTypeName` (*moved: was `LlvmBackend_Calls.cpp:397–406`, now
  **`:821–843`***) — its name set is the #1414 stage-3 registry's
  `kArrayBackedCollections` (*moved: was `src/common/BuiltinRegistry.cpp:69`, now
  **`:199`***).
- sema member-access gates (*moved: was `ExprTypecheck_Exprs.cpp:914–919,
  1996–2001`; the equivalents are **`:1848–1854`** and **`:2763–2776`**, plus
  **`ExprTypecheck_Calls.cpp:20852–20857`**, the `isRegisteredCollection` gate #3896
  is about*).
- owned-field disposal classification (*moved: `emitOwnedFieldDisposals` was
  `LlvmBackend_Disposal.cpp:415`, now **`:958`**/`:1084`; `ownedCollectionFieldType`
  was `LlvmBackend_impl.hpp:5498`, now **`:10855`***; moved out of
  `LlvmBackend_Setup.cpp`).
- *added 2026-09-17*: the five `src/ownership/Ownership_Checks.cpp` lists
  (`:7968`, `:8338`, `:8510`, `:12620`, `:18743`) and
  `src/sema/PureChecker.cpp:562–567`. Two of these carry the dead name `"List"`
  (and PureChecker also `"OwningList"`), removed from the language at v0.53 — a
  free, evidence-backed subtraction available **before** any migration.

For each type removed from the predicate, add a regression test that previously-
passing codegen tests still pass.

**Size:** Large (1 week). Primarily surgical removal with test coverage.

**Risk:** Medium. Removing a name from a predicate when the type still has
partial intrinsic handling will cause silent regressions. Gate each removal on a
passing round-trip test.

> **Scope fence added 2026-09-17 (duplication census §5).** SH-5 removes **names
> from sets**. It does **not** merge two sets. The census's "do NOT touch" list
> names this family explicitly: `isArrayBackedCollectionName` versus
> `isArrayEnumeratorWrapSourceName` are *"two different questions over overlapping
> name sets — collapsing them would either wrap a hash-table receiver as if it were
> a dense array (**crash**) or silently unorder a `SortedSet` iteration (**spec
> violation**)."* A lane that reads SH-5 as a unification mandate will produce
> exactly that. The same fence covers the `GetAsyncEnumerator` self-alias guard
> (#2356/#2485) and `LlvmBackend_Calls2.cpp`'s decl-cast soundness gate (#2821):
> untouched by every slice here.

### Slice SH-6: Remaining Collections (Multi-Run)

This is a multi-run endeavor. Rough ordering by complexity:

| Collection | Complexity | Blocker |
|---|---|---|
| `OwningArray<T>` | Medium | SH-3 proven; reference impl *moved* — now `tests/conformance/cases/stdlib_0051.bzl`, and quoted in normative stdlib §4.5 |
| `LinkedList<T>` / `OwningLinkedList<T>` | High | Needs node-type definition; `bz_llist_*` is the runtime; self-hosting would need a Bezel-source node class or keep bz_llist_* as the intrinsic floor. **Borrowing half blocked by rationale §5.20** (§6 SH-4) |
| `Set<T>` / `OwningSet<T>` | High | Hash/eq dispatch is runtime-polymorphic; needs design for how a Bezel-source set class expresses the hash function. *Open defects on the intrinsic today: #2296, #3491* |
| `Dictionary<K,V>` / `OwningDictionary<K,V>` | Very High | Two type params; generic-key hash table; drain-and-rebuild bucket mutation pattern. *Open defect on the intrinsic today: #3576* |
| **`SortedSet<T>` / `SortedDictionary<K,V>`** *(added 2026-09-17)* | High | Not in the earlier table because they did not exist. #3489 names what implementing the reference-element case needs: *"an object-`CompareTo` comparator … There is no `CompareTo`-through-the-vtable comparator anywhere in the compiler"*, plus *"a backing whose IDENTITY is the comparator, not a hash"*. #3489 itself points at this work: *"the same machinery stdlib §4.17's 'no `OwningSortedDictionary` in this revision — owning sorted collections ride the self-hosted-collections work' note defers."* |
| **`FrozenArray<T>`** *(added 2026-09-17)* | Low–Medium | §4.19; shares `bz_array_t*` with `Array<T>` (#3249). Construction is only the two `From` overloads, so its intrinsic surface is small |
| **`Counter<K>`** *(added 2026-09-17)* | n/a | §4.20; changelog v0.290 R2 records the absence check: *"the stdlib `Counter<K>` has no compiler implementation at all (`"Counter"` → 0 in `src/`)"*. It is the one stdlib collection with **no** intrinsic to retire — a natural first *greenfield* self-hosted collection if one is ever wanted, tracked at #3848 |

**Owner decision required (D4):** Whether `LinkedList<T>` and `Dictionary<K,V>`
should remain C++ intrinsics long-term (they have the most complex runtime
implementations) or be targeted for Bezel-source rewrite in a later run.
*Extended 2026-09-17: D4 should now also cover the sorted pair, because the stdlib's
own §4.17 text defers `OwningSortedDictionary` **to this proposal**, and #547 already
recorded — and was overruled on — the argument that the sorted collections should not
have shipped natively in the first place.*

---

## 7. Verdict

> **Superseded in place, 2026-09-17.** §7 was written when the prelude loader was
> the binding constraint. It no longer is (§0.5). **§0.6 is the current verdict**;
> §6.0's gate table is the current prerequisite list; the text below is kept because
> its *ordering* argument (SH-1 → SH-2 → SH-3, and no wholesale predicate removal in
> one run) is unchanged and correct.

**Is it startable?** Yes, but not yet. The keystone prerequisite is the
**prelude-bootstrap loader (Slice SH-1)**: without the ability to compile a
`.bzl` file and inject its `TypeSymbol*` into `SemaContext` before user code
runs, none of the self-hosting replacements can be wired in. That loader does not
exist today. *(Superseded: a source-text prelude exists and is in production —
`Driver.cpp:62–161`. The binding constraints are now oracle repair, family A for
the borrowing half, cross-module type identity, and #2645 on the storage primitive
— §6.0.)*

**The keystone prerequisite is SH-1**, followed immediately by SH-2 (verifying
OwningBuffer<T> as a generic class's owned field works end-to-end in AOT tests).
Once both pass, SH-3 (`OwningStack<T>` as the proof of concept) is the high-
value architectural validation: if a self-hosted generic class with an
`OwningBuffer<T>` field compiles through the prelude path, produces correct AOT
results, and passes element-disposal tests, the architecture is proven and the
remaining collections are mechanical repetition.

**This is a multi-run architectural endeavor.** A single dedicated run should
target SH-1 + SH-2 + SH-3 (prelude bootstrap + OwningBuffer field proof +
`OwningStack<T>` POC). That is four owner-decision gates (D1–D4) and approximately
two weeks of focused work. The remaining collections (SH-4 through SH-6) are
follow-on runs once the POC run validates the architecture. The *(was ~385, now
**634**)* hardcoded
name-string sites cannot all be removed in one run without risk of silent
regressions; the correct discipline is to remove each type's entries only after
that type's self-hosted round-trip tests pass.

**First recommended dedicated-run slice:** SH-1 (prelude bootstrap loader) as the
one essential infrastructure piece, immediately followed by SH-2 (OwningBuffer
field AOT proof) and SH-3 (self-hosted `OwningStack<T>`). Commit to these three
and defer SH-4 through SH-6 to subsequent runs. If SH-3 uncovers a fundamental
blocker in cross-module generic instantiation for prelude-loaded types, the run
ends cleanly with a decline-with-reasons issue filed, without having disturbed the
existing intrinsic machinery for the other 11 collection types.
*(Superseded 2026-09-17: the recommendation is now **schedule SH-2 alone, hold
SH-1/SH-3 to the campaign's Phase 2 boundary** — §0.6. The "11 other collection
types" is also stale: the array-backed, dict-backed and llist-backed registries
now name **16** collection types between them — `BuiltinRegistry.cpp:199`/`:214`/
`:219` — plus `Buffer<T>` and the unimplemented `Counter<K>`.)*

---

## 8. What this refresh could not verify

Recorded so the next reader does not mistake an unverified claim for a measured one.

- **Nothing here was run.** There is no working build in this repository (**#4451**:
  the compiler needs LLVM 21, only clang 18 is present). Every implementation claim
  above is a source read at `main` @ `29410e29`, cited by file and line. No probe, no
  test execution, no `bezelc` invocation.
- **Whether a `class` rides the source-text prelude.** The mechanism is decl-kind
  agnostic by inspection (`Driver.cpp:134`'s `dynamic_cast<ast::TypeDecl*>`), but only
  a choice and three enums have ever used it. Unverifiable without a build.
- **Cross-module type identity for a prelude type.** Internal linkage is verified in
  source (`LlvmBackend_Setup.cpp:2106`, `:2466`; `ModuleMetadataBuilder.cpp:2562`);
  what happens when a prelude *class* crosses an `--import` boundary is not
  determinable by reading.
- **Whether #2645 is a live leak or a consistency gap.** The issue says so itself
  (*"not reproduced by probe"*, and the free site *"is gated on
  `isBorrowingListInterfaceName`"* so reachability is unclear). SH-2 settles it.
- **The POC body compiles.** The §5.2 sketch was brought to the v0.306 §4.11
  signatures by reading the stdlib; it has not been fed to a compiler, and
  `Contains`'s `Equals` call is an asserted semantics, not a derived one (§5.2's
  "still owed" note).
- **Line ranges given as spans** (e.g. "the `colName` dispatch runs `:4981` to roughly
  `:6400`") are approximate by construction; the individual anchors inside them are
  exact `grep` hits.
- **Issue #4442** — *read, and it is not a gap: **closed completed 2026-09-16**, the day
  before this refresh.* It is **not** a name-gate defect and does not belong in §0.3's
  table, but it is worth recording here as evidence for §2.3: it was a **spec defect**
  (§28.11 said "reverse-insertion order", an order *"no conforming implementation can
  compute"* after `Sort`), and **the tiebreaker was the stdlib §4.5 reference
  implementation's own `Clear()` body** — reverse *index* order. The spec now reads
  *"**reverse storage order** for the ordered collections — descending index for
  `OwningArray`, tail-to-head for `OwningLinkedList`, pop order for `OwningStack`, the
  reverse of dequeue order for `OwningQueue`"* (`Bezel_Language_Spec.md:7797`; zero
  occurrences of "reverse-insertion" remain in either document). That the Bezel-source
  reference implementation was authoritative enough to settle a normative question is a
  small but direct argument for this proposal's direction. **Consequence for SH-3:** the
  POC's `Clear()` at §5.2 walks `0..<_count` forward and must be **reversed** to
  descending index before it conforms to v0.306 §28.11; flagged, not silently fixed,
  because the sketch's other owed items (§5.2's note) are scheduled together.
- **Members of campaign families E/G/K/M** were attributed from the campaign document's
  own per-family member lists, not re-derived from each issue body. The four issues
  quoted at length in §0.3 (#2296, #3576, #2645, #2648) were read in full.
