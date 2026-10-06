#include "codegen/LlvmBackend_impl.hpp"
#include "sema/GenericParams.hpp"  // fix #3143: the ONE both-sources generic-param walk (§14.1/§12.9/§2.5)
#include "sema/Sendability.hpp"  // #5305: isAggregateChannelElement — §30.14 aggregate channel element

namespace bezel::codegen {

// fix #2858 (§17.6 / §12.1 / §12.3 / §11.2, v0.240): `P++` / `P--` on a
// VIRTUAL, OVERRIDE, ABSTRACT or INTERFACE property.
//
// §17.6, verbatim: "The operand must be an assignable lvalue (a local variable,
// a writable field, an indexer slot, or a property with a setter …)", and "For
// built-in numeric types, `x++` is equivalent to `x := x + 1` (and likewise
// `x--` to `x := x - 1`)". §12.1, verbatim: "A method, property, or indexer is
// non-virtual unless marked `virtual`. To override a virtual member in a
// derived class, mark the matching member `override`."
//
// THE DEFECT this closes: `lowerStmt`'s IncrementStmt arm reached the property
// setter through `lowerPropertySetValue` (statically bound) or, for an
// auto-property, through `lvalueAddr` (a direct store into the DECLARING
// class's backing field) — never through `lowerVirtualPropertySet`, which is
// what the AssignmentStmt arm calls for both `:=` and `OP=`. The READ half went
// through `lowerExpr` and therefore DID dispatch, so the two halves of one
// statement disagreed. MEASURED on dafe6d9 with `B`'s accessors at +100/+1 and
// `D`'s at +200/+2, receiver always a `D`: `P++`, `this.P++` and `b.P++` all
// gave 202 where 203 is correct; an `abstract` read-write property and a
// virtual AUTO-property were DROPPED entirely (0); an interface-typed `q.P++`
// gave 1.
//
// Returns true when it lowered the whole statement. Everything cheap is checked
// BEFORE the getter is lowered, so a decline does not leave a stranded call:
//   * a non-property or an INDEXER target — the indexer spelling is
//     `lowerIndexerIncrDecr`'s (#2848);
//   * `base.P++` — §12.3, verbatim: "Inside a derived member, `base.Name(...)`
//     calls the base class's implementation, bypassing virtual dispatch." The
//     base property shares its vtable slot with the derived override, so
//     dispatching would run the override. Declined HERE rather than relying on
//     lowerVirtualPropertySet's own `base` bail-out, because that one fires
//     only after the getter has already been emitted;
//   * no vtable/itable slot — a NON-VIRTUAL or STATIC property (§12.1:
//     "Fields, constructors, destructors, and static members may not be
//     virtual", so buildVtable never gives one a slot). Both keep their
//     existing static lowering.
bool Emitter::lowerVirtualPropertyIncrDecr(const ast::IncrementStmt& inc) {
    if (!inc.target) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*inc.target));
    if (!ps || ps->isIndexer) return false;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(inc.target.get())) {
        if (!ma->target) return false;
        if (auto* tid =
                dynamic_cast<const ast::IdentifierExpr*>(ma->target.get()))
            if (tid->name == "base") return false;  // §12.3
    } else if (!dynamic_cast<const ast::IdentifierExpr*>(inc.target.get())) {
        return false;  // only `P` and `recv.P` are property lvalues here
    }
    const bool hasSet =
        propSetSlot_.count(ps) || ifacePropertySetIndex_.count(ps);
    const bool hasGet =
        propGetSlot_.count(ps) || ifacePropertyGetIndex_.count(ps);
    if (!hasSet || !hasGet) return false;

    llvm::Value* cur = lowerExpr(*inc.target);   // the dispatched getter
    if (!cur) return false;
    // §17.6's implicit `1`, synthesized at the getter's result type by the ONE
    // definition of that rule (#2888). Gated on the LOWERED type, not a source
    // category: a type with no `1` (a `string` property, say) declines rather
    // than emitting a malformed constant. The `decimal` special case this
    // function used to open-code is gone — emitCompoundOp owns the §6.4 arm
    // (#2883), so `d.P++` and `nullable.P++` take the same call as every other
    // numeric type.
    llvm::Type* t = cur->getType();
    const std::string binOp = inc.op == "++" ? "+" : "-";
    const bool fp = t->isFloatingPointTy();
    llvm::Value* one = emitImplicitOne(t);
    if (!one) return false;
    llvm::Value* res = emitCompoundOp(
        binOp, cur, one, fp,
        isUnsignedTargetExpr(*inc.target),
        isCharTargetExpr(*inc.target));    // §6.6/§5.2 #1961
    if (!res) return false;
    // valueIsPlusOne mirrors the AssignmentStmt compound arm's #2586 call: the
    // operation's RESULT has no source AST node, so the setter cannot recognise
    // it as a producer on its own. It is only ever a fresh +1 for a pointer
    // result, which `one`'s type gate above already excludes here — the flag is
    // kept for symmetry with that call site, not because it can fire.
    return lowerVirtualPropertySet(*inc.target, res, /*valueExpr=*/nullptr,
                                   /*valueIsPlusOne=*/true);
}


// fix #2915 (§4.7 / §8.6 "Boxing of the unit value", v0.240): the unit-RHS
// rescue for `x := ();`.
//
// `()` lowers to LLVM `void`, so a unit-typed expression has NO lowered value
// and `lowerExpr` returns null for it. Every `:=` arm below then took its
// `if (!rhs) return false;` exit — SILENTLY, with no diagnostic at all: measured
// on f069645, `var p: Object := "seed"; p := ();` left `p` holding `"seed"`,
// so the assignment simply did not happen. §4.7 requires a boxed `()` at an
// `Object` location, so supply it here, through the SAME boxForTarget the
// local-decl, argument and return positions use (boxForTarget is a no-op for a
// non-Object/interface target and for every non-unit source, so this rescue
// only ever converts "silently dropped" into "boxed"). A null return still
// means "nothing to store" for every other shape.
//
// fix #3095 (§4.7, v0.252): the same rescue is owed at a `()?` target, for the
// same reason and one step earlier in the type lattice — `u := ();` on a
// `var u: ()?` dropped its store too (MEASURED on 663a68e behind the #2961
// "store dropped" Warning, leaving `u` reading `null`), and §4.7 says `()?`
// has "two values, `null` and `()`", so a dropped store is a WRONG VALUE, not
// a missing no-op. unitValueForTarget supplies the present `{1, 0}`; it is a
// no-op for every non-`()?` target, so boxForTarget's `Object` case below is
// reached exactly as before.
llvm::Value* Emitter::rescueUnitAssignRhs(llvm::Value* rhs,
                                          const ast::AssignmentStmt& as) {
    if (rhs || !as.target || !as.value) return rhs;
    const sema::TypeRef* tgt = ctx_.exprType(*as.target);
    llvm::Value* uv = unitValueForTarget(rhs, tgt, as.value.get());
    if (uv != rhs) return uv;
    return boxForTarget(rhs, tgt, as.value.get());
}


// fix #4646 (§22.16, v0.307): the AUTO-PROPERTY row of §22.16's
// instrumentation set.
//
// §22.16, verbatim:
//
//   "- **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike. (Auto-property assignments
//      inside constructors are backing-field stores, not accessor
//      invocations, so construction does not double-check.)"
//
// #4614(b) emitted that row at `lowerPropertySetValue` — the path a BLOCK-FORM
// setter's inlined body takes — and #4651 made the exported cross-module stub
// the same checked face. Neither reaches an AUTO-property: a same-module
// `c.P := v` on a non-virtual auto-property has no setter body at all, so
// `computedSetterOf` returns null and `lowerPropertySet` declines; the store
// falls through to the generic `lvalueAddr` arm of the AssignmentStmt lowering
// and lands directly in the backing field. MEASURED on this HEAD before this
// fix, with `--checked-contracts`, on the issue's own repro: 42 where §22.16
// requires ContractViolationException (99) — a SILENT UNDER-CHECK, which is
// the direction §22.16's observable-boundary property exists to prevent.
//
// This predicate answers whether an assignment target IS such a store, and
// whose conjunction it evaluates. Every §22.16 and §13.6 question — release
// mode, class kind, `static`, accessor visibility, a non-empty conjunction,
// and the `this`-reentrancy exemption decided on the receiver's SYNTAX — is
// deferred to `instrumentedSetterClass` (#4614(b)), deliberately: the two
// setter shapes must not be able to drift apart, and re-deciding the
// exemption here is exactly how a silent under-check becomes a loud
// OVER-check (`this.P := v` inside an instance member is EXEMPT, and the
// constructor's own `P := v` is the spelling §22.16's parenthetical names).
//
// Declines for a FIELD target (`ctx_.exprSymbol` yields a FieldSymbol, never a
// PropertySymbol — §22.16 makes a public field no instrumentation point at
// all) and for an INDEXER (its own row is reached through `lowerIndexerSet`).
const sema::TypeSymbol* Emitter::instrumentedAutoPropertyStoreClass(
    const ast::Expr& target) const {
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(target));
    if (!ps || ps->isIndexer) return nullptr;
    // fix #4682 (§22.16 / §2.8.2, v0.307): decline for an IMPORTED property,
    // and RETIRE the SEAM-DEBT #4686 marker that stood here. That marker
    // asserted "an imported property never reaches this predicate" as a
    // premise about the ORDER of the sibling arms. The premise is TRUE for
    // the `:=` arm and FALSE for the two arms #4682 adds — MEASURED on this
    // HEAD with a real two-module build (`--import`) of a class carrying
    // `invariant(k > 0);` and a public auto-property `P`, by reading the
    // importer object's undefined symbols (`nm -u`):
    //
    //     c.P := 7   ->  U bz$<Lib>.CX$P$propset   (#4651's checked face)
    //     c.P += 1   ->  no propset reference
    //     c.P++      ->  no propset reference
    //
    // `lowerImportedPropertySet` claims the `:=` store before the generic
    // `lvalueAddr` fall-through, but `lowerImportedPropertyCompound` and
    // `lowerImportedPropertyIncrDecr` both DECLINE for an imported
    // AUTO-property (neither has an accessor body to call), so those two
    // spellings reach `lvalueAddr` and store straight into the backing field.
    //
    // Declining here is a deliberate SCOPE choice, not an oversight. §22.16
    // requires the check for all three spellings; what differs is WHOSE
    // `--checked-contracts` governs it. #4651 settled that for the setter row
    // with a measured 2x2 mode grid — the check lives in the EXPORTER's
    // object, so the exporter's flag governs, exactly as §22.16's
    // public-METHOD row has behaved since #2941. Instrumenting the imported
    // compound/increment HERE would put it in the IMPORTER's object instead,
    // so one property would answer to two different build flags depending on
    // which spelling the caller used. The coherent fix routes the imported
    // compound/increment through the exported checked stub, which lives in
    // the imported-property lowering rather than in these arms; it is
    // recorded as #4705. Until then the imported spellings keep their
    // pre-existing unchecked behaviour instead of acquiring a second,
    // contradictory mode policy.
    //
    // `ctx_.isImported` is the repo's settled spelling of this question
    // (SemaContext.hpp): an imported `PropertySymbol` is minted by
    // `makeImportedSymbol`, which inserts it into `imported_`. Preferred over
    // a `ps->decl == nullptr` test, which conflates "externally defined" with
    // "carries no local AST" (imported-symbol-audit-2026-08-13.md §4).
    if (ctx_.isImported(ps)) return nullptr;
    // `pd` is the OPTIONAL §13.6 accessor-visibility refinement input, not a
    // classification read; `setterInstrumentationClass` documents its
    // null-`pd` arm as "no accessor-level refinement". With the guard above,
    // every property that reaches here carries one.
    //
    // SEAM-DEBT: #4686 ps->decl read — now guarded unreachable-for-imports by
    // the `ctx_.isImported(ps)` decline immediately above, MEASURED with a
    // two-module program (see the `nm -u` table in the block above), rather
    // than relying on the ordering of the sibling `lowerImported*` arms as the
    // retired marker did. Same posture as DefiniteAssignment.cpp's #4030
    // marker. The read stays a class-3 AST-gated read and keeps its marker.
    const auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    return instrumentedSetterClass(target, ps, pd);
}


bool Emitter::lowerStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        // fix #3377 / §28.3: a `using var` is a SCOPE-BOUND consumer — §28.3
        // makes it non-movable ("cannot move out of `using var`; `using var`
        // is scope-bound"), so a deferred-activation frame bound to one
        // provably dies at this scope's end and its fresh receiver can be
        // reclaimed there too. A plain `var` is NOT published: it can be
        // moved/returned, so its receiver keeps the (sound) pre-fix leak.
        {
            auto* savedScopeBoundInit = deferredRecvScopeBoundInit_;
            deferredRecvScopeBoundInit_ =
                ld->isUsing ? ld->initializer.get() : nullptr;
            lowerLocalDecl(ld, ld->initializer.get(), ld->type.get());
            deferredRecvScopeBoundInit_ = savedScopeBoundInit;
        }
        // #823 / §27.5: a local declared INSIDE a loop body is re-initialised
        // every iteration, so a closure capturing it needs a fresh cell per
        // iteration. Record its slot so boxLocalSlot chooses fresh-each-iteration
        // boxing even when nested control flow (an inner loop / if) puts the init
        // store in a different basic block than the capture site (the current-BB
        // store heuristic would otherwise miss it → a stale first-iteration cell).
        if (!loopStack_.empty()) {
            auto lit = locals_.find(ld);
            if (lit != locals_.end())
                loopBodyDeclaredSlots_.insert(lit->second);
        }
        if (diverged_) return true;  // CG40: diverging initializer
        // fix #4566 / §15.3 *Local-to-local assignment* + §28.7 *Moved-from
        // and null* (v0.306): `var c := b;` is the spec's OWN spelling of the
        // move-only local-to-local move ("`var y := x;`: copies for an
        // ordinary value type; moves for a move-only value type"), and it
        // double-freed exactly as the `:=` form did — both slots kept the
        // owned payload and both scope-end active-case passes disposed it.
        // Runs after `lowerLocalDecl` has stored the initializer into the new
        // binding's slot, and after the `diverged_` bail (a diverging
        // initializer never reached the store, so there is no transfer to
        // record). // per §15.3, §28.7, v0.306, #4566
        markOwningChoiceLocalMovedFrom(ld, ld->initializer.get());
        // §15.3 (#747): a move-only owning inline-array local auto-disposes its
        // slots at scope end, independent of `using var` (§28.10 line 2025).
        // When handled, bypass the using/task/unwind chain (none applies to a
        // move-only value-typed local).
        if (registerInlineOwnedArrayDisposal(ld)) return false;
        // §35.4 (v0.200, #2045 stage 3): an OWNING-classified choice local
        // registers its active-case disposal (per-tag switch over the active
        // case's owned payloads). Bypasses the using/task/unwind chain because
        // a choice is a value type (the active-case disposal is the SOLE slot
        // path for its owned payloads; classOfLocal is null, unwindOwnedLocalClass
        // is null — neither catches it). The using-var normal-exit path still
        // runs on top via the separate d.dynamicDispose/etc. branches the
        // registration already covers (using var registers both layers), so
        // exactly-once disposal holds on both the normal and unwind exits.
        if (registerOwningChoiceDisposal(ld)) return false;
        // §29.11 (#943): a plain `var` holding a vtable-backed re-iterable
        // IEnumerable<T>/IOwningEnumerable<T> source (a yield-iterator result, or an
        // array/user-class wrapped into one) is disposed once at scope end — the
        // for-in over it clones a fresh enumerator per loop and leaves the source
        // alive so a second loop replays. Checked before the unwind-owned path
        // (these declaration-less iterator types have no owned-class destructor).
        if (registerIterableSourceDisposal(ld)) return false;
        if (ld->isUsing) registerUsingDisposal(ld);
        // #445 / §30.5: a plain `var` task-typed local is freed at scope end via
        // bz_task_fire_and_forget — the same disposal a `using var` task gets.
        // lowerAwait never frees a named-local task (freeTaskAfter is only set for
        // an anonymous CallExpr operand), so a plain `var t := AsyncMethod();` was
        // silently leaked. Registering the disposal here closes the gap for both
        // the awaited case (`var t := M(); await t;`) and the unawaited case
        // (`var t := M();` with t never awaited). Double-free safety: the await
        // site does NOT free a named local → fire-and-forget at scope end is the
        // single free site; using-var takes the `isUsing` branch above, so this
        // branch never fires for it. (per §30.5 / §28)
        //
        // §28.3 v0.190 SB-01 (#1786 / #1870, "calls mint, storage lends"): register
        // this disposal ONLY for a task local that OWNS the frame — one whose
        // initializer is a mint CALL. A task local that merely BORROWS the frame (a
        // bare-identifier ALIAS, or a task-typed STORAGE read — field/property/
        // indexer) is skipped: its owner already disposes the single `bz_task_t*`,
        // so a second `d.task` here would double-free it (→ UAF that corrupts async
        // continuation state → hang). taskLocalInitializerIsBorrowSource mirrors
        // producerState's Owned-vs-Borrow split so codegen and the ownership pass
        // agree on who holds the disposal obligation. A borrow source falls through
        // to the unwind-owned path below (a no-op for task types), registering no
        // disposal. // per §28.3 v0.190 SB-01 (storage lends)
        //
        // The syntactic shape gate above covers the alias / storage-read borrows,
        // but two exotic CALL shapes are ALSO borrows producerState classifies
        // Borrowed while this gate would treat as mints: a bare-type-parameter-
        // return call at a task instantiation (`First<T>(a): T` at `Task<int>`,
        // #1801) and an owning-wrapper method returning a bare receiver owned-
        // field (the #1493 shape). Re-deriving that from the AST here would drift
        // from producerState, so the ownership pass records its Borrowed verdict
        // for those on the decl (`initializerCallIsBorrow`); consulting it keeps
        // codegen and the ownership pass in agreement — the borrow no longer
        // registers a second `d.task` (a double-free of the real owner's frame).
        // // per §28.3 v0.190 SB-01 (#1872)
        // fix #3096 follow-up (v0.252): registration site TWO of the `d.task`
        // scope-end disposal. The two SB-01 conjuncts that used to be spelled
        // out here now live in taskLocalOwnsFrame, which is also what the
        // `using var` registration arm and chainedReturnOwnedTaskSource ask —
        // one expression, three readers, so the obligation cannot be moved by a
        // reader that disagrees about whether it was ever registered.
        else if (const sema::TypeRef* taskTy = taskTypeOfLocal(ld);
                 taskTy && !disposeStack_.empty() && taskLocalOwnsFrame(ld)) {
            auto it = locals_.find(ld);
            if (it != locals_.end()) {
                Disposal d{it->second, nullptr};
                d.task = true;
                // §30.15 (v0.179, #1524): a plain `var t := channel.Receive();`
                // never awaited is abandoned exactly like a `using var` one —
                // route through the give-back protocol, not ordinary task
                // disposal (see LlvmBackend_Setup.cpp's using-var registration
                // for the full rationale).
                d.channelReceiveTask = isChannelReceiveInitializer(ld);
                // §30.5 / #1258 / #1868: a plain `var` OwningTask<T:class> local
                // (SB01-C1/D2 auto-drop, #1786/#1868) owns its produced result;
                // attach the per-T disposer so it is disposed at scope end, EXACTLY
                // as the `using var` path does (shared helper — no drift). Without
                // this, a never-awaited-across-suspend OwningTask<Cell> leaked its
                // Cell (the pre-existing plain-`var` activation-local leak too).
                attachOwningTaskResultDisposer(d, taskTy);
                disposeStack_.back().push_back(d);
            }
            // Still register the unwind-owned path for any non-task cleanup
            // (task locals have no owned class under unwindOwnedLocalClass — it
            // returns null for task types — so this is a no-op; called for safety).
            registerUnwindOwnedLocal(ld);
        }
        // §28.3 v0.150 (#1008 Part B): a plain `var` owned local the ownership
        // pass flagged for affine auto-drop (Owned at a normal-exit scope end,
        // never discharged on any path) is disposed at scope end on BOTH the
        // normal and unwind edges — the same scope-end disposal a `using var`
        // gets (registerUsingDisposal handles every kind: class, OwningArray /
        // OwningDictionary collections, etc.). The flag is only set for a never-
        // moved owner, so the slot is always live at scope end and the disposal
        // is exactly-once. registerUnwindOwnedLocal (the unwind-only path) does
        // NOT cover collections, hence routing the flagged local through the
        // full using-disposal dispatcher.
        else if (ld->autoDropAtScopeEnd) registerUsingDisposal(ld);
        // fix #1892 / §17.13 / §28.5: a plain `var` owning-COLLECTION local
        // discharged via a `_ := x;` DISCARD (marker-free-position consume).
        // The ownership
        // pass markDisposedByName's it (so autoDropAtScopeEnd is false) and set
        // dischargedByDiscard on the decl. registerUnwindOwnedLocal (the class-
        // only unwind path below) does NOT cover collections, so the container
        // leaked when an operation between acquisition and the discard threw.
        // Route it through the full collection disposal (registerUsingDisposal →
        // bz_array_free/bz_dict_free/bz_llist_free with the element-disposer
        // cascade), which fires on BOTH the normal and unwind edges and is
        // null-safe. The discard codegen (lowerStmt DiscardStmt) disposes the
        // container at the discard point and nulls the slot, so this registration
        // is a no-op on the normal path (null reload) and the sole disposer on the
        // pre-discard-throw exception edge — exactly once, no double-free. The
        // flag is only set for the discard form (not `move`/`.Dispose()`), so this
        // never doubles a transfer whose collection slot codegen does not null.
        else if (ld->dischargedByDiscard &&
                 (collectionTypeOfLocal(ld) || dictTypeOfLocal(ld) ||
                  linkedListTypeOfLocal(ld)))
            registerUsingDisposal(ld);
        // §28.3 K3.4/K7.4 (v0.209, #2080): a plain `var` owned class/
        // interface local consumed by a MARKED ARM TRANSFER (`owned x` as an
        // if-expression branch / expression-`case`/`when` arm result). The
        // marker's lowering NULLS the slot on the selected path, so K7.4's
        // per-path scope-end disposal is exactly a NULL-GUARDED
        // dynamicDispose registered here (both edges): the consumed path
        // reloads null and skips; a not-selected path disposes exactly once.
        // The ownership pass suppressed both the §28.3 discharge error and
        // the affine auto-drop for this local (everDischarged excludes it
        // from autoDropAtScopeEnd), so this is its sole scope-end path.
        else if (ld->dischargedByArmTransfer) {
            auto it = locals_.find(ld);
            if (it != locals_.end() && !disposeStack_.empty()) {
                Disposal d{it->second, nullptr};
                d.dynamicDispose = true;
                disposeStack_.back().push_back(d);
            }
        }
        // fix #5437 (§28.9 / §28.2 / §28.1 / §8.1, v0.314): a plain `var` that
        // RECEIVES an `owned` transfer whose substituted type is `Object`.
        //
        // This is the shape the #1857 row pins — `class Factory<T> method
        // Make(): owned T` instantiated at `T = Object`, whose body allocates
        // `var x: Object := new Object(); return x;`. The `owned` marker ERSES
        // at the substitution (§28.1: `owned` is inadmissible over a primitive,
        // and §28.2's "`Dispose()` is not on `Object`" leaves the instance with
        // no user-visible obligation), so no earlier arm claims the receiver:
        // `autoDropAtScopeEnd` is false, so the branch above never runs, and the
        // instance escapes with nobody to free it (MEASURED: 8 bytes per call).
        //
        // It is still an OWNED TRANSFER, and §28.9's trivial-dispose free rides
        // on the INSTANCE, not on the marker: K1.6 spells the two apart —
        // "any undischarged trivial-dispose value is freed silently by the
        // compiler at its owner's release point — no warning (the ceremony
        // exemption), no user code". So the receiver frees it SILENTLY, which
        // is why no §28.3 auto-drop WARNING is warranted or emitted (the #1857
        // row `object_substitution_erased_no_auto_drop` asserts its absence, and
        // that assertion is unchanged by this).
        //
        // GATED ON AN `owned`-RETURNING CALL, which is the whole discriminator:
        // §28.10 gives a BORROW return no disposal obligation, and freeing a
        // borrowed `Object` here would be a use-after-free, not a leak fix.
        // MEASURED both twins: `method Make(): owned T` at `T = Object` needs
        // this arm, while `method Make(): Object` (same body, marker dropped)
        // must NOT take it — that program compiles today and borrows.
        //
        // A bare-identifier initializer (`var c: Object := a;`) is deliberately
        // NOT covered: it is an ALIAS, and the source local is the owner
        // (it carries the auto-drop). Registering both would double-free.
        else if (const auto* ce =
                     dynamic_cast<const ast::CallExpr*>(
                         ld->initializer.get())) {
            const sema::TypeRef* rt = ctx_.exprType(*ld->initializer);
            const sema::TypeRef* dty = ld->type ? ctx_.resolvedType(*ld->type)
                                                 : nullptr;
            const sema::TypeRef* eff = dty ? dty : rt;
            bool ownedObjectTransfer = false;
            if (eff && eff->kind == sema::TypeRef::Kind::Primitive &&
                eff->primitive == sema::PrimitiveType::Object && ce->callee) {
                if (auto* ms = dynamic_cast<const sema::MethodSymbol*>(
                        ctx_.exprSymbol(*ce->callee))) {
                    ownedObjectTransfer = sema::methodReturnsOwned(*ms, ctx_);
                }
            }
            if (ownedObjectTransfer) {
                registerUsingDisposal(ld);
                return false;
            }
            registerUnwindOwnedLocal(ld);
        }
        // §28.3: a plain `var` owned local NOT flagged for auto-drop (it is
        // discharged on some path) is auto-disposed only on the exception-
        // unwind path (an `unwindOnly` Disposal); its normal-exit discharge is
        // sema-enforced.
        else registerUnwindOwnedLocal(ld);
        return false;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        lowerLocalDecl(lc, lc->value.get(), lc->type.get());
        // #823 / §27.5: same loop-body fresh-capture handling as LocalDecl.
        if (!loopStack_.empty()) {
            auto lit = locals_.find(lc);
            if (lit != locals_.end())
                loopBodyDeclaredSlots_.insert(lit->second);
        }
        if (diverged_) return true;  // CG40: diverging initializer
        // fix #3640 (§21.1 / §29.11 / §27.5 / §29.2): a §21.1 runtime `const`
        // bound to a vtable-backed re-iterable IEnumerable<T>/IOwningEnumerable<T>
        // source (an array/user-class WRAPPED into one — `wrapArrayAsIEnumerable`
        // et al. mint a fresh 32-B `{vtable, arr, idx, esz}` borrowing wrapper
        // frame at the decl site) is BINDING a borrow, which §21.1 EXPLICITLY
        // admits. The wrapper frame's scope-end disposal was previously routed
        // ONLY through the LocalDecl arm's `registerIterableSourceDisposal` call,
        // so the const arm silently dropped the registration → measured leak
        // of 6368 B / 199 allocs (32 B per wrapper frame, #3392's own
        // measurement) for `const v: IEnumerable<int> := arr;` repeated across a
        // loop. `registerIterableSourceDisposal` was widened from
        // `const ast::LocalDecl*` to `const ast::Node*` (#3640) so it accepts
        // either decl kind; the LocalConstDecl arm now mirrors the LocalDecl
        // arm's call here so the SAME chain runs for both. Mirrored exactly to
        // avoid the §6b drift that has already produced seven instances of this
        // defect family (#2802/#3375/#3402/#3416/#3417/#3427/#3614).
        // per #3640, #3402, #3384, #3419, #942
        if (registerIterableSourceDisposal(lc)) return false;
        // A `const` cannot bind an OWNED production (an owning choice, a
        // move-only inline array, a task frame — §21.1); the
        // registerInlineOwnedArrayDisposal / registerOwningChoiceDisposal /
        // task-disposal chain is therefore legitimately unreachable from the
        // const arm and is intentionally NOT mirrored (per §21.1 admission
        // boundary).
        return diverged_;  // CG40
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        // fix #5017 (§5.1 "Nullable conversions", v0.307): the lifted
        // `S? → T?` conversion at an assignment into a nullable-VALUE slot.
        // `rhs` is a `{i8 hasValue; S}` struct and `slotTy` the `{i8; T}`
        // slot with S ≠ T: convert the payload by §5.1's underlying widening
        // and carry the source's hasValue through unchanged ("a present value
        // converts by the underlying conversion, `null` converts to `null`").
        // Mirrors the declaration-initializer path (#840, lowerLocalDecl). A
        // no-op unless both sides are the tagged-struct shape, the types
        // differ, and a §5.1 payload widening applies.
        //
        // fix #5078: hoisted to the member function `liftNullableValueToSlot`
        // (LlvmBackend_impl.hpp) so the indexer-setter arm in
        // `lowerCollectionIndexerSet` (LlvmBackend_Assign.cpp) — a SEPARATE
        // `IndexExpr` branch below that `return`s before reaching this shared
        // preamble — can reuse the identical logic instead of a second copy.
        if (as->target && as->value) {
            // per §24: `event += handler` / `event -= handler` (inside the
            // declaring class) desugar to Add/Remove — not a real compound
            // assignment. Checked first so the event target is not treated as
            // an lvalue.
            if (tryLowerEventCompound(*as)) return false;
            // §6.4 / #647: `atom.Value := v` — seq-cst store into Atomic<T> /
            // AtomicInteger<T>.  Must come before lowerPropertySet since Atomic<T>
            // has no user PropertyDecl and the fallthrough would silently drop
            // the write.
            if (tryLowerAtomicValueSet(*as)) return false;
            // stdlib §7.1 / #645: `fs.Position := n` — the one SETTABLE
            // FileStream property (`public abstract property Position: long;`
            // — Length and the Can* trio are `readonly`). Same reason as
            // Atomic<T>.Value directly above: FileStream has no user
            // PropertyDecl, so the fallthrough would silently drop the write.
            if (tryLowerFileStreamPositionSet(*as)) return false;
            // §22.9: `obj.P := v` / `obj.P OP= v` where P is a block-form
            // property runs its setter body (with `value` = v); compound
            // assignment is a read-modify-write through getter + setter.
            if (as->op == ":=") {
                // #481 / §12.1 / §12.5 / §13 / §23: a VIRTUAL or INTERFACE
                // property setter dispatches dynamically through the receiver's
                // vtable/itable. Checked before the static setter / auto-property
                // store paths (which would run the *static* type's setter and
                // miscompile an override).
                {
                    auto* tps = dynamic_cast<sema::PropertySymbol*>(
                        ctx_.exprSymbol(*as->target));
                    if (tps && !tps->isIndexer &&
                        (propSetSlot_.count(tps) ||
                         ifacePropertySetIndex_.count(tps))) {
                        llvm::Value* rhsV = lowerExpr(*as->value);
                        // fix #2490: thread the RHS expr through so a
                        // directly-passed +1 producer gets its ARGUMENT-PASS
                        // release (see lowerVirtualPropertySet's comment).
                        if (lowerVirtualPropertySet(*as->target, rhsV,
                                                    as->value.get()))
                            return false;
                    }
                }
                if (lowerPropertySet(*as->target, *as->value))
                    return false;
                // fix #2400 (§2.8.2/§2.8.3 rule 4, §11.2, §23; v0.235): a
                // DIRECT cross-module set of a plain (non-virtual,
                // non-interface) imported property — `importedObj.Prop := v`.
                // lowerPropertySet just failed because the imported
                // PropertySymbol has no ast::PropertyDecl (computedSetterOf
                // requires one); before this fix nothing else recognized this
                // shape either, so the write fell through to the generic
                // lvalueAddr fallback below, which cannot resolve a
                // PropertySymbol target and silently dropped it. Checked here,
                // after the virtual/interface and local-setter paths, so it
                // only engages for the shape neither of those covers.
                if (lowerImportedPropertySet(*as->target, *as->value))
                    return false;
                // §22.10: indexer setter for `obj[i] := v`.
                // Evaluate RHS ONCE to avoid double side-effects if
                // lowerIndexerSet fails and we fall through to the
                // plain lvalue path below.
                if (auto* ix = dynamic_cast<const ast::IndexExpr*>(
                        as->target.get())) {
                    llvm::Value* rhsOnce = nullptr;
                    // fix #4345 (§5.1/§11.2/§15.4, v0.298): the indexer-setter
                    // RHS lowering had NO case for a CollectionLiteralExpr at
                    // all — `lowerExpr` returns null for one (§15.4, not a
                    // general expression value), the assignment silently
                    // degraded via the pre-existing #3025 backstop, and the
                    // write never happened (a conforming-element
                    // `h[0] := [new Widget3Ok(s)]` ran
                    // `IndexOutOfRangeException` at the read because `_items`
                    // stayed the constructor-time empty array). `fix #4316`
                    // (the sema-side gate that admits the conforming program
                    // and rejects the non-conforming twin with
                    // "not implicitly convertible to the target type") left
                    // this codegen arm unchanged, so the materialization gap
                    // survived — this is the codegen half of that pair, in
                    // the same file #4316 named in its filing. Mirror the
                    // local-decl / return-stmt / field-init pattern: a literal
                    // RHS targeting an INLINE-array property builds the
                    // `[N x elem]` aggregate by-value; a literal targeting a
                    // HEAP collection goes through
                    // `materializeHeapCollectionLiteral` /
                    // `materializeHeapDictLiteral`. Target type is read from
                    // `ctx_.exprType(*as->target)` (the indexer element type,
                    // e.g. `Array<IHolder>`) rather than the literal's own
                    // `ctx_.exprType(*as->value)` — no #4310-family position
                    // ever sets a literal's exprType, so it is null here
                    // exactly as it was at every sibling materialization
                    // site.
                    if (auto* cl = dynamic_cast<
                                const ast::CollectionLiteralExpr*>(
                            as->value.get())) {
                        const sema::TypeRef* idxTy =
                            ctx_.exprType(*as->target);
                        if (idxTy &&
                            idxTy->kind == sema::TypeRef::Kind::InlineArray &&
                            !cl->isDict) {
                            if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(
                                    lowerTypeRef(idxTy))) {
                                llvm::Type* elemTy = aty->getElementType();
                                llvm::AllocaInst* tmp = entryAlloca(aty);
                                for (std::size_t i = 0;
                                     i < cl->elements.size(); ++i) {
                                    if (!cl->elements[i]) continue;
                                    llvm::Value* ev =
                                        lowerExpr(*cl->elements[i]);
                                    if (!ev) {
                                        // #1386: a UNIT-typed element is not a
                                        // lowering failure — same shared
                                        // helper the local-decl / return-stmt
                                        // / field-init sites call. Mirrors
                                        // fix #3712 / #3714 / #4340's identical
                                        // inline-array-element rescue.
                                        ev = lowerCollectionLiteralElementOrUnitNull(
                                            *cl->elements[i], elemTy);
                                        if (!ev) {
                                            ctx_.diagnostics().report(
                                                Severity::Error,
                                                cl->elements[i]->span.begin,
                                                "internal: lowerExpr returned "
                                                "null for indexer-setter "
                                                "inline-array collection-literal "
                                                "element — #1386");
                                            continue;
                                        }
                                    }
                                    if (ev->getType() != elemTy &&
                                        ev->getType()->isIntegerTy() &&
                                        elemTy->isIntegerTy()) {
                                        unsigned eb =
                                            ev->getType()->getIntegerBitWidth();
                                        unsigned db =
                                            elemTy->getIntegerBitWidth();
                                        ev = db > eb
                                                 ? builder_.CreateSExt(
                                                       ev, elemTy)
                                                 : builder_.CreateTrunc(
                                                       ev, elemTy);
                                    }
                                    llvm::Value* gep = builder_.CreateGEP(
                                        aty, tmp,
                                        {builder_.getInt32(0),
                                         builder_.getInt32((unsigned)i)});
                                    builder_.CreateStore(ev, gep);
                                }
                                rhsOnce = builder_.CreateLoad(
                                    aty, tmp, "ixset.inlarr");
                            }
                        } else if (idxTy && !cl->isDict) {
                            rhsOnce = materializeHeapCollectionLiteral(
                                cl, idxTy);
                        } else if (idxTy && !cl->keys.empty()) {
                            rhsOnce = materializeHeapDictLiteral(cl, idxTy);
                        }
                    }
                    if (!rhsOnce) rhsOnce = lowerExpr(*as->value);
                    // fix #3025 (§28.9/§28.3, v0.240): this dedicated
                    // indexer-setter arm precedes #2961's generic assignment
                    // fallback, so its null RHS still returned silently. Keep
                    // the existing preserve-the-slot degradation, but make it
                    // observable with the same guarded Warning posture.
                    if (!rhsOnce) {
                        // fix #3714 (§15.1/§3.2, v0.294): `lowerExpr` returns
                        // nullptr for a UNIT-typed expression by the #3421
                        // convention (there is no runtime value for `()`), so a
                        // null `rhsOnce` here is not automatically the #3025
                        // lowering-failure degrade below — for `a[i] := ()`
                        // into a plain `[N]()` inline array it is a legitimate
                        // unit-valued write. Mirrors fix #3712's
                        // lowerCollectionLiteralElementOrUnitNull decision
                        // (keyed on the RHS's OWN sema type being Kind::Unit,
                        // never on rhsOnce being null, so a genuinely failed
                        // lowering of a NON-unit RHS still degrades below).
                        // Gated on the TARGET being a plain inline array (not
                        // an indexer PROPERTY or a heap collection) so those
                        // shapes keep degrading loudly via #3025 exactly as
                        // before — this arm only ever engages the plain-array
                        // fallback a few lines down (line ~427), which
                        // `lvalueAddr` on `as->target` also reaches.
                        const sema::TypeRef* rhsTy = ctx_.exprType(*as->value);
                        const sema::TypeRef* recvArrTy = ix->target
                            ? ctx_.exprType(*ix->target) : nullptr;
                        if (rhsTy && rhsTy->kind == sema::TypeRef::Kind::Unit &&
                            recvArrTy &&
                            recvArrTy->kind == sema::TypeRef::Kind::InlineArray) {
                            llvm::Type* elemTyU = nullptr;
                            llvm::Value* addrU = lvalueAddr(*as->target, elemTyU);
                            llvm::Value* unitVal = (addrU && elemTyU)
                                ? lowerCollectionLiteralElementOrUnitNull(
                                      *as->value, elemTyU)
                                : nullptr;
                            if (addrU && unitVal) {
                                builder_.CreateStore(
                                    coerceToNullableSlot(elemTyU, unitVal),
                                    addrU);
                                return false;
                            }
                        }
                        if (!ctx_.diagnostics().hasErrors())
                            ctx_.diagnostics().report(
                                Severity::Warning, as->value->span.begin,
                                "internal: indexer-setter RHS expression failed "
                                "to lower (degraded — target retains its previous "
                                "value) — #3025");
                        return false;
                    }
                    // fix #2540: pass `as->value.get()` so the interface-
                    // dispatch branch in lowerIndexerSet can gate its paired
                    // call-site retain on the RHS being a tracked string
                    // NAMED LOCAL (matches the StringEscapeWalker's
                    // stringReleaseEligible_ / stringBorrowRetainEligible_
                    // sets — see the ifaceIdxNamedLocalEligible comment in
                    // LlvmBackend_Assign.cpp at this function's
                    // TypeDeclKind::Interface branch). nullptr would skip
                    // the retain (leak-over-UAF, the safe direction for any
                    // caller that doesn't have the source expression in
                    // scope — the ref-arg write-back path in
                    // emitAccessorRefArgWriteBacks which calls
                    // lowerIndexerSet from a pre-loaded value).
                    if (lowerIndexerSet(*ix, rhsOnce, as->value.get())) return false;
                    // issue #388 / stdlib §4.4-§4.9: heap-collection indexer
                    // setter `a[i] := v` / `dict[key] := v` (the OWNING
                    // variants dispose the displaced owner, §4.5 / §4.9).
                    if (lowerCollectionIndexerSet(*ix, rhsOnce, as->value.get()))
                        return false;
                    // Not an indexer property — fall through to plain array
                    // assignment using the already-computed RHS.
                    llvm::Type* elemTy2 = nullptr;
                    llvm::Value* addr2 = lvalueAddr(*as->target, elemTy2);
                    if (!addr2) return false;
                    // fix #4336 (§15.1.1 / §15.3 / §28.7, v0.298): a
                    // SINGLE-INDEX assignment into a plain inline-array slot
                    // of an OWNING inline array `[N owned]T` / `[N owned]T?`
                    // was missing the per-slot drop-on-overwrite pass. The
                    // whole-array reassignment arm at line ~1055 already calls
                    // `lowerOwningInlineArrayReassign` for that shape
                    // (v0.260, #1385 / #3263), but this element-write fallback
                    // overwrote the slot directly with `CreateStore` —
                    // silently leaking the displaced owner on every iteration
                    // of `outer[i] := new ...` (measured on the issue's repro
                    // shape: 200 iterations × 24 B = 4800 B dropped at the
                    // IndexExpr-arm `outer[0] := new Widget("hello")` site,
                    // byte-identical to the leak rows the v0.260 whole-array
                    // pass already closes for `outer := [...]`).
                    // Dispose the OLD slot value (only for `arrayOwned` —
                    // a non-owning `[N]IHolder` slot is, per §15.1, a borrow
                    // whose owner lives elsewhere, and disposing it here
                    // would double-free that owner's instance on a future
                    // read through its borrow source). The slot's NEW value
                    // remains under the local's existing scope-end disposal
                    // (registered by `registerInlineOwnedArrayDisposal` at
                    // the local-decl site for `[N owned]T` slots).
                    //
                    // The `as->target` here is the IndexExpr `outer[i]` —
                    // its exprType is `IHolder` (the element), NOT the
                    // enclosing array. Reach the array's type through the
                    // IndexExpr's `target` member, which IS the whole-array
                    // identifier.
                    const sema::TypeRef* arrRt = nullptr;
                    if (auto* ix2 = dynamic_cast<const ast::IndexExpr*>(
                            as->target.get())) {
                        arrRt = ctx_.exprType(*ix2->target);
                    }
                    if (arrRt && arrRt->kind ==
                            sema::TypeRef::Kind::InlineArray &&
                        arrRt->arrayOwned &&
                        elemTy2 == ptrTy()) {
                        // Element storage is `[N x ptr]`; load the old slot's
                        // interface pointer. A null ptr (an empty slot) skips;
                        // anything non-null dispatches via
                        // `emitDisposeAndReclaim` exactly as the
                        // whole-array reassignment arm does (§28.7's per-slot
                        // dispose). We do NOT null the slot here — the
                        // subsequent `CreateStore` overwrites it with the new
                        // value, which is the simpler and identical
                        // post-condition for a fresh non-null rhs (the slot's
                        // NEW contents are fully replaced; only an rhs that
                        // *equals* the slot's existing value would observe
                        // the not-yet-nulled state, and such an rhs is a
                        // self-assignment where disposal-then-same-store is
                        // observably a no-op anyway).
                        llvm::Value* oldSlot = builder_.CreateLoad(
                            ptrTy(), addr2, "ioaw.old");
                        llvm::Value* oldIsNull = builder_.CreateICmpEQ(
                            oldSlot,
                            llvm::ConstantPointerNull::get(ptrTy()),
                            "ioaw.oldnull");
                        auto* dispOldBB = llvm::BasicBlock::Create(
                            llvm_, "ioaw.dispold", currentFn_);
                        auto* contOldBB = llvm::BasicBlock::Create(
                            llvm_, "ioaw.contold", currentFn_);
                        builder_.CreateCondBr(
                            oldIsNull, contOldBB, dispOldBB);
                        builder_.SetInsertPoint(dispOldBB);
                        emitDisposeAndReclaim(oldSlot);
                        builder_.CreateBr(contOldBB);
                        builder_.SetInsertPoint(contOldBB);
                    }
                    // fix #4317 (§15.1 / §8.6, v0.298): box a value-typed RHS
                    // (a conforming RECORD) into its box representation before
                    // the store below. The generic lvalueAddr fallback further
                    // down this function (the `boxForTarget` call a few dozen
                    // lines below, guarding every non-IndexExpr target) already
                    // does this — this IndexExpr arm `return false`s before
                    // ever reaching it, so `outer[i] := new RecHolder(...)`
                    // into an `[N]IHolder` slot never boxed. Measured under
                    // ASan: `global-buffer-overflow`, READ of size 8, 2 bytes
                    // past the string global whose 8-byte fat-pointer word
                    // the record was stored as, and a later `.GetV()` dispatch
                    // walked off. boxForTarget is a no-op for a CLASS element
                    // (already a bare pointer, #399/isBoxableValueType false)
                    // and for a plain (non-interface) element target, so this
                    // is unaffected for every already-working shape (see the
                    // `*_class_element_unaffected_control` rows in
                    // inline_array_interface_record_element_store_boxing_
                    // 4317_test.cpp).
                    rhsOnce = boxForTarget(rhsOnce, ctx_.exprType(*as->target),
                                           as->value.get());
                    // #813 / §15.1: an inline `[N]T?` element slot is a nullable-
                    // value struct `{i8, T}`; wrap the bare inner value so the
                    // hasValue byte is set (this fast path bypasses the general
                    // CG-nullable wrapping below).
                    // fix #5005 (§5.1 `S → T → T?`, v0.307): thread the RHS
                    // expression's signedness so a narrower UNSIGNED source
                    // (e.g. a `byte`/`uint` element assign into a `long?`
                    // slot) widens via ZExt, not the default SExt — mirrors
                    // issue #421/#410 at every other widen-then-wrap site.
                    builder_.CreateStore(
                        coerceToNullableSlot(
                            elemTy2, rhsOnce,
                            isUnsignedTargetExpr(*as->value)),
                        addr2);
                    return false;
                }
            } else if (as->op != "??=") {
                // fix #2905 (§7.7/§6.6, v0.240): `??=` skips this whole
                // read-modify-write chain. §6.6, verbatim: "**`??=`** is not a
                // read-modify-write: `x ??= e` assigns `e` only when `x`'s
                // current value is null … `e` is evaluated — and the setter
                // invoked — only when the assignment actually occurs." None of
                // the handlers below can lower it (each ends in an
                // `emitCompoundOp` call with the operator `??`, which has no
                // arm and returns null), but `lowerPropertyCompound` EMITS the
                // getter read AND `lowerExpr(*as->value)` before discovering
                // that — an unconditional evaluation of `e` that §6.6 forbids
                // outright. The `??=` arm below owns the shape end to end.
                //
                // #481: a VIRTUAL/INTERFACE property compound assignment
                // `obj.P OP= v` is a read-modify-write — both the getter read
                // and the setter write dispatch dynamically.
                auto* tps = dynamic_cast<sema::PropertySymbol*>(
                    ctx_.exprSymbol(*as->target));
                if (tps && !tps->isIndexer &&
                    (propSetSlot_.count(tps) ||
                     ifacePropertySetIndex_.count(tps)) &&
                    (propGetSlot_.count(tps) ||
                     ifacePropertyGetIndex_.count(tps))) {
                    llvm::Value* cur = lowerExpr(*as->target);  // dyn getter
                    llvm::Value* rhs = lowerExpr(*as->value);
                    if (cur && rhs) {
                        std::string op(as->op);
                        if (!op.empty() && op.back() == '=') op.pop_back();
                        // fix #2904 (§25.1/§25.2/§6.6, v0.240): via
                        // emitCompoundOpTyped so a RECORD- or class-typed
                        // VIRTUAL/INTERFACE property takes §25's overload —
                        // measured on c7404bf, `i.VP += one` through an
                        // interface aborted the compile on
                        // `llvm.sadd.with.overflow.s_record.Pairs`.
                        llvm::Value* res = emitCompoundOpTyped(
                            op, cur, rhs, ctx_.exprType(*as->target),
                            ctx_.exprType(*as->value),
                            cur->getType()->isFloatingPointTy(),
                            isUnsignedTargetExpr(*as->target),
                            isCharTargetExpr(*as->target),      // §6.6/§5.2 #1961
                            isUnsignedTargetExpr(*as->value));  // §6.2 #2140 rUns
                        // fix #2586 (§23.3/§28.9/§4.6, v0.236): the VIRTUAL/
                        // INTERFACE compound-assign twin of #2576's non-virtual
                        // fix (lowerPropertyCompound). For a string target,
                        // emitCompoundOp dispatches to emitStringConcat which
                        // ALWAYS mints a fresh rc=1 `res` (memcpy of both
                        // operands' bytes), and `rhs = lowerExpr(*as->value)`
                        // is a SECOND +1 when the AST is a producer. The dynamic
                        // setter (lowerVirtualPropertySet) retains `res` for a
                        // CLASS-backed property, but was called with no valueExpr
                        // — so its internal emitProducerRelease did not fire for
                        // `res`. Pass valueIsPlusOne=true so lowerVirtualPropertySet
                        // releases `res` itself on the retaining (class-backed)
                        // path, using its OWN owner-kind flag branching (so a
                        // boxed RECORD's plain-store setter — which never
                        // retained res — is NOT double-released; res's +1 is the
                        // record field's only ref, §9/§9.7). `rhs`'s orphan +1
                        // (a producer rhs like `"abc" + i.ToString()`) is
                        // released at THIS call site: rhs's +1 is NOT stored
                        // anywhere (emitStringConcat memcpy's its bytes into res
                        // and leaves rhs's own +1 untouched), so releasing rhs
                        // is safe for ALL backings — class vtable, class-backed
                        // iface, AND record-backed iface (a borrow rhs — e.g. a
                        // named local — is not +1; the producer predicate gates
                        // it). `cur` (the getter read) is a BORROW — the getter
                        // accessor body returns a field load with no retain — so
                        // it is deliberately NOT released (releasing a borrow
                        // would UAF the field's own ref). Mirrors #2576's
                        // structure in lowerPropertyCompound exactly.
                        if (res && lowerVirtualPropertySet(*as->target, res,
                                                           nullptr,
                                                           /*valueIsPlusOne=*/true)) {
                            // Release rhs's orphan +1 (producer only). Safe for
                            // all backings — rhs is a distinct allocation from
                            // res (emitStringConcat memcpy's into a fresh header),
                            // and its +1 is never stored anywhere.
                            if (rhs->getType()->isPointerTy() &&
                                isPlusOneStringProducer(as->value.get()))
                                emitStringRelease(rhs);
                            // fix-forward #4952 (§4.6/§28.3/§28.9, v0.307): #4952
                            // made a dynamically-slotted (vtable/itable) string
                            // property read UNCONDITIONALLY a +1 producer (the
                            // getter-side ownership thunk now retains every such
                            // read), so `cur` — `lowerExpr(*as->target)` above,
                            // a dynamic getter dispatch — is no longer the
                            // guaranteed borrow this fix's original comment
                            // assumed. Release it here on that same predicate,
                            // mirroring rhs's release immediately above; `res` is
                            // a distinct allocation (emitStringConcat memcpy's
                            // `cur`'s bytes into a fresh header) so releasing
                            // `cur` cannot UAF `res`.
                            if (cur->getType()->isPointerTy() &&
                                isPlusOneStringProducer(as->target.get()))
                                emitStringRelease(cur);
                            return false;
                        }
                    }
                }
                if (lowerPropertyCompound(*as)) return false;
                // fix #2862 (§6.6/§2.8.2 clause 2/§12.1/§23.3, v0.240): the
                // COMPOUND twin of the `lowerImportedPropertySet` fallback in
                // the `:=` arm above. lowerPropertyCompound just declined
                // because an IMPORTED property has no ast::PropertyDecl for
                // `computedSetterOf` to find; before this call nothing else
                // recognized the shape either, so `importedObj.P += 1` fell
                // through to the generic lvalueAddr fallback — which cannot
                // resolve a PropertySymbol target — and the write was SILENTLY
                // DROPPED (measured 0 where 102 is correct, while
                // `importedObj.P := 7` on the same property was correct at 8).
                // Placed here, after the virtual/interface branch above and
                // after the local-property path, so it only engages for the
                // shape neither covers.
                if (lowerImportedPropertyCompound(*as)) return false;
                // §20 / §23.4: user-defined indexer compound assignment
                // `a[i] OP= v`. Must come after lowerPropertyCompound (which
                // handles non-indexer computed properties) and before the generic
                // lvalueAddr fallback (which handles only built-in inline arrays
                // and returns null for user-indexer targets, silently dropping
                // the write). The getter and setter are called with the receiver
                // and index evaluated exactly once.
                if (lowerIndexerCompound(*as)) return false;
                // fix #508 / §4.4 / §4.5 / §17.15: heap-collection Array<T>
                // compound assignment `arr[i] OP= v`. Must come after
                // lowerIndexerCompound (user-defined indexer) and before the
                // lvalueAddr fallback (which returns null for a bz_array_t*
                // receiver, causing the write to be silently dropped). Evaluates
                // receiver and index once; reads via bz_array_get, applies OP,
                // writes back via bz_array_set / bz_array_set_disposing.
                if (lowerCollectionIndexerCompound(*as)) return false;
            }
            llvm::Type* elemTy = nullptr;
            llvm::Value* addr = nullptr;
            llvm::Value* rhs = nullptr;
            // fix #4646 (§22.16, v0.307): the §22.16 AUTO-PROPERTY setter
            // boundary for `obj.P := v`. `invRecv4646` receives the receiver
            // pointer `lvalueAddr` already lowered for the field store (see
            // `instrumentedAutoPropertyStoreClass` above); `invCls4646` is the
            // class whose conjunction to evaluate, or null when §22.16
            // instruments nothing here.
            llvm::Value* invRecv4646 = nullptr;
            const sema::TypeSymbol* invCls4646 = nullptr;
            // #736: a whole-array collection-literal assignment
            // `target := [e0, e1, ...]` to an inline-array lvalue (field or
            // local). A CollectionLiteralExpr is not a general expression value
            // (lowerExpr returns null for it), so this store was silently dropped
            // and the array stayed zero-initialized — field-init and local-decl
            // initializers special-case it, but the assignment statement did not.
            // Mirror the local-decl element-store loop (LlvmBackend_Setup.cpp
            // ~4281). Heap collections (Set/Array/Dict) have `ptr` slots, not an
            // ArrayType elemTy, so the dyn_cast guard excludes them.
            if (as->op == ":=") {
                if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                        as->value.get())) {
                    if (!cl->isDict) {
                        llvm::Type* aelemTy = nullptr;
                        llvm::Value* aaddr = lvalueAddr(*as->target, aelemTy);
                        if (auto* aty = aelemTy
                                ? llvm::dyn_cast<llvm::ArrayType>(aelemTy)
                                : nullptr) {
                            if (aaddr) {
                                // §28.7 / §15.3 (#1385 / v0.260, K9.5): the RHS
                                // of a whole-value reassignment is evaluated
                                // FIRST, then the drop-on-overwrite pass runs,
                                // then the new value is stored. The plain
                                // AssignmentStmt path (lowerOwningInlineArray
                                // Reassign called from the `:=` block at line
                                // ~504) covers non-collection-literal RHS
                                // forms (the RHS is already evaluated at that
                                // point, per the comment there). The literal
                                // form short-circuits past that site, so this
                                // arm must itself evaluate the literal elements
                                // FIRST into temporaries, then run the disposal
                                // pass, then store. Pre-fix (defect 1,
                                // #3263): the pass ran BEFORE the per-element
                                // evaluation — a literal element of the shape
                                // `move a[0]` read slot 0 of the LHS, which the
                                // pass had already disposed (UAF on the freed
                                // slot pointer). A literal of arity < N
                                // additionally left trailing slots disposed-
                                // then-unstored (stale freed pointers in the
                                // alloca). Post-fix: every element lowers into
                                // a per-element alloca, the pass runs against
                                // the live LHS slots, then each temporary is
                                // stored — a slot shorter than N is stored as
                                // the slot's zero/null value (the trailing-
                                // empty state) and the literal-element read
                                // of `move a[i]` sees a live slot.
                                llvm::Type* et = aty->getElementType();
                                // fix #4317 (§15.1 / §8.6, v0.298): the
                                // element's DECLARED sema type, used below to
                                // box a value-typed (record) literal element
                                // into an Object/interface slot — mirrors
                                // lowerLocalDecl's inline-array init loop
                                // (LlvmBackend_Setup.cpp), which needs the
                                // identical fix (tracked separately: that
                                // call site is in a held file this lane could
                                // not edit; see follow-up issue).
                                const sema::TypeRef* wholeArrTy4317 =
                                    ctx_.exprType(*as->target);
                                const sema::TypeRef* elemSemaTy4317 =
                                    (wholeArrTy4317 &&
                                     wholeArrTy4317->kind ==
                                         sema::TypeRef::Kind::InlineArray)
                                        ? wholeArrTy4317->arrayElement
                                        : nullptr;
                                std::vector<llvm::Value*> elemTemps(
                                    cl->elements.size(), nullptr);
                                for (std::size_t i = 0;
                                     i < cl->elements.size() &&
                                     i < aty->getNumElements(); ++i) {
                                    if (!cl->elements[i]) continue;
                                    llvm::Value* ev =
                                        lowerExpr(*cl->elements[i]);
                                    if (!ev) {
                                        // #1386: ICE — lowerExpr returned null
                                        // for an inline-array assignment element;
                                        // the expression kind is unhandled.
                                        ctx_.diagnostics().report(
                                            Severity::Error,
                                            cl->elements[i]->span.begin,
                                            "internal: lowerExpr returned null "
                                            "for inline-array assignment element"
                                            " — #1386");
                                        continue;
                                    }
                                    if (ev->getType() != et &&
                                        ev->getType()->isIntegerTy() &&
                                        et->isIntegerTy()) {
                                        unsigned eb =
                                            ev->getType()->getIntegerBitWidth();
                                        unsigned db = et->getIntegerBitWidth();
                                        ev = db > eb
                                                 ? builder_.CreateSExt(ev, et)
                                                 : builder_.CreateTrunc(ev, et);
                                    }
                                    // fix #4317 (§15.1 / §8.6, v0.298): box a
                                    // value-typed (record) element into its box
                                    // representation before the nullable-
                                    // slot wrap below — see the detailed
                                    // derivation at the sibling IndexExpr-arm
                                    // fix a few dozen lines above. No-op for a
                                    // class element (already a bare pointer)
                                    // or a non-Object/interface element type.
                                    ev = boxForTarget(ev, elemSemaTy4317,
                                                      cl->elements[i].get());
                                    // #813 / §15.1: wrap into the nullable-value
                                    // slot struct for an `[N]T?` literal assign
                                    // — same wrapping the post-disposal store
                                    // below would apply. Materialized into a
                                    // per-element alloca so the disposal pass
                                    // can run AFTER every element has been
                                    // evaluated (K9.5).
                                    llvm::Value* wrapped =
                                        coerceToNullableSlot(et, ev);
                                    llvm::AllocaInst* tmp =
                                        entryAlloca(wrapped->getType());
                                    tmp->setName("ioar.lit.elem");
                                    builder_.CreateStore(wrapped, tmp);
                                    elemTemps[i] = tmp;
                                }
                                // Disposal pass runs AFTER every element is
                                // materialized, matching §28.7 v0.260's
                                // "RHS-first" rule. fix #3263 defect 2: pass
                                // `as->wholeValueLhsMovedFrom` so the
                                // whole-value-moved-from LHS skips the pass.
                                lowerOwningInlineArrayReassign(
                                    *as->target, aaddr, aelemTy,
                                    /*skipDisposal=*/
                                    as->wholeValueLhsMovedFrom);
                                // Now store the materialized temporaries into
                                // the LHS slots. Slots whose element was
                                // missing or whose `lowerExpr` returned null
                                // (and the diagnostic was reported) are left
                                // with whatever the disposal pass left —
                                // null, after defect 3's slot-empty rule. A
                                // literal of arity < N therefore leaves
                                // trailing slots null (the only observable
                                // post-condition on the non-abandoned-store
                                // path; the abandoned-store path's spec
                                // ruling is §15.1.1's "non-nullable slot
                                // invariant" reconciliation in §28.7).
                                for (std::size_t i = 0;
                                     i < cl->elements.size() &&
                                     i < aty->getNumElements(); ++i) {
                                    if (!elemTemps[i]) continue;
                                    llvm::Value* gep = builder_.CreateGEP(
                                        aty, aaddr,
                                        {builder_.getInt32(0),
                                         builder_.getInt32((unsigned)i)});
                                    // The temporary's type is `et` (the slot
                                    // element type, possibly a nullable-value
                                    // struct). Recreate the load type from
                                    // the element type — the same as what
                                    // was stored.
                                    builder_.CreateStore(
                                        builder_.CreateLoad(
                                            et, elemTemps[i]),
                                        gep);
                                }
                                return false;  // handled
                            }
                        } else if (aaddr) {
                            // #820 / §15.4: a HEAP-collection target (`Array<T>`,
                            // `OwningArray<T>`, etc.) lowers to a bare `ptr` slot,
                            // not an inline ArrayType — the inline path above
                            // missed it, so `field := [1,2,3]` lowered the
                            // CollectionLiteralExpr (which yields null as a general
                            // value) and the store was silently dropped, leaving
                            // the slot null → crash on first use. Materialize a
                            // real heap collection and store the pointer, mirroring
                            // the field-initializer path (Setup.cpp ~4117).
                            const sema::TypeRef* tRt =
                                ctx_.exprType(*as->target);
                            if (tRt && isCollectionTypeName(tRt)) {
                                if (llvm::Value* arr =
                                        materializeHeapCollectionLiteral(cl,
                                                                         tRt)) {
                                    // #824 / §28.5 / §28.9: if target is an
                                    // `owned` collection CLASS field, dispose the
                                    // old collection before overwriting. Delegates
                                    // to lowerOwnedCollectionFieldStore which does
                                    // load-old / store-new / bz_array_free(old).
                                    // For locals / borrowed fields / non-owned
                                    // targets the helper returns false and we fall
                                    // through to the plain store below.
                                    if (!lowerOwnedCollectionFieldStore(
                                            *as->target, aaddr, arr))
                                        builder_.CreateStore(arr, aaddr);
                                    return false;  // handled
                                }
                            }
                        }
                    }
                }
            }
            // #579 / §27.5: a recursive lambda assigned to a local refers to
            // that very local in its body — `counter := (n) => ... counter(n-1)`.
            // Lowering the RHS lambda BOXES the captured `counter` slot: it
            // promotes the slot to a shared heap cell and REBINDS locals_[decl]
            // to the new cell-holder alloca (so the closure's env and all later
            // reads route through the cell). If we compute the LHS address FIRST
            // (the normal order below), lvalueAddr resolves `counter` to the
            // ORIGINAL un-boxed slot; the subsequent store writes the finalized
            // closure into that stale slot while the captured cell still holds
            // the pre-box garbage. The lambda then calls through the cell and
            // dereferences the garbage pointer → SIGSEGV.
            //
            // Fix: for a plain `:=` to a local/param identifier (an lvalue with
            // NO side effects of its own — the only "effect" is selecting the
            // storage slot, which does not depend on the RHS value), lower the
            // RHS FIRST so any capture-induced boxing/rebinding completes, then
            // resolve the LHS address through the (now-rebound) slot — which
            // boxedStorage routes to the shared cell. The store then reaches the
            // same cell the closure captured. Observationally identical to the
            // spec eval order for a side-effect-free identifier target.
            //
            // Restricted to `:=` (a plain store); compound `OP=` / `??=` keep
            // the address-then-RHS order so their read-modify-write reads the
            // current value first. Field / static-field / index targets keep the
            // natural left-to-right order (their LHS may have side effects, and
            // they cannot be the lambda being initialized for the first time).
            if (as->op == "??=") {
                // §7.7 (#1190): null-coalescing assignment short-circuits —
                // the RHS must be evaluated ONLY when the LHS is null. Compute
                // the target address now but DEFER `lowerExpr(*as->value)` into
                // the LHS-is-null branch handled immediately below, so a
                // non-null LHS never runs (nor produces) the RHS. Lowering it
                // eagerly here would violate short-circuit and, for an owned
                // RHS, leave an unobserved owned value (leak / double-free).
                //
                // fix #2905 (§12.1/§23.3/§6.6, v0.240): a VIRTUAL or INTERFACE
                // property is checked BEFORE lvalueAddr, with the SAME gate the
                // #481 compound arm uses. §12.1 requires the getter and setter
                // to dispatch on the RUNTIME type, so an addressed store is
                // wrong for this shape by construction — and MEASURED on
                // c7404bf it is wrong in the worst way: `lvalueAddr` resolves
                // `iface.P` to the receiver's FIELD INDEX 0 and the write lands
                // in an unrelated field (a class with `_z`, `_n`, `_v` and
                // `i.IP ??= 9` gave `z=9, n=-1, v=-1`), with no diagnostic.
                {
                    auto* tps = dynamic_cast<sema::PropertySymbol*>(
                        ctx_.exprSymbol(*as->target));
                    if (tps && !tps->isIndexer &&
                        (propSetSlot_.count(tps) ||
                         ifacePropertySetIndex_.count(tps)) &&
                        (propGetSlot_.count(tps) ||
                         ifacePropertyGetIndex_.count(tps)) &&
                        lowerAccessorNullCoalescingAssign(*as))
                        return false;
                }
                // fix #4732 (§22.16 / §6.6, v0.307): ask for the receiver
                // pointer here too. This is the `??=` twin of the #4646 request
                // on the `:=` arm's `lvalueAddr` a few dozen lines below, and it
                // exists for the same reason: an auto-property `??=` has no
                // setter body, so the store falls through to the generic
                // backing-field path and the §22.16 boundary has to be emitted
                // around it. Threading the out-parameter keeps the receiver
                // expression evaluated exactly once and puts the boundary check
                // AFTER §28.2's disposed guard, which `fieldAddr` emits
                // internally.
                addr = lvalueAddr(*as->target, elemTy, &invRecv4646);
                // fix #2905 (§7.7/§6.6/§23.3/§23.4, v0.240): no address means
                // an ACCESSOR-MEDIATED target — a property or an indexer, which
                // has no backing slot and is reachable only through its
                // accessor bodies. `return false` here dropped the write with
                // NO diagnostic (measured: `b.NP ??= 77` left `b.NP` null,
                // exit 5 where 3 is correct — and identically for a `string?`
                // property, an interface property, a user indexer, an
                // `Array<int?>` element and a `Dictionary<K,int?>` element).
                // The accessor form is the same construct with the getter and
                // setter in place of the load and the store.
                if (!addr) {
                    lowerAccessorNullCoalescingAssign(*as);
                    return false;
                }
            } else if (as->op == ":=" &&
                dynamic_cast<const ast::IdentifierExpr*>(as->target.get()) &&
                localSlot(*as->target) != nullptr) {
                // fix #4300: if this assignment targets a Class-with-Record/Choice-field
                // local in classCallResultWithRecordFieldEligible_ and the RHS is a
                // CallExpr, arm the suppression flag before lowering the call so
                // lowerCallDispatch sees it at entry.
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        as->target.get())) {
                    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(
                            ctx_.exprSymbol(*id))) {
                        if (classCallResultWithRecordFieldEligible_.count(
                                static_cast<const void*>(loc->decl))) {
                            if (auto* call = dynamic_cast<const ast::CallExpr*>(
                                    as->value.get())) {
                                recordCallResultInitOutermostClassWithRecordField_ =
                                    true;
                            }
                        }
                    }
                }
                rhs = lowerExpr(*as->value);
                rhs = rescueUnitAssignRhs(rhs, *as);
                // fix #2961 (§28.9/§28.3, v0.240): the `:=` arms of lowerStmt
                // bailed silently on a null RHS — no diagnostic of any severity.
                // The sibling positions already treat this as a reportable
                // degradation: lowerLocalDecl (#2434, Warning), emitReturn
                // (#2491, Warning), buildOrderedArgs (#1386, Error). Make the
                // assignment arm the one member of that family with a signal.
                //
                // Severity::Warning, guarded on `!ctx_.diagnostics().hasErrors()`,
                // mirroring #2434's posture exactly: an Error would false-reject
                // §3/#605's permissive "unknown member" shape (e.g.
                // `o: Object; o := o.Anything();`, the local-decl twin
                // #2434's own corpus check catalogues) — codegen has no concrete
                // member to dispatch to, so `lowerExpr` returns null for a
                // sema-legal program, and the `return false;` degrade is what
                // keeps the target's previous value in place. Driver::compile
                // ExecutableObject prints every diagnostic regardless of
                // severity, so a Warning is fully visible on stderr without
                // failing the build.
                //
                // The unit case is already handled by rescueUnitAssignRhs above
                // (§4.7 / §8.6 "Boxing of the unit value", #2915) — a unit RHS
                // is rescued to a box (or a no-op for non-Object/interface
                // targets) before reaching this null check, so the Warning only
                // fires for non-unit shapes whose lowering actually failed.
                if (!rhs) {
                    if (!ctx_.diagnostics().hasErrors()) {
                        std::string builtinRecv, member, appliedMember;
                        builtinRecv = builtinReceiverOfMemberCall(
                            as->value.get(), &member, &appliedMember);
                        if (!builtinRecv.empty()) {
                            // A built-in call whose result has no lowered value
                            // (e.g. ChannelSender<T>.Send): name it explicitly.
                            // Keep the original sentence prefix intact so the
                            // pinned CHECK-DAG in the lit test still matches.
                            ctx_.diagnostics().report(
                                Severity::Warning, as->value->span.begin,
                                "internal: assignment RHS expression failed to "
                                "lower to a value for this assignment's target "
                                "(store dropped — target retains its previous "
                                "value) — " + builtinRecv + "." + member +
                                " produces no value here (#2961)");
                        } else {
                            ctx_.diagnostics().report(
                                Severity::Warning, as->value->span.begin,
                                "internal: assignment RHS expression failed to "
                                "lower to a value for this assignment's target "
                                "(store dropped — target retains its previous "
                                "value) — #2961");
                        }
                    }
                    return false;
                }
                addr = lvalueAddr(*as->target, elemTy);
                if (!addr) return false;
            } else {
                // fix #4646 (§22.16, v0.307): ask for the receiver pointer
                // here, on the ONE `lvalueAddr` call that an `obj.P := v`
                // auto-property store reaches. Threading the out-parameter
                // (rather than re-lowering `ma->target` at the boundary) keeps
                // the receiver expression evaluated exactly once — and puts
                // the boundary check AFTER §28.2's disposed guard, which
                // `fieldAddr` emits internally. That order is not incidental:
                // #4651 measured it on the public-METHOD row, where a class
                // whose `destructor` zeroes the field its own invariant reads
                // must answer DisposedAccessException, not
                // ContractViolationException.
                addr = lvalueAddr(*as->target, elemTy, &invRecv4646);
                // fix #4045 (§28.3/§28.9, v0.294): the generic lvalueAddr
                // fallback's null-address exit was silently dropping the write
                // with NO diagnostic of any severity, and the RHS was never
                // even evaluated. This is the exact mechanism behind #4038's
                // write-repro (a then-unresolved alias-qualified static field
                // write), but the gap is GENERAL — it fires for ANY assignment
                // target whose lvalueAddr resolution fails for any reason, not
                // just the alias case #4038 already closed at the sema layer.
                // Mirror #2961's sibling rhs-null diagnostic at this site's
                // adjacent rhs-null arm (lines below): Warning, guarded on
                // !hasErrors(), naming the assignment target's span and
                // stating the write was dropped — so a *future* lowering gap
                // that reaches this fallback (present or future) is observable
                // on stderr instead of silently no-op'ing the store. Severity
                // is Warning not Error for the same reason #2961's is: a
                // sema-legal program whose target lowering simply fails
                // (rather than is rejected) would otherwise be turned into a
                // hard build failure here, the false-reject #2434 warned
                // against checking for before choosing Error.
                if (!addr) {
                    if (!ctx_.diagnostics().hasErrors())
                        ctx_.diagnostics().report(
                            Severity::Warning, as->target->span.begin,
                            "internal: assignment target lvalue address failed to resolve (store dropped — target retains its previous value) — #4045");
                    return false;
                }
                // fix #4300: arm suppression for Class-with-Record-field "declare,
                // then assign" targets before lowering the call.
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        as->target.get())) {
                    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(
                            ctx_.exprSymbol(*id))) {
                        if (classCallResultWithRecordFieldEligible_.count(
                                static_cast<const void*>(loc->decl))) {
                            if (auto* call = dynamic_cast<const ast::CallExpr*>(
                                    as->value.get())) {
                                recordCallResultInitOutermostClassWithRecordField_ =
                                    true;
                            }
                        }
                    }
                }
                rhs = lowerExpr(*as->value);
                rhs = rescueUnitAssignRhs(rhs, *as);
                // fix #2961 — same Warning the localSlot arm emits above; the
                // generic lvalueAddr fallback covers every non-local-slot
                // assignment shape (field, property, indexer, imported target)
                // that reached this point without an earlier dedicated arm
                // having claimed the store.
                if (!rhs) {
                    if (!ctx_.diagnostics().hasErrors()) {
                        std::string builtinRecv, member, appliedMember;
                        builtinRecv = builtinReceiverOfMemberCall(
                            as->value.get(), &member, &appliedMember);
                        if (!builtinRecv.empty()) {
                            ctx_.diagnostics().report(
                                Severity::Warning, as->value->span.begin,
                                "internal: assignment RHS expression failed to "
                                "lower to a value for this assignment's target "
                                "(store dropped — target retains its previous "
                                "value) — " + builtinRecv + "." + member +
                                " produces no value here (#2961)");
                        } else {
                            ctx_.diagnostics().report(
                                Severity::Warning, as->value->span.begin,
                                "internal: assignment RHS expression failed to "
                                "lower to a value for this assignment's target "
                                "(store dropped — target retains its previous "
                                "value) — #2961");
                        }
                    }
                    return false;
                }
            }
            // §7.7 (#1190): null-coalescing assignment `x ??= e`. Mirror the
            // `??` binary operator's short-circuit (LlvmBackend_Exprs.cpp
            // lowerBinary "??"): null-test the LHS first and lower the RHS
            // inside the null branch ONLY. Self-contained — returns before the
            // `:=`-only coercion/field-store blocks below.
            if (as->op == "??=") {
                llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                llvm::Value* isNull = nullptr;
                if (cur->getType()->isPointerTy()) {
                    isNull = builder_.CreateICmpEQ(
                        cur, llvm::ConstantPointerNull::get(ptrTy()));
                } else if (cur->getType()->isStructTy()) {
                    // CG-nullable: nullable primitive {i8 hasValue; primType
                    // value}. Null when hasValue == 0.
                    isNull = builder_.CreateNot(nullablePrimHasValue(cur));
                }
                // A non-nullable target can never be null → nothing to do.
                if (!isNull) return false;
                auto* assignBB =
                    llvm::BasicBlock::Create(llvm_, "coaleq.do", currentFn_);
                auto* endBB =
                    llvm::BasicBlock::Create(llvm_, "coaleq.end", currentFn_);
                builder_.CreateCondBr(isNull, assignBB, endBB);
                builder_.SetInsertPoint(assignBB);
                // RHS evaluated here ONLY (the LHS was null) — short-circuit.
                rhs = lowerExpr(*as->value);
                if (!rhs) {
                    // fix #3025 (§28.9/§28.3, v0.240): the RHS is lowered only
                    // in §6.6's null branch, but a null lowering result used to
                    // branch around the store silently. Preserve the target and
                    // make that degradation observable, matching #2961.
                    if (!ctx_.diagnostics().hasErrors())
                        ctx_.diagnostics().report(
                            Severity::Warning, as->value->span.begin,
                            "internal: ??= short-circuit RHS expression failed "
                            "to lower (degraded — target retains its previous "
                            "value) — #3025");
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                    return false;
                }
                // §5.1: numeric widening to the target slot (mirror the `:=`
                // path's coerceNumericToType, issue #408 / #421 / #410).
                // fix #5005 (§5.1 "Nullable conversions" — `S` converts to `T?`
                // via `S → T → T?` — v0.307): `??=`'s target is nullable by
                // construction (a non-nullable target returns false at the
                // `!isNull` check above), so `elemTy` here is always the
                // CG-nullable tagged struct `{i8 hasValue, T}` for a nullable-
                // VALUE target. Widening against the whole struct type is a
                // no-op (neither an integer nor a float type), leaving `rhs`
                // at its narrower width; the wrap block below then requires an
                // EXACT payload-width match and silently falls through
                // unwrapped, storing a bare narrow int over the struct's
                // leading byte — MEASURED as `x ??= k;` (`k: int`, `x: long?`)
                // printing `0` instead of the widened `4`. `slotPayloadTy` is
                // the same unwrap the `:=` arm (a few hundred lines up) and
                // the declaration-initializer path (fix #839) already use.
                // A no-op for every non-nullable-value / already-payload-width
                // / non-numeric `elemTy`.
                if (elemTy)
                    rhs = coerceNumericToType(
                        rhs, slotPayloadTy(elemTy),
                        isUnsignedTargetExpr(*as->value));
                // fix #5017: the lifted `S? → T?` twin of the `:=` arm below
                // (same mechanism, same measurement: `x ??= ki;` with
                // `ki: int? := 4`, `x: double?` printed `0`).
                if (elemTy)
                    rhs = liftNullableValueToSlot(
                        rhs, elemTy, isUnsignedTargetExpr(*as->value));
                // CG-nullable: wrap a plain primitive RHS into the target's
                // {hasValue, value} struct (or normalize a null literal).
                if (auto* st = llvm::dyn_cast<llvm::StructType>(elemTy)) {
                    if (st->getNumElements() == 2 &&
                        st->getElementType(0) == nullablePrimHasValTy() &&
                        rhs->getType() != elemTy) {
                        if (rhs->getType()->isPointerTy() &&
                            llvm::isa<llvm::ConstantPointerNull>(rhs)) {
                            rhs = llvm::ConstantAggregateZero::get(elemTy);
                        } else if (rhs->getType() == st->getElementType(1)) {
                            llvm::Value* wrapped = builder_.CreateInsertValue(
                                llvm::UndefValue::get(elemTy),
                                llvm::ConstantInt::get(
                                    nullablePrimHasValTy(), 1), {0u});
                            rhs = builder_.CreateInsertValue(wrapped, rhs, {1u});
                        }
                    }
                }
                // fix #4732 (§22.16 / §6.6, v0.307): the §22.16 AUTO-PROPERTY
                // setter boundary for `obj.P ??= e`.
                //
                // §22.16 instruments "Entry and normal exit of every `public`
                // setter — block-form, auto-property, indexer, `set(owned)`
                // alike", and §6.6 says of this very spelling: "`??=` is not a
                // read-modify-write: `x ??= e` assigns `e` only when `x`'s
                // current value is null … `e` is evaluated — and THE SETTER
                // INVOKED — only when the assignment actually occurs." So when
                // control reaches here (inside `coaleq.do`, the LHS-was-null
                // branch) a setter invocation is happening and both boundaries
                // are owed. #4646 emitted them on the `:=` arm and #4682 on the
                // compound / `++` arms; this arm returns before either, so it
                // evaluated nothing at all.
                //
                // MEASURED on 6221ed38 with `--checked-contracts`, on a class
                // with `public property P: int? := null; invariant(P = null);`:
                // `c.P ??= 7;` gave 42 where §22.16 requires
                // ContractViolationException (99), while the byte-identical
                // `c.P := 7;` on the same class already gave 99. That is a
                // SILENT UNDER-CHECK — the direction §22.16's
                // observable-boundary property exists to prevent — and it is
                // the ONLY write form §6.6 admits on a nullable target
                // ("Rejected … every compound-assignment form; and `++`/`--`.
                // Retained: … `??=`"), so for a `T?` auto-property this arm was
                // the whole of §22.16's setter row.
                //
                // Both checks sit INSIDE the null branch, never on the
                // short-circuit path: §6.6 makes a non-null LHS no setter
                // invocation at all, so instrumenting it would be an OVER-check
                // (and would fault on an instance a caller is legitimately
                // mid-repair on). The ENTRY check is emitted after the RHS has
                // been lowered and coerced — §22.16 binds arguments before any
                // clause runs — and before the whole store sequence, so a
                // violating entry state throws before the `+1` retain below
                // rather than after it (a retained string that is never stored
                // would leak).
                //
                // `invRecv4646` is non-null only when `lvalueAddr` resolved an
                // `obj.f`-shaped target through `fieldAddr`; a shape whose
                // receiver pointer never materialised (an implicit-`this`
                // `P ??= e`, §22.16-exempt anyway) degrades to the pre-fix
                // behaviour rather than checking over a null pointer.
                //
                // WHAT DOES NOT REACH HERE, AND WHAT DOES. A BLOCK-FORM,
                // VIRTUAL or INTERFACE property is claimed above by
                // `lowerAccessorNullCoalescingAssign` — by the virtual/
                // interface gate for the dispatching shapes, by the `!addr`
                // fallback for the rest — and its store routes through
                // `lowerPropertySetValue` (#4614(b)), already instrumented.
                // An IMPORTED AUTO-property, by contrast, DOES reach here:
                // #2478 made it real storage in the importer's layout, so
                // `lvalueAddr` succeeds and the accessor path is never taken.
                // `instrumentedAutoPropertyStoreClass` then declines for it on
                // `ctx_.isImported` (#4682), so the imported spelling keeps its
                // pre-existing unchecked behaviour — MEASURED on a real
                // two-module build with BOTH halves `--checked-contracts`:
                // `c.P ??= 7` gives 42 where `c.P := 7` on the identical
                // imported property gives 99. That divergence is #4705's, and
                // the same measurement shows it covers `OP=` and `++` too; it
                // is deliberately unchanged here, because closing it belongs in
                // the exporter's checked stub (§22.16's checked face, #4651),
                // not in this arm.
                const sema::TypeSymbol* invCls4732 =
                    invRecv4646 ? instrumentedAutoPropertyStoreClass(*as->target)
                                : nullptr;
                if (invCls4732) emitInvariantCheck(invRecv4646, invCls4732);
                // #429 / §4.6 / §28.9: `field ??= e` stores into a (currently
                // null) CLASS string field/auto-property only when null, so
                // there is no old value to release — but the field still takes
                // an INDEPENDENT +1 (transfer a producer / retain a borrow),
                // released on the owner's disposal. Restricted to class string
                // field lvalues. The slot is null here, so a plain store is
                // correct after the retain.
                if (isStringFieldLvalue(*as->target) &&
                    rhs->getType()->isPointerTy() &&
                    !isPlusOneStringProducer(as->value.get()))
                    emitStringRetain(rhs);
                // fix #4965 (§4.6 / §28.9 / §14.1, v0.307): the bare-`T`
                // field at `T = string` twin — always retain (see
                // lowerStringFieldStore); the slot is null here, so there is
                // no old value either way.
                else if (isTypeParamStringFieldLvalue(*as->target) &&
                         rhs->getType()->isPointerTy())
                    emitStringRetain(rhs);
                // fix #5245 (§4.6 / §28.9 / §6.6, v0.313): a `string?` LOCAL
                // that owns a +1 for its value (a classified producer/borrow
                // local, see stringLocalSlotOwnsRef) takes an independent +1
                // for the value `??=` stores into it — it is released at scope
                // end and by a later reassign, so a borrow stored without a
                // retain would dangle when its owner is overwritten. The slot
                // is null here (no old value). A +1 producer transfers.
                else if (rhs->getType()->isPointerTy() &&
                         stringLocalSlotOwnsRef(*as->target) &&
                         !isPlusOneStringProducer(as->value.get()))
                    emitStringRetain(rhs);
                // fix #5253 (§22.3 / §28.9, v0.313): a `ref` / `out` string
                // PARAMETER — the `??=` twin of lowerStringRefParamStore. The
                // caller's slot this aliases owns one reference for whatever
                // it holds and releases it (an `out var` recipient's scope-end
                // release, a borrow local's), so a borrow stored here without
                // a retain is released by the caller while its owner still
                // holds it (`r := null; r ??= c.Val;` SEGVed once `c.Val` was
                // overwritten). The slot is null here (no old value).
                else if (rhs->getType()->isPointerTy() &&
                         isStringExprType(*as->target) &&
                         dynamic_cast<const ast::IdentifierExpr*>(
                             as->target.get()) &&
                         refParams_.count(ctx_.exprSymbol(*as->target)) &&
                         !isPlusOneStringProducer(as->value.get()))
                    emitStringRetain(rhs);
                builder_.CreateStore(rhs, addr);
                // §22.16's "throw exits check nothing": a store sequence that
                // left the current block terminated (an unwind edge) must not
                // have an exit check appended, and appending one to a
                // terminated block is malformed IR besides. Mirrors #4646's
                // `:=` exit guard and #4682's compound one.
                if (invCls4732 && !builder_.GetInsertBlock()->getTerminator())
                    emitInvariantCheck(invRecv4646, invCls4732);
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
                return false;
            }
            // §9 / §9.1 (#1759): `slot := this` in a record method stores the
            // whole `this` value into a record-typed slot. `this` lowered to the
            // receiver POINTER (CG29 / §9.2); load the struct so value semantics
            // copy the record rather than storing the 8-byte pointer as the
            // slot's leading bytes (garbage read back as the record's fields). A
            // no-op for every RHS that is not the receiver pointer, and for a
            // slot whose type is not exactly the receiver record's struct.
            if (as->op == ":=")
                rhs = coerceThisRecordValue(rhs, elemTy);
            // §8.6: a value-typed RHS assigned to an Object/interface lvalue is
            // boxed (so a later is/as/dispatch on the target sees a box). Keyed
            // off the target's static type; a no-op for reference RHS (#399).
            if (as->op == ":=")
                rhs = boxForTarget(rhs, ctx_.exprType(*as->target),
                                   as->value.get());
            // §5.1: an int assigned to a wider numeric slot (e.g. `d := n;`
            // with `d: double`, `n: int`) is implicitly widened. Materialize
            // the conversion sema admitted so the slot receives the value, not
            // the source type's raw bits (issue #408). No-op when the rhs type
            // already matches or no §5.1 numeric widening applies.
            // issue #421 / #410: thread the RHS signedness so an unsigned source
            // (`d := someUint` with `d: double`) widens via UIToFP/ZExt, not the
            // signed default — otherwise a large uint would store as negative.
            // fix #5005 (§5.1 "Nullable conversions" — `S` converts to `T?` via
            // `S → T → T?` — v0.307): when the slot is a nullable-VALUE struct
            // `{i8 hasValue, T}` (e.g. `long?`), the width to widen against is
            // the PAYLOAD `T`, not the tagged struct itself — `slotPayloadTy`
            // is the one place that unwrap already lives (used by
            // `coerceInitializerToSlot` / `coerceExprValueToSlot` for the
            // declaration-initializer and return/argument paths). Before this,
            // `coerceNumericToType(rhs, elemTy, …)` was called with the WHOLE
            // struct type as the target — neither an integer nor a float type,
            // so every one of its §5.1 arms declined and `rhs` reached the
            // CG-nullable wrap block below still at its narrower width (e.g. a
            // bare `i32` against a `long?` slot's `i64` payload). The wrap
            // block's `rhs->getType() == st->getElementType(1)` check then
            // failed too, so the store fell through UNWRAPPED: a bare `i32`
            // stored at the struct's address overwrites only the leading
            // `hasValue` byte (plus padding), leaving the `i64` payload bytes
            // at their prior value (0 for a freshly-`null`-initialized local)
            // — MEASURED as `x := k;` (`k: int`, `x: long?`) printing `0`
            // instead of the widened `4`. The declaration-initializer form
            // (`var w: long? := k;`) was already correct — fix #839 widens
            // against the payload there — this mirrors that fix for
            // REASSIGNMENT. A no-op for every non-nullable / already-payload-
            // width / non-numeric `elemTy`, since `slotPayloadTy` returns
            // `elemTy` unchanged unless it IS the tagged struct shape.
            if (elemTy)
                rhs = coerceNumericToType(rhs, slotPayloadTy(elemTy),
                                          isUnsignedTargetExpr(*as->value));
            // fix #5017 (§5.1 "Nullable conversions", v0.307: "`S?`
            // implicitly converts to `T?` whenever `S` implicitly converts to
            // `T` — a present value converts by the underlying conversion,
            // `null` converts to `null`"): a NULLABLE-VALUE RHS of a narrower
            // payload (`x := ki;` with `ki: int?`, `x: double?`) lowers to the
            // `{i8, i32}` struct. The #5005 widening above is a no-op on a
            // struct, and the wrap block below matches only a bare payload,
            // so the `{i8, i32}` was stored over the `{i8, double}` slot —
            // MEASURED printing `lift 0` instead of `lift 4`. Convert the
            // payload and KEEP the source's hasValue (null stays null), the
            // same shape the declaration-initializer path uses (#840).
            if (elemTy)
                rhs = liftNullableValueToSlot(
                    rhs, elemTy, isUnsignedTargetExpr(*as->value));
            // CG-nullable: if target slot is a nullable-prim struct and
            // rhs is a plain primitive, wrap it (or fix null literal).
            if (elemTy && llvm::isa<llvm::StructType>(elemTy)) {
                if (auto* st = llvm::dyn_cast<llvm::StructType>(elemTy)) {
                    if (st->getNumElements() == 2 && st->getElementType(0) == nullablePrimHasValTy() &&
                        rhs->getType() != elemTy) {
                        if (rhs->getType()->isPointerTy() &&
                            llvm::isa<llvm::ConstantPointerNull>(rhs)) {
                            rhs = llvm::ConstantAggregateZero::get(elemTy);
                        } else if (rhs->getType() == st->getElementType(1)) {
                            llvm::Value* res = builder_.CreateInsertValue(
                                llvm::UndefValue::get(elemTy),
                                llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
                            rhs = builder_.CreateInsertValue(res, rhs, {1u});
                        }
                    }
                }
            }
            if (as->op == ":=") {
                // fix #4646 (§22.16, v0.307): the §22.16 AUTO-PROPERTY setter
                // ENTRY boundary. Emitted here — after the RHS has been lowered
                // and coerced ("after argument binding") and before ANY part of
                // the store sequence below runs — so the instance is observed
                // in exactly the state the caller handed over, mirroring
                // `lowerPropertySetValue`'s entry check (#4614(b)) and
                // `emitFunctionBody`'s public-method entry check.
                //
                // `invRecv4646` is non-null only when `lvalueAddr` resolved an
                // `obj.f`-shaped target through `fieldAddr` (a class receiver);
                // a shape whose receiver pointer never materialized degrades to
                // the pre-fix behaviour rather than checking over a null
                // pointer.
                invCls4646 = invRecv4646
                    ? instrumentedAutoPropertyStoreClass(*as->target)
                    : nullptr;
                if (invCls4646) emitInvariantCheck(invRecv4646, invCls4646);
                // §28.7 / §15.3 (#1385): whole-value reassignment of a move-only
                // OWNING inline-array local or field (`[N owned]T` / `[N owned]T?`)
                // disposes each non-null slot of the displaced value in reverse
                // slot order, BEFORE the new-value store. Runs first so the
                // subsequent per-slot / whole-struct stores overwrite the
                // correctly-disposed (null) slots. No-op for non-move-only-array
                // targets (the helpers below / the plain CreateStore handle them).
                // RHS is already evaluated (lines 377-388 above) so the disposal
                // runs after the RHS's side effects, matching §28.7's
                // "RHS-first, then dispose displaced, then store" ordering.
                // fix #3263: pass `as->wholeValueLhsMovedFrom` as `skipDisposal`
                // — sema set it when the LHS was whole-value moved-from by a
                // prior `var b := a;` (defect 2: stale alloca pointers now
                // belong to `b`; disposing them would double-free).
                lowerOwningInlineArrayReassign(
                    *as->target, addr, elemTy,
                    /*skipDisposal=*/as->wholeValueLhsMovedFrom);
                // fix #2778 / §28.6 / §29.7 (v0.NNN): a CLASS field or
                // auto-property typed as the pure (declaration-less)
                // `IEnumerable<T>` interface (built-in iterator format —
                // §29.11 vtable dispatch) must store a vtable-backed
                // enumerator, NOT the raw collection pointer. The local-decl
                // path (lowerLocalDecl ~12860) wraps an `Array<T>`/`Stack<T>`/
                // `Queue<T>`/`Set<T>`/`LinkedList<T>`/user-class source as a
                // vtable-backed wrapper before the slot init, and the return-
                // coercion path (wrapAsIEnumerableForTarget ~7555) wraps on
                // the RETURN edge; the field-store path was missing the same
                // primitive. Pre-fix the slot held a raw `bz_array_t*` /
                // `bz_llist_t*` / etc. whose first word is the backing's count
                // field, not a vtable pointer, and the §29.7 iterator-vtable
                // slot-1 Dispose at owner disposal read the count bits as a
                // function pointer → SIGSEGV. Mirror the local-decl /
                // return-coercion wrap at the explicit-assignment site. Gated
                // to a CLASS field/auto-property (NOT a local — that's
                // lowerLocalDecl; NOT a ref-param — `ref`/`out` aliases are
                // borrow-only, the wrap doesn't apply; NOT a static field —
                // program-lifetime storage, no slot-1 Dispose; NOT a record
                // field — records have no disposer, §9.7). A null RHS
                // passes through unchanged (wrap-creator returns null for a
                // null input); an already-`IEnumerable<T>` source passes
                // through unchanged (wrap-creator keys on the headName
                // mismatch). srcOwnedByCallee matches the return-coercion's
                // alias-rejection predicate (`returnSourceIsCalleeOwned
                // Collection`): a `new` / `move e` / producer-init local
                // source is the sole owner of the backing so the OWNING
                // wrap's slot-1 Dispose frees the backing at field disposal;
                // a named local / parameter / field source keeps the
                // BORROWING wrap, and the source's own binder still disposes
                // the backing at its own scope end (no double-free).
                if (rhs && as->value.get()) {
                    const sema::TypeRef* tt = ctx_.exprType(*as->target);
                    if (tt && isVtableBackedIEnumerableType(tt)) {
                        llvm::Type* dummy = nullptr;
                        if (!localSlot(*as->target) &&
                            !staticFieldGlobal(*as->target) &&
                            !refParamAddr(*as->target, dummy)) {
                            sema::Symbol* lhSym = ctx_.exprSymbol(*as->target);
                            bool isClassFieldOrProp = false;
                            if (auto* fs = dynamic_cast<sema::FieldSymbol*>(lhSym)) {
                                isClassFieldOrProp =
                                    fs->parent &&
                                    fs->parent->kind == ast::TypeDeclKind::Class;
                            } else if (auto* ms =
                                           dynamic_cast<sema::MemberSymbol*>(lhSym)) {
                                isClassFieldOrProp =
                                    sema::isAutoPropertyStorageSymbol(ms, ctx_) &&
                                    ms->parent &&
                                    ms->parent->kind ==
                                        ast::TypeDeclKind::Class;
                            }
                            if (isClassFieldOrProp) {
                                rhs = wrapAsIEnumerableForTarget(
                                    rhs, tt, as->value.get(),
                                    /*ownLazyPipeline=*/true,
                                    /*outWrapMade=*/nullptr,
                                    /*srcOwnedByCallee=*/
                                    returnSourceIsCalleeOwnedCollection(
                                        as->value.get()));
                            }
                        }
                    }
                }
                // #429 / §4.6 / §28.9: a store into a CLASS string field /
                // auto-property takes an independent +1 (retain-new or transfer-
                // producer / release-old), released on the owner's disposal.
                // Reassigning a string LOCAL slot likewise retains-new / releases-
                // old so the slot keeps an independent +1 (a later field disposal
                // that releases the same string cannot then dangle the local — no
                // UAF). Local reassignment refcounting is gated on the analyzed
                // function (stringReleaseEligibleFn_) so it matches the bind-time
                // arming convention; a lambda/thunk body (not analyzed) keeps the
                // plain store. Both helpers return true when they handled the
                // store; otherwise fall through to the plain store (array
                // elements, ref params, static / record fields, string? slots,
                // and non-string targets are unchanged).
                bool handled =
                    lowerStringFieldStore(*as->target, addr, rhs,
                                          as->value.get());
                if (!handled && currentFn_ == stringReleaseEligibleFn_)
                    handled = lowerStringLocalReassign(*as->target, addr, elemTy,
                                                       rhs, as->value.get());
                // #2321 / §22.3 / §28.9: a `ref`/`out` string param has no
                // localSlot (lives in refParams_) and is not a class field, so
                // the helpers above both returned false — without this the
                // callee's `s := s + "!"` plain-stored the new heap string and
                // the caller's previous value's +1 leaked (and so did the new
                // value's +1 in the caller's slot, for a different reason — the
                // local-slot escape walk does not cover ref-aliased slots).
                // Mirror lowerStringLocalReassign's retain-new (transfer a
                // producer) / store / release-old. Gated on the helper returning
                // true (which requires a ref-param, string-typed, non-nullable,
                // bare-ptr target) so a non-ref-param target cannot accidentally
                // reach here.
                if (!handled)
                    handled = lowerStringRefParamStore(*as->target, addr, elemTy,
                                                       rhs, as->value.get());
                // #429 (closure FIELD-STORE category) / §27.5 / §28.9: a store
                // into a CLASS closure (function-typed) field / auto-property
                // takes an independent +1 (retain-new or transfer-producer /
                // release-old), released on the owner's disposal.  Parallel to
                // lowerStringFieldStore above.
                if (!handled)
                    handled = lowerClosureFieldStore(*as->target, addr, rhs,
                                                     as->value.get());
                // fix #4174: closure-typed LOCAL/parameter reassignment —
                // the closure twin of lowerStringLocalReassign just above
                // (internal refcounting-soundness fix; see that function's
                // own comment and the regression test file's header for why
                // no spec section is cited as authority for the mechanism).
                // Without this, `f := lambda() => 42;` plain-stores the new
                // closure and the DISPLACED previous value (the producer +1
                // a prior binding transferred into the slot) is never
                // released — a 40-byte-per-reassignment leak (issue #4174,
                // LSan-measured). Gated on the analyzed function
                // (closureReleaseEligibleFn_) exactly as the string twin is
                // gated on stringReleaseEligibleFn_. A PARAMETER target is
                // sound here only because retainAssignedClosureParams (fix
                // #4174 follow-up, review NEEDS-FIX) gives it its own
                // independent +1 at bind before this release can ever run
                // against it — see that function's comment.
                if (!handled && currentFn_ == closureReleaseEligibleFn_)
                    handled = lowerClosureLocalReassign(*as->target, addr,
                                                        elemTy, rhs,
                                                        as->value.get());
                // fix #4227: captured-cell twin of lowerClosureLocalReassign above.
                // A captured closure-typed local (its cell slot is in cellVars_)
                // reassigned from INSIDE the capturing lambda body reaches here with
                // addr = the heap cell address (from boxedStorage in lvalueAddr),
                // but lowerClosureLocalReassign's own currentFn_ ==
                // closureReleaseEligibleFn_ gate is false (the analyzed function is
                // the outer scope, not this lambda body), so it returns false and
                // the store falls through to a plain store with no release — the
                // displaced closure value leaks. The new helper handles this
                // captured-cell path unconditionally (mirrors lowerClosureFieldStore
                // / lowerClosureLocalReassign's retain-before-release ordering).
                // No null guard on slot in cellVars_ needed: the capture check
                // itself proves the cell exists.
                if (!handled) {
                    if (llvm::AllocaInst* slot = localSlot(*as->target)) {
                        if (cellVars_.count(slot)) {
                            const sema::TypeRef* tt = ctx_.exprType(*as->target);
                            if (tt && tt->kind == sema::TypeRef::Kind::Function &&
                                !tt->nullable) {
                                handled = lowerCapturedClosureLocalReassign(
                                    *as->target, addr, elemTy, rhs,
                                    as->value.get());
                            }
                        }
                    }
                }
                // #429 (box FIELD-STORE category) / §8.6 / §28.9: a store into
                // a CLASS Object/interface field whose every store (per the
                // collectBoxStorageFields pre-pass) uses a boxable value source
                // takes an independent +1 (retain-new or transfer-producer /
                // release-old), released on the owner's disposal. Parallel to
                // lowerClosureFieldStore above. Gated on isBoxFieldLvalue which
                // checks both the target type (Object/interface class field) and
                // the source type (boxable value or box-return call), so a
                // class-reference store to an Object field never reaches here.
                if (!handled)
                    handled = lowerBoxFieldStore(*as->target, addr, rhs,
                                                 as->value.get());
                // #824 / §28.5 / §28.9: a store into a CLASS `owned` collection
                // field (OwningArray<T>/OwningStack<T>/OwningQueue<T>/OwningSet<T>/
                // OwningLinkedList<T>/OwningBuffer<T>/OwningDictionary<T> etc.)
                // must dispose the old collection (and its owned elements) BEFORE
                // overwriting the pointer. Without this the old pointer is lost
                // and the old collection leaks. Mirrors the string/closure/box
                // field-store family above; bz_array_free/bz_llist_free are
                // null-safe so the first assignment (ctor-init, null old value)
                // is handled correctly without an explicit guard.
                if (!handled)
                    handled = lowerOwnedCollectionFieldStore(*as->target, addr,
                                                             rhs, as->value.get());
                // §28.7 (#1328): drop-on-overwrite for owned class/interface
                // FIELDS — the field analog of disposeDisplacedOwnedClassLocal.
                // Loads the old object pointer, applies null+identity guards (so a
                // moved-from slot or a self-assign never disposes), then dispatches
                // the old object's Dispose and reclaims its block. The new value is
                // stored after disposal. Must come AFTER the collection helper (which
                // handles OwningArray<T> etc. via bz_array_free, not emitDisposeAndReclaim)
                // and BEFORE the local helper (which only fires for plain locals).
                if (!handled)
                    handled = lowerOwnedClassFieldStore(*as->target, addr, rhs);
                // §28.7 / §30.5 (#2173): drop-on-overwrite for owned Task-family
                // FIELDS — the field analog of the using-var task drop. Mirrors
                // lowerOwnedClassFieldStore's shape with two crucial differences
                // (see the Setup.cpp body): the store-new happens BEFORE the
                // dispose (a task's bz_task_fire_and_forget completion continuation
                // can re-enter the slot on this thread), and the disposal is the
                // FULL using-var task protocol via emitOwnedTaskDisposal (error_obj
                // prelude + per-T result_disposer + fire-and-forget[_checked]) —
                // not a plain emitDisposeAndReclaim. Must come AFTER the class-
                // field helper (which would mis-handle Task-family: its
                // isOwnedClassFieldLvalue check excludes Task-family because the
                // builtin types have no user TypeDecl / no vtable) and BEFORE the
                // local helper (which only fires for plain locals).
                if (!handled)
                    handled = lowerOwnedTaskFieldStore(*as->target, addr, rhs);
                if (!handled) {
                    // fix #2960 / §8.6 / §28.9 (v0.240): drop-on-overwrite
                    // for a managed Object/interface LOCAL. The pre-pass admits
                    // only non-escaping locals whose stores have statically known
                    // fresh-box vs definitely-non-box provenance. lowerLocalDecl
                    // registered an i1 state bit beside the slot; if it says the
                    // displaced pointer is a box, release that box now (RHS has
                    // already fully evaluated and been boxed), before overwriting.
                    // A class/string/null source records false after the store, so
                    // neither this site nor scope exit can hand its pointer to
                    // bz_box_release. An unknown source defensively suppresses the
                    // old release and clears the bit (safe leak) even though the
                    // pre-pass normally excludes it.
                    llvm::AllocaInst* boxState = nullptr;
                    if (llvm::AllocaInst* local = localSlot(*as->target)) {
                        for (auto scopeIt = disposeStack_.rbegin();
                             scopeIt != disposeStack_.rend() && !boxState;
                             ++scopeIt) {
                            for (auto dispIt = scopeIt->rbegin();
                                 dispIt != scopeIt->rend(); ++dispIt) {
                                if (dispIt->slot == local && dispIt->boxRelease) {
                                    boxState = dispIt->boxReleaseState;
                                    break;
                                }
                            }
                        }
                    }
                    if (boxState) {
                        // Smear the state init to the assignment block in
                        // case the local was declared with a non-box initializer.
                        // lowerLocalDecl already stored the starting value into
                        // the state bit; nothing to do here on that front. The
                        // state is entry-block relative, so the assignment's
                        // basic block inherits the store.
                    }
                    // fix #2900 / §28.9 / §28.3 (v0.240): the object-string-
                    // release twin of the box-state lookup above. An
                    // Object/interface local in `objectStringBorrowAll_` whose
                    // state bit was registered at lowerLocalDecl gets a
                    // drop-on-overwrite bz_string_release of the displaced
                    // old value (gated on the state bit saying the slot is
                    // currently a string) and a state-bit update from the new
                    // source's static kind. Mirrors the box arm exactly so the
                    // state bits cannot drift in opposite directions.
                    llvm::AllocaInst* ostrState = nullptr;
                    if (llvm::AllocaInst* local = localSlot(*as->target)) {
                        for (auto scopeIt = disposeStack_.rbegin();
                             scopeIt != disposeStack_.rend() && !ostrState;
                             ++scopeIt) {
                            for (auto dispIt = scopeIt->rbegin();
                                 dispIt != scopeIt->rend(); ++dispIt) {
                                if (dispIt->slot == local &&
                                    dispIt->objectStringRelease) {
                                    ostrState = dispIt->objectStringReleaseState;
                                    break;
                                }
                            }
                        }
                    }
                    int newBoxKind = boxState
                        ? boxLocalAssignmentSourceKind(as->value.get())
                        : -1;
                    if (boxState && newBoxKind >= 0) {
                        llvm::Value* oldIsBox = builder_.CreateLoad(
                            llvm::Type::getInt1Ty(llvm_), boxState,
                            "box.reassign.old.armed");
                        auto* releaseBB = llvm::BasicBlock::Create(
                            llvm_, "box.reassign.release", currentFn_);
                        auto* contBB = llvm::BasicBlock::Create(
                            llvm_, "box.reassign.store", currentFn_);
                        builder_.CreateCondBr(oldIsBox, releaseBB, contBB);
                        builder_.SetInsertPoint(releaseBB);
                        llvm::Value* oldBox = builder_.CreateLoad(
                            ptrTy(), addr, "box.reassign.old");
                        emitBoxRelease(oldBox);
                        builder_.CreateBr(contBB);
                        builder_.SetInsertPoint(contBB);
                    }
                    // The new source kind for the object-string state bit.
                    // A bare string-typed identifier (the only shape admitted
                    // at the LocalDecl level) ⇒ true; everything else ⇒ false.
                    // This is the SAME test `isObjectStringSource` (declared
                    // below) runs; reusing it is the only way the bit update
                    // and the bind site can stay in lockstep.
                    int newOstrKind = ostrState
                        ? isObjectStringSource(as->value.get())
                        : -1;
                    if (ostrState && newOstrKind >= 0) {
                        llvm::Value* oldIsStr = builder_.CreateLoad(
                            llvm::Type::getInt1Ty(llvm_), ostrState,
                            "ostr.reassign.old.armed");
                        auto* releaseBB = llvm::BasicBlock::Create(
                            llvm_, "ostr.reassign.release", currentFn_);
                        auto* contBB = llvm::BasicBlock::Create(
                            llvm_, "ostr.reassign.store", currentFn_);
                        builder_.CreateCondBr(oldIsStr, releaseBB, contBB);
                        builder_.SetInsertPoint(releaseBB);
                        llvm::Value* oldStr = builder_.CreateLoad(
                            ptrTy(), addr, "ostr.reassign.old");
                        emitStringRelease(oldStr);
                        builder_.CreateBr(contBB);
                        builder_.SetInsertPoint(contBB);
                    }
                    // §28.7 (#1054): reassigning an owned class local must dispose
                    // the displaced old value before overwriting (the class analog
                    // of the string/closure/box/owned-collection field-store helpers
                    // above). No-op for a borrow local/param, an array element, a
                    // ref param, a static/record field, or a non-class target (none
                    // has an owned-class-local Disposal entry → no double-free).
                    // fix #3950 (§28.7 v0.294 / §28.3 K1.3): SKIP the pass when
                    // the ownership dataflow proved the LHS local MOVED-FROM at
                    // this store. §28.7: the displaced value is disposed only
                    // "if the previous value is live (non-null and not
                    // moved-from)" — "A moved-from or null location therefore
                    // stores without disposal". The helper below tests only the
                    // NULL half; a plain `var` local's slot is NOT nulled by the
                    // `move` lowering, so without this gate the pass disposed a
                    // pointer the move's receiver had already freed (measured
                    // heap-use-after-free, straight-line, no diagnostic).
                    // // per §28.7, §28.3 K1.3, v0.294, #3950
                    if (!as->lhsMovedFromAtStore)
                        disposeDisplacedOwnedClassLocal(as->target.get(), addr,
                                                        elemTy, rhs);
                    // fix #4552 / §35.4 *Drop-on-overwrite is defined and
                    // automatic* + §35.9 (v0.306): the owning-CHOICE sibling of
                    // the class drop above. "Whole-value reassignment of an
                    // owning-choice local or field disposes the displaced value
                    // per §28.7"; `o := null` on an owned-state `C?` location
                    // does the same. A choice is a VALUE type, so the class
                    // helper's `unwindOwnedLocalClass` gate declines it and no
                    // other path ever saw the displaced value — the scope-end
                    // active-case pass only sees whatever the slot holds LAST.
                    // Same §28.7 moved-from suppression as the class sibling,
                    // and the same position in the sequence (RHS already
                    // lowered into `rhs`; the store is emitted below).
                    if (!as->lhsMovedFromAtStore)
                        disposeDisplacedOwningChoiceLocal(as->target.get(), addr,
                                                          as->value.get());
                    // #914: a narrowed value-nullable RHS (sema proved non-null)
                    // assigned to a non-nullable target — unwrap the {flag,value}
                    // struct to its value. No-op for a nullable target (elemTy is
                    // the struct, not its value element).
                    rhs = unwrapNarrowedNullable(rhs, elemTy);
                    llvm::StoreInst* si = builder_.CreateStore(rhs, addr);
                    // fix #4566 / §15.3 *Local-to-local assignment* + §28.7
                    // *Moved-from and null* (v0.306): the MOVE half of the
                    // drop-on-overwrite pass above. An owning choice is a
                    // move-only value type, so `c := b;` TRANSFERS — "After
                    // the move, `x` is in moved-from state" — and §15.3's
                    // *Local lifetime* bullet says the source then "holds no
                    // value", so "no pass walks it". Codegen only ever
                    // plain-stored the struct, leaving BOTH slots holding the
                    // same owned payload pointer for BOTH scope-end
                    // active-case passes to dispatch `Dispose` through (a
                    // measured SIGSEGV / ASan heap-use-after-free). Emitted
                    // AFTER the store, so the value the store read out of the
                    // source slot is already in the target's.
                    // // per §15.3, §28.7, v0.306, #4566
                    markOwningChoiceLocalMovedFrom(as->target.get(),
                                                   as->value.get());
                    // fix #4068 / §28.3 / §28.9 / stdlib §5 (v0.295): the
                    // path-sensitive walker change in `collectBorrowingArray
                    // FreeEligible` (LlvmBackend_Setup.cpp ~24423 / ~28134)
                    // admits a `var items := seq.Sorted();` whose ONE
                    // escaping use is confined to a non-taken branch. The
                    // scope-end `bz_array_free` is now emitted on the
                    // unescaped branches — but on the consuming branch the
                    // local's slot still holds the array pointer (which the
                    // OWNED field now ALSO owns), so a scope-end free on the
                    // slot would double-free with the field's later
                    // destructor-side free (measured SIGSEGV in ASan after
                    // the walker change alone, in `Holder.DisposeCore`
                    // freeing the already-freed `this.Data` array). Pair the
                    // walker change with this null: for an `:=` whose RHS is
                    // a borrowing-`Array`-materializer local admitted to
                    // `borrowingArrayFreeEligible_`, null the RHS slot right
                    // after the store so `bz_array_free` on the slot at scope
                    // end is a no-op (the runtime's `bz_array_free` checks
                    // `if (!arrp) return;` first). Gated on the
                    // `borrowingArrayFreeEligible_` membership (so non-
                    // materializer-bound locals — which the path-
                    // insensitive walker still suppresses — are untouched)
                    // and on `borrowingArrayFreeEligibleFn_ == currentFn_`
                    // (so a lambda body lowered mid-function doesn't fire
                    // this gate against the OUTER function's analysis).
                    if (currentFn_ == borrowingArrayFreeEligibleFn_ &&
                        as->op == ":=" && as->value) {
                        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                as->value.get())) {
                            sema::Symbol* asym = ctx_.exprSymbol(*id);
                            auto* loc = asym ?
                                dynamic_cast<sema::LocalSymbol*>(asym) :
                                nullptr;
                            if (loc && loc->decl &&
                                borrowingArrayFreeEligible_.count(
                                    static_cast<const void*>(loc->decl))) {
                                nullTransferredOwnedLocalSlot(as->value.get());
                            }
                        }
                    }
                    if (boxState)
                        builder_.CreateStore(
                            builder_.getInt1(newBoxKind > 0), boxState);
                    if (ostrState && newOstrKind >= 0)
                        builder_.CreateStore(
                            builder_.getInt1(newOstrKind > 0), ostrState);
                    // §22.12 rule 2 store atomicity (v0.181, #1556): a store into
                    // a static REFERENCE field is a single pointer-word write the
                    // runtime performs atomically — no torn reference is ever
                    // observed by a racing first-use on another domain (the check-
                    // then-store sequence itself is deliberately NOT atomic;
                    // §22.12). Emit it as an aligned atomic monotonic pointer store
                    // (relaxed: the spec promises no-tearing, not cross-domain
                    // ordering). Scope: any pointer-typed static field — the
                    // admitted mutable statics rule 2 targets, and incidentally the
                    // write-once readonly/owned reference statics (harmless, a
                    // single-word aligned atomic store is functionally identical).
                    // Primitive-scalar statics and every non-static target (locals,
                    // instance/record fields, ref params) keep the plain store.
                    if (rhs->getType()->isPointerTy() &&
                        staticFieldGlobal(*as->target)) {
                        si->setAlignment(module_->getDataLayout().getABITypeAlign(
                            rhs->getType()));
                        si->setAtomic(llvm::AtomicOrdering::Monotonic);
                    }
                    // §27.5 / #673: guarded-boxed (non-for-var) locals keep the
                    // original pre-boxing alloca in sync so pre-emitted code that
                    // loads from it (e.g. a while-loop condition) sees the update.
                    if (llvm::AllocaInst* ns = localSlot(*as->target))
                        syncCellToOrigSlot(ns, rhs);
                    // §28.3 / §28.10 (v0.180, #1978): a bare-identifier-target
                    // `y := owned x;` assignment marker (#1965) MOVES the named
                    // owned source `x` into `y`. NULL the source slot at the
                    // transfer so the source's now-ELIGIBLE (UnwindIneligibleWalker
                    // no longer taints it, #1978) null-guarded unwindOnly disposal
                    // is exactly-once — a throw AFTER this store finds the null
                    // source slot and skips it (no double-free with `y`'s own
                    // unwind disposal), while a throw that PRECEDES the transfer
                    // still finds `x` live and disposes it once (fixes the #1978
                    // leak). handleMarkedOwnedAssignment routes only a bare-id
                    // tracked-local target (a member/index target keeps the marker
                    // inert — NOT nulled here, so the walker keeps it tainted); a
                    // self-transfer `x := owned x` is rejected earlier, but the
                    // symbol-identity guard makes doubly sure a just-stored live
                    // value is never nulled. nullTransferredOwnedLocalSlot self-
                    // gates to the owned local/param slots the walker keeps
                    // eligible, so `eligible ⟹ nulled` stays EXACT. // per §28.3
                    if (as->valueHasOwnedMarker &&
                        dynamic_cast<const ast::IdentifierExpr*>(
                            as->target.get()) &&
                        ctx_.exprSymbol(*as->target) !=
                            ctx_.exprSymbol(*as->value))
                        nullTransferredOwnedLocalSlot(as->value.get());
                }
                // fix #5218 (§28.3 K7.4 / K1.4, v0.313): a bare `owned`
                // PARAMETER stored into an `owned` field transfers its
                // obligation into the field. The ownership pass models that
                // as a discharge of the parameter (handleOwnedFieldAssignment),
                // and when the store sits on only ONE branch the join with the
                // untouched path leaves the parameter flagged for the §28.3
                // method-end auto-drop (`autoDropAtScopeEnd`). The spec makes
                // that drop PER-PATH ("scope-end disposal is per-path", K1.4 /
                // K7.4: a transferred value is in moved-from state "so the
                // scope-end disposal skips it"), so null the parameter slot at
                // the transfer, exactly as the call-argument and collection-Add
                // transfers do: the null-guarded auto-drop then skips the
                // moved path and still disposes the no-store path. Without
                // this the drop freed the object the field now owns
                // (heap-use-after-free). Self-gated: only an `owned`
                // parameter source into ANY owned field the container disposes
                // (fix #5274: not only class/collection/task, also vtable-less
                // builtins, Shared/Weak, Subscription, event sources, FFI buffers).
                if (as->value && !as->valueHasOwnedMarker) {
                    if (auto* vid = dynamic_cast<const ast::IdentifierExpr*>(
                            as->value.get())) {
                        auto* vps = dynamic_cast<sema::ParameterSymbol*>(
                            ctx_.exprSymbol(*vid));
                        if (vps && vps->modifier == "owned" &&
                            isOwnedFieldStoreTarget(*as->target))
                            nullTransferredOwnedLocalSlot(as->value.get());
                    }
                }
                // fix #4646 (§22.16, v0.307): the §22.16 AUTO-PROPERTY setter
                // NORMAL-EXIT boundary. This is the end of the whole `:=` store
                // sequence, so the instance is observed in the state the store
                // left it — the auto-property analogue of `lowerPropertySetValue`'s
                // `accEndBB` check (#4614(b)) and of the public-method
                // `finalizeEnsuresEpilogue` check.
                //
                // The terminator guard is §22.16's "throw exits check nothing":
                // a store path that left the current block terminated (an
                // unwind edge) must not have an exit check appended to it, and
                // appending one to a terminated block is malformed IR besides.
                if (invCls4646 && !builder_.GetInsertBlock()->getTerminator())
                    emitInvariantCheck(invRecv4646, invCls4646);
            } else {
                // fix #610 / §25.1 / §6.6: compound assignment `target OP= value`
                // whose underlying binary operator resolves to a user-defined
                // `static operator <op>` — desugar mechanically to
                //   target := operator_op_(target, value)
                // (per §25.1: "the compound forms desugar mechanically to
                // read-modify-write using the underlying binary operator's
                // overload"). Must be checked BEFORE the built-in
                // emitCompoundOp path, which cannot handle struct operands and
                // would SIGABRT by passing the struct type to
                // llvm.sadd.with.overflow.
                //
                // fix #2156 / §25.2 (v0.226): the operator may be declared on
                // EITHER operand's type — §25.2's candidate set is "the union
                // of the `operator <op>` overloads declared on the static types
                // of BOTH operands" with "parameters match operands in order"
                // (no commutation). The former LHS-only search missed the
                // primitive-LHS / record-RHS shape — e.g. `n *= s` where
                // `operator *(int, Scale): int` lives on `Scale`: the primitive
                // target found no overload and fell through to emitCompoundOp,
                // which fed the record struct to llvm.smul.with.overflow.i32 →
                // malformed IR (IR verification failed). Search the VALUE's type
                // too when the target's type declares no applicable overload.
                //
                // fix #2904 (§25.1/§25.2/§6.6, v0.240): the ~50-line scan that
                // stood here is now `findCompoundOperatorOverload`, because it
                // was MISSING from every accessor-mediated caller (block-form
                // property, virtual/interface property, user indexer, imported
                // property) — each of which therefore aborted the compile on
                // `llvm.sadd.with.overflow.s_record.Pairs` for a legal program.
                // One definition; this site is now the plain call.
                //
                // fix #3924 (§6.6/§7.11/§17.6, v0.288): #3830 made sema read the
                // §7.4-NARROWED type of a compound-assignment RHS instead of its
                // declared type — so `n: int; m: int? := 3; n += m;` is now
                // ADMITTED (m's narrowed static type is `int`). Narrowing is a
                // purely static/sema fact (§7.4): it does NOT change `m`'s
                // *storage* representation, which stays the declared `{i8
                // hasValue; T payload}` tagged struct. Every branch below
                // (opMethod dispatch, the decimal arm, and the built-in CG31
                // numeric arm) assumes `rhs` already has the OPERAND's natural
                // scalar/struct shape — exactly what the shared preamble's
                // `coerceNumericToType(rhs, elemTy, …)` above is a no-op for
                // (its `srcTy->isIntegerTy()` guards all fail on a struct). Left
                // unwrapped, the tagged struct reached `llvm.sadd.with.overflow`/
                // `CreateFAdd` (an IR-verifier abort for the scalar family — "Call
                // parameter type does not match function signature!") or, for a
                // `decimal` target, was handed whole to `emitDecimalArith`, which
                // blindly stores it into a bare-decimal-shaped alloca — a SILENT
                // wrong value, not a diagnostic.
                //
                // The unwrap must be keyed on the RHS OPERAND's own narrowed
                // shape/sema-type, not on the TARGET's type (unlike the sibling
                // plain-assignment fix at #914 a few lines up, which correctly
                // uses `unwrapNarrowedNullable(rhs, elemTy)` because there the
                // target and RHS are always the SAME primitive type). A compound
                // assignment admits a WIDER target than the RHS payload
                // (`decimal += int?`, §5.1/§6.4) — `elemTy` there is
                // `decimalStructType()` while the narrowed RHS's payload is
                // `i32`, so a target-keyed match (`unwrapNarrowedNullable`) would
                // silently fail to fire and leave the struct in place.
                // `unwrapNarrowedNullableOperand` keys on `as->value`'s own sema
                // type (`isNullablePrimTy`) instead, exactly mirroring the
                // already-correct nullable-VALUE-TARGET arm inside
                // `emitCompoundOp` (LlvmBackend_Layout.cpp's `rPay =
                // coerceNumericToType(rPay, decimalStructType(), rUns)` at the
                // #2149 site) — reusing the SAME helper every other
                // narrowed-operand call site in this file already uses (§6 of
                // the lane rules: no second spelling of "is this a narrowed
                // nullable-primitive operand").
                //
                // A no-op for every already-correct case: `++`/`--` (synthetic
                // literal RHS, never a struct), a genuinely-nullable (un-narrowed)
                // RHS (sema rejects those upstream — #3830's own tests), and a
                // compound assignment whose TARGET is itself nullable (that shape
                // is unaffected: `elemTy` there is the tagged struct, so the RHS
                // arrives already correctly shaped for `emitCompoundOp`'s
                // existing line-370 nullable-VALUE-LEFT-OPERAND arm).
                rhs = unwrapNarrowedNullableOperand(rhs, as->value.get());
                // fix #4682 (§22.16 / §6.6, v0.307): the §22.16 AUTO-PROPERTY
                // setter boundaries for a COMPOUND store `obj.P OP= v`.
                //
                // §6.6 makes `x op= e` a read-modify-write "equivalent to
                // `x := x op e`", so the store half is a setter invocation and
                // sits on the same §22.16 boundary #4646 instrumented for the
                // plain `:=`: "Entry and normal exit of every `public` setter
                // — block-form, auto-property, indexer, `set(owned)` alike."
                // The READ half is a getter, which §22.16 exempts by name.
                // MEASURED on this HEAD before this fix, with
                // `--checked-contracts`, on the issue's own repro
                // (`invariant(P > 0);`, `c.P -= 4;` from P == 1): 42, where
                // §22.16 requires ContractViolationException (99) — the same
                // SILENT UNDER-CHECK #4646 closed one arm over.
                //
                // `invRecv4646` is the receiver `lvalueAddr` already lowered
                // for the field store (the shared preamble threads the
                // out-parameter for BOTH the `:=` and the compound arm), so
                // the receiver expression of an `f().P -= 1` is evaluated
                // exactly ONCE — the specific hazard a read-modify-write adds
                // over a plain store, and pinned by its own row.
                //
                // WHY TWO LAMBDAS AND NOT ONE HOISTED PAIR. This arm has FOUR
                // store paths — the §25.1/§25.2 user-operator desugaring, the
                // §4.6 string concat, the §6.4 decimal helper, and the CG31
                // built-in numeric one — each with its own `return false`, and
                // an auto-property can be typed to reach any of them. The
                // ENTRY check belongs after the operand is computed, not
                // before: §22.16's evaluation table binds arguments before any
                // clause runs, §6.6's desugared argument IS `x op e`, and
                // #4646 placed the `:=` entry check after the RHS is lowered
                // and coerced for exactly that reason. So the pair cannot be
                // hoisted out of the four paths without changing which
                // exception a compound assignment that BOTH overflows and
                // violates the invariant raises (§6.1's overflow must win, as
                // it does pre-fix).
                const sema::TypeSymbol* invCls4682 =
                    invRecv4646 ? instrumentedAutoPropertyStoreClass(*as->target)
                                : nullptr;
                auto invEntry4682 = [&] {
                    if (invCls4682) emitInvariantCheck(invRecv4646, invCls4682);
                };
                // The terminator guard is §22.16's "throw exits check
                // nothing": a store path that left the current block
                // terminated (an unwind edge) must not have an exit check
                // appended to it, and appending one to a terminated block is
                // malformed IR besides. Mirrors #4646's `:=` exit guard.
                auto invExit4682 = [&] {
                    if (invCls4682 && !builder_.GetInsertBlock()->getTerminator())
                        emitInvariantCheck(invRecv4646, invCls4682);
                };
                std::string op(as->op);
                if (!op.empty() && op.back() == '=') op.pop_back();
                sema::MethodSymbol* opMethod = findCompoundOperatorOverload(
                    op, ctx_.exprType(*as->target), ctx_.exprType(*as->value),
                    ctx_.stmtOperatorSymbol(*as));
                if (opMethod) {
                    // §25.1 / §25.2: dispatch to the user-defined binary
                    // operator with args in source order {target, value} (the
                    // §25.2 in-order match). Load the current target value
                    // (struct by value or primitive scalar) and store the
                    // result back.
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    std::vector<llvm::Value*> args{cur, rhs};
                    llvm::Value* res = emitMethodCall(
                        opMethod, args, /*baseQualified=*/false);
                    if (res) {
                        // §28.7 / §17.6 (#1831): a user-defined compound
                        // `target OP= value` (e.g. `_c += k` where
                        // `operator +` returns a fresh owned class) desugars
                        // to `target := operator(target, value)` (§25.1), and
                        // the store back must DROP-ON-OVERWRITE the displaced
                        // value exactly like the plain `:=` path — for an
                        // OWNED class/interface FIELD, route through
                        // lowerOwnedClassFieldStore (the field analog of the
                        // local disposal below, mirroring the #1702 ++/-- fix
                        // in the unary branch). For an OWNED class LOCAL,
                        // fall through to disposeDisplacedOwnedClassLocal +
                        // a plain store (matching §28.7's "RHS first, dispose
                        // displaced, store new" ordering: the operator already
                        // borrowed `cur`, so the disposal of the old value
                        // runs after the operator returns, before the new
                        // value is stored). Without this the displaced
                        // value leaks (a leak per `+=` invocation — e.g. the
                        // old `_c` and the `new Counter(...)` operator
                        // result every time `_c += k` reassigns the field).
                        // Borrow / value / ref / static / record / indexer
                        // targets keep the plain store (no drop-on-overwrite
                        // entry — no double-free).
                        invEntry4682();  // §22.16 setter ENTRY (#4682)
                        if (!lowerOwnedClassFieldStore(*as->target, addr, res)) {
                            // fix #3950 — ⚠ STILL UNREACHABLE, for a DIFFERENT
                            // reason since fix #3969; kept deliberately.
                            // This is the same §28.7 moved-from gate as the
                            // plain `:=` path above. When #3950 landed it was
                            // unreachable because `lhsMovedFromAtStore` is only
                            // ever WRITTEN by MethodOwnership::handleAssignment,
                            // which returned at its second line for
                            // `as.op != ":="`, so a compound `op=` never
                            // reached the writer at all.
                            //
                            // #3969 gave the compound form a pass through
                            // `handleAssignment` — but deliberately only the
                            // READ-CHECKING half, not the `:=` state
                            // transitions, and it did NOT add a
                            // `lhsMovedFromAtStore` writer. The gate stays a
                            // no-op, now because the read-check REJECTS every
                            // program in which it could be true: §6.6 makes
                            // `x op= e` equivalent to `x := x op e`, so the
                            // target is a read position, and a read of a
                            // moved-from local is a hard §28.3 K1.3 error
                            // ("owned value in `a` was already moved out at
                            // this point") — the driver emits no object for it
                            // (verified: `bezelc -o out` on the #3969 repro
                            // exits 1 and writes no file). Writing the flag
                            // here would be an unobservable store mirroring an
                            // unreachable gate, so #3969 did not add one.
                            //
                            // The gate is kept, not deleted, because it states
                            // §28.7's rule correctly for this store ("if the
                            // previous value is live — non-null and NOT
                            // moved-from — it is disposed"; "A moved-from or
                            // null location therefore stores without
                            // disposal"). If the moved-from read is ever
                            // demoted from error to warning — as §28.2's
                            // use-after-DISPOSE read already is — this path
                            // becomes live and is already right.
                            // // per §28.7, §6.6, §28.3 K1.3, v0.294, #3950, #3969
                            if (!as->lhsMovedFromAtStore)
                                disposeDisplacedOwnedClassLocal(
                                    as->target.get(), addr, elemTy, res);
                            builder_.CreateStore(res, addr);
                        }
                        // §27.5 / #673: sync compound-assignment result back
                        // to the original alloca for guarded-boxed locals
                        // (a no-op for a field target — localSlot returns
                        // null for a non-local).
                        if (llvm::AllocaInst* ns = localSlot(*as->target))
                            syncCellToOrigSlot(ns, res);
                        invExit4682();  // §22.16 setter NORMAL EXIT (#4682)
                    }
                    return false;
                }
                // §4.6 (#857): string compound concatenation `s += t`. A string
                // is a pointer, not a number — emitCompoundOp below would feed the
                // string pointers to the integer checked-add intrinsic (malformed
                // IR). When the target is a string, `+=` is a concat: mirror the
                // working `s := s + t` path (emitStringConcat + the `:=`
                // retain/release discipline). The concat result is a
                // +1 producer, transferred into a string LOCAL/FIELD slot with the
                // old value released (other lvalues — array element, static, ref/
                // out, `string?` — get a plain store, leak-safe as before).
                // §4.6/§6.6 (#2157): sema now enforces that a `string` target's
                // `+=` RHS is itself a `string` (the retired `string + T`
                // conversion rule was removed — `s += 5` is now a §4.6 compile
                // error, matching `s + 5`). The non-string RHS branch below is
                // therefore defensive-only (unreachable for type-correct input);
                // it is retained so a non-string RHS that reaches codegen by a
                // future regression still renders rather than corrupting the
                // concat.
                if (op == "+") {
                    const sema::TypeRef* tgtTy = ctx_.exprType(*as->target);
                    if (tgtTy &&
                        tgtTy->kind == sema::TypeRef::Kind::Primitive &&
                        tgtTy->primitive == sema::PrimitiveType::String) {
                        llvm::Value* curS = builder_.CreateLoad(elemTy, addr);
                        llvm::Value* rStr = rhs;
                        const sema::TypeRef* rtv = ctx_.exprType(*as->value);
                        if (rtv &&
                            rtv->kind == sema::TypeRef::Kind::Primitive &&
                            rtv->primitive != sema::PrimitiveType::String)
                            rStr = emitPrimitiveToString(rhs, rtv->primitive);
                        llvm::Value* res = emitStringConcat(curS, rStr);
                        // §4.6: a non-string RHS was rendered to a fresh `rStr`
                        // (emitPrimitiveToString, rc=1) — release it after the
                        // concat borrows it. (#2157: defensive — sema rejects
                        // `s += <non-string>` upstream.)
                        if (rStr != rhs) emitStringRelease(rStr);
                        // #1481 / §4.6 / §28.9: a STRING-typed RHS that is itself
                        // a +1 PRODUCER (`i.ToString()`, a call returning string,
                        // a nested `+`/interpolation, `string.Join`, a
                        // `() -> string` closure call) has `rStr == rhs` and hands
                        // us its single +1 reference; the concat above borrows it,
                        // so release it or it leaks — unboundedly in a loop
                        // (`while … do s += i.ToString(); end`). A borrow RHS (a
                        // named local/field, §28.9) is not a producer and is left
                        // untouched. (Mirrors the string `+` operand discipline.)
                        else if (isPlusOneStringProducer(as->value.get()))
                            emitStringRelease(rhs);
                        bool isLocal = localSlot(*as->target) &&
                                       elemTy->isPointerTy() &&
                                       !tgtTy->nullable &&
                                       currentFn_ == stringReleaseEligibleFn_;
                        bool isField = isStringFieldLvalue(*as->target);
                        invEntry4682();  // §22.16 setter ENTRY (#4682)
                        builder_.CreateStore(res, addr);
                        if (isLocal || isField)
                            emitStringRelease(curS);  // release the old value
                        if (llvm::AllocaInst* ns = localSlot(*as->target))
                            syncCellToOrigSlot(ns, res);
                        invExit4682();  // §22.16 setter NORMAL EXIT (#4682)
                        return false;
                    }
                }
                // §6.4 (sweep #6): decimal compound assignment `d += x` / `-=` /
                // `*=` / `/=`. `decimal` is a Kind::Primitive struct, so the
                // user-operator guard above (which requires Kind::Named) misses
                // it; feeding the {i32,i32,i32,i32} struct to the integer
                // emitCompoundOp below emits an integer overflow intrinsic on a
                // struct (IR_FAIL; a `/=` SIGABRTs in LLVM). Route through the
                // decimal arithmetic helper instead.
                //
                // fix #3924 (§5.1/§6.4, v0.288): the #3924 unwrap above only
                // extracts a narrowed-nullable RHS's bare payload — for a
                // `decimal += int?` (narrowed) RHS that payload is `i32`, not
                // yet `decimal`. The ordinary (non-narrowed) `d += n` case
                // already arrives here pre-widened by the shared preamble's
                // `coerceNumericToType(rhs, elemTy, …)` a few hundred lines up
                // (its `srcTy->isIntegerTy()` guard fires on a bare scalar), but
                // that call ran BEFORE today's unwrap could turn the struct into
                // a scalar, so a narrowed-nullable RHS never got its turn. Widen
                // here — a no-op when `rhs` is already `decimal` (same-type
                // `decimal?` narrowed RHS, or the ordinary pre-widened case).
                if (elemTy == decimalStructType()) {
                    rhs = coerceNumericToType(rhs, decimalStructType(),
                                              isUnsignedTargetExpr(*as->value));
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    if (llvm::Value* res = emitDecimalArith(op, cur, rhs)) {
                        invEntry4682();  // §22.16 setter ENTRY (#4682)
                        builder_.CreateStore(res, addr);
                        if (llvm::AllocaInst* ns = localSlot(*as->target))
                            syncCellToOrigSlot(ns, res);
                        invExit4682();  // §22.16 setter NORMAL EXIT (#4682)
                    }
                    return false;
                }
                // §7.11 / §6.6 (#1964/#2141/#2149): a nullable-VALUE compound
                // assignment `n OP= e` (`int?`, `char?`, `long?`, `double?`,
                // `decimal?`, …) LIFTS — the desugared `n := n OP e` yields
                // `null` when EITHER operand is `null` (no underlying op and
                // none of its §6.1 overflow / §5.2 char / §6.1 div-zero checks
                // run), and otherwise applies the same checked arithmetic and
                // §6.6 write-back to the unwrapped payloads, rewrapped as `T?`.
                //
                // fix #2888: that lifting used to be OPEN-CODED here, ~110
                // lines, and this was the only place it existed — so the
                // property, indexer, virtual-property, imported-property and
                // heap-collection spellings of the SAME statement all fed the
                // tagged struct to `llvm.sadd.with.overflow` and aborted the
                // compile. It now lives inside emitCompoundOp, which every one
                // of those callers already funnels through, so this site is the
                // plain call below with nothing special about it.
                // CG31: built-in numeric compound assignment — read,
                // apply OP (overflow-checked like plain arithmetic),
                // write back.
                llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                llvm::Value* res = emitCompoundOp(
                    op, cur, rhs, elemTy->isFloatingPointTy(),
                    isUnsignedTargetExpr(*as->target),
                    isCharTargetExpr(*as->target),      // §6.6/§5.2 #1961
                    isUnsignedTargetExpr(*as->value));  // §6.2/§6.6 #2140 rUns
                if (res) {
                    invEntry4682();  // §22.16 setter ENTRY (#4682)
                    builder_.CreateStore(res, addr);
                    // §27.5 / #673: sync back to original alloca.
                    if (llvm::AllocaInst* ns = localSlot(*as->target))
                        syncCellToOrigSlot(ns, res);
                    invExit4682();  // §22.16 setter NORMAL EXIT (#4682)
                }
            }
        }
        return false;
    }
    // CG31: `x++` / `x--` — overflow-checked increment/decrement.
    // §17.6 / §25.1 (v0.295, #3978): `++`/`--` are NOT user-overloadable. The
    // only operands this arm ever sees are the built-in numeric primitives of
    // §6.6's compound form (integer, floating-point, `decimal`, `char`) and
    // the §17.6 indexer-slot desugaring (`a[i]++`); all class/record/enum/
    // choice operands are rejected up front by the IncrementStmt handler in
    // ExprTypecheck.cpp (see #2585/#2599/#3978/#4107/#4108). The pre-#3978
    // operator-method scan (`opMethod`) and the dispatch arm gated on it are
    // therefore unreachable from any program that passes sema — retired here
    // per #4109. For built-in numeric operands keep the x := x ± 1 path.
    //
    // fix #587 / §17.6: `++`/`--` on a computed property or user-defined indexer
    // slot must call the setter to write the incremented value back.
    // The lvalueAddr fallback returns null for these targets (no backing alloca),
    // so the store was silently dropped. After a null from lvalueAddr two additional
    // paths handle the write-back:
    //   (a) Block-setter computed property (`obj.P++`): mirrors lowerPropertyCompound
    //       — read via getter (lowerExpr), compute ±1, write via lowerPropertySetValue.
    //   (b) User-defined indexer (`v[i]++`): fix #2848 DELEGATES to
    //       lowerIndexerCompoundCore, the same body `a[i] += 1` uses, rather
    //       than mirroring it — §17.6 defines the two as one rule, and the
    //       former mirror had drifted three fixes behind the original.
    // Ahead of BOTH, and ahead of the lvalueAddr probe itself:
    //   (0) fix #2858 — a VIRTUAL / OVERRIDE / ABSTRACT / INTERFACE property
    //       dispatches through its vtable or itable
    //       (lowerVirtualPropertyIncrDecr). It must precede lvalueAddr because
    //       that call succeeds for a virtual AUTO-property and resolves to the
    //       DECLARING class's backing field, which is not where the override
    //       reads from.
    if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (inc->target) {
            // fix #2858 (§17.6/§12.1/§11.2, v0.240): a VIRTUAL / OVERRIDE /
            // ABSTRACT / INTERFACE property increments through its vtable or
            // itable, exactly as `P := v` and `P OP= v` already do (this
            // function's AssignmentStmt arm, ~L207 and ~L316). Checked BEFORE
            // `lvalueAddr` on purpose: for a virtual AUTO-property that call
            // succeeds and resolves to the DECLARING class's own backing field,
            // so the direct store below would land in the base's field while
            // every read goes through the dispatched getter to the override's —
            // MEASURED as a silently dropped write (0 where 1 is correct).
            if (lowerVirtualPropertyIncrDecr(*inc)) return false;
            llvm::Type* elemTy = nullptr;
            // fix #4682 (§22.16 / §17.6, v0.307): the §22.16 AUTO-PROPERTY
            // setter boundaries for `obj.P++` / `obj.P--`.
            //
            // §17.6 does not merely liken the increment to the compound form,
            // it DEFINES it as one: "`expr[i]++` desugars to a
            // read-modify-write", and for the named-property spelling `x++` is
            // "`x := x + 1`" (this file's own #4109 note). The store half is
            // therefore a setter invocation on §22.16's boundary — "Entry and
            // normal exit of every `public` setter — block-form,
            // auto-property, indexer, `set(owned)` alike" — and the read half
            // is a getter, which §22.16 exempts by name. MEASURED on this HEAD
            // before this fix, with `--checked-contracts`, on the issue's own
            // repro (`invariant(P < 10);`, `c.P++` from P == 9): 42, where
            // §22.16 requires ContractViolationException (99).
            //
            // The receiver out-parameter is threaded for the same reason
            // #4646 threaded it on the `:=` arm: `fieldAddr` lowers
            // `ma->target` exactly once and hands back that pointer, so
            // `f().P++` evaluates its receiver ONCE and the boundary check
            // lands AFTER §28.2's disposed guard (which `fieldAddr` emits
            // internally), keeping DisposedAccessException ahead of
            // ContractViolationException on a disposed instance.
            llvm::Value* invRecv4682 = nullptr;
            llvm::Value* addr = lvalueAddr(*inc->target, elemTy, &invRecv4682);
            // Resolved only when `lvalueAddr` produced a receiver pointer; a
            // shape whose receiver never materialised (an implicit-`this`
            // `P++`, which reaches `implicitFieldAddr` and is §22.16-exempt
            // anyway) degrades to the pre-fix behaviour rather than checking
            // over a null pointer.
            const sema::TypeSymbol* invCls4682 =
                (addr && invRecv4682)
                    ? instrumentedAutoPropertyStoreClass(*inc->target)
                    : nullptr;
            auto invEntry4682 = [&] {
                if (invCls4682) emitInvariantCheck(invRecv4682, invCls4682);
            };
            // §22.16's "throw exits check nothing" — see the compound arm's
            // twin for the full note.
            auto invExit4682 = [&] {
                if (invCls4682 && !builder_.GetInsertBlock()->getTerminator())
                    emitInvariantCheck(invRecv4682, invCls4682);
            };
            if (addr) {
                if (elemTy == decimalStructType()) {
                    // §6.4 (sweep #6): decimal `d++` / `d--`. decimal is a
                    // Kind::Primitive struct; the integer path below would build
                    // ConstantInt of a struct type and feed it to an integer
                    // overflow intrinsic (IR_FAIL). Compute `d ± 1m` via the
                    // decimal arithmetic helper.
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    if (llvm::Value* res = emitDecimalArith(
                            inc->op == "++" ? "+" : "-", cur, emitDecimalOne())) {
                        invEntry4682();  // §22.16 setter ENTRY (#4682)
                        builder_.CreateStore(res, addr);
                        if (llvm::AllocaInst* ns = localSlot(*inc->target))
                            syncCellToOrigSlot(ns, res);
                        invExit4682();  // §22.16 setter NORMAL EXIT (#4682)
                    }
                } else {
                    // Built-in numeric increment/decrement.
                    // §17.6/§7.11 (#2888): `one` comes from the ONE definition
                    // of §17.6's implicit right operand. `ConstantInt::get` on
                    // a nullable slot type (`{i8, i32}` for `n: int?`) is not a
                    // constant of that type at all — in a no-assert build it is
                    // an unchecked cast that fabricated a nonsense `i5 1` and
                    // aborted the compile, so even the LOCAL `n++` on an `int?`
                    // was an ICE while `n += 1` right beside it lifted fine.
                    bool fp = elemTy->isFloatingPointTy();
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    llvm::Value* one = emitImplicitOne(elemTy);
                    llvm::Value* res = one ? emitCompoundOp(
                        inc->op == "++" ? "+" : "-", cur, one, fp,
                        isUnsignedTargetExpr(*inc->target),
                        isCharTargetExpr(*inc->target))  // §6.6/§5.2 #1961
                        : nullptr;
                    if (res) {
                        invEntry4682();  // §22.16 setter ENTRY (#4682)
                        builder_.CreateStore(res, addr);
                        // §27.5 / #673: sync to original alloca.
                        if (llvm::AllocaInst* ns = localSlot(*inc->target))
                            syncCellToOrigSlot(ns, res);
                        invExit4682();  // §22.16 setter NORMAL EXIT (#4682)
                    }
                }
            } else {
                // fix #587 / §17.6: lvalueAddr returned null — the target is either
                // a built-in heap-collection indexer, a computed property with a
                // block setter, or a user-defined indexer.  All require a getter
                // read + arithmetic + setter write-back.
                const std::string binOp = (inc->op == "++") ? "+" : "-";
                // --- (c) Built-in heap-collection indexer: `arr[i]++` / `dict[k]++`
                // fix #613 / §25.1 / §4.4 / §4.8: Array<T> and Dictionary<K,V>
                // elements are accessed via bz_array_get/bz_dict_try_get_* (no
                // PropertyDecl, so findIndexer returns null and branch (b) never
                // fires).  Read via lowerCollectionIndexerGet, compute ±1, write
                // back via lowerCollectionIndexerSet — same encode/decode round-trip
                // as the plain `arr[i] := v` path.  Checked first so that (a)/(b)
                // are only reached for non-collection IndexExpr targets.
                if (lowerCollectionIndexerIncrDecr(*inc)) {
                    // handled: write-back emitted by lowerCollectionIndexerIncrDecr
                // --- (a) Block-setter computed property: `obj.P++` / `obj.P--` ---
                // mirrors lowerPropertyCompound (§22.9): read via getter expression,
                // compute new value, write via setter body.
                } else if (computedSetterOf(*inc->target)) {
                    llvm::Value* cur = lowerExpr(*inc->target);  // getter read
                    if (cur) {
                        // §17.6's implicit `1`, synthesized at the getter's
                        // result type by the ONE definition of that rule
                        // (#2888 — this was one of six hand-maintained copies).
                        // §6.4/#2883: `decimal` is a STRUCT-typed primitive, so
                        // `ConstantInt::get` on it does not build a constant of
                        // that type at all — it produced a nonsense `i33 1`
                        // that then reached emitCompoundOp and aborted the
                        // compile ("Call parameter type does not match function
                        // signature! i33 1 … llvm.sadd.with.overflow.
                        // s_bz.decimals"), measured on `d.P++` for a block-form
                        // `decimal` property. §7.11/#2888: a NULLABLE property
                        // (`b.NP++` where `NP: int?`) hit the same hole from the
                        // other side — `one` came out null, `res` came out null,
                        // and the increment was SILENTLY DROPPED with no
                        // diagnostic (measured 5 where 6 is correct). A type
                        // with no `1` still declines rather than materializing a
                        // bad constant.
                        llvm::Type* t = cur->getType();
                        bool fp = t->isFloatingPointTy();
                        llvm::Value* one = emitImplicitOne(t);
                        llvm::Value* res =
                            one ? emitCompoundOp(
                                      binOp, cur, one, fp,
                                      isUnsignedTargetExpr(*inc->target),
                                      isCharTargetExpr(*inc->target))  // §6.6/§5.2 #1961
                                : nullptr;
                        if (res) lowerPropertySetValue(*inc->target, res);
                    }
                // --- (a2) IMPORTED non-virtual property: `obj.P++` / `--` ---
                // fix #2862 (§17.6/§6.6/§2.8.2 clause 2/§12.1, v0.240): branch
                // (a) just declined because `computedSetterOf` needs an
                // ast::PropertyDecl and an IMPORTED property has none (§2.8.2
                // — the importer reconstructs the symbol from metadata). Branch
                // (b) below only handles an IndexExpr target, so a named
                // imported property reached NOTHING and the increment was
                // SILENTLY DROPPED (measured 0 where 102 is correct, while
                // `b.P := 7` on the same property was correct at 8). Routed
                // through the SAME body as `obj.P += 1` rather than a second
                // copy of it — §17.6 defines this spelling as an instance of
                // the §6.6 compound rule, which is exactly the consolidation
                // #2848 made for the indexer spelling.
                } else if (lowerImportedPropertyIncrDecr(*inc)) {
                    // handled: getter call, ±1, setter call, receiver shared.
                // --- (b) User-defined indexer: `v[i]++` / `v[i]--` ---
                // fix #2848 (§17.6/§6.6/§12.1/§23.4, v0.240): DELEGATE to the one
                // definition of this lowering instead of mirroring it.
                //
                // §17.6, verbatim: "For an indexer slot `expr[i]` (or
                // `expr[i, j, ...]` for a multi-dim indexer), `expr[i]++`
                // desugars to a read-modify-write through the indexer", and
                // "For `+=`, `-=`, and the other compound assignments on
                // indexer slots, the same desugaring applies with the operator
                // changed accordingly; it is the indexer instance of the
                // general compound-assignment rule of §6.6". The spec does not
                // merely liken the two forms — it defines `a[i]++` AS
                // `a[i] += 1`. This site used to carry a ~480-line hand-inlined
                // COPY of `lowerIndexerCompound` (its own comment said it
                // "mirrors lowerIndexerCompound"), and the copy had drifted
                // three fixes behind the original:
                //   * #2833's `indexAccessReceiverDecl` — it read
                //     `ctx_.exprType(*ix->target)` directly, and `this` carries
                //     no exprType, so `this[i]++` skipped the whole block and
                //     the write was DROPPED with no diagnostic (MEASURED on
                //     b0fa41a: 0, where 203 is correct);
                //   * #2836's class-vtable (#1080) and interface-itable (#730)
                //     branches — so `b[i]++` through a base-typed reference ran
                //     the BASE accessors (MEASURED: 102 where 203 is correct),
                //     and an interface-typed or `abstract` indexer, whose
                //     PropertyDecl is bodiless, found no accessor body and was
                //     likewise dropped;
                //   * #2832's `base[i]` decline (§12.3).
                // Routing through `lowerIndexerCompoundCore` fixes all of them
                // at once and removes the copy, so the next fix to this
                // lowering cannot land in one spelling and miss the other.
                //
                // Single evaluation is preserved because it is a property of
                // that one body, which lowers the receiver and every index
                // ONCE and spills them into slots shared by the getter call and
                // the setter call (§6.6: "every constituent of the left-hand
                // side's lvalue chain … is evaluated exactly once … and every
                // index expression in the chain"; §17.6: "Each subexpression in
                // the LHS — the collection expression and every index expression
                // — is evaluated exactly once. The indexer's getter and setter
                // are each called once"). ASSERTED MECHANICALLY, not assumed: a
                // side-effecting index (`b[ctr.Next()]++`) advances its counter
                // exactly once, before and after.
                //
                // Ordering is unchanged: (c) the built-in heap-collection form
                // and (a) the block-setter computed property are still tried
                // first, so this arm still sees only the non-collection,
                // non-named-property targets it saw before.
                } else {
                    lowerIndexerIncrDecr(*inc);
                }
            }
        }
        return false;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        // §21.7 (#739): a `return;` inside an INLINED property setter body ends
        // the setter (a void accessor), not the enclosing method — branch to the
        // accessor-end block rather than emitting a function return. (A setter is
        // void, so a value-bearing return cannot validly occur here.)
        if (inlinedAccessorEnd_ && !rs->value) {
            builder_.CreateBr(inlinedAccessorEnd_);
            return true;
        }
        // The return value is evaluated first, then in-scope `using
        // var`s are disposed (§28.3), then control leaves — so the
        // returned value reflects pre-disposal state.
        // #465 / §15.4: `return [e1, …]` whose declared return type is a HEAP
        // collection (`Array<T>` etc., §15.2). lowerExpr returns null for a
        // CollectionLiteralExpr, so the return would otherwise yield a null/zero
        // array (caller crashes). Materialize a real heap collection here (same
        // bz_array_alloc + bz_array_add as `new`/`.Add`); ownership transfers to
        // the caller as the §28.3 owned result.
        // #538 / stdlib §4.8: same problem for `return ["k1":v1, …]` — a dict
        // literal also falls through lowerExpr → null.  The `!cl->isDict` guard
        // below is widened to also handle dict literals via the same
        // materializeHeapDictLiteral helper that #534 introduced for local-decls.
        // §30.5 / #1332 (v0.162): detect `return await <source>` with a pointer-typed
        // result inside an async method — Pattern A chained/borrowed return. If
        // chained, this sets asyncChainedReturn_ so lowerAwait does NOT free the
        // source task immediately; lowerStmt will chain the source task to the
        // outer task after the await. Runs UNCONDITIONALLY (fix #3129) — even when
        // an enclosing `finally` is in flight — because the routed-through-finally
        // edge must also land in §30.5's kind 2/3; the flag is recorded on the
        // FinallyFrame at the routing site below so it survives to lowerTry's
        // mode-5 dispatch.
        //
        // fix #3960 (§30.5, v0.295): extracted into detectAsyncPatternAKind
        // (LlvmBackend_Async.cpp, beside the three predicates it calls) so an
        // expression-bodied async lambda's completing value — which has no
        // ReturnStmt AST node at all — can share the exact same detection
        // instead of a second, drifting copy. See that function's doc comment
        // for the full kind-1/kind-2/kind-3 derivation this block used to carry
        // inline.
        detectAsyncPatternAKind(rs->value.get());
        llvm::Value* v = nullptr;
        if (rs->value) {
            if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                    rs->value.get())) {
                // fix #1289 / §30.5 Pattern A: inside an async method (or async
                // lambda) the DECLARED return type node is the task type
                // (`Task<Array<int>>` / `OwningTask<T>`), but the value a
                // `return [e1, …]` produces is the task's RESULT T. Materializing
                // the literal against `Task<Array<int>>` matches none of the
                // collection shapes below, so `v` stayed null, `lowerExpr` also
                // returns null for a CollectionLiteralExpr, and
                // emitAsyncComplete(nullptr) completed the task with an EMPTY
                // result buffer — `t.Result` then read a null Array<int> and any
                // use of it (`.Count`) SIGSEGV'd. asyncResultSemaType_ is exactly
                // genericArgs[0] of the declared task type (set by
                // emitAsyncMethod / emitAsyncLambda), so it is the literal's real
                // target type; everything downstream (the §15.3 inline-array
                // branch, the heap array/dict materializers) is unchanged.
                // // per §30.5, §15.4, #1289
                // The `currentFn_ == asyncResumeFn_` conjunct keeps this to the
                // async body's OWN Resume function: a nested non-async lambda
                // lowered mid-body does not save/restore the sm*_ fields, so
                // `smStateStruct_` alone would misroute its `return [ … ]` to
                // the enclosing task's result type.
                const sema::TypeRef* retTargetTy = nullptr;
                if (smKind_ == SuspendKind::Await && smStateStruct_ &&
                    asyncResumeFn_ && currentFn_ == asyncResumeFn_ &&
                    asyncResultSemaType_)
                    retTargetTy = asyncResultSemaType_;
                // fix #2703 (§15.1 "Inline arrays" / §23.3, v0.240): use the
                // REPRESENTATION node, which inside a block-bodied accessor
                // falls back to the accessor's own declared type. Without it a
                // `property A: [3]int get => do return [11,22,33]; end` left
                // `retTargetTy` null, so `v` stayed null and the getter
                // returned a SILENT ZERO (reproduced: exit 0 for an expected
                // 66, with only the #2491 warning).
                else if (const ast::Type* rn = returnReprTypeNode())
                    retTargetTy = ctx_.resolvedType(*rn);
                if (const sema::TypeRef* rrt = retTargetTy) {
                    // §15.3 (#781): a `return [e1, …]` whose declared return
                    // type is an INLINE array (`[N owned]T?` / `[N]T`) builds
                    // the `[N x elem]` aggregate and returns it BY VALUE.
                    // lowerExpr returns null for a CollectionLiteralExpr, and
                    // materializeHeapCollectionLiteral only makes a heap
                    // bz_array_t* (a `ptr`, the wrong type for an inline
                    // return) — so without this the method returned an
                    // undef/zero array and the caller's `var a := Make()`
                    // materialized as all-null. Mirrors the inline-array
                    // local-decl build (lowerLocalDecl).
                    if (rrt->kind == sema::TypeRef::Kind::InlineArray &&
                        !cl->isDict) {
                        if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(
                                lowerTypeRef(rrt))) {
                            llvm::Type* elemTy = aty->getElementType();
                            llvm::AllocaInst* tmp = entryAlloca(aty);
                            for (std::size_t i = 0; i < cl->elements.size();
                                 ++i) {
                                if (!cl->elements[i]) continue;
                                llvm::Value* ev = lowerExpr(*cl->elements[i]);
                                if (!ev) {
                                    // fix #3712 (§3.2 / §15.1 / §17.11,
                                    // v0.286): a UNIT-typed element is not a
                                    // lowering failure — see
                                    // lowerCollectionLiteralElementOrUnitNull
                                    // (hoisted from fix #3528's local-decl
                                    // guard; LlvmBackend_Setup.cpp).
                                    ev = lowerCollectionLiteralElementOrUnitNull(
                                        *cl->elements[i], elemTy);
                                    if (!ev) {
                                        // #1386: ICE — lowerExpr returned
                                        // null for an inline-array return
                                        // element.
                                        ctx_.diagnostics().report(
                                            Severity::Error,
                                            cl->elements[i]->span.begin,
                                            "internal: lowerExpr returned "
                                            "null for inline-array return "
                                            "element — #1386");
                                        continue;
                                    }
                                }
                                if (ev->getType() != elemTy &&
                                    ev->getType()->isIntegerTy() &&
                                    elemTy->isIntegerTy()) {
                                    unsigned eb =
                                        ev->getType()->getIntegerBitWidth();
                                    unsigned db =
                                        elemTy->getIntegerBitWidth();
                                    ev = db > eb
                                             ? builder_.CreateSExt(ev, elemTy)
                                             : builder_.CreateTrunc(ev,
                                                                    elemTy);
                                }
                                // fix #4317 (§15.1 / §8.6, v0.298): box a
                                // value-typed (record) element into its box
                                // representation before the store — see the
                                // detailed derivation at the sibling
                                // IndexExpr-arm fix earlier in this function.
                                // No-op for a class element (already a bare
                                // pointer) or a non-Object/interface element.
                                ev = boxForTarget(ev, rrt->arrayElement,
                                                  cl->elements[i].get());
                                llvm::Value* gep = builder_.CreateGEP(
                                    aty, tmp,
                                    {builder_.getInt32(0),
                                     builder_.getInt32((int)i)});
                                builder_.CreateStore(ev, gep);
                            }
                            v = builder_.CreateLoad(aty, tmp, "ret.inlarr");
                        }
                    } else if (!cl->isDict)
                        v = materializeHeapCollectionLiteral(cl, rrt);
                    else if (!cl->keys.empty())
                        v = materializeHeapDictLiteral(cl, rrt);
                }
            }
            if (!v) v = lowerExpr(*rs->value);
        }
        // §30.5 / #1332 (v0.162): read and reset the chained-return flag now that
        // lowerExpr has completed (lowerAwait already used the flag to suppress the
        // source-task free; we must reset before any possible re-entry).
        bool chainedReturn = asyncChainedReturn_;
        asyncChainedReturn_ = false;
        // fix #3125: §30.5 Pattern A **kind 3** twin — `return await <borrowed
        // source>`. Same read-and-clear pattern as the chained-return flag: it
        // is set by the detection block above (see fix #3096 / #3129 in the
        // chained-return detection comment), survives only long enough for
        // lowerAwait to read it and for this return-statement site to route it
        // to emitAsyncPatternAResultDecision, then is reset.
        bool borrowedReturn = asyncBorrowedReturn_;
        asyncBorrowedReturn_ = false;
        if (diverged_) return true;  // CG40: the value expression threw
        // #2491 (return-path twin of #2434): a return statement's value
        // expression was present but failed to lower — either lowerExpr
        // returned null for the whole expression, or a collection-literal
        // materializer (the #465/#538 heap-array/dict arms above) yielded
        // nothing. Every downstream return route (sync emitReturn's
        // `v ? v : null` fallback in LlvmBackend_Setup.cpp, the async
        // emitAsyncComplete-with-null task completion, and the finally
        // routing's `if (slot && v)` skip-store) silently degrade a null
        // value to a zero/null return — the correct degrade (definite
        // assignment / §28.3 ownership make the zero safe), but #2434 made
        // the local-decl twin of this exact fallback LOUD and explicitly
        // asked for the same signal on this return path. Mirror #2434's
        // posture exactly:
        //   * Severity::Warning, not Error — the §3 / #605 permissive
        //     "unknown member" shape `return o.Anything();` for
        //     `o: Object` is accepted by sema BY DESIGN and currently
        //     reaches here (codegen has no concrete member to dispatch
        //     to), so an Error would false-reject legal programs exactly
        //     as #2434's empirical lit-track check found for the
        //     local-decl twin. Driver::compileExecutableObject prints
        //     every diagnostic regardless of severity, so a Warning is
        //     fully visible on stderr without failing the build.
        //   * Guarded on !hasErrors() so this does not stack on top of an
        //     already-reported, already-fatal lowerExpr error (e.g. an
        //     unhandled `in` form) — same message-quality guard #2434 used.
        // The zero/null degrade still happens downstream; this only makes
        // the failure observable instead of invisible.
        //
        // ── THREE OUTCOMES, and why THIS order (#2915 + #2935) ──────────────
        // Identical structure to the `lowerLocalDecl` twin (LlvmBackend_Setup
        // .cpp), which carries the full derivation:
        //   (a) UNIT-typed return value ⇒ exempt, silent — nothing failed.
        //   (b) member call on a BUILT-IN receiver that neither sema nor
        //       codegen could make sense of ⇒ located Error.
        //   (c) everything else ⇒ the pre-existing #2491 Warning.
        // (a) first, because it alone says "this site is not a degrade"; and
        // (a) and (b) are DISJOINT by construction — (a) needs the expression
        // to HAVE a unit type, (b) needs it to have NO type — so the order
        // cannot change any outcome, only the reading.
        //
        // ⚠ That disjointness does NOT mean a unit-returning built-in member
        // call is (a)'s business: sema does not type such a call unit, it does
        // not type it at all, so `return s.Send(42);` from an int-returning
        // method lands in (b). Rejecting it is correct — it yields nothing —
        // but (b)'s wording must not claim `Send` does not exist. See the
        // lowerLocalDecl twin for the measurement.
        //
        // fix #2915 (§4.7): a UNIT-typed return value is exempt. `()` lowers to
        // LLVM `void` (lowerTypeRef's Kind::Unit case), so it legitimately has
        // no lowered value — that is the type's representation, not a lowering
        // failure — and for an `Object`/interface return the §8.6 boxing below
        // supplies the box. Without this exemption both `return ();` from a
        // unit-returning method and the now-correct `return ();` through an
        // `Object` return printed an `internal:` note on a program with nothing
        // wrong with it.
        //
        // fix #2935 (v0.240): the returned expression is a MEMBER CALL ON A
        // BUILT-IN RECEIVER (`return r.TotallyBogusMember(1, 2);` on a
        // `Random`, measured degrading to a silent 0 on `f069645`). A built-in
        // has no user declaration, so its member surface is closed and "no arm
        // lowered this" means "not on that surface" — but that inference only
        // holds where a value was DEMANDED, because `lowerCallExpr` returns
        // nullptr both for "nothing lowered this" and for "an arm lowered it
        // and the result is unit" (#2963). §21.7 supplies the demand here and
        // states it as a language rule rather than an assumption: "this
        // callable has no return type — `return` may not carry a value", so a
        // `return expr;` that reached codegen is in a callable that has a
        // return type.
        //
        // ⚠ THE THREE SIBLING #2491 SITES ARE DELIBERATELY NOT GIVEN THIS —
        // and the reason is measured, not assumed. Two of them are the
        // ARROW-BODY (`=> expr`) faces, and a unit-returning arrow body is
        // legal Bezel that §21.7 does NOT cover: `public static method
        // Push(s: ChannelSender<int>) => s.Send(42);` compiles today, WORKS
        // (the item really is buffered), and reaches that site with `v ==
        // null` because `Send` lowered as unit. The same gate there would
        // reject it. Extending to those sites needs a distinct "this callable
        // has a return type" test (`ci.returnType`) and its own measurement;
        // filed as #2966 rather than guessed at. The
        // `builtinReceiverOfMemberCall` helper is deliberately shared with
        // lowerLocalDecl so that whoever does extend it changes ONE predicate,
        // not a fifth copy of it — #2924's four-way-mirror finding about this
        // very diagnostic.
        // fix #4665 (§14.2 *Constraints* + §4.7 / §17.11, v0.307): resolve the
        // returned expression's type through the ACTIVE monomorphization before
        // the unit test. A bare type parameter substituted at `()` arrives here
        // as a `Named T` with no declaration — its `kind` is `Named`, NOT
        // `Unit` — so exemption (a) below missed it and a *correct* unit return
        // inside a specialized generic body drew the #2491 "failed to lower"
        // internal warning. Measured on 572f5fc5 + the `default(())` fix, for
        // the issue's `class Factory<T> … Make(): T return default(T);` at
        // `Factory<()>`: `Factory$Make$spec` emits a correct `ret void` and the
        // program runs, while the compile prints an internal note about it.
        // `resolveTypeParamRef` is the predicate the repo already settled for
        // exactly this shape — `buildOrderedArgs`' #3137 unit-argument rescue
        // resolves a `Named T`→`()` formal through it before its own
        // `Kind::Unit` test — so this is that one predicate, not a second
        // spelling of it. It is a no-op outside a specialization
        // (`genericSubstSemaTy_` empty) and for every non-type-parameter ref.
        const sema::TypeRef* retValTy =
            rs->value ? resolveTypeParamRef(ctx_.exprType(*rs->value))
                      : nullptr;
        const bool unitReturnValue =
            retValTy && retValTy->kind == sema::TypeRef::Kind::Unit;
        std::string retMember, retAppliedMember;
        const std::string retBuiltinRecv =
            (rs->value && !v && !unitReturnValue)
                ? builtinReceiverOfMemberCall(rs->value.get(), &retMember,
                                              &retAppliedMember)
                : std::string();
        // fix #2974 (§11.7/§12.5, stdlib §6.8, v0.240): the `new` sibling, at
        // the same §21.7-protected `return expr;` site the member-call gate
        // covers — `return new Domain("d");` demands a value exactly as a local
        // declaration does. Disjoint from the member-call gate by construction
        // (a CallExpr is not a NewExpr), so the order is immaterial.
        const std::string retBuiltinNew =
            (rs->value && !v && !unitReturnValue && retBuiltinRecv.empty())
                ? unconstructedBuiltinNewName(rs->value.get())
                : std::string();
        if (unitReturnValue) {
            // (a) — nothing to report.
        } else if (!retBuiltinRecv.empty() &&
                   !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Error, rs->value->span.begin,
                "`" + retMember + "` on built-in type `" + retBuiltinRecv +
                "` produced no value, so it cannot supply this return value. `" +
                retBuiltinRecv + "` has no user declaration, so its member "
                "surface is exactly what the standard library documents for "
                "it: either `" + retMember + "` is not on that surface (a "
                "misspelling), or it is documented but yields nothing here "
                "(#2935 \xE2\x80\x94 if it is documented and should yield a "
                "value, it is unimplemented, #2457)" +
                builtinMemberChainNote(retMember, retAppliedMember));
        } else if (!retBuiltinNew.empty() &&
                   !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Error, rs->value->span.begin,
                unconstructedBuiltinNewNote(retBuiltinNew,
                                            "supply this return value"));
        } else if (rs->value && !v && !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Warning, rs->value->span.begin,
                "internal: return-value expression failed to lower to a "
                "value (degraded to a zero/null return value) — #2491");
        }
        // §30.1: inside an async method's Resume function, `return expression;`
        // completes the OUTER task with the value (and `return;` / fall-off
        // completes with no value), then returns void from Resume — it does NOT
        // box for or `ret` the entry function's Task ptr return type. Handled
        // before the §8.6 boxing / ownership-transfer / finally machinery, which
        // are all keyed off the (Task-typed) declared return and don't apply to
        // the unwrapped result value. (await-in-try / return-through-finally in
        // async bodies is deferred; finallyStack_ is empty for the minimal cut.)
        if (smKind_ == SuspendKind::Await && smStateStruct_) {
            // #776 / §30.5: when the async result is a bare local `Task`/`Task<T>`
            // being TRANSFERRED out (e.g. `return t;` in a `Task<Task<T>>` method),
            // null its slot before scope-end disposal. `v` already holds the task
            // pointer; without nulling, emitAllDisposals fire-and-forgets the very
            // task being returned, so the caller's `await` on the returned task
            // hangs (its completion was consumed by the scope-end fire-and-forget).
            if (v && rs->value) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        rs->value.get())) {
                    const sema::TypeRef* vt = ctx_.exprType(*id);
                    bool isTaskLocal =
                        vt && vt->kind == sema::TypeRef::Kind::Named &&
                        !vt->nullable && vt->astNode && vt->astNode->name &&
                        !vt->astNode->name->parts.empty() &&
                        (vt->headName() == "Task" ||
                         vt->headName() == "OwningTask");
                    if (isTaskLocal) {
                        llvm::Type* sty = nullptr;
                        if (llvm::Value* slot = lvalueAddr(*id, sty))
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), slot);
                    }
                }
            }
            // #721 C1 / §30.9: a `return` exiting a try-with-finally must run the
            // finally first. Stash the unwrapped result and route through the
            // active finally (exit mode 5); the post-finally dispatch (lowerTry)
            // performs emitAsyncComplete + RetVoid. Without this the finally is
            // silently skipped on the async-return edge.
            // fix #1567 / §29.7/§28.10 (v0.182): the async-method return edge
            // obeys the same reverse-establishment rule as the sync return edge
            // above — dispose ONLY the try-body scopes (>= the finally's entry
            // depth) here; the mode-5 dispatch (lowerTry) disposes the enclosing
            // scopes AFTER the finally runs. The pre-v0.182 emitAllDisposals()
            // before the finally freed an enclosing `using var` before a finally
            // that touched it, a use-after-dispose.
            if (!finallyStack_.empty()) {
                auto& fr = finallyStack_.back();
                // fix #3129 / §30.5 Pattern A kind 2: deferral on the routed-
                // through-finally edge. The post-finally mode-5 dispatch (lowerTry)
                // calls bz_task_set_chained for the outer task when the source's
                // local is itself the owner (no move occurs; §30.5 *Chaining from
                // a scope-bound source*). The deferral is per-EDGE, exactly like
                // the no-finally branch: null the source's local slot immediately
                // before emitDisposalsInRange, so the body's scope-end disposal
                // sees a null task pointer (emitDisposals' `task.faf` arm skips
                // it). The pointer itself survives in the frame's pending-task
                // slot (lowerAwait wrote it there and lowerTry's mode-5 reads it
                // back), so the eventual bz_task_set_chained finds it. // per
                // §30.5 kind 2 deferral, v0.252
                // fix #3126 (§30.5 kind 2, v0.254): for the MOVE-OUT HALF —
                // the `owned` PARAMETER source — the dispensation targets the
                // FRAME capture slot, not a local slot. nullStateMachineOwnedParamCapture
                // nulls that frame slot so emitAsyncOwnedParamAutoDispose's
                // null-guard skips this parameter on this edge (the §30.1
                // auto-drop would otherwise double-dispose a Cell the chained
                // task already owns transitively).
                // fix #3960: shared with the non-finally branch below and with
                // emitAsyncLambda's expression-body completion — see
                // deferChainedAsyncReturnSource (LlvmBackend_Async.cpp).
                if (chainedReturn && rs->value)
                    deferChainedAsyncReturnSource(rs->value.get());
                emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
                if (v && fr.asyncRetSlot)
                    builder_.CreateStore(v, fr.asyncRetSlot);
                // fix #3129 / §30.5 Pattern A: record on the FinallyFrame that
                // this async-return exit is kind-2 chained. The mode-5 dispatch
                // in lowerTry propagates this flag through each finally along
                // the path; the terminal mode-5 site (no enclosing finally)
                // uses it to call bz_task_set_chained on the outer task. // per
                // §30.5 kind 2, v0.252
                fr.chainedAsyncReturn = chainedReturn;
                // fix #3125 / §30.5 Pattern A kind 3: same propagation, twin of
                // the chained flag above, for `return await <borrowed source>`
                // routed through a `finally`. The terminal mode-5 site asks
                // emitAsyncPatternAResultDecision to skip the kind-1 disposer
                // attach — the source's owner keeps disposing the source task
                // (transitively disposing the result), and the outer task's
                // teardown disposes nothing. The two flags are mutually
                // exclusive on the same return-statement site (one operand,
                // one answer).
                fr.borrowedAsyncReturn = borrowedReturn;
                builder_.CreateStore(builder_.getInt32(5), fr.exitModSlot);
                builder_.CreateBr(fr.finNormalBB);
                return true;
            }
            // fix #3096 / §30.5 kind 2 *Chaining from a scope-bound source*
            // (v0.252): "the source's slot already lives in the enclosing
            // state-machine frame, and the chained-slot arrangement **defers
            // its disposal** from the frame's scope end to the outer task's
            // teardown … On both paths the source task is disposed exactly once
            // — by the outer task's destructor, transitively."
            //
            // This is that deferral, and it belongs HERE rather than at the
            // local's registration: the dispensation applies to THIS return
            // edge only. Every other exit of the same body — a fault re-raised
            // by the await, a different `return`, an exception — chains nothing,
            // so the local must keep its ordinary scope-end disposal on those.
            // Nulling the slot immediately before emitAllDisposals is exactly
            // "defer": the `d.task` entry is null-guarded (emitDisposals'
            // `task.faf` arm skips a null task), and this is the same technique
            // #776 uses one screen above for a task local being TRANSFERRED out.
            // The pointer itself survives in the frame's pending-task slot,
            // which emitAwaitSuspend wrote at the await and nothing clears, so
            // the chained branch below still finds it. The window between this
            // store and bz_task_set_chained is the same one the anonymous-
            // CallExpr source has always had.
            // fix #3960: shared helper — see deferChainedAsyncReturnSource
            // (LlvmBackend_Async.cpp), also used by the finally-routed branch
            // above and by emitAsyncLambda's expression-body completion.
            if (chainedReturn && rs->value)
                deferChainedAsyncReturnSource(rs->value.get());
            // No enclosing finally: dispose all live scopes, then complete.
            emitAllDisposals();
            // fix #3129 / §30.5 Pattern A: hoist the §30.5 kind 1 / kind 2 decision
            // out of the *no enclosing finally* branch into a helper called from
            // BOTH the direct branch and lowerTry's mode-5 dispatch, so a Pattern A
            // return routed through a `finally` no longer falls off the end of the
            // kind table. Same race-safety ordering as before: BOTH the chained-slot
            // and the disposer attach BEFORE emitAsyncComplete, because
            // bz_task_complete notifies continuations that may free the outer task
            // on a racing thread and any chained/disposer call AFTER complete would
            // touch freed memory. // per §30.5 kind 1 + kind 2, v0.252
            // fix #3125: pass borrowedReturn too — see emitAsyncPatternAResultDecision.
            emitAsyncPatternAResultDecision(chainedReturn, borrowedReturn);
            // fix #3788 / §27.5 / §28.9 / §30.5 (v0.286): the BLOCK-bodied twin
            // of #3773 (expression-body async lambda mint, landed in
            // LlvmBackend_Async.cpp ~L2218). This async arm completes the task
            // and returns ~90 lines BEFORE the closure/string mint arms further
            // down this function (the L2485 closure mint keys on
            // `currentLambdaReturnSemaTy_`, which is null for an async lambda —
            // `lowerLambda` nulls it at Exprs2.cpp:2114 because the closure's
            // declared return is the outer `Task<T>`, not the inner T), so an
            // `async lambda() => do return <closure-typed expr>; end` completed
            // its outer task with a +0 BORROW of a captured closure local. The
            // consumer that makes that fatal is the lambda ENVIRONMENT
            // DESTRUCTOR: when the awaited result is bound to a local that is
            // then CAPTURED by a sibling lambda, the capturing env's dtor
            // releases the captured cell keyed purely on its function type
            // (LlvmBackend_Exprs2.cpp envdtor, `ce.semaTy->kind ==
            // Kind::Function`), on the invariant that a closure local's slot
            // owns +1 — and the async lambda's OWN env released the captured
            // local while the awaited result still aliased it. Measured on
            // this parent (r12-cycle-1 = 254ae52) for the issue 3788 repro —
            // `var inner := lambda() => 7; var f := async lambda() => do
            // return inner; end; var g := await f(); var use := lambda() =>
            // g(); return use();` — as `attempting double-free on 0x...
            // in bz$lam$1$envdtor` (the async lambda's envdtor releasing
            // `inner`, whose cell `g` still aliased).
            //
            // Mirror #3773's expression-body mint token-for-token, with the
            // SAME three transfer exclusions, and key on the RETURNED
            // EXPRESSION's own sema type — `ctx_.exprType(*rs->value)` — NOT
            // `asyncResultSemaType_`. The latter is the WRAPPED `Task<T>` for
            // an unannotated return (LlvmBackend_Async.cpp:1702 stores
            // `resultElemSemaTy` there, but the call-site annotation path
            // passes `Task<T>` itself for the disposer derivation; the
            // expression body of #3773 has the same defect and was fixed by
            // the same change of key). The body's natural type — the type of
            // the value being minted for — is the only one right on BOTH the
            // annotated and unannotated derivations.
            //
            // No STRING ARM here either, for the same reason #3773 declined
            // it: `bz_string_retain` for an async result goes through the
            // async Result slot, and the consumer-side `var g := await f();`
            // does not arm a string release the way `var g := f();` does on
            // the sync venue, so a string minted here has nobody to discharge
            // it. Filed (2714 follow-on).
            //
            // ⚠ THE ASYNC-SPECIFIC EXCLUSION IS NARROWER THAN #3773's here.
            // #3773 needed to exclude any value that arrives from a callee
            // that mints on return (CallExpr of any shape, or `await`)
            // because the expression body could be `=> await C.MkA()` or
            // `=> f2()`. The block body is the `return` STATEMENT's value,
            // which has already been evaluated by the time we reach this
            // site — a callee that mints has ALREADY minted into `v`, so
            // `v` IS the callee's +1 and re-minting would over-mint. Same
            // shape, narrower authority: a direct CallExpr is by far the
            // most common callee-mints-on-return shape, and that case the
            // sync venue's own mint (lowerLambda's expr-body for an inner
            // lambda, or the L2485 / L2546 mint for an outer method) has
            // already handled. `await` as a body is excluded too — the
            // awaited task's `emitAsyncComplete` already produced the +1.
            // An Identifier (named local of this scope) is answerable
            // here as `returnedClosureLocalOwnsPlusOne`.
            if (v && rs->value && v->getType()->isPointerTy() &&
                !builder_.GetInsertBlock()->getTerminator()) {
                const sema::TypeRef* bodySemaTy =
                    ctx_.exprType(*rs->value);
                bool arrivesFromCallee =
                    dynamic_cast<const ast::CallExpr*>(rs->value.get()) !=
                        nullptr;
                if (auto* un = dynamic_cast<const ast::UnaryExpr*>(
                        rs->value.get()))
                    if (un->op == "await")
                        arrivesFromCallee = true;
                if (bodySemaTy &&
                    bodySemaTy->kind == sema::TypeRef::Kind::Function &&
                    !bodySemaTy->nullable &&
                    !arrivesFromCallee &&
                    !isPlusOneClosureProducer(rs->value.get()) &&
                    !isUserMethodClosureReturnCall(rs->value.get()) &&
                    !returnedClosureLocalOwnsPlusOne(rs->value.get()))
                    emitClosureRetain(v);
            }
            emitAsyncComplete(v);
            builder_.CreateRetVoid();
            return true;
        }
        // §8.6: box a value-typed `return expr` whose declared return type is
        // Object/interface, so the caller's is/as/virtual dispatch sees a box.
        // fix #2703 (§8.6 "Boxing" / §23.3, v0.240): keyed on the
        // REPRESENTATION node so a block-bodied accessor boxes against ITS OWN
        // declared type. Pre-fix an `Object`-typed block getter returning an
        // `int` emitted `ret i32` from a `ptr`-returning function and the
        // module failed IR verification.
        // fix #2915: the guard was `if (v && rs->value)`. A unit-typed
        // `return ();` through an `Object` return has NO lowered value (§4.7's
        // `()` lowers to LLVM `void`), so the `v &&` skipped the box and the
        // caller received null. boxForTarget bails on a null value for every
        // other shape, and is a no-op whenever the declared return type is not
        // Object/interface, so dropping the guard changes nothing else.
        // fix #3095 (§4.7, v0.252): the `()?` sibling of that rescue, at the
        // same site — `return ();` from a `: ()?` method has no lowered value
        // either, and §4.7's `()?` has "two values, `null` and `()`", so the
        // present `{i8 1, i8 0}` must be synthesized rather than `ret`'ing the
        // all-zero (null) pattern. A no-op for every non-`()?` return type, so
        // the §8.6 box below is reached exactly as before.
        if (const ast::Type* rn = returnReprTypeNode())
            if (rs->value) {
                sema::TypeRef* rnTy = ctx_.resolvedType(*rn);
                llvm::Value* uv = unitValueForTarget(v, rnTy, rs->value.get());
                v = (uv != v) ? uv : boxForTarget(v, rnTy, rs->value.get());
            }
        // §29.2 / §4.3 (#1310): a value returned as `IEnumerable<T>` must be a
        // vtable-backed enumerator, not a raw bz_array_t* / class instance — the
        // caller's for-in and its scope-end iterEnumDispose both dispatch through
        // the value's vtable (a raw array → SIGSEGV). Wrap it here as the
        // call-argument and local-decl init paths already do. #1916: pass
        // ownLazyPipeline=true — a lazy LINQ pipeline returned as IEnumerable<T>
        // is drained into an OWNING enumerator (the caller owns and disposes the
        // returned value, so an owning wrapper is leak-free).
        // fix #2703 (§29.2 "Desugaring of `for`" / §23.3, v0.240): keyed on the
        // REPRESENTATION node for the block-bodied-accessor case. Pre-fix, a
        // `property E: IEnumerable<int>` whose body returned an `Array<int>`
        // field handed the caller a raw `bz_array_t*` and the caller's for-in
        // drove it through the §29.11 enumerator vtable — reproduced as a
        // SIGSEGV (exit 139), the worst live symptom in this family. The
        // wrapper frame this mints is disposed by the reader, not leaked: a
        // for-in whose source is a NON-auto property is classified
        // `!sourceIsNamedLocal` (see lowerForIn, #1840/#2485 — "every other
        // property — expression-body, BLOCK-body … is the COMPUTED,
        // fresh-value case") and registers the loop-end iterEnumDispose. This
        // is therefore the same wrap/dispose round trip the expression-bodied
        // sibling has always had, not a new ownership obligation.
        if (const ast::Type* rn = returnReprTypeNode())
            if (v && rs->value)
                v = wrapAsIEnumerableForTarget(
                    v, ctx_.resolvedType(*rn), rs->value.get(),
                    /*ownLazyPipeline=*/true,
                    /*outWrapMade=*/nullptr,
                    /*srcOwnedByCallee=*/
                    returnSourceIsCalleeOwnedCollection(rs->value.get()));
        // #429 (RETURN-escape category) / §4.6 / §28.9 / §28.3: a method whose
        // declared return type is (non-nullable) `string` mints EXACTLY +1 for
        // the returned value, transferring ownership of one reference to the
        // caller. The caller treats the result as a +1 producer
        // (isPlusOneStringProducer) and balances it with a transfer-then-release,
        // so the round trip neither leaks (the +1 is released) nor dangles (the
        // +1 keeps a borrowed value alive past the callee). The value is either
        // TRANSFERRED (it already carries a +1 the callee owns) or RETAINED (a
        // pure borrow, so a fresh +1 is minted); bz_string_retain is a no-op on a
        // sentinel/literal/null. We TRANSFER (no retain) when the returned
        // expression already owns a +1:
        //   * a +1 PRODUCER expression (`+`/interpolation/range-slice, or a
        //     nested string-returning call), whose single reference flows out; or
        //   * an IDENTIFIER naming a non-nullable string LOCAL that owns a +1 —
        //     a producer-initialized local (transferred the producer's +1 into
        //     its slot) or a borrow local (retained +1 at bind, stringBorrowAll_).
        //     Such a returned local is escaped (escape walk) so it is NOT released
        //     at scope end, and is excluded from disposal by the retSlot path
        //     below — its single +1 flows out to the caller instead of leaking.
        // Otherwise (a param read, a field read `this.f`, a literal, an alias not
        // owning a +1) the value is a BORROW → RETAIN to mint the +1.
        // fix #2688 / §27.3 / §28.9: a LAMBDA body carries no declared
        // return-type NODE (#2515 nulls currentReturnTypeNode_ so the body does
        // not box/mint against the enclosing METHOD's node), but it does have a
        // return TYPE — currentLambdaReturnSemaTy_, the same `retSemaTy` the
        // expression-body path keys its mint off. Without this the two body
        // forms of the same lambda disagreed: `lambda(k) => k` returned +1 and
        // `lambda(k) => do return k; end` returned a +0 borrow, so every caller
        // that releases the result (a `var` bind, `Dictionary.GetOrAdd`'s
        // store) double-freed the block-body form and only that form. Mirror
        // the expr-body predicate EXACTLY, including its `!nullable` gate: a
        // `string?` lambda return does not mint there, so it must not mint here
        // either — agreement between the two forms is the invariant, and the
        // method-side `string?` mint (#2196) is a method-contract rule the
        // lambda call-site ABI does not share.
        const sema::TypeRef* lamRet = currentLambdaReturnSemaTy_;
        if (v && rs->value && !currentReturnTypeNode_ && lamRet &&
            v->getType()->isPointerTy() && !lamRet->nullable &&
            lamRet->kind == sema::TypeRef::Kind::Primitive &&
            lamRet->primitive == sema::PrimitiveType::String &&
            !isPlusOneStringProducer(rs->value.get()) &&
            !returnedStringLocalOwnsPlusOne(rs->value.get()))
            emitStringRetain(v);
        // fix #2691 / §27.3 / §27.5 / §28.9 (v0.240): the CLOSURE sibling of the
        // string mint immediately above — the category #2688 did not cover. The
        // closure branch further down keys on currentReturnTypeNode_, which
        // #2515 nulls inside a lambda body, so a BLOCK-bodied lambda whose
        // return type is a FUNCTION type handed back a +0 borrow while its
        // EXPRESSION-bodied twin minted +1 (LlvmBackend_Exprs2.cpp, keyed on
        // retSemaTy). The consumer that makes the difference fatal is the
        // lambda ENVIRONMENT DESTRUCTOR: when the returned closure is bound to a
        // local that is then CAPTURED, the capturing env's dtor releases that
        // cell keyed purely on its function type (LlvmBackend_Exprs2.cpp
        // envdtor, `ce.semaTy->kind == Kind::Function`), on the invariant that a
        // closure local's slot owns +1. The expression form established that +1;
        // the block form did not, so the same program was sound or a
        // heap-use-after-free depending on nothing but which body form the
        // author wrote:
        //     var f := lambda(): () -> int => do return inner; end;
        //     var g := f();            // +0 pre-fix, +1 for the expr sibling
        //     return lambda() => g();  // envdtor releases g -> UAF on `inner`
        // Mirror the expression-body predicate EXACTLY — same three transfer
        // exclusions, and, as there, NO `!nullable` gate (the closure category
        // has never had one, unlike the string category above): agreement
        // between the two body forms is the invariant #2688 established.
        // fix #3804 (§27.5 / §15.3 / §28.9, v0.294): `!isLambdaClosureReturnCall`
        // joins the three existing transfer exclusions — a CallExpr whose
        // callee is a lambda-typed LOCAL/PARAMETER (`return outer();` where
        // `outer` holds a lambda) already mints exactly +1 in ITS OWN body
        // (fix #2714's own reasoning: the callee is statically a LambdaExpr
        // with a function-typed return, so #2691's mint or the producer's
        // transfer already fired inside it) — the SAME fact
        // isUserMethodClosureReturnCall/isPlusOneClosureProducer already
        // rule out for a user-method callee / a bare lambda literal. Without
        // this, `return outer();` here retains a reference the callee ALREADY
        // transferred, inflating the returned closure's refcount by one — it
        // never reaches zero, so its envdtor never runs and the whole
        // chain (closure header, env, every capture cell) leaks permanently.
        if (v && rs->value && !currentReturnTypeNode_ && lamRet &&
            v->getType()->isPointerTy() &&
            lamRet->kind == sema::TypeRef::Kind::Function &&
            !isPlusOneClosureProducer(rs->value.get()) &&
            !isUserMethodClosureReturnCall(rs->value.get()) &&
            !isLambdaClosureReturnCall(rs->value.get()) &&
            !returnedClosureLocalOwnsPlusOne(rs->value.get()))
            emitClosureRetain(v);
        // ⚠ fix #2703 (v0.240) — THIS ARM DELIBERATELY KEEPS `currentReturnType
        // Node_` AND MUST NOT BE MOVED TO `returnReprTypeNode()`. It is a +1
        // MINT, not a representation conversion: it hands the caller an owned
        // reference the caller is expected to release. Axiom A4 (spec line
        // 6351, v0.240) — "Accessor read faces are lend-only: property and
        // indexer *types* carry no `owned`, and a getter may not return a fresh
        // owned value" — makes +0 MANDATORY for an accessor read face, and both
        // read call sites implement exactly that (LlvmBackend_Exprs.cpp ~4342
        // and the indexer path in LlvmBackend_Assign.cpp hand the result back
        // as a BORROW; #2304's producer recogniser deliberately excludes a
        // statement-bodied block getter). #2694 proved the consequence of
        // getting this wrong in the leak direction — an inherited mint no call
        // site discharges (LSan: 20 strings / 20 iterations). Minting here and
        // teaching the call sites to release would be the double-free
        // direction. `currentReturnTypeNode_` is never set inside an accessor
        // body, so keying on it IS the suppression.
        if (v && rs->value && currentReturnTypeNode_ &&
            v->getType()->isPointerTy()) {
            const sema::TypeRef* declRet =
                ctx_.resolvedType(*currentReturnTypeNode_);
            // fix #2196 / §28.9: a method whose declared return type is
            // nullable `string?` mints exactly +1 the same way the non-nullable
            // sibling does — the K1.5/K3.13 RETURN-escape reasoning (an announced
            // return is a fresh transfer to the caller) applies to `string?` just
            // as it does to `string`. The earlier `!declRet->nullable` gate
            // suppressed the +1 mint for nullable-string returns, leaving the
            // caller's `var s := M()` bind to leak the fresh +1 string. The
            // existing null-safety of bz_string_retain (no-op on a sentinel/
            // null) makes the extension sound — a `return null` literal is
            // still a null carry, not a +1. // per §4.6, §28.3, §28.9
            if (declRet &&
                declRet->kind == sema::TypeRef::Kind::Primitive &&
                declRet->primitive == sema::PrimitiveType::String &&
                !isPlusOneStringProducer(rs->value.get()) &&
                !returnedStringLocalOwnsPlusOne(rs->value.get()))
                emitStringRetain(v);
        }
        // #429 (closure RETURN category) / §27.5 / §28.9 / §28.3: a method
        // whose declared return type is a function type (Kind::Function) mints
        // EXACTLY +1 for the returned closure cell, transferring ownership of
        // one reference to the caller. Mirrors the string RETURN-escape logic
        // immediately above, but for closures via emitClosureRetain. We
        // TRANSFER (no retain) when the returned expression already owns a +1:
        //   * a +1 PRODUCER expression (a lambda, a method-ref, or a user-
        //     defined closure-returning call — isPlusOneClosureProducer or
        //     isUserMethodClosureReturnCall), whose single reference flows out;
        //   * an IDENTIFIER naming a closure-typed LOCAL that owns a +1 in its
        //     slot (returnedClosureLocalOwnsPlusOne — its single +1 flows out).
        // Otherwise (a param read, a field read, an identifier borrow not owning
        // a +1) the value is a BORROW → RETAIN via emitClosureRetain to mint
        // the +1. bz_closure_retain is null-safe (a null closure is a no-op).
        // ⚠ fix #2703 (v0.240): the CLOSURE twin of the string mint above, and
        // it keeps `currentReturnTypeNode_` for the identical axiom-A4 reason —
        // see the note there. Do not move this arm to `returnReprTypeNode()`.
        if (v && rs->value && currentReturnTypeNode_ &&
            v->getType()->isPointerTy()) {
            const sema::TypeRef* declRet =
                ctx_.resolvedType(*currentReturnTypeNode_);
            // fix #3804 (§27.5 / §15.3 / §28.9, v0.294): `!isLambdaClosureReturnCall`
            // joins the existing transfer exclusions — see the sibling arm
            // above (the lambda-body-return twin) for the full argument.
            // `return outer();` at a METHOD's own top level (`outer` a
            // lambda-typed LOCAL) is the exact shape issue #3804's own
            // probes need: without this exclusion, the method mints a
            // SECOND +1 on a reference `outer`'s own body already
            // transferred, so the returned closure's refcount never reaches
            // zero and its envdtor (and therefore every capture cell it is
            // the designated #3426 owner of) never runs — a permanent leak
            // of the closure header, its env, and its captures, not merely
            // the 26000 B / 1500-allocation figure LSan reports at 500
            // iterations of issue #3804's own probes.
            if (declRet && declRet->kind == sema::TypeRef::Kind::Function &&
                !isPlusOneClosureProducer(rs->value.get()) &&
                !isUserMethodClosureReturnCall(rs->value.get()) &&
                !isLambdaClosureReturnCall(rs->value.get()) &&
                !returnedClosureLocalOwnsPlusOne(rs->value.get()))
                emitClosureRetain(v);
        }
        // fix #1747 / §28.9 / §9.7 / §28.3: a method whose declared return
        // type is a RECORD with at least one `string` field does NOT mint
        // an extra +1 on each string field — the constructor's NewExpr arm
        // already wired each string field to its +1 producer (an
        // interpolation or `+` for the producer case, or the field's
        // declaration initializer for the literal case). The callee is
        // THUS NOT in the +1-minting business for `return new P(...)`;
        // the record VALUE carries its fields' +1 references to the
        // caller, and the caller's non-escaping `var p := Mk()` local
        // balances them at scope end via the recordStringFields
        // Disposal. The round trip neither leaks (the +1 is released by
        // the caller) nor dangles (no alias survived the scope — the
        // escape walk proves the local never escaped). A literal field
        // initializer is a sentinel — its bz_string_release at scope
        // end is a no-op (the literal's rc is 0 / sentinel).
        //
        // NARROW SCOPE (per the lane's prior decline): the caller-side
        // registration is restricted to a non-escaping `var p := <call>`
        // whose return type is a record with string fields. A NewExpr
        // initializer (the constructor itself is the sole +1 owner), a
        // copy (`var q := p`), a with-copy, a by-value-arg, a
        // collection/field store, or a nullable result keep the prior
        // safe-leak behaviour — the walker over-approximates them as
        // escapes and they are not registered for release. No code
        // path emits here — the prior decline pinned this scope and
        // no callee-side retain is needed.
        // fix #568 / §14.1 / §7.1: when the declared return type is a nullable
        // VALUE type whose concrete form is reached only through the active
        // monomorphization (`T?` with T = a value type — prim/enum/record), the
        // function return type is the tagged {i8 hasValue; T} struct (lowerTypeRef
        // above), but `return null` / `return <bare T value>` produce a null ptr /
        // a bare value. Wrap them into the nullable struct here, where the sema
        // return type is in scope — emitReturn's coercion only matched exact LLVM
        // value types and missed the record/enum payload (specialized struct ≠
        // the param's bare struct → IR-verify failure). A non-generic nullable
        // value return already lowers correctly through the same struct, so this
        // is a no-op there (v already IS the struct).
        // fix #2703 (§7 / §14.1 / §23.3, v0.240): keyed on the REPRESENTATION
        // node — the same arm #2716 reports for the LAMBDA body form. A plain
        // `int?` block getter happens to survive without this (emitReturn has
        // its own exact-width nullable-prim fallback), but the shapes with no
        // such backstop do not: `property L: long? … return 42;` failed IR
        // verification pre-fix ("ret i32 42" against `{ i8, i64 }`), because
        // only this call site performs the #843 narrower-numeric widen before
        // the wrap. The `T?`-through-monomorphisation and layout-identical
        // record-payload shapes reach only this arm too.
        // fix #2716 (§27.3 "Lambda expressions" / §7.1, v0.240): the LAMBDA half
        // of this same arm — the third body-form asymmetry after #2688
        // (`string`) and #2691 (closure). `lowerLambda` nulls
        // `currentReturnTypeNode_` (#2515) and a lambda has no declared
        // return-type NODE at all, so a BLOCK-bodied lambda reached this arm
        // with nothing to key on while its EXPRESSION-bodied twin performs the
        // identical promotion from `retSemaTy` (the #541 branch,
        // LlvmBackend_Exprs2.cpp ~1866-1883, numeric widen included).
        // `currentLambdaReturnSemaTy_` is exactly that `retSemaTy` — the field
        // #2688 introduced for the same reason — so the two forms now agree,
        // which is the invariant #2688 established.
        //
        // ⚠ This is the ONLY arm of the four that gets the lambda fallback.
        // Neither lambda body form ever calls `boxForTarget` or
        // `wrapAsIEnumerableForTarget` (zero occurrences on the expression
        // path), so adding those here would BREAK the agreement rather than
        // restore it. And the two +1-mint arms are untouched: they key on
        // `currentReturnTypeNode_`, and the lambda mints already have their own
        // arms above keyed on `!currentReturnTypeNode_ && lamRet`.
        //
        // ⚠ The issue's filed repro is STALE: `lambda(): int? => do return 3;
        // end` compiles and returns 3 on HEAD, because `emitReturn` has an
        // exact-width nullable-prim fallback. The real repro is the WIDENING
        // shape — `lambda(): long? => do return 42; end` — which fails IR
        // verification ("ret i32 42" against `{ i8, i64 }`), since the #843
        // narrower-numeric widen happens only inside
        // `coerceReturnToNullableValue`.
        const sema::TypeRef* nullCoerceRet = nullptr;
        if (v) {
            if (const ast::Type* rn = returnReprTypeNode())
                nullCoerceRet = ctx_.resolvedType(*rn);
            else if (lamRet)
                nullCoerceRet = lamRet;      // fix #2716
        }
        {
            if (const sema::TypeRef* declRet = nullCoerceRet) {
                // #1767 / §19.2: the returned expression's own sema type is a
                // nullable REFERENCE (`Object?`) — e.g. an all-null `case … end`,
                // whose `null` arms default to `Object?` — while the declared
                // return is a nullable VALUE (`int?`). Such a value lowers to a
                // bare `ptr` (a runtime PHI, not a constant null), which the
                // reference→value coercion below rewrites to the {i8 0, T 0} null
                // constant (sema proves it null, §20). Passing this flag keeps a
                // value-nullable source that mis-lowered to a ptr from being
                // silently nulled (it must still fail the verifier).
                const sema::TypeRef* srcTy =
                    rs->value ? ctx_.exprType(*rs->value) : nullptr;
                bool srcNullableRef = srcTy && srcTy->nullable &&
                                      !isNullableValueTy(srcTy);
                v = coerceReturnToNullableValue(
                    v, declRet,
                    rs->value && isUnsignedTargetExpr(*rs->value),  // #843
                    srcNullableRef,
                    rs->value.get());  // #2925: the constant-narrowing gate
            }
        }
        // §5.1 (#746 R41-H): a numeric return value narrower than the declared
        // return type is implicitly widened (e.g. `return arr[0]` where
        // arr: Array<byte> in an int-returning method, or `return aByte`).
        // Without this the bare i8/i16 was `ret`'d into a wider integer function
        // (IR-verify failure).
        // fix #2925 (§5.1 "Constant narrowing", v0.240): `coerceInitializer
        // ToSlot`, not `coerceNumericToType` — the latter is §5.1's WIDENING
        // direction only (`srcBits < targetBits`), and §5.1 admits the other
        // one for a compile-time constant, which sema now re-types at `return`
        // as at every other conversion site. A LITERAL lowers at the re-typed
        // width, but `return K;` (a `const` identifier loads its i32 slot) and
        // `return 40 + 2;` (folded at i32) reached `ret i32` from an i8/i16
        // function — malformed IR for a program §5.1 makes well-typed.
        // A no-op for ptr/struct returns and an already-matching width.
        // GATED on sema already typing `rs->value` at the return width — a
        // generic body compiled against an i32 placeholder must keep failing
        // loudly rather than have its wide value truncated to fit.
        if (v && rs->value && currentFn_ &&
            !currentFn_->getReturnType()->isVoidTy())
            v = coerceExprValueToSlot(v, currentFn_->getReturnType(),
                                      rs->value.get(),
                                      isUnsignedTargetExpr(*rs->value));
        // §28.3 ownership transfer: if the return expression is a bare
        // `using var` local, exclude it from disposal — ownership moves
        // to the caller.  Without this, the local would be freed before
        // `ret`, giving the caller a dangling pointer.
        //
        // fix #4955 (§17.8, §28.3, v0.307): `return y!;` is the same transfer.
        // §17.8: "if `x` has ownership state owned …, `x!` is owned" — the
        // null-forgiving operator neither moves nor copies, so `y!` IS `y` and
        // the returned local must be excluded from its scope-end auto-drop
        // exactly as a bare `return y;` is. Before this the `!` hid the
        // identifier, `y` was auto-dropped before `ret`, and the caller read
        // (and disposed) the freed object — a heap-use-after-free. Peel only
        // postfix `!` layers (never `as`, whose result is a borrow, §17.4).
        llvm::AllocaInst* retSlot = nullptr;
        const ast::Expr* retCore = rs->value.get();
        while (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(retCore)) {
            if (!pb->operand) break;
            retCore = pb->operand.get();
        }
        if (rs->value) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(retCore)) {
                sema::Symbol* sym = ctx_.exprSymbol(*id);
                if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
                    auto lit = locals_.find(loc->decl);
                    if (lit != locals_.end()) retSlot = lit->second;
                }
            }
        }
        if (retSlot) {
            for (auto& scope : disposeStack_) {
                scope.erase(
                    std::remove_if(scope.begin(), scope.end(),
                        [retSlot](const auto& e) {
                            // #1418 / §27.5 / §28.9: if the returned local is a
                            // lambda-CAPTURED value local, its slot is the shared
                            // capture-cell holder. Returning it yields a COPY of the
                            // cell's contents (a value type), NOT ownership of the
                            // cell — so the cell's boxedCellFree must still fire.
                            // Only the ownership-transfer disposals of a returned
                            // `using var` (class dispose etc.) are excluded here; the
                            // runtime-managed cell free is preserved, or the cell
                            // leaks (the #1418 4-byte leak on `return count`).
                            // fix #4509: `cellRelease` is preserved beside
                            // them and for the same reason — returning the
                            // local yields a COPY of the cell's contents, not
                            // the frame's counted reference to the cell, which
                            // still has to be dropped here.
                            return e.slot == retSlot &&
                                   !e.boxedCellFree &&
                                   !e.boxedCellFreeDeferred && !e.cellRelease;
                        }),
                    scope.end());
            }
        }
        // CG21c (§31.2): if inside a try-with-finally, stash the return value
        // and route through the finally block instead of ret-ing now.
        // fix #1567 / §29.7/§28.10 (v0.182): cleanup runs in REVERSE
        // establishment order on the return edge, uniformly with the exception
        // edge. Dispose ONLY the try-body scopes (indices >= the innermost
        // finally's entry depth) here — the enclosing scopes established before
        // the try are disposed by the finally's normal-exit dispatch AFTER the
        // finally body runs (see mode=1 in lowerTry). The pre-v0.182 code ran
        // emitAllDisposals() before the finally, disposing an enclosing
        // `using var` BEFORE a finally that may touch it — a use-after-dispose.
        if (!finallyStack_.empty()) {
            auto& fr = finallyStack_.back();
            emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
            if (fr.retValSlot && v)
                builder_.CreateStore(v, fr.retValSlot);
            builder_.CreateStore(builder_.getInt32(1), fr.exitModSlot);
            builder_.CreateBr(fr.finNormalBB);
            return true;
        }
        // No enclosing finally: dispose all live scopes, then ret.
        emitAllDisposals();
        // fix #1607 / §28.3/§28.4 (#3459: `return;` exits a destructor early):
        // an early `return;` inside a user destructor body is a second
        // reachable exit from emitDisposerCore's bz_exc park (see
        // LlvmBackend_Disposal.cpp). Without restoring here, the still
        // in-flight propagating exception parked at disposer entry would stay
        // cleared forever on this path — turning the pre-existing "early
        // return skips owned-field disposal" deferred corner into a strictly
        // worse one (the exception itself silently vanishes, rather than just
        // skipping extra cleanup). Combine via select with any genuinely NEW
        // exception the body itself raised before this point, mirroring the
        // normal-completion restore.
        //
        // fix #2872: via disposerEntryExcHere(), so this restore is emitted ONLY
        // in the disposer's own function. A `return` inside a NESTED body emitted
        // mid-destructor-body (a lambda's closure fn, a lazily-built generic
        // specialization) otherwise built this `select` against the outer
        // disposer's entry load — a cross-function reference the IR verifier
        // rejects. Such a body is an ordinary callee: the destructor's bz_exc
        // park (#1607) already gives it a clean exception context.
        if (llvm::Value* entryExc = disposerEntryExcHere()) {
            llvm::Value* excNow = builder_.CreateLoad(ptrTy(), emitExcSlot());
            llvm::Value* isNew = builder_.CreateICmpNE(
                excNow, llvm::ConstantPointerNull::get(ptrTy()));
            llvm::Value* restored = builder_.CreateSelect(
                isNew, excNow, entryExc);
            builder_.CreateStore(restored, emitExcSlot());
        }
        emitReturn(currentFn_, v);
        return true;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        // fix #2655 / §21.9 / §28.3 / §28.9 (v0.237): a BARE owned-producer
        // statement (`new Box();`) is an unbound owned temporary whose K8
        // boundary is the end of this statement (§28.3: "in an expression
        // statement, 'end of the full expression' and 'end of the statement'
        // coincide"). Free it here, exactly as the §17.13 discard spelling of
        // the same statement does — this branch is a verbatim copy of the
        // DiscardStmt::disposeOwnedValue branch below, but the FLAG IT READS IS
        // SET ELSEWHERE: sema sets it inside §21.9's own walk
        // (src/sema/ExprTypecheck.cpp, the single setter), NOT the ownership
        // pass, and NOT under `producerState == Owned`.
        //
        // ⚠ That distinction is the whole correctness argument, so do not
        // "simplify" it back. An earlier draft did key on the ownership pass's
        // `producerState == Owned`, which is far WIDER than §21.9's rejection —
        // it also admits unannotated static factories, imported `owned` returns
        // (§21.9's dynamic_cast yields null for a null ms->decl), extern/FFI,
        // function-value calls, Deserialize/TakeCurrent, `await` Pattern B,
        // operator overloads, `??`, arm-meet and `move x`, and §21.9 further
        // requires !nullable && kind == Class. Under that gate this arm ran USER
        // DESTRUCTOR CODE at end of statement, contradicting §28.9's "runs no
        // user code" and proposal 0077 §2.2 (demonstrated: the destructor ran).
        // Deriving the flag inside §21.9's walk makes the freed set a strict
        // subset of what §21.9 inspected and declined to reject, so the two
        // cannot disagree by construction.
        // Before this, the value was produced and dropped on the floor with no
        // free emitted anywhere and no diagnostic (§21.9 hard-errors only the
        // non-trivial-dispose case), so every bare trivial `new` leaked.
        if (es->expr && es->disposeOwnedValue) {
            // The value is the sole owner (sema admitted only §21.9's
            // `ownedProduced` shapes — `new`, an `owned`-returning user call, an
            // owning-collection drain — never a borrow), so it is freed EXACTLY
            // once: no leak, no double-free. dynamicDispose dispatches
            // Dispose+reclaim through the object's own vtable, null-guarded.
            //
            // Scope of the trivial-dispose claim, stated precisely: §21.9's walk
            // classifies the STATIC type, while the dispatch is through the
            // object's vtable, so two shapes can still reach a user destructor —
            // a factory returning `owned Base` whose runtime value is a
            // destructor-bearing `Derived`, and a class made non-trivial by an
            // owning-inline-array field (`[N owned]T`), which the walk does not
            // detect because it tests only `fs->modifiers` for "owned", never the
            // field's type. Neither is unsound: the value is the sole owner and
            // §28.3 K8.1 mandates the reclaim, and the already-shipped `_ := …`
            // spelling has no trivial-dispose gate at all, so this arm is
            // strictly narrower than a path live since #1009. It is only the
            // "runs no user code" phrasing that would overclaim.
            //
            // ORDER MATTERS: lower the expression FIRST, in the enclosing block
            // scope, and only THEN open the one-entry dispose scope. Opening it
            // first (the shape #1009's discard arm uses) would RE-HOME every
            // disposal that expression lowering itself pushes onto
            // `disposeStack_.back()` — notably the `ensureOutVarSlots` /
            // `ensureCtorOutVarSlots` registrations for an inline `out owned var`
            // argument (LlvmBackend_Calls2.cpp) — from the block scope into this
            // STATEMENT scope, so `new T(out owned var x);` would dispose `x` and
            // null its slot at end of statement while `x` is still live for the
            // rest of the block. Lowering first leaves those registrations where
            // they belong.
            //
            // No unwind-edge entry is needed: between the store below and
            // emitDisposals nothing throwable is emitted (an alloca and a store),
            // and a throw from the producer itself is covered independently by
            // emitNewPendingCheck / emitUnwindDisposals (LlvmBackend_Disposal.cpp)
            // — which is why this arm does not claim both-edge coverage it does
            // not provide.
            llvm::Value* v = lowerExpr(*es->expr);
            if (!diverged_ && v) {
                llvm::AllocaInst* slot = entryAlloca(ptrTy());
                builder_.CreateStore(v, slot);
                disposeStack_.emplace_back();
                Disposal d{slot, nullptr};
                d.dynamicDispose = true;
                disposeStack_.back().push_back(d);
                emitDisposals(disposeStack_.back());
                disposeStack_.pop_back();
            }
            return diverged_;
        }
        // fix #2728 / §32.6 + §28.3 + §21.9 (v0.240): a BARE FFI-buffer factory
        // statement — `CString.From(s);` on a line of its own — mints a malloc'd
        // buffer the Bezel side owns (§32.6: "The `CString` class is for
        // caller-owned strings: those that the Bezel side allocated and is
        // responsible for disposing") and binds it to nothing. §28.3: an unbound
        // owned temporary "is disposed at the end of the full expression in which
        // it is produced", with the boundary fixed in the same subsection — "in an
        // expression statement, 'end of the full expression' and 'end of the
        // statement' coincide (§21.9)". Before this the value was produced and
        // dropped on the floor with no free emitted anywhere (measured: 64
        // iterations leaked 64 allocations / 13120 B).
        //
        // Why this is a SEPARATE arm from #2655's above, not a widening of it:
        // that arm's flag is set only for §21.9's `ownedProduced` set, so
        // `disposeOwnedValue` is false here and the arm above already returned.
        // ⚠ For an ExpressionStmt that flag's ONLY setter is §21.9's walk in
        // src/sema/ExprTypecheck.cpp:9718 — NOT the ownership pass, which sets
        // the DiscardStmt flag (Ownership.cpp:2537). An earlier draft of this
        // comment attributed it to the ownership pass; the conclusion held (the
        // shape leaked rather than crashing through dynamicDispose) but the
        // mechanism named was wrong. Corrected per the #2728 review. Equally important, this must NOT use
        // `dynamicDispose`: an FFI buffer is a raw malloc with no object header,
        // so a vtable dispatch would read the first bytes of the marshalled string
        // data as a function pointer. It runs no user code — there is no user type
        // involved at all — so §28.9's "runs no user code" holds trivially, which
        // is exactly the gate #2655's review insisted on.
        {
            bool bareIsRawBytes = false;
            if (es->expr && isFfiBufferFactoryCall(*es->expr, bareIsRawBytes)) {
                llvm::Value* v = lowerExpr(*es->expr);
                if (!diverged_ && v) {
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    builder_.CreateStore(v, slot);
                    emitFfiBufferTempFree(slot, bareIsRawBytes);
                }
                return diverged_;
            }
        }
        if (es->expr) lowerExpr(*es->expr);  // side effects only (CG3: none)
        return diverged_;  // CG40: a throw-expression terminated the block
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) {
            if (ds->discardOutOwnedVar) {
                // fix #1892 / §17.13 / §28.5: FIRST, an announced-consume discard
                // of a named owning-COLLECTION local (OwningArray/OwningDictionary/
                // OwningSet/OwningStack/OwningQueue/OwningLinkedList), of ANY decl
                // form (plain `var` OR `using var`). This is handled BEFORE the
                // class reclaim path below because explicitDisposeOwnedReceiver-
                // Slot mis-serves a collection: for a plain-`var` collection it
                // returns null (unwindOwnedLocalClass excludes collections) so the
                // container was never disposed (the #1892 leak), and for a
                // `using var` collection it returns the slot, feeding the collection
                // struct into the CLASS dispatch (emitDisposeAndReclaim reads its
                // first word as a vtable → SEGV). A collection is neither: it frees
                // via a runtime call, cascading disposal into each owned element
                // (§28.5). Dispose the container HERE at the discard point (§17.13:
                // "disposed at the end of the discard statement") via the shared
                // collection-disposal machinery — registerUsingDisposal builds the
                // correct bz_array_free / bz_dict_free / bz_llist_free with the
                // element-disposer cascade, pushed into a one-shot dispose frame and
                // emitted now — then NULL the slot so the collection's own scope-end/
                // unwind disposal (a `using var`'s registerUsingDisposal at its
                // LocalDecl, or a discard-discharged plain `var`'s dischargedByDiscard
                // registration — both fire on both edges and are null-safe) reloads
                // null and no-ops. The container and its elements are disposed
                // EXACTLY ONCE, no double-free; the pre-discard-throw exception edge
                // is covered by that both-edges registration. // per §17.13, §28.5
                const ast::LocalDecl* collLd = nullptr;
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        ds->value.get()))
                    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(
                            ctx_.exprSymbol(*id)))
                        if (auto* ld2 = dynamic_cast<const ast::LocalDecl*>(
                                loc->decl))
                            if (collectionTypeOfLocal(ld2) ||
                                dictTypeOfLocal(ld2) ||
                                linkedListTypeOfLocal(ld2))
                                collLd = ld2;
                if (collLd && !diverged_) {
                    disposeStack_.emplace_back();
                    registerUsingDisposal(collLd);
                    emitDisposals(disposeStack_.back());
                    disposeStack_.pop_back();
                    auto lit = locals_.find(collLd);
                    if (lit != locals_.end() && lit->second) {
                        llvm::Type* ety = nullptr;
                        llvm::Value* addr = boxedStorage(lit->second, ety);
                        if (addr && ety && ety->isPointerTy())
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                    }
                } else if (!diverged_) {
                    // §17.13 / §28.3 v0.184 (#1387, generalized by #1651): `_ := x`
                    // where `x` is a named owned CLASS/INTERFACE binding (a plain
                    // owned local of any decl form, including the `out owned var`
                    // inline local, or an `owned` parameter) — an announced-consume
                    // §17.13 discharge. Emit null-guarded dispose+reclaim of the
                    // local's slot and null it so any other scope-end / exception-
                    // path disposal registered for the same slot (e.g.
                    // ensureOutVarSlots' unwindOnly disposal) is a no-op after this
                    // point. explicitDisposeOwnedReceiverSlot finds the slot address
                    // — the same resolver `x?.Dispose()` / `x.Dispose()` uses, so it
                    // is already general across LocalSymbol (any decl kind) and
                    // `owned` ParameterSymbol receivers.
                    //
                    // fix #2675 / §17.13 / §28.3 / §28.9 (v0.278): the
                    // announced-consume path's class-vtable dispatch (the
                    // `emitDisposeAndReclaim` below) reads the slot's word 0 as a
                    // Bezel vtable and indirect-calls slot 2 — a wild call for a
                    // vtable-less built-in class (StringBuilder, Random,
                    // EventSource<T>, AsyncEventSource<T>, TaskCompletionSource<T>,
                    // OwningTaskCompletionSource<T>, Channel<T>, Atomic<T>,
                    // AtomicInteger<T>, Stopwatch, Mutex, Semaphore, FileStream,
                    // CancellationTokenSource, the Buffer family). A
                    // `using var sb := new StringBuilder(); _ := sb;` therefore
                    // SIGSEGV'd with pc=0 (measured on 39c03b1: `DEADLYSIGNAL`,
                    // `ERROR: AddressSanitizer: SEGV on unknown address
                    // 0x000000000000` — the same signature #3157/#2973 recorded
                    // for `Random` and Channel<T>). #2655 fixed the FRESH-producer
                    // arm (`_ := new StringBuilder();`) but left the named-binding
                    // arm on the class dispatch.
                    //
                    // Detect the named-owned-binding vtable-less shape HERE — same
                    // place the explicit-Dispose chain detects it (#2967/#2921)
                    // — and route through `emitVtablelessBuiltinRelease`, the same
                    // call the `using var` scope-exit auto-drop chain routes
                    // through (#1185 / #2973). Both paths now agree on what
                    // releases a given built-in (the no-drift property the
                    // original arms were introduced to enforce), and the slot is
                    // still nulled afterwards so a following scope-end / unwind
                    // auto-drop reloads null and skips — exactly-once.
                    //
                    // The named-binding `discardOutOwnedVar` flag is set by the
                    // ownership pass only when the binding's state is Owned and
                    // not a `using var` borrow-pin alias (#3253) — so this
                    // emission runs only for the Genuine-Owner shape and cannot
                    // double-dispose a borrow-pin (whose real owner keeps the
                    // obligation). A borrow-state operand is also already
                    // excluded by `lookupState(...) == Owned`. The free path
                    // goes through `builtinTypeName` + the same
                    // `common::isVtableLessBuiltinClassName` registry gate the
                    // `using var` chain and the explicit-Dispose chain use, so
                    // adding a new built-in here is a line in
                    // `usingAutoDropVtablelessBuiltin` / `emitVtablelessBuiltin-
                    // Release`, not a third copy of the detection code in this
                    // function. When no release is known for the named built-in,
                    // `emitVtablelessBuiltinRelease` returns false and we fall
                    // through to the class dispatch — the same "leak before wild
                    // jump" trade #2874 / #2973 record.
                    llvm::Type* slotTy = nullptr;
                    llvm::Value* slotAddr =
                        explicitDisposeOwnedReceiverSlot(*ds->value, slotTy);
                    // fix #4185 (§17.13 / §28.3 / §28.9): `explicitDisposeOwnedReceiverSlot`'s
                    // LocalSymbol arm only recognizes a plain (non-`using`) `var`
                    // local as a slot this scope must reclaim via
                    // `unwindOwnedLocalClass` — which is scoped to a USER class/
                    // interface with a generated Dispose (a `disposerFns_` entry).
                    // A vtable-less BUILTIN (StringBuilder, Mutex, Stopwatch, …)
                    // never has one, so a plain `var y := new StringBuilder();
                    // _ := y;` found NO slot here and this whole branch emitted
                    // NOTHING — measured: 568 B leaked under LSan (issue #4185,
                    // amplified at x200: `var y1 := new StringBuilder(); _ := y1;`
                    // repeated leaks one per iteration). The `using var` spelling
                    // of the identical discard was unaffected: `loc->isUsing`
                    // short-circuits to `lvalueAddr` before
                    // `unwindOwnedLocalClass` is ever consulted — which is why
                    // the defect surfaced only for the plain-`var` form.
                    //
                    // Recover the slot directly via `lvalueAddr` — bypassing
                    // `unwindOwnedLocalClass` rather than widening it — when the
                    // discarded value's TYPE is one `emitVtablelessBuiltinRelease`
                    // (two lines below) actually KNOWS HOW TO RELEASE.
                    // `ds->discardOutOwnedVar` is set by the ownership pass only
                    // for a genuinely Owned named binding (Ownership.cpp ~line
                    // 3283: `dstate->state == OwnershipState::Owned`), so this
                    // fallback is never reached for a borrow.
                    //
                    // fix-forward (review NEEDS-FIX, §25): the FIRST version of
                    // this fallback gated on `common::isVtableLessBuiltinClass
                    // Name` — which proves only "carries no Bezel vtable", a
                    // STRICT SUPERSET (47 names) of the 18
                    // `emitVtablelessBuiltinRelease` handles. For a name in the
                    // gap (`Shared<T>`, `Weak<T>`, `JsonSerializer`, …) that
                    // predicate is TRUE, `emitVtablelessBuiltinRelease` returns
                    // false (no arm matches), and the `else` branch below —
                    // unchanged, pre-existing code — calls
                    // `emitDisposeAndReclaim(obj)` on the slot THIS fallback
                    // just newly supplied: the exact wild jump #2675 exists to
                    // prevent, converting a leak into a SIGSEGV. Measured on
                    // that version: `var sh := new Shared<Widget>(new Widget());
                    // _ := sh;` → "SEGV ... Maybe a wild jump?"; `var js := new
                    // JsonSerializer(); _ := js;` → "SEGV on unknown address
                    // 0x000000000010". `vtablelessBuiltinReleaseIsKnown` answers
                    // the NARROWER, correct question ("will the release two
                    // lines below actually succeed"), so a name in the gap now
                    // leaves `slotAddr` null and this whole branch emits
                    // NOTHING for it — a leak, the pre-#4185 (and still safe)
                    // behaviour — instead of reaching `emitDisposeAndReclaim`.
                    // Widening `unwindOwnedLocalClass` itself remains rejected
                    // for the reason already on record: it also drives
                    // `registerUnwindOwnedLocal`'s EXCEPTION-unwind
                    // registration, which dispatches through the SAME
                    // `emitDisposeAndReclaim` vtable call, so making it truthy
                    // for a vtable-less builtin would reintroduce the #2675
                    // jump on the unwind edge instead. // per §17.13, §28.3,
                    // §28.9, §25, #2675, #4185
                    if (!slotAddr) {
                        std::string_view fbBn;
                        if (const sema::TypeRef* fbRt =
                                ctx_.exprType(*ds->value))
                            fbBn = builtinTypeName(fbRt);
                        if (!fbBn.empty() &&
                            vtablelessBuiltinReleaseIsKnown(fbBn))
                            slotAddr = lvalueAddr(*ds->value, slotTy);
                    }
                    if (slotAddr) {
                        llvm::Value* obj =
                            builder_.CreateLoad(ptrTy(), slotAddr);
                        auto* nullVal = llvm::ConstantPointerNull::get(ptrTy());
                        auto* isNull  = builder_.CreateICmpEQ(obj, nullVal);
                        auto* dispBB  = llvm::BasicBlock::Create(
                            llvm_, "disc.disp", currentFn_);
                        auto* contBB  = llvm::BasicBlock::Create(
                            llvm_, "disc.cont", currentFn_);
                        builder_.CreateCondBr(isNull, contBB, dispBB);
                        builder_.SetInsertPoint(dispBB);
                        std::string_view bn;
                        if (const sema::TypeRef* rt =
                                ctx_.exprType(*ds->value)) {
                            bn = builtinTypeName(rt);
                        }
                        // fix #4224 (§17.13 / §28.3 / §28.9, v0.298): whether
                        // `bn` is a vtable-less builtin AT ALL — the SAME
                        // `common::isVtableLessBuiltinClassName` authority the
                        // release-arm condition below already consults — must
                        // be known BEFORE deciding the `else` arm can safely
                        // call `emitDisposeAndReclaim`. Previously that arm's
                        // comment read "class/interface (or a vtable-less
                        // builtin no arm names yet — leak-before-wild-jump,
                        // #2973)" and called `emitDisposeAndReclaim`
                        // unconditionally either way. That was safe only while
                        // `slotAddr` could exclusively come from a genuine
                        // user-class `unwindOwnedLocalClass` resolution. Once
                        // `explicitDisposeOwnedReceiverSlot`'s `loc->isUsing`
                        // short-circuit (LlvmBackend_Calls2.cpp ~line 4622)
                        // started handing back a real slot for ANY `using var`
                        // binding regardless of type, an UNCOVERED vtable-less
                        // builtin (`Shared<T>`, `Weak<T>`, `JsonSerializer`,
                        // `BinarySerializer`, `SharedEventSource<T>`, the
                        // Buffer family, …) reaches this `else` with a real
                        // slot and no #4185 fallback gate in front of it (that
                        // fallback only fires when `explicitDisposeOwnedReceiverSlot`
                        // returned null in the first place — moot here, since
                        // the `using`-short-circuit already returned non-null).
                        // Measured: `using var sh := new Shared<Widget>(new
                        // Widget()); _ := sh;` → AddressSanitizer
                        // heap-buffer-overflow, 16-byte region (`bz_shared_new`'s
                        // allocation) — the exact #2675 wild jump, one level up.
                        bool isVtablelessBuiltin =
                            !bn.empty() &&
                            common::isVtableLessBuiltinClassName(bn);
                        if (isVtablelessBuiltin &&
                            emitVtablelessBuiltinRelease(bn, obj)) {
                            // vtable-less path: builtin released; null the
                            // slot so a following scope-exit / unwind
                            // registration reloads null and skips.
                            builder_.CreateStore(nullVal, slotAddr);
                            // #2310 / §29.8 / §30.1 (#973a residual): also null
                            // the FRAME capture slot of an `owned`
                            // state-machine parameter this `_ := p` discard just
                            // consumed (no-op outside a state-machine body, for
                            // non-parameter sources, for non-owned parameters).
                            // per §29.8 / §30.1
                            nullStateMachineOwnedParamCapture(
                                ds->value.get());
                        } else if (!isVtablelessBuiltin) {
                            // Genuine user class/interface, OR a reserved
                            // builtin INTERFACE head (isVtableLessBuiltinClassName
                            // excludes those by its own contract — they keep
                            // real dynamic dispatch, per BuiltinRegistry.cpp's
                            // isBuiltinInterfaceHead comment). Word 0 is a real
                            // Bezel vtable either way, so the slot-2
                            // dynamicDispose dispatch is safe.
                            emitDisposeAndReclaim(obj);
                            builder_.CreateStore(nullVal, slotAddr);
                            // #2310 / §29.8 / §30.1 (#973a residual): also null the
                            // FRAME capture slot of an `owned` state-machine parameter
                            // this `_ := p` discard just consumed (no-op outside a
                            // state-machine body, for non-parameter sources, for
                            // non-owned parameters). per §29.8 / §30.1
                            nullStateMachineOwnedParamCapture(ds->value.get());
                        } else {
                            // fix #4224: `bn` IS a vtable-less builtin
                            // (`isVtableLessBuiltinClassName` true) but
                            // `emitVtablelessBuiltinRelease` names no arm for
                            // it — a plain C struct with no Bezel vtable at
                            // word 0, so `emitDisposeAndReclaim` would be the
                            // #2675 wild jump. Leak instead (the same
                            // leak-before-wild-jump trade #2973 / #4185 make
                            // for this exact gap elsewhere): still null the
                            // slot so a following scope-exit/unwind auto-drop
                            // reloads null and no-ops — exactly-once "no
                            // dispose", never a double free. Collections
                            // (OwningArray/OwningDictionary/OwningSet/
                            // OwningStack/OwningQueue/OwningLinkedList) do NOT
                            // reach this arm — `discardOutOwnedVar`'s
                            // collection branch above (line ~3134) claims them
                            // first, for both `using var` and plain `var`; the
                            // collection gap on the UNRELATED explicit
                            // `?.Dispose()` path is #4221, open, deliberately
                            // left untouched here. // per §17.13, §28.3,
                            // §28.9, #2675, #2973, #4185, #4221
                            builder_.CreateStore(nullVal, slotAddr);
                            nullStateMachineOwnedParamCapture(ds->value.get());
                        }
                        builder_.CreateBr(contBB);
                        builder_.SetInsertPoint(contBB);
                    }
                }
            } else if (ds->disposeOwnedValue) {
                // §17.13 / §28.3 v0.150 (#1009): the ownership pass classified
                // this discard's value as an owned PRODUCER of a user class/
                // interface — an unbound owned temporary. Dispose it at the end
                // of the discard statement. Open a one-entry dispose scope so the
                // disposal fires on BOTH the normal edge (here) and the unwind
                // edge (if some later cleanup in this statement throws); the
                // produced value is the sole owner (producerState == Owned), so
                // it is disposed EXACTLY once (no leak, no double-free). The
                // dynamicDispose entry dispatches Dispose+reclaim through the
                // object's own vtable (correct for any runtime class/interface),
                // null-guarded.
                //
                // fix #3293 / §17.13 / §22.3 / §28.3 (v0.266): the one-entry
                // dispose scope must be emplaced AFTER `lowerExpr(*ds->value)`,
                // not before — mirroring the `ExpressionStmt::disposeOwnedValue`
                // shape (this file's line 2717/2721) which already lowers first
                // and for the documented reason ("Opening it first (the shape
                // #1009's discard arm uses) would RE-HOME every disposal that
                // expression lowering itself pushes onto `disposeStack_.back()`").
                // The re-home hazard materialised as a SEGFAULT after #2681
                // (commit ab417d5) started marking `out owned var` ctor
                // recipients as Owned: the ctor's `ensureCtorOutVarSlots`
                // (LlvmBackend_Calls2.cpp) pushes the recipient's slot disposal
                // onto `disposeStack_.back()` during `lowerExpr(*ds->value)` —
                // which on this discard arm was the STATEMENT scope, not the
                // enclosing BLOCK scope. The slot was disposed-and-nulled at end
                // of statement, and a subsequent `x.Id` read on the still-in-
                // scope `x` dereferenced null (SIGSEGV, exit 139). Lowering
                // first leaves those BLOCK-scope registrations where they
                // belong, and the discard's own one-entry scope (now opened
                // after the lower) still covers both edges for the discard's
                // own produced value, exactly as before. Scoped narrowly to
                // this one arm; the `disposeOwnedBuilderValue` and
                // `disposeOwnedCollectionValue` siblings have the same shape
                // and the same re-home exposure but do not currently surface a
                // ctor-recipient interaction, so they are left unchanged to
                // keep this commit minimal (#3294 candidates).
                llvm::Value* v = lowerExpr(*ds->value);
                if (!diverged_ && v) {
                    disposeStack_.emplace_back();
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    builder_.CreateStore(v, slot);
                    Disposal d{slot, nullptr};
                    d.dynamicDispose = true;
                    disposeStack_.back().push_back(d);
                    emitDisposals(disposeStack_.back());
                    disposeStack_.pop_back();
                }
            } else if (ds->disposeOwnedBuilderValue) {
                // fix #2655 / §17.13 / §28.3 / §28.9 (v0.237): the ownership
                // pass classified this discard's value as a fresh owned
                // StringBuilder producer — the spec's own §17.13 example
                // `_ := new StringBuilder();`. A StringBuilder's VALUE *is* a
                // bz_array_t* of string parts owning a +1 per part (#1185); it
                // is a decl-null builtin with no vtable, so dynamicDispose
                // (the class branch above) would misread its first word as a
                // vtable pointer. Free it through the SAME wiring the `using
                // var` StringBuilder path has used since #1185:
                // bz_array_free with the string-element release thunk, which
                // bz_string_release's each part (null-/sentinel-safe) and frees
                // the backing array. One-entry dispose scope for both-edge
                // coverage, exactly as the class branch above.
                disposeStack_.emplace_back();
                llvm::Value* v = lowerExpr(*ds->value);
                if (!diverged_ && v) {
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    builder_.CreateStore(v, slot);
                    Disposal d{slot, nullptr};
                    d.collection = true;
                    d.collectionElemDisposer = stringElementReleaseThunk();
                    disposeStack_.back().push_back(d);
                }
                if (!diverged_) emitDisposals(disposeStack_.back());
                disposeStack_.pop_back();
            } else if (ds->disposeOwnedCollectionValue) {
                // fix #1909 / §17.13 / §28.3 / §28.11: the ownership pass
                // classified this discard's value as an owned PRODUCER whose
                // type is an OWNING collection (OwningArray/OwningDictionary/
                // OwningSet/OwningStack/OwningQueue/OwningLinkedList) — the
                // producer-TEMPORARY counterpart of the disposeOwnedValue
                // class/interface branch above, for the one producer shape
                // that branch's decl-null gate excludes. Lower the value once,
                // then free it via the collection-family runtime dispatch
                // (freeUnboundOwnedOwningCollectionTemp), which cascades
                // disposal into the collection's owned elements — never
                // dynamicDispose (an owning collection is a decl-null
                // builtin with no vtable). No exception-edge dispose-stack
                // entry is needed here: the temporary's only operand
                // (ds->value) has already fully evaluated by the time this
                // runs, so there is no LATER sub-expression in this
                // statement whose throw could skip the free.
                llvm::Value* v = lowerExpr(*ds->value);
                if (!diverged_ && v)
                    freeUnboundOwnedOwningCollectionTemp(
                        v, ctx_.exprType(*ds->value));
            } else if (ds->disposeInlineOwnedArray) {
                // §17.13 / §15.3 (#1016): a discard `_ := expr` of a move-only
                // owning inline array. Dispose each non-null slot at end-of-
                // statement, mirroring the scope-end inlineOwnedArray disposal
                // path in emitDisposals. MoveOnlyArrayTransferWalker already
                // marked the source as transferred so registerInlineOwnedArray-
                // Disposal skips the scope-end disposal — no double-free.
                // Reach the [N x ptr] alloca through the source expression:
                // unwrap `move expr` to the bare identifier, then use localSlot
                // to find the alloca (the only valid form for an owning inline
                // array expression). // per §17.13, §15.3
                const ast::Expr* src = ds->value.get();
                if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(src))
                    if (ue->op == "move" && ue->operand)
                        src = ue->operand.get();
                llvm::AllocaInst* arrSlot = src ? localSlot(*src) : nullptr;
                if (arrSlot) {
                    if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(
                            arrSlot->getAllocatedType())) {
                        unsigned N = static_cast<unsigned>(arrTy->getNumElements());
                        disposeStack_.emplace_back();
                        Disposal d{arrSlot, nullptr};
                        d.inlineOwnedArray = true;
                        d.inlineSlotCount = N;
                        disposeStack_.back().push_back(d);
                        emitDisposals(disposeStack_.back());
                        disposeStack_.pop_back();
                    }
                }
            } else {
                // §17.13 / stdlib §5 / §28 (#1015): a discard of a FRESH-ownerless
                // collection materializer (`_ := items.ToArray()` / `.Sorted()` /
                // `.Reversed()` / `.ToDictionary()`) leaks its freshly-allocated
                // backing — the §28.6/§5.11 two-axis model makes a borrowing
                // Array/Dictionary a borrow of its ELEMENTS, so producerState
                // classifies it Borrowed and #1009's class/interface path above
                // does not fire, yet the BACKING (struct + buffer) is owned by
                // nothing. Reclaim it at end-of-statement by reusing the
                // #1004/#1006/#1013 helper, which frees the array/dict struct with
                // the matching per-element disposer (borrow elements → null, except
                // the Array<string> co-owned +1) and is a NO-OP for any non-
                // materializer discard (its narrow guards) → no UAF / double-free.
                llvm::Value* v = lowerExpr(*ds->value);
                if (!diverged_ && v)
                    freeUnboundOwnedBorrowingArrayTemp(
                        ds->value.get(), v, ctx_.exprType(*ds->value));
                // fix #2728 / §32.6 + §28.3 + §17.13 (v0.240): the same reclaim
                // for a DISCARDED FFI-buffer factory (`_ := CString.From(s);`).
                // Neither existing path covered it: the helper just above is a
                // no-op for it (its guards admit only array/dict materializers),
                // and the #1009 `disposeOwnedValue` arm did not fire because the
                // ownership pass classifies the FFI factories Borrowed — so the
                // buffer leaked (measured: 64 iterations leaked 64 allocations /
                // 12864 B). §17.13's discard is a discharge point and §28.3 puts
                // the boundary at the end of this statement.
                bool discIsRawBytes = false;
                if (!diverged_ && v &&
                    isFfiBufferFactoryCall(*ds->value, discIsRawBytes)) {
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    builder_.CreateStore(v, slot);
                    emitFfiBufferTempFree(slot, discIsRawBytes);
                }
            }
        }
        return diverged_;  // CG40
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        return lowerIf(*ifs);
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        return lowerWhile(*ws);
    }
    if (auto* fors = dynamic_cast<const ast::ForStmt*>(&s)) {
        // fix #3377 / §29.2 / §28.3: a for-in's iterable is a SCOPE-BOUND
        // consumer — the §29.2 desugaring binds it to `using var __source`,
        // disposed at the loop's own scope end. Publishing it here lets
        // `disposeFreshNewReceiverTempDeferred` reclaim the fresh receiver of
        // a deferred-activation source (`for var x in new Src().Items()`)
        // at that same scope end instead of leaking it. The gate matches on
        // the RECEIVER CHAIN of this expression, so a call in the loop BODY
        // never matches even though the flag stays set across the body.
        auto* savedScopeBoundInit = deferredRecvScopeBoundInit_;
        deferredRecvScopeBoundInit_ = fors->iterable.get();
        struct RestoreForInit {
            const ast::Expr** slot;
            const ast::Expr* saved;
            ~RestoreForInit() { *slot = saved; }
        } restoreForInit{&deferredRecvScopeBoundInit_, savedScopeBoundInit};
        // §30.7: `await for var x in asyncSeq do ... end` — async iteration.
        // Must run before the synchronous for-handlers, which would misread the
        // async-enumerator source.
        if (fors->isAwait) {
            if (lowerAwaitFor(*fors)) return false;
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "not yet implemented: codegen for this `await for` statement "
                "(only `await for` over an async-iterator source is supported "
                "(§30.7); `with` cancellation / owned-async iteration are "
                "deferred)");
            return false;
        }
        if (lowerRangeFor(*fors)) return false;
        // issue #370 / §4.8: `for var k in <dict>.Keys` — iterate dict keys.
        // (`<dict>.Keys` has no sema element type, so this must run before the
        // generic for-handlers; it recognizes the AST shape directly.)
        if (lowerDictKeysFor(*fors)) return false;
        // issue #537 / §4.8: `for var v in <dict>.Values` — iterate dict values.
        // Mirrors lowerDictKeysFor but snapshots values (genericArgs[1]) instead
        // of keys; must also run before the generic for-handlers.
        if (lowerDictValuesFor(*fors)) return false;
        // #548 / stdlib §4.8 v0.135: `for var e in dict` — entry iteration over
        // a non-owning Dictionary<K,V>; the loop variable is DictionaryEntry<K,V>.
        // Must run before the generic for-handlers (DictionaryEntry has no user
        // TypeSymbol so the array/IEnumerable paths would fall through).
        if (lowerDictEntryFor(*fors)) return false;
        // issue #370 / §4.10: `for var x in <set>` over a dict-backed Set<T>.
        // Must precede lowerArrayFor (Set is a collection-type name but is NOT
        // array-backed when its element routes through the generic hash table).
        if (lowerDictBackedSetFor(*fors)) return false;
        // §15.1: `for var x in a` over an inline array.
        if (lowerArrayFor(*fors)) return false;
        // §4.6: `for var c in s` over a string — iterates chars.
        if (lowerStringFor(*fors)) return false;
        // §3.3 / §29.2 / #971 item 2: `for var c in sp` over a
        // ReadOnlyStringSpan — iterates code points over the span's
        // [start, start+len) sub-range of the source string. Runs after
        // lowerStringFor (string is a more specific check) and before the
        // enumerable paths (which would otherwise fall through to the NYI).
        if (lowerStringSpanFor(*fors)) return false;
        // stdlib §5: `for var x in <lazy LINQ pipeline>` — iterate the deferred
        // enumerator without materializing it.
        if (lowerLazyLinqFor(*fors)) return false;
        // #639 / stdlib §4.5/§4.7/§4.11 v0.135: `for var x in coll.Drain()`
        // ownership-transferring drain over OwningArray<T>, OwningLinkedList<T>,
        // OwningStack<T>, OwningQueue<T>.
        // Must run BEFORE lowerDictDrainFor and lowerIEnumerableBuiltinFor.
        if (lowerOwnedSeqDrainFor(*fors)) return false;
        // #628 / stdlib §4.9 v0.135: `for var entry in ownedDict.Drain()`
        // ownership-transferring drain over OwningDictionary<K,V>.
        // Must run BEFORE lowerIEnumerableBuiltinFor (which also handles
        // IOwningEnumerable<T> but via vtable dispatch — incorrect for the
        // dict drain case, which uses the bz_dict_drain_next_* runtime).
        if (lowerDictDrainFor(*fors)) return false;
        // §29.2 (B1): `for var x in items` where items is IEnumerable<T>
        // typed as a builtin interface — treat runtime value as bz_array_t*.
        if (lowerIEnumerableBuiltinFor(*fors)) return false;
        // §29.2 (A): `for var x in obj` over user-defined IEnumerable<T>.
        if (lowerIEnumerableFor(*fors)) return false;
        // stdlib §4.13 (#728): `for var x in <LinkedList<T>>` — walk the node
        // chain via the runtime list cursor (not array-backed).
        if (lowerLinkedListFor(*fors)) return false;
        // Non-range, non-array, non-string, non-IEnumerable iterables.
        ctx_.diagnostics().report(
            Severity::Error, s.span.begin,
            "not yet implemented: codegen for this for statement");
        return false;
    }
    // CG11: break / continue jump to the innermost loop's exit /
    // condition block. CG13: they first dispose the loop body's (and
    // any nested) `using var`s. Both terminate the current block.
    // CG21c (§31.2): if inside a try-with-finally, route through finally.
    if (auto* brk = dynamic_cast<const ast::BreakStmt*>(&s)) {
        if (!loopStack_.empty())
            return lowerLoopJump(brk->targetLoopOffset, /*isBreak=*/true);
        return false;
    }
    if (auto* cnt = dynamic_cast<const ast::ContinueStmt*>(&s)) {
        if (!loopStack_.empty())
            return lowerLoopJump(cnt->targetLoopOffset, /*isBreak=*/false);
        return false;
    }
    // CG13: a `do … end` block is a disposal scope (§A.6, §28.3).
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        return lowerScopedBlock(db->body);
    }
    // CG20: throw — store the exception (a plain `throw;` re-raises the
    // one already in flight) and branch to the unwind target.
    if (auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        llvm::Value* e = th->value ? lowerExpr(*th->value) : nullptr;
        // #387 / §31.1: pass the thrown expression's AST so emitThrowValue can
        // free the caught exception when this `throw <newExpr>` leaving a catch
        // arm provably does not reference it.
        emitThrowValue(e, th->value.get());
        return true;
    }
    // CG20: try / catch.
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        return lowerTry(*trys);
    }
    // CG38: statement-position `case`.
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        return lowerCaseStmt(*cs);
    }
    // §29.3: yield return / yield break inside an iterator method. This is the
    // `Yield` suspend point: it spills the suspend-locals into the shared
    // state-machine frame, transitions the state index, and returns from
    // MoveNext. The frame plumbing (smStateStruct_/smSelf_/smAllocaToField_/
    // smStateGEP_/smStateBBs_/smNextSuspendState_) is the kind-agnostic
    // scaffolding from LlvmBackend_Iterators.cpp; the value-producing pieces
    // (current_val store, ret true/false) are yield-specific. The §30 `await`
    // suspend point (Phase 2b) reuses the same spill/restore + state-index
    // machinery here with await-specific suspend/resume actions (register a
    // task continuation, return the in-progress Task; resume reads the result).
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        // §29.3 (Yield): a synchronous iterator's `yield` returns a bool from
        // MoveNext. §30.7 (AsyncYield): an ASYNC iterator's `yield` completes the
        // per-advance Task<bool> and returns VOID from the Resume continuation
        // (the same suspend/spill machinery; only the suspend ACTION differs).
        bool isAsyncYield = (smKind_ == SuspendKind::AsyncYield);
        if (smStateStruct_ && smSelf_ &&
            (smKind_ == SuspendKind::Yield || isAsyncYield)) {
            // Save all suspend-locals to the state frame before yielding.
            for (auto& [allocaSlot, fieldIdx] : smSpillsInFrameOrder()) {  // fix #3504
                llvm::Value* fptr = builder_.CreateStructGEP(
                    smStateStruct_, smSelf_, fieldIdx);
                llvm::Value* curVal = builder_.CreateLoad(
                    allocaSlot->getAllocatedType(), allocaSlot);
                builder_.CreateStore(curVal, fptr);
            }

            if (ys->isBreak) {
                // §29.7 / §30.9 (#721 C2): a `yield break` exiting a
                // try-with-finally must run the finally on its way out — just
                // like return/break/continue. Route through the active finally
                // (exit mode 4); the post-finally dispatch (lowerTry) performs
                // the terminal state=-1 + ret. Without this the finally is
                // silently skipped on the yield-break edge.
                if (!finallyStack_.empty()) {
                    // fix #1598 / §29.7/§28.10 (v0.182): dispose only the
                    // try-body scopes (>= this finally's entry depth) before
                    // routing to the finally — the yield-break twin of the
                    // return/break/continue fixes. The enclosing scopes (an
                    // outer `using var` established before the try) are
                    // disposed by the finally's mode-4 dispatch AFTER the
                    // finally body runs, mirroring mode 1's reverse-
                    // establishment order. Pre-fix this site disposed nothing
                    // at all before jumping to the finally.
                    auto& fr = finallyStack_.back();
                    emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
                    builder_.CreateStore(builder_.getInt32(4), fr.exitModSlot);
                    builder_.CreateBr(fr.finNormalBB);
                    return true;
                }
                // yield break: set state=-1; sync → ret false, async → complete
                // the per-advance task with false (exhausted) + ret void.
                builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
                if (isAsyncYield) {
                    emitAsyncIterComplete(/*hasValue=*/false);
                    builder_.CreateRetVoid();
                } else {
                    builder_.CreateRet(builder_.getFalse());
                }
                return true;
            }
            // yield return expr: store to current_val, set next state; sync →
            // ret true, async → complete the per-advance task with true + ret void.
            if (ys->value) {
                llvm::Value* v = lowerExpr(*ys->value);
                if (v) {
                    // #1102B / §7.1: for an `IEnumerable<T?>` iterator the element
                    // slot is the tagged nullable-value struct `{i8,T}`; a bare
                    // `yield return <T>` (or `yield return null`) lowers to the
                    // payload type, which != iterElemTy_, so the store below was
                    // silently dropped and current_val stayed calloc-zeroed (every
                    // consumed element read null). Coerce the value into the
                    // declared element type first — a no-op for non-nullable
                    // elements (coerceReturnToNullableValue returns v unchanged
                    // unless the element is a nullable value type).
                    v = coerceReturnToNullableValue(v, iterElemSemaTy_);
                    if (v->getType() != iterElemTy_ &&
                        v->getType()->isIntegerTy() && iterElemTy_->isIntegerTy()) {
                        // #1596 / §5.1: a narrower integer (e.g. a `byte` span
                        // element) yielded into a wider element type (e.g.
                        // `IEnumerable<int>`) is a WIDENING conversion, not a
                        // truncation — CreateTrunc requires DestTy narrower
                        // than SrcTy, and the verifier rejects the reverse
                        // ("DestTy too big for Trunc"). Widen with zero-extend
                        // for unsigned source primitives (byte, ushort, uint,
                        // ulong, char) and sign-extend otherwise, matching the
                        // value-preserving widening of §5.1; only truncate
                        // when the source is genuinely the wider type.
                        unsigned srcBits = v->getType()->getIntegerBitWidth();
                        unsigned dstBits = iterElemTy_->getIntegerBitWidth();
                        if (srcBits > dstBits) {
                            v = builder_.CreateTrunc(v, iterElemTy_);
                        } else if (srcBits < dstBits) {
                            const sema::TypeRef* srcSemaTy =
                                ctx_.exprType(*ys->value);
                            bool srcUnsigned = srcSemaTy &&
                                isUnsignedPrim(srcSemaTy->primitive);
                            v = srcUnsigned
                                ? builder_.CreateZExt(v, iterElemTy_)
                                : builder_.CreateSExt(v, iterElemTy_);
                        }
                    }
                    if (v->getType() == iterElemTy_) {
                        llvm::Value* cvField = builder_.CreateStructGEP(
                            smStateStruct_, smSelf_, iterCurrentValField_);
                        // §29 (#1120): before storing the new yielded value,
                        // dispose the PREVIOUS one — but only if the enumerator
                        // owned it (current_val_owned == 1, set when the previous
                        // yield was a `new` expression).  Using-var locals (owned
                        // == 0) are disposed by their own scope mechanisms; owned
                        // params (owned == 0) by disposeOwnedParams at iterator-
                        // Dispose.  Null-guard: first yield is a no-op (calloc 0).
                        // After handling the previous value, set the owned flag for
                        // the new value: 1 if the expression is a `new`, else 0.
                        if (smKind_ == SuspendKind::Yield && iterElemIsOwnedClass_) {
                            llvm::Value* ownedFld = builder_.CreateStructGEP(
                                smStateStruct_, smSelf_, iterCurrentValOwnedField_);
                            // --- dispose previous if owned ---
                            llvm::Value* prevVal = builder_.CreateLoad(
                                iterElemTy_, cvField, "ity.prev");
                            llvm::Value* prevIsNull = builder_.CreateICmpEQ(
                                prevVal,
                                llvm::ConstantPointerNull::get(ptrTy()),
                                "ity.prevnull");
                            auto* prevNonNullBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.nonnull", currentFn_);
                            auto* prevContBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.cont", currentFn_);
                            builder_.CreateCondBr(
                                prevIsNull, prevContBB, prevNonNullBB);
                            builder_.SetInsertPoint(prevNonNullBB);
                            llvm::Value* prevOwned = builder_.CreateLoad(
                                i8(), ownedFld, "ity.prevowned");
                            llvm::Value* prevIsOwned = builder_.CreateICmpNE(
                                prevOwned, builder_.getInt8(0), "ity.previso");
                            auto* dispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.disp", currentFn_);
                            auto* skipBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.skip", currentFn_);
                            auto* afterDispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.after", currentFn_);
                            builder_.CreateCondBr(prevIsOwned, dispBB, skipBB);
                            builder_.SetInsertPoint(dispBB);
                            emitDisposeAndReclaim(prevVal);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(skipBB);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(afterDispBB);
                            builder_.CreateBr(prevContBB);
                            builder_.SetInsertPoint(prevContBB);
                            // --- set owned flag for new value ---
                            // Enumerator owns it only when the expression is a
                            // direct `new` allocation (a fresh, unattached object).
                            // Variable references (using-var locals, params, etc.)
                            // are borrowed — their existing disposal handles them.
                            bool yieldIsNew =
                                dynamic_cast<const ast::NewExpr*>(
                                    ys->value.get()) != nullptr;
                            builder_.CreateStore(
                                builder_.getInt8(yieldIsNew ? 1 : 0), ownedFld);
                        }
                        // fix #4092 (§29.2 / §30.7, v0.294): the ASYNC twin
                        // of the sync §29 (#1120) block above — for an
                        // `IAsyncEnumerable<C>` / `IAsyncEnumerator<C>`
                        // iterator (iteratorElemIsOwnedClass_ set,
                        // iterIsOwningIter_ clear by construction), the
                        // YieldStmt must dispose the PREVIOUS outstanding
                        // `current_val` before overwriting it (a 2nd+ yield)
                        // AND set the owned flag for the new value based on
                        // whether the yielded expression is a `new C(...)`
                        // mint. Identical structure to the sync block
                        // (same null guard, same dispose-prev-if-owned, same
                        // owned-flag predicate), distinct basic-block
                        // labels (`aity.prev.*`) so the IR is searchable
                        // for the async-vs-sync axis. The emit-context
                        // difference: `currentFn_` here is the async
                        // iterator's `resumeFn` (set in LlvmBackend_Async.cpp
                        // at line 3136, before the body lowering), so the
                        // dispose-then-yield happens on the resume body,
                        // not on MoveNext. Mutually exclusive with the sync
                        // arm above by `smKind_` (Yield vs AsyncYield) and
                        // mutually exclusive with the owning-form dispose
                        // below by `iterIsOwningIter_` (this arm is for
                        // `iterElemIsOwnedClass_`, which is gated on
                        // `!isOwningIter` in the emitter). The owning-form
                        // async counterpart lives at `#2621`'s
                        // `iterIsOwningIter_` arm further down.
                        else if (smKind_ == SuspendKind::AsyncYield &&
                                 iterElemIsOwnedClass_) {
                            llvm::Value* ownedFld = builder_.CreateStructGEP(
                                smStateStruct_, smSelf_,
                                iterCurrentValOwnedField_);
                            // --- dispose previous if owned ---
                            llvm::Value* prevVal = builder_.CreateLoad(
                                iterElemTy_, cvField, "aity.prev");
                            llvm::Value* prevIsNull = builder_.CreateICmpEQ(
                                prevVal,
                                llvm::ConstantPointerNull::get(ptrTy()),
                                "aity.prevnull");
                            auto* prevNonNullBB = llvm::BasicBlock::Create(
                                llvm_, "aity.prev.nonnull", currentFn_);
                            auto* prevContBB = llvm::BasicBlock::Create(
                                llvm_, "aity.prev.cont", currentFn_);
                            builder_.CreateCondBr(
                                prevIsNull, prevContBB, prevNonNullBB);
                            builder_.SetInsertPoint(prevNonNullBB);
                            llvm::Value* prevOwned = builder_.CreateLoad(
                                i8(), ownedFld, "aity.prevowned");
                            llvm::Value* prevIsOwned = builder_.CreateICmpNE(
                                prevOwned, builder_.getInt8(0),
                                "aity.previso");
                            auto* dispBB = llvm::BasicBlock::Create(
                                llvm_, "aity.prev.disp", currentFn_);
                            auto* skipBB = llvm::BasicBlock::Create(
                                llvm_, "aity.prev.skip", currentFn_);
                            auto* afterDispBB = llvm::BasicBlock::Create(
                                llvm_, "aity.prev.after", currentFn_);
                            builder_.CreateCondBr(
                                prevIsOwned, dispBB, skipBB);
                            builder_.SetInsertPoint(dispBB);
                            emitDisposeAndReclaim(prevVal);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(skipBB);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(afterDispBB);
                            builder_.CreateBr(prevContBB);
                            builder_.SetInsertPoint(prevContBB);
                            // --- set owned flag for new value ---
                            // Same predicate as the sync arm: enumerator
                            // owns it only when the expression is a direct
                            // `new` mint; variable references (using-var
                            // locals, params) are re-borrowed.
                            bool yieldIsNew =
                                dynamic_cast<const ast::NewExpr*>(
                                    ys->value.get()) != nullptr;
                            builder_.CreateStore(
                                builder_.getInt8(yieldIsNew ? 1 : 0),
                                ownedFld);
                        }
                        // fix #2738 / §28.3 / §29.2 (v0.282): mirror the class-element gate above for
                        // the runtime-managed primitive element kind (non-nullable
                        // `string`). The YieldStmt's full-expression boundary
                        // discharge (§28.3 row "the producer's +1 ... the binding
                        // that consumes it ... releases it") assumes the result is
                        // BOUND to a name — but a `yield return` is NOT a binding
                        // (no `var s :=` shape), and the slot is overwritten by the
                        // next yield's store. Without this gate, every
                        // `yield return <+1 producer>` (interpolation, `+`, `??`,
                        // case, conditional, range slice, ToString) leaks one
                        // fresh +1 reference. The thread is: yield mints a +1
                        // (the producer's malloc'd header, rc=1), the yield-store
                        // holds it in the iterator's `current_val` slot as the
                        // enumerator's +1 (same discipline as §29 #1120's class-
                        // element form: the next MoveNext releases the displaced
                        // +1 before storing the next), and the iterator's Dispose
                        // releases the outstanding +1. The current_val_owned flag
                        // distinguishes "this +1 is owned by the enumerator" (1,
                        // was minted by a `+1 producer` yield) from "this +1 is
                        // borrowed from another binding" (0, was `yield return
                        // <borrow>` — no release needed here). Disjoint from the
                        // class-element gate (this fires for primitives, that
                        // fires for classes/interfaces) and from the owning-iter
                        // gate (this is for the borrowing head) — the three are
                        // exhaustive and disjoint by construction in
                        // emitIteratorMethod (LlvmBackend_Iterators.cpp lines
                        // 918-942 + elemIsRuntimeManagedPrimitive computation).
                        if (smKind_ == SuspendKind::Yield &&
                            iterElemIsRuntimeManagedPrimitive_ &&
                            iterElemTy_->isPointerTy()) {
                            llvm::Value* ownedFld = builder_.CreateStructGEP(
                                smStateStruct_, smSelf_,
                                iterCurrentValOwnedField_);
                            // --- dispose previous if owned ---
                            llvm::Value* prevVal = builder_.CreateLoad(
                                iterElemTy_, cvField, "ity.sprev");
                            llvm::Value* prevIsNull = builder_.CreateICmpEQ(
                                prevVal,
                                llvm::ConstantPointerNull::get(ptrTy()),
                                "ity.sprevnull");
                            auto* prevNonNullBB = llvm::BasicBlock::Create(
                                llvm_, "ity.sprev.nonnull", currentFn_);
                            auto* prevContBB = llvm::BasicBlock::Create(
                                llvm_, "ity.sprev.cont", currentFn_);
                            builder_.CreateCondBr(
                                prevIsNull, prevContBB, prevNonNullBB);
                            builder_.SetInsertPoint(prevNonNullBB);
                            llvm::Value* prevOwned = builder_.CreateLoad(
                                i8(), ownedFld, "ity.sprevowned");
                            llvm::Value* prevIsOwned = builder_.CreateICmpNE(
                                prevOwned, builder_.getInt8(0), "ity.spreviso");
                            auto* dispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.sprev.disp", currentFn_);
                            auto* skipBB = llvm::BasicBlock::Create(
                                llvm_, "ity.sprev.skip", currentFn_);
                            auto* afterDispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.sprev.after", currentFn_);
                            builder_.CreateCondBr(prevIsOwned, dispBB, skipBB);
                            builder_.SetInsertPoint(dispBB);
                            // #2738: release the displaced +1 reference via
                            // the refcounted string release path — bz_string_release
                            // is the canonical releaser for a non-nullable string
                            // primitive (matching how §28.9 / §28.3 releases a
                            // borrowed +1 on scope exit). emitStringRelease is
                            // null- and sentinel-safe, so the owned-flag gate is
                            // sufficient (no separate null guard needed beyond
                            // the prev non-null check above).
                            emitStringRelease(prevVal);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(skipBB);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(afterDispBB);
                            builder_.CreateBr(prevContBB);
                            builder_.SetInsertPoint(prevContBB);
                            // --- set owned flag for new value ---
                            // Enumerator owns it only when the expression is a
                            // +1 string producer (interpolation, `+`, `??`,
                            // case, conditional, range slice, ToString — see
                            // isPlusOneStringProducer). A literal (`yield return
                            // "x"`) is a static immortal sentinel with rc=∞ —
                            // not a +1 producer, do not release. A borrowed
                            // local/param (`yield return s`) is a re-borrow —
                            // the existing binding owns the +1 and the
                            // iterator never had it; do not release. Same
                            // discipline as the class-element gate above, with
                            // the isPlusOneStringProducer predicate standing in
                            // for `new X()`.
                            bool yieldIsProducer =
                                isPlusOneStringProducer(ys->value.get());
                            builder_.CreateStore(
                                builder_.getInt8(yieldIsProducer ? 1 : 0),
                                ownedFld);
                        }
                        // fix #5323 (§29/§28.9, v0.313): nullable string? element.
                        // Mirrors the non-nullable string block above (the #2738
                        // thread): the yield store holds the +1 reference in the
                        // nullable struct's payload, the next MoveNext advance
                        // releases the displaced reference before storing the next,
                        // and iterator Dispose releases the outstanding one. The
                        // current_val_owned flag has the same meaning for both
                        // nullable and non-nullable strings. Disjoint from the
                        // non-nullable string gate by iterElemIsNullableString_.
                        else if (smKind_ == SuspendKind::Yield &&
                                 iterElemIsNullableString_) {
                            // fix #5323 (§29/§28.9, v0.313): nullable string? element.
                            // A nullable reference type (`string?`) lowers to ptr in
                            // the frame (per §8.3/§9.1: nullable ref → plain ptr,
                            // null is a real null pointer — no hasValue tag), so
                            // the handling mirrors the non-nullable string path
                            // (the #2738 thread) exactly: load the ptr, null-guard
                            // it, and if owned release it. The owned flag has the
                            // same meaning for both nullable and non-nullable
                            // strings.
                            llvm::Value* ownedFld = builder_.CreateStructGEP(
                                smStateStruct_, smSelf_,
                                iterCurrentValOwnedField_);
                            // --- dispose previous if owned ---
                            llvm::Value* prevVal = builder_.CreateLoad(
                                iterElemTy_, cvField, "ity.nsprev");
                            llvm::Value* prevIsNull = builder_.CreateICmpEQ(
                                prevVal,
                                llvm::ConstantPointerNull::get(ptrTy()),
                                "ity.nsprevnull");
                            auto* prevNonNullBB = llvm::BasicBlock::Create(
                                llvm_, "ity.nsprev.nonnull", currentFn_);
                            auto* prevContBB = llvm::BasicBlock::Create(
                                llvm_, "ity.nsprev.cont", currentFn_);
                            builder_.CreateCondBr(
                                prevIsNull, prevContBB, prevNonNullBB);
                            builder_.SetInsertPoint(prevNonNullBB);
                            llvm::Value* prevOwned = builder_.CreateLoad(
                                i8(), ownedFld, "ity.nsprevowned");
                            llvm::Value* prevIsOwned = builder_.CreateICmpNE(
                                prevOwned, builder_.getInt8(0), "ity.nspredisowned");
                            auto* dispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.nsprev.disp", currentFn_);
                            auto* skipBB = llvm::BasicBlock::Create(
                                llvm_, "ity.nsprev.skip", currentFn_);
                            auto* afterDispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.nsprev.after", currentFn_);
                            builder_.CreateCondBr(prevIsOwned, dispBB, skipBB);
                            builder_.SetInsertPoint(dispBB);
                            // emitStringRelease is null-safe for both the
                            // nullable and non-nullable cases.
                            emitStringRelease(prevVal);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(skipBB);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(afterDispBB);
                            builder_.CreateBr(prevContBB);
                            builder_.SetInsertPoint(prevContBB);
                            // --- set owned flag for new value ---
                            // Owned when the yielded expression is a +1 string
                            // producer (isPlusOneStringProducer). The predicate
                            // covers BinaryExpr (+, ??), ConditionalExpr, and
                            // numeric/Object ToString() calls for both nullable
                            // and non-nullable string results.
                            bool yieldIsProducer =
                                isPlusOneStringProducer(ys->value.get());
                            builder_.CreateStore(
                                builder_.getInt8(yieldIsProducer ? 1 : 0),
                                ownedFld);
                        }
                        // fix #2621 / §29.1 (v0.236): sync IOwningEnumerable<T> /
                        // IOwningEnumerator<T> — the enumerator ALWAYS owns a
                        // non-null current_val (the §29.1 contract: ownership
                        // transfers to the consumer only at TakeCurrent). On a
                        // subsequent yield, the store below would otherwise DROP
                        // the still-owned displaced value — the second leak site
                        // from #2616 (which fixed the exhaustion transition). The
                        // borrowing head (iterElemIsOwnedClass_) is handled by the
                        // block above with its owned-flag gate; this block is the
                        // unconditional-on-non-null twin for the owning head.
                        //
                        // Why an UNCONDITIONAL dispose is sound even for
                        // `yield return ownedLocal` / `yield return ownedParam`:
                        // the §29.7 (#2306) / §29.8 (#2154) state-keyed discharge
                        // gates on `state < transferredAtState` / `state < yieldState`,
                        // both FALSE at the moment of the next yield (we're already
                        // at or past the transfer state — the local/param was
                        // transferred through the previous yield). So this dispose
                        // is the sole releaser for the displaced transferred value
                        // — no double-free against the state-keyed discharge.
                        //
                        // Narrow gate on pointer elemTy mirrors the existing
                        // disposeYieldedVal (#1837 / §29.1 v0.196) so an
                        // unspecialized generic template (T unresolved → i32
                        // placeholder) elides cleanly. Mirrors the
                        // `isOwningIter && elemTy->isPointerTy()` guards at
                        // TakeCurrent (§29.1) and MoveNext-exhaustion (line 1390
                        // / #2616).
                        if (smKind_ == SuspendKind::Yield && iterIsOwningIter_ &&
                            iterElemTy_->isPointerTy()) {
                            llvm::Value* prevVal2 = builder_.CreateLoad(
                                iterElemTy_, cvField, "ityo.prev");
                            llvm::Value* prevIsNull2 = builder_.CreateICmpEQ(
                                prevVal2,
                                llvm::ConstantPointerNull::get(ptrTy()),
                                "ityo.prevnull");
                            auto* dispBB2 = llvm::BasicBlock::Create(
                                llvm_, "ityo.prev.disp", currentFn_);
                            auto* contBB2 = llvm::BasicBlock::Create(
                                llvm_, "ityo.prev.cont", currentFn_);
                            builder_.CreateCondBr(
                                prevIsNull2, contBB2, dispBB2);
                            builder_.SetInsertPoint(dispBB2);
                            emitDisposeAndReclaim(prevVal2);
                            // Null the slot so a subsequent dispose site
                            // (disposeYieldedVal at iterator-Dispose,
                            // MoveNext-exhaustion at line 1390) no-ops. The
                            // three sites — here, exhaustion, iterator-Dispose —
                            // are all dispose-then-null under the same null
                            // guard, so whichever runs first leaves the slot
                            // null for the others. Exactly-once, not a
                            // double-free. (#2616 line 1511-1531 made the
                            // exhaustion/Dispose pair exactly-once; this lane
                            // extends the same invariant to the yield-store
                            // site.)
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()),
                                cvField);
                            builder_.CreateBr(contBB2);
                            builder_.SetInsertPoint(contBB2);
                        }
                        builder_.CreateStore(v, cvField);
                    }
                }
            }
            unsigned nextState = ++smNextSuspendState_;
            // fix #1567 / §29.7/§28.10 (v0.182): build the ONE establishment-
            // ordered cleanup stack for this suspend state — using-var disposals,
            // for-in enumerator-frame disposals, and finally bodies in the single
            // order they were established. Both the SYNC iterator Dispose and
            // (since fix #1598) the ASYNC iterator Dispose consume this SAME map
            // (`smIterCleanupAtState_`) and run it in exact reverse (§28.10: one
            // establishment-ordered stack, no category-ordered phases; issue
            // 3579 removed the write-only category-split maps this comment used
            // to describe as still feeding the async path — they never did,
            // after #1598). A finally established when the dispose-stack had
            // depth d sits, in establishment order, immediately before scope d
            // (after every scope < d, before every scope >= d), so early Dispose
            // runs it AFTER its own try body's using-var disposals and BEFORE
            // the enclosing scopes' — reverse establishment, matching the
            // exception edge. finallyStack_ is outermost-first (increasing
            // disposeDepthAtEntry) and parallel to finallyBodyStack_.
            {
                std::vector<IterCleanup> actions;
                std::size_t finIdx = 0;
                auto flushFinallysBefore = [&](std::size_t depth) {
                    while (finIdx < finallyStack_.size() &&
                           finIdx < finallyBodyStack_.size() &&
                           finallyStack_[finIdx].disposeDepthAtEntry == depth) {
                        IterCleanup act;
                        act.kind = IterCleanup::Kind::Finally;
                        act.finallyBody = finallyBodyStack_[finIdx];
                        actions.push_back(act);
                        ++finIdx;
                    }
                };
                for (std::size_t i = 0; i < disposeStack_.size(); ++i) {
                    flushFinallysBefore(i);  // finally@depth i precedes scope i
                    for (const Disposal& d : disposeStack_[i]) {
                        // fix #3604 (§29.7 / §28.3 / §29.2, v0.286): the
                        // `forInAutoDrop` flag on a `for var x in
                        // IOwningEnumerable<T>` loop element is the one
                        // `unwindOnly` exemption the §29.7 abandonment
                        // snapshot admits — every other `unwindOnly` entry
                        // is skipped (this loop's exemption predates
                        // #3604 and is the line above; the predicate below
                        // is the SOLE carve). The auto-drop is per-iteration
                        // (§29.2) and §29.7's "as if the body had exited at
                        // the suspension point" makes the suspension point
                        // an exit edge that the §28.3 plain-owned-local
                        // rule applies to. Without this carve the element
                        // leaks on abandonment (6400 B / 400 allocations on
                        // HEAD pre-this-fix). On endBB / break / throw /
                        // return the `unwindOnly` setting still fires the
                        // disposal through the existing `unwindOnly` branch
                        // in emitDisposals (isUnwind=true path), and the
                        // §29.2 normal-exit discharge-by-program rule is
                        // unchanged because every other consumer of
                        // `forInAutoDrop` reads zero of the flag — the
                        // snapshot is the only one. The comment on the
                        // `forInAutoDrop` declaration in
                        // LlvmBackend_impl.hpp is the canonical
                        // exactly-once argument.
                        if (d.caughtException ||
                            (d.unwindOnly && !d.forInAutoDrop)) continue;
                        // fix #3500 (§27.5 / §28.9 / §29.3, v0.283): the §27.5
                        // CLOSURE-release and CAPTURE-CELL frees are deliberately
                        // NOT enrolled in this abandonment snapshot.
                        //
                        // The snapshot's exclusivity argument is "Dispose stamps
                        // the DISPOSED sentinel, an exhausted iterator parks at
                        // -1" — which covers abandonment vs. exhaustion but NOT a
                        // body that THROWS after a yield. On that edge this stack
                        // runs in addition to whatever the throw path already
                        // did, and that is not a hypothesis: on ef339fa, with
                        // none of this issue's changes present,
                        //
                        //     using var r := new R(); yield return r.V;
                        //     if v > 2 then throw …; end if;
                        //
                        // is a heap-use-after-free and the same shape over a
                        // `string` local is a SEGV. That pre-existing double-run
                        // is filed separately; what matters here is that arming a
                        // FREE on it would convert this issue's LEAK into a
                        // use-after-free — measured, on exactly that shape, with
                        // an earlier revision of this commit that did not have
                        // these three lines.
                        //
                        // So the new reclaims arm only where the dispose stack is
                        // run exactly once — MoveNext's own scope exit, i.e. the
                        // run-to-exhaustion path this issue's repro takes — and
                        // an ABANDONED or THROWING enumeration keeps the
                        // pre-existing leak. No pre-existing entry is removed:
                        // before this commit a sync iterator body could not
                        // register ANY of these kinds at all (their eligibility
                        // tokens never named `moveNextFn`), so this filter is a
                        // no-op for every program that compiled before it.
                        //
                        // THE LIST MUST MATCH `consumeLastLambdaCapturedCells`
                        // EXACTLY, and `inlineOwnedArrayCell` is the one that is
                        // easy to miss: it is pushed BESIDE the `boxedCellFree`
                        // that frees the cell (both the pattern-binding and the
                        // ordinary branch), and it disposes the owned ELEMENTS
                        // THROUGH that same cell — so replaying it after the
                        // throw edge already ran it is a use-after-free on the
                        // very same allocation. A captured move-only
                        // `[2 owned]Cell` in an iterator that throws after a
                        // yield was `heap-use-after-free ... in
                        // bz$iter$Gen$CloneDispose` with a three-kind version of
                        // this filter, and is clean with the fourth.
                        //
                        // THE LIST IS COMPLETE, enumerated from the REGISTRATION
                        // SITES rather than from the kind list. Exactly three
                        // places register a Disposal under a token this commit
                        // re-establishes: `lowerLocalDecl`'s closure arm
                        // (`closureRelease`); `consumeLastLambdaCapturedCells`
                        // (`boxedCellFree`, `boxedCellFreeDeferred`,
                        // `inlineOwnedArrayCell` — `skipCellFreeOnUnwind` is a
                        // modifier on those same entries, not a kind of its
                        // own); and #793's for-var cell free (`boxedCellFree`,
                        // and gated on `!smStateStruct_`, so unreachable in a
                        // MoveNext at all). The neighbouring plain
                        // `inlineOwnedArray` is deliberately NOT in the list: it
                        // is gated on `moveOnlyArrayTransferredFn_`, a token
                        // this commit does not touch, so it is as inert in an
                        // iterator body after this change as it was before.
                        //
                        // ⚠ UPDATE — fix #3531 (§29.7 / §28.3, v0.283). The
                        // THROW-EDGE DOUBLE-RUN described above IS NOW FIXED:
                        // `propagateBB` (LlvmBackend_Disposal.cpp) marks a sync
                        // iterator done (state = -1) when its body leaves
                        // MoveNext by an exception, exactly as #623's
                        // `finUnwindBB` already did for the `try`/`finally`
                        // edge, so `Dispose` no longer replays this stack on
                        // that edge at all. That removes THIS FILTER'S STATED
                        // REASON, and issue 3541 tracks removing it.
                        //
                        // IT IS DELIBERATELY STILL HERE. Removing it also arms
                        // these four frees on the ABANDONMENT path, and that
                        // path is independently broken: a `using var` CAPTURED
                        // BY A LAMBDA in a sync iterator, abandoned by `break`
                        // with no throw anywhere in the program, is a
                        // `heap-buffer-overflow` in `bz$iter$Gen$CloneDispose`
                        // — measured identically on b43196a and on the #3531
                        // tree, filed as issue 3539. Arming four more frees on
                        // a path that already corrupts memory is the same
                        // leak-to-use-after-free trade this filter exists to
                        // avoid. 3539 first, then 3541.
                        //
                        // ⚠ UPDATE — fix #3603 / issue 3539 (§29.7 / §27.5,
                        // v0.286). 3539 IS NOW REPAIRED: the `iterEnumDispose`
                        // arm of `emitDisposals` reads its slot through
                        // `boxedStorage`, and this Dispose's restore loop
                        // registers a captured local's holder in `cellVars_`
                        // so the same authority unboxes it. "3539 first, then
                        // 3541" is therefore satisfied — AND 3541 IS STILL NOT
                        // SAFE. Deleting these three lines on top of that
                        // repair was MEASURED in this lane:
                        //
                        //   using var r := new R(7);
                        //   var f: () -> int := lambda() => r.V;
                        //   yield return f();        // consumer `break`s
                        //
                        // becomes `AddressSanitizer: SEGV on unknown address
                        // 0x000000000000 ... in bz$iter$M.Src$Gen$CloneDispose`
                        // — a NULL dereference, NOT the mis-typed-slot wild
                        // jump 3539 was — on all three abandonment shapes
                        // probed, while the full-drain and the
                        // throw-after-yield shapes stayed clean. So a THIRD
                        // defect sits behind this filter and 3541 needs its own
                        // root-cause pass, not an unblocking.
                        //
                        // What the filter COSTS is now pinned rather than only
                        // described: the `abandoned_leak_is_only_the_3541_residue`
                        // row of tests/codegen/
                        // iter_lambda_captured_iter_format_local_unbox_3603_test.cpp
                        // asserts the abandonment residue is EXACTLY the three
                        // §27.5 allocations per round (closure + env + cell),
                        // so removing this filter cannot go unnoticed.
                        // fix #3752 (§27.5 / §28.9, v0.286):
                        // `capturedCellContentRelease` joins the list for the
                        // reason the list exists — it is registered by
                        // `lowerLambda` under `cellFreeEligibleFn_`, one of the
                        // tokens this venue re-establishes, and it RELEASES A
                        // VALUE THROUGH THE CELL. Replaying it on the
                        // abandonment / Dispose edge after the scope exit
                        // already ran it is an over-release of the same
                        // reference — the same use-after-free trade
                        // `inlineOwnedArrayCell` is in the list to avoid. The
                        // residue is the pre-existing §27.5 leak, unchanged.
                        // fix #4509 (§27.5 / §28.9, v0.306): `cellRelease`
                        // joins the list for the same reason as its
                        // neighbours — it drops a COUNTED reference held by
                        // the declaring frame, and the scope exit already
                        // dropped it. Replaying it on the abandonment /
                        // Dispose edge would be a second drop of one
                        // reference: an over-release, and with an env still
                        // holding the cell, a use-after-free.
                        if (d.closureRelease || d.boxedCellFree ||
                            d.boxedCellFreeDeferred || d.inlineOwnedArrayCell ||
                            d.capturedCellContentRelease || d.cellRelease)
                            continue;
                        if (d.iterEnumDispose) {
                            auto* alloca =
                                llvm::dyn_cast_or_null<llvm::AllocaInst>(d.slot);
                            auto fit = alloca ? smIterEnumFrameSlot_.find(alloca)
                                              : smIterEnumFrameSlot_.end();
                            if (alloca && fit != smIterEnumFrameSlot_.end()) {
                                IterCleanup act;
                                act.kind = IterCleanup::Kind::EnumFrame;
                                act.frameField = fit->second;
                                actions.push_back(act);
                                continue;  // tracked by field, not by node
                            }
                            // fix #3468 (§29.7 / §29.2 / stdlib §4.12–§4.13,
                            // v0.286): an iterEnumDispose with NO frame field
                            // used to `continue` here, on the claim that it
                            // "cannot be live across this yield". That claim is
                            // false, and the counterexample is the ordinary one:
                            // a NAMED local/param bound to an iterator-format
                            // enumerable (`var seq: IEnumerable<T> := coll;`)
                            // inside an iterator body. Its scope-end disposal is
                            // an iterEnumDispose too — the value is a
                            // vtable-backed enumerator whose Dispose is at slot
                            // 1, not a class-table object — but it is a NODE-
                            // keyed local, not one of the for-in's implicit
                            // enumerator/source resources, so it is absent from
                            // smIterEnumFrameSlot_ and was dropped SILENTLY:
                            // measured 12800 B / 400 abandonments, and unchanged
                            // by `using var` (issue 3468's second, independent
                            // leak).
                            //
                            // disposeOwnedLocals (#2306) does not cover it and
                            // must not: it is keyed on the owned-CLASS pre-pass
                            // and dispatches emitDisposeAndReclaim (the class
                            // dispatch table), which is the wrong vtable for an
                            // iterator-format value.
                            //
                            // So fall through to the node-keyed path below,
                            // which preserves d.iterEnumDispose and therefore
                            // replays the SAME slot-1 disposal the normal exit
                            // paths emit. Exactly once, by the same argument
                            // every other entry in this stack relies on: Dispose
                            // stamps the DISPOSED sentinel (-2) before running
                            // the stack, so no MoveNext can resume past this
                            // yield and reach the loop's own disposal;
                            // exhaustion parks -1 and the throw edge parks -1
                            // (#3531), and neither state has a case in the
                            // switch. An entry that reaches here was previously
                            // DROPPED, never emitted, so this can only add a
                            // missing disposal — it cannot duplicate one.
                        }
                        const ast::Node* node = nullptr;
                        for (const auto& [n, s] : locals_)
                            if (s == d.slot) { node = n; break; }
                        // fix #3603 / #3539 (§29.7 / §27.5 / §28.9, v0.286):
                        // the fix-#3468 narrow exclusion that sat here
                        // (`d.iterEnumDispose && frameCapturedLocals_.count(
                        // node)` -> `continue`) kept a LAMBDA-CAPTURED
                        // iterator-format local's abandonment disposal
                        // deliberately DISABLED, preserving a leak rather than
                        // arming a wild jump: §27.5 (#808) rebinds such a
                        // local's storage to a §27.5 CELL, and the slot-1
                        // dispatch read a "vtable" out of the cell header.
                        //
                        // The wild jump was never this snapshot's to fix. Its
                        // cause was that the `iterEnumDispose` arm of
                        // `emitDisposals` (LlvmBackend_Disposal.cpp) read its
                        // slot RAW instead of through `boxedStorage`, the one
                        // authority that answers "where does the object a
                        // disposal slot names actually live" — so the SAME
                        // wild jump fired on MoveNext's own scope exit, with
                        // this exclusion fully in force and no abandonment
                        // anywhere (measured on r12-cycle-1: ASan SEGV, "PC is
                        // at a non-executable region", for `var seq:
                        // IEnumerable<T> := coll;` captured by a lambda, on
                        // BOTH the abandon and the full-drain edge). That arm
                        // now asks `boxedStorage` like its siblings, which
                        // also fixes #3539's `using var` twin
                        // (heap-buffer-overflow in `CloneDispose`) because
                        // both reach the identical mis-typed slot.
                        //
                        // With the slot-1 dispatch sound, the exclusion has no
                        // remaining justification and is retired: the entry it
                        // declined was one this snapshot had already collected
                        // and would otherwise DROP, so re-admitting it can only
                        // add a missing disposal, never duplicate one (the
                        // exactly-once argument in the #3468 comment above is
                        // unchanged and covers it verbatim).
                        if (node) {
                            IterCleanup act;
                            act.kind = IterCleanup::Kind::Disposal;
                            act.node = node;
                            act.disposal = d;
                            actions.push_back(act);
                        }
                    }
                }
                // Defensive: a finally at depth == disposeStack_.size() sorts
                // after all scopes; in practice the yield is inside the try body,
                // so a body scope exists and no such finally remains.
                flushFinallysBefore(disposeStack_.size());
                if (!actions.empty())
                    smIterCleanupAtState_[nextState] = std::move(actions);
            }
            builder_.CreateStore(builder_.getInt32((int)nextState), smStateGEP_);
            if (isAsyncYield) {
                emitAsyncIterComplete(/*hasValue=*/true);
                builder_.CreateRetVoid();
            } else {
                builder_.CreateRet(builder_.getTrue());
            }
            // Continue emitting code after this yield in the next state block.
            if (nextState < smStateBBs_.size()) {
                builder_.SetInsertPoint(smStateBBs_[nextState]);
                // Restore all iterator locals from the state struct.
                for (auto& [allocaSlot, fieldIdx] : smSpillsInFrameOrder()) {  // fix #3504
                    llvm::Value* fptr = builder_.CreateStructGEP(
                        smStateStruct_, smSelf_, fieldIdx);
                    llvm::Value* savedVal = builder_.CreateLoad(
                        allocaSlot->getAllocatedType(), fptr);
                    builder_.CreateStore(savedVal, allocaSlot);
                }
            }
            return false;  // not terminated (we continue in next state BB)
        }
        return false;
    }
    return false;  // unhandled statement kinds land in later slices
}


bool Emitter::lowerOwningInlineArrayReassign(const ast::Expr& target,
                                              llvm::Value* arrayAddr,
                                              llvm::Type* elemTy,
                                              bool skipDisposal) {
    // §28.7 / §15.3 (#1385 / v0.260): whole-value reassignment of a move-only
    // OWNING inline-array local or field (`[N owned]T` / `[N owned]T?`) disposes
    // each live (non-null, non-moved-from) slot of the displaced value in
    // REVERSE slot order, then the caller performs the new-value store. Sema's
    // `moveOnlyArrayLive_` map (#1549) tracks per-slot moved-from state
    // statically, but we rely on the runtime invariant that `move a[i]` nulls
    // the slot in the alloca (lowerMoveFromCollectionSlot, #747) — so a slot
    // that is semantically moved-from is also literally null at the
    // reassignment site, and the null-guard below skips it. Returns true iff
    // the helper ran; the caller continues with the new-value store.
    //
    // fix #3263: two correctness extensions layered on the #1385 shape:
    //   (defect 2) `skipDisposal` — the LHS was whole-value moved-from by a
    //     prior `var b := a;` (sema's `moveOnlyArrayWholeValueMovedFrom_` set
    //     populated it via `moveOnlyArrayNoteWholeTransfer`; the per-Assignment
    //     flag `wholeValueLhsMovedFrom` reads it back here). §28.7 *Moved-from
    //     and null*: "Overwriting a moved-from field/local ... stores without
    //     disposal" — the alloca's stale pointers now belong to the receiver
    //     (e.g. `b`), and the receiver's scope-end disposal will free them.
    //     Disposing them here would double-free. Suppress the entire pass.
    //   (defect 3) null each slot immediately after `emitDisposeAndReclaim`,
    //     AND wrap the per-slot disposal so a throwing slot disposer does not
    //     stop the pass — §28.7 *Ordering and exceptions* (v0.260): the pass
    //     is "a multi-value cleanup in §28.4's sense — a slot disposer that
    //     throws does not stop the pass; the remaining live slots are still
    //     disposed, a later throw superseding an earlier one under §31.2's
    //     ordinary rule ... — and the displaced value's later unwinding
    //     discharge observes only empty slots". The throwing slot is nulled
    //     so the unwinding discharge (a later `a`'s own scope-end or field
    //     reclamation) sees it as empty and skips it; the remaining live
    //     slots continue to be disposed; the latest throw wins for in-flight
    //     propagation.
    if (!arrayAddr || !elemTy) return false;
    const sema::TypeRef* rt = ctx_.exprType(target);
    if (!rt || rt->kind != sema::TypeRef::Kind::InlineArray || !rt->arrayOwned)
        return false;
    auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(elemTy);
    if (!arrTy) return false;
    // fix #3263 defect 2: the LHS was whole-value moved-from — the stale
    // alloca pointers belong to the receiver now. Suppress the entire pass.
    if (skipDisposal) return true;
    // Per §15.1.1 / #747, the owning inline-array slot storage is `[N x ptr]`
    // (a null ptr means empty; a non-null ptr owns the slot's object). Iterate
    // in REVERSE order per the spec wording on #1385: "disposes each non-moved-
    // from slot of the displaced value (reverse slot order), then stores".
    unsigned N = arrTy->getNumElements();
    // fix #3263 defect 3: a slot disposer that throws does not stop the
    // pass (per-slot multi-value cleanup, §28.7 v0.260 + §28.4 composition).
    // Capture the entry bz$exc value (parkedExc): a pre-existing propagating
    // exception in flight at the pass's entry (e.g. the unwinding discharge
    // is itself inside a try-finally whose catch arm ran the assignment).
    // Clear bz$exc for the run of the pass (the same park-discipline the
    // destructor-body block at LlvmBackend_Disposal.cpp:127 uses), so each
    // slot's dispose sees a clean exception context — a throwing disposer
    // sets bz$exc to its NEW exception and we record that as the pass's
    // currently-propagating one, leaving the parked exception for the post-
    // pass restore. The latest throw wins (later-wins, §28.7 *Ordering and
    // exceptions*); on the pass's exit, if the pass raised a new exception
    // (inFlightExc), the parked exception is disposed as superseded
    // (SupersededChain / §31.4 v0.259 is the runtime record channel; at
    // HEAD the runtime field is not yet shipped, so we dispose directly —
    // the diagnostic loss the chain would recover is on the §31.4 ledger,
    // and the *disposal* of the superseded exception is the part the
    // composition actually requires). The bz$exc slot is restored to the
    // chosen in-flight exception (or the parked one, if no throw happened)
    // at the pass's exit — symmetric with the destructor body's restore.
    //
    // The "running winner" `inFlightExc` is a loop-carried value: each
    // iteration's `advanceBB` reassigns it to the latest throw, and the
    // NEXT iteration's `checkBB` reads it. A C++ variable holding an SSA
    // value does NOT flow through the loop's CFG (each iteration's `newExc`
    // is a separate load, so the SSA value of `inFlightExc` after one
    // `advanceBB` is the previous-iter's `newExc`; the next iter's uses
    // would reference that SSA value, but the previous iter's `advanceBB`
    // is NOT dominated by it). Park the running winner in a stack slot
    // (entryAlloca) — load on each use, store on each advance — so the
    // value flows through the loop as a memory location.
    llvm::Value* excSlot = emitExcSlot();
    llvm::Value* parkedExc =
        builder_.CreateLoad(ptrTy(), excSlot, "ioar.parked");
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), excSlot);
    llvm::AllocaInst* inFlightSlot = entryAlloca(ptrTy());
    inFlightSlot->setName("ioar.inflight");
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                         inFlightSlot);
    auto loadInFlight = [&]() -> llvm::Value* {
        return builder_.CreateLoad(ptrTy(), inFlightSlot, "ioar.inflight.v");
    };
    for (unsigned i = N; i > 0; --i) {
        unsigned idx = i - 1;
        llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
            arrTy, arrayAddr,
            {builder_.getInt32(0), builder_.getInt32(idx)},
            "ioar.slot");
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), slotAddr, "ioar.val");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB =
            llvm::BasicBlock::Create(llvm_, "ioar.dispose", currentFn_);
        auto* nullSlotBB =
            llvm::BasicBlock::Create(llvm_, "ioar.nullslot", currentFn_);
        auto* contBB =
            llvm::BasicBlock::Create(llvm_, "ioar.cont", currentFn_);
        builder_.CreateCondBr(isNull, nullSlotBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // Same dispatch the scope-end disposal uses (Disposal.inlineOwnedArray,
        // LlvmBackend_Disposal.cpp ~1890): dynamic-dispatched Dispose + block
        // reclaim via the slot object's own vtable. Idempotent on a null input
        // (the guard above already routed null to nullSlotBB).
        emitDisposeAndReclaim(obj);
        // fix #3263 defect 3: leave the disposed slot EMPTY (§28.7 v0.260:
        // "every slot the pass disposed is left empty ... and the displaced
        // value's later unwinding discharge observes only empty slots"). The
        // store is to the SAME slotAddr the dispose loaded from — the nulling
        // happens AFTER the dispose so the dispose sees the live object, and
        // a subsequent unwinding scope-end disposal (or the thrower's own
        // re-disposal via the bz$exc propagation) finds the slot null and
        // skips it via its null-guard. Without this the disposed slot's
        // pointer is still in the alloca — a later path that reads or
        // re-disposes it sees a freed instance.
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
        // fix #3263 defect 3: a throwing slot disposer does NOT stop the
        // pass. Check bz$exc after the dispose — if a NEW exception was set
        // (the slot's disposer threw, or a callee inside Dispose threw), the
        // running winner is updated to it and the previous winner (if any) is
        // disposed as superseded. bz$exc itself is cleared so the NEXT slot
        // dispose sees a clean context (its own throw, if any, will be the
        // one that propagates after the pass — not this throw).
        llvm::Value* newExc =
            builder_.CreateLoad(ptrTy(), excSlot, "ioar.newexc");
        llvm::Value* hasNewExc = builder_.CreateICmpNE(
            newExc, llvm::ConstantPointerNull::get(ptrTy()));
        auto* checkBB =
            llvm::BasicBlock::Create(llvm_, "ioar.check", currentFn_);
        auto* disposePrevBB =
            llvm::BasicBlock::Create(llvm_, "ioar.disposeprev", currentFn_);
        auto* advanceBB =
            llvm::BasicBlock::Create(llvm_, "ioar.advance", currentFn_);
        auto* setInFlightBB =
            llvm::BasicBlock::Create(llvm_, "ioar.setinflight", currentFn_);
        // If the slot's dispose threw (hasNewExc), branch to checkBB to
        // dispose the previous in-flight as superseded and set the new
        // in-flight. If not, branch to advanceBB to leave the previous
        // in-flight intact (later-wins: a no-throw iter does not
        // "supersede" anything; the running exception is whatever was
        // the latest throw from a PRIOR iter).
        builder_.CreateCondBr(hasNewExc, checkBB, advanceBB);
        builder_.SetInsertPoint(checkBB);
        // The previous in-flight exception (if any) is superseded by newExc.
        // Dispose it exactly as emitPendingCheckIf / emitNewPendingCheck do
        // for the sibling finally/catch-guard supersede (#1216/#1149),
        // including the #1288/§30.9 deferred-fault clear so a superseded
        // task-owned fault's single-dispose invariant still holds. The
        // disposal itself is `emitDisposeAndReclaim` (idempotent Dispose +
        // reclaim — bz_clear_exc_deferred resets the deferred flag first).
        llvm::Value* prevExc = loadInFlight();
        llvm::Value* prevIsNull = builder_.CreateICmpEQ(
            prevExc, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(prevIsNull, setInFlightBB, disposePrevBB);
        builder_.SetInsertPoint(disposePrevBB);
        builder_.CreateCall(
            declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                     {ptrTy()}),
            {prevExc});
        emitDisposeAndReclaim(prevExc);
        builder_.CreateBr(setInFlightBB);
        // setInFlightBB: store newExc (the latest throw) into inFlightSlot,
        // clear bz$exc so the next iter's dispose sees a clean context,
        // branch to contBB. This is the ONE place newExc is written to
        // inFlightSlot, ensuring a no-throw iteration (which bypasses
        // checkBB entirely) leaves the previous in-flight throw intact.
        builder_.SetInsertPoint(setInFlightBB);
        builder_.CreateStore(newExc, inFlightSlot);
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), excSlot);
        builder_.CreateBr(contBB);
        // advanceBB: the no-throw path. Leave inFlightSlot intact (a no-throw
        // iter does not supersede a previous throw — the running exception
        // is whatever was set by a prior throw). Branch to contBB to
        // continue the loop. CRITICAL: do NOT touch inFlightSlot here; a
        // prior throw must survive a no-throw iter (later-wins, but a null
        // is not a "winner" — it just doesn't change the running exception).
        builder_.SetInsertPoint(advanceBB);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(nullSlotBB);
        // Null-slot edge: nothing to dispose. Branch to contBB.
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
    // fix #3263 defect 3: pass complete. Restore bz$exc to the running
    // winner (later-wins — last throw wins for propagation), disposing the
    // parked exception as superseded if the pass raised a new one. On the
    // no-throw path we restore the parked exception unchanged (or null if
    // there was no exception in flight at the pass's entry).
    llvm::Value* finalInFlight = loadInFlight();
    llvm::Value* passThrew = builder_.CreateICmpNE(
        finalInFlight, llvm::ConstantPointerNull::get(ptrTy()));
    auto* restoreBB =
        llvm::BasicBlock::Create(llvm_, "ioar.restore", currentFn_);
    auto* supersedeParkedBB =
        llvm::BasicBlock::Create(llvm_, "ioar.supersedeparked", currentFn_);
    auto* finalBB =
        llvm::BasicBlock::Create(llvm_, "ioar.final", currentFn_);
    builder_.CreateCondBr(passThrew, restoreBB, finalBB);
    builder_.SetInsertPoint(restoreBB);
    // Dispose the parked exception (the one in flight at the pass's entry)
    // as superseded, leaving the running winner as the new in-flight.
    llvm::Value* hadParked = builder_.CreateICmpNE(
        parkedExc, llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCondBr(hadParked, supersedeParkedBB, finalBB);
    builder_.SetInsertPoint(supersedeParkedBB);
    builder_.CreateCall(
        declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                 {ptrTy()}),
        {parkedExc});
    emitDisposeAndReclaim(parkedExc);
    builder_.CreateBr(finalBB);
    builder_.SetInsertPoint(finalBB);
    llvm::Value* restoredExc = builder_.CreateSelect(
        passThrew, finalInFlight, parkedExc);
    builder_.CreateStore(restoredExc, excSlot);
    // fix #3263 defect 3: if the pass raised a new exception, propagate it
    // here. emitPendingCheck branches to the assignment statement's unwind
    // target (the surrounding try/catch or the caller's unwind) when bz$exc
    // is non-null — the later-wins throw now lives there. Without this, the
    // throw would be silently dropped (the AssignmentStmt path's post-pass
    // code does not itself emit a pending-check, since the #1385
    // implementation never had a throwing disposer in the pass).
    emitPendingCheck();
    return true;
}


// fix #3129 / §30.5 Pattern A: the §30.5 kind-1 / kind-2 decision used to live
// only on the no-finally branch of the ReturnStmt handler, so a return routed
// through a `try/finally` skipped both arms (no chained source, no disposer).
// This helper is the single predicate-with-several-readers shape: emit one of
// the two pre-emitAsyncComplete calls based on `chainedReturn` (and skip both
// for `borrowedReturn`, fix #3125). Called from (a) the ReturnStmt no-finally
// branch and (b) lowerTry's mode-5 terminal dispatch, so the decision reaches
// emitAsyncComplete on every Pattern A exit. MUST run BEFORE emitAsyncComplete:
// bz_task_complete notifies continuations that may free the outer task on a
// racing thread, so any attachment chained AFTER complete would touch freed
// memory. // per §30.5 kind 1 + kind 2 + kind 3, v0.252 + v0.254
void Emitter::emitAsyncPatternAResultDecision(bool chainedReturn,
                                               bool borrowedReturn) {
    // fix #3125: §30.5 Pattern A **kind 3** — `return await <borrowed source>`.
    // The outer task's teardown disposes nothing on this link (the source's
    // owner keeps disposing the source task, transitively disposing the
    // result). This is structurally a NO-OP, so neither the chained-slot call
    // nor the kind-1 disposer attach runs. `chainedReturn` and `borrowedReturn`
    // are mutually exclusive on the same return-statement site — both are
    // derived from the same operand by predicates that share
    // patternAResultDisposerFor as their Pattern-A source of truth — but the
    // BORROWED check has higher priority here because it is the more
    // conservative answer: kind 1 attaching a disposer to a borrow is the bug,
    // so even if a stale `chainedReturn` reached this site (it cannot, but
    // defensively), kind 3 wins and disposes nothing.
    if (borrowedReturn) {
        return;
    }
    if (chainedReturn) {
        // §30.5 / #1332 (v0.162): Pattern A chained return — chain the source
        // task (set by emitAwaitSuspend into asyncPendingTaskField_ and NOT freed
        // because asyncChainedReturn_ suppressed bz_task_free) to the outer task
        // BEFORE bz_task_complete. This ordering is required: bz_task_complete
        // notifies continuations which may immediately free the outer task on a
        // racing thread; calling bz_task_set_chained AFTER complete would be a
        // UAF on the outer task pointer.
        // Load source task from the pending-task frame slot (still alive:
        // freeTaskAfter=false kept it from being reclaimed in emitAwaitSuspend).
        llvm::Value* pendField = builder_.CreateStructGEP(
            smStateStruct_, smSelf_, asyncPendingTaskField_);
        llvm::Value* sourceTask =
            builder_.CreateLoad(ptrTy(), pendField, "chain.src");
        // Load outer task pointer (set at async-method entry).
        llvm::Value* otField = builder_.CreateStructGEP(
            smStateStruct_, smSelf_, asyncOuterTaskField_);
        llvm::Value* outerTask =
            builder_.CreateLoad(ptrTy(), otField, "chain.outer");
        // Transfer source-task lifetime ownership to the outer task's chained
        // slot: bz_task_free(outer) will recursively free source when done.
        auto* setChainedFn = declareC(
            "bz_task_set_chained", llvm::Type::getVoidTy(llvm_),
            {ptrTy(), ptrTy()});
        builder_.CreateCall(setChainedFn, {outerTask, sourceTask});
        // fix #4159 (§30.5 "Disposing an unconsumed OwningTask<T>", v0.298):
        // detectAsyncPatternAKind's CallExpr arm (LlvmBackend_Async.cpp) does
        // not distinguish a genuine Pattern A `Task<T:class>` chain source
        // from a Pattern B `OwningTask<T:class>` one — both reach here as
        // `chainedReturn`. A Pattern A source's own kind-1/kind-2 completion
        // ALWAYS leaves it with a chained_source or a result_disposer of its
        // own (transitively reaching a real disposer), so chaining alone
        // already disposes the result exactly once. A Pattern B source
        // produced by a genuinely kind-1 body (a bare fresh value, no
        // further await — e.g. an unannotated `async lambda() => new
        // Widget()` IIFE, whose own declared/inferred type is OwningTask
        // per ExprTypecheck_Exprs.cpp's #800-Layer-2 wrap, not Pattern A —
        // patternAResultDisposerFor's Task-only guard means NOTHING attaches
        // a disposer to it) leaves NEITHER field set: the value this body
        // just extracted from it is genuinely orphaned, and THIS body's own
        // kind-1 disposer (asyncPatternAResultDisposer_, already correct for
        // OUR OWN declared Pattern A return type) must additionally be
        // attached to our own task, or nothing ever disposes it — measured
        // 3200 B / 200 objects leaked without this.
        //
        // Which case applies is a RUNTIME fact (the source's chained_source
        // / result_disposer fields, not a static type), because the source
        // may be a NAMED local bound to an arbitrary lambda whose own body
        // codegen cannot inspect from here — so query it via
        // bz_task_needs_caller_disposal rather than re-deriving a static
        // guess. Doing so is always SAFE to check unconditionally (a
        // genuine Pattern A chain source reliably answers "already
        // handled", a no-op here) and is the one place that can distinguish
        // the two without risking a double free.
        if (asyncPatternAResultDisposer_) {
            auto* needsDisposalFn = declareC(
                "bz_task_needs_caller_disposal", llvm::Type::getInt8Ty(llvm_),
                {ptrTy()});
            llvm::Value* needsDisposal =
                builder_.CreateCall(needsDisposalFn, {sourceTask});
            llvm::Value* needsDisposalCond = builder_.CreateICmpNE(
                needsDisposal,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
            llvm::Function* curFn = builder_.GetInsertBlock()->getParent();
            auto* attachBB =
                llvm::BasicBlock::Create(llvm_, "patternb.orphan.attach", curFn);
            auto* contBB =
                llvm::BasicBlock::Create(llvm_, "patternb.orphan.cont", curFn);
            builder_.CreateCondBr(needsDisposalCond, attachBB, contBB);
            builder_.SetInsertPoint(attachBB);
            auto* setDisposerFn2 = declareC(
                "bz_task_set_result_disposer", llvm::Type::getVoidTy(llvm_),
                {ptrTy(), ptrTy()});
            builder_.CreateCall(setDisposerFn2,
                                {outerTask, asyncPatternAResultDisposer_});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        }
    } else if (asyncPatternAResultDisposer_) {
        // §30.5 / #1503: Pattern A (Task<T:class>) NON-chained return —
        // the async body produced a FRESH owned value, so the task owns
        // the result directly (§30.5: "the task owns the result and its
        // destructor disposes it directly"). Attach the per-T disposer
        // to the outer task BEFORE bz_task_complete, same race-safety
        // ordering as the chained-slot branch above (a continuation may
        // free the task the instant it completes). bz_task_free
        // consults this field whenever it eventually runs — whether the
        // task is already settled at that point (an already-done `using
        // var` scope-end / anonymous-await reclaim) or still running
        // (the fire-and-forget completion continuation) — so both the
        // "already done" and "still running at disposal time" cases in
        // §30.5's "Disposing an unconsumed OwningTask<T>" text (which
        // applies here by the same logic, even though that subsection
        // is written for Pattern B) are covered by one mechanism. A
        // borrow taken via a later `await`/`.Result` read does NOT
        // consume/null the slot for Pattern A (only Pattern B's
        // bz_task_take_result does that), so the result is disposed
        // exactly once whether or not it was ever borrowed.
        llvm::Value* otField = builder_.CreateStructGEP(
            smStateStruct_, smSelf_, asyncOuterTaskField_);
        llvm::Value* outerTask =
            builder_.CreateLoad(ptrTy(), otField, "patterna.outer");
        auto* setDisposerFn = declareC(
            "bz_task_set_result_disposer", llvm::Type::getVoidTy(llvm_),
            {ptrTy(), ptrTy()});
        builder_.CreateCall(setDisposerFn,
                            {outerTask, asyncPatternAResultDisposer_});
    }
}


bool Emitter::lowerTry(const ast::TryStmt& trys) {
    bool hasFin = trys.hasFinally;
    // §30.9 / §31.2 (#438): in an async state machine, an `await` inside a catch
    // arm or the finally is a real suspend point. The resume re-enters this
    // handler scope mid-handler, and a cross-thread resume runs on a worker with
    // a fresh stack — so any handler-context alloca STORED before the in-handler
    // await and READ after it (the catch-arm exception binding, the synthetic
    // caught-exception disposal slot, the finally exit-mode / return-value slots)
    // must round-trip the HEAP frame, not a function-local stack slot. We register
    // those slots in asyncHandlerSpillSlots_; fixupSuspendDominance later
    // force-promotes each into the frame spill area (repointing the alloca to a
    // heap GEP). Only the slots of a try region that actually contains an
    // in-handler await are frame-backed (a try whose catch/finally has no await
    // keeps its handler allocas on the stack, the cheap path).
    const bool asyncSM = (smKind_ == SuspendKind::Await ||
                          smKind_ == SuspendKind::AsyncYield) &&
                         smStateStruct_ && smSelf_;
    // Whether THIS try's catch arms / finally suspend on an `await` is decided
    // per-arm below (armAwaits) and, for the finally, here.
    const bool finallyAwaits =
        asyncSM && hasFin && containsAwait(trys.finallyBody);
    // CG21b/CG21c: a `finally` runs on every exit from the try.
    // Normal flow-through and return/break/continue all go through
    // finNormalBB; exception exits go through finUnwindBB.
    // An exit-mode slot (i32) tracks which kind of exit happened so
    // finNormalBB's resume block can dispatch to the right successor.
    llvm::BasicBlock* outerUnwind =
        hasFin ? (unwindTarget_ ? unwindTarget_ : propagateBB()) : nullptr;
    auto* dispatchBB =
        llvm::BasicBlock::Create(llvm_, "catch.dispatch", currentFn_);
    auto* afterBB = llvm::BasicBlock::Create(llvm_, "try.after", currentFn_);
    llvm::BasicBlock* finNormalBB =
        hasFin ? llvm::BasicBlock::Create(llvm_, "finally", currentFn_)
               : nullptr;
    llvm::BasicBlock* finUnwindBB =
        hasFin ? llvm::BasicBlock::Create(llvm_, "finally.unwind",
                                          currentFn_)
               : nullptr;
    // Where a normal exit (body/arm fall-through) goes.
    llvm::BasicBlock* normalExit = hasFin ? finNormalBB : afterBB;

    // CG21c: per-try-with-finally exit mode + return value storage.
    llvm::AllocaInst* exitModSlot = nullptr;
    llvm::AllocaInst* retValSlot  = nullptr;
    llvm::Type* retTy = currentFn_ ? currentFn_->getReturnType() : nullptr;
    llvm::BasicBlock* loopBreakBB = loopStack_.empty()
        ? nullptr : loopStack_.back().breakTarget;
    llvm::BasicBlock* loopContBB  = loopStack_.empty()
        ? nullptr : loopStack_.back().continueTarget;
    // fix #1598 / §29.7/§28.10 (v0.182): the disposeStack_ depth at the
    // innermost loop's own entry (captured alongside loopBreakBB/loopContBB,
    // same loopStack_.back()). A break/continue that exits straight to this
    // loop (no further outer finally) disposes the enclosing range
    // [loopScopeDepth, bodyScopeIdx) — scopes established before this try's body
    // scope but still inside the loop, e.g. an enclosing `using var` — AFTER the
    // finally body runs, mirroring mode 1/5's enclosing-range dispatch (the body
    // scope itself is disposed at the exit site; see the frame-setup comment).
    std::size_t loopScopeDepth =
        loopStack_.empty() ? 0 : loopStack_.back().scopeDepth;
    // #721 C1: in an async method, `return v` completes the OUTER task with the
    // unwrapped result (asyncResultTy_), not a function `ret`. Allocate a slot of
    // that type so an async return inside this try can stash its value across the
    // finally (exit mode 5). Null for a `Task` method (asyncResultTy_ == null).
    llvm::AllocaInst* asyncRetSlot = nullptr;
    if (hasFin) {
        exitModSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), exitModSlot);
        if (retTy && !retTy->isVoidTy())
            retValSlot = entryAlloca(retTy);
        if (smKind_ == SuspendKind::Await && smStateStruct_ && asyncResultTy_)
            asyncRetSlot = entryAlloca(asyncResultTy_);
        FinallyFrame frame{finNormalBB, exitModSlot, retValSlot,
                           retTy, afterBB, loopBreakBB, loopContBB};
        frame.asyncRetSlot = asyncRetSlot;
        // #1083: remember how many loops were already open at try-entry. A
        // break/continue of a loop opened INSIDE the try body must not route
        // through this finally (see the break/continue handlers).
        frame.loopDepthAtEntry = loopStack_.size();
        // fix #2892 / §28.10 + §31.2 (v0.241, owner ruling; text current at
        // v0.252): "A `try` statement's own `finally` is **established at entry
        // to the protected block** — before any establishment the block itself
        // performs. Consequently, on every exit edge, an owned local or
        // `using var` declared **inside** the protected block (established later)
        // is disposed **before** the `finally` runs, exactly as the desugared
        // nested `try`/`finally` of this section and §29.6 would dispose it;
        // cleanups established **before** the `try` run after the `finally`."
        //
        // The finally's rank on the single establishment-ordered cleanup stack of
        // §28.10 is therefore EXACTLY this dispose-stack depth — the depth before
        // the protected block pushes its own scope. Recording that depth is the
        // whole of the ordering rule; nothing else is needed:
        //  • the body's own scope is pushed by lowerScopedBlock at index
        //    disposeDepthAtEntry, i.e. immediately ABOVE the finally, so every
        //    edge that unwinds the body scope runs it before reaching this
        //    finally. On the NORMAL edge lowerScopedBlock's own fall-through
        //    emitDisposals does it; on the EXCEPTION edge the throw site's
        //    emitUnwindDisposals does (unwindScopeDepth_ == this depth, so the
        //    body scope sits above it and is disposed there); on the
        //    RETURN/BREAK/CONTINUE/YIELD-BREAK/async-return edges the exit site's
        //    emitDisposalsInRange(disposeDepthAtEntry, top) does.
        //  • the mode dispatches (modes 1-5) and finUnwindBB then dispose only the
        //    ENCLOSING scopes (< this depth) AFTER the finally body — the
        //    #1567/#1598/#2051 reverse-establishment chain, unchanged.
        // §29.7's early-iterator Dispose already ranks a finally at exactly this
        // depth (see the flushFinallysBefore builder in lowerYield), so the
        // suspended edge and the direct edges now agree by construction.
        //
        // This REPLACES #1263's deferral, which pushed the body scope below
        // unwindScopeDepth_ so that the finally ran first (Reading B of #2892).
        // The owner ruling records why that reading was not codifiable: ranking a
        // finally at its clause's textual position inverts nesting for nested
        // finallys and would need a category-ordered phase, the construct v0.182
        // abolished. #1263's authority was a pre-v0.182 sentence that no longer
        // exists. Consequences of the revert, all deliberate: mode 0's
        // post-finally body-scope disposal is gone (the switch falls straight to
        // afterBB), and #2869's exit mode 6 / `unwoundBodyScopes_` marking are gone
        // with it — a throw out of the body now unwind-disposes the body scope at
        // the throw site, exactly as a try WITHOUT a finally always did, so the
        // `unwindOnly` entries #2869 recovered can no longer be missed.
        frame.disposeDepthAtEntry = disposeStack_.size();
        finallyStack_.push_back(frame);
        // §29.7 (#623): track the AST finally-body alongside the frame so
        // a yield inside this try can snapshot the chain for Dispose.
        finallyBodyStack_.push_back(&trys.finallyBody);
        // §30.9 / §31.2 (#438): an in-finally await re-enters the finally
        // mid-body; the exit-mode and return-value slots were set BEFORE the
        // finally (in the normal-exit path) and are read AFTER the suspend (the
        // finally.resume dispatch), so frame-back them across the cross-thread hop.
        if (finallyAwaits) {
            asyncHandlerSpillSlots_.push_back(exitModSlot);
            if (retValSlot) asyncHandlerSpillSlots_.push_back(retValSlot);
            if (asyncRetSlot) asyncHandlerSpillSlots_.push_back(asyncRetSlot);
        }
    }

    llvm::BasicBlock* savedUnwind = unwindTarget_;
    std::size_t savedDepth = unwindScopeDepth_;
    unwindTarget_ = dispatchBB;
    unwindScopeDepth_ = disposeStack_.size();
    // #467 / §28.3: the dispose-stack depth at THIS try's entry. The scopes in
    // [savedDepth, thisTryDepth) lie between this try and its enclosing handler
    // (`outerUnwind`). A throw out of this try's body only disposes scopes down
    // to thisTryDepth; on the exception path through this try's finally those
    // intervening scopes (e.g. an outer `using var`) are still live and must be
    // disposed when re-propagating to outerUnwind — see finUnwindBB below.
    std::size_t thisTryDepth = unwindScopeDepth_;
    // fix #2892 / §28.10 (v0.241): the finally ranks AT this depth, and the
    // protected block's own scope is pushed by lowerScopedBlock immediately above
    // it — so the body scope's index IS thisTryDepth. The mode dispatches
    // (modes 1-5) and finUnwindBB dispose only the ENCLOSING scopes
    // [.., bodyScopeIdx) after the finally body; everything at or above
    // bodyScopeIdx has already been disposed on the edge that left the block.
    const std::size_t bodyScopeIdx = thisTryDepth;
    // fix #2892 / §28.10 (v0.241): lower the protected block as an ordinary
    // scoped block on BOTH shapes. lowerScopedBlock disposes the body's owned
    // locals / `using var`s at its own fall-through — i.e. before control reaches
    // this try's finally, which is established at protected-block ENTRY. (#1263
    // used lowerBody here to defer that disposal past the finally body; see the
    // frame-setup comment for why that reading was retired.)
    bool bodyTerm = lowerScopedBlock(trys.body);
    unwindTarget_ = savedUnwind;
    unwindScopeDepth_ = savedDepth;
    if (!bodyTerm) builder_.CreateBr(normalExit);

    // fix #2892 / §28.3 "Exception-path discharge" + §28.10 (v0.241): control only
    // reaches the catch dispatch by an EXCEPTION leaving the try body, and by then
    // the body scope has already been unwind-disposed AT THE THROW SITE
    // (unwindScopeDepth_ == thisTryDepth, so the body scope — pushed above it by
    // lowerScopedBlock — is in emitUnwindDisposals' range) and popped. That is
    // what makes #2869's recovery machinery unnecessary here: with the body scope
    // gone from disposeStack_ before any arm is lowered, no disposal emitted from
    // this point can name it, so its `unwindOnly` / `releaseOnUnwindOnly` entries
    // can neither be skipped (the #2869 leak) nor fired twice. A try WITHOUT a
    // finally has always worked exactly this way.

    // Catch dispatch: test arms in order against the in-flight exception.
    builder_.SetInsertPoint(dispatchBB);
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
    for (const auto& arm : trys.catches) {
        // §30.9 / §31.2 (#438): does THIS arm suspend on an `await` (guard or
        // body)? If so its handler-context allocas must survive a cross-thread
        // resume — frame-back them below.
        const bool armAwaits =
            asyncSM &&
            ((arm.guard && exprContainsAwait(arm.guard.get())) ||
             stmtContainsAwait(arm.body.get()));
        auto* armBB = llvm::BasicBlock::Create(llvm_, "catch.arm",
                                               currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "catch.next",
                                                currentFn_);
        llvm::Value* matched = builder_.getInt1(false);
        for (const auto& ty : arm.types) {
            if (!ty) continue;
            sema::TypeRef* rt = ctx_.resolvedType(*ty);
            // §31.2.1 (v0.182 / #1505): a generic type parameter is admissible
            // as an arm type and matches against its SUBSTITUTED type argument
            // per instantiation. Inside a monomorphized body resolveTypeParamRef
            // maps the placeholder (`TException`) to the concrete bound type
            // (`MyFault`). Without it the placeholder ref has no `declaration`
            // and no builtin-name entry, so descriptorForTypeName returns null,
            // the arm's type test is skipped, and dispatch silently falls
            // through to a later `Exception` arm (wrong-arm selection).
            const sema::TypeRef* resolved = resolveTypeParamRef(rt);
            const ast::Type* descNode = ty.get();
            if (resolved && resolved != rt && resolved->astNode)
                descNode = resolved->astNode;
            llvm::GlobalVariable* desc =
                descriptorForTypeName(resolved, descNode);
            // §31.2.1/§12.7 (#1939 leg 3): a CONSTRUCTED generic arm type
            // (`catch var e: MyExc<string>`, or `MyExc<T>` under the active
            // substitution) must test against its per-monomorphization
            // descriptor — the same resolution `is`/`as` (lowerTypeTest) and
            // case patterns use. The shared per-declaration descriptor is NOT
            // in a per-instantiation object's chain (#1843 mints the chain
            // from per-inst elements), so the shared target silently never
            // matched a directly-thrown MyExc<string> (wrong-arm selection, a
            // false negative) — and would erased-match every sibling if it
            // did. perInstDescriptorForTarget substitutes type parameters
            // recursively and returns null for a non-generic / non-concrete
            // target, keeping the shared resolution for those.
            //
            // §12.7/§2.8.3 rule 4/§31.2.1 (#2012, #2069): if the arm's type parameter
            // substitutes to a runtime-erased kind at this monomorphization
            // (`catch var e: MyExc<T>`, T=(int)->int) the descriptor collapses
            // onto a sibling and the arm would catch a wrong-instantiation
            // exception at a wrong static type — REJECT (probe p_catch_class:
            // rc=0, answered 1 on c3ea38c). For a constructed arm (`MyExc<T>`)
            // resolveTypeParamRef leaves the inner T unbound (resolved==rt), so the
            // per-argument substitution fires; a bare-parameter arm (`T` bound to a
            // constructed generic) is rewritten top-level, so pass
            // targetFromTypeVar there. On reject skip this arm type (the reported
            // Error fails the compile).
            bool armFromTypeVar = rt && rt->kind == sema::TypeRef::Kind::Named &&
                                  !rt->declaration && resolved && resolved != rt;
            if (rejectCollapsedTypeTestTarget(resolved, ty->span.begin,
                                              armFromTypeVar))
                continue;
            if (llvm::GlobalVariable* pid = perInstDescriptorForTarget(resolved))
                desc = pid;
            if (!desc) continue;
            matched = builder_.CreateOr(matched, emitIsA(exc, desc));
        }
        bool needsPrep =
            arm.guard || (arm.hasBinding && arm.body);
        if (needsPrep) {
            auto* prepBB = llvm::BasicBlock::Create(llvm_, "catch.prep",
                                                    currentFn_);
            builder_.CreateCondBr(matched, prepBB, nextBB);
            builder_.SetInsertPoint(prepBB);
            if (arm.hasBinding && arm.body) {
                llvm::AllocaInst* slot = entryAlloca(ptrTy());
                builder_.CreateStore(exc, slot);
                locals_[arm.body.get()] = slot;
                // The binding is read in the arm body, possibly AFTER an in-arm
                // await (§31.1: the borrow's lifetime is the whole arm body, which
                // spans the suspend) — frame-back it so the caught object pointer
                // survives the cross-thread resume.
                if (armAwaits) asyncHandlerSpillSlots_.push_back(slot);
            }
            if (arm.guard) {
                // §31 (#720 B2): the `when` guard runs DURING catch dispatch,
                // while the in-flight exception is still in the global slot. Any
                // `new`/call inside the guard would otherwise misfire the
                // pending-exception checks — emitNewPendingCheck/emitPendingCheck
                // see the in-flight exception and treat it as a failure of the
                // guard's OWN operation (freeing freshly-allocated objects,
                // short-circuiting the call). Set the in-flight exception aside
                // for the guard: clear the slot before, restore it on the
                // not-thrown continuation. A throw FROM the guard sets a NEW
                // exception that the in-guard pending-checks propagate — it
                // supersedes the in-flight one (consistent with throw-in-finally,
                // #720 B1).
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
                // fix #2881 / §31.2 + §31.2.1 (v0.240): a guard that THROWS must
                // still run THIS try's `finally`. §31.2: "`finally` runs whether
                // the protected block completed normally, was exited by an
                // exception (caught or uncaught), or was exited by `return` — or
                // by a `break` or `continue` targeting an enclosing loop."; the
                // guard's new exception "propagates to the enclosing handler"
                // (§31.2.1), i.e. it LEAVES this try statement, so the try is
                // exited by an exception and its finally owes a run. Pre-fix the
                // guard was lowered while `unwindTarget_` was still the ENCLOSING
                // handler (restored right after the try body) — the redirect to
                // finUnwindBB happened only in armBB below, i.e. after the guard
                // had already selected the arm — so a guard throw branched
                // straight past this try's finally.
                //
                // WHICH exception finUnwindBB re-propagates is settled by
                // §31.2.1: "If the guard itself throws, that new exception
                // supersedes the in-flight one — the superseded exception is
                // disposed by the runtime exactly once, as if its handling had
                // completed ... — and propagates to the enclosing handler". The
                // NEW one propagates; that is what finUnwindBB does unchanged —
                // it captures whatever is in `bz$exc` on entry, which on this
                // edge is the guard's exception (the in-flight E1 was cleared
                // just above, #720 B2), and restores it after the finally body.
                // E1's single dispose is unchanged too: it is performed at the
                // guard's own throw/pending-check site by the #1108/#1216 path
                // set up immediately below, which fires iff `caughtExcArmTarget_`
                // equals the throw's resolved unwind target — both are computed
                // from `unwindTarget_`, so they still compare equal once both are
                // finUnwindBB.
                //
                // DISPOSAL IS RELOCATED, NOT DUPLICATED. With `unwindScopeDepth_`
                // raised to this try's depth for the guard, the guard's throw site
                // no longer disposes the scopes between this try and its enclosing
                // handler; finUnwindBB's emitUnwindDisposalsInRange(savedDepth,
                // thisTryDepth) disposes exactly that same range after the finally
                // body — those cleanups were established BEFORE the try, so §28.10
                // (v0.241) puts them after it. Scopes pushed INSIDE the guard
                // expression (temporaries) sit above the new depth and are still
                // disposed at the throw site. The try BODY scope is not in play on
                // this edge at all: it was disposed at the ORIGINAL throw site,
                // before the guard ran (fix #2892).
                llvm::BasicBlock* savedGuardUnwind = unwindTarget_;
                std::size_t savedGuardUnwindDepth = unwindScopeDepth_;
                if (hasFin) {
                    unwindTarget_ = finUnwindBB;
                    unwindScopeDepth_ = disposeStack_.size();
                }
                // #1108: a `when` guard that THROWS supersedes the in-flight
                // exception (E1); E1 must be freed exactly once. Expose E1 as
                // the caught object LEAVING this arm for the duration of the
                // guard so a guard `throw` routes through emitThrowValue's
                // existing #397/#866 caught-exception free (LlvmBackend_Setup
                // ~2142) — emitted at a LIVE insert point (before the throw's
                // own branch) and already guarded against double-free by
                // `isDirectRethrow` (a guard re-raising the SAME bound object
                // keeps it in flight) and `throwLeavesArm`. This adds NO new
                // free; it reuses the battle-tested path. Without it E1 leaked
                // (the arm's `caughtException` disposal is registered only in
                // armBB below, which is unreachable on the guard-throw edge).
                // Restored immediately after the guard so armBB re-establishes
                // its own caught-exception context from the correct outer state.
                llvm::Value* savedGuardCaught = currentCaughtExc_;
                std::string_view savedGuardCaughtName = currentCaughtName_;
                llvm::BasicBlock* savedGuardArmTarget = caughtExcArmTarget_;
                currentCaughtExc_ = exc;
                currentCaughtName_ =
                    arm.hasBinding ? arm.varName : std::string_view{};
                caughtExcArmTarget_ = unwindBB();
                llvm::Value* g = lowerExpr(*arm.guard);
                currentCaughtExc_ = savedGuardCaught;
                currentCaughtName_ = savedGuardCaughtName;
                caughtExcArmTarget_ = savedGuardArmTarget;
                // fix #2881: the guard is lowered; restore the enclosing unwind
                // context. armBB re-establishes its own redirect below, and the
                // not-thrown continuation (armBB/nextBB) is reached by an explicit
                // branch, never by an unwind edge.
                unwindTarget_ = savedGuardUnwind;
                unwindScopeDepth_ = savedGuardUnwindDepth;
                if (diverged_) {
                    // §31.2.1 / §31.5 (#1097): the guard expression ITSELF threw
                    // (e.g. `catch E when throw new F()` — a `throw` is a
                    // bottom-typed expression valid in any expression position).
                    // The new exception supersedes the in-flight one and
                    // propagates via the throw's own unwind branch, which already
                    // terminated this prep block. Do NOT restore the exception or
                    // branch on the throw's (poison) value — that produced a
                    // `br ptr poison` malformed-IR crash. Reset the divergence
                    // flag for the rest of the catch-dispatch chain; armBB/nextBB
                    // become unreachable from this guard (harmless dead blocks).
                    diverged_ = false;
                } else {
                    if (!g) g = builder_.getInt1(false);
                    // Guard returned normally (did not throw): restore the
                    // in-flight exception so a non-match continues matching /
                    // re-propagates with it. (The matched armBB clears the slot
                    // again below.)
                    builder_.CreateStore(exc, emitExcSlot());
                    builder_.CreateCondBr(g, armBB, nextBB);
                }
            } else {
                builder_.CreateBr(armBB);
            }
        } else {
            builder_.CreateCondBr(matched, armBB, nextBB);
        }

        builder_.SetInsertPoint(armBB);
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
        llvm::Value* savedCaught = currentCaughtExc_;
        std::string_view savedCaughtName = currentCaughtName_;
        llvm::BasicBlock* savedArmTarget = caughtExcArmTarget_;
        currentCaughtExc_ = exc;
        // #387 / §31.1: the name (if any) this arm binds the caught exception
        // to, so a `throw <newExpr>` from the body can tell whether the new
        // exception's expression references the caught object. A bare-type arm
        // ("" name) can never reference it → such throws always free it.
        currentCaughtName_ = arm.hasBinding ? arm.varName : std::string_view{};
        // #383 / §31.1: ownership of the exception transfers to the runtime,
        // which disposes it after the matching catch arm completes. Register
        // the caught object as a `caughtException` disposal in a dedicated
        // scope around the arm body: a NORMAL handled exit (fall-through,
        // return, break, continue) frees it via the disposal machinery
        // (emitDisposeAndReclaim — destructor + base chain + free, #380); the
        // UNWIND path (a re-raise `throw;` or `throw expr` from the arm body)
        // SKIPS it (emitDisposals' isUnwind), keeping it in flight so the
        // enclosing handler frees it exactly once. The binding is a borrow
        // (§31.1) that may not escape; disposal runs after the body's last
        // use (return values are evaluated before disposal — §28.3).
        disposeStack_.emplace_back();
        {
            llvm::AllocaInst* excSlot = entryAlloca(ptrTy());
            builder_.CreateStore(exc, excSlot);
            Disposal d{excSlot, nullptr};
            d.caughtException = true;
            disposeStack_.back().push_back(d);
            // §30.9 / §31.2 (#438): the caught-exception disposal runs on the
            // arm's NORMAL fall-through — which, with an in-arm await, is AFTER the
            // resume. emitDisposeAndReclaim reads the object pointer from excSlot,
            // so the slot must survive the cross-thread hop: frame-back it.
            if (armAwaits) asyncHandlerSpillSlots_.push_back(excSlot);
        }
        // An exception thrown by the arm body must still run finally.
        if (hasFin) { unwindTarget_ = finUnwindBB; unwindScopeDepth_ =
                          disposeStack_.size(); }
        // #866: record the target a throw uses to LEAVE this arm. emitThrowValue
        // frees the caught exception (the #397 optimization) only when a throw's
        // unwind target equals this; a throw caught by an INNER try within the arm
        // targets that inner dispatch instead and must NOT free it (the arm
        // continues; the caught exception stays in flight for the arm's own exit).
        // Use the RESOLVED block (unwindBB() maps a null target to the function's
        // propagate boundary) so a single try with no enclosing handler still
        // compares correctly against a throw's resolved target.
        caughtExcArmTarget_ = unwindBB();
        bool armTerm = false;
        if (arm.body) armTerm = lowerStmt(*arm.body);
        if (hasFin) { unwindTarget_ = savedUnwind;
                      unwindScopeDepth_ = savedDepth; }
        // On a normal handled fall-through, dispose (free) the caught
        // exception via the scope, then continue to the normal exit. A
        // terminating arm body (return/break/continue) already freed it via
        // emitAllDisposals/emitLoopDisposals; a re-raise/throw skipped it.
        if (!armTerm) emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
        currentCaughtExc_ = savedCaught;
        currentCaughtName_ = savedCaughtName;
        caughtExcArmTarget_ = savedArmTarget;
        if (!armTerm) {
            // fix #2892 / §28.10 (v0.241): this arm HANDLED the exception and is
            // falling through to the finally — the same destination AND the same
            // (empty) disposal as a genuine body fall-through, because the body
            // scope was disposed at the throw site. Store mode 0 explicitly rather
            // than relying on the try-entry initialization: an exit site inside
            // the body may have written a non-zero mode into the slot before an
            // intervening operation threw. (#2869's mode 6 lived here; it existed
            // only to recover the body scope's `unwindOnly` entries, which the
            // throw site now disposes.)
            if (hasFin)
                builder_.CreateStore(builder_.getInt32(0), exitModSlot);
            builder_.CreateBr(normalExit);
        }

        builder_.SetInsertPoint(nextBB);
    }
    // No arm matched → exception still pending; run finally (if any)
    // then re-propagate to the enclosing unwind target.
    if (hasFin) {
        builder_.CreateBr(finUnwindBB);
    } else {
        // #822 / §28.3: with no finally, the throw site disposed only down to
        // this try's depth, so owned locals in the scopes BETWEEN this try and
        // the enclosing handler (e.g. an outer `using var` declared before the
        // inner try in the same block) are still live and would LEAK on the
        // unmatched-arm re-propagation path. Dispose that intervening range
        // before re-propagating — the same discharge finUnwindBB performs after
        // its finally body (above).
        emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
        builder_.CreateBr(unwindBB());
    }

    if (hasFin) {
        // §21.5 (v0.201, #2051): grab this frame's labeled-break/continue routes
        // (accumulated while lowering the try body) before the pop — the switch
        // below emits a case per entry.
        std::vector<FinallyFrame::LabeledRoute> myLabeledRoutes =
            std::move(finallyStack_.back().labeledRoutes);
        // fix #3129 / §30.5 Pattern A kind 2: snapshot the chained-return flag
        // this finally's frame carries — captured at the ReturnStmt site that
        // routed through this finally (see the ReturnStmt branch at the head of
        // this file). The frame is about to be popped, and the mode-5 dispatch
        // (further below) needs the flag to decide whether to call
        // bz_task_set_chained on the outer task at the terminal emitAsyncComplete.
        bool myChainedAsyncRet = finallyStack_.back().chainedAsyncReturn;
        // fix #3125: kind-3 BORROWED twin — the same finally's frame carries
        // the borrowed-return flag (set by the same ReturnStmt site, on the
        // same routed edge). The terminal mode-5 dispatch reads it to ask
        // emitAsyncPatternAResultDecision to skip the kind-1 disposer attach.
        bool myBorrowedAsyncRet = finallyStack_.back().borrowedAsyncReturn;
        // Pop the frame — outer frames (if any) are now at back().
        finallyStack_.pop_back();
        if (!finallyBodyStack_.empty()) finallyBodyStack_.pop_back();
        FinallyFrame* outerFr = finallyStack_.empty()
            ? nullptr : &finallyStack_.back();

        // finNormalBB: run finally body, then dispatch based on exit mode.
        builder_.SetInsertPoint(finNormalBB);
        bool finTerm = lowerScopedBlock(trys.finallyBody);
        auto* resumeBB = llvm::BasicBlock::Create(llvm_, "finally.resume",
                                                  currentFn_);
        if (!finTerm) builder_.CreateBr(resumeBB);

        // Build the dispatch block after finally.
        builder_.SetInsertPoint(resumeBB);
        llvm::Value* mode = builder_.CreateLoad(i32(), exitModSlot);
        // fix #2892 / §28.10 (v0.241): mode=0 — the protected block completed
        // normally (or a catch arm of this try handled the exception and fell
        // through). Nothing is left to dispose HERE: the body's own scope was
        // disposed before control reached the finally (lowerScopedBlock's
        // fall-through on the normal edge, the throw site's emitUnwindDisposals on
        // the handled-exception edge), because this finally is established at
        // protected-block ENTRY and therefore ranks BELOW everything the block
        // establishes. The enclosing scopes outlive the try and are not disposed
        // here either. (#1263's post-finally body-scope disposal and #2869's
        // mode-6 twin of it both lived here.)
        auto* sw = builder_.CreateSwitch(mode, afterBB, 3);

        // mode=1: return — propagate through outer finally or emit ret.
        auto* retDispBB = llvm::BasicBlock::Create(llvm_,"finally.ret",currentFn_);
        sw->addCase(builder_.getInt32(1), retDispBB);
        builder_.SetInsertPoint(retDispBB);
        if (outerFr) {
            // fix #1567 / §29.7/§28.10 (v0.182): the finally body has now run;
            // dispose the scopes established between THIS finally and the next-
            // outer finally (reverse establishment) BEFORE routing the return
            // onward, then let the outer finally run and dispose its own outer
            // range. This is the normal-exit twin of the exception path's
            // emitUnwindDisposalsInRange(savedDepth, thisTryDepth) below.
            emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
            // Route through outer finally (nested try-with-finally).
            builder_.CreateStore(builder_.getInt32(1), outerFr->exitModSlot);
            if (retValSlot && outerFr->retValSlot) {
                llvm::Value* rv = builder_.CreateLoad(retTy, retValSlot);
                builder_.CreateStore(rv, outerFr->retValSlot);
            }
            builder_.CreateBr(outerFr->finNormalBB);
        } else {
            // fix #1567 / §29.7/§28.10 (v0.182): no enclosing finally — dispose
            // every remaining scope established before this try's body scope
            // (indices [0, bodyScopeIdx)), AFTER the finally body, then ret.
            // Together with the return site's disposal of the try-body scopes
            // (>= bodyScopeIdx) this disposes all live scopes in reverse
            // establishment order with the finally interleaved at its
            // establishment point.
            emitDisposalsInRange(0, bodyScopeIdx);
            llvm::Value* rv = (retValSlot && retTy && !retTy->isVoidTy())
                ? builder_.CreateLoad(retTy, retValSlot) : nullptr;
            emitReturn(currentFn_, rv);
        }

        // mode=2: break — propagate through outer finally or jump to break.
        if (loopBreakBB) {
            auto* brkDispBB = llvm::BasicBlock::Create(llvm_,"finally.brk",currentFn_);
            sw->addCase(builder_.getInt32(2), brkDispBB);
            builder_.SetInsertPoint(brkDispBB);
            if (outerFr) {
                // fix #1598 / §29.7/§28.10 (v0.182): dispose the scopes
                // established between this finally and the next-outer finally
                // (reverse establishment) before routing onward — the mode-2
                // twin of mode 1/5.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(2), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1598 / §29.7/§28.10 (v0.182): no further outer finally —
                // dispose every remaining scope down to the loop's own scope
                // depth (an enclosing `using var` established before this try
                // but inside the loop) AFTER the finally body, then jump to the
                // break target. The pre-v0.182 code disposed nothing here,
                // leaving that range disposed too early (at the break site,
                // before the finally) instead — a use-after-dispose.
                emitDisposalsInRange(loopScopeDepth, bodyScopeIdx);
                builder_.CreateBr(loopBreakBB);
            }
        }

        // mode=3: continue — propagate through outer finally or jump to cond.
        if (loopContBB) {
            auto* cntDispBB = llvm::BasicBlock::Create(llvm_,"finally.cnt",currentFn_);
            sw->addCase(builder_.getInt32(3), cntDispBB);
            builder_.SetInsertPoint(cntDispBB);
            if (outerFr) {
                // fix #1598 / §29.7/§28.10 (v0.182): mode-3 twin of mode-2 above.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(3), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1598 / §29.7/§28.10 (v0.182): mode-3 twin of mode-2 above.
                emitDisposalsInRange(loopScopeDepth, bodyScopeIdx);
                builder_.CreateBr(loopContBB);
            }
        }

        // §21.5 (v0.201, #2051): per-target modes for a labeled break/continue
        // that CROSSED this finally to reach an OUTER loop. Each mints a case
        // that, after the finally body has run, either (a) routes onward to the
        // next-outer finally when that finally is still INSIDE the target loop —
        // the labeled twin of the mode-2/3 outerFr path — or (b) terminates:
        // dispose down to the target loop's own scope depth and jump to its
        // break/continue block. The onward disposal ranges mirror modes 2/3, so
        // the reverse-establishment interleave (#1567/#1598) is preserved.
        for (const auto& route : myLabeledRoutes) {
            auto* rBB =
                llvm::BasicBlock::Create(llvm_, "finally.lbl", currentFn_);
            sw->addCase(builder_.getInt32(route.mode), rBB);
            builder_.SetInsertPoint(rBB);
            const bool outerInsideTarget =
                outerFr && outerFr->loopDepthAtEntry > route.targetLoopIdx;
            if (outerInsideTarget) {
                int outerMode = getOrAddLabeledRoute(
                    *outerFr, route.isBreak, route.targetLoopIdx,
                    route.targetBlock, route.targetScopeDepth);
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(outerMode),
                                     outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                emitDisposalsInRange(route.targetScopeDepth, bodyScopeIdx);
                builder_.CreateBr(route.targetBlock);
            }
        }

        // mode=4 (#721 C2): yield break — run any OUTER finally too, else
        // perform the terminal iterator exit the yield-break statement deferred
        // so the finally could run first (state=-1; sync → ret false, async →
        // complete with false + ret void). Added only inside an ITERATOR state
        // machine (Yield/AsyncYield) — the only place mode 4 is produced. A
        // plain async METHOD (SuspendKind::Await) also has smStateGEP_ set but
        // returns void, so emitting the iterator return here would be malformed.
        //
        // fix #2891 (§29.3/§29.7/§31.2, v0.240): read the frame's state pointer
        // through smStateGEPHere(), which additionally establishes that the GEP
        // is defined in the function being emitted RIGHT NOW. `smStateGEP_`
        // survives the emitter that published it (none of the three clears it),
        // so a plain method lowered afterwards — pointer-hash order, not
        // declaration order — otherwise opened this dispatch on the PREVIOUS
        // function's frame and emitted a cross-function store plus a `ret i1
        // false` from a function with an unrelated return type. See the
        // accessor's definition in LlvmBackend_impl.hpp.
        llvm::Value* smStateGEPForTry = smStateGEPHere();
        if (smStateGEPForTry && (smKind_ == SuspendKind::Yield ||
                                 smKind_ == SuspendKind::AsyncYield)) {
            auto* ybDispBB =
                llvm::BasicBlock::Create(llvm_, "finally.yieldbrk", currentFn_);
            sw->addCase(builder_.getInt32(4), ybDispBB);
            builder_.SetInsertPoint(ybDispBB);
            if (outerFr) {
                // fix #1598 / §29.7/§28.10 (v0.182): dispose the scopes
                // established between this finally and the next-outer finally
                // (reverse establishment) before routing onward — the mode-4
                // twin of mode 1/5.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(4), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1598 / §29.7/§28.10 (v0.182): a `yield break` is a
                // terminal exit from the whole MoveNext/Resume function (like
                // `return`), so — mirroring mode 1's emitDisposalsInRange(0,
                // bodyScopeIdx) — dispose every remaining scope established
                // before this try's body scope, AFTER the finally body, then
                // perform the terminal iterator exit. Pre-v0.182 this range was
                // never disposed on the yield-break edge at all.
                emitDisposalsInRange(0, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(-1), smStateGEPForTry);
                if (smKind_ == SuspendKind::AsyncYield) {
                    emitAsyncIterComplete(/*hasValue=*/false);
                    builder_.CreateRetVoid();
                } else {
                    builder_.CreateRet(builder_.getFalse());
                }
            }
        }

        // mode=5 (#721 C1): async-method return — run any OUTER finally too, else
        // complete the outer task with the stashed unwrapped result and ret void
        // from Resume (the async analogue of mode 1). Only inside an async method
        // state machine (SuspendKind::Await), the only place mode 5 is produced.
        if (smKind_ == SuspendKind::Await && smStateStruct_) {
            auto* arDispBB =
                llvm::BasicBlock::Create(llvm_, "finally.asyncret", currentFn_);
            sw->addCase(builder_.getInt32(5), arDispBB);
            builder_.SetInsertPoint(arDispBB);
            if (outerFr) {
                // fix #1567 / §29.7/§28.10 (v0.182): dispose the scopes between
                // this finally and the next-outer finally (reverse establishment)
                // after this finally body, before routing the async return onward
                // — the mode-5 twin of mode 1.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(5), outerFr->exitModSlot);
                if (asyncRetSlot && outerFr->asyncRetSlot && asyncResultTy_) {
                    llvm::Value* rv =
                        builder_.CreateLoad(asyncResultTy_, asyncRetSlot);
                    builder_.CreateStore(rv, outerFr->asyncRetSlot);
                }
                // fix #3129 / §30.5 Pattern A kind 2: propagate the chained
                // flag through each finally along the route. Any finally along
                // the path that ran a non-chained `return` would have written
                // its own mode-5 site with chainedAsyncReturn=false, but the
                // innermost ReturnStmt that routed here is the only producer of
                // mode 5 in the first place, so myChainedAsyncRet is the
                // single source of truth for THIS async-return edge.
                // // per §30.5 kind 2, v0.252
                outerFr->chainedAsyncReturn =
                    outerFr->chainedAsyncReturn || myChainedAsyncRet;
                // fix #3125: kind-3 BORROWED twin of the chained flag above.
                // Mutually exclusive with chainedAsyncReturn on the same
                // ReturnStmt site (both are derived from the same operand by
                // predicates sharing patternAResultDisposerFor) — propagating
                // both is harmless because emitAsyncPatternAResultDecision
                // asks borrowedReturn FIRST.
                outerFr->borrowedAsyncReturn =
                    outerFr->borrowedAsyncReturn || myBorrowedAsyncRet;
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1567 / §29.7/§28.10 (v0.182): no enclosing finally —
                // dispose every remaining scope established before this try
                // AFTER the finally body, then complete the outer task.
                emitDisposalsInRange(0, bodyScopeIdx);
                llvm::Value* rv = (asyncRetSlot && asyncResultTy_)
                    ? builder_.CreateLoad(asyncResultTy_, asyncRetSlot)
                    : nullptr;
                // fix #3129 / §30.5 Pattern A: the terminal site for an
                // async-return routed through a `finally` is the SAME one
                // that calls emitAsyncComplete — and §30.5 requires that
                // bz_task_set_chained (kind 2) or bz_task_set_result_disposer
                // (kind 1) attach BEFORE complete. Hoist the decision out
                // of the no-enclosing-finally branch into this helper so BOTH
                // exit edges — the direct branch and this routed edge — apply
                // it identically. // per §30.5 kind 1 + kind 2, v0.252
                // fix #3125: pass the borrowed-return flag too — see
                // emitAsyncPatternAResultDecision. The kind-3 BORROWED case
                // is a NO-OP (no chained-slot, no disposer).
                emitAsyncPatternAResultDecision(myChainedAsyncRet, myBorrowedAsyncRet);
                emitAsyncComplete(rv);
                builder_.CreateRetVoid();
            }
        }

        // finUnwindBB: exception exit — run finally, then re-propagate.
        builder_.SetInsertPoint(finUnwindBB);
        // §29.7 (#623): if we are inside an iterator state machine, mark the
        // iterator as done (state=-1) BEFORE running the finally body.  The
        // finally will run via this unwinding path; when the consumer's for-loop
        // cleanup subsequently calls Dispose, Dispose must see state=-1 and be a
        // no-op — otherwise the finally would run a second time.  We set -1
        // before the finally (not after) so that any finally-body code that
        // calls MoveNext/Dispose on the same iterator also sees it as done.
        // fix #2891 (§29.7/§31.2/§30.1, v0.240): "we are emitting inside a
        // sync/async iterator MoveNext / Resume function" is exactly what
        // smStateGEPHere() establishes and what a bare `smStateGEP_ != null`
        // does NOT: the member outlives the emitter that published it, so on a
        // plain method whose body merely contains a `try`/`finally` this store
        // referenced a GEP defined in a DIFFERENT function ("Referring to an
        // instruction in another function!" — a hard compile failure on the
        // spec-legal shape of §31.2's own finally example sitting next to a
        // §30.1 `async` method). A plain method has no iterator to mark done, so
        // emitting nothing here is the correct lowering, not merely the safe one.
        if (llvm::Value* smGEP = smStateGEPHere()) {
            builder_.CreateStore(builder_.getInt32(-1), smGEP);
        }
        if (finallyAwaits) {
            // §30.9 / §31.2 (#438): an `await` in the finally on the EXCEPTION
            // path suspends with an exception in flight. `bz$exc` is a single
            // process-global (not thread-local), so it would be clobbered by any
            // other task that runs on this thread while we are suspended. Capture
            // the in-flight exception object into a FRAME-BACKED slot (so it
            // survives the cross-thread resume), clear the global for the duration
            // of the finally, then restore it before re-propagating to the outer
            // handler — which expects to read the original object from bz$exc.
            llvm::AllocaInst* pendingExcSlot = entryAlloca(ptrTy());
            llvm::Value* inflight = builder_.CreateLoad(ptrTy(), emitExcSlot());
            builder_.CreateStore(inflight, pendingExcSlot);
            asyncHandlerSpillSlots_.push_back(pendingExcSlot);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
            // #1149 (sibling of #1108): a `throw` inside the finally body
            // SUPERSEDES the captured in-flight exception E1 (§31.2). E1 is then
            // abandoned and must be freed exactly once — otherwise it leaks (the
            // direct-throw path terminates the body and bypasses the supersede
            // check below; the finThrew edge only catches a fall-through-with-
            // bz$exc-set, which a direct throw never produces). Expose E1 as the
            // caught object for the duration of the finally body so a `throw`
            // routes through emitThrowValue's existing #397/#866 caught-exception
            // free — emitted at a live insert point before the throw's branch and
            // guarded against double-free by isDirectRethrow/throwLeavesArm. A
            // finally has no exception binding, so currentCaughtName_ stays empty
            // (never a direct-rethrow); caughtExcArmTarget_ = unwindBB() (= the
            // outer unwind target a finally throw branches to) makes throwLeavesArm
            // true. Restored immediately after so the supersede/restore logic and
            // outer scopes see the correct caught context. Mirrors the #1108 fix.
            llvm::Value* savedFinCaught = currentCaughtExc_;
            std::string_view savedFinCaughtName = currentCaughtName_;
            llvm::BasicBlock* savedFinArmTarget = caughtExcArmTarget_;
            currentCaughtExc_ = builder_.CreateLoad(ptrTy(), pendingExcSlot);
            currentCaughtName_ = std::string_view{};
            caughtExcArmTarget_ = unwindBB();
            bool finUTerm = lowerScopedBlock(trys.finallyBody);
            currentCaughtExc_ = savedFinCaught;
            currentCaughtName_ = savedFinCaughtName;
            caughtExcArmTarget_ = savedFinArmTarget;
            if (!finUTerm) {
                // Restore the captured in-flight exception and re-propagate. If
                // the finally itself threw (set bz$exc non-null), that NEW
                // exception supersedes the original (§31.2 finally-throws
                // semantics); keep it. Otherwise restore the original.
                llvm::Value* afterFin = builder_.CreateLoad(ptrTy(), emitExcSlot());
                llvm::Value* finThrew = builder_.CreateICmpNE(
                    afterFin, llvm::ConstantPointerNull::get(ptrTy()),
                    "fin.threw");
                auto* restoreBB =
                    llvm::BasicBlock::Create(llvm_, "finally.restore", currentFn_);
                auto* propBB =
                    llvm::BasicBlock::Create(llvm_, "finally.reraise", currentFn_);
                builder_.CreateCondBr(finThrew, propBB, restoreBB);
                builder_.SetInsertPoint(restoreBB);
                llvm::Value* orig = builder_.CreateLoad(ptrTy(), pendingExcSlot);
                builder_.CreateStore(orig, emitExcSlot());
                builder_.CreateBr(propBB);
                builder_.SetInsertPoint(propBB);
                // #467 / §28.3: same intervening-scope discharge as the
                // synchronous path below. Emitted after the in-flight exception
                // is restored to bz$exc so the enclosing handler sees the right
                // object; the disposals themselves do not touch bz$exc.
                emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
                builder_.CreateBr(outerUnwind);
            }
        } else {
            // #720 / §31.2: the synchronous unwind-path finally. Capture the
            // in-flight exception and CLEAR bz$exc before running the finally —
            // otherwise a `new`/call inside the finally body hits
            // emitNewPendingCheck/emitPendingCheck, sees the still-pending
            // ORIGINAL exception, and unwinds it immediately, BEFORE the finally's
            // own `throw` executes — silently swallowing the finally's exception.
            // After the finally: if it threw (bz$exc non-null), that NEW exception
            // supersedes (§31.2); otherwise restore the original. Mirrors the
            // async branch above, minus the cross-thread frame-spill.
            llvm::AllocaInst* pendingExcSlot = entryAlloca(ptrTy());
            builder_.CreateStore(builder_.CreateLoad(ptrTy(), emitExcSlot()),
                                 pendingExcSlot);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
            // #1149: expose the captured in-flight E1 as the caught object for
            // the finally body so a `throw` inside frees E1 exactly once via
            // emitThrowValue's #397/#866 path (§31.2 supersede). See the async
            // branch above for the full rationale + double-free guards.
            llvm::Value* savedFinCaught = currentCaughtExc_;
            std::string_view savedFinCaughtName = currentCaughtName_;
            llvm::BasicBlock* savedFinArmTarget = caughtExcArmTarget_;
            currentCaughtExc_ = builder_.CreateLoad(ptrTy(), pendingExcSlot);
            currentCaughtName_ = std::string_view{};
            caughtExcArmTarget_ = unwindBB();
            bool finBodyTerm = lowerScopedBlock(trys.finallyBody);
            currentCaughtExc_ = savedFinCaught;
            currentCaughtName_ = savedFinCaughtName;
            caughtExcArmTarget_ = savedFinArmTarget;
            if (!finBodyTerm) {
                llvm::Value* afterFin =
                    builder_.CreateLoad(ptrTy(), emitExcSlot());
                llvm::Value* finThrew = builder_.CreateICmpNE(
                    afterFin, llvm::ConstantPointerNull::get(ptrTy()),
                    "fin.threw");
                auto* restoreBB = llvm::BasicBlock::Create(
                    llvm_, "finally.restore", currentFn_);
                auto* propBB = llvm::BasicBlock::Create(
                    llvm_, "finally.reraise", currentFn_);
                builder_.CreateCondBr(finThrew, propBB, restoreBB);
                builder_.SetInsertPoint(restoreBB);
                builder_.CreateStore(
                    builder_.CreateLoad(ptrTy(), pendingExcSlot), emitExcSlot());
                builder_.CreateBr(propBB);
                builder_.SetInsertPoint(propBB);
                // #467 / §28.3: dispose owned locals/using-vars in the scopes
                // between this try and the enclosing handler (the throw site
                // only disposed down to thisTryDepth).
                emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
                builder_.CreateBr(outerUnwind);
            }
        }
    }

    builder_.SetInsertPoint(afterBB);
    return false;
}


bool Emitter::lowerIf(const ast::IfStmt& ifs) {
    llvm::BasicBlock* mergeBB = nullptr;
    auto merge = [&]() -> llvm::BasicBlock* {
        if (!mergeBB) mergeBB = llvm::BasicBlock::Create(
            llvm_, "if.end", currentFn_);
        return mergeBB;
    };

    // #1387 Item 1 / §28.11 / §22.4: push a dedicated condition scope so
    // out-inline locals declared in an if/elif CONDITION (e.g.
    // `if s.TryPop(out using var top)`) are disposed at the END of the
    // if-statement, not at the end of the enclosing block.
    //
    // ensureOutVarSlots (called inside lowerExpr for the condition call)
    // registers each out-inline local's disposal into disposeStack_.back().
    // Without this scope they land in the OUTER block's scope and are never
    // disposed until the outer block exits — causing a leak (return 122
    // instead of 121 in the test). With this scope:
    //   • normal-exit (merge): emitDisposals fires at the if's end → correct.
    //   • terminating paths (return/throw): emitAllDisposals includes this
    //     scope → correct (no disposal missed, no double-free).
    // The dynamicDispose entries are null-guarded, so paths where TryPop
    // returned false (slot null) are always safe.  // per §28.3, §22.4, §28.11
    disposeStack_.emplace_back();  // condition scope

    // The if-branch and each elif form a (condition, body) test chain.
    std::vector<const ast::IfStmt::Branch*> chain;
    chain.push_back(&ifs.ifBranch);
    for (const auto& el : ifs.elifBranches) chain.push_back(&el);

    bool allPathsTerminate = true;
    for (const ast::IfStmt::Branch* br : chain) {
        llvm::Value* cond =
            br->condition ? lowerExpr(*br->condition) : nullptr;
        // fix #5137 (§7.11, v0.313): the §7.11 'no three-valued logic' gate at
        // if-conditions (src/sema/ExprTypecheck.cpp ~6201) admits a `bool?`
        // condition narrowed non-null by §7.4. Sema returns success and codegen
        // receives the condition as the declared `{i8 hasVal; i1 payload}`
        // struct, but `CreateCondBr` below needs an `i1` — extract the payload
        // here. The pre-extraction soundness precondition of
        // unwrapNarrowedNullableOperand (a sema gate that rejects the
        // un-narrowed form) is satisfied by the #5137 sema half in this same
        // lane.
        if (cond && br->condition.get())
            cond = unwrapNarrowedNullableOperand(cond, br->condition.get());
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "if.body", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "if.next", currentFn_);
        if (cond) builder_.CreateCondBr(cond, bodyBB, nextBB);
        // #2775 (§17): NOT merely "defensive" — this arm is REACHABLE, and it
        // is where a condition that failed to lower becomes a SILENT `false`:
        // the body is branched past and ends up with no predecessors at all.
        // It is kept (removing it would turn a degraded compile into a crash),
        // but every producer that can return null for a condition expression
        // must report its own diagnostic before doing so — lowerTypeTest's two
        // bails now do. If you land here debugging a "the if body never runs"
        // report, the defect is in the condition's lowering, not here.
        else builder_.CreateBr(nextBB);

        builder_.SetInsertPoint(bodyBB);
        if (!lowerScopedBlock(br->body)) {
            allPathsTerminate = false;
            builder_.CreateBr(merge());
        }
        builder_.SetInsertPoint(nextBB);  // next test / else goes here
    }

    // The final fall-through block is the else (or an empty pass-through).
    if (ifs.hasElse) {
        if (!lowerScopedBlock(ifs.elseBody)) {
            allPathsTerminate = false;
            builder_.CreateBr(merge());
        }
    } else {
        allPathsTerminate = false;
        builder_.CreateBr(merge());
    }

    if (mergeBB) {
        builder_.SetInsertPoint(mergeBB);
        // Dispose any out-inline locals from the condition scope at the
        // if-statement's end (normal-exit path). Paths that terminated
        // (return/throw) already emitted these via emitAllDisposals; the
        // null-guard in dynamicDispose makes the slot-null paths safe.
        emitDisposals(disposeStack_.back());
        // fix #1602 / §28.10 / §28.11: emitDisposals above runs with the
        // default isUnwind=false, so any `unwindOnly` entry registered while
        // lowering the condition — the #1466 pre-declared out-owned recipient
        // written by the condition's own call, or an inline `out owned var` /
        // `out using var` recipient (§22.4) not yet discharged — was SKIPPED,
        // not fired: it is a pure exception-edge backstop, not a normal-exit
        // disposal. Popping the condition scope right after would silently
        // drop that still-live entry from disposeStack_ entirely, so a LATER
        // throw — anywhere in the enclosing block, after this if-statement,
        // before the recipient's real discharge point — would never find it,
        // and the recipient would leak (bounded leak, confirmed via valgrind
        // in #1602; never a double-free, since the entry is null-guarded and
        // an explicit discharge nulls the slot before any later dispose
        // could run). Migrate any surviving `unwindOnly` entries into the
        // enclosing scope instead of discarding them: per §22.4, the
        // recipient's true lexical home is "the enclosing block, from the
        // call onward" — it outlives this transient condition scope (#1387
        // Item 1), so its unwind backstop must too. `dynamicDispose` entries
        // need no such migration: they already fired, unconditionally, in
        // the emitDisposals call above (#1387's if-end auto-drop timing is
        // unaffected).
        if (disposeStack_.size() >= 2) {
            std::vector<Disposal>& condScope = disposeStack_.back();
            std::vector<Disposal>& outerScope =
                disposeStack_[disposeStack_.size() - 2];
            for (const Disposal& d : condScope)
                if (d.unwindOnly) outerScope.push_back(d);
        }
        disposeStack_.pop_back();
        return false;
    }
    // All paths terminated; each branch's emitAllDisposals already included
    // the condition scope. Just pop it to restore the stack.
    disposeStack_.pop_back();
    return allPathsTerminate;
}


// §21.5 (v0.201, #2051): the exit-mode value for routing a labeled break/
// continue THROUGH a finally frame. Deduped per (isBreak, targetLoopIdx).
int Emitter::getOrAddLabeledRoute(FinallyFrame& fr, bool isBreak,
                                  std::size_t targetLoopIdx,
                                  llvm::BasicBlock* targetBlock,
                                  std::size_t targetScopeDepth) {
    for (auto& r : fr.labeledRoutes)
        if (r.isBreak == isBreak && r.targetLoopIdx == targetLoopIdx)
            return r.mode;
    int m = fr.nextLabeledMode++;
    fr.labeledRoutes.push_back(
        {m, isBreak, targetBlock, targetScopeDepth, targetLoopIdx});
    return m;
}

// §21.5 (v0.201, #2051): break/continue to the loop `offset` levels out.
bool Emitter::lowerLoopJump(int offset, bool isBreak) {
    // Guard: a malformed offset (out of range) would index past the stack; sema
    // rejects unresolved labels before codegen, so this should never fire, but
    // fall back to the innermost loop rather than read out of bounds.
    if (offset < 0 || static_cast<std::size_t>(offset) >= loopStack_.size())
        offset = 0;
    std::size_t targetIdx =
        loopStack_.size() - 1 - static_cast<std::size_t>(offset);
    const LoopTargets& tgt = loopStack_[targetIdx];
    llvm::BasicBlock* targetBlock = isBreak ? tgt.breakTarget : tgt.continueTarget;

    // A `finally` lies between the jump and the target loop iff the innermost
    // active finally was entered while the target loop (or an inner loop) was
    // already open — i.e. its loopDepthAtEntry exceeds the target loop's index.
    // (For offset 0 this is exactly the pre-v0.201 site check
    //  `loopStack_.size() <= finallyStack_.back().loopDepthAtEntry`.)
    if (!finallyStack_.empty() &&
        finallyStack_.back().loopDepthAtEntry > targetIdx) {
        FinallyFrame& fr = finallyStack_.back();
        // Dispose only the try-BODY scopes here (>= this finally's body scope);
        // the enclosing scopes down to the target loop's own scope depth are
        // disposed by the finally's dispatch AFTER the finally body runs
        // (reverse establishment — #1567/#1598/§31.2).
        emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
        int mode;
        if (offset == 0) {
            // Unlabeled / innermost target: the finally's captured innermost
            // loop (mode-2/mode-3 dispatch) IS the target — reuse the fixed,
            // battle-tested modes 2 (break) / 3 (continue).
            mode = isBreak ? 2 : 3;
        } else {
            // Labeled jump to an OUTER loop: a fixed mode would target the wrong
            // (innermost) loop, so mint a per-target exit mode carrying the true
            // target block and dispose-floor; the finally dispatch emits a case
            // for it and threads it through any further-outer crossed finally.
            mode = getOrAddLabeledRoute(fr, isBreak, targetIdx, targetBlock,
                                        tgt.scopeDepth);
        }
        builder_.CreateStore(builder_.getInt32(mode), fr.exitModSlot);
        builder_.CreateBr(fr.finNormalBB);
    } else {
        // No finally crossed: dispose every scope down to the target loop's own
        // scope depth and jump straight to its break/continue block.
        emitLoopDisposals(tgt.scopeDepth);
        builder_.CreateBr(targetBlock);
    }
    return true;
}

bool Emitter::lowerWhile(const ast::WhileStmt& ws) {
    auto* condBB = llvm::BasicBlock::Create(llvm_, "while.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "while.body", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "while.end", currentFn_);

    builder_.CreateBr(condBB);
    builder_.SetInsertPoint(condBB);
    llvm::Value* cond = ws.condition ? lowerExpr(*ws.condition) : nullptr;
    // fix #5137 (§7.11, v0.313): the §7.11 'no three-valued logic' gate at
    // while-conditions (src/sema/ExprTypecheck.cpp ~6201) admits a `bool?`
    // condition narrowed non-null by §7.4. Sema returns success and codegen
    // receives the condition as the declared `{i8 hasVal; i1 payload}` struct,
    // but `CreateCondBr` below needs an `i1` — extract the payload here. The
    // pre-extraction soundness precondition of unwrapNarrowedNullableOperand
    // (a sema gate that rejects the un-narrowed form) is satisfied by the
    // #5137 sema half in this same lane.
    if (cond && ws.condition.get())
        cond = unwrapNarrowedNullableOperand(cond, ws.condition.get());
    if (cond) builder_.CreateCondBr(cond, bodyBB, endBB);
    else builder_.CreateBr(endBB);

    builder_.SetInsertPoint(bodyBB);
    // CG13: the loop body is a disposal scope; break/continue unwind
    // down to this depth (the scope index the body will occupy).
    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    bool bodyTerminated = lowerScopedBlock(ws.body);
    loopStack_.pop_back();
    if (!bodyTerminated) builder_.CreateBr(condBB);  // back-edge

    builder_.SetInsertPoint(endBB);
    return false;  // control may always exit the loop
}


// §27.5 / #754: lower a for-loop body, pre-boxing the loop variable into a
// fresh per-iteration cell at body entry IF a closure in the body captures it.
//
// Background: `boxLocalSlot(forLoopVar=true)` allocates a fresh cell each
// iteration so a `()=>i` closure captures that iteration's value (§16.5 fresh
// binding). Previously this boxing was emitted lazily, at the capturing lambda
// site, and `lowerLambda` then rebound `locals_[&fs]` to the cell — so every
// later read of the loop variable in the body routes through the cell. When the
// lambda sits inside a conditionally-executed branch (a nested `if`), an
// iteration that skips that branch never allocates a fresh cell, so the
// cell-holder still points at a stale earlier iteration's cell; reads of the
// loop variable later in the body then observe the wrong value and loop control
// flow miscompiles (#754: a counted loop returned 1 instead of 21).
//
// Pre-boxing here, unconditionally at body entry, refreshes the cell on EVERY
// iteration regardless of branch outcomes. The capturing lambda's own
// `boxLocalSlot` call then finds the slot already boxed and reuses the
// body-entry cell (no double-box). The loop COUNTER stays on the original slot
// (the cond/inc blocks read and advance it), and body entry reads that slot to
// fill the fresh cell — so reassigning the loop variable in the body writes
// through the cell without disturbing iteration (capture-by-reference, §27.5).
//
// Iterator (state-machine) methods map the loop variable to a frame field and
// have their own resume/spill handling, so we never rebox there.
bool Emitter::lowerForBody(const ast::ForStmt& fs) {
    // #793 / §27.5: the fresh-per-iteration for-var capture cell (set below) when
    // the for-var is captured ONLY by non-escaping lambdas — eligible to free at
    // the end of each iteration's body.
    llvm::AllocaInst* forVarFreshCell = nullptr;
    if (!smStateStruct_) {
        auto it = locals_.find(&fs);
        if (it != locals_.end()) {
            llvm::AllocaInst* slot = it->second;
            if (slot && !cellVars_.count(slot) && forVarCapturedInBody(fs)) {
                llvm::AllocaInst* nb = nullptr;
                boxLocalSlot(slot, nb, /*forLoopVar=*/true);
                locals_[&fs] = nb;
                // #793: boxLocalSlot(forLoopVar=true) allocates a FRESH cell each
                // iteration, and those cells were never freed (a per-iteration
                // leak, O(iterations)). When collectCellFreeEligible proves the
                // for-var is captured ONLY by non-escaping lambdas (keyed by &fs —
                // it already classifies for-vars), each iteration's cell can be
                // freed at the end of that iteration's body, after the capturing
                // closure has been released. An ESCAPING capture is NOT in
                // cellFreeEligible_, so its cell is never freed here (it outlives
                // the loop — keeps leaking, the safe direction).
                if (currentFn_ == cellFreeEligibleFn_ &&
                    cellFreeEligible_.count(static_cast<const void*>(&fs)))
                    forVarFreshCell = nb;
            }
        }
    }

    // §29.2 / §28.3 v0.157 (#1329): per-iteration null-guarded disposal for an
    // owned iteration variable the ownership pass flagged as not always discharged.
    // The disposal scope is the OUTERMOST (pushed first here, popped last): it
    // wraps everything else including the capture-cell scope and the body's own
    // lowerScopedBlock scope, so LIFO order fires body disposals first, then this
    // one — correct for an object that was owned for the whole iteration. The scope
    // is registered BELOW the loopStack_ depth recorded by the caller (lowerOwnedSeqDrainFor
    // / lowerDictDrainFor / lowerAwaitFor push the loopStack_ entry BEFORE calling us,
    // and emitLoopDisposals unwinds all scopes ABOVE the recorded depth), so break
    // / continue / return via emitLoopDisposals fire this disposal too.
    //
    // dynamicDispose=true: null-guarded vtable Dispose + free. The slot may be
    // null if the body discharged the variable on this iteration (x.Dispose()
    // nulls the slot via explicitDisposeOwnedReceiverSlot; `move x` is patched to
    // null the slot when ownedIterVarAutoDropAtBodyEnd is set). free(null) is safe.
    //
    // Applies to BOTH sync drain-for loops (smStateStruct_==null) and async
    // await-for loops (smStateStruct_!=null). In the async case the iter var slot
    // is an entryAlloca that round-trips smAllocaToField_ on every await suspend
    // in the body, so at body-end the alloca always holds the current iteration's
    // value. boxedStorage() returns the alloca directly (not the frame field), which
    // is correct since dynamicDispose fires WITHIN the current Resume invocation
    // (no suspend between bodyBB and body-end).
    llvm::AllocaInst* iterVarSlot = nullptr;
    if (fs.ownedIterVarAutoDropAtBodyEnd) {
        auto sit = locals_.find(&fs);
        if (sit != locals_.end())
            iterVarSlot = sit->second;
    }

    if (iterVarSlot) {
        // §30.5 / §29.2 (#1888): a drained `OwningTask<T>`/`Task<T>` iteration
        // variable (`for var task in tasks.Drain() do results.Add(await task);
        // end`) is a TASK, not a class object — its frame has no Bezel vtable, so
        // the class-vtable `dynamicDispose` below (emitDisposeAndReclaim) would
        // read a bogus vtable slot and crash. A bound task's frame is never freed
        // by the `await` (freeTaskAfter is only set for an anonymous CallExpr
        // operand, LlvmBackend_Async.cpp lowerAwait); it is freed at scope/body-end
        // via `bz_task_fire_and_forget`, EXACTLY as a plain `var t := M(); await t;`
        // task local is (the by-type `d.task` registration in lowerStmt's LocalDecl
        // arm above).
        // Mirror that here for the iter var: dispose it via the task path
        // (fire-and-forget, idempotent on a spent/complete task), carrying the
        // per-T owned-result disposer for an `OwningTask<T:class>` so a drained-but-
        // never-awaited task also reclaims its produced result (a no-op once the
        // await has taken/nulled it). // per §30.5 / §29.2
        const sema::TypeRef* iterElemTy = nullptr;
        if (fs.iterable) {
            if (const sema::TypeRef* itTy = ctx_.exprType(*fs.iterable))
                if (!itTy->genericArgs.empty() && itTy->genericArgs[0])
                    iterElemTy = resolveTypeParamRef(itTy->genericArgs[0]);
        }
        bool iterVarIsTask = isTaskTypeName(iterElemTy);
        // #628 / stdlib §4.9 v0.135: a `for var entry in d.Drain() do` iter var
        // is a DrainedEntry<K,V> struct {K key; V* value}, NOT a class object —
        // its slot is a struct alloca, and reading the first 8 bytes as a class
        // vtable (the dynamicDispose path) would dispatch through whatever bytes
        // happen to be at field[0] (for string keys, a bz_string_t's data ptr —
        // bogus, crash). The DrainedEntry disposal path (LlvmBackend_Disposal.cpp
        // emitDisposals's `drainedEntry` arm) loads V* from field[1], disposes it
        // via emitDisposeAndReclaim, and (for string keys) releases the bz_string_t
        // at field[0]. The fields drainedEntryKeyType / drainedEntryValType /
        // drainedEntryKeyIsString / drainedEntryValDisposer carry the LLVM types
        // and the optional vtable-static disposer thunk.
        bool iterVarIsDrainedEntry = iterElemTy &&
            iterElemTy->kind == sema::TypeRef::Kind::Named &&
            !iterElemTy->declaration &&
            iterElemTy->importedName == "DrainedEntry" &&
            iterElemTy->genericArgs.size() >= 2 &&
            iterElemTy->genericArgs[0] && iterElemTy->genericArgs[1];
        // Push outermost scope: per-iteration iter-var disposal.
        disposeStack_.emplace_back();
        Disposal id{iterVarSlot, nullptr};
        if (iterVarIsTask) {
            id.task = true;
            if (iterElemTy && !iterElemTy->genericArgs.empty() &&
                iterElemTy->genericArgs[0]) {
                ElementPolicy pol =
                    elementPolicyFor(iterElemTy->genericArgs[0],
                                     /*owningFamily=*/true);
                if (pol.cat == ElementPolicy::Category::ClassOwned)
                    id.taskOwnedResultDisposer =
                        pol.disposerThunk;
            }
        } else if (iterVarIsDrainedEntry) {
            // DrainedEntry<K,V>: dispose V (the owned class at field[1]) and
            // release the key string at field[0] when K is string. The slot is
            // a struct alloca {K key; V* value} matching the lowerDictDrainFor
            // entry layout, so the same field GEPs work.
            const sema::TypeRef* kTy = iterElemTy->genericArgs[0];
            const sema::TypeRef* vTy = iterElemTy->genericArgs[1];
            id.drainedEntry = true;
            id.drainedEntryKeyIsString =
                kTy->kind == sema::TypeRef::Kind::Primitive &&
                kTy->primitive == sema::PrimitiveType::String;
            id.drainedEntryKeyType = lowerTypeRef(kTy);
            id.drainedEntryValType = lowerTypeRef(vTy);
            id.drainedEntryValDisposer =
                elementPolicyFor(vTy, /*owning=*/true).disposerThunk;
        } else {
            id.dynamicDispose = true;
        }
        disposeStack_.back().push_back(id);
    }

    bool terminated = false;
    if (!forVarFreshCell) {
        terminated = lowerScopedBlock(fs.body);
    } else {
        // #793: wrap the body in a scope holding the for-var cell free (inner
        // scope, fires before the iter-var outer scope by LIFO order). lowerScopedBlock
        // opens the body's own innermost scope.
        disposeStack_.emplace_back();
        Disposal d{forVarFreshCell, nullptr};
        d.boxedCellFree = true;
        disposeStack_.back().push_back(d);
        terminated = lowerScopedBlock(fs.body);
        if (!terminated) emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
    }

    if (iterVarSlot) {
        if (!terminated) emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
    }
    return terminated;
}


// §16.3 (v0.183, #1586): see declaration (LlvmBackend_impl.hpp) for the
// dense-index bijection this implements. `cur` is assumed to already be a
// valid (non-surrogate) char code point, as every value this is applied to
// either came from a char literal endpoint (surrogate literals are compile
// errors, §18.3/§18.8) or was itself produced by a prior call to this
// function.
llvm::Value* Emitter::emitCharRangeStep(llvm::Value* cur, llvm::Value* step) {
    llvm::Value* gapLo = builder_.getInt32(0xD800);
    llvm::Value* gapSize = builder_.getInt32(0x800);
    // Forward map: d = cur < 0xD800 ? cur : cur - 0x800.
    llvm::Value* curAboveGap = builder_.CreateICmpSGE(cur, gapLo, "crs.above");
    llvm::Value* curShifted =
        builder_.CreateSub(cur, gapSize, "crs.shifted");
    llvm::Value* d = builder_.CreateSelect(curAboveGap, curShifted, cur, "crs.d");
    // Step in the dense (gap-free) space — plain addition, ascending or
    // descending per step's sign.
    llvm::Value* d2 = builder_.CreateAdd(d, step, "crs.d2");
    // Inverse map: cp = d2 < 0xD800 ? d2 : d2 + 0x800.
    llvm::Value* d2AboveGap = builder_.CreateICmpSGE(d2, gapLo, "crs.d2above");
    llvm::Value* d2Shifted = builder_.CreateAdd(d2, gapSize, "crs.d2shifted");
    return builder_.CreateSelect(d2AboveGap, d2Shifted, d2, "crs.next");
}


bool Emitter::lowerRangeFor(const ast::ForStmt& fs) {
    auto* range = dynamic_cast<const ast::RangeExpr*>(fs.iterable.get());

    // fix #499 / §9.8 / §16.5: `for var i in r` where r is a Range-typed
    // variable (not an inline RangeExpr literal). Load the stored struct and
    // synthesise the same counted-loop that lowerRangeFor produces for a
    // literal range — using the struct's start/end/step/flags fields.
    if (!range && fs.iterable) {
        auto* iterTy = ctx_.exprType(*fs.iterable);
        if (!isRangeType(iterTy)) return false;  // not a Range — try other for-handlers
        // §16.3 (v0.183, #1586): a Range<char> steps across the UTF-16
        // surrogate gap instead of the plain arithmetic every other Range<T>
        // uses (see emitCharRangeStep).
        bool isCharRange = iterTy && !iterTy->genericArgs.empty() &&
            iterTy->genericArgs[0] &&
            iterTy->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
            iterTy->genericArgs[0]->primitive == sema::PrimitiveType::Char;

        llvm::Value* rv = lowerExpr(*fs.iterable);
        if (!rv) return false;

        llvm::Value* lo64 = builder_.CreateExtractValue(rv, {0u}, "rfv.lo");
        llvm::Value* hi64 = builder_.CreateExtractValue(rv, {1u}, "rfv.hi");
        llvm::Value* step64 = builder_.CreateExtractValue(rv, {2u}, "rfv.step");
        llvm::Value* flags8 = builder_.CreateExtractValue(rv, {3u}, "rfv.flg");
        llvm::Value* incl   = builder_.CreateTrunc(flags8, i1(), "rfv.incl");

        // Truncate to i32 for the loop variable (int Range). This matches the
        // behavior of the inline-literal path which uses lo->getType() = i32.
        llvm::Value* lo   = builder_.CreateTrunc(lo64, i32(), "rfv.lo32");
        llvm::Value* hi   = builder_.CreateTrunc(hi64, i32(), "rfv.hi32");
        llvm::Value* step = builder_.CreateTrunc(step64, i32(), "rfv.step32");

        // §16.4: the iteration direction is determined by the step's SIGN.
        // Unlike the inline-literal path below (where the step is a compile-time
        // constant), the step here is loaded from the Range struct and is
        // generally NOT a ConstantInt, so direction must be decided at RUNTIME.
        // A range whose bounds-direction disagrees with its step sign produces
        // an empty iteration — this falls out naturally from selecting the
        // step-sign-derived comparison against the (unchanged) bounds: e.g.
        // `1..5 step -1` takes the descending branch and `1 >= 5` is false on
        // entry, so the body never runs. (The prior code only ever set
        // `descending` for a constant step, so a Range variable always used the
        // ascending comparison — both miscompiling `5..1 step -1` into an empty
        // loop and turning `1..5 step -1` into a non-terminating descent.)
        llvm::Value* stepNeg =
            builder_.CreateICmpSLT(step, builder_.getInt32(0), "rfv.desc");

        // #1243 §21.4: use a hidden counter (rfvCounter) so that assignment to
        // the loop variable in the body does not alter the iteration count.
        // rfvVar is the body-visible slot; it is reloaded from rfvCounter at
        // the top of each iteration so each iteration starts with the correct
        // value regardless of what the body wrote to i.
        llvm::AllocaInst* rfvCounter = entryAlloca(i32());
        llvm::AllocaInst* rfvVar     = entryAlloca(i32());
        locals_[&fs] = rfvVar;
        builder_.CreateStore(lo, rfvCounter);

        auto* condBB = llvm::BasicBlock::Create(llvm_, "rfv.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rfv.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "rfv.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "rfv.end",  currentFn_);
        // §16.4 (v0.242, #3052): the pre-v0.242 `ArgumentException` guard that
        // stood here — the at-`GetEnumerator` half of the three-way zero-step
        // limbo — is DELETED with the rule that required it. The step is now
        // validated where the `Range<T>` value is CONSTRUCTED (the range
        // expression; see the RangeExpr-in-value-position lowering in
        // LlvmBackend_Exprs.cpp), and §16.4 states the consequence normatively:
        // "Range expressions are the sole producer of `Range<T>` values (the
        // record is compiler-intrinsic and declares no constructor, §16.3;
        // §17.7's closed-constructor-surface rule …), so no `Range<T>` value
        // with a zero step can exist." §16.3 relies on the same fact ("No
        // operation needs a zero-step case: a `Range<T>` value with a zero step
        // cannot exist, §16.4"). A step-0 range reaching this loop is therefore
        // unconstructible, and re-checking here would reintroduce exactly the
        // GetEnumerator-time behavior the revision removed.
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        llvm::Value* cur = builder_.CreateLoad(i32(), rfvCounter);
        // Direction-aware bound check: pick the ascending (cur <=/< hi) or
        // descending (cur >=/> hi) comparison by the runtime step sign, using
        // the incl flag from the struct for the </<= vs <=/>= choice.
        llvm::Value* ascLE  = builder_.CreateICmpSLE(cur, hi);
        llvm::Value* ascLT  = builder_.CreateICmpSLT(cur, hi);
        llvm::Value* asc    = builder_.CreateSelect(incl, ascLE, ascLT);
        llvm::Value* descGE = builder_.CreateICmpSGE(cur, hi);
        llvm::Value* descGT = builder_.CreateICmpSGT(cur, hi);
        llvm::Value* desc   = builder_.CreateSelect(incl, descGE, descGT);
        llvm::Value* cond   = builder_.CreateSelect(stepNeg, desc, asc, "rfv.cond");
        builder_.CreateCondBr(cond, bodyBB, endBB);

        builder_.SetInsertPoint(bodyBB);
        // #1243: copy hidden counter into the per-iteration loop variable so
        // the body starts each iteration with the right value of i.
        builder_.CreateStore(
            builder_.CreateLoad(i32(), rfvCounter, "rfv.iv"), rfvVar);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        bool term = lowerForBody(fs);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);

        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(i32(), rfvCounter);
        llvm::Value* nextV = isCharRange ? emitCharRangeStep(v, step)
                                          : builder_.CreateAdd(v, step);
        builder_.CreateStore(nextV, rfvCounter);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        return true;
    }

    if (!range || !range->lo || !range->hi) return false;
    llvm::Value* lo = lowerExpr(*range->lo);
    llvm::Value* hi = lowerExpr(*range->hi);
    if (!lo || !hi) return false;

    // §16.3 (v0.183, #1586): the literal-range counted loop below must match
    // emitCharRangeStep's surrogate-gap-skipping increment for Range<char>,
    // same as the Range-variable path above.
    auto* litIterTy = ctx_.exprType(*fs.iterable);
    bool isCharRange = litIterTy && !litIterTy->genericArgs.empty() &&
        litIterTy->genericArgs[0] &&
        litIterTy->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
        litIterTy->genericArgs[0]->primitive == sema::PrimitiveType::Char;

    // §16.4: resolve the step (default = 1; negative steps descend).
    llvm::Value* stepVal = llvm::ConstantInt::get(lo->getType(), 1);
    bool descending = false;
    if (range->step) {
        llvm::Value* s = lowerExpr(*range->step);
        if (!s) return false;
        auto* cs = llvm::dyn_cast<llvm::ConstantInt>(s);
        if (cs && cs->isZero()) return false;  // zero constant step: empty, defer
        if (!cs) {
            // §16.4 (sweep #8): a RUNTIME-computed step (`for var i in 0..10 step s`).
            // The constant-step path below picks the loop direction at compile
            // time; a non-constant step must pick it at runtime by the step's
            // sign. Emit a direction-aware loop, mirroring the Range-VARIABLE path
            // above (previously this case bailed to the "not implemented" catch-all).
            if (s->getType() != lo->getType())
                s = builder_.CreateSExtOrTrunc(s, lo->getType());
            // §16.4 (v0.242, #3052): the range EXPRESSION is right here — the
            // inline `lo..hi step s` of a `for` header — so this IS the
            // construction site, and a computed zero step raises
            // `OverflowException` "at the expression" (the fault type of the
            // revision's joint checked-posture ruling; through v0.241 it was
            // `ArgumentException` deferred to `GetEnumerator`). The
            // constant-step-0 case never reaches here (sema rejects it, and
            // the `cs && cs->isZero()` early return above declines anyway).
            emitGuardThrow(builder_.CreateICmpEQ(
                               s, llvm::ConstantInt::get(lo->getType(), 0), "rf.step0"),
                           "OverflowException");
            llvm::Value* stepNeg = builder_.CreateICmpSLT(
                s, llvm::ConstantInt::get(lo->getType(), 0), "rf.desc");
            // #1243 §21.4: hidden counter + body-visible var (per rfv path above).
            llvm::AllocaInst* rfCounter = entryAlloca(lo->getType());
            llvm::AllocaInst* rfVar     = entryAlloca(lo->getType());
            locals_[&fs] = rfVar;
            builder_.CreateStore(lo, rfCounter);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "rf.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rf.body", currentFn_);
            auto* incBB  = llvm::BasicBlock::Create(llvm_, "rf.inc",  currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "rf.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* cur = builder_.CreateLoad(lo->getType(), rfCounter);
            llvm::Value* asc = range->inclusive
                ? builder_.CreateICmpSLE(cur, hi)
                : builder_.CreateICmpSLT(cur, hi);
            llvm::Value* desc = range->inclusive
                ? builder_.CreateICmpSGE(cur, hi)
                : builder_.CreateICmpSGT(cur, hi);
            llvm::Value* cond =
                builder_.CreateSelect(stepNeg, desc, asc, "rf.cnd");
            builder_.CreateCondBr(cond, bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            // #1243: copy hidden counter into the per-iteration loop variable.
            builder_.CreateStore(
                builder_.CreateLoad(lo->getType(), rfCounter, "rf.iv"), rfVar);
            loopStack_.push_back({incBB, endBB, disposeStack_.size()});
            bool term = lowerForBody(fs);
            loopStack_.pop_back();
            if (!term) builder_.CreateBr(incBB);
            builder_.SetInsertPoint(incBB);
            llvm::Value* v = builder_.CreateLoad(lo->getType(), rfCounter);
            llvm::Value* nextV =
                isCharRange ? emitCharRangeStep(v, s) : builder_.CreateAdd(v, s);
            builder_.CreateStore(nextV, rfCounter);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            return true;
        }
        stepVal = cs;
        descending = cs->getValue().isNegative();
    }

    llvm::AllocaInst* slot = entryAlloca(lo->getType());
    // §29.3: in an iterator method, map the loop variable to a struct field.
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(&fs);
        if (fit != smLocalFields_.end())
            smAllocaToField_[slot] = fit->second;
    }
    builder_.CreateStore(lo, slot);
    // Anchor on the ForStmt node — sema gave `lsym->decl = fs` so that
    // identifier `i` in the body resolves through `locals_[fs]`.
    locals_[&fs] = slot;

    // §29.3: in an iterator method, re-evaluate `hi` inside `condBB`
    // rather than pre-computing it into an alloca. Pre-computed allocas
    // are uninitialized in resumed states (state≥1), causing the upper
    // bound to be garbage. Re-evaluating in condBB is correct because
    // all params/locals it depends on ARE restored from the state struct.
    if (smStateStruct_) {
        auto* condBB = llvm::BasicBlock::Create(llvm_, "for.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "for.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "for.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "for.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* cur = builder_.CreateLoad(slot->getAllocatedType(), slot);
        // Re-evaluate hi here so it picks up any saved/restored values.
        llvm::Value* hiVal = lowerExpr(*range->hi);
        if (!hiVal) hiVal = hi;  // fallback to pre-computed value
        llvm::Value* cond;
        if (descending) {
            cond = range->inclusive
                       ? builder_.CreateICmpSGE(cur, hiVal)
                       : builder_.CreateICmpSGT(cur, hiVal);
        } else {
            cond = range->inclusive
                       ? builder_.CreateICmpSLE(cur, hiVal)
                       : builder_.CreateICmpSLT(cur, hiVal);
        }
        builder_.CreateCondBr(cond, bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        bool term = lowerForBody(fs);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);
        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
        llvm::Value* next = isCharRange ? emitCharRangeStep(v, stepVal)
                                         : builder_.CreateAdd(v, stepVal);
        builder_.CreateStore(next, slot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return true;
    }

    // #1243 §21.4: introduce a separate body-visible loop variable (varSlot)
    // so that assignment to i in the body does not alter the hidden counter
    // (slot).  The SM path above already returned; that path keeps using
    // 'slot' directly because it must survive yield via smAllocaToField_ and
    // adding a second frame field would require smLocalFields_ to reserve an
    // extra entry per range ForStmt (deferred to a follow-up).
    llvm::AllocaInst* varSlot = entryAlloca(lo->getType());
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "for.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "for.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "for.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "for.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: direction-aware bound check.
    // Ascending: i <= hi (inclusive `..`) or i < hi (half-open `..<`).
    // Descending: i >= hi (inclusive `..`) or i > hi (half-open `..<`).
    builder_.SetInsertPoint(condBB);
    llvm::Value* cur = builder_.CreateLoad(slot->getAllocatedType(), slot);
    llvm::Value* cond;
    if (descending) {
        cond = range->inclusive
                   ? builder_.CreateICmpSGE(cur, hi)
                   : builder_.CreateICmpSGT(cur, hi);
    } else {
        cond = range->inclusive
                   ? builder_.CreateICmpSLE(cur, hi)
                   : builder_.CreateICmpSLT(cur, hi);
    }
    builder_.CreateCondBr(cond, bodyBB, endBB);

    // body: `continue` targets incBB, `break` targets endBB.
    builder_.SetInsertPoint(bodyBB);
    // #1243: copy hidden counter into the per-iteration loop variable so the
    // body sees the correct value of i and its mutations stay local.
    builder_.CreateStore(
        builder_.CreateLoad(slot->getAllocatedType(), slot, "for.iv"), varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: counter := counter + step (body mutations to varSlot are ignored).
    // §16.3 (v0.183, #1586): for a Range<char>, step across the surrogate gap
    // instead of plain addition (emitCharRangeStep).
    builder_.SetInsertPoint(incBB);
    llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
    llvm::Value* next = isCharRange ? emitCharRangeStep(v, stepVal)
                                     : builder_.CreateAdd(v, stepVal);
    builder_.CreateStore(next, slot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// stdlib §4.13 / §29.2 (#728): `for var x in <LinkedList<T>>` (and
// OwningLinkedList<T>). A linked list is NOT array-backed, so lowerArrayFor's
// isCollectionTypeName guard excludes it and it would otherwise hit the
// not-yet-implemented error. Walk the node chain with the runtime cursor
// (bz_llist_first_node / bz_llist_node_value_ptr / bz_llist_node_next) already
// used by the First/Last/Find lowering. Mirrors the span/array loop structure
// (cond/body/inc/end + loopStack_ for break/continue).
bool Emitter::lowerLinkedListFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!isLinkedListTypeName(iterTy)) return false;
    llvm::Value* listObj = lowerExpr(*fs.iterable);
    if (!listObj) return false;
    llvm::Type* elemTy = i32();
    if (iterTy && !iterTy->genericArgs.empty() && iterTy->genericArgs[0])
        elemTy = lowerTypeRef(iterTy->genericArgs[0]);
    auto* firstFn = declareC("bz_llist_first_node", ptrTy(), {ptrTy()});
    auto* valFn = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
    auto* nextFn = declareC("bz_llist_node_next", ptrTy(), {ptrTy()});
    // §29.12 / issue #1232: capture modification epoch before first node fetch.
    auto* getModLlistFn = declareC("bz_llist_get_mod", i64(), {ptrTy()});
    llvm::Value* llEpochVal = builder_.CreateCall(getModLlistFn, {listObj}, "ll.epoch");
    auto* llEpochSlot = entryAlloca(i64());
    builder_.CreateStore(llEpochVal, llEpochSlot);
    auto* nodeSlot = entryAlloca(ptrTy());
    builder_.CreateStore(builder_.CreateCall(firstFn, {listObj}), nodeSlot);
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;
    // #747 R42 + §29.12/#1232: spill node, loop-var, and epoch into the async
    // frame so they survive a yield/await suspend in the body.
    registerForLoopSpillSlots(fs, {nodeSlot, varSlot, llEpochSlot});
    auto* condBB = llvm::BasicBlock::Create(llvm_, "ll.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ll.body", currentFn_);
    auto* incBB = llvm::BasicBlock::Create(llvm_, "ll.inc", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "ll.end", currentFn_);
    builder_.CreateBr(condBB);
    builder_.SetInsertPoint(condBB);
    // §29.12 / issue #1232: check epoch on every MoveNext — mismatch → InvalidOperationException.
    {
        llvm::Value* curMod = builder_.CreateCall(getModLlistFn, {listObj}, "ll.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), llEpochSlot, "ll.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "ll.modchk"),
                       "InvalidOperationException");
    }
    llvm::Value* node = builder_.CreateLoad(ptrTy(), nodeSlot);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(node, llvm::ConstantPointerNull::get(ptrTy())),
        bodyBB, endBB);
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* nodeB = builder_.CreateLoad(ptrTy(), nodeSlot);
    llvm::Value* vp = builder_.CreateCall(valFn, {nodeB});
    builder_.CreateStore(builder_.CreateLoad(elemTy, vp), varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);
    builder_.SetInsertPoint(incBB);
    llvm::Value* nodeC = builder_.CreateLoad(ptrTy(), nodeSlot);
    builder_.CreateStore(builder_.CreateCall(nextFn, {nodeC}), nodeSlot);
    builder_.CreateBr(condBB);
    builder_.SetInsertPoint(endBB);
    return true;
}

void Emitter::registerForLoopSpillSlots(
        const ast::ForStmt& fs,
        std::initializer_list<llvm::AllocaInst*> slots) {
    // §29.3 / §30.7 (#747 R42) — see the declaration comment. All three suspend
    // kinds now use the side spill area + the fixupSuspendDominance pass that
    // consumes asyncHandlerSpillSlots_: Await / AsyncYield (async resume) and
    // Yield (sync MoveNext — emitIteratorMethod sets asyncSpillPtrField_ and runs
    // the fixup when any slot is registered).
    if (!smStateStruct_) return;
    if (smKind_ != SuspendKind::Await && smKind_ != SuspendKind::AsyncYield &&
        smKind_ != SuspendKind::Yield)
        return;
    if (!containsYield(fs.body) && !containsAwait(fs.body)) return;
    for (llvm::AllocaInst* s : slots)
        if (s) asyncHandlerSpillSlots_.push_back(s);
}

bool Emitter::lowerArrayFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    // Handle collection literal `for var x in [e1, e2, ...] do`:
    // Lower each element into a temp alloca and iterate.
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
            fs.iterable.get())) {
        if (!cl->isDict && !cl->elements.empty()) {
            // Determine element type from first element.
            llvm::Value* first = lowerExpr(*cl->elements[0]);
            if (!first) return false;
            llvm::Type* elemTy = first->getType();
            std::size_t N = cl->elements.size();
            auto* at = llvm::ArrayType::get(elemTy, N);
            auto* tmp = entryAlloca(at);
            // Fill the temp array.
            builder_.CreateStore(first,
                builder_.CreateGEP(at, tmp, {builder_.getInt32(0), builder_.getInt32(0)}));
            for (std::size_t i = 1; i < N; ++i) {
                llvm::Value* ev = lowerExpr(*cl->elements[i]);
                if (!ev) {
                    // #1386: ICE — lowerExpr returned null for a for-in
                    // collection literal element.
                    ctx_.diagnostics().report(
                        Severity::Error,
                        cl->elements[i]->span.begin,
                        "internal: lowerExpr returned null for for-in "
                        "collection literal element — #1386");
                    continue;
                }
                builder_.CreateStore(ev,
                    builder_.CreateGEP(at, tmp, {builder_.getInt32(0), builder_.getInt32(i)}));
            }
            // Now iterate as a normal array.
            uint64_t n = N;
            auto* idxSlot2 = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), idxSlot2);
            auto* varSlot2 = entryAlloca(elemTy);
            locals_[&fs] = varSlot2;
            auto* condBB2 = llvm::BasicBlock::Create(llvm_, "clit.cond", currentFn_);
            auto* bodyBB2 = llvm::BasicBlock::Create(llvm_, "clit.body", currentFn_);
            auto* endBB2 = llvm::BasicBlock::Create(llvm_, "clit.end", currentFn_);
            builder_.CreateBr(condBB2);
            builder_.SetInsertPoint(condBB2);
            llvm::Value* idx2 = builder_.CreateLoad(i32(), idxSlot2);
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx2, builder_.getInt32((uint32_t)n)),
                bodyBB2, endBB2);
            builder_.SetInsertPoint(bodyBB2);
            llvm::Value* elem2 = builder_.CreateLoad(elemTy,
                builder_.CreateGEP(at, tmp,
                    {builder_.getInt32(0), idx2}));
            builder_.CreateStore(elem2, varSlot2);
            lowerForBody(fs);
            llvm::Value* inc2 = builder_.CreateAdd(
                builder_.CreateLoad(i32(), idxSlot2), builder_.getInt32(1));
            builder_.CreateStore(inc2, idxSlot2);
            builder_.CreateBr(condBB2);
            builder_.SetInsertPoint(endBB2);
            return true;
        }
    }
    // §4.15 (#549): for var x in span — direct indexed loop over ArraySpan<T>
    // or ReadOnlyArraySpan<T> (per spec §29.2, desugared as indexed loop over
    // span.Start .. span.Start+span.Length with bz_array_ptr_at on Source).
    {
        auto* iterTy = ctx_.exprType(*fs.iterable);
        if (isSpanLikeTypeName(iterTy) || isIReadOnlySpanType(iterTy)) {
            llvm::Value* spanVal = nullptr;
            if (isIReadOnlySpanType(iterTy)) {
                // §29.2 / §4.16 (#1262): an IReadOnlySpan<T> value is a box
                // { vtable, rc, bz.arrayspan } with the span struct at byte
                // offset 16 (#970). Unbox it and iterate it with the SAME direct
                // indexed loop as a concrete span (spec §29.2 mandates the
                // indexed-loop desugaring). The unboxed `src` field is the same
                // bz_array_t* the concrete path uses, so bz_array_ptr_at applies.
                llvm::Value* boxPtr = lowerExpr(*fs.iterable);
                if (!boxPtr) return false;
                llvm::Value* spanGep = builder_.CreateGEP(
                    llvm::Type::getInt8Ty(llvm_), boxPtr,
                    builder_.getInt64(16), "irs.spanp");
                spanVal = builder_.CreateLoad(arraySpanStructType(), spanGep,
                                              "irs.span");
            } else {
                spanVal = lowerExpr(*fs.iterable);
                if (!spanVal) return false;
            }
            auto* spanSt = arraySpanStructType();
            auto* spanSlot = entryAlloca(spanSt);
            builder_.CreateStore(spanVal, spanSlot);
            llvm::Value* src  = builder_.CreateLoad(ptrTy(),
                builder_.CreateStructGEP(spanSt, spanSlot, 0), "sp.src");
            llvm::Value* base = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, spanSlot, 1), "sp.st");
            llvm::Value* slen = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, spanSlot, 2), "sp.len");
            llvm::Type* elemTy = i32();
            int64_t esz = 8;
            if (iterTy && !iterTy->genericArgs.empty() && iterTy->genericArgs[0]) {
                if (auto* et = lowerTypeRef(iterTy->genericArgs[0])) elemTy = et;
                esz = collectionElemSize(iterTy);
            }
            auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            auto* idxSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), idxSlot);
            auto* varSlot = entryAlloca(elemTy);
            locals_[&fs] = varSlot;
            auto* condBB = llvm::BasicBlock::Create(llvm_, "sp.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sp.body", currentFn_);
            auto* incBB  = llvm::BasicBlock::Create(llvm_, "sp.inc",  currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "sp.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot);
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx32, slen), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* idx32b = builder_.CreateLoad(i32(), idxSlot);
            llvm::Value* absIdx = builder_.CreateAdd(
                builder_.CreateSExt(base, i64()),
                builder_.CreateSExt(idx32b, i64()), "sp.ai");
            llvm::Value* ep = builder_.CreateCall(ptrAtFn,
                {src, absIdx, builder_.getInt64(esz)});
            llvm::Value* elem = builder_.CreateLoad(elemTy, ep);
            builder_.CreateStore(elem, varSlot);
            loopStack_.push_back({incBB, endBB, disposeStack_.size()});
            bool termSp = lowerForBody(fs);
            loopStack_.pop_back();
            if (!termSp) builder_.CreateBr(incBB);
            builder_.SetInsertPoint(incBB);
            llvm::Value* idx32c = builder_.CreateLoad(i32(), idxSlot);
            builder_.CreateStore(
                builder_.CreateAdd(idx32c, builder_.getInt32(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            return true;
        }
    }
    // §3: for var x in array (runtime Array<T> object).
    {
        auto* iterTy = ctx_.exprType(*fs.iterable);
        // fix #2553 (stdlib §4.18, v0.236): SortedSet<T> is in
        // isCollectionTypeName's set (the registry comment notes it routes
        // through this gate alongside Set/OwningSet), but it's NOT array-backed
        // — its count is in the same struct slot but `data` is a hash-bucket
        // array, so this loop's bz_array_ptr_at readback would walk the bucket
        // array as element bytes and yield garbage. SortedSet is supposed to
        // be handled by lowerDictBackedSetFor (above), which only covers
        // SORTABLE element types; for non-sortable (string, decimal-as-record,
        // user choice, etc.) it now defers to lowerIEnumerableFor's SortedSet
        // helper (#2553), which routes through wrapSortedSetAsIEnumerable for
        // the correct sorted snapshot. Belt-and-suspenders: if the dispatch
        // above falls through for any reason (shouldn't, after #2553), refuse
        // the array-backed iteration here and return false so the generic
        // for-handlers take over instead of miscompiling.
        if (iterTy && iterTy->astNode && iterTy->astNode->name &&
            !iterTy->astNode->name->parts.empty() &&
            iterTy->headName() == "SortedSet") {
            return false;
        }
        if (isCollectionTypeName(iterTy)) {
            llvm::Value* arrObj = lowerExpr(*fs.iterable);
            if (!arrObj) return false;
            // Determine element type from generic arg.
            llvm::Type* elemTy = i32();
            int64_t esz = 8;
            if (iterTy && !iterTy->genericArgs.empty() && iterTy->genericArgs[0]) {
                elemTy = lowerTypeRef(iterTy->genericArgs[0]);
                esz = collectionElemSize(iterTy);
            }
            // #2472 (stdlib §4.11 "Iteration order" v0.195; language spec
            // §29.1's protocol / §29.2's desugaring). Every array-backed
            // collection shares this one direct-indexed-loop lowering, an
            // elision of the GetEnumerator()/MoveNext()/Current desugaring
            // sanctioned by language spec §29.2's "Compiler optimization"
            // clause ONLY when "the observable behavior is identical to the
            // desugared form" — normatively true for a forward scan (index 0
            // upward) EXCEPT for `Stack<T>`/`OwningStack<T>`, whose stdlib
            // §4.11 contract is "a stack top-to-bottom (pop order)", the
            // REVERSE of physical storage order. Physical index 0 is the
            // BOTTOM of the stack (first pushed) and index Count-1 is the TOP
            // (most recently pushed, the same index `Pop()` itself reads —
            // LlvmBackend_Calls.cpp ~4566-4588's `m == "Pop"` case) — a
            // forward scan would silently visit bottom-to-top, the exact
            // reverse of what §4.11 requires, with no diagnostic (confirmed
            // live before this fix: `Push(1);Push(2);Push(3); for x in s ...`
            // visited 1,2,3, not the required 3,2,1). `Queue<T>`/
            // `OwningQueue<T>`'s required order (front-to-back / dequeue
            // order) coincides with physical/insertion order (front = index
            // 0, the same index `Dequeue()` reads) so it needs no special
            // case; neither does any other member of this shared dispatch
            // (`Array`, `Set`, `SortedSet`, `OwningBuffer`), whose stdlib
            // sections state no take-order contract distinct from storage
            // order. `OwningStack<T>`'s default (borrowing)
            // `IReadOnlyCollection<T>` conformance "iterate[s] identically"
            // per §4.11, so it shares the reversal; `Drain()`'s SEPARATE
            // `IOwningEnumerable<T>` path (an interface-typed source, so this
            // `isCollectionTypeName` dispatch never matches it) is untouched
            // by this fix.
            bool isStackFamily = iterTy && iterTy->astNode &&
                iterTy->astNode->name && !iterTy->astNode->name->parts.empty() &&
                (iterTy->headName() == "Stack" ||
                 iterTy->headName() == "OwningStack");
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            // §29.12 / #1232: capture mod epoch before the loop starts.
            auto* getModFn = declareC("bz_array_get_mod", i64(), {ptrTy()});
            llvm::Value* epochVal = builder_.CreateCall(getModFn, {arrObj}, "arr.epoch");
            auto* epochSlot = entryAlloca(i64());
            builder_.CreateStore(epochVal, epochSlot);
            llvm::Value* len = builder_.CreateCall(lenFn, {arrObj});
            auto* idxSlotR = entryAlloca(i64());
            // Forward scan starts at 0 (unchanged); the Stack reversal starts
            // at `len - 1` (top) — for an empty stack this is -1, which the
            // `>= 0` loop condition below immediately fails, correctly
            // running zero iterations, the same as the forward `0 < len`
            // check does for an empty forward-scanned collection.
            builder_.CreateStore(
                isStackFamily
                    ? builder_.CreateSub(len, builder_.getInt64(1))
                    : builder_.getInt64(0),
                idxSlotR);
            auto* varSlotR = entryAlloca(elemTy);
            locals_[&fs] = varSlotR;
            // #747 R42: spill the index + loop-var + epoch into the async frame
            // so they survive a yield/await suspend in the body (else garbage on
            // resume). epochSlot must also be spilled: if a resume reads a stale
            // epoch, it would spuriously throw InvalidOperationException (§29.12).
            registerForLoopSpillSlots(fs, {idxSlotR, varSlotR, epochSlot});
            auto* condBBR = llvm::BasicBlock::Create(llvm_, "rt.cond", currentFn_);
            auto* bodyBBR = llvm::BasicBlock::Create(llvm_, "rt.body", currentFn_);
            auto* incBBR  = llvm::BasicBlock::Create(llvm_, "rt.inc",  currentFn_);
            auto* endBBR  = llvm::BasicBlock::Create(llvm_, "rt.end",  currentFn_);
            builder_.CreateBr(condBBR);
            builder_.SetInsertPoint(condBBR);
            // §29.12 / #1232: check for concurrent modification on every MoveNext.
            // If the collection was mutated since the loop started, throw
            // InvalidOperationException before reading the next element.
            {
                llvm::Value* curMod = builder_.CreateCall(getModFn, {arrObj}, "arr.curmod");
                llvm::Value* epV    = builder_.CreateLoad(i64(), epochSlot, "arr.epv");
                emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "arr.modchk"),
                               "InvalidOperationException");
                // After emitGuardThrow the insert point is the continuation block.
            }
            llvm::Value* idxR = builder_.CreateLoad(i64(), idxSlotR);
            llvm::Value* loopCond = isStackFamily
                ? builder_.CreateICmpSGE(idxR, builder_.getInt64(0))
                : builder_.CreateICmpSLT(idxR, len);
            builder_.CreateCondBr(loopCond, bodyBBR, endBBR);
            builder_.SetInsertPoint(bodyBBR);
            llvm::Value* idxR2 = builder_.CreateLoad(i64(), idxSlotR);
            llvm::Value* ep = builder_.CreateCall(ptrAtFn,
                {arrObj, idxR2, builder_.getInt64(esz)});
            llvm::Value* elem = builder_.CreateLoad(elemTy, ep);
            builder_.CreateStore(elem, varSlotR);
            // The loop's continue-target is incBBR; the body falls through to
            // it unless it already terminated (e.g. a trailing return/throw).
            // Emit the increment INTO incBBR (not the body block) so a
            // `continue` that branches to incBBR lands on a real, terminated
            // block — emitting the increment in bodyBBR left incBBR without a
            // terminator (malformed IR), mirroring the far.inc path below.
            loopStack_.push_back({incBBR, endBBR, disposeStack_.size()});
            bool termR = lowerForBody(fs);
            loopStack_.pop_back();
            if (!termR) builder_.CreateBr(incBBR);
            builder_.SetInsertPoint(incBBR);
            llvm::Value* idxR3 = builder_.CreateLoad(i64(), idxSlotR);
            // #2472: the Stack family steps the cursor DOWN (top-to-bottom);
            // every other array-backed collection steps it up, unchanged.
            builder_.CreateStore(
                isStackFamily
                    ? builder_.CreateSub(idxR3, builder_.getInt64(1))
                    : builder_.CreateAdd(idxR3, builder_.getInt64(1)),
                idxSlotR);
            builder_.CreateBr(condBBR);
            builder_.SetInsertPoint(endBBR);
            // #1055: a fresh `owned` Array materializer iterated directly as the
            // for-in source (`for var p in s.Split(',') do … end`) is owned by
            // nothing and leaks. Reclaim it here at the loop exit, which both
            // normal completion and `break` reach (a `return` inside the body
            // diverges before this point — the source leaks, the safe direction).
            // No-op unless the iterable is exactly a fresh ownerless array
            // materializer (its narrow guards) — a bound / `move`d / aliased
            // iterable is never matched, so no UAF / double-free; the iteration
            // variable is a borrow, so the elements are reclaimed exactly once.
            freeUnboundOwnedBorrowingArrayTemp(fs.iterable.get(), arrObj, iterTy);
            return true;
        }
    }
    // Try the lvalue path first (local/field/parameter — no spill needed).
    llvm::Type* arrSlotTy = nullptr;
    llvm::Value* arrAddr = lvalueAddr(*fs.iterable, arrSlotTy);
    // If the iterable is an expression (e.g. a method call returning [N]T),
    // evaluate it and spill the array value to a temp alloca so we have an
    // address to GEP through.
    if (!arrAddr || !arrSlotTy) {
        // Check exprType to see if the iterable is an InlineArray.
        auto* tt = ctx_.exprType(*fs.iterable);
        if (!tt || tt->kind != sema::TypeRef::Kind::InlineArray) return false;
        llvm::Value* v = lowerExpr(*fs.iterable);
        if (!v) {
            // #1386: ICE — lowerExpr returned null for a confirmed inline-array
            // for-in iterable; the expression kind is not yet handled by codegen.
            ctx_.diagnostics().report(
                Severity::Error, fs.iterable->span.begin,
                "internal: lowerExpr returned null for inline-array for-in "
                "iterable — #1386");
            return false;
        }
        auto* at = llvm::dyn_cast<llvm::ArrayType>(v->getType());
        if (!at) return false;
        llvm::AllocaInst* tmp = entryAlloca(at);
        builder_.CreateStore(v, tmp);
        arrAddr = tmp;
        arrSlotTy = at;
    }
    auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy);
    if (!arrTy) return false;
    uint64_t N = arrTy->getNumElements();
    if (N == 0) return true;  // empty array — no iterations
    llvm::Type* elemTy = arrTy->getElementType();

    // Index counter (i32).
    auto* idxSlot = entryAlloca(i32());
    builder_.CreateStore(builder_.getInt32(0), idxSlot);

    // Element value slot — what the body sees as `x`.
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "far.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "far.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "far.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "far.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: i < N.
    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot);
    llvm::Value* cond = builder_.CreateICmpSLT(
        idx, builder_.getInt32(static_cast<uint32_t>(N)));
    builder_.CreateCondBr(cond, bodyBB, endBB);

    // body: load a[i] into varSlot so the body sees x = a[i].
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* cur = builder_.CreateLoad(i32(), idxSlot);
    llvm::Value* elemAddr = builder_.CreateGEP(
        arrTy, arrAddr, {builder_.getInt64(0), cur});
    llvm::Value* elem = builder_.CreateLoad(elemTy, elemAddr);
    builder_.CreateStore(elem, varSlot);

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: i := i + 1.
    builder_.SetInsertPoint(incBB);
    llvm::Value* v = builder_.CreateLoad(i32(), idxSlot);
    builder_.CreateStore(
        builder_.CreateAdd(v, builder_.getInt32(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// issue #370 / §4.8 / §4.10: emit a counted loop over a snapshot of a generic
// hash table's keys. bz_dict_keys_gen returns a malloc'd array of `n` pointers,
// each a malloc'd copy of one key's raw bytes; we load each key with the
// element type's LLVM type and bind it to the loop variable, then free the copy
// and the array. The snapshot decouples iteration from the live table (a body
// that mutates the set/dict does not invalidate the loop), matching the
// existing bz_dict_keys_str-based behavior for string keys.
bool Emitter::emitGenericKeyTableFor(const ast::ForStmt& fs,
                                     llvm::Value* tableObj,
                                     const sema::TypeRef* elemTyRef,
                                     bool sorted) {
    if (!tableObj || !elemTyRef) return false;
    // This helper also lowers dict-backed Set iteration. Do not admit its
    // cross-suspend spill registration for that non-Keys caller (#3602).
    bool registerSpill = false;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get()))
        registerSpill = ma->member == "Keys";
    llvm::Type* elemTy = lowerTypeRef(elemTyRef);
    if (!elemTy) return false;

    // §29.12 / #1232: capture the dict/set's mod epoch before taking the snapshot.
    auto* getModDictFn = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* dictEpochVal = builder_.CreateCall(getModDictFn, {tableObj}, "dk.epoch");
    auto* dictEpochSlot = entryAlloca(i64());
    builder_.CreateStore(dictEpochVal, dictEpochSlot);

    auto* keysOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());
    auto* keysFn = declareC("bz_dict_keys_gen", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(keysFn, {tableObj, keysOutSlot, cntSlot});
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);  // void**
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);

    // fix #1426 / stdlib §4.18: for SortedSet, sort the snapshot in ascending
    // key order before iterating. O(n log n) — hash-backing limitation; real
    // ordered rep rides #545.  buildSortedKeyComparator is null for unsupported
    // key types (record/decimal) — leave unsorted in that case (correctness note
    // in the commit message; follow-up needed for those types).
    if (sorted) {
        llvm::Function* cmpFn = buildSortedKeyComparator(elemTyRef);
        if (cmpFn) {
            auto* sortFn = declareC("bz_sort_keys_gen", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(sortFn, {keysArr, cnt, cmpFn});
        }
        // If cmpFn is null, fall through unsorted (unsupported elem type).
    }

    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;
    if (registerSpill)
        registerForLoopSpillSlots(fs, {dictEpochSlot, keysOutSlot, cntSlot,
                                       idxSlot, varSlot});

    auto* condBB = llvm::BasicBlock::Create(llvm_, "gk.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "gk.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "gk.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "gk.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    // §29.12 / #1232: check for concurrent modification on every MoveNext.
    {
        llvm::Value* curMod = builder_.CreateCall(getModDictFn, {tableObj}, "dk.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), dictEpochSlot, "dk.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "dk.modchk"),
                       "InvalidOperationException");
    }
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
    // keysArr[idxB] is a void* to the key bytes; load the key with elemTy.
    llvm::Value* keyPtrPtr = builder_.CreateGEP(ptrTy(), keysArr, idxB);
    llvm::Value* keyPtr    = builder_.CreateLoad(ptrTy(), keyPtrPtr);
    llvm::Value* keyVal    = builder_.CreateLoad(elemTy, keyPtr);
    builder_.CreateStore(keyVal, varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
    // Free this iteration's key-bytes copy before advancing.
    llvm::Value* keyPtrPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
    llvm::Value* keyPtr2    = builder_.CreateLoad(ptrTy(), keyPtrPtr2);
    builder_.CreateCall(freeFn, {keyPtr2});
    builder_.CreateStore(builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    builder_.CreateCall(freeFn, {keysArr});  // free the snapshot array itself
    return true;
}


// issue #370 / §4.10: `for var x in <set>` over a dict-backed Set<T> /
// OwningSet<T>. The set's element routes through the generic hash table (records,
// primitives/enums, AND strings — see lowerNew / genericBackingType), so the
// array-backed for-loop (bz_array_*) would read garbage. Enumerate the table's
// keys (= the set's elements) instead.
bool Emitter::lowerDictBackedSetFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || !iterTy->astNode || !iterTy->astNode->name ||
        iterTy->astNode->name->parts.empty() || iterTy->declaration)
        return false;
    auto n = iterTy->headName();
    if (n != "Set" && n != "OwningSet" &&
        // stdlib §4.18 (#1426): SortedSet<T> uses the same generic-key hash table.
        // Route through emitGenericKeyTableFor with sorted=true so elements are
        // visited in ascending order per §4.18's ordered-iteration contract.
        n != "SortedSet") return false;
    const sema::TypeRef* elemTy =
        genericBackingType(iterTy, 0, /*isSet=*/true, nullptr);
    if (!elemTy) return false;  // not dict-backed → let lowerArrayFor handle it
    // fix #2553 (stdlib §4.18, v0.236): SortedSet<T> with an element type
    // buildSortedKeyComparator can't rank (string, decimal-as-record,
    // user choice, …) would otherwise silently drop the sort inside
    // emitGenericKeyTableFor (the "fall through unsorted" branch) and yield
    // insertion order — violating §4.18's ascending-iteration contract on a
    // fundamental element type. Defer to lowerIEnumerableFor's SortedSet-aware
    // helper, which routes through wrapSortedSetAsIEnumerable (its own string
    // comparator from #2550, the dictionary-entry wrap for records, etc.) so
    // the §4.18 promise holds for every SortedSet element type the wrapper
    // supports.
    if (n == "SortedSet" && !buildSortedKeyComparator(elemTy)) {
        // fix #3976 (stdlib §4.18, v0.295): fix #3947 rejected a no-CompareTo
        // eligible element at `.Add`/`.Contains`/etc (LlvmBackend_Calls.cpp's
        // Set-family arm) but for-in iteration is a SEPARATE call site that
        // also consults `genericBackingType` and was left silently admitting
        // the identical shape (falling through to `lowerIEnumerableFor`'s
        // SortedSet-aware wrap, which degrades to an unsorted/empty snapshot
        // rather than rejecting). Reuse the ONE authority fix #3947
        // established. A LEGITIMATE defer (string/decimal/user-choice
        // element — `buildSortedKeyComparator` null for a reason unrelated
        // to missing `CompareTo`) is unaffected: `elemTy` there is neither
        // `recordTypeOfRef` nor `hasObjectHeaderEquality`, so
        // `reportSortedElementNeedsCompareTo` correctly declines and this
        // still falls through to `lowerIEnumerableFor` below, unchanged.
        if (reportSortedElementNeedsCompareTo(iterTy, elemTy,
                                              fs.span.begin,
                                              "`for`-in iteration"))
            return true;  // reported; no codegen — compile fails on this Error
        return false;
    }
    llvm::Value* setObj = lowerExpr(*fs.iterable);
    if (!setObj) return false;
    bool sorted = (n == "SortedSet");
    return emitGenericKeyTableFor(fs, setObj, elemTy, sorted);
}


// issue #370 / §4.8: `for var k in <dict>.Keys` — iterate a Dictionary's keys.
// `<dict>.Keys` carries no sema element type, so this recognizes the AST shape
// (a MemberAccessExpr named "Keys" over a Dictionary-typed target) directly. A
// string-key dict enumerates via bz_dict_keys_str (the string fast path); a
// non-string (generic-table) key dict enumerates via bz_dict_keys_gen.
bool Emitter::lowerDictKeysFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get());
    if (!ma || !ma->target || ma->member != "Keys") return false;
    auto* dictTy = ctx_.exprType(*ma->target);
    if (!isDictTypeName(dictTy)) return false;
    if (dictTy->genericArgs.empty() || !dictTy->genericArgs[0]) return false;
    const sema::TypeRef* keyTy = dictTy->genericArgs[0];

    llvm::Value* dictObj = lowerExpr(*ma->target);
    if (!dictObj) return false;

    // Non-string keys live in the generic table — reuse the shared enumerator.
    if (const sema::TypeRef* genKey =
            genericBackingType(dictTy, 0, /*isSet=*/false, nullptr))
        return emitGenericKeyTableFor(fs, dictObj, genKey);

    // String keys: the dedicated string-key table. bz_dict_keys_str returns
    // parallel (char* data, int64 len) arrays; rebuild a string object per key
    // and bind it to the loop variable (a ptr). Mirrors the Dictionary.Keys
    // string-array construction in lowerExpr.
    using K = sema::TypeRef::Kind;
    bool isString = keyTy->kind == K::Primitive &&
                    keyTy->primitive == sema::PrimitiveType::String;
    if (!isString) return false;  // other shapes not wired

    // §29.12 / #1232: capture dict mod epoch before taking the string-key snapshot.
    auto* getModDictFnDk = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* dkEpochVal = builder_.CreateCall(getModDictFnDk, {dictObj}, "dks.epoch");
    auto* dkEpochSlot = entryAlloca(i64());
    builder_.CreateStore(dkEpochVal, dkEpochSlot);

    auto* keysOutSlot = entryAlloca(ptrTy());
    auto* lensOutSlot = entryAlloca(ptrTy());
    auto* widsOutSlot = entryAlloca(ptrTy());   // fix #4499: per-key §4.6 width
    auto* cntSlot     = entryAlloca(i64());
    auto* keysFn = declareC("bz_dict_keys_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(keysFn, {dictObj, keysOutSlot, lensOutSlot, widsOutSlot,
                                 cntSlot});
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);  // char**
    llvm::Value* lensArr = builder_.CreateLoad(ptrTy(), lensOutSlot);  // i64*
    llvm::Value* widsArr = builder_.CreateLoad(ptrTy(), widsOutSlot);  // i8*
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);
    auto* freeFn   = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);
    auto* varSlot = entryAlloca(ptrTy());  // loop var is a string-object pointer
    locals_[&fs] = varSlot;
    registerForLoopSpillSlots(fs, {dkEpochSlot, keysOutSlot, lensOutSlot,
                                   widsOutSlot, cntSlot, idxSlot, varSlot});
    // #1058 / §28/§4.8: initialise to null so the endBB break-path release is
    // null-safe when the loop body is never entered (cnt == 0).
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), varSlot);

    auto* condBB = llvm::BasicBlock::Create(llvm_, "dk.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dk.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "dk.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dk.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    // §29.12 / #1232: check for concurrent modification on every MoveNext.
    {
        llvm::Value* curMod = builder_.CreateCall(getModDictFnDk, {dictObj}, "dks.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), dkEpochSlot, "dks.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "dks.modchk"),
                       "InvalidOperationException");
    }
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
    // fix #4499: this loop carried its own copy of the raw-bytes → bz_string
    // decode, hard-coding width 1 and copying `lens[i]` BYTES (a code-point
    // count read as a byte count). Route it through the one shared decoder so
    // the §4.6 width rule is stated in exactly one place. Ownership is
    // unchanged: a fresh +1 string per iteration, released in incBB/endBB, and
    // the snapshot's own blob still freed by incBB below.
    llvm::Value* strObj =
        decodeDictStringKeyAt(keysArr, lensArr, widsArr, idxB).first;
    builder_.CreateStore(strObj, varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
    // #1058 / §28/§4.8: release the per-iteration string key object (rc 1→0 →
    // free dataCpy + strObj header).  Null varSlot so the endBB release below
    // is a no-op on normal completion (bz_string_release is null-safe).
    {
        llvm::Value* so = builder_.CreateLoad(ptrTy(), varSlot);
        emitStringRelease(so);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), varSlot);
    }
    // Free this iteration's strndup'd key bytes from bz_dict_keys_str.
    llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
    llvm::Value* kp2    = builder_.CreateLoad(ptrTy(), kpPtr2);
    builder_.CreateCall(freeFn, {kp2});
    builder_.CreateStore(builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    // #1058 / §28/§4.8: on `break` incBB is skipped — release the in-progress
    // iteration's strObj now.  On normal completion varSlot was nulled by incBB,
    // so bz_string_release(null) is a no-op (no double-free).
    {
        llvm::Value* so = builder_.CreateLoad(ptrTy(), varSlot);
        emitStringRelease(so);  // null-safe
    }
    builder_.CreateCall(freeFn, {keysArr});
    builder_.CreateCall(freeFn, {lensArr});
    builder_.CreateCall(freeFn, {widsArr});   // fix #4499: third snapshot spine
    return true;
}


// issue #537 / §4.8: `for var v in <dict>.Values` — iterate a Dictionary's
// values.  The runtime snapshot helpers bz_dict_values_str (string-key table)
// and bz_dict_values_gen (generic-key table) return a malloc'd void** array of
// the live slot values — the same void* encoding used by Add/Set (inttoptr for
// scalars, direct pointer for strings/classes, heap-box pointer for records).
// The loop decodes each slot value into the V LLVM type using the same logic as
// TryGetValue and GetOrAdd.
bool Emitter::lowerDictValuesFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get());
    if (!ma || !ma->target || ma->member != "Values") return false;
    auto* dictTy = ctx_.exprType(*ma->target);
    if (!isDictTypeName(dictTy)) return false;
    // genericArgs: [0] = K, [1] = V
    if (dictTy->genericArgs.size() < 2 || !dictTy->genericArgs[1]) return false;
    const sema::TypeRef* valTy = dictTy->genericArgs[1];

    // fix #3768 (§4.8 / §28.9, v0.286): the struct-vs-scalar-vs-pointer decode
    // decision is now made ONCE by decodeDictValueAt (LlvmBackend_impl.hpp /
    // LlvmBackend_Stmts.cpp, via the shared dictValueIsBoxedStruct predicate)
    // instead of this loop re-deriving its OWN `valRec || (valNullableVal &&
    // isStructTy())` copy — before this fix that copy was a SEVENTH,
    // independent instance of the exact narrow predicate fix #3758 unified
    // everywhere else, so `for var v in d.Values` over a `Dictionary<int,
    // Choice>` still crashed the IR verifier (`Trunc only produces integer`)
    // after #3758 landed. See decodeDictValueAt's declaration for why routing
    // through it is a strict widening (record / nullable-int / int / string /
    // class V all decode identically to before; only a struct-lowering V that
    // is neither a record nor a nullable value type — a choice — is newly
    // admitted).
    llvm::Type* valLlvm = lowerTypeRef(valTy);
    if (!valLlvm) return false;

    llvm::Value* dictObj = lowerExpr(*ma->target);
    if (!dictObj) return false;

    // §29.12 / #1232: capture dict mod epoch before taking the values snapshot.
    auto* getModDictFnDv = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* dvEpochVal = builder_.CreateCall(getModDictFnDv, {dictObj}, "dv.epoch");
    auto* dvEpochSlot = entryAlloca(i64());
    builder_.CreateStore(dvEpochVal, dvEpochSlot);

    // Snapshot the values array.  Choose the string-key or generic-key helper
    // based on whether the key (genericArgs[0]) routes through the generic table.
    auto* valsOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());

    bool useGen = genericBackingType(dictTy, 0, /*isSet=*/false, nullptr) != nullptr;
    if (useGen) {
        // Generic-key dict: bz_dict_values_gen(dict, void*** vals_out, int64_t* n)
        auto* fn = declareC("bz_dict_values_gen", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
    } else {
        // String-key dict: bz_dict_values_str(dict, void*** vals_out, int64_t* n)
        auto* fn = declareC("bz_dict_values_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
    }
    llvm::Value* valsArr = builder_.CreateLoad(ptrTy(), valsOutSlot); // void**
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);

    // Loop variable slot — stores the decoded V value.  For all types
    // (including records) the slot holds the value itself (a struct for records,
    // not a pointer).  fix #563: the previous code used ptrTy() for records and
    // stored the raw heap-box pointer; fieldAddr then GEP'd into the
    // pointer-holding alloca rather than the record struct, yielding garbage
    // field reads.  Mirror emitGenericKeyTableFor: load the struct out of the
    // box and store the struct value into a slot typed as valLlvm.
    // issue #539: nullable value types lower to a tagged struct — same: valLlvm.
    auto* varSlot = entryAlloca(valLlvm);
    locals_[&fs] = varSlot;
    registerForLoopSpillSlots(fs, {dvEpochSlot, valsOutSlot, cntSlot,
                                   idxSlot, varSlot});

    auto* condBB = llvm::BasicBlock::Create(llvm_, "dv.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dv.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "dv.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dv.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    // §29.12 / #1232: check for concurrent modification on every MoveNext.
    {
        llvm::Value* curMod = builder_.CreateCall(getModDictFnDv, {dictObj}, "dv.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), dvEpochSlot, "dv.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "dv.modchk"),
                       "InvalidOperationException");
    }
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);

    // fix #3768 (§4.8 / §28.9, v0.286): decode void* → V through the SAME
    // decodeDictValueAt helper lowerDictEntryFor's for-in loop already routes
    // through (see that function's own comment) — struct-load a boxed slot
    // (record / nullable value type / choice, per dictValueIsBoxedStruct),
    // else undo the ptrtoint scalar encoding, else pass the raw pointer
    // through as a borrow (string / class object). The box itself is owned by
    // the dict and must not be freed here — this only reads its bytes.
    llvm::Value* decoded = decodeDictValueAt(valLlvm, valsArr, idxB, "dv.val");
    builder_.CreateStore(decoded, varSlot);

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateStore(builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    builder_.CreateCall(freeFn, {valsArr});  // free the snapshot array itself
    return true;
}


// #548 / stdlib §4.8 v0.135: `for var e in dict` — single-pass borrowing entry
// iteration over a non-owning Dictionary<K,V>.  The loop variable `e` has
// the synthetic element type DictionaryEntry<K,V> (set by sema above); this
// function detects that type on the loop var and lowers the iteration using
// parallel key+value snapshot arrays exactly as lowerDictKeysFor and
// lowerDictValuesFor do.
//
// Struct layout: per-iteration entry alloca is a 2-field struct {K key; V val}.
// Field 0 = Key (LLVM type for K), field 1 = Value (LLVM type for V).
// e.Key/e.Value reads are handled in lowerExpr via GEP on that struct
// (before the generic fieldAddr path, which knows nothing of DictionaryEntry).
//
// The value is a borrow (per §28.11): no retain/dispose on the value slot.
// The key string (string-key dict) is ref-counted: the copied string object
// produced by bz_dict_keys_str is retained and lives exactly for the loop body,
// then released in the increment block (mirrors lowerDictKeysFor's free pass).
//
// The snapshot arrays themselves (void** from bz_dict_values_str and void**/i64*
// from bz_dict_keys_str) are malloc'd by the runtime and freed at endBB.

// narrow #3717 / §4.8 / §28.9: the per-index DictionaryEntry<K,V> decode
// shared by this function, `wrapDictionaryAsIEnumerable` (LlvmBackend_
// Assign.cpp) and #3576's First/Last/ElementAt arm (LlvmBackend_Calls.cpp).
// See the declarations in LlvmBackend_impl.hpp for the full rationale;
// snapshotting, sorting and freeing stay copy-specific at each call site.
std::pair<llvm::Value*, llvm::Value*> Emitter::decodeDictStringKeyAt(
        llvm::Value* keysArr, llvm::Value* lensArr, llvm::Value* widthsArr,
        llvm::Value* idx) {
    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
    auto* callocFn = declareC("calloc", ptrTy(), {i64(), i64()});
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    uint64_t strSz = module_->getDataLayout().getTypeAllocSize(stringType());
    llvm::Value* kpPtr = builder_.CreateGEP(ptrTy(), keysArr, idx);
    llvm::Value* kp    = builder_.CreateLoad(ptrTy(), kpPtr);
    llvm::Value* lpPtr = builder_.CreateGEP(i64(), lensArr, idx);
    llvm::Value* kl    = builder_.CreateLoad(i64(), lpPtr);
    // fix #4499 (spec §4.6, v0.306): the snapshot's third parallel spine is the
    // key's storage WIDTH. Pre-fix this decode hard-coded width 1 and copied
    // `kl` BYTES — i.e. it read a code-point count as a byte count, the same
    // confusion the storage layer had, so every width>1 key came back as a
    // truncated, mis-widthed string. The byte size is `len * width`, and the
    // rebuilt string carries the source key's own width so `=` against a
    // literal (emitStringEquals' equal-width memcmp path) still answers §4.6's
    // code-point-sequence question.
    llvm::Value* wpPtr = builder_.CreateGEP(i8Ty, widthsArr, idx);
    llvm::Value* kwB   = builder_.CreateLoad(i8Ty, wpPtr, "dk.width");
    llvm::Value* kw64  = builder_.CreateZExt(kwB, i64(), "dk.w64");
    llvm::Value* kbytes = builder_.CreateMul(kl, kw64, "dk.bytes");
    // One extra width-sized zero slot, matching how bz_string data buffers are
    // terminated (and what bz_key_dup writes on the runtime side).
    llvm::Value* dataSz  = builder_.CreateAdd(kbytes, kw64);
    llvm::Value* dataCpy = builder_.CreateCall(callocFn,
        {dataSz, builder_.getInt64(1)});
    builder_.CreateCall(memcpyFn, {dataCpy, kp, kbytes});
    llvm::Value* strObj = builder_.CreateCall(callocFn,
        {builder_.getInt64(1), builder_.getInt64((int64_t)strSz)});
    builder_.CreateStore(dataCpy,
        builder_.CreateStructGEP(stringType(), strObj, 0));
    builder_.CreateStore(kl,
        builder_.CreateStructGEP(stringType(), strObj, 1));
    builder_.CreateStore(kwB,
        builder_.CreateStructGEP(stringType(), strObj, 2));
    initStringRc(strObj);
    return {strObj, kp};
}

std::pair<llvm::Value*, llvm::Value*> Emitter::decodeDictGenericKeyAt(
        llvm::Type* keyLlvm, llvm::Value* keysArr, llvm::Value* idx,
        const char* namePrefix) {
    llvm::Value* kpPtr = builder_.CreateGEP(ptrTy(), keysArr, idx);
    llvm::Value* keyPtr = builder_.CreateLoad(ptrTy(), kpPtr);
    llvm::Value* keyDecoded = builder_.CreateLoad(keyLlvm, keyPtr, namePrefix);
    return {keyDecoded, keyPtr};
}

llvm::Value* Emitter::decodeDictValueAt(llvm::Type* valLlvm, llvm::Value* valsArr,
                                        llvm::Value* idx,
                                        const char* namePrefix) {
    llvm::Value* vpPtr = builder_.CreateGEP(ptrTy(), valsArr, idx);
    llvm::Value* slotV = builder_.CreateLoad(ptrTy(), vpPtr);
    // fix #3758, now routed through the #3770/#3768 shared predicate: derived
    // from the lowered type itself (a struct-lowering V is ALWAYS
    // struct-loaded from the box, whether it is a record, a nullable value
    // type, or a choice) rather than a caller-supplied bool — see
    // dictValueIsBoxedStruct's declaration in LlvmBackend_impl.hpp for why
    // this is a strict widening, not a behaviour change, for every V
    // reachable before fix #3758.
    if (dictValueIsBoxedStruct(valLlvm))
        return builder_.CreateLoad(valLlvm, slotV, namePrefix);
    if (!valLlvm->isPointerTy()) {
        llvm::Value* asI64 = builder_.CreatePtrToInt(slotV, i64());
        if (valLlvm->isDoubleTy())
            return builder_.CreateBitCast(asI64, valLlvm);
        if (valLlvm->isFloatTy())
            return builder_.CreateBitCast(
                builder_.CreateTrunc(asI64, i32()), valLlvm);
        if (valLlvm->isIntegerTy(64))
            return asI64;
        return builder_.CreateTrunc(asI64, valLlvm);
    }
    return slotV;  // pointer value (string/class): a borrow
}

bool Emitter::lowerDictEntryFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    // Detect: iterable type is Dictionary<K,V>.
    // (lowerDictKeysFor/ValuesFor handle MemberAccessExpr targets; this handles
    //  the bare-dictionary-expression case.)
    auto* dictTy = ctx_.exprType(*fs.iterable);
    if (!dictTy || !isDictTypeName(dictTy)) return false;
    if (dictTy->genericArgs.size() < 2 ||
        !dictTy->genericArgs[0] || !dictTy->genericArgs[1]) return false;
    // Confirm element type is DictionaryEntry<K,V> (set by sema).
    // Without this guard a `.Keys`/`.Values` access would be a
    // MemberAccessExpr (caught earlier) or a Set/Array iteration; checking
    // that the iterable is NOT a MemberAccessExpr suffices, but the sema
    // element-type check is the definitive gate: if sema typed the loop var
    // as DictionaryEntry the for-stmt IS an entry iteration.
    // (If fs.iterable is a MemberAccessExpr, lowerDictKeysFor/ValuesFor
    //  already returned true above — we never reach here for those.)
    if (dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get()))
        return false;

    const sema::TypeRef* keyTy = dictTy->genericArgs[0];
    const sema::TypeRef* valTy = dictTy->genericArgs[1];

    // fix #3976 (stdlib §4.17, v0.295): fix #3947 rejected a no-CompareTo
    // eligible key at `.Add`/`.ContainsKey`/etc (LlvmBackend_Calls.cpp's
    // isDict block) but entry for-in iteration (`for var e in sd`) is a
    // SEPARATE call site that also consults `genericBackingType` (via
    // `useGen`/`kTy` below, for the SORT step) and was left silently
    // admitting the identical shape — pre-fix, the sort step's own `cmpFn`
    // check just silently declined and iteration fell back to hash/
    // insertion order rather than rejecting. Checked here, BEFORE any
    // codegen for this loop (the snapshot calls, the sort, the loop body),
    // reusing the ONE authority fix #3947 established rather than a third
    // inline copy of the same predicate. `genericBackingType` is queried
    // again below (as `kTy`) for the sort step's own purpose (finding the
    // comparator on an ALREADY-admitted key) — this earlier call is a
    // classification-only query with no side effects, so recomputing it is
    // not the duplicated-PREDICATE hazard CLAUDE.md warns about.
    if (dictTy->headName() == "SortedDictionary") {
        const sema::TypeRef* genKeyTy =
            genericBackingType(dictTy, 0, /*isSet=*/false, nullptr);
        if (reportSortedElementNeedsCompareTo(dictTy, genKeyTy, fs.span.begin,
                                              "`for`-in iteration"))
            return true;  // reported; no codegen — compile fails on this Error
    }

    // fix #3758: `valRec` still selects a debug-name prefix below;
    // decodeDictValueAt's struct-vs-scalar decision no longer needs a
    // per-caller flag (it now reads valLlvm's own LLVM type).
    const sema::TypeSymbol* valRec =
        (valTy->kind == sema::TypeRef::Kind::Named &&
         valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;

    llvm::Type* keyLlvm = lowerTypeRef(keyTy);
    llvm::Type* valLlvm = lowerTypeRef(valTy);
    if (!keyLlvm || !valLlvm) return false;

    llvm::Value* dictObj = lowerExpr(*fs.iterable);
    if (!dictObj) return false;

    // §29.12 / #1232: capture dict mod epoch before taking the key+value snapshot.
    auto* getModDictFnDe = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* deEpochVal = builder_.CreateCall(getModDictFnDe, {dictObj}, "de.epoch");
    auto* deEpochSlot = entryAlloca(i64());
    builder_.CreateStore(deEpochVal, deEpochSlot);

    // Snapshot keys + values in parallel.
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    bool useGen = genericBackingType(dictTy, 0, /*isSet=*/false, nullptr) != nullptr;

    // --- key snapshot ---
    auto* keysOutSlot = entryAlloca(ptrTy()); // char** (string) or void** (gen)
    auto* lensOutSlot = entryAlloca(ptrTy()); // i64*   (string only; unused gen)
    auto* widsOutSlot = entryAlloca(ptrTy()); // i8*    (string only; fix #4499)
    auto* cntSlot     = entryAlloca(i64());

    // --- value snapshot ---
    auto* valsOutSlot = entryAlloca(ptrTy()); // void**

    using K = sema::TypeRef::Kind;
    bool isStringKey = keyTy->kind == K::Primitive &&
                       keyTy->primitive == sema::PrimitiveType::String;

    if (useGen) {
        // Generic-key dict: bz_dict_keys_gen(dict, void*** keys_out, int64_t* n)
        auto* kfn = declareC("bz_dict_keys_gen", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(kfn, {dictObj, keysOutSlot, cntSlot});
        auto* vfn = declareC("bz_dict_values_gen", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(vfn, {dictObj, valsOutSlot, entryAlloca(i64())});
    } else {
        // String-key dict: bz_dict_keys_str + bz_dict_values_str
        auto* kfn = declareC("bz_dict_keys_str", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(kfn, {dictObj, keysOutSlot, lensOutSlot,
                                  widsOutSlot, cntSlot});
        auto* vfn = declareC("bz_dict_values_str", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(vfn, {dictObj, valsOutSlot, entryAlloca(i64())});
    }
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
    llvm::Value* lensArr = isStringKey
                               ? builder_.CreateLoad(ptrTy(), lensOutSlot)
                               : nullptr;
    llvm::Value* widsArr = isStringKey            // fix #4499
                               ? builder_.CreateLoad(ptrTy(), widsOutSlot)
                               : nullptr;
    llvm::Value* valsArr = builder_.CreateLoad(ptrTy(), valsOutSlot);
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);

    // fix #1426 / stdlib §4.17: for SortedDictionary, sort keys and values
    // together in ascending key order before the loop.  O(n log n) per
    // enumeration (hash-backing limitation; ordered rep rides #545).
    if (dictTy && dictTy->astNode && dictTy->astNode->name &&
        !dictTy->astNode->name->parts.empty() &&
        dictTy->headName() == "SortedDictionary") {
        if (useGen) {
            const sema::TypeRef* kTy =
                genericBackingType(dictTy, 0, /*isSet=*/false, nullptr);
            llvm::Function* cmpFn = kTy ? buildSortedKeyComparator(kTy) : nullptr;
            if (cmpFn) {
                auto* sortKvFn = declareC("bz_sort_kv_gen",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(sortKvFn, {keysArr, valsArr, cnt, cmpFn});
            }
        } else {
            // String-key: sort keys (char**), lens (i64*), and values together.
            auto* sortKvStrFn = declareC("bz_sort_kv_str",
                llvm::Type::getVoidTy(llvm_),
                {ptrTy(), ptrTy(), ptrTy(), ptrTy(), i64()});
            llvm::Value* lensForSort = lensArr
                ? lensArr
                : llvm::ConstantPointerNull::get(ptrTy());
            // fix #4499: the width spine is permuted with the others.
            llvm::Value* widsForSort = widsArr
                ? widsArr
                : llvm::ConstantPointerNull::get(ptrTy());
            builder_.CreateCall(sortKvStrFn,
                {keysArr, lensForSort, widsForSort, valsArr, cnt});
        }
    }

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);

    // Loop variable alloca: 2-field struct {K key; V value}.
    // The struct layout matches what lowerExpr's DictionaryEntry intercept
    // expects (field 0 = Key, field 1 = Value).
    llvm::StructType* entryStTy =
        llvm::StructType::get(llvm_, {keyLlvm, valLlvm});
    auto* varSlot = entryAlloca(entryStTy);
    locals_[&fs] = varSlot;
    // #1058 / §28/§4.8: for string keys the key field (field 0) carries a
    // ref-counted bz_string_t* built per iteration.  Initialise to null so
    // the endBB break-path release (see below) is null-safe when the loop body
    // is never entered (cnt == 0).
    if (isStringKey)
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                             builder_.CreateStructGEP(entryStTy, varSlot, 0));

    auto* condBB = llvm::BasicBlock::Create(llvm_, "de.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "de.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "de.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "de.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    {
        // §29.12 / #1232: check for concurrent modification on every MoveNext.
        {
            llvm::Value* curMod = builder_.CreateCall(getModDictFnDe, {dictObj}, "de.curmod");
            llvm::Value* epV    = builder_.CreateLoad(i64(), deEpochSlot, "de.epv");
            emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "de.modchk"),
                           "InvalidOperationException");
        }
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);
    }

    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);

        // --- decode key (narrow #3717: shared with wrapDictionaryAsIEnumerable
        // and #3576's arm; the raw key pointer these return is freed BELOW in
        // incBB, unchanged from before this fix — see the helpers' declarations
        // in LlvmBackend_impl.hpp) ---
        llvm::Value* keyDecoded;
        if (isStringKey) {
            keyDecoded =
                decodeDictStringKeyAt(keysArr, lensArr, widsArr, idxB).first;
        } else if (useGen) {
            // bz_dict_keys_gen returns void** where each element is a malloc'd
            // copy of the key's raw bytes (NOT inttoptr-encoded — the raw bytes
            // are loaded directly as keyLlvm). The copy must be freed in incBB
            // (same free pass as emitGenericKeyTableFor performs).
            keyDecoded = decodeDictGenericKeyAt(keyLlvm, keysArr, idxB, "de.kgen").first;
        } else {
            // Non-string non-generic key — should not happen; emit null.
            keyDecoded = llvm::Constant::getNullValue(keyLlvm);
        }

        // --- decode value (mirrors lowerDictValuesFor; fix #3758: struct-vs-
        // scalar is now decided BY decodeDictValueAt from valLlvm itself) ---
        llvm::Value* valDecoded = decodeDictValueAt(
            valLlvm, valsArr, idxB, valRec ? "de.vrec" : "de.vnv");

        // Store key at field 0, value at field 1 of the entry struct.
        builder_.CreateStore(keyDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 0));
        builder_.CreateStore(valDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 1));
    }

    // fix #4129 (§29.12/§28.3, stdlib §4.8, v0.295): register the snapshot
    // spine (keysArr/lensArr/valsArr + the in-flight string Key) as its own
    // disposeStack_ scope BEFORE loopStack_'s own entry, so a `return`/`throw`
    // from inside the body reclaims it (see the field declarations in
    // LlvmBackend_impl.hpp for the full derivation). Pushed AFTER the
    // snapshot's own allocas exist so the registration can carry them; popped
    // WITHOUT emission once the manual endBB code below has run (see there).
    //
    // fix #4129 review self-pass: `forVarCapturedInBody(fs)` is the SAME
    // static, whole-body AST scan `lowerForBody` (below) uses to decide
    // whether to box `entry` for a capturing closure — computed once here,
    // BEFORE lowerForBody runs, so it reflects the body regardless of
    // whether boxing actually happens for this specific loop. When true, a
    // captured `entry`'s struct was copied into a heap cell an escaping
    // closure may read after this disposal fires; releasing the in-flight
    // string Key here would be a use-after-free on that alias (measured,
    // fixed before landing — see dictEntryForReleaseInFlightKey's
    // declaration). The snapshot-array frees themselves are unaffected: no
    // capture ever aliases them.
    bool forVarCaptured = forVarCapturedInBody(fs);
    // fix #4161 (§27.5/§28.9/§28.3, stdlib §4.8, v0.295): #4129 gated ONLY
    // its own new dictEntryForSnapshot disposal (the return/throw path) on
    // `forVarCaptured` — a coarse, escape-BLIND predicate, true the instant
    // ANY closure in the body references the loop var at all, whether or not
    // that closure outlives the iteration. incBB/endBB's own #1058
    // per-iteration/end-of-loop release below had the IDENTICAL hazard on
    // the normal-completion/`break` path (no return/throw needed at all —
    // #4161's own repro) and was left completely ungated.
    //
    // A bare `!forVarCaptured` gate for incBB/endBB would trade the UAF for
    // a leak on EVERY capture, including the common, provably-safe shape
    // (`for var e in d do var f := lambda() => e.Key; f(); end;` — a purely
    // local, non-escaping closure invoked and disposed inside the SAME
    // iteration's body, before incBB ever runs). `cellFreeEligible_` is the
    // escape-aware authority that already answers this precisely: it is the
    // SAME whole-body pre-pass `lowerForBody` (below) consults, keyed by
    // `&fs` exactly like `forVarCapturedInBody`, to decide whether the
    // for-var's own boxed capture CELL can be freed at each iteration's body
    // end (`collectCellFreeEligible`, LlvmBackend_Setup.cpp). It contains
    // `&fs` iff (a) at least one lambda captures the for-var, AND (b) NO
    // escaping lambda captures it — condition (b) is exactly "does the
    // closure actually outlive the iteration". When it holds, EVERY
    // capturing closure's env — and with it, the boxed cell's own alias of
    // this same `bz_string*` pointer — is disposed by `lowerForBody`'s own
    // scope-end/break-unwind handling strictly BEFORE control reaches
    // incBB/endBB for this iteration (`forVarFreshCell`'s `boxedCellFree`
    // disposal is pushed INSIDE the body's own scope and fires on both the
    // fall-through and the loop-unwind-via-`break` edges, both before this
    // function's incBB/endBB code runs), so releasing the ORIGINAL pointer
    // here is sound. Falls back to the coarse, always-safe "any capture
    // leaks" answer when `cellFreeEligible_` was not computed for the
    // CURRENT function (`currentFn_ != cellFreeEligibleFn_`, e.g. a for-loop
    // lowered inside a lambda body analyzed separately) — the pre-existing
    // conservative default every other cellFreeEligible_ consumer in this
    // file already uses.
    bool forVarCapturedEscaping = forVarCaptured &&
        !(currentFn_ == cellFreeEligibleFn_ &&
          cellFreeEligible_.count(static_cast<const void*>(&fs)));
    disposeStack_.push_back({});
    {
        Disposal d{nullptr, nullptr};
        d.dictEntryForSnapshot = true;
        d.dictEntryForKeysOutSlot = keysOutSlot;
        d.dictEntryForLensOutSlot = isStringKey ? lensOutSlot : nullptr;
        d.dictEntryForWidsOutSlot = isStringKey ? widsOutSlot : nullptr;  // #4499
        d.dictEntryForValsOutSlot = valsOutSlot;
        d.dictEntryForCntSlot = cntSlot;
        d.dictEntryForIdxSlot = idxSlot;
        d.dictEntryForIsStringKey = isStringKey;
        d.dictEntryForUseGen = useGen;
        d.dictEntryForVarSlot = varSlot;
        d.dictEntryForReleaseInFlightKey = isStringKey && !forVarCapturedEscaping;
        disposeStack_.back().push_back(d);
    }

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    {
        llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
        if (isStringKey) {
            // #1058 / §28/§4.8: release the per-iteration string key object
            // (rc 1→0 → free dataCpy + strObj header).  Null field 0 so the
            // endBB release below is a no-op on normal completion.
            //
            // fix #4161: gated on !forVarCapturedEscaping (see its
            // declaration above, at the dictEntryForSnapshot registration).
            // An escaping closure's boxed cell may still alias this exact
            // `bz_string*` pointer, so both the release AND the null-out are
            // skipped together — endBB below carries the SAME gate, so
            // leaving field 0 non-null here does not risk a double-release
            // there; the value is simply never released (a bounded leak,
            // the safe direction, matching dictEntryForReleaseInFlightKey's
            // return/throw-path fallback).
            if (!forVarCapturedEscaping) {
                llvm::Value* so = builder_.CreateLoad(
                    ptrTy(), builder_.CreateStructGEP(entryStTy, varSlot, 0));
                emitStringRelease(so);
                builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                    builder_.CreateStructGEP(entryStTy, varSlot, 0));
            }
            // Free the strndup'd char* data for this iteration (mirrors
            // lowerDictKeysFor's per-slot free in the increment block). This
            // buffer (`kp2`) is decodeDictStringKeyAt's SOURCE bytes — a
            // malloc'd allocation distinct from the `bz_string*` built from
            // it (`so` above) — never aliased by a capturing closure, so
            // freeing it is unconditionally safe regardless of capture.
            llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
            llvm::Value* kp2    = builder_.CreateLoad(ptrTy(), kpPtr2);
            builder_.CreateCall(freeFn, {kp2});
        } else if (useGen) {
            // Free the malloc'd raw-bytes key copy for this iteration
            // (mirrors emitGenericKeyTableFor's per-slot free).
            llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
            llvm::Value* kp2    = builder_.CreateLoad(ptrTy(), kpPtr2);
            builder_.CreateCall(freeFn, {kp2});
        }
        builder_.CreateStore(
            builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
    }

    builder_.SetInsertPoint(endBB);
    // #1058 / §28/§4.8: on `break` incBB is skipped — release the in-progress
    // iteration's strObj now.  On normal completion field 0 was nulled by incBB,
    // so bz_string_release(null) is a no-op (no double-free).
    //
    // fix #4161: gated on !forVarCapturedEscaping (see its declaration above,
    // at the dictEntryForSnapshot registration) — same reasoning as incBB's
    // own gate. An escaping closure's boxed cell may still alias this
    // iteration's `so`, whether reached via `break` (field 0 holds the
    // in-progress iteration's un-nulled value) or via normal completion
    // (field 0 holds whatever the last incBB left there, un-nulled under
    // this same gate). Skipping here is the safe-direction leak, not a
    // use-after-free.
    if (isStringKey && !forVarCapturedEscaping) {
        llvm::Value* so = builder_.CreateLoad(
            ptrTy(), builder_.CreateStructGEP(entryStTy, varSlot, 0));
        emitStringRelease(so);  // null-safe
    }
    // #1057: the increment block frees each per-iteration key box (string /
    // generic keys are separately malloc'd), but a `break` out of the loop skips
    // the boxes from the break index onward — they leak. Free the remaining
    // boxes (idxSlot .. cnt) here so every box is reclaimed on EVERY exit. On
    // normal completion idxSlot == cnt, so this loop frees nothing (no double-
    // free of the boxes the increment block already freed).
    if (isStringKey || useGen) {
        auto* clCondBB = llvm::BasicBlock::Create(llvm_, "de.cl.cond", currentFn_);
        auto* clBodyBB = llvm::BasicBlock::Create(llvm_, "de.cl.body", currentFn_);
        auto* clEndBB  = llvm::BasicBlock::Create(llvm_, "de.cl.end",  currentFn_);
        builder_.CreateBr(clCondBB);
        builder_.SetInsertPoint(clCondBB);
        llvm::Value* ci = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(ci, cnt), clBodyBB, clEndBB);
        builder_.SetInsertPoint(clBodyBB);
        llvm::Value* ckp = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(ptrTy(), keysArr, ci));
        builder_.CreateCall(freeFn, {ckp});
        builder_.CreateStore(
            builder_.CreateAdd(ci, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(clCondBB);
        builder_.SetInsertPoint(clEndBB);
    }
    builder_.CreateCall(freeFn, {keysArr});
    if (isStringKey) {
        builder_.CreateCall(freeFn, {lensArr});
        builder_.CreateCall(freeFn, {widsArr});   // fix #4499
    }
    builder_.CreateCall(freeFn, {valsArr});
    // fix #4129: the manual frees just above did EXACTLY what the
    // dictEntryForSnapshot scope pushed before loopStack_ would do — pop it
    // WITHOUT emission (a plain std::vector pop; nothing is emitted) so a
    // LATER return/throw elsewhere in this method does not walk it a second
    // time and double-free this snapshot. Every `return`/`throw` reachable
    // from INSIDE the loop body already fired it (via emitAllDisposals /
    // emitDisposalsInRange / the unwind walk) before control could reach
    // here, so this pop is reached only by normal completion or `break` —
    // the two exits the manual code above already covers.
    disposeStack_.pop_back();
    // #1055: a fresh `owned Dictionary` materializer iterated directly as the
    // for-in source (`for var e in src.ToDictionary(k) do … end`) is owned by
    // nothing and leaks. Reclaim it here at the loop exit, which both normal
    // completion and `break` reach (a `return` in the body diverges before this
    // — leaks, the safe direction). No-op unless the iterable is exactly a fresh
    // ownerless dict materializer (its narrow guard) — a bound / `move`d /
    // aliased dict is never matched, so no UAF / double-free. The temp key/value
    // extraction arrays are freed above first; this frees the dict itself.
    freeUnboundOwnedBorrowingArrayTemp(fs.iterable.get(), dictObj,
                                       ctx_.exprType(*fs.iterable));
    return true;
}


bool Emitter::lowerStringFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* tt = ctx_.exprType(*fs.iterable);
    if (!tt || tt->kind != sema::TypeRef::Kind::Primitive ||
        tt->primitive != sema::PrimitiveType::String)
        return false;

    llvm::Value* strPtr = lowerExpr(*fs.iterable);
    if (!strPtr) return false;
    llvm::Value* len = loadStringLen(strPtr);          // i64 code-point count

    // Index counter (i64).
    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);

    // Loop variable slot — the body sees `c` as the current char (i32).
    auto* varSlot = entryAlloca(i32());
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "sfor.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sfor.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "sfor.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "sfor.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    llvm::Value* cond = builder_.CreateICmpSLT(idx, len, "sfor.cond");
    builder_.CreateCondBr(cond, bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateStore(loadCodePoint(strPtr, idxB), varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    lowerForBody(fs);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxInc = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateStore(builder_.CreateAdd(idxInc, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// §3.3 / §29.2 / #971 item 2: `for var c in sp do … end` over a
// ReadOnlyStringSpan — iterate code points over the span's
// [start, start+len) sub-range of the source string. Same lowering shape
// as lowerStringFor but the source pointer (field 0 of the {ptr, i32, i32}
// span struct), start offset (field 1) and code-point length (field 2) come
// from the span; the loop variable is loaded via loadCodePoint(src, start+idx).
// A null iterable / non-span static type / type without the span struct
// falls through (returns false) so the regular for-handler chain can take it.
bool Emitter::lowerStringSpanFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* tt = ctx_.exprType(*fs.iterable);
    if (!tt || tt->kind != sema::TypeRef::Kind::Named || tt->declaration ||
        !tt->astNode || !tt->astNode->name ||
        tt->astNode->name->parts.empty())
        return false;
    if (tt->headName() != "ReadOnlyStringSpan") return false;

    llvm::Value* sv = lowerExpr(*fs.iterable);
    if (!sv) return false;
    // {ptr source, i32 start, i32 length}
    llvm::Value* srcPtr = builder_.CreateExtractValue(sv, {0u}, "ssp.src");
    llvm::Value* start  = builder_.CreateExtractValue(sv, {1u}, "ssp.start");
    llvm::Value* len32  = builder_.CreateExtractValue(sv, {2u}, "ssp.len");

    // Index counter (i64) into the span's logical [0, len) range.
    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);

    // Loop variable slot — the body sees `c` as the current char (i32).
    auto* varSlot = entryAlloca(i32());
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "ssp.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ssp.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "ssp.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "ssp.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    // idx < len, widened from i32 → i64 for the compare. The widening is a
    // SIGN extension and the compare is SIGNED, matching every sibling span
    // path (the array-span for-in above compares `slen` with ICmpSLT and
    // sign-extends `base`). `Length` and `Start` are §3.3 `int` fields — signed
    // — so a zero extension plus an UNSIGNED compare reinterprets a negative
    // Length as ~4 billion and turns what must be a zero-trip loop into a
    // multi-gigabyte out-of-bounds read of the source string. No producer can
    // hand a negative Length here today (every AsSpan/Slice path guards
    // start >= 0 and len >= 0 with emitGuardThrow), so this changes no
    // reachable behavior — it removes the trap rather than fixing a live bug.
    llvm::Value* len64 =
        builder_.CreateSExt(len32, i64(), "ssp.len64");
    llvm::Value* cond = builder_.CreateICmpSLT(idx, len64, "ssp.cond");
    builder_.CreateCondBr(cond, bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
    // Source index = start + idx (start is a signed i32 §3.3 `int` field).
    llvm::Value* start64 =
        builder_.CreateSExt(start, i64(), "ssp.start64");
    llvm::Value* srcIdx =
        builder_.CreateAdd(start64, idxB, "ssp.srcidx");
    builder_.CreateStore(loadCodePoint(srcPtr, srcIdx), varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    lowerForBody(fs);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxInc = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateStore(builder_.CreateAdd(idxInc, builder_.getInt64(1)),
                         idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// #639 / stdlib §4.5/§4.7/§4.11 v0.135: `for var x in coll.Drain()` —
// ownership-transferring drain over OwningArray<T>, OwningLinkedList<T>,
// OwningStack<T>, OwningQueue<T>.
//
// Detection: the iterable expression has type IOwningEnumerable<T> (set by sema
// for the four collection names above) with genericArgs[0] NOT being a
// DrainedEntry (which would be the OwningDictionary case, handled below).
// Additionally, the iterable must be a Drain() call whose receiver has one of
// the four recognised type names.
//
// Drain order per §28.11 / stdlib §4.5/§4.7/§4.11:
//   OwningArray<T>       — front-to-back (bz_array_drain_next)
//   OwningQueue<T>       — FIFO = front-to-back (bz_array_drain_next)
//   OwningStack<T>       — LIFO = top-first (bz_array_drain_next_lifo)
//   OwningLinkedList<T>  — head-to-tail (bz_llist_drain_next)
//
// Runtime shape: a single call to the appropriate drain function per loop
// iteration.  The function pops one element pointer into *val_out and
// decrements the collection's count.  Returns false when the collection is
// empty.  The loop variable alloca holds the popped pointer; lowerExpr reads it
// as the owned T value.
//
// Partial drain: break exits the loop; remaining elements stay in the
// collection and are disposed by the collection's own Dispose() at scope-end.
// Exception mid-drain: same — unwinding invokes the collection's disposer via
// the standard using-var cleanup chain.
bool Emitter::lowerOwnedSeqDrainFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;

    // Must be IOwningEnumerable<T>.
    std::string_view iterName;
    if (iterTy->astNode && iterTy->astNode->name &&
        !iterTy->astNode->name->parts.empty())
        iterName = iterTy->headName();
    else if (!iterTy->importedName.empty())
        iterName = iterTy->importedName;
    if (iterName != "IOwningEnumerable") return false;

    // Must have one generic arg T that is NOT DrainedEntry (that's the dict path).
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return false;
    const sema::TypeRef* elemTy = iterTy->genericArgs[0];
    if (elemTy->kind == sema::TypeRef::Kind::Named &&
        elemTy->importedName == "DrainedEntry")
        return false;  // let lowerDictDrainFor handle it

    // The iterable must be a Drain() call on one of the five collection types.
    // fix #1475 / stdlib §4.10: OwningSet<T> is included alongside OwningArray/
    // OwningLinkedList/OwningStack/OwningQueue. OwningSet<T> requires
    // `where T: class` (stdlib §4.10), so its element is never eligible for the
    // generic-key dict backing (genericBackingType returns null for every class
    // type, per #1474) — every valid OwningSet<T> is therefore ARRAY-backed,
    // constructed via the same bz_array_alloc allocation and manipulated by the
    // same bz_array_* entry points as OwningArray (see the Add/TryAdd/Remove/
    // Contains/Count/Clear handlers in LlvmBackend_Calls.cpp, which already
    // operate on an OwningSet receiver through bz_array_len/bz_array_ptr_at/
    // bz_array_add directly). Draining it is thus mechanically identical to
    // draining an OwningArray: bz_array_drain_next pops index 0 each iteration,
    // transferring ownership of the popped element to the loop body — order is
    // unspecified by §4.10 (a set has no ordering contract), so front-to-back
    // is a legitimate, simple choice.
    const ast::Expr* collExpr = nullptr;
    std::string_view collName;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(fs.iterable.get())) {
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get())) {
            if (ma->member == "Drain" && ma->target) {
                collExpr = ma->target.get();
                const sema::TypeRef* ct = ctx_.exprType(*collExpr);
                if (ct && ct->kind == sema::TypeRef::Kind::Named) {
                    if (ct->astNode && ct->astNode->name &&
                        !ct->astNode->name->parts.empty())
                        collName = ct->headName();
                    else if (!ct->importedName.empty())
                        collName = ct->importedName;
                }
            }
        }
    } else if (auto* id =
                   dynamic_cast<const ast::IdentifierExpr*>(fs.iterable.get())) {
        // fix #2167 / stdlib §4.5/§4.7/§4.11 v0.135: the bound form
        // `var d := coll.Drain(); for var x in d do ... end` — when the iterable
        // is an IOwningEnumerable<T>-typed LOCAL whose initializer is the
        // builtin Drain() call, the drain view IS the receiver collection
        // (stdlib §4.5 "no separate enumerable object") and the for-in should
        // drain IT, not iterate it as a generic bz_array / vtable iterator. The
        // bz_array protocol path would read the class-instance layout as an
        // array header (the vtable pointer becomes the count — a SEGV); the
        // vtable path would dispatch Dispose via slot 1 (the iterator-frame
        // Dispose) on a class instance whose slot 1 is a field offset, not a
        // function pointer (a SEGV too, §29.7). Recover the original Drain()
        // call expression from the LocalDecl and lower the same way as the
        // inline form — semantic equivalence, same discharge / disposal story
        // (no separate owning binding for `d`, aliasing per §28.10 / #2146).
        if (auto* sym = ctx_.exprSymbol(*id);
            sym && dynamic_cast<const sema::LocalSymbol*>(sym)) {
            const auto* ls = static_cast<const sema::LocalSymbol*>(sym);
            // The local's declaration must be a plain LocalDecl (not an inline
            // `out var` whose `decl` is an IdentifierExpr). Inline locals have
            // no `initializer` and are excluded by definition.
            const auto* ld =
                dynamic_cast<const ast::LocalDecl*>(ls->decl);
            if (ld && ld->initializer) {
                if (auto* ce2 =
                        dynamic_cast<const ast::CallExpr*>(ld->initializer.get())) {
                    if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(
                            ce2->callee.get())) {
                        if (ma2->member == "Drain" && ma2->target) {
                            collExpr = ma2->target.get();
                            const sema::TypeRef* ct =
                                ctx_.exprType(*collExpr);
                            if (ct && ct->kind == sema::TypeRef::Kind::Named) {
                                if (ct->astNode && ct->astNode->name &&
                                    !ct->astNode->name->parts.empty())
                                    collName = ct->headName();
                                else if (!ct->importedName.empty())
                                    collName = ct->importedName;
                            }
                        }
                    }
                }
            }
        }
    }
    if (!collExpr) return false;
    if (collName != "OwningArray" && collName != "OwningLinkedList" &&
        collName != "OwningStack" && collName != "OwningQueue" &&
        collName != "OwningSet")
        return false;

    // Evaluate the collection object.
    llvm::Value* collObj = lowerExpr(*collExpr);
    if (!collObj) return false;

    // The element type is always a class pointer (ptrTy()) for owned collections.
    // #990 / §4.5 / §14.2: in a GENERIC collection (`OwningArray<T>` with T an
    // unresolved class type parameter) `elemTy` is the bare param `T`.  When this
    // method is lowered as the generic TEMPLATE body (no active substitution),
    // lowerTypeRef(T) returns the i32 placeholder, so the element-slot store below
    // bitcast the popped `ptr` to i32 — malformed IR (IR_FAIL).  Resolve `T`
    // through the active per-instantiation substitution first; if it stays an
    // unresolved param, the owning collection's element is a class object by the
    // stdlib invariant (only classes are owned elements — `where T: class`), so
    // lower it as a class pointer (ptrTy()).  The store path below uses `rawPtr`
    // directly when elemLlvm == ptrTy(), so no bitcast is emitted.
    const sema::TypeRef* resolvedElemTy = resolveTypeParamRef(elemTy);
    llvm::Type* elemLlvm;
    if (resolvedElemTy && resolvedElemTy != elemTy)
        elemLlvm = lowerTypeRef(resolvedElemTy);
    else if (elemTy->kind == sema::TypeRef::Kind::Named &&
             elemTy->declaration == nullptr)
        // Unresolved bare type parameter for an owning collection: a class ptr.
        elemLlvm = ptrTy();
    else
        elemLlvm = lowerTypeRef(elemTy);
    if (!elemLlvm) return false;

    // Alloca for the loop variable (holds the popped owned pointer).
    auto* varSlot = entryAlloca(elemLlvm);
    // §30.7 / §29.2 (#1888): if the drained loop variable is live across an
    // `await` in the BODY (the canonical Pattern B drain-and-await:
    // `for var task in tasks.Drain() do results.Add(await task); end`), it is a
    // suspend-local — frame-back its slot exactly as range-for
    // (LlvmBackend_Stmts.cpp) and await-for (lowerAwaitFor) already do.
    // collectSuspendLocals (LlvmBackend_Iterators.cpp) reserves the frame field,
    // keyed on the ForStmt node. WITHOUT this the slot is a bare stack alloca
    // that is NOT spilled/restored across the body's await suspend, so on resume
    // (a fresh Resume invocation with a fresh stack) it holds a stale/garbage
    // task pointer — the body-end iter-var disposal then dereferences a dangling
    // (freed) task (`pthread_mutex` ESRCH / SEGV in bz$async$*$Resume). Framing
    // it makes the slot hold the current iteration's live task at body-end, so
    // the disposal reclaims it exactly once. // per §30.7, §29.2
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(&fs);
        if (fit != smLocalFields_.end())
            smAllocaToField_[varSlot] = fit->second;
    }
    locals_[&fs] = varSlot;

    // Alloca for the raw element output from the drain function.
    auto* valOutSlot = entryAlloca(ptrTy());

    // Choose the drain runtime function name based on the collection kind.
    const char* drainFnName;
    if (collName == "OwningLinkedList") {
        drainFnName = "bz_llist_drain_next";      // pops head → head-to-tail
    } else if (collName == "OwningStack") {
        drainFnName = "bz_array_drain_next_lifo"; // pops tail → LIFO
    } else {
        // OwningArray and OwningQueue: both drain front-to-back. OwningSet
        // (fix #1475) shares the same array backing and has no ordering
        // contract to honour, so it drains front-to-back too.
        drainFnName = "bz_array_drain_next";      // pops index 0 → FIFO/front-to-back
    }
    // bz_array_drain_next / bz_array_drain_next_lifo / bz_llist_drain_next
    //   (void* coll, void** val_out) → bool (i1)
    auto* drainFn = declareC(drainFnName, i1(), {ptrTy(), ptrTy()});

    auto* condBB = llvm::BasicBlock::Create(llvm_, "sd.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sd.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "sd.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "sd.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: call the drain function; it pops one element into valOutSlot.
    builder_.SetInsertPoint(condBB);
    llvm::Value* popped = builder_.CreateCall(drainFn,
        {collObj, valOutSlot}, "sd.popped");
    builder_.CreateCondBr(popped, bodyBB, endBB);

    // body: load the popped element pointer and store into the loop var slot.
    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* rawPtr = builder_.CreateLoad(ptrTy(), valOutSlot);
        // The drain functions return a void* holding the class object pointer.
        // If elemLlvm is ptrTy() we use rawPtr directly; otherwise bitcast.
        llvm::Value* elemVal =
            (elemLlvm == ptrTy())
            ? rawPtr
            : builder_.CreateBitCast(rawPtr, elemLlvm, "sd.elem");
        builder_.CreateStore(elemVal, varSlot);
    }

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: jump back to cond (the collection is already smaller after each pop).
    builder_.SetInsertPoint(incBB);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// #628 / stdlib §4.9 v0.135: `for var entry in ownedDict.Drain()` —
// ownership-transferring drain iteration over OwningDictionary<K,V>.
//
// Detection: the iterable expression has type IOwningEnumerable<DrainedEntry<K,V>>
// (set by sema for OwningDictionary<K,V>.Drain()). The first generic arg of
// IOwningEnumerable<T> is T = DrainedEntry<K,V> with importedName=="DrainedEntry".
//
// Loop variable layout: a 2-field struct alloca {K key; V* value} — mirroring
// the DictionaryEntry layout used by lowerDictEntryFor. Field 0 = Key, field 1 =
// Value. lowerExpr intercepts e.Key/e.Value GEPs on these (the DrainedEntry
// handler mirrors the DictionaryEntry handler in LlvmBackend_Exprs.cpp).
//
// Ownership: the loop var's Value field IS the owned class pointer, transferred
// out of the dict on each iteration. The key is a fresh copy (string: malloc'd
// bz_string_t; generic: raw bytes). Both are the loop body's responsibility.
// The for-loop owns the entry alloca; using var e := entry discharges the entry
// (and its Value) at the end of the iteration body.
//
// Partial drain: break exits the loop leaving remaining entries in the dict.
// The dict's Dispose() (scope-end) will dispose them via bz_dict_free.
//
// Per §4.9: OwningDictionary makes no insertion-order guarantee; iteration walks
// the order[] array in the natural (insertion-recorded) order as a convenience —
// the spec does not prohibit this, and the runtime note says the order depends
// on insertion-and-mutation history (not a strict contract). For correctness,
// what matters is: every live entry is yielded exactly once.
bool Emitter::lowerDictDrainFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;
    // Must be IOwningEnumerable<DrainedEntry<K,V>>.
    // Check the type name is IOwningEnumerable.
    std::string_view iterName;
    if (iterTy->astNode && iterTy->astNode->name &&
        !iterTy->astNode->name->parts.empty())
        iterName = iterTy->headName();
    else if (!iterTy->importedName.empty())
        iterName = iterTy->importedName;
    if (iterName != "IOwningEnumerable") return false;
    // Must have one generic arg = DrainedEntry<K,V>.
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return false;
    const sema::TypeRef* entryTy = iterTy->genericArgs[0];
    if (entryTy->kind != sema::TypeRef::Kind::Named ||
        entryTy->importedName != "DrainedEntry" ||
        entryTy->genericArgs.size() < 2 ||
        !entryTy->genericArgs[0] || !entryTy->genericArgs[1])
        return false;

    const sema::TypeRef* keyTy = entryTy->genericArgs[0];
    const sema::TypeRef* valTy = entryTy->genericArgs[1];

    // The iterable must be a Drain() call on an OwningDictionary.
    // Recover the dictionary object pointer by re-evaluating the target of
    // the Drain() CallExpr.
    const ast::Expr* dictExpr = nullptr;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(fs.iterable.get())) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get())) {
            if (ma->member == "Drain" && ma->target)
                dictExpr = ma->target.get();
        }
    }
    if (!dictExpr) return false;

    // Confirm the target has type OwningDictionary<K,V>.
    const sema::TypeRef* dictTy = ctx_.exprType(*dictExpr);
    if (!isDictTypeName(dictTy)) return false;

    // Evaluate the dictionary object.
    llvm::Value* dictObj = lowerExpr(*dictExpr);
    if (!dictObj) return false;

    llvm::Type* keyLlvm = lowerTypeRef(keyTy);
    llvm::Type* valLlvm = lowerTypeRef(valTy);  // V* (pointer to owned class)
    if (!keyLlvm || !valLlvm) return false;

    // Determine key mode: string (key_size == 0) vs generic.
    bool useGen = genericBackingType(dictTy, 0, /*isSet=*/false, nullptr) != nullptr;

    // Alloca for the drain cursor (i64, starts at 0).
    auto* cursorSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), cursorSlot);

    // Loop variable alloca: {K key; V* value} struct (same layout as
    // DictionaryEntry<K,V> in lowerDictEntryFor).
    llvm::StructType* entryStTy = llvm::StructType::get(llvm_, {keyLlvm, valLlvm});
    auto* varSlot = entryAlloca(entryStTy);
    locals_[&fs] = varSlot;

    // Alloca for temp key/val outputs from the runtime pop function.
    auto* keyOutSlot = entryAlloca(ptrTy());   // char** (str) or void** (gen)
    auto* keyLenSlot = entryAlloca(i64());     // int64_t (str key_len only)
    // fix #4499: the popped key's §4.6 storage width (string mode only).
    auto* keyWidSlot = entryAlloca(llvm::Type::getInt8Ty(llvm_));
    auto* valOutSlot = entryAlloca(ptrTy());   // void** for value

    auto* condBB = llvm::BasicBlock::Create(llvm_, "dd.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dd.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "dd.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dd.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: call bz_dict_drain_next_str / bz_dict_drain_next_gen.
    // Returns bool (i1): true = entry popped, false = drain complete.
    builder_.SetInsertPoint(condBB);
    llvm::Value* cursor = builder_.CreateLoad(i64(), cursorSlot);
    // We pass &cursor as a pointer (already in cursorSlot).
    llvm::Value* popped;
    if (!useGen) {
        // bz_dict_drain_next_str(dict, int64_t* cursor,
        //                        char** key_out, int64_t* key_len_out,
        //                        void** val_out) → bool
        auto* drainFn = declareC("bz_dict_drain_next_str", i1(),
                                 {ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy(),
                                  ptrTy()});
        popped = builder_.CreateCall(drainFn,
            {dictObj, cursorSlot, keyOutSlot, keyLenSlot, keyWidSlot,
             valOutSlot},
            "dd.popped");
    } else {
        // bz_dict_drain_next_gen(dict, int64_t* cursor,
        //                        void** key_out, void** val_out) → bool
        auto* drainFn = declareC("bz_dict_drain_next_gen", i1(),
                                 {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        popped = builder_.CreateCall(drainFn,
            {dictObj, cursorSlot, keyOutSlot, valOutSlot},
            "dd.popped");
    }
    builder_.CreateCondBr(popped, bodyBB, endBB);

    // body: decode key+val into the entry struct slot.
    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* rawKeyPtr = builder_.CreateLoad(ptrTy(), keyOutSlot);
        llvm::Value* rawValPtr = builder_.CreateLoad(ptrTy(), valOutSlot);

        // Decode key.
        llvm::Value* keyDecoded;
        using K = sema::TypeRef::Kind;
        bool isStringKey = keyTy->kind == K::Primitive &&
                           keyTy->primitive == sema::PrimitiveType::String;
        if (isStringKey) {
            // rawKeyPtr is a malloc'd char* (the key bytes; null-terminated).
            // Build a bz_string_t object from it — same pattern as
            // lowerDictEntryFor's string-key decode.
            auto* callocFn  = declareC("calloc", ptrTy(), {i64(), i64()});
            auto* i8Ty      = llvm::Type::getInt8Ty(llvm_);
            uint64_t strSz  = module_->getDataLayout().getTypeAllocSize(stringType());
            // rawKeyPtr is char*; key_len_out holds the CODE-POINT count and
            // key_w_out the storage width (fix #4499). The comment here used to
            // read "the byte length" — that was the #4499 defect stated in a
            // comment: the buffer is `len * width` bytes.
            llvm::Value* kl = builder_.CreateLoad(i64(), keyLenSlot);
            llvm::Value* kw = builder_.CreateLoad(i8Ty, keyWidSlot, "dd.kw");
            // The char* from bz_dict_drain_next_str is already a malloc'd copy
            // (bz_key_dup, which allocates `len * width` bytes PLUS a
            // width-sized zero terminator); use it directly as the string data
            // buffer. Pre-fix this site wrote a NUL at byte offset `kl` into
            // that buffer, which strndup had sized to strnlen(key)+1 — past
            // the allocation whenever the key held an embedded U+0000.
            // Allocate the bz_string_t header.
            llvm::Value* strObj = builder_.CreateCall(callocFn,
                {builder_.getInt64(1), builder_.getInt64((int64_t)strSz)});
            builder_.CreateStore(rawKeyPtr,
                builder_.CreateStructGEP(stringType(), strObj, 0));
            builder_.CreateStore(kl,
                builder_.CreateStructGEP(stringType(), strObj, 1));
            builder_.CreateStore(kw,
                builder_.CreateStructGEP(stringType(), strObj, 2));
            initStringRc(strObj);
            keyDecoded = strObj;
        } else if (useGen) {
            // Generic key: rawKeyPtr is a malloc'd copy of the key bytes.
            // Load the key value out of the raw bytes.
            keyDecoded = builder_.CreateLoad(keyLlvm, rawKeyPtr, "dd.kgen");
            // Free the raw bytes copy immediately (the value is loaded).
            auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn, {rawKeyPtr});
        } else {
            // Non-string, non-generic key — should not happen.
            keyDecoded = llvm::Constant::getNullValue(keyLlvm);
        }

        // Value: rawValPtr IS the owned class pointer (void* stored directly).
        // V is always a class (OwningDictionary<K,V> requires V: class), so
        // valLlvm == ptrTy(). No further decoding needed.
        llvm::Value* valDecoded = rawValPtr;

        // Store key+val into the loop variable struct.
        builder_.CreateStore(keyDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 0));
        builder_.CreateStore(valDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 1));
    }

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: just jump back to cond (cursor is already advanced inside the
    // drain runtime function).
    builder_.SetInsertPoint(incBB);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// fix #3468 / §29.2 / §29.7 / stdlib §4.12–§4.13 (v0.286): the ONE
// classification of a builtin-enumerable `for-in` source — see the header
// comment on `BuiltinForInSource` for why answering it twice was a leak
// generator. Extracted VERBATIM from `lowerIEnumerableBuiltinFor`, which is the
// authority of the pair: every arm below, and every comment on it, is that
// function's own text. `forInBuiltinEnumeratorNeedsFrameSlot` — the drifted
// copy — now reads this instead of re-deriving it.
//
// Pure inspection: no IR is emitted, so `collectSuspendLocals` may call it at
// frame-layout time, before any lowering has run.
Emitter::BuiltinForInSource
Emitter::classifyBuiltinForInSource(const ast::ForStmt& fs) {
    BuiltinForInSource k;
    if (!fs.iterable) return k;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return k;
    // §2.5 / §29.3 (issue #495): for a LOCALLY-declared iterable the name
    // comes from astNode; for an IMPORTED-type iterable (cross-module call
    // returning IEnumerable<T>) astNode is null and the name is in importedName
    // (set by ModuleLoader from the metadata namedName field).  Declaration must
    // be null in both cases — a non-null declaration means a user class whose
    // GetEnumerator is handled by lowerIEnumerableFor.
    if (iterTy->declaration) return k;
    std::string_view n;
    if (iterTy->astNode && iterTy->astNode->name &&
        !iterTy->astNode->name->parts.empty()) {
        n = iterTy->headName();
    } else if (!iterTy->importedName.empty()) {
        n = iterTy->importedName;
    } else {
        return k;
    }
    // Only handle the iteration-interface family.
    // fix #3468 (§29.2, v0.286): the six-name list is now a TABLE in the builtin
    // registry, not a comparison chain spelled here and again in the frame-slot
    // predicate — the shape umbrella #3419's grid ratchet exists to retire.
    if (!common::isBuiltinForInEnumerableHead(n)) return k;
    // Must have a generic arg to determine element type.
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return k;
    k.inFamily = true;
    k.head = n;
    k.iterTy = iterTy;

    // Determine if the value is a bz_array_t (variable/param backed by Array)
    // or an iterator state machine (result of a call to an iterator method).
    //
    // Heuristic: if the iterable is a CallExpr → assume iterator (vtable-backed).
    // If it's an identifier (parameter/local) → assume array-backed (bz_array_t*).
    // If it's a local variable declared as Array<T> → definitely array.
    //
    // This heuristic is correct for:
    //   for var x in Odds(10) do   ← CallExpr → vtable
    //   for var x in items do       ← identifier → array
    // Determine protocol based on expression kind:
    // - CallExpr → vtable dispatch (iterator state machine result)
    // - NewExpr → vtable dispatch (user class implementing IEnumerable)
    // - IdentifierExpr typed as concrete user class → vtable dispatch
    // - Everything else → array protocol (assumes bz_array_t*)
    //
    // Invariant (fix #1177 / §4.5 / §12.1): lowerLocalDecl wraps ANY
    // bz_array_t-backed collection (Array, OwningArray, OwningBuffer,
    // Stack/OwningStack, Queue/OwningQueue) and (Owned)LinkedList with
    // a vtable-backed IEnumerable object whenever the local is declared as
    // IEnumerable<T> (see LlvmBackend_Setup.cpp).  So when this path reaches
    // the vtable-dispatch branch below for an IEnumerable<T> local, the slot
    // already holds a proper vtable-backed wrapper and slot 4 (GetEnumerator)
    // is always valid — no longer limited to yield-iterator results.
    bool isArrayBacked = true;  // default: array protocol
    if (dynamic_cast<const ast::CallExpr*>(fs.iterable.get())) {
        isArrayBacked = false;  // iterator method call → vtable
    } else if (dynamic_cast<const ast::NewExpr*>(fs.iterable.get())) {
        isArrayBacked = false;  // new C() → vtable
    } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(fs.iterable.get())) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        // Helper: is `rt` a concrete user class (has a declaration pointer)?
        auto checkNamed = [&](sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named && rt->declaration;
        };
        // §29.2 (#437): a parameter/local typed as the pure `IEnumerable<T>`
        // interface uses vtable dispatch. buildOrderedArgs wraps any array-
        // backed collection argument (Array, OwningArray, OwningBuffer,
        // Stack/OwningStack, Queue/OwningQueue, LinkedList/OwningLinkedList)
        // at the call site, and lowerLocalDecl (#1177) wraps them at the
        // local-variable initializer site, so every IEnumerable<T>-typed
        // identifier always holds a proper vtable-backed wrapper object.
        // Richer interfaces (IReadOnlyList etc.) also support indexer access
        // and are NOT wrapped; they keep the array-backed protocol.
        //
        // fix #3187 / §29.2 (v0.254): the pre-#3187 `rt->astNode` guard
        // excluded every TypeRef reconstructed from cross-module metadata
        // (no AST exists for an imported builtin interface). An imported
        // `IEnumerable<T>`-typed local holding a yield-iterator result
        // (the dominant cross-module shape, the issue's Shape B) was then
        // classified as array-backed, the bz_array_len read landed at the
        // iterator frame's state-index offset (= 8, after the vtable
        // pointer), initially 0 → `0 < 0` false, body never runs, exit 0
        // (the "yields NOTHING" symptom the issue pins). Resolve the head
        // across AST and importedName — both channels the n-computation
        // above (line ~6666) already handles for the iterable expression
        // itself, so the same widening here keeps the iterable-EXPRESSION
        // and the local-DECL sites in lockstep (a misclassification on one
        // side would be exactly the inconsistent-state defect class the
        // #552/#1177/#2154 wrapper-invariants document).
        auto checkPureIEnumerable = [&](sema::TypeRef* rt) -> bool {
            if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
            if (rt->declaration) return false;  // user class → handled by checkNamed
            std::string_view head;
            if (rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty())
                head = rt->headName();
            else if (!rt->importedName.empty())
                head = rt->importedName;
            // fix #3419 (§29.2, v0.283): read the shared head classifier.
            return common::isSyncIterFormatEnumerableHead(head);
        };
        if (auto* par = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (par->declType) {
                auto* rt = ctx_.resolvedType(*par->declType);
                if (checkNamed(rt) || checkPureIEnumerable(rt))
                    isArrayBacked = false;
            }
        } else if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
            if (checkNamed(loc->declType) || checkPureIEnumerable(loc->declType))
                isArrayBacked = false;
        } else if (dynamic_cast<sema::FieldSymbol*>(sym) ||
                   dynamic_cast<sema::PropertySymbol*>(sym)) {
            // §29.2 / §28.5 (#1642): a bare field/property source (`for var x in
            // seq` where `seq` is an owned field or a property) holds a vtable-
            // backed iterator object for the pure iterator interfaces
            // IEnumerable<T> / IOwningEnumerable<T> — the same coercion invariant
            // (#1310) that makes a local/param vtable-backed applies to storage
            // members. Without this the source defaults to the bz_array protocol
            // and reads the iterator frame as an array (a 0-element silent
            // miscompile). Richer interfaces (IReadOnlyList, etc.) keep the
            // indexer-based array protocol, exactly as for a local/param.
            if (common::isSyncIterFormatEnumerableHead(n))
                isArrayBacked = false;
        }
    } else if (dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get())) {
        // §29.2 / §28.5 (#1642): an explicit `this.seq` / `obj.seq` field or
        // property read — the same object-owned, vtable-backed iterator source as
        // the bare-identifier field/property case above (exprSymbol resolves the
        // member access to its FieldSymbol / PropertySymbol).
        sema::Symbol* sym = ctx_.exprSymbol(*fs.iterable);
        if ((dynamic_cast<sema::FieldSymbol*>(sym) ||
             dynamic_cast<sema::PropertySymbol*>(sym)) &&
            common::isSyncIterFormatEnumerableHead(n))
            isArrayBacked = false;
    } else if (dynamic_cast<const ast::IndexExpr*>(fs.iterable.get())) {
        // fix #3356 / §29.2 / §23.4 (v0.278): an INDEXER read `obj[i]` whose
        // declared return type is `IEnumerable<T>` / `IOwningEnumerable<T>` —
        // the EXPRESSION-BODIED indexer inline path (LlvmBackend_Assign.cpp
        // `lowerIndexerGet`, the §23.4 inline body lowering) now wraps the
        // raw backing collection (LinkedList / Set / user-class
        // IEnumerable) into a vtable-backed enumerator via
        // `wrapPropertyArrayAsIEnumerable` (#3356's fix); a BLOCK-BODIED
        // indexer was already wrapped via `wrapAsIEnumerableForTarget` on its
        // RETURN edge (#2703's family). Either way the slot now holds a
        // proper vtable-backed wrapper (first 8 bytes = vtable pointer, NOT
        // a `bz_array_t.data`), and the array-backed protocol
        // (`bz_array_len` / `bz_array_ptr_at`) would read past the wrapper
        // and out of bounds — measured as a global-buffer-overflow on
        // `bz$llenum$i32$vtable` (the wrapper's slot-0 = vtable, mis-read
        // as `data`, then `bz_array_ptr_at(vtable, 0, 8)` reads slot 5 of
        // the 5-slot vtable). Pre-#3356 the array-backed path was the only
        // safe dispatch (the raw backing was a `bz_array_t*` /
        // `bz_llist_t*`, whose first word is count / node-pointer — both
        // would also mis-dispatch, but to a heap-buffer-overflow rather
        // than a global-buffer-overflow; either way, SIGSEGV-class). The
        // detection reads `iterTy` — the iterable EXPRESSION's own static
        // type, computed at the head as `ctx_.exprType(*fs.iterable)` — and
        // gates on the head-name being `IEnumerable` / `IOwningEnumerable`
        // (the same head-name gate the MemberAccessExpr arm above uses, and
        // the same gate the wrap helpers themselves key on; an
        // `IReadOnlyList<T>` / `IList<T>` iterable stays array-backed so
        // its indexer protocol still works). A NON-`IEnumerable<T>`
        // indexer return (the standard `property [i: int]: T` shape, the
        // `Object`-returning indexer, etc.) is intentionally NOT widened —
        // its array protocol is correct.
        if (common::isSyncIterFormatEnumerableHead(n))
            isArrayBacked = false;
    }
    k.isArrayBacked = isArrayBacked;
    return k;
}

bool Emitter::lowerIEnumerableBuiltinFor(const ast::ForStmt& fs) {
    // fix #3468 (§29.2 / §29.7, v0.286): the family gate and the array-vs-vtable
    // protocol decision both come from the shared classifier above, so
    // `forInBuiltinEnumeratorNeedsFrameSlot` (which reserves the §29.7 frame
    // field this function wires) cannot disagree with what is lowered here.
    BuiltinForInSource k = classifyBuiltinForInSource(fs);
    if (!k.inFamily) return false;
    std::string_view n = k.head;
    sema::TypeRef* iterTy = k.iterTy;

    llvm::Value* enumObj = lowerExpr(*fs.iterable);
    if (!enumObj) return false;

    // fix #3537 (§4.7 / §29.2, v0.283): the CONSUMER face of the element-type
    // substitution `emitIteratorMethod` makes on the producer side. `elemTy` here
    // types the `Current` (vtable slot 3) callee AND the store into the loop
    // variable's slot; the producer emits slot 3 returning the one-byte
    // placeholder for a `()` element, so reading `void` here built a
    // `void (ptr)` callee type and then stored a void value — MEASURED SIGILL in
    // `DataLayout::getABITypeAlign` under `CreateAlignedStore`. Both faces must
    // substitute identically or producer and consumer disagree about slot 3.
    llvm::Type* elemTy = unitStorageTy(lowerTypeRef(iterTy->genericArgs[0]));
    bool isArrayBacked = k.isArrayBacked;

    if (isArrayBacked) {
        // Use direct bz_array_t protocol.
        // per §9: a record element type lowers to an LLVM StructType whose
        // allocation size may exceed 8 bytes; use DataLayout for structs
        // so the bz_array_ptr_at stride matches the actual element layout
        // (same fix as collectionElemSize in LlvmBackend_Calls.cpp).
        int64_t esz = 8;
        if (elemTy->isIntegerTy(32) || elemTy->isFloatTy()) esz = 4;
        else if (elemTy->isIntegerTy(1) || elemTy->isIntegerTy(8)) esz = 1;
        else if (elemTy->isIntegerTy(16)) esz = 2;
        else if (elemTy->isStructTy())
            esz = (int64_t)module_->getDataLayout().getTypeAllocSize(elemTy);
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        llvm::Value* len = builder_.CreateCall(lenFn, {enumObj});
        auto* idxSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxSlot);
        auto* varSlot = entryAlloca(elemTy);
        locals_[&fs] = varSlot;
        registerForLoopSpillSlots(fs, {idxSlot, varSlot});  // #747 R42
        auto* condBB = llvm::BasicBlock::Create(llvm_, "ib.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ib.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "ib.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "ib.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* idx2 = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* ep = builder_.CreateCall(ptrAtFn, {enumObj, idx2, builder_.getInt64(esz)});
        llvm::Value* elem = builder_.CreateLoad(elemTy, ep);
        builder_.CreateStore(elem, varSlot);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        lowerForBody(fs);
        loopStack_.pop_back();
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(incBB);
        builder_.SetInsertPoint(incBB);
        llvm::Value* oldIdx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateStore(builder_.CreateAdd(oldIdx, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return true;
    }

    // §29.11 (#943): obtain a FRESH, INDEPENDENT enumerator from the source by
    // calling GetEnumerator() (vtable slot 4) — the source value (e.g. the
    // `IEnumerable<int>` produced by a yield method) is the IEnumerABLE, not the
    // enumerator. The loop advances and disposes ITS OWN enumerator; the source is
    // left untouched so a second `for` loop over the same value replays from the
    // start instead of dereferencing the exhausted-and-freed first enumerator (the
    // use-after-free SIGSEGV this fix targets). Every sync IEnumerable vtable
    // producer (yield iterator, array wrapper, user-class wrapper) now carries a
    // GetEnumerator at slot 4 that returns a distinct fresh enumerator, so this
    // slot-4 read is always in-bounds and always yields an independent enumerator.
    //
    // Source ownership: the source value (the IEnumerABLE) is disposed exactly
    // once at its own lifetime end — at scope end for a named local/param
    // (registered in lowerLocalDecl, §29.11), or here as a one-shot temporary when
    // the for-in iterates a freshly produced value (a CallExpr/NewExpr result with
    // no owning binding). The per-loop enumerator is always disposed below.
    auto* srcSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumObj, srcSlot);
    {
        llvm::Value* srcVt = builder_.CreateLoad(ptrTy(), enumObj);
        llvm::Value* geSlotPtr =
            builder_.CreateGEP(ptrTy(), srcVt, {builder_.getInt32(4)});
        llvm::Value* geFnPtr = builder_.CreateLoad(ptrTy(), geSlotPtr);
        auto* geTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
        enumObj = builder_.CreateCall(geTy, geFnPtr, {enumObj}, "ib.getenum");
        // §29.8 / §29.11 (#2154, #973a): GetEnumerator RAISES. When the frame's
        // owned captures are affine — the state-keyed yield-transfer shape
        // (#2154) or a conditionally-adopted `owned` parameter (#973a) — the
        // first enumerator CLAIMS them and a second GetEnumerator sets a pending
        // InvalidOperationException and returns NULL rather than replaying
        // pointers the first enumerator already consumed. Without this standard
        // post-call gate the null flowed straight into the MoveNext vtable load
        // below and the intended loud exception became a SIGSEGV. The check is
        // the ordinary CG20 gate: a no-op on every non-raising GetEnumerator.
        //
        // Placed BEFORE the loop-owned-source registration below on purpose, and
        // that cannot leak the source: the claim raises only on a SECOND
        // GetEnumerator over the SAME source object, which requires the source to
        // be a NAMED binding (a one-shot `for … in Gen(…)` temporary is
        // enumerated exactly once by construction) — and a named source is
        // disposed at its own scope end, not here.
        emitPendingCheck();
    }

    // Store the (fresh) enumerator in a slot.
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumObj, enumSlot);

    // §29.7 (#944): when this for-in is inside a yielding iterator, the
    // enumerator is an owned resource scoped to the loop body — like a `using
    // var`. If the enclosing iterator is abandoned (Dispose) while suspended at a
    // yield INSIDE this loop, the enumerator is still live and the state-machine
    // Dispose must dispose it (§29.7: "Dispose on every using var active at the
    // point of suspension"). To make it reachable from the Dispose function —
    // which does not re-lower the loop body — it round-trips through a DEDICATED
    // frame field (reserved by collectSuspendLocals, keyed on the iterable node),
    // NOT only the alloca-keyed spill area Dispose cannot reach. Wire the
    // enumerator alloca to that field (smAllocaToField_ → the per-yield spill
    // saves/restores it) and record slot→field in smIterEnumFrameSlot_ so the
    // yield snapshot logs this frame's cleanup action into smIterCleanupAtState_
    // (fix #1567; issue 3579) for every suspend state inside the loop. The
    // for-in's OWN loop-exit disposal (emitDisposals at
    // endBB) and break/return paths dispose it on a normal exit; the state-machine
    // Dispose disposes it ONLY when suspended inside the loop — exactly once on
    // every exit (discriminated by the suspend state).
    bool enumHasFrameField = false;
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(fs.iterable.get());
        if (fit != smLocalFields_.end()) {
            smAllocaToField_[enumSlot] = fit->second;
            smIterEnumFrameSlot_[enumSlot] = fit->second;
            enumHasFrameField = true;
        }
    }

    // §29.7 (#623): register the enumerator for disposal on all exit paths
    // (break / return / exception / normal exhaustion).  Use a dedicated scope
    // around the loop so the loop body / `continue` reads from slots owned by
    // MY scope, not the enclosing one (the wrapper/enumerator must outlive
    // every continue). The iterator vtable has Dispose at slot 1 (not slot 2
    // like class vtables), so we use iterEnumDispose rather than dynamicDispose.
    disposeStack_.emplace_back();
    {
        Disposal d{enumSlot, nullptr};
        d.iterEnumDispose = true;
        disposeStack_.back().push_back(d);
    }
    // §29.11 (#943): a freshly produced source value with no owning binding (a
    // CallExpr/NewExpr temporary, e.g. `for var x in P.Seq() do`) is owned by this
    // loop — dispose it once when the loop ends, alongside the enumerator. A named
    // local/param source (an IdentifierExpr) is disposed at its own scope end, so
    // it is NOT disposed here (that would double-free).
    // §29.2 / §28.5 (#1642): a for-in source that is a NAMED owned binding — a
    // local/param, or a field/property owned by the enclosing object — is
    // disposed at ITS OWN lifetime end (the local's scope end, or the object's
    // owned-field cascade, §28.5), NOT here. Per §29.2 the loop BORROWS such a
    // source (pins it, `using var __source` becomes a no-op borrow) and does not
    // assume its disposal obligation; only a freshly produced temporary (a
    // CallExpr/NewExpr result with no owning binding) is loop-owned and disposed
    // here. exprSymbol resolves both a bare identifier (`seq`) and a member
    // access (`this.seq` / `obj.seq`) to the underlying binding symbol, so a
    // field/property source (whether spelled bare or `this.`-qualified) is
    // correctly recognized and NOT double-freed against the field cascade.
    // #1840 / §29.2 / §28.5: a NAMED source — a local/param/field/auto-property —
    // is disposed at its own lifetime end, NEVER by the for-in. A COMPUTED
    // property getter (`public property P: T => expr;`) is invoked at the
    // call site and its result is a fresh value (owned when T is owning),
    // exactly like a method call — the for-in BORROWS nothing and must dispose
    // the produced source frame once at loop end. Classify via hasAstAutoPropertyDecl
    // (an auto-property backed by a compiler-generated field is the OWNED
    // NAMED case; every other property — expression-body, block-body,
    // expression-bodied `=>` — is the COMPUTED, fresh-value case).
    // fix #2485 (§9.2/§22.8/§29.2, v0.235): sema::hasAstAutoPropertyDecl is
    // DECL-DEPENDENT and always false for an IMPORTED PropertySymbol, so an
    // imported auto-property source was misclassified as COMPUTED — the loop
    // then disposed a source it does not own. CONFIRMED live heap-use-after-
    // free (a second `for x in h.Prop` read the source through the same,
    // now-freed, pointer the first loop's erroneous disposal reclaimed).
    // isAutoPropertyStorageSymbol (sema/Symbol.hpp — the Emitter copy was
    // DELETED by #3152's unification) is the SAME
    // predicate #2478 already built for the storage-layout family: local
    // decl-based check first (byte-for-byte unchanged), wire-carried fallback
    // for a decl-null imported Property.
    bool sourceIsNamedLocal = false;
    if (sema::Symbol* sym = ctx_.exprSymbol(*fs.iterable)) {
        if (dynamic_cast<sema::LocalSymbol*>(sym) != nullptr ||
            dynamic_cast<sema::ParameterSymbol*>(sym) != nullptr ||
            dynamic_cast<sema::FieldSymbol*>(sym) != nullptr) {
            sourceIsNamedLocal = true;
        } else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(sym)) {
            sourceIsNamedLocal = sema::isAutoPropertyStorageSymbol(ps, ctx_);
        }
    }
    // fix #2779 / §29.2 / §29.11 / §23.3 / §28.3 K3 / §28.9 (v0.240): an
    // INTERFACE-dispatched computed `IEnumerable<T>`-family property read —
    // `for var x in s.E do ... end;` where `s: ISeq` and `ISeq.readonly
    // property E: IEnumerable<T>;` is body-less — is statically misclassified
    // as an auto-property storage by the `sema::isAutoPropertyStorageSymbol`
    // arm above (the interface's `PropertyDecl` records `bodyKind == Auto`
    // because the interface declaration has no accessor body), so the for-in
    // refuses to register a loop-end dispose. The runtime is the opposite: the
    // wrap-creator (`wrapAsIEnumerableForTarget` on the BLOCK-bodied getter's
    // RETURN edge, or `wrapPropertyArrayAsIEnumerable` on the EXPRESSION-BODIED
    // read site) fires UNCONDITIONALLY for any iterator-format-typed property
    // read, regardless of the implementer (Array/Stack/Queue/Set/LinkedList/
    // user-class IEnumerable<T>), and the wrapper frame is a fresh heap
    // object the reader must reclaim. Widen the verdict to
    // `!sourceIsNamedLocal` for the interface-dispatched iterator-format case
    // so the loop-end slot-1 iterEnumDispose registers; the predicate is
    // sound for that family (it is the only family the wrap-creator touches)
    // and preserves the #2174 borrow-pin for non-iterator-format interface
    // properties. The opposite-direction narrowing for the bound-spelling
    // (`var e := s.E; for x in e do ...`) is the symmetric change in
    // `registerIterableSourceDisposal` below — the local claims ownership
    // there because the wrap happened at the read site, not the local-decl
    // site. // per #2779, #2703, #2815, #2174
    if (sourceIsNamedLocal &&
        isInterfaceDispatchedIteratorRead(fs.iterable.get()))
        sourceIsNamedLocal = false;
    if (!sourceIsNamedLocal) {
        Disposal ds{srcSlot, nullptr};
        ds.iterEnumDispose = true;
        disposeStack_.back().push_back(ds);
        // §29.7 (#944): the loop-owned source IEnumERABLE is a second implicit
        // frame live across a suspend inside the loop (independent of the
        // enumerator clone, §29.11). Wire it to its dedicated frame field
        // (forInSrcFields, reserved by collectSuspendLocals) so the enclosing
        // iterator's Dispose disposes it too when abandoned mid-loop — else the
        // source frame leaks even though its using-vars were freed via the
        // enumerator's Dispose. A named-local source is NOT wired here: it is
        // disposed at its own scope end via the using/local machinery.
        if (enumHasFrameField) {
            auto sit = smForInSrcFields_.find(fs.iterable.get());
            if (sit != smForInSrcFields_.end()) {
                smAllocaToField_[srcSlot] = sit->second;
                smIterEnumFrameSlot_[srcSlot] = sit->second;
            }
        }
    }

    // Use vtable-based MoveNext (slot 2) and Current (slot 3).
    // Layout: [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current
    auto* moveNextTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);

    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;
    // §29.7 (#944): when the enumerator round-trips through its OWN dedicated
    // frame field (enumHasFrameField — wired into smAllocaToField_ above so the
    // per-yield spill saves/restores it and Dispose can reach it), it must NOT
    // also be registered in the alloca-keyed spill area — that would create two
    // independent round-trip mechanisms for the same slot (the spill-area copy
    // would shadow the frame-field copy the Dispose path reads). The loop var is
    // unaffected and keeps the spill-area round-trip.
    if (enumHasFrameField)
        registerForLoopSpillSlots(fs, {varSlot});            // #747 R42
    else
        registerForLoopSpillSlots(fs, {enumSlot, varSlot});  // #747 R42

    // §28.3 / §29.2 (#895): for an `IOwningEnumerable<T>` the loop variable OWNS
    // each yielded element — sema requires it to be discharged on a normal body
    // exit (a `for var x in Gen() do end` with no discharge is a compile error).
    // On the EXCEPTION path, though, the current element must be auto-disposed
    // implicitly (§28.3). Register a null-guarded unwindOnly disposal for the
    // loop variable's slot so a throw out of the body disposes the current
    // element exactly once. The enumerator's own Dispose (registered above) frees
    // the iterator's remaining state, not the already-yielded current element, so
    // there is no double-free. Null-init the slot so a throw in the first
    // MoveNext (before any element is read) finds null and skips; an explicit
    // discharge in the body nulls the slot → skip; on normal exhaustion the
    // unwindOnly disposal skips (every element was discharged in the body).
    if (n == "IOwningEnumerable" && elemTy->isPointerTy() &&
        !disposeStack_.empty()) {
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), varSlot);
        Disposal dv{varSlot, nullptr};
        dv.unwindOnly = true;
        // fix #3604 (§29.7 / §28.3 / §29.2, v0.286): also flag this entry as
        // `forInAutoDrop` so the §29.7 abandonment snapshot (which skips
        // `unwindOnly` entries by design — see the snapshot's "§29.7 disposes
        // using-var only" comment) picks it up. The auto-drop is per-iteration
        // (§29.2) and the suspension point is one of the §29.7 edge cases that
        // §29.7 replays "as if the body had exited at the suspension point";
        // without this flag the auto-drop never runs on abandonment and the
        // element leaks (6400 B / 400 allocations, measured on HEAD before
        // this fix). The `unwindOnly` setting is kept for endBB / break /
        // throw / return — every one of those is an isUnwind=true
        // emitDisposals path which the `unwindOnly` branch already handles
        // (verified with `for var x in seq do break; end;` loop-amplified to
        // 400 iters: 0 leaked both before and after, so no behavior change
        // there). The snapshot is the only consumer that reads
        // `forInAutoDrop`.
        dv.forInAutoDrop = true;
        disposeStack_.back().push_back(dv);
        // fix #3604 (§29.7 / §29.3 / §29.2, v0.286): also wire the
        // loop variable's slot into the §29.7 frame field keyed on `&fs` (the
        // same wiring the drain-for-in arm above (line ~7323) and the
        // await-for arm (line ~9006) make for their loops). WITHOUT this,
        // smLocalFields_ reserves a field for the loop var but no producer
        // writes it, so the iterator's Dispose loads garbage into its
        // restored `locals_[&fs]` slot — the disposal entry above then
        // null-checks and skips, which is exactly the 6400 B / 400 alloc leak
        // #3604 measures. (The slot is also in `asyncHandlerSpillSlots_`
        // via `registerForLoopSpillSlots`, but the spill area is heap-freed
        // at Dispose time without restoring its values into `locals_[&fs]` —
        // the frame-field copy is the one the iterator's Dispose reads.)
        // Idempotent with `smAllocaToField_[varSlot]` being absent — the
        // loop-amplified repro goes from 6400 B / 400 alloc to 0 with this
        // line.
        if (smStateStruct_ && smSelf_) {
            auto fit = smLocalFields_.find(&fs);
            if (fit != smLocalFields_.end())
                smAllocaToField_[varSlot] = fit->second;
        }
    }

    auto* condBB = llvm::BasicBlock::Create(llvm_, "ib.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ib.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "ib.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: call vtable slot 2 (MoveNext)
    builder_.SetInsertPoint(condBB);
    llvm::Value* obj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* mnSlotPtr = builder_.CreateGEP(ptrTy(), vtable, {builder_.getInt32(2)});
    llvm::Value* mnFnPtr = builder_.CreateLoad(ptrTy(), mnSlotPtr);
    llvm::Value* moved = builder_.CreateCall(moveNextTy, mnFnPtr, {obj}, "ib.moved");
    // §29.2/§31 (#630): MoveNext runs the iterator body; an exception thrown
    // there sets bz$exc and returns false (propagateBB in MoveNext).  Without
    // this pending-check the consumer never sees the exception and the for-loop
    // silently ends.  emitPendingCheck is the standard CG20 post-call gate:
    // if bz$exc is non-null, unwind (disposing the enumerator via the scope
    // on the disposeStack_) and propagate to the enclosing handler.
    emitPendingCheck();
    builder_.CreateCondBr(moved, bodyBB, endBB);

    // body: call vtable slot 3 (Current for IEnumerable / TakeCurrent for
    // IOwningEnumerable — §29.1 v0.161 #1349). emitPendingCheck() after the
    // call handles the IOE that TakeCurrent raises on protocol misuse (double-
    // take etc.); for IEnumerable Current the check is a harmless no-op.
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* obj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable2 = builder_.CreateLoad(ptrTy(), obj2);
    llvm::Value* curSlotPtr = builder_.CreateGEP(ptrTy(), vtable2, {builder_.getInt32(3)});
    llvm::Value* curFnPtr = builder_.CreateLoad(ptrTy(), curSlotPtr);
    llvm::Value* curVal = builder_.CreateCall(currentTy, curFnPtr, {obj2}, "ib.cur");
    emitPendingCheck();  // TakeCurrent may raise IOE (#1349)
    builder_.CreateStore(curVal, varSlot);

    // §29.7 (#623) / fix #2556 (v0.236): scopeDepth = MY scope's index (after
    // push), not outerDepth (before push). The prior `outerDepth` form made
    // emitLoopDisposals on `continue` walk the loop's wrapper/enumerator scope
    // too — `iterEnumDispose` nulls the enumSlot, then condBB re-loads via
    // `Load(enumSlot)` and dereferences NULL → SEGV. Matches the existing
    // lowerIEnumerableForSortedSet (#2553): `disposeStack_.size()` here, since
    // the scope was just pushed via `disposeStack_.emplace_back()` a few
    // lines up — the current size IS the new scope's index. The wrapper/
    // enumerator resources must outlive every continue; `break` / normal
    // exhaustion still dispose them via the endBB / outer-scope paths.
    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    lowerForBody(fs);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(condBB);

    // Normal exhaustion: endBB. Dispose the enumerator (Dispose runs the
    // pending finally blocks and frees the frame), then pop the scope.
    builder_.SetInsertPoint(endBB);
    emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return true;
}


bool Emitter::lowerIEnumerableFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;
    auto* ts = iterTy->declaration;

    // fix #2553 (stdlib §4.18, v0.236): `for var x in <SortedSet<T>>` — see
    // lowerDictBackedSetFor for the comparator-availability gate that defers
    // a non-int/float SortedSet here. The stdlib-synthesised GetEnumerator
    // iterates the dict's internal order[] insertion-order array; the
    // `emitMethodCall(getEnumSym, ...)` path below would yield insertion
    // order, violating §4.18's ascending-iteration contract. Route through
    // wrapSortedSetAsIEnumerable (#2550/#2546) so the for-in surface matches
    // the explicit `s.GetEnumerator()` surface on the same receiver.
    // NOTE: SortedSet<string> reaches this handler with iterTy->declaration
    // NULL (the registry treats SortedSet as a builtin name — the explicit
    // GetEnumerator path sees the same null declaration, see
    // LlvmBackend_Calls2.cpp:4800), so the SortedSet dispatch must NOT gate
    // on the same `if (!ts) return false` the user-class path below uses.
    if (iterTy->headName() == "SortedSet") {
        if (lowerIEnumerableForSortedSet(fs, iterTy))
            return true;
    }
    if (!ts) return false;  // builtin/interface without concrete class — skip

    // #1776 / §21.4 / §29.2: the source's STATIC type is a user INTERFACE that
    // (transitively) extends IEnumerable<T> — e.g. `interface IMyColl:
    // IEnumerable<int>`. Such an interface declares no GetEnumerator of its own,
    // so findMethodByName(ts, "GetEnumerator") below returns null and this
    // handler bailed, dropping the statement through to the codegen NYI (the
    // #1776 gap; sema §21.4 correctly admits the interface as iterable — #1775).
    // GetEnumerator is inherited from the *builtin* IEnumerable, which carries
    // no itable/vtable dispatch slot (unlike IComparable/IEquatable it is not a
    // synthesized itable-participating interface — SemaContext::
    // builtinInterfaceSym), so the object exposes no dynamic handle to it.
    //
    // Devirtualize when every class implementing this interface resolves
    // GetEnumerator to the SAME method (the common case: a single implementing
    // class, or a base class whose subclasses inherit its non-virtual
    // GetEnumerator). The runtime object is then necessarily an instance of
    // that class (or a subclass sharing the identical GetEnumerator), so a
    // direct static call on the interface-typed receiver is sound: rewrite `ts`
    // to the implementing class and fall into the ordinary concrete-class path
    // below (emitMethodCall issues the direct call; the receiver pointer IS-A
    // the class at runtime). If the implementers resolve GetEnumerator to two
    // or more DISTINCT methods, no single static call is sound — leave `ts` as
    // the interface so findMethodByName fails and the statement falls through to
    // the NYI (a compile error, never a miscompile); that truly-polymorphic
    // case needs itable dispatch over a builtin IEnumerable descriptor (#1776).
    if (ts->kind == ast::TypeDeclKind::Interface &&
        !findMethodByName(ts, "GetEnumerator")) {
        sema::MethodSymbol* uniqueGetEnum = nullptr;
        bool ambiguous = false;
        // fix #3197 / §29.2 / §2.8.2 (v0.254): also walk
        // `ctx_.importedSymbols()` — a class declared in a separate module
        // that is imported here was NEVER considered as a candidate
        // implementer, so the unique-implementer devirtualization below
        // would conclude "no implementer compiled" and return false (the
        // NYI fallthrough), instead of devirtualizing to the imported
        // class and proceeding through the (now-fixed) enumerator-
        // discovery path. The `ge != nullptr` gate answers correctly for
        // an imported method because `declareImportedFunctions` populates
        // an external-linkage stub under `methodFns_[ms]`.
        auto considerCandidate = [&](const sema::TypeSymbol* cand) {
            if (!cand || cand->kind != ast::TypeDeclKind::Class) return;
            std::vector<const sema::TypeSymbol*> supers;
            collectTypeAndBases(cand, supers);
            bool implementsIface = false;
            for (auto* s : supers) if (s == ts) { implementsIface = true; break; }
            if (!implementsIface) return;
            sema::MethodSymbol* ge = findMethodByName(cand, "GetEnumerator");
            if (!ge) return;  // abstract implementer with no compiled body
            if (!uniqueGetEnum) uniqueGetEnum = ge;
            else if (uniqueGetEnum != ge) { ambiguous = true; }
        };
        for (const auto& sym : ctx_.symbols()) {
            considerCandidate(dynamic_cast<const sema::TypeSymbol*>(sym.get()));
            if (ambiguous) break;
        }
        if (!ambiguous) {
            for (const sema::Symbol* sym : ctx_.importedSymbols()) {
                considerCandidate(dynamic_cast<const sema::TypeSymbol*>(sym));
                if (ambiguous) break;
            }
        }
        // #1779 / §21.4 / §29.2: when MULTIPLE classes implement the interface
        // with DISTINCT `GetEnumerator` methods (truly polymorphic), no single
        // static call on the interface-typed receiver is sound. Emit a specific
        // diagnostic rather than falling through to the generic codegen NYI so
        // users see the actual reason for the failure and the tracked issue.
        // Single-implementer / shared-base cases still devirtualize below.
        if (ambiguous || !uniqueGetEnum) {
            if (ambiguous) {
                ctx_.diagnostics().report(
                    Severity::Error, fs.span.begin,
                    "for-in over interface `" + std::string(ts->name) +
                    "` is polymorphic: implementing classes define distinct "
                    "`GetEnumerator` methods, and the builtin `IEnumerable<T>` "
                    "is not yet synthesized as an itable-participating interface "
                    "(#1779; §29.2 — would require runtime itable dispatch); "
                    "iterate a concrete class, or refactor the implementers to "
                    "share a `GetEnumerator`");
                // Report-only path — claim handled so the caller doesn't append
                // the generic "not yet implemented: codegen for this for
                // statement" diagnostic on top of ours. No IR was emitted; the
                // compilation is already a hard error and the rest of the
                // statement body is dead.
                return true;
            }
            return false;  // non-ambiguous miss (no implementer compiled) → NYI
        }
        ts = uniqueGetEnum->parent;  // devirtualize to the implementing class
    }

    // Find GetEnumerator on ts (walks base types).
    sema::MethodSymbol* getEnumSym = findMethodByName(ts, "GetEnumerator");
    if (!getEnumSym) return false;

    // Find the enumerator class. GetEnumerator's return type is typically
    // the builtin `IEnumerator<T>` (null declaration). We need the concrete
    // class that implements it — search all compiled TypeSymbols for a class
    // whose base types include a name starting with "IEnumerator".
    //
    // fix #3197 / §29.1 / §29.2 / §29.11 / §2.8.2 (v0.254): when
    // `getEnumSym` is an IMPORTED MethodSymbol, `getEnumSym->decl` is null
    // (`MemberSymbol::decl` is null for imports — the loader does not
    // re-parse body text for non-generic classes). The loader does record
    // the return type on the side table (`ctx_.importedReturnType`); reuse
    // it. Mirrors the parallel change in `wrapUserClassAsIEnumerable`
    // (LlvmBackend_Assign.cpp), and is the reason the `for var x in
    // importedColl` direct spelling at the issue's Shape B reaches the
    // §29.11 enumerator vtable when the cross-module class implements
    // IEnumerable<T>.
    auto* getEnumDecl = dynamic_cast<const ast::MethodDecl*>(getEnumSym->decl);
    sema::TypeRef* enumTyRef = nullptr;
    if (getEnumDecl && getEnumDecl->returnType) {
        enumTyRef = ctx_.resolvedType(*getEnumDecl->returnType);
    } else {
        enumTyRef = ctx_.importedReturnType(getEnumSym);
    }
    if (!enumTyRef) return false;

    // Try to get the concrete TypeSymbol from the return type's declaration.
    auto* enumTs = (enumTyRef->kind == sema::TypeRef::Kind::Named)
                   ? enumTyRef->declaration : nullptr;

    // If null (builtin IEnumerator<T>), search for a user class that
    // implements it by checking all compiled TypeSymbols.
    // If enumTs is null (builtin IEnumerator<T> interface), find the
    // concrete class structurally: any user class with both "MoveNext"
    // and ("Current" or a property) in its compiled methods.
    //
    // fix #2384 / §29.1/§29.11 (v0.235): check `ts` (the iterable source
    // class) itself FIRST — same sibling defect, same fix, as
    // `wrapUserClassAsIEnumerable` (LlvmBackend_Assign.cpp): a class may
    // legally implement BOTH `IEnumerable<T>` and `IEnumerator<T>` and return
    // `this` from `GetEnumerator()`. The old code unconditionally skipped
    // `candidate == ts`, so `for var x in selfIterInstance do ... end` (a
    // concrete-class-typed source, not routed through the interface wrapper)
    // fell through every `for`-handler to the generic "not yet implemented:
    // codegen for this for statement" diagnostic — confirmed via probe.
    if (!enumTs) {
        auto structurallyIsEnumerator = [&](const sema::TypeSymbol* candidate) {
            bool hasMoveNext = false, hasCurrent = false;
            for (auto* m : candidate->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (!ms || !methodFns_.count(ms)) continue;
                if (ms->name == "MoveNext") hasMoveNext = true;
                if (ms->name == "Current") hasCurrent = true;
            }
            // Also check PropertySymbol for Current
            if (!hasCurrent) {
                for (auto* m : candidate->members) {
                    auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                    if (ps && ps->name == "Current") { hasCurrent = true; break; }
                }
            }
            return hasMoveNext && hasCurrent;
        };
        if (structurallyIsEnumerator(ts)) {
            enumTs = const_cast<sema::TypeSymbol*>(ts);
        } else {
            // fix #3197 / §29.1 / §29.11 (v0.254): walk imported symbols too
            // — the same fix as `wrapUserClassAsIEnumerable`'s
            // (LlvmBackend_Assign.cpp). `ctx_.symbols()` excludes imported
            // symbols by construction (`makeImportedSymbol` keeps them
            // out), so a class declared in a separate module was NEVER
            // considered as the enumerator class; `enumTs` stayed null and
            // this handler bailed to the §21.4 NYI. The
            // `methodFns_.count(ms)` gate above already answers correctly
            // for an imported method: `declareImportedFunctions` populates
            // an external-linkage `llvm::Function` for every imported
            // MethodSymbol under `methodFns_[ms]`.
            auto scanSymbols = [&](const sema::Symbol* symPtr) -> bool {
                auto* candidate = dynamic_cast<const sema::TypeSymbol*>(symPtr);
                if (!candidate || candidate->kind != ast::TypeDeclKind::Class)
                    return false;
                if (candidate == ts) return false;  // skip the iterable itself
                if (!structurallyIsEnumerator(candidate)) return false;
                enumTs = const_cast<sema::TypeSymbol*>(candidate);
                return true;
            };
            for (const auto& sym : ctx_.symbols()) {
                if (scanSymbols(sym.get())) break;
            }
            if (!enumTs) {
                for (const sema::Symbol* sym : ctx_.importedSymbols()) {
                    if (scanSymbols(sym)) break;
                }
            }
        }
    }
    if (!enumTs) return false;  // couldn't find concrete enumerator class

    // Find MoveNext on the enumerator type.
    sema::MethodSymbol* moveNextSym = findMethodByName(enumTs, "MoveNext");
    if (!moveNextSym) return false;

    // Find Current — may be a PropertySymbol (readonly property) or MethodSymbol.
    sema::MethodSymbol* currentSym = findMethodByName(enumTs, "Current");
    sema::PropertySymbol* currentPropSym = nullptr;
    if (!currentSym) {
        for (auto* m : enumTs->members) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
            if (ps && ps->name == "Current") { currentPropSym = ps; break; }
        }
    }

    // Evaluate the iterable to get the source object pointer.
    llvm::Value* srcPtr = lowerExpr(*fs.iterable);
    if (!srcPtr) return false;

    // Call GetEnumerator() → enumerator object (ptr).
    std::vector<llvm::Value*> geArgs = {srcPtr};
    llvm::Value* enumPtr = emitMethodCall(getEnumSym, geArgs, false);
    if (!enumPtr) return false;

    // Store the enumerator in a slot so it can be disposed at loop exit.
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumPtr, enumSlot);

    // §29.2 / §28.3 (#972): push a dedicated disposal scope for the enumerator
    // so that break / early return / normal exhaustion / exception ALL call
    // Dispose() exactly once.  Mirror lowerIEnumerableBuiltinFor: record the
    // outer depth (used by loopStack_ so `break` disposes the enumerator scope
    // too), push a fresh scope, register the enumerator in it.  At normal-exit
    // (endBB) we call emitDisposals + pop_back, just like the builtin sibling.
    // Previously we added to disposeStack_.back() (the enclosing scope) which
    // caused break/exception to double-dispose (the enclosing scope already ran
    // it) AND normal exhaustion to never dispose (endBB had no emitDisposals
    // call) — see issue #972.
    //
    // fix #2384 / §29.1/§29.11 (v0.235): who disposes what when the class is
    // its OWN enumerator (enumTs == ts, the self-returning idiom — `enumPtr`
    // and `srcPtr` are then the SAME object). This scope's disposal reads the
    // object back through `enumSlot` (a dedicated alloca) and, on a non-null
    // slot, RECLAIMS the block (`emitDisposeAndReclaim`, LlvmBackend_Disposal.cpp)
    // — unlike the idempotent flag-guarded `Dispose()` call itself, reclaiming
    // physically frees the heap block. A NAMED iterable source (a local,
    // parameter, field, or auto-property) is independently owned and disposes
    // the SAME object again at ITS OWN scope end/using-var boundary — through
    // a DIFFERENT alloca that this scope's null-guard cannot see nulled, so
    // both sites would reclaim the identical block: a genuine double-free
    // (confirmed under ASan as a heap-use-after-free — the second reclaim's
    // vtable-pointer read lands in memory the first reclaim already freed).
    // A fresh TEMPORARY source (`for var x in new SelfIter(3) do ... end`) has
    // no independent owner, so this scope's registration remains its ONLY
    // disposal and must still fire.
    //
    // The `enumPtr == srcPtr` test is a RUNTIME compare, mirroring
    // `lowerAwaitFor`'s `afor.enown` guard (#1268/#2356): `enumTs == ts` is
    // only a STRUCTURAL fact (the class exposes MoveNext + Current), not a
    // proof that this particular `GetEnumerator()` body returns `this` — a
    // self-implementing class could instead construct and return a fresh
    // instance of its own type, in which case the two pointers differ and the
    // ordinary distinct-object disposal is correct and must not be skipped.
    // fix #2485 (§9.2/§22.8/§29.1, v0.235): isAutoPropertyStorageSymbol, not the
    // bare decl-dependent sema::hasAstAutoPropertyDecl — see the sibling fix a
    // few lines up in lowerIEnumerableBuiltinFor's plain sync path for the
    // confirmed heap-use-after-free this same misclassification produces.
    bool sourceIsNamedOwner = false;
    if (sema::Symbol* sym = ctx_.exprSymbol(*fs.iterable)) {
        if (dynamic_cast<sema::LocalSymbol*>(sym) != nullptr ||
            dynamic_cast<sema::ParameterSymbol*>(sym) != nullptr ||
            dynamic_cast<sema::FieldSymbol*>(sym) != nullptr) {
            sourceIsNamedOwner = true;
        } else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(sym)) {
            sourceIsNamedOwner = sema::isAutoPropertyStorageSymbol(ps, ctx_);
        }
    }
    llvm::AllocaInst* enumDisposeSlot = enumSlot;
    if (enumTs == ts && sourceIsNamedOwner) {
        llvm::Value* enumIsSrc =
            builder_.CreateICmpEQ(enumPtr, srcPtr, "ien.selfen");
        enumDisposeSlot = entryAlloca(ptrTy());
        builder_.CreateStore(
            builder_.CreateSelect(enumIsSrc,
                                  llvm::ConstantPointerNull::get(ptrTy()),
                                  enumPtr, "ien.enown"),
            enumDisposeSlot);
    }
    disposeStack_.emplace_back();
    // fix #2424 (§28 preamble/§28.9, §29.1, v0.235): a genuinely UNOWNED
    // temporary iterable source (`for var x in new IntRange(...) do ... end`
    // — the `new` result is bound to no other name anywhere) has no
    // independent disposer in the program; this loop is its SOLE owner and
    // must dispose it too, exactly as it already disposes the enumerator it
    // constructs. Only the TWO-CLASS idiom needs a SEPARATE registration
    // here (`enumTs != ts`): `srcPtr` is then a genuinely distinct object
    // from `enumPtr`. The self-returning idiom (`enumTs == ts`) needs no
    // separate entry — `enumPtr == srcPtr` at runtime, and the
    // `enumDisposeSlot` registration just below (identity-guarded to null
    // when `sourceIsNamedOwner`, or the plain `enumSlot` otherwise) already
    // disposes that single shared object exactly once. A NAMED source
    // (`sourceIsNamedOwner`) is disposed at ITS OWN scope end by the
    // ordinary local/param/field disposal machinery — registering it again
    // here would double-free. Mirrors `lowerIEnumerableBuiltinFor`'s
    // identical `!sourceIsNamedLocal` gate for the array/collection sibling
    // path (§29.2: "a freshly produced source value with no owning binding
    // ... is owned by this loop"). Pushed BEFORE the enumerator's own entry
    // so LIFO scope-end disposal (`emitDisposals` iterates in reverse) frees
    // the enumerator first, the source second — the natural reverse of their
    // construction order; the two never alias here (`enumTs != ts` is
    // exactly this branch's guard) so the order has no correctness
    // consequence, only readability.
    llvm::AllocaInst* srcDisposeSlot = nullptr;
    if (!sourceIsNamedOwner && enumTs != ts && disposerFns_.count(ts)) {
        srcDisposeSlot = entryAlloca(ptrTy());
        builder_.CreateStore(srcPtr, srcDisposeSlot);
        disposeStack_.back().push_back({srcDisposeSlot, ts});
    }
    if (disposerFns_.count(enumTs))
        disposeStack_.back().push_back({enumDisposeSlot, enumTs});

    // Determine the loop variable's element type.
    llvm::Type* elemTy = ptrTy();  // default: reference type
    if (currentSym) {
        auto* curDecl = dynamic_cast<const ast::MethodDecl*>(currentSym->decl);
        if (curDecl && curDecl->returnType) {
            if (auto* rt = ctx_.resolvedType(*curDecl->returnType))
                elemTy = lowerTypeRef(rt);
        }
    } else if (currentPropSym) {
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(currentPropSym->decl);
        if (pd && pd->type) {
            if (auto* rt = ctx_.resolvedType(*pd->type))
                elemTy = lowerTypeRef(rt);
        }
    }
    // Fall back to first generic arg of enumerator's return type.
    if (elemTy == ptrTy() && !enumTyRef->genericArgs.empty() && enumTyRef->genericArgs[0]) {
        llvm::Type* ga = lowerTypeRef(enumTyRef->genericArgs[0]);
        if (ga != ptrTy()) elemTy = ga;
    }

    // Loop variable slot — the body sees `x` here.
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;
    // fix #2424: srcDisposeSlot (null unless the two-class-idiom-owned-
    // temporary branch above registered it) must survive a suspend the same
    // way enumSlot already does — it is read back only at loop end (endBB),
    // which may be reached after a resume. registerForLoopSpillSlots
    // null-guards each slot itself, so passing a possibly-null entry is safe.
    registerForLoopSpillSlots(fs, {enumSlot, srcDisposeSlot, varSlot});  // #747 R42

    auto* condBB = llvm::BasicBlock::Create(llvm_, "ien.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ien.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "ien.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: __enum.MoveNext() → bool.
    builder_.SetInsertPoint(condBB);
    llvm::Value* enumObj = builder_.CreateLoad(ptrTy(), enumSlot);
    std::vector<llvm::Value*> mnArgs = {enumObj};
    llvm::Value* moved = emitMethodCall(moveNextSym, mnArgs, false);
    if (!moved) {
        // If MoveNext can't be resolved via vtable, fall back to false.
        moved = builder_.getFalse();
    }
    builder_.CreateCondBr(moved, bodyBB, endBB);

    // body: x := __enum.Current; then run the user body.
    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* enumObj2 = builder_.CreateLoad(ptrTy(), enumSlot);
        llvm::Value* curVal = nullptr;
        if (currentSym) {
            // Method-form Current.
            std::vector<llvm::Value*> curArgs = {enumObj2};
            curVal = emitMethodCall(currentSym, curArgs, false);
        } else if (currentPropSym) {
            // Property-form Current — evaluate the getter with enumObj2 as this.
            //
            // fix #3197 / §23 / §29.11 (v0.254): for an IMPORTED enumerator
            // class, `currentPropSym->decl` is null (PropertySymbol::decl is
            // never set for imports). The cross-module ABI is the `$propget`
            // stub declared by `declareImportedPropertyGetters`
            // (LlvmBackend_Setup.cpp ~L2633), which populates
            // `importedPropGetterFns_[ps]` — and it engages for this shape:
            // the `Current` here is non-virtual and class-parented, so
            // neither of `declareImportedPropertyGetters`' two skips
            // (virtual, interface-parented) applies. Mirrors the parallel
            // change in `wrapUserClassAsIEnumerable` (LlvmBackend_Assign.cpp).
            //
            // #3275 (§23 / §29.1+§29.2, v0.254): the loader's documented
            // invariant is that `PropertySymbol::decl` is null for every
            // IMPORTED PropertySymbol (ModuleLoader.cpp ~L891, the
            // `MetaMemberKind::Property` arm of `convertMember`, only sets
            // `parent`/`name`/`modifiers`/`scope`/`visibility`/side-table
            // entries; never `decl`). Verified empirically for every shape
            // the for-in driver can land on (see the sibling comment in
            // `wrapUserClassAsIEnumerable` at LlvmBackend_Assign.cpp ~L9782).
            //
            // The `#ifndef NDEBUG` invariant check below pins the invariant
            // here too. Gated on `ctx_.isImported(enumTs)` to match the
            // sibling pattern at LlvmBackend_Assign.cpp ~L9811 — a LOCAL
            // enumerator class DOES have `currentPropSym->decl != nullptr`
            // (its PropertySymbol carries a real `ast::PropertyDecl` with
            // accessor bodies the local `if (pd)` arm reads), so a debug
            // build must not trip this check on every single-module for-in
            // loop over a property-form `Current` (the twin half of this
            // regression test family). Any future loader change that
            // re-introduces `decl` on an IMPORTED PropertySymbol trips
            // `abort()` here in debug builds — caught at compile, not at
            // runtime.
#ifndef NDEBUG
            if (ctx_.isImported(enumTs) && currentPropSym->decl != nullptr) {
                llvm::errs() << "bezel codegen: imported-symbol invariant "
                                "violated (§23 / #3275): property `"
                             << currentPropSym->name
                             << "` of imported enumerator `"
                             << enumTs->name
                             << "` carries a non-null decl. The cross-module "
                                "getter thunk path below would compile against "
                                "the WRONG class layout (importer-side offsets, "
                                "exporter-side runtime object). Failing "
                                "compilation early so the loader regression "
                                "is caught at first repro, not at runtime.\n";
                abort();
            }
#endif
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(currentPropSym->decl);
            if (pd) {
                // Save/restore currentThis_.
                llvm::Value* savedThis = currentThis_;
                currentThis_ = enumObj2;
                // Try expression getter (get => expr).
                if (const ast::Expr* getterExpr = computedGetterOf(currentPropSym)) {
                    curVal = lowerExpr(*getterExpr);
                    // fix #2761 (§8.6 "Boxing" / §29.11, v0.240): the
                    // DUCK-TYPED enumerator's `Current` read. This is the same
                    // missing box as the ordinary property read, but its
                    // failure mode is the worst one found for this defect: the
                    // loop variable takes the raw scalar where the element type
                    // is `Object`, and the body's `v is int` drove it as a
                    // pointer — SIGSEGV, exit 139, reproduced on 0e554e3 with a
                    // hand-written `MoveNext`/`Current` pair whose `Current` is
                    // `property Current: Object => 7;`. The block-bodied twin
                    // (the `else if` just below) calls a compiled getter, which
                    // #2703 already made return a box.
                    if (pd->type)
                        curVal = boxForTarget(
                            curVal, ctx_.resolvedType(*pd->type), getterExpr);
                }
                // Try block getter (get => do...end).
                else if (const ast::PropertyAccessor* acc = blockGetterOf(currentPropSym)) {
                    llvm::Function* getterFn = ensurePropertyBlockGetter(pd, acc);
                    if (getterFn) {
                        auto* fnTy = getterFn->getFunctionType();
                        curVal = builder_.CreateCall(fnTy, getterFn, {enumObj2});
                        // §31.1/§31.2 (FINDING [4]): a throw inside the
                        // standalone block getter must propagate, not be
                        // swallowed — check the pending flag like a method call.
                        emitPendingCheck();
                    }
                }
                currentThis_ = savedThis;
            } else {
                // Imported enumerator class — call the cross-module
                // `$propget` stub. `declareImportedPropertyGetters` has
                // declared it as `bz$<Class>$<Current>$propget(ptr %this)
                // -> T`, which is the ABI the exporter emits in
                // `emitExportedPropertyGetters`. The stub call is the only
                // sound option here: there is no local block/expression
                // getter (the imported PropertySymbol carries no body), so
                // the `pd` branch above cannot resolve it.
                auto curIt = importedPropGetterFns_.find(currentPropSym);
                if (curIt != importedPropGetterFns_.end()) {
                    llvm::Function* getterFn = curIt->second;
                    auto* fnTy = getterFn->getFunctionType();
                    curVal = builder_.CreateCall(fnTy, getterFn, {enumObj2});
                    // §31.1/§31.2 (FINDING [4]): a throw inside the
                    // imported getter must propagate, not be swallowed —
                    // check the pending flag like a method call.
                    emitPendingCheck();
                }
            }
        }
        if (curVal) {
            // Coerce to elemTy if sizes differ.
            if (curVal->getType() != elemTy && elemTy->isIntegerTy() &&
                curVal->getType()->isIntegerTy())
                curVal = builder_.CreateTrunc(curVal, elemTy);
            if (curVal->getType() == elemTy)
                builder_.CreateStore(curVal, varSlot);
        }
    }

    // §29.7 (#972) / fix #2556 (v0.236): scopeDepth = MY scope's index (after
    // push), not outerDepth (before push). See the sibling comment in
    // lowerIEnumerableBuiltinFor: `outerDepth` made `continue` walk the
    // enumerator's slot to dispose, nulling it, and condBB's `Load(enumSlot)`
    // then dereferenced NULL → SEGV. Matches lowerIEnumerableForSortedSet's
    // existing `disposeStack_.size()` (after push). The enumerator/wrapper
    // remain loop-scope resources that must outlive every continue; `break` /
    // normal exhaustion still dispose them via the endBB path.
    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(condBB);

    // §29.2 / §28.3 (#972): normal exhaustion — dispose the enumerator and pop
    // the dedicated scope, mirroring lowerIEnumerableBuiltinFor's endBB handling.
    builder_.SetInsertPoint(endBB);
    emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return true;
}


// fix #2553 (stdlib §4.18, v0.236): `for var x in <SortedSet<T>>` — build the
// same sorted-snapshot wrapper the explicit `.GetEnumerator()` call site uses
// (wrapSortedSetAsIEnumerable, #2550/#2546), obtain the enumerator via the
// wrapper's slot-4 GetEnumerator (which returns a non-owning clone over the
// same sorted array), and dispatch MoveNext/Current through the enumerator's
// vtable (slot 2 / slot 3 — the standard iterator-vtable layout, identical to
// the Array/Stack/Queue/LinkedList enumerable wrappers the builtin path uses
// in lowerIEnumerableBuiltinFor). The stdlib-synthesised SortedSet.GetEnumerator
// iterates the dict's internal `order[]` insertion-order array, so the
// `emitMethodCall(getEnumSym, ...)` path lowerIEnumerableFor would otherwise
// take yields insertion order — diverging from the explicit-GetEnumerator
// surface on the same receiver (#2550 made the explicit surface ascending;
// this fix makes the for-in surface match).
//
// Disposal (#972 / §29.2): dedicated scope so break / early return / normal
// exhaustion / exception ALL dispose the enumerator AND the wrapper exactly
// once. LIFO at endBB — the wrapper (pushed first) is disposed LAST, freeing
// the sorted snapshot array AFTER the enumerator (pushed second) has freed
// its (non-owning) frame. Mirrors the explicit-GetEnumerator dispatch's
// "dispose wrapper after the enumerator clone" ordering (#2471 / §29.11) so
// the snapshot array stays live for the clone's full consumption — a dispose-
// first wrapper would free the array out from under the live enumerator's
// Current reads (confirmed UAF under ASan, see #2471).
bool Emitter::lowerIEnumerableForSortedSet(const ast::ForStmt& fs,
                                           const sema::TypeRef* iterTy) {
    if (!fs.iterable || !iterTy) return false;
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return false;
    const sema::TypeRef* elemTyRef = iterTy->genericArgs[0];
    llvm::Type* elemTy = lowerTypeRef(elemTyRef);
    if (!elemTy) return false;

    // Evaluate the iterable to get the source object pointer.
    llvm::Value* srcPtr = lowerExpr(*fs.iterable);
    if (!srcPtr) return false;

    // Snapshot + sort + wrap as the OWNING enumerable the #2550 wrap emits.
    // The wrapper's vtable:
    //   [0]=td, [1]=Dispose(frees sorted array, frees self),
    //   [2]=MoveNext, [3]=Current, [4]=GetEnumerator(non-owning clone)
    // — same shape wrapOwnedArrayAsIEnumerable produces.
    llvm::Value* wrapper = wrapSortedSetAsIEnumerable(srcPtr, iterTy);
    if (!wrapper || !wrapper->getType()->isPointerTy()) return false;

    // slot-4 GetEnumerator — returns a non-owning clone over the same array.
    llvm::Value* wt = builder_.CreateLoad(ptrTy(), wrapper);
    llvm::Value* geSlotPtr =
        builder_.CreateGEP(ptrTy(), wt, {builder_.getInt32(4)});
    llvm::Value* geFnPtr = builder_.CreateLoad(ptrTy(), geSlotPtr);
    auto* geTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    llvm::Value* enumPtr = builder_.CreateCall(
        geTy, geFnPtr, {wrapper}, "ssenum.getenum");

    // §29.7 (#623) / §28.3 (#972): dedicated disposal scope for both the
    // enumerator (non-owning — Dispose frees only its frame) and the wrapper
    // (owning — Dispose frees the sorted array then self). outerDepth saved
    // BEFORE the push so `loopStack_` records the enclosing depth for
    // break-path emitLoopDisposals.
    std::size_t outerDepth = disposeStack_.size();
    disposeStack_.emplace_back();

    // Push wrapper FIRST, enumerator SECOND — LIFO reverse iteration on
    // endBB runs the enumerator first (cheap frame-free, harmless because
    // non-owning), then the wrapper (frees the sorted array, safe because
    // no Current reads happen after the enumerator is gone). This matches
    // the explicit-GetEnumerator dispatch's "defer wrapper disposal AFTER
    // the enumerator clone" ordering (#2471).
    llvm::AllocaInst* wrapperSlot = entryAlloca(ptrTy());
    builder_.CreateStore(wrapper, wrapperSlot);
    {
        Disposal wd{wrapperSlot, nullptr};
        wd.iterEnumDispose = true;
        disposeStack_.back().push_back(wd);
    }
    llvm::AllocaInst* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumPtr, enumSlot);
    {
        Disposal ed{enumSlot, nullptr};
        ed.iterEnumDispose = true;
        disposeStack_.back().push_back(ed);
    }

    // Use vtable-based MoveNext (slot 2 → i1) and Current (slot 3 → T) — the
    // same iterator-vtable layout lowerIEnumerableBuiltinFor dispatches
    // through for Array/Stack/Queue/LinkedList enumerable wrappers. We can't
    // reuse lowerIEnumerableFor's emitMethodCall(moveNextSym, ...) path
    // because moveNextSym / currentSym are looked up on the SortedSet's
    // declared enumerator class, NOT on the wrapper's bz$arrenum$...$ clone
    // — the wrapper's class has its own MoveNext/Current with no sema
    // MethodSymbol accessible from this scope. Vtable dispatch is the
    // canonical escape (§29.2 vtable protocol).
    auto* moveNextTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);

    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;
    // NOTE: #747 R42 (state-machine spill) intentionally OMITTED for this
    // helper — SortedSet for-in inside a yield iterator is a separate
    // concern (#2553 follow-up; today's spec fix lands the simple synchronous
    // case first). Filing a follow-up issue.

    auto* condBB = llvm::BasicBlock::Create(llvm_, "ss.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ss.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "ss.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: enumerator.MoveNext() → i1 via vtable slot 2.
    builder_.SetInsertPoint(condBB);
    llvm::Value* obj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* mnSlotPtr =
        builder_.CreateGEP(ptrTy(), vtable, {builder_.getInt32(2)});
    llvm::Value* mnFnPtr = builder_.CreateLoad(ptrTy(), mnSlotPtr);
    llvm::Value* moved = builder_.CreateCall(
        moveNextTy, mnFnPtr, {obj}, "ss.moved");
    builder_.CreateCondBr(moved, bodyBB, endBB);

    // body: enumerator.Current → T via vtable slot 3, then run the user body.
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* obj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable2 = builder_.CreateLoad(ptrTy(), obj2);
    llvm::Value* curSlotPtr =
        builder_.CreateGEP(ptrTy(), vtable2, {builder_.getInt32(3)});
    llvm::Value* curFnPtr = builder_.CreateLoad(ptrTy(), curSlotPtr);
    llvm::Value* curVal = builder_.CreateCall(
        currentTy, curFnPtr, {obj2}, "ss.cur");
    builder_.CreateStore(curVal, varSlot);

    // §29.7 (#623) / #2553 (v0.236): scopeDepth = MY scope's index (after push),
    // not outerDepth (before push). The existing patterns in lowerIEnumerableFor
    // / lowerIEnumerableBuiltinFor use outerDepth so emitLoopDisposals runs the
    // body's iter-var scope + the loop's wrapper/enumerator scope on `break`;
    // that double-disposes the wrapper/enumerator on `continue` too — the
    // slots get nulled by iterEnumDispose and condBB's `Load(enumSlot)` reads
    // NULL → SEGV (a pre-existing latent bug the vtable-protocol paths inherit,
    // first exercised by #2553's SortedSet<string> for-in + `continue`).
    // Moving scopeDepth to MY scope's index makes emitLoopDisposals a no-op on
    // continue (the wrapper/enumerator stay live for the next MoveNext read)
    // and on break (endBB picks them up via emitDisposals(disposeStack_.back()).
    // The wrapper/enumerator are STILL disposed exactly once on every exit:
    //   * normal exhaustion (condBB false-branch → endBB) — emitDisposals.
    //   * break (lowerLoopJump → endBB) — emitDisposals.
    //   * early `return` / exception — the next-emitDisposals on the unwind
    //     path runs the scope unconditionally (§31.2), and `iterEnumDispose`
    //     nulls the slot after Dispose so a second pass is a no-op.
    //   * `continue` — slot stays live; loop body reuses the same enumerator.
    // This matches LinkedList's iter-var scope (its scopeDepth is the
    // current disposeStack size at push time, NOT before — see the original
    // for-in patterns); the wrapper/enumerator are loop-scope resources that
    // must outlive every continue.
    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(condBB);

    // Normal exhaustion: endBB. emitDisposals iterates the scope in REVERSE
    // so the enumerator (pushed second) is disposed FIRST (frees its
    // non-owning frame, harmless), then the wrapper (pushed first) is
    // disposed SECOND (frees the sorted array, then self). See #972 for
    // the disposal-protocol reasoning; mirrors lowerIEnumerableBuiltinFor's
    // endBB handling exactly.
    builder_.SetInsertPoint(endBB);
    emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return true;
}


// §30.7: lower `await for var x in src do ... end`. The source is an
// IAsyncEnumerable<T> / IAsyncEnumerator<T>; for an async-iterator method call
// (`Gen()`) the value IS the enumerator (the generated frame, whose vtable has
// [1]=Dispose, [2]=MoveNextAsync, [3]=Current). Each iteration: call
// MoveNextAsync → Task<bool>, AWAIT it (a real suspend point in the consuming
// async method's own state machine, reusing emitAwaitSuspend), and on a true
// result read Current and run the body. The enumerator is disposed at loop exit.
//
// The enumerator pointer is kept as an SSA value computed once before the loop;
// because the loop head (which uses it) is re-entered by the consuming method's
// resume dispatch across each iteration's await suspend, fixupSuspendDominance
// promotes it into the heap frame so it survives a real cross-thread resume —
// exactly the mechanism the embedded-await pre-suspend operands already rely on.
bool Emitter::lowerAwaitFor(const ast::ForStmt& fs) {
    if (!fs.isAwait || !fs.iterable) return false;
    // §30.7/§30.8 (#765, #1525): an optional `with token` clause. "The `with
    // token` clause passes `token` as the argument to `GetAsyncEnumerator(token)`.
    // When omitted, `CancellationToken.None` is passed." (§30.7 v0.228) — the
    // token is now evaluated right after the source (below) and passed as the
    // second argument to the slot-4 GetAsyncEnumerator call, so a
    // compiler-generated async iterator captures it in its frame and checks it
    // internally (LlvmBackend_Async.cpp's Resume-entry check). The per-iteration
    // consumer-side check further below (§30.8 / #790) is KEPT as well — it is
    // the only enforcement for enumerator producers with no frame of their own
    // (e.g. `Task.WhenEach`'s runtime enumerator) and is a harmless redundant
    // early-out for the compiler-generated case (same condition, checked one
    // step earlier in program order).
    // The consuming context must be an async state machine (the await suspend
    // lives in its Resume function). sema rejects `await for` outside async; this
    // is a defensive guard so a stray case produces a diagnostic, not bad IR.
    if ((smKind_ != SuspendKind::Await && smKind_ != SuspendKind::AsyncYield) ||
        !smStateStruct_ || !smSelf_ || !asyncResumeFn_)
        return false;

    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy) return false;
    // Element type T from the source's first generic arg (IAsyncEnumerable<T>).
    llvm::Type* elemTy = i32();
    if (!iterTy->genericArgs.empty() && iterTy->genericArgs[0])
        // fix #3537 (§4.7 / §30.7, v0.283): the `await for` face of the same
        // producer/consumer agreement `lowerIEnumerableBuiltinFor` states — see
        // that site. `emitAsyncIteratorMethod` emits slot 3 returning the
        // one-byte placeholder for a `()` element.
        elemTy = unitStorageTy(lowerTypeRef(iterTy->genericArgs[0]));

    // Evaluate the source to the enumerator pointer and store it in a slot.
    // #792 / §29.7/§30.7: the slot is frame-backed (asyncHandlerSpillSlots_) so it
    // survives the loop's own per-iteration await suspends AND is reachable on the
    // unwind path — mirroring the synchronous iterator-for (lowerIEnumerableBuiltinFor).
    // This replaces the prior SSA-promoted enumerator: holding it in a frame slot
    // lets the disposal scope below dispose the frame on EVERY exit edge.
    llvm::Value* srcPtr = lowerExpr(*fs.iterable);
    if (!srcPtr) return false;
    // §30.7/§30.8 (#765/#1525): evaluate the optional `with` token right after
    // the source, before the GetAsyncEnumerator(token) call below — the
    // desugaring's own argument-evaluation order (receiver, then the token
    // argument). `tokenVal` stays null (C++) when no `with` clause is written;
    // `tokenArg` is the LLVM value actually passed (None ⇒ a null cell pointer).
    llvm::Value* tokenVal = fs.withExpr ? lowerExpr(*fs.withExpr) : nullptr;
    llvm::Value* tokenArg =
        tokenVal ? tokenVal : llvm::ConstantPointerNull::get(ptrTy());
    // fix #1268 / §29.3 + §29.11: the source is the async-ITERABLE; ask it for a
    // FRESH, INDEPENDENT enumerator through vtable slot 4 (GetAsyncEnumerator)
    // rather than driving — and Disposing — the source object itself. Without
    // this a second `await for` over the same bound `IAsyncEnumerable<T>` local
    // re-drove the frame the first loop had already Disposed and freed: a
    // heap-use-after-free (SIGSEGV). Slot 4 is now carried by BOTH producers of
    // an async-enumerable value the desugaring can see: the compiler-generated
    // async iterator (emitAsyncIteratorMethod — returns a byte-copy clone reset
    // to the start) and the runtime `Task.WhenEach`/`WhenEachOwned` enumerator
    // (bezel_runtime_concurrency.cpp — returns `self`, its enumerable and
    // enumerator being one object), so the slot-4 read is always in bounds.
    llvm::Value* enumPtr = nullptr;
    {
        llvm::Value* srcVt = builder_.CreateLoad(ptrTy(), srcPtr);
        llvm::Value* geSlotPtr =
            builder_.CreateGEP(ptrTy(), srcVt, {builder_.getInt32(4)});
        llvm::Value* geFnPtr = builder_.CreateLoad(ptrTy(), geSlotPtr);
        auto* geTy =
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false);
        enumPtr = builder_.CreateCall(geTy, geFnPtr, {srcPtr, tokenArg},
                                      "afor.getenum");
        // fix #2356 / §29.8: an owned-param compiler-generated async iterator's
        // GetAsyncEnumerator (LlvmBackend_Async.cpp) can now raise
        // InvalidOperationException (the affine claim-once guard) — the first
        // vtable-slot-4 producer able to throw, so this is the first call site
        // that needs the check. A non-throwing producer (the no-owned-param
        // clone path, or the runtime Task.WhenEach self-return) leaves no
        // pending exception and this is a harmless no-op branch.
        emitPendingCheck();
    }
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumPtr, enumSlot);
    asyncHandlerSpillSlots_.push_back(enumSlot);  // survive the condBB await suspend
    // §29.11 / §29.2 (#1268): who disposes what.
    //   * The per-loop ENUMERATOR is loop-owned and disposed on every exit edge
    //     — EXCEPT when the source handed back ITSELF (`en == src`) and the
    //     source is a NAMED binding, in which case disposing it here would free
    //     an object the binder disposes at its own scope end (a double free).
    //   * The SOURCE is disposed by the loop only when it is a freshly produced
    //     temporary (a CallExpr/NewExpr with no owning binding) AND it handed
    //     back a distinct enumerator; when `en == src` the enumerator disposal
    //     above already covers it. A named local/param/field source is disposed
    //     at its own lifetime end (registerIterableSourceDisposal, §29.11).
    // The `en == src` test is a runtime compare because the two producers differ
    // and the static type (`IAsyncEnumerable<T>`) cannot tell them apart.
    // fix #2485 (§9.2/§22.8/§29.8, v0.235): isAutoPropertyStorageSymbol — the async
    // `for await` twin of the sync for-in fix above (same misclassification,
    // same confirmed heap-use-after-free direction: an imported auto-property
    // source disposed early by a loop that does not own it).
    bool sourceIsNamedLocal = false;
    if (sema::Symbol* sym = ctx_.exprSymbol(*fs.iterable)) {
        if (dynamic_cast<sema::LocalSymbol*>(sym) != nullptr ||
            dynamic_cast<sema::ParameterSymbol*>(sym) != nullptr ||
            dynamic_cast<sema::FieldSymbol*>(sym) != nullptr) {
            sourceIsNamedLocal = true;
        } else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(sym)) {
            sourceIsNamedLocal = sema::isAutoPropertyStorageSymbol(ps, ctx_);
        }
    }
    // fix #2779 / §29.2 / §30.5 / §23.3 / §28.3 K3 / §28.9 (v0.240): the
    // async-enumerable twin of the lowerIEnumerableBuiltinFor widening above.
    // An `await for` whose source is an INTERFACE-dispatched
    // `IAsyncEnumerable<T>`-family property read is statically misclassified
    // as an auto-property storage (the interface's `PropertyDecl` records
    // `bodyKind == Auto`); the runtime is the opposite — the wrap-creator
    // fires UNCONDITIONALLY for the iterator-format-typed read, and the
    // wrapper frame is a fresh heap object the reader must reclaim. Widen
    // the verdict to `!sourceIsNamedLocal` for that family so the async loop
    // registers the loop-end source disposal via the async-state-machine
    // wire (smIterEnumFrameSlot_) alongside the enumerator's own disposal.
    // The predicate is the same `isInterfaceDispatchedIteratorRead` and
    // matches its sibling exactly; the #2174 borrow-pin for non-iterator-
    // format interface properties is preserved. // per #2779, #2703, #1525
    if (sourceIsNamedLocal &&
        isInterfaceDispatchedIteratorRead(fs.iterable.get()))
        sourceIsNamedLocal = false;
    llvm::Value* enumIsSrc = builder_.CreateICmpEQ(enumPtr, srcPtr, "afor.selfen");
    auto* enumOwnSlot = entryAlloca(ptrTy());
    builder_.CreateStore(
        sourceIsNamedLocal
            ? builder_.CreateSelect(
                  enumIsSrc, llvm::ConstantPointerNull::get(ptrTy()), enumPtr,
                  "afor.enown")
            : enumPtr,
        enumOwnSlot);
    asyncHandlerSpillSlots_.push_back(enumOwnSlot);
    llvm::AllocaInst* srcOwnSlot = nullptr;
    if (!sourceIsNamedLocal) {
        srcOwnSlot = entryAlloca(ptrTy());
        builder_.CreateStore(
            builder_.CreateSelect(enumIsSrc,
                                  llvm::ConstantPointerNull::get(ptrTy()),
                                  srcPtr, "afor.srcown"),
            srcOwnSlot);
        asyncHandlerSpillSlots_.push_back(srcOwnSlot);
    }
    // §30.8 (#765/#790): `tokenVal` (evaluated earlier, above) is a pointer to
    // the source's cancellation-state cell (null for CancellationToken.None). It
    // is ALSO observed at each iteration's suspend point (condBB below): a
    // cancelled token raises OperationCanceledException — the consumer-side half
    // of the redundant pair described above.

    // Loop variable slot — the body sees `x` here.
    auto* varSlot = entryAlloca(elemTy);
    // §30.7: if the loop variable is live across an await in the BODY, it is a
    // suspend-local — frame-back it like range-for does.
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(&fs);
        if (fit != smLocalFields_.end())
            smAllocaToField_[varSlot] = fit->second;
    }
    locals_[&fs] = varSlot;

    // #792 / §29.7 (#623): register the enumerator frame for disposal on ALL exit
    // paths (break / return / exception-unwind / normal exhaustion) via a dedicated
    // scope around the loop — the iterator frame's Dispose is at vtable slot 1, so
    // use iterEnumDispose. Previously the frame was disposed only at endBB (normal
    // completion), leaking it when the body threw / returned / broke out.
    // fix #2566 (v0.236): the loop stack entry below uses `disposeStack_.size()`
    // (AFTER this emplace_back), not a pre-push `outerDepth`. The pre-push form
    // made `continue` walk the enumerator's slot to dispose, which FREEs the
    // iterator frame; the next condBB then reads freed memory → heap-use-after-
    // free. See the matching comment at the loopStack_.push_back below.
    disposeStack_.emplace_back();
    {
        Disposal d{enumOwnSlot, nullptr};
        d.iterEnumDispose = true;
        disposeStack_.back().push_back(d);
    }
    if (srcOwnSlot) {  // #1268: loop-owned temporary source (null when `en==src`)
        Disposal ds{srcOwnSlot, nullptr};
        ds.iterEnumDispose = true;
        disposeStack_.back().push_back(ds);
    }

    // Vtable-dispatched MoveNextAsync (slot 2) / Current (slot 3); Dispose
    // (slot 1) is handled by the disposal scope above (iterEnumDispose).
    auto* moveNextTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);

    auto* condBB = llvm::BasicBlock::Create(llvm_, "afor.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "afor.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "afor.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: task = MoveNextAsync(enum); hasValue = await task.
    builder_.SetInsertPoint(condBB);
    // §30.8 (#790): cooperative cancellation — before advancing the iterator,
    // observe the `with` token. When it is cancelled, dispose the enumerator
    // frame and raise OperationCanceledException; the consuming method observes
    // cancellation at this suspend point (§30.8). CancellationToken.None (a null
    // state cell) never cancels.
    if (tokenVal) {
        auto* isCancFn = declareC("bz_ctoken_is_cancelled", i32(), {ptrTy()});
        llvm::Value* cancelled = builder_.CreateICmpNE(
            builder_.CreateCall(isCancFn, {tokenVal}, "afor.canc"),
            builder_.getInt32(0));
        auto* cancelBB =
            llvm::BasicBlock::Create(llvm_, "afor.cancel", currentFn_);
        auto* proceedBB =
            llvm::BasicBlock::Create(llvm_, "afor.proceed", currentFn_);
        builder_.CreateCondBr(cancelled, cancelBB, proceedBB);
        // cancelBB: throw a catchable OperationCanceledException. The iterator
        // frame is disposed by the disposal scope on the resulting unwind (#792 —
        // no longer an explicit per-cancel-path dispose). emitThrowValue
        // terminates the block (branch to unwind).
        builder_.SetInsertPoint(cancelBB);
        auto vit = builtinVtable_.find("OperationCanceledException");
        if (vit != builtinVtable_.end()) {
            llvm::Value* oce = builder_.CreateCall(
                callocFn(),
                {builder_.getInt64(1), builder_.getInt64(kExcObjSize)}, "afor.oce");
            builder_.CreateStore(vit->second, oce);
            emitThrowValue(oce);
        } else {
            builder_.CreateBr(proceedBB);  // no OCE vtable available — fall back
        }
        builder_.SetInsertPoint(proceedBB);
    }
    llvm::Value* enumObj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vt = builder_.CreateLoad(ptrTy(), enumObj);
    llvm::Value* mnSlotPtr =
        builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(2)});
    llvm::Value* mnFnPtr = builder_.CreateLoad(ptrTy(), mnSlotPtr);
    llvm::Value* task =
        builder_.CreateCall(moveNextTy, mnFnPtr, {enumObj}, "afor.task");
    // §30.7: await the per-advance task (a single-use producer task — free it
    // after its bool is read). This is a suspend point in the consuming method.
    llvm::Value* hasValue =
        emitAwaitSuspend(task, i1(), /*freeTaskAfter=*/true);
    if (!hasValue) return false;
    builder_.CreateCondBr(hasValue, bodyBB, endBB);

    // body: x := Current(enum) for IAsyncEnumerable / TakeCurrent(enum) for
    // IOwningAsyncEnumerable (§29.1 v0.161 #1349; same vtable slot 3).
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* enumObj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vt2 = builder_.CreateLoad(ptrTy(), enumObj2);
    llvm::Value* curSlotPtr =
        builder_.CreateGEP(ptrTy(), vt2, {builder_.getInt32(3)});
    llvm::Value* curFnPtr = builder_.CreateLoad(ptrTy(), curSlotPtr);
    // #5305 / §30.15 (v0.313) / stdlib §6.9: on a `Channel<T>.ReceiveAll()`
    // drain whose element T is an AGGREGATE value type, slot 3 is the
    // borrowing `bz_chrecvall_current`, which returns the channel's item WORD
    // verbatim — and for an aggregate that word is the CELL the Send allocated
    // (encodeChannelItemWord), not the value's bytes. The `currentTy` call
    // above reinterprets the returned pointer AS `elemTy` and would store the
    // cell ADDRESS into the loop variable: valid IR, silently wrong values
    // (MEASURED pre-unbox: a `record P(X,Y)` drain printed the cell address as
    // `X,Y`). §30.15 requires the drain to yield "each item with `Receive`'s
    // transfer semantics", so the word is read at its true `void*` type and
    // unboxed. The cell is released here — for the value-T (borrowing)
    // variant `bz_chrecvall_dispose` frees `current_yielded` only when
    // `is_owning`, and this path is `is_owning == 0`, so the decoder's release
    // is the ONLY one and the slot's stale copy is never dereferenced again.
    // Every other async enumerable is untouched: the by-value slot-3 call is
    // exactly what they produce.
    llvm::Value* curVal = nullptr;
    // #5305-ff2: the element TypeRef T travels with the flag, so the decode
    // gates on `sema::isAggregateChannelElement(T)` — the same predicate on
    // the same T the encoder boxed for.
    if (const sema::TypeRef* drainElemTy =
            channelDrainAggregateElementTy(fs.iterable.get())) {
        auto* cellTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
        llvm::Value* cell =
            builder_.CreateCall(cellTy, curFnPtr, {enumObj2}, "afor.boxed.ptr");
        curVal = decodeChannelItemWord(cell, drainElemTy, elemTy,
                                       "afor.boxed");
    } else {
        curVal = builder_.CreateCall(currentTy, curFnPtr, {enumObj2}, "afor.cur");
    }
    emitPendingCheck();  // TakeCurrent may raise IOE on OwnedAsync path (#1349)
    builder_.CreateStore(curVal, varSlot);

    // #792 / §29.7 / fix #2566 (v0.236): scopeDepth = MY scope's index (after
    // push), not outerDepth (before push). The prior `outerDepth` form made
    // emitLoopDisposals on `continue` walk the enumerator's slot to dispose,
    // which (with `iterEnumDispose`) FREES the iterator frame; the next condBB
    // re-loads the frame's enum slot and reads freed memory → heap-use-after-
    // free (`READ` in `bz$async$Main$Resume`, `free` in
    // `bz$asynciter$Gen$Dispose`). Matches the existing
    // lowerIEnumerableForSortedSet (#2553) and the post-#2556 form in
    // lowerIEnumerableFor / lowerIEnumerableBuiltinFor: `disposeStack_.size()`
    // here, since the scope was just pushed via `disposeStack_.emplace_back()`
    // a few lines up — the current size IS the new scope's index. The wrapper/
    // enumerator resources must outlive every continue; `break` / normal
    // exhaustion still dispose them via the endBB path.
    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(condBB);

    // end: normal exhaustion. Dispose the enumerator frame (vtable slot 1, via the
    // scope's iterEnumDispose — runs any pending finally + frees the frame), then
    // pop the scope. #792: this single scope now covers normal exit AND the
    // break/return/unwind paths (previously only this normal-exit dispose existed).
    builder_.SetInsertPoint(endBB);
    emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return true;
}


bool Emitter::lowerLazyLinqFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    // Only a lazy LINQ pipeline result (the structural sentinel) is handled
    // here; iterator state machines and Array<T> go through other paths.
    //
    // stdlib §5 (#755, finding [17]) / spec §29.2: an `IGrouping<TKey, T>` source
    // is also handled here. `IGrouping : IEnumerable<T>` (stdlib §5:3076), so
    // `for var el in g do … end` — the inner loop of the canonical
    // `for var g in xs.GroupBy(k) do … end` shape — is well-formed Bezel; it
    // was rejected with "not yet implemented: codegen for this for statement"
    // because no for-handler recognized an IGrouping-typed source.
    // lowerToLazyEnumerator wraps the grouping's (grouping-owned) element array
    // as a BORROWING lazy array source, so the enumerator protocol below drives
    // it exactly as it drives any lazy pipeline.
    const sema::TypeRef* igroupingElem = iGroupingElemTypeRef(iterTy);
    if (!igroupingElem && !isLazyEnumerableType(iterTy)) return false;

    // #752: route through lowerToLazyEnumerator (not lowerExpr) so a bare
    // `GroupBy(...)` iterable — which lowerExpr cannot lower to a usable
    // enumerator — is materialized into a lazy ARRAY source over its
    // IGrouping elements, exactly as the Count()/Any()/Sum() consumers do.
    // For an ordinary lazy pipeline (Where/Select/…) lowerToLazyEnumerator
    // returns lowerExpr(iterable) unchanged, so this is behaviour-preserving.
    sema::TypeRef* lazyElemRef = nullptr;
    // #1914: a bare `for var g in seq.GroupBy(sel)` drains the fresh groups and
    // disposes the enumerator at loop end — so it OWNS the groups temporary (each
    // grouping consumed inside the loop, freed only after). Pass ownFreshGroupings
    // so the lazy source frees the groupings + array on dispose (no leak); the
    // borrowing default is kept for every re-owning / snapshot-reading consumer.
    llvm::Value* lazy = lowerToLazyEnumerator(*fs.iterable, lazyElemRef,
                                              /*ownFreshGroupings=*/true);
    if (!lazy) return false;
    // #755: an IGrouping's element type is genericArgs[1] (genericArgs[0] is the
    // key); every other source here carries its element at genericArgs[0].
    llvm::Type* elemTy = lowerTypeRef(
        igroupingElem ? igroupingElem : iterTy->genericArgs[0]);
    if (!elemTy) elemTy = i32();

    // Store the enumerator in a slot; iterate via its vtable.
    // Lazy enumerator vtable: [0]=td, [1]=Dispose, [2]=MoveNext(->i32),
    // [3]=Current(->void* element-bytes pointer).
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(lazy, enumSlot);

    auto* moveNextTy = llvm::FunctionType::get(i32(), {ptrTy()}, false);
    auto* currentTy  = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);

    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "lz.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "lz.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "lz.end",  currentFn_);
    // contBB: join point for the normal loop-exit path (endBB → contBB).
    // Code after the for-in continues here.
    auto* contBB = llvm::BasicBlock::Create(llvm_, "lz.cont", currentFn_);

    // #1226 / §29.11: a bound lazy LINQ var is single-consumption (affine).
    // After the first for-loop the slot is nulled (see endBB).  On a second
    // for-loop over the same bound variable the slot holds null; detect this
    // and throw InvalidOperationException — a silent-empty second iteration
    // (the prior #1218 behaviour) is unsound: it masks the programming error
    // without a useful signal.  A fresh inline lazy (iterable is a call
    // expression, not a named local) can never be null here.
    llvm::Value* lazyIsNull = builder_.CreateICmpEQ(
        lazy, llvm::ConstantPointerNull::get(ptrTy()), "lz.isnull");
    emitGuardThrow(lazyIsNull, "InvalidOperationException");
    builder_.CreateBr(condBB);

    // cond: MoveNext (vtable slot 2) → i32 (1 = advanced).
    builder_.SetInsertPoint(condBB);
    llvm::Value* obj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* mnSlot = builder_.CreateGEP(ptrTy(), vtable, {builder_.getInt32(2)});
    llvm::Value* mnFn = builder_.CreateLoad(ptrTy(), mnSlot);
    llvm::Value* moved = builder_.CreateCall(moveNextTy, mnFn, {obj}, "lz.moved");
    llvm::Value* movedB = builder_.CreateICmpNE(moved, builder_.getInt32(0));
    builder_.CreateCondBr(movedB, bodyBB, endBB);

    // body: Current (vtable slot 3) → void*; load the element value.
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* obj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable2 = builder_.CreateLoad(ptrTy(), obj2);
    llvm::Value* curSlot = builder_.CreateGEP(ptrTy(), vtable2, {builder_.getInt32(3)});
    llvm::Value* curFn = builder_.CreateLoad(ptrTy(), curSlot);
    llvm::Value* curPtr = builder_.CreateCall(currentTy, curFn, {obj2}, "lz.curp");
    llvm::Value* curVal = builder_.CreateLoad(elemTy, curPtr, "lz.cur");
    builder_.CreateStore(curVal, varSlot);

    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    lowerForBody(fs);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    // Dispose the enumerator chain (vtable slot 1).
    llvm::Value* dObj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* dVt = builder_.CreateLoad(ptrTy(), dObj);
    llvm::Value* dSlot = builder_.CreateGEP(ptrTy(), dVt, {builder_.getInt32(1)});
    llvm::Value* dFn = builder_.CreateLoad(ptrTy(), dSlot);
    auto* disposeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                              {ptrTy()}, false);
    builder_.CreateCall(disposeTy, dFn, {dObj});
    // #1218 / §5: null the bound variable's slot so a subsequent access —
    // a second for-loop or a chained terminal — safely sees null (the null-guard
    // above skips the loop body cleanly rather than dereferencing freed memory).
    // Only fires when the iterable is a named local or parameter (IdentifierExpr);
    // a fresh inline lazy (CallExpr / chained operator) has no owning slot to null.
    // Per §29.11 pattern: localSlot handles both unboxed and cell-boxed variables.
    //
    // #755: NOT for an IGrouping source. The slot there holds the GROUPING
    // pointer (typically the enclosing `for var g in xs.GroupBy(k)` loop
    // variable), not the enumerator this loop consumed — the enumerator is the
    // fresh borrowing wrapper built above and already disposed. Nulling the
    // grouping slot would dangle `g.Key` / the outer loop's own disposal.
    if (!igroupingElem) {
        if (llvm::AllocaInst* boundSlot = localSlot(*fs.iterable)) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(boundSlot, ety);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }
    builder_.CreateBr(contBB);

    builder_.SetInsertPoint(contBB);
    return true;
}


bool Emitter::lowerCaseStmt(const ast::CaseStmt& cs) {
    // §19 / §A (#727): a scrutinee-less `case cond => ... end` statement is an
    // ordered boolean-condition chain (like if/elif); each arm's pattern is a
    // bool condition and `_` is the catch-all.
    llvm::Value* scrut = nullptr;
    const sema::TypeSymbol* enumTy = nullptr;
    bool isStr = false;
    const sema::TypeRef* scrutTy = nullptr;
    sema::TypeRef choiceScrutStorage;  // backs a recovered `this`-choice scrutTy
    // K8 full-expression boundary (#1795): the case scrutinee may be a fresh
    // owned temporary. The ownership pass flagged this on cs->disposeOwned-
    // Scrutinee when scrut is `new T(...)` for a heap class, an owned-returning
    // factory call, or an owning-collection materializer — every shape that
    // would otherwise leak at end-of-case. We capture the scrut in an entry
    // alloca, register the slot for unwind disposal (an arm-body throw
    // disposes the temp), and emit a normal-path disposal at endBB top.
    llvm::AllocaInst* scrutOwnedSlot = nullptr;
    bool scrutOwnIsOwningCollection = false;
    // fix #4552 / §35.5 *The owning match* + §35.4 (v0.306): the owning-CHOICE
    // spelling of the same K8.3 boundary. An owning choice is a VALUE type, so
    // `scrut` is a struct in a register and every gate above (which requires a
    // pointer) declines it — the scrutinee temporary was owned by nothing and
    // its payloads leaked outright. See `unboundOwningChoiceTemp`.
    llvm::AllocaInst* scrutChoiceSlot = nullptr;
    const sema::TypeRef* scrutChoiceRef = nullptr;
    if (cs.scrutinee) {
        scrut = lowerExpr(*cs.scrutinee);
        if (!scrut) return false;
        enumTy = enumTypeOf(*cs.scrutinee);
        isStr = isStringExprType(*cs.scrutinee);
        scrutTy = ctx_.exprType(*cs.scrutinee);
        // §35.5 (v0.200, #2047): recover a choice scrutinee whose sema exprType
        // is absent — `this` inside a choice instance method (`case this of …`)
        // is left untyped by sema, like a record `this`. Mirror lowerCaseExpr:
        // load the struct through the by-pointer choice `this` and synthesize
        // the scrutinee TypeRef the §35.5 choice-pattern branch keys on, so the
        // arm matches instead of silently degrading to the no-match fall-through.
        if (!scrutTy || scrutTy->kind != sema::TypeRef::Kind::Named ||
            !scrutTy->declaration ||
            scrutTy->declaration->kind != ast::TypeDeclKind::Choice) {
            if (const sema::TypeSymbol* chTy = choiceTypeOf(*cs.scrutinee)) {
                if (scrut->getType()->isPointerTy())
                    scrut = builder_.CreateLoad(choiceStruct(chTy), scrut,
                                                "choice.this.val");
                choiceScrutStorage.kind = sema::TypeRef::Kind::Named;
                choiceScrutStorage.declaration =
                    const_cast<sema::TypeSymbol*>(chTy);
                scrutTy = &choiceScrutStorage;
            }
        }
        if (cs.disposeOwnedScrutinee && scrutTy &&
            scrutTy->kind == sema::TypeRef::Kind::Named &&
            scrut->getType()->isPointerTy()) {
            scrutOwnedSlot = entryAllocaNullPtr();
            builder_.CreateStore(scrut, scrutOwnedSlot);
            scrutOwnIsOwningCollection = common::isOwningCollectionHead(
                scrutTy->headName());
            Disposal d{scrutOwnedSlot, nullptr};
            d.unwindOnly = true;   // arm-body throw disposes the temp
            if (!scrutOwnIsOwningCollection)
                d.dynamicDispose = true;  // class instance via vtable Dispose
            else
                d.collection = true;      // owning-collection free path
            if (!disposeStack_.empty())
                disposeStack_.back().push_back(d);
        }
        // fix #4552 / §35.5 (v0.306): the owning-CHOICE scrutinee temporary.
        // §35.5: "payloads not transferred into bindings (discarded slots like
        // `Fetched(_)`, and the payloads of the value when a `_` or
        // non-destructuring arm is selected) are disposed when the construct
        // completes, by the same unbound-temporary pass that disposes the
        // container."
        //
        // EXACTLY-ONCE. §35.5's owning match TRANSFERS the selected case's
        // owned payloads into the arm's bindings, and its own adoption
        // spelling (`using var u := move r;` in a `do … end` arm) then
        // disposes the payload at the arm's scope end. Disposing the container
        // wholesale at construct completion double-frees exactly that shape —
        // MEASURED as a SIGSEGV on rule-corpus case s35_5_0002, which is
        // written from the spec and was passing before the reclaim was added.
        //
        // fix #4609 (§35.5, v0.306): the discharge is now PER ARM, at the top
        // of the selected arm's body, instead of #4552's whole-construct
        // `armsBind` gate. Same safety, strictly more precision: a construct
        // that mixes a binding arm with a discarding one (`A(var x) => …,
        // B(_) => …`) had its reclaim disabled on EVERY arm by the coarse
        // gate, so selecting `B(_)` leaked a payload §35.5 says is "disposed
        // at construct completion". The discharge is a runtime zero-store over
        // the temp slot, so it also covers the UNWIND edge (an exception out
        // of a binding arm must not re-dispose what the arm's `using var`
        // already owns) with no second mechanism — the same #4566 moved-from
        // encoding used everywhere else for a choice. What remains open is
        // unchanged from #4552: arm-selection transfer is not implemented, so
        // a binding arm's UN-transferred siblings (`Fetched(var w, _)`) still
        // leak rather than double-free — the sound direction.
        //
        // fix #4609: §35.5's SECOND announcing scrutinee form — `case move o
        // of …` over a named owned binding — is admitted here alongside the
        // unbound temporary. The two are one construct in §35.5; only the
        // announcement differs. The marked form additionally CONSUMES its
        // source ("the consumed binding is left moved-from when the construct
        // completes"), which `markedOwningChoiceScrutineeSlot` records with
        // the same zero-store, so the source's own scope-end active-case pass
        // becomes a no-op instead of disposing payloads the arms already took.
        llvm::AllocaInst* markedSrcSlot =
            markedOwningChoiceScrutineeSlot(cs.scrutinee.get(), scrutTy);
        if (!scrutOwnedSlot && scrut &&
            (unboundOwningChoiceTemp(cs.scrutinee.get(), scrutTy) ||
             markedSrcSlot) &&
            !scrut->getType()->isPointerTy() && !disposeStack_.empty()) {
            scrutChoiceSlot = entryAlloca(scrut->getType());
            builder_.CreateStore(scrut, scrutChoiceSlot);
            scrutChoiceRef = scrutTy;
            Disposal d{scrutChoiceSlot, nullptr};
            // fix #4609 (§35.5, v0.306): NOT `unwindOnly`. #4552 registered
            // this entry for the throw edge only, because the normal edge is
            // the reclaim at endBB — but a `return` (or `break`/`continue`)
            // out of an arm body takes neither: `emitDisposals` skips an
            // `unwindOnly` entry when `dropUndischarged` is false, and endBB
            // is never reached. MEASURED on
            // `case move o of W(_) => do Console.WriteLine("hit");
            // return 5; end, X => () end case;` — `LeakSanitizer: 16 byte(s)
            // leaked in 1 allocation(s)`, where the pre-#4609 compiler printed
            // `widget disposed` (the source local's scope-end pass covered the
            // return path). A plain entry covers every edge, and the two
            // runtime zero-stores keep it exactly-once: endBB zeroes after its
            // reclaim (so a later traversal is a no-op) and a BINDING arm
            // zeroes at its body top (so a return out of an adopting arm does
            // not re-dispose what the arm's `using var` owns).
            d.unwindOnly = false;
            d.owningChoice = true;
            d.owningChoiceTy = scrutTy->declaration;
            d.owningChoiceRef = scrutTy;
            disposeStack_.back().push_back(d);
            // §35.5 form 2 is a CONSUME of the named binding: mark it
            // moved-from now that the value is captured in the temp above,
            // which is the only owner from here on. Emitted at the consume
            // site — not at construct completion — so a `return` or `throw`
            // out of an arm body finds the source already discharged.
            if (markedSrcSlot)
                builder_.CreateStore(
                    llvm::Constant::getNullValue(
                        markedSrcSlot->getAllocatedType()),
                    markedSrcSlot);
        }
    }
    auto* endBB =
        llvm::BasicBlock::Create(llvm_, "casestmt.end", currentFn_);
    for (const auto& arm : cs.arms) {
        llvm::Value* pm = nullptr;
        if (!cs.scrutinee) {
            // #727: scrutinee-less arm — `_` always matches; otherwise the arm's
            // pattern is one or more bool conditions (comma = OR).
            if (dynamic_cast<const ast::DiscardPattern*>(arm.pattern.get())) {
                pm = builder_.getInt1(true);
            } else if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(
                           arm.pattern.get())) {
                for (const auto& c : cp->constants) {
                    if (!c) continue;
                    llvm::Value* cond = lowerExpr(*c);
                    if (!cond) continue;
                    if (!cond->getType()->isIntegerTy(1))
                        cond = builder_.CreateICmpNE(
                            cond, llvm::ConstantInt::get(cond->getType(), 0));
                    pm = pm ? builder_.CreateOr(pm, cond) : cond;
                }
            }
        } else {
            pm = patternMatches(arm.pattern.get(), scrut, enumTy, isStr, scrutTy);
        }
        if (!pm) {  // deferred pattern kind — degrade to no-op
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            return false;
        }
        auto* bodyBB =
            llvm::BasicBlock::Create(llvm_, "casestmt.arm", currentFn_);
        auto* nextBB =
            llvm::BasicBlock::Create(llvm_, "casestmt.next", currentFn_);
        // CG38b: a BindingPattern arm exposes the scrutinee value via a
        // local slot keyed on the BindingPattern AST node — the same node
        // sema used as lsym->decl, so localSlot() resolves the identifier.
        // The slot must be filled before the guard (which may reference the
        // bound name) and before the arm body. The alloca goes in the
        // function entry block; the store is emitted in the current
        // (pattern-test) block, before we branch to the guard or body.
        if (auto* bp = dynamic_cast<const ast::BindingPattern*>(
                           arm.pattern.get())) {
            llvm::AllocaInst* slot = entryAlloca(scrut->getType());
            builder_.CreateStore(scrut, slot);
            if (!disposeStack_.empty())
                recordDeclScopeDepth(bp);  // fix #4523
            locals_[bp] = slot;
        }
        if (arm.guard) {
            auto* guardBB = llvm::BasicBlock::Create(
                llvm_, "casestmt.guard", currentFn_);
            builder_.CreateCondBr(pm, guardBB, nextBB);
            builder_.SetInsertPoint(guardBB);
            llvm::Value* g = lowerExpr(*arm.guard);
            if (!g) return false;
            builder_.CreateCondBr(g, bodyBB, nextBB);
        } else {
            builder_.CreateCondBr(pm, bodyBB, nextBB);
        }
        builder_.SetInsertPoint(bodyBB);
        // fix #4609 / §35.5 *The owning match* (v0.306): the per-arm discharge
        // of the owning-choice scrutinee's construct-completion reclaim.
        // §35.5 disposes at completion only what was "not transferred into
        // bindings"; an arm that BINDS takes the payloads (its `using var u :=
        // move r;` adoption then owns them), so for THAT arm the reclaim must
        // decline. `bodyBB` is reached only on selection — a guard-declined
        // arm branches to `nextBB` and "transfers nothing" (§35.5 *Guards*) —
        // so zeroing here is exactly the §35.5 boundary. The zero-store is
        // #4566's moved-from encoding, so ONE store discharges every reclaim
        // the value has — the one at endBB, and the dispose-stack entry that
        // covers a `throw`, `return` or `break` out of this arm body; the
        // bindings were filled from the scrutinee VALUE above, not re-read
        // from this slot, so nothing in the arm observes the zeros.
        //
        // fix #4617 / §35.5 (v0.307): the discharge is now PER SLOT. §35.5
        // states BOTH halves at slot granularity — the arm "transfers the
        // case's `owned` and move-only value payloads into its bindings" and
        // "the un-transferred remainder — unlisted fields, unmarked named
        // slots, discarded siblings — is disposed at construct completion" —
        // but #4609 marked the WHOLE value as soon as the arm bound anything,
        // so `W(var w, _)` over `W(owned a: Widget, owned b: Widget)` adopted
        // `a` and left `b` reclaimed by nobody. `dischargeOwningMatchTransfers`
        // marks exactly the transferred slots and falls back to this same
        // whole-value store for any shape it cannot decide per slot.
        if (scrutChoiceSlot && patternBindsAnySlot(arm.pattern.get()))
            dischargeOwningMatchTransfers(scrutChoiceSlot, scrutChoiceRef,
                                          arm.pattern.get());
        bool armTerm = lowerScopedBlock(arm.stmts);
        if (!armTerm) builder_.CreateBr(endBB);
        builder_.SetInsertPoint(nextBB);
    }
    // No arm matched.
    if (cs.totalityGranted) {
        // fix #3073 / §21.7 last sentence: the grant's soundness must not
        // rest on the exhaustiveness computation alone — lower the dead
        // no-match edge as a defined trap (MatchFailureException, §19.6)
        // rather than fall through to end-of-callable + implicit-return.
        // Without the trap, a bug in caseArmsExhaustive would degrade
        // silently to a wrong value instead of a loud fault, defeating
        // §21.7's stated guarantee. emitThrowBuiltin calloc's the exception
        // object, stores its vtable, emits unwind disposals, and branches
        // to the function's unwindBB — so this block needs no explicit
        // terminator and we must NOT fall through to endBB.
        emitThrowBuiltin("MatchFailureException");
        // Defensive: emitThrowBuiltin always terminates with a br to
        // unwindBB, so the next setInsertPoint only matters for analysis.
        builder_.SetInsertPoint(endBB);
    } else {
        // §19.6: an unmatched value in statement position falls through
        // with no action (the historical shape, kept for non-granted cases).
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
    }
    // K8 full-expression boundary (#1795): dispose the case-scrutinee owned
    // temporary at the top of endBB and route to a fresh scrutEndBB. The
    // scrut slot holds a fresh non-null pointer (always populated by the store
    // registered above), so the dispose is unconditional on the normal edge —
    // paired with the `unwindOnly` Disposal entry on the dispose stack that
    // fires on an arm-body throw, the two paths are mutually exclusive. The
    // old `endBB` becomes a thin dispatch into `scrutEndBB` so the function's
    // remaining control flow (the case's enclosing statement) sees endBB as
    // its convergence point with no PHI-pred mismatch.
    if (cs.scrutinee && scrutOwnedSlot) {
        llvm::Value* scrutVal = builder_.CreateLoad(
            ptrTy(), scrutOwnedSlot, "casescrut.load");
        if (!scrutOwnIsOwningCollection)
            emitDisposeAndReclaim(scrutVal);
        else
            freeUnboundOwnedOwningCollectionTemp(scrutVal, scrutTy);
        // Discharge the matching unwind entry so a later emitDisposals pass
        // (return / throw out of the case construct's containing scope) sees
        // an empty slot and skips — no double-free. The Disposal is held by
        // pointer reference inside the on-stack entry, so erasing the entry
        // from the back vector's range is sufficient.
        if (!disposeStack_.empty()) {
            auto& scope = disposeStack_.back();
            for (auto it = scope.begin(); it != scope.end(); ++it) {
                if (it->slot == scrutOwnedSlot) {
                    scope.erase(it);
                    break;
                }
            }
        }
        auto* scrutEndBB = llvm::BasicBlock::Create(
            llvm_, "casescrut.end", currentFn_);
        builder_.CreateBr(scrutEndBB);
        builder_.SetInsertPoint(scrutEndBB);
    }
    // fix #4552 / §35.5 (v0.306): the owning-CHOICE scrutinee temporary's
    // NORMAL-edge disposal, at the construct's completion — K8.3's boundary,
    // which §35.5 applies "in statement position exactly as in expression
    // position". Emitted at the top of endBB, after every arm body has
    // finished, so a pattern binding (a borrow of the scrutinee, §19.3) is
    // never read past it.
    if (scrutChoiceSlot && scrutChoiceRef) {
        emitOwningChoiceLocationDisposal(scrutChoiceSlot, scrutChoiceRef);
        // Zero the slot so any later traversal of this Disposal entry is a
        // no-op at RUNTIME as well as after the compile-time erasure below:
        // a zeroed choice reads tag 0 with null payload pointers, and every
        // payload disposal in the active-case walk is null-guarded. (The
        // erasure alone is edge-order-dependent — #2816's lesson — so the
        // runtime discharge is what actually makes the reclaim exactly-once.)
        builder_.CreateStore(
            llvm::Constant::getNullValue(scrutChoiceSlot->getAllocatedType()),
            scrutChoiceSlot);
        if (!disposeStack_.empty()) {
            auto& scope = disposeStack_.back();
            for (auto it = scope.begin(); it != scope.end(); ++it) {
                if (it->slot == scrutChoiceSlot) {
                    scope.erase(it);
                    break;
                }
            }
        }
    }
    return false;
}


// §6.4 / issue #647: `atom.Value := v` — seq-cst store into Atomic<T> /
// AtomicInteger<T> via bz_atomic_store(atom, val_as_u64, cell_size).
// Returns true when handled (so the main assignment dispatcher returns early).
bool Emitter::tryLowerAtomicValueSet(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value || as.op != ":=") return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(as.target.get());
    if (!ma || !ma->target || ma->member != "Value") return false;
    auto* recvTy = ctx_.exprType(*ma->target);
    std::string_view rbn = builtinTypeName(recvTy);
    if (rbn != "Atomic" && rbn != "AtomicInteger") return false;

    // Evaluate the atom pointer and the new value.
    llvm::Value* atom = lowerExpr(*ma->target);
    if (!atom) return true;  // handled (to suppress fallthrough to lvalueAddr)
    llvm::Value* newVal = lowerExpr(*as.value);
    if (!newVal) return true;

    // Determine cell_size.
    int cs = 4;
    if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
        const sema::TypeRef* tArg = recvTy->genericArgs[0];
        if (tArg->kind == sema::TypeRef::Kind::Primitive) {
            using P = sema::PrimitiveType;
            switch (tArg->primitive) {
                case P::Bool: case P::Byte: case P::SByte: cs = 1; break;
                case P::Short: case P::UShort: cs = 2; break;
                case P::Long: case P::ULong:  cs = 8; break;
                default: cs = 4; break;
            }
        } else {
            cs = 8;  // IntPtr/UIntPtr
        }
    }

    // Widen newVal to i64 for bz_atomic_store's uint64_t parameter.
    // #2605 / §7.4: a flow-narrowed `T?` RHS (proved non-null by a preceding
    // null-check) lowers to its tagged-optional {i8,T} struct; unwrap to the
    // bare T payload before widening — same defect/fix as the
    // Exchange/CompareExchange/Add path in tryLowerAtomicCall
    // (LlvmBackend_Calls3.cpp). sema admits the narrowed `T?` to the Value
    // setter via the §7.4 narrowing exception; without this unwrap the
    // bz_atomic_store call gets {i8,i32} where i64 is expected and fails
    // verifyModule. unwrapNarrowedNullable is a no-op for a plain primitive.
    if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
        if (auto* tTy = lowerTypeRef(recvTy->genericArgs[0]))
            newVal = unwrapNarrowedNullable(newVal, tTy);
    llvm::Value* v64 = newVal;
    if (newVal->getType()->isIntegerTy() && !newVal->getType()->isIntegerTy(64))
        v64 = builder_.CreateZExt(newVal, i64(), "aset.v64");

    auto* storeFn = declareC("bz_atomic_store",
                              llvm::Type::getVoidTy(llvm_),
                              {ptrTy(), i64(), i32()});
    builder_.CreateCall(storeFn,
        {atom, v64, builder_.getInt32(cs)});
    return true;
}

// stdlib §7.1 / issue #645: `fs.Position := n` — an absolute seek from the
// beginning, via bz_filestream_set_position. Returns true when handled (so the
// main assignment dispatcher returns early rather than falling through to the
// lvalue path, which would silently drop the write: FileStream has no user
// PropertyDecl to store into).
bool Emitter::tryLowerFileStreamPositionSet(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value || as.op != ":=") return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(as.target.get());
    if (!ma || !ma->target || ma->member != "Position") return false;
    if (builtinTypeName(ctx_.exprType(*ma->target)) != "FileStream") return false;

    llvm::Value* fs = lowerExpr(*ma->target);
    if (!fs) return true;   // handled (suppress the lvalue fallthrough)
    llvm::Value* pos = lowerExpr(*as.value);
    if (!pos) return true;
    // fix #2617 / §7.4 ("Member access on nullable values" — flow narrowing) /
    // stdlib §7.1 (`Position: long`): a §7.4-narrowed `T?` RHS — one sema
    // proved non-null by a preceding null test and therefore admitted for the
    // NON-nullable `long` property — keeps its declared tagged-optional
    // `{i8 hasVal; T}` layout at lowering. Unwrap it to the bare `T` payload
    // before the widen, with the same unwrapNarrowedNullable helper the
    // argument / return / local-assignment paths use (#914,
    // LlvmBackend_impl.hpp:3430) — the identical defect and fix as #2605's
    // atomic intrinsics. Without it the `{i8,T}` struct is handed to
    // bz_filestream_set_position's i64 parameter (the isIntegerTy widen guard
    // skips the struct) and verifyModule fails: "Call parameter type does not
    // match function signature!" — a codegen ICE on a legal §7.4 idiom.
    //
    // The payload type comes from the LOWERED value's own optional shape rather
    // than a hardcoded i64, because the admissible narrowed RHS is not only a
    // `long?` read. All three of these ICEd on origin/main and are covered:
    //   * a narrowed `long?`  -> `{i8, i64}` (the issue's repro);
    //   * a narrowed `int?`   -> `{i8, i32}`, admitted through §6's `S → T → T?`
    //     numeric widening — a shape the issue as filed did not name;
    //   * a §7.11 LIFTED-operator result over a narrowed operand (`p + 1L`),
    //     whose value is an `insertvalue {i8, i64}` the sema expression type
    //     does not report as a nullable primitive.
    // After the unwrap the bare payload takes the existing widen below, exactly
    // as the corresponding plain `int`/`long` RHS does.
    //
    // Soundness of the unconditional shape test: sema admits only a value
    // implicitly convertible to the non-nullable `long`, and it rejects a
    // nullable one — both confirmed mechanically on this very statement form
    // ("the assigned value has a type that is not implicitly convertible to the
    // target type (§5.1)" for a `record`; "the assigned value is nullable but
    // the target type is non-nullable (§7)" for both an un-narrowed `long?` and
    // an un-narrowed lifted `s + 1L`). So the only tagged optional that can
    // reach here is one sema PROVED non-null, and the unwrap can never strip a
    // value that is actually null. The integer-payload requirement keeps the
    // test to the numeric optionals that are actually admissible. A no-op for
    // every plain primitive RHS (not a struct).
    if (auto* st = llvm::dyn_cast<llvm::StructType>(pos->getType()))
        if (st->getNumElements() == 2 && st->getElementType(1)->isIntegerTy())
            pos = unwrapNarrowedNullable(pos, st->getElementType(1));
    // fix #2617 / §5.1 (v0.236): widen by SIGNEDNESS, not unconditionally.
    // §5.1's implicit-conversion table admits `byte`, `ushort`, `uint`, and
    // `char` -> `long` ("Widening from unsigned to a strictly larger signed
    // type … is permitted because no value is lost"), and each lowers to an
    // integer NARROWER than i64. A blanket CreateSExt sign-extends those: a
    // `byte` of 200 (i8 0xC8) became -56, so `fs.Position := b` seeked to a
    // negative offset.
    //
    // PRE-EXISTING vs NEWLY REACHABLE — the distinction that makes this part
    // of the same commit rather than a separate concern:
    //   * for a PLAIN unsigned RHS (`var b: byte := byte(200);`) the bug is
    //     pre-existing: the SExt has always been the only widen here;
    //   * for a NARROWED unsigned nullable (`byte?`/`ushort?`/`uint?`/`char?`)
    //     the unwrap added above is what lets the value reach this widen at
    //     all. On origin/main that shape died loudly at verifyModule. Shipping
    //     the unwrap WITHOUT this would have converted a loud ICE into a
    //     silent wrong seek offset — trading a crash for silent bad data, so
    //     the two changes must land together.
    // Keyed on the source's sema type via isUnsignedPrim, the convention this
    // codebase already uses for exactly this choice (LlvmBackend_Stmts.cpp
    // ~2686, the iterator element widen); this site was the outlier. Falls
    // back to SExt when the sema type is unavailable, preserving the previous
    // behaviour for anything not positively known to be unsigned.
    if (pos->getType()->isIntegerTy() && !pos->getType()->isIntegerTy(64)) {
        const sema::TypeRef* srcSemaTy = ctx_.exprType(*as.value);
        bool srcUnsigned =
            srcSemaTy && isUnsignedPrim(srcSemaTy->primitive);
        pos = srcUnsigned ? builder_.CreateZExt(pos, i64(), "fs.pos64")
                          : builder_.CreateSExt(pos, i64(), "fs.pos64");
    }
    auto* fn = declareC("bz_filestream_set_position",
                        llvm::Type::getVoidTy(llvm_), {ptrTy(), i64()});
    builder_.CreateCall(fn, {fs, pos});
    return true;
}


}  // namespace bezel::codegen
