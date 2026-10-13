# 0051 - Task-ownership defaults: flip the producer guidance now; a recorded study of bounded lifetime extension

**Status:** graduated-v0.204 — Part A superseded — graduated v0.204 (rationale §12.2 flip + §12.1 adapter sentence, docs-only; Q2 declined as redundant). Part B remains a deferred study, gated on the §3.5 measurements taken after the flip has had effect; not adopted.
**Tracking issue:** [#2038](https://github.com/s8lap/bezel-lang/issues/2038)
**Target:** Part A (the flip — docs-only): rationale [§12.2](../Bezel_Design_Rationale.md#sec-choosing-between-taskt-and-taskowned-t) (decision tree + the "when in doubt" paragraph rewritten), rationale [§12.1](../Bezel_Design_Rationale.md#sec-pattern-a-vs-pattern-b-is-type-directed) (one qualifying sentence), optionally spec [§30.5](../Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal) (one sentence appended to the existing producer-guidance paragraph — owner question Q2). **No stdlib change** (§1.2: the inventory found zero signatures the flip would re-choose). **No guide change** (§2.5: neither guide teaches the producer decision). Part B (the study): **no target** — its deliverable is decision criteria; if the criteria ever fire, adoption would be its own numbered proposal amending spec §30.5/§28.3 + rationale §5.24/§12; if they don't, a [DECLINED.md](DECLINED.md) entry.
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

## 0. Summary

This proposal expands item **P11** of [future-directions-2026-07-18.md](papers/future-directions-2026-07-18.md). It contains two deliberately separated moves:

- **Part A — the guidance flip (§2), docs-only, no semantics change.** Rationale §12.2 currently ends its producer decision tree with *"When in doubt, choose Pattern A."* The evidence (§1) says the language has outgrown that default: the affine model (v0.150, rationale §5.23) removed the leak-risk argument that motivated it, the v0.190 calls-mint ruling (rationale §12.10) removed the fewer-obligations argument, the standard library itself has resolved the doubt toward Pattern B in **every** resource-bearing case (§1.2: 21 `OwningTask<T>` API methods, zero resource-bearing Pattern A APIs), and both recent API proposals (0001, 0018) chose B citing stdlib precedent (§1.3). The flip makes the recorded guidance match the recorded practice: **when in doubt over a fresh, single-consumer class result, choose Pattern B (`OwningTask<T>`); Pattern A (`Task<T>` for class T) becomes the marked, justified case** — fan-out and chained republication, stated in a doc comment. Not one signature, not one program, and not one compiler rule changes.

- **Part B — the study (§3), explicitly *not* adoption.** The one-rule alternative to §30.5's growing consumer-side admission list — defer an awaited task temporary's disposal to the `var` binding's scope end — is stated precisely, its soundness walked, its cost stated plainly, and **concrete adoption criteria** are recorded. The deliverable of Part B is the criteria, not the rule. The recommendation is to defer the decision, land Part A, and re-measure: the flip itself shrinks the population the rule would serve, and may dissolve it. This is also not a fresh idea — the v0.182 revision already **declined** exactly this mechanism ("auto-binding the temporary to an anonymous scope-bound slot … kept as a liftable, tracked", v0.182 changelog) — so Part B is the recorded liftable finally evaluated with criteria, not a silent re-litigation (§3.4).

## 1. Problem and evidence

All section citations verified against v0.197 (spec, stdlib, rationale, changelog headings grepped before citing).

### 1.1 The friction, and the relaxation sequence that proves it

The most common first-contact async shape —

```bezel
var config := await FetchConfigAsync(url);   // ERROR under Pattern A (Task<Config>, class result)
```

— is a compile error when the producer chose Pattern A, because the await's borrow would outlive the unbound task temporary that sources it (spec §30.5: *"A bare `var x := await someTask;` where `someTask` is not in one of the admitted positions is a compile error"*; Appendix C.6 restates it). The sanctioned form is two lines:

```bezel
using var task := FetchConfigAsync(url);
var config := await task;
```

Each individual rule here is right: the task owns the result, a borrow needs a source, a temporary dies at its full expression. The evidence that the *composition* fights the dominant idiom is the admission history. §30.5's Pattern A admitted-position list began at **four** positions (v0.94, per the changelog: `using var`-bound local, `owned` parameter, `owned` field of `this`, chained producer-return) and now stands at **nine** bullets plus a temporary-confinement rule, grown by three separate relaxation trains:

| Revision | Admission added | Record |
|---|---|---|
| v0.182 | unbound owned temporary, borrow confined to the full expression — `Process(await Producer())` admitted; `.Result` in lockstep | changelog v0.182; spec §28.3 *Unbound owned temporaries* |
| v0.184 | plain owned locals (#1533); admitted static `Task<T>` fields (§22.13(b) caches) | spec §30.5 bullets 3 and 6 |
| v0.193 | plain borrowed locals and parameters of `Task<T>` (#1800, PG-13) | spec §30.5 bullet 4 |

The list as it stands at v0.197 (spec §30.5, "the list is not exhaustive but illustrates the rule"):

| # | Admitted task-expression position | Since |
|---|---|---|
| 1 | `using var` / `await using var`-bound local | v0.94 |
| 2 | `owned` parameter | v0.94 |
| 3 | plain owned local | v0.184 (#1533) |
| 4 | plain (non-`owned`) local or parameter | v0.193 (#1800) |
| 5 | `owned` field of `this` | v0.94 |
| 6 | admitted static `Task<T>` field (§22.13(b) cache) | v0.184 |
| 7 | indexer access on an owning collection of tasks | pre-v0.182 |
| 8 | borrowing-iteration variable over an array of tasks | pre-v0.182 |
| 9 | producer call directly inside a Pattern A `return` (chained slot) | v0.94 |
| — | unbound owned temporary, borrow confined to the full expression | v0.182 |

Three revisions in a row each spent a ruling widening the same consumer-side rule, and the shape users actually reach for — the one-line `var` bind of a fresh producer's result — is *still* the rejected one. The relaxations were individually principled (each reused the named-location machinery); the *sequence* is the signal. P11's reading, adopted here: the rule is fighting the dominant idiom, and there are two distinct responses — stop steering producers into Pattern A when they don't need it (Part A, cheap, now), and/or admit the one-line form by a bounded lifetime rule (Part B, a language change, only behind evidence).

One more piece of context the flip rests on: **none of this friction exists under Pattern B.** `using var x := await AcquireAsync();` is the canonical form, and `var x := await theTask;` has compiled since v0.90 (the changelog's own loosening record) — §30.5's owned-consuming contexts; the entire admission history above is Pattern-A-only. The rule set is not asymmetrically broken — it is asymmetrically *taxed*, and the tax falls on the pattern the guidance steers doubt toward.

### 1.2 Standard-library inventory: the doubt-default is already dead in practice

Method: grep of `Task`/`OwningTask` return positions over every declaration line in `Bezel_Standard_Library.md` code blocks (v0.197), multi-line signatures hand-checked, worked-example methods (`LoadAllConfigs`, `FailFast`, domain examples) excluded as non-API.

**Pattern B — `OwningTask<T>` (class result, ownership transfers): 21 API methods + 1 property.**

| Stdlib § | Declaration (return type) | Kind of result |
|---|---|---|
| §6.2 | `Mutex.AcquireAsync(token := …): OwningTask<Lock>` | lock token |
| §6.2 | `Mutex.TryAcquireAsync(timeout, token := …): OwningTask<Lock?>` | lock token (nullable) |
| §6.3 | `Semaphore.AcquireAsync(token := …): OwningTask<SemaphoreToken>` | permit token |
| §6.3 | `Semaphore.TryAcquireAsync(timeout, token := …): OwningTask<SemaphoreToken?>` | permit token (nullable) |
| §6.7 | `Task.FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class` | supplied fresh value |
| §6.7 | `Task.WhenAllOwned<T>(owned tasks): OwningTask<OwningArray<T>> where T: class` | aggregated owned results |
| §6.8 | `Domain.Spawn` — 3 sync rows `(… -> owned R): owned OwningTask<R>` | spawned result |
| §6.8 | `Domain.Spawn` — 3 async rows `(… -> OwningTask<R>): owned OwningTask<R>` | spawned result |
| §6.9 | `Channel<T>.Receive(): OwningTask<T>` (class T) | transferred item |
| §6.11 | `Locked<T>.LockAsync` / `TryLockAsync`: `OwningTask<LockGuard<T>[?]>` | data-lock guard |
| §6.11 | `RwLocked<T>.ReadAsync` / `TryReadAsync`: `OwningTask<ReadGuard<T>[?]>` | read guard |
| §6.11 | `RwLocked<T>.WriteAsync` / `TryWriteAsync`: `OwningTask<WriteGuard<T>[?]>` | write guard |
| §6.12 | `Parallel.Map(owned items, …): owned OwningTask<OwningArray<R>>` | partitioned results |
| §7.1 | `Stream.ReadAllAsync(token := …): OwningTask<Array<byte>>` | fresh byte buffer |
| §6.6 | `OwningTaskCompletionSource<T>.Task: OwningTask<T>` *(property — storage, lends)* | imperative completion |

Note what the table's last column shows: the B population is **not** all tokens and guards. `Receive`, `ReadAllAsync`, `WhenAllOwned`, `Map`, and the `Spawn` result rows are *data* — fresh values the caller will hold — which is exactly the category §12.2's current text leaves "in doubt" and steers to A.

**Pattern A — `Task<T>` for a class T: zero resource-bearing APIs.** The only class-typed `Task<T>` shapes in the library are:

- `Task.FromException<T>`/`FromCancelled<T>` — admit class T, but no value ever flows; stdlib §6.7 states explicitly that *"the Pattern A vs Pattern B distinction that motivates `OwningTask<T>` does not apply."*
- `Task.WhenAll<T>: Task<Array<T>>` (non-class element T) and the frozen-input `Parallel.Map` row (`owned Task<Array<R>>`, R non-class) — a fresh trivial-dispose container of value results, not a resource.
- `TaskCompletionSource<T>.Task` — documented as the non-class-T form (§6.6).

**No pattern (value/`string`/unit results): the bulk of the surface** — `DisposeAsync`, `MoveNextAsync`, `RaiseAsync`, `SendAsync`, `TaskScope.Join`, `Delay`×4, `WhenAll`×2, `WhenAny`, `FromResult`, `Stream.ReadAsync`/`WriteAsync`, `File.ReadAllTextAsync`/`WriteAllTextAsync`, twelve of the eighteen `Domain.Spawn` rows, the `Parallel.ForEach`/`Reduce` rows, both `TaskCompletionSource` task properties.

The finding: **§12.2's doubt-default has never once been exercised toward A in the library it governs.** Every time a stdlib operation produced a fresh class result the caller should hold, the choice was B. The "conservative default" is guidance nobody follows — including the guidance's own library.

### 1.3 Proposals drift: recent designs already default to B

- **0018 (async File family, accepted):** `ReadAllBytesAsync: OwningTask<Array<byte>>`, `ReadAllLinesAsync: OwningTask<Array<string>>` — "per the `Stream.ReadAllAsync` precedent" (its §8 Q1 argues Pattern A is *wrong* for a caller who wants the bytes beyond the task's lifetime).
- **0001 (Http, draft):** every request method returns `OwningTask<HttpResponse>`, arguing from spec §30.5's own producer sentence; its body readers `Bytes(): OwningTask<Array<byte>>` and `JsonOwned<T>(): OwningTask<T>` are B; fan-out is routed through `WhenAllOwned`.
- **0023 (Task.WhenEach, accepted):** provides `WhenEachOwned` so B inputs stay B through aggregation.

Fresh designs are not "in doubt" — they read §30.5's purpose-keyed sentence (*"`OwningTask<T>` is the right choice for resource acquisition … `Task<T>` is the right choice for fan-out"*) and pick B for fresh results. The only text still pointing the other way at the margin is §12.2's closing paragraph.

### 1.4 Test-corpus sampling (approximate, method and caveats stated)

Method: substring greps over `tests/` (gtest `.cpp` harness files with embedded Bezel snippets, plus `.bzl`), v0.197 main. **Caveats:** no type resolution (a `var x := await F();` may be a value/string result — legal today — or a Pattern B owned bind; grep cannot split those), multi-line and renamed shapes are missed, deliberate-rejection tests are counted alongside accepted code, and a compiler test corpus over-represents error shapes relative to application code. Numbers are directional, not precise.

- 196 files contain `await`; ~1,546 occurrences total (~62 in `tests/conformance`).
- Direct producer-call operand `await F(…)`: ~683 — the single largest shape.
- One-line binds: `var x := await …` ~143 (7 in conformance); `using var x := await …` ~107 (+5 `await using var`; 14 in conformance).
- `return await …`: ~98.
- The two-line Pattern A shape: ~180 `using var t := <task-producing call>;` bindings; 27 files co-contain `using var t := …` and a later `await t`.

Directional conclusion, stated at its honest strength: wherever the rules admit a one-line consumption shape, the corpus uses it; the two-line form appears predominantly where Pattern A forces it or where the test exists to exercise it. The stronger claim P11 makes — that the one-line bind is what the target audience writes *unprompted* — rests on the guides' and spec's own example progressions and on the §1.1 admission pressure, not on these counts; the type-resolved classification that could settle it precisely is deferred to Part B's gate (§3.5), where it is load-bearing.

### 1.5 What the evidence does and does not show

It shows: the doubt-default is exercised toward B universally in practice (§1.2, §1.3); the consumer-side friction is real and has cost three rulings (§1.1). It does **not** show: that Pattern A is dispensable (fan-out and chained republication are real — §30.4, §22.13(b) caches, the chained-slot machinery of §30.5); nor that the language change of Part B is needed (the flip may dissolve the residual pressure). The proposal is shaped accordingly: flip the words now, gate the rule on measurements taken after the words have had their effect.

## 2. The design, Part A: the guidance flip

> **Docs-only. No semantics change.** No grammar production, no spec rule, no stdlib signature, no compiler behavior, and no existing program changes under Part A. The flip governs what *new* producers are advised to write.

### 2.1 Proposed replacement text for rationale §12.2's decision tree

The Step 1 (value/`string`/runtime-managed → `Task<T>`, no pattern) is unchanged. Steps 2–4 and the closing paragraphs are replaced with the following (drop-in, written in the rationale's voice):

---

**Step 2: The result is a class type. Will more than one consumer legitimately read the *same result instance*?**

This is the fan-out question, and it is architectural, not incidental: a config cache shared by every caller (§22.13(b) static task caches), a lookup result several subscribers await, a task stored once and awaited from multiple places (§30.4). Fan-out is announced in a design — it is rarely discovered late.

If yes → **Pattern A: `Task<T>`** — and say why at the declaration:

```bezel
/// Pattern A (fan-out): one fetch, every caller borrows the same Config.
public async method FetchConfigAsync(): Task<Config>
```

The same answer covers **chained republication** — an async body that re-exposes a longer-lived owner's result by chaining (`return await repo.LoadAsync(id);`, §30.5's chained slots) or that shares a fresh value through the task's owned slot. Callers borrow through the task; the task's teardown is the defined owner at every link.

**Step 3: Otherwise — the result is a fresh, single-consumer class value → Pattern B: `OwningTask<T>`.**

This is the default, and it covers more than classic resource acquisition. Connections, lock tokens, and file handles qualify, but so does ordinary fresh *data* the caller will hold: a parsed document, a materialized array, a response object. The consumer writes the form every caller writes unprompted:

```bezel
using var conn := await ConnectAsync(url);    // owned; disposed at scope end
var doc := await ParseAsync(input);           // owned local; affine discharge rules apply
```

**When in doubt, choose Pattern B.** Four reasons, each a recorded fact of the current model:

1. *The failure mode is caught.* Under the affine model (§5.23, v0.150), an owned result the caller never discharges is auto-dropped at scope end with the must-use warning — forgetting to dispose is not a leak, it is a diagnostic. The old argument for A-as-conservative ("no one can accidentally leak") is grounded in the pre-affine model; it is now equally true of B.
2. *A does not reduce the caller's obligations.* Since v0.190 (§12.10) the caller owns the *task* in either pattern. A moves the discharge obligation from the value the caller wants onto the wrapper around it; B puts it on the value itself and lets the await discharge the wrapper.
3. *The consumer form is the natural one.* B's canonical call site is the one-line `using var x := await …;` A's is the two-line bound-task form, and the one-line `var` bind is a compile error (§30.5).
4. *B is recoverable; the recovery is one line.* A producer who chose B and later discovers a fan-out need does not narrow the existing signature (that would break owners); it publishes a sibling (§12.1's "two methods with distinct names"), and the adapter is the shape §30.5 itself anticipates — "Pattern A class-typed tasks arise from async methods whose body returns a value", the async re-publishing wrapper its factory note names: a Pattern B await is an owned expression, and a Pattern A async body's `return` of an owned expression fills the task's owned slot (§30.5, slot kind 1):

```bezel
/// Pattern A façade over the Pattern B core, for subscribers.
public async method FetchSharedAsync(url: string): Task<Config>
  return await FetchOwnedAsync(url);   // B result (owned) fills the A task's owned slot
end FetchSharedAsync;
```

**The question that settles it** is no longer "would forgetting to dispose leak?" (the affine model answers that for both patterns) but: ***"can a second consumer legitimately need this same result instance?"*** If yes, Pattern A — the task is the shared owner. If no — or if you are unsure — Pattern B: hand the caller the value, and let the ownership machinery do what it exists to do.

| Question | Answer | Pattern | Return type |
|---|---|---|---|
| Result is value type / `string` / runtime-managed? | yes | neither | `Task<T>` |
| Several consumers read the same result instance (fan-out), or the body republishes a longer-lived owner's result (chaining)? | yes | A — say why at the declaration | `Task<T>` |
| Fresh, single-consumer class result — including plain data the caller will hold? | yes | **B (the default)** | `OwningTask<T>` |
| Still in doubt? | — | **B** | `OwningTask<T>` |

---

### 2.2 Reconciling with the recorded reversibility argument

§12.2's current closing paragraph rests on a real asymmetry, and the flip does not deny it — it re-weighs it. The recorded facts, restated and kept:

- **A→B remains the mechanical-migration direction.** Widening `Task<T>` to `OwningTask<T>` breaks callers *loudly and compilably*: one-line binds keep compiling (a Pattern B `var x := await t;` is an owned bind), non-owned-consuming awaits and multi-await sites are rejected with the §30.5 diagnostics, and the fixes are local. This fact is unchanged by the flip and stays recorded.
- **B→A remains breaking.** Callers wrote `using var x := await t;` and owned the result; under A that same line is rejected (the await produces a borrow, §30.5). Also unchanged.

What the flip changes is which default you *start* from, and it argues the start matters more than the escape route: the reversibility argument buys an option — start at A, widen to B once ownership need is proven — that the recorded surface has never needed (§1.2: no stdlib API started at A and widened; every doubt-case went straight to B), and it pays for that option with the two-line consumer tax on every call site in the meantime. Meanwhile the reverse risk ("chose B, needed A") has the one-line recovery of §2.1 point 4, which does not touch the existing signature at all. Optimizing the everyday path over an unexercised option is the trade; it is made here with the §1 evidence attached, so it can be revisited if the evidence ever inverts.

### 2.3 The §12.1 qualifying sentence

§12.1's mechanism description is untouched. One sentence is appended to its closing line ("Producers who want both shapes declare two methods with distinct names"), pointing at §2.1's adapter shape so the two-methods advice carries its implementation. No other rationale change.

### 2.4 The splice map (for near-mechanical graduation)

Exact edit list against the v0.197 rationale text, keyed by each passage's opening words:

| §12.2 passage (v0.197) | Disposition |
|---|---|
| Spelling note (*"revisions v0.78–v0.96 wrote Pattern B as…"*) | **keep** verbatim |
| Intro (*"The choice between Pattern A and Pattern B is the producer's…"*) | **keep** verbatim |
| **Step 1** (*"Is the result type a value type…"*) + its example block | **keep** verbatim |
| **Step 2** (*"Does the operation produce a fresh resource…"*) + examples | **replace** with §2.1's Step 2 (fan-out asked first) |
| **Step 3** (*"Could multiple consumers need the same result?"*) + examples | **replace** — content moves into the new Step 2 |
| **Step 4** (*"…produced by a query or lookup against longer-lived state"*) | **fold** into the new Step 2's chained-republication paragraph; the #1332-corrected chaining explanation is kept, condensed, with the §30.5 chained-slot citation |
| **Summary table** | **replace** with §2.1's table |
| *"When in doubt, choose Pattern A."* paragraph | **replace** with §2.1's "When in doubt, choose Pattern B" paragraph (four numbered reasons) |
| *"The one question that settles it"* paragraph | **replace** with §2.1's closing question paragraph |

Plus: one sentence appended to §12.1 (§2.3), and a changelog entry recording the flip *as a guidance reversal with the evidence cited* — the reversal of a recorded position must itself be recorded, per the project's decline-with-reasons discipline.

### 2.5 Affected passages — the verified list

- **Rationale §12.2**: Steps 2–4, the summary table, and the two closing paragraphs replaced per §2.1. (Step 1 and the spelling note stay.)
- **Rationale §12.1**: one appended sentence (§2.3).
- **Spec §30.5**: *no required change.* The existing producer-guidance paragraph is already purpose-keyed ("`OwningTask<T>` is the right choice for resource acquisition… `Task<T>` is the right choice for fan-out") and is consistent with the flip. An optional single sentence — "When neither purpose clearly applies, prefer `OwningTask<T>` for a fresh class result (rationale §12.2)" — is offered as owner question Q2; the flip works without it.
- **Guides**: **zero edits.** Verified by grep: Guide 01 mentions the task family only in its §4 producers rule-of-thumb and the lazy-pipeline note (both about v0.190 minting, pattern-neutral; its "Pattern A/B" in §8 names an unrelated branch-merge idiom); Guide 02's `await` mentions are narrowing-related only. Neither guide teaches the producer decision. Whether a guide *should* gain a producer-decision passage is deferred (Q7) — it is additive either way.
- **Stdlib**: **zero edits.** No signature changes by construction (docs-only); and §1.2 found no A-typed signature the new guidance would even advise re-choosing.

## 3. The design, Part B: the study

> **This part proposes no adoption.** Its deliverable is the candidate rule stated precisely enough to evaluate, the soundness walk, the honest cost, and the adoption criteria. The recommendation (§3.6) is to defer the decision until Part A's effect is measurable.

### 3.1 The candidate rule, stated precisely

> **Deferred disposal of a `var`-bound awaited task temporary (candidate; not adopted).** In a local variable declaration `var x := await E;` whose initializer full expression is exactly the await, where `E` is an **unbound owned temporary** of type `Task<T>` for class T (§28.3: a bare producer call or other unnamed owned task expression), the temporary's disposal — today performed at the end of the full expression, which is what makes the binding an error — is instead deferred to the **end of the innermost scope enclosing the declaration**. The temporary occupies a compiler-introduced anonymous slot, scope-pinned exactly as a `using var` (§28.10), established on the cleanup stack at the declaration. `x` is an ordinary borrowed local whose source is that slot; every existing borrow rule applies to `x` against it. The slot is not nameable: no program text can read, move, consume, re-await, or dispose it.

```bezel
public async method Handle(url: string): Task
  var cfg := await FetchConfigAsync(url);   // candidate: the Task<Config> temporary is pinned
  Apply(cfg);                               // cfg is a borrow of the pinned slot's result
end Handle;                                 // slot disposed here; cfg's referent with it
```

Boundary decisions folded into the statement (each would be normative at adoption):

- **Only the exact form.** The initializer must be the await itself. Compositions (`var y := (await E).Field;`, `var z := await E ?? fallback;`) stay under today's rules — widening them is a separate question (Q5); the study evaluates the minimal rule.
- **`using var x := await E;` stays rejected** — the await still produces a borrow, and `using var` still requires an owned right-hand side (§30.5). The rule extends a lifetime; it manufactures no ownership.
- **Named operands unaffected.** `var x := await t;` over any admitted named position keeps its current meaning; the rule fires only where today's program is an error. It is a **pure widening**: no currently-legal program changes meaning.
- **Loops**: the enclosing scope of a loop-body declaration is the loop body — per-iteration disposal, no accumulation across iterations, the same per-evaluation concern §5.24 already resolved for condition temporaries.
- **`.Result` lockstep** (`var x := SomeProducer().Result;` on a completed task, §30.3) — open question Q5; the v0.182 precedent moved the two in lockstep, and the study's default assumption is lockstep, but nothing below depends on it.

What stays rejected under the candidate rule — the boundary shown by example, since a bounded stretch is defined as much by what it does not admit:

```bezel
using var r := await FetchConfigAsync(url);   // still ERROR: await produced a borrow,
                                              //   using var requires owned (§30.5)

_stored := await FetchConfigAsync(url);       // still ERROR: field store escapes the
                                              //   binding form; no scope to pin to

return await FetchConfigAsync(url);           // unchanged: the chained-slot rule of
                                              //   §30.5 governs, not this rule

var a := (await FetchConfigAsync(url)).Name;  // outside the study's minimal form;
                                              //   today's rules apply ("Name" is a
                                              //   string — this one already compiles)
```

Proposed diagnostics, in §28.3's recommended-message style, so the rule teaches itself if adopted: at the binding, an informational note on request (*"the task returned by `FetchConfigAsync` is anonymous and pinned to the end of this scope; `cfg` borrows its result"*); wherever the pinned slot participates in a later error (*"the result borrowed by `cfg` is owned by the awaited task pinned at line N; it is disposed at the end of that scope"*). If the rule is declined, the **existing** §30.5 error message gains nothing — the current text already names the two-line fix.

### 3.2 Soundness walk

- **Exception paths dispose as today.** If `E`'s production or the await faults, the unwinding pass disposes the already-produced temporary exactly as §28.3's exception-path discharge does now (established at production on the §28.10 cleanup stack); the binding never comes into being and the deferral never starts. No new edge.
- **No second await is expressible.** No binding names the task: the single await already ran, the slot is unnameable, and `x` reaches only the *result*. §30.4's multi-await surface never opens; the deferred interval holds a **settled** task — no live frame, no suspended borrows, just the container and its owned result. (This is also why §30.1's source-outlives-settlement obligation is discharged before the deferral begins: the await *is* the settlement.)
- **Consume-site freeze: no surface.** The freeze (§28.3) rejects consuming a binding while a borrow it sources is live. Here there is no user-visible binding to consume, move, reassign, or dispose — the freeze has nothing to guard, and correspondingly nothing new to track. Reassigning `x` merely ends `x`'s interest in the slot; the slot's disposal stays at scope end (a timing question only, and scope end is the recorded answer).
- **Escapes of `x` are already handled.** `x` is a plain borrowed local with a defined source whose scope is the enclosing block; returning it, storing it, or capturing it past that scope is rejected by the ordinary borrow-lifetime rules with no new machinery.
- **Checked-mode backstop unchanged.** The task and its result remain disposal-tracked; a use that reaches the result through a path the static rules cannot see faults deterministically as `DisposedAccessException` in checked disposal mode (§28.2), exactly as today.

The walk finds no soundness hole; the objections to the rule are not soundness objections (§3.4).

### 3.3 The §5.24 posture, engaged honestly

Rationale §5.24 records the language's deliberate resistance to lifetime extension: the full-expression boundary is maximal and per-evaluation; the **one** admitted stretch is the `case` scrutinee, whose entry closes with the template this study reuses — *"The stretch is recorded as the bounded thing it is: the boundary ends with the construct that syntactically contains the scrutinee, everywhere else §9.7's reject-not-extend posture stands, and Bezel still has no general lifetime-extension mechanism."*

The candidate rule would be a **second bounded stretch of the same kind**: one syntactic form (`var x := await E;`), one bound (the binding's enclosing scope), everywhere else the reject-not-extend posture untouched. If adopted, it must land with exactly that recording — a named, bounded carve-out, not a principle — and §5.24 gains a parallel paragraph.

Two honest differences from the `case` precedent, stated rather than smoothed over:

1. **The `case` stretch was forced; this one is elective.** The scrutinee extension existed because rejecting it would have outlawed a long-admitted expression-position form. Nothing forces this rule; the two-line form always exists. An elective stretch needs stronger evidence, which is what §3.5's criteria are for.
2. **This exact mechanism carries a recorded decline.** The v0.182 train considered "auto-binding the temporary to an anonymous scope-bound slot" and declined it as admitting the `var`-binding form "by silent lifetime extension — the recorded reject-not-extend posture; kept as a liftable, tracked" (v0.182 changelog). Part B is that liftable's evaluation. Revisiting a recorded decline requires new evidence against the stated reason; the stated reason was the posture itself, so the evidence must show the posture's cost has grown — which is precisely what criterion C2 (§3.5) measures: whether the admission list keeps growing *after* the guidance flip removes the steering-into-A pressure.

### 3.4 The cost, stated plainly

**The strongest objection is the invisible lifetime.** Everywhere else in the language, a borrow's source is visible at the site: a named binding, a parameter, a field path, or a temporary whose extent is the statement the reader is looking at (§5.24's boundary), or a construct whose own brackets show the stretch (`case … end`). Under this rule, `var cfg := await FetchConfigAsync(url);` reads as "cfg is the config" while an unnamed object the reader cannot see holds the config until a scope end the line does not mention. The reader auditing disposal order at scope exit must know the rule to know the slot is there. That is a real erosion of the program-text-carries-the-meaning property, and it is the reason the recommendation below is defer, not adopt.

Mitigations, priced honestly: an IDE inlay hint (a ghost `⟨task pinned to scope end⟩` annotation at the binding) restores visibility for editor users but not for review diffs or plain-text reading; diagnostics that mention the slot when it participates in an error ("the result borrowed by `cfg` is owned by the awaited task, pinned to this scope") keep the error surface teachable. Neither removes the objection; both narrow it.

Secondary costs: a second §5.24 carve-out spends posture-credibility (each stretch makes the next one easier to argue and the doctrine harder to state); the implicit slot participates in scope-exit disposal order (defined — reverse establishment, §28.10 — but one more invisible participant); and spec surface grows in §30.5/§28.3, the two densest sections the project maintains.

### 3.5 Adoption criteria (the deliverable), and the decline path

Adopt only if **all** of the following hold at evaluation time; otherwise decline permanently.

- **C1 — the shape still dominates after the flip.** A type-resolved classification (compiler-assisted, not grep — the §1.4 caveats are exactly what it removes) of every Pattern A await site in the test corpus, conformance suite, and any application corpora then available shows that the bound-task-awaited-once shape — `using var t := P(); … await t;` with `t` awaited exactly once and never otherwise used, the mechanical signature of "wanted the one-liner" — still accounts for **≥ 25%** of Pattern A await sites (threshold: owner question Q3). If the flip has done its work, Pattern A's residual population is announced fan-out and chaining, and this shape should be rare; if it still dominates, the friction survived the guidance fix.

  *Measurement protocol, fixed now so the later run cannot be gamed by definition drift:* classify each `await` whose operand's static type is `Task<T>` for non-trivial-dispose class T (the compiler's own §28.9 classification, excluding the trivial-dispose containers of §1.2) into: (a) bound-task-awaited-once (the target shape); (b) multi-await of one binding (genuine fan-out); (c) chained `return await`; (d) expression-confined temporary (v0.182 form); (e) borrowed-parameter helper (v0.193 form); (f) other. C1 fires on a/(a+b+c+d+e+f) ≥ 0.25. Deliberate-rejection test sites are excluded (they encode the rule, not usage). The tool is a small compiler-query pass — a natural first client of the P12 query API if that lands first, but not blocked on it.
- **C2 — the admission list grows again.** After v0.197, at least one further consumer-side admission (a tenth §30.5 position, or a new temporary-confinement carve-out) is requested with enough force to be filed and seriously considered. The rule's entire value proposition is replacing the *next three admissions* with one recorded stretch; if no next admission materializes, the list has converged and the rule buys nothing.
- **C3 — no new machinery.** A prototype confirms the implementation is the §28.10 cleanup-stack slot and nothing else — no new analysis, no consume-freeze extension, no checked-mode changes. If the prototype needs more, the "one crisp rule" framing was wrong and the cost side of §3.4 was understated.

**If C1 or C2 fails:** decline **permanently** — a one-line entry in [DECLINED.md](DECLINED.md) citing this proposal's §3 as the reasons record and closing the v0.182 liftable, so the question stays settled rather than perennially reopened (that file's own standard). **If all three hold:** adoption is a fresh numbered proposal amending spec §30.5/§28.3, rationale §5.24 (the parallel bounded-stretch paragraph) and §12, with its own critique pass — this study pre-writes its §3.1 statement and §3.2 walk but does not pre-authorize it.

### 3.6 Recommendation

Defer the decision. Land Part A; let it govern the stdlib implementation waves (D1–D3) and the in-flight proposals; run the C1 classification after that surface exists; evaluate C1–C3 then. The study's honest expectation, recorded so the later evaluation can be checked against it: **the flip dissolves most of the pressure** (the shapes that drove v0.182/v0.184/v0.193 were largely fresh-single-consumer results steered into Pattern A), and the likely outcome is a permanent decline with a clean record.

## 4. Interactions

Enumerated against every touching feature; "unaffected" claims were checked against the cited sections, not assumed.

1. **v0.190 calls-mint (§28.3 producers; rationale §12.10).** Both parts compose with it unchanged. Part A leans on it (§2.1 point 2). Part B's candidate rule governs only the *disposal timing* of a temporary the caller already owns by v0.190; the classification itself (calls mint, storage lends) is untouched.
2. **§30.4 multiple awaiters.** Part A keeps fan-out as Pattern A's first face — nothing narrows `Task<T>` semantics. Part B never opens a multi-await surface (§3.2, unnameable slot).
3. **§28.3 unbound temporaries + the §5.24 full-expression boundary.** Part A: no contact. Part B is a named exception to the boundary for one form; §3.3 is the engagement. The deferred-activation argument rules (temporary passed *into* an async call) are unaffected — they concern arguments, not the awaited operand.
4. **Consume-site freeze (§28.3).** No contact from either part (§3.2: no freezable binding exists).
5. **`.Result` (§30.3).** Part A: no contact. Part B: lockstep is Q5; §30.3's blocking-wait rules are orthogonal and unaffected either way.
6. **§30.1 settlement obligations and the abandonment idioms.** Part A: no contact. Part B: the deferral starts only after settlement (§3.2); `using var t := call();`-and-drop and `FireAndForget` shapes are untouched.
7. **§30.11 completion sources; §30.5's no-Pattern-A-factory note.** The flip aligns the guidance with the machinery that actually exists: B has `FromOwnedResult` and `OwningTaskCompletionSource`; imperative completion of a Pattern A class task is recorded as an open future-revision question in §30.5. Defaulting producers toward B means defaulting them toward the fully-expressible pattern. No text in §30.11 changes.
8. **Aggregation (`WhenAll`/`WhenAllOwned`, §30.10; 0023's `WhenEachOwned`).** More B producers means more `WhenAllOwned`/`WhenEachOwned` fan-in, which is the direction the surface already built for. No change to the aggregation contracts.
9. **Checked disposal mode (§28.2).** Backstop semantics unchanged in both parts (§3.2).
10. **Static task caches (§22.13(b)) and chained slots (§30.5 kinds 2–3).** The Pattern A faces the flip *keeps*, now stated as the marked case with its justification convention. The v0.184/v0.193 admissions that serve them are untouched.
11. **Proposals in flight (0001, 0006, 0018, 0023).** The flip codifies the choice they already made; no proposal text needs amendment. 0006's `Stream.CopyToAsync`-family additions inherit the new default at graduation time.
12. **Proposal 0048 (explicit factory ownership; same run).** Orthogonal by construction: 0048 governs *synchronous* static returns and explicitly exempts `Task`-family returns as already announced by type. The two can land in either order.
13. **Async iterators (`IOwningAsyncEnumerable<T>`, §30.1/§29.2).** The owned-yielding convention is the streaming analogue of Pattern B and already the guidance for owned element streams; the flip makes the two consistent rather than interacting.

## 5. Migration and compatibility

**Part A: none, by construction.** No signature changes anywhere; no existing caller changes; the flip governs new APIs and new user code. The compatibility facts of record are restated in §2.2 and *kept*: A→B stays the mechanical (loud, compilable) migration direction; B→A stays breaking; the B-first recovery for a late fan-out need is a new sibling method plus the one-line adapter (§2.1) — additive, non-breaking.

**Part B, if ever adopted:** a pure widening — every program legal today keeps its exact meaning (the rule fires only on today's errors), so there is no migration; the compiler work is the §28.10 slot plus diagnostics. **If declined:** nothing to migrate; the DECLINED.md entry closes the question.

One deliberate non-goal, recorded: this proposal does **not** revisit any existing stdlib `Task<T>` signature (there is nothing to revisit, §1.2) and does not propose renaming or restyling the pattern vocabulary ("Pattern A/B" naming stays; a rename is churn with no capability).

## 6. Staging

1. **Stage 1 — the flip (one docs revision).** Rationale §12.2 rewrite + §12.1 sentence per §2; changelog entry; Q2's optional spec sentence if approved. Rides any docs train; no compiler work; the spec-change-process class is "clarification/guidance" (no semantics).
2. **Stage 2 — the measurement window (no action).** The stdlib implementation waves and in-flight proposals proceed under the new guidance. The window's exit condition is having a real post-flip API surface plus whatever corpora exist — not a calendar date.
3. **Stage 3 — the gate.** Run the C1 type-resolved classification; check C2 against the issue tracker; prototype C3 only if C1 and C2 both held. Then either the fresh adoption proposal or the DECLINED.md entry (§3.5). Either exit closes the v0.182 liftable explicitly.

## 7. Alternatives considered and declined

- **Keep the A doubt-default (status quo).** Declined: §1.2/§1.3 show the default is followed by no one, including the stdlib; recorded guidance that diverges from recorded practice is worse than either alone, because it teaches newcomers the pattern whose consumer form they then can't write in one line.
- **Adopt the lifetime extension now, skip the study.** Declined: it spends a §5.24 carve-out and overturns a recorded v0.182 decline *before* the cheap fix has been tried; if the flip dissolves the pressure, the language change would have been paid for nothing — and it is the kind of cost (invisible lifetime) that cannot be refunded later without a breaking change.
- **Make `using var x := await task;` legal for Pattern A by transferring the result out of the task.** Declined as unsound, not just undesirable: Pattern A results may have multiple borrowers (§30.4); transferring the result out from under one awaiter dangles every other, and the task's own teardown contract ("disposing the task disposes the result") would need a per-instance mode. This is the B semantics; the type system already spells it `OwningTask<T>`.
- **Infer the pattern from the await context (syntactic peeking).** Declined; §30.5 already records the principle — "there is no syntactic peeking at `await`'s context" — and peeking would make a producer's contract depend on each consumer, destroying the signature-carries-the-choice property that §12.1 is built on.
- **Retire `Task<T>`-for-class entirely (an all-B world).** Declined: fan-out and chained republication are real and load-bearing (§30.4, §22.13(b), §30.5 kinds 2–3); removing A forces every shared-result idiom through `Shared<T>`-style machinery it doesn't need. The flip demotes A to the marked case; it does not abolish it.
- **A compiler lint nudging class-typed `Task<T>` declarations toward B.** Declined: the two legitimate A faces are common enough that a signature-level warning would misfire exactly where the author made the right call, and the decision inputs (fan-out intent) are invisible to the compiler. Guidance belongs in the rationale; the diagnostic surface is already good at the consumer side.
- **Widening the candidate rule to all escaping uses of the awaited borrow (stores, returns).** Declined even as a study target: a store or return has no single enclosing scope to pin to; only the `var`-binding form has a crisp, reader-computable bound. The minimal rule is the only one that stays "one crisp rule."

## 8. Open questions / owner decisions

1. **Q1 — approve Part A as drafted?** The §2.1 replacement text is written to be graduated near-mechanically into rationale §12.2.
2. **Q2 — the optional spec §30.5 sentence.** Include the one-sentence doubt-default in the spec's producer-guidance paragraph, or keep the flip rationale-only? (Recommendation: rationale-only; the spec paragraph is purpose-keyed and correct as is.)
3. **Q3 — the C1 threshold.** Is ≥ 25% of Pattern A await sites the right dominance bar, and should application corpora (once they exist) be required or merely admitted as evidence?
4. **Q4 — what counts for C2.** Any new §30.5 consumer-side admission, or only one that the flip's guidance could not have prevented? (Recommendation: the latter — an admission serving fan-out/chaining counts; one serving a fresh-single-consumer shape that should have been B does not.)
5. **Q5 — `.Result` lockstep in the candidate rule.** If Part B is ever adopted, does `var x := P().Result;` (completed task, outside domains) get the same deferral? The v0.182 precedent says lockstep; the blocking-wait rules make the form rare.
6. **Q6 — where a decline is recorded.** DECLINED.md entry (proposed — it is the register whose job this is) versus a rationale §5.24 addendum; or both, with §5.24 gaining one sentence pointing at the register.
7. **Q7 — guides.** Should Guide 01 (or a future async guide) gain a short producer-decision passage at flip time, or is the rationale the right sole home until an async guide exists? (Non-blocking either way; no current guide text is wrong.)

## Appendix: divergences from the P11 sketch (recorded, with reasons)

1. **P11 said "flip §12.2's default and the stdlib's choices where compatible."** The inventory (§1.2) found **zero** stdlib Pattern A resource APIs — there are no stdlib choices to flip. The stdlib half of the sketch dissolves into evidence *for* the flip rather than work under it.
2. **P11 gated the flip on measuring first** ("classify every await … if the single-consumer shape dominates as I expect, flip"). The measurement was run here (§1.4) with stated caveats; combined with the stdlib and proposals evidence it supports flipping without a further gate, so the proposal stages the flip immediately and reserves the type-resolved classification for Part B's gate, where it is load-bearing (C1) rather than confirmatory.
3. **P11 presented the lifetime extension as an un-studied alternative.** Research found the v0.182 train already declined this exact mechanism and kept it as a tracked liftable (§3.3). The study is therefore framed as that liftable's formal evaluation — which *raises* its evidence bar (a recorded decline must be overcome, not just a gap filled) and strengthens P11's own not-recommending-adoption stance.
4. **Scope narrowing:** P11's "the doubt-default should be B for any fresh, single-consumer class result" is kept, but the proposal makes the two legitimate Pattern A faces (fan-out, chained republication) explicit first-class steps of the rewritten tree rather than a residual, so the flip cannot be misread as deprecating Pattern A.
