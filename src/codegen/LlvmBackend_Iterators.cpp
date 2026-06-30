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
        n = iterTy->astNode->name->parts[0];
    } else if (!iterTy->importedName.empty()) {
        n = iterTy->importedName;
    } else {
        return false;
    }
    if (n != "IEnumerable" && n != "IReadOnlyCollection" && n != "IReadOnlyList" &&
        n != "ICollection" && n != "IList" && n != "OwnedEnumerable")
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
            return rt->astNode->name->parts[0] == "IEnumerable";
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

void Emitter::collectSuspendLocals(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::vector<std::pair<const ast::Node*, llvm::Type*>>& out) {
    for (const auto& s : body) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            // #461 (§30 async frames): the frame SLOT type for a suspend-live
            // local must be at least as wide as its alloca, or the per-suspend
            // spill (which stores `alloca->getAllocatedType()` bytes into the
            // slot — see emitAwaitSuspend) overflows into the adjacent frame
            // field and corrupts it (→ SIGSEGV on resume). When neither an
            // annotation nor an initializer exprType is available, default to a
            // POINTER-WIDTH slot (not i32): ptr is large enough to hold any
            // scalar alloca (i32/i64/bool/ptr) without overflow, so a local with
            // an unresolved type can never clobber its neighbours. With a known
            // type the exact lowered type is used (so an `int?`/record/value
            // struct still gets its precise slot and round-trips correctly).
            llvm::Type* ty = ptrTy();
            if (ld->type) {
                if (auto* rt = ctx_.resolvedType(*ld->type)) ty = lowerTypeRef(rt);
            } else if (ld->initializer) {
                if (auto* rt = ctx_.exprType(*ld->initializer)) ty = lowerTypeRef(rt);
            }
            out.push_back({ld, ty});
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
                    llvm::Type* ty = i32();
                    if (ald->type) {
                        if (auto* rt = ctx_.resolvedType(*ald->type))
                            ty = lowerTypeRef(rt);
                    } else if (ald->initializer) {
                        if (auto* rt = ctx_.exprType(*ald->initializer))
                            ty = lowerTypeRef(rt);
                    }
                    out.push_back({ald, ty});
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
        }
    }
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


