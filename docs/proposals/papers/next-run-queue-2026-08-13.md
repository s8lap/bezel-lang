# Next-run work queue — batched by ROOT CAUSE, not by issue number

*Supporting record, written at the close of the 2026-08-12 catch-up run. Purpose: let the next
run fire wave 1 in minutes instead of spending ~40 min per wave re-reading issues and extracting
rulings. Every batch below is scoped to **the code that must change**, because the run's own
evidence is that fixing N issues that share one predicate is roughly one lane, while fixing them
as N issues is N lanes plus N chances to add a fresh divergent copy.*

**Measured basis.** 49 commits landed across three waves; ~50 issues filed. Three of five Wave-3
lanes needed a send-back, and in every case the finding was a **duplicated classification
predicate** or an **oracle that could not fail** — not a wrong algorithm.

---

## A. The imported-symbol audit — START HERE (highest yield, mechanically enumerable)

**Root cause.** `ModuleLoader` never sets `TypeSymbol::decl`. So every classification that reads
`dynamic_cast<const ast::TypeDecl*>(sym->decl)` **silently answers "no" for an imported symbol** —
no diagnostic, wrong answer only across a module boundary. There are **140 such reads** in
`src/sema` + `src/codegen`.

**Already-proven instances (all found the hard way, one at a time, this run):**
- `#2934`'s `ToString` site → qualified head where §8.5 forbids it, in one of three disagreeing spellings
- `isPlusOneStringProducer` → **962 B / 40 allocations leaked**, because the producer test and the
  lowering test were "kept in lockstep" by being two copies of one predicate
- **#3132** → `recordTypeOfRef`; per `emitRemoveElementEquality` the record arm is *"mandatory (not
  an extension)"* since an `ICmp` on a struct is invalid, so this risks a **wrong answer or an
  invalid compare**, not a slow path
- **#3130** → an imported body record's constructor does not store at all (adjacent, same seam)
- `#3046`'s sweep could not see a class that exists only as C++-built `ModuleMetadata`

