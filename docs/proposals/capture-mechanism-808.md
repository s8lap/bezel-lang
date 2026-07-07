<!-- Research memo: the principled deferred-callback capture mechanism for #808 (owner-directed research replacing per-API carve-outs). Two independent Fable designs adversarially compared; grounding verified against current main. -->

All grounding claims verified against current main (spec headings, grammar lines, checker symbols, §24/§30.11 text, `scoped` keyword availability). Composing the memo.

> **Status: superseded.** Landed as spec §28.14 (v0.175, `scoped` parameters + storage-bounded capture) with the v0.177 call-bounded-default tightening; tracked residue #1432/#1425/#1427. Retained for the design record.

# Capture mechanism recommendation (#808)

Two independent designs for the deferred-callback capture mechanism were adversarially compared: **A** ("storage-bounded capture", lifetime-through-storage bias) and **B** ("anchored capture", declared-capability bias). Both propose the same surface — a `scoped` modifier on function-typed parameters plus a `scoped` field qualifier — and converge on nearly identical checker machinery. They differ in the *bounding-object rule*, the *trust posture* for quiescence, and the *migration story*. All factual claims below were re-verified against current main (spec v0.156-line numbering, `src/ownership/*`).

## 1. Adversarial soundness analysis

**Attacks on A (all but one blocked; one shared hole found):**

- *Bounding object moved/returned after storing.* Blocked. Caller admission requires the bounding object to resolve to a `using var`/`await using var` local L, and a `using var` cannot be moved out of (§28.3 *Move semantics*, "Cannot move out of `using var`" is a spec-listed diagnostic). The owned-return form requires the call to *be* L's initializer, so an unbound or returned bounding object admits no escape-bound captures.
- *Re-entrant invocation.* A callback running during L's drain that touches L's members is sound only via §28.12 two-phase disposal (logical disposal precedes physical reclamation). **A never cites this; B does.** A's spec text must add it. A callback that re-registers during the drain (`scope.Spawn(...)` inside a worker) cannot smuggle new escape-bound captures — lambda-body locals fail the "declared before L in an enclosing block" test — but *drain completeness* (the drain must loop until the pending set is empty) is part of the quiescence obligation and must be stated normatively for `TaskScope`.
- *Escape via `Shared`.* Fails. Capturing a `Shared<T>` handle copies a refcounted handle (not escape-bound; referent kept alive — sound). Wrapping L itself in `Shared` requires moving L, which `using var` forbids.
- *The owned-field extraction hole (**shared by both designs**, previously unreported).* Both callee rules admit storing/forwarding the callback into a `scoped` field reachable "through owned fields" of the bounding object (A rules ii–iii; B rule (b)). But owned fields can be moved out (§28.8.1, drop-on-overwrite §28.7): move the child object holding the `scoped` field out of the bounding object and the callback outlives its bound. **Required tightening for either design:** v1 must restrict `scoped`-field storage to fields of the bounding object *itself* (drop the transitive owned-field reach), or make "transitively holds `scoped` fields" an infectious, non-extractable type property. The v1 cut costs little: `TaskScope`, TCS, `Subscription` all store directly.

**Attacks on B:**

- *A lying callee.* B's callee confinement is compiler-verified for user code, so a lying callee must lie in the *temporal* dimension (disposal that doesn't join in-flight invocations). This is B's declared residual — but note B's backstop is real and verified: checked disposal mode raises `DisposedAccessException` on any member access through a logically-disposed instance (§28.2, §28.3 borrowed-field bullet, §31.4), converting the use-after-dispose into a deterministic fault in checked builds. **This backstop is a general §28.2 feature, so it backstops A identically — A just fails to claim it.**
- *FFI (**shared hole, neither design states it**).* `scoped` on an extern declaration (§32) has unverifiable callee obligations *and* unverifiable quiescence. v1 must forbid `scoped` on extern imports outright.
- *B's static-anchor rule* ("the call itself, non-retaining") is verified for user code by the confinement walk; only FFI can lie, covered above.
- *B's plain-owned-local anchors* (auto-drop locals as anchors, not just `using var`). The freeze-self rule blocks moving the anchor, but the reverse-declaration-order guarantee B leans on is textually explicit only for the exception path ("implicit cleanup ordering follows reverse declaration order"); the normal-exit interleaving of auto-drop and `using var` disposal rests on thinner text, and a plain-`var` anchor triggers the §28.3 must-use lint at its own implicit drop. Marginal expressiveness for real spec-interpretation risk.

