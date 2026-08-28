#include "codegen/LlvmBackend_impl.hpp"

#include <set>

namespace bezel::codegen {

// ===========================================================================
// Suspend/resume state-machine scaffolding (§29 iterators / §30 async).
//
// Phase 2a extracted the kind-agnostic machinery — frame layout, the spill set
// (locals live across a suspend point), capture spill/restore, and the resume
// switch — out of the yield-specific iterator emitter so the async (`await`)
// path can reuse it (§30). The iterator (`Yield`) path below layers the
// yield-specific driver (IEnumerator<T> MoveNext/Current/Dispose + vtable) on
// top of those helpers. No behavior changes for iterators.
// ===========================================================================

bool Emitter::forInBuiltinEnumeratorNeedsFrameSlot(const ast::ForStmt& fs) {
    // §29.7 (#944): mirror lowerIEnumerableBuiltinFor's family check + the
    // `isArrayBacked == false` decision so collectSuspendLocals reserves a frame
    // field for EXACTLY the for-ins whose lowering creates an iterator-format
    // enumerator (Dispose at vtable slot 1). Any divergence would leave the
    // wired-in enumerator slot pointing at a missing / mismatched field.
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;
    if (iterTy->declaration) return false;  // user class → lowerIEnumerableFor
    std::string_view n;
    if (iterTy->astNode && iterTy->astNode->name &&
        !iterTy->astNode->name->parts.empty()) {
        n = iterTy->headName();
    } else if (!iterTy->importedName.empty()) {
        n = iterTy->importedName;
    } else {
        return false;
    }
    if (n != "IEnumerable" && n != "IReadOnlyCollection" && n != "IReadOnlyList" &&
        n != "ICollection" && n != "IList" && n != "IOwningEnumerable")
        return false;
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return false;

    // isArrayBacked == false ⇒ vtable-backed enumerator (the slot-1-Dispose
    // enumerator). Matches lowerIEnumerableBuiltinFor's protocol heuristic.
    if (dynamic_cast<const ast::CallExpr*>(fs.iterable.get())) return true;
    if (dynamic_cast<const ast::NewExpr*>(fs.iterable.get())) return true;
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(fs.iterable.get())) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        auto checkNamed = [&](sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named && rt->declaration;
        };
        auto checkPureIEnumerable = [&](sema::TypeRef* rt) -> bool {
            if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
            if (rt->declaration) return false;
            if (!rt->astNode || !rt->astNode->name ||
                rt->astNode->name->parts.empty())
                return false;
            return rt->headName() == "IEnumerable";
        };
        if (auto* par = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (par->declType) {
                auto* rt = ctx_.resolvedType(*par->declType);
                if (checkNamed(rt) || checkPureIEnumerable(rt)) return true;
            }
        } else if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
            if (checkNamed(loc->declType) || checkPureIEnumerable(loc->declType))
                return true;
        }
    }
    return false;
}

// §27.5 / §30 (#808 exposure): see the header comment on frameCapturedLocals_.
// One pass over every lambda in the frame body; each captured LocalSymbol's
// decl node (a LocalDecl or the anchoring ForStmt) enters the set.
//
// fix #3466 (§27.5 / §30.1, v0.283): the SAME walk also records the captured
// PARAMETER symbols. Both halves answer one question — "which of this frame
// body's variables does a lambda box into a shared cell?" — and the answer
// decides a frame-layout obligation for each storage class (a ptr-WIDE field
// for a local, a ptr field of its OWN for a parameter). Deriving them from two
// walks would be exactly the duplicated-classification-predicate shape that
// keeps generating defects here, so they share one.
void Emitter::computeFrameCapturedLocals(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    frameCapturedLocals_.clear();
    frameCapturedParams_.clear();
    frameLocalPreBoxTy_.clear();   // fix #3484 follow-up; filled by collectSuspendLocals
    std::vector<const ast::LambdaExpr*> lambdas;
    collectAllLambdas(body, lambdas);
    for (const ast::LambdaExpr* lam : lambdas) {
        std::unordered_set<const sema::Symbol*> caps;
        collectCapturedSyms(*lam, caps);
        for (const sema::Symbol* s : caps) {
            if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                if (loc->decl)
                    frameCapturedLocals_.insert(
                        static_cast<const void*>(loc->decl));
            } else if (dynamic_cast<const sema::ParameterSymbol*>(s)) {
                frameCapturedParams_.insert(s);
            }
        }
    }
}

void Emitter::collectSuspendLocals(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::vector<std::pair<const ast::Node*, llvm::Type*>>& out) {
    // #461 (§30 async frames): the frame SLOT type for a suspend-live local
    // must be at least as wide as its alloca, or the per-suspend spill (which
    // stores `alloca->getAllocatedType()` bytes into the slot — see
    // emitAwaitSuspend) overflows into the adjacent frame field and corrupts it
    // (→ SIGSEGV on resume). When neither an annotation nor an initializer
    // exprType is available, default to a POINTER-WIDTH slot (not i32): ptr is
    // large enough to hold any scalar alloca (i32/i64/bool/ptr) without
    // overflow, so a local with an unresolved type can never clobber its
    // neighbours. With a known type the exact lowered type is used (so an
    // `int?`/record/value struct still gets its precise slot and round-trips
    // correctly).
    //
    // §27.5 / §30 (#808 exposure): a lambda-captured local's slot is rebound
    // to a shared CELL POINTER (boxLocalSlot); the frame field must be
    // ptr-typed or the 8-byte cell-pointer spill overflows a narrower value
    // field and corrupts its neighbour (SIGSEGV on resume).
    // frameCapturedLocals_ is computed per frame body.
    //
    // fix #3375: hoisted out of the LocalDecl arm so the §21.1 `const` arm
    // below derives its slot type by the SAME rule rather than a second copy —
    // the two forms differ only in which field carries the annotation
    // (`LocalDecl::type` / `LocalConstDecl::type`) and the initializer
    // (`LocalDecl::initializer` / `LocalConstDecl::value`).
    auto frameSlotTy = [&](const ast::Node* node, const ast::Type* declType,
                           const ast::Expr* init) -> llvm::Type* {
        llvm::Type* ty = ptrTy();
        if (declType) {
            if (auto* rt = ctx_.resolvedType(*declType)) ty = lowerTypeRef(rt);
        } else if (init) {
            if (auto* rt = ctx_.exprType(*init)) ty = lowerTypeRef(rt);
        }
        if (frameCapturedLocals_.count(static_cast<const void*>(node))) {
            // fix #3484 follow-up (§27.5 / §30.1, v0.283): remember what the
            // field WOULD have been. #808 widens this one to ptr because
            // lowerLambda MOVES it onto the cell holder; the pre-boxing alloca
            // is then left with no field, and #3484 made that alloca the
            // fall-back address a null holder reads. buildStateMachineFrame
            // gives it a field of this exact type so the fall-back survives a
            // suspension. Precise type, not ptr — a record/value-struct local
            // spilled into a ptr-width field is the #461 overflow.
            frameLocalPreBoxTy_[node] = ty;
            ty = ptrTy();
        }
        return ty;
    };
    for (const auto& s : body) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            out.push_back(
                {ld, frameSlotTy(ld, ld->type.get(), ld->initializer.get())});
        } else if (auto* lc =
                       dynamic_cast<const ast::LocalConstDecl*>(s.get())) {
            // fix #3375 (§21.1 runtime const locals / §29.3 / §30.2): a `const`
            // local is a SINGLE-ASSIGNMENT local binding — initialized once at
            // its declaration by ordinary runtime evaluation and readable for
            // the rest of its scope, exactly like a `var` in every respect
            // except assignability. It therefore needs a state-machine frame
            // field for the same reason a `var` does: without one its alloca
            // lives only in the current invocation of the resume / MoveNext
            // function, so after a suspend the reload sees a FRESH, zeroed (or
            // garbage) stack slot. Before this arm existed, `const a: int := 41;
            // await ...; return a;` returned stack garbage, a `const s: string`
            // SEGV'd on first use after the await, and a `const d: Dictionary<..>
            // := [...]` literal's §28.9 scope-end reclaim read null out of the
            // slot and skipped the free (the #3375 leak). `lowerLocalDecl`
            // already keys its frame-backing path on the generic `declNode`
            // (LlvmBackend_Setup.cpp) and lowerStmt lowers a LocalConstDecl
            // through it, so registering the node here is all that is needed.
            out.push_back(
                {lc, frameSlotTy(lc, lc->type.get(), lc->value.get())});
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            // The loop variable is anchored on the ForStmt node.
            if (!fs->varName.empty()) {
                llvm::Type* ty = i32();  // range/int loop var default
                if (fs->varType) {
                    if (auto* rt = ctx_.resolvedType(*fs->varType)) ty = lowerTypeRef(rt);
                } else if (fs->iterable) {
                    // #833 (§30.7): an INFERRED loop variable (`await for var x in
                    // seq` / `for var x in coll`) has no annotation, so without
                    // this it took the i32 default — but a class/string/wide
                    // element (a ptr, or a value struct) overflows the i32 frame
                    // slot into the adjacent outerTask field on spill → SIGSEGV.
                    // Derive the element type from the iterable's element
                    // (genericArgs[0]), exactly as the for-loop lowering does
                    // (LlvmBackend_Stmts.cpp). Range loops keep the i32 default
                    // (a RangeExpr iterable has no genericArgs).
                    if (auto* it = ctx_.exprType(*fs->iterable))
                        if (!it->genericArgs.empty() && it->genericArgs[0])
                            ty = lowerTypeRef(it->genericArgs[0]);
                }
                // §27.5 / §30 (#808 exposure): a captured loop variable is
                // cell-boxed too — its frame field must be ptr-typed (see the
                // LocalDecl branch above).
                if (frameCapturedLocals_.count(static_cast<const void*>(fs)))
                    ty = ptrTy();
                out.push_back({fs, ty});
            }
            // §29.7 (#944): a for-in whose lowering creates an iterator-format
            // enumerator (Dispose at vtable slot 1) needs that enumerator — and,
            // when the iterable is a loop-owned temporary (a CallExpr/NewExpr like
            // `P.Inner()`), its SOURCE IEnumerABLE too — to round-trip through
            // DEDICATED frame fields, so the enclosing iterator's Dispose can
            // restore and dispose them when abandoned while suspended INSIDE the
            // loop. Both are independent heap frames (§29.11) live and owned by the
            // loop at such a suspend point; the enclosing Dispose must dispose BOTH
            // (else the abandoned source frame leaks even after its using-vars were
            // freed via the enumerator's Dispose). Two ptr fields are pushed for
            // the iterable node: the FIRST occurrence becomes localFields[iterable]
            // (the ENUMERATOR / GetEnumerator clone) and the SECOND becomes
            // forInSrcFields[iterable] (the loop-owned SOURCE IEnumERABLE).
            // buildStateMachineFrame resolves the duplicate key — the first
            // occurrence fills localFields, the second fills forInSrcFields (so
            // localFields is NOT overwritten). The array protocol has no enumerator
            // and the user-class path (lowerIEnumerableFor) disposes through its
            // own class vtable — neither reserves here. (See
            // forInBuiltinEnumeratorNeedsFrameSlot.)
            if (forInBuiltinEnumeratorNeedsFrameSlot(*fs)) {
                out.push_back({fs->iterable.get(), ptrTy()});  // enumerator
                out.push_back({fs->iterable.get(), ptrTy()});  // source (+1)
            }
            collectSuspendLocals(fs->body, out);
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            collectSuspendLocals(ws->body, out);
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            collectSuspendLocals(ifs->ifBranch.body, out);
            for (const auto& el : ifs->elifBranches)
                collectSuspendLocals(el.body, out);
            if (ifs->hasElse) collectSuspendLocals(ifs->elseBody, out);
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectSuspendLocals(db->body, out);
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // §30.9 / §31.2 (#438): a named local declared in a try BODY, a catch
            // ARM body, or a FINALLY that lives across an in-region await must be
            // frame-backed too (the resume re-enters the region across the
            // suspend). collectSuspendLocals previously skipped try regions because
            // await-in-catch/finally was deferred; now that those are supported,
            // recurse so e.g. `catch E => do var a := await f(); var b := await
            // g(); … end` round-trips `a` through the frame across the cross-thread
            // hop. The catch-arm exception BINDING is frame-backed separately (it
            // is not a LocalDecl — lowerTry registers its slot in
            // asyncHandlerSpillSlots_).
            collectSuspendLocals(ts->body, out);
            for (const auto& arm : ts->catches) {
                const ast::Stmt* ab = arm.body.get();
                if (auto* adb = dynamic_cast<const ast::DoEndBlock*>(ab))
                    collectSuspendLocals(adb->body, out);
                else if (auto* ald = dynamic_cast<const ast::LocalDecl*>(ab)) {
                    // A one-liner `catch E => var a := await f();` arm body.
                    //
                    // fix #3404 (§30.9 / §30.1, v0.283): routed through
                    // `frameSlotTy` instead of keeping a private copy of the
                    // derivation. The copy computed the same lowered type on
                    // every path where a type IS available, and differed on the
                    // two where one is not — it defaulted to `i32()`, which
                    // contradicts #461's rule stated 150 lines above it:
                    //
                    //   "When neither an annotation nor an initializer exprType
                    //    is available, default to a POINTER-WIDTH slot (not
                    //    i32): ptr is large enough to hold any scalar alloca
                    //    (i32/i64/bool/ptr) without overflow, so a local with an
                    //    unresolved type can never clobber its neighbours."
                    //
                    // and it skipped #808's `frameCapturedLocals_` ptr-widening.
                    // What an undersized slot costs was measured rather than
                    // assumed: on a scratch compiler with this branch forced to
                    // `i32()`, the per-suspend spill emits a mistyped 8-byte
                    // `store ptr` into the 4-byte field, and the neighbouring
                    // frame field silently reads back 0 instead of its value —
                    // a WRONG ANSWER with no diagnostic, not a crash. Whether it
                    // is observable at all depends on the next field's alignment
                    // padding: with an 8-aligned `ptr` neighbour the padding
                    // absorbed the overflow and the same program ran clean. That
                    // layout-dependence is exactly why the invariant is stated
                    // once and consulted, rather than re-derived per branch.
                    //
                    // No shape that reaches the old default is reachable from
                    // source on this tree — see the commit message and
                    // tests/codegen/async_catch_arm_frame_slot_3404_test.cpp for
                    // the shapes tried and what rejected each (every initializer
                    // with a null exprType is refused by the front end; an
                    // await-initialized one resolves; and #808's
                    // frameCapturedLocals_ widening cannot apply here at all,
                    // because a one-liner arm's body IS the declaration, so no
                    // lambda in the frame body can reference the local). The
                    // change is therefore a de-duplication that removes a
                    // contradicted invariant, not an observable behaviour fix.
                    out.push_back(
                        {ald, frameSlotTy(ald, ald->type.get(),
                                          ald->initializer.get())});
                }
                // fix #3375 (§21.1): the `const` sibling of the one-liner arm
                // above — `catch E => const a := await f();`. Same reason as the
                // LocalConstDecl arm in the main loop: a §21.1 const local needs
                // a frame field or it does not survive the suspend. (Both
                // siblings now derive their slot type from `frameSlotTy`; the
                // `var` one's private copy, and its i32 default, were removed by
                // #3404 — see the branch directly above.)
                else if (auto* alc =
                             dynamic_cast<const ast::LocalConstDecl*>(ab)) {
                    out.push_back(
                        {alc, frameSlotTy(alc, alc->type.get(),
                                          alc->value.get())});
                }
            }
            if (ts->hasFinally) collectSuspendLocals(ts->finallyBody, out);
        }
    }
}


