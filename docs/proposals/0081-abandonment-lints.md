# Proposal 0081 — The two statically-certain-hazard lints: fresh non-trivial stores, and terminal accessors over minting pipelines

| | |
|---|---|
| **Status** | under-review — drafted 2026-08-11 (owner-directed, from the elegance review) |
| **Tracking issue** | [#3047](https://github.com/s8lap/bezel-lang/issues/3047) |
| **Target module(s)** | **spec** — §15.4 (the store-lint paragraph: type set extended from trivial-dispose to all fresh unbound owned temporaries; the recorded #1813 follow-on taken), §28.11 (*Borrowing collections* store paragraph: one cross-reference sentence), §29.11/§28.3 (one sentence anchoring lint B's provenance predicate), stdlib §5 (one sentence at the accessor contract naming the lint), §30.10 area (cross-reference from the abandonment idioms). **rationale** — §5.28's Q1 record gains the taken-follow-on note; a short new decision paragraph for lint B. **guides** — Guide 04 §2 and Guide 05 §6 rewording at graduation (the traps become "the diagnostic you will see"). **No grammar change; no program that compiles today stops compiling.** |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | Elegance review 2026-08-11, findings 4 and 8. Finding 4: `tasks.Add(FreshAsync())` compiles and silently abandons the task — Guide 04 §2's "the trap", the #1813 seam — while `_ := someAsyncCall();` is a hard error for the same act. Finding 8: `probes.First()` over a minting pipeline compiles and hands back a borrow of an already-disposed element — Guide 05 §6's trap #4. Rationale §5.28's Q1 record already built the identical lint for the trivial-dispose store case and says in terms: *"Recorded follow-on, not taken: the same lint shape would serve the non-trivial fresh-`Task`-slot trap (#1813)."* This proposal takes the follow-on and extends the shape to the accessor trap. |

*Reference convention: unprefixed §-references are language-spec references; stdlib and rationale references are marked.*

---

## 0. Thesis

The v0.238 Q1 ruling (proposal 0077; rationale §5.28; spec §15.4) established a doctrine: **a statically-certain hazard emitted silently is the one place the language's silence stops being honest** — and the answer is a *targeted lint*: a normative warning, the program stays accepted, the remedies are named in the diagnostic, no new syntax. That ruling shipped the lint for exactly one type set (fresh **trivial-dispose** arguments in borrowing-store position) and recorded, without taking, its obvious extension.

This proposal ships the two remaining instances of the same doctrine, one proposal because they share the machinery (a classification the compiler already computes, a warning keyed on it, a remedies list):

- **Lint A** — a **fresh non-trivial-dispose** producer (the `Task` family foremost) in `Add`/`Insert`/indexer-set position on a **borrowing-family** collection: the #1813 trap, statically certain abandonment at the argument's K8 boundary.
- **Lint B** — a **class-element terminal accessor** (`First()` and family, stdlib §5) whose receiver's provenance is a **fresh minting pipeline**: Guide 05 trap #4, a statically-recognizable dead-on-arrival borrow.

Neither lint changes what compiles. Both convert a guide-taught trap into a diagnostic.

## 1. Lint A — fresh non-trivial producers in borrowing store position

### 1.1 The current state, precisely

§15.4 (v0.241) already contains the machinery this lint extends. Its store paragraph:

- **admits** `coll.Add(new Connection("a"))` — the literal-only-rejection asymmetry, recorded with its reason: *"an opaque callee's signature does not announce retention"* — leaving the shape "the borrow-lifetime hazard that checked disposal mode backstops";
- **mandates a warning** for the *trivial-dispose* instance of the same shape (`groups.Add("odds", [1, 3, 5]);`): *"Because the ceremony exemption suppresses every other diagnostic on such a value, the compiler **must warn** at this shape"* — naming the K8 boundary and two remedies (bind the source first; use the owning family);
- and **records the follow-on**: *"the same lint shape would serve fresh non-trivial `Task`-typed arguments in the same store positions — the fresh-slot abandonment trap of #1813."*

The asymmetry the elegance review flagged is real and inverted from what the doctrine demands: for a *trivial* value (whose loss is a dangling `Box`) the shape warns; for a **task** (whose loss is an abandoned operation — the thing axiom A6 calls "a decision, not a disposal", and whose discard spelling `_ := call();` is a **hard error**, §17.13) the identical store compiles with zero diagnostics.

### 1.2 The rule

Amendment to §15.4's store-lint sentence (drop-in sketch; the surrounding admission text is unchanged):

> Because the retention is statically certain to outlive the value, the compiler **must warn** when a store-position argument on a **borrowing-family** collection — `Add`/`Insert` and the indexer-set, key and value positions alike — is a **fresh unbound owned temporary of class type** ([§28.3](…)): a `new` expression, a collection literal, an `owned`-returning call result, a `move` result, or a **`Task`-family call result** (calls mint, [§28.3](…)). The warning names the K8 boundary at which the argument dies and the remedies. For a **trivial-dispose** argument the boundary event is the compiler's free and the hazard is the retained dangling reference (checked-mode `DisposedAccessException` on later use, [§28.2](…)); for a **`Task`-family** argument the boundary event is the task's disposal — the operation is *abandoned*: it runs to its terminal state unsupervised and its result, if owned, is disposed on arrival ([§30.5](…)) — and the slot dangles besides; for any other non-trivial-dispose class the boundary event is the value's disposal and the hazard is the dangling slot. The warning does not narrow the admission: the program remains accepted, no `using var` is demanded, and the recorded literal-only-rejection asymmetry stands unchanged.

Proposed diagnostic texts (§28.3's recommended-message style):

- *Task family:* **"The task produced by `FetchAsync(path)` is freshly minted here and has no holder: a borrowing `Array<Task<string>>` slot takes no ownership, so the task is disposed — the operation abandoned — at the end of this statement, and the stored reference dangles. Bind each task to a named holder first, or use the owning family (`OwningArray<OwningTask<string>>` with `Task.WhenAllOwned`) for dynamic fan-in (#1813)."**
- *Other non-trivial:* **"The fresh `Connection` stored here dies at the end of this statement — the collection retains a dangling borrow (checked mode: `DisposedAccessException` on use). Bind the source first (`using var c := new Connection("a"); conns.Add(c);`) or use `OwningArray<Connection>`, whose `Add` takes ownership."**
- *Trivial-dispose:* the existing §15.4 text, unchanged.

### 1.3 False-positive analysis — the shapes that must NOT warn

This is where the lint earns its keep; each row was checked against the v0.241 rules it cites.

| Shape | Why silent |
|---|---|
| `tasks.Add(t1)` where `t1` is a named task binding | argument is a borrow of a named location, not a fresh temporary — the sanctioned #1813 spelling ("build the borrowed array over named bindings", stdlib §6.7); the ordinary store-rule residual attaches to `t1` and is the recorded trust regime, not a statically-certain dangle |
| `owning.Add(LoadConfigAsync(path))` on `OwningArray<OwningTask<Config>>` | owning-family `Add` takes `owned` — a transfer, no hazard; this is the corpus's only real `.Add(…Async(…))` shape (§1.4) |
| `conns.Add(c)` where `c` is a parameter, field read, or iteration variable | borrow of a named location — the store rule's ordinary residual |
| `coll.Add(sb.ToString())`, `xs.Add(42)` | `string`/value-typed arguments carry no classification (K1.6) |
| `sink.Consume(new X())` — an opaque callee | not a compiler-known borrowing-collection store position; §5.28's Q1 record already establishes that rejection/warning *cannot* be scoped by signature for opaque callees — the lint is keyed to the compiler-known borrowing-family members, exactly as the shipped trivial lint is |
| `groups["odds"] := odds;` where `odds` is named | indexer-set of a borrow — the normal store |
| fresh argument to an **owned-ingress** position reached through a literal (`OwningArray<Connection> := [Connect("a")]`) | owning ingress consumes; §15.4's literal-tree rule already assigns ownership |
| `Process(await FetchAsync(url))` and every other expression-confined fresh temporary | not a store position; the K8 machinery disposes after use, no retention exists |

Residual imprecision, stated: a fresh task stored into a **purely-local** borrowing collection that the program finishes reading before statement end is not expressible (the K8 boundary is the statement — the retention *always* outlives the temporary), so within the keyed positions the hazard is genuinely certain, not heuristic. The one shape a user might defend — "I deliberately want an abandoned task and also a record of it" — is served by the named-holder spelling plus `using var t := call();` (the sanctioned abandonment idiom, §30.10), which the diagnostic's remedy list teaches.

### 1.4 Corpus evidence

Grep (2026-08-11, v0.241 HEAD): `\.(Add|Insert)\([A-Za-z_]*Async\(` over `tests/` and `docs/`. Every hit is the **owning-family** shape (`OwningArray<OwningTask<Config>>` + `Task.WhenAllOwned` — `tests/conformance/cases/stdlib_0088.bzl`, stdlib §6.7's `LoadAllConfigs`, 0023's examples) — **zero borrowing-family fresh-task stores exist in accepted corpus code**. The only occurrences of the trap shape are Guide 04 §2's deliberate illustration and stdlib §6.7's #1813 seam note. Lint A therefore fires on **zero** existing corpus sites; its entire effect is on future code — the cheapest possible migration for a diagnostic.

## 2. Lint B — class-element terminal accessors over minting pipelines

### 2.1 The current state, precisely

Stdlib §5 (scoped at v0.235, #1666): the element-access and no-selector aggregation terminals — `First`/`Last`/`Single`/`ElementAt` with the `*OrNull` and predicate forms, `Min()`/`Max()` and the selector forms — at a **class or interface element type** return **borrows of the element, sourced from the receiver**. Over a collection-backed sequence the element's backing outlives the call and the borrow is sound. Over a **fresh-yield (enumerator-owned) source** — an iterator minting its elements, or an operator chain over one — *"the element does not survive the terminal's internal enumerator, so a later use of the result faults deterministically in checked mode"* (stdlib §5's own words; the K4.1 backstop, §28.2). Guide 05 §6 must teach this as the trap, with the materialize-first convention as the remedy.

The mechanism of the fault, traced against §29.2: a borrowing iterator's fresh yield is **enumerator-owned** — *"the next `MoveNext()` disposes the previous value"*, and *"disposing the enumerator … disposes any outstanding yielded value"* (§29.2 *Iterator yield ownership*, first bullet). A terminal operator constructs its own enumerator internally, takes the first element, and disposes that enumerator before returning — so the returned "borrow of the element" refers to an instance the terminal itself already caused to be disposed. **The hazard is not merely statically suspicious; on the fresh-yield path it is dead on arrival** — even `Probes(8).First().Run()` inline faults, because the element died inside `First()`.

### 2.2 The provenance predicate, precisely

The lint must fire only where the checker *knows* the receiver is fresh-yield-backed, and the knowledge already exists: §29.11's affine-query machinery tracks lazy-query values **by provenance, not by type** (its own phrase), tracing a bound query through the deferred-operator compositions to its root, and §29.2's yield-ownership classification is already computed per yield site. The predicate:

> A call to a stdlib §5 class-element terminal accessor or no-selector aggregation **warns** iff the receiver's statically-tracked provenance — the same flow tracing §29.11 performs for single-consumption, followed through the deferred lazy operators (callback-free and callback-taking alike) — roots at a **directly-called iterator method, compiled in the same compilation unit, at least one of whose reachable `yield return` sites classifies as an enumerator-owned fresh yield** (§29.2's first owned-class-typed bullet: a fresh producer or `move` result yielded from a borrowing iterator). Every other root stays **silent**: a concrete collection type; a materializer result (`ToArray()`/`ToSet()`/`ToDictionary(…)` — owned collections, collection-backed by construction); an interface-typed parameter, field, or call result (statically untracked — §29.11's own stated boundary); a cross-module iterator (the artifact interface does not carry yield classification); and any receiver whose element type is value, `string`, or runtime-managed (no borrow is produced).

When *all* reachable yields of the traced iterator are fresh, the diagnostic states the fault as certain; when yields are mixed (some re-borrows of parameter-sourced collections, some fresh), it states "may not survive." Proposed text:

> **"`First()` here returns a borrow of an element that is minted by `Probes` and owned by the terminal's internal enumerator — it does not survive this call (checked mode: `DisposedAccessException` on use; undefined unchecked). Give the elements a backing first: materialize with `.ToArray()`, build them into an owning collection, or consume the producer through the owning path (`IOwningEnumerable<T>` + owning iteration)."**

The remedies are stdlib §5's own, verbatim in spirit.

### 2.3 False-positive analysis — the shapes that must NOT warn

| Shape | Why silent |
|---|---|
| `fleet.FirstOrNull(lambda(c) => not c.IsBusy())` on `OwningArray<Connection>` | receiver is a concrete collection — collection-backed, the sound case Guide 05 §6 opens with |
| `xs.Where(p).First()` where `xs` is an `Array<T>` binding | chain roots at a collection; every corpus `.Where(…).First()` site is this shape (`tests/codegen/linq_test.cpp` etc.) |
| `probes.ToArray()` … then `arr.First()` — **the materialized receiver** | root is the owned `Array<T>` the materializer produced; **must not fire**, and does not: `ToArray()` is a terminal, not a deferred operator, so the trace stops at its owned result |
| `Process(xs: IEnumerable<Probe>) … xs.First()` | interface-typed parameter — §29.11's statically-untracked case; the runtime backstop remains the guard, exactly as for affine violations there |
| `Probes(8).Count()`, `.Any()`, `.ToArray()` | terminals that return values/fresh collections, not element borrows — outside the accessor set |
| value/`string` elements (`text.Split(",").First()` shapes) | no borrow; stdlib §5's classification only bites at class elements |
| `for var p in Probes(8) do p.Run(); end;` | iteration borrows are per-advance and in-loop — the sound consumption §29.2 defines; no terminal involved |
| `Aggregate(seed, fold)` at class accumulator | result borrows {receiver, seed}; the seed leg is caller-supplied and sound; only the receiver leg participates, under the same predicate |

The under-approximation is deliberate and recorded: a cross-module minting iterator escapes the lint (the checked-mode backstop still catches it at runtime). Widening via artifact-interface carriage of yield classification is a recorded liftable (Q3), not v1.

### 2.4 Corpus evidence

Terminal-accessor usage in `tests/`: 74 `.First(`, 17 `.FirstOrNull(`, 12 `.Single(`, 11 `.ElementAt(`, plus the `Last`/`OrNull` forms (grep, v0.241 HEAD). Every chained `.Where(…).First()` site found roots at a concrete collection (`linq_test.cpp:665`, `array_unresolved_member_2457_test.cpp:328`, `stack_queue_unresolved_member_2457_test.cpp:493` — all `Array<T>`/`Stack<T>` receivers, value elements besides). The only fresh-pipeline accessor in the corpus is Guide 05 §6's own trap illustration. **Lint B fires on zero existing corpus sites.**

## 3. Shared design: the doctrine, the suppression story, the relationship to v0.238 Q1

**One doctrine, three instances.** All three lints (the shipped trivial-store lint, A, and B) share the exact §5.28 Q1 design shape: (i) the hazard is *statically certain or statically recognizable* from classifications the compiler already computes (K8 temporary status; calls-mint; §29.11 provenance; §29.2 yield classification); (ii) the shape stays **admitted** — rejection was considered and declined for recorded reasons that are type-agnostic (§15.4's opaque-callee argument; §29.11's untracked-parameter boundary); (iii) the diagnostic **names the boundary event and the remedies**; (iv) no new syntax. The spec amendment for A is an extension of §15.4's existing lint sentence (same paragraph, extended type set — the v0.238 Q1 doctrine over §15.4/§28.11's store machinery); B's normative home is one sentence at stdlib §5's accessor contract plus the provenance predicate anchored in §29.11's tracking (the same §15.4-style must-warn phrasing).

**Suppression.** Recommendation: **no suppression spelling.** Binding-first is the suppression for A (naming the value is precisely the act that makes the program's intent checkable — the same act the diagnostic requests), and materializing (or the owning path) for B. Both remedies are one line, both already sanctioned, and both *change the program from hazardous to sound* rather than merely silencing — a suppression spelling that left the hazard in place would be a new way to write the silent dangle, which is what the lints exist to remove. The must-use warning's precedent applies for tooling: these are lints over sound-per-the-admission programs, so a project may tune or silence them project-wide (§28.3's stated posture); no per-site spelling is added.

**Program acceptance is invariant.** Neither lint rejects anything; neither narrows §15.4's admission or stdlib §5's contract; the checked-mode backstop remains the runtime guarantee in every case.

## 4. Alternatives considered

1. **Status quo.** Guide-taught traps with zero diagnostics, while the discard spelling of the same abandonment (`_ := call();`) is a hard error and the trivial-dispose spelling of the same store warns. Declined: the recorded Q1 argument — "a statically-certain dangle emitted silently is the one place the silence stops being honest" — applies *a fortiori* to tasks (finding 4's point), and §5.28 already names A as the follow-on.
2. **Rejection instead of a lint (either shape).** Declined per the recorded grounds: for A, rejection cannot be scoped by signature (`coll.Add(new X())` vs `sink.Consume(new X())` are indistinguishable plain-parameter calls — §5.28), and §15.4's literal-only-rejection reason is type-agnostic; for B, rejection would have to fire on the untracked-parameter case too or be trivially launderable through one helper, and stdlib §5's admission is a recorded v0.235 scoping this proposal does not reopen.
3. **Task-family-only lint A** (the literal #1813 scope). Declined: the trivial lint already covers trivial-dispose, so a task-only extension would leave precisely the middle band (`conns.Add(new Connection("a"))` — non-trivial, non-task) as the *only* fresh store shape with no diagnostic — an inverted asymmetry stranger than today's. The full non-trivial type set costs nothing extra (same classification) and the diagnostic already splits its wording by family.
4. **A type-system fix for B** (make fresh-pipeline terminals return owned values, or reject at the accessor). Declined: it changes stdlib §5's v0.235-scoped contract and the accessors' compiler-known classification for all receivers to fix a subset the lint can name precisely; the admitted-then-backstopped design is recorded, and this proposal's lane is diagnostics.
5. **Runtime-only (keep the checked-mode fault as the whole story).** Declined: checked mode is a per-build choice and the fault fires at the *use*, far from the defect; the entire point of the doctrine is to move the signal to the line that creates the hazard, at compile time.

## 5. Migration & compatibility (changelog house categories)

- **Newly warned:** (A) fresh non-trivial-dispose class-typed arguments — the `Task` family included — in `Add`/`Insert`/indexer-set position on borrowing-family collections; (B) class-element terminal accessors whose receiver provenance roots at a same-unit fresh-yield iterator. Zero existing corpus sites fire (§1.4, §2.4).
- **Newly rejected:** nothing.
- **Doc-only:** §15.4 lint-sentence extension; the §28.11/§29.11/stdlib §5 anchor sentences; rationale §5.28's follow-on note flipped from "not taken" to "taken at #3047"; Guide 04 §2 / Guide 05 §6 rewording at graduation.

## 6. Implementation sketch

- **Lint A:** `src/ownership` — the store-position classification and the K8-temporary/fresh-producer classification both exist (the shipped trivial lint keys on them; `producerState` and the §15.4 warning path); the extension is widening the warned type set and adding the per-family diagnostic texts. Size: **S**.
- **Lint B:** `src/sema`/`src/ownership` — rides the §29.11 affine-provenance trace (exists: single-consumption static errors) plus the per-yield-site ownership classification (exists: §29.2 enforcement); new code is the terminal-accessor keying (the compiler-known §5 member set, which the v0.235 scoping already special-cases) and the root test. Size: **M** (the trace-join is the work).
- **Tests** (spec-cited, fail-without/pass-with): warning fires — borrowing `Add` of a fresh task / fresh `Connection` / indexer-set key position (§15.4); silent — every row of §1.3's table that is expressible as a test (named holder, owning family, opaque callee, value elements); warning fires — `Probes(…).First()` same-unit minting iterator, mixed-yield "may" wording, chained through `Where` (stdlib §5/§29.11); silent — every row of §2.3's table (materialized receiver foremost — the must-not-fire case the tracking issue names).

## 7. Open questions

- **Q1 — lint A's type set:** all fresh non-trivial-dispose producers (recommended, §4 item 3) or `Task`-family only (#1813's literal scope)?
- **Q2 — suppression:** confirm no per-site suppression spelling (recommended, §3), leaving project-level lint tuning as the only knob?
- **Q3 — lint B's reach:** same-compilation-unit iterators only in v1 (recommended — zero false positives, recorded liftable) or carry yield classification in the artifact interface now (§2.8.2 surface change, its own delta-classification questions)?
- **Q4 — normative homes:** lint A as an extension of §15.4's existing lint sentence with §28.11 cross-referencing it (recommended — one lint, one paragraph), or a freestanding §28.11 clause? Lint B's must-warn sentence at stdlib §5 with the predicate in §29.11 (recommended), or spec-side only?

## 8. Interactions

- **0077/v0.238 (shipped Q1 lint):** this proposal is that ruling's recorded follow-on taken; the doctrine sentence is shared and the §15.4 paragraph is extended, not duplicated.
- **0078 (front-half sibling, checked-posture family):** shared doctrine — loud-where-certain; no textual overlap.
- **0082 (Pattern A scoped bind):** no contact — 0082's shape is a binding, never a store; a fresh task bound by 0082's sugar is held, not abandoned.
- **0090 (transfer-marker study):** no contact; lint diagnostics quote marker spellings in remedies (`owned`/`move` per whichever marker regime holds) — a wording dependency only, noted for graduation ordering.
- **#1813 / stdlib §6.7:** the seam note's two sanctioned spellings become the lint's remedy list verbatim; the ruled "no borrowing-family dynamic-build facility is planned" is untouched.
- **Proposal 0060 (channel give-back):** §6.7's reserved-seam note contemplates a future conditional-consume form; if one ever lands, lint A's fresh-argument predicate is unaffected (a give-back parameter would be an announced consume, not a plain borrow).