**The quiescence residual is identical in substance in both designs** (compiler-verified escape/confinement; trusted drain). B's *framing* is strictly better: it names the precedented shape (§28.3's borrowed-field rule — "the assignment-site check is structural and decidable; the residual is trust", backstopped by checked mode), rather than A's bare "defective like a non-releasing destructor".

## 2. Checker implementability

Effectively a tie — both designs cite the same, verified, real hooks: `ParameterSymbol` modifier strings (`isRefOutParam`, `Ownership_Checks.cpp:1464`), capture enumeration from `checkClosureEscapesOnReturn` (:1214), source resolution via `borrowSourceForExpr` (:1570), `implicitDischarge` (`OwnershipState.hpp:54`), a decl-index extension of the `collectLocalDepths` walk (`Ownership.cpp:573`, `Ownership_impl.hpp:235`), a `frozenBy` map maintained like `catchBorrows_` (:226), and sink extensions to `checkBorrowStoreEscape` (:2117) / `handleReturn`. Both correctly identify that **no call-argument closure-escape check exists today** (only return/assignment sinks fire — verified), so either design also ships the missing default check; existing-code fallout must be triaged when it lands (A weakness 7 / B weakness 5 — same fact). Neither needs CFG post-dominance. A adds one extra caller-side case (call-is-initializer-of-`using var` for the owned-return form — trivial against `ast/Node.hpp:129-130`); B adds anchor-kind dispatch of similar size. B's callee walk is marginally simpler as specified, but the §1 tightening shrinks A's to the same shape.

## 3. API coverage — the decisive axis

| API | A (storage-bounded) | B (anchored) |
|---|---|---|
| `TaskScope.Spawn` | receiver form; direct | receiver anchor; direct — equivalent |
| TCS (§30.11) | derived instance retained during migration; better `BeginLoad` shape via owned-return | four-condition text retired immediately; raw shape inexpressible until APIs grow anchor objects |
| **Events (§24)** | **unifies as-is**: `Subscribe(scoped handler): owned Subscription` *is* the owned-return form — the verified §24.3/§24.4 signature (`method Subscribe(handler): owned Subscription`, "using var sub := button.Clicked.Subscribe(...)") needs only the modifier | **does not unify**: B has no owned-return anchor kind (receiver / constructed object / static-call only). The receiver anchor resolves to the *event source* — typically a field or borrow, so no anchor, no captures. Reshaping `Subscribe` into a constructor breaks the `IEvent<T>` interface. |
| Future `Timer.Schedule(period, cb)` | static + owned return ⇒ `using var t := Timer.Schedule(...)` works directly | static ⇒ "call itself, non-retaining" — wrong for a retaining registration; must reshape to `new Timer(...)` |

B's factory-shaped gap is structural, and §24 proves it matters *today*: event handlers are exactly the "stored callbacks" this mechanism exists for, and the stdlib's subscribe-side API is already owned-return-shaped. To cover it, B would have to adopt A's owned-return bounding rule — at which point B *is* A. Conversely, B's genuinely good ideas (trust framing, anchor-table unification of §28.3's three admitted forms, immediate §30.11 pointerization) graft onto A without changing A's mechanism. One caveat for A on events: `EventSource.Subscribe` physically stores the handler in the receiver's subscriber list, not in the returned `Subscription`; the stdlib types are certified normatively (disposal of the `Subscription` severs the handler — quiescence by removal, synchronous per §24.6's single-domain serialization), while a *user* type must structure storage per the callee rule. Acceptable, but it must be said in the stdlib doc.