Emitter::StateMachineFrame Emitter::buildStateMachineFrame(
    sema::MethodSymbol* ms,
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    llvm::ArrayRef<llvm::Type*> extraTrailing,
    const std::string& structName) {
    StateMachineFrame frame;

    // Collect all parameters to capture: `this` (instance methods) then params.
    bool isInstance = !isStaticMethod(ms);
    if (isInstance)
        frame.captures.push_back({nullptr, ptrTy()});  // 'this' capture
    for (auto* param : ms->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(param);
        if (!ps) continue;
        llvm::Type* pty = ptrTy();  // default ptr for class params
        if (ps->declType) {
            if (auto* rt = ctx_.resolvedType(*ps->declType))
                pty = lowerTypeRef(rt);
        }
        frame.captures.push_back({ps, pty});
    }

    // Pre-scan body to find all locals that need to live across a suspend point.
    // §27.5 / §30 (#808 exposure): first compute which locals are lambda-
    // captured (cell-boxed) so collectSuspendLocals plans ptr-typed fields.
    computeFrameCapturedLocals(body);
    collectSuspendLocals(body, frame.bodyLocals);

    // Build state struct type:
    //   { ptr vtable, i32 state, captures..., locals..., extraTrailing... }
    std::vector<llvm::Type*> fields;
    fields.push_back(ptrTy());   // vtable  → field 0
    fields.push_back(i32());     // state   → field 1
    for (auto& [sym, ty] : frame.captures) fields.push_back(ty);  // captures
    frame.localStart = (unsigned)(2 + frame.captures.size());
    for (auto& [node, ty] : frame.bodyLocals) fields.push_back(ty);  // locals
    // Kind-specific trailing fields (e.g. `Yield`'s `current_val`). Record the
    // index of the first one as `currentValField` for the yield driver.
    frame.currentValField =
        (unsigned)(frame.localStart + frame.bodyLocals.size());
    for (llvm::Type* ty : extraTrailing) fields.push_back(ty);

    // fix #3466 (§27.5 / §30.1, v0.283): one POINTER field per lambda-captured
    // PARAMETER, holding that parameter's capture-CELL HOLDER across a
    // suspension. Appended LAST — after `extraTrailing` — precisely so that no
    // index any caller already computed from `captureStart` / `localStart` /
    // `currentValField` (+1, +2 for the async trailing trio) moves. The frame is
    // calloc'd at construction, so a field starts null, which is exactly the
    // "cell not yet allocated" state boxLocalSlot's guard already tests for.
    //
    // Reserved from the frame LAYOUT, not from the capture set actually boxed:
    // whether a given lambda reaches boxLocalSlot is decided later, during body
    // lowering, and the struct body is finalized here. An unused field costs 8
    // bytes in a frame that is already per-call heap-allocated.
    for (auto* param : ms->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(param);
        if (!ps) continue;
        if (!frameCapturedParams_.count(ps)) continue;
        frame.paramCellFields[ps] = (unsigned)fields.size();
        fields.push_back(ptrTy());
    }

    // fix #3484 follow-up (§27.5 / §30.1, v0.283): the LOCAL twin — one field per
    // lambda-captured local, of the local's PRE-BOXING type, so the alloca
    // #3484's fall-back addresses survives a suspension. Appended after the
    // parameter fields for the same index-stability reason. Deduped because
    // §29.7 (#944) pushes a for-in iterable node into bodyLocals TWICE.
    for (auto& [node, ty] : frame.bodyLocals) {
        (void)ty;
        if (frame.localPreBoxFields.count(node)) continue;
        auto pit = frameLocalPreBoxTy_.find(node);
        if (pit == frameLocalPreBoxTy_.end()) continue;
        frame.localPreBoxFields[node] = (unsigned)fields.size();
        fields.push_back(pit->second);
    }

    frame.structTy = llvm::StructType::create(llvm_, fields, structName);
    frame.vtableField = 0;
    frame.stateField = 1;
    frame.captureStart = 2;

    // Build mapping from AST nodes to struct field indices. §29.7 (#944): a
    // for-in iterable node may appear TWICE (enumerator + loop-owned source). The
    // FIRST occurrence fills localFields (the enumerator); a SECOND occurrence of
    // the same node fills forInSrcFields (the source) WITHOUT overwriting
    // localFields. Every other node is unique.
    for (unsigned li = 0; li < frame.bodyLocals.size(); ++li) {
        const ast::Node* node = frame.bodyLocals[li].first;
        unsigned fieldIdx = frame.localStart + li;
        if (frame.localFields.count(node))
            frame.forInSrcFields[node] = fieldIdx;   // second push → source
        else
            frame.localFields[node] = fieldIdx;      // first push → enumerator
    }

    return frame;
}


void Emitter::emitFrameCaptureStores(const StateMachineFrame& frame,
                                     llvm::Value* st) {
    unsigned idx = frame.captureStart;
    for (auto& [sym, ty] : frame.captures) {
        llvm::Value* capField =
            builder_.CreateStructGEP(frame.structTy, st, idx++);
        llvm::Value* val = nullptr;
        if (!sym) {
            // 'this' capture
            val = currentThis_ ? currentThis_ : currentFn_->arg_begin();
        } else {
            auto it = paramSlots_.find(sym);
            if (it != paramSlots_.end())
                val = builder_.CreateLoad(ty, it->second);
        }
        if (val) builder_.CreateStore(val, capField);
    }
}


void Emitter::emitFrameCaptureRestores(const StateMachineFrame& frame,
                                       llvm::Value* self) {
    // §29.8 / §30.1 (#973a): record, FOR THE FUNCTION BEING EMITTED, the frame
    // the restores below read and the capture-field index of every `owned`
    // PARAMETER in it, so a discharge lowered into this body can null the FRAME
    // slot (the runtime moved-from flag) and not merely the alloca it is about
    // to bind. Per-function, so a nested emission cannot clobber it — see the
    // declaration comment.
    SmFrameCaptures* fc = nullptr;
    if (llvm::BasicBlock* bb = builder_.GetInsertBlock()) {
        fc = &smFrameCaptures_[bb->getParent()];
        fc->self = self;
        fc->structTy = frame.structTy;
        fc->ownedParamField.clear();
    }
    unsigned idx = frame.captureStart;
    for (auto& [sym, ty] : frame.captures) {
        llvm::Value* capField =
            builder_.CreateStructGEP(frame.structTy, self, idx++);
        llvm::Value* val = builder_.CreateLoad(ty, capField);
        if (!sym) {
            // 'this' — set currentThis_
            currentThis_ = val;
        } else {
            // param — create a slot and store it
            auto* slot = entryAlloca(ty);
            builder_.CreateStore(val, slot);
            paramSlots_[sym] = slot;
            // fix #3484 follow-up: this store is the parameter's unconditional
            // cross-suspension restore, which is what lets boxedStorage use this
            // alloca as a fall-back address after a resume.
            smRestoredParamSlots_.insert(slot);
            if (fc)
                if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym))
                    if (ps->modifier == "owned" && ty->isPointerTy())
                        fc->ownedParamField[sym] = idx - 1;
        }
    }
}


// §29.8 / §30.1 (#973a): see the declaration comment in LlvmBackend_impl.hpp.
void Emitter::nullStateMachineOwnedParamCapture(const ast::Expr* src) {
    llvm::BasicBlock* bb = builder_.GetInsertBlock();
    if (!bb) return;
    auto fit = smFrameCaptures_.find(bb->getParent());
    if (fit == smFrameCaptures_.end()) return;   // not a state-machine body
    const SmFrameCaptures& fc = fit->second;
    if (!fc.self || !fc.structTy) return;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(src);
    if (!id) return;
    auto* ps = dynamic_cast<sema::ParameterSymbol*>(ctx_.exprSymbol(*id));
    if (!ps || ps->modifier != "owned") return;
    auto it = fc.ownedParamField.find(ps);
    if (it == fc.ownedParamField.end()) return;
    llvm::Value* fptr =
        builder_.CreateStructGEP(fc.structTy, fc.self, it->second);
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), fptr);
}


std::vector<llvm::BasicBlock*> Emitter::emitResumeDispatch(
    const StateMachineFrame& frame, llvm::Value* self,
    llvm::Value* stateGEP, unsigned numSuspends,
    const std::function<void()>& emitDone) {
    // State 0: beginning. States 1..N: after each suspend point. The switch's
    // default + state -1 land in `doneBB`, which emits the kind's exhausted-
    // return.
    auto* doneBB = llvm::BasicBlock::Create(llvm_, "mn.done", currentFn_);
    std::vector<llvm::BasicBlock*> stateBBs;
    for (unsigned s = 0; s <= numSuspends; ++s)
        stateBBs.push_back(
            llvm::BasicBlock::Create(llvm_, "mn.s" + std::to_string(s),
                                     currentFn_));

    llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP);
    auto* sw = builder_.CreateSwitch(curState, doneBB, numSuspends + 1);
    for (unsigned s = 0; s <= numSuspends; ++s)
        sw->addCase(builder_.getInt32((int)s), stateBBs[s]);

    // doneBB: exhausted → kind-specific return.
    builder_.SetInsertPoint(doneBB);
    emitDone();

    // Publish the live state so the per-suspend-point spill/restore (lowerStmt)
    // can run.
    smStateStruct_ = frame.structTy;
    smSelf_ = self;
    smStateGEP_ = stateGEP;
    smStateBBs_ = stateBBs;
    smDoneBB_ = doneBB;
    smNextSuspendState_ = 0;
    smLocalFields_ = frame.localFields;
    smForInSrcFields_ = frame.forInSrcFields;  // §29.7 (#944)

    return stateBBs;
}


bool Emitter::containsYield(const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (dynamic_cast<const ast::YieldStmt*>(s.get())) return true;
        // Recurse into compound statements.
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            if (containsYield(fs->body)) return true;
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get()))
            if (containsYield(ws->body)) return true;
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (containsYield(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (containsYield(el.body)) return true;
            if (ifs->hasElse && containsYield(ifs->elseBody)) return true;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get()))
            if (containsYield(db->body)) return true;
        // §30.7 (#440): recurse into a try / catch / finally so a `yield` inside a
        // protected region still classifies the method as an iterator. The async
        // iterator path then rejects that shape (suspendInTryPosition); a sync
        // iterator's yield-in-try is already rejected earlier by sema flow checks.
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            if (containsYield(ts->body)) return true;
            for (const auto& arm : ts->catches) {
                const ast::Stmt* ab = arm.body.get();
                if (auto* db = dynamic_cast<const ast::DoEndBlock*>(ab)) {
                    if (containsYield(db->body)) return true;
                } else if (dynamic_cast<const ast::YieldStmt*>(ab)) {
                    return true;
                }
            }
            if (containsYield(ts->finallyBody)) return true;
        }
        // #1107/#16: recurse into `case`-statement arms so an iterator whose
        // body yields inside a `case` arm is routed to the iterator state
        // machine (in lockstep with sema's stmtsContainYield). Without this the
        // method would be lowered as a normal function and its arm yields would
        // be mis-emitted.
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s.get()))
            for (const auto& arm : cs->arms)
                if (containsYield(arm.stmts)) return true;
    }
    return false;
}


// fix #2895 (§29.3 / §30.1 / §30.7, v0.240): retract the entire suspend/resume
// frame in one place. Sets every member to the documented neutral value —
// containers cleared, pointers null, indices zero, smKind_ set to Yield (the
// kind with no neutral value but the only one that does not itself OPEN a
// consumer gate once smStateGEP_ is null; see the NestedBodyScope comment on
// the constructor reset, LlvmBackend_Assign.cpp ~4602). Called by the
// StateMachineFrameScope RAII guard at every exit from a state-machine emitter
// (normal return and every diagnostic early-return path).
void Emitter::clearSmFrame() {
    smKind_                    = SuspendKind::Yield;
    smStateStruct_             = nullptr;
    smSelf_                    = nullptr;
    smStateGEP_                = nullptr;
    smStateBBs_.clear();
    smDoneBB_                  = nullptr;
    smNextSuspendState_        = 0;
    smLocalFields_.clear();
    smForInSrcFields_.clear();
    smAllocaToField_.clear();
    smParamCellField_.clear();          // fix #3466
    smLocalPreBoxField_.clear();        // fix #3484 follow-up
    smRestoredParamSlots_.clear();      // fix #3484 follow-up
    smCellFrameFn_             = nullptr;
    iterCurrentValField_       = 0;
    iterElemTy_                = nullptr;
    iterElemSemaTy_            = nullptr;
    iterElemIsOwnedClass_      = false;
    iterCurrentValOwnedField_  = 0;
    iterIsOwningIter_          = false;    // fix #2621
    asyncOuterTaskField_       = 0;
    asyncPendingTaskField_     = 0;
    asyncSpillPtrField_        = 0;
    asyncResultTy_             = nullptr;
    asyncResultSemaType_       = nullptr;
    asyncPatternAResultDisposer_ = nullptr;
    asyncOwnedParamFrame_      = nullptr;
    asyncResumeFn_             = nullptr;
}


void Emitter::collectYields(const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
                           std::vector<const ast::YieldStmt*>& out) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s.get())) {
            // §29.3: only `yield return` consumes a resume-state slot; `yield break`
            // is a terminal that unconditionally returns false from MoveNext without
            // advancing the state counter. Counting it here would create a dangling
            // state block (mn.sN) with no predecessor and no terminator, which the
            // IR verifier rejects. (#437 fix)
            if (!ys->isBreak) out.push_back(ys);
            continue;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            collectYields(fs->body, out);
        else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get()))
            collectYields(ws->body, out);
        else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            collectYields(ifs->ifBranch.body, out);
            for (const auto& el : ifs->elifBranches)
                collectYields(el.body, out);
            if (ifs->hasElse) collectYields(ifs->elseBody, out);
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectYields(db->body, out);
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // §29.6 / §30.7 (#554): `yield return` inside a try body with ONLY a
            // finally is now supported in async iterators. Collect yields from the
            // try body so the resume-switch has the correct number of state blocks.
            // Do NOT collect from catch arm bodies or the finally: §29.6 forbids
            // yield there, and suspendInTryPosition rejects such programs before
            // the resume switch is built.
            collectYields(ts->body, out);
        }
        // #1107/#16/#17: a `yield return` inside a `case`-statement arm consumes
        // a resume-state slot too. Recurse into each block-form arm IN ARM ORDER
        // (the same order lowerCaseStmt lowers them) so the resume-switch
        // allocates the right number of state blocks. Without this the arm
        // yields were uncounted: the yield-return lowering's
        // `nextState < smStateBBs_.size()` guard then failed, the insert point
        // stayed on the already-`ret`'d arm block, and the case lowering's
        // branch was appended after the terminator → "Terminator found in the
        // middle of a basic block" IR-verification crash.
        else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s.get())) {
            for (const auto& arm : cs->arms)
                collectYields(arm.stmts, out);
        }
    }
}