void Emitter::emitIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                         const CallableInfo& ci) {
    if (!ci.stmts) return;
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
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0]) {
            elemSemaTy = retRef->genericArgs[0];
            elemTy = lowerTypeRef(elemSemaTy);
        }
    }

    // §29 (#1120): true when T is an owned class or interface AND the return type
    // is IEnumerable<T> (or a related non-owning interface) — meaning the
    // enumerator frame RETAINS ownership of the most-recently-yielded value.
    // The enumerator must dispose it on MoveNext advance (before storing the next)
    // and on Dispose (break / exhaustion / free).  Records/enums/primitives are
    // value types and never need this.
    //
    // Excluded: OwnedEnumerable<T> iterators transfer ownership of each yielded
    // value to the consumer loop variable on each MoveNext call — the consumer
    // (not the enumerator) is responsible for disposing it; the enumerator must
    // NOT also dispose current_val or the result is a double-free.
    bool elemIsOwnedClass = false;
    if (elemSemaTy &&
        elemSemaTy->kind == sema::TypeRef::Kind::Named &&
        elemSemaTy->declaration != nullptr &&
        (elemSemaTy->declaration->kind == ast::TypeDeclKind::Class ||
         elemSemaTy->declaration->kind == ast::TypeDeclKind::Interface)) {
        // Check the return type name is NOT OwnedEnumerable.
        std::string_view retName;
        if (ci.returnType) {
            auto* retRef = ctx_.resolvedType(*ci.returnType);
            if (retRef && retRef->astNode && retRef->astNode->name &&
                !retRef->astNode->name->parts.empty())
                retName = retRef->astNode->name->parts[0];
            else if (retRef && !retRef->importedName.empty())
                retName = retRef->importedName;
        }
        if (retName != "OwnedEnumerable")
            elemIsOwnedClass = true;
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
        // Install the clone vtable so disposing the enumerator frees it without
        // touching the source's owned parameters.
        gb.CreateStore(cloneVtGV,
            gb.CreateStructGEP(stateStructTy, clone, VTABLE_FIELD));
        gb.CreateRet(clone);
    }

    // --- Current: load current_val from struct ---
    {
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", currentFn);
        llvm::IRBuilder<> cb(entry);
        llvm::Value* self = &*currentFn->arg_begin();
        llvm::Value* cvField = cb.CreateStructGEP(stateStructTy, self, CURRENT_VAL_FIELD);
        cb.CreateRet(cb.CreateLoad(elemTy, cvField));
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
        smIterEnumFrameSlot_.clear();      // §29.7 (#944)
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", moveNextFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = &*moveNextFn->arg_begin();

        // Restore captured params (this + params) from the frame.
        emitFrameCaptureRestores(frame, self);

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
            // Dispose function below.) State is set to -1, so the consumer's
            // subsequent Dispose() is a no-op → each resource is disposed once.
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
    }

    // --- Dispose(ptr self) -> void (real body, §29.7 #623) ---
    // Emitted after MoveNext so smIterFinallyAtState_ is populated.
    // If the iterator was suspended inside a try/finally, run the finally
    // chains (innermost first) before freeing the frame.  A second Dispose
    // (state == -1) is a no-op per §29.7.
    //
    // §29.11 (#943): emitted TWICE — once as `disposeFn` (the SOURCE/IEnumerable
    // Dispose, which runs §29.8 owned-parameter auto-dispose) and once as
    // `cloneDisposeFn` (a per-loop enumerator Dispose, which skips owned-param
    // auto-dispose because the clone borrows the captures from the source). The
    // bodies are otherwise identical (same finally chains, same spill free, same
    // state guard). Parameterised by `targetFn` and `disposeOwned`.
    auto emitDisposeBody = [&](llvm::Function* targetFn, bool disposeOwned) {
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
                if (!ps || !ps->iteratorAutoDispose) continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                llvm::Value* fptr =
                    builder_.CreateStructGEP(stateStructTy, self, fieldIdx);
                llvm::Value* obj = builder_.CreateLoad(ptrTy(), fptr, "itp.obj");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()), "itp.isnull");
                auto* dispBB =
                    llvm::BasicBlock::Create(llvm_, "itp.dispose", targetFn);
                auto* contBB =
                    llvm::BasicBlock::Create(llvm_, "itp.cont", targetFn);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), fptr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
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
        auto disposeYieldedVal = [&]() {
            if (!elemIsOwnedClass) return;
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
            emitDisposeAndReclaim(yv);
            builder_.CreateBr(afterBB);
            builder_.SetInsertPoint(skipBB);
            builder_.CreateBr(afterBB);
            builder_.SetInsertPoint(afterBB);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), cvField);
            builder_.CreateStore(builder_.getInt8(0), ownedFld);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        };

        // If no yield was ever inside a try/finally AND no `using var` was active
        // at any yield (#821) AND no for-in enumerator/source frame was live at a
        // yield (#944), emit a simple free + return.
        if (smIterFinallyAtState_.empty() && smIterDisposalsAtState_.empty() &&
            smIterEnumFramesAtState_.empty()) {
            // Mark done so a second Dispose is a no-op (not strictly necessary
            // when there are no finally chains, but keeps the contract clean).
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            builder_.CreateStore(builder_.getInt32(-1), stateGEP2);
            disposeYieldedVal();       // §29 (#1120): dispose outstanding yield val
            disposeOwnedParams();      // §29.8 (#945)
            emitFreeSpillArea(self);   // #747: free any spilled for-loop frame
            builder_.CreateCall(freeFn, {self});
            builder_.CreateRetVoid();
        } else {
            // Build a switch on the current state.  State == -1 or 0: already
            // done or never suspended inside a try — just free.  State N: run
            // the finally chain recorded for that state, then free.
            auto* doneBB = llvm::BasicBlock::Create(llvm_, "dsp.done", targetFn);
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP2);
            // Mark done immediately so re-entrant Dispose is a no-op.
            builder_.CreateStore(builder_.getInt32(-1), stateGEP2);
            // The union of states that have a finally chain, active using-var
            // disposals (#821), and/or live for-in enumerator/source frames (#944).
            std::set<unsigned> states;
            for (auto& kv : smIterFinallyAtState_) states.insert(kv.first);
            for (auto& kv : smIterDisposalsAtState_) states.insert(kv.first);
            for (auto& kv : smIterEnumFramesAtState_) states.insert(kv.first);
            auto* sw = builder_.CreateSwitch(curState, doneBB,
                                             (unsigned)states.size());

            for (unsigned stateIdx : states) {
                auto* chainBB = llvm::BasicBlock::Create(
                    llvm_, "dsp.s" + std::to_string(stateIdx), targetFn);
                sw->addCase(builder_.getInt32((int)stateIdx), chainBB);
                builder_.SetInsertPoint(chainBB);
                // #821 / §29.7: dispose the `using var` resources active at this
                // yield, innermost-first (recorded outermost-first). Rebind each
                // Disposal's slot to the Dispose-restored local (the frame field
                // was restored into locals_ above) and run the normal disposal.
                auto dit = smIterDisposalsAtState_.find(stateIdx);
                if (dit != smIterDisposalsAtState_.end()) {
                    for (auto rit = dit->second.rbegin();
                         rit != dit->second.rend(); ++rit) {
                        auto lit = locals_.find(rit->first);
                        if (lit == locals_.end()) continue;  // not restored
                        Disposal d = rit->second;
                        d.slot = lit->second;
                        std::vector<Disposal> one{d};
                        emitDisposals(one);
                    }
                }
                // §29.7 (#944): dispose the for-in implicit enumerator/source
                // frames live at this yield, innermost-first (recorded
                // outermost-first). Each is a frame ptr in a dedicated frame
                // field; GEP it, load, null-guard, dispatch the iterator-vtable
                // slot-1 Dispose (which recursively disposes the inner iterator's
                // own active using-vars + frees its frame), then null the field so
                // a re-entrant Dispose is a no-op. This is the SOLE disposal of
                // these frames on the abandon-mid-loop path (the loop's own
                // scope-exit/break disposal never ran — we suspended out of the
                // body), so exactly once.
                auto eit = smIterEnumFramesAtState_.find(stateIdx);
                if (eit != smIterEnumFramesAtState_.end()) {
                    for (auto rfit = eit->second.rbegin();
                         rfit != eit->second.rend(); ++rfit) {
                        unsigned fieldIdx = *rfit;
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
                    }
                }
                // Run finally bodies innermost-first (chain is outermost-first,
                // so iterate in reverse per §29.7 / §28.11).
                auto fit = smIterFinallyAtState_.find(stateIdx);
                if (fit != smIterFinallyAtState_.end()) {
                    for (auto it = fit->second.rbegin();
                         it != fit->second.rend(); ++it) {
                        disposeStack_.emplace_back();
                        lowerBody(**it);
                        disposeStack_.pop_back();
                    }
                }
                builder_.CreateBr(doneBB);
            }

            builder_.SetInsertPoint(doneBB);
            disposeYieldedVal();       // §29 (#1120): dispose outstanding yield val
            disposeOwnedParams();      // §29.8 (#945): after the finally chains
            emitFreeSpillArea(self);   // #747: free any spilled for-loop frame
            builder_.CreateCall(freeFn, {self});
            builder_.CreateRetVoid();
        }
    };

    // §29.11 (#943): the source/IEnumerable Dispose disposes owned params; the
    // clone/enumerator Dispose does not. For a single-use IEnumerator<T> iterator
    // there is no separate clone, but emitting cloneDisposeFn is harmless (it is
    // never reached) and keeps the vtable layout uniform.
    emitDisposeBody(disposeFn, /*disposeOwned=*/true);
    emitDisposeBody(cloneDisposeFn, /*disposeOwned=*/false);

    // Clear the per-iterator finally / disposal tracking.
    smIterFinallyAtState_.clear();
    smIterDisposalsAtState_.clear();  // #821
    smIterEnumFramesAtState_.clear();  // §29.7 (#944)
    smIterEnumFrameSlot_.clear();      // §29.7 (#944)
    finallyBodyStack_.clear();
}


}  // namespace bezel::codegen
