# Proposal 0198 — Narrowing across `try`: every statement of the protected block is a throw point for every arm

| | |
|---|---|
| **Status** | graduated-v0.340 — superseded — graduated v0.340 (accepted 2026-10-21 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-21 direction for this train ("let's do the next train that fixes all current spec issues"), [#6707](https://github.com/s8lap/bezel-lang/issues/6707), option A — flow narrowing models neither throw points nor arm selection: a `catch` arm and the `finally` begin with the facts that held at entry to the protected block less every fact the block invalidates at any point, no fact the block establishes reaching them, and a later arm also loses what an earlier arm's guard invalidates; the point after the statement takes the every-path meet of the block's normal completion and every arm's exit, then the `finally`'s own statements; critique round 1 applied (a `break` or `continue` reaches its target through the `finally`'s statements); landed with the v0.340 changelog entry) |
| **Tracking issue** | [#6707](https://github.com/s8lap/bezel-lang/issues/6707) (the question); [#6673](https://github.com/s8lap/bezel-lang/issues/6673) (the soundness bug whose fix measured the four programs); compiler tracker to be filed at the v0.340 landing (§8) |
| **Target module(s)** | **spec**: §7.4 (a new paragraph, *Narrowing across `try`*, before *Narrowing inside a lambda body*), §31.2 (one sentence after the `finally`-establishment paragraph). **rationale**: §6.1 (one paragraph). **code-generation reference**: §7.8 (one *Looks right, is rejected* pair). **proposals**: `README.md` (this row), `DECLINED.md` (two `declined-at-proposal` rows). No stdlib or guide text changes. |
| **Written against** | spec **v0.339** at `d60128c46` (main). Section numbers are v0.339's. |
| **Origin** | The #6673 fix (staging `211c0738`; its reviewer's measurement is quoted in [#6707](https://github.com/s8lap/bezel-lang/issues/6707)) closed a stack-smashing null dereference by applying to locals the model the `readonly`-field set already used (#2116): an arm, the `finally` and the point after the `try` see the entry facts minus anything invalidated anywhere in the block. Four programs the compiler accepted are rejected under it, and §7.4's every-path clause, read with exact throw points, admits each of them. Drafted by lane B3 of the v0.340 train. |

*Reference convention: unprefixed §-references are language-spec references at v0.339. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares or declares its own; none invents an API.*

---

## 0. Summary

§7.4 narrows locals, non-`ref` parameters and `readonly`-field paths by flow: "a fact holds at a point only when it holds on every path that reaches it". §31.2 defines the `try` statement's paths at run time — an exception raised anywhere in the protected block reaches the first arm whose pattern and guard select it, the `finally` runs on every exit — but says nothing about how the static analysis models them. Read with exact throw points and exact arm selection, the every-path clause admits `try s := null; s := "y"; catch IOException => … end; s.Length` (no statement between the two stores can raise, so no path reaches the arm with `s` null) and `s := null; try s := "x"; Log("a"); finally s.Length end` (every path into the `finally` passes the store). The compiler has never modelled either: since v0.214 (#2116) the `readonly`-field set treats every statement of the block as a possible throw point for every arm, and the #6673 fix extends that to locals, because the local set's cruder entry ∩ after-body intersection was blind to a store of `null` followed by a re-establishing store (probe-confirmed SEGV, [#6673](https://github.com/s8lap/bezel-lang/issues/6673)).

This proposal decides that **the analysis models neither throw points nor arm selection**. A `catch` arm and the `finally` block begin with the facts that held at entry to the protected block, less every fact the block invalidates at any point, and no fact the block establishes reaches them; a later arm's entry is further reduced by what an earlier arm's guard invalidates; the point after the statement holds the facts common to the block's normal completion and every arm's exit, then runs the `finally`'s own statements. The rule is one sentence a writer can hold ("inside an arm or a `finally`, nothing the block did counts"), it is what the compiler does, and the remedy is local: re-test, or re-assign, where the fact is needed.

**Reach, measured (§5):** no rule-corpus, doc-corpus, specification, guide, standard-library, gtest or lit program changes verdict. The four newly rejected programs are the #6673 reviewer's probes (QUOTED from the issue; they live outside the repository), each a shape no test in the tree writes.

---

## 1. The text, and what it does not say

> an early exit: after `if x = null then return; end;` […] the surviving path is narrowed […]; a fact holds at a point only when it holds on every path that reaches it;
>
> — §7.4, *How narrowing is established*, v0.339

> **How narrowing is invalidated.** The narrowing of a variable is invalidated when, within the controlled block: the variable is reassigned from a nullable-typed expression; or the variable is passed as a `ref` **or `out`** argument […]; or the variable is **mutably captured** […] and the block performs *any* operation that can transfer control to user code […]
>
> — §7.4, v0.339

> The arms are tried in order; the first arm whose pattern matches the thrown exception (and whose guard, if any, evaluates to `true`) is selected, its body runs, and exception handling completes. […] `finally` runs whether the protected block completed normally, was exited by an exception (caught or uncaught), or was exited by `return` […]
>
> — §31.2, v0.339

Three things are decided: the facts a straight-line sequence establishes and invalidates; the every-path meet at a join; and the run-time paths a `try` statement has. Two things are not:

1. **Which program points the analysis treats as raising.** A call can raise; so can an indexer, a conversion, an arithmetic overflow, a `!`, a `new` ([§17.7](#sec-the-new-operator)'s fatal rule aside). A plain store of a literal or of a local cannot. Nothing in §7.4 says whether the analysis knows this. Read as the run-time semantics, it does: `try s := null; s := "y"; catch …` has no path into the arm, and the every-path clause holds the fact vacuously at the arm's entry.
2. **Which arm a raise reaches.** §31.2 selects by type and guard at run time. Read as the run-time semantics, a `throw new IOException(…)` in the block reaches an `IOException` arm and not an `ArgumentException` arm, so a fact invalidated only before that throw is lost in one arm and kept in the other.

The compiler has never implemented either reading. The question is whether the text should state the model it does implement, or the finer one the text now implies.

---

## 2. Why the finer models cost more than they return

**Throw points (reading B).** The analysis would have to classify every expression form as raising or not: calls and function-value invocations raise; so do `!`, `as`, indexing, division and `mod`, checked arithmetic (§4.2), conversions (§5.2), interpolation (`ToString` runs), `await`, a getter, a user operator, and every compiler-provided operation that invokes user members (equality, `Contains`, disposal at scope end). The non-raising residue is a store of a literal, of a local or parameter read, of a `default`, of an address-free field read on a value kind — and a `new` of a class, since §17.7 makes allocation failure fatal rather than an exception, but not a `new` whose constructor body runs user code. The list is long, it is a property of every future construct, and it buys exactly the shape `s := null; s := "y";` — two stores with nothing between them, which a program writes as one store.

**Arm selection (reading C).** The analysis would have to know, at each throw point, the static type of what is raised: a `throw` expression's operand type is known; a call's raise set is not declared anywhere in the language (there are no throws clauses, and [§31.1](#sec-the-throw-operand) rules them out), so a call raises "any `Exception`", which every arm can match. Reading C therefore refines only the `throw` statements written in the block itself, and only against arms that cannot match the thrown type; `d05`'s three-level rethrow chain is the shape, and it ran correctly on the base only because the base did not drop the fact at all.

**What (A) costs.** A fact a program needs inside an arm or a `finally` is established inside it (`if s <> null then …`), and a fact needed after the `try` is re-established after it or carried by every arm. The remedy is at the use, local, and the same one §7.4 already teaches for a mutable field.

---

## 3. Options

### 3.1 Status quo

Leave §7.4's every-path clause to imply exact paths. The compiler diverges on every `try` whose block invalidates and re-establishes a fact, and the divergence is the unsound direction (the text admits, the sound compiler rejects), so the rule corpus would record an admission the implementation must refuse.

### 3.2 Option A — every statement is a throw point for every arm (decided)

State the conservative model: an arm and the `finally` see the entry facts less what the block invalidates anywhere; nothing the block establishes reaches them; a later arm's entry drops what an earlier arm's guard invalidates; the point after the `try` is the every-path meet of the block's normal completion and each arm's exit, through the `finally`'s own statements.

### 3.3 Option B — straight-line stores keep their final fact

A store sequence with no intervening raising operation keeps its final fact at arm entry; the rest as (A).

### 3.4 Option C — arm selection by exception type

Additionally model which arm a written `throw` can reach, so a fact invalidated only before a `throw` of type `X` is kept in arms that cannot match `X`.

---

## 4. Evaluation

| | status quo | **A (decided)** | B | C |
|---|---|---|---|---|
| **Soundness** | the text admits a null dereference the compiler must reject | sound: the arm's entry state is a subset of the state at every point of the block | sound with a correct raise classification; unsound the first time a new construct is misclassified | sound only for written `throw`s; a call's raise set is unknown |
| **Rules a writer learns** | one (the every-path clause) plus an unstated model | **one: inside an arm or a `finally`, nothing the block did counts** | the raise classification of every expression form | the above, plus per-arm reasoning |
| **Text** | — | one paragraph in §7.4, one sentence in §31.2 | the paragraph plus a raise-classification table that every future construct must join | the above plus an arm-reachability rule |
| **Compiler** | diverges | matches the #6673 fix (the field set since v0.214) | new analysis | new analysis |
| **Measured reach** (§5) | — | 0 in-tree programs; the four #6673 probes | the probes `c03`, `c08` admitted | `b18`, `d05` admitted |

Against the owner's standing directions — soundness first, then simplicity, narrowing a feature with negligible measured reach over adding rules — A states the model the compiler has always used, at zero in-tree cost, in one paragraph. B and C each add an analysis a reader must learn to predict what compiles, for shapes no program in the tree writes. The issue's recommendation (A) is followed.

---

## 5. Reach, measured

**Method.** `scratch/v0340/B3work/measure.py` scanned every `.bzl` under `tests/` (the rule corpus, the doc corpus, lit), every fenced ```` ```bezel ```` block in `docs/*.md`, and every gtest source under `tests/` for a `try` statement whose protected block or `finally` assigns a nullable-declared local that is then read with `.`, `!` or as an argument after the statement's `end`, and for a read inside an arm or `finally` of a local the block assigns. Six regex hits, inspected by hand: `tests/ownership/case_arm_local_depth_3552_test.cpp:195–206` reads `holder!` after a `try` (null-forgiving, no fact needed); `tests/codegen/numeric_shift_nan_char_513_test.cpp` (three) and `tests/codegen/ensure_ctor_out_var_slots_default_init_2402_test.cpp` (two) declare non-nullable locals inside the block or match comment text. **Zero** programs depend on a fact established inside a protected block reaching an arm, a `finally` or the point after the `try` through an invalidation.

**Rule corpus.** No `s7_4_*` or `s31_2_*` case states a narrowing across a `try` (`manifest.tsv`, searched by `try`, `catch`, `finally`, `narrow`). No banked verdict moves. Coverage gap recorded for the tracker: an arm-entry reject (`c03`'s shape), a `finally` reject (`c08`'s shape), and an after-`try` accept with no arm.

**lit.** `tests/lit/sema/readonly_field_narrowing_2116__catch_interior_drop_rejected.bzl` pins the field set's conservative arm entry — the rule this proposal states, already enforced for fields. No lit program asserts the finer reading.

**Guides, specification, stdlib, code-generation reference.** No example narrows across a `try` (Guide 02's `try` examples at lines 198–212 establish no fact inside the block).

**Newly rejected (QUOTED from #6707, the #6673 reviewer's probes, outside the repository):** `c03` (`try s := null; s := "y"; catch IOException => … end; s.Length`), `c08` (`s := null; try s := "x"; Log("a"); finally s.Length end`), `b18` (a catch arm that sets `s` non-null and then throws to an outer arm, the outer arm reading `s`), `d05` (a three-level arm rethrow chain). Each is rejected at the read, and each is repaired by a re-test at the use.

---

## 6. Soundness and the neighbouring cells

- **Arm entry is a subset of every block point.** The entry facts less every fact invalidated anywhere in the block are held at every program point of the block, so whatever point raised, the arm's state holds there. Nothing the block establishes enters, so a raise before the establishing statement cannot be mis-narrowed (the #2164 SEGV).
- **An earlier arm's guard.** §31.2.1: a guard runs when its pattern matches, whether or not the arm is then selected, and it may invalidate (`F(ref s)`). A later arm's entry drops what an earlier arm's *guard* invalidates; an earlier arm's *body* never runs before a later arm is tried, so its stores do not reach the later arm. The compiler shrinks by the whole earlier arm's exit, which is a subset of this and therefore sound.
- **The `finally`.** It runs after normal completion, after any arm and on the propagating path, so its entry is the entry facts less everything the block or any arm's guard or body invalidates, and nothing any of them establishes. Its own statements then establish and invalidate as straight-line code.
- **After the statement.** Reached only through normal completion of the block or an arm's exit (an uncaught raise leaves the statement), each through the `finally`. The every-path meet of those exits, then the `finally`'s effect. With no arm, the block's final facts survive — `try s := Fetch(); finally Log(); end; s.Length` is admitted.
- **`return`, `break`, `continue` inside the block or an arm** carry their own path's facts out, as elsewhere; they run the `finally` first (§31.2), whose entry state is the conservative one above, and the `finally`'s own statements apply to the facts a `break` or `continue` carries to its target, exactly as they do at the point after the statement (critique: the first draft's "carries its own path's facts out" skipped the `finally`, which can invalidate on that path).
- **`await` inside the block** is already an invalidating form for a mutably captured local (§7.4) and a `readonly`-field fact survives it; nothing here changes either.
- **The `readonly`-field set** has used this model since v0.214; the paragraph states one model for every carrier, which is also §7.4's posture ("one analysis for null and type facts", v0.339).
- **Type facts** (§12.7, §7.4 v0.339) flow like null facts and take the same model: a `case` arm's or exiting `is not T`'s type fact established inside a protected block does not reach an arm.
- **Nested `try`.** Each statement is analysed by the same rule with its own entry state; an inner arm's exit joins the inner statement's after-point, which is a point of the outer block.
- **Kernel contact:** none (§28/§30.5 untouched). **Body-locality:** local — no cross-body information.

---

## 7. Declined alternatives (rows for `DECLINED.md`, `declined-at-proposal`)

1. **Option B, a store sequence with no intervening raising operation keeps its final fact for arm entry.** Reason: it needs a raise classification of every expression form, which every future construct must join and a reader must learn, for the shape `s := null; s := "y";`, which a program writes as one store; zero in-tree programs. Reopening bar: a measured corpus of programs that write a non-raising store sequence inside a protected block and need its fact in an arm.
2. **Option C, arm selection by exception type, and with it the fully precise reading the v0.339 text implied.** Reason: a call's raise set is undeclared ([§31.1](#sec-the-throw-operand) rules out throws clauses), so the refinement reaches only written `throw`s, and a per-arm state makes what compiles depend on which arm is read; zero in-tree programs. Reopening bar: a declared raise surface on calls.

---

## 8. The normative edits (`scratch/v0340/B3.json`, ids `B3-6707-*`), changelog, migration, trackers

**Spec §7.4.** ▲ A new paragraph, *Narrowing across `try`*, before *Narrowing inside a lambda body*: the analysis models neither throw points nor arm selection; an arm and the `finally` begin with the entry facts less every fact the block invalidates at any point, no block establishment reaches them, a later arm's entry drops what an earlier arm's guard invalidates; the point after the statement is the every-path meet of the block's normal completion and every arm's exit, through the `finally`'s own statements; the remedy is a re-test or re-assignment at the use. **Spec §31.2.** ▲ One sentence after the `finally`-establishment paragraph pointing at §7.4's model. **Rationale §6.1.** ▲ One paragraph (v0.340) recording the decision and the two declines. **Code-generation reference §7.8.** ▲ One *Looks right, is rejected* pair (a `finally` reading a local the block assigned). **Proposals.** This row in `README.md` (*Accepted*); two `declined-at-proposal` rows.

**Changelog (draft; lane B3's notes carry the house-style bullet).** Design change, proposal 0198; #6707 (with #6673). Kernel: no contact. Body-locality: local.

**Migration.** *Newly rejected:* a read inside a `catch` arm, a `finally`, or after the `try`, of a fact the protected block established, or re-established after invalidating it (the four #6673 probes; zero in-tree programs). *Newly admitted:* nothing. *Doc-only:* the compiler matches on staging (the #6673 fix) for locals and since v0.214 for `readonly` fields; the paragraph states the model. *Spec-ahead-of-impl:* none once the #6673 fix lands; until then the local set's entry ∩ after-body intersection (`src/sema/ExprTypecheck.cpp:25449–25600`) is the #6673 bug.

**Compiler tracker (filed at landing).** Confirm the #6673 fix matches the paragraph in its three legs — arm entry (less everything invalidated anywhere in the block, not entry ∩ after-body), `finally` entry (less what any arm invalidates too), after-`try` (block's normal completion ∩ every arm exit, then the `finally`) — for the local null set and the type-fact set as the field set already does (`fieldFactDropEpoch_`); write the three rule-corpus cases §5 names. The lit pin `readonly_field_narrowing_2116__catch_interior_drop_rejected.bzl` stays.

---

## 9. Self-critique (adversarial pass)

- **"The text already decides it: 'every path' is the run-time path set, and the compiler is simply behind."** The run-time reading admits programs whose only correct implementation is a throw-point analysis the language has never had and that reading B shows to be a per-construct classification; stating the model the compiler uses narrows what the text admits, so it is a design change under process §2, and it takes this proposal rather than a ruling.
- **"Option A rejects a program that cannot fail (`c03`)."** It does, at zero measured sites, and the repair is a one-line re-test. A rule that admits it needs the classification of §2, which every future expression form must join; the owner's direction prefers the narrower feature.
- **"Does A reject anything a `finally`-only `try` does today?"** No: with no arm, the after-point is the block's normal completion through the `finally`; only a read *inside* the `finally` of a block-established fact is rejected, and the field set has rejected that since v0.214.
- **"A later arm's entry shrunk by an earlier arm's guard — is that stated anywhere?"** §31.2.1 says a guard is evaluated when its pattern matches and the arm is skipped when it returns `false`; so a guard can run before a later arm is tried. The paragraph states the consequence; the compiler is more conservative (whole-arm exit) and therefore sound under it.
- **"Does the paragraph contradict the `const` clause?"** No: a `const` local's fact is never invalidated, so it survives the block, the arms and the `finally` under this paragraph too.
- **Measurement limits.** The scan is regex-based over raw source; a gtest program assembled from helpers is seen only as text. Every hit was read by hand. The four rejected programs are quoted from #6707, not re-run (no compiler in this container).
- **Shared paragraphs.** Lane B2 (#6570, #6575, #6576, #6584, #6581) edits §22.6, §12.1, §27.1.1, §8.3, §19.3 and §10.5; lane B1 edits §28; lane E edits stdlib §2.18, rationale §11/§12/§5.3 and the code-generation reference §3. The anchors here are §7.4's lambda-paragraph head, §31.2's `finally`-establishment sentence, rationale §6.1's closing sentence and the code-generation reference §7.8's lambda pair — none of which another lane quotes.