// §29.8 (#2154 part 2): walk the iterator body in EXECUTION order (same order
// as `collectYields`) and record the 1-based yield state at which each
// `owned`-class-typed parameter is transferred out via `yield return`.
// Mirrors the sema-side linear check at Ownership.cpp but extends through
// `try` bodies so the `try { yield return a; } ...; yield return b;` shape
// (forbidden by §29.6 only when there's a catch — a finally-only try is
// allowed) and the equivalent `do/end` block both participate. Any yield
// return that is not a bare identifier-expression of an `owned` parameter
// aborts the scan (`ok=false`); the iterator is then routed through the
// legacy monotone ownership path. `if/else`/`while`/`for` are NOT
// descended — their state ordering is data-dependent and the #2154 narrow
// shape covers a single, statically-determined resume sequence.
static void collectYieldStatesForOwnedParams(
    sema::SemaContext& ctx,
    const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
    unsigned& state,
    std::unordered_map<sema::ParameterSymbol*, unsigned>& out,
    bool& ok) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (!ok) return;
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s.get())) {
            if (ys->isBreak || !ys->value) { ok = false; return; }
            auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                ys->value.get());
            sema::Symbol* sym = id ? ctx.exprSymbol(*id) : nullptr;
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
            if (!ps || ps->modifier != "owned") { ok = false; return; }
            if (out.count(ps)) { ok = false; return; }
            out[ps] = ++state;
            continue;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // Recurse through the protected body only; §29.6 already
            // forbids `yield` in catches / finally, so no leak there.
            collectYieldStatesForOwnedParams(
                ctx, ts->body, state, out, ok);
            continue;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectYieldStatesForOwnedParams(
                ctx, db->body, state, out, ok);
            continue;
        }
        // IfStmt / WhileStmt / ForStmt / lambdas / anything else has
        // data-dependent state ordering — abort the linear scan.
        ok = false;
        return;
    }
}


// §29.7 (#2306): the LOCAL counterpart of the owned-parameter linear scan
// above. For each plain `var L := ...` local of an owned class/interface type
// in the iterator body (a `unwindOnly`-class plain `var`), record the
// 1-based yield state at which L becomes live (declaration reached) and
// the 1-based yield state at which L is transferred out via `yield return L;`.
// `establishedAtState` lets the iterator Dispose distinguish "not yet
// reached" (state S < establishedAtState) from "live" (establishedAtState <=
// S < transferredAtState); a monotone "ever discharged" fact is NOT enough
// because a transferred local is logically moved-from past the L-th yield —
// the state-keyed predicate is required. The scan walks in EXECUTION order,
// extending through `try` bodies and `do/end` blocks (mirroring the
// parameter version) so the multi-yield shape
// `try { yield return a; } ... ; var b := ...; yield return b;` is included.
// If/else/while/for and lambdas have data-dependent state ordering — the
// scan aborts (`ok=false`) and the iterator falls back to the legacy
// monotone path (the existing #1897 leak persists for those shapes, but
// calling the new path with an ambiguous timeline would risk a double-free
// when a local is transferred by a path the linear scan can't statically
// place).
static void collectYieldStatesForOwnedLocals(
    sema::SemaContext& ctx,
    const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
    unsigned& state,
    std::unordered_map<const ast::LocalDecl*,
        std::pair<unsigned, unsigned>>& out,
    bool& ok) {
    // (decl → (establishedAtState, transferredAtState))
    for (const auto& s : stmts) {
        if (!s) continue;
        if (!ok) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            // Only plain (non-`using`) locals are in scope — `using var` has its
            // own disposal path (#821 / smIterDisposalsAtState_) and an
            // autoDropAtScopeEnd plain `var` is the affine-never-discharged
            // case whose disposal runs unconditionally at scope end. The type
            // gate (class/interface vs. collection/task/subscription) is
            // applied at emit time via unwindOwnedLocalClass so the runtime
            // vtable-slot-2 Dispose we emit is sound for the local type.
            if (ld->isUsing || ld->autoDropAtScopeEnd) continue;
            auto oit = out.find(ld);
            if (oit != out.end()) {
                // Re-declaration in the linear scan: not legal Bezel, abort.
                ok = false; return;
            }
            // establishedAtState = next yield state after this declaration.
            // After K yields have been emitted we're at state K (resumed for
            // MoveNext state K); the next yield (yield K+1) advances to state
            // K+1. A declaration seen BEFORE any yield (state=0) becomes live
            // at state 1; one seen between yield K and yield K+1 becomes
            // live at state K+1. So `state + 1` captures it.
            out[ld] = {state + 1, 0};
            continue;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s.get())) {
            if (ys->isBreak || !ys->value) { ok = false; return; }
            // A `yield return <plain bare-identifier local>` (IOwningEnumerable)
            // TRANSFERS L's ownership out via the consumer's TakeCurrent. The
            // sema side (Ownership.cpp #1863) has already markDisposedByName'd
            // the local; the codegen-side question is the SUSPENSION STATE at
            // which that happens, which this scan records. Any other yielded
            // expression (a `new`, a call, a field/member read) does NOT
            // transfer a tracked local — fall through without touching `out`.
            auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                ys->value.get());
            if (id && (id->name == "this" || id->name == "base")) {
                ok = false; return;
            }
            sema::Symbol* sym = id ? ctx.exprSymbol(*id) : nullptr;
            auto* loc = dynamic_cast<sema::LocalSymbol*>(sym);
            if (id && (!loc || !loc->decl || loc->isUsing)) {
                // Bare identifier that isn't a tracked plain owned local —
                // ambiguous, abort.
                ok = false; return;
            }
            if (loc && loc->decl) {
                auto* ldDecl = dynamic_cast<const ast::LocalDecl*>(loc->decl);
                if (!ldDecl) { ok = false; return; }
                auto oit = out.find(ldDecl);
                if (oit == out.end()) {
                    // Yielding a local that wasn't pre-registered (e.g., a
                    // local declared in a branch the linear scan couldn't
                    // see, or a non-owned local) — ambiguous.
                    ok = false; return;
                }
                if (oit->second.second != 0) {
                    // Two transfers of the same local — sema would reject
                    // an affine move-out more than once anyway, so abort.
                    ok = false; return;
                }
                oit->second.second = state + 1;
            }
            ++state;
            continue;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // Recurse through the protected body only; §29.6 forbids `yield`
            // in catches / finally, so no yield-position transfer occurs
            // there for us to miss.
            collectYieldStatesForOwnedLocals(
                ctx, ts->body, state, out, ok);
            continue;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectYieldStatesForOwnedLocals(
                ctx, db->body, state, out, ok);
            continue;
        }
        // IfStmt / WhileStmt / ForStmt / lambdas / anything else has
        // data-dependent state ordering — abort the linear scan. The legacy
        // monotone path (#1897) keeps working; the brief explicitly scopes
        // the new state-keyed path to the linear shape.
        ok = false;
        return;
    }
}