**Triage rule (from the #2934 review, already validated):** a site that reads *only* a flag such as
`hasPositionalFields` **must be unified**; a site that goes on to dereference `td->positionalFields`
legitimately needs the AST decl and should be left alone.

**Deliverable.** One shared accessor per classification question; the 140 sites triaged into
unify / leave / already-correct, with the count of each recorded. Then a checker that flags a *new*
AST-only classification read outside a sanctioned helper.

**Issues closed or de-risked:** #3132, #3130, #3119 (partly), plus the whole class behind #2934.

---

## B. Predicate unification — the run's dominant defect generator

**Root cause.** The same question answered in more than one place; the copies drift silently and the
failure is always silent (wrong value / leak / double free), never loud.

| question | copies found | damage |
|---|---|---|
| "does this body own the task frame?" | 2, lane added a 3rd matched to the wrong one; review then found a **4th** (`Stmts.cpp:4500`, drain-loop iter var) | **double free** |
| "is this record positional / auto-bearing?" | >=4 | leak + two rendering divergences |
| "is this statement an await site?" | a hand-kept list of 12 vs the AST's **17** kinds | awaits in `const` initialisers and `case` arms never §30.5-checked |
| "is this Exception-derived?" | **1, by design** (#3046) | that lane passed review clean — the counter-example that proves the rule |

**Deliverable.** Enumerate every classification predicate across sema+codegen (owns-frame,
positional/auto-bearing, boxable, trivial-dispose, exception-derived, nullable-value, plus-one
producer). Unify each to one call — not two agreeing copies. **Make "cannot drift" structural.**

⚠ **Known trap for the owns-frame work (#3126):** a `LocalDecl`-keyed predicate structurally cannot
see the drain-loop iteration variable, whose symbol is anchored on the `ForStmt`. A *drained* task
iter var **owns** its frame; a *borrowing* iteration does not. The three-way classifier needs an
operand-level entry point.

---

## C. Async / §30.5 reclaim cluster — one lane, ASan, SOLO

`#3125` (owned-param source: `return await <owned param>` not recognised as the move-out §30.5 says
it is, **plus** `emitAsyncOwnedParamAutoDispose` disposing a `Task` through **class-vtable dispatch**
-> SEGV, reachable with **no chaining at all**; `unwindOwnedLocalClass` already has the missing guard
one file over) · `#3126` (kind-3 borrowed sources; needs the three-way predicate) · `#3129`
(`finallyStack_.empty()` gate: a `return await` through a `finally` records **neither** of §30.5's
kinds) · `#3121` (the three flips on the caught-arm edge, of which only one is currently recorded).

**#3125's second half should lead** — it is independently reachable and is a SEGV.

---

## D. Value-narrowing cluster — one lane (all §12.7/§8.6, all adjacent to the landed #3090)

`#3092` (the generic path: `o is T` in `Box<[3]int>` reads FALSE — the *type-test* half of §17.4's
named remedy, still dead) · `#3115` (unguarded `Object`->value initializer, still reinterprets a
pointer) · `#3120` (`is` on a non-nullable value operand silently dropped) · `#3122` (value-builtins
do not narrow) · `#3116` (the narrowed copy-out leaks the box) · `#3117` (**resolved upstream at
v0.254** — writes through value-narrowed bindings at boxed locations are now rejected; implement).

---

## E. The degrade-guard family — one lane

`#3105` (the silent-statement-drop guard covers only `Console`; ordinary call arguments and the `?.`
twin still drop the enclosing call — the highest-traffic shape) · `#3104` (the guard's own two
branches disagree: a unit argument is silently dropped while the untyped-builtin sibling hard-errors)
· `#3134` · `#3136` · `#3137`.

---

## F. Test-oracle integrity — cheap, prevents whole classes

- **`scripts/check_vacuous_tests.py`** (new, this run) finds gtests that assert `r.ran` **and** the
  absence of a diagnostic from the same `runAot` result — structurally unable to fail (#3138).
  Currently reports **10 assertions across 3 files**, including one shipped *during* this run by a
  lane that believed it had removed them all. Wire it as a gate phase (`--strict`).
  ⚠ It was validated against the known instance **after** a first draft reported the tree clean:
  the `BEZEL_TEST(` macro is frequently wrapped across two lines, so a `^BEZEL_TEST` regex matched
  zero tests. **Always control-run a detector against a known-positive before trusting a green.**
- **One sanitizer-honouring xmod helper.** Hand-rolled `driver.compileFile` runners bypass
  `BEZEL_AOT_FORCE_SANITIZE` (honoured only inside `runAot`), which is why a leak was invisible to
  **both** gate oracles. Route every xmod runner through one helper.

---

## G. The coverage step change — the spec is already a test suite, and we run a quarter of it

Conformance: **567 cases, 137 active, 434 skipped.** But the skips are mostly *harness artifacts*,
not missing features:

| reason | count | compiler's fault? |
|---|---|---|
| `incomplete-fragment` | 176 | no — extractor |
| `signature` / `signature-listing` | 127 | no — extractor |
| `mixed` / `syntax-snippet` / `elided` / `ebnf` | 69 | no — extractor |
| `blocked-on-#NNNN`, `not-implemented`, `stdlib-unresolved` | ~58 | **yes** |

Teaching `scripts/extract_doc_examples.py` to wrap fragments and signature listings (the way
`preamble.bzl` already wraps some cases) would take the executable spec from 137 to ~300+ cases
checked every gate. That converts unknown-unknowns into a burn-down list, which is what R0 lacks.

**Better progress metric than open-issue count** (which the owner has already ruled is not a metric):
**active conformance cases** and **defect-generator classes closed**.

---

## Protocol changes carried into the next run

1. **Never wait on a process pattern.** `scripts/gate_and_push.sh` now writes
   `$BEZEL_GATE_PIDFILE` + `$BEZEL_GATE_DONEFILE`; wait with `scripts/wait_pid.sh`. Better still,
   launch the gate as a harness-tracked background job so completion *notifies*. Two pattern-matching
   wait loops cost ~3.2 h this run — about half of all non-lane time, and more than the gates.
2. **Cherry-pick only after APPROVED.** Broken once; it let a double free reach local `main` and cost
   a revert-and-reapply.
3. **Reuse a fixed lane-worktree pool** rather than fresh worktrees per wave (~9 min cold build per
   lane per wave; ~70 min over three waves).
4. **Overlap reviews with the next wave's analysis phase** — a lane's first ~20-30 min is reading, not
   building, so the gate's zero-builders requirement still holds.
5. **Discourage gratuitous `LlvmBackend_impl.hpp` edits.** Three Wave-3 lanes added private members
   there; it is included by every `LlvmBackend_*` TU, which turned the gate's build phase from ~0s
   into **521s**.
6. **Gate arithmetic, so nobody is surprised again:** a push gate is `plain + ASan + conformance` —
   **two full suite passes**, ~10 min of shard each, plus build. Sub-10-minute figures in the
   telemetry are single-oracle diagnostic runs with a warm build; they are not acceptance runs.
