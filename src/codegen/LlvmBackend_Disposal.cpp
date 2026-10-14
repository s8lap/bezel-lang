#include "codegen/LlvmBackend_impl.hpp"
#include "sema/GenericParams.hpp"  // fix #3143: the ONE both-sources generic-param walk (§14.1/§12.9/§2.5)

#include "common/OwnershipFacts.hpp"  // §28.1 per-instantiation owned-marker erasure (#2047)
#include "common/PatternAResultCollection.hpp"  // fix #3657/#3658: the ONE both-sides §30.5 Pattern A collection-membership table (§30.5/§28.3, #3638)

// This translation unit holds the definitions of Emitter's §28 disposal /
// unwind / auto-drop member functions, extracted mechanically from
// LlvmBackend_Setup.cpp (definition-motion only, no behavior change;
// #1414 stage 5-replacement) to carve the disposal machinery out of that
// ~15.4k-line TU.  The Emitter class is declared in LlvmBackend_impl.hpp and
// its declarations are unchanged; only definitions moved here.
//
// Contents (all §28 machinery): the per-type disposer body / core emission
// (findDestructor, emitDisposer*, emitOwnedFieldDisposals, emitSpecializedDisposer);
// the exception-cleanup / unwind-disposal path (excGlobal, emitExcSlot,
// propagateBB, unwindBB, emitUnwindDisposals*, emitPendingCheck*, emitDynamicDispose,
// emitDisposeAndReclaim); the element/shared/weak/grouping/subscription/box
// disposer thunks and emitBaseDisposeChain; the drop-on-overwrite disposal
// (disposeDisplacedOwnedClassLocal, §28.7); and the scope-exit disposal driver
// (emitDisposals, emitAllDisposals, emitLoopDisposals).
//
// `common::isOwningPrefixedName` (used by the collection-element disposer paths)
// is declared in common/BuiltinRegistry.hpp, transitively included via
// LlvmBackend_impl.hpp.

namespace bezel::codegen {


// §28.4: at most one destructor per class. Returns the AST DestructorDecl for
// `ts` (or nullptr if none — emitDisposerBody then skips the destructor-body
// step and runs only owned-field disposal + base-chain Dispose).
const ast::DestructorDecl* Emitter::findDestructor(const sema::TypeSymbol* ts) {
    for (const sema::Symbol* m : ts->members) {
        if (dynamic_cast<const sema::DestructorSymbol*>(m)) {
            return dynamic_cast<const ast::DestructorDecl*>(m->decl);
        }
    }
    return nullptr;
}


void Emitter::emitDisposerBody(const sema::TypeSymbol* ts, llvm::Function* fn) {
    // §28.2 / §28.4 / §28.6: `fn` is the PUBLIC, idempotent Dispose (vtable slot
    // 1). It guards a single call to the UNGUARDED core (disposerCoreFns_[ts])
    // with the per-object run-once flag, so the destructor body, owned-field
    // auto-disposal and base chain execute AT MOST ONCE per object no matter how
    // many dispose sites reach this Dispose (explicit `.Dispose()`, scope-end
    // auto-dispose, owned-field auto-dispose, exception unwind). The block free
    // is NOT performed here; it is done once by the disposal site that holds the
    // owning slot (emitDisposals / emitOwnedFieldDisposals / the caught-exception
    // and unwind paths), so a re-dispatch never reads an already-freed object.
    llvm::Function* coreFn = disposerCoreFns_[ts];
    emitDisposerCore(ts, coreFn);

    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    // §28.2: load the run-once flag; if already set, this is a repeat disposal —
    // a no-op (§28.2: "subsequent calls are no-ops"). Else set it BEFORE running
    // the core so a re-entrant dispose during the core (e.g. a destructor that
    // disposes `this` transitively) still short-circuits.
    llvm::StructType* st = classStruct(ts);
    unsigned fi = disposedFlagIndex(ts);
    llvm::Value* flagAddr =
        builder_.CreateStructGEP(st, currentThis_, fi, "disposed.flag");
    llvm::Value* already = builder_.CreateICmpNE(
        builder_.CreateLoad(disposedFlagTy(), flagAddr),
        llvm::ConstantInt::get(disposedFlagTy(), 0), "already.disposed");
    auto* doBB = llvm::BasicBlock::Create(llvm_, "dispose.do", fn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "dispose.ret", fn);
    builder_.CreateCondBr(already, retBB, doBB);
    builder_.SetInsertPoint(doBB);
    builder_.CreateStore(llvm::ConstantInt::get(disposedFlagTy(), 1), flagAddr);
    builder_.CreateCall(coreFn, {currentThis_});
    builder_.CreateBr(retBB);
    builder_.SetInsertPoint(retBB);
    builder_.CreateRetVoid();
}


void Emitter::emitDisposerCore(const sema::TypeSymbol* ts, llvm::Function* fn) {
    // fix #3936 / #2702 item 2 (§28.3 / §28.4 / §28.9, v0.290+): the
    // per-body emitter fields a NESTED body emission must not INHERIT —
    // the destructor-body pair (emittingDisposerBody_ / disposerEntryExc_),
    // the catch-arm pair, the accessor-representation pair
    // (accessorReturnTypeNode_ / accessorReturnTypeFn_), the choice
    // receiver-identity recovery field, the suspend/resume frame, the
    // narrow per-call channels, and the root-scope registration channels
    // (see NestedBodyScope's definition in LlvmBackend_Assign.cpp ~5114
    // for the full list and the members deliberately inherited) — are
    // saved, neutralised and restored by ONE guard. This is the same
    // #2702 shape the other six lazy emitters already adopt
    // (ensurePropertyBlockGetter / ensureIndexerGetterFn /
    // ensureIndexerSetterFn / ensurePropertyAccessorFn /
    // emitIteratorMethod / emitAsyncMethod). Without it, the two
    // mid-emission call paths this function is reached from
    // (`emitSpecializedDisposer` from `specializedVtableFor` ->
    // `emitInstanceInit` for a generic instantiation with an `owned T`
    // field, #816 / §28.5; and `specializedDisposerCoreFor` from
    // `emitBaseDisposeChain`) leave the disposer body of a TYPED-`Owner`
    // reached MID-emission of a HOLDER destructor carrying the HOLDER's
    // `emittingDisposerBody_ = true` — and on the way out, the cached
    // `Owner.dispose` IR is frozen module-wide by `disposerCoreFns_[ts]`
    // with that flag's *wrong* value. The disposer's own `Sink.Seen := _n`
    // (a block-body property getter lowered from this disposer body)
    // therefore reads the HOLDER caller's stored `Sink.Seen` instead of
    // the property's own `_n` (#3936's measured divergence, two callers
    // in two orders). The RAII guard's dtor restores everything on exit.
    NestedBodyScope nestedScope(*this);
    // fix #3921 (§27.5 / §28.4 / §28.9, v0.294): the SAVE/RESTORE half of the
    // capture-cell clear+re-pin block ~150 lines below (fix #2887's). A
    // `clear()` on per-lowering emitter state and a save/restore around nested
    // emission are two halves of ONE contract (issue 3505), and this emitter
    // shipped only the first half — the same asymmetry #3500 closed for
    // `emitIteratorMethod`, #3507 for `emitAsyncIteratorMethod` and #3705 for
    // `emitAsyncMethod`, all three of which already construct exactly this
    // scope. (Not #3697/#3705 for both async emitters: those two issues are
    // the `BorrowingReclaimSetScope` that sits on the NEXT line at each of
    // those sites — a different member family. Verified against
    // `eea0ebe` "fix #3507: re-establish the §27.5 capture-cell reclaim
    // analyses for an async iterator's Resume" and `17931fb` "fix #3705: give
    // emitIteratorMethod and emitAsyncMethod the reclaim save/restore #3697
    // gave the async twin".)
    //
    // WHY IT MATTERS HERE. `emitDisposerCore` is not only reached from the
    // top-level disposer pass (where the next disposer's own pre-scan re-pins
    // everything, so clobbering is harmless). `emitSpecializedDisposer`
    // (reached from `specializedVtableFor` -> `emitInstanceInit`, i.e. from
    // `lowerNew` for a generic instantiation with an `owned T` field, #816 /
    // §28.5) and `specializedDisposerCoreFor` (from `emitBaseDisposeChain`)
    // both call it MID-EMISSION of an outer body. Those two sites save and
    // restore the per-function emit state — `currentFn_`, `locals_`, the
    // dispose/loop/finally stacks, the insert point — precisely for this
    // reason, but neither ever grew a save for the capture-cell family. So a
    // single `new Box<Res>(…)` anywhere in a method body silently wiped that
    // body's own §27.5 capture-cell eligibility (and its `sharedCellEnvdtor
    // FreeOwner_` #3426 decisions) for every lambda lowered AFTER it, with no
    // diagnostic and no crash: a leak, never a double-free.
    //
    // MEASURED (issue 3921, at 500 iterations under ASan/LSan): fix #3804's
    // own escaping-nested-lambda probe with one `using var b := new
    // Box<Res>(new Res());` inserted BEFORE the lambda leaked 26000 B / 1500
    // allocations — 52 B per activation, i.e. the escaped closure header (40),
    // its env (8) and the shared capture cell (4), byte-for-byte #3804's own
    // figure — while the identical program WITHOUT that one line is clean.
    // Invisible to any test that does not instantiate an owned-`T` generic in
    // the same body that also captures.
    CaptureReclaimSetScope captureReclaimScope(*this);
    // fix #3926 (§28.3 / §28.4 / §28.5 / §28.9, v0.294): the SAVE/RESTORE
    // halves of the §28.3 STRING-release and §28.9 BOX-release clear+re-pin
    // blocks below (fix #2807's string half, fix #2854's companion).  Same
    // two-halves-of-one-contract rule as #3921's `CaptureReclaimSetScope`
    // above: `emitDisposerCore` clears and re-pins these families for the
    // destructor body it is about to emit, but the two mid-emission call
    // paths (`emitSpecializedDisposer` and `specializedDisposerCoreFor`) leave
    // us MID-EMISSION of an outer body, so without the restore the outer
    // body's own `var s := "abc" + n.ToString();` and `var o: Object := n;`
    // silently lose their bind-time retain / scope-end release (19392 B /
    // 1000 allocs and 12000 B / 500 allocs at 500 iterations, issue #3926).
    //
    // Both scopes are header-local RAII beside `CaptureReclaimSetScope` in
    // `LlvmBackend_impl.hpp` so the two new scopes stay byte-identical to
    // `buildSpecializedMethodFn`'s hand-rolled six-member string block and
    // three-member box block (LANE_RULES §6 — one authority, not a second
    // drifting copy).  The `BoxReclaimSetScope` deliberately does NOT
    // re-pin `boxReleaseEligibleFn_` inside the destructor body — the
    // destructor's entries are AST-keyed and the outer body's token is
    // exactly what we want to leave in place when emission returns.  See the
    // issue body for the measured figures.
    StringReclaimSetScope stringReclaimScope(*this);
    BoxReclaimSetScope boxReclaimScope(*this);
    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    bool terminated = false;
    // fix #1607: the exception parked across the destructor body (see below),
    // kept in a local (disposerEntryExc_ itself is reset to null once the body
    // finishes) so the normal-completion restore after the `if` below can still
    // reach it.
    llvm::Value* parkedDtorExc = nullptr;
    if (const ast::DestructorDecl* dd = findDestructor(ts)) {
        // #383 / §28.8.1: suppress dynamic disposal of `using var u := move
        // this._f;` inside the destructor — the auto field-dispose below would
        // dispose the same (moved-from) field, double-freeing it.
        //
        // fix #2852 / #2817 / #2807 / #429 / §4.6 / §28.9 / §28.3 / §28.4 (v0.240):
        // run the `var` STRING-local refcount analysis for the destructor body
        // and pin the token to this fn. EVERY consumer of the three string sets
        // gates on `currentFn_ == stringReleaseEligibleFn_` (#2807/#2817), and
        // this function sets `currentFn_` but never touched that token, so no
        // string local in any destructor body was ever retained OR released:
        // both allocations (bz_string header + payload) leaked, per local, per
        // disposal (measured 2033 B / 80 allocations for 20 disposals x 2 locals).
        // The #2817 template transfers: collectStringReleaseEligible classifies
        // the body's own `var` string locals, and the pin unlocks the bind-time
        // retain + scope-end Disposal{stringRelease} that armStringLocalRefcount
        // registers.
        //
        // ORDERING. The walk is a pure AST inspection of `dd->body`, so running
        // it here (before the body is lowered) merely describes the function
        // about to be emitted; it registers no IR. The scope-end releases
        // themselves are emitted by lowerScopedBlock below at the body-end scope,
        // sitting BEFORE the post-body field-disposal cascade below (§28.4 step
        // 2) — which is the only ordering the §28.4 sequence constrains. A
        // string local cannot be live before its own declaration, so no release
        // can precede a field disposal that might read the same heap string.
        // A `var s := this.SomeStringField` borrow local (§28.3) inside the
        // body aliases a field the disposer is about to release; the borrow
        // model retains at bind and releases at scope end, so the OVERALL
        // release of the field's reference is unaffected — the local's release
        // releases the borrow, then the field cascade drops the original. The
        // `armValueLikeChoiceStrRelease` `if (emittingDisposerBody_) return;`
        // guard (LlvmBackend_Setup.cpp) covers the §11 choice-value path, not
        // the `var` string-local path; the analysis here is the correct one
        // for the latter.
        //
        // CO-GATED PARAMETER RETAIN. A destructor takes no parameters (§18),
        // so retainAssignedStringParams with an empty params vector is a
        // structural no-op — but the invariant is that the two predicates move
        // together, and the pin also enables the drop-on-overwrite release the
        // pin above enables for anything `localSlot()` resolves (an enclosing
        // local read of a string parameter is impossible here). Calling it
        // makes the invariant checkable at review. The empty vector is the
        // documented "no params" representation (see the destructor's
        // `DestructorDecl::body` AST — no `params` field, by language design).
        collectStringReleaseEligible(dd->body);
        stringReleaseEligibleFn_ = fn;
        retainAssignedStringParams(dd->body, /*params=*/{});
        //
        // fix #2887 / §28.4 / §27.5 / §28.3 / §28.9 (v0.240): the `var` CLOSURE-
        // LOCAL refcount analysis and the captured-cell free analysis must
        // describe this disposer fn too. EVERY consumer of the three closure
        // sets gates on `currentFn_` matching the analysed function —
        // `lowerLocalDecl` registers the scope-end `bz_closure_release` of a
        // non-escaping `var` closure local (and its `boxedCellFree` for any
        // captured local's cell, #429) ONLY when `currentFn_ ==
        // closureReleaseEligibleFn_` / `currentFn_ == cellFreeEligibleFn_`.
        // This function set `currentFn_` to the freshly-created disposer
        // function but never armed the two tokens, so the gate was false for
        // every closure local and every captured cell in any destructor body
        // — both allocations (closure cell + env block on the local path;
        // capture cell on the captured path) leaked, per local, per disposal
        // (measured 48 B / 20 allocations for 20 disposals of one closure
        // local). The #2817 / #422 / #429 template transfers: classify the
        // body's own `var` closure locals, classify its captured cells (in the
        // documented dependency order — `collectCellFreeEligible` consumes
        // `closureReleaseEligible_`'s result, so it MUST run AFTER it), and
        // pin all three fn tokens to `fn`. The pin unlocks the scope-end
        // release registrations the consumers emit during `lowerScopedBlock`
        // below, which run BEFORE the post-body field-disposal cascade
        // (the only ordering §28.4 step 1 ↔ step 2 constrains). The
        // `unwindUnsafeCellFree_` set is the #2334 escape gate; the
        // `boxCaptureLocals_` / `singleCaptureLocals_` / `assignedCaptureVars_`
        // / `definitelyEscapingLambdas_` clears are the #694 / #2706 / #2960
        // invariants the analysers' preconditions assume (a stale entry from
        // a previous body would otherwise mis-admit a local whose
        // lifetime-escape verdict was already computed for THAT body). The
        // `boxReleaseEligible_` / `boxReassignedLocals_` clears are the
        // parallel invariants for the #672 / #8.6 / #28.9 Object-typed value
        // box release path. Mirrors `buildSpecializedMethodFn` (Setup.cpp
        // ~8425-8482) and `emitAsyncLambda` (Async.cpp ~1268-1309) verbatim
        // modulo the `*ci.stmts` → `dd->body` substitution.
        closureReleaseEligible_.clear();
        closureReleaseEligibleFn_ = fn;
        boxReleaseEligible_.clear();
        boxReassignedLocals_.clear();
        boxCaptureLocals_.clear();
        singleCaptureLocals_.clear();
        assignedCaptureVars_.clear();
        returnedCaptureVars_.clear();  // fix #4198
        definitelyEscapingLambdas_.clear();
        boxCaptureLocalsFn_ = fn;
        cellFreeEligible_.clear();
        unwindUnsafeCellFree_.clear();
        sharedCellEnvdtorFreeOwner_.clear();  // fix #3426
        cellFreeEligibleFn_ = fn;
        collectClosureReleaseEligible(dd->body);
        // fix #4174 follow-up: the closure twin of the string parameter
        // retain above — see retainAssignedClosureParams' own comment. A
        // destructor takes no parameters (§18), so this is a structural
        // no-op (mirrors the CO-GATED PARAMETER RETAIN note above for
        // strings) — called to keep the retain/release invariant checkable
        // at review, exactly as retainAssignedStringParams(dd->body, {}) is.
        retainAssignedClosureParams(dd->body, /*params=*/{});
        collectBoxReleaseEligible(dd->body);
        collectBoxCaptureLocals(dd->body);
        collectDefinitelyEscapingLambdas(dd->body);
        // Must run AFTER collectClosureReleaseEligible (uses its result).
        collectCellFreeEligible(dd->body);
        //
        // fix #1468 / §28.3 "Exception-path discharge" / §28.4: capture bz_exc as
        // it stands at disposer entry. When this destructor runs as an owned
        // local/field is disposed DURING stack unwinding, bz_exc already holds the
        // PROPAGATING exception here. The body's pending-checks (emitPendingCheck)
        // compare against this captured value so they fire only for a NEW
        // exception the body itself threw — not the pre-existing propagating one,
        // which would otherwise misfire, abandon the rest of the destructor, and
        // leak the object plus its whole owned subtree. The load sits in the entry
        // block (the current insert point), so it dominates every body block. On
        // the normal, non-unwind disposal path this value is null at runtime, so
        // the comparison in emitPendingCheck reduces to the plain `exc != null`.
        disposerEntryExc_ =
            builder_.CreateLoad(ptrTy(), emitExcSlot(), "disposer.entry.exc");
        // fix #1607 / §28.3 "Exception-path discharge" / §28.4: the AND-trick
        // above (emitPendingCheck comparing against disposerEntryExc_) only
        // covers pending-checks emitted DIRECTLY inside this destructor body
        // (gated on emittingDisposerBody_). It does NOT cover a callee compiled
        // as an ORDINARY function — e.g. `destructor S.Bump(); end destructor;`
        // where Bump is a plain static method whose own body touches a user
        // static. Bump is compiled once with emittingDisposerBody_ == false, so
        // its own §22.13 static-init guard's emitPendingCheck reads the raw
        // bz_exc slot with no adjustment: seeing the propagating exception
        // still parked there, it misreads it as "Bump itself just threw" and
        // returns before running the rest of Bump's body (S.x never
        // increments) — the residual #1468 leak this fixes.
        //
        // PARK bz_exc for the run of the destructor body: clear it here so
        // every callee (Bump, and anything it transitively calls) sees a
        // clean exception context, and restore it — combined via select with
        // any genuinely NEW exception the body raised — at every reachable
        // exit from the body (the normal fallthrough below, and an early
        // `return;` inside the body, per #3459: LlvmBackend_Stmts.cpp's plain
        // ReturnStmt path). On the normal, non-unwind disposal path this
        // value is null at runtime, so clear/restore is a no-op there.
        parkedDtorExc = disposerEntryExc_;
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                             emitExcSlot());
        emittingDisposerBody_ = true;
        // fix #1546 / §28.4 (v0.180 throwing-destructor defined behavior):
        // "Before it propagates, the remaining steps of the generated
        // Dispose() still run: the remaining owned-field disposals ...
        // and the base-class chain complete, and physical reclamation
        // still occurs." Without a local redirect, a `throw` (or a
        // propagating call) directly inside the destructor body branches
        // straight to this function's unwindBB()/propagateBB() — which,
        // for a non-`main` function, is an immediate `ret void` — skipping
        // every step below. Install a local unwind target for the body
        // only: a throw inside it lands in dtorThrewBB (after
        // emitThrowValue / emitPendingCheck has already stored the new
        // exception into bz_exc and run any unwind disposals the body's
        // OWN scopes need — e.g. a `using var` inside the destructor),
        // which then falls into the SAME post-body cleanup used for
        // normal completion, so the field cascade / base chain / reclaim
        // run on the throwing path exactly as on the non-throwing one.
        // (An early `return;` inside the body is a separate, pre-existing
        // deferred corner — see the comment below — unaffected by this.)
        llvm::BasicBlock* savedUnwindTarget = unwindTarget_;
        auto* dtorThrewBB =
            llvm::BasicBlock::Create(llvm_, "dtor.threw", fn);
        unwindTarget_ = dtorThrewBB;
        bool bodyFellThrough = !lowerScopedBlock(dd->body);
        unwindTarget_ = savedUnwindTarget;
        emittingDisposerBody_ = false;
        disposerEntryExc_ = nullptr;
        if (bodyFellThrough) {
            // Normal completion (no throw reached dtorThrewBB on this
            // path): join with any throwing path via the same cleanup
            // block below instead of running the shared cleanup twice.
            builder_.CreateBr(dtorThrewBB);
        }
        builder_.SetInsertPoint(dtorThrewBB);
        terminated = false;  // fall through into the shared cleanup below,
                              // on BOTH the thrown and the fell-through path
    }
    // §28.4: after the destructor body, dispose owned fields (reverse
    // declaration order), then chain to the base class's CORE disposer.
    // (A destructor that returns early skips this — destructors with
    // both an early return and owned fields are a deferred corner.)
    if (!terminated) {
        // fix #1607: restore the parked exception (see above) before running
        // the owned-field cascade / base chain, which may themselves recurse
        // into another type's emitDisposerCore (the "transitive owned-field
        // compounding" case #1468's tests already cover) — that nested call
        // must see the correctly-restored propagating value at ITS OWN entry.
        if (parkedDtorExc) {
            llvm::Value* excNow =
                builder_.CreateLoad(ptrTy(), emitExcSlot(), "postdtor.exc");
            llvm::Value* isNew = builder_.CreateICmpNE(
                excNow, llvm::ConstantPointerNull::get(ptrTy()));
            // fix #2358 / §28.4 + §31.2 (finally-supersession discipline, cited
            // by §28.4's "the later exception supersedes the earlier one"
            // multi-value-cleanup-pass rule): when the destructor body ITSELF
            // threw (isNew) while an exception was ALREADY propagating at this
            // disposal's entry (parkedDtorExc non-null AT RUNTIME — an
            // "exception-path discharge", §28.3), the new exception supersedes
            // the parked one. Per §31.2, "the superseded exception is disposed
            // by the runtime exactly once, as if its handling had completed" —
            // dispose+reclaim it HERE, exactly as emitPendingCheckIf /
            // emitNewPendingCheck already do for the sibling finally/catch-guard
            // supersede (#1216/#1149), including the #1288/§30.9 deferred-fault
            // clear so a superseded task-owned fault's single-dispose invariant
            // still holds. Without this, parkedDtorExc was silently dropped on
            // the floor by the select below — losing the original exception's
            // disposal and leaking its object (the reported #2358 48-byte leak).
            // Gated on BOTH conditions at runtime (not just isNew) so the
            // ordinary non-unwind throwing-destructor path (parkedDtorExc ==
            // null here — no exception was in flight) takes the plain-select
            // path unchanged: freeing a null parkedDtorExc would be wrong.
            llvm::Value* hadPending = builder_.CreateICmpNE(
                parkedDtorExc, llvm::ConstantPointerNull::get(ptrTy()));
            llvm::Value* supersedes = builder_.CreateAnd(isNew, hadPending);
            auto* supersedeBB =
                llvm::BasicBlock::Create(llvm_, "dtor.supersede", currentFn_);
            auto* restoreBB =
                llvm::BasicBlock::Create(llvm_, "dtor.restore", currentFn_);
            builder_.CreateCondBr(supersedes, supersedeBB, restoreBB);
            builder_.SetInsertPoint(supersedeBB);
            builder_.CreateCall(
                declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                         {ptrTy()}),
                {parkedDtorExc});
            emitDisposeAndReclaim(parkedDtorExc);
            builder_.CreateBr(restoreBB);
            builder_.SetInsertPoint(restoreBB);
            llvm::Value* restored =
                builder_.CreateSelect(isNew, excNow, parkedDtorExc);
            builder_.CreateStore(restored, emitExcSlot());
        }
        emitPhysicalReclamationSteps(ts);
        builder_.CreateRetVoid();
    }
}


// §28.12 step 2, verbatim: "owned fields are disposed in reverse declaration
// order, the base class's reclamation chains, and the heap block is released.
// This phase is the same regardless of which logical-disposal path ran."  The
// first two of those three steps live here (the block release is
// emitReclaimBlock, at the owning disposal SITE, because a re-dispatchable
// Dispose must never free); this is the ONE spelling of them, called from
// emitDisposerCore after the destructor body and from emitDisposerReclaimBody
// INSTEAD of it (fix #3470: the `await using` path had no spelling at all — it
// called a bare `free(obj)`, so every owned field and every base level of an
// awaited-DisposeAsync object was stranded).  Emits no terminator.
void Emitter::emitPhysicalReclamationSteps(const sema::TypeSymbol* ts) {
    emitOwnedFieldDisposals(ts);
    emitBaseDisposeChain(ts);
    // fix #2951 (§28.4 step 3 / §28.2 / §28.9 / §31.4, v0.240): release the
    // INHERITED §31.4 header. §28.4's generated `Dispose()` step 3 states,
    // verbatim: "Chains to the base class's `Dispose()`." `emitBaseDisposeChain`
    // above iterates `ts->baseTypes`, which holds only ancestors that HAVE a
    // TypeSymbol — and a built-in exception has none — so for a class whose
    // base is `Exception` (or any built-in exception) step 3 chained to
    // NOTHING. `vt$<C>` slot 2 is the class's own `bz$<C>$Dispose`, whose
    // core never touched the header, so the shared `bz$exDisposeMsg` that a
    // directly-constructed `new Exception(msg)` disposes through was never
    // reached from a subclass and NOTHING released its Message (or
    // ArgumentException's ParamName). Measured on the 20-iteration repro:
    // `711 byte(s) leaked in 40 allocation(s)`, against an identical loop on
    // the built-in `Exception` that is clean.
    //
    // §28.9 makes the slot's obligation explicit for a runtime-managed
    // string field, and §28.2's exactly-once is the whole contract here, so
    // both directions were measured, not just the leak:
    //
    //   * UNDER-release — LSan on the amplified loops; clean now.
    //   * OVER-release — LSan is blind to it, so it is measured by keeping
    //     an independent reference alive ACROSS the disposal and reading it
    //     after, with churn in between: one `+1` string aliased into TWO
    //     subclass instances, both disposed, then the original read. An
    //     over-release faults there under ASan; it does not.
    //
    // Exactly-once holds structurally, not just on the probes, for the two
    // shapes that would otherwise release twice:
    //   * A MULTI-LEVEL hierarchy (`ExB: ExA: Exception`). The gate is the
    //     TRANSITIVE `hasBuiltinExceptionBase`, so both levels' cores carry
    //     this call and `emitBaseDisposeChain` runs ExA's core from ExB's.
    //     `bz$exDisposeMsg` NULLS each slot after releasing it (see
    //     setupBuiltinExceptions) and `bz_string_release` is null-safe, so
    //     the second call is a no-op. The transitive gate is deliberate: it
    //     also covers a LOCAL class over an IMPORTED exception base, where
    //     the chain's reach into the dependency's compiled core is not this
    //     module's to guarantee.
    //   * §28.6 REPEAT DISPATCH (explicit `Dispose()` then scope-end). The
    //     public wrapper's run-once flag already stops the second entry, and
    //     the slot-nulling makes it idempotent even if it did not.
    //
    // BLOCKED UNTIL NOW, and both blockers are real: #2902 (the `base(msg)`
    // chain stored the message UN-RETAINED, so releasing it here freed a
    // reference the caller still owned — measured `heap-use-after-free ...
    // #2 bz$exDisposeMsg`) and #2950 (an imported subclass was laid out
    // WITHOUT the header, so the exporter-compiled disposer handed the
    // importer's FIELD VALUE to `bz_string_release` — `SEGV on unknown
    // address 0x2b`, rdi = the field's value). Both are in this batch; this
    // call is only sound with them.
    //
    // CauseChain@24 is deliberately NOT released here — see the #811/#1389
    // reasoning on `bz$exDisposeMsg`: it is reclaimed at the terminal
    // `emitDisposeAndReclaim` site, which runs exactly once per object,
    // rather than in a disposer that can be re-dispatched. (That reclaim
    // now ALSO covers a user subclass: #2989 widened
    // `bz_exc_reclaim_causechain`'s gate to walk the type-descriptor chain,
    // so a user subclass's vt$<C> is recognized via its btd$Exception
    // ancestor — the same authority `o is Exception` uses — not just the
    // built-in vtable-identity table.)
    //
    // Inherits the pre-existing corner noted above: a destructor that
    // returns early leaves `terminated` set and skips this block entirely,
    // exactly as it already skips the owned-field cascade and the base chain.
    if (hasBuiltinExceptionBase(ts) && builtinExceptionDisposeFn_ &&
        currentThis_)
        builder_.CreateCall(builtinExceptionDisposeFn_, {currentThis_});
}


// fix #3470 / §28.12 step 2 (v0.283): emit `bz$<C>$DisposeReclaim` — the
// PHYSICAL RECLAMATION the `await using` path issues after the awaited
// `DisposeAsync` has returned.  §28.12 step 2, verbatim:
//
//   "Physical reclamation runs once, after logical disposal has completed:
//    owned fields are disposed in reverse declaration order, the base class's
//    reclamation chains, and the heap block is released. This phase is the
//    same regardless of which logical-disposal path ran. For `await using`, the
//    runtime invokes physical reclamation as the second step of the implicit
//    `Dispose()` it issues after `DisposeAsync` returns; the destructor body
//    that would normally run in the synchronous path is skipped."
//
// So this is EXACTLY emitDisposerBody minus the destructor body: the same
// run-once flag guard around the same emitPhysicalReclamationSteps.  The heap
// block is released by the disposal SITE (emitReclaimBlock), as for every other
// disposer, so a repeat dispatch can never read a freed object.
//
// THE FLAG GUARD IS A PARITY ROW, NOT A MEASURED-FALLIBLE GUARD — stated that
// way because it was measured and the measurement refuted the obvious claim.
// It is here because this function is the same KIND of entry point as
// emitDisposerBody (a once-per-object cascade over the same fields) and must
// carry the same §28.2 idempotence, and because a second cascade WOULD be
// unsound if anything reached it: emitOwnedFieldDisposals nulls the slot after
// disposing an owned CLASS/task/iterator/event/Shared field, but the owned
// COLLECTION arm (`it->collTy` — `bz_array_free` / `bz_llist_free` /
// `bz_dict_free`) does NOT — it frees the backing and leaves the field pointing
// at it, so a repeat pass double-frees every owned collection field.
//
// What was measured: a deliberately OVER-ADMITTING compiler, with both the flag
// CHECK and the flag SET deleted from this function, was built and run against
// the whole grid — the 7 rows of await_using_physical_reclamation_3470, the
// `await_using` (57), `disposer` (50) and `taskscope` (36) filters, the 2188-test
// lit corpus, and all seven ASan probes. Every one was green, and each probe's
// exit code and LeakSanitizer summary was identical to the guarded build. So no
// program reachable on today's compiler enters this function twice: sema
// forbids `this.Dispose()` (§28.6) and `Dispose()` through a borrow, an explicit
// `Dispose()` on the owning local DISCHARGES the obligation so no scope-end
// disposal is registered at all, and emitDisposals nulls the slot immediately
// after this call. The guard costs a compare and is the difference between
// "unreachable today" and "unsound the day some path reaches it"; do not remove
// it on the grounds that nothing exercises it.
void Emitter::emitDisposerReclaimBody(const sema::TypeSymbol* ts,
                                      llvm::Function* fn) {
    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    // §28.2 idempotence, exactly as emitDisposerBody spells it: load the
    // run-once flag; if already set this reclamation has run (or the full
    // synchronous Dispose has), so it is a no-op.  Set it BEFORE the cascade so
    // a re-entrant disposal during the cascade still short-circuits.
    llvm::StructType* st = classStruct(ts);
    unsigned fi = disposedFlagIndex(ts);
    llvm::Value* flagAddr =
        builder_.CreateStructGEP(st, currentThis_, fi, "disposed.flag");
    llvm::Value* already = builder_.CreateICmpNE(
        builder_.CreateLoad(disposedFlagTy(), flagAddr),
        llvm::ConstantInt::get(disposedFlagTy(), 0), "already.disposed");
    auto* doBB = llvm::BasicBlock::Create(llvm_, "reclaim.do", fn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "reclaim.ret", fn);
    builder_.CreateCondBr(already, retBB, doBB);
    builder_.SetInsertPoint(doBB);
    builder_.CreateStore(llvm::ConstantInt::get(disposedFlagTy(), 1), flagAddr);
    // The class's own destructor body is deliberately NOT run here (§28.12 step
    // 1: "the destructor body (if any) is not also run, because `DisposeAsync`
    // supersedes it").  emitBaseDisposeChain, inside the shared steps, still
    // chains to each BASE's core — which does run that base's destructor body.
    // That is the conservative reading of step 2's "the base class's
    // reclamation chains": only the class whose `DisposeAsync` ran has its
    // synchronous cleanup superseded, so a base level's cleanup is preserved
    // rather than silently dropped, and the emitted sequence differs from the
    // synchronous path by exactly the one body §28.12 names.  The alternative
    // reading (physical reclamation is destructor-free at every level) is
    // filed as a spec-clarification issue rather than chosen silently here.
    emitPhysicalReclamationSteps(ts);
    builder_.CreateBr(retBB);
    builder_.SetInsertPoint(retBB);
    builder_.CreateRetVoid();
}


// §28.7 / §30.5 (#2173 / #1889): drop-on-overwrite / scope-end of an owned
// Task-family FIELD. The full using-var task drop sequence MUST run on the
// OLD task pointer at fieldAddr (load-old already done by the caller; the
// slot also already null-guarded by the caller — for the field-store case,
// the caller has nulled the field while retaining both the OLD task in `oldVal`
// and the already-evaluated new task in SSA; for the scope-end case, the caller
// has loaded the field and confirmed it is non-null). The sequence is identical
// the using-var task disposal (the disposal sequence in this file's
// emitDisposals -> if (it->task) block), EXTRACTED here so a single
// maintenance burden covers both the field-store path (#2173) and the
// scope-end path (#1889). Specifically:
//
//   1. Error-obj prelude (the §30.9 fault-ownership check): a faulted task
//      owns its fault exception and disposes it exactly once at task
//      disposal. We dispose the fault now (or defer if it is in flight per
//      the #1288 / #1197 scope-end protocol), or skip unconditionally for
//      a borrowed aggregate (§30.10 / #1529).
//
//   2. Per-T disposer attach (§30.5 / #1258 / #1503): for an OwningTask
//      <T:class> field, the disposer thunk for T is attached via
//      bz_task_set_result_disposer so an unconsumed Owned T result is freed
//      whenever bz_task_free reclaims the task (whether synchronously now
//      or later via fire-and-forget's completion continuation).
//
//   3. Fire-and-forget (§30.5 / §30.10): bz_task_fire_and_forget
//      registers a continuation that calls bz_task_free when the task is
//      done. In checked mode, bz_task_fire_and_forget_checked is used so
//      an abandoned continuation touching disposed state raises
//      DisposedAccessException (instead of UAF).
//
// `target` is the field expression (used to look up the field symbol and
// hence the declared Task/Task<T>/OwningTask<T> type); for the field-store
// case the original `target` is passed in by lowerOwnedTaskFieldStore, for
// the scope-end case the field's expression is reconstructed from the
// FieldSymbol pointer (its `identifier` is on the field itself).
//
// `fieldAddr` is the slot address (struct field GEP); `oldVal` is the
// already-loaded task pointer at that slot. The helper does NOT null the
// slot — the scope-end caller nulls the slot after the call (the re-
// entrant auto-dispose guard mirrors the subscriptionField / eventSource-
// Field pattern lower in this file); the field-store caller nulled the slot
// before this call and stores the already-evaluated new value afterward.
void Emitter::emitOwnedTaskDisposal(const sema::FieldSymbol* fs,
                                    llvm::Value* oldVal) {
    emitTaskDisposal(fs ? taskFamilyTypeFromField(fs) : nullptr, oldVal);
}

// §28.3 / §30.5 (#1905): the full Task-family disposal protocol for an
// already-evaluated owned task value.  Field disposal delegates here above;
// unbound Task temporaries consumed by synchronous `Wait()` use the same
// protocol at their full-expression boundary.  Sharing this sequence is
// load-bearing for fault-object ownership and unconsumed OwningTask<T> results —
// a bare bz_task_free would reclaim only the task struct.
void Emitter::emitTaskDisposal(const sema::TypeRef* taskTy,
                               llvm::Value* oldVal) {
    if (!oldVal) return;
    if (oldVal == llvm::ConstantPointerNull::get(ptrTy())) return;
    // §30.5 / §29.2 (#1891): "the iteration variable is owned; the §28.3
    // discharge rules apply on every body-exit edge" — for an await-for
    // binding, TakeCurrent transferred the element into the for-slot, so
    // explicit `x.Dispose()` (or the auto-drop at iteration end) is this
    // scope's sole reclaim path.  The async path was previously omitted; run
    // the dispose+free the destructor call did not.
    // Per §30.15 a Channel<T>.Receive() task uses bz_channel_receive_abandon
    // instead. A plain field typed Task<T> / OwningTask<T> cannot
    // syntactically be a Channel<T>.Receive() call (the field's type is the
    // task type, not a call), so the receive-abandon branch is unreachable
    // here. Defensive note: a `using var` assigned `Channel<T>.Receive()`
    // gets a *local* not a field, so emitOwnedTaskDisposal is never called
    // for it.
    //
    // §30.9 fault ownership (v0.179): a faulted task OWNS its fault
    // exception and disposes it exactly once at task disposal. The task
    // struct is freed by bz_task_free / fire-and-forget but NOT the Bezel
    // exception object; only Bezel codegen can run its destructor. Three
    // paths:
    //   • BORROWED aggregate (§30.10 / #1529): a WhenAll/WhenAll<T>
    //     aggregate never owns its error_obj — skip unconditionally.
    //   • NOT in flight (not borrowed): dispose the exception now.
    //   • IN FLIGHT (borrowed by an awaiter mid-dispatch): defer disposal
    //     to the catch arm's terminal act (#1288 / #1197).
    // (A still-running async task is not yet errored here → skipped; a
    // non-faulted task has a null error_obj → skipped. bz_task_free below
    // ignores error_obj, so the now-dangling task->error_obj is never read.)
    {
        auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
        llvm::Value* err = builder_.CreateCall(isErrFn, {oldVal}, "otask.err");
        auto* edBB = llvm::BasicBlock::Create(llvm_, "otask.errdisp",
                                              currentFn_);
        auto* eaBB = llvm::BasicBlock::Create(llvm_, "otask.errcont",
                                              currentFn_);
        builder_.CreateCondBr(err, edBB, eaBB);
        builder_.SetInsertPoint(edBB);
        auto* fbFn = declareC("bz_task_fault_borrowed",
                              llvm::Type::getInt8Ty(llvm_), {ptrTy()});
        llvm::Value* fb = builder_.CreateCall(fbFn, {oldVal}, "otask.faultborrowed");
        llvm::Value* isFB = builder_.CreateICmpNE(
            fb, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
        auto* borrowedAggBB = llvm::BasicBlock::Create(
            llvm_, "otask.eo.aggborrowed", currentFn_);
        auto* checkBB = llvm::BasicBlock::Create(llvm_, "otask.eo.check",
                                                 currentFn_);
        builder_.CreateCondBr(isFB, borrowedAggBB, checkBB);
        builder_.SetInsertPoint(borrowedAggBB);
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(checkBB);
        auto* eoFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
        llvm::Value* eo = builder_.CreateCall(eoFn, {oldVal}, "otask.errobj");
        llvm::Value* eoNull = builder_.CreateICmpEQ(
            eo, llvm::ConstantPointerNull::get(ptrTy()));
        auto* doBB = llvm::BasicBlock::Create(llvm_, "otask.eodisp",
                                              currentFn_);
        builder_.CreateCondBr(eoNull, eaBB, doBB);
        builder_.SetInsertPoint(doBB);
        auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                      llvm::Type::getInt8Ty(llvm_), {ptrTy()});
        llvm::Value* bv = builder_.CreateCall(isBorrowedFn, {eo},
                                              "otask.eo.borrowed");
        llvm::Value* isB = builder_.CreateICmpNE(
            bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
        auto* deferBB = llvm::BasicBlock::Create(llvm_, "otask.eo.defer",
                                                 currentFn_);
        auto* dispBB  = llvm::BasicBlock::Create(llvm_, "otask.eo.dispnow",
                                                 currentFn_);
        builder_.CreateCondBr(isB, deferBB, dispBB);
        builder_.SetInsertPoint(deferBB);
        auto* deferFn = declareC("bz_defer_exc_dispose",
                                 llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(deferFn, {eo});
        // §30.9 / #3030: null the task's error_obj field even on the
        // DEFER path. The exception is in flight (the await/`.Result`
        // re-raise marked it borrowed) and the catch arm's terminal act
        // is the disposer of record (it disposes via bz_is_exc_deferred →
        // emitDisposeAndReclaim + bz_clear_exc_deferred, the §30.9
        // in-flight-deferral contract). The task's error_obj field, if
        // left non-null after the defer, would be re-disposed by
        // bz_task_fire_and_forget's FAST path below (the #3030 fix's
        // new `if (error_obj && !fault_borrowed)` arm) — and the catch
        // arm then reads the freed pointer (SIGSEGV / ASan UAF). The
        // dispBB branch (synchronous dispose) gets the same null-write
        // a few lines below for the same reason. Without this null the
        // in-flight path becomes a use-after-free the moment #3030's
        // FAST-path disposal guard is enabled.
        auto* clearEoDeferFn = declareC("bz_task_clear_error_obj",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(clearEoDeferFn, {oldVal});
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(eo);
        // §30.9 / #3030: the task's error_obj field still holds a dangling
        // pointer to the exception we just freed. bz_task_fire_and_forget
        // (called below) takes the FAST path on a settled task and its
        // error_obj-disposal guard would re-dispose the freed pointer. Null
        // the field now so the FAST path's `if (error_obj && ...)` sees null.
        auto* clearEoFn = declareC("bz_task_clear_error_obj",
                                   llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(clearEoFn, {oldVal});
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(eaBB);
    }
    // §30.5 / #1258 / #1503: attach the per-T disposer for the
    // produced-but-unconsumed owned class result of an OwningTask<T:class>
    // field BEFORE the task is reclaimed below. Detected by the field's
    // declared type via taskFamilyTypeFromField — mirrors the using-var
    // path's taskOwnedResultDisposer field, which is set at registration
    // time from the local's initializer type. For a Task<T> / Task / value-
    // result OwningTask<T>, the disposer is null and the conditional
    // set_result_disposer call is skipped.
    llvm::Value* taskOwnedResultDisposer = nullptr;
    if (taskTy) {
        auto taskName = [](const sema::TypeRef* rt) -> std::string_view {
            if (!rt) return {};
            if (!rt->declaration) {
                if (rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty())
                    return rt->headName();
            } else if (!rt->declaration->decl) {
                return rt->declaration->name;
            }
            return {};
        };
        if (taskName(taskTy) == "OwningTask" &&
            !taskTy->genericArgs.empty() &&
            taskTy->genericArgs[0] &&
            taskTy->genericArgs[0]->kind == sema::TypeRef::Kind::Named &&
            taskTy->genericArgs[0]->declaration &&
            (taskTy->genericArgs[0]->declaration->kind ==
                 ast::TypeDeclKind::Class ||
             taskTy->genericArgs[0]->declaration->kind ==
                 ast::TypeDeclKind::Interface)) {
            llvm::Function* thunk =
                elementPolicyFor(taskTy->genericArgs[0],
                                 /*owningFamily=*/true).disposerThunk;
            if (thunk) {
                taskOwnedResultDisposer =
                    llvm::cast<llvm::Value>(thunk);
            }
        }
    }
    if (taskOwnedResultDisposer) {
        auto* setDispFn = declareC("bz_task_set_result_disposer",
                                   llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), ptrTy()});
        builder_.CreateCall(setDispFn, {oldVal, taskOwnedResultDisposer});
    }
    // §30.10 (v0.179 / #1520): in checked disposal mode, route this
    // abandonment through bz_task_fire_and_forget_checked instead of the
    // plain entry point. See the using-var task disposal for the full
    // rationale (a still-running task's continuation may resume past
    // this point and touch state this scope's OTHER disposals just tore
    // down, and DisposedAccessException is the §30.10 source-outlives-
    // settlement diagnostic).
    if (checkedDisposal_) {
        llvm::Value* daVt = llvm::ConstantPointerNull::get(ptrTy());
        { auto vit = builtinVtable_.find("DisposedAccessException");
          if (vit != builtinVtable_.end()) daVt = vit->second; }
        auto* fn = declareC("bz_task_fire_and_forget_checked",
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {oldVal, daVt});
    } else {
        auto* fn = declareC("bz_task_fire_and_forget",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {oldVal});
    }
}


// fix #3125 (§29.8 / §30.1 / §30.5, v0.254): THE frame-teardown release of one
// `owned` PARAMETER capture, routed by the parameter's declared type.
//
// §30.1: "any `owned` parameter never moved out is disposed by the frame when
// the task reaches its terminal state"; §29.8 states the iterator twin. What
// the rule does NOT say — and what all three frame-owning emitters assumed — is
// that "disposed" means class-vtable dispatch. For a `Task`/`Task<T>`/
// `OwningTask<T>` parameter it does not: a `bz_task_t*` is a runtime struct
// with no Bezel vtable, and §30.5's disposal of a task is ABANDONMENT (the
// operation "runs to its terminal state unsupervised and its result, if owned,
// is disposed on arrival"), which is what emitTaskDisposal performs — the fault
// object first, then the per-`T` result disposer, then fire-and-forget. Sending
// a task through emitDisposeAndReclaim instead loaded "the vtable pointer" out
// of task-struct bytes and made an indirect call through it: a wild jump, and a
// SEGV on the very first slot read (measured at 701cb2d on all three emitters —
// bz$async$SinkAsync$Resume, bz$iter$Gen$Dispose, bz$asynciter$Gen$Dispose,
// each "SEGV on unknown address 0x10").
//
// This function is the single place that answers the question, so the three
// loops cannot drift the way `unwindOwnedLocalClass`'s task exclusion and this
// path already had. It is a pure ROUTING decision: whether the capture must be
// released at all is the caller's (each loop's `iteratorAutoDispose` /
// `iteratorFrameConditionalAutoDispose` / yield-state gate), and `obj` arrives
// already proven non-null by the caller's guard — which is also its runtime
// moved-from test. The caller nulls the slot afterwards, unchanged.
//
// fix #3157 (§29.8 / §30.1, v0.254): #3125's "Task ⇒ task protocol, anything
// else ⇒ vtable" fallback left every OTHER vtable-less built-in class to the
// same wild-jump shape (a `bz_random_t` / `bz_channel_t` / `bz_string_builder_t`
// has no Bezel vtable at word 0 — word 0 is seed bytes / opaque payload — so
// "load vtable, call slot 2" reads struct bytes and indirect-jumps into
// garbage). MEASURED at 6da66d4 on each of Random, Channel<int>, StringBuilder:
// SIGSEGV in bz$async$SinkAsync$Resume under generated-code ASan, identical
// signature to the #3125 Task-fault; `rax` holds `0xd673470553fada15`, the
// four seed bytes interpreted as a pointer.
//
// This function is the ONE routing decision shared by the three frame emitters
// (#3125 settled this) — `#emitAsyncOwnedParamAutoDispose` (plain async), the
// async iterator's Dispose sweep (both LlvmBackend_Async.cpp), and the sync
// iterator's owned-param sweep (LlvmBackend_Iterators.cpp). So widening it
// here closes ALL three populations at the same call site.
//
// ROUTING. The classifier below is informed — every arm names a PRECISE class
// (verified by `common::isVtableLessBuiltinClassName` or a vtable
// declaration), not a negative list:
//   (1) Task / Task<T> / OwningTask<T>          → `emitTaskDisposal`
//       (#3125).
//   (2) Any other vtable-less built-in class    → `emitVtablelessBuiltinRelease`,
//       the same per-name release the explicit `x.Dispose()` lowering and the
//       `using var` scope-exit path consult (#2967 / #2921 / #2973). Adding a
//       new vtable-less builtin to that function (next to the release it must
//       agree with) is the single place this fix has to be kept in sync. If a
//       vtable-less builtin has no arm there yet, an `internal:` warning is
//       raised and the slot is left untouched — the same #2992 "leak before
//       wild-jump" trade the `using var` chain makes, applied here too.
//   (3) Recognised class (user class / builtin exception / imported user
//       class) — has a real vtable, vtable dispatch is correct
//       (`emitDisposeAndReclaim`).
//   (4) Anything else (a primitive, a borrow-only record, an alias into
//       case (3), etc.) — `internal:` WARNING. Not the silent vtable jump.
//
// NEGATIVE LIST AVOIDANCE. The previous shape — "not a task ⇒ vtable" — IS the
// category defect the inline comment above warned about: every population this
// fix didn't explicitly name silently took the wild-jump path. Arms (2)/(3)/(4)
// together enumerate the three shapes an `owned` capture's declared type can
// take, so the negative list's silent-fail mode is gone; a future built-in
// either enters (2) (after its arm lands) or hits the (2)-no-arm warning.
// // per §29.8 / §30.1, fix #3157
void Emitter::emitOwnedParamCaptureDispose(sema::Symbol* p, llvm::Value* obj) {
    // (1) Task family — emitted by #3125.
    if (const sema::TypeRef* taskTy = taskFamilyTypeFromParam(p)) {
        emitTaskDisposal(taskTy, obj);
        return;
    }
    // (2) Any other vtable-less built-in class. `builtinTypeName` sees BOTH the
    // direct TypeRef shape and the import/alias-resolved shape (#2997);
    // `isVtableLessBuiltinClassName` is the same subtraction used to short-
    // circuit the explicit-Dispose chain (#2969) — two established predicates,
    // brought together here so the frame path matches them.
    const sema::TypeRef* pt = paramTypeOf(p);
    std::string_view bn = pt ? builtinTypeName(pt) : std::string_view{};
    if (!bn.empty() && common::isVtableLessBuiltinClassName(bn)) {
        if (emitVtablelessBuiltinRelease(bn, obj)) return;
        // No release arm yet for this name — emit a loud `internal:`, do NOT
        // fall through to the wild vtable jump the previous default did.
        ctx_.diagnostics().report(
            Severity::Warning, p->declLoc,
            std::string("internal: emitOwnedParamCaptureDispose: vtable-less "
                        "built-in class '") + std::string(bn) +
                "' has no release arm; the slot is left un-reclaimed "
                "(a bounded leak) — extend emitVtablelessBuiltinRelease to "
                "reclaim it; #3157");
        return;
    }
    // (3) Recognised class — has a vtable; `emitDisposeAndReclaim` is correct.
    // Catches user classes (local and imported) AND built-in exceptions (whose
    // `builtinVtable_` slot #3191 reads through). `pt->declaration` is set for
    // all three populations and is deliberately the discriminator (the import
    // alias / case (2) take the vtable-less path above, never this one).
    if (pt && pt->kind == sema::TypeRef::Kind::Named && pt->declaration) {
        emitDisposeAndReclaim(obj);
        return;
    }
    // (4) fix #4328 (§28.3 / §28.2 / §28.12, v0.298): an owned PARAMETER whose
    // declared type is one of the BUILT-IN INTERFACE HEADS `IDisposable` /
    // `IAsyncDisposable` (both declaration-less, in `kBuiltinInterfaceHeads` —
    // fix #4326 deliberately KEEPS them out of the vtable-less set, so case
    // (2) above correctly declines; the runtime value is always a vtable-
    // bearing user class per §8.1, every class implicitly implements
    // `IDisposable` and a class may explicitly implement `IAsyncDisposable`).
    // `emitDisposeAndReclaim` loads vtable slot 2 = Dispose and reclaims —
    // the SAME path the explicit-Dispose chain takes for an `IDisposable`-
    // typed owned slot (lowerCall's `emitExplicitDisposeChain` else-branch),
    // and the same path the explicit-Dispose chain takes when it IS the
    // declared class. For an async method holding an `owned d: IAsyncDisposable`,
    // this is a sync fallback (§28.12: "the synchronous-fallback path runs
    // for an iterator-format `await using`"; the same sync fallback applies
    // here — the compiler does not yet emit a DisposeAsync+await state
    // machine for owned-param auto-drop, so the vtable Dispose is the
    // defined degraded behavior, not a fresh defect).
    if (pt && pt->kind == sema::TypeRef::Kind::Named && !pt->declaration &&
        pt->astNode && pt->astNode->name &&
        !pt->astNode->name->parts.empty()) {
        std::string_view bn = pt->headName();
        if (bn == "IDisposable" || bn == "IAsyncDisposable") {
            emitDisposeAndReclaim(obj);
            return;
        }
    }
    // (5) Anything unrecognised — LOUD `internal:` warning, do NOT take the
    // silent vtable jump. The fix's premise is that the default must invert:
    // a path the existing classifier can answer takes its answer; a path it
    // cannot answer MUST surface the question, not paper over it.
    ctx_.diagnostics().report(
        Severity::Warning, p->declLoc,
        "internal: emitOwnedParamCaptureDispose: unrecognised owned parameter "
        "type; no Task arm, no vtable-less built-in arm, no recognised class "
        "declaration; the slot is left un-reclaimed (a bounded leak) — #3157");
}


// fix #4290 / #5249 / #5270 / #5274 (§28.4 / §28.5, v0.313): the builtin type
// name of an `owned` field of a VTABLE-LESS builtin class (Mutex, Lock,
// Semaphore, CancellationTokenSource, Channel, Stopwatch, FileStream, ...),
// or empty when `fs` is not one. These have no user TypeDecl and no vtable, so
// `isOwnedReferenceField` is false for them. The declared type is resolved
// through the ACTIVE substitution first (#5249: a class type parameter shadows
// a same-named builtin), and only an `owned` field qualifies (#5270: a plain
// field is a borrow). Single source for the container disposer
// (emitOwnedFieldDisposals) and the owned-field store gate.
std::string_view Emitter::ownedVtablelessBuiltinFieldName(
        const sema::FieldSymbol* fs) {
    const sema::TypeRef* rt = nullptr;
    if (fs->decl) {
        // SEAM-DEBT: #4290 vtableless-builtin release walker — same
        // imported-symbol seam as the rest of #3149/#3149; fix #4290
        // demands this read; canonical accessor (sema::fieldDeclTypeOf)
        // would still need the FieldDecl decl guard for the
        // `no user TypeDecl` builtin class case.
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (fd && fd->type) rt = ctx_.resolvedType(*fd->type);
    } else {
        rt = ctx_.importedMemberType(fs);
    }
    rt = resolveTypeParamRef(rt);
    if (!rt || !hasOwnedModifier(fs)) return {};
    std::string_view n = builtinTypeName(rt);
    if (n.empty() ||
        classifyVtablelessBuiltinRelease(n) == VtablelessBuiltinReleaseArm::None)
        return {};
    return n;
}


void Emitter::emitOwnedFieldDisposals(const sema::TypeSymbol* ts) {
    // Each owned field carries either a class/interface object (disposed via
    // its vtable) or — #362 / stdlib §4.5 — a heap collection (freed via
    // bz_array_free, cascading into class elements for an owning collection),
    // or — §28.13 / stdlib §2.16 (#620) — a Shared<T>/Weak<T> handle (released
    // via bz_shared_release / bz_weak_release),
    // or — §28.5/§24 (#879) — a Subscription handle (unsubscribed via
    // bz_subscription_dispose, which is null-safe).
    struct OwnedField {
        // fix #2778 / §29.7 (v0.NNN): auto-property storage is a MemberSymbol,
        // not a FieldSymbol — widening this from FieldSymbol* to Symbol* lets
        // the same struct carry both cases through the slot-1 dispatch loop.
        // The downstream consumers only need this for the `fieldIndex_` lookup
        // (which is Symbol*-keyed) and the fieldIndex_/.dwo path; the typed
        // FieldSymbol*-typed predicates in the population loop are restricted to
        // FieldSymbol below (the MemberSymbol autoprop case is classified
        // up-front and reuses the iterator-vtable disposal shape).
        const sema::Symbol* fs;
        const sema::TypeRef* collTy;  // non-null → collection field
        // §28.13 (#620): true for Shared<T> owned field (bz_shared_release),
        // false for ordinary class/exception/collection field.
        bool sharedField = false;
        // §28.13 (#620): true for Weak<T> owned field (bz_weak_release).
        bool weakField = false;
        // §28.5/§24 (#879): true for Subscription (or Subscription?) owned
        // field (bz_subscription_dispose — null-safe, so no guard needed).
        bool subscriptionField = false;
        // #1643 / §29.7: true for an owned IEnumerable<T>/IOwningEnumerable<T>
        // field — an iterator-vtable object whose Dispose is at slot 1 (which
        // frees the frame itself), dispatched below via the iterEnumDispose
        // path, NOT the class-vtable slot-2 dynamicDispose.
        bool iterField = false;
        // §6.5 / §24 (#1646): true for an owned EventSource<T>/AsyncEventSource<T>
        // field — a bz_event_source_t* freed via bz_event_source_free (null-safe),
        // which reclaims the subscriber list and the struct. No vtable.
        bool eventSourceField = false;
        // fix #1832 / stdlib §6.5 (v0.187): true for an owned
        // SharedEventSource<T> field — the cross-domain publication-point
        // sealed class. Same runtime rep (a bz_event_source_t*) as the
        // instance pair, but the disposal call is bz_shared_event_source_free
        // so the synchronized quiescence / lock discipline lands on the right
        // runtime entry point (a future mutex acquired at free time must not
        // disturb the instance path's drain).
        bool sharedEventSourceField = false;
        // §28.6 / §30.5 (#1889): true for an owned Task / Task<T> / OwningTask<T>
        // field — their Dispose() runs the full using-var task protocol via
        // emitOwnedTaskDisposal (error_obj prelude + per-T result_disposer +
        // bz_task_fire_and_forget[_checked]), the field analog of the using-var
        // task drop. isOwnedReferenceField rejects Task-family (they have no user
        // TypeDecl / no vtable), so a separate branch is needed. Mirrors the
        // iterField / eventSourceField / sharedField / weakField / subscription
        // companions for the other builtin-handle owned-field families.
        bool taskField = false;
        // fix #2830: true for an owned field of a §10 FFI-boundary buffer type
        // (CString / WString / RawBytes — stdlib §10.1–§10.3). These are
        // headerless malloc'd byte buffers (or, for RawBytes, a { ptr, len }
        // struct whose `ptr` owns a malloc'd byte buffer) with no object header
        // and no vtable, so isOwnedReferenceField's vtable-Dispose path would
        // either skip them entirely (the declaration-less annotation shape) or
        // crash on a raw-buffer vtable dispatch. Released below via a null-
        // guarded free() (CString/WString) or bz_rawbytes_free (RawBytes) —
        // the SAME call the `using var`-bound FFI buffer's `ffiBuffer` arm and
        // #2823's `registerUnwindOwnedParam` `ffiBuffer` arm emit, so the bound
        // and unbound spellings free identically. RawBytes is dispatched via
        // bz_rawbytes_free which frees the byte buffer THEN the struct
        // (stdlib §10.3 / fix #985), and CString/WString via libc free().
        bool ffiBufferField = false;
        // fix #2830: RawBytes uses bz_rawbytes_free; CString/WString use
        // libc free(). Mirrors the `rawBytes` flag on the `ffiBuffer` Disposal
        // entry used by the local-scope / parameter paths.
        bool rawBytes = false;
        // fix #4290: the type name for an `owned` field of a vtableless builtin
        // type (Mutex, Lock, Semaphore, SemaphoreToken, CancellationTokenSource,
        // Channel, ChannelSender, Stopwatch, FileStream, TaskCompletionSource,
        // Atomic, AtomicInteger, etc.).  These have no user TypeDecl and no vtable,
        // so isOwnedReferenceField returns false and the field would otherwise be
        // silently skipped.  The name is stored here so emitVtablelessBuiltinRelease
        // can dispatch to the correct runtime free function.  Empty = not a
        // vtableless builtin field.
        std::string vtablelessBuiltin = {};
        // fix #5440 / §35.4 (v0.315): true for an `owned` choice field
        // (Owning classification per classifyChoiceType). The disposal is via
        // emitOwningChoiceLocationDisposal, NOT the class-vtable path.
        bool owningChoiceField = false;
        // The resolved TypeRef for the owning choice field, passed to
        // emitOwningChoiceLocationDisposal so it can resolve the choice's
        // case declarations and per-case payload types under any substitution.
        const sema::TypeRef* owningChoiceRef = nullptr;
    };
    std::vector<OwnedField> owned;
    for (const sema::Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        // fix #2778 / §28.6 / §29.7 (v0.NNN): an `owned IEnumerable<T>` /
        // `IOwningEnumerable<T>` auto-property's backing storage is a
        // MemberSymbol (not a FieldSymbol) and was therefore skipped by the
        // FieldSymbol-only check below — the wrapped enumerator's slot-1
        // Dispose never fired, the wrap frame leaked (and the OWNING wrap's
        // own slot-1 Dispose — bz_array_free + free(self) — never ran, so the
        // backing array leaked too). The disposal walker now classifies the
        // auto-property storage here, mirroring the FieldSymbol arm below:
        // `iterField = true` lets the slot-1 dispatch loop free it through the
        // iterator-vtable path (the same code path the `isOwnedIteratorInterface
        // Field` arm uses). Gated on `isAutoPropertyStorageSymbol` so a
        // computed-property accessor (no storage) cannot enter this branch.
        if (!fs && sema::isAutoPropertyStorageSymbol(m, ctx_) &&
            fieldIndex_.find(m) != fieldIndex_.end()) {
            const sema::TypeRef* fieldTy = nullptr;
            if (auto* ms = dynamic_cast<const sema::MemberSymbol*>(m)) {
                if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ms->decl)) {
                    if (pd->type) fieldTy = ctx_.resolvedType(*pd->type);
                }
            }
            if (!fieldTy) fieldTy = ctx_.importedMemberType(m);
            if (fieldTy && isVtableBackedIEnumerableType(fieldTy)) {
            // `hasOwnedModifier` keys on `modifiers`; MemberSymbol inherits
            // that vector from Symbol. We mirror the FieldSymbol arm's
            // `isOwnedIteratorInterfaceField` gate explicitly because the
            // helper is FieldSymbol-typed and an autoprop-storage MemberSymbol
            // would not pass its `fs->decl` checks.
            bool hasOwned = false;
            if (auto* ms = dynamic_cast<const sema::MemberSymbol*>(m)) {
                for (auto mod : ms->modifiers)
                    if (mod == "owned") { hasOwned = true; break; }
            }
            if (hasOwned) {
                OwnedField of{m, nullptr};
                of.iterField = true;
                owned.push_back(of);
            }
            }
        }
        if (!fs) continue;
        if (fieldIndex_.find(fs) == fieldIndex_.end()) continue;
        if (isOwnedReferenceField(fs)) {
            owned.push_back({fs, nullptr});
        } else if (isOwnedBuiltinExceptionField(fs)) {
            // #405 / §28.5 / §31.1: an `owned` field of BUILTIN exception type.
            // It has no user TypeSymbol (so isOwnedReferenceField rejects it),
            // but it is an ordinary owned heap class instance and is disposed
            // through the same dynamic-dispatch path as a class field below
            // (vtable slot 2 = bz$exDisposeMsg, which #1033 made release the
            // owned Message/ParamName strings before the block is reclaimed).
            owned.push_back({fs, nullptr});
        } else if (const sema::TypeRef* ct = ownedCollectionFieldType(fs)) {
            owned.push_back({fs, ct});
        } else if (isOwnedSubscriptionField(fs)) {
            // §28.5/§24 (#879): an `owned` field of Subscription (or
            // Subscription?) type.  Subscription is a compiler-known opaque
            // handle with no vtable; unsubscribe via bz_subscription_dispose
            // (null-safe: `if (!subp) return;` — so Subscription? is safe too).
            OwnedField of{fs, nullptr};
            of.subscriptionField = true;
            owned.push_back(of);
        } else if (isOwnedIteratorInterfaceField(fs)) {
            // #1643 / §29.7: an `owned` IEnumerable<T>/IOwningEnumerable<T>
            // field. Its runtime value is an iterator-vtable object (Dispose at
            // slot 1, which frees the frame itself); disposed below via the
            // slot-1 path, NOT the class-vtable slot-2 emitDisposeAndReclaim.
            OwnedField of{fs, nullptr};
            of.iterField = true;
            owned.push_back(of);
        } else if (isOwnedEventSourceField(fs)) {
            // §6.5 / §24 (#1646): an `owned` EventSource<T>/AsyncEventSource<T>
            // field (the event pattern's source, now user-constructible). A
            // bz_event_source_t* with no vtable; freed via bz_event_source_free
            // (null-safe), which reclaims the subscriber list + struct.
            OwnedField of{fs, nullptr};
            of.eventSourceField = true;
            owned.push_back(of);
        } else if (isOwnedSharedEventSourceField(fs)) {
            // fix #1832 / stdlib §6.5 (v0.187): an `owned`
            // SharedEventSource<T> field. Same rep as the instance pair but
            // freed via bz_shared_event_source_free so the cross-domain
            // synchronized quiescence lands on the right entry point.
            OwnedField of{fs, nullptr};
            of.sharedEventSourceField = true;
            owned.push_back(of);
        } else if (taskFamilyTypeFromField(fs)) {
            // §28.6 / §30.5 (#1889): an `owned` Task / Task<T> / OwningTask<T>
            // FIELD. isOwnedReferenceField returns false for Task-family (they
            // have no user TypeDecl / no vtable, and the isTaskTypeName check
            // is what excludes them), so a separate branch is needed — the
            // sibling branches above (subscriptionField, iterField,
            // eventSourceField, sharedField, weakField) follow the same shape
            // for their respective builtin-handle types. The dispose sequence
            // (error_obj prelude + per-T result_disposer + fire-and-forget
            // [_checked]) is the using-var task disposal protocol, dispatched
            // in the iteration loop below via emitOwnedTaskDisposal.
            //
            // #2260: the `owned` test lives INSIDE taskFamilyTypeFromField
            // (LlvmBackend_Setup.cpp), so this arm — like every sibling above —
            // fires only for an OWNING slot. A NON-`owned` Task-family field is
            // a spec-legal BORROW (§28.5 / §22.12 rule 2(ii)) whose owner is
            // elsewhere; disposing it here was a double free.
            OwnedField of{fs, nullptr};
            of.taskField = true;
            owned.push_back(of);
        } else {
            // fix #2830: an `owned` field of §10 FFI-buffer type (CString /
            // WString / RawBytes — stdlib §10.1–§10.3). The companion arms
            // (subscriptionField, iterField, eventSourceField, sharedField,
            // weakField, taskField) all reject this case: the runtime value is
            // a headerless malloc'd byte buffer (or, for RawBytes, a { ptr,
            // len } struct whose `ptr` owns a malloc'd byte buffer, §10.3)
            // with no object header and no vtable, so the vtable-Dispose path
            // is wrong AND isOwnedReferenceField's declaration-based test
            // misses the declaration-less annotation shape (§10 names
            // resolve as a declaration-less Named TypeRef, #2809). The result
            // pre-#2830 was that the field was silently SKIPPED here and the
            // malloc'd buffer leaked at every disposal (measured: 21 B per
            // Holder from bz_string_to_utf8z in Holder$ctor; 420 B / 20
            // constructions in a loop for an imported Holder cross-module).
            // Released below via a null-guarded free() (CString/WString) or
            // bz_rawbytes_free (RawBytes) — the SAME call the using-var-bound
            // FFI buffer's ffiBuffer arm (#2733) and the owned-parameter
            // ffiBuffer arm (#2823) emit, so bound / field / parameter paths
            // free identically.
            bool isRaw = false;
            if (isOwnedFfiBufferField(fs, &isRaw)) {
                OwnedField of{fs, nullptr};
                of.ffiBufferField = true;
                of.rawBytes = isRaw;
                owned.push_back(of);
            } else {
            // fix #4290: an `owned` field of a vtableless builtin type (Mutex,
            // Lock, Semaphore, SemaphoreToken, CancellationTokenSource, Channel,
            // ChannelSender, Stopwatch, FileStream, TaskCompletionSource, Atomic,
            // AtomicInteger, etc.).  These have no user TypeDecl
            // (declaration == nullptr in their TypeRef) and no vtable, so
            // isOwnedReferenceField returns false and the field would otherwise be
            // silently skipped.  Classify by name via builtinTypeName +
            // classifyVtablelessBuiltinRelease.
            // fix #5274 (§28.3 / §28.8, v0.313): the classification (resolve
            // the declared type through the active substitution, require the
            // `owned` modifier per #5270/#5249, take the builtin's name) lives
            // in ONE helper, `ownedVtablelessBuiltinFieldName`, shared with the
            // owned-field STORE gate (`isOwnedFieldStoreTarget`) so the store's
            // consume of an `owned` parameter and this disposal cannot drift.
            if (std::string_view n = ownedVtablelessBuiltinFieldName(fs);
                !n.empty()) {
                OwnedField of{fs, nullptr};
                of.vtablelessBuiltin = std::string(n);
                owned.push_back(of);
            }
            // §28.13 / stdlib §2.16 (#620): an `owned` field of Shared<T> or
            // Weak<T> type.  These are compiler-known ref-counted heap objects
            // with no user TypeDecl / no vtable; release via the appropriate
            // runtime function instead of the vtable-Dispose path.
            bool isWeak = false;
            if (isOwnedSharedWeakField(fs, &isWeak)) {
                OwnedField of{fs, nullptr};
                if (isWeak) of.weakField = true;
                else        of.sharedField = true;
                owned.push_back(of);
            }
            // fix #5440 / §35.4 (v0.315): an `owned` CHOICE field (Owning
            // classification per classifyChoiceType). All the branches above
            // (isOwnedReferenceField, taskFamilyTypeFromField, etc.) explicitly
            // reject Choice declarations, so this arm is needed to actually
            // dispose owning choice fields at scope end. The disposal is via
            // emitOwningChoiceLocationDisposal (the per-tag switch), NOT the
            // class-vtable path.
            if (const sema::TypeRef* choiceRef = owningChoiceFieldType(fs)) {
                OwnedField of{fs, nullptr};
                of.owningChoiceField = true;
                of.owningChoiceRef = choiceRef;
                owned.push_back(of);
            }
        }
        }
    }
    for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
        llvm::StructType* st = classStruct(ts);
        unsigned idx = fieldIndex_[it->fs];
        llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx);
        llvm::Value* val = builder_.CreateLoad(ptrTy(), addr);
        // #362 / stdlib §4.5: an owned collection field frees its backing and
        // cascades disposal into its class elements (for an owning collection).
        // bz_array_free / bz_llist_free are null-safe, so no field null-check needed.
        if (it->collTy) {
            // #602 / §28.11 v0.135: derive element disposer directly from
            // elementPolicyFor (covering class, interface, Shared/Weak elements).
            // A borrowing Array/Stack/Queue<string> co-owns a +1 per element
            // (retainRelease=true); use the string-release thunk for that row.
            // The two paths are mutually exclusive (string is not a class).
            //
            // §28.5/§28.11, #893 #894: OwningDictionary<K,V> owns its V values
            // (genericArgs[1]), NOT its K keys (genericArgs[0]).  Free via
            // bz_dict_free(ptr, valueDisposer) so class V elements are disposed.
            // Array/Stack/Queue/LinkedList/Buffer own their single generic element
            // (genericArgs[0]) and use bz_array_free / bz_llist_free as before.
            bool isDict = isDictTypeName(it->collTy);
            const sema::TypeRef* collElem =
                isDict
                    ? ((it->collTy->genericArgs.size() > 1)
                           ? it->collTy->genericArgs[1]
                           : nullptr)
                    : ((!it->collTy->genericArgs.empty())
                           ? it->collTy->genericArgs[0]
                           : nullptr);
            // #816: in a per-instantiation (specialized) disposer the element may
            // be a generic type PARAMETER (`owned _arr: OwningArray<T>`); resolve it
            // to the concrete substituted type so elementPolicyFor classifies it
            // (and builds the element disposer thunk) — otherwise a bare `T`
            // element has a null declaration → Value policy → elements leak. A
            // no-op outside a substitution (the shared disposer keeps the bare T).
            collElem = resolveTypeParamRef(collElem);
            bool collIsOwning =
                it->collTy->astNode && it->collTy->astNode->name &&
                !it->collTy->astNode->name->parts.empty() &&
                (common::isOwningPrefixedName(it->collTy->headName()) ||
                 // #2606 / stdlib §4.5: IOwningList<T> is the OWNING list
                 // interface — its elements are owned by the holder — but
                 // isOwningPrefixedName("IOwningList") is false (the head starts
                 // with "I", not "Owning"), so consult the interface recognizer
                 // to mark the family owning (matches owned OwningArray<T>).
                 isOwningListInterfaceName(it->collTy));
            ElementPolicy fieldCollPol = elementPolicyFor(collElem, collIsOwning);
            llvm::Function* elemDisp = fieldCollPol.disposerThunk;
            if (!elemDisp && !isDict &&
                    fieldCollPol.cat == ElementPolicy::Category::RuntimeManaged &&
                    collElem && collElem->kind == sema::TypeRef::Kind::Primitive &&
                    collElem->primitive == sema::PrimitiveType::String &&
                    !collElem->nullable &&
                    it->collTy->astNode && it->collTy->astNode->name &&
                    !it->collTy->astNode->name->parts.empty() &&
                    [](std::string_view n){
                        // #2603 / issue #401: IList<T>/IReadOnlyList<T> are
                        // array-backed + borrowing (same bz_array_t* + per-
                        // element string +1 as Array<T>), so a string-element
                        // field of those interface types releases its elements
                        // through the SAME stringElementReleaseThunk. IOwningList
                        // is owning and never reaches this string gate (owning-
                        // family disposer runs via elementPolicyFor instead).
                        //
                        // fix #2692: LinkedList joins the list. It is llist-
                        // backed rather than array-backed (the `isLL` switch
                        // below picks bz_llist_free), but the ELEMENT obligation
                        // is identical — a borrowing `LinkedList<string>` co-owns
                        // a +1 per element (§28.11's element-slot table), and
                        // since #2686 all five insert paths take it. It was
                        // excluded only because none of them did, which made a
                        // release here unpaired; that is no longer true, and the
                        // exclusion was leaking one reference per element of an
                        // `owned` LinkedList<string> field. OwningLinkedList
                        // cannot reach this gate: elementPolicyFor puts an
                        // owning-family string in the Value row, so the
                        // RuntimeManaged test above already fails (and sema
                        // rejects `OwningLinkedList<string>` outright — stdlib
                        // §4.2 / §14.4, #1327).
                        return n == "Array" || n == "Stack" || n == "Queue" ||
                               n == "IList" || n == "IReadOnlyList" ||
                               n == "LinkedList";
                    }(it->collTy->headName()))
                elemDisp = stringElementReleaseThunk();
            llvm::Value* disp = elemDisp
                ? llvm::cast<llvm::Value>(elemDisp)
                : llvm::ConstantPointerNull::get(ptrTy());
            // per §4.6/§4.7: LinkedList fields free via bz_llist_free.
            // §28.5/§28.11, #893: OwningDictionary fields free via bz_dict_free.
            bool isLL = isLinkedListTypeName(it->collTy);
            // #2608 / stdlib §4.3: an `owned` field typed by a BORROWING list
            // interface (IReadOnlyList<T>/IList<T>) is array-backed, but those
            // interfaces are the read-only view shared by BOTH hierarchies, so
            // the dynamic backing may be a borrowing `Array<T>` (elements must
            // NOT be disposed) or an owning `OwningArray<T>` (elements MUST be).
            // The static type cannot tell them apart, and guessing "owning" here
            // would double-free the borrowing case. When no static element
            // disposer was derived, defer to the disposer the OWNING collection
            // stamped into its header at construction: bz_array_free_dynamic.
            // An untagged (borrowing) backing yields NULL there, i.e. exactly
            // the `bz_array_free(val, null)` this replaces — never a UAF.
            if (!isLL && !isDict && !elemDisp &&
                isBorrowingListInterfaceName(it->collTy)) {
                auto* dynFn = declareC("bz_array_free_dynamic",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(dynFn, {val});
                continue;
            }
            // fix #6270 (§28.5 / §28.11, v0.338): a borrowing `Set<T>` whose
            // element is generic-dict-eligible is bz_dict_t*-backed at
            // construction (genericBackingType: records, non-nullable
            // primitives/enums, strings, plain-value inline arrays), not
            // bz_array_t*-backed.
            // bz_array_free on that table freed the header but leaked the
            // insertion-order array and every key copy (68 B per instance at
            // T=int). Free it exactly as a `using var` Set local does: release
            // a string element's +1 (held as a KEY), then bz_dict_free with no
            // value disposer (a Set's values are sentinels). Narrow: the
            // borrowing `Set` head only; an array-backed Set (class, interface
            // or nullable element) and the owning family keep the free below.
            if (!isLL && !isDict && builtinTypeName(it->collTy) == "Set" &&
                genericBackingType(it->collTy, 0, /*isSet=*/true, nullptr)) {
                if (collElem && !collElem->nullable &&
                    collElem->kind == sema::TypeRef::Kind::Primitive &&
                    collElem->primitive == sema::PrimitiveType::String) {
                    auto* relFn = declareC("bz_set_string_keys_release",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy()});
                    builder_.CreateCall(relFn, {val});
                }
                auto* dfn = declareC("bz_dict_free", llvm::Type::getVoidTy(llvm_),
                                     {ptrTy(), ptrTy()});
                builder_.CreateCall(
                    dfn, {val, llvm::ConstantPointerNull::get(ptrTy())});
                continue;
            }
            const char* freeName = isLL   ? "bz_llist_free"
                                  : isDict ? "bz_dict_free"
                                  :          "bz_array_free";
            auto* fn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {val, disp});
            continue;
        }
        // §28.5/§24 (#879): owned Subscription (or Subscription?) field —
        // unsubscribe via bz_subscription_dispose (null-safe: first thing it
        // does is `if (!subp) return;`, so Subscription? left null is safe).
        // Null the field after so a re-entrant dispose is a no-op.
        if (it->subscriptionField) {
            auto* dispFn = declareC("bz_subscription_dispose",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(dispFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            continue;
        }
        // §6.5 / §24 (#1646): owned EventSource<T>/AsyncEventSource<T> field —
        // free the subscriber list + struct via bz_event_source_free (null-safe:
        // `if (!s) return;`, so a ctor that threw before the field was assigned is
        // safe). This is the same runtime free the keyword-form EventSymbol
        // backing-field loop later in this function uses (#480). The freed source
        // does NOT cascade into the borrowed handler closures (those are owned by
        // their Subscriptions). Null the field after so a re-entrant dispose is a
        // no-op.
        if (it->eventSourceField) {
            auto* freeFn = declareC("bz_event_source_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            continue;
        }
        // fix #1832 / stdlib §6.5 (v0.187): owned SharedEventSource<T> field —
        // free via bz_shared_event_source_free (null-safe, reclaims the
        // subscriber list + struct). The shared path is structurally a sibling
        // of the instance pair above, with the synchronized quiescence /
        // lock discipline on the runtime side; the codegen contract is
        // identical (null-safe free, null-after to make re-entrant passes
        // no-ops).
        if (it->sharedEventSourceField) {
            auto* freeFn = declareC("bz_shared_event_source_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            continue;
        }
        // §28.13 / stdlib §2.16 (#620): owned Shared<T>/Weak<T> field —
        // release via bz_shared_release / bz_weak_release (null-safe, no vtable).
        // Null the field after so a re-entrant dispose is a no-op.
        if (it->sharedField || it->weakField) {
            const char* relName = it->weakField ? "bz_weak_release"
                                                : "bz_shared_release";
            auto* relFn = declareC(relName, llvm::Type::getVoidTy(llvm_),
                                   {ptrTy()});
            builder_.CreateCall(relFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            continue;
        }
        // #1643 / §29.7: an owned IEnumerable<T>/IOwningEnumerable<T> field
        // holds an iterator-vtable object. Dispatch Dispose via vtable slot 1
        // (the iterator format [0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current),
        // which runs the state machine's pending finally blocks and FREES THE
        // FRAME ITSELF (§29.7) — so NO separate free/reclaim here (unlike the
        // class-vtable slot-2 path below, where Dispose only runs the destructor
        // and emitDisposeAndReclaim must also free). Calling slot 2 here would
        // invoke MoveNext, not Dispose, and leak the frame. Null-guard (a never-
        // assigned or moved-from slot holds null) + null-after (the #396 slot-
        // null precedent) so a re-entrant / second pass is a no-op. Mirrors the
        // scope-end iterEnumDispose path (emitDisposals) and the iterator
        // Dispose-body EnumFrame cleanup (emitIteratorMethod).
        if (it->iterField) {
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()));
            auto* iterDispBB = llvm::BasicBlock::Create(
                llvm_, "owned.iter.dispose", currentFn_);
            auto* iterContBB = llvm::BasicBlock::Create(
                llvm_, "owned.iter.cont", currentFn_);
            builder_.CreateCondBr(isNull, iterContBB, iterDispBB);
            builder_.SetInsertPoint(iterDispBB);
            // Dispatch vtable slot 1 (Dispose) — frees the frame.
            llvm::Value* vt = builder_.CreateLoad(ptrTy(), val);
            llvm::Value* dispSlot =
                builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(1)});
            llvm::Value* dispFnPtr = builder_.CreateLoad(ptrTy(), dispSlot);
            auto* dispTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
            builder_.CreateCall(dispTy, dispFnPtr, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(iterContBB);
            builder_.SetInsertPoint(iterContBB);
            continue;
        }
        // fix #2830: owned FFI-buffer field (CString / WString / RawBytes — stdlib
        // §10.1–§10.3). The runtime value is a headerless malloc'd byte buffer
        // (or, for RawBytes, a { ptr, len } struct whose `ptr` owns a malloc'd
        // byte buffer, §10.3) — no object header, no vtable. Released via a
        // null-guarded free() (CString/WString) or bz_rawbytes_free (RawBytes,
        // stdlib §10.3 / fix #985 — frees the byte buffer THEN the struct).
        // The shape mirrors emitFfiBufferTempFree's null-guarded free and the
        // iterField / taskField branches above (null-guard + null-after-store
        // so a re-entrant / second pass is a no-op, per the #396 slot-null
        // precedent). A `using var`-bound FFI buffer uses the SAME call
        // (Disposal.cpp's ffiBuffer arm / #2733), so a `using var c := …` and
        // a `_c := …; … _c.Dispose();` field dispose identically.
        if (it->ffiBufferField) {
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()));
            auto* ffbDispBB = llvm::BasicBlock::Create(
                llvm_, "owned.ffibuf.free", currentFn_);
            auto* ffbContBB = llvm::BasicBlock::Create(
                llvm_, "owned.ffibuf.cont", currentFn_);
            builder_.CreateCondBr(isNull, ffbContBB, ffbDispBB);
            builder_.SetInsertPoint(ffbDispBB);
            if (it->rawBytes) {
                // stdlib §10.3 / fix #985: a RawBytes is a { ptr, len } struct
                // whose buffer is a separate allocation — both are reclaimed
                // by the runtime helper.
                auto* fn = declareC("bz_rawbytes_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {val});
            } else {
                builder_.CreateCall(freeFn(), {val});
            }
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(ffbContBB);
            builder_.SetInsertPoint(ffbContBB);
            continue;
        }
        // §28.6 / §30.5 (#1889): an owned Task / Task<T> / OwningTask<T>
        // FIELD. The full using-var task protocol runs (error_obj prelude,
        // per-T result_disposer attach, bz_task_fire_and_forget[_checked])
        // via emitOwnedTaskDisposal — the field analog of the using-var task
        // drop. Mirrors the iterField branch above (null-guard + null-after
        // so a re-entrant / second pass is a no-op, per the #396 slot-null
        // precedent). The field-store path at #2173 shares the same helper
        // (lowerOwnedTaskFieldStore calls emitOwnedTaskDisposal too).
        if (it->taskField) {
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()));
            auto* taskDispBB = llvm::BasicBlock::Create(
                llvm_, "owned.task.dispose", currentFn_);
            auto* taskContBB = llvm::BasicBlock::Create(
                llvm_, "owned.task.cont", currentFn_);
            builder_.CreateCondBr(isNull, taskContBB, taskDispBB);
            builder_.SetInsertPoint(taskDispBB);
            // taskField is set only on the FieldSymbol arm below; the
            // MemberSymbol autoprop-storage case (fix #2778) is classified
            // up-front with iterField=true and never reaches this branch.
            emitOwnedTaskDisposal(
                static_cast<const sema::FieldSymbol*>(it->fs), val);
            // Null the slot AFTER the disposal so a re-entrant / second
            // pass is a no-op (the #396 slot-null precedent; mirrors
            // iterField / eventSourceField / sharedField / weakField
            // / subscriptionField below).
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(taskContBB);
            builder_.SetInsertPoint(taskContBB);
            continue;
        }
        // fix #4290: an `owned` field of a vtableless builtin type (Mutex, Lock,
        // Semaphore, SemaphoreToken, CancellationTokenSource, Channel, ChannelSender,
        // Stopwatch, FileStream, TaskCompletionSource, Atomic, AtomicInteger, etc.).
        // These have no vtable; dispose via the appropriate runtime free function
        // (bz_mutex_dispose, bz_lock_release, bz_semaphore_release, etc.) using
        // the same emitVtablelessBuiltinRelease helper the using-var path uses.
        // Null the slot after so a re-entrant / second pass is a no-op.
        if (!it->vtablelessBuiltin.empty()) {
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()));
            auto* vbDispBB = llvm::BasicBlock::Create(
                llvm_, "owned.vb.dispose", currentFn_);
            auto* vbContBB = llvm::BasicBlock::Create(
                llvm_, "owned.vb.cont", currentFn_);
            builder_.CreateCondBr(isNull, vbContBB, vbDispBB);
            builder_.SetInsertPoint(vbDispBB);
            emitVtablelessBuiltinRelease(it->vtablelessBuiltin, val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(vbContBB);
            builder_.SetInsertPoint(vbContBB);
            continue;
        }
        // fix #5440 / §35.4 (v0.315): an `owned` choice field (Owning
        // classification). All existing branches above handle class/interface,
        // collection, task, iterator, event, FFI buffer, shared, and vtableless
        // builtin fields; none of them handle choice types (the class/interface
        // branch explicitly returns false for non-Class/Interface declarations).
        // The disposal is via emitOwningChoiceLocationDisposal — the per-tag
        // switch that reads the runtime tag and disposes only the active case's
        // owned payloads — NOT the class-vtable path below. No null-guard or
        // null-after is needed for a bare (non-nullable) choice: the switch arm
        // for a payload-free case is a no-op, and a moved-from choice holds
        // tag 0 which is also a no-op arm.
        if (it->owningChoiceField) {
            emitOwningChoiceLocationDisposal(addr, it->owningChoiceRef);
            continue;
        }
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "owned.dispose",
                                                currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "owned.cont",
                                                currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // §28.4 step 2: dispatch the field object's idempotent Dispose and
        // reclaim its block once, then null the field. §28.5 (line 5066): a
        // destructor body may explicitly `field.Dispose()` BEFORE this auto-
        // dispose — that explicit call only dispatched the field's idempotent
        // Dispose (it did not free), so reclaiming here frees the field exactly
        // once (no double-free). Nulling the field after makes any further
        // disposal a guarded no-op (the #396 slot-null precedent), so a second
        // §28.4-step-2 pass (or a re-entrant dispose) finds null and skips.
        emitDisposeAndReclaim(val);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }

    // §15.1.1 line 1957 / §15.3 (#747): a move-only OWNING inline-array FIELD
    // ([N owned]T? / [N owned]T) — "for class fields, the enclosing class's
    // destructor invokes the disposal." Each non-null slot's object is disposed
    // via its own vtable, then nulled (the #396 slot-null precedent guards a
    // re-entrant pass). A slot left moved-from via `move this.field[i]` was
    // nulled at the move (lowerMoveFromCollectionSlot), so the null-guard skips
    // it — no double-free. Unlike the owned-reference fields above there is no
    // single object to dispatch on; the field is a [N x ptr] sub-aggregate.
    //
    // fix #4552 (§15.1.1 / §28.4 v0.307, ruling #4388): "the array's auto-
    // dispose pass disposes each non-null slot, in reverse slot order — the
    // same order ... as the whole-value-reassignment pass of §15.3 ... one
    // pass definition governs all four of this paragraph's endings [scope
    // end, instance-field Dispose(), whole-value reassignment, static
    // program exit]." This loop iterated 0..n-1 (establishment order); the
    // reassignment pass (lowerOwningInlineArrayReassign,
    // LlvmBackend_Stmts.cpp) already iterates N..1 for exactly this reason.
    // Iterate in reverse to match.
    {
        llvm::StructType* st = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs) continue;
            if (fieldIndex_.find(fs) == fieldIndex_.end()) continue;
            const sema::TypeRef* ft = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                if (fd->type) ft = ctx_.resolvedType(*fd->type);
            }
            if (!ft || ft->kind != sema::TypeRef::Kind::InlineArray ||
                !ft->arrayOwned)
                continue;
            unsigned n = ft->arraySizeValue >= 0
                ? (unsigned)ft->arraySizeValue : 0;
            if (n == 0) continue;
            unsigned idx = fieldIndex_[fs];
            llvm::Value* fieldAddr =
                builder_.CreateStructGEP(st, currentThis_, idx, "ioaf.addr");
            auto* arrTy = llvm::ArrayType::get(ptrTy(), n);
            for (unsigned fwd = 0; fwd < n; ++fwd) {
                // fix #4552 (§15.1.1 v0.307): reverse slot order.
                unsigned i = n - 1 - fwd;
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, fieldAddr,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioaf.slot");
                llvm::Value* obj =
                    builder_.CreateLoad(ptrTy(), slotAddr, "ioaf.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioaf.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioaf.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
        }
    }

    // #429 / §4.6 / §28.9: release every STRING field / auto-property of THIS
    // class (own members only; an inherited slot is released by the base CORE
    // disposer via emitBaseDisposeChain — so visiting only ts->members avoids a
    // double-release). A class string field holds an INDEPENDENT +1 (retained at
    // store via lowerStringFieldStore / armed in emitInstanceInit's initializer
    // path); on disposal that +1 is released exactly once, freeing the
    // data+header at rc 0. bz_string_release is null-safe (a never-assigned
    // string? slot is null in the calloc'd object) and a no-op on a sentinel
    // (a field holding a literal is never freed). After releasing, NULL the slot
    // so a re-entrant / double dispose finds null and skips (the #396 slot-null
    // precedent) — the §28.6 run-once flag already guards the public Dispose, and
    // this nulling guards any base-core re-entry. A local that aliases this field
    // value holds its OWN +1 (#429 borrow-retain), so this release never dangles
    // it (no UAF). A RECORD has no disposer (§9.7) and its string fields are
    // borrows (§9) — never reached here.
    //
    // fix #3152 (§9.2/§2.8.2 clause 1): this walk and the two below classify
    // through `sema::isAutoPropertyStorageSymbol`, not the AST-only
    // `hasAstAutoPropertyDecl` they used to. `ts` IS an imported TypeSymbol on
    // the `emitSpecializedDisposer` / `emitSpecializedBaseDisposerCore` paths
    // (an imported GENERIC class monomorphized here — measured, `ts->decl` is
    // null there), so the AST-only predicate answered `false` for every
    // imported auto-property while `collectInstanceStorage` — already on the
    // both-sources predicate since #2478 — had given that same member a
    // `fieldIndex_` slot. One member list, two answers.
    //
    // MEASURED, and worth recording because it decides the DIRECTION of this
    // change: converting these three is a no-op today. `isStringStorageMember`
    // / `isClosureStorageMember` / `boxStorageFields_` are a SECOND, independent
    // gate that also excludes an imported property (`recordMemberType`'s
    // property arm has no decl-null fallback), so the release still does not
    // fire — instrumented on a two-module build of an imported generic with a
    // `string` auto-property: `isAutoPropertyStorageSymbol=1, isStringStorage=0`.
    //
    // ⚠ The accompanying "200 iterations under detect_leaks=1 reported nothing"
    // is WEAKER EVIDENCE THAN IT LOOKS, and #3152's review said so: a clean LSan
    // run is equally consistent with a member that never receives a `+1` at all.
    // The review could not reproduce the leak on the named shape — with the
    // property ASSIGNED IN THE CONSTRUCTOR it SEGVs both before and after (a
    // separate, pre-existing defect). So read this as "no leak was observed",
    // not "the path was proven inert".
    // The reason to convert anyway is that the imbalance runs the other way:
    // the RETAIN half (`isStringFieldLvalue`, #2504) is already both-sources and
    // does NOT depend on `recordMemberType`. So the day that second gate gains
    // its fallback, an unconverted walk retains without releasing — a leak —
    // whereas a converted one is balanced. This is the ordering #2874's own
    // comment warns about, taken in the safe direction.
    {
        llvm::StructType* sst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !sema::isAutoPropertyStorageSymbol(m, ctx_))
                continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            // fix #4965 (§4.6 / §28.9 / §14.1, v0.307): also a bare-`T`
            // member at `T = string` — this walk runs inside the
            // per-instantiation disposer (needsSpecDispose's layoutDiffers
            // clause forces one at every pointer-sized instantiation) with the
            // substitution active, and every store into such a member now
            // retains (see isTypeParamBoundToString). The slot-type check keeps
            // the release off anything that is not a pointer slot in the
            // struct actually being walked.
            if (!isStringStorageMember(m) &&
                !isTypeParamStringStorageMember(m))
                continue;
            unsigned idx = fieldIndex_[m];
            if (!isStringStorageMember(m) &&
                !sst->getElementType(idx)->isPointerTy())
                continue;
            llvm::Value* addr = builder_.CreateStructGEP(sst, currentThis_, idx,
                                                         "strf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "strf.val");
            emitStringRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #429 (closure FIELD-STORE category) / §27.5 / §28.9: release every CLOSURE
    // field / auto-property of THIS class (own members only; an inherited slot
    // is released by the base CORE disposer via emitBaseDisposeChain — so
    // visiting only ts->members avoids a double-release). A class closure field
    // holds an INDEPENDENT +1 (retained at store via lowerClosureFieldStore /
    // emitInstanceInit's initializer path); on disposal that +1 is released
    // exactly once via bz_closure_release, freeing the cell + env block at rc 0.
    // bz_closure_release is null-safe (a never-assigned closure field slot is
    // null in the calloc'd object) and does NOT touch captured values (they are
    // borrows, §28.9 — no double-dispose of a capture). After releasing, NULL
    // the slot so a re-entrant / double dispose finds null and is a no-op (the
    // #396 slot-null precedent, parallel to the string-field nulling above).
    {
        llvm::StructType* cst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !sema::isAutoPropertyStorageSymbol(m, ctx_))
                continue;  // fix #3152 — see the string walk above
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            // fix #5389 (§23.1 / §28.9, v0.315): also a bare-`T` member at
            // `T = function type` — this walk runs inside the per-instantiation
            // disposer (needsSpecDispose's layoutDiffers clause forces one at every
            // pointer-sized instantiation) with the substitution active, and every
            // store into such a member now retains (see isTypeParamBoundToFunction).
            // The slot-type check is unnecessary here (closure fields are always ptr).
            if (!isClosureStorageMember(m) &&
                !isTypeParamFunctionStorageMember(m))
                continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(cst, currentThis_, idx,
                                                         "clf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "clf.val");
            emitClosureRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #429 (box FIELD-STORE category) / §8.6 / §28.9: release every Object/
    // interface field / auto-property of THIS class (own members only; an
    // inherited slot is released by the base CORE disposer via
    // emitBaseDisposeChain — visiting only ts->members avoids double-release).
    // A box storage field holds an INDEPENDENT +1 (taken at store via
    // lowerBoxFieldStore / emitInstanceInit); on disposal that +1 is released
    // EXACTLY ONCE via bz_box_release, freeing the box wrapper at rc 0. Safety:
    //   - isBoxStorageMember gates on boxStorageFields_, which is populated ONLY
    //     for fields where collectBoxStorageFields + lowerBoxFieldStore confirmed
    //     EVERY store uses a boxable value source. A field that ever received a
    //     class-reference store is NOT in boxStorageFields_ → NOT released here.
    //     This is the cardinal box-vs-class discriminator: bz_box_release is
    //     NEVER called on a class instance. (Corrupting offset 8 of a class
    //     instance — the disposed-flag word — would cause double-dispose / UAF.)
    //   - bz_box_release is null-safe (a calloc-zeroed null field is a no-op).
    //   - After releasing, NULL the slot so a re-entrant / double dispose finds
    //     null and is a no-op (the #396 slot-null precedent, parallel to the
    //     string-field and closure-field nulling above).
    //   - The boxed value is a value-type leaf (no owned sub-objects): the
    //     release frees ONLY the wrapper — no cascade.
    {
        llvm::StructType* bst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !sema::isAutoPropertyStorageSymbol(m, ctx_))
                continue;  // fix #3152 — see the string walk above
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            if (!isBoxStorageMember(m)) continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(bst, currentThis_, idx,
                                                         "boxf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "boxf.val");
            emitBoxRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // fix #4253 (§9.7 / §28.9, v0.298): release every RECORD-typed field /
    // auto-property of THIS class whose declared type owns at least one
    // `string` field (isRecordWithStringFieldsTypeRef — recurses through
    // nested records/choices, same authority the LOCAL scope-end release
    // uses) and whose OWN declaration initializer is provably a fresh,
    // alias-free construction of exactly that record type
    // (isFreshRecordConstructionOfExactType — the SAME eligibility proof
    // #2448/#4237 established for a plain `var` record local; reused
    // verbatim, not re-derived, per LANE_RULES §6/§25).
    //
    // BEFORE THIS FIX there was NO release path for this shape AT ALL:
    // emitOwnedFieldDisposals (this function) only ever walks `owned`-
    // annotated fields (reference types) — a record can never carry
    // `owned` (§9.7 forbids it; records are value types embedded inline)
    // — so a class field like `public readonly property P: Box := new
    // Box("s");` (`record Box(Value: string);`) leaked its string
    // unconditionally, at ONE level of nesting, no generics, no cycle
    // (issue #4253 — measured 11290 B / 600 allocations at 300 loop
    // iterations, identical in magnitude to #4237's own local-variable
    // figures for the analogous shape). Own members only (mirrors the
    // string/closure/box walks above): an inherited slot is released by
    // the base CORE disposer via emitBaseDisposeChain.
    //
    // DIRECTION / SAFETY: this walk only ever ADDS a release where none
    // existed (a safe-direction leak becomes balanced). It can turn that
    // leak into a double-free / UAF only if the field's value at Dispose
    // time could differ from what the admitted initializer constructed,
    // or could alias another live binding — i.e. only if some OTHER store
    // to the slot can run.
    //
    // fix #4277 (§9.7 / §28.9 / §13.2 / §23.1, v0.306): that is now asked
    // DIRECTLY, by `declarationInitializerIsSoleStore` (LlvmBackend_Setup.cpp).
    // #4253 asked it through three structural PROXIES instead — `readonly`
    // on the member, `sealed` on `ts`, and `!hasAnyConstructor(ts)` — and
    // each over-rejected on its own axis, so three whole shapes with exactly
    // ONE store went on leaking: a non-sealed class with no subclass, a class
    // whose only constructor never mentions the field, and a non-readonly
    // property nothing ever assigns (7490 B / 400 allocations, 8600 B / 400,
    // and 7490 B / 400 respectively at 200 loop iterations, measured on
    // origin/main cafdfc52). The predicate proves the same safety property
    // the three proxies were reaching for — the declaration initializer is
    // the SOLE store the slot ever receives — out of a module-wide write-site
    // scan plus §13.2/§23.1 cross-module reachability, and is fail-closed on
    // any AST node kind it does not recognize.
    //
    // The two sema facts the proxies were standing in for are unchanged and
    // still load-bearing, just consulted at their source rather than
    // approximated: §23.1 (+ fix #4276's declaring-type narrowing) is what
    // makes a `readonly` member's write sites all local, and §13.2's
    // default-`internal` top-level visibility is what makes a non-readonly
    // member of an undecorated type unreachable from an importer.
    //
    // A field whose initializer is not a provably-fresh NewExpr of the
    // declared type, or which any other store site can reach, is UNCHANGED —
    // still safe-leaks exactly as before. The residual #4277 records and this
    // fix does NOT close: a field assigned by a CONSTRUCTOR-BODY statement
    // (`this.P := new Box(...);`) rather than its own declaration
    // initializer. That shape needs the store site itself to transfer a +1
    // (drop-on-overwrite for record-typed fields, §28.7's reference-field
    // mechanism generalized), not merely a proof about how many stores there
    // are, and stays filed.
    {
        llvm::StructType* rst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !sema::isAutoPropertyStorageSymbol(m, ctx_))
                continue;  // fix #3152 — see the string walk above
            auto idxIt = fieldIndex_.find(m);
            if (idxIt == fieldIndex_.end()) continue;
            const sema::TypeRef* fieldTr = recordMemberType(m);
            if (!isRecordWithStringFieldsTypeRef(fieldTr)) continue;
            // fix #4277 (§9.7 / §28.9 / §13.2 / §23.1, v0.306): ONE predicate
            // in place of #4253's three structural proxies (`sealed`,
            // `!hasAnyConstructor`, `readonly`). See its definition in
            // LlvmBackend_Setup.cpp — it asks the question those three were
            // each a coarse stand-in for, and so admits the three shapes they
            // each over-rejected without widening what is being claimed.
            if (!declarationInitializerIsSoleStore(ts, m)) continue;
            const ast::Expr* init = nullptr;
            // SEAM-ACCESSOR: `m->decl` is nullptr for an IMPORTED field (no
            // ast::FieldDecl crosses the module boundary, the #3671 seam) --
            // the fallback immediately below (ctx_.importedInstanceFieldInitExpr)
            // is the sanctioned both-sources accessor for exactly this arm,
            // the same one emitOwnFieldInit (LlvmBackend_Setup.cpp) and the
            // record-preamble init walk (LlvmBackend_Exprs2.cpp) already use.
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
                init = fd->initializer.get();
            // SEAM-ACCESSOR: ditto -- see the FieldDecl arm immediately above.
            else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
                init = pd->autoInitializer.get();
            // fix #4279 (§9.7/§28.9, v0.298): an imported field/property has
            // no ast::FieldDecl/PropertyDecl at all (m->decl == nullptr), so
            // the two arms above always miss for it and this walk's release
            // silently never fired for an imported record-typed field --
            // narrow relative to the local case, but NOT a wrong answer: it
            // just never released. `importedInstanceFieldInitExpr` is the
            // #2874 load-time-reconstructed ast::Expr* for exactly this case
            // (ModuleLoader re-parses and sema-types the exporting module's
            // initializer text, then publishes the resolved Expr* here) --
            // the SAME accessor emitOwnFieldInit and the record-preamble
            // init walk already consult for the identical m->decl==nullptr
            // gap, reused verbatim rather than re-derived (LANE_RULES §6/§25).
            if (!init && m->decl == nullptr) {
                if (const ast::Expr* parsedInit =
                        ctx_.importedInstanceFieldInitExpr(m))
                    init = parsedInit;
            }
            if (!init) continue;
            if (!isFreshRecordConstructionOfExactType(init,
                                                       fieldTr->declaration))
                continue;
            llvm::Value* addr = builder_.CreateStructGEP(
                rst, currentThis_, idxIt->second, "recf.addr");
            emitRecordStringFieldReleases(addr, fieldTr);
        }
    }

    // fix #6170 step 3 (§28.9 / §35.4, v0.338): release the strings a
    // value-like choice FIELD owns (the module scan admitted it, so every
    // store retained — see collectValueLikeChoiceFieldOwners), then zero the
    // slot so a re-entrant base-core dispose finds tag 0 with null strings and
    // releases nothing. Own members only, like the walks above: an inherited
    // slot is released by the base CORE disposer.
    {
        llvm::StructType* vst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            auto idxIt = fieldIndex_.find(m);
            if (idxIt == fieldIndex_.end()) continue;
            const sema::TypeRef* vrt = valueLikeChoiceFieldOwnerType(m);
            if (!vrt) continue;
            llvm::Value* addr = builder_.CreateStructGEP(
                vst, currentThis_, idxIt->second, "vlcf.addr");
            emitValueLikeChoiceActiveCaseStrRelease(addr, vrt->declaration, vrt,
                                                    /*retain=*/false);
            builder_.CreateStore(
                llvm::Constant::getNullValue(vst->getElementType(idxIt->second)),
                addr);
        }
    }

    // #480 / §24 / §28 (#1176): free each instance event's backing
    // EventSource. emitInstanceInit allocated one bz_event_source_new per
    // instance EventSymbol (sync and async); those allocations are NOT visited
    // by the FieldSymbol loop above because EventSymbol does not inherit from
    // FieldSymbol and the dynamic_cast returns null.  Collect all instance-event
    // symbols that have a fieldIndex_ entry (non-static events, the same
    // predicate as collectInstanceStorage) and emit a null-guarded
    // bz_event_source_free call for each, then store null back so a re-entrant
    // dispose is a no-op.  Ordering relative to FieldSymbol disposals is not
    // safety-critical: EventSource.free (handler array + struct) does not
    // cascade into any owned sub-objects tracked by this class.
    llvm::StructType* st = classStruct(ts);
    auto* freeFn = declareC("bz_event_source_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    for (const sema::Symbol* m : ts->members) {
        auto* es = dynamic_cast<const sema::EventSymbol*>(m);
        if (!es) continue;
        auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
        if (!ed || ed->isStatic) continue;  // instance events (sync + async)
        auto idxIt = fieldIndex_.find(es);
        if (idxIt == fieldIndex_.end()) continue;
        unsigned idx = idxIt->second;
        llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx,
                                                     "evt.addr");
        llvm::Value* val  = builder_.CreateLoad(ptrTy(), addr, "evt.src");
        // Null-guard: calloc zeroes the slot; if the ctor threw before
        // emitInstanceInit allocated this source the slot is still null.
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()));
        auto* freeBB = llvm::BasicBlock::Create(llvm_, "evt.free",  currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "evt.cont",  currentFn_);
        builder_.CreateCondBr(isNull, contBB, freeBB);
        builder_.SetInsertPoint(freeBB);
        builder_.CreateCall(freeFn, {val});
        // Null the slot so a second disposal pass (re-entrant or double-dispose)
        // finds null and skips, mirroring the FieldSymbol disposal pattern above.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
}


// §28.5: does `ts` declare at least one `owned` field whose type is one of its
// own generic type parameters (`private owned _slot: T`)? Such a field is
// classified — and so auto-disposed — at INSTANTIATION (the substituted type),
// not in the shared type-erased disposer, so a class with one needs a per-
// instantiation specialized Dispose/vtable.
bool Emitter::classHasOwnedTypeParamField(const sema::TypeSymbol* ts) {
    if (!ts) return false;
    for (const sema::Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        if (!fs) continue;
        // (a) a direct owned type-parameter field (`owned _f: T`).
        if (isOwnedTypeParamFieldShape(fs)) return true;
        // #816 (b): a direct owned generic OWNING-collection field whose element
        // is a type parameter (`owned _arr: OwningArray<T>`) — its elements are
        // only disposed by a per-instantiation disposer.
        if (isOwnedTypeParamCollectionField(fs)) return true;
    }
    // #816 (c): a class whose (transitive) BASE has an owned type-parameter field
    // (`class Child<T> : Base<T>` where Base has `owned _f: T`) must ALSO get a
    // specialized disposer, so its emitBaseDisposeChain can invoke the base's
    // SUBSTITUTED disposer (the shared base disposer is type-erased and skips the
    // base's owned-T field — the leak in #816 repro (a)).
    for (const sema::TypeSymbol* base : ts->baseTypes)
        if (base && base->kind == ast::TypeDeclKind::Class &&
            classHasOwnedTypeParamField(base))
            return true;
    return false;
}


// §28.5: build (once per instantiation) the specialized core + public disposer
// for the active monomorphization of `ts`. Mirrors emitDisposerCore /
// emitDisposerBody but is emitted with the current genericSubst_ active, so an
// owned type-parameter field is laid out (the spec struct) and classified (the
// substituted concrete type) correctly. `suffix` disambiguates the function
// names across instantiations. Returns the public idempotent Dispose.
//
// State: the caller (lowerNew, mid-body) owns builder_/currentFn_/etc.; we save
// the full disposer-emit state and the builder insert point and restore them so
// the caller can continue emitting where it left off.
llvm::Function* Emitter::emitSpecializedDisposer(const sema::TypeSymbol* ts,
                                                 const std::string& suffix) {
    auto* fnType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
    auto* pubFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$Dispose" + suffix, module_.get());
    auto* coreFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$DisposeCore" + suffix, module_.get());

    // Save all state the disposer-emit clobbers, plus the builder insert point.
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParamSlots = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    auto savedFinallyStack = finallyStack_;
    llvm::BasicBlock* savedPropagate = propagateBB_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::Value* savedCaughtExc = currentCaughtExc_;
    std::string_view savedCaughtName = currentCaughtName_;
    std::size_t savedScopeDepth = unwindScopeDepth_;
    llvm::IRBuilder<>::InsertPoint savedIP = builder_.saveIP();

    // The core (destructor body + owned-field disposal + base chain) and the
    // public run-once wrapper — identical to emitDisposerCore / emitDisposerBody
    // but specialized: genericSubst_ stays active so classStruct(ts) resolves to
    // the spec struct and isOwnedReferenceField sees the concrete T.
    emitDisposerCore(ts, coreFn);

    currentFn_ = pubFn;
    currentThis_ = &*pubFn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", pubFn));
    llvm::StructType* st = classStruct(ts);
    unsigned fi = disposedFlagIndex(ts);
    llvm::Value* flagAddr =
        builder_.CreateStructGEP(st, currentThis_, fi, "disposed.flag");
    llvm::Value* already = builder_.CreateICmpNE(
        builder_.CreateLoad(disposedFlagTy(), flagAddr),
        llvm::ConstantInt::get(disposedFlagTy(), 0), "already.disposed");
    auto* doBB = llvm::BasicBlock::Create(llvm_, "dispose.do", pubFn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "dispose.ret", pubFn);
    builder_.CreateCondBr(already, retBB, doBB);
    builder_.SetInsertPoint(doBB);
    builder_.CreateStore(llvm::ConstantInt::get(disposedFlagTy(), 1), flagAddr);
    builder_.CreateCall(coreFn, {currentThis_});
    builder_.CreateBr(retBB);
    builder_.SetInsertPoint(retBB);
    builder_.CreateRetVoid();

    // Restore the caller's state and insert point.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParamSlots);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);
    propagateBB_ = savedPropagate;
    unwindTarget_ = savedUnwind;
    currentCaughtExc_ = savedCaughtExc;
    currentCaughtName_ = savedCaughtName;
    unwindScopeDepth_ = savedScopeDepth;
    builder_.restoreIP(savedIP);
    return pubFn;
}


llvm::GlobalVariable* Emitter::excGlobal() {
    if (!excGlobal_) {
        // fix #492 / §31.1 / §2.6: emit an ExternalLinkage DECLARATION (no
        // initializer) so all separately-compiled modules reference the SAME
        // "bz_exc" symbol defined once in bezel_runtime.cpp. Before this fix
        // each module defined its own InternalLinkage (BSS) copy under the name
        // "bz$exc"; a cross-module `throw` wrote the library's copy while the
        // importer's pending-check read a still-null copy — the exception
        // silently disappeared.
        //
        // An external declaration (nullptr initializer) in LLVM means "defined
        // elsewhere"; the linker resolves all references to the runtime's single
        // definition. The name "bz_exc" must match the name in bezel_runtime.cpp.
        // (Renamed from "bz$exc" — a `$` in an external symbol name is a
        // clang-only extension that triggers -Wdollar-in-identifier-extension in
        // code that declares it in a header. "bz_exc" is a valid C identifier.)
        //
        // fix #704: bz_exc is now thread_local in the runtime, but generated
        // code accesses it via emitExcSlot() → call bz_exc_addr() → load/store
        // through the returned pointer.  This GlobalVariable is kept only as an
        // anchor (it is no longer referenced by any load/store in generated IR);
        // emitExcSlot() is the sole code path that touches the slot at runtime.
        excGlobal_ = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            /*Initializer=*/nullptr,  // external declaration — defined in runtime
            "bz_exc");
    }
    return excGlobal_;
}


llvm::Value* Emitter::emitExcSlot() {
    // fix #704 / §30.14: return a ptr to the calling thread's bz_exc slot by
    // calling bz_exc_addr().  Using a plain extern "C" function (not a TLS
    // GlobalVariable) lets the LLVM ORC JIT resolve the symbol via
    // DynamicLibrarySearchGenerator — the host process exports "bz_exc_addr"
    // as a regular function, whereas a TLS GlobalVariable would require the
    // JIT to emit __emutls_v.bz_exc references which are NOT exported by the
    // host process (the host uses real, not emulated, TLS).
    // Per-call (not cached): each load/store gets a fresh call so the slot
    // address is always current (required when called from a thread-pool
    // worker that may switch stacks).
    if (!excAddrFn_) {
        // Declare: ptr bz_exc_addr() — returns &bz_exc for the current thread
        auto* fnTy = llvm::FunctionType::get(ptrTy(),
                                             llvm::ArrayRef<llvm::Type*>{},
                                             /*isVarArg=*/false);
        // fix #2148 follow-up: DEDUPE on an existing declaration. The
        // iterator/async owning-`TakeCurrent` error path independently declares
        // `bz_exc_addr` via module_->getOrInsertFunction (LlvmBackend_Iterators.cpp,
        // LlvmBackend_Async.cpp). A plain llvm::Function::Create does NOT check for
        // an existing global — so when emitExcSlot ran SECOND (that TakeCurrent
        // path emitted first), LLVM renamed this new function to "bz_exc_addr.1",
        // which emitExcSlot then cached and every emitExcSlot caller referenced —
        // an UNDEFINED symbol at link (the runtime exports only "bz_exc_addr"),
        // breaking any owning iterator whose emission order put TakeCurrent first.
        // The type is identical (ptr()) at all three sites, so getOrInsertFunction
        // returns the one canonical Function regardless of which site ran first.
        // per §30.14 / fix #704
        excAddrFn_ = llvm::cast<llvm::Function>(
            module_->getOrInsertFunction("bz_exc_addr", fnTy).getCallee());
    }
    return builder_.CreateCall(excAddrFn_, {}, "exc.slot");
}


llvm::BasicBlock* Emitter::propagateBB() {
    if (!propagateBB_) {
        auto* save = builder_.GetInsertBlock();
        propagateBB_ = llvm::BasicBlock::Create(llvm_, "propagate",
                                                currentFn_);
        builder_.SetInsertPoint(propagateBB_);
        // fix #3531 (§29.7 / §28.3 / §29.3, v0.283): a SYNC ITERATOR body that
        // leaves `MoveNext` BY AN EXCEPTION must mark the state machine done
        // (state = -1) before it returns, exactly as the `try`/`finally`
        // unwind edge already does.
        //
        // This is not a new rule and not a new predicate — it is #623's rule,
        // stated verbatim at its own site in LlvmBackend_Stmts.cpp
        // (`finUnwindBB`): "if we are inside an iterator state machine, mark
        // the iterator as done (state=-1) BEFORE running the finally body …
        // when the consumer's for-loop cleanup subsequently calls Dispose,
        // Dispose must see state=-1 and be a no-op — otherwise the finally
        // would run a second time." That site is reached ONLY when the body
        // has a `try`/`finally` around the throw; a body that throws with no
        // enclosing `try` never passed through it, so the state stayed parked
        // at the last yield and the consumer's `Dispose` replayed
        // `smIterCleanupAtState_` — the very disposals §28.3's EDGE PARITY
        // rule ("the exception edge performs the same implicit disposal as
        // the normal edge, on the single establishment-ordered cleanup stack;
        // the only difference is the lint") had just run on the way out.
        // Measured on b43196a: `using var r := new R(); yield return r.V;
        // if v > 2 then throw …; end if;` is a `heap-use-after-free … READ of
        // size 8 … in bz$iter$Gen$CloneDispose`, and the same shape over a
        // `string` local is a SEGV inside the allocator (a double free).
        //
        // THE THROW EDGE OWNS THEM, not `Dispose`. §28.3's edge parity makes
        // MoveNext's own unwind the discharger of the body's establishment-
        // ordered stack, and it has already run by the time control reaches
        // this block (`emitUnwindDisposals` at the throw site / the pending
        // check). §29.7 then says the abandonment replay is for a consumer
        // that "stops iterating early", and that the state machine
        // "Transitions to a completed state; subsequent `MoveNext()` returns
        // `false`; second `Dispose()` is a no-op" — a body that threw HAS
        // completed, so -1 is the state it owes, and the replay must not run.
        // Choosing this direction is also the leak-safe one: the throw edge
        // disposes, `Dispose` skips. The reverse (let `Dispose` own it) would
        // need the throw edge to skip, which is unreachable-in-general
        // because a manually-driven enumerator need never be disposed.
        //
        // WHY `propagateBB` AND NOT THE THROW SITES: `unwindBB()` is
        // `unwindTarget_ ? unwindTarget_ : propagateBB()`, and every
        // `unwindTarget_` chain inside MoveNext ends either at a `catch`
        // (handled — the iterator keeps going, and stamping there would be
        // wrong) or at a `finally` unwind (which stamps -1 itself and then
        // re-propagates outward). So this block is the SINGLE edge on which
        // an exception actually leaves the state machine, and it is created
        // lazily — an iterator body with NO UNWIND EDGE AT ALL emits
        // byte-identical IR (measured: a two-`yield` generator over `int`
        // diffs 0 lines against b43196a). Where an unwind edge does exist the
        // delta is exactly ONE instruction, this store, whether the edge comes
        // from a `throw`, a call's pending check, an overflow check, a `new`
        // that can fail, or the static-init guard — all of which end an
        // activation the same way, and on all of which -1 is the state §29.7
        // owes.
        //
        // `smStateGEPHere()` is the existing authority for "we are emitting
        // inside the iterator's own MoveNext right now" (#2891): it is null
        // in a nested lambda body, in the Dispose body that replays a
        // `finally`, and in any plain method compiled after this one, so no
        // cross-function store can be emitted. Scoped to `SuspendKind::Yield`
        // because the async twins were MEASURED not to have this defect — the
        // same program shape over `IAsyncEnumerable<int>` is clean on this tree
        // AND on b43196a, because the async paths park their state through
        // their own completion protocols rather than leaving it at the last
        // yield. That measurement is pinned as
        // `parity_async_iterator_throw_after_yield_unaffected_spec_29_7` in
        // tests/codegen/iterator_throw_after_yield_double_run_3531_test.cpp, so
        // widening this gate later is a decision with evidence attached rather
        // than a guess.
        // per §29.7, §28.3 rule 15, §29.3
        if (smKind_ == SuspendKind::Yield) {
            if (llvm::Value* smGEP = smStateGEPHere()) {
                // THE STATE FIELD DOES DOUBLE DUTY, and the two duties part
                // company on exactly this edge. It is (a) a DISCHARGE record —
                // what the `smIterCleanupAtState_` switch and the -1/-2
                // sentinels read — and (b) a POSITION, which §29.8's "the state
                // machine disposes any `owned` parameter that has not been
                // moved out of by the body" (extended to plain owned locals by
                // #2306 under §29.7) reads through `disposeOwnedParams` /
                // `disposeOwnedLocals` as `established <= state <
                // transferred`. On the throw edge (a) is true and (b) is NOT:
                // the body threw where it stood, and a local established before
                // that point and transferred after it is STILL OWED. Stamping
                // -1 alone answered (b) with "past everything" and skipped that
                // disposal — a measured 3200 B / 200 objects over 200
                // activations of
                //
                //     var a := new Box(1);
                //     yield return new Box(2);
                //     yield return C.Boom(v);   /* throws */
                //     yield return a;
                //
                // on an `IOwningEnumerable<Box>` head. (The linear pre-pass
                // `collectYieldStatesForOwnedLocals` aborts on statement FORMS
                // it does not model, but a `yield return <call>` is a YieldStmt
                // it tolerates — so a body can have both a fully populated
                // `ownedLocalYieldStates` AND an unwind edge.)
                //
                // So spill the POSITION first, then stamp the DISCHARGE. Only
                // the two positional visits prefer the spill; every other
                // reader of the state field (resume dispatch, the -2
                // idempotency guard, the cleanup switch) keeps seeing -1, which
                // is what §29.7's "Transitions to a completed state" asks for.
                //
                // The spill is `select(state > 0, state, 0)`: 0 is the calloc
                // default AND the correct answer for a body that threw before
                // its first yield (nothing established yet), and the guard also
                // makes this a no-op when #623's `finUnwindBB` already stamped
                // -1 on the way through a `finally` — that path keeps its
                // pre-existing behaviour rather than acquiring new.
                if (llvm::Value* parkGEP = smParkedStateGEPHere()) {
                    llvm::Value* cur = builder_.CreateLoad(
                        builder_.getInt32Ty(), smGEP, "prop.curstate");
                    llvm::Value* suspended = builder_.CreateICmpSGT(
                        cur, builder_.getInt32(0), "prop.suspended");
                    builder_.CreateStore(
                        builder_.CreateSelect(suspended, cur,
                                              builder_.getInt32(0),
                                              "prop.park"),
                        parkGEP);
                }
                builder_.CreateStore(builder_.getInt32(-1), smGEP);
            }
        }
        // per §33.3 / §22.13 (line 3845): propagateBB is the per-function
        // fallback when an exception reaches a frame with no enclosing handler
        // (unwindTarget_ unset). For an inner frame the exception is still live
        // in bz$exc, so propagating == returning (emitImplicitReturn leaves
        // bz$exc set and the caller's emitPendingCheck continues the unwind).
        // For `main` (the entry point — the only function named "main", §33.2),
        // this block IS the process boundary: the exception is genuinely
        // uncaught. §33.3 reserves exit code 0 for NORMAL completion (and a
        // legitimate `return 0`); §22.13 distinguishes "Main returning normally"
        // from "Main returning by an uncaught exception that the runtime
        // intercepts". Emitting `ret i32 0` here (the old emitImplicitReturn for
        // an i32 function) conflated the two — every uncaught exception reported
        // success. Instead, hand off to the runtime's abnormal-termination
        // interceptor, which writes to stderr and exits non-zero. This fires
        // ONLY for main's no-handler boundary: inner frames and every
        // caught/handled path (unwindTarget_ set → not propagateBB) are
        // untouched, so finally + owned-local unwind disposal still run.
        if (currentFn_->getName() == "main") {
            // #1366 / §33.3: hand the in-flight exception (and the built-in
            // exception vtable table, so the runtime reads §31.4 fields only on
            // objects that have that layout) to the runtime for the report +
            // exit(70).
            llvm::Value* exc =
                builder_.CreateLoad(ptrTy(), emitExcSlot(), "uncaught.exc");
            auto* fn = declareC("bz_unhandled_exception",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            fn->setDoesNotReturn();
            builder_.CreateCall(fn, {exc, builtinExcVtableTablePtr()});
            builder_.CreateUnreachable();
        } else {
            // §22.16 (v0.203, #2053) Slice 4b: emitImplicitReturn is the
            // redirect-aware variant (it routes through the armed ensures
            // epilogue when one is armed). But here we're synthesizing the
            // per-function fallback block for the exception path — there is
            // NO body here, just a `ret` to unwind the frame. Forcing the
            // redirect here would route the exception path back into the
            // ensures epilogue and re-evaluate clauses that have already
            // fired (or, worse, loop the frame). Disable the redirect for
            // this one emission so the propagate block emits a clean `ret`.
            llvm::Function* savedEnsuresFn = ensuresFn_;
            llvm::BasicBlock* savedEnsuresExitBB = ensuresExitBB_;
            llvm::AllocaInst* savedEnsuresResultSlot = ensuresResultSlot_;
            const ast::Type* savedEnsuresRetNode = ensuresReturnTypeNode_;
            const std::vector<ast::ContractClause>* savedEnsuresClauses =
                ensuresClauses_;
            std::string savedEnsuresTypeName = std::move(ensuresTypeName_);
            std::string savedEnsuresMemberName = std::move(ensuresMemberName_);
            ensuresFn_ = nullptr;
            ensuresExitBB_ = nullptr;
            ensuresResultSlot_ = nullptr;
            ensuresReturnTypeNode_ = nullptr;
            ensuresClauses_ = nullptr;
            ensuresTypeName_.clear();
            ensuresMemberName_.clear();
            emitImplicitReturn(currentFn_);
            ensuresFn_ = savedEnsuresFn;
            ensuresExitBB_ = savedEnsuresExitBB;
            ensuresResultSlot_ = savedEnsuresResultSlot;
            ensuresReturnTypeNode_ = savedEnsuresRetNode;
            ensuresClauses_ = savedEnsuresClauses;
            ensuresTypeName_ = std::move(savedEnsuresTypeName);
            ensuresMemberName_ = std::move(savedEnsuresMemberName);
        }
        if (save) builder_.SetInsertPoint(save);
    }
    return propagateBB_;
}


llvm::BasicBlock* Emitter::unwindBB() {
    return unwindTarget_ ? unwindTarget_ : propagateBB();
}


void Emitter::emitUnwindDisposals() {
    // #383: the unwind path — a `caughtException` entry is skipped (the
    // exception stays in flight; the enclosing handler frees it).
    for (std::size_t i = disposeStack_.size(); i-- > unwindScopeDepth_;)
        emitDisposals(disposeStack_[i], /*isUnwind=*/true);
}


void Emitter::emitUnwindDisposalsInRange(std::size_t lo, std::size_t hi) {
    // #467 / §28.3 (lines 4914, 4922): dispose the scopes in disposeStack_
    // indices [lo, hi), innermost (higher index) first — the reverse-declaration,
    // inner-before-outer order the §28.3 exception-path discharge requires. Used
    // when a `try`-with-`finally` re-propagates an exception to its enclosing
    // handler after running its own finally: the throw site only disposed scopes
    // down to the inner try's depth (unwindScopeDepth_), so any scope that lies
    // BETWEEN the inner try and the enclosing handler (e.g. an outer `using var`
    // declared in the same block as, but textually before, the inner try) is
    // still live and must be disposed here — AFTER the explicit finally body, as
    // the scope unwinds further outward (§28.10, v0.252: the finally is
    // "established at entry to the protected block", so "cleanups established
    // before the `try` run after the `finally`"; the body's OWN scope is not in
    // this range — it was established after the finally and is disposed at the
    // throw site).  (This comment previously quoted a §28.3 sentence — "the
    // explicit finally runs first ... then the implicit auto-dispose runs as the
    // scope unwinds further" — that no longer exists; #2892 ruled on which
    // establishment point it had meant.)
    // All entries are disposed on the isUnwind path; caughtException entries are
    // skipped — they stay in flight for the enclosing handler.
    for (std::size_t i = hi; i-- > lo;)
        emitDisposals(disposeStack_[i], /*isUnwind=*/true);
}


llvm::Value* Emitter::snapshotInFlightExc() {
    return builder_.CreateLoad(ptrTy(), emitExcSlot(), "disp.preexc");
}


void Emitter::emitDisposalFaultCheck(llvm::Value* excBefore,
                                     std::size_t unwindDisposeHi) {
    // fix #1878 + #1882 (§28.4, v0.306): route a fault RAISED BY THE DISPOSAL
    // that `excBefore` brackets, and only that.
    //
    // The obvious condition — `emitPendingCheck`'s plain `bz_exc != null` — is
    // WRONG here, and measurably so. A disposal site is one of the few places
    // reached with an exception ALREADY in the slot, and not only inside a user
    // destructor body (#1468's case, which `emitPendingCheck` does gate on): the
    // §29.7 abandonment disposal of an `await for` iterator runs the iterator's
    // compiler-generated teardown — sync-fallback destructor included — while
    // the exception that abandoned the loop is in flight. A plain non-null test
    // fires there on somebody else's exception, branches out of the teardown and
    // LEAKS the iterator frame (measured: 80 bytes, `async_iter_early_-
    // abandonment_dispose_1525.exception_from_loop_body_disposes_and_preserves_-
    // the_exception`). That is the #1468 defect class exactly, one context wider.
    //
    // So compare against the value in the slot before the disposal ran: fire iff
    // an exception is in flight AND it is not the one that was already there.
    // This is the same "did this operation RAISE" shape `emitEnsureStaticInit`
    // passes to `emitPendingCheckIf`, and it subsumes #1468's disposer-entry
    // gate — inside a destructor body the bracketed value IS the entry
    // exception. Supersession still works: `DisposeCore` frees the superseded
    // instance and leaves the NEW one in the slot (#2358), so the pointer
    // differs and the check fires on the survivor.
    //
    // ⚠ WHAT THE COMPARISON GUARANTEES, EXACTLY. It is an ADDRESS test over a
    // recycling allocator, so it is ABA-exposed: with `excBefore` = E1@A, a
    // first disposal that throws E2 makes #2358's supersede tail free E1 —
    // releasing A — and a second disposal that throws E3 can have `malloc`
    // hand back A. Then `after == excBefore` and this check does NOT fire. So
    // the guarantee is one-directional: firing implies a NEW fault (no false
    // positive, which is the property that matters — a false positive is the
    // measured iterator-frame leak above), while not firing does NOT imply no
    // fault. The miss degrades to base behaviour: E3 stays in the bz_exc slot
    // and the enclosing unwind's next pending check routes it, one site later
    // than §28.4 would like. Not reproduced, and not worth an identity counter
    // unless it is; recorded so the next reader does not mistake this for a
    // total test.
    llvm::Value* after =
        builder_.CreateLoad(ptrTy(), emitExcSlot(), "disp.postexc");
    llvm::Value* inFlight = builder_.CreateICmpNE(
        after, llvm::ConstantPointerNull::get(ptrTy()));
    llvm::Value* isNew = builder_.CreateICmpNE(after, excBefore);
    emitPendingCheckIf(builder_.CreateAnd(inFlight, isNew), unwindDisposeHi);
}


void Emitter::emitPendingCheck(std::size_t unwindDisposeHi) {
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    // fix #1468 / §28.3 "Exception-path discharge" / §28.4: inside a USER
    // destructor body that runs while an exception unwinds the stack, bz_exc
    // ALREADY holds the propagating exception when the body begins (§28.3: an
    // owned local/field live at the throw is disposed during unwinding). A
    // pending-check for an operation in the body — most commonly the §22.13
    // static-init guard emitted when the destructor touches a user static member
    // — must fire ONLY for a NEW exception that this operation itself threw, never
    // for the pre-existing propagating one. Firing on the pre-existing exception
    // branches straight to the enclosing unwind/return, abandoning the rest of
    // the destructor body, its owned-field auto-dispose (§28.4 step 2) and its
    // base chain — leaking the object and its whole owned subtree (the reported
    // unsound leak). So AND in "different from the exception captured at disposer
    // entry": the check fires iff in-flight AND new. When the entry value is null
    // (the normal, non-unwind disposal path — always the case for a destructor-
    // less type or a scope that exits without a throw) this reduces exactly to
    // the plain `exc != null`, so this path is behaviour-identical there. The
    // gate is emittingDisposerBody_, so ONLY user destructor-body emission is
    // affected — the general dispatch and every built-in/runtime disposer (e.g.
    // event-subscription and CancellationTokenSource cleanup) are untouched.
    //
    // fix #2872: read the entry value through disposerEntryExcHere(), which also
    // requires that we are still emitting into the DISPOSER's own function. A
    // lambda body or a lazily-built specialization emitted mid-destructor-body
    // is a different llvm::Function, and this `icmp` against the disposer's
    // entry load was then a cross-function reference (malformed IR).
    if (llvm::Value* entryExc = disposerEntryExcHere()) {
        llvm::Value* isNew = builder_.CreateICmpNE(exc, entryExc);
        pending = builder_.CreateAnd(pending, isNew);
    }
    emitPendingCheckIf(pending, unwindDisposeHi);
}

// #792 / §22.13: the shared route block for a conditional pending check. Branch
// to the unwind target (disposing unwound using-vars first, with the #1216
// supersede of a captured in-flight exception) iff `cond` is true, else
// continue. emitPendingCheck computes its condition (bz_exc != null, ANDed with
// the #1468 disposer "is-new" gate) and delegates here; emitEnsureStaticInit
// passes a "the static-init guard RAISED a new exception" condition so a
// pre-existing in-flight exception is not misrouted at the static-access site.
void Emitter::emitPendingCheckIf(llvm::Value* cond,
                                 std::size_t unwindDisposeHi) {
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "call.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "call.cont", currentFn_);
    builder_.CreateCondBr(cond, unwind, cont);
    builder_.SetInsertPoint(unwind);
    // #1216 (sibling of #1149 / §31.2): if a call inside a finally body (or
    // catch arm) propagates an exception E2 via this pending-check, and there
    // is a captured in-flight exception E1 exposed as currentCaughtExc_ (set by
    // the #1149 / #1108 finally / catch-guard supersede fix), E1 is SUPERSEDED
    // by E2 and must be freed exactly once HERE — the #1149 emitThrowValue path
    // handles direct `throw` but is never reached on a call-propagation edge.
    // Guard mirrors emitThrowValue's #397/#866 logic: throwLeavesArm ensures we
    // only free when this unwind target is the arm/finally's own outer boundary
    // (a throw caught by an INNER try within the finally must not free E1 —
    // the finally continues and E1 stays in flight). No isDirectRethrow guard
    // is needed: a call-propagated exception (bz$exc set by the callee) is
    // never the same object as currentCaughtExc_; they are distinct heap objects.
    bool throwLeavesArm =
        caughtExcArmTarget_ && unwindBB() == caughtExcArmTarget_;
    if (currentCaughtExc_ && throwLeavesArm) {
        // §30.9 (v0.179): if E1 is a task-owned fault whose disposal was deferred
        // (its owning task was disposed while E1 was in flight, #1288), its
        // handling ends HERE via the supersede — the catch-arm's handling-
        // completion drain will not fire on this unwind edge — so clear the
        // deferred record before disposing to keep the single dispose here and
        // leave no stale pointer in the deferred set. A no-op for a handler-owned
        // E1 (the common #1216 case).
        builder_.CreateCall(
            declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                     {ptrTy()}),
            {currentCaughtExc_});
        emitDisposeAndReclaim(currentCaughtExc_);
    }
    // fix #1882 (§28.4, v0.306): kAllLiveScopes is every pre-existing caller's
    // meaning and keeps emitUnwindDisposals' behaviour bit-for-bit. A SCOPE-END
    // disposal pass passes its own scope's index instead: that scope's entries
    // were just discharged by the pass that raised this exception, and it is
    // still on disposeStack_ (its owner pops it after the pass returns), so
    // re-disposing it here would double-release every entry that does not null
    // its own slot (a plain `stringRelease` is exactly such an entry).
    // fix #6052 (§28.3 *The return slot*, v0.338): a cleanup of a return
    // edge raised — "a `using var`'s or an auto-dropped local's disposer that
    // throws" — so "the return is abandoned ... and the slot's value is
    // disposed by the unwind ... at the raise, before every cleanup still
    // pending on the edge". Set only while such an edge's disposals are
    // being emitted, and only for the body that owns the slot (a disposer
    // body built lazily meanwhile is another function).
    if (asyncReturnEdgeSlot_ &&
        asyncReturnEdgeSlot_->getFunction() == currentFn_)
        emitAsyncReturnSlotUnwind(asyncReturnEdgeSlot_);
    if (unwindDisposeHi == kAllLiveScopes)
        emitUnwindDisposals();
    else
        emitUnwindDisposalsInRange(unwindScopeDepth_, unwindDisposeHi);
    builder_.CreateBr(unwindBB());
    builder_.SetInsertPoint(cont);
}


void Emitter::emitNewPendingCheck(llvm::Value* obj) {
    // §22.9 / §28.3 (FINDING [4]): same as emitPendingCheck, but on the unwind
    // edge ALSO reclaim the in-flight instance block `obj` whose constructor
    // threw. The ctor's own cleanup disposed the owned fields it had assigned and
    // the base subobject (it does not free `this`); freeing the block here — the
    // allocating site — reclaims it exactly once. This branch is taken only when
    // the ctor set the pending exception, so the free never reaches a normally-
    // constructed object (which is reclaimed later by its owning slot).
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "new.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "new.cont", currentFn_);
    builder_.CreateCondBr(pending, unwind, cont);
    builder_.SetInsertPoint(unwind);
    builder_.CreateCall(freeFn(), {obj});  // reclaim the instance block once
    // #1216: same as emitPendingCheck — if a constructor inside a finally body
    // (or catch arm) throws and supersedes E1, free E1 exactly once here.
    // See emitPendingCheck for the full rationale and double-free guards.
    bool throwLeavesArm =
        caughtExcArmTarget_ && unwindBB() == caughtExcArmTarget_;
    if (currentCaughtExc_ && throwLeavesArm) {
        // §30.9 (v0.179): see emitPendingCheck — clear any deferred-dispose
        // record for a superseded task-owned fault before its single dispose.
        builder_.CreateCall(
            declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                     {ptrTy()}),
            {currentCaughtExc_});
        emitDisposeAndReclaim(currentCaughtExc_);
    }
    emitUnwindDisposals();
    builder_.CreateBr(unwindBB());
    builder_.SetInsertPoint(cont);
}


llvm::BasicBlock* Emitter::newArgUnwindCleanup(llvm::Value* obj) {
    // §28.3 (#1968): build the unwind cleanup for an in-flight `new T(...)` whose
    // CONSTRUCTOR ARGUMENT throws DURING evaluation — before the constructor
    // runs. `obj` is calloc'd but uninitialized (every field zero, the ctor never
    // ran) and bound to no scope, so the ordinary unwind disposal never reclaims
    // it. RAW-free it here (a plain `free`, NOT a dispose — no destructor / owned-
    // field disposal, because no field is initialized), then continue to the
    // enclosing unwind exactly as the arg-throw would have without this cleanup.
    // Capturing `unwindBB()` BEFORE the caller overrides `unwindTarget_` preserves
    // the enclosing catch/propagate boundary; the pending-check at the arg site
    // has already run emitUnwindDisposals() for the enclosing scopes (this cleanup
    // does not repeat them), so a branch here simply adds the missing `obj` free.
    auto* savedInsert = builder_.GetInsertBlock();
    llvm::BasicBlock* enclosingUnwind = unwindBB();
    auto* cleanupBB =
        llvm::BasicBlock::Create(llvm_, "new.argthrow.cleanup", currentFn_);
    builder_.SetInsertPoint(cleanupBB);
    builder_.CreateCall(freeFn(), {obj});  // reclaim the uninitialized block once
    builder_.CreateBr(enclosingUnwind);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    return cleanupBB;
}


llvm::BasicBlock* Emitter::closureArgUnwindCleanup(
    const std::vector<llvm::Value*>& closures) {
    // fix #1073 / §28.3 "Exception-path discharge" / §28.9: the EXCEPTION-edge
    // half of the §28.14 call-bounded closure ARGUMENT-PASS release
    // (LlvmBackend_Calls2.cpp). The producer's +1 on an inline-closure argument
    // is dropped after the call on the normal edge; when the callee (or an
    // operation emitted inside emitMethodCall, e.g. a static-init guard)
    // PROPAGATES instead, `emitPendingCheck` branches away before that release
    // and the cell + env would leak on the caught/propagate path. Install this
    // block as `unwindTarget_` for the duration of emitMethodCall so every
    // unwind edge it emits drops those same temporaries on the way out.
    //
    // EXACTLY-ONCE. The block's only predecessors are the unwind edges emitted
    // WHILE it is installed; each of them is a `CreateCondBr` whose other side
    // is the `call.cont` continuation carrying the normal-path release. The two
    // are therefore mutually exclusive on any concrete execution: an exception
    // edge branches out of the expression for good (it ends in a branch to the
    // enclosing catch/propagate boundary and never rejoins `cont`), and the
    // normal edge is only reachable when no pending exception fired. Nested
    // `try` lowering inside an inlined callee body saves/restores
    // `unwindTarget_` around its own catch dispatch, so a HANDLED throw resumes
    // on the normal edge and reaches the post-call release exactly as it would
    // without this block.
    //
    // Capturing `unwindBB()` BEFORE the caller overrides `unwindTarget_`
    // preserves the enclosing catch/propagate boundary; the pending check has
    // already run emitUnwindDisposals() for the enclosing scopes, so this
    // cleanup only adds the missing closure releases (mirrors
    // newArgUnwindCleanup above). // per §28.14, §28.3, §28.9
    auto* savedInsert = builder_.GetInsertBlock();
    llvm::BasicBlock* enclosingUnwind = unwindBB();
    auto* cleanupBB =
        llvm::BasicBlock::Create(llvm_, "clarg.throw.cleanup", currentFn_);
    builder_.SetInsertPoint(cleanupBB);
    for (llvm::Value* cl : closures) emitClosureRelease(cl);  // null-safe
    builder_.CreateBr(enclosingUnwind);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    return cleanupBB;
}


// fix-forward #5262-ff (§4.6 / §28.3 "Exception-path discharge" / §28.9,
// v0.313): the EXCEPTION-edge half of #5262's borrowed-string-argument pin.
// The pin mints a +1 per admissible argument and the call site released it on
// the NORMAL-return edge only, so a callee that THREW leaked exactly one
// string pair per throwing call. #5262 documented that as "the safe direction"
// — a leak, not a use-after-free — but it is a real per-throw leak, and
// §28.3's Exception-path discharge is precisely the rule it violates: "the
// compiler treats every owned local and owned parameter as if it were wrapped
// in an implicit try/finally … Exceptions therefore do not produce leaks."
//
// This is `closureArgUnwindCleanup`'s exact twin with `emitStringRelease` in
// place of `emitClosureRelease`, and inherits its exactly-once argument
// verbatim: the block is installed as `unwindTarget_` ONLY across the call,
// its only predecessors are that call's unwind edges (each a `CreateCondBr`
// whose other side is the pending-check continuation carrying the normal-path
// release), and an exception edge never rejoins that continuation. Capturing
// `unwindBB()` BEFORE the caller overrides `unwindTarget_` preserves the
// enclosing catch/propagate boundary. A nested `try` inside an inlined callee
// body saves and restores `unwindTarget_` around its own catch dispatch, so a
// HANDLED throw resumes on the normal edge and still reaches the post-call
// release exactly once.
llvm::BasicBlock* Emitter::stringPinArgUnwindCleanup(
    const std::vector<llvm::Value*>& pins) {
    auto* savedInsert = builder_.GetInsertBlock();
    llvm::BasicBlock* enclosingUnwind = unwindBB();
    auto* cleanupBB =
        llvm::BasicBlock::Create(llvm_, "strpin.throw.cleanup", currentFn_);
    builder_.SetInsertPoint(cleanupBB);
    for (llvm::Value* pv : pins) emitStringRelease(pv);  // null-safe (§28.9)
    builder_.CreateBr(enclosingUnwind);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    return cleanupBB;
}


void Emitter::emitDynamicDispose(llvm::Value* obj) {
    // §28.2 / §28.4: dispatch the object's PUBLIC, idempotent Dispose through
    // its vtable (slot 2, CG35-itable) — which runs the destructor body, owned-field
    // disposals and the full base chain, AT MOST ONCE per object (the run-once
    // flag, §28.6). This does NOT free the object's heap block.
    //
    // #380 / §28: "dispose" still means run the destructor AND reclaim the block
    // (no GC). But the free is now performed by the disposal SITE (see
    // emitDisposeAndReclaim), not here. Reason: explicit `.Dispose()` (§28.5
    // line 5066) plus the scope-end / owned-field auto-dispose reach Dispose
    // TWICE on one object; if the first dispatch freed the block, the second
    // dispatch's vtable load would read freed memory (use-after-free) regardless
    // of any flag. Splitting "dispatch" (idempotent, repeatable) from "reclaim"
    // (done once, by the owning slot's auto-dispose) keeps repeated disposal
    // safe. vtable layout (CG35-itable + §8.6/§8.5 #417): [0]=td$, [1]=itmap$,
    // [2]=Dispose, [3]=ToString, [4+]=virtuals. Dispose stays at the FIXED slot 2.
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = builder_.CreateGEP(ptrTy(), vptr,
                                           {builder_.getInt32(2)});
    llvm::Value* disposeFn = builder_.CreateLoad(ptrTy(), slot);
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    builder_.CreateCall(fnTy, disposeFn, {obj});
    // fix #1882 (§28.4, v0.306): this call can run a USER destructor body, so
    // it can leave an exception in the bz_exc slot. Record that fact for
    // emitDisposals' post-pass propagate check — this function is the single
    // funnel every disposal site's dispatch goes through, so counting here
    // makes the gate exact for present and future arms alike. Counting rather
    // than setting a flag keeps nesting honest: a pass reads the DIFFERENCE
    // across itself, so a lazily-emitted inner function built mid-pass cannot
    // clear a bit the pass still needs.
    ++userCodeDisposalCount_;
}


void Emitter::emitDisposeAndReclaim(llvm::Value* obj, bool cdMayFree) {
    // #380 / §28: an auto-dispose site that OWNS the slot/field holding `obj`
    // (using-var scope-end, owned-field auto-dispose, exception unwind, the
    // caught-exception normal-exit free). It is the LAST dispose of this object
    // — sema guarantees no use-after-dispose, and any earlier explicit
    // `.Dispose()` only dispatched (it did not free). So dispatch the idempotent
    // Dispose (a no-op if already run) and THEN reclaim the block exactly once.
    // The caller is responsible for null-ing its slot afterward where a second
    // dispatch through the same slot is reachable. free(null) is a no-op.
    //
    // fix #4540 (§28.3, §30.1, v0.306) — the #3460 residual. The cross-task
    // borrow wait MUST run here, at the FRONT of the owner's teardown, not
    // only in front of the physical `free` in emitReclaimBlock.
    //
    // #3460 placed it immediately before `free(obj)`, which closes exactly one
    // race: the worker reading the object's BYTES after the owner unmapped
    // them. But `emitDynamicDispose` below dispatches the vtable Dispose, and
    // that runs the object's LOGICAL teardown — the destructor body and the
    // owned/refcounted FIELD releases (bz$<C>$DisposeCore) — while the async
    // frame that registered the borrow is still live and still writing those
    // same fields. §28.3's "the borrow of the captured state must outlive any
    // pending resumption" is a claim about the whole of the owner's teardown,
    // not just about the heap block.
    //
    // Measured (issue #4540): `using var c := new C(); var t := c.Fill(7);`
    // where the async `Fill` assigns `this._h := lambda() => k;` after an
    // `await`. The owner's Dispose released `_h` (closure A) and only THEN
    // waited; the worker's `this._h := B` read the same A and released it too
    // — two releases against one reference. The loser's `__atomic_sub_fetch`
    // returns -1, which is not `> 0`, so both callers fall into
    // bz_closure_release's last-drop path and both `free(env)`:
    // "AddressSanitizer: attempting double-free ... bz_closure_release <-
    // bz$async$M.C$Fill$Resume <- domain_drain". Waiting first makes the
    // worker's store happen-before the owner's field release, so each closure
    // is released exactly once (and B is no longer stranded).
    //
    // Null-safe, and a no-op for the objects nobody borrowed (a mutex lock and
    // a failed side-table lookup, no syscall). emitReclaimBlock keeps its own
    // call: that function is also entered directly from the `await using` path
    // (#3470), which never passes through here, and a second wait on an
    // already-drained object costs the same failed lookup.
    builder_.CreateCall(
        declareC("bz_wait_for_borrows", llvm::Type::getVoidTy(llvm_),
                 {ptrTy()}),
        {obj});
    emitDynamicDispose(obj);
    emitReclaimBlock(obj, cdMayFree);
}


// fix #3470 / §28.12 step 2 ("...and the heap block is released"): the
// block-release tail of emitDisposeAndReclaim, split out so the `await using`
// path can reuse it verbatim. That path's logical disposal is the awaited
// `DisposeAsync`, not the vtable Dispose, so it cannot call
// emitDisposeAndReclaim as a whole — but §28.12 says the RECLAMATION "is the
// same regardless of which logical-disposal path ran", and it was not: the
// async arm emitted a bare `free(obj)`, skipping the identity-hash side-table
// reap, the §31.4 CauseChain reclaim, and §28.2 checked-disposal's quarantine
// (it freed a block whose disposed flag a surviving borrow's member-entry
// guard is entitled to read).
void Emitter::emitReclaimBlock(llvm::Value* obj, bool cdMayFree) {
    // §8.3 / #2058: reap the side-table entry BEFORE the heap free (the object
    // is still live here, so the erase cannot read freed memory). Bounds the
    // identity-hash side-table for long-running services; the per-domain
    // deterministic counter is untouched, so §30.13 cross-run determinism
    // holds. Fires in BOTH release and §28.2 checked-disposal modes — a
    // quarantined disposed instance is logically dead (member access is
    // DisposedAccessException, §31.4), so its identity hash need not be
    // preserved. A never-hashed object (the common case) is a cheap mutex
    // lock + failed find inside the runtime helper.
    builder_.CreateCall(
        declareC("bz_obj_identity_hash_forget",
                 llvm::Type::getVoidTy(llvm_), {ptrTy()}),
        {obj});
    // §31.4/#811/#1389: reclaim the CauseChain snapshot HERE, at the terminal
    // reclaim site, immediately before the block itself is freed — NOT inside
    // the shared vtable Dispose (bz$exDisposeMsg, setupBuiltinExceptions). The
    // shared disposer releases Message/ParamName this same release-and-null
    // way, and freeing CauseChain there looks like the same idiom, but it is
    // NOT safe there: a still-borrowed (not-yet-reclaimed) exception's
    // CauseChain can be READ after ITS Dispose has already dispatched on some
    // path (e.g. `throw new X(msg, ex)` inside `ex`'s own catch arm reads
    // `ex.CauseChain` — bz_exc_causechain_link's `nested` argument — to build
    // the new snapshot). Freeing CauseChain from the shared disposer produced
    // a use-after-free there (diagnosed via gdb: bz_exc_causechain_link
    // reading a `nested` pointer into an already-freed/reused block) across
    // the several non-single-site exception disposal paths (catch-arm
    // dispose, scope-end dispose, async fault propagation). THIS site is
    // different: by ownership discipline it runs EXACTLY ONCE per object,
    // strictly after every live borrow/read of it has concluded (the true end
    // of the object's life) — it cannot race a still-in-flight CauseChain
    // read, so it is safe to free here.
    //
    // Gate: `bz_exc_reclaim_causechain` checks obj's vtable against the same
    // built-in-exception vtable table bz_unhandled_exception gates on (only a
    // built-in exception type installs bz$exDisposeMsg / carries the
    // CauseChain slot at BZ_EXC_CAUSE_OFFSET); a no-op for every other class.
    // The third argument is the built-in exception DESCRIPTOR table: a USER
    // subclass of a built-in exception (§31.4) carries the same header but its
    // vtable is vt$<C>, absent from the vtable table — issue #2989/#2979. The
    // runtime then walks obj's type-descriptor chain (vtable[0] = td$<C>, the
    // same null-terminated ancestor array emitIsA uses for `o is Exception`)
    // and reclaims iff a built-in exception descriptor is among the ancestors,
    // which is the one fact the compiler already computes (hasBuiltinExceptionBase
    // / builtinChainOf in setupBuiltinExceptions). See bezel_runtime_itable.cpp.
    //
    // Scope: this covers every reclaim that funnels through this function —
    // catch-arm dispose, using-var / owned-field scope-end dispose, and a
    // faulted task's scope-end fault-exception dispose. The pure-runtime
    // "vtable-Dispose-then-free" sites that bypass codegen entirely cannot
    // reach this gate (they have no access to the codegen-built vtable table),
    // so each consults bz_exc_is_runtime_owned directly, with the same
    // exemption this branch encodes: bz_dispose_class_obj (Task.FireAndForget's
    // abandoned-task continuation and the WhenAll/WhenAllOwned aggregate
    // fault/result cleanup), bz_domain_spawn's DomainFaultedException
    // wrap-and-dispose of the original fault, and bz_scope_dispose_exc's
    // first-fault-wins swallow. Those three are the complete set; if a fourth
    // terminal free of an exception object is added to
    // bezel_runtime_concurrency.cpp it must carry the same guard.
    builder_.CreateCall(
        declareC("bz_exc_reclaim_causechain", llvm::Type::getVoidTy(llvm_),
                 {ptrTy(), ptrTy(), ptrTy()}),
        {obj, builtinExcVtableTablePtr(), builtinExcDescTablePtr()});
    // §28.2 v0.134 / issue #601: in checked disposal mode the block is
    // quarantined (not freed) after disposal so that the per-instance disposed
    // flag at slot 1 (or 2) remains readable when a subsequent member-entry
    // guard loads it. This is a deliberate memory leak in checked (debug)
    // builds; the spec explicitly endorses deferred reclamation here. In
    // unchecked (release) mode the block is freed immediately as before.
    // (The CauseChain reclaim above runs unconditionally, ahead of this
    // branch, matching checked disposal's own deliberate-leak posture for
    // everything else the block owns — only the block header itself is kept
    // readable for the disposed-flag guard.)
    // #4602 (§28.2, v0.338): a checked build may still free a block no
    // surviving borrow can reach. `cdMayFree` is set only by the scope-end
    // release of a fresh, never-reassigned local; there the runtime's escaped
    // set decides (bz_cd_may_free: 1 = no escape recorded). A marked block,
    // and every block released anywhere else, keeps the quarantine.
    llvm::BasicBlock* cdKeepBB = nullptr;
    if (checkedDisposal_ && cdMayFree) {
        llvm::Value* mayFree = builder_.CreateCall(
            declareC("bz_cd_may_free", llvm::Type::getInt8Ty(llvm_),
                     {ptrTy()}), {obj});
        llvm::Value* doFree = builder_.CreateICmpNE(
            mayFree, builder_.getInt8(0), "cd.mayfree");
        llvm::BasicBlock* cdFreeBB = llvm::BasicBlock::Create(
            llvm_, "cd.free", currentFn_);
        cdKeepBB = llvm::BasicBlock::Create(llvm_, "cd.quarantine",
                                            currentFn_);
        builder_.CreateCondBr(doFree, cdFreeBB, cdKeepBB);
        builder_.SetInsertPoint(cdFreeBB);
    }
    if (!checkedDisposal_ || cdKeepBB) {
        // fix #3460 (§28.3 / §30.1): before freeing the heap block, wait for
        // any cross-task BORROWS on this object to be released. A borrow is
        // registered by an async frame whose `this = c` capture points at c
        // (the heap-use-after-free of #3460 was main's `using var c` exit
        // firing `free(c)` while the worker's resume continuation still read
        // `this._h`). The wait blocks until the worker's emitAsyncComplete
        // has run and released its borrow — closing the race. Null-safe and
        // cheap for objects nobody borrowed (the runtime side-table find is a
        // mutex lock + failed lookup, no syscall). Runs BEFORE the free,
        // AFTER the CauseChain reclaim — the CauseChain read races only with
        // a still-borrowed object, but the borrow's release synchronizes
        // with the wait's acquire-load so the reclaim is safe.
        builder_.CreateCall(
            declareC("bz_wait_for_borrows",
                     llvm::Type::getVoidTy(llvm_), {ptrTy()}),
            {obj});
        llvm::Value* isRO = builder_.CreateCall(
            declareC("bz_exc_is_runtime_owned", llvm::Type::getInt8Ty(llvm_),
                     {ptrTy()}), {obj});
        llvm::Value* notRO = builder_.CreateICmpEQ(
            isRO, builder_.getInt8(0), "obj.notro");
        llvm::BasicBlock* freeBB = llvm::BasicBlock::Create(
            llvm_, "obj.free", currentFn_);
        llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(
            llvm_, "obj.keeplive", currentFn_);
        builder_.CreateCondBr(notRO, freeBB, skipBB);
        builder_.SetInsertPoint(freeBB);
        builder_.CreateCall(freeFn(), {obj});
        builder_.CreateBr(skipBB);
        builder_.SetInsertPoint(skipBB);
        if (cdKeepBB) {
            builder_.CreateBr(cdKeepBB);
            builder_.SetInsertPoint(cdKeepBB);
        }
    }
}


// #4602 (§28.2, v0.338): record that `v` may outlive the evaluating frame
// (see the cdBenign_ declaration). Null-safe at runtime; a closed block (the
// value's producer terminated it) has nowhere to emit and nothing to mark.
void Emitter::emitCdMarkEscaped(llvm::Value* v) {
    if (!checkedDisposal_ || !v || !v->getType()->isPointerTy() ||
        llvm::isa<llvm::ConstantPointerNull>(v))
        return;
    llvm::BasicBlock* bb = builder_.GetInsertBlock();
    if (!bb || bb->getTerminator()) return;
    builder_.CreateCall(
        declareC("bz_cd_mark_escaped", llvm::Type::getVoidTy(llvm_),
                 {ptrTy()}), {v});
}


// #4602: can a value of static type `t` be (an alias of) a user class
// instance — the only blocks a free-eligible release ever frees? A class or
// interface type, `Object` (§5.1: the universal reference type), a built-in
// exception, and a type parameter that does not resolve to something else.
// Built-in collection / task / span types are runtime-managed blocks with
// their own release rules and are not marked (marking them would only grow
// the escaped set).
bool Emitter::cdMarkableType(const sema::TypeRef* t) {
    if (!t) return false;
    if (isBareTypeParamRef(t)) {
        const sema::TypeRef* r = resolveTypeParamRef(t);
        if (!r || r == t || isBareTypeParamRef(r)) return true;
        t = r;
    }
    using K = sema::TypeRef::Kind;
    if (t->kind == K::Primitive)
        return t->primitive == sema::PrimitiveType::Object;
    if (t->kind != K::Named) return false;
    const sema::TypeSymbol* ts = t->declaration;
    if (!ts) return true;  // an imported name codegen cannot classify
    if (ts->kind == ast::TypeDeclKind::Interface) return true;
    if (ts->kind != ast::TypeDeclKind::Class) return false;
    if (ts->decl) return true;  // a user-declared (or imported) class
    return isBuiltinExceptionTypeRef(t);
}


// #4602: does a call whose result has static type `t` leave no reference to
// its RECEIVER behind once it returns, other than through a value the result
// position itself is marked for? A plain value result, a string, or a
// user-declared type qualifies (a user type that holds the receiver got it
// through a store, an argument or a capture, each marked where it happens).
// A function value (a bound method), a built-in generic carrier (an
// iterator's IEnumerable, a Task, a collection) or an unknown type does not:
// such a result can hold the receiver without any marked position.
bool Emitter::cdCallResultKeepsNoReceiver(const sema::TypeRef* t) {
    if (!t) return false;
    using K = sema::TypeRef::Kind;
    switch (t->kind) {
    case K::Unit:
    case K::Primitive:
        return true;
    case K::Named:
        return t->declaration && t->declaration->decl;
    default:
        return false;
    }
}


// #4602: the children of `e` whose value `e` itself does not retain. A
// receiver of a member access or a method call (the callee's own stores are
// marked inside it), an operand whose value flows on into `e`'s own result
// (marked at `e`'s position if that position retains it), or a value only
// compared or tested. Everything else — call and constructor arguments,
// collection and `with` elements, a lambda's expression body, a thrown
// operand — stays unmarked here and is therefore marked.
void Emitter::noteCdBenignChildren(const ast::Expr& e) {
    // Two kinds of parent. A CONSUMER uses the child's value without handing
    // it on (a member-access target, a comparison operand, an `is` test), so
    // the child is benign whatever the parent is. A PASS-THROUGH parent's value
    // IS the child's value (`x!`, `(if c then x else y)`, `x ?? y`, `x as T`),
    // so the child is benign only when the parent position itself is benign
    // or is marked. FAIL CLOSED: an untyped pass-through parent that is not
    // itself benign leaves its children marked. The child's entry is erased
    // first, because the same AST node is re-lowered per generic
    // instantiation and an earlier verdict must not carry over.
    auto benign = [this](const ast::Expr* c) {
        if (c) cdBenign_.insert(c);
    };
    auto passThrough = [this, &e](const ast::Expr* c) {
        if (!c) return;
        cdBenign_.erase(c);
        if (cdBenign_.count(&e) || cdExprMarkable(e)) cdBenign_.insert(c);
    };
    auto isFn = [this](const ast::Expr& x) {
        const sema::TypeRef* t = ctx_.exprType(x);
        return !t || t->kind == sema::TypeRef::Kind::Function;
    };
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (!isFn(e)) benign(ma->target.get());
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (!isFn(e)) benign(nm->target.get());
        return;
    }
    const ast::Expr* callee = nullptr;
    if (auto* c = dynamic_cast<const ast::CallExpr*>(&e)) callee = c->callee.get();
    else if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(&e))
        callee = c->callee.get();
    if (callee) {
        if (!cdCallResultKeepsNoReceiver(ctx_.exprType(e))) return;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee))
            benign(ma->target.get());
        else if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(callee))
            benign(nm->target.get());
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        benign(ix->target.get());
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (tt->op == "as") passThrough(tt->operand.get());
        else benign(tt->operand.get());
        return;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        passThrough(pb->operand.get());
        return;
    }
    if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        passThrough(ce->thenValue.get());
        passThrough(ce->elseValue.get());
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->op == "??") {
            passThrough(be->left.get());
            passThrough(be->right.get());
        } else {
            benign(be->left.get());
            benign(be->right.get());
        }
        return;
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        benign(ue->operand.get());
        return;
    }
    if (auto* cx = dynamic_cast<const ast::CaseExpr*>(&e)) {
        benign(cx->scrutinee.get());
        return;
    }
}


// #4602: will lowerExpr mark the value of `e` (absent a benign parent)?
// Typed: by cdMarkableType. UNTYPED: yes (fail closed), except a bare
// `this`/`base` inside a record/enum/choice instance body, whose receiver is
// a value pointer (cdNonClassThisFns_). emitCdMarkEscaped itself skips a
// non-pointer value, so an untyped scalar costs nothing.
bool Emitter::cdExprMarkable(const ast::Expr& e) {
    if (const sema::TypeRef* t = ctx_.exprType(e)) return cdMarkableType(t);
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if (id->name == "this" || id->name == "base") {
            llvm::BasicBlock* bb = builder_.GetInsertBlock();
            const llvm::Function* fn = bb ? bb->getParent() : nullptr;
            return !(fn && cdNonClassThisFns_.count(fn));
        }
    }
    return true;
}


// §6.6 / issue #974 / §30.11: release a NON-NULL TaskCompletionSource<T> /
// OwningTaskCompletionSource<T> / TaskCompletionSource (`bz_tcs_t*`). The
// caller supplies the null guard and, where a slot holds `tcs`, nulls it
// afterwards (§28.2 exactly-once).
//
// Factored out by fix #2921 (v0.240) so the `using var` scope-exit auto-drop
// and an EXPLICIT `tcs.Dispose()` share ONE resolution of "what releases a
// TCS". Before that, only the scope-exit path had one, and the explicit call
// fell through to the generic vtable dispatch — which a `bz_tcs_t*` has no
// vtable for (measured: SIGSEGV out of `Main`).
//
// The four steps, all on the non-null path:
//   1. bz_tcs_fault_if_pending(tcs, ioe_vtable): if the inner task has not been
//      completed yet, fault it with InvalidOperationException so that any
//      awaiter wakes up with an error rather than hanging indefinitely.
//   2. Error-obj cleanup on the inner task (mirrors the d.task arm): if the
//      task is faulted, dispose the error object exactly once.
//   3. bz_task_fire_and_forget(inner_task): release the task struct when it has
//      finished running (immediate if already done, deferred if running).
//   4. bz_tcs_free(tcs): free the bz_tcs_t wrapper itself.
void Emitter::emitTcsRelease(llvm::Value* tcs) {
    // Step 1: fault the inner task if not yet completed.
    llvm::Value* ioeVt = llvm::ConstantPointerNull::get(ptrTy());
    {   auto vit = builtinVtable_.find("InvalidOperationException");
        if (vit != builtinVtable_.end()) ioeVt = vit->second;
    }
    auto* faultFn = declareC("bz_tcs_fault_if_pending",
                              llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
    builder_.CreateCall(faultFn, {tcs, ioeVt});

    // Step 2: get the inner task pointer and clean up its error object.
    auto* innerFn = declareC("bz_tcs_task", ptrTy(), {ptrTy()});
    llvm::Value* t = builder_.CreateCall(innerFn, {tcs}, "tcs.inner");
    {
        auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
        llvm::Value* err = builder_.CreateCall(isErrFn, {t}, "tcs.task.err");
        auto* edBB = llvm::BasicBlock::Create(llvm_, "tcs.task.errdisp",
                                               currentFn_);
        auto* eaBB = llvm::BasicBlock::Create(llvm_, "tcs.task.errcont",
                                               currentFn_);
        builder_.CreateCondBr(err, edBB, eaBB);
        builder_.SetInsertPoint(edBB);
        auto* eoFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
        llvm::Value* eo = builder_.CreateCall(eoFn, {t}, "tcs.task.errobj");
        llvm::Value* eoNull = builder_.CreateICmpEQ(
            eo, llvm::ConstantPointerNull::get(ptrTy()));
        auto* doBB = llvm::BasicBlock::Create(llvm_, "tcs.task.eodisp",
                                               currentFn_);
        builder_.CreateCondBr(eoNull, eaBB, doBB);
        builder_.SetInsertPoint(doBB);
        // §30.9 in-flight-deferral (v0.179): if the fault is still in
        // flight (borrowed by an awaiter mid-dispatch — a TCS-backed task
        // bound inside a `try`, unwound by its own re-raised fault),
        // defer its disposal to the catch arm's terminal act rather than
        // freeing the object mid-dispatch (UAF). Otherwise dispose now.
        auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                      llvm::Type::getInt8Ty(llvm_), {ptrTy()});
        llvm::Value* bv = builder_.CreateCall(isBorrowedFn, {eo},
                                              "tcs.eo.borrowed");
        llvm::Value* isB = builder_.CreateICmpNE(
            bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
        auto* deferBB = llvm::BasicBlock::Create(llvm_, "tcs.eo.defer",
                                                 currentFn_);
        auto* dispBB  = llvm::BasicBlock::Create(llvm_, "tcs.eo.dispnow",
                                                 currentFn_);
        builder_.CreateCondBr(isB, deferBB, dispBB);
        builder_.SetInsertPoint(deferBB);
        auto* deferFn = declareC("bz_defer_exc_dispose",
                                 llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(deferFn, {eo});
        // §30.9 / #3030: null the task's error_obj field even on the
        // DEFER path. The exception is in flight (the await/`.Result`
        // re-raise marked it borrowed) and the catch arm's terminal act
        // is the disposer of record (it disposes via bz_is_exc_deferred →
        // emitDisposeAndReclaim + bz_clear_exc_deferred, the §30.9
        // in-flight-deferral contract). The task's error_obj field, if
        // left non-null after the defer, would be re-disposed by
        // bz_task_fire_and_forget's FAST path below (the #3030 fix's
        // new `if (error_obj && !fault_borrowed)` arm) — and the catch
        // arm then reads the freed pointer (SIGSEGV / ASan UAF). The
        // dispBB branch (synchronous dispose) gets the same null-write
        // a few lines below for the same reason.
        auto* clearEoDeferFn = declareC("bz_task_clear_error_obj",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(clearEoDeferFn, {t});
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(eo);
        // §30.9 / #3030: the task's error_obj field still holds a dangling
        // pointer to the exception we just freed. bz_task_fire_and_forget
        // (called below) takes the FAST path on a settled task and its
        // error_obj-disposal guard would re-dispose the freed pointer. Null
        // the field now so the FAST path's `if (error_obj && ...)` sees null.
        auto* clearEoFn = declareC("bz_task_clear_error_obj",
                                   llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(clearEoFn, {t});
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(eaBB);
    }

    // Step 2.5: §28.3 / #1250: for OwningTaskCompletionSource<T:class>, dispose
    // the inner task's owned result if set but never consumed via .Result/await.
    // bz_tcs_dispose_owned_result is safe on all paths:
    //   • owned_result_disposer == null → plain TCS or value-type; no-op.
    //   • task->is_error → TrySetException or fault_if_pending fired; no result.
    //   • task->result == null → bz_task_take_result already consumed it; no-op.
    // Must run BEFORE fire-and-forget while inner_task is still alive.
    {
        auto* dispOwnedFn = declareC("bz_tcs_dispose_owned_result",
                                      llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(dispOwnedFn, {tcs});
    }

    // Step 3: fire-and-forget the inner task.
    auto* fafFn = declareC("bz_task_fire_and_forget",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    builder_.CreateCall(fafFn, {t});

    // Step 4: free the TCS struct itself.
    auto* freeFn2 = declareC("bz_tcs_free",
                              llvm::Type::getVoidTy(llvm_), {ptrTy()});
    builder_.CreateCall(freeFn2, {tcs});
}


// fix #3685 / §30.5 / §28.11 / §28.3 (v0.287): per-element disposer thunk for
// a TaskCompletionSource-family value (TaskCompletionSource,
// TaskCompletionSource<T>, OwningTaskCompletionSource<T>) when the surrounding
// collection / Pattern A task result slot is OWNING — i.e., the slot is
// responsible for releasing the TCS at scope-end / clear / remove / task
// disposal. The runtime representation is identical for all three TCS names
// (a `bz_tcs_t*`); the disposal protocol is the same four-step
// `emitTcsRelease` sequence (fault_if_pending, error-obj cleanup with §30.9
// in-flight deferral, owned_result_dispose for OwningTaskCompletionSource
// <T:class> keyed inside the runtime, fire-and-forget the inner task,
// bz_tcs_free the TCS struct).
//
// The element slot holds a NON-NULL `bz_tcs_t*` by construction — slot-emptied
// sites null the slot before recycling, the same invariant the `using var`
// scope-exit path holds. Same shape as subscriptionDisposeThunk: cached
// module-level thunk, own IRBuilder + insertion point restored on the way out.
// No per-T variety is needed: the per-T owned-result disposer is registered
// on the bz_tcs_t itself via bz_tcs_set_owned_disposer at `new`
// initialisation (#1250), and bz_tcs_dispose_owned_result consults that slot
// — the emitter never sees the per-T disposer, so one thunk covers every T.
//
// Pre-fix this arm did not exist: elementPolicyFor fell through to the Value
// row for TCS-typed slots (no declaration ⇒ no class/interface arm), so an
// OwningArray<TaskCompletionSource<T>> freed with a null disposer leaked the
// bz_tcs_t struct + the inner task frame + the produced result, and a
// Pattern A `Task<TaskCompletionSource<T>>` returned from an async method
// and never awaited leaked the same (MEASURED in #3685's repro:
// 24 B bz_tcs_t + 184 B inner bz_task_t + 4 B produced int result under
// LSan, all allocated in the async body).
// per §30.5 / §30.11 / §28.3, v0.287
llvm::Function* Emitter::tcsElementDisposerThunk() {
    if (tcsElementDisposerThunk_) return tcsElementDisposerThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$tcsElementDispose", module_.get());
    llvm::Argument* tcs = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    builder_.SetInsertPoint(entry);
    emitTcsRelease(tcs);
    builder_.CreateRetVoid();
    currentFn_ = savedFn;
    builder_.restoreIP(savedIP);
    tcsElementDisposerThunk_ = fn;
    return fn;
}


// fix #2921 (spec §28.2 / §28.9 / §8.1, v0.240): emit the RELEASE of a
// vtable-less built-in class receiver `recv` whose built-in name is `bn`, and
// report whether one was emitted.
//
// WHY THIS EXISTS. The explicit-`Dispose()` lowering (LlvmBackend_Calls2.cpp)
// is a chain of per-built-in arms ending in the generic
// `emitDisposeAndReclaim`, which loads word 0 as a vtable and calls slot 2. A
// built-in class has no vtable there — word 0 is payload data — so every
// built-in the chain did not name fell into a wild indirect jump. MEASURED on
// f069645: `var sb := new StringBuilder(); sb.Dispose();` emitted
// `mov (%rax),%rcx ; call *0x10(%rcx)` and SIGSEGV'd at address 0, with no
// diagnostic; the same for `Random`, `EventSource<T>`, `AsyncEventSource<T>`,
// `TaskCompletionSource<T>` and `OwningTaskCompletionSource<T>`.
//
// The caller gates on `common::isVtableLessBuiltinClassName` and, when this
// returns false, emits NOTHING rather than the generic dispatch. That is
// deliberate and is the point of the split: a built-in whose release this
// function does not yet know leaks a bounded amount of memory, where the old
// fall-through corrupted control flow. A leak is recoverable; a wild jump is
// not (the same trade recorded on #2874 and #2823).
//
// Each release below is the SAME call the `using var` scope-exit auto-drop
// already emits for that built-in, reached from the one place it is written:
// StringBuilder → the borrowing-`Array<string>` free with the string-element
// release thunk (LlvmBackend_Setup.cpp `registerUsingDisposal`, #1185);
// EventSource/AsyncEventSource → `bz_event_source_free` (#2026);
// TaskCompletionSource family → `emitTcsRelease` (#974). `Random` had NO
// resolution on either path (its `bz_random_free` was declared in the runtime
// header and referenced by no emitter at all — measured: 3200 bytes leaked in
// 200 allocations under LSan), so this fix adds the one resolution and wires
// BOTH paths to it.
// fix #2973 (§28.3 / §28.9, stdlib §6.4, v0.240): which built-ins does the
// `using var` SCOPE-EXIT auto-drop route through emitVtablelessBuiltinRelease?
//
// #2921 wired `Random` by hardcoding the name into `registerUsingDisposal`'s
// arm. That arm's three DETECTION probes (declared type, NewExpr type name,
// initializer exprType) are the reusable part and the name is the parameter, so
// the name lives here and the arm reads it — adding a built-in to the auto-drop
// is a line in this function, next to the release it must agree with, rather
// than a fourth copy of the detection code somewhere else.
//
// ⚠ THIS IS STILL A POSITIVE LIST, and a positive list fails by omission. What
// bounds the damage is that it fails toward a LEAK (the pre-existing behaviour)
// and never toward a double free. The population was MEASURED rather than
// guessed: a 200-iteration `using var x := new T(…)` loop under generated-code
// LSan over every constructible built-in — Random, StringBuilder, Stopwatch,
// Mutex, Semaphore, CancellationTokenSource, EventSource, AsyncEventSource,
// Channel, TaskCompletionSource, Array, Dictionary, Set, SortedSet,
// SortedDictionary, Stack, Queue, LinkedList, OwningBuffer, Buffer — found
// EXACTLY TWO leaking, `Atomic<T>` and `AtomicInteger<T>`, at 4800 bytes in 200
// allocations each. Everything else is already claimed by an arm. The general
// defect ("a `using var` on a built-in that no arm claims emits NOTHING, in
// silence") is filed separately; it needs a "does this built-in own heap?"
// authority that does not exist yet, and inventing one here would be a negative
// list — the shape that fails by ACCEPTING garbage.
const char* Emitter::usingAutoDropVtablelessBuiltin(std::string_view n) {
    if (n == "Random")        return "Random";           // #2921
    if (n == "Atomic")        return "Atomic";           // #2973
    if (n == "AtomicInteger") return "AtomicInteger";    // #2973
    return nullptr;
}


// fix-forward #4185 (review round 2, §25 / §6): the ONE place `bn` is
// compared against this builtin-family's names. Pure — no IR emitted, no
// Emitter state read beyond the argument — so both
// `emitVtablelessBuiltinRelease` (which switches on the result to decide
// WHAT to emit) and `vtablelessBuiltinReleaseIsKnown` (which only asks
// whether a match exists) are downstream of this single classification;
// there is no second list left to drift out of sync with it. See the
// declaration in LlvmBackend_impl.hpp for the round-1-vs-round-2 history.
Emitter::VtablelessBuiltinReleaseArm
Emitter::classifyVtablelessBuiltinRelease(std::string_view bn) {
    if (bn == "StringBuilder") return VtablelessBuiltinReleaseArm::StringBuilder;
    if (bn == "Random") return VtablelessBuiltinReleaseArm::Random;
    if (bn == "Atomic" || bn == "AtomicInteger")
        return VtablelessBuiltinReleaseArm::AtomicFamily;
    if (bn == "EventSource" || bn == "AsyncEventSource")
        return VtablelessBuiltinReleaseArm::EventSourceFamily;
    if (bn == "TaskCompletionSource" || bn == "OwningTaskCompletionSource")
        return VtablelessBuiltinReleaseArm::TaskCompletionSourceFamily;
    if (bn == "Channel") return VtablelessBuiltinReleaseArm::Channel;
    if (bn == "ChannelSender") return VtablelessBuiltinReleaseArm::ChannelSender;
    if (bn == "Stopwatch") return VtablelessBuiltinReleaseArm::Stopwatch;
    if (bn == "Mutex") return VtablelessBuiltinReleaseArm::Mutex;
    if (bn == "Lock") return VtablelessBuiltinReleaseArm::Lock;
    if (bn == "Semaphore") return VtablelessBuiltinReleaseArm::Semaphore;
    if (bn == "SemaphoreToken")
        return VtablelessBuiltinReleaseArm::SemaphoreToken;
    if (bn == "CancellationTokenSource")
        return VtablelessBuiltinReleaseArm::CancellationTokenSource;
    if (bn == "FileStream") return VtablelessBuiltinReleaseArm::FileStream;
    if (bn == "Subscription") return VtablelessBuiltinReleaseArm::Subscription;
    return VtablelessBuiltinReleaseArm::None;
}

// fix-forward #4185 (review round 2, §25): "did classification find a
// match" — no name list of its own (round 1's hand-synced array is gone;
// this and `emitVtablelessBuiltinRelease` below both ask
// `classifyVtablelessBuiltinRelease`, the one place names are spelled).
bool Emitter::vtablelessBuiltinReleaseIsKnown(std::string_view bn) {
    return classifyVtablelessBuiltinRelease(bn) !=
           VtablelessBuiltinReleaseArm::None;
}

bool Emitter::emitVtablelessBuiltinRelease(std::string_view bn,
                                           llvm::Value* recv) {
    if (bn.empty() || !recv || !recv->getType()->isPointerTy()) return false;
    auto callVoid = [&](const char* fn, llvm::ArrayRef<llvm::Value*> args) {
        std::vector<llvm::Type*> ps(args.size(), ptrTy());
        builder_.CreateCall(declareC(fn, llvm::Type::getVoidTy(llvm_), ps),
                            args);
    };
    switch (classifyVtablelessBuiltinRelease(bn)) {
    // stdlib §3.1 / #1185: a StringBuilder IS a `bz_array_t*` of string parts
    // and owns a +1 per part; the string-element release thunk discharges each.
    case VtablelessBuiltinReleaseArm::StringBuilder: {
        llvm::Value* disp = stringElementReleaseThunk();
        builder_.CreateCall(
            declareC("bz_array_free", llvm::Type::getVoidTy(llvm_),
                     {ptrTy(), ptrTy()}),
            {recv, disp ? disp
                        : llvm::cast<llvm::Value>(
                              llvm::ConstantPointerNull::get(ptrTy()))});
        return true;
    }
    // stdlib §3 (v0.240): `Random`'s Dispose is a documented no-op — "`Random`
    // is a class with a no-op `Dispose` (its only state is its internal seed;
    // there is nothing to release)" — but the INSTANCE is still reclaimed:
    // §28.9's trivial-dispose subsection says "the compiler frees the
    // undischarged value at its owner's release point", which stdlib §1
    // restates as "A trivial-dispose class's instances are compiler-freed at
    // their owner's release point". `bz_random_alloc` mallocs one and
    // `bz_random_free` is a plain `free`.
    case VtablelessBuiltinReleaseArm::Random:
        callVoid("bz_random_free", {recv});
        return true;
    // fix #2973 (§28.3 / §28.9, stdlib §6.4, v0.240): `Atomic<T>` /
    // `AtomicInteger<T>`. `bz_atomic_dispose` FREES — verified in the runtime,
    // not inferred: it flips a `disposed.exchange(true)` once and then calls
    // `atomic_core_release`, which `delete`s the `bz_atomic_t` when the last
    // core ref drops (bezel_runtime_sync.cpp), while `bz_atomic_new` hands back
    // refs=1. So a single call here is a complete release, not a mark — the
    // question #2973 asked to settle before wiring anything, because a mark
    // would have needed a free as well and a free plus a free is a double free.
    //
    // Exactly-once across the two spellings is already structural: #2967 made
    // the explicit `a.Dispose()` arm NULL the receiver's slot, and the
    // `vtablelessBuiltin` scope-exit path null-GUARDS its reload — measured
    // clean here under generated-code ASan for `using var` + explicit
    // `Dispose()` in the same 200-iteration loop.
    case VtablelessBuiltinReleaseArm::AtomicFamily:
        callVoid("bz_atomic_dispose", {recv});
        return true;
    // §24 / §6.5 / #2026: sync and async sources share one runtime
    // representation, so both free identically (null-safe).
    case VtablelessBuiltinReleaseArm::EventSourceFamily:
        callVoid("bz_event_source_free", {recv});
        return true;
    // §6.6 / §30.11 / #974: the four-step TCS release (see emitTcsRelease).
    case VtablelessBuiltinReleaseArm::TaskCompletionSourceFamily:
        emitTcsRelease(recv);
        return true;
    // §30.15 (#600 phase c): a Channel<T>. Dispose via bz_channel_dispose
    // (closes, faults pending receivers with ChannelClosedException, disposes
    // undelivered items in reverse order per §28.11, frees the struct). The
    // CCE vtable is read from `builtinVtable_` (set up by
    // setupBuiltinExceptions); may be null if Channel<T> reaches this path
    // before setup — bz_channel_dispose tolerates a null second argument
    // (verified in the runtime).
    case VtablelessBuiltinReleaseArm::Channel: {
        llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
        {   auto vit = builtinVtable_.find("ChannelClosedException");
            if (vit != builtinVtable_.end()) cceVt = vit->second;
        }
        auto* fn = declareC("bz_channel_dispose",
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, cceVt});
        return true;
    }
    // §30.15: a ChannelSender<T>. bz_channel_sender_release decrements the
    // sender count (may close the channel if it reaches zero). fix #5852 /
    // §30.15 v0.334: the runtime takes (tx, cce_vtable) — the vtable is used to
    // fault PENDING RECEIVERS when this release closes the channel. This arm
    // passed {recv} alone; declareC reuses the first declaration of the name, so
    // declaring it 1-param here also poisoned every other call site in the
    // module. Mirrors the Channel arm directly above.
    case VtablelessBuiltinReleaseArm::ChannelSender: {
        llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
        {   auto vit = builtinVtable_.find("ChannelClosedException");
            if (vit != builtinVtable_.end()) cceVt = vit->second;
        }
        callVoid("bz_channel_sender_release", {recv, cceVt});
        return true;
    }
    // §8.1 / #646: a Stopwatch — a `bz_stopwatch_t*` freed via
    // bz_stopwatch_free (null-safe). Same release the `using var` arm emits.
    case VtablelessBuiltinReleaseArm::Stopwatch:
        callVoid("bz_stopwatch_free", {recv});
        return true;
    // §6.2 / #647: a Mutex. bz_mutex_dispose marks disposed and drops the
    // Mutex object's core ref. Idempotent per §28.2 (the disposed flag
    // prevents double-release). Null-safe (handles Mutex?).
    case VtablelessBuiltinReleaseArm::Mutex:
        callVoid("bz_mutex_dispose", {recv});
        return true;
    // §6.2 / #647: a Lock (owned guard returned by Mutex.Acquire /
    // TryAcquire). bz_lock_release checks the released flag, null-safe.
    case VtablelessBuiltinReleaseArm::Lock:
        callVoid("bz_lock_release", {recv});
        return true;
    // §6.3 / #647: a Semaphore. bz_semaphore_dispose marks disposed and drops
    // the Semaphore object's core ref. Idempotent.
    case VtablelessBuiltinReleaseArm::Semaphore:
        callVoid("bz_semaphore_dispose", {recv});
        return true;
    // §6.3 / #647: a SemaphoreToken (owned permit). bz_semaphore_token_release
    // checks the released flag, null-safe.
    case VtablelessBuiltinReleaseArm::SemaphoreToken:
        callVoid("bz_semaphore_token_release", {recv});
        return true;
    // #463: a CancellationTokenSource. bz_cts_dispose signals all tokens and
    // frees the source. Idempotent (signalled flag prevents double-signal).
    case VtablelessBuiltinReleaseArm::CancellationTokenSource:
        callVoid("bz_cts_dispose", {recv});
        return true;
    // stdlib §7.2 / #645: a FileStream — a `bz_filestream_t*` disposed via
    // bz_filestream_dispose (flush + close the OS handle, free the wrapper).
    // Null-safe (a failed open stores null).
    case VtablelessBuiltinReleaseArm::FileStream:
        callVoid("bz_filestream_dispose", {recv});
        return true;
    // §2.11 / §24: disposing a Subscription removes the handler from its
    // event source. The explicit-Dispose chain reaches this only when
    // `isSubscriptionExpr`'s STRUCTURAL detection (the local's initializer is an
    // event `Subscribe(...)` call) did not fire — a Subscription-typed
    // parameter/field, say — where the fall-through was the wild jump. Same
    // release call as that arm.
    case VtablelessBuiltinReleaseArm::Subscription:
        callVoid("bz_subscription_dispose", {recv});
        return true;
    case VtablelessBuiltinReleaseArm::None:
        return false;
    }
    return false;  // unreachable — every enumerator handled above
}


// fix #2967 (spec §28.2 / §28.3, v0.240): an explicit `x.Dispose()` on a
// BUILT-IN receiver performs that built-in's release right here; this is the
// one spelling of the obligation that comes with having done so — null the
// receiver's slot, so §28.3's scope-exit (or unwind) auto-drop reloads null and
// discharges nothing. Without it the SAME object is released twice.
//
// WHY IT IS A PROPERTY OF THE RELEASE, NOT A PER-ARM HABIT. The explicit-
// `Dispose()` lowering (LlvmBackend_Calls2.cpp) is a chain of per-built-in arms,
// and most of them ended with a hand-copied nine-line "null the slot" block —
// while the collection arms (`isCollectionTypeName` → `bz_array_free`,
// `isLinkedListTypeName` → `bz_llist_free`, the dict-backed-`Set` arm), the
// `Stopwatch` arm and the `Weak` arm did not. Those arms are not safe by
// design; they were safe only where the runtime free HAPPENED to tolerate a
// repeat (`bz_dict_free` returns early on a disposed table), which is an
// accident of the runtime rather than a rule about disposal. Where it did not,
// the second discharge was a double free with no diagnostic.
//
// MEASURED on this fix's parent, 200-iteration loops of
// `using var x := new T(…); x.Dispose();` under generated-code ASan — TWELVE
// receivers, against the three the issue named: `Array<T>`, `OwningArray<T>`,
// `Set<T>`, `SortedSet<T>`, `Stack<T>`, `OwningStack<T>`, `Queue<T>`,
// `OwningQueue<T>`, `LinkedList<T>`, `OwningLinkedList<T>` and `OwningBuffer<T>`
// SEGV in the allocator on the second free; `Stopwatch` reports "attempting
// double-free"; `Weak<T>`'s repeat release corrupts the handle count into an
// 8000-byte leak. Each is clean in BOTH single-spelling controls (the explicit
// call alone, and the `using var` auto-drop alone) — the signature of a double
// DISCHARGE, not of a broken release. `objdump` of the `Array<int>` body shows
// it directly: `bz_array_alloc` → store to the slot → `bz_array_free` →
// unconditional reload of that same slot → `bz_array_free`, with no null test
// anywhere between.
//
// So this is asked once, from one place, for the whole family; the arms it
// serves state the rule by CALLING it rather than by re-typing it.
//
// Two deliberate silences:
//   * CHECKED disposal mode (§28.2 v0.135) quarantines instead of freeing and
//     the disposed-flag guard reads THROUGH the slot at the next member entry,
//     so the pointer must stay readable — the same exclusion every arm that
//     already nulled applied. (`FileStream` is the one arm that nulls even in
//     checked mode, because it has no quarantine, and it keeps its own store.)
//   * A receiver that is not a NAMED lvalue has no slot to null — a temporary
//     (`new Array<int>(2).Dispose()`) owns nothing that outlives the statement.
//     Restricting to `IdentifierExpr` / `MemberAccessExpr` — the test the
//     `isDictTypeName` arm already used — also keeps this from re-lowering an
//     `a[i].Dispose()` subscript through `lvalueAddr`, which would re-evaluate
//     the index expression and re-emit its §15.1 bounds check.
// // per §28.2 / §28.3
void Emitter::nullExplicitlyDisposedBuiltinSlot(const ast::Expr& recvExpr) {
    if (checkedDisposal_) return;
    if (!dynamic_cast<const ast::IdentifierExpr*>(&recvExpr) &&
        !dynamic_cast<const ast::MemberAccessExpr*>(&recvExpr))
        return;
    llvm::Type* slotTy = nullptr;
    if (llvm::Value* slot = lvalueAddr(recvExpr, slotTy))
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), slot);
}

// #1833 (b) / #1467 / §22.4 / §28.3: dispose every `out _` discard-owned
// scratch slot recorded by buildOrderedArgs in the `[base, end)` range,
// on the normal-return path (`!diverged_`). Byte-identical pattern to
// lowerCall's end-of-statement disposal pass — extracted here so lowerNew
// can drive its own recorded slots the same way. On the divergent path
// (the ctor threw) no scratch is written, so no slot reaches a non-null
// value: the unwind reclaims the heap block via the enclosing obj
// (emitNewPendingCheck / the inlineCtorBody cleanup), not through these
// scratch slots, so skipping them on `diverged_` is sound. The
// `resize(base)` trims the stack back so a NESTED discard inside an arg
// builds its own scope (the lowerCall-side discipline).
void Emitter::disposeNewDiscardOwnedOutSlots(std::size_t base) {
    if (diverged_) {
        discardOwnedOutSlots_.resize(base);
        return;
    }
    for (std::size_t di = base; di < discardOwnedOutSlots_.size(); ++di) {
        llvm::AllocaInst* slot = discardOwnedOutSlots_[di];
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), slot, "new.outdisc.obj");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(
            llvm_, "new.outdisc.dispose", currentFn_);
        auto* contBB = llvm::BasicBlock::Create(
            llvm_, "new.outdisc.cont", currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(obj);
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), slot);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
    discardOwnedOutSlots_.resize(base);
}


void Emitter::emitOwningChoiceActiveCaseDisposal(
        llvm::Value* slot, const sema::TypeSymbol* choiceTy,
        const sema::TypeRef* choiceRef) {
    // §35.4 (v0.200, #2045 stage 3) "Disposal is defined by the active
    // case." Reads the runtime TAG (field 0 of the choice struct), switches
    // on it, and for each case whose payload has at least one `owned`-marked
    // field, GEPs into that case's payload slot (field 1+caseIndex), extracts
    // each owned payload pointer, null-guards, and dispatches Dispose+reclaim
    // via the runtime vtable — the concrete class is the runtime payload
    // type, statically unknown at the choice site, so dynamic dispatch
    // (emitDisposeAndReclaim, which loads the vtable slot 2 and reclaims the
    // block) is the only safe site. Payload-free / value-payload cases
    // dispose nothing, so their switch arm is a direct branch to the join.
    //
    // WHY DYNAMIC DISPATCH (§35.4 + §8.6): the choice's payload type is the
    // STATIC field type — a class, an interface, or a generic parameter
    // (§28.1's per-instantiation erasure of `owned`). The runtime TYPE of
    // the payload is the concrete class the user stored (a subclass of the
    // declared type, or an interface's concrete implementation). The vtable
    // slot 2 holds the most-derived Dispose; emitDisposeAndReclaim loads it
    // and reclaims the block. This is the SAME path class-field auto-
    // disposal uses (§28.4 / CG18) — exactly the right invariant.
    //
    // WHY NO NULL-AFTER (yet): unlike a class field, the choice struct is a
    // value-typed slot on the stack. After dispose, the slot is dead (its
    // payloads are gone); a re-entrant dispose through the same slot would
    // read the same tag and re-dispatch on the (now-disposed) owned field,
    // which is freed memory. emitDisposals fires this at scope-end on a
    // single exit edge, so a second pass through the same Disposal entry is
    // structurally impossible (the slot goes out of scope). The null-after
    // pattern would matter for a heap-resident choice or for a nested
    // owning match (which is the v0.200 stage 4 follow-up) — keep that
    // for the future; the current invariant (single fire, fresh tag)
    // means the dispose is exactly-once.
    if (!slot || !choiceTy) return;
    // §14.1 / §28.1 (#2047): a generic owning choice (`Result<C, string>`) is
    // disposed from a NON-generic body (Main), so no substitution is active and
    // choiceStruct/lowerTypeRef would otherwise yield the i32-placeholder layout
    // (a tag/payload type mismatch → an ICmp assertion). Install the
    // instantiation's substitution so the specialized payload struct is used and
    // each owned field's instantiated type is decidable for §28.1 erasure.
    RecordSubstGuard choiceSubst(this, choiceRef);
    // fix #3144 (family F9 of #3152): the SYMBOL, not
    // `dynamic_cast<const ast::TypeDecl*>(choiceTy->decl)` — which is null for
    // every imported choice, so the §28.1 per-instantiation payload
    // substitution below silently did not run across a module boundary.
    const sema::TypeSymbol* choiceDecl = choiceTy;
    auto cases = choiceCasesOf(choiceTy);
    if (cases.empty()) return;
    // The choice struct is { i32 tag, case0_payload, case1_payload, … }
    // (see choiceStruct). The tag is field 0 (i32); the i-th case's payload
    // struct is field 1+i.
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return;
    llvm::Value* choiceVal = builder_.CreateLoad(st, slot, "choice.val");
    llvm::Value* tag = builder_.CreateExtractValue(
        choiceVal, {0}, "choice.tag");
    auto* joinBB =
        llvm::BasicBlock::Create(llvm_, "ocd.join", currentFn_);
    auto* switchBB =
        llvm::BasicBlock::Create(llvm_, "ocd.switch", currentFn_);
    builder_.CreateBr(switchBB);
    builder_.SetInsertPoint(switchBB);
    auto* sw = builder_.CreateSwitch(
        tag, joinBB, static_cast<unsigned>(cases.size()));
    // Walk every case in declaration order, building one switch arm per
    // case that has at least one owning payload field (an `owned`-marked
    // payload whose instantiated type is a class/interface, or a payload
    // whose type is itself an owning choice / move-only value type —
    // §35.4 "and, recursively, any move-only payloads").
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const sema::ChoiceCaseSymbol* cc = cases[i];
        if (!cc || !cc->caseDecl) continue;
        // fix #4132 / §35.4 (v0.300, "and, recursively, any move-only
        // payloads"): collect indices of every OWNING payload — either an
        // explicit `owned` payload whose instantiated type is a class/interface
        // (the vtable Dispose path below) OR a payload whose declared type is
        // itself an owning choice (the recursive per-tag disposal path). The
        // pre-#4132 walk only saw the `owned` marker, so a non-`owned` field
        // whose type was itself an owning choice (the rule-1b classification
        // trigger, `y: D<Widget>` in `Outer.B(y: D<Widget>)`) was silently
        // skipped — the case emitted zero owning fields, the switch arm was
        // omitted entirely, and the nested owning choice's own `owned` payload
        // was never disposed. The kind discriminator drives the disposal arm
        // below.
        std::vector<unsigned> owningFieldIdxs;
        std::vector<bool> fieldIsOwningChoice;
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                pf, choiceDecl, choiceRef, ctx_);
            // §35.4 rule 1a: `owned` + class/interface → vtable disposal.
            // §28.1 per-instantiation erasure (#2047): an `owned` field whose
            // instantiated type is value-like or runtime-managed (e.g.
            // `Error(owned error: string)` at `Result<C, string>`) does NOT
            // participate in vtable disposal — disposing it would dispatch a
            // Dispose through a non-object payload.
            if (pf.modifier == "owned") {
                if (common::ownedMarkerActiveOnInstantiatedType(ftInst, ctx_)) {
                    owningFieldIdxs.push_back(static_cast<unsigned>(j));
                    // `owned f: D<Widget>` — `owned` is admitted-but-redundant
                    // (§28.1) on an owning choice; route to the recursive path
                    // since the field IS the owning choice (not a class ref).
                    bool isChoice = ftInst && ftInst->kind ==
                        sema::TypeRef::Kind::Named && ftInst->declaration &&
                        ftInst->declaration->kind == ast::TypeDeclKind::Choice;
                    fieldIsOwningChoice.push_back(isChoice);
                }
                continue;
            }
            // §35.4 rule 1b: a non-`owned` payload whose TYPE is itself a
            // move-only value type (an owning inline array or — the issue's
            // shape — an owning choice) owns heap objects transitively and
            // must be disposed. Same kind test isMoveOnlyValueType uses
            // (OwnershipFacts.cpp:1840, #3775): owning choice or owning inline
            // array. Inline-array payload disposal is not yet wired (filed
            // separately); the recursive disposal is identical to a class
            // payload's — a temp alloca holding the choice struct, then the
            // recursive per-tag switch.
            const char* mvKind = nullptr;
            if (ftInst && common::isMoveOnlyValueType(ftInst, ctx_, mvKind)) {
                bool isChoice = ftInst->kind == sema::TypeRef::Kind::Named &&
                    ftInst->declaration &&
                    ftInst->declaration->kind == ast::TypeDeclKind::Choice;
                if (isChoice) {
                    owningFieldIdxs.push_back(static_cast<unsigned>(j));
                    fieldIsOwningChoice.push_back(true);
                }
                // Owning inline array in choice payload: deferred (#4132
                // covers the choice-in-choice case; a future issue tracks the
                // inline-array variant).
            }
        }
        if (owningFieldIdxs.empty()) continue;  // nothing to dispose — switch
                                                // arm collapses to join.
        auto* armBB =
            llvm::BasicBlock::Create(llvm_, "ocd.arm", currentFn_);
        sw->addCase(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_), i), armBB);
        builder_.SetInsertPoint(armBB);
        // GEP into the case's payload slot (field 1+i) and load the
        // payload struct value, then extract each owning field and dispose.
        llvm::Value* casePayload = builder_.CreateExtractValue(
            choiceVal, {1 + (unsigned)i}, "ocd.case.payload");
        for (std::size_t k = 0; k < owningFieldIdxs.size(); ++k) {
            unsigned j = owningFieldIdxs[k];
            bool isChoice = fieldIsOwningChoice[k];
            const ast::Param& pf = cc->caseDecl->payload[j];
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                pf, choiceDecl, choiceRef, ctx_);
            llvm::Value* fieldVal = builder_.CreateExtractValue(
                casePayload, {j}, "ocd.owned.field");
            if (isChoice) {
                // fix #4132 / §35.4 rule 1b: the nested choice's per-tag
                // disposal needs an AllocaInst slot to read the value from;
                // the field is a struct value embedded in casePayload, so
                // alloca a fresh slot of the choice struct's specialized
                // type, store the value, and recurse. The temp slot is dead
                // the moment the recursive disposal completes (no live
                // alias — the recursive switch reads the tag fresh each
                // pass), so no NULLing is needed across re-entry.
                //
                // §14.1 / §28.1 (#2047): under an OUTER substitution (this
                // emit's own RecordSubstGuard installed the OUTER choice's
                // mapping), choiceStruct(ftInst->declaration) would lay out
                // the bare choice with the OUTER's instantiation context —
                // e.g. `Outer`'s subst doesn't name `T`, so `D`'s `T`
                // falls back to the i32 placeholder (a too-small slot for
                // the 24-byte `D<Widget>` value we already extracted).
                // `store %D$spec0 into %D` then overruns the alloca's right
                // redzone by exactly 4 bytes — the measured ASan report.
                // Install ftInst's own subst for the alloca + the recursive
                // call (which would otherwise inherit the outer map).
                llvm::StructType* nestedSt = nullptr;
                {
                    RecordSubstGuard nestedSubst(this, ftInst);
                    nestedSt = choiceStruct(ftInst->declaration);
                    if (!nestedSt) continue;  // defensive — never fires
                                              // (substitutedChoicePayloadType
                                              // already proved it's a choice)
                    llvm::AllocaInst* nestedSlot =
                        builder_.CreateAlloca(
                            nestedSt, nullptr, "ocd.nested.slot");
                    builder_.CreateStore(fieldVal, nestedSlot);
                    emitOwningChoiceActiveCaseDisposal(
                        nestedSlot, ftInst->declaration, ftInst);
                }
                continue;
            }
            // Class/interface `owned` payload — the pre-#4132 path.
            // Null-guard: a never-bound arm of the active-case match could
            // leave an `owned`-payload slot null (the binding took it out);
            // or a payload-defaulted construction. `emitDisposeAndReclaim`
            // is null-safe (free(null) no-op) but the dispatch on a null
            // vtable would crash — guard explicitly.
            llvm::Value* isNull = builder_.CreateICmpEQ(
                fieldVal, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(
                llvm_, "ocd.dispose", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(
                llvm_, "ocd.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitDisposeAndReclaim(fieldVal);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        }
        builder_.CreateBr(joinBB);
    }
    builder_.SetInsertPoint(joinBB);
}


void Emitter::emitValueLikeChoiceActiveCaseStrRelease(
        llvm::Value* slot, const sema::TypeSymbol* choiceTy,
        const sema::TypeRef* choiceRef, bool retain) {
    // fix #2115 / §28.9 / §35.4: release the ACTIVE case's runtime-managed
    // (`string`) payload(s) of a VALUE-LIKE choice struct at scope end. The
    // choice struct is { i32 tag, case0_payload, case1_payload, … } (see
    // choiceStruct); a string payload field is a `ptr` slot holding a +1
    // `bz_string*` stored by the §35.3 construction path with no retain/release
    // tracking. This emit reads the runtime TAG (field 0), switches on it, and
    // for the active case releases each string-typed payload field via
    // emitStringRelease — the value-like analog of
    // emitOwningChoiceActiveCaseDisposal (which disposes `owned` heap payloads
    // via the vtable). bz_string_release is null-safe (a never-bound string?
    // slot) and a no-op on a sentinel/literal, so an arm whose string payload
    // was a literal releases nothing. Payload-free / non-string-payload cases
    // release nothing, so their switch arm collapses to the join.
    //
    // SOUNDNESS: the registration (armValueLikeChoiceStrRelease) guarantees the
    // local is non-escaping (StringEscapeWalker) AND the initializer is a choice
    // construction every string payload arg of which is a +1 producer or a
    // literal — so the slot provably owns exactly +1 on the active case's
    // strings, and no copy/alias/release-site exists to double-free. Fires on a
    // SINGLE exit edge (the slot goes out of scope), so like the owning-choice
    // active-case disposal the release is exactly-once per exit. A reassigned
    // local is excluded at registration (no release-old-on-reassign machinery —
    // reassignment is a conservative escape), so the active case here is always
    // the constructed case and its string slots are the +1 the walk balances.
    // // per §28.9 (string refcount discipline), §35.4 v0.226.
    if (!slot || !choiceTy) return;
    // §14.1 / §28.1 (#2047): a generic value-like choice (`Result<int, string>`)
    // is released from a non-generic body, so install the instantiation's
    // substitution → the specialized payload struct (a tag/payload type mismatch
    // would otherwise fail an ICmp assertion). Same guard the owning-choice emit
    // and choiceStruct consult.
    RecordSubstGuard choiceSubst(this, choiceRef);
    // fix #3144 (family F9 of #3152): the SYMBOL, not
    // `dynamic_cast<const ast::TypeDecl*>(choiceTy->decl)` — which is null for
    // every imported choice, so the §28.1 per-instantiation payload
    // substitution below silently did not run across a module boundary.
    const sema::TypeSymbol* choiceDecl = choiceTy;
    auto cases = choiceCasesOf(choiceTy);
    if (cases.empty()) return;
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return;
    // Only emit the switch if at least one case has a string payload field;
    // otherwise every arm collapses to the join and there is nothing to release.
    auto caseHasStringPayload = [&](const sema::ChoiceCaseSymbol* cc) -> bool {
        if (!cc || !cc->caseDecl) return false;
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (!pf.type) continue;
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                pf, choiceDecl, choiceRef, ctx_);
            if (ftInst &&
                ftInst->kind == sema::TypeRef::Kind::Primitive &&
                ftInst->primitive == sema::PrimitiveType::String)
                return true;
        }
        return false;
    };
    bool any = false;
    for (auto* cc : cases)
        if (caseHasStringPayload(cc)) { any = true; break; }
    if (!any) return;
    llvm::Value* choiceVal = builder_.CreateLoad(st, slot, "vlchoice.val");
    llvm::Value* tag = builder_.CreateExtractValue(choiceVal, {0}, "vlchoice.tag");
    auto* joinBB =
        llvm::BasicBlock::Create(llvm_, "vlcsr.join", currentFn_);
    auto* switchBB =
        llvm::BasicBlock::Create(llvm_, "vlcsr.switch", currentFn_);
    builder_.CreateBr(switchBB);
    builder_.SetInsertPoint(switchBB);
    auto* sw = builder_.CreateSwitch(
        tag, joinBB, static_cast<unsigned>(cases.size()));
    // Walk every case in declaration order; one switch arm per case that has at
    // least one string payload field. The string payload slot is released in
    // payload declaration order (§35.4 "in payload declaration order" — the same
    // ordering the owning-choice active-case disposal uses).
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const sema::ChoiceCaseSymbol* cc = cases[i];
        if (!cc || !cc->caseDecl) continue;
        // Collect indices of this case's STRING payload fields (instantiated
        // type). Non-string fields are ignored — only runtime-managed string
        // slots carry the +1 the construction mints.
        std::vector<unsigned> strFieldIdxs;
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (!pf.type) continue;
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                pf, choiceDecl, choiceRef, ctx_);
            if (ftInst &&
                ftInst->kind == sema::TypeRef::Kind::Primitive &&
                ftInst->primitive == sema::PrimitiveType::String) {
                strFieldIdxs.push_back(static_cast<unsigned>(j));
            }
        }
        if (strFieldIdxs.empty()) continue;  // nothing to release — arm
                                             // collapses to join.
        auto* armBB =
            llvm::BasicBlock::Create(llvm_, "vlcsr.arm", currentFn_);
        sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_), i),
                    armBB);
        builder_.SetInsertPoint(armBB);
        // GEP into the case's payload slot (field 1+i) and extract each string
        // field. The payload slot is field 1+i of the choice struct; field j of
        // that payload is the string pointer.
        llvm::Value* casePayload = builder_.CreateExtractValue(
            choiceVal, {1 + (unsigned)i}, "vlcsr.case.payload");
        for (unsigned j : strFieldIdxs) {
            llvm::Value* strPtr = builder_.CreateExtractValue(
                casePayload, {j}, "vlcsr.str.field");
            // bz_string_release is null-safe (a never-bound string? slot) and a
            // no-op on a sentinel/literal payload — release unconditionally.
            if (retain)
                emitStringRetain(strPtr);  // fix #6170 step 2
            else
                emitStringRelease(strPtr);
        }
        builder_.CreateBr(joinBB);
    }
    builder_.SetInsertPoint(joinBB);
}


// fix #2425 / §9.8 / §28.9 / §35.4 v0.235: does value-like choice `ts` own,
// directly or transitively (through a non-nullable nested-record or
// nested-choice payload field), at least one `string`? See the declaration
// in LlvmBackend_impl.hpp for the full termination argument (§35.1's choice
// self-containment check makes the record+choice value-type graph a finite
// DAG). GENERIC choices are excluded (returns false unconditionally): a
// generic choice's payload struct layout is per-instantiation
// (choiceStruct's #2047 specialization), and neither this classifier nor
// emitChoiceStringFieldReleases_impl below installs a RecordSubstGuard for
// a NESTED generic choice/record reached mid-recursion — the same narrow,
// documented scope #2405's record recursion already has (it does not
// thread generic substitution through nested levels either). A generic
// value-like choice component/payload is therefore a residual gap, not a
// new one this fix introduces — filed if found in a live shape (none of
// this issue's own repros are generic).
bool Emitter::choiceHasStringFields(const sema::TypeSymbol* ts) {
    if (!ts || ts->kind != ast::TypeDeclKind::Choice) return false;
    if (!ts->genericParams.empty()) return false;
    for (const sema::ChoiceCaseSymbol* cc : choiceCasesOf(ts)) {
        if (!cc || !cc->caseDecl) continue;
        for (const ast::Param& pf : cc->caseDecl->payload) {
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft) continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String)
                return true;
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): pass `ft` itself
            // (not `ft->declaration`) — a choice payload field may be a
            // CONCRETE generic-record instantiation (`Val(B: Box<string>)`)
            // even when the enclosing choice is not generic; recordHasStringFields
            // now needs the full TypeRef to install the substitution for
            // such a field's own bare-type-parameter members.
            if (ft->declaration->kind == ast::TypeDeclKind::Record &&
                recordHasStringFields(ft))
                return true;
            if (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                choiceHasStringFields(ft->declaration))
                return true;
        }
    }
    return false;
}


// fix #2425 / §9.8 / §28.3 / §28.9 / §35.4 v0.235: release the ACTIVE case's
// `string`-owning payload(s) of value-like choice `choiceTy` at struct
// ADDRESS `choiceAddr`, recursing into a non-nullable nested-record or
// nested-choice payload field to ARBITRARY depth. This is the choice-side
// counterpart to emitRecordStringFieldReleases_impl's #2405 arbitrary-depth
// recursion, and the address-based (GEP, not load+extractvalue) analog of
// #2115's emitValueLikeChoiceActiveCaseStrRelease above — that function is
// VALUE-based (loads the whole struct once, ExtractValue per field) and
// stops at ONE level (direct string payload fields only); this one is
// ADDRESS-based so a nested record/choice payload's OWN storage can be
// reached by a further GEP off the same base, exactly as
// emitRecordStringFieldReleases_impl reaches a nested record. The two
// functions are NOT merged: this one is used only by the #2398/#2425
// deserialize disposal-scope registration (LlvmBackend_Serialization.cpp),
// which already holds an ADDRESS (an alloca'd component/payload slot); the
// #2115 mechanism is used by the scope-end LOCAL release
// (LlvmBackend_Setup.cpp), which arms off a VALUE-level escape analysis
// unrelated to this one. See choiceHasStringFields just above for the
// termination argument and the (documented, narrow) generic exclusion this
// function shares via that gate.
//
// A NULLABLE nested-record/nested-choice payload field is excluded, matching
// emitRecordStringFieldReleases_impl's own exclusion: it lowers to a
// `{i8, T}` wrapper struct, not the bare layout this walker assumes.
void Emitter::emitChoiceStringFieldReleases_impl(
        llvm::Value* choiceAddr, const sema::TypeSymbol* choiceTy) {
    if (!choiceAddr || !choiceTy || choiceTy->kind != ast::TypeDeclKind::Choice)
        return;
    if (!choiceHasStringFields(choiceTy)) return;  // also excludes generics
    auto cases = choiceCasesOf(choiceTy);
    if (cases.empty()) return;
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return;
    llvm::Value* tagAddr =
        builder_.CreateStructGEP(st, choiceAddr, 0, "chsfr.tag.addr");
    llvm::Value* tag = builder_.CreateLoad(i32(), tagAddr, "chsfr.tag");
    auto* joinBB = llvm::BasicBlock::Create(llvm_, "chsfr.join", currentFn_);
    auto* switchBB = llvm::BasicBlock::Create(llvm_, "chsfr.switch", currentFn_);
    builder_.CreateBr(switchBB);
    builder_.SetInsertPoint(switchBB);
    auto* sw = builder_.CreateSwitch(
        tag, joinBB, static_cast<unsigned>(cases.size()));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const sema::ChoiceCaseSymbol* cc = cases[i];
        if (!cc || !cc->caseDecl) continue;
        // Does THIS case own a string, directly or transitively? Skip the
        // arm entirely (collapses to the join) if not — mirrors #2115's own
        // per-case caseHasStringPayload guard.
        bool caseOwnsString = false;
        for (const ast::Param& pf : cc->caseDecl->payload) {
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft) continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String) {
                caseOwnsString = true;
                break;
            }
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): `ft`, not
            // `ft->declaration` — see the identical reasoning above.
            if ((ft->declaration->kind == ast::TypeDeclKind::Record &&
                 recordHasStringFields(ft)) ||
                (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                 choiceHasStringFields(ft->declaration))) {
                caseOwnsString = true;
                break;
            }
        }
        if (!caseOwnsString) continue;
        auto* armBB = llvm::BasicBlock::Create(llvm_, "chsfr.arm", currentFn_);
        sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_), i),
                    armBB);
        builder_.SetInsertPoint(armBB);
        // GEP into the case's payload slot (field 1+i) — embedded inline in
        // the SAME outer struct, exactly like a record's nested-record
        // member (§35.1: a choice's case payloads are value-typed storage).
        llvm::Value* payloadAddr = builder_.CreateStructGEP(
            st, choiceAddr, 1 + (unsigned)i, "chsfr.case.addr");
        llvm::Type* payloadTy = st->getElementType(1 + (unsigned)i);
        auto* payloadStruct = llvm::dyn_cast<llvm::StructType>(payloadTy);
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft || !payloadStruct || j >= payloadStruct->getNumElements())
                continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String) {
                llvm::Value* fAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j, "chsfr.str.addr");
                llvm::Value* fVal =
                    builder_.CreateLoad(ptrTy(), fAddr, "chsfr.str.val");
                emitStringRelease(fVal);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), fAddr);
                continue;
            }
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): `ft`, not
            // `ft->declaration` — see the choiceHasStringFields comment
            // above (a choice payload field may be a concrete generic-
            // record instantiation even when the choice itself is not).
            if (ft->declaration->kind == ast::TypeDeclKind::Record &&
                recordHasStringFields(ft)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j,
                    "chsfr.nested.rec.addr");
                emitRecordStringFieldReleases_impl(nestedAddr, ft);
                continue;
            }
            if (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                choiceHasStringFields(ft->declaration)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j,
                    "chsfr.nested.ch.addr");
                emitChoiceStringFieldReleases_impl(nestedAddr, ft->declaration);
            }
        }
        builder_.CreateBr(joinBB);
    }
    builder_.SetInsertPoint(joinBB);
}


// fix #3974 (§9.7 / §28.9 / stdlib §4.8, v0.294): box-mint-side counterpart
// to emitChoiceStringFieldReleases_impl. Same tag-switched skeleton (one
// arm per case, collapsed to the join when the case owns no string),
// same `caseOwnsString` per-case guard, same per-payload walk. The
// per-string field path is `load → emitStringRetain` (no nulling — the
// box now owns the slot, and the box's `free` at disposal time drops
// the rc to zero). The nested record/choice arm recurses into the
// respective retain walker. See emitRecordStringFieldRetains_impl's
// header for the rationale and the boxValueNeedsStringRetain dispatch
// that decides whether this walker is reached at all.
void Emitter::emitChoiceStringFieldRetains_impl(
        llvm::Value* choiceAddr, const sema::TypeSymbol* choiceTy) {
    if (!choiceAddr || !choiceTy || choiceTy->kind != ast::TypeDeclKind::Choice)
        return;
    if (!choiceHasStringFields(choiceTy)) return;  // also excludes generics
    auto cases = choiceCasesOf(choiceTy);
    if (cases.empty()) return;
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return;
    llvm::Value* tagAddr =
        builder_.CreateStructGEP(st, choiceAddr, 0, "chsft.tag.addr");
    llvm::Value* tag = builder_.CreateLoad(i32(), tagAddr, "chsft.tag");
    auto* joinBB = llvm::BasicBlock::Create(llvm_, "chsft.join", currentFn_);
    auto* switchBB = llvm::BasicBlock::Create(llvm_, "chsft.switch", currentFn_);
    builder_.CreateBr(switchBB);
    builder_.SetInsertPoint(switchBB);
    auto* sw = builder_.CreateSwitch(
        tag, joinBB, static_cast<unsigned>(cases.size()));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const sema::ChoiceCaseSymbol* cc = cases[i];
        if (!cc || !cc->caseDecl) continue;
        // Does THIS case own a string, directly or transitively? Skip the
        // arm entirely (collapses to the join) if not — mirrors the
        // release walker's identical guard.
        bool caseOwnsString = false;
        for (const ast::Param& pf : cc->caseDecl->payload) {
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft) continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String) {
                caseOwnsString = true;
                break;
            }
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            if ((ft->declaration->kind == ast::TypeDeclKind::Record &&
                 recordHasStringFields(ft)) ||
                (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                 choiceHasStringFields(ft->declaration))) {
                caseOwnsString = true;
                break;
            }
        }
        if (!caseOwnsString) continue;
        auto* armBB = llvm::BasicBlock::Create(llvm_, "chsft.arm", currentFn_);
        sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_), i),
                    armBB);
        builder_.SetInsertPoint(armBB);
        llvm::Value* payloadAddr = builder_.CreateStructGEP(
            st, choiceAddr, 1 + (unsigned)i, "chsft.case.addr");
        llvm::Type* payloadTy = st->getElementType(1 + (unsigned)i);
        auto* payloadStruct = llvm::dyn_cast<llvm::StructType>(payloadTy);
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft || !payloadStruct || j >= payloadStruct->getNumElements())
                continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String) {
                llvm::Value* fAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j, "chsft.str.addr");
                llvm::Value* fVal =
                    builder_.CreateLoad(ptrTy(), fAddr, "chsft.str.val");
                emitStringRetain(fVal);
                continue;
            }
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            if (ft->declaration->kind == ast::TypeDeclKind::Record &&
                recordHasStringFields(ft)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j,
                    "chsft.nested.rec.addr");
                emitRecordStringFieldRetains_impl(nestedAddr, ft);
                continue;
            }
            if (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                choiceHasStringFields(ft->declaration)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j,
                    "chsft.nested.ch.addr");
                emitChoiceStringFieldRetains_impl(nestedAddr, ft->declaration);
            }
        }
        builder_.CreateBr(joinBB);
    }
    builder_.SetInsertPoint(joinBB);
}


// fix #2326 / §9.8 v0.228: does the (statically-resolved) element type `tr`
// of a non-nullable inline array carry a string — directly (a `string`
// element), or transitively (through a non-nullable nested record / choice /
// inline-array element)? Mirrors the per-field arm of recordHasStringFields
// (LlvmBackend_impl.hpp) in shape: walk the element type one layer, and
// recurse into the named record / choice / inline-array cases. The
// element-type recursion is finite for the same reason as the record /
// choice walks: sema rejects a self-containing value-type graph at the
// declaration site (recordValueContainsSelf / checkChoiceSelfContainment
// in TypeResolution_Checks.cpp), so the reachable type graph is a finite
// DAG. A nullable element type is treated as string-free: a `string?` field
// in a record lowers to a {i8, ptr} wrapper that the existing nullable
// exclusion already handles, and the inline-array shape carries no parallel
// obligation in the spec.
//
// Generic named element types are excluded by their own gate (declaration
// null at this layer) — the same narrow, documented scope
// choiceHasStringFields above has, mirroring #2405's record recursion. A
// generic value-type element is therefore a residual gap, not a new one
// this fix introduces; the live repros of this issue are all concrete.
bool Emitter::inlineArrayElementCarriesString(const sema::TypeRef* tr) {
    if (!tr || tr->nullable) return false;
    using K = sema::TypeRef::Kind;
    if (tr->kind == K::Primitive)
        return tr->primitive == sema::PrimitiveType::String;
    if (tr->kind == K::InlineArray)
        return tr->arrayElement &&
               inlineArrayElementCarriesString(tr->arrayElement);
    if (tr->kind == K::Named && tr->declaration) {
        // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): `tr`, not
        // `tr->declaration` — a CONCRETE generic-record instantiation as an
        // inline-array element (`[N]Box<string>`) needs the substitution;
        // a genuinely BARE type-parameter element (`[N]T`) still falls to
        // the `!tr->declaration` early return above, unchanged (the
        // documented residual gap the comment above this function notes).
        if (tr->declaration->kind == ast::TypeDeclKind::Record)
            return recordHasStringFields(tr);
        if (tr->declaration->kind == ast::TypeDeclKind::Choice)
            return choiceHasStringFields(tr->declaration);
        return false;  // enum: integer representation, no string to release
    }
    return false;
}


// fix #5882 (§3.6 / §8.3, v0.338; #6238): see the declaration. Termination is
// the same finite value-type DAG argument as inlineArrayElementCarriesString.
bool Emitter::inlineArrayElementIsPlainValue(const sema::TypeRef* tr) {
    if (!tr) return false;
    using K = sema::TypeRef::Kind;
    if (tr->kind == K::Primitive)
        return tr->primitive != sema::PrimitiveType::String &&
               tr->primitive != sema::PrimitiveType::Object;
    if (tr->kind == K::InlineArray)
        return !tr->arrayOwned && tr->arrayElement &&
               inlineArrayElementIsPlainValue(tr->arrayElement);
    if (tr->kind != K::Named || !tr->declaration) return false;
    // A class / interface element is a borrowed reference compared by
    // identity (§8.3); the key copy holds it exactly as a `[N]C` local does,
    // with no count to keep.
    if (tr->declaration->kind == ast::TypeDeclKind::Enum ||
        tr->declaration->kind == ast::TypeDeclKind::Class ||
        tr->declaration->kind == ast::TypeDeclKind::Interface)
        return true;
    if (tr->declaration->kind != ast::TypeDeclKind::Record) return false;
    RecordSubstGuard guard(this, tr);
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(tr->declaration, storage);
    for (const sema::Symbol* m : storage) {
        const sema::TypeRef* mty = recordMemberTypeResolved(m);
        if (mty == tr || !inlineArrayElementIsPlainValue(mty)) return false;
    }
    return true;
}

// fix #2326 / §9.8 v0.228: release every `string` storage reachable from
// inline-array element type at array ADDRESS `arrAddr` for the array
// type `arrayTy` (the OUTER inline-array TypeRef). Parallels
// emitInlineArrayEquality (LlvmBackend_Layout.cpp, §8.3 / §15.1.2): N is
// compile-time-constant (`arraySizeValue`, folded by sema per §15.1.2),
// so the walk is unrolled. Per-element dispatch mirrors
// emitRecordStringFieldReleases_impl's nested-field dispatch: a `string`
// element is bz_string_release'd and the slot nulled (matching the field
// nulling in the record walker); a non-nullable record element recurses
// via emitRecordStringFieldReleases_impl; a non-nullable choice element
// recurses via emitChoiceStringFieldReleases_impl; a non-nullable nested
// inline-array element recurses into itself. The element-type recursion
// terminates for the same reason as the predicate's: a finite DAG over
// the value-type graph.
//
// A NULLABLE element is excluded by the same reasoning the choice /
// record walkers use: it lowers to a {i8, T} wrapper, not the bare layout
// this walker assumes. A nullable element on a non-nullable array (e.g.
// `[N]string?`) is therefore a separate concern: the wire path stores an
// `i8` discriminator alongside each element; the inline-array
// serialization lane currently rejects nullable element types
// (isSerializationEligibleType's own gate, see TypeResolution_Checks.cpp),
// so a nullable-element inline array is not a live shape here.
//
// Takes the OUTER array TypeRef (not just the element type) so the
// per-element loop can derive N from `arraySizeValue` directly. Modern
// LLVM opaque pointers would otherwise force a `getElementType` on the
// pointee (which may be a primitive ptr, not a PointerType).
//
// Used by:
//   * emitRecordStringFieldReleases_impl (LlvmBackend_impl.hpp), to
//     recurse into a non-nullable inline-array field of a record;
//   * the #2326 deserialize-disposal-scope registration
//     (LlvmBackend_Serialization.cpp), to release the strings on a
//     non-nullable inline-array component / payload slot against a
//     LATER sibling's read throwing — its own entry, parallel to the
//     recordStringFields / choiceStringFields entries.
void Emitter::emitInlineArrayStringReleases_impl(
        llvm::Value* arrAddr, const sema::TypeRef* arrayTy) {
    if (!arrAddr || !arrayTy) return;
    // The element type is the array's `arrayElement`; N is `arraySizeValue`
    // (§15.1.2, folded by sema). §15.1 forbids an inline array of inline
    // arrays in practice, so a NESTED `[M][N]T` is never live here — the
    // recursion below handles only the defensive "what if §15.1 changes"
    // case, and `arrayElement` will be a string-bearing non-Array type in
    // every actual live shape.
    const sema::TypeRef* elemTy = arrayTy->arrayElement;
    int n_signed = arrayTy->arraySizeValue;
    if (!elemTy || n_signed < 0) return;
    if (elemTy->nullable) return;
    // Guard with the same predicate: if the element type carries no
    // string, there is nothing to release (the caller only ever invokes
    // this when the gate was true, but defense-in-depth matches the
    // emitChoiceStringFieldReleases_impl pattern).
    if (!inlineArrayElementCarriesString(elemTy)) return;
    unsigned n = static_cast<unsigned>(n_signed);

    llvm::Type* elemLlvmTy = lowerTypeRef(elemTy);
    if (!elemLlvmTy) return;
    auto* arrLlvmTy = llvm::ArrayType::get(elemLlvmTy, n);
    for (unsigned i = 0; i < n; ++i) {
        llvm::Value* slotAddr = builder_.CreateConstGEP2_64(
            arrLlvmTy, arrAddr, 0, i, "inlarr.elem.addr");
        if (elemTy->kind == sema::TypeRef::Kind::Primitive &&
            elemTy->primitive == sema::PrimitiveType::String) {
            llvm::Value* v =
                builder_.CreateLoad(ptrTy(), slotAddr, "inlarr.str.val");
            emitStringRelease(v);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 slotAddr);
            continue;
        }
        if (elemTy->kind == sema::TypeRef::Kind::InlineArray) {
            // Nested inline array: recurse into the sub-element type
            // against the sub-array's address. §15.1 forbids this in
            // practice (an inline array may not be the element of
            // another inline array) — left in place defensively in case
            // §15.1 ever changes; the recursion is finite.
            emitInlineArrayStringReleases_impl(slotAddr, elemTy);
            continue;
        }
        if (elemTy->nullable || elemTy->kind != sema::TypeRef::Kind::Named ||
            !elemTy->declaration)
            continue;
        // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): `elemTy`, not
        // `elemTy->declaration` — a concrete generic-record inline-array
        // element (`[N]Box<string>`) needs the substitution installed.
        if (elemTy->declaration->kind == ast::TypeDeclKind::Record &&
            recordHasStringFields(elemTy)) {
            emitRecordStringFieldReleases_impl(slotAddr, elemTy);
            continue;
        }
        if (elemTy->declaration->kind == ast::TypeDeclKind::Choice &&
            choiceHasStringFields(elemTy->declaration)) {
            emitChoiceStringFieldReleases_impl(slotAddr, elemTy->declaration);
        }
    }
}


// fix #3974 (§9.7 / §28.9 / stdlib §4.8, v0.294): box-mint-side counterpart
// to emitInlineArrayStringReleases_impl — walks N unrolled slots and
// RETAINS each string-typed slot (or recurses into a nested record/choice/
// inline-array element). Mirrors the release walker exactly in shape: same
// `inlineArrayElementCarriesString` guard, same dispatch on each slot's
// static type, same `recordSubst` guard via the outer `RecordSubstGuard`
// installed at the call site. The retain is `load → emitStringRetain` — no
// nulling, since the box now owns the slot with a higher rc and the box's
// `free` at disposal time drops the rc to zero (the source's own +1
// remains, released at its scope end). See
// emitRecordStringFieldRetains_impl's header for the rationale and the
// boxValueNeedsStringRetain dispatch.
void Emitter::emitInlineArrayStringRetains_impl(
        llvm::Value* arrAddr, const sema::TypeRef* arrayTy) {
    if (!arrAddr || !arrayTy) return;
    const sema::TypeRef* elemTy = arrayTy->arrayElement;
    int n_signed = arrayTy->arraySizeValue;
    if (!elemTy || n_signed < 0) return;
    if (elemTy->nullable) return;
    if (!inlineArrayElementCarriesString(elemTy)) return;
    unsigned n = static_cast<unsigned>(n_signed);
    llvm::Type* elemLlvmTy = lowerTypeRef(elemTy);
    if (!elemLlvmTy) return;
    auto* arrLlvmTy = llvm::ArrayType::get(elemLlvmTy, n);
    for (unsigned i = 0; i < n; ++i) {
        llvm::Value* slotAddr = builder_.CreateConstGEP2_64(
            arrLlvmTy, arrAddr, 0, i, "inlarr.ret.elem.addr");
        if (elemTy->kind == sema::TypeRef::Kind::Primitive &&
            elemTy->primitive == sema::PrimitiveType::String) {
            llvm::Value* v =
                builder_.CreateLoad(ptrTy(), slotAddr, "inlarr.ret.str.val");
            emitStringRetain(v);
            continue;
        }
        if (elemTy->kind == sema::TypeRef::Kind::InlineArray) {
            // Same defensive recursion as the release twin (§15.1 forbids
            // this in practice; left in place in case §15.1 ever changes).
            emitInlineArrayStringRetains_impl(slotAddr, elemTy);
            continue;
        }
        if (elemTy->nullable || elemTy->kind != sema::TypeRef::Kind::Named ||
            !elemTy->declaration)
            continue;
        if (elemTy->declaration->kind == ast::TypeDeclKind::Record &&
            recordHasStringFields(elemTy)) {
            emitRecordStringFieldRetains_impl(slotAddr, elemTy);
            continue;
        }
        if (elemTy->declaration->kind == ast::TypeDeclKind::Choice &&
            choiceHasStringFields(elemTy->declaration)) {
            emitChoiceStringFieldRetains_impl(slotAddr, elemTy->declaration);
        }
    }
}


bool Emitter::freshNewReceiverTempEligible(const ast::Expr* recvExpr,
                                           llvm::Value* recvPtr,
                                           const sema::TypeRef* resultTy,
                                           bool calleeReturnsOwned) const {
    // #1039 / #1053 / §28.3: a fresh owned-class producer used ONLY as the
    // receiver of a member access or method call — never bound to a variable —
    // is owned by nothing and leaks. Dispose it after the value result has been
    // extracted, when the receiver is a fresh `new`/`owned`-returning producer
    // (#1039/#1053-case-2) and the result cannot alias into it.
    if (!recvExpr || !recvPtr || !resultTy) return false;
    if (!isFreshOwnedClassProducer(recvExpr)) return false;
    // The extracted result must be a NON-REFERENCE value — provably free of any
    // reference or borrow into the receiver: a numeric/bool/char/decimal
    // primitive, an enum, or unit. EXCLUDED, because each may alias into the
    // receiver: `string` and `object` (reference-typed primitives — a field /
    // method result borrows the receiver's refcounted string or sub-object); a
    // struct (could embed a borrow of a receiver sub-object); any class /
    // interface result (a reference). Excluding these frees the temporary ONLY
    // when nothing aliases it. (Bezel's borrow checker guarantees an instance
    // method cannot escape `this` past the call when its result is such a
    // value, so post-call disposal is sound — §15.3.)
    bool valueResult = false;
    if (resultTy->kind == sema::TypeRef::Kind::Unit) {
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Primitive &&
               resultTy->primitive != sema::PrimitiveType::None &&
               resultTy->primitive != sema::PrimitiveType::String &&
               resultTy->primitive != sema::PrimitiveType::Object) {
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Named &&
               resultTy->declaration) {
        // fix #3142 (§2.8.3 rule 4, v0.252): read the kind off the TypeSymbol,
        // for BOTH arms below. Through `->decl` it was null for every imported
        // type, so neither the enum arm nor the #1859 owned-class arm ever fired
        // for an imported result: the fresh receiver temp was not eligible for
        // the §15.3 / §28.3 post-call disposal this predicate gates, and leaked.
        // Converting only one arm would leave the other reading a stale source —
        // the partial-unification failure the audit warns about (#1462).
        const ast::TypeDeclKind rk = resultTy->declaration->kind;
        if (rk == ast::TypeDeclKind::Enum)
            valueResult = true;
        // fix #1859 / §28.3: a CLASS/INTERFACE result under an ANNOUNCED
        // `owned` return type is a FRESH transfer, not a borrow of the
        // receiver — the declared return type IS the announcement (K1.5 +
        // K3.13, "Fresh productions are owned: … owned-returning calls").
        // The ordinary (unmarked) instance-method-return row is the one that
        // aliases the receiver ("Borrowed — instance method: source is the
        // receiver") and stays excluded (calleeReturnsOwned is false there).
        // An owned return could, in principle, construct a fresh object
        // whose OWN field stores a borrow of the receiver (the admitted
        // `_owner`-back-reference field-store shape, §28.6) — that is the
        // SAME residual-trust hazard the language already accepts at every
        // other field-store site (§28.6's "honest limit of the static
        // rule", backstopped by checked-disposal mode's DisposedAccess-
        // Exception), not a new one this fix introduces: the receiver was
        // ALREADY due for disposal at the end of the full expression per
        // §28.3's unbound-temporary rule (its only use was as this call's
        // receiver — a borrowing use), independent of what the callee's
        // body happens to do internally.
        else if ((rk == ast::TypeDeclKind::Class ||
                  rk == ast::TypeDeclKind::Interface) &&
                 calleeReturnsOwned) {
            valueResult = true;
        }
    } else if (resultTy->kind == sema::TypeRef::Kind::Named &&
               !resultTy->declaration && calleeReturnsOwned) {
        // fix #2205 / §28.3: an owning-collection / owning-wrapper result
        // (decl-null builtin, e.g. OwningArray<T>, OwningDictionary<K,V>,
        // OwningList<T>, Shared<T>, …) under an ANNOUNCED `owned` return is
        // likewise a FRESH transfer, not a borrow of the receiver — same
        // K1.5/K3.13 reasoning as #1859's class/interface case, just
        // expressed through the name-head instead of an AST TypeDecl (the
        // owning types are decl-null in the type system). The ordinary
        // (unmarked) instance-method-return row of an owning return is
        // itself an owned producer per §28.3's producer table, and the
        // receiver was ALREADY due for disposal at the end of the full
        // expression per §28.3's unbound-temporary rule.
        //
        // fix #2261 / §28.3 / §29.1 / §30.1: NARROW the #2205 admission to
        // EXCLUDE the deferred-activation heads — `OwningTask` (the Task-
        // family intersection of `isOwningTypeHead`) and the four
        // `IOwning*` iterable-interface heads (the iterable-family
        // intersection of `isOwningTypeHead`). These are exactly the heads
        // §28.3's *Calls mint for the mint-announcing families* row 2
        // calls out as "the produced frame still **carries** the call's
        // receiver's and plain arguments' borrows until settlement"
        // (§29.1 for iterator frames, settlement = completion or disposal;
        // §30.1 for async tasks, settlement = `Task.Wait` completion). The
        // same #2205 K1.5/K3.13 reasoning DOES apply to the
        // element-owning collection heads and to the non-deferred owning
        // wrappers (Shared, Weak, ChannelSender, CancellationTokenSource)
        // — those heads do NOT carry the receiver's borrow into a frame
        // that outlives the call, and stay admitted. The element-owning
        // collections are also independently listed by
        // `isOwningCollectionHead` so the precise-exclusion form
        //   isOwningCollectionHead(h) || (isOwningTypeHead(h) &&
        //                                !isTaskFamilyHead(h) &&
        //                                !isIterableInterfaceHead(h))
        // matches them either way. The intersection of the excluded sets
        // with `isOwningTypeHead` is exactly `OwningTask` + the four
        // `IOwning*` heads (plain `Task` is in `isTaskFamilyHead` but not
        // in `isOwningTypeHead`; plain `IEnumerable<T>` etc. are in
        // `isIterableInterfaceHead` but not in `isOwningTypeHead`).
        std::string_view head = resultTy->headName();
        if (common::isOwningCollectionHead(head) ||
            (common::isOwningTypeHead(head) &&
             !common::isTaskFamilyHead(head) &&
             !common::isIterableInterfaceHead(head))) {
            valueResult = true;
        }
    }
    return valueResult;
}


// fix #2816 / §28.3 "Unbound owned temporaries" (v0.240) — the EXCEPTION-PATH
// half of the #1039 family. See the declaration comment (LlvmBackend_impl.hpp)
// for the mechanism; this is the soundness argument.
//
// WHY A REGISTRATION IS OWED. §28.3, immediately after the sentence #1039/#2751/
// #2814 implement: "When the full expression is instead exited by an
// **exception** — an operand or argument evaluated later throws before the
// temporary's transfer or consumption point — every already-produced,
// not-yet-consumed unbound owned temporary is disposed during unwinding by the
// same implicit `try`/`finally` mechanism as named locals … each temporary's
// disposal is established at its production, so the pass runs in reverse order
// of production on the establishment-ordered cleanup stack". The whole
// disposeFreshNewReceiverTemp family emitted its reclaim INLINE after the
// accessor call, in the `emitPendingCheck` continuation — so a getter/indexer/
// method that threw skipped it and the receiver leaked (#2816: MEASURED live
// count 20 for 20 throwing iterations, against 0 for 20 normal ones).
//
// WHY IT CANNOT DOUBLE-DISPOSE. The entry is `unwindOnly`, so emitDisposals
// skips it on every NORMAL exit; and it is DISCHARGED AT RUNTIME by the reclaim,
// which stores null into the slot before freeing the block (the same
// null-guarded discharge #1918 uses for a transferred owned local, and for the
// same reason it gives there: "nulls at runtime instead of erasing the
// registration"). Every unwindOnly disposal loads the slot, skips a null, and
// nulls it again after disposing — so on any concrete execution the block is
// disposed by exactly one of {the reclaim, the first unwind pass that reaches a
// non-null slot}, never both. Registration and reclaim share ONE eligibility
// predicate (freshNewReceiverTempEligible), so the two can never disagree about
// whether a given temporary is covered.
//
// WHY THE DISCHARGE IS RUNTIME AND NOT AN ERASURE OF THE ENTRY. An erasure at
// the reclaim would be cheaper IR, and was the first draft — but an erasure is a
// COMPILE-TIME removal, so it only covers disposal passes emitted after it, and
// this entry's passes are not all emitted in program order. When #2816 measured
// it, the shape that broke was try-WITH-`finally`: lowerTry then pushed the try
// body's disposal scope below `unwindScopeDepth_` (#1263), deferring its unwind
// pass to `finUnwindBB` — emitted after the whole body, hence after the reclaim
// had erased the entry — and the erasing draft leaked all 20 receivers on that
// spelling while every non-`finally` spelling was clean. That particular
// deferral is gone (#2892, §28.10 v0.241: the finally is established at
// protected-block entry, so the body scope is unwind-disposed at the throw
// site), but the argument for the runtime null is not: it is edge-order-
// independent, so it stays correct no matter which block a given pass lands on.
//
// The slot is `entryAllocaNullPtr` (#2733), NOT a bare `entryAlloca`: a disposal
// pass can be reached on a path that never executed the production store (a
// sibling branch of an `if`, an unwind out of an operand evaluated before it),
// and a bare alloca would hand it an undef pointer to free.
//
// WHY IT CANNOT UAF. The value is a fresh `new`-produced heap class instance
// whose only use in the expression is as this receiver (isFreshOwnedClassProducer
// + the non-reference-result gate). On the unwind edge no result was produced at
// all, so nothing can alias into it; on the normal edge the slot is nulled
// before the inline reclaim frees the block, so no later pass reads a freed
// pointer.
llvm::AllocaInst* Emitter::registerFreshNewReceiverTempUnwind(
        const ast::Expr* recvExpr, llvm::Value* recvPtr,
        const sema::TypeRef* resultTy, bool calleeReturnsOwned) {
    if (!freshNewReceiverTempEligible(recvExpr, recvPtr, resultTy,
                                      calleeReturnsOwned))
        return nullptr;
    // No enclosing scope to unwind through (a disposer body, or a function whose
    // scopes are not yet established) — nothing to register against.
    if (disposeStack_.empty()) return nullptr;
    if (emittingDisposerBody_) return nullptr;
    // The eligibility gate already restricts to a heap CLASS producer, so this
    // is belt-and-braces: a non-pointer or literal-null receiver is never a
    // disposable heap instance.
    if (!recvPtr->getType()->isPointerTy()) return nullptr;
    if (llvm::isa<llvm::ConstantPointerNull>(recvPtr)) return nullptr;
    llvm::AllocaInst* slot = entryAllocaNullPtr();
    builder_.CreateStore(recvPtr, slot);
    Disposal d{slot, nullptr};
    d.unwindOnly = true;
    d.dynamicDispose = false;
    disposeStack_.back().push_back(d);
    return slot;
}


void Emitter::dischargeFreshNewReceiverTempUnwind(llvm::AllocaInst* slot) {
    if (!slot) return;
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), slot);
}


// fix #3377 / §28.3 / §29.1 / §30.1 (v0.282): the NORMAL-exit counterpart of
// #2261's narrowing. The narrowing correctly keeps `disposeFreshNewReceiverTemp`'s
// inline reclaim OFF for the deferred-activation result heads (OwningTask +
// the four `IOwning*` iterable-interface heads) — the iterator/task frame's
// settlement may still read `this`, so a same-expression inline dispose is a
// use-after-free. But the narrowing leaves the fresh receiver's ownership LOST
// — the iterator/task frame's Dispose disposes owned params / owned locals /
// closure captures / yielded value but does NOT dispose its `this` capture, so
// the receiver leaks. Thread the ownership to the frame's settlement by pushing
// the receiver onto the current `disposeStack_` scope (the same scope the result
// is bound to via the using-var / for-in source). Reverse-order disposal fires
// `source.Dispose()` first (which reads `this` during finally-chain restoration
// via emitFrameCaptureRestores), then the receiver.
//
// ⚠ THE SCOPE-BOUND GATE is the load-bearing soundness condition — see the
// declaration comment in LlvmBackend_impl.hpp. Pushing onto a LEXICAL scope is
// correct only for a frame that provably dies in that scope, which is why this
// fires only for a result bound to a `using var` initializer or a for-in
// iterable. Without it, `return new Src().Items();` frees the receiver inside
// the callee and the caller's first MoveNext reads freed memory (MEASURED —
// pinned by the c3 control in the #3377 test file). #3390 tracks the escaping
// shapes, which keep the (sound) leak until ownership lives in the frame.
//
// PAIRING. Called from `lowerCallDispatch` at the receiver-registration point,
// immediately after `registerFreshNewReceiverTempUnwind` — so BEFORE the
// arguments are evaluated and before the call runs. The inline reclaim
// (`disposeFreshNewReceiverTemp`) runs LATER, at the end of that function.
// The two cannot both fire: this helper requires the result to be a
// deferred-activation head, which is exactly the set #2261's narrowing makes
// `freshNewReceiverTempEligible` reject, and the inline reclaim requires that
// predicate to ACCEPT. So there is no double-dispose. Neither fires when the
// receiver is a borrow (isFreshOwnedClassProducer returns false), so the
// bound-receiver `using var s := new Src(); s.Items()` shape (#2440 / §29.1)
// is untouched.
//
// UAF AVOIDANCE. The entry is registered BEFORE the call (the call site pushes
// it as the last step of the receiver-side emission, before buildOrderedArgs
// runs). On a throw out of an argument evaluation or the call itself, the
// receiver is on the heap with no owner; the unwind pass fires this entry
// exactly once (null-guarded, the slot stays live), disposing the receiver.
//
// SCOPE ORDERING. The receiver is pushed first (at the call site); the
// using-var / for-in source's disposal is registered LATER (after the call
// returns and the binding completes). Reverse-order scope-end iteration fires
// the source's `Dispose` first (which reads `this`), then the receiver — so
// `this` is alive for the source's cleanup. No double-dispose: the source's
// Dispose does not touch `capture[0]` post-restoration (only finally chains
// do), and this helper's entry is the sole owner of the receiver slot.
//
// UNWIND-ONLY PATH. `registerFreshNewReceiverTempUnwind` returns null for the
// deferred-activation case (its predicate is the same
// `freshNewReceiverTempEligible`, narrowed by #2261), so there is no parallel
// `unwindOnly` entry to discharge. The single `unwindOnly=false` entry this
// helper pushes fires on either path (normal exit or unwind) — the standard
// scope-end disposal, with no double-dispose risk because nothing else holds
// the receiver pointer.
bool Emitter::exprReceiverChainContains(const ast::Expr* root,
                                        const ast::Expr* target) const {
    if (!root || !target) return false;
    for (const ast::Expr* e = root; e;) {
        if (e == target) return true;
        if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
            e = ce->callee.get();
            continue;
        }
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            e = ma->target.get();
            continue;
        }
        break;
    }
    return false;
}


void Emitter::disposeFreshNewReceiverTempDeferred(
        const ast::Expr* recvExpr, llvm::Value* recvPtr,
        const sema::TypeRef* resultTy, const ast::Expr* callExpr) {
    if (!recvExpr || !recvPtr || !resultTy) return;
    if (!isFreshOwnedClassProducer(recvExpr)) return;
    // ⚠ THE SOUNDNESS GATE (see the declaration comment). Only a frame bound
    // to a SCOPE-BOUND consumer in this statement — a `using var` initializer
    // or a for-in iterable — provably dies in the scope we are about to push
    // the receiver onto. `return new Src().Items();` does not, and pushing
    // there frees the receiver inside the callee while the returned frame
    // still holds it (heap-use-after-free — MEASURED). Everything that does
    // not match keeps the pre-fix leak, which is the sound direction.
    if (!exprReceiverChainContains(deferredRecvScopeBoundInit_, callExpr))
        return;
    // The result must be a deferred-activation head (the predicate's excluded
    // set — see freshNewReceiverTempEligible's #2261 narrowing comment). If
    // the predicate admits the result (owned class/interface, owning collection,
    // owning wrapper, value-result) the inline reclaim path handles disposal
    // and this helper must NOT push a parallel entry.
    if (resultTy->kind != sema::TypeRef::Kind::Named) return;
    std::string_view head = resultTy->headName();
    bool isDeferredActivation =
        common::isOwningTypeHead(head) &&
        (common::isTaskFamilyHead(head) ||
         common::isIterableInterfaceHead(head));
    if (!isDeferredActivation) return;
    // No enclosing scope to dispose through (a disposer body, or a function
    // whose scopes are not yet established).
    if (disposeStack_.empty()) return;
    if (emittingDisposerBody_) return;
    // Belt-and-braces: a non-pointer or literal-null receiver is never a
    // disposable heap instance. The eligibility gate above already restricts
    // to a heap CLASS producer, so this is defensive.
    if (!recvPtr->getType()->isPointerTy()) return;
    if (llvm::isa<llvm::ConstantPointerNull>(recvPtr)) return;
    // Per the #2733 commentary in registerFreshNewReceiverTempUnwind: a
    // null-init slot is required because a disposal pass can be reached on a
    // path that never executed the production store. We register before the
    // call here (not after), but the same null-init discipline applies — the
    // unwind path through an operand evaluated before the production store
    // would otherwise hand the disposal pass an undef pointer to free.
    llvm::AllocaInst* slot = entryAllocaNullPtr();
    builder_.CreateStore(recvPtr, slot);
    Disposal d{slot, nullptr};
    // Fires on both normal exit and unwind: the receiver's lifetime must
    // match the deferred-activation frame's settlement whether the consumer
    // finishes the loop normally, abandons mid-loop (a `break`/throw out of
    // the body, or an iterator whose MoveNext throws), or throws out of an
    // argument evaluation BEFORE the call returns (in which case the frame
    // was never constructed and the receiver is the sole owner of itself).
    d.unwindOnly = false;
    // dynamicDispose dispatches through the vtable at runtime (the receiver's
    // concrete class is not always statically known here — #3152's family
    // showed classOfLocal returning null for an imported generic). The
    // alternative `cls`-based path (line 6102) gates on `disposerFns_` having
    // an entry for `it->cls`; leaving `cls` null silently SKIPS the entry on
    // every scope end, which is the bug this attribute avoids. dynamicDispose
    // also nulls the slot after reclaim so a doubled scope traversal
    // (mid-expression binding-atom dispose scope, the same shape the
    // `ffiBuffer` arm documents) is exactly-once. // per #383 / §28.
    d.dynamicDispose = true;
    disposeStack_.back().push_back(d);
}


void Emitter::disposeFreshNewReceiverTemp(const ast::Expr* recvExpr,
                                          llvm::Value* recvPtr,
                                          const sema::TypeRef* resultTy,
                                          bool calleeReturnsOwned,
                                          llvm::AllocaInst* unwindSlot) {
    // fix #2816: the normal path is about to reclaim the temporary, so retire
    // its exception-edge registration FIRST — a slot left holding the pointer
    // would make a later unwind through this scope dispose an already-freed
    // block. Discharged unconditionally (before the eligibility test): the slot
    // is non-null only when registerFreshNewReceiverTempUnwind accepted the same
    // arguments, so this pairs exactly with a registration that happened.
    dischargeFreshNewReceiverTempUnwind(unwindSlot);
    if (!freshNewReceiverTempEligible(recvExpr, recvPtr, resultTy,
                                      calleeReturnsOwned))
        return;
    // Dispose (idempotent vtable Dispose + reclaim exactly once). A fresh `new`
    // is non-null, but null-guard for safety/uniformity (folded away for a
    // provably-non-null pointer).
    llvm::Value* isNull = builder_.CreateICmpEQ(
        recvPtr, llvm::ConstantPointerNull::get(ptrTy()));
    auto* dispBB =
        llvm::BasicBlock::Create(llvm_, "freshrecv.dispose", currentFn_);
    auto* contBB =
        llvm::BasicBlock::Create(llvm_, "freshrecv.cont", currentFn_);
    builder_.CreateCondBr(isNull, contBB, dispBB);
    builder_.SetInsertPoint(dispBB);
    emitDisposeAndReclaim(recvPtr);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}


// fix #4320 (§26.1 / §4.6 / §28.9, v0.298): the STRING-RECEIVER sibling of
// `disposeFreshNewReceiverTemp` (above). The class-receiver reclaim fires only
// for a heap-CLASS producer; a `+1` STRING producer reaches CG33b as a plain
// `ptr` and was leaking its allocation (the issue's repro: `P.Make().IsBlank`
// over 500 iterations under ASan/LSan — 18000 B / 1000 allocs from
// `bz$M.P$Make`, byte-identical to pre-#4243 because the leak is on the CG33b
// path #4243 left untouched). Releases via `bz_string_release` (refcounted;
// null- and sentinel-safe so the literal / sentinel / null producers in the
// §28.9 list release as a no-op) AFTER the getter body's result has been
// wrapped/boxed — nothing reads the receiver after this point on the normal
// exit.
//
// THE TWO GATES, with their rationale (full version in LlvmBackend_impl.hpp):
//   1. `isPlusOneStringProducer(recvExpr)` — the same predicate every other
//      §28.9 producer-release keys on. A literal / sentinel / named-local
//      receiver is correctly declined here: a literal receiver is an immortal
//      sentinel (release is a no-op), and a named-local receiver's lifetime
//      is the scope's, not the property read's.
//   2. The PROPERTY RESULT must be a NON-REFERENCE value — the same soundness
//      stance `freshNewReceiverTempEligible` (the class twin's predicate,
//      immediately above) takes for the class-receiver path: the result must
//      be provably free of any reference or borrow into the receiver. EXCLUDED
//      for an alias risk:
//        • `string` (reference-typed primitive — an identity getter `=> this`
//          returns the receiver itself, releasing would free the string
//          `v` (= obj) still points at)
//        • `object` (reference-typed primitive — a field/method result on the
//          receiver may borrow)
//        • any class/interface result (a reference — could embed a borrow)
//        • any struct/record (could embed a borrow of a receiver sub-object)
//      ADMITTED: Unit, numeric/bool/char/decimal primitives (Length/IsAscii /
//      IsBlank's shape), and enum. Mirroring the class predicate's exact
//      primitive set (PrimitiveType::None / String / Object excluded) keeps
//      the two paths' accept/reject surface aligned; the borrowed-receiver
//      `string` shape is excluded identically to the class path's borrowed-
//      receiver `string`-result shape that `freshNewReceiverTempEligible`'s
//      comment cites as the reason for the string-primitive exclusion there.
//
// `unwindSlot` is the slot returned by the parallel
// `registerFreshNewReceiverTempUnwind` — today always null for a string
// receiver (the class-only eligibility predicate rejects strings), but
// threaded through for symmetry and to leave room for a future unified
// predicate that admits both without touching call sites. A throw inside the
// getter body skips this inline release; that matches CG27a's own behavior
// on its simpler intrinsics (`loadStringLen` / `bz_string_is_ascii` are
// non-throwing; the issue's IsBlank getter is `=`/`or`/`Length`/`Trim` —
// none throw — so the repro exercises the inline path exactly once per
// iteration).
void Emitter::disposeFreshNewStringReceiverTemp(
        const ast::Expr* recvExpr, llvm::Value* recvPtr,
        const sema::TypeRef* resultTy, llvm::AllocaInst* /*unwindSlot*/) {
    // (unwindSlot is documented above; always null today — the class-only
    // registerFreshNewReceiverTempUnwind never admits strings.)
    if (!recvExpr || !recvPtr || !resultTy) return;
    if (!isPlusOneStringProducer(recvExpr)) return;
    // The result must be a NON-REFERENCE value (mirror the class twin's gate
    // at freshNewReceiverTempEligible, with `isPlusOneStringProducer` in the
    // producer-receiver slot).
    bool valueResult = false;
    if (resultTy->kind == sema::TypeRef::Kind::Unit) {
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Primitive &&
               resultTy->primitive != sema::PrimitiveType::None &&
               resultTy->primitive != sema::PrimitiveType::String &&
               resultTy->primitive != sema::PrimitiveType::Object) {
        // Length/IsAscii / IsBlank / int-valued user-property: a non-
        // reference primitive. The receiver is provably not aliased by the
        // result (an `int`/`bool` cannot hold a string).
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Named &&
               resultTy->declaration &&
               resultTy->declaration->kind == ast::TypeDeclKind::Enum) {
        // An enum result is a value type — no alias into the receiver.
        valueResult = true;
    }
    // Records/structs, classes, interfaces, `string`, `Object`: excluded,
    // because each may alias into the receiver (a class/interface result
    // could embed a borrow; a string result may be the receiver itself;
    // a record could embed a borrow of a receiver sub-object). The leak
    // stays — exactly the conservative stance `freshNewReceiverTempEligible`
    // documents for the class twin.
    if (!valueResult) return;
    // Belt-and-braces: only release a non-null heap pointer. Sentinel /
    // literal receivers are null-safely released by `bz_string_release`
    // anyway, but the guard keeps the IR shape identical to the class twin.
    if (!recvPtr->getType()->isPointerTy()) return;
    emitStringRelease(recvPtr);
}


llvm::Function* Emitter::elementDisposerThunk() {
    // #362 / stdlib §4.5: a standalone `void(void*)` thunk that runs an owning
    // collection element's destructor via the element's own vtable (slot 2,
    // CG35-itable), skipping a null element. Built once with its own IRBuilder
    // so the active `builder_` insert point is untouched.
    if (elementDisposerThunk_) return elementDisposerThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$elemDispose", module_.get());
    llvm::Argument* obj = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "elem.dispose", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "elem.ret", fn);
    llvm::IRBuilder<> b(entry);
    // A null/already-moved slot is safe to skip (no double-dispose / UAF).
    llvm::Value* isNull = b.CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, dispBB);
    b.SetInsertPoint(dispBB);
    // vtable layout (CG35-itable): [0]=td$, [1]=itmap$, [2]=Dispose.
    llvm::Value* vptr = b.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = b.CreateGEP(ptrTy(), vptr, {b.getInt32(2)});
    llvm::Value* disposeFn = b.CreateLoad(ptrTy(), slot);
    auto* dispTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                           {ptrTy()}, /*isVarArg=*/false);
    b.CreateCall(dispTy, disposeFn, {obj});
    // #380 / §28: an owning collection's element is a heap class instance; after
    // its (most-derived) Dispose runs the destructor + base chain, reclaim its
    // block. This thunk is the element-dispose path that does NOT route through
    // emitDynamicDispose (it is invoked per live element by bz_array_free), so
    // the free must be emitted here too. obj is non-null in this block.
    //
    // §28.2 v0.134 / issue #601 / #2514: mirror emitDisposeAndReclaim's
    // checked-disposal quarantine — in checked mode the block is NOT freed
    // after its Dispose runs, so the per-instance disposed flag (set by the
    // vtable Dispose call above) stays readable for a later member-entry /
    // field-access guard. This thunk is ALSO the per-T result_disposer attached
    // to a Pattern A `Task<T:class>` at production
    // (asyncPatternAResultDisposer_ → patternAResultDisposerFor → this thunk),
    // so without the gate a stale borrow of a disposed task's result — the
    // §28.7/§30.5/§30.10 drop-on-overwrite shape — reads FREED heap before the
    // flag guard fires (a heap-use-after-free under ASan, #2173/#2514). Gating
    // the free keeps the quarantined block's flag valid, making the stale read
    // deterministically raise DisposedAccessException (§31.4) instead. The
    // owning-collection element path is made sound the same way (each element
    // is quarantined alongside the already-quarantined header,
    // bz_array_mark_disposed). `checkedDisposal_` is fixed for the whole
    // module compile, so baking the branch into the cached thunk is correct.
    if (!checkedDisposal_)
        b.CreateCall(freeFn(), {obj});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    elementDisposerThunk_ = fn;
    return fn;
}


bool Emitter::isPlainObjectRef(const sema::TypeRef* t) {
    return t && t->kind == sema::TypeRef::Kind::Primitive &&
           t->primitive == sema::PrimitiveType::Object;
}


// fix #5949 (§30.5 / §28.9 / §8.1, v0.338): the result disposer of a
// `Task<Object>` (Pattern A: "the task owns any class-typed result it
// produces", `Object` included), attached by the producing async body at each
// return whose operand can be an `Object` instance. `elementPolicyFor` has no `Object` row — `Object` is a Primitive-kind
// TypeRef with no declaration — so the task teardown attached NO disposer and a
// `new Object()` result leaked on every consumption form.
//
// The slot may hold more than an `Object` instance (§5.1: `Object` is the
// universal reference type): a §8.6 box, a string, or a user-class instance
// upcast to `Object`. §30.5 kind 1 ("a box under `Task<Object>` ... with
// nothing for the teardown to dispose") and §28.9 put boxes on their own
// release rules, so — exactly as the `using var` local arm of fix #5437 does
// (emitDisposals' `objectReclaim`) — the thunk reclaims iff word 0 is the
// module's `Object` vtable BY ADDRESS and otherwise leaves the value alone.
// That fails safe: an unrecognized value is a bounded leak, never a free of
// memory another owner still holds (no double free of an upcast class
// instance, no free of a box). Reclaim is the free alone: §8.1 gives `Object`
// no destructor and no fields, so there is no logical-disposal step. Mirrors
// emitReclaimBlock: the identity-hash side-table entry is reaped first and the
// block is quarantined (not freed) in §28.2 checked-disposal mode.
llvm::Function* Emitter::objectReclaimThunk() {
    if (objectReclaimThunk_) return objectReclaimThunk_;
    auto objVtable = builtinVtable_.find("Object");
    if (objVtable == builtinVtable_.end()) return nullptr;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$objReclaim", module_.get());
    llvm::Argument* obj = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* tstBB = llvm::BasicBlock::Create(llvm_, "obj.test", fn);
    auto* relBB = llvm::BasicBlock::Create(llvm_, "obj.reclaim", fn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "obj.ret", fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(obj, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, tstBB);
    b.SetInsertPoint(tstBB);
    llvm::Value* word0 = b.CreateLoad(ptrTy(), obj);
    b.CreateCondBr(
        b.CreateICmpEQ(word0, llvm::cast<llvm::Constant>(objVtable->second)),
        relBB, retBB);
    b.SetInsertPoint(relBB);
    b.CreateCall(declareC("bz_obj_identity_hash_forget",
                          llvm::Type::getVoidTy(llvm_), {ptrTy()}),
                 {obj});
    if (!checkedDisposal_) b.CreateCall(freeFn(), {obj});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    objectReclaimThunk_ = fn;
    return fn;
}


// §28.13 / stdlib §2.16 (#603): void(void*) thunk that calls
// bz_shared_release(ptr), used as the per-element disposal callback for an
// OwningArray<Shared<T>> (or similar owning collection of shared handles).
// Mirrors elementDisposerThunk but calls the runtime release rather than
// dispatching through a class vtable (Shared<T> has no vtable).
llvm::Function* Emitter::sharedReleaseThunk() {
    if (sharedReleaseThunk_) return sharedReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$sharedRelease", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* relBB  = llvm::BasicBlock::Create(llvm_, "h.rel",  fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "h.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, relBB);
    b.SetInsertPoint(relBB);
    auto* relFn = declareC("bz_shared_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    sharedReleaseThunk_ = fn;
    return fn;
}

// §28.13 / stdlib §2.16 (#603): void(void*) thunk that calls
// bz_weak_release(ptr), used as the per-element disposal callback for an
// OwningArray<Weak<T>>.
llvm::Function* Emitter::weakReleaseThunk() {
    if (weakReleaseThunk_) return weakReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$weakRelease", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* relBB  = llvm::BasicBlock::Create(llvm_, "h.rel",  fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "h.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, relBB);
    b.SetInsertPoint(relBB);
    auto* relFn = declareC("bz_weak_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    weakReleaseThunk_ = fn;
    return fn;
}

// stdlib §5 (#651): void(void*) thunk that calls bz_grouping_free(ptr),
// used as the per-element disposal callback for an Array<IGrouping<TKey,T>>.
// Null-checks the pointer first (same pattern as sharedReleaseThunk).
llvm::Function* Emitter::groupingFreeThunk() {
    if (groupingFreeThunk_) return groupingFreeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$groupingFree", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "g.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "g.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    auto* freeFn = declareC("bz_grouping_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(freeFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    groupingFreeThunk_ = fn;
    return fn;
}

// §2.11 (#838): the per-element disposer thunk for a Subscription element of an
// owning collection (OwningArray<Subscription>, etc.). A Subscription is a
// runtime-managed opaque handle (bz_subscription_t*) disposed by
// bz_subscription_dispose (which unsubscribes), not via a vtable — so an owning
// collection of Subscriptions needs this thunk to unsubscribe each element at
// scope-end/clear/remove. Null-safe + idempotent (mirrors the `using var`
// Subscription auto-dispose). Same shape as groupingFreeThunk.
llvm::Function* Emitter::subscriptionDisposeThunk() {
    if (subscriptionDisposeThunk_) return subscriptionDisposeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$subscriptionDispose", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "s.disp", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "s.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, dispBB);
    b.SetInsertPoint(dispBB);
    auto* dispFn = declareC("bz_subscription_dispose",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(dispFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    subscriptionDisposeThunk_ = fn;
    return fn;
}

// §28.11 (#868): void(void*) thunk that frees a nested builtin owning collection
// element (e.g. the OwningArray<R> inside an OwningArray<OwningArray<R>>) by calling
// the right runtime free fn with the recursive inner-element disposer.
// Mirrors sharedReleaseThunk / groupingFreeThunk in shape: own IRBuilder, null
// check, call free fn, return void. Cached per inner element-collection TypeRef*.
//
// Free fn selection (mirrors emitOwnedFieldDisposals / emitDisposals):
//   OwningArray / OwningStack / OwningQueue  → bz_array_free(ptr, innerDisposer)
//   OwningLinkedList                       → bz_llist_free(ptr, innerDisposer)
//   OwningDictionary                       → bz_dict_free(ptr, innerValueDisposer)
//     (OwningDictionary<K,V> owns its V elements; genericArgs[1] is V)
//   OwningSet / OwningBuffer                → bz_array_free(ptr, elementDisposer)
//     (#888: an OwningSet<class>'s elements are disposed by bz_array_free with the
//     element disposer, the same call the top-level/field OwningSet disposal uses.)
//
// The recursion terminates: each level peels one collection layer; eventually the
// inner element is a class R → elementDisposerThunk, or a plain value → null.
llvm::Function* Emitter::nestedCollectionDisposerThunk(const sema::TypeRef* elemCollTy) {
    // Cache hit.
    auto it = nestedCollectionDisposerThunks_.find(elemCollTy);
    if (it != nestedCollectionDisposerThunks_.end()) return it->second;

    // Determine the inner element type and free fn name from the collection's
    // kind (using the same name-detection as isCollectionTypeName /
    // isLinkedListTypeName / isDictTypeName, per §28.11 #868).
    std::string_view collName;
    if (elemCollTy && elemCollTy->kind == sema::TypeRef::Kind::Named &&
        !elemCollTy->declaration &&
        elemCollTy->astNode && elemCollTy->astNode->name &&
        !elemCollTy->astNode->name->parts.empty())
        collName = elemCollTy->headName();

    bool isLL   = (collName == "OwningLinkedList");
    bool isDict = (collName == "OwningDictionary");

    const char* freeName = isLL ? "bz_llist_free"
                         : isDict ? "bz_dict_free"
                         : "bz_array_free";

    // Resolve the inner element type (the thing the inner collection owns):
    //   OwningArray/Stack/Queue/LinkedList/Buffer/Set → genericArgs[0]
    //   OwningDictionary                              → genericArgs[1] (the value)
    // §28.11 (#888): OwningSet's class elements are freed via bz_array_free with
    // the element disposer — the SAME call the top-level/field OwningSet disposal
    // uses (it is not a dict-type-name, so the field path at emitOwnedField-
    // Disposals selects bz_array_free + the element disposer). Earlier this thunk
    // passed a null disposer for a nested OwningSet, so an OwningSet<class> inside
    // another owning collection leaked its elements; routing genericArgs[0]
    // through elementPolicyFor disposes them, mirroring the top-level path.
    const sema::TypeRef* innerElem = nullptr;
    if (elemCollTy && !isDict) {
        innerElem = (!elemCollTy->genericArgs.empty()) ? elemCollTy->genericArgs[0]
                                                       : nullptr;
    } else if (isDict) {
        innerElem = (elemCollTy->genericArgs.size() > 1) ? elemCollTy->genericArgs[1]
                                                         : nullptr;
    }

    // Recursively get the inner element's disposer (elementPolicyFor does its
    // own caching / base cases, including further nested-collection detection).
    llvm::Function* innerDisposer =
        elementPolicyFor(innerElem, /*owningFamily=*/true).disposerThunk;

    // Build the thunk: void(void* innerCollPtr) { if(!ptr) return;
    //                                              freeFn(ptr, innerDisposer); }
    // Uses its own IRBuilder so `builder_` insert point is untouched.
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    // Name the thunk after the collection kind for debuggability.
    std::string thunkName = std::string("bz$nestedFree$") + std::string(collName);
    // Make unique if same collection name appears multiple times (shouldn't, but
    // guard against symbol clash if two OwningArray<R1> and OwningArray<R2> both
    // need a thunk — give each its own symbol via the type's §2.8.3 rule 3
    // identity, not the cache-key POINTER (fix #3548: a pointer value is a
    // fresh heap address every process, so the old spelling made `bezelc`'s
    // output non-reproducible — nm'd different bytes across two runs of the
    // SAME compiler on the SAME source; mangleTypeRefForSymbol recurses
    // through genericArgs, so OwningArray<R1> and OwningArray<R2> still land
    // on distinct, but now deterministic, names).
    if (module_->getFunction(thunkName)) {
        thunkName += "$" + mangleTypeRefForSymbol(elemCollTy);
    }
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    llvm::Argument* ptr = &*fn->args().begin();
    auto* entry  = llvm::BasicBlock::Create(llvm_, "entry",    fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "coll.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "coll.ret",  fn);
    llvm::IRBuilder<> b(entry);
    // Null-guard: bz_array_free/bz_llist_free/bz_dict_free are null-safe but
    // check here anyway (mirrors sharedReleaseThunk pattern).
    b.CreateCondBr(
        b.CreateICmpEQ(ptr, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    llvm::Value* innerDispArg = innerDisposer
        ? llvm::cast<llvm::Value>(innerDisposer)
        : llvm::ConstantPointerNull::get(ptrTy());
    auto* freeFn = declareC(freeName,
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
    b.CreateCall(freeFn, {ptr, innerDispArg});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();

    nestedCollectionDisposerThunks_[elemCollTy] = fn;
    return fn;
}


// fix #4054 / §28.11 / §28.9: void(void*) thunk that frees a BORROWING
// built-in collection held in an OWNING slot (e.g. the Array<int> inside
// an OwningArray<Array<int>>). Mirrors nestedCollectionDisposerThunk above
// in shape (own IRBuilder, null check, call free fn, return void; cached per
// inner TypeRef*) but is fixed at NULL for the inner disposer — the borrow
// family does not own its elements, so passing the recursive inner-element
// disposer (what nestedCollectionDisposerThunk does for the OWNING case)
// would double-free values whose real owner is elsewhere.
//
// Free-fn selection (mirrors nestedCollectionDisposerThunk's, with SortedSet
// / Set excluded — they have dynamic backing, see #2726):
//   Array / Stack / Queue / OwningBuffer / FrozenArray → bz_array_free
//   LinkedList                                          → bz_llist_free
//   Dictionary / SortedDictionary                       → bz_dict_free
//
// `collName` is the head name already known to the caller (the std::optional
// returned by the isBorrowingBuiltinCollection detection lambda in
// elementPolicyFor), avoiding a second head-name decode here.
llvm::Function* Emitter::borrowingCollectionDisposerThunk(
        const sema::TypeRef* elemCollTy, std::string_view collName) {
    // Cache hit.
    auto it = borrowingCollectionDisposerThunks_.find(elemCollTy);
    if (it != borrowingCollectionDisposerThunks_.end()) return it->second;

    bool isLL   = (collName == "LinkedList");
    bool isDict = (collName == "Dictionary" || collName == "SortedDictionary");

    const char* freeName = isLL ? "bz_llist_free"
                         : isDict ? "bz_dict_free"
                         : "bz_array_free";

    // Build the thunk: void(void* innerCollPtr) { if(!ptr) return;
    //                                              freeFn(ptr, null); }
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    // fix #3548: name from the type's §2.8.3 rule 3 identity
    // (mangleTypeRefForSymbol), not the cache-key POINTER — a pointer value
    // is a fresh heap address every process, so the old spelling made
    // `bezelc`'s output non-reproducible (same defect #3548 pinned for
    // nestedCollectionDisposerThunk above).
    std::string thunkName = std::string("bz$borrowFree$") + std::string(collName);
    if (module_->getFunction(thunkName)) {
        thunkName += "$" + mangleTypeRefForSymbol(elemCollTy);
    }
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    llvm::Argument* ptr = &*fn->args().begin();
    auto* entry  = llvm::BasicBlock::Create(llvm_, "entry",    fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "coll.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "coll.ret",  fn);
    llvm::IRBuilder<> b(entry);
    // Null-guard (mirrors nestedCollectionDisposerThunk).
    b.CreateCondBr(
        b.CreateICmpEQ(ptr, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    // Inner disposer is FIXED at NULL (the borrow family does not own its
    // elements; see header).
    auto* freeFn = declareC(freeName,
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
    b.CreateCall(freeFn,
        {ptr, llvm::ConstantPointerNull::get(ptrTy())});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();

    borrowingCollectionDisposerThunks_[elemCollTy] = fn;
    return fn;
}


// fix #1889 / §28.6 / §30.5: dispose one Task / Task<T> / OwningTask<T>
// element owned by an owning collection. Collection free callbacks have the
// same `void(void*)` ABI as the class-element thunks above (passed to
// bz_array_free / bz_llist_free / bz_dict_free as the per-element disposer),
// but a task needs the full non-blocking protocol rather than a vtable Dispose
// call: its unconsumed class result (OwningTask<T:class> via
// bz_task_set_result_disposer, §30.5) and its fault object (§30.9 / #3030's
// deferred-or-now-disposed prelude) are both part of the task's disposal
// contract — a bare bz_task_free would reclaim only the task struct and leak
// both. So this thunk calls emitTaskDisposal — the same helper the `using
// var` task scope-end path (Disposal.cpp ~5127) and the owned-field path
// (emitOwnedTaskDisposal, line 454) already share.
//
// The thunk is built in a private LLVM function while temporarily making it
// the active emitter function (`currentFn_`) and insertion point, then
// restoring the caller's. emitTaskDisposal reads/writes `builder_` /
// `currentFn_` (CreateCondBr, CreateCall, declareC) so we must swap them
// around the call — a LOCAL IRBuilder (the shape sharedReleaseThunk /
// groupingFreeThunk / nestedCollectionDisposerThunk use) would not survive
// emitTaskDisposal's first builder_ read. The save/restore is the same
// pattern the existing field-path uses (emitTaskDisposal's own block
// transitions); a null input is short-circuited inside emitTaskDisposal so
// no extra guard is needed here. Cached per Task TypeRef* (each Task /
// Task<T> / OwningTask<T> gets its own thunk so the per-T result_disposer
// matches that element's concrete T — see emitTaskDisposal line 593 which
// reads taskTy->genericArgs[0] via elementPolicyFor). // per §30.5 / §30.9
llvm::Function* Emitter::taskElementDisposerThunk(const sema::TypeRef* taskTy) {
    // Cache hit.
    auto it = taskElementDisposerThunks_.find(taskTy);
    if (it != taskElementDisposerThunks_.end()) return it->second;

    // void(void*) thunk: void bz$taskElemDispose$<mangled T>(bz_task_t* task)
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    // Per-TypeRef key on the symbol name (mirrors nestedCollectionDisposerThunk
    // — guards against name clash if a module needs more than one Task thunk).
    // fix #3548: name from the type's §2.8.3 rule 3 identity
    // (mangleTypeRefForSymbol), not the taskTy cache-key POINTER — a pointer
    // value is a fresh heap address every process (this was the issue's own
    // repro: `bz$taskElemDispose$24965040` vs `…$447905712` for the SAME
    // source compiled twice). Two distinct TypeRef* nodes CAN denote the same
    // semantic Task<T> (one per textual occurrence) and so want the identical
    // mangled name; llvm::Function::Create's Module insertion auto-renames a
    // collision via ValueSymbolTable::createValueName (deterministic on
    // creation order, which is itself deterministic — this thunk is built
    // synchronously during a single-threaded, syntax-order codegen walk, not
    // from unordered-container iteration), so no manual counter is needed.
    std::string name = "bz$taskElemDispose";
    name += "$" + mangleTypeRefForSymbol(taskTy);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      name, module_.get());
    taskElementDisposerThunks_[taskTy] = fn;
    llvm::Argument* task = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);

    // Swap builder_ / currentFn_ so emitTaskDisposal emits into `fn`. Restored
    // before returning; the caller's insertion point is unchanged.
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    builder_.SetInsertPoint(entry);
    emitTaskDisposal(taskTy, task);
    builder_.CreateRetVoid();
    currentFn_ = savedFn;
    builder_.restoreIP(savedIP);
    return fn;
}


// #2279 (§30.5 Pattern A, v0.236): build the per-result-collection-type
// `void(void*)` thunk that frees a Pattern A task's RESULT when that result
// is itself an intrinsic collection (Array<T> / Set<T> / Dictionary<K,V> /
// LinkedList<T> / Stack<T> / Queue<T> / OwningBuffer<T> / OwningArray<T> /
// Owning* collections). §30.5 producer-side rules: a `Task<T:class>` for
// class T always owns its result directly, and "disposing the task disposes
// the result" — for a class instance that means calling its destructor via
// the vtable; for an intrinsic collection that means freeing the
// collection's backing storage (the bz_array_t*/bz_dict_t*/bz_llist_t*/*
// bz_buffer_t* header+data) with the right per-element disposer. The
// existing elementPolicyFor-based path (`patternAResultDisposerFor`'s
// ClassOwned branch) returns null when T is e.g. Array<int> (int is a Value
// category, no disposer thunk) — the result then leaked (LSan: 104 bytes
// from bz_array_alloc per #2279's repro). This helper fills that gap.
//
// Free fn selection (mirrors the `using var` collection disposal path and
// `nestedCollectionDisposerThunk` above, so the result-disposal contract
// stays consistent regardless of whether the consumer is the task's own
// teardown or a `using var` of the task):
//   Array / Stack / Queue / Set / SortedSet / OwningArray / OwningStack
//     / OwningQueue / OwningSet              → bz_array_free(ptr, elemDisposer)
//   Dictionary / OwningDictionary / SortedDictionary
//                                             → bz_dict_free(ptr, valueDisposer)
//   LinkedList / OwningLinkedList             → bz_llist_free(ptr, elemDisposer)
//   OwningBuffer                             → bz_buffer_free(ptr)
//
// Per-element disposer is the per-element-policy disposer for the SAME
// family (owning vs borrowing) as the result collection — so a borrowing
// Array<MyClass> passes null (the array only borrows its class elements per
// §28.11; elements are owned elsewhere and never disposed by the array),
// an OwningArray<MyClass> passes the element-disposer thunk, an
// Array<string> passes stringElementReleaseThunk (the +1 retained at insert
// is released at free), an OwningDictionary<MyClass, MyClass> passes the
// value-side disposer (genericArgs[1]). Returns null when `resultCollTy` is
// not an intrinsic collection — the caller falls back to the existing
// class-result path which is correct for plain classes.
//
// fix #3657 / #3658 (§30.5 / §28.3, v0.286): MEMBERSHIP — "is `resultCollTy`
// one of these intrinsic collections at all?" — now comes from the shared
// closed table `common::isPatternAResultCollectionHead`
// (common/PatternAResultCollection.hpp, #3638), not from the open-ended
// `common::isOwningPrefixedName(collName)` fallback that used to stand here.
// That fallback matched ANY name starting with "Owning" — including
// `OwningTask` — so a `Task<OwningTask<U>>` result entered this array-backed
// free path and was freed with `bz_array_free` over a task object, even
// though this file's own `patternAResultDisposerFor` (LlvmBackend_Async.cpp)
// documents (MEASURED: sixteen lines past its call to this function) that a nested
// Task-family result is BORROWED, not OWNED and must not be disposed here
// (fix #1889 / #776 / #777) — that guard was unreachable for `OwningTask`
// because this function returned non-null first. KIND DISPATCH — which
// runtime free fn and which per-element disposer a MEMBER gets — is
// UNCHANGED (isLL/isDict/isBuffer and the isOwningColl element-family flag
// below still decide that exactly as before); only the gate that decides
// whether a name is a member at all has moved onto the shared table, so it
// now agrees BY CONSTRUCTION with the ownership-side `taskResultArgIsClass`
// (Ownership_Checks.cpp), which has read the same table since #3638.
//
// §6b exclusion diff — every name the old open-ended fallback admitted that
// the shared table does not, and what handles it now:
//   * `OwningTask` — the #3657 defect above. Now excluded at the gate below;
//     falls through in `patternAResultDisposerFor` to the pre-existing
//     `isTaskTypeName(tElem)` check (already there, already correct for
//     `Task<Task<U>>` — plain `Task` was never `Owning*`-prefixed, so it
//     never reached this array-free path), which returns null: the outer
//     task's teardown does NOT dispose the inner task. That is not a new
//     leak — it is the exact same "borrowed, not owned" contract
//     `Task<Task<U>>` has always gotten, extended to `Task<OwningTask<U>>`.
//     Proven by `nested_owning_task_result_use_after_free_3657` and its
//     loop-amplified sibling `nested_owning_task_result_amplified_200x_3657`
//     in tests/codegen/pattern_a_task_result_collection_membership_3657_
//     3658_test.cpp: LSan-clean, correct value, over 200 produce/await
//     iterations.
//   * `OwningTaskCompletionSource` — also `Owning*`-prefixed, also not
//     isLL/isDict/isBuffer, so the old fallback admitted it too, and (MEASURED
//     with a debug probe against the pre-fix build) it really did reach this
//     function's array-free-thunk construction with `collName ==
//     "OwningTaskCompletionSource"`. Empirically, though, exercising a
//     `Task<OwningTaskCompletionSource<T>>` result through that wrong thunk
//     did NOT crash and did NOT leak in 5/5 runs (`bz_array_free`'s `free()`
//     on the base pointer apparently satisfied the allocator here — this is
//     UB that happened not to fault on this input, not a proof of
//     correctness). Now excluded at the gate below; falls through to
//     `elementPolicyFor`, which has NO special case for
//     TaskCompletionSource/OwningTaskCompletionSource (unlike its
//     Shared/Weak/IGrouping/Subscription/owning-builtin-collection/Task-
//     family siblings, each of which gets a dedicated arm in
//     LlvmBackend_ElementPolicy.cpp routing to the type's real release
//     function) and reports the default Value category — so post-fix the
//     outer task's teardown deterministically LEAKS the
//     `OwningTaskCompletionSource` result (MEASURED: 208 bytes/instance,
//     runAotChecked LSan report, reproduces 5/5). A real fix belongs in
//     LlvmBackend_ElementPolicy.cpp: add a TaskCompletionSource/
//     OwningTaskCompletionSource arm that calls `emitTcsRelease` — the SAME
//     function this file's `bn == "TaskCompletionSource" || bn ==
//     "OwningTaskCompletionSource"` builtin-dispose arm already uses for a
//     `using var tcs := new OwningTaskCompletionSource<T>()` local (line
//     ~2578), so the release mechanism exists; `elementPolicyFor` just never
//     learned to reach it for a collection-element or Pattern-A-result slot.
//     Out of scope for #3657/#3658's membership-routing fix (a different
//     file's function needs the new arm); filed as #3685 with both
//     measurements rather than silently shipped. Per §6c: this trades
//     UB-with-no-observed-symptom-here for a deterministic, LSan-detectable
//     leak — strictly MORE diagnosable, not less, even though neither is the
//     correct final answer.
//   * Nothing else: every other `Owning*`-prefixed stdlib name
//     (`OwningArray`/`OwningStack`/`OwningQueue`/`OwningSet`/
//     `OwningDictionary`/`OwningLinkedList`/`OwningBuffer`) is one of the 15
//     table members and behaves exactly as before — including a separately-
//     filed (#3684), PRE-EXISTING, fix-unrelated `OwningBuffer` gap (MEASURED
//     identical on this fix and its parent: a `Task<OwningBuffer<T>>` result
//     leaks its `bz_array_alloc_count_n` backing at task teardown; the
//     `isBuffer` kind-dispatch flag below is set by the literal `collName ==
//     "OwningBuffer"` test, never by the membership gate, so this fix cannot
//     have changed it). A user class literally named e.g. `OwningBox` (the
//     spec §28.9 example type) is unaffected in either direction — it has a
//     real `declaration`, so it never reached this function at all (the
//     guard just below rejects any `resultCollTy->declaration != nullptr`),
//     before or after this fix. `OwningSortedDictionary` does not exist in
//     this stdlib revision (the stdlib's own §4.17 prose says so) — there is
//     no such type to construct a `Task<OwningSortedDictionary<K,V>>` from.
llvm::Function* Emitter::patternAResultCollectionDisposer(
    const sema::TypeRef* resultCollTy) {
    // Cache hit.
    auto it = patternAResultCollectionDisposers_.find(resultCollTy);
    if (it != patternAResultCollectionDisposers_.end()) return it->second;

    // Reject non-intrinsic-collection types — the existing class-result path
    // is correct for plain classes. An intrinsic collection has no
    // declaration (it's a compiler-known builtin, like the Shared/Weak/
    // IGrouping/Subscription builtins) with a name-typed astNode. Mirror
    // the builtin-collTy detection already used by nestedCollectionDisposer-
    // Thunk / collectionTypeOfLocal / linkedListTypeOfLocal — same keying,
    // so a user class literally named `Array` keeps the existing path.
    std::string_view collName;
    if (!resultCollTy || resultCollTy->kind != sema::TypeRef::Kind::Named ||
        resultCollTy->declaration ||
        !resultCollTy->astNode || !resultCollTy->astNode->name ||
        resultCollTy->astNode->name->parts.empty()) {
        return nullptr;
    }
    collName = resultCollTy->headName();

    // fix #3657 / #3658 (§30.5 / §28.3, v0.286): MEMBERSHIP — is `collName`
    // one of the 15 §30.5 Pattern A intrinsic-collection heads at all? — is
    // now the ONE both-sides answer, `common::isPatternAResultCollectionHead`
    // (common/PatternAResultCollection.hpp, #3638), not the open-ended
    // `common::isOwningPrefixedName(collName)` fallback the KIND DISPATCH
    // below used to also (mis)use for this question. See the §6b exclusion
    // diff in the function's header comment above for exactly what this
    // excludes that the old fallback admitted (`OwningTask`,
    // `OwningTaskCompletionSource`) and what handles each now.
    if (!common::isPatternAResultCollectionHead(collName)) {
        return nullptr;
    }

    // KIND DISPATCH — reached only for an agreed member — decides which
    // runtime free fn and which per-element-disposer FAMILY a member gets.
    // UNCHANGED by #3657/#3658: the runtime free fns accept a null
    // per-element disposer for value-typed elements (bz_array_free skips the
    // dispose loop) and for borrowing elements (the array only borrows its
    // elements per §28.11). For a borrowing collection of strings,
    // stringElementReleaseThunk is the right disposer (the +1 retained at
    // insert must be released at free).
    bool isLL          = (collName == "OwningLinkedList" || collName == "LinkedList");
    bool isDict        = (collName == "Dictionary" || collName == "OwningDictionary" ||
                          collName == "SortedDictionary");
    bool isBuffer      = (collName == "OwningBuffer");
    bool isOwningColl  = isLL || isDict || isBuffer
                          ? false  // isOwning derived per-coll below
                          : common::isOwningPrefixedName(collName) ||
                            collName == "Array" || collName == "Stack" ||
                            collName == "Queue" || collName == "Set" ||
                            collName == "SortedSet";
    // For LinkedList / Dictionary / OwningBuffer, the owning property is the
    // head's "Owning" prefix (per stdlib §4.6/§4.7/§4.8/§4.9 — OwningLinkedList
    // / OwningDictionary own their elements; the borrowing forms don't).
    // fix #3684: §4.14 OwningBuffer<T> admits BOTH value-typed T (no per-slot
    // obligation — destructor just releases backing storage) AND class-typed T
    // (the destructor disposes each non-null slot). Setting isOwningColl from
    // the prefix makes class-T slots route through their per-element dispose
    // thunk via bz_array_free; value-T elements get a null disposer because
    // elementPolicyFor returns no thunk for value-typed T. Pre-fix this arm
    // forced isOwningColl=false (because bz_buffer_free was single-arg and
    // no element disposer could be passed), so an OwningBuffer<MyClass> Task
    // result leaked its class slots — measured as 32 B / 2 Widgets.
    if (collName == "LinkedList" || collName == "Dictionary" ||
        collName == "SortedDictionary" || collName == "OwningBuffer") {
        isOwningColl = common::isOwningPrefixedName(collName);
    }
    if (!isLL && !isDict && !isBuffer && !isOwningColl) {
        // Unreachable for any name that passed the membership gate above (all
        // 15 table members set one of these four flags) — kept as a
        // defense-in-depth check against a FUTURE table/kind-dispatch drift
        // (a name added to the table without a matching kind-dispatch arm),
        // which is exactly the failure mode this whole fix exists to end.
        return nullptr;
    }
    // §4.14 OwningBuffer<T>: runtime storage is `bz_array_t*` (allocated via
    // `bz_array_alloc_count_n` — see the new-OwningBuffer arm in
    // LlvmBackend_Exprs2.cpp:5070-5096), the SAME layout every other
    // `isCollectionTypeName` collection uses. Pre-fix this function below
    // routed OwningBuffer through `bz_buffer_free(ptr)` — a free fn that
    // targets the separately-managed `bz_buffer_hdr_t*` layout of `Buffer<T>`
    // (single calloc'd block, header + inline payload) and does `free(buf)`
    // only. That freed the bz_array_t header as if it were one block but did
    // NOT free `arr->data`, so a `Task<OwningBuffer<T>>` Pattern A result
    // leaked its data payload at task teardown (#3684) — exactly the 16-byte
    // LSan signature in the issue body. The using-var scope-end path (in
    // `registerUsingDisposal`/emit disposal, `d.collection = true` arm) and the
    // nested-collection disposer thunk (`nestedCollectionDisposerThunk`
    // above) BOTH already route OwningBuffer through `bz_array_free` — Pattern
    // A was the only orphan. Pinned in
    // tests/codegen/owning_buffer_task_pattern_a_disposal_3684_test.cpp
    // (runAotChecked, LSan ON, multiple rows: int value-T control,
    // PlainClass class-T control, the recursive nested-owning-collection row,
    // and the amplified 200× buffer-count check for the row-1 orphan).
    // // per §30.5 / §14.4 / §4.14, v0.287

    // fix #3684 / §30.5 / §14.4: OwningBuffer joins the array-backed free path
    // (bz_array_free) — see the paragraph above.
    const char* freeName = isLL ? "bz_llist_free"
                         : isDict ? "bz_dict_free"
                         : "bz_array_free";
    // The free fn signature: every free fn in scope (bz_array_free,
    // bz_llist_free, bz_dict_free) takes (ptr, dispose_elem). For an
    // OwningBuffer<T> with class-typed T the element disposer is the class
    // Destroy thunk; for value-typed T elementPolicyFor returns no disposer
    // and bz_array_free's per-element loop is skipped. (Pre-fix this was a
    // bool that branched between bz_buffer_free(ptr) and the two-arg form;
    // with the buffer arm merged with the bz_array_free path it is
    // invariantly two-arg.)

    // Per-element (or per-value) disposer:
    //   Array/Stack/Queue/Set/OwningArray/OwningStack/OwningQueue/OwningSet/
    //   LinkedList/OwningLinkedList/OwningBuffer: elementType = genericArgs[0],
    //     isOwning = isOwningColl (OWNING for OwningBuffer per §4.14)
    //   Dictionary/OwningDictionary/SortedDictionary: elementType is the
    //     KEY (genericArgs[0]) for isOwning purposes, BUT the value
    //     (genericArgs[1]) is the one that carries the disposal obligation
    //     (§28.11 "owning dictionary owns its values, keys are value-like").
    //     For the runtime free call we pass the VALUE disposer; the value
    //     element policy (owningDictionaryElementDisposer etc.) already uses
    //     genericArgs[1].
    const sema::TypeRef* elemForDisposer = nullptr;
    if (isDict) {
        elemForDisposer = (resultCollTy->genericArgs.size() > 1)
                              ? resultCollTy->genericArgs[1] : nullptr;
    } else {
        elemForDisposer = (!resultCollTy->genericArgs.empty())
                              ? resultCollTy->genericArgs[0] : nullptr;
    }
    // Resolve the disposer thunk through elementPolicyFor — it returns null
    // for value elements (no obligation), the class element-disposer thunk
    // for class elements, and a string-typed variant via the same call. For
    // the array-of-string case specifically, elementPolicyFor returns
    // Category::RuntimeManaged with disposerThunk=null but retainRelease=true;
    // we explicitly use stringElementReleaseThunk below because the runtime
    // call needs a real function pointer for the per-element release.
    llvm::Value* elemDisposer = nullptr;
    if (elemForDisposer) {
        ElementPolicy elemPol = elementPolicyFor(elemForDisposer, isOwningColl);
        if (elemPol.disposerThunk) {
            elemDisposer = elemPol.disposerThunk;
        } else if (elemPol.cat == ElementPolicy::Category::RuntimeManaged &&
                   elemForDisposer->kind == sema::TypeRef::Kind::Primitive &&
                   elemForDisposer->primitive == sema::PrimitiveType::String &&
                   // fix #2692 / §28.11: `!nullable` — every one of the retain
                   // guards on the construction side excludes a NULLABLE string
                   // element (`litIsStrArr` / `llitIsStrElem` in
                   // materializeHeapCollectionLiteral, `dictIsStringValue` in
                   // materializeHeapDictLiteral, `llIsStrElem` in
                   // LlvmBackend_Calls3.cpp, `arrIsStringElem` in
                   // LlvmBackend_Calls.cpp), because elementPolicyFor does NOT
                   // look at `nullable` and still reports RuntimeManaged for a
                   // `string?`. This site was the only release site WITHOUT the
                   // matching guard, so an `Array<string?>` / `LinkedList<string?>`
                   // Pattern-A task result released a +1 nothing retained — an
                   // unpaired release, i.e. the double-free direction. Dropping
                   // it leaves a `string?` element leaking instead, which is the
                   // standing safe direction of this file. Pre-existing and
                   // family-wide; found while completing #2692's LinkedList half.
                   !elemForDisposer->nullable) {
            // Borrowing Array<string> / Dictionary<string, int> / etc. —
            // release each string element. Mirrors the
            // collectionElemDisposer wiring in LlvmBackend_Setup.cpp
            // (which sets stringElementReleaseThunk for string-element
            // collections).
            elemDisposer = stringElementReleaseThunk();
        }
    }

    // Build the thunk: void(void* resultCollPtr) {
    //                       if (!ptr) return;
    //                       freeFn(ptr, elemDisposer); }
    // fix #3684: every free fn in scope (bz_array_free / bz_llist_free /
    // bz_dict_free) is the two-arg form (ptr, dispose_elem); OwningBuffer
    // used to route through bz_buffer_free (single-arg), but OwningBuffer's
    // runtime storage is a bz_array_t* (allocated via bz_array_alloc_count_n),
    // so the single-arg bz_buffer_free was the wrong free fn — see the
    // big comment block above the kind-dispatch for the full diagnosis.
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    std::string thunkName = std::string("bz$patAResultCollFree$") +
                            std::string(collName);
    // fix #3548: disambiguate by the type's §2.8.3 rule 3 identity, not the
    // resultCollTy cache-key POINTER — see nestedCollectionDisposerThunk above
    // for the full rationale (this is the same pattern: collName alone
    // collapses e.g. OwningArray<R1> and OwningArray<R2>, and the pointer
    // suffix that used to disambiguate them was ASLR-fresh every process).
    if (module_->getFunction(thunkName)) {
        thunkName += "$" + mangleTypeRefForSymbol(resultCollTy);
    }
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    llvm::Argument* ptr = &*fn->args().begin();
    auto* entry  = llvm::BasicBlock::Create(llvm_, "entry",    fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "coll.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "coll.ret",  fn);
    llvm::IRBuilder<> b(entry);
    // Null-guard: every free fn above is null-safe but the guard matches the
    // nestedCollectionDisposerThunk convention.
    b.CreateCondBr(
        b.CreateICmpEQ(ptr, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    {
        llvm::Value* dispArg = elemDisposer
            ? elemDisposer
            : llvm::ConstantPointerNull::get(ptrTy());
        auto* freeFn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
        b.CreateCall(freeFn, {ptr, dispArg});
    }
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();

    patternAResultCollectionDisposers_[resultCollTy] = fn;
    return fn;
}


bool Emitter::collectionElemIsClass(const sema::TypeRef* collTy, unsigned idx) {
    // fix #619 / §28.11 v0.135 / §14.4: the §28.11 table's owned/borrow column
    // applies to "class / interface type" — both class AND interface elements are
    // class-like from the collection's perspective (vtable-dispatch, pointer slot).
    // Widening to Class|Interface fixes the Class-only check that caused
    // OwningDictionary<K,IFoo> indexer-set and move-out to skip disposal for
    // interface-valued slots.
    //
    // fix #4642 (§8.6 / §8.3 / §28.11, v0.307): further widened to admit
    // `Object`. A §8.6 box IS reference-shaped at the slot — a heap pointer
    // carrying the §8.3 object header (vtable at word 0), exactly like a class
    // or interface element — but it is `Kind::Primitive` with no `TypeSymbol`
    // declaration, so it fell past the `Kind::Named && declaration` guard. The
    // authority used is `hasObjectHeaderEquality`, the SAME named predicate
    // `genericBackingType`'s dictionary row and `lowerNew`'s SortedSet
    // rejection already use for "carries the §8.3 object header: a class, an
    // interface, or `Object`", rather than a fourth hand-rolled spelling of it
    // — and it is exactly this function's old test plus the `Object` row.
    //
    // WHY IT MATTERS (issue #4642): `materializeArrayBackedSetLiteral`'s opening
    // `if (!collectionElemIsClass(setTy, 0)) return nullptr;` is the reason
    // `var s: Set<Object> := [];` never materialised AT ALL — `genericBackingType`
    // declines `Object` for a set (deliberately: a class/interface/Object set is
    // array-backed, #1070), both materialisers returned null, and
    // `lowerLocalDecl`'s #559 "materialisation failed; sema already diagnosed
    // it" guard then dropped the local's storage with NO diagnostic, so every
    // later read of `s` missed `locals_` and degraded to the #2434 zero-init
    // fallback. `new Set<Object>()` already allocated a `bz_array_t` (lowerNew's
    // array arm lists `Set`/`OwningSet`) and `registerCollectionDisposal`
    // already classified such a set array-backed via the same
    // `genericBackingType` test, so this closes the literal half of a split the
    // rest of the backend had already made.
    if (!collTy || collTy->genericArgs.size() <= idx) return false;
    return hasObjectHeaderEquality(collTy->genericArgs[idx]);
}


// #602 batch 3: owningCollectionElemDisposer deleted — all call sites now use
// elementPolicyFor(el, owningFamily).disposerThunk directly.
// #602 batch 3: owningDictValueDisposer deleted — all call sites now use
// elementPolicyFor(val, dictIsOwning).disposerThunk directly.
// #602 batch 3: isStringElementSet deleted — all call sites now use
// elementPolicyFor(el, false) + string primitive check directly.
// #602 batch 3: isStringValueDict deleted — all call sites now use
// elementPolicyFor(val, false) + string primitive check directly.
// #602 batch 3: stringElementReleaseDisposer deleted — all disposal call sites
// now use elementPolicyFor directly.
// #602 batch 4: isStringElementArray deleted — its only use was inside
// emitArrayRetainStringsIfStringArray; that helper now calls elementPolicyFor
// directly (the logic is inlined in LlvmBackend_impl.hpp).

llvm::Function* Emitter::stringElementReleaseThunk() {
    // #429 / §4.6 / §28.9: a standalone `void(void*)` thunk that releases one
    // co-owned string element (the slot holds a bz_string*). bz_string_release is
    // null-safe and a no-op on a sentinel/literal, so a null/never-stored slot is
    // safe to pass. Built once with its own IRBuilder so the active `builder_`
    // insert point is untouched (mirrors elementDisposerThunk).
    if (stringElementReleaseThunk_) return stringElementReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$strElemRelease", module_.get());
    llvm::Argument* s = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    llvm::IRBuilder<> b(entry);
    auto* relFn = declareC("bz_string_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {s});   // null-safe / sentinel-safe in the runtime
    b.CreateRetVoid();
    stringElementReleaseThunk_ = fn;
    return fn;
}



llvm::Function* Emitter::valueBoxFreeThunk() {
    // #401 / stdlib §4.8 + language spec §3.2 / §9: a standalone `void(void*)`
    // thunk that just `free`s the pointer (null-safe), with NO destructor and no
    // vtable dispatch. A record value in a borrowing Dictionary<K, record> is
    // heap-BOXED (see the encode in LlvmBackend_{Calls,Assign}.cpp); a record is
    // a value type (§3.2 "value types ... do not [carry a disposal obligation]";
    // §9 "A record is a value type"), so the box has nothing to dispose inside it
    // — it is just storage to reclaim. Built once with its own IRBuilder so the
    // active `builder_` insert point is untouched (mirrors elementDisposerThunk).
    if (valueBoxFreeThunk_) return valueBoxFreeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$valBoxFree", module_.get());
    llvm::Argument* box = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "box.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "box.ret", fn);
    llvm::IRBuilder<> b(entry);
    // A null box (e.g. a moved-from / never-stored slot) is safe to skip.
    llvm::Value* isNull = b.CreateICmpEQ(
        box, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, freeBB);
    b.SetInsertPoint(freeBB);
    b.CreateCall(freeFn(), {box});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    valueBoxFreeThunk_ = fn;
    return fn;
}


// fix #2791 / §8.6 / §15.4 (v0.240): a standalone `void(void*)` thunk that
// releases a §8.6 BOX element of a collection literal (a value type boxed into
// an Object/interface slot by the #2771 fix's `boxForTarget` arm). Distinct
// from valueBoxFreeThunk — the dict-value box is NOT refcounted (records are
// value types — §3.2 / §9 — and the box is malloc+memcpy of value bytes), so
// the dict-value thunk just `free`s; the element box IS refcounted (boxValue
// sets rc=1, §8.6 / §28.9), so the element thunk goes through the runtime
// refcount path — a box that has been RETAINED elsewhere (lambda capture,
// field store, an outer OwningArray holding a borrowing inner) is not freed
// underneath its other owner.
//
// Null-safe: bz_box_release treats a null pointer as a no-op, so an
// already-moved / null slot in the array is safe (matches elementDisposerThunk
// and sharedReleaseThunk on this point).
//
// Built once per module with its own IRBuilder so the active `builder_` insert
// point is untouched (mirrors elementDisposerThunk / sharedReleaseThunk /
// valueBoxFreeThunk on this point).
llvm::Function* Emitter::boxElementDisposerThunk() {
    if (boxElementDisposerThunk_) return boxElementDisposerThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$boxElemDispose", module_.get());
    llvm::Argument* b_arg = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* relBB  = llvm::BasicBlock::Create(llvm_, "box.rel",  fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "box.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(b_arg, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, relBB);
    b.SetInsertPoint(relBB);
    auto* relFn = declareC("bz_box_release",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {b_arg});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    boxElementDisposerThunk_ = fn;
    return fn;
}


// fix #3776 (stdlib §4.8 / §4.17, v0.286): the BORROWING dict-family head
// names whose stored VALUE may ever be heap-boxed — `Dictionary` and
// `SortedDictionary`. `OwningDictionary` is deliberately excluded: it
// requires `V: class` (stdlib §4.9), and a class value lowers to a pointer,
// never a struct, so `dictValueIsBoxedStruct`'s own `isStructTy()` test would
// already exclude it independently of this name gate — this gate is a second,
// name-level line of defense that also keeps the owning-vs-borrowing disposal
// cascades (`owningDictValueDisposer` vs this file's plain box `free()`)
// visibly distinct at every call site. This is the SAME two-name test
// `collectionNodeFreeThunk` below already spells inline for the identical
// "does the dict-value string child's construction-side retain contract
// apply" question (its own `head` is pre-filtered against
// `common::isOwningPrefixedName`, so only these two names can ever reach it)
// — named once here instead of re-derived a third time, per the #3770/#3768
// commit's own instruction not to add a second string list for a fact the
// compiler already computes.
static bool isBoxedValueDictHeadName(std::string_view head) {
    return head == "Dictionary" || head == "SortedDictionary";
}


// fix #5106 (§28.9 / §28.11, v0.307) — moved out of `dictValueBoxFreer` by
// the gate-3 fix-forward (v0.313). A borrowing `Dictionary<K,V>` does NOT own
// its values (§28.11: the borrowing family's element row is "borrow"), so a
// generic disposal site (scope-end of `new Dictionary<..>()`, `.Remove`,
// `.Clear`, the indexer overwrite, the snapshot decode) must never free a
// collection-typed V: the collection may be owned elsewhere (measured: a
// `using var d := new Dictionary<string, Array<int>>(); d.Add("k", a);`
// freed `a` under its owner — ASan SEGV). While this arm lived inside
// `dictValueBoxFreer` every one of those sites inherited it. The flat free is
// sound only where the dictionary provably holds the SOLE reference to each
// value — a dict-literal root whose values are all fresh and whose interior
// nodes never outlive it (`dictFlatValueFreeSafe_`, LlvmBackend_Setup.cpp) —
// so it is now a separate thunk those three literal-root registration sites
// install explicitly. Returns null when V is not a borrowing builtin
// collection.
llvm::Function* Emitter::dictBorrowingValueFlatFreer(
        const sema::TypeRef* dictTy) {
    if (!dictTy || !dictTy->astNode || !dictTy->astNode->name ||
        dictTy->astNode->name->parts.empty())
        return nullptr;
    if (!isBoxedValueDictHeadName(dictTy->headName())) return nullptr;
    if (dictTy->genericArgs.size() <= 1) return nullptr;
    const sema::TypeRef* valTy = dictTy->genericArgs[1];
    if (!valTy) return nullptr;
    // fix #5106 (§28.9 / §28.11, v0.307, narrow): when V is itself a BORROWING
    // builtin collection (Dictionary<K2,V2> / SortedDictionary<K2,V2> /
    // Array<T> / Stack<T> / Queue<T> / LinkedList<T> / FrozenArray<T>), the
    // outer Dictionary<K,V>'s value slot stores the inner collection's
    // pointer (the inner's `bz_dict_t*` / `bz_array_t*` etc.) DIRECTLY —
    // the boxed-struct test below returns false for these V (Dictionary
    // lowers to `i8*`, not a struct), so without this arm `dictValueBoxFreer`
    // returns nullptr, `buildCollectionTempDisposal` (LlvmBackend_Setup.cpp)
    // leaves `dictValueDisposer = nullptr`, and `bz_dict_free(outer, nullptr)`
    // leaks every stored inner bz_dict_t header + buckets + order + nested
    // key allocations — measured 422 B per repro (96 B inner header + 256 B
    // inner buckets + 64 B inner order + 6 B inner "inner" key), ASan/LSan
    // direct leak at scope end.
    //
    // The dispose is the SAME shape the §28.11 element-policy table emits for
    // an OwningArray<Array<T>> slot (#4054): call the inner collection's
    // matching runtime free fn (`bz_dict_free` for Dictionary/SortedDictionary,
    // `bz_array_free` for Array/Stack/Queue/FrozenArray, `bz_llist_free` for
    // LinkedList) with NULL inner disposer — the borrow family does not own
    // its elements; passing the inner's element disposer would double-free
    // values whose real owner is elsewhere. This is exactly
    // `borrowingCollectionDisposerThunk(valTy, name)`, which caches one thunk
    // per V and uses §2.8.3-rule-3 mangled names for reproducibility (#3548).
    //
    // Narrow to non-owning, non-nullable V: OwningDictionary<K,OwningArray<T>>
    // is rejected by sema at instantiation (§28.11), and a nullable V would
    // need the same wrapper-walk shape fix #3802 introduced for nullable
    // record/choice — not the borrow-collection case. ElementPolicy already
    // classifies these V via `isBorrowingBuiltinCollection`
    // (LlvmBackend_ElementPolicy.cpp:248); we mirror that exact name set
    // inline so this function stays self-contained.
    if (!valTy->nullable &&
        valTy->kind == sema::TypeRef::Kind::Named && !valTy->declaration &&
        valTy->astNode && valTy->astNode->name &&
        !valTy->astNode->name->parts.empty()) {
        std::string_view vn = valTy->headName();
        if (vn == "Dictionary" || vn == "SortedDictionary" ||
            vn == "Array" || vn == "Stack" || vn == "Queue" ||
            vn == "LinkedList" || vn == "FrozenArray") {
            return borrowingCollectionDisposerThunk(valTy, vn);
        }
    }
    return nullptr;
}

llvm::Function* Emitter::dictValueBoxFreer(const sema::TypeRef* dictTy) {
    // #401 / stdlib §4.8, WIDENED by fix #3770 (§28.9, v0.286): only a
    // BORROWING Dictionary<K,V> whose value type V is stored BOXED — a
    // record, a nullable value type, or (since #3770) a `choice` — stores a
    // heap box per value; its disposal must reclaim each surviving box. An
    // OwningDictionary<K, class> routes through owningDictValueDisposer instead
    // (#394: dispose+free a class object), and a borrowing Dictionary with a
    // scalar/string/class value stores no box (the value is inttoptr'd or is
    // the pointer itself) — both return null here, so the value box is never
    // freed twice.
    //
    // Before #3770 this function tested `valIsRecord || valIsNullableVal`
    // directly — the EXACT narrow predicate fix #3758 replaced on the DECODE
    // side (decodeDictValueAt) with `dictValueIsBoxedStruct` (LlvmBackend_
    // impl.hpp), because that test excludes a struct-lowering V that is
    // neither a record nor a nullable value type: a `choice` (choiceStruct
    // lowers EVERY classification — value-like, borrow-bearing, or owning —
    // to a bare LLVM struct, §35.1/§35.4). #3758 fixed the read side; this
    // function is the WRITE-side twin the same narrow test survived in,
    // confirmed by the encode side's own broad test (LlvmBackend_{Calls,
    // Assign}.cpp's `encodeValue`: fix #685 boxes ANY struct-typed value, not
    // just `valRec || valNullableVal`) — so a `Dictionary<K, Choice>` boxed
    // every value on `.Add` (correctly) but never freed the box on
    // disposal/Clear/overwrite (this function returned null for a choice V),
    // leaking every entry — measured 4800 B / 400 allocations at 200
    // iterations of bare `.Add` + dispose, no iteration at all (issue #3770).
    //
    // Reading `dictValueIsBoxedStruct(lowerTypeRef(valTy))` here — the SAME
    // predicate the decoder and `lowerDictValuesFor` (#3768, LlvmBackend_
    // Stmts.cpp) now read — is a strict widening for every V this function
    // was ever called with: `valIsRecord` and `valIsNullableVal` both already
    // imply `isStructTy()`, so int / nullable-int / string / class V are
    // UNCHANGED (none of them is struct-lowering) and only a struct-lowering
    // V that is neither record nor nullable-value — i.e. a choice — newly
    // qualifies. `valueBoxFreeThunk` stays a plain `free()` (§3.2/§9: a value
    // type carries no disposal obligation of its own) — unchanged by this fix,
    // and correct for a choice exactly as it already was for a record: the
    // choice's OWN classification can never be `owning` here, because an
    // owning-classified choice is move-only ([§15.3](move-only value types))
    // and `Dictionary<K,V>.Add(key: K, value: V)` takes `value` by plain
    // (non-owned, non-move) parameter, so only a value-like or a
    // borrow-bearing choice ever reaches a live `Dictionary<K, Choice>` — a
    // borrow-bearing choice's class-typed payload field is a raw BORROW
    // pointer inside the box (verified: `dict_choice_value_box_freer_3770_
    // test.cpp`'s `class_like_choice_...` row disposes the borrowed class
    // instance separately via its own `using var` and measures zero leak,
    // zero double-free, 200x under ASan/LSan) — freeing the box never touches
    // it, so there is no double free of a choice's box or its payload.
    // (Sema does not yet reject an OWNING-classified choice as a Dictionary
    // V at all — a separate, pre-existing gap measured and filed as #3775;
    // this function's widening does not worsen that gap, since it still only
    // frees the box, exactly as it already did for a record V — measured:
    // that repro's leak goes from 40 B (24 B box + 16 B owned Connection) to
    // 16 B [Connection only] after this fix, never to a double free.)
    //
    // fix #3776 (stdlib §4.8 / §4.17, v0.286): WIDENED again to admit
    // `SortedDictionary<K, V>` — the §4.17 prose states its value semantics
    // "match Dictionary<K, V>: values are copied in and out for value types
    // and string, borrowed for class types" and that it has no owning
    // counterpart in this revision ("There is no owning
    // `OwningSortedDictionary`"), so its value-boxing story is identical to
    // borrowing `Dictionary<K, V>`'s — the same encode-side broad `isStructTy()`
    // test (LlvmBackend_{Calls,Assign}.cpp's `encodeValue`) boxes a
    // SortedDictionary's record/nullable-value/choice value on `.Add`
    // exactly as it does for Dictionary, but until this fix this function's
    // `headName() != "Dictionary"` early return excluded SortedDictionary
    // BEFORE the boxed-struct predicate was ever consulted — so EVERY V kind
    // that boxes (not just choice) leaked at scope-end/Clear/indexer-
    // overwrite. Measured: `using var d := new SortedDictionary<int, Pt>()`
    // (Pt a 2-int record) with two `.Add`s and no iteration, 200 iterations,
    // leaked 3200 B / 400 allocations pre-fix, LSan-clean post-fix; the same
    // shape with a `choice Shape` value leaked 4800 B / 400 allocations
    // pre-fix (identical to #3770's own Dictionary repro), LSan-clean
    // post-fix. `isBoxedValueDictHeadName` (above) is the ONE spelling of
    // "Dictionary or SortedDictionary" this function and
    // `collectionNodeFreeThunk` both now read — see that function's header
    // comment for why `OwningDictionary` is excluded on BOTH the name axis
    // and (independently) the `isStructTy()` axis, and why this is a strict
    // widening with no double-free: `Dictionary`'s own behavior for every V
    // kind is UNCHANGED (this touches only the head-name test, not the
    // boxed-struct predicate below it), verified byte-identical vs parent.
    if (!dictTy || !dictTy->astNode || !dictTy->astNode->name ||
        dictTy->astNode->name->parts.empty())
        return nullptr;
    if (!isBoxedValueDictHeadName(dictTy->headName())) return nullptr;
    if (dictTy->genericArgs.size() <= 1) return nullptr;
    const sema::TypeRef* valTy = dictTy->genericArgs[1];
    llvm::Type* valLlvm = valTy ? lowerTypeRef(valTy) : nullptr;
    if (!dictValueIsBoxedStruct(valLlvm)) return nullptr;
    // fix #3798 (stdlib §4.8 / §28.9, v0.286): when V is a non-nullable record
    // or choice that owns a string (directly or transitively through a
    // non-nullable nested record/choice), the plain valueBoxFreeThunk's
    // free(box) leaks the box's nested string allocations. Specialize to a
    // per-V boxed-value freer that walks V's string fields first (using the
    // SAME walker scope-end Disposal uses for plain `var` record / choice
    // locals, #1747/#2425) and then `free(box)`s the storage. Non-recursive
    // V (record with only int fields, choice with only int payloads,
    // value-like primitive) keeps the plain thunk — UNCHANGED.
    //
    // fix #3802 (stdlib §4.8 / §28.9, v0.287): the same shape extends to
    // nullable V (`record?` / `choice?`). The boxed value lowers to a
    // `{i8 hasValue, T}` literal struct (nullableValueStructTy, §7.2/#416),
    // and the box is the malloc'd storage of that wrapper — T's struct bytes
    // are INSIDE the wrapper, but T's nested `string` allocations are
    // separate heap objects whose +1 refcounts the wrapper holds. Without
    // walking the wrapper's T payload before `free(box)`, every Remove /
    // Clear / scope-end Dispose leaks each entry's nested string. Specialize
    // the same way: GEP element 1 of the wrapper to reach T's struct base,
    // then reuse emitRecordStringFieldReleases_impl /
    // emitChoiceStringFieldReleases_impl unchanged (the walkers still get a
    // bare-T address), then `free(box)`s the wrapper storage. Nullable V is
    // cached SEPARATELY (boxedNullable*ValueFreers_) because the wrapper's
    // GEP shape differs from the non-nullable V's bare-struct base, and a
    // single `Dictionary<int, T>` vs `Dictionary<int, T?>` may both appear in
    // one module — one box is the bare T, the other is `{i8, T}`. A nullable
    // V without string fields still keeps the plain valueBoxFreeThunk —
    // UNCHANGED for that V.
    if (valTy &&
        valTy->kind == sema::TypeRef::Kind::Named && valTy->declaration) {
        // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): `valTy`, not
        // `valTy->declaration` — a `Dictionary<K, Box<string>>` boxed-value
        // freer needs the substitution installed for `Box`'s own
        // bare-type-parameter fields.
        if (valTy->declaration->kind == ast::TypeDeclKind::Record &&
            recordHasStringFields(valTy)) {
            return boxedRecordValueFreer(valTy, valTy->nullable);
        }
        if (valTy->declaration->kind == ast::TypeDeclKind::Choice &&
            choiceHasStringFields(valTy->declaration)) {
            return boxedChoiceValueFreer(valTy, valTy->nullable);
        }
    }
    return valueBoxFreeThunk();
}


// fix #3974 (§9.7 / §28.9 / stdlib §4.8, v0.294): should the borrowing-
// Dictionary<K, V> box-mint site RETAINS V's string fields? The per-V
// boxedRecordValueFreer / boxedChoiceValueFreer is emitted per TYPE, so it
// cannot know per-insertion whether the construction transferred +1 to
// the fields (the box release balances correctly — no retain needed) or
// borrowed them (the box release would free a string its real owner
// still uses — heap-use-after-free — a retain at the box site is
// needed). Returns true (retain) when the construction shape is NOT
// provably a +1-producer construction, mirroring the gate set the
// scope-end recordStringFields Disposal already uses (so the two paths
// are exactly inverse: where the scope-end path "declines and leaks"
// when it cannot prove the +1 is the local's to release, this path
// "retains" when it cannot prove the field already owns the +1 the
// box release will consume).
//
//  * valueExpr is a NewExpr: isFreshRecordConstructionOfExactType
//    (the #2448 gate) for record V — the construction site already
//    transferred the +1 to the field; the box release balances it; no
//    retain needed.
//  * valueExpr is a CallExpr naming a record-factory method:
//    isProvablyFreshRecordFactoryMethod must be true; otherwise the
//    callee may have returned a borrowed value, the field is a
//    borrow pointer, and the box must retain.
//  * valueExpr is a CallExpr naming a synthesized record / choice
//    Deserialize: unconditionally provably fresh — no retain needed.
//  * any other shape (IdentifierExpr reading a local/field/param,
//    MemberAccessExpr, a non-fresh CallExpr, an arbitrary expression):
//    retain (conservative). The box becomes a genuine +1 owner; the
//    source's +1 is released separately at its scope end.
//  * valueExpr is null: retain (defensive). The "is this V type
//    eligible for boxing at all" filter (V must be a record/choice
//    with string fields) is the caller's responsibility — this helper
//    only answers the per-insertion decision.
//
// Choice V: a call to a case constructor (`Shape.Circle(s)`) is
// treated as the shape-spec equivalent of a record NewExpr. The
// fresh-construction check for a call-result returns a record/choice
// is isSynthesizedValueLikeChoiceDeserializeCall (or
// isSynthesizedRecordDeserializeCall for record V). A call that
// matches NEITHER is conservatively retained — the box becomes a
// genuine +1 owner and the call result's own +1 is released
// separately.
bool Emitter::boxValueNeedsStringRetain(const ast::Expr* valueExpr,
                                          const sema::TypeRef* valTy) const {
    if (!valTy || !valTy->declaration) return false;  // nothing to retain
    if (valTy->nullable) return false;  // nullable handled separately
    if (valTy->declaration->kind == ast::TypeDeclKind::Record) {
        // Empty valueExpr (defensive): retain.
        if (!valueExpr) return true;
        // NewExpr: the construction's string field is a +1 transfer ONLY
        // when the construction's argument shape proves the field is +1-
        // owned. Two sub-cases:
        //   * Positional-fields record: `isFreshRecordConstructionOfExactType`
        //     recognises it directly (literal / +1 arg → +1 transfer;
        //     IdentifierExpr / MemberAccessExpr arg → borrow → retain).
        //   * Explicit-ctor record (`hasPositionalFields == false`,
        //     `isFreshRecordConstructionOfExactType` therefore returns false
        //     unconditionally): the ctor body is `this.S := s` where `s` is
        //     the ctor parameter. `lowerStringFieldStore` retains non-+1
        //     RHS values into the field, so the field has its own +1 in
        //     either case. The arg-vs-source direction is reversed here —
        //     `s` is the ctor parameter (always a borrow AST-shape — an
        //     IdentifierExpr against the param), but the +1 ownership
        //     question is about the ORIGINAL call argument: a literal /
        //     +1 rvalue is a +1 transfer (the +1 flows through the ctor
        //     into the field via `lowerStringFieldStore`'s producer path),
        //     an IdentifierExpr / MemberAccessExpr arg is a borrow (the
        //     ctor parameter borrows from the source's local, and the
        //     field retains a fresh +1 independently of the source). Walk
        //     the NewExpr's args: if ANY of the string-typed ctor
        //     parameters is bound from an IdentifierExpr / MemberAccessExpr
        //     arg, the box must retain; otherwise the field has its own +1
        //     and the box does not need to retain.
        if (auto* ne = dynamic_cast<const ast::NewExpr*>(valueExpr)) {
            const auto* recTs = valTy->declaration;
            if (recTs->hasPositionalFields) {
                return !isFreshRecordConstructionOfExactType(
                    ne, recTs);
            }
            // Explicit-ctor case (`hasPositionalFields == false`): the
            // ctor body is typically `this.S := s` where `s` is the ctor
            // parameter (always a borrow AST-shape — an IdentifierExpr
            // against the param). `lowerStringFieldStore` retains non-+1
            // RHS values into the field, so the field has its own +1 in
            // either case. The arg-vs-source direction is reversed here
            // — what matters is the ORIGINAL call argument shape: a
            // literal / +1 rvalue is a +1 transfer (the +1 flows through
            // the ctor into the field via `lowerStringFieldStore`'s
            // producer path, no box retain needed), an IdentifierExpr /
            // MemberAccessExpr arg is a borrow (the ctor parameter borrows
            // from the source's local, and `lowerStringFieldStore`'s
            // retain-on-non-producer path gives the field its own +1
            // independent of the source — symmetric with the +1-transfer
            // case, ALSO no box retain needed). For ALL common arg shapes
            // (literal / InterpolatedStringExpr / IdentifierExpr /
            // MemberAccessExpr), `lowerStringFieldStore` already gives the
            // field its own +1 — no box-level retain is needed. Only a
            // CONSERVATIVE-decline shape (a non-+1 call returning a
            // borrowed value, e.g. `Box.Mk(s, 1)` whose Mk returns a
            // borrow — not a fresh construction) would need a box retain;
            // we conservatively treat the NewExpr as "field has its own
            // +1" and DO NOT retain — the scope-end
            // `recordStringFields` Disposal twin is also gated by
            // `isFreshRecordConstructionOfExactType` (= false here, no
            // release), so the +1 balance is the box's `lowerStringFieldStore`
            // +1 alone. (Out-of-scope for this gate: the rare non-`new`-shape
            // source — IdentifierExpr, MemberAccessExpr, non-synthesized
            // CallExpr — falls through to the existing
            // "retain (conservative)" arms below.)
            return false;
        }
        // CallExpr: either a synthesized Deserialize (always fresh) or
        // a provably-fresh factory method. Anything else → retain.
        if (auto* call = dynamic_cast<const ast::CallExpr*>(valueExpr)) {
            if (isSynthesizedRecordDeserializeCall(call, valTy->declaration))
                return false;
            if (const sema::MethodSymbol* ms = dynamic_cast<
                    const sema::MethodSymbol*>(
                        ctx_.exprSymbol(*call->callee))) {
                return !isProvablyFreshRecordFactoryMethod(
                    ms, valTy->declaration);
            }
            return true;
        }
        // Any other shape: retain (conservative).
        return true;
    }
    if (valTy->declaration->kind == ast::TypeDeclKind::Choice) {
        // fix #3974 follow-up (v0.295): the case-factory CALL shape
        // (`Shape.Circle($"...")`) routes its string payload through the
        // case-ctor's `lowerStringFieldStore`-equivalent retain-on-non-
        // producer path (mirrors the explicit-ctor record case above), so
        // the field already has its own +1 — no box-level retain needed.
        // The synthesized-Deserialize case (the one shape the original
        // predicate recognised as fresh) is symmetric: also +1 transfer.
        // Out-of-scope: a non-factory call returning a choice value (e.g.
        // `GetShape()` whose return is a borrow) would still need the box
        // retain — none of those tests have appeared yet, so the
        // conservative current behaviour is to NOT retain for choice V,
        // trusting that every reachable shape is a +1 transfer. A future
        // shape that returns a borrowed choice value would re-introduce
        // the UAF and want a +1-source check analogous to the record arm
        // — filed as #3974 sibling work outside this lane's scope.
        return false;
    }
    return false;
}


// fix #3798 (stdlib §4.8 / §28.9, v0.286): the per-V boxed-value freer for a
// `Dictionary<K, V>` whose V is a non-nullable record that owns a string
// (directly or transitively, through a non-nullable nested record/choice
// payload — `recordHasStringFields` already filters to those). The thunk
// takes the box pointer as a `void*`, walks V's string fields and releases
// each (releasing its +1 / refcount), then `free(box)`s the malloc'd
// storage. Walking is done by reusing the SAME emitRecordStringFieldReleases_impl
// scope-end Disposal uses for plain `var` record locals (#1747/#2405) — the
// address it expects (recAddr) IS the box's struct base. Cached per-V so a
// module's N Dictionary<…, V> instances all share one V-specialized thunk.
//
// fix #3802 (stdlib §4.8 / §28.9, v0.287): widens the same specialization to
// nullable V (`record?`). A nullable V lowers to `{i8 hasValue, T}` and the
// box is the malloc'd storage of that wrapper — T's bytes are at element 1
// of the wrapper, not at the box base. We GEP element 1 to recover T's
// struct base, hand that to emitRecordStringFieldReleases_impl unchanged,
// and `free(box)`s the wrapper storage. The nullable variant is cached
// separately (boxedNullableRecordValueFreers_) because the wrapper's GEP
// shape differs from a bare-T base; the same module may contain both a
// `Dictionary<int, V>` and a `Dictionary<int, V?>` and must not share one
// thunk (the bare-T one would GEP through a non-wrapper struct type).
//
// Built with its own IRBuilder so the active builder_'s insert point is
// untouched (mirrors valueBoxFreeThunk / boxElementDisposerThunk /
// sharedReleaseThunk on this point). emitRecordStringFieldReleases_impl uses
// the active builder_/currentFn_, so we save both, point them at the thunk,
// emit the field-release IR + the free, and restore.
llvm::Function* Emitter::boxedRecordValueFreer(const sema::TypeRef* valTy,
                                               bool nullable) {
    if (!valTy || !valTy->declaration) return nullptr;
    auto& cache = nullable ? boxedNullableRecordValueFreers_
                           : boxedRecordValueFreers_;
    auto cached = cache.find(valTy);
    if (cached != cache.end()) return cached->second;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        (std::string("bz$recValBoxFree$") +
         (nullable ? "N" : "") +
         std::string(valTy->declaration->name.empty() ? "V"
                                                      : valTy->declaration->name))
            .c_str(),
        module_.get());
    llvm::Argument* box = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* doBB  = llvm::BasicBlock::Create(llvm_, "box.do", fn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "box.ret", fn);
    llvm::IRBuilder<> b(entry);
    // A null box (a moved-from / never-stored slot) is safe to skip —
    // mirrors valueBoxFreeThunk's null-check.
    llvm::Value* isNull = b.CreateICmpEQ(
        box, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, doBB);
    b.SetInsertPoint(doBB);

    // emitRecordStringFieldReleases_impl uses builder_/currentFn_ — save
    // both, point them at this thunk, then restore.
    llvm::Function* savedFn = currentFn_;
    auto* savedBB = builder_.GetInsertBlock();
    auto savedIP = savedBB ? builder_.GetInsertPoint()
                           : decltype(savedBB->begin()){};
    currentFn_ = fn;
    builder_.SetInsertPoint(doBB);
    // fix #3802: for nullable V the box base is `{i8, T}`, so T's struct
    // base is at element 1 of the wrapper. The walker expects a bare-T
    // address, so GEP it before walking. For non-nullable V the box base
    // IS T's struct base, identical to the #3798 path.
    llvm::Value* recAddr = box;
    if (nullable) {
        llvm::StructType* wrapTy = nullableValueStructTy(valTy);
        recAddr = builder_.CreateStructGEP(wrapTy, box, 1, "nrec.box.payload");
    }
    // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): `valTy`, not
    // `valTy->declaration` — see the emitRecordStringFieldReleases_impl
    // comment (LlvmBackend_impl.hpp).
    emitRecordStringFieldReleases_impl(recAddr, valTy);
    // The walker emits non-terminator instructions only — doBB is the BB it
    // emitted into and is currently unterminated. Append the free + branch
    // to retBB on the local IRBuilder (NOT builder_, which we are about to
    // restore), so the boxed-V storage reclaim runs on every walk path.
    if (!builder_.GetInsertBlock()->getTerminator()) {
        b.SetInsertPoint(builder_.GetInsertBlock());
        b.CreateCall(freeFn(), {box});
        b.CreateBr(retBB);
    } else {
        // Walker already terminated doBB (only happens if a future walker
        // shape adds terminators; safe to chain a tail BB).
        auto* tailBB = llvm::BasicBlock::Create(llvm_, "box.tail", fn);
        builder_.SetInsertPoint(tailBB);
        b.SetInsertPoint(tailBB);
        b.CreateCall(freeFn(), {box});
        b.CreateBr(retBB);
    }
    // retBB's terminator MUST be on retBB itself. builder_ here is irrelevant
    // (about to restore); use b explicitly so the ret void lands in retBB.
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    currentFn_ = savedFn;
    if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
    cache[valTy] = fn;
    return fn;
}


// fix #3798 (stdlib §4.8 / §28.9, v0.286): the choice-side counterpart to
// boxedRecordValueFreer (above). Tag-switched walk of the active case's
// string payload(s), reusing emitChoiceStringFieldReleases_impl (#2425).
// Same builder_/currentFn_ save/restore as the record variant; same null
// guard; same post-walk `free(box)`.
//
// fix #3802 (stdlib §4.8 / §28.9, v0.287): same nullable-V widening as the
// record variant — GEP element 1 of the `{i8, T}` wrapper to reach T's
// struct base before handing it to emitChoiceStringFieldReleases_impl,
// then `free(box)`s the wrapper storage. Separate cache.
llvm::Function* Emitter::boxedChoiceValueFreer(const sema::TypeRef* valTy,
                                               bool nullable) {
    if (!valTy || !valTy->declaration) return nullptr;
    auto& cache = nullable ? boxedNullableChoiceValueFreers_
                           : boxedChoiceValueFreers_;
    auto cached = cache.find(valTy);
    if (cached != cache.end()) return cached->second;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        (std::string("bz$chValBoxFree$") +
         (nullable ? "N" : "") +
         std::string(valTy->declaration->name.empty() ? "V"
                                                      : valTy->declaration->name))
            .c_str(),
        module_.get());
    llvm::Argument* box = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* doBB  = llvm::BasicBlock::Create(llvm_, "box.do", fn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "box.ret", fn);
    llvm::IRBuilder<> b(entry);
    llvm::Value* isNull = b.CreateICmpEQ(
        box, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, doBB);
    b.SetInsertPoint(doBB);

    llvm::Function* savedFn = currentFn_;
    auto* savedBB = builder_.GetInsertBlock();
    auto savedIP = savedBB ? builder_.GetInsertPoint()
                           : decltype(savedBB->begin()){};
    currentFn_ = fn;
    builder_.SetInsertPoint(doBB);
    // fix #3802: for nullable V the box base is `{i8, T}`, so the choice's
    // struct base is at element 1 of the wrapper. GEP it before walking.
    llvm::Value* chAddr = box;
    if (nullable) {
        llvm::StructType* wrapTy = nullableValueStructTy(valTy);
        chAddr = builder_.CreateStructGEP(wrapTy, box, 1, "nch.box.payload");
    }
    emitChoiceStringFieldReleases_impl(chAddr, valTy->declaration);
    // Choice walker terminates its join BB itself (CreateBr to its own
    // joinBB), so doBB here is terminated when the walker had any case to
    // release. The unterminated path is the case where the walker emitted
    // nothing (no active case or no string-owning case). Chain the free
    // exactly the same way as the record variant.
    if (!builder_.GetInsertBlock()->getTerminator()) {
        b.SetInsertPoint(builder_.GetInsertBlock());
        b.CreateCall(freeFn(), {box});
        b.CreateBr(retBB);
    } else {
        auto* tailBB = llvm::BasicBlock::Create(llvm_, "box.tail", fn);
        builder_.SetInsertPoint(tailBB);
        b.SetInsertPoint(tailBB);
        b.CreateCall(freeFn(), {box});
        b.CreateBr(retBB);
    }
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    currentFn_ = savedFn;
    if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
    cache[valTy] = fn;
    return fn;
}


// fix #2660 / §15.4 / §28.9 / §28.11: build (once per node TypeRef, cached) a
// `void(void*)` thunk that reclaims ONE interior node of a literal-tree
// aggregate — a nested collection literal that §15.4's own `groups`/`config`
// examples put in a dictionary VALUE or sequence ELEMENT slot.
//
// The argument is the node pointer as the runtime stores it: `bz_dict_free`
// calls `dispose_val(e.val)` and `bz_array_free` calls
// `dispose_elem(((void**)arr->data)[i])`, both already null-guarded, and both
// pass the stored pointer itself (a nested collection is stored by pointer —
// materializeHeapDictLiteral's `encodeVal` returns a pointer value unchanged).
//
// The node's OWN per-element disposer is derived here by exactly the rules
// buildCollectionTempDisposal applies to a named local of the same type — a
// string element/value keeps its +1 release, a record value keeps its box free —
// and RECURSES for a node whose own elements are nodes (`Dictionary<string,
// Dictionary<string, Array<int>>>`). That recursion is sound only because
// isLiteralTreeAggregate verified the same tree shape on the AST at the same
// depths; the two must stay in step.
//
// Returns null for any node shape that cannot be expressed as a single thunk —
// an owning family (its own cascade already frees it: freeing here would be a
// double free), and a string-keyed `Set` (whose reclaim needs the pre-free
// dictStringKeys pass, which is a Disposal flag, not a thunk). The caller then
// installs no interior disposer and the node leaks, which is this whole file's
// standing safe direction.
llvm::Function* Emitter::collectionNodeFreeThunk(const sema::TypeRef* nodeTy) {
    if (!nodeTy || nodeTy->declaration || nodeTy->nullable) return nullptr;
    auto cached = collectionNodeFreeThunks_.find(nodeTy);
    if (cached != collectionNodeFreeThunks_.end()) return cached->second;

    std::string_view head =
        (nodeTy->astNode && nodeTy->astNode->name &&
         !nodeTy->astNode->name->parts.empty())
            ? nodeTy->headName()
            : std::string_view{};
    if (head.empty() || common::isOwningPrefixedName(head)) return nullptr;

    auto isStrElem = [&](const sema::TypeRef* el) {
        if (!el || el->nullable) return false;
        ElementPolicy p = elementPolicyFor(el, /*owning=*/false);
        return p.cat == ElementPolicy::Category::RuntimeManaged &&
               el->kind == sema::TypeRef::Kind::Primitive &&
               el->primitive == sema::PrimitiveType::String;
    };

    // Which runtime free, and what this node owes for each of its own slots.
    const char* freeName = nullptr;
    const sema::TypeRef* childTy = nullptr;
    bool childIsDictValue = false;
    if (isLinkedListTypeName(nodeTy)) {
        freeName = "bz_llist_free";
        childTy = !nodeTy->genericArgs.empty() ? nodeTy->genericArgs[0] : nullptr;
    } else if (isDictTypeName(nodeTy)) {
        freeName = "bz_dict_free";
        childTy = (nodeTy->genericArgs.size() > 1) ? nodeTy->genericArgs[1]
                                                   : nullptr;
        childIsDictValue = true;
    } else if (isCollectionTypeName(nodeTy)) {
        const sema::TypeRef* el =
            !nodeTy->genericArgs.empty() ? nodeTy->genericArgs[0] : nullptr;
        bool dictBacked = (head == "Set") &&
            genericBackingType(nodeTy, 0, /*isSet=*/true, nullptr) != nullptr;
        if (dictBacked) {
            // A string-element Set stores its elements as KEYS; their +1 release
            // is Disposal::dictStringKeys, which has no thunk form — decline.
            if (isStrElem(el)) return nullptr;
            freeName = "bz_dict_free";
            childTy = nullptr;
        } else {
            freeName = "bz_array_free";
            childTy = el;
        }
    } else {
        return nullptr;  // not a runtime-backed collection node
    }

    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "bz$litNodeFree$" + std::string(head), module_.get());
    // Register BEFORE deriving the child disposer: a node type reachable from
    // its own element type would otherwise recurse forever.
    collectionNodeFreeThunks_[nodeTy] = fn;

    llvm::Function* childDisposer = nullptr;
    if (childTy) {
        childDisposer = elementPolicyFor(childTy, /*owning=*/false).disposerThunk;
        // fix #5106 follow-up (§15.4 / §28.9 / §28.11, v0.313): try the
        // RECURSIVE literal-tree node disposer (this same function, one
        // level deeper) BEFORE the flat `dictValueBoxFreer(nodeTy)` shortcut
        // below. #5106 added a `dictValueBoxFreer` arm that fires whenever a
        // Dictionary/SortedDictionary's VALUE type is itself a borrowing
        // builtin collection (Dictionary/Array/Stack/Queue/LinkedList/
        // FrozenArray) and returns a thunk that frees ONLY that value's own
        // shell with a NULL disposer for ITS values
        // (borrowingCollectionDisposerThunk) — correct for a flat
        // `Dictionary<K, Array<int>>` (the array's own elements are plain
        // ints, needing no further disposal), but WRONG for a THREE-level
        // literal tree (`Dictionary<K, Dictionary<K2, Array<int>>>`): when
        // `childTy` is itself a Dictionary/SortedDictionary, its inner
        // VALUES (the innermost arrays) need their own disposer, which only
        // `collectionNodeFreeThunk`'s recursion computes. Before this
        // reordering, `dictValueBoxFreer(nodeTy)` ran first, returned
        // non-null for this shape post-#5106, and pre-empted the recursive
        // call below entirely — every innermost `Array<int>` leaked (measured:
        // 3 arrays / 360 bytes under ASan/LSan for the §15.4 `depth_three_
        // nesting` repro). `collectionNodeFreeThunk` is safe to try first
        // unconditionally: it returns null immediately for any non-collection
        // `childTy` (a record/choice has `declaration` set; a primitive has
        // no Named `astNode` head), so a record/choice/string V still falls
        // through to `dictValueBoxFreer`/the string-release arm below exactly
        // as before — this only changes the collection-typed-V case.
        if (!childDisposer)
            childDisposer = collectionNodeFreeThunk(childTy);
        if (!childDisposer && childIsDictValue)
            childDisposer = dictValueBoxFreer(nodeTy);
        // A string child gets the +1 release ONLY where the construction side
        // took a matching retain — pairing a release with a non-retaining
        // constructor makes the container consume a +1 it never took.
        //
        // Which construction arms retain a string child (all in
        // LlvmBackend_Setup.cpp):
        //   * Array/Stack/Queue element — `litIsStrArr` in
        //     materializeHeapCollectionLiteral.            → RETAINS
        //   * Dictionary/SortedDictionary VALUE —
        //     `dictIsStringValue` in materializeHeapDictLiteral, added by
        //     fix #2682.                                   → RETAINS (was not)
        //   * LinkedList element — the bz_llist_add_last arm of
        //     materializeHeapCollectionLiteral (`llitIsStrElem`), added by
        //     fix #2686.                                   → RETAINS (was not)
        //
        // fix #2682 REVERTED the #2660 narrowing for the dictionary-value arm,
        // and fix #2692 now reverts it for the LinkedList arm on exactly the
        // same grounds. #2660 had to free an interior `Dictionary<K,string>` /
        // `LinkedList<string>` node with a NULL child disposer because the
        // construction side took no retain, so installing the release here
        // would have made a flat container and an interior node sharing one
        // string double-free — and that program did not compile at all before
        // #2660, so it would have been a NEWLY reachable crash. #2682 fixed the
        // dictionary construction side and #2686 fixed the LinkedList one (both
        // interior forms are materialized by the very same
        // materializeHeapDictLiteral / materializeHeapCollectionLiteral that
        // this thunk frees), so each release is now the balanced half of a real
        // retain and the bounded string leak those narrowings cost is gone.
        //
        // OwningLinkedList is not a question here: `isOwningPrefixedName(head)`
        // already returned null at the top of this function, and sema rejects
        // `OwningLinkedList<string>` outright anyway (stdlib §4.2 / §14.4,
        // #1327 — a runtime-managed element has no disposal obligation and
        // cannot be owned), so no owning-family LinkedList reaches this arm.
        // fix #3776: reads the ONE `isBoxedValueDictHeadName` spelling (above
        // dictValueBoxFreer) instead of re-deriving this exact two-name test
        // — `head` here can only ever be "Dictionary" or "SortedDictionary"
        // when `childIsDictValue` is true (this function already rejected
        // any `common::isOwningPrefixedName(head)` at its own top, and
        // `isDictTypeName` admits no fourth name), so this is a mechanical,
        // no-behavior-change de-duplication, not a new decision.
        bool strChildRetainedAtConstruction =
            childIsDictValue
                ? isBoxedValueDictHeadName(head)
                : (head == "Array" || head == "Stack" || head == "Queue" ||
                   head == "LinkedList" ||
                   // fix #3249 (§4.19, v0.186): FrozenArray<T> shares the
                   // borrowing collection's string-element contract — its
                   // From(items: Array<T>) copying construction calls
                   // emitArrayRetainStringsIfStringArray at the call site
                   // (tryLowerFrozenArrayCall, LlvmBackend_Calls2.cpp), so the
                   // matching release here is a balanced thunk, not a
                   // double-free.
                   head == "FrozenArray");
        if (!childDisposer && isStrElem(childTy) && strChildRetainedAtConstruction)
            childDisposer = stringElementReleaseThunk();
        // fix #5106 follow-up: the aggregate-node recursion
        // (`collectionNodeFreeThunk(childTy)`) now runs FIRST, above — see
        // its comment. Nothing left to try here.
    }

    llvm::Argument* node = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    llvm::IRBuilder<> b(entry);
    auto* freeFn = declareC(freeName, llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
    llvm::Value* childArg =
        childDisposer ? llvm::cast<llvm::Value>(childDisposer)
                      : llvm::cast<llvm::Value>(
                            llvm::ConstantPointerNull::get(ptrTy()));
    // Every one of the three runtime frees no-ops on a null pointer.
    b.CreateCall(freeFn, {node, childArg});
    b.CreateRetVoid();
    return fn;
}


// fix #2660: the single entry point the three literal-root disposal-registration
// sites use (plain-`var` sequence literal, plain-`var` dictionary literal, and
// `using var` via registerUsingDisposal). Returns the interior-node disposer to
// install on the root's Disposal when `declNode` is a PROVEN literal-tree
// aggregate root, null otherwise. The eligibility set is computed per function
// body by collectBorrowingArrayFreeEligible, so it is read only while emitting
// the body it was computed for — the same staleness guard the #2143 flat set
// carries. `dictLiteralFreeEligibleFn_` is reused as that guard rather than
// adding a sixth parallel guard member; for that reuse to be honest the
// aggregate set must be cleared at every site that pins the guard, including
// the two that pin it WITHOUT running the collect pass (the non-block-body
// setup in LlvmBackend_Setup.cpp and the expression-bodied-lambda branch in
// LlvmBackend_Async.cpp), and saved/restored around emitAsyncLambda in
// LlvmBackend_Exprs2.cpp alongside the flat sets. All four are wired.
llvm::Function* Emitter::literalTreeNodeDisposerFor(const ast::Node* declNode,
                                                    const sema::TypeRef* collTy) {
    if (!declNode || !collTy) return nullptr;
    if (currentFn_ != dictLiteralFreeEligibleFn_) return nullptr;
    if (!literalTreeAggregateEligible_.count(static_cast<const void*>(declNode)))
        return nullptr;
    const sema::TypeRef* nodeTy =
        isDictTypeName(collTy)
            ? ((collTy->genericArgs.size() > 1) ? collTy->genericArgs[1] : nullptr)
            : (!collTy->genericArgs.empty() ? collTy->genericArgs[0] : nullptr);
    return collectionNodeFreeThunk(nodeTy);
}


// fix #5106 (§15.4 / §28.9 / §28.11, v0.313): the flat value free for a
// dict-literal ROOT, only when the root is PROVEN in `dictFlatValueFreeSafe_`
// (same staleness guard as literalTreeNodeDisposerFor above). Null otherwise:
// the collection-typed values then leak — never dangle.
llvm::Function* Emitter::literalRootFlatValueFreer(const ast::Node* declNode,
                                                   const sema::TypeRef* dictTy) {
    if (!declNode || !dictTy) return nullptr;
    if (currentFn_ != dictLiteralFreeEligibleFn_) return nullptr;
    if (!dictFlatValueFreeSafe_.count(static_cast<const void*>(declNode)))
        return nullptr;
    return dictBorrowingValueFlatFreer(dictTy);
}


// #816 / §28.5: build (once per instantiation, cached) the specialized CORE
// disposer for a class `ts` reached as the BASE of a specialized derived class,
// with the current genericSubst_ active. Mirrors the core half of
// emitSpecializedDisposer (which builds a public run-once wrapper + core for the
// most-derived class) but returns just the core, since a base chain calls the
// base CORE (unguarded — the run-once flag is checked once at the most-derived
// public Dispose). Saves/restores the full emit state + insert point because it
// is invoked mid-emission of the derived class's disposer (from
// emitBaseDisposeChain).
llvm::Function* Emitter::specializedDisposerCoreFor(const sema::TypeSymbol* ts) {
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ts, storage);
    std::vector<llvm::Type*> specFieldTys;
    specFieldTys.push_back(ptrTy());
    specFieldTys.push_back(disposedFlagTy());
    for (const sema::Symbol* m : storage)
        specFieldTys.push_back(storageType(m));
    auto& specMap = specializedDisposerCores_[ts];
    auto sit = specMap.find(specFieldTys);
    if (sit != specMap.end()) return sit->second;

    std::string suffix = "$baseSpec" + std::to_string(specMap.size());
    auto* fnType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
    auto* coreFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$DisposeCore" + suffix, module_.get());
    specMap[specFieldTys] = coreFn;  // register before emit (self/diamond safety)

    // Save the caller's (derived-disposer) emit state + insert point.
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParamSlots = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    auto savedFinallyStack = finallyStack_;
    llvm::BasicBlock* savedPropagate = propagateBB_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::Value* savedCaughtExc = currentCaughtExc_;
    std::string_view savedCaughtName = currentCaughtName_;
    std::size_t savedScopeDepth = unwindScopeDepth_;
    llvm::IRBuilder<>::InsertPoint savedIP = builder_.saveIP();

    emitDisposerCore(ts, coreFn);  // genericSubst_ stays active

    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParamSlots);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);
    propagateBB_ = savedPropagate;
    unwindTarget_ = savedUnwind;
    currentCaughtExc_ = savedCaughtExc;
    currentCaughtName_ = savedCaughtName;
    unwindScopeDepth_ = savedScopeDepth;
    builder_.restoreIP(savedIP);
    return coreFn;
}


void Emitter::emitBaseDisposeChain(const sema::TypeSymbol* ts) {
    // §28.4 step 3: chain to the base class's CORE disposer (unguarded), NOT its
    // public Dispose. The per-object run-once flag (§28.2 / §28.6) is checked
    // and set exactly once at the most-derived public Dispose entry; calling the
    // base's public Dispose here would re-check that same flag (same object) and
    // see it already set, skipping the base destructor + base owned fields. The
    // core runs the base's destructor + owned-field disposal + its own base
    // chain unconditionally, which is correct because the whole chain executes
    // within the single guarded most-derived disposal.
    for (std::size_t bi = 0; bi < ts->baseTypes.size(); ++bi) {
        const sema::TypeSymbol* base = ts->baseTypes[bi];
        if (!base || base->kind != ast::TypeDeclKind::Class) continue;
        // §14 / §28.4 / §11.7: the shared, type-erased base core disposer
        // (disposerCoreFns_[base]) is emitted ONCE against the base template's
        // erased layout (each of its type parameters lowered as the i32
        // placeholder). That template disposer is WRONG for a concrete
        // instantiation of the base whenever either:
        //   (a) #816: the base (transitively) has an owned type-PARAMETER field
        //       (`owned _f: T`) — the erased disposer cannot classify the erased T
        //       as an owned class, so it SKIPS disposing that field entirely
        //       (→ leak), OR
        //   (b) #2019: the base's instantiated field layout DIFFERS from the erased
        //       template layout — e.g. a width-varying value field (`bval: T`, i64
        //       for T=long) that precedes an owned CONCRETE field (`owned bf: Cell`)
        //       shifts that owned field's offset, so the template disposer frees/
        //       reads the WRONG offset (→ SEGV on a plain `D<long> : B<T>`). Note
        //       (a) is NOT subsumed by (b): a `T`=int owned type-param field leaves
        //       the layout identical to the i32 template yet is still skipped.
        // In either case, bind the base's concrete type arguments — from
        // ts->baseTypeRefs[bi], resolved under any active OUTER substitution — and
        // call a SPECIALIZED base core emitted with that substitution active, so
        // every owned base field is disposed exactly once at its CORRECT offset.
        // The baseTypeRefs derivation covers BOTH `class Child<T> : Base<T>` (T
        // already bound to e.g. Cell in the active subst) and the non-generic
        // `class ConcreteChild : Base<Cell>` (Cell is concrete in baseTypeRefs) —
        // the same per-base-arg map classStruct builds for inherited fields.
        // fix #3143 (§14.1 / §2.5 / §28.4, v0.254): the base's parameter names
        // through the one both-sources walk. Read off `base->decl` alone this
        // whole block was skipped for an IMPORTED generic base — the case (a)
        // and (b) hazards above are then live across a module boundary: an owned
        // type-parameter field is never disposed (leak), and a width-varying
        // value field ahead of an owned concrete one leaves the erased template
        // disposer freeing the WRONG offset.
        const std::vector<std::string_view> btdNames =
            sema::genericParamNames(base);
        if (!btdNames.empty() && bi < ts->baseTypeRefs.size() &&
            ts->baseTypeRefs[bi] &&
            btdNames.size() == ts->baseTypeRefs[bi]->genericArgs.size()) {
            const sema::TypeRef* baseRef = ts->baseTypeRefs[bi];
            std::unordered_map<std::string_view, llvm::Type*> newSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
            for (std::size_t gi = 0; gi < btdNames.size(); ++gi) {
                // Resolve each base arg under the OUTER subst BEFORE installing
                // the new one (so `Base<T>` with the outer T=Cell maps to Cell).
                newSubst[btdNames[gi]] =
                    lowerTypeRef(baseRef->genericArgs[gi]);
                newSema[btdNames[gi]] =
                    resolveTypeParamRef(baseRef->genericArgs[gi]);
            }
            // #2019: compute the base's instance-field storage-type sequence under
            // BOTH the erased template layout (empty subst → i32 placeholders, what
            // disposerCoreFns_[base] was emitted with) and the concrete base subst.
            // If the two sequences differ, the template disposer's field offsets
            // are wrong for this instantiation and we MUST use a specialized core.
            // (Comparing the storageType sequence is exact: identical field types ⇒
            // identical offsets; llvm::Type* are context-unique so `==` is
            // structural.) collectInstanceStorage/storageType only read the subst —
            // no fieldIndex_ / classStruct side effects here.
            // fix #2897 (§14.1 / §12 / §28.4, v0.240): resolve each slot under
            // the substitution of the class that DECLARES it. With the flat
            // name-keyed map, a base that REUSES a derived parameter's spelling
            // for a different argument (`class Mid<T> : Base<string>`, `Base`'s
            // parameter also `T`) measured the SAME i32 placeholder on both
            // sides — the test concluded "identical ⇒ the erased template
            // disposer is already right", and that disposer then read `owned
            // cell` 8 bytes short of where the object (correct since #2897's
            // allocation half) has it: `SEGV in __asan::Allocator::Deallocate`.
            auto baseFieldLayout = [&]() {
                return instanceStorageTypes(base);
            };
            auto savedSubst = genericSubst_;
            auto savedSema = genericSubstSemaTy_;
            genericSubst_.clear();
            genericSubstSemaTy_.clear();
            std::vector<llvm::Type*> tmplLayout = baseFieldLayout();
            genericSubst_ = std::move(newSubst);
            genericSubstSemaTy_ = std::move(newSema);
            // fix #2889 (§14.1 / §12 / §28.4 step 3, v0.240): when `base` is
            // ITSELF a generic derived class (`L<X> : M<W> : B<T>`), the
            // substitution built just above binds only `M`'s parameter `W`.
            // `baseFieldLayout` walks M's INHERITED storage too, and `v: T` is
            // named in B's parameter — unbound here, so `storageType` produced
            // the same i32 placeholder under BOTH substitutions and the #2019
            // layout-identity test concluded "identical ⇒ the erased template
            // disposer is already right". It is not: the object is laid out at
            // the composed offsets, so the erased core read the wrong slot and
            // the owned base field was never disposed — measured as a 16-byte
            // Cell leak on a three-level generic chain, with no crash. Compose
            // the rest of the chain so BOTH the comparison and the specialized
            // core see the real layout.
            mergeAncestorGenericSubst(base);
            std::vector<llvm::Type*> specLayout = baseFieldLayout();
            bool needSpec =
                classHasOwnedTypeParamField(base) || (tmplLayout != specLayout);
            llvm::Function* specCore =
                needSpec ? specializedDisposerCoreFor(base) : nullptr;
            genericSubst_ = std::move(savedSubst);
            genericSubstSemaTy_ = std::move(savedSema);
            if (specCore) {
                builder_.CreateCall(specCore, {currentThis_});
                return;
            }
        }
        auto it = disposerCoreFns_.find(base);
        if (it != disposerCoreFns_.end()) builder_.CreateCall(it->second,
                                                          {currentThis_});
        return;  // single base class
    }
}


void Emitter::disposeDisplacedOwnedClassLocal(const ast::Expr* target,
                                              llvm::Value* addr,
                                              llvm::Type* elemTy,
                                              llvm::Value* newVal) {
    // §28.7 (#913/#1054): reassigning an owned class LOCAL must dispose the
    // displaced value before overwriting (the class analog of the string/
    // closure/box/owned-collection field-store helpers). Sound owned-detection:
    // the target must be a plain local variable that `unwindOwnedLocalClass`
    // classifies as a definitely-owned plain class/interface local (`new C()` /
    // `owned`-returning initializer). That EXCLUDES a borrow-initialized local,
    // a collection/task/subscription local, and any param (borrow OR owned — a
    // ParameterSymbol is not a LocalDecl), so a borrowed value owned elsewhere
    // is never disposed here (no double-free). The scope-end / explicit-dispose
    // path re-loads the slot and disposes the NEW value; here we dispose the OLD
    // one — exactly once each.
    if (!addr) return;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(target);
    if (!id) return;  // field / index / static target — not a plain local
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc) return;  // a parameter (borrow or owned) — excluded
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (!ld || !unwindOwnedLocalClass(ld)) return;  // not an owned plain class local
    llvm::Value* old = builder_.CreateLoad(elemTy, addr, "reassign.old");
    if (!old->getType()->isPointerTy()) return;
    // Null-guard (a moved-out slot holds null) AND identity-guard (a self-
    // assignment `c := c` would otherwise dispose the value being stored → UAF).
    llvm::Value* nullp = llvm::ConstantPointerNull::get(ptrTy());
    llvm::Value* live = builder_.CreateICmpNE(old, nullp, "reassign.live");
    llvm::Value* doDispose = live;
    if (newVal && newVal->getType()->isPointerTy())
        doDispose = builder_.CreateAnd(
            live, builder_.CreateICmpNE(old, newVal, "reassign.diff"));
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "reassign.dispose", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "reassign.cont", currentFn_);
    builder_.CreateCondBr(doDispose, dispBB, contBB);
    builder_.SetInsertPoint(dispBB);
    emitDisposeAndReclaim(old);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}

// fix #5421 (§28.7 / §28.9, v0.315): drop-on-overwrite for collection locals.
// Mirrors `disposeDisplacedOwnedClassLocal` for collection-typed locals. When a
// Dictionary/Array/LinkedList local is reassigned, the displaced value must be
// disposed. Finds the Disposal entry registered for this slot in disposeStack_
// (by registerUsingDisposal at lowerLocalDecl) and calls the appropriate free
// function. Skips if the slot is null or self-assigned.
void Emitter::disposeDisplacedCollectionLocal(const ast::Expr* target,
                                             llvm::Value* addr,
                                             llvm::Value* newVal) {
    if (!addr) return;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(target);
    if (!id) return;  // field / index / static target — not a plain local
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc) return;
    // LOCALDECL-ONLY: `target` is an ASSIGNMENT target — this helper's
    // sole caller is the `:=` store sequence in lowerAssignStmt
    // (LlvmBackend_Stmts.cpp:2102) — and §21.1 makes a `const` local a
    // single-assignment binding, so sema rejects the store before codegen
    // runs ("cannot assign to const local ...", ExprTypecheck.cpp). A
    // LocalConstDecl cannot reach here. Same reason as the sibling class
    // helper's site above. // per §21.1
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (!ld) return;
    // Only collection types need this helper; plain class is handled by
    // disposeDisplacedOwnedClassLocal.
    bool isColl = collectionTypeOfLocal(ld);
    bool isDict = dictTypeOfLocal(ld);
    bool isLL = linkedListTypeOfLocal(ld);
    if (!isColl && !isDict && !isLL) return;
    // Find the Disposal entry for this slot in disposeStack_.
    // slot is llvm::AllocaInst*; use localSlot() to get the alloca for id.
    llvm::AllocaInst* allocaSlot = localSlot(*id);
    if (!allocaSlot) return;
    const Disposal* entry = nullptr;
    for (auto scopeIt = disposeStack_.rbegin();
         scopeIt != disposeStack_.rend() && !entry;
         ++scopeIt) {
        for (auto it = scopeIt->rbegin(); it != scopeIt->rend(); ++it) {
            if (it->slot == allocaSlot) {
                entry = &(*it);
                break;
            }
        }
    }
    if (!entry) return;  // no registered disposal entry (e.g. borrow-typed local)
    llvm::Value* old = builder_.CreateLoad(ptrTy(), addr, "coll.reassign.old");
    llvm::Value* nullp = llvm::ConstantPointerNull::get(ptrTy());
    llvm::Value* live = builder_.CreateICmpNE(old, nullp, "coll.reassign.live");
    llvm::Value* doDispose = live;
    if (newVal && newVal->getType()->isPointerTy())
        doDispose = builder_.CreateAnd(
            live, builder_.CreateICmpNE(old, newVal, "coll.reassign.diff"));
    auto* frBB  = llvm::BasicBlock::Create(llvm_, "coll.reassign.free", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "coll.reassign.cont", currentFn_);
    builder_.CreateCondBr(doDispose, frBB, contBB);
    builder_.SetInsertPoint(frBB);
    if (isDict) {
        // Dictionary / Set: bz_dict_free(tbl, dictValueDisposer).
        // Mirror the dict arm in emitDisposals (lines 8119-8177).
        llvm::Value* tbl = builder_.CreateLoad(ptrTy(), addr);
        llvm::Value* isNullTbl = builder_.CreateICmpEQ(
            tbl, llvm::ConstantPointerNull::get(ptrTy()));
        auto* fr2BB = llvm::BasicBlock::Create(llvm_, "dict.free2", currentFn_);
        builder_.CreateCondBr(isNullTbl, contBB, fr2BB);
        builder_.SetInsertPoint(fr2BB);
        if (entry->dictStringKeys) {
            auto* relFn = declareC("bz_set_string_keys_release",
                                   llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(relFn, {tbl});
        }
        if (entry->dictKeyDisposer) {
            auto* keysDispFn = declareC("bz_set_coll_keys_dispose",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(keysDispFn, {tbl, entry->dictKeyDisposer});
        }
        llvm::Value* disp = entry->dictValueDisposer
            ? llvm::cast<llvm::Value>(entry->dictValueDisposer)
            : llvm::ConstantPointerNull::get(ptrTy());
        auto* fn = declareC("bz_dict_free",
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {tbl, disp});
        if (elementDisposerRunsUserCode(entry->dictValueDisposer))
            ++userCodeDisposalCount_;
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), addr);
    } else if (isColl) {
        // Array: bz_array_free(arr, collectionElemDisposer).
        // Mirror the collection arm in emitDisposals (lines 8045-8100).
        llvm::Value* arr = builder_.CreateLoad(ptrTy(), addr);
        llvm::Value* isNullArr = builder_.CreateICmpEQ(
            arr, llvm::ConstantPointerNull::get(ptrTy()));
        auto* fr2BB = llvm::BasicBlock::Create(llvm_, "arr.free2", currentFn_);
        builder_.CreateCondBr(isNullArr, contBB, fr2BB);
        builder_.SetInsertPoint(fr2BB);
        if (entry->collArrayDynamicFree) {
            auto* dynFn = declareC("bz_array_free_dynamic",
                                   llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(dynFn, {arr});
            ++userCodeDisposalCount_;
        } else {
            llvm::Value* disp = entry->collectionElemDisposer
                ? llvm::cast<llvm::Value>(entry->collectionElemDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_array_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {arr, disp});
        }
    } else {
        // LinkedList: bz_llist_free(llist, collectionElemDisposer).
        llvm::Value* ll = builder_.CreateLoad(ptrTy(), addr);
        llvm::Value* isNullLl = builder_.CreateICmpEQ(
            ll, llvm::ConstantPointerNull::get(ptrTy()));
        auto* fr2BB = llvm::BasicBlock::Create(llvm_, "ll.free2", currentFn_);
        builder_.CreateCondBr(isNullLl, contBB, fr2BB);
        builder_.SetInsertPoint(fr2BB);
        llvm::Value* disp = entry->collectionElemDisposer
            ? llvm::cast<llvm::Value>(entry->collectionElemDisposer)
            : llvm::ConstantPointerNull::get(ptrTy());
        auto* fn = declareC("bz_llist_free",
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {ll, disp});
    }
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}


// fix #4552 / §35.9 *Nullable `C?`* (v0.306): dispose the owning-choice value
// held AT AN ADDRESS, unwrapping §35.9's `C?` representation first.
//
// §35.9 makes `C?` "the ordinary nullable value type (§7.2)", and §7.2's
// representation for a nullable value type is the tagged wrapper
// `{ i8 hasValue, <payload> }` (`nullableValueStructTy`, whose own
// `isNullableChoiceTy` arm builds it with `choiceStruct` as the payload). So a
// `C?` LOCATION does not hold a choice struct at offset 0 — it holds the
// wrapper, with the choice struct at field 1.
//
// ⚠ EVERY pre-#4552 caller read it as though it did. `registerOwningChoiceDisposal`
// registers a nullable owning-choice local exactly as it registers a
// non-nullable one (its gate reads `rt->declaration->kind == Choice`, which a
// `C?` TypeRef satisfies), and the scope-end pass then loaded `%choice.C` from
// the wrapper's address — reading `hasValue` (plus padding) AS THE TAG.
// MEASURED on `var o: Outcome? := Outcome.Fetched(new Widget());`: `hasValue`
// is 1, `Fetched` is case 0 and `Failed` is case 1, so the tag read 1, the
// switch took the payload-free `Failed` arm, and the widget was silently
// leaked. The same misread is a CRASH rather than a leak whenever the
// mis-decoded tag lands on a case that DOES carry an owned payload: the
// extracted "payload pointer" is then another field's bytes.
void Emitter::emitOwningChoiceLocationDisposal(llvm::Value* addr,
                                               const sema::TypeRef* choiceRef) {
    if (!addr || !choiceRef || !choiceRef->declaration) return;
    if (!choiceRef->nullable) {
        emitOwningChoiceActiveCaseDisposal(addr, choiceRef->declaration,
                                           choiceRef);
        return;
    }
    llvm::StructType* wrap = nullableValueStructTy(choiceRef);
    if (!wrap || wrap->getNumElements() < 2) return;
    // §35.9: "`null` assignment into an owned-state `C?` location is
    // drop-on-overwrite — the displaced value's payloads are disposed". The
    // converse is equally normative: a `C?` location that IS null holds no
    // payloads, so the walk must be guarded rather than run on zeroed bytes.
    llvm::Value* hvAddr =
        builder_.CreateStructGEP(wrap, addr, 0, "ocd.opt.hv.addr");
    llvm::Value* hv =
        builder_.CreateLoad(nullablePrimHasValTy(), hvAddr, "ocd.opt.hv");
    llvm::Value* live = builder_.CreateICmpNE(
        hv, llvm::ConstantInt::get(nullablePrimHasValTy(), 0), "ocd.opt.live");
    auto* dispBB =
        llvm::BasicBlock::Create(llvm_, "ocd.opt.dispose", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "ocd.opt.cont", currentFn_);
    builder_.CreateCondBr(live, dispBB, contBB);
    builder_.SetInsertPoint(dispBB);
    llvm::Value* inner =
        builder_.CreateStructGEP(wrap, addr, 1, "ocd.opt.val.addr");
    emitOwningChoiceActiveCaseDisposal(inner, choiceRef->declaration,
                                       choiceRef);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}


// fix #4552 / §35.4 *Drop-on-overwrite is defined and automatic* + §35.9
// (v0.306). §35.4: "Whole-value reassignment of an owning-choice local or field
// *disposes the displaced value* per §28.7, like an owned class binding. A
// choice has no partial states, so the single-value form of that rule applies
// as written"; and "`x := null;` on an owned `C?` location likewise disposes
// the displaced value's payloads (§35.9)".
//
// Codegen had NO drop-on-overwrite path for a choice before this: the class
// sibling (`disposeDisplacedOwnedClassLocal`, above) returns at its
// `unwindOwnedLocalClass` gate for a choice-typed local (a choice is a
// value type, not a class reference), and the scope-end active-case pass
// (`registerOwningChoiceDisposal` → `emitDisposals`' `owningChoice` arm) only
// ever sees the FINAL value in the slot. So every displaced value leaked:
// MEASURED on `var b := Bundle.Ready(new Resource("old")); b := Bundle.Ready(
// new Resource("new"));` — spec output `dispose old / reassigned / dispose
// new`, observed `reassigned / dispose new`.
//
// ORDER. Called from the `:=` lowering AFTER the RHS has been lowered and
// BEFORE the store — §35.4's own sequencing ("the RHS is evaluated first, then
// the displaced value is disposed, then the new value is stored").
//
// THE TWO SUPPRESSIONS, both §28.7's: the pass runs only "if the previous value
// is live (**non-null and not moved-from**)".
//   1. moved-from — supplied by the caller via `as->lhsMovedFromAtStore`, the
//      ownership dataflow's converged state at this store (#3950). Without it
//      `b := Pass(move b);` would dispose the value the callee just handed
//      back through the same slot.
//   2. self-assignment — `b := b;` is admitted today and lowers to a
//      load-then-store of the same bytes, so disposing the displaced value
//      would free the payload the store is about to write back. The class
//      sibling catches this with a pointer IDENTITY guard on the loaded old
//      value; a choice is a struct in an SSA register with no pointer to
//      compare, so the equivalent test is syntactic: the RHS is a bare
//      identifier naming the same local.
// The NULL half of §28.7's conjunction needs no separate gate here: an owning
// choice always holds a live case, and the `C?` spelling's `hasValue` guard is
// inside `emitOwningChoiceLocationDisposal`.
void Emitter::disposeDisplacedOwningChoiceLocal(const ast::Expr* target,
                                                llvm::Value* addr,
                                                const ast::Expr* rhsExpr) {
    if (!addr || !target) return;
    if (emittingDisposerBody_) return;  // disposer locals: field pass owns them
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(target);
    if (!id) return;  // field / index / static target — not a plain local
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc) return;  // a parameter (borrow or owned) — excluded
    // LOCALDECL-ONLY: `target` is an ASSIGNMENT target — this helper is reached
    // only from the `:=` lowering — and §21.1 makes a `const` local a
    // single-assignment binding, so sema rejects the store before codegen ever
    // runs ("cannot assign to const local `b` — a const local is a
    // single-assignment binding (§21.1)", verified by probe). A LocalConstDecl
    // therefore cannot reach here, and there is no `const` binding form for the
    // #3591 family to silently skip. // per §21.1
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (!ld) return;
    // Suppression 2 (see the header): `b := b;`.
    if (auto* rhsId = dynamic_cast<const ast::IdentifierExpr*>(rhsExpr))
        if (ctx_.exprSymbol(*rhsId) == static_cast<sema::Symbol*>(loc)) return;
    // The declared type decides — the same resolution order
    // `registerOwningChoiceDisposal` uses, so the store-site drop and the
    // scope-end active-case pass can never disagree about which locals are
    // owning choices.
    const sema::TypeRef* rt =
        ld->type ? ctx_.resolvedType(*ld->type)
                 : (ld->initializer ? ctx_.exprType(*ld->initializer) : nullptr);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return;
    rt = resolveTypeParamRef(rt);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return;
    if (rt->declaration->kind != ast::TypeDeclKind::Choice) return;
    // §3.6 rule 2 + §35.4: classification is per-instantiation.
    auto verdict = common::classifyChoiceType(rt, ctx_);
    if (!verdict || *verdict != common::ChoiceClassification::Owning) return;
    emitOwningChoiceLocationDisposal(addr, rt);
}


// fix #4566 / §15.3 *Local-to-local assignment* + §28.7 *Moved-from and null*
// (v0.306). §15.3 makes an owning-classified choice a MOVE-ONLY value type:
// "Assignment transfers the value from source to destination, leaving the
// source in moved-from state", spelled out at the first *Rules* bullet as
// "**Local-to-local assignment** `var y := x;`: copies for an ordinary value
// type; moves for a move-only value type. After the move, `x` is in moved-from
// state" — unmarked, because "a move-only value binding has no borrow reading
// to disambiguate". §15.3's *Local lifetime* bullet then states the
// consequence for the SOURCE: a whole-value move "leaves the local holding no
// value rather than a hole, so no pass walks it (K1's moved-from state:
// 'discharge machinery skips it')", which is §28.7's *Moved-from and null*
// clause — "A moved-from or null location therefore stores without disposal".
//
// Codegen implemented only the STORE half. `c := b;` and `var c := b;`
// plain-copied the choice struct, so both slots held the same owned payload
// pointer and BOTH scope-end active-case passes dispatched `Dispose` through
// it. MEASURED on §35.4's own shape (`var b := Bundle.Ready(new
// Resource("b")); var c := Bundle.Ready(new Resource("c")); c := b;`):
// `dispose c` / `moved` / `dispose b` / SIGSEGV, the second dispatch reading
// a freed vtable, and `AddressSanitizer: heap-use-after-free` under
// `--sanitize=address`.
//
// THE MARK IS A RUNTIME ZERO-STORE over the source slot — the choice analogue
// of `nullTransferredOwnedLocalSlot`, which nulls an owned class local's slot
// at a transfer so the null-guarded disposal downstream is a no-op. Zeroing
// works for a choice for the same reason, one level in: the scope-end pass
// reads tag 0, enters case 0's arm, and finds payload pointers its EXISTING
// null-guard skips ("Null-guard: a never-bound arm ... could leave an `owned`-
// payload slot null"); a case-0 payload that is itself an owning choice
// recurses into the same all-zero state and terminates there. So no consumer
// of the slot needs a new predicate:
//   * the scope-end active-case pass disposes nothing;
//   * `disposeDisplacedOwningChoiceLocal` at a later re-acquiring store
//     (`c := b; b := Bundle.Ready(new Resource("b2"));` — measured as its own
//     double free, `dispose c` / `dispose b` / SIGSEGV) reads the same zeros
//     and disposes nothing, which is exactly §28.7's moved-from clause;
//   * the next store into the source re-arms it with a live value, so a move
//     inside a loop body is correct every iteration.
// Being a runtime store it is EXACT on every control-flow path: a move under
// an `if` suppresses the source's disposal on the taken path and leaves it on
// the other. A static "transferred somewhere in this body" set — the move-only
// inline array's `moveOnlyArrayTransferred_`, the other move-only kind's
// answer to the same question — cannot make that distinction and pays for it
// with a leak on the untaken path.
//
// WHY NOT A SENTINEL TAG instead of a zero-store: an out-of-range tag would
// also make the disposal switch fall to its no-op default, but it puts the
// slot in a state the CASE lowering's exhaustive switch has no arm for. Zero
// is an in-range, constructible state, so a read of the moved-from source —
// which §15.3 makes a compile error but which this compiler still ACCEPTS
// (filed separately; not fixed here) — stays a defined read rather than
// becoming undefined behaviour.
//
// SCOPE. #4566 is local-to-local, so both ends must be plain locals with an
// `ast::LocalDecl`: that is exactly the set `registerOwningChoiceDisposal`
// enrolls in the scope-end pass, which is what makes "the target has taken
// over the disposal obligation" a fact rather than an assumption. A
// field/index/static target, a parameter at either end, `_ := b;` (§35.10's
// move-only discard) and §35.5 arm-selection payload transfer are declined
// here and remain open.
void Emitter::markOwningChoiceLocalMovedFrom(const ast::Expr* targetExpr,
                                             const ast::Expr* srcExpr) {
    // THE TARGET must be a plain local — the recipient has to be a location
    // this backend actually enrolls in the scope-end active-case pass, or the
    // suppression below would turn the double free into a leak. Same gate, in
    // the same order, as `disposeDisplacedOwningChoiceLocal` above.
    if (!targetExpr) return;
    auto* tid = dynamic_cast<const ast::IdentifierExpr*>(targetExpr);
    if (!tid) return;
    auto* tloc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*tid));
    if (!tloc) return;
    // This overload is reached only from the `:=` lowering, so `targetExpr` is
    // an ASSIGNMENT target; the same marker, for the same reason, as
    // `disposeDisplacedOwningChoiceLocal` above. // per §21.1
    // LOCALDECL-ONLY: §21.1 makes a runtime `const` a single-assignment
    // binding, so sema rejects a store into one before codegen runs and an
    // `ast::LocalConstDecl` can never be an assignment target here.
    auto* tld = dynamic_cast<const ast::LocalDecl*>(tloc->decl);
    markOwningChoiceLocalMovedFrom(tld, srcExpr);
}

// The shared core, reached from the `:=` overload above and directly from the
// `var c := b;` declaration site (a LocalDecl has no target EXPRESSION — the
// binding itself is the target).
void Emitter::markOwningChoiceLocalMovedFrom(const ast::LocalDecl* targetLd,
                                             const ast::Expr* srcExpr) {
    if (!targetLd || !srcExpr) return;
    markOwningChoiceMovedFromCore(targetLd, srcExpr);
}

// fix #5436 (§15.3 *Parameter passing*, v0.315): a call ARGUMENT whose formal
// is a by-value-move parameter. §15.3: "A parameter declared `(x: V)` for an
// ordinary V is by value (copy); for a move-only V, it is by value-move
// (transfer ownership). The caller's argument expression is in moved-from
// state after the call." §35.4: "parameter passing is by value-move".
//
// The overloads above both require a TARGET local, because #4566's
// local-to-local shape needs the target's identity to reject the self-
// assignment `b := b;`. A call argument has no such target: the recipient is
// the callee's formal, a PARAMETER, which is exactly the shape #4566's SCOPE
// note declined ("a parameter at either end"). The recipient being a
// parameter is not a reason to withhold the mark, though — the thing the mark
// suppresses is the SOURCE's scope-end disposal, and the source is a plain
// local in both cases. So this form passes a null target: no self-assignment
// to exclude, the same zero-store otherwise.
//
// MEASURED at `r23-c28` (6f341713) on
// `var b := Bundle.Ready(new Resource("b")); var d := Host.Pass(b);`:
// `moved` / `dispose b` / then SIGSEGV, and `AddressSanitizer:
// heap-use-after-free ... READ of size 8` under `--sanitize=address`. The
// sema half (Ownership_Checks' `noteOwningChoiceWholeMoveRead` call, the
// choice twin of #4677's inline-array arm) makes the program REJECT a later
// read, but the runtime double free is here.
void Emitter::markOwningChoiceArgMovedFrom(const ast::Expr* srcExpr) {
    if (!srcExpr) return;
    markOwningChoiceMovedFromCore(/*targetLd=*/nullptr, srcExpr);
}

void Emitter::markOwningChoiceMovedFromCore(const ast::LocalDecl* targetLd,
                                            const ast::Expr* srcExpr) {
    if (!srcExpr) return;
    if (emittingDisposerBody_) return;  // disposer locals: field pass owns them
    // THE SOURCE must be a bare identifier naming a DIFFERENT local. A bare
    // identifier is the only shape §15.3 bullet 1 moves; `b.f`, `b[i]` and
    // `b.M()` are borrows of `b` (the same reading `MoveOnlyArrayTransfer
    // Walker` gives those positions for the other move-only kind).
    //
    // The `move` SPELLING of that same transfer — `var t := move b;` — is the
    // one §15.3 bullet 1's scope note makes UNMARKED in the plain binding form
    // ("copies are forbidden, so the plain binding can only move - and therefore
    // stays unmarked, as this rule has always defined"). It is still the SAME
    // transfer, and it arrives here as an `ast::UnaryExpr` with `op == "move"`
    // wrapping the identifier (Parser_Exprs.cpp, A.7 `MoveExpr ::= "move"
    // PrimaryExpr`), so the bare cast below declined it and the source kept
    // its scope-end owning-choice disposal while the taker took one too.
    // // per §15.3 / §A.4
    //
    // MEASURED at r23-c28 (fece431f) on
    //   var src := Slot.Full(new Conn()); var t := move src;
    // heap-use-after-free, READ of size 8 in `main` (ASan), region allocated by
    // `new Conn()` in the initializer and freed by the taker's scope-end pass;
    // without ASan the same binary exits SIGSEGV. The UNMARKED spelling
    // `var t := src;` of the identical transfer is clean, which is the
    // differential control: the marker alone decided the double free.
    //
    // Unwrapping is the sanctioned shape, not a new one:
    // `nullTransferredOwnedLocalSlot` (LlvmBackend_Assign.cpp) does exactly
    // this for the call-ARGUMENT position, which is why `C.Sink(move b!)` never
    // double-freed while this declaration form did.
    const ast::IdentifierExpr* sid =
        dynamic_cast<const ast::IdentifierExpr*>(srcExpr);
    if (!sid) {
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(srcExpr))
            if (u->op == "move")
                sid = dynamic_cast<const ast::IdentifierExpr*>(
                    u->operand.get());
    }
    if (!sid) return;
    auto* sloc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*sid));
    if (!sloc) return;
    // `lowerStmt`'s own `LocalConstDecl` arm records why its sibling does not
    // mirror the `registerOwningChoiceDisposal` chain: "A `const` cannot bind
    // an OWNED production (an owning choice, a move-only inline array, a task
    // frame — §21.1)". // per §21.1
    // LOCALDECL-ONLY: a §21.1 runtime `const` cannot BIND an owning choice, so
    // it is never enrolled in the scope-end active-case pass and has no
    // disposal for a moved-from mark to suppress.
    auto* sld = dynamic_cast<const ast::LocalDecl*>(sloc->decl);
    if (!sld) return;  // a parameter / §21.1 `const` — neither is enrolled in
                       // the scope-end pass, so neither has a disposal to
                       // suppress (§21.1 does not admit an owned production
                       // at a `const` binding at all).
    if (targetLd && sld == targetLd)
        return;  // `b := b;` — §15.3's *Whole-value
                  // reassignment* bullet makes self-assignment
                                  // "a no-op"; zeroing here would destroy the
                                  // value the store just wrote back.
    // The declared type decides, resolved in the SAME order
    // `registerOwningChoiceDisposal` uses — the store below and the scope-end
    // pass must never disagree about which locals are owning choices.
    const sema::TypeRef* rt =
        sld->type ? ctx_.resolvedType(*sld->type)
                  : (sld->initializer ? ctx_.exprType(*sld->initializer)
                                      : nullptr);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return;
    rt = resolveTypeParamRef(rt);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return;
    if (rt->declaration->kind != ast::TypeDeclKind::Choice) return;
    // §3.6 rule 2 + §35.4: classification is per-instantiation.
    auto verdict = common::classifyChoiceType(rt, ctx_);
    if (!verdict || *verdict != common::ChoiceClassification::Owning) return;
    auto it = locals_.find(sld);
    if (it == locals_.end() || !it->second) return;
    llvm::AllocaInst* slot = it->second;
    // The expected storage type, derived the way the DISPOSAL derives it:
    // §35.9's `C?` is the ordinary nullable value type `{ i8 hasValue,
    // <choice struct> }` (zeroing it clears `hasValue`, so
    // `emitOwningChoiceLocationDisposal`'s guard skips the walk outright),
    // and a bare `C` is the choice struct, laid out under its own
    // instantiation's substitution (#2047).
    llvm::Type* want = nullptr;
    if (rt->nullable) {
        want = nullableValueStructTy(rt);
    } else {
        RecordSubstGuard choiceSubst(this, rt);
        want = choiceStruct(rt->declaration);
    }
    if (!want) return;
    // Refuse anything whose alloca is not that exact storage. The one shape
    // this excludes in practice is a §27.5 capture-boxed local, whose alloca
    // has been rewritten to a pointer-sized cell holder — a zero-store there
    // would null the cell pointer rather than the value. §15.3's consume-site
    // freeze makes moving a move-only local while a closure capture of it is
    // live a compile error, so the shape should not reach here; declining is
    // the conservative answer if it ever does (the pre-fix double free, never
    // a fresh corruption).
    if (slot->getAllocatedType() != want) return;
    builder_.CreateStore(llvm::Constant::getNullValue(want), slot);
}


// fix #4552 / §35.5 *The owning match* + §35.4 *Disposal is defined by the
// active case* (v0.306): is `e` an UNBOUND owned temporary of owning-choice
// type — the scrutinee shape §35.5 calls an "announcing scrutinee form"?
//
// §35.5: "The scrutinee temporary's full expression is the **whole `case`
// construct** — K8.3, applied unchanged — so payloads not transferred into
// bindings (discarded slots like `Fetched(_)`, and the payloads of the value
// when a `_` or non-destructuring arm is selected) are disposed when the
// construct completes, by the same unbound-temporary pass that disposes the
// container."
//
// THE ADMITTED SHAPE IS A CALL, and only a call. §15.3 makes an owning choice
// a move-only value type whose "return transfers", and §35.3's case
// construction (`Outcome.Fetched(new Widget())`) is itself a CallExpr whose
// callee resolves to a ChoiceCaseSymbol — so both mint. EXCLUDED, each because
// something else already owns the value and a second disposal here would be a
// double free:
//   • a bare identifier (`case o of …`) — a named local, disposed by
//     `registerOwningChoiceDisposal`'s scope-end active-case pass;
//   • a field / property / indexer read — a borrow of its storage (K1.5).
//
// `case move o of …` — §35.5's OTHER announcing scrutinee form — is NOT in
// either list: it was excluded here as "#4552's follow-up" until fix #4609
// implemented the marked scrutinee transfer, and it now reaches the same
// construct-completion reclaim through the sibling gate
// `markedOwningChoiceScrutineeSlot`, which additionally marks the source
// binding moved-from. The old exclusion's stated consequence — "the source
// local's slot is left populated and its scope-end pass still disposes it" —
// was not the leak it reads as but a DOUBLE FREE, since the arm had already
// adopted the payload; see that helper for the measurement.
//
// This gate is deliberately NOT the ownership pass's `disposeOwnedScrutinee`
// flag: that flag's own predicate is `producerState == Owned` restricted to a
// heap class/interface or an owning-collection head, and widening it would
// change a pass whose verdicts feed diagnostics. The owning-choice family is
// already classified codegen-side by `registerOwningChoiceDisposal` (which
// likewise consults no ownership flag), so keeping this gate beside it keeps
// the two owning-choice disposal sites reading one authority.
bool Emitter::patternBindsAnySlot(const ast::Pattern* p) {
    if (!p) return false;
    if (dynamic_cast<const ast::BindingPattern*>(p)) return true;
    if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(p)) {
        for (const auto& sp : dp->subPatterns) {
            if (!sp.nakedBinding.empty()) return true;
            if (patternBindsAnySlot(sp.pattern.get())) return true;
        }
        return false;
    }
    if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
        for (const auto& m : ap->members)
            if (patternBindsAnySlot(m.get())) return true;
        return false;
    }
    return false;  // discard / null / constant / named / range / relop / type
}


// fix #4617 / §35.5 *The owning match* (v0.307): the PER-SLOT discharge of an
// owning-choice scrutinee temporary's construct-completion reclaim.
//
// §35.5 states both halves of arm selection at SLOT granularity: the selected
// arm "transfers the case's `owned` and move-only value payloads into its
// bindings", and "the un-transferred remainder — unlisted fields, unmarked
// named slots, discarded siblings — is disposed at construct completion".
// #4609's discharge was per ARM — `patternBindsAnySlot` gating one zero-store
// over the whole temp — so an arm binding SOME slots of a multi-payload case
// discharged the reclaim for ALL of them and the un-bound siblings were
// reclaimed by nobody. MEASURED on §35.5's own shape, over
// `W(owned a: Widget, owned b: Widget)`:
//
//   case move o of
//     W(var w, _) => do using var live := move w; end,   // `b` reclaimed by nobody
//     X           => ()
//   end case;
//
// printed `dispose a` and never `dispose b` (a leak, amplified to 16 B and one
// allocation per iteration under LSan).
//
// THE MARK IS #4566's moved-from encoding, applied at slot granularity rather
// than to the whole value: `emitOwningChoiceActiveCaseDisposal` null-guards
// every class/interface payload pointer explicitly, and a zeroed nested choice
// struct reads tag 0 with null payload pointers, which its recursive walk
// likewise skips. So nulling exactly the transferred slots leaves the
// completion reclaim (and the dispose-stack entry covering a `return`/`throw`
// out of the arm) disposing exactly the remainder — §35.5's four disposal
// traces unchanged, now at the granularity the section states them at.
//
// THE TWO ERROR DIRECTIONS ARE NOT SYMMETRIC, and that asymmetry is the whole
// design: failing to mark a slot the arm adopted is a DOUBLE FREE, while
// marking one it did not is a leak. Every shape whose transfer set cannot be
// computed per slot therefore falls back to the pre-#4617 whole-value mark —
// the sound direction, and exactly the previous behaviour, so no shape that
// works today can regress.
void Emitter::dischargeOwningMatchTransfers(llvm::AllocaInst* slot,
                                            const sema::TypeRef* choiceRef,
                                            const ast::Pattern* armPattern) {
    if (!slot) return;
    if (markOwningMatchTransferredSlots(slot, choiceRef, armPattern)) return;
    // Fallback: #4609's whole-value mark.
    builder_.CreateStore(
        llvm::Constant::getNullValue(slot->getAllocatedType()), slot);
}


// The recursive core of the above. `choiceAddr` is the address of a choice
// struct (the scrutinee temp's alloca at the top level, a nested payload
// field's address one level in); returns false — "I could not decide this
// shape, mark the whole value" — for every case listed in the caller's
// asymmetry note.
bool Emitter::markOwningMatchTransferredSlots(llvm::Value* choiceAddr,
                                              const sema::TypeRef* choiceRef,
                                              const ast::Pattern* p) {
    if (!choiceAddr || !choiceRef || !choiceRef->declaration || !p) return false;
    if (choiceRef->declaration->kind != ast::TypeDeclKind::Choice) return false;
    // §35.9's `C?` storage is the §7.2 wrapper `{ i8 hasValue, <choice> }`, not
    // a choice struct; decline rather than GEP a case payload out of the wrong
    // layout. The caller's whole-value mark keeps the pre-fix behaviour.
    if (choiceRef->nullable) return false;
    // Only a case destructuring carries a per-slot transfer set. A whole-value
    // `BindingPattern` arm (`case move o of var whole => …`) transfers the
    // entire value, and an alternative list is binding-free by §35.5 — both
    // take the whole-value mark.
    auto* dp = dynamic_cast<const ast::DestructuringPattern*>(p);
    if (!dp || !dp->typeName || dp->typeName->parts.empty()) return false;
    const sema::TypeSymbol* choiceTy = choiceRef->declaration;
    std::size_t tag = 0;
    const sema::ChoiceCaseSymbol* cc =
        choiceCaseByName(choiceTy, dp->typeName->parts.back(), &tag);
    if (!cc || !cc->caseDecl) return false;   // head names no case of this
                                              // choice (a record destructure)
    // §14.1 / §28.1 (#2047): the layout is per-instantiation, exactly as
    // `emitOwningChoiceActiveCaseDisposal` and the match-side binding path
    // derive it — the two must never disagree about where a payload lives.
    RecordSubstGuard choiceSubst(this, choiceRef);
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return false;
    // The storage gate, the same one `markedOwningChoiceScrutineeSlot` applies:
    // the alloca must hold exactly the layout this instantiation implies.
    if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(choiceAddr))
        if (ai->getAllocatedType() != st) return false;
    if (1 + tag >= st->getNumElements()) return false;
    auto* payloadTy = llvm::dyn_cast<llvm::StructType>(
        st->getElementType(static_cast<unsigned>(1 + tag)));
    if (!payloadTy) return false;
    llvm::Value* payloadAddr = builder_.CreateStructGEP(
        st, choiceAddr, static_cast<unsigned>(1 + tag), "omt.payload");
    for (std::size_t j = 0;
         j < dp->subPatterns.size() && j < cc->caseDecl->payload.size(); ++j) {
        if (j >= payloadTy->getNumElements()) break;
        const auto& sp = dp->subPatterns[j];
        // §19.3 "Named-slot bindings and ownership" (v0.334): an UNMARKED
        // named slot NEVER transfers, so it is part of the un-transferred
        // remainder §35.5 disposes at construct completion — "unlisted
        // fields, unmarked named slots, discarded siblings" — and NOT part of
        // the slots this mark discharges.
        //
        // No named slot transfers (`move` is not a slot spelling, §19.3,
        // v0.338). This MUST agree with `collectOwningMatchTransferBindings`,
        // which skips every named slot: marking a slot the arm did not adopt
        // LEAKS it, so the payload is left to the completion reclaim.
        if (sp.namedSlot) continue;
        // §35.5: a slot that binds nothing — `_`, a constant, a range — is
        // "the un-transferred remainder"; leave it for the completion reclaim.
        // THIS IS THE FIX: before #4617 the loop did not exist and every slot
        // of a binding arm was marked.
        bool slotBinds = !sp.nakedBinding.empty() ||
                         patternBindsAnySlot(sp.pattern.get());
        if (!slotBinds) continue;
        llvm::Type* fieldTy =
            payloadTy->getElementType(static_cast<unsigned>(j));
        llvm::Value* fieldAddr = builder_.CreateStructGEP(
            payloadTy, payloadAddr, static_cast<unsigned>(j), "omt.slot");
        // A whole-slot binding — `W(var w, _)`'s `w`, or the naked `W(w, _)`
        // spelling — transfers the payload into an owned local of the arm
        // body's scope (§35.5). Mark just that slot.
        bool wholeSlot =
            !sp.nakedBinding.empty() ||
            dynamic_cast<const ast::BindingPattern*>(sp.pattern.get()) !=
                nullptr;
        if (!wholeSlot) {
            // A nested pattern that binds. §35.5 routes the recursion "exactly
            // through the slots that transfer", and the one nested move-only
            // shape with a pattern surface is a choice payload — recurse into
            // it so ITS un-bound remainder survives to the completion reclaim
            // too. Every other nested shape (a class-typed payload, whose
            // nested slots are property-getter borrows) takes the conservative
            // whole-slot mark: a leak, never a double free.
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                cc->caseDecl->payload[j], choiceTy, choiceRef, ctx_);
            bool recursed = false;
            if (ftInst && !ftInst->nullable &&
                ftInst->kind == sema::TypeRef::Kind::Named &&
                ftInst->declaration &&
                ftInst->declaration->kind == ast::TypeDeclKind::Choice) {
                // fix #4132's lesson: compute the nested layout under the
                // NESTED instantiation's substitution and require it to be the
                // field's actual storage before GEPing through it.
                llvm::StructType* nestedSt = nullptr;
                {
                    RecordSubstGuard nestedSubst(this, ftInst);
                    nestedSt = choiceStruct(ftInst->declaration);
                }
                if (nestedSt && nestedSt == fieldTy)
                    recursed = markOwningMatchTransferredSlots(
                        fieldAddr, ftInst, sp.pattern.get());
            }
            if (recursed) continue;
        }
        builder_.CreateStore(llvm::Constant::getNullValue(fieldTy), fieldAddr);
    }
    return true;
}


bool Emitter::unboundOwningChoiceTemp(const ast::Expr* e,
                                      const sema::TypeRef* rt) {
    if (!e || !rt) return false;
    if (!dynamic_cast<const ast::CallExpr*>(e)) return false;
    if (rt->kind != sema::TypeRef::Kind::Named || !rt->declaration) return false;
    rt = resolveTypeParamRef(rt);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    if (rt->declaration->kind != ast::TypeDeclKind::Choice) return false;
    auto verdict = common::classifyChoiceType(rt, ctx_);
    return verdict && *verdict == common::ChoiceClassification::Owning;
}


// fix #4609 / §35.5 *The owning match* (v0.306): the SECOND announcing
// scrutinee form — "a named owned binding under an explicit scrutinee
// transfer marker — `case move outcome of …`, the scrutinee an
// identifier-operand `move` expression (§28.8.1)".
//
// §35.5 makes this form a consume with a stated end state: "the consumed
// binding is left **moved-from** when the construct completes". Codegen never
// recorded that. `unboundOwningChoiceTemp`'s own exclusion list said so — "`case
// move o of …` — the §35.5 marked scrutinee transfer, which codegen does not
// yet implement (the source local's slot is left populated and its scope-end
// pass still disposes it; #4552's follow-up)" — and the consequence was not a
// leak but a DOUBLE FREE: the arm's `using var u := move w;` adoption disposes
// the transferred payload, and the source local's scope-end active-case pass
// then disposes the same block again. MEASURED on the rule corpus' own
// s35_5_0003 (which is written from §35.5 and was passing): `adopted` /
// `widget disposed` / `after` then SIGSEGV, and `AddressSanitizer:
// heap-use-after-free ... READ of size 8` under `--sanitize=address`. The
// nested spelling in #4609 (an owning choice inside an owning choice) is the
// same defect one level in.
//
// THE MARK IS #4566's, at the third site its SCOPE note left open: a runtime
// zero-store over the source slot. See `markOwningChoiceLocalMovedFrom` for
// why zero is the right moved-from encoding for a choice (tag 0, null payload
// pointers, every downstream walk already null-guarded) and why a runtime
// store rather than a static set (exact on every control-flow path).
//
// The gate here is the STORAGE TYPE, not the declaration kind, and it is
// deliberately wider than #4566's `ast::LocalDecl` gate in one direction and
// narrower in another. Wider: an arm binding of nested owning-choice type
// (`case move o of W(var i) => … case move i of …`, #4609's own repro) and a
// by-value-move parameter (§15.3: "parameter passing is by value-move") both
// reach here and both must be marked, because the caller registers a
// replacement disposal for the value it took. Narrower: the slot must hold
// EXACTLY the storage `rt` implies, so a §27.5 capture-boxed local — whose
// alloca is a pointer-sized cell holder — is declined, and a decline is safe
// because the caller then registers nothing and the pre-fix path stands.
llvm::AllocaInst* Emitter::markedOwningChoiceScrutineeSlot(
        const ast::Expr* e, const sema::TypeRef* rt) {
    if (!e || !rt) return nullptr;
    auto* un = dynamic_cast<const ast::UnaryExpr*>(e);
    if (!un || un->op != "move" || !un->operand) return nullptr;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(un->operand.get());
    if (!id) return nullptr;   // `move this._f` / `move a[i]` — §35.5 rejects a
                               // borrowed scrutinee, and neither has a slot
                               // this backend owns.
    if (rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return nullptr;
    const sema::TypeRef* res = resolveTypeParamRef(rt);
    if (!res || res->kind != sema::TypeRef::Kind::Named || !res->declaration)
        return nullptr;
    if (res->declaration->kind != ast::TypeDeclKind::Choice) return nullptr;
    // §3.6 rule 2 + §35.4 / §35.11: classification is per-instantiation.
    auto verdict = common::classifyChoiceType(res, ctx_);
    if (!verdict || *verdict != common::ChoiceClassification::Owning)
        return nullptr;
    llvm::AllocaInst* slot = localSlot(*id);
    if (!slot) return nullptr;
    // The expected storage, derived exactly as the DISPOSAL derives it —
    // §35.9's `C?` is `{ i8 hasValue, <choice struct> }`, a bare `C` is the
    // choice struct under its own instantiation's substitution (#2047).
    llvm::Type* want = nullptr;
    if (res->nullable) {
        want = nullableValueStructTy(res);
    } else {
        RecordSubstGuard choiceSubst(this, res);
        want = choiceStruct(res->declaration);
    }
    if (!want || slot->getAllocatedType() != want) return nullptr;
    return slot;
}


// fix #4552 / §28.3 *Unbound owned temporaries* (v0.306). See the declarations
// in LlvmBackend_impl.hpp for the rule, the measured repro and the
// non-nullable-`string`-only soundness argument.
bool Emitter::freshNewReceiverTempStringResult(
        const ast::Expr* recvExpr, llvm::Value* recvPtr,
        const sema::TypeRef* resultTy) const {
    if (!recvExpr || !recvPtr || !resultTy) return false;
    if (!isFreshOwnedClassProducer(recvExpr)) return false;
    if (resultTy->kind != sema::TypeRef::Kind::Primitive) return false;
    if (resultTy->primitive != sema::PrimitiveType::String) return false;
    if (resultTy->nullable) return false;   // `string?` returns are NOT minted
    if (!recvPtr->getType()->isPointerTy()) return false;
    if (llvm::isa<llvm::ConstantPointerNull>(recvPtr)) return false;
    return true;
}


void Emitter::registerFreshNewReceiverTempK8(const ast::Expr* recvExpr,
                                             llvm::Value* recvPtr,
                                             const sema::TypeRef* resultTy) {
    if (!freshNewReceiverTempStringResult(recvExpr, recvPtr, resultTy)) return;
    if (disposeStack_.empty()) return;
    if (emittingDisposerBody_) return;
    if (!currentFn_) return;
    // `entryAllocaNullPtr` (not a bare `entryAlloca`) for the #2733 reason the
    // unwind registration documents: a disposal pass can be reached on a path
    // that never executed the production store.
    llvm::AllocaInst* slot = entryAllocaNullPtr();
    builder_.CreateStore(recvPtr, slot);
    Disposal d{slot, nullptr};
    d.unwindOnly = true;      // the normal edge is the K8 flush below
    d.dynamicDispose = false;
    disposeStack_.back().push_back(d);
    k8RecvTemps_.push_back(K8RecvTemp{slot, currentFn_});
}


bool Emitter::deferUnboundOwnedTempToK8(llvm::AllocaInst* slot) {
    if (!slot || !currentFn_ || emittingDisposerBody_) return false;
    if (slot->getAllocatedType() != ptrTy()) return false;
    if (slot->getFunction() != currentFn_) return false;
    // Entry-block null init, placed right after the alloca so it dominates every
    // path (#2733). A frame-spilled slot is zeroed by the frame calloc instead
    // (the entryAllocaNullPtr rationale: the promoted GEP would not dominate an
    // entry store).
    if (smStateStruct_) {
        if (std::find(asyncHandlerSpillSlots_.begin(),
                      asyncHandlerSpillSlots_.end(),
                      slot) == asyncHandlerSpillSlots_.end())
            asyncHandlerSpillSlots_.push_back(slot);
    } else {
        llvm::IRBuilder<> tmp(slot->getParent(), std::next(slot->getIterator()));
        tmp.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), slot);
    }
    if (!disposeStack_.empty()) {
        auto& scope = disposeStack_.back();
        bool covered = false;
        for (const auto& d : scope)
            if (d.slot == slot && d.dynamicDispose) { covered = true; break; }
        if (!covered) {
            Disposal d{slot, nullptr};
            d.unwindOnly = true;
            d.dynamicDispose = true;
            scope.push_back(d);
        }
    }
    k8RecvTemps_.push_back(K8RecvTemp{slot, currentFn_});
    return true;
}


void Emitter::flushK8RecvTemps() {
    // Mirrors flushFfiExprTemps / flushAwaitedTaskTemps exactly, including the
    // two retains those document: an entry belonging to ANOTHER function is
    // kept (a nested function body's boundary fires while the enclosing
    // expression is still open), and a diverged (terminated) block drops the
    // entry without emitting — the unwind edge's disposeStack_ walk has
    // already reclaimed it and emitting after a terminator is invalid IR.
    llvm::BasicBlock* bb = builder_.GetInsertBlock();
    bool live = currentFn_ && bb && !bb->getTerminator();
    std::vector<K8RecvTemp> keep;
    for (const auto& t : k8RecvTemps_) {
        if (t.fn != currentFn_) { keep.push_back(t); continue; }
        if (!live) continue;
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), t.slot, "k8recv.load");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB =
            llvm::BasicBlock::Create(llvm_, "k8recv.dispose", currentFn_);
        auto* contBB =
            llvm::BasicBlock::Create(llvm_, "k8recv.cont", currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(obj);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
        // Discharge at RUNTIME (not by erasing the registration): the
        // disposeStack_ entry stays but now loads null, so scope end / return /
        // unwind are all no-ops, and a LOOP re-arms the same entry-block alloca
        // each iteration. Same discipline as the two sibling queues.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), t.slot);
    }
    k8RecvTemps_.swap(keep);
}


void Emitter::emitDisposals(const std::vector<Disposal>& scope, bool isUnwind) {
    // fix #2892 / §28.10 (v0.241): the UNWIND-EXCLUSIVE entries (`unwindOnly`,
    // `releaseOnUnwindOnly`) fire when the scope's owned locals never reached
    // their static discharge point, which — now that a `try`'s finally is
    // established at protected-block entry and so no longer defers its body
    // scope's disposal past itself — coincides with `isUnwind` on every edge.
    // (#2869 needed a separate `undischargedOwned` question for the one edge where
    // they diverged; see the declaration in LlvmBackend_impl.hpp.)
    const bool dropUndischarged = isUnwind;
    // fix #1882 (§28.4, v0.306): snapshot the user-code counter so the
    // post-pass propagate check below fires only for a pass that could actually
    // have raised, and bracket the slot so it fires only on a fault THIS pass
    // raised. See the tail of this function for the rule. The bracket is a
    // load at the pass's own entry point, which dominates every block the pass
    // goes on to create.
    //
    // ⚠ EMISSION COST, stated plainly because it is easy to overclaim here:
    // `emitPendingCheck`/`emitPendingCheckIf` ARE byte-identical for every
    // pre-existing caller (they take the `kAllLiveScopes` default), but THIS
    // snapshot is not free — it is emitted at the top of every non-unwind,
    // non-empty pass whether or not the gate goes on to trip, and
    // `bz_exc_addr` carries no attributes, so LLVM cannot DCE the dead ones.
    // A program with no disposal semantics therefore gains one call + load per
    // scope exit. It is the same shape every call site already emits for its
    // own pending check, so it is in line with existing cost rather than a new
    // class of it. The cheap remedy, if this ever shows up in a profile, is to
    // save the insert point here and materialise the snapshot lazily at the
    // tail only when the gate trips — deliberately not done now, because the
    // saved-insert-point dance is the kind of complexity this project trades
    // away for simplicity until the compiler is functionally complete.
    const unsigned dispatchesAtPassEntry = userCodeDisposalCount_;
    llvm::Value* excAtPassEntry = nullptr;
    if (!isUnwind && !scope.empty() && builder_.GetInsertBlock() &&
        !builder_.GetInsertBlock()->getTerminator())
        excAtPassEntry = snapshotInFlightExc();
    for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
        // fix #3070 / §30.5 + §28.3 *Unbound owned temporaries* (v0.249): an
        // awaited Pattern A `Task<T:class>` temporary produced by an anonymous
        // producer call. Its reclaim belongs at the K8 full-expression boundary
        // (flushAwaitedTaskTemps), which NULLS the slot; this entry is the
        // establishment-ordered EXCEPTION-edge cover for the window between the
        // await and that boundary, plus the conservative backstop if no boundary
        // ever reaches it. Freed via bz_task_free — which cascades into the
        // per-`T` result disposer (#1503) and the chained source (#1332), so the
        // borrowed result is disposed with the task exactly as §30.5 requires.
        // Null-guarded, so a discharged (flushed) entry is a no-op on every
        // later edge; checked FIRST because such an entry carries no other flag
        // and must not be mistaken for one of the arms below.
        if (it->awaitedTaskTemp) {
            llvm::Value* tk =
                builder_.CreateLoad(ptrTy(), it->slot, "awaittmp.d.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tk, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB =
                llvm::BasicBlock::Create(llvm_, "awaittmp.d.free", currentFn_);
            auto* contBB =
                llvm::BasicBlock::Create(llvm_, "awaittmp.d.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* freeTaskFn = declareC("bz_task_free",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeTaskFn, {tk});
            // Null after reclaiming: one scope can be traversed twice (a
            // mid-expression binding-atom dispose scope, the same shape the
            // `ffiBuffer` arm below documents), and the reclaim must stay
            // exactly-once.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 it->slot);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #422 / §4.6 / §28.9: a provably-non-escaping `var` string local.
        // Release its single (producer) reference at scope end — on BOTH the
        // normal and the unwind exit (the reference dies either way; a given
        // exit goes one way only, so this runs exactly once per scope exit). The
        // local was proven non-escaping, so nothing else holds the pointer:
        // bz_string_release frees the data+header exactly once (no UAF, no
        // double-free). bz_string_release is null-safe and a no-op on a
        // sentinel/literal — but an eligible local always holds a heap +1 value.
        if (it->stringRelease) {
            llvm::Value* s = builder_.CreateLoad(ptrTy(), it->slot, "str.rel");
            emitStringRelease(s);
            // fix #2641 / §22.4 / §22.3: an inline `out var` recipient's slot is
            // read back after this release — the next loop iteration's callee
            // store through the hidden `out` reference loads it to release what
            // it displaces (#2321). Null it so that load cannot see the pointer
            // just freed. Off for every other stringRelease registration.
            if (it->nullSlotAfterRelease)
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), it->slot);
            continue;
        }
        // #422 / §27.5 / §28.9: a provably-non-escaping `var` closure
        // (function-typed) local. Release its single (producer) reference at
        // scope end — on BOTH the normal and unwind exit (the reference dies
        // either way; a given exit goes one way only, so this runs exactly once
        // per scope exit). The local was proven non-escaping, so nothing else
        // holds the cell pointer: bz_closure_release frees the cell + its env
        // block exactly once (no UAF, no double-free of the cell). Captured
        // values are borrows (§28.9), so the env's CONTENTS are not freed — no
        // double-dispose of a capture. bz_closure_release is null-safe.
        if (it->closureRelease) {
            llvm::Value* c = builder_.CreateLoad(ptrTy(), it->slot, "cl.rel");
            emitClosureRelease(c);
            continue;
        }
        // #1930 / stdlib §5 / §28.9: a provably-non-escaping `var g :=
        // <src>.GroupBy(sel).First()/…` local — the returned IGrouping of a bare-
        // GroupBy element-returning terminal. Its SIBLING groups + the outer array
        // were already reclaimed by the terminal (bz_groupby_free_groups_except),
        // keeping only this returned grouping; free it here (element array + key
        // buffer + group object) at scope end — on BOTH the normal and unwind exit
        // (the reference dies either way; a given exit goes one way only, so exactly
        // once per exit). The local was proven non-escaping, so nothing else holds
        // the pointer: bz_grouping_free frees the grouping exactly once (no UAF, no
        // double-free). bz_grouping_free is null-safe (an *OrNull empty result is
        // null → a no-op).
        if (it->grouping) {
            llvm::Value* g = builder_.CreateLoad(ptrTy(), it->slot, "grp.free");
            auto* gfree = declareC("bz_grouping_free",
                                   llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(gfree, {g});
            continue;
        }
        // fix #3720 / stdlib §5 / §4.8 (v0.294): a provably-non-escaping
        // `var`/`const fe := d.First()/.Last()/.Single()/.ElementAt(i)` local
        // over a STRING-keyed Dictionary/SortedDictionary. `slot` holds the
        // entry's OWN 2-field `{K key; V value}` struct (field 0 = Key, a
        // freshly-rebuilt `bz_string*` with a lone +1 — LlvmBackend_
        // Calls.cpp's isDict block mints exactly one per call, no aliasing;
        // see dictEntryKeyReleaseEligible_'s declaration comment,
        // LlvmBackend_impl.hpp). Release field 0 at scope end — on BOTH the
        // normal and unwind exit (the reference dies either way; a given
        // exit goes one way only, so exactly once per exit) — then null it
        // (matching stringRelease/recordStringFields semantics, a few arms
        // above/below). The local was proven non-escaping, so nothing else
        // holds the string: bz_string_release frees it exactly once (no UAF,
        // no double-free). bz_string_release is null-/sentinel-safe.
        if (it->dictEntryKeyRelease) {
            if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(
                    it->slot->getAllocatedType())) {
                llvm::Value* keyAddr =
                    builder_.CreateStructGEP(st, it->slot, 0, "dictek.addr");
                llvm::Value* keyVal =
                    builder_.CreateLoad(ptrTy(), keyAddr, "dictek.val");
                emitStringRelease(keyVal);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), keyAddr);
            }
            continue;
        }
        // fix #4129 (§29.12/§28.3, stdlib §4.8, v0.295): a `for var entry in
        // dict` loop's key/value snapshot spine — see the field declarations
        // in LlvmBackend_impl.hpp for the full derivation. Mirrors
        // lowerDictEntryFor's own manual endBB code (LlvmBackend_Stmts.cpp)
        // exactly, reading every value back through its slot because this
        // arm fires from a `return`/`throw` reached from an arbitrary point
        // deep inside the loop body, not from a block dominated by the
        // original SSA values (only the entry-block allocas are).
        if (it->dictEntryForSnapshot) {
            auto* freeFnD =
                declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            llvm::Value* keysArrD = builder_.CreateLoad(
                ptrTy(), it->dictEntryForKeysOutSlot, "def.keys");
            llvm::Value* valsArrD = builder_.CreateLoad(
                ptrTy(), it->dictEntryForValsOutSlot, "def.vals");
            llvm::Value* cntD =
                builder_.CreateLoad(i64(), it->dictEntryForCntSlot, "def.cnt");
            // String key AND the loop variable is not captured by any
            // closure in the body: release the in-flight iteration's
            // rebuilt Key (loop-var struct field 0) — null-safe, mirrors
            // endBB's own isStringKey branch. Gated on
            // dictEntryForReleaseInFlightKey (see its declaration,
            // LlvmBackend_impl.hpp) — a captured loop variable's struct was
            // boxed into a heap cell holding a COPY of this same pointer,
            // which an escaping closure may read after this disposal fires;
            // releasing here would free it out from under that alias (a
            // measured use-after-free, fixed before landing). The snapshot
            // spine frees below are unaffected by this gate — they are
            // never aliased by anything Bezel code can observe.
            if (it->dictEntryForReleaseInFlightKey && it->dictEntryForVarSlot) {
                if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(
                        it->dictEntryForVarSlot->getAllocatedType())) {
                    llvm::Value* keyAddr = builder_.CreateStructGEP(
                        st, it->dictEntryForVarSlot, 0, "def.key.addr");
                    llvm::Value* keyVal =
                        builder_.CreateLoad(ptrTy(), keyAddr, "def.key.val");
                    emitStringRelease(keyVal);
                }
            }
            // Free the remaining per-key snapshot boxes at [idx, cnt) — the
            // increment block already freed [0, idx); mirrors endBB's own
            // break-path closing loop verbatim (including mutating the
            // original idxSlot in place — safe, since this arm only runs on
            // a path that is about to leave the function/unwind, and no
            // consumer reads idxSlot again after this point).
            if (it->dictEntryForIsStringKey || it->dictEntryForUseGen) {
                auto* clCondBB = llvm::BasicBlock::Create(
                    llvm_, "def.cl.cond", currentFn_);
                auto* clBodyBB = llvm::BasicBlock::Create(
                    llvm_, "def.cl.body", currentFn_);
                auto* clEndBB = llvm::BasicBlock::Create(
                    llvm_, "def.cl.end", currentFn_);
                builder_.CreateBr(clCondBB);
                builder_.SetInsertPoint(clCondBB);
                llvm::Value* ci = builder_.CreateLoad(
                    i64(), it->dictEntryForIdxSlot, "def.cl.idx");
                builder_.CreateCondBr(
                    builder_.CreateICmpSLT(ci, cntD), clBodyBB, clEndBB);
                builder_.SetInsertPoint(clBodyBB);
                llvm::Value* ckp = builder_.CreateLoad(
                    ptrTy(), builder_.CreateGEP(ptrTy(), keysArrD, ci));
                builder_.CreateCall(freeFnD, {ckp});
                builder_.CreateStore(
                    builder_.CreateAdd(ci, builder_.getInt64(1)),
                    it->dictEntryForIdxSlot);
                builder_.CreateBr(clCondBB);
                builder_.SetInsertPoint(clEndBB);
            }
            builder_.CreateCall(freeFnD, {keysArrD});
            if (it->dictEntryForIsStringKey) {
                llvm::Value* lensArrD = builder_.CreateLoad(
                    ptrTy(), it->dictEntryForLensOutSlot, "def.lens");
                builder_.CreateCall(freeFnD, {lensArrD});
                if (it->dictEntryForWidsOutSlot) {   // fix #4499
                    llvm::Value* widsArrD = builder_.CreateLoad(
                        ptrTy(), it->dictEntryForWidsOutSlot, "def.wids");
                    builder_.CreateCall(freeFnD, {widsArrD});
                }
            }
            builder_.CreateCall(freeFnD, {valsArrD});
            continue;
        }
        // #422 / #2960 / §8.6 / §28.9: a provably-non-escaping `var`
        // Object/interface local whose box-vs-class contents are tracked by the
        // placement layer. Reassignable slots carry `boxReleaseState`: release
        // only when the CURRENT value came from a known box producer. A class,
        // string, closure, null, or other definitely-non-box source stores false,
        // so its overlapping pointer representation never reaches bz_box_release.
        // Unknown Object/interface provenance and escaping locals never receive
        // this Disposal entry (safe leak direction). The null state fallback is
        // the pre-#2960 unconditional form, retained defensively.
        if (it->boxRelease) {
            if (!it->boxReleaseState) {
                llvm::Value* b =
                    builder_.CreateLoad(ptrTy(), it->slot, "box.rel");
                emitBoxRelease(b);
                continue;
            }
            llvm::Value* armed = builder_.CreateLoad(
                llvm::Type::getInt1Ty(llvm_), it->boxReleaseState,
                "box.rel.armed");
            auto* releaseBB = llvm::BasicBlock::Create(
                llvm_, "box.rel.do", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(
                llvm_, "box.rel.cont", currentFn_);
            builder_.CreateCondBr(armed, releaseBB, contBB);
            builder_.SetInsertPoint(releaseBB);
            llvm::Value* b =
                builder_.CreateLoad(ptrTy(), it->slot, "box.rel");
            emitBoxRelease(b);
            builder_.CreateStore(builder_.getFalse(), it->boxReleaseState);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // fix #2900 / §28.9 / §28.3 (v0.240): the `Object`-slot / string-borrow
        // twin of the box-release arm directly above. A non-escaping
        // Object/interface local whose initializer was a bare string-typed
        // identifier (one of `objectStringReleaseEligible_`) holds an
        // independent +1 on the heap string (the bind site in
        // `lowerLocalDecl` emitted `bz_string_retain`); at scope end the
        // state bit is consulted and, if still set, bz_string_release frees
        // exactly the alias's reference (no UAF: the source's own +1 lives in
        // the source's slot; no double-free: the alias is the sole owner of
        // this +1). The state bit lets a plain `o := <non-string>`
        // reassignment flip the slot out of the string-release path so a
        // class instance / box / null is never mis-released as a string.
        // Mirrors `boxRelease` + `boxReleaseState` exactly. `slot` is the
        // Object alloca.
        if (it->objectStringRelease) {
            if (!it->objectStringReleaseState) {
                llvm::Value* s = builder_.CreateLoad(
                    ptrTy(), it->slot, "ostr.rel");
                emitStringRelease(s);
                continue;
            }
            llvm::Value* armed = builder_.CreateLoad(
                llvm::Type::getInt1Ty(llvm_), it->objectStringReleaseState,
                "ostr.rel.armed");
            auto* releaseBB = llvm::BasicBlock::Create(
                llvm_, "ostr.rel.do", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(
                llvm_, "ostr.rel.cont", currentFn_);
            builder_.CreateCondBr(armed, releaseBB, contBB);
            builder_.SetInsertPoint(releaseBB);
            llvm::Value* s = builder_.CreateLoad(
                ptrTy(), it->slot, "ostr.rel");
            emitStringRelease(s);
            builder_.CreateStore(builder_.getFalse(),
                                 it->objectStringReleaseState);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // fix #1747 / §28.9 / §9.7 / §28.3: a plain `var` RECORD local bound
        // from a record-returning CALL (`var p := Mk()`), proven non-escaping
        // by the conservative escape walk (StringEscapeWalker with
        // recordFieldReleaseMode — see LlvmBackend_Setup.cpp). fix #2448 /
        // §9.7 (v0.235): no callee-side retain mints this +1 — #2453 removed
        // the never-wired `emitRecordStringFieldRetains`/`currentReturnIsRecord_`
        // this comment used to credit with that mint (grep-confirmed dead:
        // neither was ever called/set). The actual soundness mechanism is
        // `recordCallResultReleaseEligible_` only admitting a local whose
        // initializer is PROVABLY a fresh, alias-free construction
        // (isFreshRecordConstructionOfExactType /
        // isProvablyFreshRecordFactoryMethod, LlvmBackend_Setup.cpp): each
        // string field's value already carries whatever +1/borrow state it
        // had AT CONSTRUCTION (a by-value return is a bit-copy that touches
        // no refcount), and that eligibility proof is what establishes this
        // local as the value's sole owner. At scope end — on
        // BOTH the normal and unwind exit (the fields die either way; a
        // given exit goes one way only, so each is released exactly once)
        // — every string field is bz_string_release'd and the slot nulled.
        // The local was proven non-escaping, so no alias survives the scope
        // (no UAF) and each field owns exactly one +1 (no double-free).
        // bz_string_release is null-/sentinel-safe so a literal field is a
        // no-op. `slot` is the record's struct alloca; `cls` is the record
        // TypeSymbol used to GEP each string storage member.
        if (it->recordStringFields) {
            // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): `it->clsRef` (the
            // per-instantiation TypeRef, e.g. `Box<string>`), not `it->cls`
            // (the bare TypeSymbol `Box`) — see Disposal::clsRef and
            // emitRecordStringFieldReleases_impl's own comment
            // (LlvmBackend_impl.hpp). Every `recordStringFields` entry sets
            // `clsRef` at registration (LlvmBackend_Setup.cpp /
            // _Serialization.cpp).
            emitRecordStringFieldReleases(it->slot, it->clsRef);
            continue;
        }
        // fix #2425 / §9.8 / §28.3/§28.9 / §35.4: the choice-side counterpart
        // just above — an already-populated deserialize component/payload
        // slot whose type is a value-like choice owning a string (directly
        // or through a nested record/choice payload), guarded against a
        // LATER sibling's read throwing. Runs on BOTH the normal and unwind
        // exit for the same reason `recordStringFields` does (the #2398/
        // #2425 registration is popped WITHOUT disposal once every required
        // component is confirmed present, so this only ever fires on the
        // throw path this scope exists to guard).
        if (it->choiceStringFields) {
            emitChoiceStringFieldReleases(it->slot, it->cls);
            continue;
        }
        // fix #2326 / §9.8 v0.228: the array-side mirror of
        // `choiceStringFields` just above — a non-nullable INLINE-ARRAY
        // deserialize component/payload slot whose element type carries
        // a string. The disposer's element TypeRef drives the per-element
        // dispatch (string → release; record → recurse; choice →
        // choice walker; nested array → recurse). Runs on BOTH the
        // normal and unwind exits, gated on the same `stringReleaseEligible_`
        // class as `stringRelease` / `recordStringFields` /
        // `choiceStringFields` — the registration is popped WITHOUT
        // disposal once every required component is confirmed present
        // (#2398 / #2425 disposal-scope semantics), so this only ever
        // fires on the throw path the scope exists to guard.
        if (it->inlineArrayStringFields) {
            emitInlineArrayStringReleases(it->slot, it->inlineArrayElemType);
            continue;
        }
        // fix #3752 (§28.2 / §27.5 / §28.9, v0.286): the DEFERRED CONTENT
        // release for a `string` / function-typed local captured from a
        // STRICTLY DEEPER scope. `slot` is the cell HOLDER; the value the local
        // owns lives one indirection further in, in the cell. The capturing
        // env's envdtor was suppressed at the capture site (it would have run at
        // the INNER scope's end, freeing a value the local still uses), so this
        // is the single release of that single +1 — on BOTH exit edges, since
        // the reference dies either way and one exit goes one way only.
        // NULL SAFETY: guarded boxing null-initialises the holder in the entry
        // block, and a path that never reached the capture site leaves it null,
        // so the holder load is branch-guarded (both release helpers are
        // null-safe for the CONTENT, but the cell load itself is not).
        // Nulling the cell slot afterwards keeps a re-entered guarded cell (a
        // case arm inside a loop) from releasing the same value twice.
        if (it->capturedCellContentRelease) {
            llvm::Function* hfn = builder_.GetInsertBlock()->getParent();
            llvm::Value* cell =
                builder_.CreateLoad(ptrTy(), it->slot, "cellcnt.cell");
            llvm::Value* live = builder_.CreateICmpNE(
                cell, llvm::ConstantPointerNull::get(ptrTy()), "cellcnt.live");
            auto* doBB = llvm::BasicBlock::Create(llvm_, "cellcnt.do", hfn);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "cellcnt.cont", hfn);
            builder_.CreateCondBr(live, doBB, contBB);
            builder_.SetInsertPoint(doBB);
            llvm::Value* content =
                builder_.CreateLoad(ptrTy(), cell, "cellcnt.val");
            if (it->capturedCellContentString) emitStringRelease(content);
            else emitClosureRelease(content);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), cell);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #429 / §27.5: a boxed capture cell (shared heap allocation from
        // boxLocalSlot) whose owning lambda is proven non-escaping.  `slot`
        // is the ptr-alloca (newSlot) holding the cell pointer.  At scope end
        // we load the cell pointer and call free() on it.
        // ORDER: this entry is registered for the captured-local slot, which
        // was declared BEFORE the lambda local.  In reverse-decl order the
        // lambda local's closureRelease fires FIRST (LIFO), running the envdtor
        // which reads all values through the cell.  Only then does this entry
        // fire and free the cell itself — no UAF.
        // NULL SAFETY: the guarded boxing path null-initialises the cell-holder
        // in the function entry block; free(null) is a no-op (POSIX).
        // BOTH normal and unwind paths: the cell has no live external references
        // at this point (all lambdas referencing it are non-escaping and have
        // been released), so freeing on either path is safe.
        //
        // fix #2334: EXCEPT when skipCellFreeOnUnwind is set — the TaskScope.
        // Spawn special case's "non-escaping" proof depends on the bounding
        // `using var`'s disposal being quiescent, which holds only on the
        // properly-awaited NORMAL edge (see Disposal::skipCellFreeOnUnwind's
        // declaration for the full argument and the #1422 non-blocking-
        // zombie-drain citation). Skip the free on the unwind edge for such
        // an entry — the cell leaks there instead of a use-after-free.
        if (it->boxedCellFree) {
            if (isUnwind && it->skipCellFreeOnUnwind) continue;
            // fix #4165 (§27.5/§28.9, stdlib §4.8, v0.295): if this
            // boxedCellFree entry also carries dictEntryKeyReleaseDeferred
            // (set in consumeLastLambdaCapturedCells when the captured local
            // is a string-keyed DictionaryEntry<string,V>), emit the Key
            // release BEFORE freeing the cell. singleCaptureLocals_ is
            // required for boxedCellFree registration, so exactly one holder
            // and exactly one +1 to balance.
            //
            // Narrow (fix-forward of c1237c9f): the original fix's
            // `dictEntryForVarSlot` lookup via `locals_[ce.key]` returned the
            // REBOUND cell-holder ptr* alloca (lowerLambda's `locals_[decl]
            // = newSlot` at Exprs2.cpp:1664 rebinds to the boxed cell holder
            // before consumeLastLambdaCapturedCells reads it), not the
            // original DictionaryEntry struct alloca — so the GEP type cast
            // `dyn_cast<StructType>(...->getAllocatedType())` returned null
            // and the key release was a no-op (17500 B / 1000 allocs leak
            // unchanged from the pre-fix HEAD). The original struct alloca
            // is no longer recoverable from locals_[key]. `fix #4612` since
            // populates cellVarSyncSlot_ on the FRESH boxing path too (it was
            // GUARDED-only when this was written), so that map is no longer a
            // reason to avoid — but cellVars_ (keyed by the cell holder, i.e.
            // it->slot itself) is populated by BOTH paths and records the
            // cell's TYPE, which is what the GEP below actually needs — look
            // up the struct type there for the field 0 load.
            if (it->dictEntryKeyReleaseDeferred) {
                if (auto cvIt = cellVars_.find(it->slot);
                    cvIt != cellVars_.end()) {
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(cvIt->second)) {
                        llvm::Value* structPtr;
                        if (it->slot->getAllocatedType()->isPointerTy()) {
                            // Guarded: it->slot is cell holder (ptr*). Load
                            // cell and cast.
                            llvm::Value* cellTyped = builder_.CreatePointerCast(
                                builder_.CreateLoad(ptrTy(), it->slot,
                                                    "dictek.cell.ptr"),
                                st->getPointerTo(), "dictek.cell.cast");
                            structPtr = cellTyped;
                        } else {
                            // Non-guarded: it->slot IS the struct alloca
                            // (the cell is the struct itself; no
                            // indirection).
                            structPtr = it->slot;
                        }
                        llvm::Value* keyAddr = builder_.CreateStructGEP(
                            st, structPtr, 0, "dictek.addr");
                        llvm::Value* keyVal = builder_.CreateLoad(
                            ptrTy(), keyAddr, "dictek.val");
                        emitStringRelease(keyVal);
                    }
                }
            }
            llvm::Value* cell =
                builder_.CreateLoad(ptrTy(), it->slot, "cell.ptr");
            builder_.CreateCall(freeFn(), {cell});
            // A guarded capture may be re-entered (for example, a case arm in
            // a loop). Null the holder after reclaim so the next entry allocates
            // a fresh cell instead of reusing freed storage.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 it->slot);
            continue;
        }
        // #1017: a DEFERRED boxed-cell free is handled in the SECOND pass below
        // (after every main-loop disposal in this scope), not here — the owner
        // disposal reads the object through the cell and must run first.
        if (it->boxedCellFreeDeferred) continue;
        // fix #4509 (§27.5 / §28.9, v0.306): a refcounted capture cell's FRAME
        // reference is dropped in the same SECOND PASS, for the same ordering
        // reason — the owner's disposal reads the object through the cell, so
        // the frame may not let go until every main-loop entry has run.
        if (it->cellRelease) continue;
        // #383 / §31.1: the caught exception object. On a normal handled exit
        // it is freed (destructor + base chain + block, via its vtable); on the
        // unwind path it is SKIPPED — a re-raise/throw leaving the arm keeps it
        // in flight for the enclosing handler. It is non-null here (it was the
        // in-flight exception that matched this arm).
        if (it->caughtException) {
            if (isUnwind) continue;
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), it->slot);
            // §30.4 / §30.9 (#854): a task-fault exception re-raised by `await t`
            // of a BOUND task is BORROWED from the task (codegen marked it via
            // bz_mark_exc_borrowed) — the awaiter does NOT own it, so a SECOND
            // `await t` of the same faulted task must still find a LIVE error_obj.
            // Skip the reclaim + unmark for a borrowed object; a normal (owned)
            // caught exception is reclaimed as before (§28.2/#380: idempotent
            // Dispose + base chain + block reclaim). Branch at runtime on the
            // per-object borrowed-set membership.
            llvm::Function* fn = builder_.GetInsertBlock()->getParent();
            auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                          llvm::Type::getInt8Ty(llvm_), {ptrTy()});
            llvm::Value* bv =
                builder_.CreateCall(isBorrowedFn, {obj}, "exc.borrowed");
            llvm::Value* isB = builder_.CreateICmpNE(
                bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
            auto* borrowBB = llvm::BasicBlock::Create(llvm_, "exc.borrow", fn);
            auto* ownedBB  = llvm::BasicBlock::Create(llvm_, "exc.owned", fn);
            auto* contBB   = llvm::BasicBlock::Create(llvm_, "exc.dispcont", fn);
            auto* drainBB = llvm::BasicBlock::Create(llvm_, "exc.eo.drain", fn);
            builder_.CreateCondBr(isB, borrowBB, ownedBB);
            // Borrowed: the fault is owned by the task, not this awaiter — the
            // awaiter must NOT reclaim it (a SECOND `await t` re-raises the same
            // live instance, §30.4). Unmark it; handling has now completed.
            // §30.9 in-flight-deferral (v0.179) terminal act: if the owning task
            // was disposed WHILE this fault was in flight (a faulted task bound
            // inside this `try`, unwound by its own fault — #1288 / #1197), its
            // scope-end recorded the exception for deferred disposal instead of
            // freeing it mid-dispatch. Perform that deferred dispose now, as the
            // terminal act of handling; otherwise leave the live instance to its
            // still-owning task's own scope-end.
            builder_.SetInsertPoint(borrowBB);
            auto* unmarkFn = declareC("bz_unmark_exc_borrowed",
                                      llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(unmarkFn, {obj});
            auto* isDeferredFn = declareC("bz_is_exc_deferred",
                                          llvm::Type::getInt8Ty(llvm_), {ptrTy()});
            llvm::Value* dv = builder_.CreateCall(isDeferredFn, {obj},
                                                  "exc.deferred");
            llvm::Value* isD = builder_.CreateICmpNE(
                dv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
            builder_.CreateCondBr(isD, drainBB, contBB);
            builder_.SetInsertPoint(drainBB);
            auto* clearFn = declareC("bz_clear_exc_deferred",
                                     llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(clearFn, {obj});
            emitDisposeAndReclaim(obj);
            builder_.CreateBr(contBB);
            // Owned: normal reclaim.
            builder_.SetInsertPoint(ownedBB);
            emitDisposeAndReclaim(obj);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §15.3 / §15.1.1 (#747): a move-only owning inline-array local. Dispose
        // each non-null slot of the [N x ptr] alloca via the slot object's own
        // vtable (dynamic dispatch), then null the slot. Runs on BOTH normal and
        // unwind exit — the array as a whole carries no class Dispose (it is a
        // value type, not IDisposable), so unlike a plain owned class local
        // (unwindOnly below) it auto-disposes on normal exit too (§15.3 line 2021:
        // "a move-only local's owned slots are auto-disposed at scope end").
        // Only registered for locals proven never transferred out, so this is the
        // single disposal of each slot's object (no double-free).
        //
        // fix #4552 (§15.1.1 v0.307, ruling #4388): "the array's auto-dispose
        // pass disposes each non-null slot, in reverse slot order ... one
        // pass definition governs all four of this paragraph's endings"
        // (scope end included). This loop iterated 0..N-1 (establishment
        // order); the reassignment pass (lowerOwningInlineArrayReassign,
        // LlvmBackend_Stmts.cpp) already iterates N..1 for exactly this
        // reason. Iterate in reverse to match.
        if (it->inlineOwnedArray) {
            auto* arrTy = llvm::ArrayType::get(ptrTy(), it->inlineSlotCount);
            for (unsigned fwd = 0; fwd < it->inlineSlotCount; ++fwd) {
                unsigned i = it->inlineSlotCount - 1 - fwd;
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, it->slot,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioa.slot");
                llvm::Value* obj = builder_.CreateLoad(ptrTy(), slotAddr, "ioa.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioa.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioa.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            continue;
        }
        // §15.3 / §27.5 (#782): a CAPTURED move-only owning inline-array — its
        // [N x ptr] value lives in the boxed cell (boxLocalSlot copied it there).
        // `slot` is the cell-HOLDER ptr-alloca; load the cell pointer, then dispose
        // each non-null element by GEPing the [N x ptr] over the CELL (heap), and
        // null the slot. The cell itself is freed separately by the boxedCellFree
        // that fires AFTER this. Non-escaping-gated at registration so the cell is
        // alive here and no escaping closure still references the elements.
        //
        // fix #4552 (§15.1.1 v0.307, ruling #4388): same reverse-slot-order
        // fix as the plain `inlineOwnedArray` arm above — a boxed/captured
        // local is disposed at the same "scope end" ending, just through
        // heap-cell storage instead of a plain alloca.
        if (it->inlineOwnedArrayCell) {
            llvm::Value* cell =
                builder_.CreateLoad(ptrTy(), it->slot, "ioac.cell");
            auto* cellNull = builder_.CreateICmpEQ(
                cell, llvm::ConstantPointerNull::get(ptrTy()));
            auto* liveBB = llvm::BasicBlock::Create(llvm_, "ioac.live", currentFn_);
            auto* afterBB = llvm::BasicBlock::Create(llvm_, "ioac.after", currentFn_);
            builder_.CreateCondBr(cellNull, afterBB, liveBB);
            builder_.SetInsertPoint(liveBB);
            auto* arrTy = llvm::ArrayType::get(ptrTy(), it->inlineSlotCount);
            for (unsigned fwd = 0; fwd < it->inlineSlotCount; ++fwd) {
                unsigned i = it->inlineSlotCount - 1 - fwd;
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, cell,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioac.slot");
                llvm::Value* obj =
                    builder_.CreateLoad(ptrTy(), slotAddr, "ioac.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioac.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioac.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            builder_.CreateBr(afterBB);
            builder_.SetInsertPoint(afterBB);
            continue;
        }
        // §28.3 (line 4910): a plain `var` owned local / `owned` parameter. It is
        // auto-disposed ONLY when an exception unwinds out of its scope; on a
        // normal exit it must have reached a discharge point (sema-enforced), so
        // disposing it here would double-dispose a moved-out / returned value.
        // On the unwind path: dispatch its idempotent Dispose and reclaim the
        // block once, null-guarded (an uninitialized/conditional owned local's
        // slot may be null — registration is restricted to definitely-assigned,
        // never-moved/disposed owned locals/params, but the null-check is kept
        // for the nullable-owned and defensive cases). The dispatch is dynamic
        // (runtime class), matching every other class-instance dispose site.
        // #2000 / §28.13 / §28.4 / §28.6: an `unwindOnly` entry whose type is a
        // compiler-known builtin owned handle (ChannelSender<T> / Channel<T> /
        // Shared<T> / Weak<T>) — released via a TYPE-SPECIFIC runtime call
        // (bz_channel_sender_release / bz_channel_dispose / bz_shared_release /
        // bz_weak_release) rather than the generic vtable slot 2, which does
        // not touch the handle refcount and cannot close the channel. After the
        // release the slot is NULLed — same dispose-exactly-once pattern as the
        // class path below — so a sibling / outer disposal registered against
        // the same slot reads null on its own (later, in reverse-declaration-
        // order iteration) unwind path and skips. The unbox is load → null-
        // check → release → store null, identical shape to the class path.
        // fix #4552 / §35.5 (v0.306): `!it->owningChoice` — a case-scrutinee
        // owning-CHOICE temporary registers an `unwindOnly` entry whose slot
        // holds a choice STRUCT, not a pointer. This arm loads a `ptr` from
        // the slot and dispatches a vtable Dispose on it, which for a choice
        // struct would read the tag word as an object pointer. Let such an
        // entry fall through to the `owningChoice` arm below, which carries
        // its own `unwindOnly` gate.
        if (it->unwindOnly && !it->owningChoice) {
            if (!dropUndischarged) continue;   // fix #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "unwind.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "unwind.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            if (it->sharedHandle) {
                auto* fn = declareC("bz_shared_release",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {obj});
            } else if (it->channel) {
                // fix #5852 / §30.15 v0.334: bz_channel_dispose takes
                // (ch, cce_vtable) — the second parameter is the vtable the
                // runtime faults PENDING RECEIVERS with (§30.15 "Closing"). The
                // arm declared {ptrTy()} and passed {obj} alone, so (a) when a
                // 2-param declaration already existed in the module the call
                // failed IR verification (rejected program), and (b) when this
                // arm declared the function first, declareC reused the 1-param
                // type and %rsi held whatever the previous call left there —
                // a garbage vtable memcpy'd into a live receiver's exception
                // object. Same 2-param + builtinVtable_ idiom as the
                // non-unwindOnly channel arm below.
                llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
                {   auto vit = builtinVtable_.find("ChannelClosedException");
                    if (vit != builtinVtable_.end()) cceVt = vit->second;
                }
                auto* fn = declareC("bz_channel_dispose",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {obj, cceVt});
            } else if (it->channelSender) {
                // fix #5852 / §30.15 v0.334: same defect, same arm chain —
                // bz_channel_sender_release(tx, cce_vtable) takes two params and
                // closes the channel with that vtable when it was the last
                // sender. Was {ptrTy()} / {obj}.
                llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
                {   auto vit = builtinVtable_.find("ChannelClosedException");
                    if (vit != builtinVtable_.end()) cceVt = vit->second;
                }
                auto* fn = declareC("bz_channel_sender_release",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {obj, cceVt});
            } else if (it->weakHandle) {
                auto* fn = declareC("bz_weak_release",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {obj});
            } else {
                emitDisposeAndReclaim(obj);
            }
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // per §2.11: a `using var` Subscription auto-disposes by removing its
        // registration through the events runtime — it has no vtable. (Dispose
        // is idempotent, so a null/already-removed handle is safe.)
        if (it->subscription) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sub = builder_.CreateLoad(ptrTy(), addr);
            auto* fn = declareC("bz_subscription_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sub});
            continue;
        }
        // #362 / stdlib §4.5: a `using var` heap collection. Free the backing
        // via bz_array_free; for an owning collection of class elements the
        // element-disposer thunk cascades disposal into each (non-null) element
        // exactly once (the thunk null-checks; the array iterates live slots).
        // narrow #3036 / §28.3 (v0.240+): the unwind-only `releaseOnUnwindOnly`
        // gate (`releaseOnUnwindOnly && !dropUndischarged` ⇒ skip — a
        // discharged `owned` collection parameter's exception-edge cover, set
        // by `registerUnwindOwnedParam`'s collection branch), and the null-
        // guarded dispose + null-store pattern (the class path's
        // dispose-exactly-once): a sibling / outer disposal registered
        // against the same slot reads null on its later unwind path and
        // skips. Without the null-store, a duplicate entry would double-
        // free; without the gate, a discharged parameter's normal-exit
        // disposal would run twice. Both halves are load-bearing.
        if (it->collection) {
            if (it->releaseOnUnwindOnly && !dropUndischarged) continue;   // fix #3036 / #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* arr = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                arr, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB  = llvm::BasicBlock::Create(llvm_, "coll.free", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "coll.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            // fix #2630 / §28.5/§28.11 / stdlib §4.3: a `using var` typed by a
            // BORROWING list interface (IReadOnlyList<T>/IList<T>) whose static
            // head erases the dynamic backing — could be borrowing Array<T>
            // (no disposer) or owning OwningArray<T> (disposer stamped at
            // construction per #2608). When no static element disposer was
            // derived AND the static head is a borrowing list interface, free
            // via bz_array_free_dynamic, which consults the disposer header
            // recorded at construction. An untagged (borrowing) backing yields
            // NULL there and the dynamic call degenerates EXACTLY to
            // bz_array_free(arr, null) — the call it replaces for the
            // borrowing case, bit-for-bit. Mirrors the field free site
            // (LlvmBackend_Disposal.cpp ~L873). Owned IOwningList<T> and
            // concrete OwningArray<T> locals keep the static disposer path:
            // collectionElemDisposer is set by registerUsingDisposal for
            // them and they never set collArrayDynamicFree.
            if (it->collArrayDynamicFree) {
                auto* dynFn = declareC("bz_array_free_dynamic",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(dynFn, {arr});
                // fix #1878 (§28.4, v0.306): the backing is statically erased
                // here (§28.11: a borrowing list interface may be backed by an
                // owning array), so the disposer consulted at runtime MAY be
                // the class-element cascade. Count it — an over-count costs a
                // dead pending check, an under-count swallows a fault.
                ++userCodeDisposalCount_;
            } else {
                llvm::Value* disp = it->collectionElemDisposer
                    ? llvm::cast<llvm::Value>(it->collectionElemDisposer)
                    : llvm::ConstantPointerNull::get(ptrTy());
                auto* fn = declareC("bz_array_free",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {arr, disp});
                // fix #1878 (§28.4, v0.306): "the owning container's cascade"
                // is a disposal point too. bz_array_free walks the live slots
                // calling this thunk, which for a class element dispatches the
                // element's own vtable Dispose — user code, reached WITHOUT
                // emitDynamicDispose, so the pass gate has to be told here.
                if (elementDisposerRunsUserCode(it->collectionElemDisposer))
                    ++userCodeDisposalCount_;
            }
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #394 / stdlib §4.9: a `using var` hash-backed dictionary. Free the
        // backing table (and its malloc'd key bytes) via bz_dict_free; for an
        // OwningDictionary with a class VALUE type the value-disposer thunk
        // cascades disposal into each surviving (non-null) value exactly once
        // (bz_dict_free iterates the live entries; the thunk null-checks). A
        // value DISPLACED earlier by the indexer setter (#388) was already
        // removed from the table and disposed there, so it is not re-disposed
        // here — no double-free. bz_dict_free is null-safe.
        // #429 / §28.9 / §4.10: a Set<string> stores its string elements as
        // KEYS (not values) in the generic-key table. Before bz_dict_free frees
        // each key copy, emit bz_set_string_keys_release to call
        // bz_string_release on every live key's string pointer. This discharges
        // the set's co-owned +1 per element (retained at Add). bz_dict_free is
        // then called with a null value disposer (sentinels, nothing to dispose).
        // narrow #3036 / §28.3: same unwind-only gate + null-store pattern as
        // the collection arm above.
        if (it->dict) {
            if (it->releaseOnUnwindOnly && !dropUndischarged) continue;   // fix #3036 / #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tbl = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tbl, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB  = llvm::BasicBlock::Create(llvm_, "dict.free", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "dict.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            if (it->dictStringKeys) {
                // Set<string>: release each string element's +1 before freeing.
                auto* relFn = declareC("bz_set_string_keys_release",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(relFn, {tbl});
            }
            // fix #4091 / §28.11 / §28.9: OwningSet<T> with T a BORROWING
            // built-in collection — free each non-null key (the inner
            // collection pointer stored in the generic-key table's key slot)
            // BEFORE bz_dict_free reclaims the key copies. The dictKeyDisposer
            // is the per-TypeRef thunk built by elementPolicyFor's #4054
            // borrowing-builtin arm — calls bz_array_free / bz_llist_free /
            // bz_dict_free with NULL inner disposer. Without this an
            // `OwningSet<Array<int>>` (`OwningSet<Stack<int>>` /
            // `OwningSet<Queue<int>>` / `OwningSet<LinkedList<int>>` /
            // `OwningSet<FrozenArray<int>>` / `OwningSet<Dictionary<int,int>>` /
            // `OwningSet<SortedDictionary<int,int>>`) leaks its inner
            // collection's header + data buffer per stored element (issue
            // #4091, measured 96,120 B at 800 iterations for Array<int>).
            // The subsequent bz_dict_free(tbl, null) reclaims the key copies
            // and the table itself — dictKeyDisposer and dictValueDisposer
            // are mutually exclusive here (Set stores elements as KEYS; the
            // value is a sentinel never disposed).
            if (it->dictKeyDisposer) {
                auto* keysDispFn = declareC("bz_set_coll_keys_dispose",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                builder_.CreateCall(keysDispFn, {tbl, it->dictKeyDisposer});
            }
            llvm::Value* disp = it->dictValueDisposer
                ? llvm::cast<llvm::Value>(it->dictValueDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_dict_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {tbl, disp});
            // fix #1878 (§28.4, v0.306): the OwningDictionary value cascade —
            // same reasoning as the collection arm above. dictKeyDisposer is
            // deliberately NOT counted: it is the #4091 borrowing-builtin
            // thunk, which frees inner collection headers and never reaches a
            // user destructor.
            if (elementDisposerRunsUserCode(it->dictValueDisposer))
                ++userCodeDisposalCount_;
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // per §4.6/§4.7: a `using var` LinkedList<T> / OwningLinkedList<T>.
        // Free the node chain via bz_llist_free; for an OwningLinkedList<T> of
        // class/interface elements the element-disposer thunk cascades disposal
        // into each (non-null) element exactly once (same policy as OwningArray —
        // but on the node value pointer, not an array slot).
        // #602 batch 3/4: owningCollectionElemDisposer deleted; d.collectionElemDisposer
        // was set from elementPolicyFor at registerLocal time (covers class, interface,
        // Shared<T>, Weak<T> elements per §28.11 v0.135).
        // narrow #3036 / §28.3: same unwind-only gate + null-store pattern as
        // the collection arm above.
        if (it->linkedList) {
            if (it->releaseOnUnwindOnly && !dropUndischarged) continue;   // fix #3036 / #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* list = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                list, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB  = llvm::BasicBlock::Create(llvm_, "llist.free", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "llist.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            llvm::Value* disp = it->collectionElemDisposer
                ? llvm::cast<llvm::Value>(it->collectionElemDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_llist_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {list, disp});
            // fix #1878 (§28.4, v0.306): the OwningLinkedList element cascade —
            // same reasoning as the collection arm above, on the node value
            // pointer rather than an array slot.
            if (elementDisposerRunsUserCode(it->collectionElemDisposer))
                ++userCodeDisposalCount_;
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` BinarySerializer — free the
        // serializer buffer + state via bz_binser_free.
        if (it->binarySerializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "binser.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "binser.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binser_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` BinaryDeserializer — free the
        // deserializer cursor state via bz_binder_free.
        if (it->binaryDeserializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "binder.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "binder.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binder_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` JsonSerializer — free via
        // bz_binser_free (same as BinarySerializer; kind field differentiates).
        if (it->jsonSerializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "jser.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "jser.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binser_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` JsonDeserializer — free via
        // bz_binder_free (same as BinaryDeserializer; kind field differentiates).
        if (it->jsonDeserializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "jder.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "jder.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binder_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §8.1 / issue #646: a `using var` Stopwatch. Free the timing struct
        // via bz_stopwatch_free. Null-guarded for consistency with other
        // builtin disposal sites.
        if (it->stopwatch) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sw = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sw, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "sw.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "sw.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_stopwatch_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sw});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §7.2 / issue #645: a `using var` FileStream. Dispose via
        // bz_filestream_dispose — flushes buffered writes, closes the OS handle
        // and frees the handle object. Null-guarded like the sites above: an
        // explicit `fs.Dispose()` earlier in the scope nulls the slot, and a
        // failed open stores null, so the guard is what makes both cases a
        // no-op rather than a second dispose.
        if (it->fileStream) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* fs = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                fs, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "fs.dispose", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "fs.cont",    currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_filestream_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {fs});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.1 / §30.8: a `using var` CancellationTokenSource. Dispose via
        // bz_cts_dispose (idempotent: signals all tokens, then frees the source).
        // null-guarded: bz_cts_dispose handles null internally (its disposed flag
        // prevents double-free), but we guard anyway for consistency with other
        // builtin disposal sites.
        if (it->cancellationTokenSource) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* cts = builder_.CreateLoad(ptrTy(), addr);
            auto* fn = declareC("bz_cts_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {cts});
            continue;
        }
        // §28.13 / stdlib §2.16 (#603): a `using var` Shared<T> handle.
        // Null-guarded: bz_shared_release handles null gracefully, but we load
        // and call only when non-null for consistency with other disposal sites.
        if (it->sharedHandle) {
            // #1957: an `owned` Shared<T> PARAMETER discharged on the normal path
            // releases here only on unwind (the discharge site owns the normal
            // exit). A `using var` / auto-dropped owner leaves the flag false and
            // releases on both exits.
            if (it->releaseOnUnwindOnly && !dropUndischarged) continue;   // fix #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "shared.rel",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "shared.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_shared_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §28.13 / stdlib §2.16 (#603): a `using var` Weak<T> handle.
        if (it->weakHandle) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "weak.rel",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "weak.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_weak_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §6.7 / §30.5: a `using var` Task / Task<T>. At scope end, the
        // task's Dispose() runs "but does not block waiting for completion" (§30.5).
        // We implement this via bz_task_fire_and_forget, which registers a
        // continuation that calls bz_task_free when the task eventually completes.
        // This is safe for both already-complete tasks (the continuation fires
        // immediately via bz_task_then's fast path) and still-running tasks
        // (the task struct remains live until the continuation fires). The
        // alternative of calling bz_task_free directly at scope end would be
        // unsafe if the task is still running on the thread pool (UAF). Null-
        // guarded: bz_task_fire_and_forget tolerates a null task gracefully (no
        // continuation is registered for null). Double-free safety: lowerAwait only
        // frees tasks whose operand is an anonymous CallExpr (`await Producer()`),
        // never a named local (`await t`), so a using-var-bound task is fired-
        // and-forgotten exactly once here at scope end.
        // fix #3096 / §30.5 kind 2 (v0.252): the ONE edge on which that scope-end
        // disposal must NOT run is a `return await <this local>` chained return,
        // where §30.5 "defers its disposal … to the outer task's teardown". The
        // return edge nulls this slot immediately before emitAllDisposals
        // (LlvmBackend_Stmts.cpp), so the null guard above is what implements the
        // deferral — and it stays per-edge: every other exit still disposes here.
        if (it->task) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* t = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                t, llvm::ConstantPointerNull::get(ptrTy()));
            auto* fafBB = llvm::BasicBlock::Create(llvm_, "task.faf", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "task.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, fafBB);
            builder_.SetInsertPoint(fafBB);
            // §30.9 fault ownership (v0.179): a faulted task OWNS its fault
            // exception and disposes it exactly once at task disposal — the task
            // LOCAL's scope-end here (bz_task_free / fire-and-forget frees the task
            // struct + its buffers but NOT the Bezel exception object; only Bezel
            // codegen can run its destructor). Three paths:
            //   • BORROWED aggregate (§30.10 / #1529): a WhenAll/WhenAll<T>
            //     aggregate never owns its error_obj (only borrows a reference
            //     from the input task that does) — skip unconditionally; see
            //     bz_task_fault_borrowed below.
            //   • NOT in flight (not borrowed): the fault was already fully handled
            //     (or never awaited) — dispose the exception now.
            //   • IN FLIGHT (borrowed by an awaiter mid-dispatch): a faulted task
            //     bound INSIDE a `try` is unwound by its OWN re-raised fault while
            //     the exception is still routing to the enclosing catch (#1288 /
            //     #1197). Disposing it here would free an object the catch arm's
            //     type-test / binding still reads (UAF / double-free). Instead
            //     record it for deferred disposal (bz_defer_exc_dispose); the catch
            //     arm's normal exit performs the dispose as its terminal act (the
            //     borrowed branch above drains bz_is_exc_deferred). Refinement (a).
            // (A still-running async task is not yet errored here → skipped; its
            // unobserved-fault error_obj is a separate residual, #1021.) A
            // non-faulted task has a null error_obj → skipped. bz_task_free below
            // ignores error_obj, so the now-dangling task->error_obj is never read.
            {
                auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
                llvm::Value* err = builder_.CreateCall(isErrFn, {t}, "task.err");
                auto* edBB = llvm::BasicBlock::Create(llvm_, "task.errdisp",
                                                      currentFn_);
                auto* eaBB = llvm::BasicBlock::Create(llvm_, "task.errcont",
                                                      currentFn_);
                builder_.CreateCondBr(err, edBB, eaBB);
                builder_.SetInsertPoint(edBB);
                // §30.10 / #1529: a WhenAll/WhenAll<T> AGGREGATE task whose
                // error_obj was propagated from an input task's OWNED fault is
                // only BORROWING that reference (spec §30.10: "an aggregate
                // task owns no fault of its own"). Its own scope-end must never
                // reclaim it — the input task remains the sole owner and
                // disposes it at ITS OWN, independent scope end. Skip entirely,
                // unconditionally: without this check, a bound aggregate whose
                // fault was already unmarked-not-deferred by a catch arm (the
                // "owned" branch below) would free the exception out from under
                // the still-live input task, which then double-frees it at its
                // own disposal (the crash this fix closes).
                auto* fbFn = declareC("bz_task_fault_borrowed",
                                      llvm::Type::getInt8Ty(llvm_), {ptrTy()});
                llvm::Value* fb = builder_.CreateCall(fbFn, {t}, "task.faultborrowed");
                llvm::Value* isFB = builder_.CreateICmpNE(
                    fb, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
                auto* borrowedAggBB = llvm::BasicBlock::Create(
                    llvm_, "task.eo.aggborrowed", currentFn_);
                auto* checkBB = llvm::BasicBlock::Create(llvm_, "task.eo.check",
                                                         currentFn_);
                builder_.CreateCondBr(isFB, borrowedAggBB, checkBB);
                builder_.SetInsertPoint(borrowedAggBB);
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(checkBB);
                auto* eoFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
                llvm::Value* eo = builder_.CreateCall(eoFn, {t}, "task.errobj");
                llvm::Value* eoNull = builder_.CreateICmpEQ(
                    eo, llvm::ConstantPointerNull::get(ptrTy()));
                auto* doBB = llvm::BasicBlock::Create(llvm_, "task.eodisp",
                                                      currentFn_);
                builder_.CreateCondBr(eoNull, eaBB, doBB);
                builder_.SetInsertPoint(doBB);
                // In flight (borrowed) → defer; else dispose now.
                auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                              llvm::Type::getInt8Ty(llvm_), {ptrTy()});
                llvm::Value* bv = builder_.CreateCall(isBorrowedFn, {eo},
                                                      "task.eo.borrowed");
                llvm::Value* isB = builder_.CreateICmpNE(
                    bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
                auto* deferBB = llvm::BasicBlock::Create(llvm_, "task.eo.defer",
                                                         currentFn_);
                auto* dispBB  = llvm::BasicBlock::Create(llvm_, "task.eo.dispnow",
                                                         currentFn_);
                builder_.CreateCondBr(isB, deferBB, dispBB);
                builder_.SetInsertPoint(deferBB);
                auto* deferFn = declareC("bz_defer_exc_dispose",
                                         llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(deferFn, {eo});
                // §30.9 / #3030: null the task's error_obj field on the
                // DEFER path too. The catch arm's terminal act is the
                // disposer of record (this is the in-flight-deferral
                // contract); leaving the field non-null lets
                // bz_task_fire_and_forget's FAST path below re-dispose the
                // in-flight exception, and the catch arm then reads the
                // freed pointer (UAF). The dispBB branch (synchronous
                // dispose) gets the same null-write a few lines below.
                auto* clearEoDeferFn = declareC("bz_task_clear_error_obj",
                                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(clearEoDeferFn, {t});
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(eo);
                // §30.9 / #3030: null the task's error_obj field after dispose
                // (third of three sites: emitTaskDisposal at line ~446, the
                // OwningTCS field-store disposal at line ~1740, and THIS site
                // — the Task<...> result-binding / await-fault scope-end
                // disposal of an owned task local in a `try { ... } catch`
                // arm or the await-result re-raise). All three funnel into
                // bz_task_fire_and_forget (or its checked sibling) below; the
                // same dangling-field guard is required to keep #3030's
                // FAST-path error_obj-disposal from re-disposing a freed
                // pointer.
                auto* clearEoFn = declareC("bz_task_clear_error_obj",
                                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(clearEoFn, {t});
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(eaBB);
            }
            // §30.5 / #1258, #1503: attach the per-T disposer for the
            // produced-but-unconsumed owned class result of an OwningTask
            // <T:class> BEFORE the task is reclaimed below. bz_task_free
            // (called either synchronously by the fast path just below, when
            // the task is already done, or later by bz_task_fire_and_forget's
            // completion continuation when it is not) is what actually runs
            // the disposer, guarded so it never double-frees a result an
            // awaiter already took (a consuming await/`.Result` nulls the
            // result slot first via bz_task_take_result) and never fires on a
            // faulted task (no result was produced).
            //
            // #1503: this used to call bz_task_dispose_owned_result directly
            // here, which only ever disposed a result already sitting in the
            // task's buffer AT THIS EXACT SYNCHRONOUS POINT — a task still
            // running at this scope-end drop (task->result not yet set) saw a
            // no-op, and the per-T disposer was never retained anywhere, so
            // its eventual result leaked once bz_task_fire_and_forget's
            // continuation later called plain bz_task_free. Persisting the
            // disposer ON THE TASK via bz_task_set_result_disposer instead
            // means it survives to whichever bz_task_free call actually
            // reclaims the task, matching §30.5's "the Dispose() call
            // attaches a continuation that disposes the produced result
            // whenever it appears" for a not-yet-settled task.
            // §30.15 (v0.179, #1524): a Channel<T>.Receive() task overrides the
            // ordinary "attach a disposer, then fire-and-forget" sequence below.
            // The synchronous already-faulted-at-scope-end exception disposal
            // above (the `if (err) {...}` block) still applies unchanged — it is
            // about the fault OBJECT, orthogonal to the channel ITEM the give-
            // back protocol is about. What must NOT happen for a Receive() task
            // is (a) attaching a result_disposer that would eventually DESTROY a
            // produced item (bz_task_free's default for an unconsumed owned
            // result), or (b) plain fire-and-forget's continuation silently
            // dropping a not-yet-produced item once it arrives. Both are exactly
            // what spec §30.15 calls "the one place §30.5's dispose-continuation
            // semantics do not apply to a pending result": instead
            // bz_channel_receive_abandon deregisters a still-parked waiter, or
            // gives a produced item back to the channel's head, so "no message
            // is consumed or lost."
            if (it->channelReceiveTask) {
                auto* fn = declareC("bz_channel_receive_abandon",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {t});
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
                continue;
            }
            if (it->taskOwnedResultDisposer) {
                auto* setDispFn = declareC("bz_task_set_result_disposer",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy(), ptrTy()});
                builder_.CreateCall(setDispFn, {t, it->taskOwnedResultDisposer});
            }
            // §30.10 (v0.179 / #1520): in checked disposal mode, route this
            // abandonment through bz_task_fire_and_forget_checked instead of
            // the plain entry point. If the task is STILL RUNNING at this
            // scope-end drop (the genuinely-abandoned-continuation case the
            // scope-end error_obj handling above deliberately skips, per the
            // #1021 comment), its eventual continuation may resume past this
            // point and touch state this scope's OTHER disposals just tore
            // down — the §30.1 source-outlives-settlement violation this
            // whole backstop exists for. Spec §30.10: "a DisposedAccessException
            // raised in an abandoned continuation is first reported through
            // the same implementation-defined diagnostic channel as
            // FireAndForget's logging, then swallowed." An unchecked build
            // never emits the disposed-flag guard that raises
            // DisposedAccessException in the first place, so the plain
            // (unchanged) entry point is correct and zero-cost there.
            if (checkedDisposal_) {
                llvm::Value* daVt = llvm::ConstantPointerNull::get(ptrTy());
                { auto vit = builtinVtable_.find("DisposedAccessException");
                  if (vit != builtinVtable_.end()) daVt = vit->second; }
                auto* fn = declareC("bz_task_fire_and_forget_checked",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {t, daVt});
            } else {
                auto* fn = declareC("bz_task_fire_and_forget",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {t});
            }
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.6 / issue #974: a `using var` TaskCompletionSource<T> /
        // OwningTaskCompletionSource<T> / TaskCompletionSource.
        // Disposal sequence (all null-guarded; runs on both normal and unwind):
        //   1. bz_tcs_fault_if_pending(tcs, ioe_vtable): if the inner task has not
        //      been completed yet, fault it with InvalidOperationException so that
        //      any awaiter wakes up with an error rather than hanging indefinitely.
        //   2. Error-obj cleanup on the inner task (mirrors the d.task arm above):
        //      if the task is faulted, dispose the error object exactly once.
        //   3. bz_task_fire_and_forget(inner_task): release the task struct when it
        //      has finished running (immediate if already done, deferred if running).
        //   4. bz_tcs_free(tcs): free the bz_tcs_t wrapper itself.
        if (it->tcs) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tcs = builder_.CreateLoad(ptrTy(), addr, "tcs.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tcs, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "tcs.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "tcs.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitTcsRelease(tcs);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // fix #2921 (spec §28.9 / §28.3 / §28.2, v0.240): a `using var` bound to
        // a VTABLE-LESS BUILT-IN CLASS whose release no arm above spells (today:
        // `Random`). Routed through `emitVtablelessBuiltinRelease` — the SAME
        // resolution an explicit `x.Dispose()` takes — so the scope-exit and
        // explicit spellings cannot drift about what releases a given built-in,
        // which is exactly how `Random` came to leak on one path and SIGSEGV on
        // the other. Null-guarded (a ctor that threw before the local was
        // assigned, or a moved-out slot), and the slot is nulled after so a
        // re-entrant / unwind pass is a no-op.
        if (it->vtablelessBuiltin) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr, "vlb.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "vlb.free", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "vlb.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            emitVtablelessBuiltinRelease(it->vtablelessBuiltin, obj);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.5 / §24 (#2026): a `using var` EventSource<T>/AsyncEventSource<T>
        // — a bz_event_source_t* with no vtable. Free the subscriber list + struct
        // via bz_event_source_free (null-safe: `if (!s) return;`, so a ctor that
        // threw before the local was assigned, or a moved-out slot, is safe). The
        // freed source does NOT cascade into the borrowed handler closures (those
        // are owned by their Subscriptions, disposed separately). Null-guard for
        // consistency with other builtin disposal sites; null the slot after so a
        // re-entrant / unwind pass is a no-op (no double-free).
        if (it->eventSource) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* src = builder_.CreateLoad(ptrTy(), addr, "esrc.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                src, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "esrc.free", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "esrc.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_event_source_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {src});
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // fix #1832 / stdlib §6.5 (v0.187): a `using var` SharedEventSource<T>
        // — a bz_event_source_t* with no vtable, freed via
        // bz_shared_event_source_free (null-safe). Same null-guard /
        // null-after discipline as `it->eventSource` above so a ctor that
        // threw before the local was assigned (or a moved-out slot) is safe,
        // and a re-entrant / unwind pass is a no-op. The runtime entry point
        // is the only difference from `eventSource` — the synchronized
        // quiescence discipline lives there, so a future mutex acquired at
        // free time won't disturb the instance path.
        if (it->sharedEventSource) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* src = builder_.CreateLoad(ptrTy(), addr,
                                                   "sesrc.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                src, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "sesrc.free",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "sesrc.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_shared_event_source_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {src});
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.2 / issue #647: a `using var` Mutex. Dispose via bz_mutex_dispose
        // (marks disposed, drops the Mutex object's core ref; destroys the
        // underlying pthread_mutex when the last Lock is also released).
        // Null-guarded: bz_mutex_dispose is null-safe, but we guard for
        // consistency with other builtin disposal sites.
        if (it->mutex) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* mx = builder_.CreateLoad(ptrTy(), addr, "mx.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                mx, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "mx.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "mx.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            auto* fn = declareC("bz_mutex_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {mx});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.2 / issue #647: a `using var` Lock. Release via bz_lock_release
        // (idempotent per §28.2: bz_lock_release checks the released flag).
        // Null-guarded: bz_lock_release handles null (null Lock? on timeout path).
        if (it->lock) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* lk = builder_.CreateLoad(ptrTy(), addr, "lk.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                lk, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "lk.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "lk.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_lock_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {lk});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.3 / issue #647: a `using var` Semaphore. Dispose via bz_semaphore_dispose
        // (marks disposed, drops the Semaphore object's core ref; destroys the
        // underlying sem_t when the last SemaphoreToken is also released).
        if (it->semaphore) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sem = builder_.CreateLoad(ptrTy(), addr, "sem.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sem, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "sem.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "sem.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            auto* fn = declareC("bz_semaphore_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sem});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.3 / issue #647: a `using var` SemaphoreToken. Release via
        // bz_semaphore_token_release (idempotent per §28.2: released flag
        // prevents double-release).  Null-safe (handles null SemaphoreToken?
        // on the timeout path).
        if (it->semaphoreToken) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tok = builder_.CreateLoad(ptrTy(), addr, "stok.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tok, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "stok.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "stok.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_semaphore_token_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {tok});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // issue #545: a `using var` Buffer<T>. Free via bz_buffer_free (null-safe
        // plain free — value elements carry no disposal obligation, so no per-element
        // callback is needed). Null-guarded for consistency with other builtin disposal.
        if (it->buffer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* buf = builder_.CreateLoad(ptrTy(), addr, "buf.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                buf, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "buf.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "buf.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_buffer_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {buf});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977 / #985: a `using var`
        // CString, WString, or RawBytes. All are produced by static From /
        // FromBytes factories. CString/WString are bare malloc'd buffers
        // (bz_string_to_utf8z / bz_string_to_utf16z) released by a single
        // null-guarded free(). RawBytes (fix #985) is a TWO-LEVEL allocation —
        // a { ptr, len } struct whose `ptr` owns a separate byte buffer — so it
        // is released via bz_rawbytes_free, which frees the buffer THEN the
        // struct, exactly once. The disposal contract (stdlib §10.x destructor)
        // says "releases the underlying buffer when this instance allocated it".
        //
        // fix #2823 / §28.3 / §32.5 (v0.273): the same arm also serves an
        // `owned c: CString` / `owned w: WString` / `owned b: RawBytes`
        // PARAMETER registered by `registerUnwindOwnedParam`'s new sibling
        // branch (#2823, Setup.cpp ~16156). The `releaseOnUnwindOnly && !
        // dropUndischarged` skip gate mirrors the collection / dict /
        // channelSender / channel / shared / mutex / lock / semaphore /
        // semaphoreToken / tcs / subscription arms (#3036 / #2869): a discharged
        // owned parameter (explicit Dispose(), returned, or moved out on the
        // normal path) registers `releaseOnUnwindOnly = true`, so the disposal
        // fires only on the unwind edge — the normal-path discharge site is
        // the sole release on its edge; an auto-dropped owner leaves the flag
        // false and frees on both exits. Without this gate, a disposed-then-
        // thrown parameter would dispose AGAIN here on its normal-path scope-
        // end (or on the unwind edge if the throw precedes the explicit
        // Dispose), and free() / bz_rawbytes_free on a freed pointer is a
        // genuine double-free (NOT idempotent — malloc'd buffers have no
        // released-flag machinery). // per §28.3, §32.5; stdlib §10.1–§10.3.
        if (it->ffiBuffer) {
            if (it->releaseOnUnwindOnly && !dropUndischarged) continue;   // fix #2823 / #3036 / #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* fbuf = builder_.CreateLoad(ptrTy(), addr, "ffibuf.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                fbuf, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "ffibuf.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "ffibuf.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            if (it->rawBytes) {
                // fix #985: free the owned buffer AND the { ptr, len } struct.
                auto* fn = declareC("bz_rawbytes_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {fbuf});
            } else {
                builder_.CreateCall(freeFn(), {fbuf});
            }
            // fix #2733 / §28.3 (v0.240): null the slot after reclaiming, exactly
            // as the `unwindOnly` arm above does. Required now that #2733
            // registers EXTERN-ARGUMENT buffer temporaries through this arm: it
            // has no `isUnwind` gate, so one scope can be traversed twice.
            //
            // ⚠ THE SHAPE THAT ACTUALLY FORCES THIS is a BINDING ATOM, not the
            // `catch`-resumes-normally shape an earlier draft named. Review
            // could not construct the catch shape: emitUnwindDisposals walks only
            // down to unwindScopeDepth_, and lowerTry sets that floor
            // (LlvmBackend_Stmts.cpp:3313) BEFORE the body scope is pushed, so on
            // a throw the body scope is disposed on the unwind edge and its
            // normal-path scope-end code is not reached on that path. The real
            // one: lowerBindExpr pushes a dispose scope MID-EXPRESSION
            // (LlvmBackend_Exprs2.cpp:7137) and runs emitDisposals(isUnwind=false)
            // at :7176 before popping, so a temp produced in an atom body is
            // freed at the atom's completion and would then be freed AGAIN by
            // flushFfiExprTemps at the enclosing boundary — a guaranteed double
            // free on the same straight-line path. That hazard is INTRODUCED by
            // #2733 (pre-fix there was no disposeStack_ entry) and closed here.
            // Harmless for the `using
            // var` registrations this arm already served (scope end is their last
            // use); it makes the reclaim idempotent on every path, which is
            // §28.3's dispose-exactly-once.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §30.15 (#600 phase c): a `using var` Channel<T>. Dispose via
        // bz_channel_dispose (closes, faults pending receivers with
        // ChannelClosedException, disposes undelivered items in reverse order
        // per §28.11, frees the struct). The ChannelClosedException vtable is
        // retrieved from builtinVtable_ (set up by setupBuiltinExceptions).
        if (it->channel) {
            // #1957: an `owned` Channel<T> PARAMETER discharged on the normal
            // path disposes here only on unwind; a `using var` / auto-dropped
            // owner leaves the flag false and disposes on both exits.
            if (it->releaseOnUnwindOnly && !dropUndischarged) continue;   // fix #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* ch = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                ch, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "ch.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "ch.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Get ChannelClosedException vtable (may be null before setup).
            llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
            {   auto vit = builtinVtable_.find("ChannelClosedException");
                if (vit != builtinVtable_.end()) cceVt = vit->second;
            }
            auto* fn = declareC("bz_channel_dispose",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {ch, cceVt});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #628 / stdlib §4.9 v0.135: a `using var` DrainedEntry<K,V> — the
        // loop variable from `for var entry in d.Drain() do … end`. The entry
        // struct is {K key; V* value} allocated on the stack. At scope end:
        //   1. Load V* from field[1] and dispose+reclaim the owned class value.
        //   2. If K is string: load the bz_string_t* from field[0] and release
        //      it (bz_string_release) — the drain runtime transferred ownership
        //      of the heap-allocated key string to the loop entry struct.
        // The slot is a struct AllocaInst, not a pointer-to-heap-object; no
        // boxedStorage detour — load fields directly via GEP.
        if (it->drainedEntry) {
            // drainedEntryKeyType / drainedEntryValType carry the LLVM types for
            // fields 0 and 1 of the entry struct (same layout as the alloca in
            // lowerDictDrainFor: { keyLlvm, valLlvm }). They may be null if
            // lowerTypeRef failed at registration time; skip the whole disposal
            // in that case (should not happen in well-formed code).
            llvm::Type* kTy = it->drainedEntryKeyType;
            llvm::Type* vTy = it->drainedEntryValType;
            if (kTy && vTy) {
                llvm::StructType* entStTy =
                    llvm::StructType::get(llvm_, {kTy, vTy});
                // Step 1: dispose the owned V class value (field[1]).
                // Guard: V must be a pointer type (class). OwningDictionary<K,V>
                // requires V: class per §4.9, so in well-formed code this always
                // holds; the guard prevents codegen crashes on ill-formed programs.
                if (vTy->isPointerTy()) {
                    llvm::Value* valFldPtr =
                        builder_.CreateStructGEP(entStTy, it->slot, 1, "de.val.ptr");
                    llvm::Value* valPtr =
                        builder_.CreateLoad(ptrTy(), valFldPtr, "de.val");
                    llvm::Value* isNullVal = builder_.CreateICmpEQ(
                        valPtr, llvm::ConstantPointerNull::get(ptrTy()));
                    auto* dispBB = llvm::BasicBlock::Create(
                        llvm_, "de.dispose", currentFn_);
                    auto* afterValBB = llvm::BasicBlock::Create(
                        llvm_, "de.after_val", currentFn_);
                    builder_.CreateCondBr(isNullVal, afterValBB, dispBB);
                    builder_.SetInsertPoint(dispBB);
                    // Dispose via dynamic dispatch (emitDisposeAndReclaim handles
                    // the vtable call + bz_free; dynamic dispatch is correct for
                    // both user and builtin V class types).
                    emitDisposeAndReclaim(valPtr);
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), valFldPtr);
                    builder_.CreateBr(afterValBB);
                    builder_.SetInsertPoint(afterValBB);
                }
                // Step 2: if K is string, release the owned key string.
                if (it->drainedEntryKeyIsString) {
                    llvm::Value* keyFldPtr =
                        builder_.CreateStructGEP(entStTy, it->slot, 0, "de.key.ptr");
                    llvm::Value* keyPtr =
                        builder_.CreateLoad(ptrTy(), keyFldPtr, "de.key");
                    emitStringRelease(keyPtr);
                }
            }
            continue;
        }
        // §30.15 (#600 phase c): a `using var` ChannelSender<T>. Release via
        // bz_channel_sender_release (decrements sender count; may close the
        // channel if this was the last sender).
        if (it->channelSender) {
            // #1957: an `owned` ChannelSender<T> PARAMETER discharged on the
            // normal path releases here only on unwind (the discharge site — a
            // move/return/.Dispose() — owns the normal exit); a `using var` or an
            // AUTO-DROPPED owned sender (§28.3, the never-discharged owner) leaves
            // the flag false and releases on both exits. As the last sender this
            // release closes the channel (§30.15).
            if (it->releaseOnUnwindOnly && !dropUndischarged) continue;   // fix #2869
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tx = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tx, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "tx.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "tx.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
            {   auto vit = builtinVtable_.find("ChannelClosedException");
                if (vit != builtinVtable_.end()) cceVt = vit->second;
            }
            auto* fn = declareC("bz_channel_sender_release",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {tx, cceVt});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §6.10 (v0.175, #808): an `await using var` TaskScope. Normal
        // exit: bz_task_scope_dispose_async + await (cancel, drain, swallow;
        // IOE iff Join was never awaited). Unwind / non-async: the
        // non-blocking bz_task_scope_dispose_unwind zombie drain. The runtime
        // frees the scope struct on both paths.
        if (it->taskScope) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sc = builder_.CreateLoad(ptrTy(), addr, "tscope.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sc, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "tscope.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "tscope.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Null the slot BEFORE the (possibly awaited) dispose: the
            // normal-path dispose task may FAULT (missing-Join IOE, §6.10),
            // whose re-raise unwinds into the isUnwind disposal pass — which
            // must find the slot already null (the runtime freed the scope
            // when the dispose task completed; a second dispose through the
            // stale pointer is a UAF).
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            emitTaskScopeDispose(sc, isUnwind);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §28.12 / §30 (`await using var`): async disposal path. On the NORMAL
        // exit (not isUnwind): call DisposeAsync() directly, await the returned
        // Task (a proper suspend point), then free the block (physical reclamation).
        // On the UNWIND path (exception in flight): synchronous Dispose() fallback —
        // the resource is reclaimed on every exit path.
        if (it->asyncDispose) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "awaitusing.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "awaitusing.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // fix #3494 (§28.12 step 2's fault edge, v0.283): null the slot
            // BEFORE the (possibly awaited) disposal, exactly as the `taskScope`
            // arm above already does and for the same reason. A `DisposeAsync`
            // that completes BY THROWING re-raises inside `emitAwaitSuspend`'s
            // fault block, which runs `emitUnwindDisposals()` — the isUnwind
            // pass over this very stack. With the slot still holding the object
            // there, that pass disposed it AGAIN, through the SYNCHRONOUS
            // fallback (this call passes a null `disposeAsyncFn` when isUnwind),
            // so the destructor body ran on the fault edge — the body §28.12
            // step 1 says `DisposeAsync` supersedes. The reclamation §28.12 step
            // 2 does require on that edge is emitted by the `onFault` hook
            // inside `emitAsyncDisposeAwaitAndFree`, so nulling here does not
            // strand the object: it moves its reclamation from the wrong
            // mechanism to the right one.
            //
            // Moving the store is safe on every other edge too: nothing between
            // here and the call reads the slot, and the normal path's own
            // reclamation uses the already-loaded `obj`, not a reload.
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            // Pass the direct function pointer; null means sync fallback.
            // On the unwind path, always use sync fallback (pass null).
            emitAsyncDisposeAwaitAndFree(
                obj, isUnwind ? nullptr : it->disposeAsyncFn,
                isUnwind ? nullptr : it->asyncDisposeCls,
                isUnwind ? -1 : it->disposeAsyncVSlot);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §29.7 (#623): an iterator enumerator whose Dispose is at vtable slot 1
        // (iterator format: [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current).
        // Call Dispose(self) via slot 1 (which runs pending finally blocks inside
        // the state machine and sets state=-1) then free the heap frame.
        if (it->iterEnumDispose) {
            // fix #3603 / #3539 (§29.7 / §27.5 / §28.9, v0.286): ask
            // `boxedStorage` for the address that actually HOLDS the object,
            // exactly as the `dynamicDispose` / class / `asyncDispose` arms
            // of this same loop already do. This arm was the ONE disposal arm
            // that read `it->slot` RAW — a second, drifting spelling of the
            // question "given a disposal slot, where does the object live?"
            // that `boxedStorage` is the single authority for.
            //
            // The drift is only observable for a §27.5 (#808) LAMBDA-CAPTURED
            // local: `boxLocalSlot` rebinds such a local's storage to a
            // ptr-alloca holding a shared heap CELL, and the object lives
            // INSIDE the cell. The raw load therefore produced the CELL
            // pointer, and the slot-1 dispatch below read a "vtable" out of
            // the cell header and jumped through it — a wild jump (measured
            // on r12-cycle-1: `AddressSanitizer: SEGV ... PC is at a
            // non-executable region` in `bz$iter$<M>$MoveNext`, for
            // `var seq: IEnumerable<T> := coll;` and the `using var` twin
            // alike, on both the abandonment and the full-drain edge).
            //
            // For every NON-captured slot `boxedStorage` returns the slot
            // itself and emits no instruction, so this arm's IR is
            // byte-for-byte unchanged for them.
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "iter.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "iter.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Dispatch vtable slot 1 (Dispose).
            llvm::Value* vt = builder_.CreateLoad(ptrTy(), obj);
            llvm::Value* dispSlot =
                builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(1)});
            llvm::Value* dispFnPtr = builder_.CreateLoad(ptrTy(), dispSlot);
            auto* dispTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
            builder_.CreateCall(dispTy, dispFnPtr, {obj});
            // Dispose now freed the frame — null the storage (the cell's
            // payload for a captured local, the slot itself otherwise), so a
            // second disposal through the same storage is a no-op.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #383 / §28: a `using var` owning a class/interface object whose
        // concrete class is not statically known here (interface-typed slot,
        // call returning `owned T`, or `move x.f`). Dispose dynamically through
        // its own vtable, null-guarded — an interface/nullable slot may hold
        // null. The dynamic dispatch runs the most-derived destructor + base
        // chain (idempotent, §28.2) and reclaims the block once (#380); the slot
        // is nulled afterward so any further disposal through it is a no-op
        // (§28.2; the #396 slot-null precedent).
        // §35.4 (v0.200, #2045 stage 3): an OWNING-classified choice local.
        // The active-case disposal emits a switch on the runtime tag and
        // disposes the active case's owned payloads. Fires on BOTH the
        // normal and unwind exit — like an `inlineOwnedArray` disposal, the
        // slot value is whatever the active case holds at scope end, and
        // disposing it is the SOLE path for those payloads.
        if (it->owningChoice) {
            // fix #4552 / §35.5 (v0.306): an `unwindOnly` owning-choice entry
            // is a case-scrutinee temporary whose NORMAL-edge disposal is
            // emitted at the construct's completion (lowerCaseStmt); this
            // entry covers only the arm-body throw edge.
            if (it->unwindOnly && !dropUndischarged) continue;
            // fix #4552 / §35.9 (v0.306): route through the LOCATION helper,
            // which unwraps a `C?` slot's `{ i8 hasValue, <choice> }` before
            // walking the active case. `owningChoiceRef` is the local's own
            // (instantiated) TypeRef, so it carries the `nullable` bit;
            // `registerOwningChoiceDisposal` admits a `C?` local exactly as it
            // admits a `C` one, and reading the wrapper's leading bytes as the
            // tag is what made the nullable spelling leak (or, for a different
            // case order, dispatch through the wrong payload slot).
            if (it->owningChoiceRef)
                emitOwningChoiceLocationDisposal(it->slot, it->owningChoiceRef);
            else
                emitOwningChoiceActiveCaseDisposal(it->slot, it->owningChoiceTy,
                                                   it->owningChoiceRef);
            continue;
        }
        // fix #2115 / §28.9 / §35.4: a VALUE-LIKE choice local whose
        // construction mints a +1 string payload. The active-case string
        // release emits a switch on the runtime tag and calls
        // bz_string_release on the active case's string payload field(s). Fires
        // on BOTH the normal and unwind exit (the value dies either way; a
        // given exit goes one way only → exactly once per exit). Registered
        // ONLY for a non-escaping local (the escape walk's verdict), so the
        // slot is the sole owner of the +1 and no alias can double-free.
        if (it->valueLikeChoiceStrRelease) {
            emitValueLikeChoiceActiveCaseStrRelease(
                it->slot, it->valueLikeChoiceStrReleaseTy,
                it->valueLikeChoiceStrReleaseRef);
            continue;
        }
        if (it->dynamicDispose) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "using.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "using.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitDisposeAndReclaim(obj);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        if (it->objectReclaim) {
            // fix #5437 (§28.9 / §28.2 / §8.1, v0.315). `Object` is a
            // trivial-dispose CLASS (§8.1 declares no destructor and no fields),
            // so §28.9's rule — "the compiler frees the undischarged value at
            // its owner's release point" — applies to `new Object()` instances.
            // What was missing is the RECLAIM: `Object`'s vtable Dispose slot
            // holds the shared no-op `bz$exDispose` (correct — §8.7 / §28.2 put
            // no `Dispose()` on `Object`), so the instance never reached
            // `emitReclaimBlock` and its 8 bytes leaked at every holder.
            //
            // The guard is the crux, and it is a RUNTIME test on purpose. §5.1
            // makes `Object` the universal reference type ("any type to
            // `Object` … value types … by boxing (§8.6)"), so an `Object`-typed
            // slot can hold a §8.6 BOX as well as an `Object` instance —
            // MEASURED: `var o: Object := 42;` allocates a 24-byte box, and
            // `o = <box>` stores one into an `Object` slot. Boxes are
            // runtime-managed (§28.9) and are reclaimed by their OWN rule; a
            // static type test cannot tell the two apart, and freeing a box here
            // would be a soundness regression, not a leak fix. So: reclaim iff
            // word 0 is the `Object` vtable this module built.
            //
            // The vtable comparison is exact rather than "not a box" for two
            // reasons. (1) It fails SAFE in the direction that matters: an
            // unrecognized value is left alone, i.e. a bounded leak, never a
            // free of memory the runtime still owns. (2) It keeps §8.6 boxes,
            // and any other vtable-bearing value an `Object` slot can hold, on
            // their own release rules — this arm changes the `Object`
            // instance's fate ONLY.
            //
            // The disposition itself is `emitReclaimBlock` alone, not
            // `emitDisposeAndReclaim`: §8.1 gives `Object` no destructor and no
            // owned fields, so there is no logical-disposal step to run, and
            // calling the vtable's slot 2 would re-enter the very shared no-op
            // this fix deliberately does not touch.
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto objVtable = builtinVtable_.find("Object");
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "obj.reclaim",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "obj.reclaim.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            if (objVtable != builtinVtable_.end()) {
                // Word 0 of the object is its vtable pointer. Compare it against
                // the `Object` vtable by ADDRESS: the vtable is a module-local
                // constant array (`bvt$Object`, InternalLinkage), so the address
                // identity is the type identity, exactly as `emitIsA`'s vtable
                // dispatch and `bz_exc_reclaim_causechain`'s vtable-table gate
                // already rely on.
                llvm::Value* word0 = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateBitCast(
                        obj, llvm::PointerType::get(ptrTy(), 0)));
                llvm::Value* isObj = builder_.CreateICmpEQ(
                    word0, llvm::cast<llvm::Constant>(objVtable->second));
                auto* reclaimBB = llvm::BasicBlock::Create(
                    llvm_, "obj.reclaim.free", currentFn_);
                auto* skipBB = llvm::BasicBlock::Create(
                    llvm_, "obj.reclaim.skip", currentFn_);
                builder_.CreateCondBr(isObj, reclaimBB, skipBB);
                builder_.SetInsertPoint(reclaimBB);
                emitReclaimBlock(obj);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(skipBB);
                builder_.CreateBr(contBB);
            } else {
                // No `Object` vtable in this module means nothing in it ever
                // allocated one, so the slot cannot hold an instance; skip the
                // reclaim rather than guess.
                builder_.CreateBr(contBB);
            }
            builder_.SetInsertPoint(contBB);
            continue;
        }
        if (disposerFns_.find(it->cls) == disposerFns_.end()) continue;
        // CG18 / §28.2: dispatch Dispose dynamically through the object's vtable
        // (idempotent) and reclaim the block once, so a `using var` bound to a
        // subclass instance runs the correct runtime Dispose. This scope-end
        // disposal runs exactly once per scope (normal and unwind exits are
        // mutually exclusive; a break/continue terminates the block first).
        // §27.5: a captured ("boxed") `using var` local lives in a shared
        // heap cell; load the object through the cell, not the cell pointer.
        // #415 / §28.2 / §28.5 (line 5066): an earlier explicit `x.Dispose()`
        // now RECLAIMS the block and NULLS this slot (the explicit-dispose
        // lowering in LlvmBackend_Calls.cpp), so this scope-end site must skip a
        // null slot — reloading null and calling emitDisposeAndReclaim(null)
        // would dispatch through a null vtable. Null-guard (mirroring the
        // dynamicDispose path) and null the slot after reclaim so the object is
        // freed EXACTLY ONCE across the explicit + scope-end paths (no double-
        // free): whichever site disposes first reclaims and nulls; the other
        // finds null and skips.
        llvm::Type* ety = nullptr;
        llvm::Value* addr = boxedStorage(it->slot, ety);
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "using.cls.dispose",
                                                currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "using.cls.cont",
                                                currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // #4602 (§28.2, v0.338): a fresh, never-reassigned local's own
        // release may free its block in checked mode (collectCdFreeEligible).
        emitDisposeAndReclaim(obj, cdFreeEligibleSlots_.count(it->slot) != 0);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
    // #1017 SECOND PASS: free any boxed capture cells whose plain boxedCellFree
    // was suppressed (boxedCellFreeDeferred). This runs AFTER every main-loop
    // disposal in this scope — including the owner's reads-through-cell disposal
    // (using var / collection / dict / subscription / ... — it loads the object
    // THROUGH the cell) and every capturing closure's release — so the cell has
    // no live reader or env reference left. Freeing here (instead of the
    // suppressed boxedCellFree, which LIFO order would have fired BEFORE the owner
    // disposal → UAF) closes the 8-byte capture-cell leak. free(null) is a no-op
    // (guarded boxing may leave the cell-holder null). Exactly-once: a deferred
    // entry is registered ONLY where boxedCellFree was suppressed (mutually
    // exclusive with boxedCellFree) and deduped per slot, so the cell is freed
    // exactly once across both the normal and the unwind exit (a given exit goes
    // one way only). Fires on both paths, symmetric with boxedCellFree —
    // EXCEPT skipCellFreeOnUnwind entries (fix #2334; see boxedCellFree's own
    // handling above and Disposal::skipCellFreeOnUnwind's declaration).
    for (const auto& d : scope) {
        if (!d.boxedCellFreeDeferred) continue;
        if (isUnwind && d.skipCellFreeOnUnwind) continue;
        llvm::Value* cell =
            builder_.CreateLoad(ptrTy(), d.slot, "cell.deferred.ptr");
        // fix #4165 (§27.5/§28.9, stdlib §4.8, v0.295): if this deferred
        // entry also carries dictEntryKeyReleaseDeferred, emit the DictionaryEntry
        // Key release BEFORE freeing the cell — the same GEP field 0 +
        // bz_string_release as dictEntryKeyRelease (LlvmBackend_Setup.cpp's
        // emitDisposals arm), firing at the identical point with the identical
        // safety case: singleCaptureLocals_ is required for boxedCellFreeDeferred
        // registration, so exactly one holder and exactly one +1 to balance.
        //
        // d.dictEntryForVarSlot stores the ORIGINAL local's struct alloca
        // (NOT the rebound cell holder; set at registration time in Exprs2.cpp).
        // Use it to get the struct type for the GEP — the slot may be the
        // struct itself (non-guarded: newSlot == slot) OR a cell holder
        // (guarded: newSlot != slot). In the guarded case the local's struct
        // was copied INTO the cell at boxing time and is still readable through
        // the cell; the SECOND PASS runs after all reads-through-cell disposals,
        // so the value is provably dead (no UAF). Mutually exclusive with
        // dictEntryForSnapshot's own use of this field (the for-var snapshot
        // path is a completely different disposal kind; the two flags never
        // coincide).
        if (d.dictEntryKeyReleaseDeferred) {
            // fix #4165 (§27.5/§28.9, stdlib §4.8, v0.295) — narrow
            // (fix-forward of c1237c9f): see FIRST PASS note. Look up the
            // cell's struct type via cellVars_ (keyed by the cell holder
            // d.slot, set on both boxing paths in boxLocalSlot). The original
            // struct alloca is no longer recoverable from locals_[key] (it is
            // rebound to the cell holder by lowerLambda before this consumer
            // runs). (cellVarSyncSlot_ was GUARDED-only when this was written;
            // `fix #4612` populates it on the non-for-var FRESH path too. It
            // still is not what this needs: the cell itself holds the
            // DictionaryEntry struct by value, and cellVars_ records its type.)
            if (auto cvIt = cellVars_.find(d.slot);
                cvIt != cellVars_.end()) {
                if (auto* st = llvm::dyn_cast<llvm::StructType>(cvIt->second)) {
                    // The slot is either the struct itself or a cell holder.
                    // Check the slot's allocated type: struct = direct
                    // access, ptr* = load cell and cast.
                    llvm::Value* structPtr;
                    if (d.slot->getAllocatedType()->isPointerTy()) {
                        // Guarded: d.slot is cell holder (ptr*). The cell
                        // pointer is already loaded into `cell` above
                        // (before this block, line ~7956). Cast and use.
                        llvm::Value* cellTyped = builder_.CreatePointerCast(
                            cell, st->getPointerTo(),
                            "dictek.deferred.cellcast");
                        structPtr = cellTyped;
                    } else {
                        // Non-guarded: d.slot IS the struct alloca.
                        structPtr = d.slot;
                    }
                    llvm::Value* keyAddr = builder_.CreateStructGEP(
                        st, structPtr, 0, "dictek.deferred.addr");
                    llvm::Value* keyVal = builder_.CreateLoad(
                        ptrTy(), keyAddr, "dictek.deferred.val");
                    emitStringRelease(keyVal);
                }
            }
        }
        builder_.CreateCall(freeFn(), {cell});
        // Null the guarded holder so a later re-entry allocates a fresh cell.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), d.slot);
    }

    // fix #4509 (§27.5 / §28.9, v0.306) SECOND PASS, second half: drop the
    // DECLARING FRAME's reference to each refcounted capture cell this scope
    // owns. Placed beside #1017's deferred free and after it for the identical
    // ordering reason — every reads-through-cell disposal in this scope has
    // run, so the frame has no further access path through the cell.
    //
    // WHAT IS DIFFERENT from the loop above, and it is the point of the fix:
    // that one FREES, under a static proof that no env outlives the frame.
    // This one RELEASES, under no such proof — the cell is freed here only if
    // the runtime count says the frame was the last holder, and stays alive for
    // an env that outlived it. That is why this arm can be armed for the cells
    // #1017's cannot: the cells an escaping capturer reaches
    // (`declsCapturedByEscaping_`), which before this had no reclaim site at
    // all and simply leaked, 8 B per capture (#4509: 1592 B over 200
    // iterations, measured identical with and without #1882's check).
    //
    // BOTH EDGES, exactly once: a given exit goes one way only, so the normal
    // and unwind emissions of this pass are alternatives, not a pair. There is
    // no `skipCellFreeOnUnwind` analogue here — that flag exists because a
    // FREE on the unwind edge could outrun a #1422 zombie drain still reading
    // the cell; a release cannot, since a reader that still holds a counted
    // reference keeps the cell alive by construction.
    //
    // THE HOLDER IS NULLED afterwards, exactly as `boxedCellFree` nulls its
    // own, and for a reason that is easy to get wrong the other way (this code
    // did, and the regression is worth recording). A scope can be RE-ENTERED:
    // a captured local declared inside a `while` body has its declaring scope
    // exited once per iteration, so this release runs once per iteration — but
    // the cell HOLDER is a function-entry alloca that keeps the previous
    // iteration's cell pointer whenever the next iteration does not reach the
    // capture site again (a capture behind an `if`, say). Without the null,
    // iteration 1 releases iteration 0's cell a SECOND time; with an env still
    // holding it, that is a use-after-free, measured on
    // `codegen_const_narrowing_capture_2597.loop_declared_const_captures_per_iteration_spec_27_5`
    // (`const t` bound behind an `if` inside a `while`, captured by a closure
    // that outlives the loop). Nulling makes the re-entry release a null, which
    // `bz_cell_release` treats as a no-op, and leaves `boxLocalSlot`'s guard to
    // allocate a fresh cell on the next capture — the same contract the free
    // above relies on. The frame's reference is gone either way once this has
    // run, so the holder is genuinely dead at this point.
    for (const auto& d : scope) {
        if (!d.cellRelease) continue;
        llvm::Value* cell =
            builder_.CreateLoad(ptrTy(), d.slot, "cell.rc.ptr");
        builder_.CreateCall(
            declareC("bz_cell_release", llvm::Type::getVoidTy(llvm_),
                     {ptrTy()}),
            {cell});
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), d.slot);
    }

    // fix #1882 (§28.4, v0.306): "The exception propagates from the **disposal
    // point** — ... the `using var`/auto-drop scope-end site ...". A destructor
    // that throws during THIS pass leaves its exception parked in the bz_exc
    // slot (the generated `DisposeCore`'s `dtor.threw`/`dtor.restore` tail,
    // #1546) and returns normally; nothing here ever looked. The exception then
    // sat in the slot until some LATER, unrelated pending check picked it up —
    // typically the §22.13 static-init guard of the next static access, which
    // is usually OUTSIDE the `try` whose body held the disposal point, so it
    // escaped the enclosing `catch` as UNHANDLED (the reported repro: a
    // `using var` disposed inside a `try`/`catch Exception`, exit 70 instead of
    // the catch arm running). With no later check in the function at all the
    // same omission SWALLOWED it silently instead (#1878's face). Both
    // manifestations, and their maddening dependence on unrelated surrounding
    // code, are this one missing check.
    //
    // WHY AT THE END OF THE PASS, not after each disposal. §28.4: "In a
    // multi-value cleanup pass ... a throwing disposer does not skip the
    // remaining values: every remaining disposal still runs. If a later
    // disposal in the same pass also throws, the later exception supersedes the
    // earlier one". Checking per entry would branch out on the first fault and
    // skip the rest; checking once at the end lets the whole pass run and lets
    // each `DisposeCore`'s own entry-exception supersede tail (#2358) do the
    // §31.2 record-before-drop accounting between them. Whatever survives the
    // pass is what propagates — exactly the spec's "N−1 chronological entries"
    // shape.
    //
    // WHY isUnwind IS EXCLUDED: an unwind pass's caller branches to the unwind
    // target itself the moment the pass returns, and a fault raised *during* it
    // has already superseded the in-flight exception inside `DisposeCore`. A
    // check here would only re-route to the same place.
    //
    // WHY THE COUNTER GATE: `emitPendingCheckIf` materialises the enclosing
    // scopes' unwind disposals at its branch target, so emitting it at EVERY
    // scope end — including the overwhelming majority that dispose only strings,
    // closures and capture cells — would duplicate that code for passes that
    // provably cannot raise. `userCodeDisposalCount_` counts the constructs this
    // change is about: a vtable `Dispose` dispatch through `emitDynamicDispose`,
    // and an owning container's element cascade through the `bz$elemDispose`
    // thunk (§28.4's "the owning container's cascade", which reaches the element
    // destructor via `bz_array_free`/`bz_dict_free`/`bz_llist_free` rather than
    // via `emitDynamicDispose`, and so announces itself at those three arms).
    //
    // The gate is SOUND-BY-UNDERCOUNT, NOT EXACT — see the counter's own
    // declaration for the enumeration of routes that reach a user destructor
    // without being counted (nested-collection and `Shared`/`Weak` element
    // thunks, the TCS element thunk, the task-owned result disposer, and the
    // enumerator-frame arm below, which dispatches vtable slot 1 by hand).
    // An uncounted route means the fault keeps BASE behaviour — it stays in the
    // slot for a later check, exactly as before this change — never a
    // double-dispose or a lost object. Those disposal points are pre-existing
    // gaps neither #1878 nor #1882 names, and are tracked separately.
    //
    // WHY THE CHECK IS BRACKETED rather than a plain "is anything in flight":
    // a scope-end pass is routinely reached with somebody else's exception
    // already in the slot — a destructor body on an unwind edge (#1468), and,
    // one context wider, the §29.7 abandonment teardown of an `await for`
    // iterator, which runs the iterator's compiler-generated cleanup (its
    // sync-fallback destructor included) while the exception that abandoned the
    // loop propagates. A plain non-null test fires there on that exception,
    // branches out of the teardown, and leaks the iterator frame — measured at
    // 80 bytes before this was bracketed. `emitDisposalFaultCheck` compares
    // against `excAtPassEntry` instead, so only a fault THIS pass raised routes.
    if (!isUnwind && excAtPassEntry &&
        userCodeDisposalCount_ != dispatchesAtPassEntry) {
        // The scope being torn down is still on disposeStack_ (its owner pops
        // it after this returns), so exclude it — and everything nested inside
        // it, which is already gone — from the unwind edge's disposals. A
        // free-standing vector (a mid-expression atom scope that was never
        // pushed) is not found and keeps the all-live-scopes meaning.
        std::size_t hi = kAllLiveScopes;
        for (std::size_t i = 0; i < disposeStack_.size(); ++i)
            if (&disposeStack_[i] == &scope) { hi = i; break; }
        // A pass over a FREE-STANDING vector — one that is not on
        // disposeStack_ at all — is NOT a scope end, and must not take this
        // check. The only such callers that can trip the gate are the §29.7
        // iterator / async-iterator ABANDONMENT teardowns
        // (`emitDisposals(one)`, LlvmBackend_Iterators.cpp and
        // LlvmBackend_Async.cpp), which run inside the generated enumerator
        // `Dispose` and still have work to finish after this pass: the
        // remaining `IterCleanup` entries, the yielded value, the owned
        // parameters and locals, and the FRAME FREE itself. Branching to
        // `unwindBB()` from there abandons all of it and leaks the frame —
        // measured 88 B (async `await for`) and 48 B (sync `for`) when the
        // abandoned iterator's own `using var` destructor throws. Base
        // behaviour there is to fall through and let a later check find the
        // fault; that disposal point keeps base behaviour and is tracked
        // separately (§29.7 is not one of the disposal points §28.4 or these
        // two issues name). The other free-standing callers,
        // `emitDisposals(stringReleases)`, dispose only strings and never trip
        // the gate, so this guard costs them nothing.
        //
        // ⚠ WHAT THIS CHECK DOES *NOT* CAUSE, recorded because it looks exactly
        // like a regression and was investigated as one (#4509). Emitting the
        // check adds two blocks and a `bz_exc_addr` load to the tail of a
        // scope-end pass, which perturbs `main`'s register/stack layout — and
        // that is enough to move a §27.5 capture cell OUT of LeakSanitizer's
        // conservative stack scan. `codegen_event_subscription_closure_841.
        // capturing_handler_env_released_on_sub_dispose_24_27_5` is a ONE-SHOT
        // LSan row over a `using var` class local captured by a lambda passed
        // to `Event<T>.Subscribe`; its 8-byte cell has no owner (§27.5 / §28.9:
        // the scope-end route declines on `declsCapturedByEscaping_`, and both
        // envdtor routes decline because a lambda "merely passed as an
        // argument" proves nothing about when its env dies — see
        // `sharedCellEnvdtorFreeOwner_`'s declaration, condition (b)). The row
        // passes on base only because the cell pointer stays REACHABLE, not
        // because the cell is freed.
        //
        // Measured (generated code uninstrumented over an ASan-instrumented
        // runtime — the row's own recipe): with this check disabled, one-shot
        // clean but `LSAN_OPTIONS=use_stacks=0` reports the same 8 B / 1 alloc;
        // loop-amplified to 200 iterations, 1592 B / 199 allocs, BYTE-IDENTICAL
        // with and without this check. So the leak is wholly pre-existing and
        // this check neither creates nor widens it. Do NOT narrow the gate to
        // silence that row: narrowing would re-mask a real leak and would drop
        // a §28.4 disposal point the issue this change closes is about. #4509
        // tracks the cell's missing owner; the durable test-side remedy is the
        // loop amplification this project's own oracle rules already require.
        if (hi != kAllLiveScopes)
            emitDisposalFaultCheck(excAtPassEntry, hi);
    }
}


void Emitter::emitAllDisposals() {
    for (auto it = disposeStack_.rbegin(); it != disposeStack_.rend(); ++it)
        emitDisposals(*it);
}


void Emitter::emitLoopDisposals(std::size_t loopScopeDepth) {
    for (std::size_t i = disposeStack_.size(); i-- > loopScopeDepth;)
        emitDisposals(disposeStack_[i]);
}


void Emitter::emitDisposalsInRange(std::size_t lo, std::size_t hi) {
    // fix #1567 / §29.7/§28.10 (v0.182): the normal-exit twin of
    // emitUnwindDisposalsInRange — dispose scopes [lo, hi) innermost-first, but
    // on the NON-unwind (isUnwind=false) path so caughtException entries are not
    // skipped. A `return` routing through a `finally` uses this to dispose only
    // the try-body scopes before the finally and, in the finally's normal-exit
    // dispatch, the enclosing scopes after it — reverse establishment order.
    if (hi > disposeStack_.size()) hi = disposeStack_.size();
    for (std::size_t i = hi; i-- > lo;)
        emitDisposals(disposeStack_[i], /*isUnwind=*/false);
}


}  // namespace bezel::codegen