void Emitter::emitIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                         const CallableInfo& ci) {
    if (!ci.stmts) return;
    // fix #2895 (§29.3 / §30.1 / §30.7, v0.240): retract the entire
    // suspend/resume frame on every exit from this emitter (the partial
    // clear at ~1585 happens BEFORE the Dispose body and is defensive; this
    // guard handles the leak that survived into the next function compiled
    // into the module). Spans MoveNext AND the Dispose body — the Dispose
    // body re-publishes the two members it reads (smStateStruct_,
    // asyncSpillPtrField_) just-in-time, so retracting the frame at scope
    // exit does not break it.
    StateMachineFrameScope smFrameScope(*this);
    smKind_ = SuspendKind::Yield;

    // §29.6: a SYNCHRONOUS iterator (returning IEnumerable<T>/IEnumerator<T>)
    // is subject to the same forbidden-`yield`-position rules as an async one
    // (emitAsyncIteratorMethod already enforces these). The forbidden positions
    // are: `yield` inside a `try` body whose surrounding statement has a `catch`,
    // `yield` inside a catch arm body, and `yield` inside a `finally` block
    // (`yield` in a `try` body with only a `finally` is allowed). Without this
    // check a `yield` in a `finally` reached codegen and produced malformed IR
    // ("Terminator found in the middle of a basic block"), and a `yield` in a
    // try-with-catch was accepted unsoundly. suspendInTryPosition detects all
    // three (it only looks for yields here — a sync iterator has no awaits).
    // A diagnostic error suppresses executable emission, so a trivial valid
    // factory body is enough to keep the module well-formed for verification.
    if (suspendInTryPosition(*ci.stmts)) {
        ctx_.diagnostics().report(
            Severity::Error,
            ms->decl ? ms->decl->span.begin : SourceLocation{},
            "forbidden by §29.6: `yield` inside a `try` body with catch arms, "
            "`yield` inside a catch arm body, or `yield` inside a `finally` "
            "block in an iterator (`yield` in a `try` body with only a "
            "`finally` is allowed)");
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
        builder_.SetInsertPoint(entry);
        llvm::Type* rt = fn->getReturnType();
        if (rt->isVoidTy())
            builder_.CreateRetVoid();
        else
            builder_.CreateRet(llvm::Constant::getNullValue(rt));
        return;
    }

    // Determine element type T from the return type's generic arg.
    llvm::Type* elemTy = i32();  // default
    const sema::TypeRef* elemSemaTy = nullptr;  // #1102B
    // Compute the return type name once — used by both elemIsOwnedClass and
    // isOwningIter below.
    std::string_view retName;
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && retRef->astNode && retRef->astNode->name &&
            !retRef->astNode->name->parts.empty())
            retName = retRef->headName();
        else if (retRef && !retRef->importedName.empty())
            retName = retRef->importedName;
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0]) {
            elemSemaTy = retRef->genericArgs[0];
            elemTy = lowerTypeRef(elemSemaTy);
        }
    }

    // #1349 / §29.1 v0.161: true when the return type is IOwningEnumerable<T> —
    // ownership transfers from the iterator to the consumer at each TakeCurrent()
    // call. TakeCurrent (vtable slot 3) checks for null (before-first-advance /
    // after-exhaustion / double-take) and raises InvalidOperationException.
    // The iterator nulls current_val after transfer and on exhaustion.
    //
    // §29.1 v0.236 / #2610: recognize BOTH owning iterator heads.
    // IOwningEnumerable<T> is the spec-sanctioned iterator return (§29.2); the
    // enumerator-direct `IOwningEnumerator<T>` return is the owning sibling of
    // the `IEnumerator<T>`-direct single-use form §29.3/§29.11 supports, reached
    // via `using var en := Gen();` where Gen's declared return type is
    // IOwningEnumerator<T>. Pre-#2610 this head fell through here, so the codegen
    // treated the frame as the BORROWING form: slot 3 was emitted as a plain
    // non-destructive Current (no null-on-take), while the consumer dispatched
    // `.TakeCurrent()` against the same slot expecting ownership transfer — a
    // heap-use-after-free (the producer's #1120 advance/Dispose then touched the
    // value the consumer already owned). Recognizing it makes TakeCurrent, the
    // disposeYieldedVal owning-branch, and the owned-local scan below all apply,
    // so the frame implements the §29.1 owning contract. Mirrors the async
    // isOwningIter extension landed under #2591 (IOwningAsyncEnumerator).
    bool isOwningIter = (retName == "IOwningEnumerable" ||
                         retName == "IOwningEnumerator");

    // §29.8 (#2154/#2217): narrow CLONE-DISPOSES-CAPTURES prototype. The
    // ownership pass records a yield state only when the complete body is a
    // linear sequence of direct owned-parameter transfers. In that shape the
    // first enumerator claims the captures from the source and its Dispose can
    // distinguish not-yet-yielded parameters from already-transferred ones by
    // the current state.
    //
    // #2154 part 2: re-run the same scan HERE in the codegen, extending
    // through `try` bodies (and `do/end` blocks) so the multi-yield shape
    // `try { yield return a; } finally { } ; yield return b;` is included
    // in the state-keyed cleanup. The sema-side check is unchanged (so the
    // ownership flagging for `iteratorAutoDispose` / scope-end reject is
    // exactly what sema told us), but the codegen-side MAP is what the
    // LLVM disposal loop consults per parameter — it can be richer than
    // what sema's narrow prototype accepted without widening sema's
    // contract. The legacy monotone path still applies when the scan
    // yields an ambiguous shape (`ok=false`).
    std::unordered_map<sema::ParameterSymbol*, unsigned> ownedParamYieldStates;
    if (isOwningIter) {
        bool scanOk = true;
        unsigned scanState = 0;
        collectYieldStatesForOwnedParams(
            ctx_, *ci.stmts, scanState, ownedParamYieldStates, scanOk);
        // Cross-check: every owned parameter on the method must appear
        // in the scan, and only `owned` parameters may appear. If sema
        // flagged something we couldn't statically pin to a yield
        // state, prefer the legacy monotone disposal there.
        if (scanOk && !ownedParamYieldStates.empty()) {
            bool allMatched = true;
            for (auto* sym : ms->params) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
                if (!ps || ps->modifier != "owned") continue;
                if (ownedParamYieldStates.count(ps) == 0) {
                    allMatched = false;
                    break;
                }
            }
            if (!allMatched) ownedParamYieldStates.clear();
        } else {
            ownedParamYieldStates.clear();
        }
    }
    // §29.7 (#2306): the LOCAL counterpart of the owned-parameter scan above.
    // Recorded here (before `frame` is built below at buildStateMachineFrame)
    // so the linear walk is purely AST-driven and doesn't depend on the
    // frame layout. Frame-field indices are filled in after the frame is
    // built (see the second pass below, just after buildStateMachineFrame).
    struct LocalKeyedDisposal {
        unsigned establishedAtState = 0;
        unsigned transferredAtState = 0;   // 0 = not transferred (legacy fallback)
        unsigned frameField = 0;
    };
    std::unordered_map<const ast::LocalDecl*, LocalKeyedDisposal>
        ownedLocalYieldStates;
    // §29.8 (#973a): does any `owned` parameter carry the CONDITIONAL frame
    // auto-dispose flag (discharged on some path, every occurrence admitted by
    // common::stateMachineOwnedParamIsFrameNullable)? For such a parameter the
    // frame capture slot is the runtime moved-from flag, and the body nulls it
    // in whichever frame MoveNext is driving. For an `IEnumerable<T>` iterator
    // that frame is the per-loop CLONE, so the clone must also be the OWNER —
    // otherwise the source's Dispose would still see its own non-null slot and
    // free a value the body had already adopted. Reuse the #2154 claim: the
    // captures move source → clone at GetEnumerator and a second
    // GetEnumerator raises InvalidOperationException, which is the correct
    // affine reading anyway (a body that may consume the parameter cannot be
    // replayed). For an `IEnumerator<T>` iterator GetEnumerator is never
    // called, the source IS the driven frame, and its own Dispose does the
    // null-guarded work.
    bool frameConditionalOwnedParams = false;
    for (sema::Symbol* sym : ms->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
        if (ps && ps->modifier == "owned" &&
            ps->iteratorFrameConditionalAutoDispose) {
            frameConditionalOwnedParams = true;
            break;
        }
    }
    // Which owned captures does the CLONE take ownership of at GetEnumerator?
    // Identical to the pre-#973a `hasState` test whenever no conditional
    // parameter is present, so the #2154 path is bit-for-bit unchanged.
    auto cloneOwnsParam = [&](sema::ParameterSymbol* ps) {
        if (!ps || ps->modifier != "owned") return false;
        // (#2154 part 2) include both the sema-side prototype's assignment
        // (linear-flat body) AND the codegen-side extended scan (linear body
        // that descends through `try`).
        if (ps->iteratorMoveYieldState != 0 ||
            ownedParamYieldStates.count(ps) != 0)
            return true;
        // #973a: once the claim is armed, EVERY owned capture the frame
        // disposes must move to the clone — leaving one behind would have the
        // source and the clone dispose the same pointer.
        return frameConditionalOwnedParams &&
               (ps->iteratorAutoDispose ||
                ps->iteratorFrameConditionalAutoDispose);
    };
    bool cloneDisposesOwnedCaptures = frameConditionalOwnedParams;
    for (sema::Symbol* sym : ms->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
        if (!ps || ps->modifier != "owned") continue;
        // Per-param gate: include the param in CLONE-DISPOSES-CAPTURES
        // only if BOTH the sema-side prototype (linear-flat shape
        // already covered by #2154) AND/OR the codegen-side scan
        // (extended linear shape including try-bodies) has placed a
        // yield state on it. A parameter with no state assignment
        // either way is handled by the legacy monotone loop.
        bool semaState = ps->iteratorMoveYieldState != 0;
        bool codegenState = ownedParamYieldStates.count(ps) != 0;
        if (semaState || codegenState) {
            cloneDisposesOwnedCaptures = true;
            break;
        }
    }

    // §29 (#1120): true when T is an owned class or interface AND the return type
    // is IEnumerable<T> (or a related non-owning interface) — meaning the
    // enumerator frame RETAINS ownership of the most-recently-yielded value.
    // The enumerator must dispose it on MoveNext advance (before storing the next)
    // and on Dispose (break / exhaustion / free).  Records/enums/primitives are
    // value types and never need this.
    //
    // Excluded: IOwningEnumerable<T> iterators transfer ownership of each yielded
    // value to the consumer via TakeCurrent() on each MoveNext call — the consumer
    // (not the enumerator) is responsible for disposing it; the enumerator must
    // NOT also dispose current_val or the result is a double-free.
    //
    // §29.1 v0.236 / #2610: the enumerator-direct IOwningEnumerator<T> return is
    // the same owning contract (isOwningIter above) and is excluded for the same
    // reason — without this, the #1120 advance-dispose / exhaustion-dispose
    // machinery (gated on elemIsOwnedClass) would fire for an owning iterator
    // and race the consumer's TakeCurrent (the slot-3 TakeCurrent nulls on take,
    // but the owned-flag-gated borrowing-form disposal is the wrong model for the
    // owning form, whose every yielded value transfers regardless of `new`-vs-
    // local). Keeping the two owning heads symmetric makes IOwningEnumerator<T>-
    // direct behave identically to IOwningEnumerable<T>.
    bool elemIsOwnedClass = false;
    if (elemSemaTy &&
        elemSemaTy->kind == sema::TypeRef::Kind::Named &&
        elemSemaTy->declaration != nullptr &&
        (elemSemaTy->declaration->kind == ast::TypeDeclKind::Class ||
         elemSemaTy->declaration->kind == ast::TypeDeclKind::Interface)) {
        if (retName != "IOwningEnumerable" && retName != "IOwningEnumerator")
            elemIsOwnedClass = true;
    }
    // fix #2738 / §28.3 / §29.2 (v0.282): non-nullable `string` element kind. The
    // enumerator owns the +1 reference minted by `yield return <+1 producer>`
    // (interpolation, `+`, `??`, case, conditional, range slice, ToString) per
    // §29.2 row 3 ("shared reference ... the enumerator does not dispose
    // anything"). Spec says no ownership transfer, but for a borrowing head
    // (IEnumerable<string>, IEnumerator<string>, IList<string>, etc.) the
    // caller-side §28.3 full-expression boundary discharge only fires for a
    // NAMED bind — a `yield return $"g-{i}"` is a full-expression whose result
    // is "consumed" by the yield store, so the §28.3 row "the producer's
    // +1 ... the binding that consumes it ... releases it" was misapplying:
    // the yield store does not bind it (no name), and the slot is reused on
    // the next yield (which would silently drop the mint's +1). The fix
    // threads the ownership through the iterator's own frame using the same
    // current_val_owned flag mechanism as the class-element discipline
    // (§29 #1120), so the next MoveNext advance releases the displaced +1
    // and the iterator's Dispose releases the outstanding one. Narrowly gated
    // on the non-nullable string primitive kind — value-type and nullable-
    // value element iterators keep their existing handling (no +1 reference
    // is ever produced, so no leak can occur). Mutually exclusive with
    // elemIsOwnedClass (class/interface kind) and isOwningIter (owning head)
    // by construction.
    bool elemIsRuntimeManagedPrimitive = false;
    if (!elemIsOwnedClass && !isOwningIter &&
        elemSemaTy &&
        elemSemaTy->kind == sema::TypeRef::Kind::Primitive &&
        elemSemaTy->primitive == sema::PrimitiveType::String &&
        !elemSemaTy->nullable) {
        elemIsRuntimeManagedPrimitive = true;
    }

    // Build the kind-agnostic frame; the yield driver's extra trailing fields are
    // the `current_val` of element type T, then (#747) a spill-area pointer for a
    // suspending for-loop's implicit locals — promoteDemotedSlotsToFrame heap-
    // spills the loop index/array-ptr/enumerator into it when MoveNext crosses a
    // yield, just like the async resume path. calloc-zeroed, so it starts null
    // and stays null (and free(null) is a no-op) for iterators that never spill.
    // Layout: { ptr vtable, i32 state, captures..., locals..., T current_val,
    //           ptr spill }
    // Trailing frame fields (after captures + locals):
    //   [currentValField+0] T          current_val      — last yielded value
    //   [currentValField+1] i8         current_val_owned — 1=enumerator owns it, 0=borrow
    //   [currentValField+2] ptr        spill            — #747 for-loop spill area
    StateMachineFrame frame = buildStateMachineFrame(
        ms, *ci.stmts, {elemTy, i8(), ptrTy()},
        std::string("iter.") + std::string(ms->name));
    auto* stateStructTy = frame.structTy;
    const unsigned VTABLE_FIELD = frame.vtableField;
    const unsigned STATE_FIELD = frame.stateField;
    const unsigned CURRENT_VAL_FIELD = frame.currentValField;
    const unsigned CURRENT_VAL_OWNED_FIELD = CURRENT_VAL_FIELD + 1;  // §29 (#1120)
    const unsigned SPILL_PTR_FIELD = CURRENT_VAL_FIELD + 2;          // #747

    bool isInstance = !isStaticMethod(ms);
    std::string prefix = std::string("bz$iter$") + std::string(ms->name) + "$";

    // §29.7 (#2306): finish the local pre-pass now that `frame` is built.
    // Run the linear scan (it was declared above but its frame-field slot
    // filling was deferred) and populate ownedLocalYieldStates with both
    // (establishedAtState, transferredAtState) and frame field indices.
    // Restricted to IOwningEnumerable iterators — `yield return L;` only
    // transfers a local in the owning form (#1863); for an
    // IEnumerable<owned T> iterator the bare-identifier yield is a borrow
    // and the local's disposal is bounded by sema's never-discharged auto-
    // drop or unwind (no Abandoned-state-keyed gap analogous to the bug).
    // Cross-check: every registered local must have transferredAtState > 0
    // (the linear scan only succeeded when each transfer was at a yield);
    // any non-yield transfer means we can't statically place the
    // discharge — fall back to "do not dispose" (bounded leak) rather than
    // risk a double-free against a runtime non-yield discharge site.
    if (isOwningIter) {
        bool lscanOk = true;
        unsigned lscanState = 0;
        std::unordered_map<const ast::LocalDecl*,
            std::pair<unsigned, unsigned>> lscanOut;
        collectYieldStatesForOwnedLocals(
            ctx_, *ci.stmts, lscanState, lscanOut, lscanOk);
        if (lscanOk && !lscanOut.empty()) {
            bool allTransferred = true;
            for (auto& [ld, pr] : lscanOut) {
                if (pr.second == 0) {
                    allTransferred = false;
                    break;
                }
                if (!unwindOwnedLocalClass(ld)) {
                    // The local's type is not a class/interface instance that
                    // admits dispose-via-vtable-slot-2 (it might be a
                    // collection, a Task, a built-in handle, etc.) — those
                    // have their own disposal paths and aren't this fix's
                    // target.
                    allTransferred = false;
                    break;
                }
            }
            if (allTransferred) {
                ownedLocalYieldStates.clear();
                for (auto& [ld, pr] : lscanOut) {
                    LocalKeyedDisposal info;
                    info.establishedAtState = pr.first;
                    info.transferredAtState = pr.second;
                    auto fit = frame.localFields.find(ld);
                    if (fit != frame.localFields.end())
                        info.frameField = fit->second;
                    ownedLocalYieldStates[ld] = info;
                }
            } else {
                ownedLocalYieldStates.clear();
            }
        } else {
            ownedLocalYieldStates.clear();
        }
    }

    // --- MoveNext(ptr self) -> i1 ---
    auto* moveNextTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
    auto* moveNextFn = llvm::Function::Create(
        moveNextTy, llvm::Function::InternalLinkage,
        prefix + "MoveNext", module_.get());

    // --- get$Current(ptr self) -> T ---
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);
    auto* currentFn = llvm::Function::Create(
        currentTy, llvm::Function::InternalLinkage,
        prefix + "Current", module_.get());

    // --- Dispose(ptr self) -> void ---
    // §29.7 (#623): the real Dispose is emitted AFTER MoveNext so we know
    // which yield states have active finally chains (smIterFinallyAtState_).
    // Declare the function now so the vtable can reference it; fill the body
    // below after MoveNext emission.
    auto* disposeFn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
        llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());

    // §29.11 (#943): a re-iterable IEnumerable<T> iterator must hand out a FRESH,
    // INDEPENDENT enumerator on every GetEnumerator() call so a second `for` loop
    // over the same source replays from the start instead of dereferencing the
    // already-exhausted-and-freed first enumerator (a use-after-free SIGSEGV).
    // The value the factory returns (`P.Seq()`) is the IEnumerABLE; for-in calls
    // GetEnumerator (vtable slot 4) to obtain an independent IEnumerATOR which it
    // alone advances and disposes, leaving the source untouched (the source is
    // disposed once at its own scope end). The enumerator is a CLONE of the source
    // struct (a fresh calloc copy with state reset to 0); the clone carries a
    // distinct vtable whose Dispose (`cloneDisposeFn`) frees the clone WITHOUT
    // running the §29.8 owned-parameter auto-dispose — the owned parameters belong
    // to the IEnumerable (source) and are disposed exactly once when IT is
    // disposed, not once per enumerator (which would multi-free).
    //
    // The clone slot is emitted uniformly for every sync yield iterator. Whether
    // it is exercised is decided by the return type: an IEnumerable<T> iterator is
    // re-iterable (the for-in clones a fresh enumerator per GetEnumerator), while
    // an IEnumerator<T> iterator is single-use (the call returns the enumerator
    // directly, §29.3) and for-in over a bare IEnumerator<T> source is not
    // supported — so the clone slot is only ever reached for the IEnumerable form.
    //
    // The clone's Dispose: same finally/spill cleanup as disposeFn but WITHOUT
    // owned-parameter auto-dispose (the clone borrows the captures; only the
    // source owns them). Declared now, filled alongside disposeFn below.
    auto* cloneDisposeFn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
        llvm::Function::InternalLinkage, prefix + "CloneDispose", module_.get());

    // GetEnumerator(ptr self) -> ptr: clone the state struct (fresh enumerator).
    auto* getEnumFn = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
        llvm::Function::InternalLinkage, prefix + "GetEnumerator", module_.get());

    // --- Vtable globals for this iterator's IEnumerator<T> impl ---
    // Layout: [0]=td(self-ptr), [1]=Dispose, [2]=MoveNext, [3]=Current,
    //         [4]=GetEnumerator (§29.11 #943).
    // Two vtables: the SOURCE (IEnumerable) uses disposeFn (full, owned-param-
    // disposing); a CLONE (the per-loop enumerator) uses cloneDisposeFn.
    const unsigned VT_SIZE = 5;
    auto* vtGVType = llvm::ArrayType::get(ptrTy(), VT_SIZE);
    std::vector<llvm::Constant*> vtElems(VT_SIZE,
        llvm::ConstantPointerNull::get(ptrTy()));
    auto* vtGV = new llvm::GlobalVariable(*module_, vtGVType, true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(vtGVType, vtElems),
        prefix + "vtable");
    auto* cloneVtGV = new llvm::GlobalVariable(*module_, vtGVType, true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(vtGVType, vtElems),
        prefix + "clonevtable");
    // Source vtable: full Dispose.
    vtElems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
    vtElems[1] = disposeFn;
    vtElems[2] = moveNextFn;
    vtElems[3] = currentFn;
    vtElems[4] = getEnumFn;
    vtGV->setInitializer(llvm::ConstantArray::get(vtGVType, vtElems));
    // Clone vtable: clone-Dispose (no owned-param auto-dispose).
    vtElems[0] = llvm::ConstantExpr::getPointerCast(cloneVtGV, ptrTy());
    vtElems[1] = cloneDisposeFn;
    cloneVtGV->setInitializer(llvm::ConstantArray::get(vtGVType, vtElems));

    // --- GetEnumerator (= the original method body): alloc + fill struct ---
    // fn is the already-declared function for this method. The value it returns
    // is the IEnumerABLE (re-iterable) — for-in clones a fresh enumerator from it
    // via getEnumFn (slot 4). For a single-use IEnumerator<T> iterator the same
    // struct IS the enumerator (the consumer drives it directly).
    {
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
        builder_.SetInsertPoint(entry);
        bindParameters(fn, isInstance, &ms->params);

        // Allocate state struct
        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* st = builder_.CreateCall(callocFn2,
            {builder_.getInt64(1), builder_.getInt64(stSz)}, "iter.state");
        // Store vtable at field 0
        llvm::Value* vtField = builder_.CreateStructGEP(stateStructTy, st, VTABLE_FIELD);
        builder_.CreateStore(vtGV, vtField);
        // Store state = 0
        llvm::Value* stateField = builder_.CreateStructGEP(stateStructTy, st, STATE_FIELD);
        builder_.CreateStore(builder_.getInt32(0), stateField);
        // Store each capture (this + params) into the frame.
        emitFrameCaptureStores(frame, st);
        // #1864 / §27.5 / §28.9: a closure (function-typed) parameter captured
        // into the iterator FRAME is a BORROW the frame must be able to invoke
        // across every MoveNext, i.e. it must outlive the producing call. Give
        // the frame its OWN +1 on the closure cell here (parallel to a closure
        // FIELD-STORE retaining a borrow, lowerClosureFieldStore); the SOURCE
        // Dispose (disposeOwned=true) drops it (releaseClosureCaptures below).
        // The caller then releases its transient producer reference at the call
        // site (the sync-iterator ARGUMENT-PASS release in lowerCall). Without
        // this retain the cell would be freed while the frame still holds it (a
        // UAF once the caller-side release lands); without both the cell simply
        // leaked (LSan: 40 bytes). bz_closure_retain is null-safe.
        for (std::size_t ci = 0; ci < frame.captures.size(); ++ci) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                frame.captures[ci].first);
            if (!ps) continue;                       // 'this' capture — skip
            sema::TypeRef* pt = paramTypeOf(ps);
            if (!pt || pt->kind != sema::TypeRef::Kind::Function) continue;
            unsigned fieldIdx = frame.captureStart + (unsigned)ci;
            llvm::Value* fptr =
                builder_.CreateStructGEP(frame.structTy, st, fieldIdx);
            llvm::Value* cl = builder_.CreateLoad(ptrTy(), fptr, "iter.clcap");
            emitClosureRetain(cl);
        }
        // Return the state struct (as ptr)
        builder_.CreateRet(st);
    }

    // --- GetEnumerator(ptr self) -> ptr: clone a fresh enumerator (§29.11 #943) ---
    // Independence: a new calloc'd struct is a byte copy of the source (captures
    // intact so the body sees `this`/params), with state/current_val/spill reset
    // to the start, and the CLONE vtable installed (so the clone's Dispose frees
    // the clone without re-disposing the source's owned parameters). The source
    // struct itself is never advanced by enumeration — each `for` loop clones,
    // drives, and disposes its own enumerator, so N loops replay N times.
    {
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", getEnumFn);
        llvm::IRBuilder<> gb(entry);
        llvm::Value* self = &*getEnumFn->arg_begin();
        llvm::Value* sourceState = gb.CreateStructGEP(
            stateStructTy, self, STATE_FIELD);
        auto* cloneBB = entry;
        if (cloneDisposesOwnedCaptures) {
            // The captures carry affine obligations, so only one clone may claim
            // them. A later GetEnumerator is a loud failure rather than replaying
            // pointers whose ownership the first clone already consumed.
            auto* claimedBB = llvm::BasicBlock::Create(
                llvm_, "claim.used", getEnumFn);
            cloneBB = llvm::BasicBlock::Create(llvm_, "claim.ok", getEnumFn);
            llvm::Value* prior = gb.CreateLoad(i32(), sourceState, "claim.state");
            gb.CreateCondBr(gb.CreateICmpEQ(prior, gb.getInt32(-3)),
                            claimedBB, cloneBB);

            gb.SetInsertPoint(claimedBB);
            auto* callocTy = llvm::FunctionType::get(
                ptrTy(), {i64(), i64()}, false);
            auto* callocFnV = module_->getOrInsertFunction(
                "calloc", callocTy).getCallee();
            llvm::Value* excObj = gb.CreateCall(
                callocTy, callocFnV,
                {llvm::ConstantInt::get(i64(), 1),
                 llvm::ConstantInt::get(i64(), kExcObjSize)},
                "claim.excobj");
            auto vit = builtinVtable_.find("InvalidOperationException");
            if (vit != builtinVtable_.end()) gb.CreateStore(vit->second, excObj);
            auto* excAddrTy = llvm::FunctionType::get(ptrTy(), {}, false);
            auto* excAddrFnV = module_->getOrInsertFunction(
                "bz_exc_addr", excAddrTy).getCallee();
            llvm::Value* excSlot = gb.CreateCall(
                excAddrTy, excAddrFnV, {}, "claim.exc");
            gb.CreateStore(excObj, excSlot);
            gb.CreateRet(llvm::ConstantPointerNull::get(ptrTy()));
            gb.SetInsertPoint(cloneBB);
        }

        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFn3 = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* clone = gb.CreateCall(callocFn3,
            {gb.getInt64(1), gb.getInt64(stSz)}, "iter.clone");
        // memcpy(clone, self, stSz): copy vtable + state + captures + locals.
        auto* memcpyFn = declareC("memcpy", ptrTy(),
            {ptrTy(), ptrTy(), i64()});
        gb.CreateCall(memcpyFn, {clone, self, gb.getInt64(stSz)});
        // Reset to the start state: state = 0, current_val = 0,
        // current_val_owned = 0, spill ptr = null.
        gb.CreateStore(gb.getInt32(0),
            gb.CreateStructGEP(stateStructTy, clone, STATE_FIELD));
        gb.CreateStore(llvm::Constant::getNullValue(elemTy),
            gb.CreateStructGEP(stateStructTy, clone, CURRENT_VAL_FIELD));
        gb.CreateStore(gb.getInt8(0),  // §29 (#1120): not-owned
            gb.CreateStructGEP(stateStructTy, clone, CURRENT_VAL_OWNED_FIELD));
        gb.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
            gb.CreateStructGEP(stateStructTy, clone, SPILL_PTR_FIELD));
        // In the linear §29.8 shape, ownership of each capture moves source →
        // clone. Nulling the source makes its later Dispose a no-op for them.
        if (cloneDisposesOwnedCaptures) {
            for (std::size_t i = 0; i < frame.captures.size(); ++i) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!cloneOwnsParam(ps)) continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                gb.CreateStore(llvm::Constant::getNullValue(frame.captures[i].second),
                    gb.CreateStructGEP(stateStructTy, self, fieldIdx));
            }
            gb.CreateStore(gb.getInt32(-3), sourceState);
        }
        // Install the clone vtable. In the narrow owned-capture shape its Dispose
        // also runs the state-keyed §29.8 parameter cleanup.
        gb.CreateStore(cloneVtGV,
            gb.CreateStructGEP(stateStructTy, clone, VTABLE_FIELD));
        gb.CreateRet(clone);
    }

    // --- Current: load current_val from struct ---
    // For IOwningEnumerable<T> iterators this implements TakeCurrent() (vtable slot 3):
    // check current_val for null (before-first-advance / after-exhaustion /
    // double-take) → raise InvalidOperationException; on success, null the slot
    // and return the value. Per §29.1 v0.161 / #1349.
    {
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", currentFn);
        if (isOwningIter && elemTy->isPointerTy()) {
            // #1349 / §29.1 v0.161: TakeCurrent with IOE guard.
            auto* ioeErrBB = llvm::BasicBlock::Create(llvm_, "tc.ioe",  currentFn);
            auto* tcOkBB   = llvm::BasicBlock::Create(llvm_, "tc.ok",   currentFn);
            llvm::IRBuilder<> cb(entry);
            llvm::Value* self     = &*currentFn->arg_begin();
            llvm::Value* cvField  = cb.CreateStructGEP(stateStructTy, self,
                                                        CURRENT_VAL_FIELD);
            llvm::Value* curVal   = cb.CreateLoad(elemTy, cvField, "tc.cur");
            llvm::Value* isNull   = cb.CreateICmpEQ(
                curVal, llvm::ConstantPointerNull::get(ptrTy()), "tc.isnull");
            cb.CreateCondBr(isNull, ioeErrBB, tcOkBB);

            // IOE path: allocate exception object, store vtable, signal via
            // bz_exc_addr(), return null so the caller can emitPendingCheck().
            cb.SetInsertPoint(ioeErrBB);
            auto* callocTy  = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
            auto* callocFnV = module_->getOrInsertFunction("calloc", callocTy)
                                  .getCallee();
            llvm::Value* excObj = cb.CreateCall(
                callocTy, callocFnV,
                {llvm::ConstantInt::get(i64(), 1),
                 // §31.4: the uniform exception block (kExcObjSize) — a
                 // smaller alloc would make the CauseChain@24 / ParamName@32
                 // reads (catch arms, uncaught report) read out of bounds.
                 llvm::ConstantInt::get(i64(), kExcObjSize)},
                "tc.excobj");
            auto vit = builtinVtable_.find("InvalidOperationException");
            if (vit != builtinVtable_.end())
                cb.CreateStore(vit->second, excObj);
            auto* excAddrTy  = llvm::FunctionType::get(ptrTy(), {}, false);
            auto* excAddrFnV = module_->getOrInsertFunction(
                                   "bz_exc_addr", excAddrTy).getCallee();
            llvm::Value* excSlot = cb.CreateCall(excAddrTy, excAddrFnV, {}, "tc.exc");
            cb.CreateStore(excObj, excSlot);
            cb.CreateRet(llvm::ConstantPointerNull::get(ptrTy()));

            // OK path: null the slot (transfer ownership), return value.
            cb.SetInsertPoint(tcOkBB);
            cb.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), cvField);
            cb.CreateRet(curVal);
        } else {
            // Borrowing iterator (IEnumerable<T>): plain load.
            llvm::IRBuilder<> cb(entry);
            llvm::Value* self    = &*currentFn->arg_begin();
            llvm::Value* cvField = cb.CreateStructGEP(stateStructTy, self,
                                                       CURRENT_VAL_FIELD);
            cb.CreateRet(cb.CreateLoad(elemTy, cvField));
        }
    }

    // --- MoveNext: the state machine body ---
    // We emit MoveNext by re-running the method body in a special "iterator" mode.
    // This re-uses lowerScopedBlock but with yield points turned into
    // state transitions.
    {
        currentFn_ = moveNextFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        // §29.7 (#623): clear the finally-chain tracking for this iterator.
        finallyBodyStack_.clear();
        smIterFinallyAtState_.clear();
        smIterDisposalsAtState_.clear();  // #821
        smIterEnumFramesAtState_.clear();  // §29.7 (#944)
        smIterCleanupAtState_.clear();     // fix #1567 §29.7/§28.10 (v0.182)
        smIterEnumFrameSlot_.clear();      // §29.7 (#944)
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        // fix #1897 / §28.3 "Exception-path discharge" / §29.2: compute the
        // unwind-disposal eligibility of the iterator body's plain owned locals
        // and key it to the MOVENEXT fn (where the body lowers). registerUnwind-
        // OwnedLocal gates on `currentFn_ == unwindIneligibleFn_` (so a body whose
        // moves/disposes were not analyzed cannot register a double-dispose);
        // without this the MoveNext body's currentFn_ (moveNextFn) never matched
        // the stale unwindIneligibleFn_ left by the last synchronous method, so
        // EVERY plain `owned` local hoisted into the iterator state-machine frame
        // was silently skipped — never registered for the exception-path
        // (`unwindOnly`) disposal — and leaked when the iterator body threw after a
        // yield (the #1897 leak). emitThrowValue / emitPendingCheck already emit
        // emitUnwindDisposals on the throw edge; they simply had nothing registered
        // to dispose. Mirror the synchronous emitFunctionBody setup (Setup.cpp
        // ~3318). The unwindOnly disposal is skipped on the normal edge (§28.3:
        // normal-exit discharge is sema-enforced), so no double-free on the
        // non-throwing path. // per §28.3
        unwindIneligibleFn_ = moveNextFn;
        collectUnwindIneligible(*ci.stmts);
        // fix #2148 / #2142 / #2143 / #1871 / #1003 / #1028 / #1930 / §28.9 /
        // §28.11: the scope-end reclaim of a plain `var` borrowing-`Array<T>` /
        // `Set<T>` / `Dictionary<K,V>` LITERAL (its backing is "reclaimed with
        // the instance"), and of a `var` fresh borrowing-array / dict /
        // grouping MATERIALIZER, must be re-established for the MOVENEXT fn too
        // — the registration sites in lowerLocalDecl gate on `currentFn_ ==
        // borrowingArrayFreeEligibleFn_` (and the dict/grouping/set-literal/
        // dict-literal analogues), but the iterator body lowers with
        // currentFn_ == moveNextFn while emitFunctionBody left these keyed to
        // the ENTRY (factory) fn (Setup.cpp), so every such reclaim was
        // silently skipped and the backing leaked when the iterator ran to
        // normal completion (the #2148 leak: an `Array<T>` literal / Split
        // materializer local in a `yield` body — the MoveNext scope-end drop
        // never freed it; the same gap applies to a Set<T>/Dictionary<K,V>
        // literal in a yield body, fixed here alongside #2142/#2143's entry-
        // and resume-fn fixes for parity). One call re-populates all five
        // sets; re-pin all five fn keys to moveNextFn. Mirrors emitAsyncMethod's
        // resume-fn re-establishment (LlvmBackend_Async.cpp) and the unwind
        // re-establishment just above. // per §28.9, §28.11
        collectBorrowingArrayFreeEligible(*ci.stmts);
        borrowingArrayFreeEligibleFn_ = moveNextFn;
        dictMaterializerFreeEligibleFn_ = moveNextFn;
        groupingFreeEligibleFn_ = moveNextFn;
        setLiteralFreeEligibleFn_ = moveNextFn;   // #2142
        dictLiteralFreeEligibleFn_ = moveNextFn;  // #2143
        // fix #2817 / #2807 / #429 / §4.6 / §28.9 / §28.3 / §29.2 (v0.240): the
        // `var` STRING-local refcount analysis must describe the MOVENEXT fn too
        // — the same re-establishment #1897 made just above for the unwind sets
        // and #2148/#2142/#2143 made for the collection-reclaim sets. EVERY
        // consumer of the three string sets gates on `currentFn_ ==
        // stringReleaseEligibleFn_`, and the iterator body lowers with currentFn_
        // == moveNextFn while emitFunctionBody left the token naming the last
        // SYNCHRONOUS method emitted, so no string local in any `yield return`
        // body was ever retained OR released: both allocations (bz_string header
        // + payload) leaked, per local, per element (#2817 — measured 1845 bytes
        // / 78 allocations over 20 advances).
        //
        // THE ACTIVATION MODEL. A MoveNext resume runs ONCE PER ADVANCE, so the
        // bind and the scope-end release of a local declared inside the yielding
        // loop land in DIFFERENT advances: advance k binds it and suspends at the
        // `yield`; advance k+1 resumes just past the yield, runs the loop body's
        // scope-end disposals (releasing exactly the value the frame field
        // restored — collectSuspendLocals gives every LocalDecl a frame field and
        // the yield spills every mapped alloca), and only then re-binds for
        // iteration k+1. One bind, one release, per element.
        //
        // ABANDONMENT is covered rather than leaked: the yield handler snapshots
        // the disposals live at that suspend into smIterCleanupAtState_
        // (LlvmBackend_Stmts.cpp) — a Disposal{stringRelease} is neither
        // `caughtException` nor `unwindOnly`, and its slot IS in locals_, so it is
        // recorded — and Dispose restores every frame local and runs that stack in
        // reverse. The two are mutually exclusive: Dispose stamps the DISPOSED
        // sentinel (-2) so a later MoveNext cannot resume past the yield, and a
        // run-to-exhaustion iterator parks at -1, whose switch default runs no
        // cleanup. So still exactly once on either exit. // per §29.7, §28.10
        collectStringReleaseEligible(*ci.stmts);
        stringReleaseEligibleFn_ = moveNextFn;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", moveNextFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = &*moveNextFn->arg_begin();

        // Restore captured params (this + params) from the frame.
        emitFrameCaptureRestores(frame, self);

        // fix #3466 (§27.5 / §29.3 / §30.1, v0.283): publish the per-parameter
        // capture-CELL fields for MoveNext — the fn the iterator body lowers
        // into. A `yield` suspends exactly as an `await` does, and the same
        // entry-block re-null applies (MoveNext opens with the state-dispatch
        // switch, so the cell holder's null-init runs on every advance), so a
        // parameter a lambda captures needs its holder carried by the frame here
        // for the same reason. Measured on the pre-fix compiler: a `v := v + 5`
        // between two `yield return`s did not reach the closure — 9 where 14 is
        // §27.5's answer.
        smParamCellField_ = frame.paramCellFields;
        smLocalPreBoxField_ = frame.localPreBoxFields;
        smCellFrameFn_ = moveNextFn;

        // fix #2817 / fix #2696 / §28.1 / §28.9 / §4.6 (v0.240): CO-GATE the
        // parameter retain with the parameter release the pin above just turned
        // on. The `:=` / §4.6 `+=` drop-on-overwrite release admits ANY slot
        // localSlot() resolves — including a `string` PARAMETER, whose single
        // reference belongs to the CALLER (§28.1: on a `string`, "`owned` is a
        // compile error: such values carry no disposal obligation to transfer
        // (§28.9)"). Enabling that release without the paired bind-time retain
        // frees the caller's borrow under it — the #2696 use-after-free — so the
        // two predicates must move together.
        //
        // Emitted in the MoveNext ENTRY block, which dominates every state block
        // and re-runs on EVERY advance, right after emitFrameCaptureRestores has
        // re-restored the parameter slot from its capture field. A given advance
        // can release the incoming value at most once (after the first assignment
        // the slot holds a value this frame owns), and the assignment does not
        // survive the suspend (capture fields are spilled once at construction
        // and never re-spilled), so retains >= releases on every path: a surplus
        // retain leaks one reference per advance, it can never over-release.
        retainAssignedStringParams(*ci.stmts, ms->params);

        // State field GEP — read by the resume dispatch, written at each yield.
        llvm::Value* stateGEP =
            builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);

        // Collect all yield stmts and assign state numbers.
        std::vector<const ast::YieldStmt*> yields;
        collectYields(*ci.stmts, yields);
        unsigned numYields = (unsigned)yields.size();

        // Emit the resume switch + state blocks. `Yield`'s exhausted-return is
        // `ret false` (MoveNext: "no more elements").
        std::vector<llvm::BasicBlock*> stateBBs = emitResumeDispatch(
            frame, self, stateGEP, numYields,
            [&]() { builder_.CreateRet(builder_.getFalse()); });

        // Yield-specific live state for the YieldStmt handler in lowerStmt.
        iterCurrentValField_ = CURRENT_VAL_FIELD;
        iterCurrentValOwnedField_ = CURRENT_VAL_OWNED_FIELD;  // §29 (#1120)
        iterElemTy_ = elemTy;
        iterElemSemaTy_ = elemSemaTy;  // #1102B (nullable-value element coercion)
        iterElemIsOwnedClass_ = elemIsOwnedClass;  // §29 (#1120)
        // fix #2621 / §29.1 (v0.236): mirror iterElemIsOwnedClass_ with the
        // OWNING-head flag. Mutually exclusive by construction (elemIsOwnedClass
        // is set only when retName is neither owning head, lines 879-882). The
        // YieldStmt handler in lowerStmt gates the new yield-store dispose on
        // this flag — for the owning head the enumerator always owns a
        // non-null current_val (the §29.1 contract), so the displaced value
        // must be disposed BEFORE the next yield store overwrites the slot.
        iterIsOwningIter_ = isOwningIter;
        // fix #2738 / §28.3 / §29.2 (v0.282): thread ownership of the +1 reference
        // minted by `yield return <+1 string producer>` through the iterator frame.
        // YieldStmt gates its new prev-dispose on this flag; the iterator's Dispose
        // extends disposeYieldedVal below to also release the outstanding +1 when
        // this flag is set. Mutually exclusive with iterElemIsOwnedClass_ (class
        // kind) and iterIsOwningIter_ (owning head) by construction (see
        // elemIsRuntimeManagedPrimitive computation above).
        iterElemIsRuntimeManagedPrimitive_ = elemIsRuntimeManagedPrimitive;
        // #747: enable for-loop frame-spill for THIS MoveNext, reusing the async
        // spill-area mechanism. registerForLoopSpillSlots records a suspending
        // for-loop's implicit allocas (index / array-ptr / enumerator) into
        // asyncHandlerSpillSlots_ during the body lowering below; fixupSuspend-
        // Dominance then heap-spills them (and any SSA temporary whose def no
        // longer dominates a use across a yield) into the frame's spill field.
        asyncSpillPtrField_ = SPILL_PTR_FIELD;
        asyncHandlerSpillSlots_.clear();

        // Emit MoveNext body starting from state 0.
        // The body is emitted linearly; YieldStmt handlers interrupt it to
        // emit the yield transition and jump to the next state block.
        builder_.SetInsertPoint(stateBBs[0]);
        disposeStack_.emplace_back();
        bool term = lowerBody(*ci.stmts);
        // If body didn't terminate, fall through to done.
        if (!term && !builder_.GetInsertBlock()->getTerminator()) {
            // #821 / §29.7: on NORMAL completion (the body runs off its end), the
            // `using var` resources active at the end must be disposed before the
            // iterator reports done. (The abandonment path — Dispose while still
            // suspended at a yield — is handled by smIterDisposalsAtState_ in the
            // Dispose function below.) State is set to -1 (iteration logically
            // complete; the frame is STILL ALLOCATED and still owes disposal).
            // The consumer's subsequent Dispose() proceeds (the Dispose-entry guard
            // tests the disposed sentinel -2, NOT this -1), frees the frame, and
            // disposes owned params — but its state switch hits the default (-1)
            // and skips re-running these using-var disposals, so each is disposed
            // exactly once.
            emitDisposals(disposeStack_.back());
            // §29 (#1120): on exhaustion, dispose the last outstanding yielded
            // owned value if (and only if) the enumerator owns it (it was produced
            // by a `new` expression, not borrowed from a using-var local or param).
            // Null current_val and current_val_owned either way so the subsequent
            // for-loop Dispose's null-guard makes it a no-op.
            if (elemIsOwnedClass) {
                llvm::Value* cvField = builder_.CreateStructGEP(
                    stateStructTy, self, CURRENT_VAL_FIELD);
                llvm::Value* lastVal =
                    builder_.CreateLoad(elemTy, cvField, "ity.last");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    lastVal, llvm::ConstantPointerNull::get(ptrTy()),
                    "ity.lastnull");
                auto* nonNullBB = llvm::BasicBlock::Create(
                    llvm_, "ity.last.nonnull", moveNextFn);
                auto* contBB = llvm::BasicBlock::Create(
                    llvm_, "ity.last.cont", moveNextFn);
                builder_.CreateCondBr(isNull, contBB, nonNullBB);
                builder_.SetInsertPoint(nonNullBB);
                llvm::Value* ownedFld = builder_.CreateStructGEP(
                    stateStructTy, self, CURRENT_VAL_OWNED_FIELD);
                llvm::Value* ownedByte =
                    builder_.CreateLoad(i8(), ownedFld, "ity.lastowned");
                llvm::Value* isOwned = builder_.CreateICmpNE(
                    ownedByte, builder_.getInt8(0), "ity.lastisowned");
                auto* dispBB = llvm::BasicBlock::Create(
                    llvm_, "ity.last.disp", moveNextFn);
                auto* skipBB = llvm::BasicBlock::Create(
                    llvm_, "ity.last.skip", moveNextFn);
                auto* afterBB = llvm::BasicBlock::Create(
                    llvm_, "ity.last.after", moveNextFn);
                builder_.CreateCondBr(isOwned, dispBB, skipBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(lastVal);
                builder_.CreateBr(afterBB);
                builder_.SetInsertPoint(skipBB);
                builder_.CreateBr(afterBB);
                builder_.SetInsertPoint(afterBB);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), cvField);
                builder_.CreateStore(builder_.getInt8(0), ownedFld);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            // #1349 / §29.1 v0.161: for IOwningEnumerable<T> iterators, null
            // current_val on exhaustion so TakeCurrent raises IOE afterwards.
            // In the normal for-loop protocol TakeCurrent already nulled it, but
            // defensive null on the exhaustion path covers manual-protocol usage
            // where MoveNext returning false without a prior TakeCurrent is
            // possible (e.g. zero-element owning iterator, or skip pattern).
            // fix #2616 / §29.1 (v0.236): the null must be preceded by a
            // DISPOSE. Under the §29.1 owning contract ownership transfers to
            // the consumer only at TakeCurrent, so a NON-NULL current_val here
            // is a value the consumer never took — the enumerator still owns
            // it, and nulling the only reference leaked it. This is reachable
            // exactly on manual-protocol abandonment (`en.MoveNext()` without a
            // matching `en.TakeCurrent()`, then driving on to exhaustion); the
            // dominant `for var x in src` desugaring takes every value, so
            // current_val is null here and the branch below is a no-op.
            //
            // Exactly-once, not a double-free: the abandon-then-Dispose path
            // (disposeYieldedVal, below) is the same dispose-then-null under
            // the same null guard, so after this runs its load sees null and
            // no-ops. TakeCurrent is the only other reader and raises §29.1's
            // InvalidOperationException on null (unchanged: after exhaustion it
            // already saw null). The BORROWING head is untouched — this branch
            // is gated on isOwningIter, and the `elemIsOwnedClass` block just
            // above (mutually exclusive with isOwningIter by construction,
            // since elemIsOwnedClass is only set when retName is neither owning
            // head) keeps its owned-flag-gated behaviour.
            if (isOwningIter && elemTy->isPointerTy()) {
                llvm::Value* cvField = builder_.CreateStructGEP(
                    stateStructTy, self, CURRENT_VAL_FIELD);
                llvm::Value* pend =
                    builder_.CreateLoad(elemTy, cvField, "ity.pend");
                llvm::Value* pendNull = builder_.CreateICmpEQ(
                    pend, llvm::ConstantPointerNull::get(ptrTy()),
                    "ity.pendnull");
                auto* dispBB = llvm::BasicBlock::Create(
                    llvm_, "ity.pend.disp", moveNextFn);
                auto* contBB2 = llvm::BasicBlock::Create(
                    llvm_, "ity.pend.cont", moveNextFn);
                builder_.CreateCondBr(pendNull, contBB2, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(pend);
                builder_.CreateBr(contBB2);
                builder_.SetInsertPoint(contBB2);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), cvField);
            }
            builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
            builder_.CreateRet(builder_.getFalse());
        }
        disposeStack_.pop_back();

        // #1102 (sweep #8): a `yield return` that is UNREACHABLE — e.g. one that
        // follows a `yield break` — still consumes a resume-state block (mn.sN),
        // but the body flow stops at the break, so that block's continuation is
        // never emitted and it is left with no terminator (an IR-verification
        // crash). A resume into such a dead state cannot occur at runtime (the
        // state counter never advances past the break / off the end), so
        // terminate any orphan state block with `ret false` (exhausted). Only
        // blocks the body never reached are affected — reachable yield states
        // already carry their terminators.
        for (llvm::BasicBlock* bb : stateBBs) {
            if (bb && !bb->getTerminator()) {
                builder_.SetInsertPoint(bb);
                builder_.CreateRet(builder_.getFalse());
            }
        }

        // #747: if a suspending for-loop registered implicit allocas (or an SSA
        // temporary fails to dominate a use across a yield), promote them into the
        // frame's spill area so they survive the resume — fixing the sync-iterator
        // "Instruction does not dominate all uses" IR_FAIL. Scoped to MoveNexts
        // that actually registered slots, so a sync iterator with no problematic
        // for-loop is byte-for-byte unchanged.
        if (!asyncHandlerSpillSlots_.empty())
            fixupSuspendDominance(moveNextFn);
        asyncHandlerSpillSlots_.clear();
        asyncSpillPtrField_ = 0;

        // Clear state-machine state.
        smStateStruct_ = nullptr;
        smSelf_ = nullptr;
        smLocalFields_.clear();
        smForInSrcFields_.clear();  // §29.7 (#944)
        smAllocaToField_.clear();
        iterElemIsOwnedClass_ = false;      // §29 (#1120)
        iterCurrentValOwnedField_ = 0;      // §29 (#1120)
        iterIsOwningIter_ = false;          // fix #2621 / §29.1 (v0.236)
    }

    // --- Dispose(ptr self) -> void (real body, §29.7 #623) ---
    // Emitted after MoveNext so smIterFinallyAtState_ is populated.
    // If the iterator was suspended inside a try/finally, run the finally
    // chains (innermost first) before freeing the frame.  A second Dispose
    // is a no-op per §28.2/§29.7: the entry guard tests the DISPOSED sentinel
    // (state == -2, set only by Dispose itself) — distinct from MoveNext's
    // exhaustion value (state == -1), which must still let the first Dispose
    // proceed to free the frame and dispose owned params (§29.8).
    //
    // §29.11 (#943): emitted TWICE — once as `disposeFn` (the SOURCE/IEnumerable
    // Dispose, which runs §29.8 owned-parameter auto-dispose) and once as
    // `cloneDisposeFn` (a per-loop enumerator Dispose, which skips owned-param
    // auto-dispose because the clone borrows the captures from the source). The
    // bodies are otherwise identical (same finally chains, same spill free, same
    // state guard). Parameterised by `targetFn` and `disposeOwned`.
    // `releaseClosures` is deliberately SEPARATE from `disposeOwned`: the frame's
    // +1 on a closure parameter belongs to the SOURCE frame alone and must be
    // dropped exactly once, whereas `disposeOwned` also becomes true for the
    // clone once the owned-capture claim is armed (#2154, and #973a below). They
    // were the same flag before, so an armed claim had both the source and the
    // clone release the same closure cell. // per §27.5 / §28.9 (#1864)
    auto emitDisposeBody = [&](llvm::Function* targetFn, bool disposeOwned,
                               bool stateKeyedOwned, bool releaseClosures) {
        currentFn_ = targetFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        finallyBodyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;

        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", targetFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = &*targetFn->arg_begin();
        // #747: emitFreeSpillArea reads the frame's spill-ptr field; set the
        // frame context it needs. free(null) is a no-op for an iterator that
        // never spilled, so this is safe for every sync iterator.
        smStateStruct_ = stateStructTy;
        asyncSpillPtrField_ = SPILL_PTR_FIELD;

        // §28.2 / §29.7 (#1107, rework): idempotent disposal. §28.2 models
        // idempotency as a "per-instance disposed flag"; we encode that flag as
        // a distinct STATE SENTINEL — `-2` — that ONLY Dispose ever sets (at the
        // mark-done stores below, before running cleanup + free). MoveNext never
        // produces `-2`: it writes the positive yield states 1..N and, on natural
        // exhaustion, writes `-1` (meaning "iteration logically complete; the
        // frame is still allocated and STILL OWES DISPOSAL" — §29.7).
        //
        // The guard must therefore test the DISPOSED sentinel (`-2`), NOT the
        // MoveNext exhaustion value (`-1`). Testing `-1` (the original #1107
        // guard) was wrongly predicated: on the dominant exhaust-then-Dispose
        // pattern MoveNext has already set `state = -1`, so the FIRST (legitimate)
        // Dispose would no-op BEFORE free(self) / disposeOwnedParams() /
        // emitFreeSpillArea — regressing the pre-fix behavior into a frame + owned-
        // param LEAK (§29.7/§29.8). Re-predicating on `-2` lets the first Dispose
        // (whether after exhaustion `-1`, a suspended yield `N`, or never-iterated
        // `0`) proceed and free the frame, while a SECOND Dispose (after the first
        // set `-2`) no-ops. This covers checked mode (frame quarantined, slot not
        // nulled, so the call-site null guard does not fire) and any alias path.
        llvm::Value* disposeState = nullptr;
        {
            llvm::Value* stateGEP = builder_.CreateStructGEP(
                stateStructTy, self, STATE_FIELD);
            llvm::Value* curState =
                builder_.CreateLoad(i32(), stateGEP, "dsp.curstate");
            disposeState = curState;
            llvm::Value* isDisposed = builder_.CreateICmpEQ(
                curState, builder_.getInt32(-2), "dsp.isdisposed");
            auto* earlyRet =
                llvm::BasicBlock::Create(llvm_, "dsp.idem", targetFn);
            auto* proceed =
                llvm::BasicBlock::Create(llvm_, "dsp.proceed", targetFn);
            builder_.CreateCondBr(isDisposed, earlyRet, proceed);
            builder_.SetInsertPoint(earlyRet);
            builder_.CreateRetVoid();
            builder_.SetInsertPoint(proceed);
        }

        // Restore captures so finally bodies can access `this` / params.
        emitFrameCaptureRestores(frame, self);

        // Restore suspend-locals from the frame so finally bodies that
        // reference iterator-body locals round-trip correctly.
        for (auto& [node, fieldIdx] : frame.localFields) {
            llvm::Type* lty =
                frame.bodyLocals[fieldIdx - frame.localStart].second;
            llvm::Value* fptr = builder_.CreateStructGEP(
                stateStructTy, self, fieldIdx);
            llvm::Value* val = builder_.CreateLoad(lty, fptr);
            auto* slot = entryAlloca(lty);
            builder_.CreateStore(val, slot);
            locals_[node] = slot;
        }

        auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

        // §29.8 (#945): dispose each `owned` parameter the body never moved out,
        // in REVERSE declaration order (mirroring §28.4's destructor rule), AFTER
        // the body's `using var` / `finally` cleanups and before the frame is
        // freed. The capture field still holds the original object (captures are
        // spilled once at construction and never re-spilled on suspend), and sema
        // (Ownership.cpp) flagged ONLY params it proved are never discharged by
        // the body — so each is disposed exactly once (no double-free, no leak).
        // Runs on every iterator-Dispose: normal exhaustion (state -1), early
        // break / abandonment (a suspend state), and never-iterated.
        // §29.11 (#943): SKIPPED for `cloneDisposeFn` (disposeOwned=false) — a
        // per-loop enumerator does not own the captures; the source disposes them
        // once at its own scope end. Disposing them here too would multi-free.
        auto disposeOwnedParams = [&]() {
            if (!disposeOwned) return;
            for (std::size_t i = frame.captures.size(); i-- > 0;) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!ps || ps->modifier != "owned") continue;
                // (#2154 part 2) Visit the slot when EITHER sema flagged
                // it as auto-disposable (the legacy monotone flag, true
                // for params sema proved undischarged on every path) OR
                // the codegen-side scan placed a yield-state on it
                // (extended linear shape including try-bodies). A param
                // outside either set has already been consumed in the
                // source frame and the clone's memcpy'd slot matches a
                // null/empty value — nothing to dispose.
                unsigned yieldState = ps->iteratorMoveYieldState;
                auto cit = ownedParamYieldStates.find(ps);
                if (cit != ownedParamYieldStates.end()) yieldState = cit->second;
                bool semaAutoDispose = ps->iteratorAutoDispose;
                // #973a: a CONDITIONALLY discharged parameter enters the loop
                // too. It carries no yield state (the two sema shapes are
                // mutually exclusive: the state-keyed prototype requires every
                // yield operand to be a bare owned parameter, which the
                // marked-adoption shape is not), so it takes the pure
                // null-guard below — and that guard IS its moved-from test,
                // because the adoption lowering nulled this very slot in the
                // frame MoveNext was driving.
                if (!semaAutoDispose && !ps->iteratorFrameConditionalAutoDispose &&
                    yieldState == 0)
                    continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                llvm::Value* fptr =
                    builder_.CreateStructGEP(stateStructTy, self, fieldIdx);
                llvm::Value* obj = builder_.CreateLoad(ptrTy(), fptr, "itp.obj");
                auto* dispBB =
                    llvm::BasicBlock::Create(llvm_, "itp.dispose", targetFn);
                auto* contBB =
                    llvm::BasicBlock::Create(llvm_, "itp.cont", targetFn);
                llvm::Value* shouldDispose = builder_.CreateICmpNE(
                    obj, llvm::ConstantPointerNull::get(ptrTy()), "itp.nonnull");
                if (stateKeyedOwned && yieldState != 0) {
                    // State N means yield N has already transferred its parameter.
                    // State 0 still owns all captures; -1 means fully exhausted.
                    llvm::Value* notComplete = builder_.CreateICmpSGE(
                        disposeState, builder_.getInt32(0), "itp.active");
                    llvm::Value* beforeMove = builder_.CreateICmpSLT(
                        disposeState,
                        builder_.getInt32((int)yieldState),
                        "itp.beforemove");
                    shouldDispose = builder_.CreateAnd(
                        shouldDispose,
                        builder_.CreateAnd(notComplete, beforeMove),
                        "itp.shoulddispose");
                }
                builder_.CreateCondBr(shouldDispose, dispBB, contBB);
                builder_.SetInsertPoint(dispBB);
                // fix #3125 (§29.8 / §30.5, v0.254): as in the two async frames'
                // identical loops — a Task-family capture is released by the
                // §30.5 abandonment protocol, not by class-vtable dispatch
                // (measured: SEGV in bz$iter$Gen$Dispose). One shared routing
                // decision, so the three frame emitters cannot drift.
                emitOwnedParamCaptureDispose(frame.captures[i].first, obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), fptr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
        };

        // §29.7 (#2306): the LOCAL counterpart of disposeOwnedParams. For each
        // plain `var L` local of an owned-class type whose pre-pass produced a
        // (establishedAtState, transferredAtState, frameField) tuple, dispose
        // L's frame slot iff the current state is in L's live window:
        //   establishedAtState <= state < transferredAtState
        // (a non-transferred local uses unbounded upper bound). The null guard
        // handles "never reached (state 0)" — the calloc-zeroed slot is null
        // because MoveNext never ran past the declaration. Mirrors the
        // parameter version's state-keyed test (§29.8 #2154/#2217) plus the
        // extra establishment gate for the local's declaration. Reverse
        // establishment order matches §28.4's reverse-declaration destructor
        // rule. Only registered locals are visited, so the lambda is a no-op
        // (zero IR) for body shapes the pre-pass could not statically pin —
        // preserving the existing #1897 bug for those shapes rather than
        // risking a double-free against a non-yield discharge.
        auto disposeOwnedLocals = [&]() {
            if (ownedLocalYieldStates.empty()) return;
            for (auto bit = frame.bodyLocals.rbegin();
                 bit != frame.bodyLocals.rend(); ++bit) {
                const ast::Node* node = bit->first;
                auto* ld = dynamic_cast<const ast::LocalDecl*>(node);
                if (!ld) continue;  // for-loop var / for-in iterable node.
                auto oit = ownedLocalYieldStates.find(ld);
                if (oit == ownedLocalYieldStates.end()) continue;
                const LocalKeyedDisposal& info = oit->second;
                if (info.establishedAtState == 0 ||
                    info.transferredAtState == 0 ||
                    info.frameField == 0)
                    continue;
                unsigned fieldIdx = info.frameField;
                llvm::Value* fptr = builder_.CreateStructGEP(
                    stateStructTy, self, fieldIdx);
                llvm::Value* obj = builder_.CreateLoad(
                    ptrTy(), fptr, "itl.obj");
                llvm::Value* nonNull = builder_.CreateICmpNE(
                    obj, llvm::ConstantPointerNull::get(ptrTy()),
                    "itl.nonnull");
                // Established: suspension state >= establishedAtState.
                llvm::Value* established = builder_.CreateICmpSGE(
                    disposeState,
                    builder_.getInt32((int)info.establishedAtState),
                    "itl.estab");
                // Not transferred: state < transferredAtState. (If
                // transferredAtState == 0, fall back to always-live, but the
                // pre-pass already filtered such cases out — every registered
                // local has transferredAtState > 0.)
                llvm::Value* beforeMove = builder_.CreateICmpSLT(
                    disposeState,
                    builder_.getInt32((int)info.transferredAtState),
                    "itl.beforemove");
                llvm::Value* shouldDispose = builder_.CreateAnd(
                    nonNull,
                    builder_.CreateAnd(established, beforeMove),
                    "itl.shoulddispose");
                auto* dispBB = llvm::BasicBlock::Create(
                    llvm_, "itl.dispose", targetFn);
                auto* contBB = llvm::BasicBlock::Create(
                    llvm_, "itl.cont", targetFn);
                builder_.CreateCondBr(shouldDispose, dispBB, contBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), fptr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
        };

        // #1864 / §27.5 / §28.9: release each closure (function-typed) parameter
        // the frame RETAINED at construction (emitIteratorMethod's capture-store
        // retain). The frame held its own +1 so it could invoke the closure
        // across every MoveNext; that reference is dropped here. Like
        // disposeOwnedParams, this runs on the SOURCE Dispose ONLY
        // (disposeOwned=true): the per-loop clone (cloneDisposeFn,
        // disposeOwned=false) BORROWS the captures from the source — the source
        // is disposed last (reverse order) and drops the frame's reference
        // exactly once. bz_closure_release is null-safe and reclaims the cell +
        // env block at rc 0. Owned-class params (disposeOwnedParams) and closure
        // params are disjoint sets, so the two loops never touch the same field.
        auto releaseClosureCaptures = [&]() {
            if (!releaseClosures) return;
            for (std::size_t i = frame.captures.size(); i-- > 0;) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!ps) continue;
                sema::TypeRef* pt = paramTypeOf(ps);
                if (!pt || pt->kind != sema::TypeRef::Kind::Function) continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                llvm::Value* fptr =
                    builder_.CreateStructGEP(stateStructTy, self, fieldIdx);
                llvm::Value* cl = builder_.CreateLoad(ptrTy(), fptr, "itc.cl");
                emitClosureRelease(cl);   // null-safe
            }
        };

        // §29 (#1120): dispose the outstanding yielded owned value stored in the
        // current_val frame field — BUT ONLY if the enumerator actually owns it
        // (current_val_owned == 1, set when the yielded expression was a `new`).
        // Using-var locals and owned params carry owned==0 and are disposed by
        // their own mechanisms (smIterDisposalsAtState_ / disposeOwnedParams).
        // Null-guarded: current_val==null ⟹ never yielded / already cleaned up.
        // Runs for BOTH source (disposeOwned=true) and clone (disposeOwned=false)
        // Dispose: the source's current_val is always null (GetEnumerator resets
        // it) so it's a guaranteed no-op for the source.
        //
        // #1349 / §29.1 v0.161: additionally, for IOwningEnumerable<T> iterators
        // (isOwningIter), the enumerator always owns current_val (no owned-flag
        // needed); if it is non-null when Dispose is called the consumer never
        // called TakeCurrent (manual-protocol abandonment) and we must dispose.
        auto disposeYieldedVal = [&]() {
            if (!elemIsOwnedClass && !isOwningIter &&
                !elemIsRuntimeManagedPrimitive) return;
            // #1837 / §4.13 / §29.1 (v0.196): the cleanup below loads current_val
            // and compares it against a null POINTER, so it requires elemTy to be
            // a pointer. An IOwningEnumerable<T> iterator emitted as its
            // UNSPECIALIZED generic template (T is an unresolved type parameter, so
            // lowerTypeRef falls to the i32 value-kind placeholder) has isOwningIter
            // true but a non-pointer elemTy — the raw i32-vs-ptr ICmp then trips
            // llvm::ICmpInst::AssertOK. The template is never called (each call site
            // emits a monomorphized specialization where the class-constrained T is
            // a concrete reference type ⇒ elemTy is ptr and this dispose runs in
            // full), so skip it here — mirroring the `isOwningIter &&
            // elemTy->isPointerTy()` guards already at the TakeCurrent (§29.1) and
            // MoveNext-exhaustion sites. (elemIsOwnedClass always implies a
            // class/interface element ⇒ a pointer elemTy, so this never elides a
            // real IEnumerable<owned T> disposal.)
            if (!elemTy->isPointerTy()) return;
            llvm::Value* cvField = builder_.CreateStructGEP(
                stateStructTy, self, CURRENT_VAL_FIELD);
            llvm::Value* yv = builder_.CreateLoad(elemTy, cvField, "ity.yv");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                yv, llvm::ConstantPointerNull::get(ptrTy()), "ity.yvnull");
            auto* nonNullBB = llvm::BasicBlock::Create(
                llvm_, "ity.yv.nonnull", targetFn);
            auto* contBB = llvm::BasicBlock::Create(
                llvm_, "ity.yv.cont", targetFn);
            builder_.CreateCondBr(isNull, contBB, nonNullBB);
            builder_.SetInsertPoint(nonNullBB);
            if (isOwningIter) {
                // #1349: IOwningEnumerable<T> — enumerator always owns a
                // non-null current_val (consumer didn't call TakeCurrent);
                // dispose it unconditionally then null the slot.
                emitDisposeAndReclaim(yv);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), cvField);
                builder_.CreateBr(contBB);
            } else {
                // §29 (#1120) / #2738 (v0.282): gating the dispose on the
                // current_val_owned flag distinguishes two cases — for
                // IEnumerable<owned T> (class element, #1120) the enumerator
                // owns the +1 only when the yield is a `new`; for
                // IEnumerable<string> / IList<string> / etc. (runtime-
                // managed primitive, #2738) the enumerator owns the +1 only
                // when the yield is a +1 string producer (interpolation, +,
                // ??, case, conditional, range slice, ToString — see
                // isPlusOneStringProducer). In both cases the predicate is
                // "the enumerator owns the reference"; the predicate differs
                // because the minting shape differs. The flag byte is shared
                // (CURRENT_VAL_OWNED_FIELD) and was already wired by both
                // yield-store sites.
                llvm::Value* ownedFld = builder_.CreateStructGEP(
                    stateStructTy, self, CURRENT_VAL_OWNED_FIELD);
                llvm::Value* ownedByte =
                    builder_.CreateLoad(i8(), ownedFld, "ity.yvowned");
                llvm::Value* isOwned = builder_.CreateICmpNE(
                    ownedByte, builder_.getInt8(0), "ity.yvisowned");
                auto* dispBB = llvm::BasicBlock::Create(
                    llvm_, "ity.yv.disp", targetFn);
                auto* skipBB = llvm::BasicBlock::Create(
                    llvm_, "ity.yv.skip", targetFn);
                auto* afterBB = llvm::BasicBlock::Create(
                    llvm_, "ity.yv.after", targetFn);
                builder_.CreateCondBr(isOwned, dispBB, skipBB);
                builder_.SetInsertPoint(dispBB);
                if (elemIsRuntimeManagedPrimitive) {
                    // fix #2738: release the +1 reference via the refcounted
                    // string release path — bz_string_release is the
                    // canonical releaser for a non-nullable string primitive
                    // (matching how the §28.3 / §28.9 borrow site releases
                    // a borrowed +1 on scope exit). emitDisposeAndReclaim
                    // would dispatch through the class dispatch table and
                    // is wrong for a non-class element type.
                    emitStringRelease(yv);
                } else {
                    // §29 (#1120): class element — emitDisposeAndReclaim
                    // walks the vtable dispatch chain.
                    emitDisposeAndReclaim(yv);
                }
                builder_.CreateBr(afterBB);
                builder_.SetInsertPoint(skipBB);
                builder_.CreateBr(afterBB);
                builder_.SetInsertPoint(afterBB);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), cvField);
                builder_.CreateStore(builder_.getInt8(0), ownedFld);
                builder_.CreateBr(contBB);
            }
            builder_.SetInsertPoint(contBB);
        };

        // If no yield was ever inside a try/finally AND no `using var` was active
        // at any yield (#821) AND no for-in enumerator/source frame was live at a
        // yield (#944), emit a simple free + return. fix #1567: the unified
        // establishment-ordered stack (smIterCleanupAtState_) is the superset of
        // all three; empty iff there is nothing to run.
        if (smIterCleanupAtState_.empty()) {
            // Set the DISPOSED sentinel (-2, §28.2 per-instance flag) BEFORE any
            // cleanup so a re-entrant / second Dispose no-ops at the entry guard.
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            builder_.CreateStore(builder_.getInt32(-2), stateGEP2);
            disposeYieldedVal();       // §29 (#1120): dispose outstanding yield val
            disposeOwnedParams();      // §29.8 (#945)
            disposeOwnedLocals();      // §29.7 (#2306): state-keyed plain-owned locals
            releaseClosureCaptures();  // #1864 §27.5/§28.9: drop frame's +1 on closure captures
            emitFreeSpillArea(self);   // #747: free any spilled for-loop frame
            builder_.CreateCall(freeFn, {self});
            builder_.CreateRetVoid();
        } else {
            // Build a switch on the current state.  State == -1 (MoveNext
            // exhaustion) or 0 (never iterated): nothing suspended inside a
            // try — just free.  State N: run the finally chain recorded for
            // that state, then free.
            auto* doneBB = llvm::BasicBlock::Create(llvm_, "dsp.done", targetFn);
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP2);
            // Set the DISPOSED sentinel (-2, §28.2 per-instance flag) immediately
            // — BEFORE the finally chains run — so a re-entrant / second Dispose
            // no-ops at the entry guard. (Switch below dispatches on the already-
            // loaded `curState`, so overwriting the field here is safe.)
            builder_.CreateStore(builder_.getInt32(-2), stateGEP2);
            // fix #1567 / §29.7/§28.10 (v0.182): the states that have any pending
            // cleanup — one unified establishment-ordered stack per state (a
            // finally chain, active using-var disposals #821, and/or live for-in
            // enumerator/source frames #944, in one list).
            std::set<unsigned> states;
            for (auto& kv : smIterCleanupAtState_) states.insert(kv.first);
            auto* sw = builder_.CreateSwitch(curState, doneBB,
                                             (unsigned)states.size());

            for (unsigned stateIdx : states) {
                auto* chainBB = llvm::BasicBlock::Create(
                    llvm_, "dsp.s" + std::to_string(stateIdx), targetFn);
                sw->addCase(builder_.getInt32((int)stateIdx), chainBB);
                builder_.SetInsertPoint(chainBB);
                // fix #1567 / §29.7/§28.10 (v0.182): run the ONE establishment-
                // ordered cleanup stack active at this yield in exact REVERSE —
                // using-var disposals, for-in enumerator-frame disposals, and
                // finally bodies interleaved by establishment position (a finally
                // established before an enclosing `using var` runs before that
                // using var's disposal, and vice versa; §28.10). The pre-v0.182
                // code ran all using-var disposals, then all finally bodies
                // (category phases), disposing an enclosing `using var` BEFORE a
                // finally that touched it — a use-after-dispose.
                auto cit = smIterCleanupAtState_.find(stateIdx);
                if (cit != smIterCleanupAtState_.end()) {
                    for (auto rit = cit->second.rbegin();
                         rit != cit->second.rend(); ++rit) {
                        const IterCleanup& ac = *rit;
                        if (ac.kind == IterCleanup::Kind::Disposal) {
                            // #821 / §29.7: rebind the Disposal's slot to the
                            // Dispose-restored local (the frame field was restored
                            // into locals_ above) and run the normal disposal.
                            auto lit = locals_.find(ac.node);
                            if (lit == locals_.end()) continue;  // not restored
                            Disposal d = ac.disposal;
                            d.slot = lit->second;
                            std::vector<Disposal> one{d};
                            emitDisposals(one);
                        } else if (ac.kind == IterCleanup::Kind::EnumFrame) {
                            // §29.7 (#944): dispose a for-in implicit enumerator/
                            // source frame — a frame ptr in a dedicated field. GEP
                            // it, load, null-guard, dispatch the iterator-vtable
                            // slot-1 Dispose (which recursively disposes the inner
                            // iterator's own active using-vars + frees its frame),
                            // then null the field so a re-entrant Dispose is a
                            // no-op. This is the SOLE disposal of these frames on
                            // the abandon-mid-loop path (the loop's own
                            // scope-exit/break disposal never ran — we suspended
                            // out of the body), so exactly once.
                            unsigned fieldIdx = ac.frameField;
                            llvm::Value* fptr = builder_.CreateStructGEP(
                                stateStructTy, self, fieldIdx);
                            llvm::Value* obj =
                                builder_.CreateLoad(ptrTy(), fptr, "fie.obj");
                            llvm::Value* isNull = builder_.CreateICmpEQ(
                                obj, llvm::ConstantPointerNull::get(ptrTy()),
                                "fie.isnull");
                            auto* dispBB = llvm::BasicBlock::Create(
                                llvm_, "fie.dispose", targetFn);
                            auto* contBB = llvm::BasicBlock::Create(
                                llvm_, "fie.cont", targetFn);
                            builder_.CreateCondBr(isNull, contBB, dispBB);
                            builder_.SetInsertPoint(dispBB);
                            // Iterator-vtable slot 1 = Dispose.
                            llvm::Value* vt = builder_.CreateLoad(ptrTy(), obj);
                            llvm::Value* dispSlot = builder_.CreateGEP(
                                ptrTy(), vt, {builder_.getInt32(1)});
                            llvm::Value* dispFnPtr =
                                builder_.CreateLoad(ptrTy(), dispSlot);
                            auto* dispTy = llvm::FunctionType::get(
                                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
                            builder_.CreateCall(dispTy, dispFnPtr, {obj});
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), fptr);
                            builder_.CreateBr(contBB);
                            builder_.SetInsertPoint(contBB);
                        } else {  // IterCleanup::Kind::Finally
                            // Run one finally body (§29.7 / §28.11).
                            //
                            // fix #2853 / §29.7 / §28.10 / §4.6 / §28.9 (v0.240):
                            // the `finally` body lowered here is the ABANDONMENT-
                            // side replay of the one inline in MoveNext for the
                            // NORMAL/EXCEPTION edge (lowerTry §30.9 / §31.2, line
                            // 4269). The two lowerings are over the SAME
                            // LocalDecl AST nodes — keyed by node, the three
                            // string-eligibility sets agree by construction —
                            // but the allocas are allocated afresh in EACH
                            // function (MoveNext in the first lowering, the
                            // Dispose fn `targetFn` here), and each lowering
                            // must register ITS OWN Disposal{stringRelease}
                            // into ITS OWN disposeStack_. The two edges are
                            // mutually exclusive: Dispose stamps the DISPOSED
                            // sentinel (`-2`, §28.2 / §29.7) so a later
                            // MoveNext cannot resume past the yield, and a
                            // run-to-exhaustion iterator parks at `-1`, whose
                            // switch default skips cleanup. So each release
                            // fires exactly once.
                            //
                            // armStringLocalRefcount — the only registrar of
                            // the scope-end Disposal{stringRelease} — gates on
                            // `currentFn_ == stringReleaseEligibleFn_`, and
                            // the token still named the MOVENEXT fn here
                            // (pinned at #2817, never re-set for `targetFn`):
                            // EVERY `var` string local declared inside a
                            // replayed `finally` body leaked its bz_string
                            // header + data buffer (measured 380 B / 20 for a
                            // 10x-abandoned iterator, down from #2817's
                            // 5112 B / 222 — #2817 closed the MoveNext-body
                            // share and left exactly the finally-in-Dispose
                            // share). The fix mirrors #2817's MOVE-NEXT pin
                            // (#1897 has the unwind pin in this same fn, and
                            // #2148/#2142/#2143 the borrowing-collection pins)
                            // and #2852's destructor pin: save the token, run
                            // collectStringReleaseEligible over THE FINALLY
                            // BODY (the AST walk is pure inspection; no IR
                            // registered), pin the token to `targetFn` for the
                            // duration of the body lowering, restore. // per
                            // §29.7, §28.10, §4.6, §28.9.
                            llvm::Function* savedStringReleaseEligibleFn =
                                stringReleaseEligibleFn_;
                            collectStringReleaseEligible(*ac.finallyBody);
                            stringReleaseEligibleFn_ = targetFn;
                            disposeStack_.emplace_back();
                            lowerBody(*ac.finallyBody);
                            // The replay uses `lowerBody` (not
                            // `lowerScopedBlock`), so the per-scope
                            // emit-on-pop that `lowerScopedBlock` performs
                            // does NOT run here. armStringLocalRefcount
                            // pushes its Disposal onto this frame, and a
                            // bare `pop_back()` would silently drop it.
                            //
                            // The other scope-end analysis tokens
                            // (`unwindIneligibleFn_`,
                            // `closureReleaseEligibleFn_`,
                            // `borrowingArrayFreeEligibleFn_`) are likewise
                            // not re-armed for `targetFn`, so any plain owned
                            // local / closure-local / borrowing-collection
                            // literal declared in a replayed `finally` is
                            // NOT registered to this frame (their gates are
                            // false) — but `registerUsingDisposal` is
                            // unconditional and WOULD be on this frame if
                            // such a local were present. Filter to ONLY the
                            // string-release entries armStringLocalRefcount
                            // pushed (this issue's scope); emit those. The
                            // other / broader in-finally-body leak class is
                            // filed separately and out of #2853's scope.
                            std::vector<Disposal> stringReleases;
                            stringReleases.reserve(disposeStack_.back().size());
                            for (const auto& d : disposeStack_.back()) {
                                if (d.stringRelease) {
                                    stringReleases.push_back(d);
                                }
                            }
                            if (!stringReleases.empty()) {
                                emitDisposals(stringReleases);
                            }
                            disposeStack_.pop_back();
                            stringReleaseEligibleFn_ = savedStringReleaseEligibleFn;
                        }
                    }
                }
                builder_.CreateBr(doneBB);
            }

            builder_.SetInsertPoint(doneBB);
            disposeYieldedVal();       // §29 (#1120): dispose outstanding yield val
            disposeOwnedParams();      // §29.8 (#945): after the finally chains
            disposeOwnedLocals();      // §29.7 (#2306): state-keyed plain-owned locals
            releaseClosureCaptures();  // #1864 §27.5/§28.9: drop frame's +1 on closure captures
            emitFreeSpillArea(self);   // #747: free any spilled for-loop frame
            builder_.CreateCall(freeFn, {self});
            builder_.CreateRetVoid();
        }
    };

    // §29.11 (#943): the source/IEnumerable Dispose disposes owned params; the
    // clone/enumerator Dispose does not. For a single-use IEnumerator<T> iterator
    // there is no separate clone, but emitting cloneDisposeFn is harmless (it is
    // never reached) and keeps the vtable layout uniform.
    //
    // #2271 item 2 / #973a — why the SOURCE runs with `stateKeyedOwned=false`:
    // a parameter that enters the loop on `yieldState != 0` alone (sema proved
    // it discharged) exists only for an `IOwningEnumerable<T>` iterator, whose
    // only consumption path is `GetOwningEnumerator` (stdlib §4.13) — so the
    // source is NEVER the driven frame, and the claim above has already nulled
    // its slots by the time any transfer can have happened. The state guard
    // would therefore be redundant there, while the null-guard still gives the
    // right answer in the one remaining case (GetEnumerator never called ⇒
    // slots live ⇒ dispose once, no leak). The same holds for a #973a
    // conditional parameter: on the `IEnumerable<T>` path the claim nulls the
    // source; on the `IEnumerator<T>` path there is no clone, the source IS the
    // driven frame, and the body's own null is what the guard reads.
    emitDisposeBody(disposeFn, /*disposeOwned=*/true,
                    /*stateKeyedOwned=*/false, /*releaseClosures=*/true);
    emitDisposeBody(cloneDisposeFn,
                    /*disposeOwned=*/cloneDisposesOwnedCaptures,
                    /*stateKeyedOwned=*/cloneDisposesOwnedCaptures,
                    /*releaseClosures=*/false);

    // Clear the per-iterator finally / disposal tracking.
    smIterFinallyAtState_.clear();
    smIterDisposalsAtState_.clear();  // #821
    smIterEnumFramesAtState_.clear();  // §29.7 (#944)
    smIterCleanupAtState_.clear();     // fix #1567 §29.7/§28.10 (v0.182)
    smIterEnumFrameSlot_.clear();      // §29.7 (#944)
    finallyBodyStack_.clear();
}


}  // namespace bezel::codegen
