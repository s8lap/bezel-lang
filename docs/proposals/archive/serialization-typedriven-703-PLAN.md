> **Status:** APPROVED migration plan for the NEXT autonomous session (owner directed: retire keyword-marked serialization for type-driven Shared/Weak ser/des, #703). Drafted by workflow wf_5b01d510-057 (spec-scope + compiler-breakdown + migration/tier5-fate + adversarial-readiness + Opus), verified against current main. Companion: serialization-typedriven-703-proposal.md (design note + Appendix A co-owned-return + Appendix B owned-aliased-wire). **Verdict: READY-WITH-PRECONDITIONS** — 4 owner decisions (P2/P3/P4/P6, all with non-breaking recommendations) + a §5.22 rationale working-out gate before any spec text.

---

## 0. Owner decisions — recorded 2026-06-14 (CONFIRMED)

- **P2 = REMOVE the keywords outright** (not keep-as-assertion). Cascading effects now in scope:
  1. **Grammar edit E4 is REQUIRED** — drop `SerializedClassClause` from `ClassDecl`, remove `tree/graph/cyclic/polymorphic` from the contextual-keyword list (Appendix A).
  2. **A mass test-source migration PR is required and must be sequenced BEFORE the parser/sema removal** — the 190 keyword lines across 15 files (tier2/3/4, json_550, serialized_class_clause, etc.) drop the keyword and rely on field-type inference. tree-shaped (all-owned) and graph-shaped (owned+borrow back-edge) tests should infer correctly; verify each. The `serialized cyclic` (borrow-cycle) positive tests RETIRE (the unconstructable case) — cycles re-expressed via `Shared`/`Weak`.
  3. **P6 is now LIVE** (it does NOT dissolve): the `polymorphic` registry loses its syntactic home under `SerializedClassClause`. **Recommendation: rehome it to a class-body declaration** (e.g. a `polymorphic` member-level clause listing the discriminator→type registry); the §5.22 working-out must specify the exact form. This is the one remaining design sub-decision — surface it for owner review in the §5.22 issue.
  4. **assert-and-enforce is not "mitigated" but RETIRED** — with no keyword, there is no user assertion of shape; shape is solely inferred from field types. §5.22 must argue this is acceptable (the field types ARE the contract; a user who wants "no aliasing" expresses it by using `owned`/borrow, not `Shared`). The compiler can no longer catch a "declared tree that is actually a graph" because there is no declaration — but it also cannot diverge.
  - **S9 becomes keyword REMOVAL + inference** (larger than the demote-to-assertion form): the eligibility gate flips from "must have a clause" to "inferred from field types"; the parser/AST keyword removal lands in S9's region (after the test migration), not deferred.
- **P3 = BIND-BACK, no two-phase** (recommended). Tier-5 OBSOLETE confirmed; do not merge feat-550-cyclic's binder.
- **P4 = LOCALLY-DECIDABLE return rule** (recommended). `owned C` for a container holding Shared/Weak; `owned Shared<C>` only when the entry class IS the shared node.

**Remaining gate items for the next session's Phase 0:** the **§5.22 rationale working-out** (now must also justify outright keyword removal + the assert-and-enforce retirement + the polymorphic-rehoming), and **owner confirmation of P6** (the rehomed `polymorphic` syntax). P2/P3/P4 are settled above.

> **⚠️ §0 is AUTHORITATIVE and supersedes the pre-decision recommendation prose below.** §§1–3 were drafted before the owner ruled and still describe the *recommended* path of **keeping the keyword as an optional checked assertion** (a "non-breaking, no test migration" framing). That path was **NOT taken.** Wherever §1–3 say "keep keyword as assertion," "demote gate to optional checked assertion," "non-breaking," "the 190 keyword lines never need touching," or "P6 dissolves if P2 keeps the keyword," read instead per §0:
> - **P2 = REMOVE outright** → grammar edit **E4 is REQUIRED** (drop `SerializedClassClause` + the `tree/graph/cyclic/polymorphic` contextual keywords); **E2 demotes to REMOVES** the keyword.
> - A **mass test-source migration PR is REQUIRED**, sequenced **before** the parser/sema removal (190 keyword lines / 15 files → field-type inference; `serialized cyclic` positive tests retire).
> - **assert-and-enforce is RETIRED**, not mitigated (no keyword = no user shape-assertion; field types are the contract).
> - **P6 is LIVE** (does not dissolve): rehome `polymorphic` to a class-body declaration — the one open sub-decision for owner review in the §5.22 issue.
> - **S9 = keyword REMOVAL + inference**, not demotion.
>
> The next session must reconcile §1–3 prose against §0 as part of the §5.22 working-out.

---

# THE PLAN — Retire keyword-marked serialization for type-driven Shared<T>/Weak<T> ser/des (#703)

**Scope:** Replace the `serialized tree | graph | cyclic` class-header keywords with wire-form inference from field types: owned class fields → inline tree edges; `Shared<T>` → identity-participant ($id/$ref keyed on `Get()`); `Weak<T>` / plain borrow → back-reference ($ref). Closes #703 (test-honesty + ownership-validity), resolves #550's held tier-5 by obsoleting it. Spec target: amend from v0.141.

---

## 1. Readiness verdict

**READY-WITH-PRECONDITIONS.**

The design is coherent and the three hardest gating questions are *genuinely resolved* against ground truth:

- **No dependency on #689 (sendable Shared) or on `feat-550-cyclic`'s two-phase binder.** Serialization produces external bytes, not cross-domain messages; §28.13 domain-locality is an execution property, not a wire constraint (spec:5736/5755). Cycles close through `Weak<T>` → `$ref`, the ordinary graph-DAG resolve path already in `main`.
- **Double-free on deserialize is structurally impossible.** `bz_shared_release` guards on `cb->value_disposed` before `dispose_value` (bezel_runtime.cpp:3110). An owned-`Shared<T>` return starts refcount at 1; `Clone()` per alias; last handle disposes exactly once.
- **Wire is unchanged.** `BZ_SER_INLINE` (0x01) / `BZ_SER_REF` (0x02) already carry the full vocabulary (bezel_runtime.cpp:8131-8171); the dynamic inline-or-$ref template already exists (LlvmBackend_Serialization.cpp:1561-1604). No wire-version migration story.

**Preconditions that MUST be discharged before any compiler code lands** (in order):

| # | Precondition | Owner-gated? | Resolution path |
|---|---|---|---|
| P1 | **Rationale working-out (new §5.22)** retiring three well-reasoned `serialized tree/graph/cyclic` clauses (each individually reasoned in §5.19, v0.119→v0.124, declared "complete, no undischarged obligation"). | Reasoning-first, then owner-approved | Drafted by next session as the *first* artifact; routed through docs workflow. **CLAUDE.md mandates reasoning-first before retiring live normative text.** |
| P2 | **Keyword fate decision** — remove outright vs keep as optional compiler-checked assertion vs deprecate. | **Owner must rule** (recorded in §5.22) | Recommendation below: **keep as optional compiler-checked assertion** (non-breaking, preserves assert-and-enforce). |
| P3 | **Weak<T> deserialization strategy** — owner-before-borrower construction of the `Shared<T>` owning tree + pointer-table bind-back for `Weak<T>`, vs full two-phase binder. | **Owner must rule** (recorded in §5.22) | Recommendation: **bind-back, no two-phase.** Feasible on current runtime; obsoletes tier-5. |
| P4 | **Recognized-factory return shape** for a `Shared<T>`-rooted graph (`owned Shared<C>` vs `owned C`). | **Owner must rule** (stdlib §2.14 amendment) | Recommendation below: locally-decidable rule (class with `Shared<T>/Weak<T>` fields → `Deserialize` returns `owned C` the container; a class that *is* the `T` inside `Shared<T>` is wrapped by its caller). |
| P5 | **Weak-reachability rule** — static (compile-error if `Weak<T>` target not strongly reachable) vs dynamic (`SerializationException`). | Needs working-out in §5.22 | Recommendation: **static same-type ordering check** in sema (S1) + a dynamic `SerializationException` backstop for the general unreachable-orphan case. |
| P6 | **Polymorphic-registry fate** (the `PolymorphicClause` loses its syntactic home if `SerializedClassClause` is removed). | **Owner must rule** | Recommendation: if P2 keeps the keyword as an assertion, the registry keeps its home and this question dissolves. Otherwise move to a class-body declaration. |

**What is resolved vs still owner-gated:** Resolved — no-new-wire, no #689/tier-5 dependency, double-free impossibility, the `Get()`-keying invariant, tier-5 obsolescence, test-honesty mechanism. Owner-gated — P2, P3, P4, P6 (four decisions, all recordable in the §5.22 working-out and confirmable in one owner review pass). P1 and P5 are reasoning-first artifacts the session produces *for* that review.

The recommendation set below is chosen so that **every precondition resolves non-breaking**: the keyword stays valid as an assertion, the 190 existing keyword test lines never need touching, and the work stages purely additively.

---

## 2. Consolidated resolved decisions

These are settled by the research and confirmed against `main`. Carry these as fixed constraints.

1. **Co-owned return = owned `Shared<T>` for a co-owned entry node.** The deserialized root handle starts at refcount 1 (the caller's vote); further consumers obtain handles via `Clone()` off the tag table. The `value_disposed` guard makes this leak-free and double-free-free.
   - *Open sub-decision (P4):* the *exact* signature. **Recommendation:** use the locally-decidable rule — `A.Deserialize(d)` returns `owned A` (the container) when `A` *has* `Shared<T>/Weak<T>` fields; a class that *is the `T` inside someone's `Shared<T>`* is wrapped to `Shared` by its caller in the recursive deserialize path (S5/S6), not at its own top-level entry. The pure `owned Shared<C>` form is synthesized only when the entry class is itself the shared node. This is the only criterion decidable without whole-program analysis.

2. **No new wire token.** `BZ_SER_INLINE` / `BZ_SER_REF` suffice. Existing serialized bytes round-trip unmodified. The dynamic inline-or-$ref template is reused verbatim.

3. **Minimal field-type-driven wire.** Three statically-distinguishable field categories drive reconstruction (all known at synthesis time from the field type): owned → inline; `Shared<T>` → identity-participant ($id first, $ref alias, reconstruct via `Clone()`); `Weak<T>` → always $ref (reconstruct via `Downgrade()`); plain borrow → $ref (reconstruct via raw pointer, **unchanged**). Borrow-back-edge and Shared/Weak **coexist orthogonally** — a class may mix them per-field.

4. **`Get()`-keying invariant (CRITICAL LANDMINE).** The identity tag table MUST key on `bz_shared_get(handle)` (the underlying value pointer, bezel_runtime.cpp:3023), **not** the handle pointer. Two independently-`Clone()`d handles to the same value have different handle pointers but the same `Get()` value. Keying on the handle pointer silently serializes the body twice and destroys aliasing — **no compile-time or runtime error in non-checked builds.** This is the single must-not-miss codegen constraint.

5. **Tier-5 obsolescence finding.** The held `feat-550-cyclic` two-phase bind-back (1,404 net insertions) is OBSOLETE in this model and **MUST NOT MERGE.** Strong-ownership cycles are unrepresentable: the `Shared<T>` owning graph is always a DAG (Shared cycles are *leaks*, not owning cycles); cycles close only through `Weak<T>` → `$ref` on an already-tagged node. No forward reference, no two-phase. (See §5 for salvage.)

6. **Test-honesty resolution (#703).** The bypass dissolves *structurally*: `Shared<T>`/`Weak<T>` construction is ownership-valid (`Clone()` → `owned Shared<T>`, `Downgrade()` → `owned Weak<T>`, both proper owned values per §28.13). New round-trip tests run the **full pipeline including `checkOwnership`** — currently `codegen_support.hpp runJit` (line ~106) omits it; the new tests use a checked harness or are written ownership-clean. The `serialized cyclic` positive tests that bypass `checkOwnership` (the literal #703 defect) are retired with the keyword's tier-5 role.

---

## 3. Spec / rationale work (dedicated docs workflow)

**All edits below go through the dedicated docs workflow** (per-revision changelog + full mechanical validation), *not* as part of implementation PRs (CLAUDE.md, out-of-scope-for-autonomous list). The session produces the working-out and the proposed diffs; landing them is the docs workflow. Target a single revision bump (suggest v0.142) covering the additive step, with the keyword-gate demotion in the same or a follow-on revision per P2.

**GATE — Rationale working-out first (blocks all spec text):**

- **New rationale §5.22 — "Type-driven class serialization: retiring graph-shape keyword *gates*."** Must argue: (a) why field-type inference supersedes the keyword *gate* (the §5.19 v0.118 placement principle held "graph shape is an intrinsic property of the type's serialization contract" — under the new model the shape is *inferred from field types*, which *are* the type's contract, so the principle is satisfied, not violated); (b) the assert-and-enforce loss and its mitigation (keep the keyword as a *checked assertion* — P2 recommendation — so the compiler still catches a declared-`tree` class holding a `Shared<T>` field); (c) prove `Shared<T>.Get()` is the correct $id/$ref key, sound under the `value_disposed` guard; (d) work out disposal-on-deserialize for the owned-`Shared<T>` return; (e) resolve P5 (Weak-reachability rule) and P6 (polymorphic registry). Cross-reference §5.21 (Shared/Weak, v0.134).
- **Rationale §5.19 closing paragraph.** Add a paragraph recording why "complete, no undischarged obligation" (v0.124) was revisited: `Shared<T>/Weak<T>` (v0.134) made the keyword *gate* redundant — field types already encode the graph shape the keywords asserted, making the assertion both duplicate and *divergable*. Narrative addition, not mechanical. Cross-reference §5.22.

**Ordered spec/stdlib edits (each cites the dependency; mechanical = no further working-out once §5.22 approved):**

| Edit | Location | Change | Dep | Kind |
|---|---|---|---|---|
| E1 | Spec §9.8:997 | Admit `Shared<T>` (eligible iff `T` eligible ∧ `T` is class) and `Weak<T>` (same) to the closed component-type set, recursively. **Additive — does not yet touch the keyword.** | §5.22 | Mechanical |
| E2 | Spec §9.8:1001-1076 | Define the field-type → wire-role inference mapping; specify $id assignment on first `Shared<T>` encounter (keyed on `Get()`), $ref on alias; `Weak<T>` always $ref; demote the keyword from *gate* to *optional checked assertion* (P2). Self-contained-constructor rule survives for `Weak<T>` cycle-forming fields. | E1, §5.22 | **Load-bearing, not mechanical** |
| E3 | Spec §9.8 (new) | Weak-ordering / strong-reachability rule (P5): static same-type ordering check + dynamic `SerializationException` backstop for general unreachable orphans. | E2 | Needs working-out (in §5.22) |
| E4 | Appendix A grammar:7387-7447 | Per P2: if keyword retained-as-assertion, **no grammar change** (cleanest). If removed: drop `SerializedClassClause` from `ClassDecl`, drop `tree/graph/cyclic/polymorphic` from the contextual-keyword list (:8063/8071/8088), rehome `PolymorphicClause` (P6). | E2, P2, P6 | Mechanical once P2/P6 fixed |
| E5 | Appendix C §C.8:8227-8235 | Revise the five synthesis invariants: eligibility entry criterion, retire the weaker-shape-assertion check (or keep as assertion per P2), keep self-contained-ctor for `Weak<T>` cycles, polymorphic-registry invariant per P6. | E1-E4 | Mechanical |
| E6 | Stdlib §2.14:548-553 | Amend recognized-factory rule to admit `owned Shared<C>` as a valid `Deserialize` return alongside `owned C`, per the P4 criterion. | E1, §5.22 | Mechanical |
| E7 | Stdlib §2.14:704 | Update the $id/$ref JSON-mapping citation: remove the `serialized graph/cyclic` keyword references, cite the new type-driven rule. Interface decls (:466-510) **unchanged**. | E2 | Mechanical |

**Is a new rationale working-out needed beyond Appendices A/B of the proposal?** Yes — §5.22 is mandatory (it retires live, individually-reasoned normative text; CLAUDE.md forbids silent reversal). The proposal's Appendices A/B are *implementation* analysis; they do not substitute for the rationale's design working-out. §5.22 + the §5.19 closing paragraph are the two narrative artifacts; E1-E7 are the resulting normative edits.

---

## 4. Compiler work — the slice roadmap (S1..S9)

Each slice = one small reviewable PR that leaves the suite green. Order is **S1 → S9**; only S4 and the nullable-guard part of S8 touch a non-additive path. All file:line citations are against current `main` and were carried from the source-reading research.

| Slice | What | Reuse vs new | Test (through `checkOwnership` where noted) |
|---|---|---|---|
| **S1 — Sema eligibility** (TypeResolution_Checks.cpp:2071-2144, 2284-2298) | In `isSerializationEligibleType()`, after the Class branch (:2138), admit builtin Named `Shared`/`Weak` (declaration==null, `name->parts[0]`), recurse on `genericArgs[0]`. Stop rejecting them as ineligible back-edges. Add the **Weak-ordering diagnostic** (P5 static part): every `Weak<T>` field's `T` must match a *preceding* `Shared<T>` field's `T` in declaration order. | Reuse: subtype BFS, record-nested recursion. New: the Weak-ordering walk. No AST/parser change. | Sema tests: owned `Shared<T>` + `Weak<T>` back-field passes; `Weak<T>` before `Shared<T>` rejected; `Shared<NonSerializableT>` rejected via recursive `T`. |
| **S2 — Component classification** (LlvmBackend_Serialization.cpp:97-188) | Add `isSharedParticipant` / `isWeakRef` to `struct ClassComponent`. In `collectClassComponents`, classify via `builtinTypeName()` (impl.hpp:4743): `Shared` → `isSharedParticipant=true, isOwned=false`; `Weak` → `isWeakRef=true, isBackEdge=false`. Plain-borrow `isBackEdge` path **unchanged**. | Reuse: `builtinTypeName()`. New: two bool fields + branch. | Codegen classification check via emitted IR shape / controlled byte sequence. |
| **S3 — Shared serialize** (LlvmBackend_Serialization.cpp:1558-1604) | Reuse the inline-or-ref template verbatim; the **only** delta is the key: load handle, call `bz_shared_get(handle)` → underlying ptr, key `get_tag`/`assign_tag` on **that** (the §2 landmine). Inline body = `emitSerializeField(underlyingPtr, T, ser)`. `Weak<T>` serialize: `bz_weak_try_upgrade` → extract `Get()` → write `BZ_SER_REF`+tag → release temp; on upgrade-fail write null marker (analog of :1479-1493). | Reuse: inline-or-ref template (:1561-1604), `BZ_SER_REF` path (:1497-1522). New: `bz_shared_get` key call. | **Through `checkOwnership`:** two classes, one with `Shared<Node>`, one owning it; serialize; assert exactly one `BZ_SER_INLINE`+`BZ_SER_REF` pair. |
| **S4 — Deserialize handle-table ABI** (bezel_runtime.cpp:9369, .h:1348-1356) | Add `void** tag_handles; int8_t* tag_is_shared` to `bz_binder_t` (approach (a), avoids reinterpreting existing raw-`T*` `tag_ptrs`). New ABI: `bz_binder_graph_register_shared` / `bz_binder_graph_resolve_shared`. `bz_binder_free` calls `bz_shared_release` on every shared slot (registration ref). Grow both arrays in `ensure_slot`. | Pure runtime ABI; **no codegen dependency.** Existing raw-`T*` path untouched. | C-level unit test: register mock Shared handle → resolve → `Clone` increments → `bz_binder_free` decrements to 0 → dispose runs once. |
| **S5 — Deserialize re-bind** (LlvmBackend_Serialization.cpp:1862-1944) | `Shared<T>` node: `emitClassDeserialize` → raw ptr → `bz_shared_new(rawPtr, disposerThunk)` → `register_shared(tagId, handle)`; alias: `resolve_shared` + `bz_shared_clone`. `Weak<T>` $ref: `resolve_shared` → `bz_shared_downgrade` → store. Plain borrow $ref → raw ptr (**unchanged**). | Reuse: `elementDisposerThunk`/`sharedReleaseThunk_` (impl.hpp:742), `bz_shared_new` lowering (Exprs.cpp:5886). | **Through `checkOwnership`:** graph with one `Shared<Node>` owner + one `Shared<Node>` aliaser; deserialize; assert both `Get()` return the same address + correct fields. |
| **S6 — Co-owned return** (LlvmBackend_Serialization.cpp:2183, 2243-2258) | Add `bool returnAsShared` to `emitClassDeserialize`. Top-level entry (`tryLowerSerializationCall`) wraps `bz_shared_new` and returns the handle when the type-driven shape infers a Shared root (per P4 criterion); recursive child calls pass `returnAsShared=false` (they need the raw ptr to wrap themselves). Container-return rule: class *holding* `Shared<T>` returns `owned C`. | Reuse: existing return path. New: the bool + wrap. | Sema-level: `C.Deserialize(d)` with `Shared<Node>` field returns the P4-correct type; S5 covers runtime. |
| **S7 — Exception cleanup** (LlvmBackend_Serialization.cpp:1771-1808, 2008-2028) | In truncated-input and missing-field cleanup: when `isSharedParticipant`, `bz_shared_release(slot)` instead of `emitDisposeAndReclaim`; when `isWeakRef`, `bz_weak_release(slot)`; raw-ptr path unchanged. | Reuse: `declareC('bz_shared_release'/'bz_weak_release')` (Calls.cpp:8164/8179). | **LeakSanitizer** (pattern of `runUnderLeakSanitizer550t3`): deserialize truncated input from a `Shared<T>` class; assert no control-block leak. |
| **S8 — Runtime-rep glue** (LlvmBackend_Calls.cpp:3892-3937, .h:1136-1162; Layout.cpp:438) | `bz_shared_get/clone/downgrade`, `bz_weak_try_upgrade/release` already declared & lowered — used here via `declareC` from the serialization emitter; **no new runtime entry points.** Nullable `Shared<T>?` → `ptrTy()` (Layout.cpp:438), so the existing nullable-ptr branch in `emitSerializeField` (:316-340) already writes the null marker — no new code. `Downgrade()` on null `Weak` handle: guard with `ICmpNE null`. | Reuse: all of the above. | Tests: null `Shared<T>?` → null marker; non-null → `BZ_SER_INLINE`+body. |
| **S9 — Keyword-inference / retirement (integration)** (ast/Node.hpp:266-269; TypeResolution_Checks.cpp:2257-2282; Serialization.cpp:1355-1359, 1640-1642) | `isGraphMode` becomes derived: `(td->serializedClass && (shape==graph||cyclic)) || hasAnySharedParticipant(comps)`. The eligibility gate at :2258 changes from "must have a clause" to "error only if no clause **and** no Shared/Weak fields **and** has ambiguous plain-borrow back-edges". Per P2, the keyword is demoted to **optional checked assertion** (compile error if a declared shape contradicts the inferred field-type shape). **Parser/AST keyword removal DEFERRED** to a later cleanup PR so all 190 existing test lines keep compiling. | Reuse: existing graph-mode path. New: the derived predicate + the contradiction check. | **The main integration test, through `checkOwnership`:** a class with `Shared<T>`+`Weak<T>` fields and **no clause**, declaring `ISerializable`/`IDeserializable`, passes eligibility and round-trips correctly. |

**Reuse vs new at a glance:** S1, S2, S3, S5, S6, S7, S8 are purely additive — no existing path breaks; the existing tier-3 `serialized graph` keyword path is untouched until S9. S4 is new runtime ABI alongside the unchanged raw-`T*` path. The existing `serialization_tier3_graph_test.cpp` stays valid throughout. **S9 is the first slice where the new no-clause path must pass `checkOwnership` cleanly** — which it can, because Shared/Weak programs are ownership-valid by construction.

**Test strategy (the #703 fix in practice):** every new round-trip test is written in **ownership-valid Bezel** (`Shared`/`Weak` fields, `Clone()`/`Downgrade()`) and run through a harness that calls `checkOwnership`. This is now *possible* — the old `serialized cyclic` positive tests could not be expressed in ownership-valid Bezel at all (#703), which is exactly why they bypassed the checker. Extend `runJit` to call `checkOwnership`, or baseline new tests on the ASan native harness that already calls it (tier2:116, tier3:82).

---

## 5. Migration & the held-tier-5 / #550 / #703 resolution

**Keyword surface migration:** 190 lines across 15 internal test files (tier2:87, tier3:29, tier4:19, json_550:16, plus `serialized_class_clause_test.cpp`, `serialization_668_669`, `synthesis_550`/`_sema`, `e2e_kitchen_sink`) + spec §9.8 + stdlib §2.14 examples + conformance cases (spec_0021/0284, stdlib_0020-0023). **Zero external users** (CLAUDE.md: no users, compiler doesn't exist yet). Under the **P2 recommendation (keep keyword as checked assertion)**, the test Bezel source strings need **not** be touched — they keep compiling and the assertion is honored. This is the decisive reason to recommend retain-as-assertion over hard-remove: hard-remove makes all 190 lines compile-errors on day one (parser :1442 / sema :2258), a slice-1 execution cliff with no behavioral payoff for a pre-user language.

**Does borrow-back-edge coexist?** Yes — orthogonally and per-field. A class with only owned+borrow fields uses the existing tree/DAG path; a class with Shared/Weak uses the new type-driven path; a mixed class uses both per field. Same `$id/$ref` wire; different reconstruction (borrow→raw ptr, `Weak`→`Downgrade`, alias-`Shared`→`Clone`), all statically distinguishable from the receiving field type. Confirm explicitly in S9's tests that a `serialized graph` class with owned `T` + non-owned `T?` back-edge still round-trips.

**Tier-5: OBSOLETE — do not merge `feat-550-cyclic`'s two-phase codegen.** The two-phase bind-back (Serialization.cpp +534, Exprs.cpp +33, runtime +121, impl.hpp +31, plus `serialization_tier5_cyclic_test.cpp` 435 lines) has no role: Shared/Weak cycles always close through `Weak<T>` → `$ref` on an already-tagged node, the existing graph-DAG resolve path (`bz_binder_graph_resolve_tag`, :1617-1651). No forward reference arises (owning-spanning-tree post-order constructs the `Shared` target before any `Weak` observer). Leave it on the held branch.

**Salvage three non-cyclic fixes from `feat-550-cyclic` as standalone PRs against `main`** (independent of this milestone; each has its own spec citation):
1. The serialize null-check fix in the `json_550` diff (the DAG-raises-on-cycle contract split).
2. The self-contained-constructor sema checker (~260 lines: `cyclicCtorViolation`/`cyclicStmtsViolation`) — a standalone §9.8 compile-time correctness check; useful regardless, and **needed for the retained `serialized cyclic` assertion case** under P2.
3. The transitive base-constructor-chain scope (§9.8:1037-1044).

**Backward-compat:** wire unchanged — existing serialized bytes round-trip without migration. No wire-version story.

**Non-breaking staging:** Step 1 (E1 + S1-S8) is **purely additive** — admit Shared/Weak to the eligible set; classes with these fields auto-enter graph mode; existing owned+borrow tests unaffected. Step 2 (E2 keyword demotion + S9) demotes the gate to an assertion in a separate revision. This preserves the v0.119-v0.124 per-revision "purely additive; classes without it are unaffected" discipline.

**#703 / #550 closure:** #703 closes when the test-honesty defect dissolves structurally (S5/S9 round-trips run through `checkOwnership` on ownership-valid Shared/Weak programs) and the bypassing `serialized cyclic` positive tests are retired. #550 (synthesize Serialize/Deserialize) closes when S1-S9 land the working type-driven synthesis — the type-driven path *is* the §9.8 synthesis #550 asks for, now expressible in ownership-valid Bezel.

---

## 6. Risks & mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| **`Get()`-keying landmine** — keying the tag table on the handle ptr (current `fieldVal` from the struct GEP, Serialization.cpp:1567) silently serializes clones twice; no compile/runtime error in non-checked builds. | **Critical, silent** | S3 mandates `bz_shared_get(handle)` as the key; S3's first test asserts exactly one `INLINE`+`REF` pair for an aliased clone — a failure here flags the landmine immediately. Call out in §5.22(c) and the S3 PR description. |
| **Disposal-order semantic change** — owned fields dispose in reverse declaration order at owner destruction (§28.4/spec:5025); a `Shared<T>` field's value disposes when the *last handle* releases, anywhere in the program (refcount dynamics). | Medium, semantic | No current test depends on disposal order (grep confirms). **Document explicitly in the §5.22 working-out and E2** as an intended behavioral difference from the keyword model. Not a regression; a contract change. |
| **Handle-table ABI** — `tag_ptrs` is a flat O(n) linear scan (runtime:8102); a Shared-heavy graph could mix `Get()`-value and handle keys. | Low (correctness), latent (perf) | S4 adds a *parallel* `tag_handles`/`tag_is_shared` rather than reinterpreting the raw-`T*` array — no aliasing of the two keyings. O(n) scan is a latent perf issue only; CLAUDE.md defers perf until functional completeness. |
| **Recognized-factory asymmetry** — `owned Shared<C>` deviates from "return type is the declaring type"; sema recognizer (ExprTypecheck_Calls.cpp:5432) matches `owned C`. | Medium | E6 amends stdlib §2.14 *before* S6 changes the recognizer; P4's locally-decidable criterion keeps the common case `owned C` (only the true shared-node entry returns `owned Shared<C>`), minimizing the recognizer change. |
| **Domain-locality of the returned handle** — `Shared<T>` is domain-local for non-synchronized `T` (§28.13:5755). | Low (resolved) | Serialization yields external bytes, not a cross-domain message; the deserialized handle is created in the deserializing domain; the caller distributes via normal `Clone()`. State explicitly in E1/E2 that Shared-eligibility does not imply cross-domain transfer of the result. |
| **Weak-orphan** — a `Weak<T>` target reachable *only* by `Weak` is disposed at table teardown → dangling `Weak` immediately on the deserialized graph. | Medium | P5: static same-type ordering check in S1 + a dynamic `SerializationException` backstop for the general unreachable case; specified in E3 before the feature is declared sound. |
| **JIT harness skips `checkOwnership`** — a new test could "pass" because the checker is skipped yet be ownership-invalid. | Medium (test-validity) | Run new Shared/Weak tests through a `checkOwnership`-enabled harness (the #703 structural fix); write test programs ownership-clean. S9's integration test is the gate. |
| **Hard-remove cliff** (if P2 chooses remove-outright) — 190 test lines + parser/sema break on day one. | High (avoidable) | **Recommend P2 = retain-as-assertion**, which avoids the cliff entirely. If owner insists on remove-outright, sequence a mass test-source migration PR *before* the parser change. |

---

## 7. Sequenced session agenda

A concrete ordered checklist for the next autonomous session. Gate-merge each compiler slice (full suite or `BEZEL_TEST_FILTER=serialization` per slice + clean rebuild at consolidation).

**Phase 0 — Precondition decisions (no code).**
1. Draft the **§5.22 rationale working-out** (P1) covering the field-type-supersedes-gate argument, assert-and-enforce mitigation, `Get()`-key soundness, owned-`Shared<T>` disposal, **P5** (Weak-reachability), **P6** (polymorphic registry). Include the recommendations for **P2** (retain-as-assertion), **P3** (bind-back, no two-phase), **P4** (locally-decidable `owned C` / `owned Shared<C>` criterion).
2. **Open an issue** presenting P2/P3/P4/P6 with recommendations and **request owner confirmation** (CLAUDE.md: spec-unclear → stop and open an issue; these retire reasoned normative text). Do not write spec text until confirmed.

**Phase 1 — Rationale + spec via the docs workflow** (after owner confirms).
3. Land §5.22 + the §5.19 closing paragraph through the **docs workflow** (per-revision changelog, mechanical validation). Bump to v0.142.
4. Land **E1** (additive eligibility) — the upstream prerequisite for S1.
5. Land **E2, E3, E6, E7, E5** and **E4** (E4 only if P2 chose remove; otherwise no grammar change), per the dependency order in §3. Cite section+revision in every edit.

**Phase 2 — Compiler slices (each a gated, reviewable PR).**
6. **S1** sema eligibility + Weak-ordering diagnostic → gate.
7. **S2** component classification → gate.
8. **S3** Shared serialize (enforce the `Get()` key; first test asserts single INLINE+REF for an aliased clone) → gate.
9. **S4** deserialize handle-table ABI (C-level test) → gate.
10. **S5** deserialize re-bind (round-trip through `checkOwnership`) → gate.
11. **S6** co-owned return → gate.
12. **S7** exception cleanup (LeakSanitizer) → gate.
13. **S8** runtime-rep glue + nullable guard → gate.
14. **S9** keyword-inference + retention-as-assertion; **main integration test** (no-clause Shared/Weak class round-trips through `checkOwnership`) → gate. Parser/AST keyword removal deferred to a later cleanup PR.

**Phase 3 — Migration & closeout.**
15. Extract the **three salvageable `feat-550-cyclic` fixes** as standalone PRs against `main` (null-check/contract-split; self-contained-ctor checker; transitive base-ctor scope). Do **not** merge the tier-5 two-phase codegen; leave `feat-550-cyclic` held.
16. Retire the `serialized cyclic` positive tests that bypassed `checkOwnership`; confirm the suite is green with all new round-trips honest.
17. **Close #550** (type-driven synthesis lands, expressible in ownership-valid Bezel) and **#703** (test-honesty dissolved structurally). Record the tier-5-obsolete decision and the salvage outcomes in the session memory log.

---

**Bottom line:** READY-WITH-PRECONDITIONS. Six preconditions, four of them single owner decisions confirmable in one review pass, all with non-breaking recommendations. The §5.22 rationale working-out is the gate and the session's first artifact. The roadmap is nine small additive slices with a single critical correctness landmine (`Get()`-keying), tier-5 is obsolete-not-salvage (three independent fixes excepted), the wire is unchanged, and #703's test-honesty defect dissolves structurally because Shared/Weak programs are — at last — ownership-valid by construction.