## 4. Syntax and teaching cost

Syntax cost is identical: one contextual modifier in `ParamModifiers`/`FuncParamMods` (grammar :7801/:7850/:7940 verified; `scoped` verified unused). Function-type-identity exact-matching is a shared friction (B weakness 6). Teaching: B's anchor table ("three anchor kinds subsume the three §28.3 escape forms") is the better *story*; A's "declare captures before the scope; they're frozen until it exits" is the better two-line *rule* — and its worked rejections (declaration order, freeze) give mechanical fixes ("swap the declarations", "move the archive after scope end"). Note on example fidelity: B's Bezel examples match the verified surface syntax (`new`, `end Name`); A's use invented brace syntax and must be rewritten before any spec text lands. B, however, proposes §28.13 — **which is taken** (Shared/Weak); A's §28.14 numbering is correct.

## 5. Recommendation

**Adopt Design A's mechanism, amended with four elements of B** — i.e., a hybrid with A as the chassis:

1. A's bounding-object rule *including the owned-return form* (constructor → constructed; owned return → returned, call must initialize the `using var`; else receiver). This is the piece B lacks and events/Timer/`BeginLoad` require.
2. B's trust posture, verbatim in spirit: quiescence for user-authored types is the *same residual-trust shape as §28.3's borrowed-field rule*, backstopped by `DisposedAccessException` in checked disposal mode, UB unchecked (§28.2). Cite §28.12 two-phase disposal for self-capture validity during the drain.
3. B's presentation: the anchor/bounding table subsuming §28.3's three admitted escape forms, rather than appending a fourth.
4. The §1 tightening (v1: `scoped`-field storage only into the bounding object itself, no transitive owned-field reach) and the FFI prohibition (`scoped` illegal on extern declarations).

**Declined, with reasons:**

- *B as chassis* — declined: no owned-return bounding form, so §24 events and every factory-shaped registration API are inexpressible without reshapes that break existing verified stdlib interfaces; fixing that converges to A.
- *B's plain-owned-local anchors* — declined for v1: marginal expressiveness; ordering guarantee rests on exception-path wording; collides with the must-use lint. Revisit only with a spec clarification of normal-exit interleaved disposal order.
- *B's immediate retirement of §30.11's four-condition text* — declined: the raw one-shot registration shape (`BeginRead` in §30.11's own example) is not expressible under `scoped` until anchor-object APIs exist; keep §30.11 as the derived settlement-quiescent instance (per A §5, with the settle-then-touch race recorded in the rationale as the reason the storage-bounded form is primary), retire it in the revision that ships the reshaped stdlib APIs.
- *A's bare trust framing and A's example syntax* — declined in favor of B's; both are text-level fixes.

**Spec sections amended by the winning mechanism:** new **§28.14** (scoped callback parameters — storage-bounded capture; §28.13 is Shared/Weak); **§28.3** (admitted-escape-forms bullet gains the pointer to §28.14); **§28.2** (checked-mode note: post-quiescence invocation faults as `DisposedAccessException`); **§30.11** (re-derived as the settlement-quiescent instance; four conditions kept as legacy until stdlib migration); **§24.3/§24.4** (`Subscribe` handler parameters become `scoped`); **§22.3** and grammar **A.4/A.6** (`ParamModifiers`/`FuncParamMods` gain `scoped`; A.7 field-modifier position); **§27.2** (function-type identity); **§32** (`scoped` forbidden on extern); stdlib doc (normative quiescence contracts for `TaskScope`, TCS classes, `EventSource`/`Subscription`); rationale (decision record for this comparison, the owned-field-extraction tightening, and the TCS race note). Follow-up issues to file on adoption: the missing default call-argument escape check's repo-wide fallout, and the deferred owned-payload `Spawn(owned T, work)` overload.
