#include "codegen/LlvmBackend_impl.hpp"

#include <set>

namespace bezel::codegen {

// ===========================================================================
// §30 async/await state-machine codegen (Phase 2b).
//
// Layers the task-driver shape on top of the kind-agnostic suspend/resume
// scaffolding extracted in Phase 2a (LlvmBackend_Iterators.cpp). The async
// path mirrors the iterator (`Yield`) path with smKind_ == Await:
//
//   - The original method (declared `async`, returning Task / Task<T> /
//     OwningTask<T>) becomes the ENTRY function. It allocates the outer Task,
//     calloc's the heap frame, spills the captures (`this` + params) and the
//     outer-task pointer into the frame, then invokes the Resume function once.
//     Because the runtime's bz_task_then has an already-done fast path and the
//     synchronous prefix runs inline (§30.1: "begins execution synchronously …
//     until the first await of an incomplete task"), a body whose awaited tasks
//     are all already-complete runs to completion within this single inline
//     drive. The entry returns the outer Task pointer.
//
//   - A Resume(void* completedTask, void* frame) continuation matches the
//     bz_task_then ABI. It restores the captures from the frame and runs the
//     resume dispatch (LlvmBackend_Iterators.cpp emitResumeDispatch). Each
//     `await` suspend point (LlvmBackend_Exprs.cpp lowerAwait) spills the
//     suspend-locals, registers Resume on the awaited task, and returns; the
//     resume re-enters at the next state block and reads the awaited result.
//     `return e;` completes the outer task (LlvmBackend_Stmts.cpp ReturnStmt,
//     gated on smKind_ == Await).
//
// #438: `await` inside a loop body (while / range-for / nested) is supported.
// Loop-carried and pre-/post-loop locals are frame-backed allocas (the iterator
// path's spill/restore machinery, reused), and fixupSuspendDominance handles the
// remaining anonymous cross-suspend temporaries (e.g. the left operand of
// `total + await f()`).
//
// #439-followup: those anonymous cross-suspend temporaries now live in the HEAP
// FRAME, not a function-local stack slot, so they survive a REAL cross-thread
// suspend. fixupSuspendDominance demotes the offending values and then
// promoteDemotedSlotsToFrame moves each into a side spill area pointed to from
// the frame (asyncSpillPtrField_) — reached through the single `state` pointer
// bz_task_then carries to the worker — and freed on the terminal paths
// (emitFreeSpillArea). This makes `var x := total + await f()` (and the
// loop-carried / multi-await variants) correct across a thread hop, where a
// stack slot held garbage on the worker's fresh stack.
//
// Deferred to later increments (not emitted here): await inside try/catch/
// finally (resuming into the right handler scope — rejected with a §30.9
// diagnostic), §30.9 faulting re-raise beyond the basic check, async iterators
// (IAsyncEnumerable / `await for`), Task.FireAndForget, and `await using`.
// ===========================================================================

bool Emitter::exprContainsAwait(const ast::Expr* e) {
    if (!e) return false;
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(e)) {
        if (un->op == "await") return true;
        return exprContainsAwait(un->operand.get());
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(e))
        return exprContainsAwait(bin->left.get()) ||
               exprContainsAwait(bin->right.get());
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return exprContainsAwait(me->target.get());
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(e))
        return exprContainsAwait(nm->target.get());
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        if (exprContainsAwait(ce->callee.get())) return true;
        for (const auto& a : ce->args)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        if (exprContainsAwait(nc->callee.get())) return true;
        for (const auto& a : nc->args)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        if (exprContainsAwait(ix->target.get())) return true;
        for (const auto& a : ix->indices)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(e))
        return exprContainsAwait(bg->operand.get());
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
        for (const auto& a : ne->args)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e))
        return exprContainsAwait(cd->condition.get()) ||
               exprContainsAwait(cd->thenValue.get()) ||
               exprContainsAwait(cd->elseValue.get());
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(e))
        return exprContainsAwait(te->operand.get());
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e))
        return exprContainsAwait(th->operand.get());
    if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        if (exprContainsAwait(we->source.get())) return true;
        for (const auto& ia : we->assigns)
            if (exprContainsAwait(ia.value.get())) return true;
        return false;
    }
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(e))
        return exprContainsAwait(re->lo.get()) ||
               exprContainsAwait(re->hi.get()) ||
               exprContainsAwait(re->step.get());
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& k : cl->keys)
            if (exprContainsAwait(k.get())) return true;
        for (const auto& v : cl->elements)
            if (exprContainsAwait(v.get())) return true;
        return false;
    }
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& p : is->parts)
            if (exprContainsAwait(p.expr.get())) return true;
        return false;
    }
    // LambdaExpr is a separate async scope (§30.6) — not descended here.
    return false;
}


bool Emitter::containsAwait(const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            if (exprContainsAwait(ld->initializer.get())) return true;
        } else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s.get())) {
            if (exprContainsAwait(lc->value.get())) return true;
        } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s.get())) {
            if (exprContainsAwait(as->value.get())) return true;
        } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s.get())) {
            if (exprContainsAwait(es->expr.get())) return true;
        } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get())) {
            if (exprContainsAwait(rs->value.get())) return true;
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            // §30.7: an `await for` itself contains an await (MoveNextAsync).
            if (fs->isAwait) return true;
            if (containsAwait(fs->body)) return true;
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            if (exprContainsAwait(ws->condition.get())) return true;
            if (containsAwait(ws->body)) return true;
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (exprContainsAwait(ifs->ifBranch.condition.get())) return true;
            if (containsAwait(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches) {
                if (exprContainsAwait(el.condition.get())) return true;
                if (containsAwait(el.body)) return true;
            }
            if (ifs->hasElse && containsAwait(ifs->elseBody)) return true;
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            if (containsAwait(db->body)) return true;
        }
    }
    return false;
}


// §30.2: count the await suspend points in a body (drives the resume switch's
// state-block count, exactly like collectYields for the iterator path).
static unsigned countAwaits(const std::vector<std::unique_ptr<ast::Stmt>>& stmts);
static unsigned countAwaitsInStmt(const ast::Stmt* s);

static unsigned countAwaitsInExpr(const ast::Expr* e) {
    if (!e) return 0;
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(e)) {
        unsigned n = countAwaitsInExpr(un->operand.get());
        if (un->op == "await") n += 1;
        return n;
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(e))
        return countAwaitsInExpr(bin->left.get()) +
               countAwaitsInExpr(bin->right.get());
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return countAwaitsInExpr(me->target.get());
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(e))
        return countAwaitsInExpr(nm->target.get());
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        unsigned n = countAwaitsInExpr(ce->callee.get());
        for (const auto& a : ce->args) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        unsigned n = countAwaitsInExpr(nc->callee.get());
        for (const auto& a : nc->args) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        unsigned n = countAwaitsInExpr(ix->target.get());
        for (const auto& a : ix->indices) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(e))
        return countAwaitsInExpr(bg->operand.get());
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
        unsigned n = 0;
        for (const auto& a : ne->args) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e))
        return countAwaitsInExpr(cd->condition.get()) +
               countAwaitsInExpr(cd->thenValue.get()) +
               countAwaitsInExpr(cd->elseValue.get());
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(e))
        return countAwaitsInExpr(te->operand.get());
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e))
        return countAwaitsInExpr(th->operand.get());
    if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        unsigned n = countAwaitsInExpr(we->source.get());
        for (const auto& ia : we->assigns) n += countAwaitsInExpr(ia.value.get());
        return n;
    }
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(e))
        return countAwaitsInExpr(re->lo.get()) +
               countAwaitsInExpr(re->hi.get()) +
               countAwaitsInExpr(re->step.get());
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        unsigned n = 0;
        for (const auto& k : cl->keys) n += countAwaitsInExpr(k.get());
        for (const auto& v : cl->elements) n += countAwaitsInExpr(v.get());
        return n;
    }
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        unsigned n = 0;
        for (const auto& p : is->parts) n += countAwaitsInExpr(p.expr.get());
        return n;
    }
    // LambdaExpr is a separate async scope (§30.6) — not descended here.
    return 0;
}

static unsigned countAwaits(const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    unsigned n = 0;
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            n += countAwaitsInExpr(ld->initializer.get());
            // §28.12 / §30: `await using var` carries an implicit scope-end
            // `await DisposeAsync()` suspend point (counted here so the resume
            // switch has a state block for it — omitting it would leave the
            // dispose-await's state unreachable and hang cross-thread like #441).
            if (ld->isAwaitUsing) n += 1;
        }
        else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s.get()))
            n += countAwaitsInExpr(lc->value.get());
        else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s.get())) {
            n += countAwaitsInExpr(as->value.get());
        }
        // fix #2333: `_ := await …` parses as DiscardStmt, not AssignmentStmt;
        // the resume switch must count the await in its `value` or the post-
        // await continuation lands on the default (return) and the outer task
        // never completes — the same hang shape as #441 for try-body awaits.
        else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s.get()))
            n += countAwaitsInExpr(ds->value.get());
        else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s.get()))
            n += countAwaitsInExpr(es->expr.get());
        else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get()))
            n += countAwaitsInExpr(rs->value.get());
        else if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s.get())) {
            // §30.7 (#437): `yield return await Expr` in an async iterator body
            // contains an `await` in its value expression. Count it so the resume
            // switch has the correct number of state blocks — omitting it would
            // leave the yield-return's post-yield state block missing, causing the
            // for-loop continuation (br incBB) to be emitted into the still-current
            // await.ok block (after its ret void terminator → IR verifier error).
            if (!ys->isBreak) n += countAwaitsInExpr(ys->value.get());
        }
        else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            // §30.7: `await for` desugars to a loop whose head awaits the
            // enumerator's MoveNextAsync() each iteration — one suspend point
            // (the loop re-enters the same await.resume block). Count it so the
            // consuming method's resume switch has a state block for it. This
            // includes the `await for ... with token` form (#765): the `with`
            // clause is lowered the same way (the token is evaluated and then
            // discarded), so it carries the same single suspend point.
            if (fs->isAwait) n += 1;
            n += countAwaits(fs->body);
        }
        else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            n += countAwaitsInExpr(ws->condition.get());
            n += countAwaits(ws->body);
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            n += countAwaitsInExpr(ifs->ifBranch.condition.get());
            n += countAwaits(ifs->ifBranch.body);
            for (const auto& el : ifs->elifBranches) {
                n += countAwaitsInExpr(el.condition.get());
                n += countAwaits(el.body);
            }
            if (ifs->hasElse) n += countAwaits(ifs->elseBody);
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            n += countAwaits(db->body);
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // §30.9 / #441: count awaits in the try BODY (the only position where
            // await is lowered — awaitInDeferredTryPosition rejects catch arms and
            // finally). Recurse into the try body so a nested compound statement
            // containing an await is counted; also recurse into catch/finally for
            // future-proofness (counting them is harmless — if they are ever
            // rejected by awaitInDeferredTryPosition the compile fails before
            // emitting the resume switch, so the extra states are never reached).
            // This fixes a HANG: without this case the try-body await is uncounted,
            // numAwaits is too small, the resume switch lacks the state for the
            // in-try await, and when a pool worker fires the continuation with that
            // state the switch hits its default (return) — bz_task_complete is
            // never called, so the outer Task never completes and
            // bz_task_await_sync hangs forever.
            n += countAwaits(ts->body);
            // §30.9 / §31.2 (#438): count awaits in catch arms (guard + body) and
            // the finally too — each is now a real suspend point (await-in-catch /
            // await-in-finally are supported), so each needs a resume-switch state.
            // A catch arm body is a single Stmt (a DoEndBlock for multi-statement
            // arms, or a bare expr/assign/local/return for one-liners); count
            // through any of those shapes. Undercounting here is the #441 hang: a
            // suspend with no resume state lands the switch on its default
            // (return), so the outer task never completes.
            for (const auto& arm : ts->catches) {
                if (arm.guard) n += countAwaitsInExpr(arm.guard.get());
                n += countAwaitsInStmt(arm.body.get());
            }
            // §31.2 (#438): lowerTry emits the finally body TWICE — once in the
            // normal-exit path (finNormalBB) and once in the exception-unwind path
            // (finUnwindBB) — because the finally runs on every completion path.
            // Each lowering of an in-finally `await` consumes its OWN resume state
            // (++smNextSuspendState_), so a finally with k awaits needs 2*k states.
            // Counting it once would leave the unwind-path finally's await without
            // a resume state → #441 hang on the faulted-body path.
            if (ts->hasFinally) n += 2 * countAwaits(ts->finallyBody);
        }
    }
    return n;
}

// §30.9 / §31.2 (#438): count awaits in a SINGLE statement — used for a catch
// arm body, which is one Stmt (a DoEndBlock for the multi-statement form, or a
// bare expr/assignment/local/return for the one-liner form). Recurses into the
// common shapes so an await anywhere in the arm body is counted.
static unsigned countAwaitsInStmt(const ast::Stmt* s) {
    if (!s) return 0;
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s))
        return countAwaits(db->body);
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s))
        return countAwaitsInExpr(ld->initializer.get());
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s))
        return countAwaitsInExpr(lc->value.get());
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s))
        return countAwaitsInExpr(as->value.get());
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s))
        return countAwaitsInExpr(es->expr.get());
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s))
        return countAwaitsInExpr(rs->value.get());
    return 0;
}


// §29.7 (#2574): the async counterpart of collectYieldStatesForOwnedLocals
// (LlvmBackend_Iterators.cpp, fix #2306). Same linear-scan contract — record
// per-LocalDecl the (establishedAtState, transferredAtState) window — but the
// async state machine numbers BOTH yields AND awaits as suspend states
// (smNextSuspendState_ is pre-incremented for each, §30.7), so the scan must
// advance `state` for awaits too. Without this, an await between a local's
// declaration and its transfer-yield shifts the actual runtime states, and the
// yield-only window would either miss a live await-state (leak) or over-fire
// (double-free). The scan counts awaits in every simple-statement expression
// position via countAwaitsInExpr, recurses through try bodies and do/end
// blocks (matching the sync version), and ABORTS on any non-linear shape
// (IfStmt / WhileStmt / ForStmt / lambdas), on `await using var` (its implicit
// dispose-await at scope-end is a suspend point whose position the linear walk
// cannot statically place), and on try statements with awaits in catch arms or
// finally (the finally body is lowered TWICE — normal exit + unwind — doubling
// its await count, making the linear scan's state numbering unreliable). A
// try with catches/finally that have NO awaits is safe: they consume zero
// suspend states, so the try body's yields+awaits fully determine the state
// counter for code after the try. Aborting leaves the legacy bounded leak in
// place rather than risking a double-free.
static void collectAsyncSuspendStatesForOwnedLocals(
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
            // `await using var` carries an implicit `await DisposeAsync()` at
            // scope end — a suspend point whose position depends on the scope
            // exit, not the linear walk. The linear scan cannot statically
            // place it, so abort to avoid a state-numbering mismatch. This MUST
            // precede the `isUsing` skip below: `isAwaitUsing` implies
            // `isUsing` (ast/Node.hpp), so checking `isUsing` first would
            // `continue` past this abort, silently mis-counting the scope-end
            // await instead of bailing out.
            if (ld->isAwaitUsing) { ok = false; return; }
            if (ld->isUsing || ld->autoDropAtScopeEnd) continue;
            auto oit = out.find(ld);
            if (oit != out.end()) { ok = false; return; }
            out[ld] = {state + 1, 0};
            // Count awaits in the initializer — they consume suspend states
            // BEFORE the next yield, advancing the state counter.
            state += countAwaitsInExpr(ld->initializer.get());
            continue;
        }
        // Simple statements whose expression positions may contain awaits.
        // Each await consumes a suspend state, advancing `state`.
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s.get())) {
            state += countAwaitsInExpr(lc->value.get());
            continue;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s.get())) {
            state += countAwaitsInExpr(as->value.get());
            continue;
        }
        // fix #2333: `_ := await …` parses as DiscardStmt, not AssignmentStmt.
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s.get())) {
            state += countAwaitsInExpr(ds->value.get());
            continue;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s.get())) {
            state += countAwaitsInExpr(es->expr.get());
            continue;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get())) {
            state += countAwaitsInExpr(rs->value.get());
            continue;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s.get())) {
            if (ys->isBreak || !ys->value) { ok = false; return; }
            // §30.7 (#437): `yield return await Expr` contains an await in the
            // value expression that suspends BEFORE the yield. Count it first.
            state += countAwaitsInExpr(ys->value.get());
            // Process the yield transfer (mirrors sync #2306).
            auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                ys->value.get());
            if (id && (id->name == "this" || id->name == "base")) {
                ok = false; return;
            }
            sema::Symbol* sym = id ? ctx.exprSymbol(*id) : nullptr;
            auto* loc = dynamic_cast<sema::LocalSymbol*>(sym);
            if (id && (!loc || !loc->decl || loc->isUsing)) {
                ok = false; return;
            }
            if (loc && loc->decl) {
                auto* ldDecl = dynamic_cast<const ast::LocalDecl*>(loc->decl);
                if (!ldDecl) { ok = false; return; }
                auto oit = out.find(ldDecl);
                if (oit == out.end()) { ok = false; return; }
                if (oit->second.second != 0) { ok = false; return; }
                oit->second.second = state + 1;
            }
            ++state;
            continue;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // Recurse into the protected body only (§29.6 forbids `yield` in
            // catches / finally, so no yield-position transfer occurs there).
            collectAsyncSuspendStatesForOwnedLocals(
                ctx, ts->body, state, out, ok);
            if (!ok) return;
            // Catches and finally with awaits consume suspend states in a
            // non-linear order (the finally body is lowered TWICE, doubling
            // its await count). If any await is present, abort — the linear
            // scan's state numbering would be unreliable past this point.
            for (const auto& arm : ts->catches) {
                if (arm.guard && countAwaitsInExpr(arm.guard.get()) > 0) {
                    ok = false; return;
                }
                const ast::Stmt* ab = arm.body.get();
                unsigned armAwaits = ab
                    ? (dynamic_cast<const ast::DoEndBlock*>(ab)
                         ? countAwaits(
                               dynamic_cast<const ast::DoEndBlock*>(ab)->body)
                         : countAwaitsInStmt(ab))
                    : 0;
                if (armAwaits > 0) { ok = false; return; }
            }
            if (ts->hasFinally && countAwaits(ts->finallyBody) > 0) {
                ok = false; return;
            }
            continue;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectAsyncSuspendStatesForOwnedLocals(
                ctx, db->body, state, out, ok);
            continue;
        }
        // IfStmt / WhileStmt / ForStmt / lambdas / anything else has
        // data-dependent state ordering — abort the linear scan.
        ok = false;
        return;
    }
}


// §30.9 / §31.2 (#438): detect an `await` in a STILL-DEFERRED try position — a
// catch arm (guard or body) or a `finally` body. An `await` in the try BODY is
// now supported: lowerTry lowers the body (and hence the await's resume block)
// inside the try's protected region, so the resume keeps the catch dispatch /
// finally as its unwind target — a faulted await (re-raised with the original
// exception object, §30.9) routes to the enclosing catch, and the finally runs
// on every completion path. What remains hard is resuming INTO a catch handler
// (re-establishing the caught-exception disposal scope + currentCaughtExc_
// across a suspend) or into a finally (the exit-mode re-dispatch across a
// suspend); those are rejected with a diagnostic + safe stub. We recurse into
// every compound statement — including a try BODY — so a deferred position
// nested inside a loop / if / do-block / inner try is still caught.
bool Emitter::awaitInDeferredTryPosition(
    const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // §30.9 / §31.2 (#438): `await` in a catch GUARD remains deferred —
            // a guard runs DURING catch dispatch (before the arm is entered and
            // before currentCaughtExc_ / the caught-exc disposal scope are set
            // up), so suspending there would have to re-establish and re-run the
            // dispatch sequence at resume. `await` in a catch ARM BODY and in a
            // FINALLY are now supported (lowerTry frame-backs the handler-context
            // allocas and the in-flight exception across the suspend), so they are
            // NOT flagged here.
            for (const auto& arm : ts->catches) {
                if (arm.guard && exprContainsAwait(arm.guard.get()))
                    return true;
            }
            // The try BODY, catch arm bodies, and finally may each contain a
            // NESTED try whose catch GUARD awaits — recurse into all of them.
            if (awaitInDeferredTryPosition(ts->body)) return true;
            for (const auto& arm : ts->catches) {
                const ast::Stmt* ab = arm.body.get();
                if (auto* db = dynamic_cast<const ast::DoEndBlock*>(ab))
                    if (awaitInDeferredTryPosition(db->body)) return true;
            }
            if (awaitInDeferredTryPosition(ts->finallyBody)) return true;
            continue;
        }
        // Recurse into other compound statements so a deferred position nested
        // inside a loop, if, or do-block is still caught.
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            if (awaitInDeferredTryPosition(fs->body)) return true;
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            if (awaitInDeferredTryPosition(ws->body)) return true;
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (awaitInDeferredTryPosition(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (awaitInDeferredTryPosition(el.body)) return true;
            if (ifs->hasElse && awaitInDeferredTryPosition(ifs->elseBody))
                return true;
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            if (awaitInDeferredTryPosition(db->body)) return true;
        }
    }
    return false;
}


// §30.7 (#440): does a SINGLE statement contain a suspend point (a `yield` or an
// `await`)? Used by suspendInTryPosition to inspect a catch-arm body (which is a
// single Stmt, possibly a DoEndBlock). Recurses into the common compound shapes.
bool Emitter::stmtContainsSuspend(const ast::Stmt* s) {
    if (!s) return false;
    if (dynamic_cast<const ast::YieldStmt*>(s)) return true;
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s))
        return containsYield(db->body) || containsAwait(db->body);
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s))
        return exprContainsAwait(ld->initializer.get());
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s))
        return exprContainsAwait(es->expr.get());
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s))
        return exprContainsAwait(as->value.get());
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s))
        return exprContainsAwait(rs->value.get());
    return false;
}

// §29.6 / #554: does a SINGLE statement contain a `yield` (but not necessarily
// an `await`)? Used by suspendInTryPosition to check catch arm bodies and finally
// blocks, where only `yield` is forbidden (§29.6) — `await` in those positions
// is supported (§30.9 allows await in catch arm bodies and finally).
bool Emitter::stmtContainsYield(const ast::Stmt* s) {
    if (!s) return false;
    if (dynamic_cast<const ast::YieldStmt*>(s)) return true;
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s))
        return containsYield(db->body);
    return false;
}


// §30.9 / §31.2 (#438): does a single statement (a catch-arm body) contain an
// `await`? A catch arm body is one Stmt — a DoEndBlock for the multi-statement
// form, or a bare expr/assignment/local/return for the one-liner. Recurses into
// the common shapes. Unlike stmtContainsSuspend it does NOT report `yield`
// (irrelevant for the plain-async catch/finally path).
bool Emitter::stmtContainsAwait(const ast::Stmt* s) {
    if (!s) return false;
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s))
        return containsAwait(db->body);
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s))
        return exprContainsAwait(ld->initializer.get());
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s))
        return exprContainsAwait(lc->value.get());
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s))
        return exprContainsAwait(as->value.get());
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s))
        return exprContainsAwait(es->expr.get());
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s))
        return exprContainsAwait(rs->value.get());
    return false;
}


// §30.7 v0.137 (#655): detect an `await` inside a `finally` block anywhere in
// the body.  Forbidden in async iterators because the Dispose path (§29.7) runs
// synchronously — when a consumer abandons iteration early there is no async
// driver, so a suspension inside a `finally` would have nowhere to resume; the
// continuation would be silently discarded.
//
// This check is ONLY for async iterators.  Ordinary async methods may await in
// their `finally` (§30.9); emitAsyncMethod must not call this function.
//
// Recurses through compound statements (loops, if, do-blocks, nested try bodies
// and catch arm bodies) but not into nested try's finally blocks visited via
// the top-level (they are found when we descend into that TryStmt).
bool Emitter::awaitInFinallyPosition(
    const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // Check: is there an await directly in this try's finally body?
            if (containsAwait(ts->finallyBody)) return true;
            // Recurse into the try body and catch arm bodies for nested try stmts.
            if (awaitInFinallyPosition(ts->body)) return true;
            for (const auto& arm : ts->catches) {
                const ast::Stmt* ab = arm.body.get();
                if (auto* db = dynamic_cast<const ast::DoEndBlock*>(ab)) {
                    if (awaitInFinallyPosition(db->body)) return true;
                }
            }
            // Recurse into this finally for nested try statements inside it
            // (a try/finally nested inside a finally).
            if (awaitInFinallyPosition(ts->finallyBody)) return true;
            continue;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            if (awaitInFinallyPosition(fs->body)) return true;
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            if (awaitInFinallyPosition(ws->body)) return true;
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (awaitInFinallyPosition(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (awaitInFinallyPosition(el.body)) return true;
            if (ifs->hasElse && awaitInFinallyPosition(ifs->elseBody)) return true;
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            if (awaitInFinallyPosition(db->body)) return true;
        }
    }
    return false;
}


// §29.6 / §30.7 (#554): detect a FORBIDDEN suspend position in an async
// iterator body. The spec (§29.6) allows `yield return` / `yield break` inside a
// `try` body whose surrounding statement has ONLY a `finally` (no catch arms) —
// the same rule that governs synchronous iterators. For async iterators (§30.7),
// `await` in the try body, a catch arm body, and a finally is also allowed (§30.9
// plain-async rule applies here too). The FORBIDDEN positions are:
//
//   • yield inside a `try` body whose surrounding `try` has any catch arms.
//   • yield inside a catch arm body (§29.6: "yield inside a catch arm: forbidden").
//   • yield inside a `finally` block (§29.6: "yield inside finally: forbidden").
//   • await inside a catch GUARD (`catch E when await ...`) — the guard runs
//     during catch dispatch before the handler scope exists; §30.9 deferred.
//
// Everything else is allowed: yield or await in a try body with only finally
// (§29.6 "allowed"), await in catch arm bodies and finally bodies (§30.9).
// Recurses into nested compound statements so a forbidden position nested inside
// a loop / if / do-block / outer try is still caught.
bool Emitter::suspendInTryPosition(
    const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            const bool hasCatch = !ts->catches.empty();

            // §29.6: yield in a try body WITH catch arms is forbidden.
            if (hasCatch && containsYield(ts->body)) return true;

            // §29.6: yield inside a catch arm body is forbidden (regardless of
            // whether the outer try also has a finally).
            for (const auto& arm : ts->catches) {
                if (stmtContainsYield(arm.body.get())) return true;
                // §30.9: await in a catch GUARD is still deferred (runs during
                // dispatch before the handler scope exists).
                if (arm.guard && exprContainsAwait(arm.guard.get())) return true;
            }

            // §29.6: yield inside a finally block is forbidden.
            if (containsYield(ts->finallyBody)) return true;

            // Recurse into the try body for NESTED try statements.
            if (suspendInTryPosition(ts->body)) return true;
            // Recurse into catch arm bodies for nested try.
            for (const auto& arm : ts->catches) {
                const ast::Stmt* ab = arm.body.get();
                if (auto* db = dynamic_cast<const ast::DoEndBlock*>(ab)) {
                    if (suspendInTryPosition(db->body)) return true;
                }
            }
            // Recurse into the finally for nested try.
            if (suspendInTryPosition(ts->finallyBody)) return true;
            continue;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            if (suspendInTryPosition(fs->body)) return true;
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            if (suspendInTryPosition(ws->body)) return true;
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (suspendInTryPosition(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (suspendInTryPosition(el.body)) return true;
            if (ifs->hasElse && suspendInTryPosition(ifs->elseBody)) return true;
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            if (suspendInTryPosition(db->body)) return true;
        }
    }
    return false;
}


// §30.5 / #1503: classify `taskTypeRef` (the FULL declared/annotated async
// return type, e.g. `Task<Config>` or `OwningTask<Token>` — NOT just the
// generic argument T) and return the per-T disposer thunk iff it names
// Pattern A `Task<T:class>`. Task<T> always owns its produced class-typed
// result directly: every await and every `.Result` read only ever produces a
// BORROW (§30.5 "Consumer-side rules" / §30.3), never a consuming move, so
// the task itself — not any particular consumer — is the one place that can
// discharge the result's disposal. Returns null for:
//   - `Task` (no generic arg — no result to dispose);
//   - `Task<T>` where T is not class/interface (§30.5: "no ownership
//     obligation crosses the await boundary" for value/string/runtime-managed
//     T);
//   - `OwningTask<T>` (Pattern B) — an unconsumed OwningTask<T> result is the
//     CONSUMER's disposal responsibility at its `using var` scope end (§30.5
//     "Disposing an unconsumed OwningTask<T>"), wired separately via
//     bz_task_set_result_disposer in LlvmBackend_Disposal.cpp's `using var`
//     Task-disposal block — attaching it here too would be redundant, not
//     unsound (bz_task_free's guard already tolerates a second identical
//     set), but the two call sites would then disagree about WHO owns the
//     decision, so this function deliberately excludes it.
// Shared by emitAsyncMethod and emitAsyncLambda.
// §30.5 / #2279 (v0.236): when T is itself an INTRINSIC COLLECTION
// (Array<T>/Set<T>/Dictionary<K,V>/LinkedList<T>/Stack<T>/Queue<T>/
// OwningBuffer<T>/Owning* collections), the task owns the collection's
// backing storage directly — the elementPolicyFor path returns a null
// per-element disposer (the element is a value/string/etc., not a class),
// so the produced collection was never reclaimed at task disposal (an
// LSan leak of the bz_array_t*/bz_dict_t*/bz_llist_t* header+data —
// #2279's repro leaks 104 bytes from bz_array_alloc per async_body exit).
// patternAResultCollectionDisposer (declared in impl.hpp, defined in
// Disposal.cpp alongside the matching `using var` collection path) fills
// that gap with a per-collection-kind free thunk.
llvm::Function* Emitter::patternAResultDisposerFor(
    const sema::TypeRef* taskTypeRef) {
    // isTaskTypeName is the established Task/OwningTask recognizer (used by
    // taskTypeOfLocal for `using var` locals) — it handles both shapes a
    // builtin task TypeRef can take: no declaration at all (name read from
    // astNode), or a declaration with no user TypeDecl (name read from
    // declaration->name). Reuse it rather than re-deriving the check.
    if (!taskTypeRef || !isTaskTypeName(taskTypeRef)) return nullptr;
    std::string_view name;
    if (!taskTypeRef->declaration)
        name = taskTypeRef->headName();
    else if (!taskTypeRef->declaration->decl)
        name = taskTypeRef->declaration->name;
    if (name != "Task") return nullptr;  // excludes OwningTask (Pattern B)
    if (taskTypeRef->genericArgs.empty() || !taskTypeRef->genericArgs[0])
        return nullptr;
    const sema::TypeRef* tElem = taskTypeRef->genericArgs[0];
    // #2279: try the intrinsic-collection result path first. If T is a
    // recognized intrinsic collection, the per-collection-kind free thunk
    // frees it at task disposal (with the right per-element disposer
    // internally). Returns null when T is not an intrinsic collection, in
    // which case we fall through to the existing class-instance path.
    if (llvm::Function* collDisp = patternAResultCollectionDisposer(tElem))
        return collDisp;
    // fix #1889 element-half / #776 + #777 regression guard: a nested Task-
    // family result (`Task<Task<U>>`, `Task<OwningTask<U>>`) is BORROWED, not
    // OWNED — `Task<U>` does not own its U, so the outer `Task<Task<U>>`'s
    // fire-and-forget cascade MUST NOT dispose the inner task (it must
    // transfer the inner task pointer to the awaiter; disposing here is a
    // double-free / UAF when the awaiter subsequently awaits the inner task).
    // Before #1889 elementPolicyFor returned the Value row for a Task-family
    // tElem (no class declaration ⇒ falls through), so the disposer was null
    // and transfer worked. With #1889 the Task-family branch now classifies
    // the inner task ClassOwned and returns taskElementDisposerThunk — which
    // is correct for an OWNING COLLECTION element (#1889's main case) but
    // wrong here, where the outer Task's fire-and-forget is the call site.
    // Exclude nested Task-family results explicitly so the two policies stay
    // disjoint. // per §30.5 (Task<T> doesn't own its T)
    if (isTaskTypeName(tElem)) return nullptr;
    ElementPolicy pol = elementPolicyFor(tElem, /*owningFamily=*/true);
    return pol.cat == ElementPolicy::Category::ClassOwned ? pol.disposerThunk
                                                           : nullptr;
}


// fix #3096 / §30.5 "Pattern A result slots are of three kinds", kind 2 —
// *Chaining from a scope-bound source* (v0.252). Returns the `LocalDecl` of the
// task local a `return await <operand>` chains FROM, when this async body OWNS
// that local's task frame; null otherwise (including for every kind-3 BORROWED
// source, which transfers nothing).
//
// §30.5 kind 2: "When the source is a `using var`/`await using var` local —
// which §28.3 forbids moving — no move occurs: the source's slot already lives
// in the enclosing state-machine frame, and the chained-slot arrangement
// **defers its disposal** from the frame's scope end to the outer task's
// teardown … On both paths the source task is disposed exactly once — by the
// outer task's destructor, transitively."
//
// Two predicates are ASKED rather than re-derived, which is what keeps this from
// drifting away from the code it must agree with:
//
//  (1) `patternAResultDisposerFor` — the one function that decides "disposing
//      this task disposes its result" (#1503, and the gate #3070/#3099 use).
//      It is null for `Task<value/string/runtime-managed T>` (no borrow is
//      produced, so the source's ordinary scope-end disposal is already
//      correct) and for `OwningTask<T>` — Pattern B, where the await TAKES the
//      result and the outer task owns it directly (kind 1), so chaining the
//      source would be wrong. Restricting to this row keeps the change to
//      exactly the shape that has two owners today.
//
//  (2) `taskLocalOwnsFrame` (LlvmBackend_Setup.cpp) — THE owns-the-frame test,
//      and the same function BOTH `d.task` registration sites are gated on.
//      That sharing is the point, and it is a correction: #3096 first shipped
//      this arm reading `ld->isUsing` alone, which is NOT the registration
//      test, because registerUsingDisposal returns before registering anything
//      when `ld->usingVarSourceIsBorrow` is set. A §28.10 borrow PIN
//      (`using var t2 := task;`, or a task-typed field read) therefore looked
//      owned here while owning nothing, and the chain handed the outer task's
//      teardown a frame this body never held — a double free of the
//      `bz_task_t*` plus a second cascade through the #1503 result disposer.
//      Asking one function makes "the obligation moved is the obligation
//      registered" structural: a local with no `d.task` is a BORROW, it is
//      §30.5 kind 3, and its real owner must keep disposing it.
const ast::LocalDecl* Emitter::chainedReturnOwnedTaskSource(
    const ast::Expr& operand) {
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&operand);
    if (!id) return nullptr;
    auto* ls = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!ls) return nullptr;
    const auto* ld = dynamic_cast<const ast::LocalDecl*>(ls->decl);
    if (!ld) return nullptr;
    const sema::TypeRef* taskTy = taskTypeOfLocal(ld);
    if (!taskTy) return nullptr;
    if (!patternAResultDisposerFor(taskTy)) return nullptr;   // (1)
    return taskLocalOwnsFrame(ld) ? ld : nullptr;             // (2)
}
// fix #3126 (§30.5 "Pattern A result slots are of three kinds", kind 2 —
// *Chaining from a scope-bound source*, MOVE-OUT HALF, v0.254): the
// `ParameterSymbol` of the `owned` parameter a `return await <operand>` chains
// FROM. The sibling of `chainedReturnOwnedTaskSource` for parameters — a kind 2
// source is one or the other, never both, because an identifier resolves to
// exactly one symbol kind.
//
// §30.5 kind 2 (move-out half): "When the source of `return await <source>`
// is an `owned` parameter of the enclosing async method, the transfer into the
// chained slot is an ordinary §28.3 **move-out** of the parameter: it is
// thereby moved out, so §30.1's dispose-at-terminal-state rule for not-moved-
// out `owned` parameters does not apply to it — the outer task's teardown
// disposes the chained task instead."
//
// Two predicates are ASKED rather than re-derived, mirroring the local arm so
// the two agree:
//   (1) `taskFamilyTypeFromParam` + `patternAResultDisposerFor` — the same
//       Task-family + Pattern-A classification. A non-`Task` parameter, a
//       `Task<value/string/runtime-managed T>` parameter (no borrow), or an
//       `OwningTask<T>` parameter (Pattern B — await takes the result, kind 1
//       owns it) is NOT chainable.
//   (2) The parameter is an `owned` parameter (a borrow parameter would be
//       §30.5 kind 3, no transfer possible). The ownership pass sets
//       `iteratorAutoDispose` for every `owned` parameter whose body never
//       discharges it on any path; the chained-edge suppression below (via
//       `nullStateMachineOwnedParamCapture`) is the per-edge "moved-out"
//       flag — null-guarded in `emitAsyncOwnedParamAutoDispose` (#3125) —
//       that converts that undischarged flag into the §30.5 move-out half
//       specifically on this return edge, exactly as the local arm's nulled
//       local-slot does.
//
// THE DEFECT fixed here (#3126). Pre-fix the kind-2 move-out half was never
// detected for an `owned` parameter source — this function returned null — so
// `lowerStmt(ReturnStmt)` kept the kind-1 disposer attach path. The outer task
// then OWNED the borrowed result through BOTH the §30.1 auto-drop (which now
// correctly routes Task-family to `bz_task_free` after #3125, freeing the
// chained Cell) AND the kind-1 per-`T` disposer (attached to the outer task
// at the same return site), so the outer task's destructor freed the SAME Cell
// a second time — measured on the §30.5 kind 2 repro as
// `heap-use-after-free in bz$async$Main$Resume`, freed by the #1503 per-T
// disposer. After #3125's routing fix removed the wild-jump, the SEGV went
// away but the semantic double-owner remained; this fix removes the second
// owner by chaining the parameter (kind 2) and nulling its frame slot, so the
// outer task owns the source task transitively and the §30.1 auto-drop no
// longer fires for this parameter on this edge.
//
// Returns null for every kind-3 BORROWED source (a plain non-`owned`
// parameter — its frame slot is never in this auto-drop set, and chaining it
// would dispose a frame the body never owned); this is recorded and not a
// defect. // per §30.5 kind 2, v0.254
sema::ParameterSymbol* Emitter::chainedReturnOwnedTaskParamSource(
    const ast::Expr& operand) {
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&operand);
    if (!id) return nullptr;
    auto* ps = dynamic_cast<sema::ParameterSymbol*>(ctx_.exprSymbol(*id));
    if (!ps || ps->modifier != "owned") return nullptr;
    const sema::TypeRef* taskTy = taskFamilyTypeFromParam(ps);
    if (!taskTy) return nullptr;
    if (!patternAResultDisposerFor(taskTy)) return nullptr;   // (1)
    return ps;
}
// Not admitted here:
//   - every kind-3 BORROWED source (a plain borrow parameter, a `using var` pin,
//     a field read, a static task field, etc.) — fixed by `chainedReturnBorrowed
//     TaskSource` below (#3125), a sibling predicate that admits every Pattern A
//     source this body does NOT own.
//   - an `owned` PARAMETER source — fixed by `chainedReturnOwnedTaskParamSource`
//     (#3126), a sibling predicate that admits exactly that row and suppresses
//     the §30.1 frame auto-drop on the chained edge.
//
// fix #3125: §30.5 Pattern A **kind 3** — `return await <borrowed source>`.
//
//   "When the source is a **borrow** — an admitted static task field or
//    property, an `owned` field of `this` read without `move`, an owning-
//    collection slot, a borrowing-iteration variable, or any other admitted
//    borrowed operand — no ownership transfers: the outer task's result
//    slot records a **borrow through the source task**, and the outer task's
//    teardown disposes **nothing** on that link."
//
// Companion to chainedReturnOwnedTaskSource (kind 2): that one returns non-null
// only when the body OWNS the source's frame; this one returns true only when
// the body has a BORROW of a Pattern A source — i.e. the operand's static type
// is a `Task<T:class>` the body does not own. Mutual exclusion is what makes
// the same return-statement site able to call exactly one of three arms
// (kind 1 disposer attach / kind 2 chain / kind 3 dispose-nothing) without
// any overlap.
//
// Both predicates MUST agree on what "Pattern A" means, so they share the
// single source of truth — patternAResultDisposerFor. Asking it directly is
// what keeps this from drifting away from chainedReturnOwnedTaskSource's
// decision (which itself asks it the same way). The two complementary
// predicates therefore cover exactly the rows where an `await` inside a
// `Task<T:class>`-returning async method can appear:
//
//   chainedReturnOwnedTaskSource:   CallExpr (anonymous producer)        → kind 2
//                                  IdentifierExpr + owns-the-frame local → kind 2
//                                  (Owned parameter source: still unfixed #3126/#3157.)
//
//   chainedReturnBorrowedTaskSource: anything else whose type is Pattern A → kind 3
//   (this function)
//
// Everything else (Task<value/string/runtime-managed> T, or OwningTask<T>):
// the `else if (asyncPatternAResultDisposer_)` arm in
// emitAsyncPatternAResultDecision is null, so no kind-1 disposer attaches —
// either because there is no result to dispose (value/string/runtime-managed)
// or because the await itself takes ownership (OwningTask<T>, Pattern B).
//
// Note the deliberate asymmetry: chainedReturnOwnedTaskSource returns the
// `LocalDecl*` because chainedReturnOwnedTaskSource's caller uses it to NULL
// the source's slot at scope-end deferral (Stmts.cpp ~2075). This function
// only needs a yes/no — the kind-3 path emits nothing on the outer task at
// all, so there is no source-slot to coordinate with.
bool Emitter::chainedReturnBorrowedTaskSource(const ast::Expr& operand) {
    const sema::TypeRef* ty = ctx_.exprType(operand);
    if (!patternAResultDisposerFor(ty)) return false;  // not Pattern A
    // Anonymous producer — kind 2 chained (handled by chainedReturnOwnedTaskSource).
    if (dynamic_cast<const ast::CallExpr*>(&operand)) return false;
    // Owned named local — kind 2 chained (handled by chainedReturnOwnedTaskSource).
    if (chainedReturnOwnedTaskSource(operand)) return false;
    // Everything else whose type is Pattern A is a borrow: a plain (non-owned)
    // parameter, an `owned` field of `this` read without `move`, a borrow
    // field, an owning-collection slot, a borrowing-iteration variable, an
    // admitted static task field/property, a §28.10 borrow PIN
    // (`using var t2 := task;` — registered no `d.task`, so this body never
    // owned the frame), or any other admitted borrowed operand.
    return true;
}


void Emitter::emitAsyncMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                              const CallableInfo& ci) {
    if (!ci.stmts) return;
    // fix #2895 (§29.3 / §30.1 / §30.7, v0.240): retract the entire
    // suspend/resume frame on every exit (including the diagnostic early
    // returns below). Replaces the partial clear inside the Resume body
    // block, which left smStateGEP_/smKind_/smStateBBs_/smDoneBB_/
    // smNextSuspendState_/smForInSrcFields_ live for the next function.
    StateMachineFrameScope smFrameScope(*this);
    smKind_ = SuspendKind::Await;

    // §30.9 / §31.2 (#438): `await` is now supported in a try BODY, a catch ARM
    // BODY, and a FINALLY (lowerTry lowers each handler's resume block in place
    // and frame-backs the handler-context allocas + the in-flight exception so
    // they survive a cross-thread suspend). The ONE remaining deferred position is
    // an `await` in a catch GUARD (`catch E when await ...`): the guard runs
    // during catch DISPATCH, before the arm scope / caught-exception state exist,
    // so resuming there would have to re-run the dispatch sequence. Reject that
    // sub-case with a clear diagnostic + safe stub so no malformed IR escapes.
    if (awaitInDeferredTryPosition(*ci.stmts)) {
        ctx_.diagnostics().report(
            Severity::Error,
            ms->decl ? ms->decl->span.begin : SourceLocation{},
            "not yet implemented: `await` inside a catch GUARD (`catch ... when "
            "await ...`) — the guard runs during catch dispatch, before the "
            "handler scope exists, so resuming there is a deferred increment "
            "(`await` in a try BODY, a catch arm body, or a `finally` is "
            "supported) (§30.9)");
        // Emit a minimal well-formed body so no malformed IR escapes: alloc the
        // task, complete it with a zero/default result, and return it (or the
        // exit code for async Main). The diagnostic above fails the compile.
        emitAsyncStub(ms, fn, ci);
        return;
    }

    // Determine the result element type T from the return type's generic arg.
    //   Task           → no result value (asyncResultTy_ = null)
    //   Task<T>        → T
    //   OwningTask<T>   → T (Pattern B; ownership transfer is the awaiter's job)
    llvm::Type* resultTy = nullptr;
    const sema::TypeRef* resultSemaTy = nullptr;
    const sema::TypeRef* declaredTaskTy = nullptr;
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        declaredTaskTy = retRef;
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0]) {
            resultSemaTy = retRef->genericArgs[0];
            resultTy = lowerTypeRef(resultSemaTy);
        }
    }
    asyncResultTy_ = resultTy;
    // §30.1 / §7.2 (#493): keep the sema TypeRef for T so emitAsyncComplete
    // can detect a nullable value type and wrap the returned plain value.
    asyncResultSemaType_ = resultSemaTy;
    // §30.5 / #1503: Pattern A (Task<T:class>) always owns its produced
    // result directly; compute the per-T disposer once here from the
    // declared return type. lowerStmt(ReturnStmt) attaches it to the outer
    // task at each non-chained `return <owned>` site. Null for `Task`,
    // `Task<non-class T>`, and `OwningTask<T>` (Pattern B).
    asyncPatternAResultDisposer_ = patternAResultDisposerFor(declaredTaskTy);

    // Build the kind-agnostic frame with three trailing async fields:
    //   [.. ] outerTask (ptr), [.. ] pendingTask (ptr), [.. ] spillArea (ptr).
    // Layout: { ptr vtable, i32 state, captures..., locals...,
    //           ptr outer, ptr pending, ptr spill }
    // §30.2 / §30.13 (#439-followup): `spill` points at a SEPARATELY-calloc'd
    // heap region holding the anonymous cross-suspend temporaries that
    // fixupSuspendDominance would otherwise leave in function-local stack slots
    // (e.g. the pre-suspend operand of `total + await f()`). Those slots are
    // garbage after a real cross-thread resume (the worker has a fresh stack), so
    // such temporaries must round-trip the HEAP frame like named locals do. The
    // spill area is appended as a side region (rather than grown into this struct,
    // whose body is already finalized) and pointed-to from a frame field so the
    // single `state` pointer that bz_task_then carries across the thread boundary
    // reaches it. It is lazily calloc'd on the first resume entry (see
    // fixupSuspendDominance) and stays null when the body has no such temporary.
    StateMachineFrame frame = buildStateMachineFrame(
        ms, *ci.stmts, {ptrTy(), ptrTy(), ptrTy()},
        std::string("async.") + std::string(ms->name));
    auto* stateStructTy = frame.structTy;
    const unsigned STATE_FIELD = frame.stateField;
    const unsigned OUTER_TASK_FIELD = frame.currentValField;        // first trailing
    const unsigned PENDING_TASK_FIELD = frame.currentValField + 1;  // second trailing
    const unsigned SPILL_PTR_FIELD = frame.currentValField + 2;     // third trailing
    asyncOuterTaskField_ = OUTER_TASK_FIELD;
    asyncPendingTaskField_ = PENDING_TASK_FIELD;
    asyncSpillPtrField_ = SPILL_PTR_FIELD;
    // §30.1 (#1521): publish this frame so emitAsyncComplete (every normal
    // completion site) and the async.fault boundary below can dispose any
    // `owned`/move-only-value parameter Ownership.cpp flagged
    // iteratorAutoDispose (never discharged on any path) — the frame-transfer
    // + terminal-state auto-dispose §30.1 requires. Cleared at the end of
    // this function.
    asyncOwnedParamFrame_ = &frame;

    bool isInstance = !isStaticMethod(ms);
    std::string prefix = std::string("bz$async$") + std::string(ms->name) + "$";

    // --- Resume(ptr completedTask, ptr frame) -> void ---
    // The bz_task_then continuation ABI is void(void* task, void* state); the
    // frame is the state.
    auto* resumeTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false);
    auto* resumeFn = llvm::Function::Create(
        resumeTy, llvm::Function::InternalLinkage, prefix + "Resume",
        module_.get());
    asyncResumeFn_ = resumeFn;

    // --- ENTRY (= the original method body): alloc task + frame, drive once ---
    {
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        pendingArgsDisposal_.reset();
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
        builder_.SetInsertPoint(entry);
        // fix #633 / §33.4: async Main(args: Array<string>) has C ABI signature
        // `i32(i32 argc, ptr argv)`.  Build Array<string> before spilling params
        // into the heap frame so the frame capture gets the real pointer.
        if (isMain(ms) && !ms->params.empty()) {
            auto argIt = fn->args().begin();
            llvm::Value* argc = &*argIt++;
            llvm::Value* argv = &*argIt;
            auto* bz_args_fn = declareC("bz_args_array", ptrTy(), {i32(), ptrTy()});
            llvm::AllocaInst* argsSlot = entryAlloca(ptrTy());
            builder_.CreateStore(
                builder_.CreateCall(bz_args_fn, {argc, argv}, "main.args"),
                argsSlot);
            paramSlots_[ms->params[0]] = argsSlot;
            pendingArgsDisposal_ = Disposal{argsSlot, nullptr};
            pendingArgsDisposal_->collection = true;
            pendingArgsDisposal_->collectionElemDisposer = stringElementReleaseThunk();
        } else {
            bindParameters(fn, isInstance, &ms->params);
        }

        // outerTask = bz_task_alloc()
        auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
        llvm::Value* outerTask = builder_.CreateCall(allocFn, {}, "async.task");

        // frame = calloc(1, sizeof(Frame))
        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* st = builder_.CreateCall(
            callocFn2, {builder_.getInt64(1), builder_.getInt64(stSz)},
            "async.frame");

        // state = 0
        llvm::Value* stateField =
            builder_.CreateStructGEP(stateStructTy, st, STATE_FIELD);
        builder_.CreateStore(builder_.getInt32(0), stateField);
        // outerTask field
        llvm::Value* otField =
            builder_.CreateStructGEP(stateStructTy, st, OUTER_TASK_FIELD);
        builder_.CreateStore(outerTask, otField);
        // captures (this + params)
        emitFrameCaptureStores(frame, st);

        // §22.7 / §22.13 / §24.1 (#1502): run the entry-point static-init
        // prologue BEFORE driving the state machine. emitFunctionBody's async
        // routing returns before its own inline prologue site, so without this
        // an async Main never allocated the static event sources (nor ran the
        // SerializationException vtable init / non-lazy static ctors) — a
        // subscribe/raise on a static event inside an async method reached a
        // null source and silently dropped every handler. currentFn_ is the C
        // `main` here, whose propagateBB routes an uncaught static-init fault to
        // bz_unhandled_exception (§33.3), exactly as in the synchronous path.
        if (isMain(ms))
            emitMainStaticInitPrologue();
        // §22.13: a static async method's declaring type initializes before its
        // body runs; the body lowers in Resume, so trigger the lazy ensureinit
        // here in the entry (the synchronous path does this at method-body entry
        // for every static method — emitFunctionBody, `emitEnsureStaticInit`).
        if (isStaticMethod(ms) && ms->parent)
            emitEnsureStaticInit(ms->parent);

        // §30.13 (#439): start the runtime thread pool before driving an async
        // Main. The pool's worker threads run the continuations of genuinely-
        // incomplete tasks (Task.Delay, and any task whose producer suspends),
        // resuming the state machine across the thread boundary. Starting it in
        // the Main prologue (a single program-wide root, §22.7 / §33.2) means the
        // pool is up before any await that might suspend. A purely synchronous /
        // already-done body never touches the pool (each await hits the fast path
        // and runs inline), so the threads simply idle until shutdown — but
        // starting them is cheap and keeps the cross-thread path correct. For a
        // non-Main async method the pool is initialized lazily by the runtime
        // (bz_task_delay / bz_task_schedule) on first use. bz_scheduler_init is
        // idempotent (a no-op if already started), so a lazy init that already ran
        // is harmless.
        if (isMain(ms)) {
            auto* schedInitFn = declareC("bz_scheduler_init",
                                         llvm::Type::getVoidTy(llvm_), {i32()});
            // A small fixed pool (4 workers) — enough to run nested/concurrent
            // continuations without unbounded thread growth (§30.13).
            builder_.CreateCall(schedInitFn, {builder_.getInt32(4)});
        }

        // Drive the state machine once synchronously (§30.1 synchronous prefix).
        builder_.CreateCall(resumeFn, {llvm::ConstantPointerNull::get(ptrTy()), st});

        if (isMain(ms)) {
            // §33.2: async Main. The C entry `main` returns i32 — block for the
            // task's completion (bz_task_await_sync; already-done returns
            // immediately) and use its result as the process exit code. A
            // `Task<int>` yields the int; a bare `Task` yields 0 (§33.3 normal
            // completion).
            auto* awaitFn = declareC("bz_task_await_sync", ptrTy(), {ptrTy()});
            llvm::Value* resPtr =
                builder_.CreateCall(awaitFn, {outerTask}, "main.result.ptr");
            // §33.3 / §30.9: a faulted Main task is an uncaught exception at the
            // process boundary — surface it (stderr + non-zero exit) rather than
            // dereferencing the null result buffer.
            auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
            llvm::Value* mainErr =
                builder_.CreateCall(isErrFn, {outerTask}, "main.faulted");
            auto* errBB = llvm::BasicBlock::Create(llvm_, "main.fault", fn);
            auto* okBB = llvm::BasicBlock::Create(llvm_, "main.ok", fn);
            builder_.CreateCondBr(mainErr, errBB, okBB);
            builder_.SetInsertPoint(errBB);
            // §33.3 / §30.9 (fix #1609, superseding #1366): report the REAL fault
            // exception at the async-Main no-handler boundary, matching the
            // synchronous path (LlvmBackend_Disposal.cpp propagateBB). The outer
            // task carries the fault exception OBJECT in its error_obj slot — the
            // async.fault boundary stored it there via bz_task_complete_error_obj
            // (from the body's in-flight bz$exc, itself an awaited task's re-raised
            // error_obj or a synchronous throw). It is STILL LIVE here: bz_task_free
            // frees only result/error_msg, never error_obj (§30.9 fault ownership),
            // and an UNCAUGHT async fault has no catch arm to run the deferred
            // dispose — so the object survives, un-disposed, to this process-exit
            // boundary exactly as the synchronous in-flight exception does. Passing
            // it (with the built-in exception vtable table, so §31.4 field reads are
            // gated to that layout) makes the runtime print the §33.3 type / message
            // / CauseChain instead of the previous bare "Unhandled exception" header.
            // #1366's earlier "not live here" note was overcautious — no fault path
            // between async.fault and this boundary disposes error_obj.
            auto* errObjFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
            llvm::Value* faultObj =
                builder_.CreateCall(errObjFn, {outerTask}, "main.fault.exc");
            auto* unhandledFn = declareC("bz_unhandled_exception",
                                         llvm::Type::getVoidTy(llvm_),
                                         {ptrTy(), ptrTy()});
            unhandledFn->setDoesNotReturn();
            builder_.CreateCall(unhandledFn,
                {faultObj, builtinExcVtableTablePtr()});
            builder_.CreateUnreachable();
            builder_.SetInsertPoint(okBB);
            llvm::Value* res = nullptr;
            if (resultTy && resultTy->isIntegerTy()) {
                res = builder_.CreateLoad(resultTy, resPtr, "main.result");
                if (resultTy != i32()) res = builder_.CreateTruncOrBitCast(res, i32());
            }
            // §30.13 (#439): shut the thread pool down cleanly before Main
            // returns so the process exits without hung worker threads (and
            // leak-clean under LSan: the pool's threads + queue are joined and
            // freed). This runs AFTER bz_task_await_sync(outerTask) above, so the
            // outer task is already complete — every continuation has either run
            // or is the one that completed it; shutdown drains the queue and joins
            // each worker, so no continuation is still in flight when we free the
            // task below. bz_scheduler_shutdown is a no-op if the pool was never
            // started (a purely synchronous body that lazily-init'd nothing).
            auto* schedShutdownFn = declareC("bz_scheduler_shutdown",
                                             llvm::Type::getVoidTy(llvm_), {});
            builder_.CreateCall(schedShutdownFn, {});
            // §30.5 (minimal): reclaim the outer task (its result buffer) on the
            // normal completion path so the process is leak-clean. The frame was
            // already freed by the completing resume (emitAsyncComplete). Safe
            // here: the outer task is complete and the pool is shut down, so no
            // continuation is still in flight referencing the task.
            auto* freeTaskFn = declareC("bz_task_free", llvm::Type::getVoidTy(llvm_),
                                        {ptrTy()});
            builder_.CreateCall(freeTaskFn, {outerTask});
            builder_.CreateRet(res ? res : builder_.getInt32(0));
        } else {
            // Return the outer task pointer (the method's Task / Task<T> result).
            builder_.CreateRet(outerTask);
        }
    }

    // --- Resume: the state machine body ---
    {
        currentFn_ = resumeFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        // #422 / §27.5 / §28.9 (#488 follow): a `var` CLOSURE local in an async
        // method body — e.g. the canonical async-lambda form `var f := async …`
        // (§30.6) — must be released at scope end, exactly like in a synchronous
        // method. The async routing in emitFunctionBody returns BEFORE that pass's
        // collectClosureReleaseEligible / eligible-fn setup runs, so without this
        // the closure cell (+ its env block) leaks. Compute eligibility for the
        // async body here and key it to the resume fn (where the body lowers); the
        // release registers in the resume's disposeStack_ and emitAllDisposals
        // (run on every async `return` / fall-off before emitAsyncComplete) frees
        // it.
        collectClosureReleaseEligible(*ci.stmts);
        closureReleaseEligibleFn_ = resumeFn;
        // fix #1896 / §28.3 "Exception-path discharge" / §30.1: compute the
        // unwind-disposal eligibility of the async body's plain owned locals and
        // key it to the RESUME fn (where the body lowers). registerUnwindOwnedLocal
        // gates on `currentFn_ == unwindIneligibleFn_` (so a body whose moves/
        // disposes were not analyzed cannot register a double-dispose); without
        // this the resume body's currentFn_ (resumeFn) never matched the stale
        // unwindIneligibleFn_ left by the last synchronous method, so EVERY plain
        // `owned` local in an async body was silently skipped — never registered
        // for the exception-path (`unwindOnly`) disposal — and leaked when an
        // operation threw before the local's normal-path discharge (the #1896
        // leak). emitThrowValue / emitPendingCheck already emit emitUnwindDisposals
        // on the throw edge; they simply had nothing registered to dispose. Mirror
        // the synchronous emitFunctionBody setup (Setup.cpp ~3318). The unwindOnly
        // disposal is skipped on the normal edge (§28.3: normal-exit discharge is
        // sema-enforced), so no double-free on the non-throwing path. // per §28.3
        unwindIneligibleFn_ = resumeFn;
        collectUnwindIneligible(*ci.stmts);
        // #1416 / #1418 / §27.5 / §28.9: the CAPTURE CELL of a frame-local that a
        // lambda captures (the `calloc` from boxLocalSlot) is freed at scope end by
        // the same boxedCellFree / deferred-cell-free machinery as in a synchronous
        // method — but ONLY if its cell-free-eligibility pre-scans run and are keyed
        // to the resume fn (where the body lowers). emitFunctionBody's async routing
        // returned before Setup's collectBoxCaptureLocals / collectCellFreeEligible
        // ran, so previously an async method leaked every captured local's cell (a
        // plain capture cell for a value local — #1418 — or the 8-byte cell holding
        // a captured `using var`/event-subscription closure's target — #1416).
        // Mirror the synchronous setup here so lowerLambda records the cells in
        // lastLambdaCapturedCells_ and lowerLocalDecl registers their frees.
        boxCaptureLocals_.clear();
        singleCaptureLocals_.clear();
        assignedCaptureVars_.clear();  // fix #2706
        definitelyEscapingLambdas_.clear();
        boxCaptureLocalsFn_ = resumeFn;
        cellFreeEligible_.clear();
        unwindUnsafeCellFree_.clear();  // fix #2334
        sharedCellEnvdtorFreeOwner_.clear();  // fix #3426
        cellFreeEligibleFn_ = resumeFn;
        collectBoxCaptureLocals(*ci.stmts);
        collectDefinitelyEscapingLambdas(*ci.stmts);
        // Must run AFTER collectClosureReleaseEligible (uses its result).
        collectCellFreeEligible(*ci.stmts);
        // fix #2132 / #2142 / #2143 / #1871 / #1003 / #1028 / #1930 / §28.9 /
        // §28.11: the scope-end reclaim of a plain `var` borrowing-`Array<T>` /
        // `Set<T>` / `Dictionary<K,V>` LITERAL (its backing is "reclaimed with
        // the instance"), of a `var` fresh borrowing-array / dict / grouping
        // MATERIALIZER, must be re-established for the RESUME fn too — the
        // registration sites in lowerLocalDecl gate on `currentFn_ ==
        // borrowingArrayFreeEligibleFn_` (and the dict/grouping/set-literal/
        // dict-literal analogues), but the async body lowers with currentFn_ ==
        // resumeFn while emitFunctionBody pinned these to the ENTRY fn
        // (Setup.cpp), so every such reclaim was silently skipped and the
        // backing leaked across the async return edge (the #2132 drop-after-
        // await leak: `var xs := [ … ]; await …;` — the array is spilled/
        // restored fine now, but never freed; #2142/#2143 found the Set/Dict
        // literal shapes leaked in BOTH sync and async — they were never
        // registered in ANY eligibility set, entry or resume). One call
        // populates all five sets; re-pin all five fn keys to resumeFn. Mirrors
        // the closure/unwind/cell re-establishment above. // per §28.9, §28.11
        collectBorrowingArrayFreeEligible(*ci.stmts);
        borrowingArrayFreeEligibleFn_ = resumeFn;
        dictMaterializerFreeEligibleFn_ = resumeFn;
        groupingFreeEligibleFn_ = resumeFn;
        setLiteralFreeEligibleFn_ = resumeFn;   // #2142
        dictLiteralFreeEligibleFn_ = resumeFn;  // #2143
        // fix #2807 / #429 / §4.6 / §28.9 / §28.3 (v0.240): the `var` STRING-
        // local refcount analysis must describe the RESUME fn too. EVERY
        // consumer of the three string sets gates on `currentFn_ ==
        // stringReleaseEligibleFn_` — armStringLocalRefcount's bind-time retain
        // and its scope-end `Disposal{stringRelease}`, returnedStringLocalOwns
        // PlusOne's transfer-on-return, the two record-backed interface-property
        // retains (LlvmBackend_Assign.cpp / LlvmBackend_Exprs.cpp), and the
        // `:=` / §4.6 `+=` drop-on-overwrite release — and emitFunctionBody's
        // async routing returns BEFORE Setup's collectStringReleaseEligible
        // runs. The async body therefore lowered with currentFn_ == resumeFn
        // while the token still named the last SYNCHRONOUS method emitted, so
        // NO string local in ANY async body was ever retained OR released:
        // both allocations (bz_string header + payload) leaked, per local, per
        // call (#2807 — measured 1904 bytes / 80 allocations for 20 calls x 2
        // locals). Mirrors the closure / unwind / cell / borrowing-array
        // re-establishment above.
        //
        // The frame-spill dimension needs no extra machinery: collectSuspend
        // Locals (LlvmBackend_Iterators.cpp) gives EVERY LocalDecl in the body
        // a frame field, and emitAwaitSuspend spills / restores every mapped
        // alloca around each suspend, so the alloca the Disposal loads holds
        // the live value on whichever activation reaches the exit. The release
        // itself runs on exactly one edge per activation — emitAllDisposals on
        // an explicit `return`, emitDisposals on the fall-off, emitUnwind
        // Disposals on the throw edge — and a SUSPEND returns from resume
        // without touching disposeStack_, so a suspended local is released once,
        // by the activation that finally completes. // per §28.9, §30.2
        collectStringReleaseEligible(*ci.stmts);
        stringReleaseEligibleFn_ = resumeFn;
        // §30.9 / §31.2 (#438): start with no pending handler-context spill slots;
        // lowerTry registers them as it lowers any in-handler await.
        asyncHandlerSpillSlots_.clear();
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", resumeFn);
        builder_.SetInsertPoint(entry);

        // arg0 = completedTask (the task that fired this resume; unused on the
        //        initial drive and read via the frame's pendingTask slot at the
        //        await site instead), arg1 = frame.
        llvm::Value* self = resumeFn->getArg(1);

        // Restore captured params (this + params) from the frame.
        emitFrameCaptureRestores(frame, self);

        // fix #3466 (§27.5 / §30.1, v0.283): publish the per-parameter capture-
        // CELL fields for the fn the body actually lowers into (the resume, not
        // the entry). lowerLambda reads this when it boxes a captured parameter
        // and maps the fresh cell-HOLDER alloca to its field, so the existing
        // emitAwaitSuspend spill/restore carries the holder across every
        // suspension lowered after the capture site. Keyed by function because
        // paramSlots_/locals_ are rebuilt per emitted function and a nested
        // emission must not inherit this frame's field indices.
        smParamCellField_ = frame.paramCellFields;
        smParamCellFieldFn_ = resumeFn;

        // fix #2807 / fix #2696 / §28.1 / §28.9 / §4.6 (v0.240): CO-GATE the
        // parameter retain with the parameter release the pin above just turned
        // on. lowerStringLocalReassign (and the §4.6 `+=` branch's `isLocal`)
        // admit ANY slot localSlot() resolves — which includes a `string`
        // PARAMETER, whose single reference belongs to the CALLER (§28.1: on a
        // `string`, "`owned` is a compile error: such values carry no disposal
        // obligation to transfer (§28.9)"). Enabling that release without the
        // paired bind-time retain frees the caller's borrow under it — exactly
        // the #2696 use-after-free — so the two predicates must move together,
        // which is the invariant retainAssignedStringParams' step 0 states.
        //
        // Emitted in the resume ENTRY block, which dominates every assignment
        // and re-runs on every activation, while a given activation can release
        // the incoming value at most once (after the first assignment the slot
        // holds a value this frame owns). So retains >= releases on every path:
        // a surplus retain leaks one reference per resume, it can never
        // over-release. (An async body's param slot is re-restored from the
        // frame capture on each activation and is NOT spilled back, so an
        // assignment to a `string` param does not survive a suspend — a
        // separate, pre-existing async defect this fix neither causes nor
        // fixes; the retain-per-activation is what keeps it leak-safe.)
        retainAssignedStringParams(*ci.stmts, ms->params);

        // State field GEP — read by the resume dispatch, written at each await.
        llvm::Value* stateGEP =
            builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);

        unsigned numAwaits = countAwaits(*ci.stmts);

        // Emit the resume switch + state blocks. The async "done" / fall-off
        // return completes the outer task with no value (§30.1: falling off a
        // Task-returning async method == `return;`) and returns from resume.
        std::vector<llvm::BasicBlock*> stateBBs = emitResumeDispatch(
            frame, self, stateGEP, numAwaits,
            [&]() {
                emitAsyncComplete(/*value=*/nullptr);
                builder_.CreateRetVoid();
            });

        // §30.9 (basic): the async fault boundary. An uncaught throw in the
        // async body — a synchronous `throw`, or the await-faulted re-raise —
        // branches here (it is the resume function's default unwindTarget_). It
        // completes the OUTER task with an error so the awaiter observes the
        // fault, then returns from resume. A try/catch in the body installs its
        // own nearer unwindTarget_ via lowerTry, so handled throws never reach
        // here; only genuinely-uncaught faults do.
        //
        // §30.9: the in-flight exception object lives in the bz$exc global at this
        // point (a synchronous `throw` stored it there before branching to the
        // unwind target; a faulted-await re-raise — lowerAwait below — likewise
        // restores the awaited task's exception object into bz$exc before
        // branching here when no nearer handler exists). Carry that ORIGINAL
        // object through the outer task (bz_task_complete_error_obj) so the next
        // awaiter — including one inside its own try/catch — re-raises the real
        // exception type, not a reconstructed message. If bz$exc happens to be
        // null (a non-object fault), the awaiter's await.fault path falls back to
        // completing with no object (the unhandled boundary still exits non-zero).
        auto* faultBB = llvm::BasicBlock::Create(llvm_, "async.fault", resumeFn);
        unwindTarget_ = faultBB;
        {
            llvm::IRBuilder<>::InsertPointGuard guard(builder_);
            builder_.SetInsertPoint(faultBB);
            llvm::Value* otField = builder_.CreateStructGEP(
                stateStructTy, self, OUTER_TASK_FIELD);
            llvm::Value* outerTask = builder_.CreateLoad(ptrTy(), otField);
            llvm::Value* excObj =
                builder_.CreateLoad(ptrTy(), emitExcSlot(), "async.fault.exc");
            auto* completeErrObjFn = declareC("bz_task_complete_error_obj",
                                              llvm::Type::getVoidTy(llvm_),
                                              {ptrTy(), ptrTy()});
            builder_.CreateCall(completeErrObjFn, {outerTask, excObj});
            // The exception object's ownership transfers to the outer task's
            // awaiter (or the unhandled boundary); clear bz$exc so it is not
            // double-handled if this resume activation continues elsewhere.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 emitExcSlot());
            // §30.1 (#1521): the fault/cancellation completion edge — dispose any
            // never-discharged `owned` parameter before the frame is freed, exactly
            // as the success edge (emitAsyncComplete) does. Runs BEFORE the frame
            // free below so it can still read/GEP through `self`.
            emitAsyncOwnedParamAutoDispose(self);
            // §30.5: the frame is dead on this terminal (fault) path — free it.
            // §30.13 (#439-followup): free the cross-suspend spill area first (if
            // any) — it is a side allocation pointed to from the frame.
            emitFreeSpillArea(self);
            auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn2, {self});
            builder_.CreateRetVoid();
        }

        // Emit Resume body starting from state 0. The await suspend points and
        // ReturnStmt handlers (gated on smKind_ == Await) interrupt it to emit
        // the suspend transitions / task completion.
        builder_.SetInsertPoint(stateBBs[0]);
        disposeStack_.emplace_back();
        bool term = lowerBody(*ci.stmts);
        // §28.3 / §30.1 (#938): on NORMAL fall-off, dispose the async body's
        // method-level scope's owned locals (`using var` / owned) BEFORE
        // completing the task. Previously pop_back() discarded the scope's
        // registered disposals without emitting them, so a method-level
        // `using var c := new Cell()` (with no nested block, await, or explicit
        // return) leaked — only nested `do…end` scopes, which run their own
        // emitDisposals, were freed. A body that terminated (an explicit
        // `return`, which runs emitAllDisposals via the return handler) already
        // disposed them, so this is gated on the un-terminated fall-off path.
        if (!term && !builder_.GetInsertBlock()->getTerminator())
            emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
        // If the body fell through without a terminator, complete with no value
        // and return (§30.1 fall-off == return;).
        if (!term && !builder_.GetInsertBlock()->getTerminator()) {
            emitAsyncComplete(/*value=*/nullptr);
            builder_.CreateRetVoid();
        }

        // §28.12 / §30 (`await using` state-block cleanup): any `await using var`
        // whose DisposeAsync was not found at registration time (sync fallback;
        // disposeAsyncFn == null) did NOT consume its pre-allocated state block.
        // countAwaits counted each `isAwaitUsing` as +1, so smStateBBs_ may have
        // blocks at indices > smNextSuspendState_ that are unterminated. Terminate
        // each with `br smDoneBB_` so the function is IR-valid (an unterminated
        // block fails LLVM's IR verifier). These blocks are dead code — the switch
        // has a case for each state, but the state is never stored in the frame
        // (the sync-fallback path never called bz_task_then with that state), so
        // the block is unreachable at runtime. `br smDoneBB_` makes it well-formed.
        if (smDoneBB_) {
            for (unsigned s = smNextSuspendState_ + 1; s < smStateBBs_.size(); ++s) {
                auto* orphanBB = smStateBBs_[s];
                if (orphanBB && !orphanBB->getTerminator()) {
                    llvm::IRBuilder<> tmp(orphanBB);
                    tmp.CreateBr(smDoneBB_);
                }
            }
        }

        // §30.2 (#438): repair SSA dominance broken by awaits embedded in larger
        // expressions (e.g. the loop-carried `total := total + await f(i)`): a
        // temporary computed before the suspend but used in the resume block,
        // which the dispatch switch / loop back-edge re-enters. Named locals
        // already round-trip through the frame; this demotes the remaining
        // anonymous temporaries to stack slots so the resume function verifies.
        fixupSuspendDominance(resumeFn);

        // Clear state-machine state.
        smStateStruct_ = nullptr;
        smSelf_ = nullptr;
        smLocalFields_.clear();
        smAllocaToField_.clear();
        asyncResumeFn_ = nullptr;
        asyncResultTy_ = nullptr;
        asyncResultSemaType_ = nullptr;
        asyncPatternAResultDisposer_ = nullptr;
        // Clear the published owned-param frame ref so it does not dangle after
        // emitAsyncMethod returns — asyncOwnedParamFrame_ must be null outside an
        // emitting async method (the fault-boundary handler null-guards it; §30.1).
        // #1521.
        asyncOwnedParamFrame_ = nullptr;
    }
}


// §30.6 (#488): async-lambda state machine. See the impl-header doc comment.
//
// This is the LAMBDA analogue of emitAsyncMethod. The structural difference is
// the capture set: a named async method captures `this` + its method params; an
// async lambda captures its OWN user params (arriving as the entry fn's args
// 1..N, already bound into paramSlots_ by lowerLambda's prologue) PLUS the
// outer-scope variables it closes over (arriving via the env pointer, already
// bound into paramSlots_/locals_ as shared-cell holders — cellVars_ marks those
// slots) PLUS `this` (when the lambda was created inside an instance method).
// All of these are live across the lambda body's awaits, so all must round-trip
// the heap frame, exactly like a method's captures do. Everything else — the
// await suspend lowering, ReturnStmt completion, the fault boundary, the
// suspend-local spill/restore — is the kind-agnostic scaffolding, driven by the
// sm*_/async*Field_ state, identical to the method path.
void Emitter::emitAsyncLambda(const ast::LambdaExpr& lam, llvm::Function* fn,
                              const std::vector<sema::Symbol*>* paramSyms,
                              const sema::TypeRef* resultElemSemaTy,
                              const sema::TypeRef* declaredTaskTy) {
    // `fn` is the closure entry; the builder is already positioned at its entry
    // block (lowerLambda's prologue), and currentFn_ == fn. We drive the entry
    // through builder_ / currentFn_, so `fn` itself is referenced only for the
    // contract (kept as a parameter for clarity / future use).
    (void)fn;
    // fix #2895 (§30.6 / §30.1, v0.240): retract the entire suspend/resume
    // frame at every exit (the partial clear at ~1977 left smKind_/smStateGEP_/
    // smStateBBs_/smDoneBB_/smNextSuspendState_/smForInSrcFields_/asyncOuter/
    // asyncPending/asyncSpillPtrField_ live for the next function compiled
    // into the module).
    StateMachineFrameScope smFrameScope(*this);
    smKind_ = SuspendKind::Await;

    // §30.6 result element type T (from the lambda's Task<T>/OwningTask<T> return
    // type; null for a no-value `Task`). Mirrors emitAsyncMethod's resultTy/
    // resultSemaTy setup (which reads it from the method's declared return type).
    llvm::Type* resultTy =
        resultElemSemaTy ? lowerTypeRef(resultElemSemaTy) : nullptr;
    asyncResultTy_ = resultTy;
    asyncResultSemaType_ = resultElemSemaTy;
    // §30.5 / #1503: Pattern A (Task<T:class>) result disposer, mirroring
    // emitAsyncMethod. `declaredTaskTy` is null when the caller had no
    // call-site annotation to derive it from (e.g. an IIFE-invoked async
    // lambda); in that case no disposer is attached here.
    asyncPatternAResultDisposer_ = patternAResultDisposerFor(declaredTaskTy);

    // ---- Snapshot the capture set bound by lowerLambda's prologue ----
    // A FrameCapture records how to spill (in the entry, from the current slot)
    // and restore (in the resume, into a fresh slot). `isThis` captures the
    // receiver by value; `isCell` captures a shared-cell POINTER (the lambda's
    // capture of an outer variable, §27.5) and must re-mark cellVars_ on restore
    // so body reads/writes route through the cell; otherwise it is a plain
    // by-value lambda param.
    struct FrameCapture {
        llvm::Type*          fieldTy;   // frame field type (value type, or ptr)
        llvm::Value*         spillVal;  // value to store into the frame (entry)
        const sema::Symbol*  paramSym;  // non-null → rebind paramSlots_[sym]
        const ast::Node*     localDecl; // non-null → rebind locals_[decl]
        llvm::Type*          cellValTy; // non-null → this is a shared cell (§27.5)
        bool                 isThis;
    };
    std::vector<FrameCapture> caps;

    // Lambda params + captured outer PARAMS live in paramSlots_. Distinguish a
    // by-value lambda param (no cellVars_ entry) from a captured outer-param cell
    // (cellVars_ entry present). The order is the iteration order of paramSlots_;
    // it is stable within this single emission (we snapshot now and rebuild the
    // same order in the resume).
    for (auto& [sym, slot] : paramSlots_) {
        FrameCapture fc{};
        fc.paramSym = sym;
        fc.localDecl = nullptr;
        fc.isThis = false;
        auto cv = cellVars_.find(slot);
        if (cv != cellVars_.end()) {
            // Captured outer param: slot holds the cell pointer (§27.5).
            fc.cellValTy = cv->second;
            fc.fieldTy = ptrTy();
            fc.spillVal = builder_.CreateLoad(ptrTy(), slot, "asynclam.cap.cell");
        } else {
            // By-value lambda param.
            fc.cellValTy = nullptr;
            fc.fieldTy = slot->getAllocatedType();
            fc.spillVal = builder_.CreateLoad(fc.fieldTy, slot, "asynclam.param");
        }
        caps.push_back(fc);
    }
    // Captured outer LOCALS live in locals_ (each a shared-cell holder).
    for (auto& [decl, slot] : locals_) {
        auto cv = cellVars_.find(slot);
        if (cv == cellVars_.end()) continue;  // a non-cell local is body-private
        FrameCapture fc{};
        fc.paramSym = nullptr;
        fc.localDecl = decl;
        fc.cellValTy = cv->second;
        fc.fieldTy = ptrTy();
        fc.spillVal = builder_.CreateLoad(ptrTy(), slot, "asynclam.cap.cell");
        fc.isThis = false;
        caps.push_back(fc);
    }
    // `this` (captured by value from the enclosing instance method, §27.5).
    llvm::Value* capturedThis = currentThis_;
    if (capturedThis) {
        FrameCapture fc{};
        fc.fieldTy = ptrTy();
        fc.spillVal = capturedThis;
        fc.paramSym = nullptr;
        fc.localDecl = nullptr;
        fc.cellValTy = nullptr;
        fc.isThis = true;
        caps.push_back(fc);
    }

    // ---- Build the state frame ----
    // Layout: { ptr vtable, i32 state, captures..., bodyLocals...,
    //           ptr outerTask, ptr pendingTask, ptr spill }. The three trailing
    //           async slots mirror emitAsyncMethod exactly.
    std::vector<std::pair<const ast::Node*, llvm::Type*>> bodyLocals;
    // §27.5 / §30 (#808 exposure): compute the lambda-captured locals of THIS
    // body first so collectSuspendLocals plans ptr-typed fields for cell-boxed
    // locals (see frameCapturedLocals_).
    if (!lam.exprBody) {
        computeFrameCapturedLocals(lam.stmts);
        collectSuspendLocals(lam.stmts, bodyLocals);
    }

    std::vector<llvm::Type*> fields;
    fields.push_back(ptrTy());          // vtable → 0 (unused; kept for layout parity)
    fields.push_back(i32());            // state  → 1
    const unsigned STATE_FIELD = 1;
    const unsigned CAPTURE_START = 2;
    for (auto& fc : caps) fields.push_back(fc.fieldTy);
    const unsigned LOCAL_START = (unsigned)(CAPTURE_START + caps.size());
    for (auto& [node, ty] : bodyLocals) fields.push_back(ty);
    const unsigned OUTER_TASK_FIELD = (unsigned)(LOCAL_START + bodyLocals.size());
    const unsigned PENDING_TASK_FIELD = OUTER_TASK_FIELD + 1;
    const unsigned SPILL_PTR_FIELD = OUTER_TASK_FIELD + 2;
    fields.push_back(ptrTy());          // outerTask
    fields.push_back(ptrTy());          // pendingTask
    fields.push_back(ptrTy());          // spill

    static std::atomic<unsigned> lamSmCounter{0};
    unsigned smId = lamSmCounter.fetch_add(1);
    auto* stateStructTy = llvm::StructType::create(
        llvm_, fields, "async.lambda." + std::to_string(smId));

    asyncOuterTaskField_ = OUTER_TASK_FIELD;
    asyncPendingTaskField_ = PENDING_TASK_FIELD;
    asyncSpillPtrField_ = SPILL_PTR_FIELD;

    // bodyLocal AST-node → frame field index (consumed by lowerLocalDecl's
    // frame-backing path, which keys off smLocalFields_).
    std::unordered_map<const ast::Node*, unsigned> localFields;
    for (unsigned li = 0; li < bodyLocals.size(); ++li)
        localFields[bodyLocals[li].first] = LOCAL_START + li;

    // ---- Resume(ptr completedTask, ptr frame) -> void ----
    auto* resumeTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false);
    std::string prefix = "bz$asynclam$" + std::to_string(smId) + "$";
    auto* resumeFn = llvm::Function::Create(
        resumeTy, llvm::Function::InternalLinkage, prefix + "Resume",
        module_.get());
    asyncResumeFn_ = resumeFn;

    // ---- ENTRY (= the closure body fn, builder positioned at its entry block,
    //              with caps already bound) ----
    {
        // outerTask = bz_task_alloc()
        auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
        llvm::Value* outerTask = builder_.CreateCall(allocFn, {}, "asynclam.task");

        // frame = calloc(1, sizeof(Frame))
        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* st = builder_.CreateCall(
            callocFn2, {builder_.getInt64(1), builder_.getInt64(stSz)},
            "asynclam.frame");

        // state = 0
        builder_.CreateStore(
            builder_.getInt32(0),
            builder_.CreateStructGEP(stateStructTy, st, STATE_FIELD));
        // outerTask field
        builder_.CreateStore(
            outerTask,
            builder_.CreateStructGEP(stateStructTy, st, OUTER_TASK_FIELD));
        // captures (lambda params + cell pointers + this), spilled from the
        // values captured above (in entry context, before any state mutation).
        for (unsigned i = 0; i < caps.size(); ++i) {
            llvm::Value* capField =
                builder_.CreateStructGEP(stateStructTy, st,
                                         (unsigned)(CAPTURE_START + i));
            if (caps[i].spillVal)
                builder_.CreateStore(caps[i].spillVal, capField);
        }

        // Drive the state machine once synchronously (§30.1 synchronous prefix).
        builder_.CreateCall(resumeFn,
                            {llvm::ConstantPointerNull::get(ptrTy()), st});
        // Return the outer task pointer (the closure's Task / Task<T> result).
        builder_.CreateRet(outerTask);
    }

    // ---- Resume: the state machine body ----
    {
        // Reset emitter state for the resume function context. The lambda's
        // captures are rebound below from the frame; no outer-frame slot state
        // carries over (the resume runs on a fresh — possibly cross-thread —
        // activation, reading only the heap frame).
        currentFn_ = resumeFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        cellVars_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        asyncHandlerSpillSlots_.clear();
        // #422 (see emitAsyncMethod): a `var` CLOSURE local in the async LAMBDA's
        // own block body must be released at scope end. Compute eligibility for
        // the lambda body and key it to the lambda resume fn (where the body
        // lowers). An expression-bodied lambda has no stmts (and no such local).
        // #1416 / #1418: likewise compute the CAPTURE-CELL free eligibility so a
        // frame-local captured by a nested lambda inside this async lambda's body
        // has its cell freed at scope end (mirrors emitAsyncMethod above).
        boxCaptureLocals_.clear();
        singleCaptureLocals_.clear();
        assignedCaptureVars_.clear();  // fix #2706
        definitelyEscapingLambdas_.clear();
        boxCaptureLocalsFn_ = resumeFn;
        cellFreeEligible_.clear();
        unwindUnsafeCellFree_.clear();  // fix #2334
        sharedCellEnvdtorFreeOwner_.clear();  // fix #3426
        cellFreeEligibleFn_ = resumeFn;
        if (!lam.exprBody) {
            collectClosureReleaseEligible(lam.stmts);
            collectBoxCaptureLocals(lam.stmts);
            collectDefinitelyEscapingLambdas(lam.stmts);
            // Must run AFTER collectClosureReleaseEligible (uses its result).
            collectCellFreeEligible(lam.stmts);
        } else {
            closureReleaseEligible_.clear();
        }
        closureReleaseEligibleFn_ = resumeFn;
        // fix #2132 / #2142 / #2143 / #1871 / §28.9 / §28.11: re-establish the
        // borrowing-`Array<T>` / `Set<T>` / `Dictionary<K,V>` literal /
        // materializer scope-end reclaim eligibility for the async LAMBDA's
        // resume fn too (see emitAsyncMethod's mirror). Without it a
        // `var xs: Array<T> := [ … ]` in an async lambda body held across an await
        // is spilled/restored (the smAllocaToField_ fix) but never FREED on the
        // lambda's async return edge — its backing leaks. Expression-bodied lambdas
        // have no stmts (and no such local); clear the (enclosing method's) set so
        // its entries cannot misapply under this resume fn's key.
        if (!lam.exprBody)
            collectBorrowingArrayFreeEligible(lam.stmts);
        else {
            borrowingArrayFreeEligible_.clear();
            dictMaterializerFreeEligible_.clear();
            groupingFreeEligible_.clear();
            setLiteralFreeEligible_.clear();   // #2142
            dictLiteralFreeEligible_.clear();  // #2143
            literalTreeAggregateEligible_.clear();  // fix #2660
        }
        borrowingArrayFreeEligibleFn_ = resumeFn;
        dictMaterializerFreeEligibleFn_ = resumeFn;
        groupingFreeEligibleFn_ = resumeFn;
        setLiteralFreeEligibleFn_ = resumeFn;   // #2142
        dictLiteralFreeEligibleFn_ = resumeFn;  // #2143
        // CRITICAL (nested lambda): the await spill/restore loop iterates
        // smAllocaToField_, mapping a suspend-local's alloca → its frame field.
        // An ENCLOSING async method's resume populated this with ITS allocas
        // before lowerLambda was reached; if not cleared, the lambda's await
        // would spill/restore those foreign allocas inside the lambda's resume
        // function — a cross-function instruction reference (IR verifier failure).
        // Reset it so only the lambda body's own suspend-locals are tracked.
        // (smLocalFields_ is overwritten wholesale by emitResumeDispatch below.)
        smAllocaToField_.clear();

        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", resumeFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = resumeFn->getArg(1);

        // Restore captures from the frame into fresh slots, re-establishing the
        // exact binding lowerLambda's prologue set up (so body identifier reads
        // resolve identically). A shared-cell capture (§27.5) re-marks cellVars_
        // so reads/writes route through the cell; `this` sets currentThis_.
        for (unsigned i = 0; i < caps.size(); ++i) {
            const FrameCapture& fc = caps[i];
            llvm::Value* capField =
                builder_.CreateStructGEP(stateStructTy, self,
                                         (unsigned)(CAPTURE_START + i));
            llvm::Value* val = builder_.CreateLoad(fc.fieldTy, capField);
            if (fc.isThis) {
                currentThis_ = val;
                continue;
            }
            auto* slot = entryAlloca(fc.fieldTy);
            builder_.CreateStore(val, slot);
            if (fc.cellValTy) cellVars_[slot] = fc.cellValTy;
            if (fc.paramSym) paramSlots_[fc.paramSym] = slot;
            if (fc.localDecl) locals_[fc.localDecl] = slot;
        }
        // fix #2817 / #2807 / #429 / §4.6 / §28.9 / §28.3 / §30.6 (v0.240): the
        // `var` STRING-local refcount analysis must describe the async LAMBDA's
        // RESUME fn too — the same re-establishment performed just above for the
        // closure (#422), capture-cell (#1416/#1418) and borrowing-collection
        // (#2132/#2142/#2143) analyses. Without it every `var` string local in
        // an async lambda body leaked both of its allocations, per invocation
        // (#2817 — measured 1904 bytes / 80 allocations for 20 invocations x 2
        // locals). Block bodies only: an expression body has no LocalDecl to
        // arm, and leaving the token unpinned there keeps that path unchanged.
        // The enclosing function's sets/token are saved and restored by
        // lowerLambda around this whole call (fix #2817), so this pin cannot
        // leak back into the body that is building the closure.
        //
        // The frame-spill dimension is the one emitAsyncMethod already argued:
        // collectSuspendLocals gives every LocalDecl in the body a frame field
        // and emitAwaitSuspend spills/restores every mapped alloca around each
        // suspend, so the alloca a scope-end Disposal loads holds the live value
        // on whichever activation reaches the exit; a SUSPEND returns from resume
        // without touching disposeStack_, so a local live across an `await` is
        // released once, by the activation that completes. // per §28.9, §30.2
        if (!lam.exprBody) {
            collectStringReleaseEligible(lam.stmts);
            stringReleaseEligibleFn_ = resumeFn;
            // fix #2696 / §28.1 / §28.9: CO-GATE the parameter retain with the
            // parameter release the pin just enabled (see lowerLambda's mirror
            // and emitAsyncMethod's). Emitted in the resume ENTRY block, which
            // dominates every assignment and re-runs on every activation, while a
            // given activation can release the incoming value at most once (after
            // the first assignment the slot holds a value this frame owns) — so
            // retains >= releases on every path: a surplus retain leaks one
            // reference per resume, it can never over-release.
            if (paramSyms) retainAssignedStringParams(lam.stmts, *paramSyms);
        }
        (void)paramSyms;  // params are restored via caps (paramSlots_) above

        // State field GEP — read by the resume dispatch, written at each await.
        llvm::Value* stateGEP =
            builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);

        // Build a StateMachineFrame view for the kind-agnostic dispatch + the
        // lowerLocalDecl frame-backing path (which keys off smLocalFields_).
        StateMachineFrame frame;
        frame.structTy = stateStructTy;
        frame.stateField = STATE_FIELD;
        frame.captureStart = CAPTURE_START;
        frame.localStart = LOCAL_START;
        frame.currentValField = OUTER_TASK_FIELD;
        frame.bodyLocals = bodyLocals;
        frame.localFields = localFields;

        unsigned numAwaits = lam.exprBody
                                 ? countAwaitsInExpr(lam.exprBody.get())
                                 : countAwaits(lam.stmts);

        // Publish the live state BEFORE emitResumeDispatch: its `emitDone`
        // callback (the switch-default "exhausted" block) runs emitAsyncComplete,
        // which reads smStateStruct_/smSelf_/asyncOuterTaskField_. emitResumeDispatch
        // sets these only AFTER calling emitDone, so for a NESTED async lambda the
        // enclosing method's (stale) frame would otherwise be used — producing a
        // GEP into %async.Main with the lambda resume's frame arg (cross-function
        // reference → IR verifier failure). Setting them here makes emitDone use
        // the LAMBDA's frame. (A top-level async method is unaffected: there the
        // enclosing smStateStruct_ is null, so emitAsyncComplete is a no-op in the
        // method's mn.done — which is never the real completion path anyway.)
        smStateStruct_ = stateStructTy;
        smSelf_ = self;

        std::vector<llvm::BasicBlock*> stateBBs = emitResumeDispatch(
            frame, self, stateGEP, numAwaits,
            [&]() {
                emitAsyncComplete(/*value=*/nullptr);
                builder_.CreateRetVoid();
            });

        // §30.9 (basic): the async fault boundary — an uncaught throw / faulted-
        // await re-raise completes the OUTER task with the in-flight exception
        // object and returns. Identical to emitAsyncMethod.
        auto* faultBB = llvm::BasicBlock::Create(llvm_, "asynclam.fault", resumeFn);
        unwindTarget_ = faultBB;
        {
            llvm::IRBuilder<>::InsertPointGuard guard(builder_);
            builder_.SetInsertPoint(faultBB);
            llvm::Value* otField = builder_.CreateStructGEP(
                stateStructTy, self, OUTER_TASK_FIELD);
            llvm::Value* outerTask = builder_.CreateLoad(ptrTy(), otField);
            llvm::Value* excObj =
                builder_.CreateLoad(ptrTy(), emitExcSlot(), "asynclam.fault.exc");
            auto* completeErrObjFn = declareC("bz_task_complete_error_obj",
                                              llvm::Type::getVoidTy(llvm_),
                                              {ptrTy(), ptrTy()});
            builder_.CreateCall(completeErrObjFn, {outerTask, excObj});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 emitExcSlot());
            emitFreeSpillArea(self);
            auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_),
                                     {ptrTy()});
            builder_.CreateCall(freeFn2, {self});
            builder_.CreateRetVoid();
        }

        // Emit the body from state 0.
        builder_.SetInsertPoint(stateBBs[0]);
        disposeStack_.emplace_back();
        if (lam.exprBody) {
            // §30.6: an expression-bodied async lambda's value completes the
            // outer task (§30.1 `return expression;`). `await` inside the
            // expression lowers as a suspend point; the resulting value is what
            // completes the task. A no-value `Task` lambda (resultTy == null)
            // ignores the value and completes with no result.
            llvm::Value* v = lowerExpr(*lam.exprBody);
            if (!builder_.GetInsertBlock()->getTerminator()) {
                emitAsyncComplete(resultTy ? v : nullptr);
                builder_.CreateRetVoid();
            }
        } else {
            bool term = lowerBody(lam.stmts);
            if (!term && !builder_.GetInsertBlock()->getTerminator()) {
                emitAsyncComplete(/*value=*/nullptr);
                builder_.CreateRetVoid();
            }
        }
        disposeStack_.pop_back();

        // `await using` orphan-state termination (parity with emitAsyncMethod).
        if (smDoneBB_) {
            for (unsigned s = smNextSuspendState_ + 1; s < smStateBBs_.size();
                 ++s) {
                auto* orphanBB = smStateBBs_[s];
                if (orphanBB && !orphanBB->getTerminator()) {
                    llvm::IRBuilder<> tmp(orphanBB);
                    tmp.CreateBr(smDoneBB_);
                }
            }
        }

        // Repair SSA dominance broken by awaits in larger expressions (#438).
        fixupSuspendDominance(resumeFn);

        // Clear state-machine state.
        smStateStruct_ = nullptr;
        smSelf_ = nullptr;
        smLocalFields_.clear();
        smAllocaToField_.clear();
        asyncResumeFn_ = nullptr;
        asyncResultTy_ = nullptr;
        asyncResultSemaType_ = nullptr;
        asyncPatternAResultDisposer_ = nullptr;
    }
}


// §30.7: emit an async iterator (declared `async`, contains `yield`, returns
// IAsyncEnumerable<T> / IAsyncEnumerator<T>). This is the iterator state machine
// (§29.3) and the async state machine (§30.1) fused: the body both YIELDs values
// and AWAITs tasks between them. The driver is an IAsyncEnumerator<T> whose
// MoveNextAsync returns a Task<bool> — each advance drives the body to the next
// `yield return` (Task<bool> = true) or to exhaustion (false), suspending on any
// `await` encountered along the way.
//
// Layout reuses the kind-agnostic frame with FOUR trailing fields (the iterator's
// `current_val` + the three async slots):
//   { ptr vtable, i32 state, captures..., locals...,
//     T current_val, ptr perAdvanceTask, ptr pendingTask, ptr spill }
// The frame persists for the whole iteration; MoveNextAsync re-allocates the
// per-advance Task<bool> each call. Dispose frees the frame (and any spill area).
void Emitter::emitAsyncIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                                      const CallableInfo& ci) {
    if (!ci.stmts) return;
    // fix #2895 (§29.3 / §30.1 / §30.7, v0.240): retract the entire
    // suspend/resume frame on every exit from this emitter (the partial
    // clear at ~2767 happens BEFORE the Dispose body and is defensive; this
    // guard handles the leak that survived into the next function compiled
    // into the module). Spans MoveNextAsync AND the Dispose body — the
    // Dispose body re-publishes the members it reads just-in-time.
    StateMachineFrameScope smFrameScope(*this);
    smKind_ = SuspendKind::AsyncYield;

    // §29.6 / §30.7 (#554): check for FORBIDDEN suspend positions in this async
    // iterator body. The allowed positions are (§29.6 / §30.9):
    //   • `yield` or `await` in a try body with ONLY a finally (no catch arms).
    //   • `await` in a catch arm body (§30.9 plain-async rules apply).
    // The forbidden positions (yield in try-with-catch, yield in catch arm body,
    // yield in finally, await in catch guard) are rejected with a clean diagnostic.
    // NOTE: `await` in a finally is now also forbidden (§30.7 v0.137); that check
    // follows immediately below.
    if (suspendInTryPosition(*ci.stmts)) {
        ctx_.diagnostics().report(
            Severity::Error,
            ms->decl ? ms->decl->span.begin : SourceLocation{},
            "forbidden by §29.6: `yield` inside a try body with catch arms, "
            "`yield` inside a catch arm body, or `yield` inside a finally block "
            "in an async iterator (`yield` in a try body with only a finally is "
            "allowed; `await` in a try body or catch arm body is allowed per §30.9; "
            "`await` in a finally is forbidden per §30.7 v0.137)");
        emitAsyncStub(ms, fn, ci);
        return;
    }
    // §30.7 v0.137 (#655): `await` inside a `finally` block in an async iterator
    // is forbidden.  The Dispose path (§29.7) runs synchronously when a consumer
    // abandons iteration early — there is no async driver at that point, so a
    // suspension inside a `finally` would have nowhere to resume; the continuation
    // would be silently discarded.  Remedy: hoist the `await` before the `try`,
    // or move the asynchronous cleanup into the type's `Dispose` path.
    // (An ordinary async method may await in its `finally`; §30.9 is unaffected —
    // this check is only called from emitAsyncIteratorMethod, not emitAsyncMethod.)
    if (awaitInFinallyPosition(*ci.stmts)) {
        ctx_.diagnostics().report(
            Severity::Error,
            ms->decl ? ms->decl->span.begin : SourceLocation{},
            "forbidden by §30.7 v0.137 (#655): `await` inside a `finally` block "
            "in an async iterator — the iterator's `finally` blocks run "
            "synchronously inside `Dispose` (§29.7) when a consumer abandons "
            "iteration early; there is no async driver at that point, so a "
            "suspension would be silently discarded. "
            "Remedy: hoist the `await` before the `try`, or move async cleanup "
            "into the type's `Dispose` path");
        emitAsyncStub(ms, fn, ci);
        return;
    }
    // §30.9: also reject the plain-async deferred position — await in a catch
    // guard — in the async iterator body, using the same check as emitAsyncMethod.
    if (awaitInDeferredTryPosition(*ci.stmts)) {
        ctx_.diagnostics().report(
            Severity::Error,
            ms->decl ? ms->decl->span.begin : SourceLocation{},
            "not yet implemented: `await` inside a catch GUARD (`catch ... when "
            "await ...`) inside an async iterator — the guard runs during catch "
            "dispatch before the handler scope exists (§30.9)");
        emitAsyncStub(ms, fn, ci);
        return;
    }

    // Element type T from the return type's first generic arg
    // (IAsyncEnumerable<T> / IAsyncEnumerator<T>).
    llvm::Type* elemTy = i32();
    const sema::TypeRef* elemSemaTy = nullptr;  // #1102B
    // #1349: detect IOwningAsyncEnumerable<T> so TakeCurrent gets the IOE guard.
    bool isOwningIter = false;
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0]) {
            elemSemaTy = retRef->genericArgs[0];
            elemTy = lowerTypeRef(elemSemaTy);
        }
        if (retRef) {
            std::string_view rn;
            if (retRef->astNode && retRef->astNode->name &&
                !retRef->astNode->name->parts.empty())
                rn = retRef->headName();
            else if (!retRef->importedName.empty())
                rn = retRef->importedName;
            // §29.1 v0.236 / #2591: recognize BOTH owning async iterator heads.
            // IOwningAsyncEnumerable<T> is the spec-sanctioned iterator return
            // (§30.7); IOwningAsyncEnumerator<T> is the enumerator-direct return
            // form (the owning sibling of the IAsyncEnumerator<T>-direct form
            // fixed under #759), reached via `await using var en := Gen();`
            // where Gen's declared return type is IOwningAsyncEnumerator<T>.
            // Pre-#2591 this head fell through here, so the codegen treated the
            // frame as the BORROWING form: Current was non-destructive and the
            // Dispose path never disposed the yielded current_val — an owning
            // iterator that leaked every yielded value. Recognizing it makes
            // TakeCurrent (vtable slot 3), the disposeYieldedVal pass in
            // emitFreeAndReturn, and the owned-local scan below all apply, so
            // the frame implements the §29.1 owning contract (an advanced-but-
            // untaken value is disposed by Dispose).
            isOwningIter = (rn == "IOwningAsyncEnumerable" ||
                            rn == "IOwningAsyncEnumerator");
        }
    }
    // The per-advance MoveNextAsync result is Task<bool>; the await-result type
    // the Resume completes is bool (i1). asyncResultTy_ sizes bz_task_complete.
    asyncResultTy_ = i1();

    // Frame: locals + [ T current_val, ptr perAdvanceTask, ptr pendingTask,
    //                   ptr spill, ptr token ].
    // §30.7 (v0.228, #1525): TOKEN_FIELD is the CancellationToken cell captured
    // by GetAsyncEnumerator(token) — null (CancellationToken.None) by default
    // (calloc-zeroed at construction), never cancels until GetAsyncEnumerator
    // stores a real token.
    StateMachineFrame frame = buildStateMachineFrame(
        ms, *ci.stmts, {elemTy, ptrTy(), ptrTy(), ptrTy(), ptrTy()},
        std::string("asynciter.") + std::string(ms->name));
    auto* stateStructTy = frame.structTy;
    const unsigned VTABLE_FIELD = frame.vtableField;
    const unsigned STATE_FIELD = frame.stateField;
    const unsigned CURRENT_VAL_FIELD = frame.currentValField;       // first trailing
    const unsigned PER_ADVANCE_TASK_FIELD = frame.currentValField + 1;
    const unsigned PENDING_TASK_FIELD = frame.currentValField + 2;
    const unsigned SPILL_PTR_FIELD = frame.currentValField + 3;
    const unsigned TOKEN_FIELD = frame.currentValField + 4;
    // The Resume completes the per-advance task; reuse asyncOuterTaskField_ for it.
    asyncOuterTaskField_ = PER_ADVANCE_TASK_FIELD;
    asyncPendingTaskField_ = PENDING_TASK_FIELD;
    asyncSpillPtrField_ = SPILL_PTR_FIELD;

    bool isInstance = !isStaticMethod(ms);
    std::string prefix = std::string("bz$asynciter$") + std::string(ms->name) + "$";

    // §29.7 (#2574): the LOCAL counterpart of the async iterator's owned-
    // parameter scan. Restricted to IOwningAsyncEnumerable iterators — a bare-
    // identifier `yield return L;` only transfers a local in the owning form
    // (#1863); for an IAsyncEnumerable<owned T> iterator the bare-identifier
    // yield is a borrow and the local's disposal is bounded by sema's never-
    // discharged auto-drop or unwind (no Abandoned-state-keyed gap analogous
    // to the bug). The scan counts BOTH yields AND awaits (async suspend
    // states) so the (establishedAtState, transferredAtState) window matches
    // the runtime smNextSuspendState_ numbering. On any non-linear shape the
    // scan returns ok=false and the legacy bounded-leak path keeps working.
    struct AsyncLocalKeyedDisposal {
        unsigned establishedAtState = 0;
        unsigned transferredAtState = 0;   // 0 = not transferred (legacy fallback)
        unsigned frameField = 0;
    };
    std::unordered_map<const ast::LocalDecl*, AsyncLocalKeyedDisposal>
        asyncOwnedLocalDisposals;
    if (isOwningIter) {
        bool lscanOk = true;
        unsigned lscanState = 0;
        std::unordered_map<const ast::LocalDecl*,
            std::pair<unsigned, unsigned>> lscanOut;
        collectAsyncSuspendStatesForOwnedLocals(
            ctx_, *ci.stmts, lscanState, lscanOut, lscanOk);
        if (lscanOk && !lscanOut.empty()) {
            bool allTransferred = true;
            for (auto& [ld, pr] : lscanOut) {
                if (pr.second == 0) {
                    allTransferred = false;
                    break;
                }
                if (!unwindOwnedLocalClass(ld)) {
                    allTransferred = false;
                    break;
                }
            }
            if (allTransferred) {
                asyncOwnedLocalDisposals.clear();
                for (auto& [ld, pr] : lscanOut) {
                    AsyncLocalKeyedDisposal info;
                    info.establishedAtState = pr.first;
                    info.transferredAtState = pr.second;
                    auto fit = frame.localFields.find(ld);
                    if (fit != frame.localFields.end())
                        info.frameField = fit->second;
                    asyncOwnedLocalDisposals[ld] = info;
                }
            }
        }
    }

    // --- MoveNextAsync(ptr self) -> ptr (Task<bool>) ---
    auto* moveNextTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    auto* moveNextFn = llvm::Function::Create(
        moveNextTy, llvm::Function::InternalLinkage,
        prefix + "MoveNextAsync", module_.get());

    // --- get$Current(ptr self) -> T ---
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);
    auto* currentFn = llvm::Function::Create(
        currentTy, llvm::Function::InternalLinkage,
        prefix + "Current", module_.get());

    // --- Resume(ptr completedTask, ptr frame) -> void --- (bz_task_then ABI) ---
    auto* resumeTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false);
    auto* resumeFn = llvm::Function::Create(
        resumeTy, llvm::Function::InternalLinkage, prefix + "Resume",
        module_.get());
    asyncResumeFn_ = resumeFn;

    // --- Dispose(ptr self) -> void ---
    // §29.7 (#623): Declare now so the vtable can reference it; real body is
    // emitted after Resume emission (once smIterFinallyAtState_ is populated).
    auto* disposeFn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
        llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());

    // --- GetAsyncEnumerator(ptr self, ptr token) -> ptr (vtable slot 4) ---
    // fix #1268 / §29.3 + §30.7: `IAsyncEnumerable<T>` is RE-ITERABLE — "each
    // call to GetAsyncEnumerator() runs the body again from the start" (§29.3),
    // and §29.11 "Each GetEnumerator() call produces an independent enumerator".
    // The async iterator previously had no such slot: the value the factory
    // returns WAS treated as the enumerator, so a second `await for` over the
    // same bound source drove — and the first loop's exit had already Disposed
    // and freed — the very same frame (ASan: heap-use-after-free in the
    // consumer's Resume). This is the async twin of the sync fix #943
    // (LlvmBackend_Iterators.cpp), and the slot is emitted with the same
    // semantics: a fresh calloc byte-copy of the frame with the state reset to
    // the start, so N `await for` loops replay the body N times.
    //
    // §30.7 (v0.228, #1525): "the enumerator returned by the generated
    // GetAsyncEnumerator(token) captures the token" — the interface signature
    // is `GetAsyncEnumerator(token: CancellationToken := CancellationToken.None):
    // owned IAsyncEnumerator<T>`, so the slot now takes a second ptr argument (the
    // CancellationToken cell; null = None) and stores it into the returned
    // enumerator's TOKEN_FIELD.
    auto* getAsyncEnumFn = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
        llvm::Function::InternalLinkage, prefix + "GetAsyncEnumerator",
        module_.get());

    // --- Vtable: [0]=self, [1]=Dispose, [2]=MoveNextAsync, [3]=Current,
    //             [4]=GetAsyncEnumerator (#1268, §29.11) ---
    const unsigned VT_SIZE = 5;
    std::vector<llvm::Constant*> vtElems(
        VT_SIZE, llvm::ConstantPointerNull::get(ptrTy()));
    auto* vtGVType = llvm::ArrayType::get(ptrTy(), VT_SIZE);
    auto* vtGV = new llvm::GlobalVariable(
        *module_, vtGVType, true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(vtGVType, vtElems), prefix + "vtable");
    vtElems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
    vtElems[1] = disposeFn;
    vtElems[2] = moveNextFn;
    vtElems[3] = currentFn;
    vtElems[4] = getAsyncEnumFn;
    vtGV->setInitializer(llvm::ConstantArray::get(vtGVType, vtElems));

    // --- GetAsyncEnumerator body (vtable slot 4, #1268; owned-param CLAIM
    // mechanism fix #2356) ---
    // §29.3 (v0.234): "`IAsyncEnumerable<T>` (re-iterable)" — the same defining
    // clause as the sync form, "re-iterable: each call to `GetEnumerator()` runs
    // the body again from the start" — with NO carve-out for an owned parameter;
    // "`owned` parameters remain admitted (§29.8)" merely says the parameter
    // kind does not disqualify the method from being an iterator at all (unlike
    // `ref`/`out`, forbidden by the same sentence's neighbor), not that it
    // waives re-iterability. §29.11: "Each `GetEnumerator()` call produces an
    // independent enumerator." §30.7's own interface block types the method
    // `GetAsyncEnumerator(...): owned IAsyncEnumerator<T>` — an `owned`-returning
    // method mints a fresh, caller-owned value per call (§29.1: "the caller
    // takes ownership of the enumerator and must dispose it"); handing back
    // `self` — an object the ENUMERABLE's own binder still owns too — breaks
    // that contract regardless of the disposal bookkeeping around it. Returning
    // `self` unconditionally (the pre-#2356 shape) is therefore read as spec-
    // NON-compliant, not merely leaky: issue #2356's own double-free was the
    // symptom, not the defect (two bindings validly OWNING the SAME object, per
    // §28's "every heap object is disposed exactly once").
    //
    // Fix: GetAsyncEnumerator ALWAYS hands back a genuine independent clone
    // (byte-copy + state reset — the SAME machinery already used below for the
    // no-owned-param case). For the affine `owned` parameter specifically (it
    // cannot be duplicated — no implicit Clone()), the FIRST call CLAIMS every
    // owned-param capture: the clone's byte-copy takes the (still-live) pointer,
    // and `self`'s own copy is nulled — so `self`'s later Dispose
    // (emitFreeAndReturn's existing null-guarded sweep, unchanged) becomes a
    // no-op for it, exactly the same null-guard the moved-from case already
    // relies on. `self`'s state becomes the sentinel `-3` (unused elsewhere in
    // this state space — Dispose's own state switch (below) only recognizes
    // non-negative suspension states and treats anything else, `-1` for
    // "completed" included, as "no pending cleanup, just free"), so a SECOND
    // GetAsyncEnumerator call sees the claim and raises a loud
    // `InvalidOperationException` rather than silently handing out a second
    // alias of the same affine resource — the same "claim once, IOE on repeat"
    // shape as the sync iterator's #973a/#2154 GetEnumerator claim
    // (LlvmBackend_Iterators.cpp), simplified: EVERY owned param is claimed
    // uniformly by the first call (no attempt to distinguish a param the body
    // never moves out from one it does — that finer-grained sharing the sync
    // path allows for a never-consumed capture is intentionally NOT replicated
    // here, tracked as a follow-up, #2377, for whoever revisits this for
    // throughput; claim-once is conservative but sound for every shape).
    // `await for`'s desugaring (lowerAwaitFor) already drives exclusively
    // through the RETURNED enumerator pointer (never `self` directly), so it
    // needs no change beyond the pending-exception check this fix adds at both
    // call sites (here callers must, since this is now the first vtable-slot-4
    // path that can throw).
    bool hasOwnedParamCapture = false;
    for (sema::Symbol* psym : ms->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(psym);
        if (ps && ps->modifier == "owned") { hasOwnedParamCapture = true; break; }
    }
    {
        auto* geEntry =
            llvm::BasicBlock::Create(llvm_, "entry", getAsyncEnumFn);
        llvm::IRBuilder<> gb(geEntry);
        llvm::Value* geSelf = getAsyncEnumFn->getArg(0);
        llvm::Value* geToken = getAsyncEnumFn->getArg(1);
        llvm::Value* sourceStateField =
            gb.CreateStructGEP(stateStructTy, geSelf, STATE_FIELD);
        auto* cloneBB = geEntry;
        if (hasOwnedParamCapture) {
            auto* claimedBB =
                llvm::BasicBlock::Create(llvm_, "gae.claimed", getAsyncEnumFn);
            cloneBB = llvm::BasicBlock::Create(llvm_, "gae.claim", getAsyncEnumFn);
            llvm::Value* priorState =
                gb.CreateLoad(i32(), sourceStateField, "gae.priorstate");
            gb.CreateCondBr(
                gb.CreateICmpEQ(priorState, gb.getInt32(-3), "gae.isclaimed"),
                claimedBB, cloneBB);

            // Already claimed by an earlier GetAsyncEnumerator call: raise
            // InvalidOperationException synchronously (this method is NOT
            // itself async — a plain `owned IAsyncEnumerator<T>`-returning
            // call — so the pending-exception protocol, not a faulted Task,
            // is the right channel; mirrors the sync claim block's IOE, and
            // the #1349 TakeCurrent IOE emission a few functions below).
            // NOTE: this block builds `getAsyncEnumFn` through its OWN local
            // `gb` IRBuilder, not the shared `builder_` — `emitExcSlot()`
            // (like most Emitter helpers) emits through `builder_` and would
            // insert the bz_exc_addr() call into whatever function/block
            // `builder_` currently points at (a DIFFERENT function's IR —
            // "referring to an instruction in another function", caught by
            // the verifier). Declare and call bz_exc_addr directly through
            // `gb` instead, exactly mirroring the sync iterator's GetEnumerator
            // claim block (LlvmBackend_Iterators.cpp) and the #1349 TakeCurrent
            // IOE emission (both of which build their own vtable function
            // bodies the same self-contained way).
            gb.SetInsertPoint(claimedBB);
            llvm::Value* excObj = gb.CreateCall(
                callocFn(),
                {gb.getInt64(1), gb.getInt64((int64_t)kExcObjSize)},
                "gae.excobj");
            auto vit = builtinVtable_.find("InvalidOperationException");
            if (vit != builtinVtable_.end())
                gb.CreateStore(vit->second, excObj);
            auto* excAddrTy = llvm::FunctionType::get(ptrTy(), {}, false);
            auto* excAddrFnGE =
                module_->getOrInsertFunction("bz_exc_addr", excAddrTy)
                    .getCallee();
            llvm::Value* excSlotGE =
                gb.CreateCall(excAddrTy, excAddrFnGE, {}, "gae.excslot");
            gb.CreateStore(excObj, excSlotGE);
            gb.CreateRet(llvm::ConstantPointerNull::get(ptrTy()));
            gb.SetInsertPoint(cloneBB);
        }

        uint64_t stSz =
            module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFnGE = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* clone = gb.CreateCall(
            callocFnGE, {gb.getInt64(1), gb.getInt64((int64_t)stSz)},
            "asynciter.clone");
        auto* memcpyFnGE =
            declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        gb.CreateCall(memcpyFnGE,
                      {clone, geSelf, gb.getInt64((int64_t)stSz)});
        // Reset to the start: state 0, no yielded value, no in-flight
        // per-advance / pending task, and no spill area (the clone lazily
        // allocates its own on first resume — copying the source's pointer
        // would make two frames free() the same region).
        gb.CreateStore(gb.getInt32(0),
            gb.CreateStructGEP(stateStructTy, clone, STATE_FIELD));
        gb.CreateStore(llvm::Constant::getNullValue(elemTy),
            gb.CreateStructGEP(stateStructTy, clone, CURRENT_VAL_FIELD));
        gb.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
            gb.CreateStructGEP(stateStructTy, clone,
                               PER_ADVANCE_TASK_FIELD));
        gb.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
            gb.CreateStructGEP(stateStructTy, clone, PENDING_TASK_FIELD));
        gb.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
            gb.CreateStructGEP(stateStructTy, clone, SPILL_PTR_FIELD));
        // §30.7 (#1525): the clone captures the CALLER's token — NOT the
        // source's (the source's own TOKEN_FIELD is whatever was stored the
        // last time GetAsyncEnumerator was called on it, normally still null
        // since the constructing call `Gen()` never sets it).
        gb.CreateStore(geToken,
            gb.CreateStructGEP(stateStructTy, clone, TOKEN_FIELD));
        gb.CreateStore(vtGV,
            gb.CreateStructGEP(stateStructTy, clone, VTABLE_FIELD));
        if (hasOwnedParamCapture) {
            // Transfer: the clone's byte-copy above already took a live
            // pointer for every owned-param capture; null `self`'s own copies
            // so `self`'s later Dispose (emitFreeAndReturn's null-guarded
            // sweep, unchanged) is a no-op for them, and mark `self` claimed
            // so a repeat call raises IOE above instead of aliasing the clone.
            for (std::size_t i = 0; i < frame.captures.size(); ++i) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!ps || ps->modifier != "owned") continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                gb.CreateStore(
                    llvm::Constant::getNullValue(frame.captures[i].second),
                    gb.CreateStructGEP(stateStructTy, geSelf, fieldIdx));
            }
            gb.CreateStore(gb.getInt32(-3), sourceStateField);
        }
        // fix #2609 / §27.5 / §28.9 (v0.236), retain 2-of-2 (the CLONE):
        // the memcpy above ALIASED every closure-cell pointer into the clone,
        // and — unlike the sync iterator, which emits a SEPARATE clone vtable
        // whose Dispose runs with releaseClosures=false — the async iterator has
        // ONE vtable, so the clone's Dispose is the very same function as the
        // source's and will run the release loop too. Give the clone its own +1
        // here so each frame releases exactly the reference it holds; without
        // this, one `await for` (clone Dispose + source Dispose) would release
        // the same cell twice — a double-free/UAF, strictly worse than the leak
        // being fixed. Emitted through `gb` (this vtable body's OWN builder),
        // never `builder_`, which points at a different function (the same
        // cross-function-reference hazard the IOE block above documents);
        // declareC only touches the module, so it is safe here.
        {
            auto* retainFnGE = declareC("bz_closure_retain",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
            for (std::size_t i = 0; i < frame.captures.size(); ++i) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!ps) continue;                   // 'this' capture — skip
                sema::TypeRef* pt = paramTypeOf(ps);
                if (!pt || pt->kind != sema::TypeRef::Kind::Function) continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                llvm::Value* fptr =
                    gb.CreateStructGEP(stateStructTy, clone, fieldIdx);
                llvm::Value* cl = gb.CreateLoad(ptrTy(), fptr, "gae.clcap");
                gb.CreateCall(retainFnGE, {cl});     // null-safe
            }
        }
        gb.CreateRet(clone);
    }

    // --- Construction entry (= the original method): calloc + fill frame ---
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

        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* st = builder_.CreateCall(
            callocFn2, {builder_.getInt64(1), builder_.getInt64(stSz)},
            "asynciter.frame");
        llvm::Value* vtField =
            builder_.CreateStructGEP(stateStructTy, st, VTABLE_FIELD);
        builder_.CreateStore(vtGV, vtField);
        llvm::Value* stateField =
            builder_.CreateStructGEP(stateStructTy, st, STATE_FIELD);
        builder_.CreateStore(builder_.getInt32(0), stateField);
        emitFrameCaptureStores(frame, st);
        // fix #2609 / §27.5 / §28.9 (v0.236), retain 1-of-2 (the SOURCE frame):
        // the async sibling of the sync iterator's capture-store retain (#1864,
        // LlvmBackend_Iterators.cpp). A function-typed (`func`) parameter stored
        // into the frame is a BORROW the frame must be able to invoke across
        // every MoveNextAsync, i.e. it must outlive the producing call — so the
        // frame takes its OWN +1 on the closure cell here, and the Dispose tail
        // (emitFreeAndReturn's release loop) drops exactly that +1. Without the
        // retain, releasing at Dispose would drop a reference the frame never
        // owned (a UAF), which is why the call-site ARGUMENT-PASS release in
        // lowerCall was previously scoped to sync-iterator callees only.
        // bz_closure_retain is null-safe.
        for (std::size_t ci = 0; ci < frame.captures.size(); ++ci) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                frame.captures[ci].first);
            if (!ps) continue;                       // 'this' capture — skip
            sema::TypeRef* pt = paramTypeOf(ps);
            if (!pt || pt->kind != sema::TypeRef::Kind::Function) continue;
            unsigned fieldIdx = frame.captureStart + (unsigned)ci;
            llvm::Value* fptr =
                builder_.CreateStructGEP(stateStructTy, st, fieldIdx);
            llvm::Value* cl =
                builder_.CreateLoad(ptrTy(), fptr, "asynciter.clcap");
            emitClosureRetain(cl);
        }
        builder_.CreateRet(st);
    }

    // --- Current: load current_val from the frame ---
    // For IOwningAsyncEnumerable<T> this implements TakeCurrent() (vtable slot 3):
    // check for null → IOE; on success null slot and return value. §29.1/#1349.
    {
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", currentFn);
        if (isOwningIter && elemTy->isPointerTy()) {
            // #1349 / §29.1 v0.161: TakeCurrent with IOE guard.
            auto* ioeErrBB = llvm::BasicBlock::Create(llvm_, "tc.ioe", currentFn);
            auto* tcOkBB   = llvm::BasicBlock::Create(llvm_, "tc.ok",  currentFn);
            llvm::IRBuilder<> cb(entry);
            llvm::Value* self    = &*currentFn->arg_begin();
            llvm::Value* cvField = cb.CreateStructGEP(stateStructTy, self,
                                                       CURRENT_VAL_FIELD);
            llvm::Value* curVal  = cb.CreateLoad(elemTy, cvField, "tc.cur");
            llvm::Value* isNull  = cb.CreateICmpEQ(
                curVal, llvm::ConstantPointerNull::get(ptrTy()), "tc.isnull");
            cb.CreateCondBr(isNull, ioeErrBB, tcOkBB);

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
            llvm::Value* excSlot = cb.CreateCall(excAddrTy, excAddrFnV, {},
                                                 "tc.exc");
            cb.CreateStore(excObj, excSlot);
            cb.CreateRet(llvm::ConstantPointerNull::get(ptrTy()));

            cb.SetInsertPoint(tcOkBB);
            cb.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), cvField);
            cb.CreateRet(curVal);
        } else {
            llvm::IRBuilder<> cb(entry);
            llvm::Value* self    = &*currentFn->arg_begin();
            llvm::Value* cvField = cb.CreateStructGEP(stateStructTy, self,
                                                       CURRENT_VAL_FIELD);
            cb.CreateRet(cb.CreateLoad(elemTy, cvField));
        }
    }

    // --- MoveNextAsync: alloc a fresh per-advance Task<bool>, drive Resume ---
    {
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", moveNextFn);
        llvm::IRBuilder<> mb(entry);
        llvm::Value* self = &*moveNextFn->arg_begin();
        auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
        llvm::Value* task = mb.CreateCall(allocFn, {}, "advance.task");
        llvm::Value* taskField =
            mb.CreateStructGEP(stateStructTy, self, PER_ADVANCE_TASK_FIELD);
        mb.CreateStore(task, taskField);
        // Drive the state machine once synchronously (§30.1 synchronous prefix).
        mb.CreateCall(resumeFn,
                      {llvm::ConstantPointerNull::get(ptrTy()), self});
        mb.CreateRet(task);
    }

    // --- Resume: the fused yield/await state machine body ---
    {
        currentFn_ = resumeFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        // §29.7 (#623): clear the finally-chain tracking for this async iterator.
        finallyBodyStack_.clear();
        smIterFinallyAtState_.clear();
        smIterDisposalsAtState_.clear();  // #821 (async)
        smIterEnumFramesAtState_.clear();  // §29.7 (#944): no stale entries
        smIterCleanupAtState_.clear();     // fix #1567: no stale unified entries
        smIterEnumFrameSlot_.clear();      // §29.7 (#944)
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        // fix #1896 / #1897 (async-iterator sibling) / §28.3 "Exception-path
        // discharge" / §29.2 / §30.1: same exception-path-discharge gap as the
        // plain-async and sync-iterator state machines — the async-iterator
        // (async + yield / IAsyncEnumerable) resume body never keyed
        // unwindIneligibleFn_ to its resume fn, so registerUnwindOwnedLocal's
        // `currentFn_ == unwindIneligibleFn_` gate never matched and a plain
        // `owned` local hoisted into this state-machine frame was skipped for the
        // `unwindOnly` disposal — leaking when the body threw after a suspend.
        // Mirror the synchronous setup. // per §28.3
        unwindIneligibleFn_ = resumeFn;
        collectUnwindIneligible(*ci.stmts);
        // fix #2817 / #2807 / #429 / §4.6 / §28.9 / §28.3 / §30.7 (v0.240): the
        // `var` STRING-local refcount analysis must describe the async-ITERATOR
        // resume fn too — the same re-establishment #1896/#1897 made just above
        // for the unwind sets. Without it no string local in any `async` +
        // `yield return` body was ever retained OR released: both allocations
        // leaked, per local, per element (#2817 — measured 1904 bytes / 80
        // allocations over 20 advances). Same activation model the SYNC iterator
        // fix argues in full (LlvmBackend_Iterators.cpp emitIteratorMethod): the
        // bind and the scope-end release of a local declared inside the yielding
        // loop land in consecutive ADVANCES, one each, and the abandonment path
        // is covered by the yield handler's smIterCleanupAtState_/
        // smIterDisposalsAtState_ snapshot, which the async Dispose replays with
        // the frame locals restored. // per §29.7, §30.7
        collectStringReleaseEligible(*ci.stmts);
        stringReleaseEligibleFn_ = resumeFn;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", resumeFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = resumeFn->getArg(1);
        emitFrameCaptureRestores(frame, self);

        // fix #3466 (§27.5 / §30.1 / §30.7, v0.283): the async-ITERATOR spelling
        // of the same publish — see the async-method site above. This body
        // suspends on BOTH an `await` and a `yield return`, and both spill/restore
        // loops iterate smAllocaToField_, so one registration covers both.
        smParamCellField_ = frame.paramCellFields;
        smParamCellFieldFn_ = resumeFn;

        // fix #2817 / fix #2696 / §28.1 / §28.9 / §4.6 (v0.240): CO-GATE the
        // parameter retain with the parameter release the pin above just turned
        // on — a `string` PARAMETER's single reference belongs to the CALLER
        // (§28.1: on a `string`, "`owned` is a compile error: such values carry
        // no disposal obligation to transfer (§28.9)"), so an unpaired release is
        // the #2696 use-after-free. Emitted in the resume ENTRY block, which
        // dominates every state block and re-runs on every activation, while a
        // given activation can release the incoming value at most once — so
        // retains >= releases on every path.
        retainAssignedStringParams(*ci.stmts, ms->params);

        // §30.7 (v0.228, #1525): "every MoveNextAsync() performs an implicit
        // token.ThrowIfCancellationRequested() — on entry, and at each resume
        // from suspension within the advance — so a requested cancellation
        // faults the in-flight MoveNextAsync task with OperationCanceledException
        // at the next suspension boundary." Resume is the SOLE trampoline for
        // both cases: MoveNextAsync calls it directly (the "on entry" call), and
        // the bz_task_then completion callback of every internal `await` calls it
        // again to continue the advance (the "at each resume from suspension"
        // call) — so one check here, before the state dispatch below, covers
        // both. On cancellation the in-flight per-advance task is faulted with a
        // fresh OperationCanceledException and STATE IS LEFT UNTOUCHED: the
        // pending using-var/finally cleanup for whichever suspension point the
        // frame is currently parked at is the consumer's Dispose() responsibility
        // when it unwinds on the faulted task (§29.7) — exactly the same
        // abandonment path a `break`/exception exit already takes, not a second,
        // divergent cleanup written here.
        {
            llvm::Value* tokenFieldGEP =
                builder_.CreateStructGEP(stateStructTy, self, TOKEN_FIELD);
            llvm::Value* tokenLoaded =
                builder_.CreateLoad(ptrTy(), tokenFieldGEP, "asynciter.tok");
            auto* isCancFn = declareC("bz_ctoken_is_cancelled", i32(), {ptrTy()});
            llvm::Value* isCancelled = builder_.CreateICmpNE(
                builder_.CreateCall(isCancFn, {tokenLoaded}, "asynciter.tok.canc"),
                builder_.getInt32(0));
            auto* tokCancelBB =
                llvm::BasicBlock::Create(llvm_, "asynciter.tokcancel", resumeFn);
            auto* tokProceedBB =
                llvm::BasicBlock::Create(llvm_, "asynciter.tokproceed", resumeFn);
            builder_.CreateCondBr(isCancelled, tokCancelBB, tokProceedBB);

            builder_.SetInsertPoint(tokCancelBB);
            llvm::Value* advTaskField = builder_.CreateStructGEP(
                stateStructTy, self, PER_ADVANCE_TASK_FIELD);
            llvm::Value* advTask =
                builder_.CreateLoad(ptrTy(), advTaskField, "asynciter.tok.task");
            auto ceVit = builtinVtable_.find("OperationCanceledException");
            if (ceVit != builtinVtable_.end()) {
                llvm::Value* oceObj = builder_.CreateCall(
                    callocFn(),
                    {builder_.getInt64(1), builder_.getInt64(kExcObjSize)},
                    "asynciter.tok.oce");
                builder_.CreateStore(ceVit->second, oceObj);
                auto* completeErrFn = declareC(
                    "bz_task_complete_error_obj", llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy()});
                builder_.CreateCall(completeErrFn, {advTask, oceObj});
            }
            builder_.CreateRetVoid();

            builder_.SetInsertPoint(tokProceedBB);
        }

        llvm::Value* stateGEP =
            builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);

        // The resume switch state count = #yields + #awaits (every suspend point
        // is a state). collectYields / countAwaits count each kind.
        std::vector<const ast::YieldStmt*> yields;
        collectYields(*ci.stmts, yields);
        unsigned numSuspends = (unsigned)yields.size() + countAwaits(*ci.stmts);

        // Yield-specific live state for the YieldStmt handler (lowerStmt).
        iterCurrentValField_ = CURRENT_VAL_FIELD;
        iterElemTy_ = elemTy;
        iterElemSemaTy_ = elemSemaTy;  // #1102B (nullable-value element coercion)

        // Publish the live sm*_ state BEFORE emitResumeDispatch so the emitDone
        // lambda below (which emitResumeDispatch runs before it itself publishes
        // these) can use emitAsyncIterComplete. emitResumeDispatch re-publishes
        // the same values, so this is harmless.
        smStateStruct_ = stateStructTy;
        smSelf_ = self;
        smStateGEP_ = stateGEP;

        // fix #2627 / §29.1 v0.236: on every async-iterator exhaustion transition
        // — the resume-dispatch "Done" lambda (state >= numSuspends, hit on any
        // subsequent MoveNextAsync after exhaustion), the fault path (uncaught
        // throw / faulted await in the body), AND the body-runs-off-its-end path
        // — dispose-then-null current_val BEFORE marking state -1 so a later
        // TakeCurrent() sees null and raises InvalidOperationException per the
        // §29.1 owning contract (transfers only at TakeCurrent). Mirrors the
        // sync head's exhaustion pass (#1349 + #2616, LlvmBackend_Iterators.cpp)
        // and the async Dispose's #2591 disposeYieldedVal (this file at
        // emitFreeAndReturn): the enumerator owns the yielded value until
        // TakeCurrent transfers it; if the consumer abandoned the protocol
        // (MoveNextAsync false without a prior TakeCurrent, or faulted before
        // TakeCurrent, or body returned off the end without the consumer taking
        // the last value), the value is leaked if we just null — so dispose it
        // first. Null-guarded: TakeCurrent-nulled slot is a no-op. Only fires
        // for IOwningAsyncEnumerable<T> / IOwningAsyncEnumerator<T> with a
        // pointer element type — the same predicate as the async Dispose's
        // disposeYieldedVal; the async frame carries no separate
        // current_val_owned flag (collapsed into the owning-form dispose under
        // #2591). Exactly-once: the abandon-then-Dispose pass at #2591 and
        // these new exhaustion passes are all dispose-then-null under the same
        // null guard, so whichever runs first leaves the slot null for the
        // other.
        auto disposeCurrentValOnExhaust = [&]() {
            if (!isOwningIter || !elemTy->isPointerTy()) return;
            llvm::Value* cvField = builder_.CreateStructGEP(
                stateStructTy, self, CURRENT_VAL_FIELD);
            llvm::Value* yv = builder_.CreateLoad(elemTy, cvField, "aity.ex.yv");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                yv, llvm::ConstantPointerNull::get(ptrTy()), "aity.ex.yvnull");
            auto* nonNullBB = llvm::BasicBlock::Create(
                llvm_, "aity.ex.yv.nonnull", resumeFn);
            auto* contBB = llvm::BasicBlock::Create(
                llvm_, "aity.ex.yv.cont", resumeFn);
            builder_.CreateCondBr(isNull, contBB, nonNullBB);
            builder_.SetInsertPoint(nonNullBB);
            emitDisposeAndReclaim(yv);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), cvField);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        };

        // "Done" (exhausted / fall-off / state -1): complete the per-advance task
        // with FALSE and return. The frame is NOT freed here (it persists for the
        // next MoveNextAsync / until Dispose).
        std::vector<llvm::BasicBlock*> stateBBs = emitResumeDispatch(
            frame, self, stateGEP, numSuspends,
            [&]() {
                emitAsyncIterComplete(/*hasValue=*/false);
                // fix #2627 / §29.1 v0.236: dispose-then-null current_val before
                // marking state -1 so a later TakeCurrent() raises IOE. Defensive
                // here — every transition into state -1 below already nulls the
                // slot, but the "Done" lambda also fires on subsequent
                // MoveNextAsync calls past the original exhaustion site, and a
                // stale current_val would re-hand back the old value. Null-
                // guarded; under the dominant `await for` protocol current_val
                // is already null and this is a no-op.
                disposeCurrentValOnExhaust();
                // Mark state -1 so a further MoveNextAsync stays exhausted.
                builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
                builder_.CreateRetVoid();
            });

        // §30.9: the fault boundary for an uncaught throw / faulted await in the
        // iterator body — complete the per-advance task with the error object so
        // the consuming `await for` re-raises it, then return. (No nearer try
        // handler installs a closer unwindTarget_.)
        auto* faultBB =
            llvm::BasicBlock::Create(llvm_, "asynciter.fault", resumeFn);
        unwindTarget_ = faultBB;
        {
            llvm::IRBuilder<>::InsertPointGuard guard(builder_);
            builder_.SetInsertPoint(faultBB);
            llvm::Value* taskField = builder_.CreateStructGEP(
                stateStructTy, self, PER_ADVANCE_TASK_FIELD);
            llvm::Value* task = builder_.CreateLoad(ptrTy(), taskField);
            llvm::Value* excObj = builder_.CreateLoad(
                ptrTy(), emitExcSlot(), "asynciter.fault.exc");
            auto* completeErrObjFn = declareC(
                "bz_task_complete_error_obj", llvm::Type::getVoidTy(llvm_),
                {ptrTy(), ptrTy()});
            builder_.CreateCall(completeErrObjFn, {task, excObj});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 emitExcSlot());
            // fix #2627 / §29.1 v0.236: dispose-then-null current_val before
            // marking state -1 — a fault in the body may have left a pending
            // yielded value (the last `yield return` before the throw) that the
            // consumer never took. Without this null the post-fault TakeCurrent
            // returns the stale value instead of raising IOE.
            disposeCurrentValOnExhaust();
            // Stay exhausted on any later advance.
            builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
            builder_.CreateRetVoid();
        }

        builder_.SetInsertPoint(stateBBs[0]);
        disposeStack_.emplace_back();
        bool term = lowerBody(*ci.stmts);
        if (!term && !builder_.GetInsertBlock()->getTerminator()) {
            // #821 / §29.7 (async): on NORMAL completion (the async iterator body
            // runs off its end), dispose the `using var` resources still active
            // at the end before reporting done. The abandonment path — Dispose
            // while suspended at a yield — is handled by smIterDisposalsAtState_
            // in the Dispose body below. State is set to -1 so a later advance /
            // Dispose stays a no-op → each resource is disposed exactly once.
            emitDisposals(disposeStack_.back());
            // fix #2627 / §29.1 v0.236: dispose-then-null current_val before
            // marking state -1 on normal completion. Reachable via the manual
            // protocol (the dominant `await for` always TakeCurrent's the last
            // value so current_val is already null and this is a no-op) — when
            // the consumer abandons the last value and drives to exhaustion,
            // current_val holds a stale owned value that would otherwise leak
            // (per #2616) AND be re-handed-out by a subsequent TakeCurrent.
            disposeCurrentValOnExhaust();
            emitAsyncIterComplete(/*hasValue=*/false);
            builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
            builder_.CreateRetVoid();
        }
        disposeStack_.pop_back();

        // §28.12 / §30: terminate any orphaned await-using state blocks (same as
        // emitAsyncMethod above — sync-fallback `await using` that did not consume
        // its pre-allocated block).
        if (smDoneBB_) {
            for (unsigned s = smNextSuspendState_ + 1; s < smStateBBs_.size(); ++s) {
                auto* orphanBB = smStateBBs_[s];
                if (orphanBB && !orphanBB->getTerminator()) {
                    llvm::IRBuilder<> tmp(orphanBB);
                    tmp.CreateBr(smDoneBB_);
                }
            }
        }

        // Repair SSA dominance for awaits embedded in larger expressions (e.g.
        // `yield return await Compute()`), promoting cross-suspend temporaries
        // into the heap spill area so they survive a real cross-thread suspend.
        fixupSuspendDominance(resumeFn);

        smStateStruct_ = nullptr;
        smSelf_ = nullptr;
        smLocalFields_.clear();
        smAllocaToField_.clear();
        asyncResumeFn_ = nullptr;
        asyncResultTy_ = nullptr;
    }

    // --- Dispose(ptr self) -> void (real body, §29.7 #623) ---
    // Emitted after Resume so smIterFinallyAtState_ is populated.
    // §29.7: runs finally clauses active at the suspension point, then frees
    // the spill area and the frame.  Finally bodies are run synchronously
    // (await-in-finally-on-Dispose is not yet supported; §29.7 allows the
    // sync-finally subset — an await-in-Dispose corner is reported separately).
    {
        currentFn_ = disposeFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        finallyBodyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;

        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", disposeFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = &*disposeFn->arg_begin();

        // fix #2612 / §28.2 / §29.7 (v0.236): idempotent disposal — the ENTRY
        // GUARD, the async sibling of the sync iterator Dispose's (#1107 rework,
        // LlvmBackend_Iterators.cpp). §28.2 models idempotency as a
        // "per-instance disposed flag"; as on the sync path we encode that flag
        // as a distinct STATE SENTINEL that ONLY Dispose ever writes, and check
        // it before any cleanup runs, so a Dispose re-entered from within its
        // own cleanup (a finally body, or an owned param/local destructor, that
        // reaches this same frame) returns immediately instead of running the
        // sweeps a second time and, at the end, free()ing the spill area and the
        // frame AGAIN — a double-free of the frame.
        //
        // The sentinel MUST be distinct from `-1`. The pre-fix code stored `-1`
        // here and its comment claimed that made "re-entrant Dispose a no-op",
        // but (a) nothing ever tested it, and (b) `-1` is also what Resume
        // writes on natural exhaustion (the stores in emitResumeDispatch's done
        // edges), which means "iteration complete, frame still allocated and
        // STILL OWES DISPOSAL" — so guarding on `-1` would make the FIRST,
        // legitimate Dispose after full consumption no-op before the owned-param
        // (§29.8) / owned-local (§29.7) / yielded-value (§29.1) sweeps and the
        // frame free, turning this fix into a leak. That is exactly the
        // mis-predication #1107 shipped and the #1107 rework corrected on the
        // sync path; `-2` is taken from there verbatim. The async state space is
        // otherwise 0..N (suspension states), `-1` (exhausted) and `-3` (an
        // owned-param source claimed by GetAsyncEnumerator), so `-2` collides
        // with nothing and no live frame can present it before its first
        // Dispose.
        //
        // SCOPE, stated plainly: this closes the RE-ENTRANT window — a nested
        // Dispose that arrives while the first is still running, before the
        // frame is freed. It does NOT make a SECOND, sequential Dispose safe:
        // the first one free()s the frame, so a later call reads freed memory
        // whatever it finds there. The sync guard has the same limit. Both
        // sequential-double-dispose shapes reachable from source today (an
        // `await using var` binding plus `await for` over the same enumerator;
        // an explicit `.Dispose()` plus the scope-end disposal) are already
        // handled upstream by the call-site null guard — verified under ASan.
        {
            llvm::Value* stateGEP0 = builder_.CreateStructGEP(
                stateStructTy, self, STATE_FIELD);
            llvm::Value* curState0 =
                builder_.CreateLoad(i32(), stateGEP0, "dsp.entrystate");
            llvm::Value* isDisposed = builder_.CreateICmpEQ(
                curState0, builder_.getInt32(-2), "dsp.isdisposed");
            auto* earlyRet =
                llvm::BasicBlock::Create(llvm_, "dsp.idem", disposeFn);
            auto* proceed =
                llvm::BasicBlock::Create(llvm_, "dsp.proceed", disposeFn);
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

        // §29.7 (#2574): load the suspension state ONCE, before any branch
        // sets it to -1, so disposeOwnedLocals (inside emitFreeAndReturn) can
        // test the live window (establishedAtState <= state < transferredAtState).
        llvm::Value* disposeState = builder_.CreateLoad(
            i32(),
            builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD),
            "dsp.curstate");

        auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

        auto emitFreeAndReturn = [&]() {
            // §29.1 v0.236 / #2591: the async sibling of the sync iterator's
            // disposeYieldedVal (§29 / #1120, LlvmBackend_Iterators.cpp:1719).
            // For an IOwningAsyncEnumerable<T> / IOwningAsyncEnumerator<T>
            // iterator whose element type is a reference type, current_val holds
            // the last yielded OWNED value. TakeCurrent (vtable slot 3, the
            // Current path above) nulls the slot when the consumer takes the
            // value, so a NON-NULL current_val at Dispose time means the
            // consumer abandoned after a successful MoveNextAsync WITHOUT
            // calling TakeCurrent — the §29.1 "advanced-but-untaken" pending
            // value. Dispose it exactly once (null-guarded) and null the slot.
            // Mirrors the sync isOwningIter branch (unconditional dispose of a
            // non-null current_val): the async frame carries no current_val_owned
            // flag (there is no IAsyncEnumerable<owned T> flag form — only the
            // owning interfaces transfer ownership per §29.1), so only the
            // owning form (isOwningIter) with a pointer element type is handled.
            // Null-guarded so full consumption (TakeCurrent already nulled the
            // slot) and a re-entrant Dispose are no-ops — never a double-free.
            // Runs BEFORE the owned-param/local sweeps, matching the sync
            // Dispose's ordering (disposeYieldedVal → disposeOwnedParams → …);
            // current_val is a dedicated trailing field that never aliases any
            // param/local slot, so the ordering introduces no double-free.
            if (isOwningIter && elemTy->isPointerTy()) {
                llvm::Value* cvField = builder_.CreateStructGEP(
                    stateStructTy, self, CURRENT_VAL_FIELD);
                llvm::Value* yv = builder_.CreateLoad(elemTy, cvField, "aity.yv");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    yv, llvm::ConstantPointerNull::get(ptrTy()), "aity.yvnull");
                auto* nonNullBB = llvm::BasicBlock::Create(
                    llvm_, "aity.yv.nonnull", disposeFn);
                auto* contBB = llvm::BasicBlock::Create(
                    llvm_, "aity.yv.cont", disposeFn);
                builder_.CreateCondBr(isNull, contBB, nonNullBB);
                builder_.SetInsertPoint(nonNullBB);
                emitDisposeAndReclaim(yv);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), cvField);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            // §29.8 (#973b): dispose never-discharged `owned` parameters captured
            // in the frame BEFORE freeing it — the async analogue of the sync
            // iterator's disposeOwnedParams (LlvmBackend_Iterators.cpp). Runs on
            // EVERY Dispose path (both call sites funnel here), AFTER any finally
            // chains, reverse capture order. Null-guarded and the field is nulled
            // after disposal so a re-entrant Dispose (or a param already moved out
            // and nulled) never double-frees. Only params flagged
            // iteratorAutoDispose (never discharged on any path, set in
            // Ownership.cpp) are disposed.
            //
            // #973a: `iteratorFrameConditionalAutoDispose` extends the same loop
            // to a CONDITIONALLY discharged parameter. Nothing else changes —
            // the null-guard IS the runtime moved-from test: the marked-adoption
            // lowering nulls this very slot
            // (Emitter::nullStateMachineOwnedParamCapture), so the adopting path
            // loads null and skips while the fall-through path disposes exactly
            // once. fix #2356 / §29.8: an owned-param async iterator's
            // GetAsyncEnumerator (vtable slot 4, above) now DOES clone — the
            // first call transfers every owned-param capture into the clone
            // and NULLS `self`'s own copies, so this sweep is a genuine no-op
            // when it later runs on the (inert, never-driven) SOURCE frame,
            // and disposes the still-live captures when it runs on the CLONE
            // (the frame the body actually ran on). Both cases are exactly-
            // once-safe through the same null guard; no clone/source
            // distinction is needed here because the transfer already made
            // "self's own copy" the single source of truth for whichever
            // frame this call is disposing.
            for (std::size_t i = frame.captures.size(); i-- > 0;) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!ps || (!ps->iteratorAutoDispose &&
                            !ps->iteratorFrameConditionalAutoDispose))
                    continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                llvm::Value* fptr =
                    builder_.CreateStructGEP(stateStructTy, self, fieldIdx);
                llvm::Value* obj =
                    builder_.CreateLoad(ptrTy(), fptr, "aitp.obj");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()), "aitp.isnull");
                auto* dispBB =
                    llvm::BasicBlock::Create(llvm_, "aitp.dispose", disposeFn);
                auto* contBB =
                    llvm::BasicBlock::Create(llvm_, "aitp.cont", disposeFn);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                // fix #3125 (§29.8 / §30.5, v0.254): as in the plain-async
                // frame's emitAsyncOwnedParamAutoDispose — a Task-family
                // capture is released by the §30.5 abandonment protocol, not
                // by class-vtable dispatch (measured: SEGV in
                // bz$asynciter$Gen$Dispose). One shared routing decision.
                emitOwnedParamCaptureDispose(frame.captures[i].first, obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), fptr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            // §29.7 (#2574): the LOCAL counterpart of the owned-parameter
            // sweep above — the async sibling of the sync iterator's
            // disposeOwnedLocals (#2306, LlvmBackend_Iterators.cpp). For each
            // plain `var L` local of an owned-class type whose pre-pass
            // produced a (establishedAtState, transferredAtState, frameField)
            // tuple, dispose L's frame slot iff the current state is in L's
            // live window:
            //   establishedAtState <= state < transferredAtState
            // The null guard handles "never reached (state 0)" — the calloc-
            // zeroed slot is null because MoveNextAsync never ran past the
            // declaration. Reverse establishment order matches §28.4's
            // reverse-declaration destructor rule. Runs AFTER owned-param
            // disposal and BEFORE the spill/frame free — no double-free with
            // anything downstream. Only registered locals are visited, so the
            // loop is a no-op (zero IR) for body shapes the pre-pass could
            // not statically pin — preserving the existing bounded leak for
            // those shapes rather than risking a double-free.
            if (!asyncOwnedLocalDisposals.empty()) {
                for (auto bit = frame.bodyLocals.rbegin();
                     bit != frame.bodyLocals.rend(); ++bit) {
                    const ast::Node* node = bit->first;
                    auto* ld = dynamic_cast<const ast::LocalDecl*>(node);
                    if (!ld) continue;
                    auto oit = asyncOwnedLocalDisposals.find(ld);
                    if (oit == asyncOwnedLocalDisposals.end()) continue;
                    const AsyncLocalKeyedDisposal& info = oit->second;
                    if (info.establishedAtState == 0 ||
                        info.transferredAtState == 0 ||
                        info.frameField == 0)
                        continue;
                    unsigned fieldIdx = info.frameField;
                    llvm::Value* fptr = builder_.CreateStructGEP(
                        stateStructTy, self, fieldIdx);
                    llvm::Value* obj = builder_.CreateLoad(
                        ptrTy(), fptr, "aitl.obj");
                    llvm::Value* nonNull = builder_.CreateICmpNE(
                        obj, llvm::ConstantPointerNull::get(ptrTy()),
                        "aitl.nonnull");
                    llvm::Value* established = builder_.CreateICmpSGE(
                        disposeState,
                        builder_.getInt32((int)info.establishedAtState),
                        "aitl.estab");
                    llvm::Value* beforeMove = builder_.CreateICmpSLT(
                        disposeState,
                        builder_.getInt32((int)info.transferredAtState),
                        "aitl.beforemove");
                    llvm::Value* shouldDispose = builder_.CreateAnd(
                        nonNull,
                        builder_.CreateAnd(established, beforeMove),
                        "aitl.shoulddispose");
                    auto* ldispBB = llvm::BasicBlock::Create(
                        llvm_, "aitl.dispose", disposeFn);
                    auto* lcontBB = llvm::BasicBlock::Create(
                        llvm_, "aitl.cont", disposeFn);
                    builder_.CreateCondBr(shouldDispose, ldispBB, lcontBB);
                    builder_.SetInsertPoint(ldispBB);
                    emitDisposeAndReclaim(obj);
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), fptr);
                    builder_.CreateBr(lcontBB);
                    builder_.SetInsertPoint(lcontBB);
                }
            }
            // fix #2609 / §27.5 / §28.9 (v0.236): drop THIS frame's +1 on each
            // function-typed (`func`) parameter capture — the async sibling of
            // the sync iterator's releaseClosureCaptures (#1864,
            // LlvmBackend_Iterators.cpp:1715). Paired 1:1 with the two retains
            // above (construction entry for the source frame, GetAsyncEnumerator
            // for each clone), so every frame that reaches Dispose releases
            // exactly the one reference it took. Runs AFTER the owned-param and
            // owned-local sweeps and BEFORE the spill/frame free, matching the
            // sync Dispose's ordering. The field is NOT nulled: unlike an owned
            // object, a released closure cell may still be live (another frame
            // or the caller can hold a reference) and the slot is dead the
            // instant the frame is freed on the next two lines. Owned-class
            // params (the iteratorAutoDispose loop above) and function-typed
            // params are DISJOINT sets — an `owned` parameter has a Named class
            // type, never Kind::Function — so the two loops never touch the same
            // field. bz_closure_release is null-safe, so a capture the frame
            // never received (null slot) is a no-op.
            for (std::size_t i = frame.captures.size(); i-- > 0;) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!ps) continue;                   // 'this' capture — skip
                sema::TypeRef* pt = paramTypeOf(ps);
                if (!pt || pt->kind != sema::TypeRef::Kind::Function) continue;
                unsigned fieldIdx = frame.captureStart + (unsigned)i;
                llvm::Value* fptr =
                    builder_.CreateStructGEP(stateStructTy, self, fieldIdx);
                llvm::Value* cl = builder_.CreateLoad(ptrTy(), fptr, "aitc.cl");
                emitClosureRelease(cl);   // null-safe
            }
            // Free the spill area first (null-safe: free(null) is a no-op).
            llvm::Value* spillField2 =
                builder_.CreateStructGEP(stateStructTy, self, SPILL_PTR_FIELD);
            llvm::Value* spillPtr2 = builder_.CreateLoad(ptrTy(), spillField2);
            builder_.CreateCall(freeFn2, {spillPtr2});
            builder_.CreateCall(freeFn2, {self});
            builder_.CreateRetVoid();
        };

        // fix #1598 / §29.7/§28.10 (v0.182): the async iterator Dispose now
        // consumes the SAME unified establishment-ordered cleanup stack the
        // sync iterator Dispose consumes (fix #1567, LlvmBackend_Iterators.cpp)
        // — `using var` disposals, for-in enumerator-frame disposals, and
        // `finally` bodies interleaved by establishment position, run in exact
        // reverse. The pre-v0.182 code here ran ALL using-var disposals
        // (smIterDisposalsAtState_), THEN all finally bodies
        // (smIterFinallyAtState_), as two category-ordered phases — the same
        // inversion #1567 fixed for the sync path: a finally that touched an
        // enclosing `using var` established after it saw the var already
        // disposed. That code also never disposed live for-in enumerator/
        // source frames (smIterEnumFramesAtState_) at all on this path — a
        // separate latent leak; smIterCleanupAtState_ is the superset of all
        // three, so consuming it closes both defects together.
        if (smIterCleanupAtState_.empty()) {
            // No yields inside try/finally and no active using-var/enum-frame
            // cleanup — mark done and free.
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            // fix #2612: the DISPOSED sentinel (-2), not the exhaustion value
            // (-1) — see the entry guard above for why they must differ.
            builder_.CreateStore(builder_.getInt32(-2), stateGEP2);
            emitFreeAndReturn();
        } else {
            auto* doneBB = llvm::BasicBlock::Create(llvm_, "dsp.done", disposeFn);
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP2);
            // Mark done immediately — BEFORE the finally chains run — so a
            // Dispose re-entered from inside one of them no-ops at the entry
            // guard. fix #2612: the DISPOSED sentinel (-2), not the exhaustion
            // value (-1). The switch below dispatches on the already-loaded
            // `curState`, so overwriting the field here is safe.
            builder_.CreateStore(builder_.getInt32(-2), stateGEP2);
            std::set<unsigned> states;
            for (auto& kv : smIterCleanupAtState_) states.insert(kv.first);
            auto* sw = builder_.CreateSwitch(curState, doneBB,
                                             (unsigned)states.size());

            for (unsigned stateIdx : states) {
                auto* chainBB = llvm::BasicBlock::Create(
                    llvm_, "dsp.s" + std::to_string(stateIdx), disposeFn);
                sw->addCase(builder_.getInt32((int)stateIdx), chainBB);
                builder_.SetInsertPoint(chainBB);
                // fix #1598 / §29.7/§28.10 (v0.182): run the ONE establishment-
                // ordered cleanup stack active at this yield in exact REVERSE,
                // mirroring the sync iterator Dispose.
                auto cit = smIterCleanupAtState_.find(stateIdx);
                if (cit != smIterCleanupAtState_.end()) {
                    for (auto rit = cit->second.rbegin();
                         rit != cit->second.rend(); ++rit) {
                        const IterCleanup& ac = *rit;
                        if (ac.kind == IterCleanup::Kind::Disposal) {
                            // #821 / §29.7: rebind the Disposal's slot to the
                            // Dispose-restored local and run the normal disposal.
                            auto lit = locals_.find(ac.node);
                            if (lit == locals_.end()) continue;
                            Disposal d = ac.disposal;
                            d.slot = lit->second;
                            std::vector<Disposal> one{d};
                            emitDisposals(one);
                        } else if (ac.kind == IterCleanup::Kind::EnumFrame) {
                            // §29.7 (#944): dispose a for-in implicit enumerator/
                            // source frame — a frame ptr in a dedicated field.
                            unsigned fieldIdx = ac.frameField;
                            llvm::Value* fptr = builder_.CreateStructGEP(
                                stateStructTy, self, fieldIdx);
                            llvm::Value* obj =
                                builder_.CreateLoad(ptrTy(), fptr, "fie.obj");
                            llvm::Value* isNull = builder_.CreateICmpEQ(
                                obj, llvm::ConstantPointerNull::get(ptrTy()),
                                "fie.isnull");
                            auto* dispBB = llvm::BasicBlock::Create(
                                llvm_, "fie.dispose", disposeFn);
                            auto* contBB = llvm::BasicBlock::Create(
                                llvm_, "fie.cont", disposeFn);
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
                            // fix #2853 / §29.7 / §30.7 / §28.10 / §4.6 / §28.9
                            // (v0.240): the ASYNC-TWIN of the sync-iterator fix
                            // at `LlvmBackend_Iterators.cpp` —
                            // `stringReleaseEligibleFn_` is pinned to the
                            // RESUME fn here (line above, #2817), and the
                            // `finally` body REPLAYED in this Dispose fn was
                            // lowered with `currentFn_` set to `disposeFn`
                            // (just above), so without re-pinning the token
                            // for the replay every `var` string local
                            // declared inside a finally body in an async
                            // iterator leaked. Mirror the sync fix exactly:
                            // save / analyze / pin / lower / filter-and-emit
                            // / restore. // per §29.7, §30.7.
                            llvm::Function* savedStringReleaseEligibleFn =
                                stringReleaseEligibleFn_;
                            collectStringReleaseEligible(*ac.finallyBody);
                            stringReleaseEligibleFn_ = disposeFn;
                            disposeStack_.emplace_back();
                            lowerBody(*ac.finallyBody);
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
            emitFreeAndReturn();
        }

        smIterFinallyAtState_.clear();
        smIterDisposalsAtState_.clear();  // #821
        smIterEnumFramesAtState_.clear();  // §29.7 (#944)
        smIterCleanupAtState_.clear();     // fix #1567 §29.7/§28.10 (v0.182)
        smIterEnumFrameSlot_.clear();      // §29.7 (#944)
        finallyBodyStack_.clear();
    }
}


// §30.7: complete the in-flight async iterator's PER-ADVANCE Task<bool> with the
// given boolean (true = a value was yielded and is in current_val; false =
// exhausted). Unlike emitAsyncComplete (plain async), this does NOT free the
// frame — the frame persists across MoveNextAsync calls until Dispose. The bool
// is copied by the runtime (1 byte).
void Emitter::emitAsyncIterComplete(bool hasValue) {
    llvm::Value* self = smSelf_;
    if (!self || !smStateStruct_) return;
    llvm::Value* taskField =
        builder_.CreateStructGEP(smStateStruct_, self, asyncOuterTaskField_);
    llvm::Value* task = builder_.CreateLoad(ptrTy(), taskField);
    auto* completeFn = declareC("bz_task_complete", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64()});
    llvm::AllocaInst* slot = entryAlloca(i1());
    builder_.CreateStore(
        hasValue ? builder_.getTrue() : builder_.getFalse(), slot);
    uint64_t sz = module_->getDataLayout().getTypeAllocSize(i1());
    builder_.CreateCall(completeFn, {task, slot, builder_.getInt64((int64_t)sz)});
}


llvm::Value* Emitter::lowerAwait(const ast::UnaryExpr& un) {
    // Defensive: `await` outside an async state machine is rejected by sema; if
    // we somehow reach here without the async frame live, produce nothing.
    // §30.7: an async iterator (AsyncYield) also lowers `await` suspend points
    // in its MoveNextAsync resume function, so both kinds are valid here.
    if ((smKind_ != SuspendKind::Await && smKind_ != SuspendKind::AsyncYield) ||
        !smStateStruct_ || !smSelf_ || !asyncResumeFn_)
        return nullptr;
    if (!un.operand) return nullptr;

    // Lower the operand to the awaited task pointer.
    llvm::Value* task = lowerExpr(*un.operand);
    if (!task) return nullptr;

    // The await result type (§30.3 / §30.5): T for Task<T>/OwningTask<T>, or
    // unit for `await Task`. sema set this on the UnaryExpr.
    llvm::Type* resultTy = nullptr;
    if (auto* rt = ctx_.exprType(un)) {
        if (rt->kind != sema::TypeRef::Kind::Unit)
            resultTy = lowerTypeRef(rt);
    }

    // §30.5: reclaim the awaited task at the await site when its operand is an
    // anonymous producer result (`await Producer()`) — a single-use task with no
    // other owner. A task bound to a local is left to its binder's disposal.
    // §30.5 / #1332 (v0.162): if this await is a Pattern A chained return
    // (asyncChainedReturn_ set by lowerStmt for `return await <call>` with a
    // pointer result type), do NOT free the source task — lowerStmt will chain
    // it to the outer task after the await completes.
    bool freeTaskAfter =
        dynamic_cast<const ast::CallExpr*>(un.operand.get()) != nullptr &&
        !asyncChainedReturn_;
    // §28.3 / #1250: for a BOUND OwningTask<T:class> await, null the result buffer
    // after loading so bz_tcs_dispose_owned_result (called at TCS disposal) sees no
    // result and skips the dispose — preventing a double-free of the caller-owned obj.
    // Only fires for OwningTask (not plain Task) with a ClassOwned element type.
    bool takeResultAfter = false;
    // §30.5 (v0.162 / #1352): OwningTask single-consumer tracking. After the first
    // await (success or fault), mark the task as spent so a second await raises
    // InvalidOperationException. Only fires for BOUND OwningTask (!freeTaskAfter):
    // an anonymous inline `await Producer()` (freeTaskAfter=true) is freed
    // immediately after the first await, so no second await is possible and
    // calling mark_spent after free would be UAF. Plain Task<T> is re-awaitable
    // and must NOT be marked spent.
    bool markSpentAfter = false;
    {
        const sema::TypeRef* taskTy = ctx_.exprType(*un.operand);
        bool isOwnedTask = (taskTy && taskTy->kind == sema::TypeRef::Kind::Named &&
            !taskTy->declaration && taskTy->astNode && taskTy->astNode->name &&
            !taskTy->astNode->name->parts.empty() &&
            taskTy->headName() == "OwningTask");
        if (isOwnedTask && !freeTaskAfter) {
            markSpentAfter = true;
            if (!taskTy->genericArgs.empty() && taskTy->genericArgs[0]) {
                takeResultAfter =
                    (elementPolicyFor(taskTy->genericArgs[0], /*owningFamily=*/true).cat ==
                     ElementPolicy::Category::ClassOwned);
            }
        }
    }
    // fix #3070 / §30.5 + §28.3 *Unbound owned temporaries* (v0.249): when the
    // anonymous producer's task is Pattern A `Task<T>` for a class (or intrinsic
    // collection) T, the awaited value is a BORROW of the task's result and
    // bz_task_free cascades into the per-`T` result disposer (#1503) — so freeing
    // at the await destroys the borrow §30.5 says is live "for the duration of
    // the call". Defer that reclaim to K8's full-expression boundary.
    //
    // The predicate is patternAResultDisposerFor itself, not a re-derivation of
    // it: it is the ONE place that decides "disposing this task disposes its
    // result", and asking it directly is what keeps the two sides from drifting
    // apart (the same drift that made the original comment here — "the value and
    // the task's freed result buffer do not alias afterwards" — true for `int`
    // and false for a class from #1503 onward).
    bool deferTaskFree = false;
    if (freeTaskAfter && resultTy)
        deferTaskFree =
            patternAResultDisposerFor(ctx_.exprType(*un.operand)) != nullptr;
    // fix #2750 / §30.5 Pattern A kind 2 chained return (v0.240): on the
    // SUCCESS path, lowerStmt chains the source task to the outer task via
    // bz_task_set_chained (so its destructor frees it transitively). The FAULT
    // path never reaches that code (control diverts to unwindBB() before any
    // return-statement lowering runs), so the source task leaks. Reclaim it
    // on the fault path. Only fires for the ANONYMOUS PRODUCER row
    // (CallExpr operand — a fresh `Producer()` result with no other owner).
    // The owned-LOCAL row (chainedReturnOwnedTaskSource) is reclaimed by
    // emitUnwindDisposals on the unwind edge; the owned-PARAMETER row
    // (chainedReturnOwnedTaskParamSource) by emitAsyncOwnedParamAutoDispose.
    // Reclaiming those rows here would double-free.
    bool freeOnFault = asyncChainedReturn_ &&
        dynamic_cast<const ast::CallExpr*>(un.operand.get()) != nullptr;
    return emitAwaitSuspend(task, resultTy, freeTaskAfter, takeResultAfter,
                            markSpentAfter, deferTaskFree, freeOnFault);
}


// §30.2 / §30.7: the reusable await suspend mechanism — shared by `lowerAwait`
// (an `await <task>` expression) and the `await for` desugaring (each iteration
// awaits the enumerator's MoveNextAsync() task). `task` is the awaited task
// pointer; `resultTy` is the lowered await-result type (null for a no-value
// `await Task`); `freeTaskAfter` reclaims the task once its result is read (true
// for anonymous single-use producers, e.g. `await Producer()` or a per-advance
// MoveNextAsync task). Emits the already-done fast path inline and the slow path
// that registers the resume continuation + returns; the resume / fast-path
// convergence reads the result and re-raises a faulted task (§30.9).
llvm::Value* Emitter::emitAwaitSuspend(llvm::Value* task, llvm::Type* resultTy,
                                       bool freeTaskAfter, bool takeResultAfter,
                                       bool markSpentAfter,
                                       bool deferTaskFree,
                                       bool freeOnFault) {
    if (!task) return nullptr;

    // Store the awaited task into the frame's pending-task slot so the resumed
    // state can read its result (the local `task` value does not survive a
    // suspend).
    llvm::Value* pendField =
        builder_.CreateStructGEP(smStateStruct_, smSelf_, asyncPendingTaskField_);
    builder_.CreateStore(task, pendField);

    // Spill all suspend-locals into the frame BEFORE the fast/slow branch, so
    // the frame is the canonical store across the await boundary on BOTH paths.
    // The resume block (the convergence point) restores from the frame
    // unconditionally; if the fast path skipped this spill, that restore would
    // clobber the live alloca with the frame's stale (zero) value. (This is the
    // analogue of the yield handler's pre-yield spill, but here it must precede
    // the already-done fast path too.)
    for (auto& [allocaSlot, fieldIdx] : smAllocaToField_) {
        llvm::Value* fptr =
            builder_.CreateStructGEP(smStateStruct_, smSelf_, fieldIdx);
        llvm::Value* curVal =
            builder_.CreateLoad(allocaSlot->getAllocatedType(), allocaSlot);
        builder_.CreateStore(curVal, fptr);
    }

    // Allocate the next state block — both the already-done fast path and the
    // resumed continuation converge here to read the result (§30.2).
    unsigned nextState = ++smNextSuspendState_;
    llvm::BasicBlock* resumeBB = (nextState < smStateBBs_.size())
                                     ? smStateBBs_[nextState]
                                     : llvm::BasicBlock::Create(
                                           llvm_, "await.resume", currentFn_);

    // Fast path test: bz_task_is_done(task)?
    auto* isDoneFn = declareC("bz_task_is_done", i1(), {ptrTy()});
    llvm::Value* done = builder_.CreateCall(isDoneFn, {task}, "await.done");

    auto* suspendBB = llvm::BasicBlock::Create(llvm_, "await.suspend", currentFn_);
    builder_.CreateCondBr(done, resumeBB, suspendBB);

    // --- Suspend path (§30.2: the task is running) ---
    builder_.SetInsertPoint(suspendBB);
    // Record the resume state index, register the continuation, and return.
    builder_.CreateStore(builder_.getInt32((int)nextState), smStateGEP_);
    auto* thenFn = declareC("bz_task_then", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(thenFn, {task, asyncResumeFn_, smSelf_});
    builder_.CreateRetVoid();

    // --- Resume / fast-path convergence: read the result (§30.2) ---
    builder_.SetInsertPoint(resumeBB);
    // Restore all suspend-locals from the frame (they were spilled above; on the
    // fast path they are simply re-loaded with the same values).
    for (auto& [allocaSlot, fieldIdx] : smAllocaToField_) {
        llvm::Value* fptr =
            builder_.CreateStructGEP(smStateStruct_, smSelf_, fieldIdx);
        llvm::Value* savedVal =
            builder_.CreateLoad(allocaSlot->getAllocatedType(), fptr);
        builder_.CreateStore(savedVal, allocaSlot);
    }
    // Reload the awaited task from the pending slot.
    llvm::Value* awaitedTask =
        builder_.CreateLoad(ptrTy(), builder_.CreateStructGEP(
                                         smStateStruct_, smSelf_,
                                         asyncPendingTaskField_));

    // §30.5 (v0.162 / #1352): OwningTask single-consumer guard — if a SECOND await
    // is attempted on a spent OwningTask (one already consumed on any path, including
    // a faulted/cancelled first await), raise InvalidOperationException instead of
    // reading freed or stale memory. markSpentAfter is true only for OwningTask.
    if (markSpentAfter) {
        auto* isSpentFn = declareC("bz_task_is_spent", i1(), {ptrTy()});
        llvm::Value* spent =
            builder_.CreateCall(isSpentFn, {awaitedTask}, "await.spent");
        // emitGuardThrow splits the block: true branch throws IOE + unwinds;
        // the insert point is left at the continuation (false branch).
        emitGuardThrow(spent, "InvalidOperationException");
    }

    // §30.9: awaiting a faulted task RE-RAISES the original exception "with the
    // original stack trace preserved" (spec §30.9). The state machine routes this
    // re-raise through the SAME unwind machinery a synchronous `throw` uses:
    // restore the awaited task's exception object into the bz$exc global, run the
    // intervening unwind disposals, and branch to the current unwindTarget_.
    //   - Inside a try BODY, unwindTarget_ is the try's catch.dispatch, so the
    //     fault is CAUGHT by the enclosing catch (and the finally runs) — §31.2.
    //   - Outside any try, unwindTarget_ is the resume function's async.fault
    //     boundary, which completes THIS outer task with the same exception
    //     object (bz_task_complete_error_obj) so the next awaiter re-raises it in
    //     turn; at the top (async Main) the process exits non-zero (§33.3).
    // Carrying the exception OBJECT (not just a message) lets a type-specific
    // catch arm match it (emitIsA needs the real object). Emitted unconditionally
    // so `await Task` (no value) re-raises a fault too.
    {
        auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
        llvm::Value* faulted =
            builder_.CreateCall(isErrFn, {awaitedTask}, "await.faulted");
        auto* faultBB = llvm::BasicBlock::Create(llvm_, "await.fault", currentFn_);
        auto* okBB = llvm::BasicBlock::Create(llvm_, "await.ok", currentFn_);
        builder_.CreateCondBr(faulted, faultBB, okBB);

        builder_.SetInsertPoint(faultBB);
        // Restore the awaited task's exception object into the in-flight slot.
        // The object is a Bezel heap exception independent of the task's own
        // buffers, so freeing the task below does not free it.
        auto* errObjFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
        llvm::Value* excObj =
            builder_.CreateCall(errObjFn, {awaitedTask}, "await.errobj");
        builder_.CreateStore(excObj, emitExcSlot());
        // §30.4 / §30.9 (#854): for a BOUND task (freeTaskAfter == false — the task
        // outlives this await and may be awaited AGAIN, §30.4), the re-raised
        // error_obj is BORROWED from the task; the catch arm that handles it must
        // NOT reclaim it, or a SECOND `await t` re-reads a freed pointer (SIGSEGV).
        // Mark it so the caught-exception disposal skips a borrowed object. For an
        // ANONYMOUS producer (`await Producer()`, freeTaskAfter == true) the task is
        // single-use and freed just below — normally the handler-owns behavior (no
        // mark) applies, so the lone awaiter reclaims error_obj exactly once.
        //
        // §30.10 / #1529 EXCEPTION: an anonymous-producer WhenAll/WhenAll<T>
        // AGGREGATE never owns its fault at all — it only borrows a reference
        // from whichever input task produced it (spec §30.10: "an aggregate
        // task owns no fault of its own"). That input task is still alive and
        // will dispose the SAME exception object at its own, independent scope
        // end, so the catch arm here must NOT reclaim it either — mark it
        // borrowed regardless of freeTaskAfter. bz_task_fault_borrowed is false
        // for every ordinarily-faulted task (the common case), so this adds one
        // runtime check without changing behavior there.
        {
            llvm::Value* shouldMark;
            if (!freeTaskAfter) {
                shouldMark = builder_.getTrue();
            } else {
                auto* fbFn = declareC("bz_task_fault_borrowed",
                                      llvm::Type::getInt8Ty(llvm_), {ptrTy()});
                llvm::Value* fb = builder_.CreateCall(
                    fbFn, {awaitedTask}, "await.faultborrowed");
                shouldMark = builder_.CreateICmpNE(
                    fb, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
            }
            auto* markBB = llvm::BasicBlock::Create(llvm_, "await.eo.mark",
                                                     currentFn_);
            auto* noMarkBB = llvm::BasicBlock::Create(llvm_, "await.eo.nomark",
                                                       currentFn_);
            builder_.CreateCondBr(shouldMark, markBB, noMarkBB);
            builder_.SetInsertPoint(markBB);
            auto* markFn = declareC("bz_mark_exc_borrowed",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(markFn, {excObj});
            builder_.CreateBr(noMarkBB);
            builder_.SetInsertPoint(noMarkBB);
        }
        // §30.5: reclaim the awaited task here when it is an anonymous producer
        // result (`await Producer()`) — a single-use task with no other owner.
        // bz_task_free frees the task struct + its result/message buffers but NOT
        // the exception object, which now lives on in bz$exc for the handler.
        if (freeTaskAfter) {
            auto* freeTaskFn = declareC("bz_task_free",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeTaskFn, {awaitedTask});
        }
        // fix #2750 / §30.5 Pattern A kind 2 chained return (v0.240): when
        // this await is the operand of `return await Producer()` (the
        // anonymous-producer row of the chained-slot table), the SUCCESS path
        // chains the source task to the outer task via bz_task_set_chained in
        // lowerStmt (so the outer task's destructor frees it transitively).
        // The FAULT path never reaches that code — control diverts to
        // unwindBB() (catch.dispatch inside a try, or async.fault otherwise)
        // before any return-statement lowering runs — so the source task leaks
        // unless we reclaim it here. Use the same null-after-free guard
        // pattern as #2733's ffiBuffer arm: reload from the frame slot, check
        // for null, free and null the slot. Idempotence is required because
        // bz_task_set_chained (called only on the success path) is not
        // reachable from here — the null-check is defensive against any
        // future code path that might race this reclaim with another. For the
        // owned-LOCAL / owned-PARAMETER rows of the chained-slot table,
        // freeOnFault is false: emitUnwindDisposals (local) and
        // emitAsyncOwnedParamAutoDispose (parameter) reclaim on the unwind
        // edge, and reclaiming here would double-free.
        if (freeOnFault) {
            llvm::Value* pendField = builder_.CreateStructGEP(
                smStateStruct_, smSelf_, asyncPendingTaskField_);
            llvm::Value* tk = builder_.CreateLoad(
                ptrTy(), pendField, "chain.fault.src");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tk, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB = llvm::BasicBlock::Create(
                llvm_, "chain.fault.free", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(
                llvm_, "chain.fault.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* freeTaskFn = declareC("bz_task_free",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeTaskFn, {tk});
            // fix #2733 / §30.5 null-after-free: clear the slot so any future
            // load sees null (defensive — this slot has no live reader on the
            // fault path, but the guard costs nothing and matches the
            // ffiBuffer / awaitTaskTemp shapes).
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), pendField);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        }
        // §30.5 (v0.162 / #1352): mark OwningTask spent BEFORE the unwind so any
        // catch arm that re-awaits the same variable gets IOE, not the re-raised
        // original exception. Applies on fault path regardless of freeTaskAfter.
        if (markSpentAfter) {
            auto* markSpentFn = declareC("bz_task_mark_spent",
                                         llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(markSpentFn, {awaitedTask});
        }
        // §28.3 / CG21: dispose the using-vars unwound between here and the
        // handler (a caught-exception entry is skipped — it stays in flight), the
        // same as emitThrowValue's synchronous-throw path, then branch to the
        // unwind target (catch.dispatch inside a try, or async.fault otherwise).
        emitUnwindDisposals();
        builder_.CreateBr(unwindBB());

        builder_.SetInsertPoint(okBB);
    }

    // Read the result. bz_task_await_sync returns a pointer to the result buffer
    // (already-complete → returns immediately). For `await Task` (no value) the
    // result is unit; return a null placeholder.
    auto* awaitFn = declareC("bz_task_await_sync", ptrTy(), {ptrTy()});
    llvm::Value* result = nullptr;
    if (!resultTy) {
        // Drain to ensure completion ordering even with no value.
        builder_.CreateCall(awaitFn, {awaitedTask});
        result = llvm::ConstantPointerNull::get(ptrTy());
    } else {
        llvm::Value* resPtr =
            builder_.CreateCall(awaitFn, {awaitedTask}, "await.result.ptr");
        result = builder_.CreateLoad(resultTy, resPtr, "await.result");
        // The result is loaded BY VALUE into the await site before the task is
        // freed below, so the value (and the task's freed result buffer) do not
        // alias afterwards.
        //
        // ⚠ fix #3070 (§30.5 / §28.3, v0.249): that reasoning holds ONLY for a
        // value / `string` / runtime-managed result. For a Pattern A CLASS result
        // the loaded "value" IS the heap pointer, and since #1503 bz_task_free
        // cascades into the per-`T` result disposer and frees the pointee — so
        // the load does NOT decouple them. `deferTaskFree` is exactly that row,
        // and moves the reclaim to the K8 boundary below.
        // §28.3 / #1250: for a BOUND OwningTask<T:class> await, null the result
        // buffer after loading (consumed-tracking). bz_tcs_dispose_owned_result
        // checks task->result; a null here tells it the result was already taken.
        if (takeResultAfter) {
            auto* takeResultFn = declareC("bz_task_take_result",
                                          llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(takeResultFn, {awaitedTask});
        }
    }
    // §30.5 (v0.162 / #1352): mark OwningTask spent after successful result
    // consumption so a second await gets IOE (spend-on-all-edges, not just fault).
    if (markSpentAfter) {
        auto* markSpentFn = declareC("bz_task_mark_spent",
                                     llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(markSpentFn, {awaitedTask});
    }
    // §30.5 (minimal): reclaim the awaited task when its operand is an anonymous
    // producer result (`await Producer()`) — a single-use task with no other
    // owner. A task bound to a local (`await t`, `using var t := ...`) is left to
    // its binder's disposal (the using-var / §30.5 disposal path), so we do NOT
    // free it here (that would double-free). This keeps the common
    // `await SomeAsyncCall()` chain leak-clean without risking the bound-task
    // case. (Multi-await Pattern A on a class-typed Task<T> is deferred; the
    // value-typed minimal cut's anonymous producer results are single-consumer.)
    if (freeTaskAfter) {
        if (deferTaskFree) {
            // fix #3070 / §30.5 + §28.3 (v0.249): a Pattern A class-typed result.
            // The await produced a BORROW of it, and §30.5 says the temporary
            // "is then disposed at the expression's end, AFTER EVERY USE of the
            // borrow". Park the task pointer in a slot, register the reclaim on
            // the cleanup stack (exception edge + backstop) and queue it for the
            // K8 boundary, exactly as registerFfiExprTemp does for an FFI buffer
            // temporary. The slot is frame-spilled because a LATER await in the
            // SAME full expression (`Use2(await P(), await Q())`) suspends
            // between the store here and the boundary read, and a resume may run
            // on a different thread's stack (§30.13).
            llvm::AllocaInst* taskSlot = entryAlloca(ptrTy());
            builder_.CreateStore(awaitedTask, taskSlot);
            registerAwaitedTaskTemp(taskSlot);
            if (smStateStruct_) asyncHandlerSpillSlots_.push_back(taskSlot);
        } else {
            auto* freeTaskFn = declareC("bz_task_free",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeTaskFn, {awaitedTask});
        }
    }
    return result;
}


// fix #3070 / §30.5 + §28.3 *Unbound owned temporaries* (v0.249): record an
// awaited Pattern A task temporary at its await's success edge. The two halves
// mirror registerFfiExprTemp (LlvmBackend_Calls.cpp) exactly, for the same two
// reasons:
//
// (1) An `awaitedTaskTemp` Disposal is pushed onto disposeStack_.back(). §28.3:
//     "every already-produced, not-yet-consumed unbound owned temporary is
//     disposed during unwinding … each temporary's disposal is established at
//     its production, so the pass runs in reverse order of production on the
//     establishment-ordered cleanup stack". That covers the exception edge — a
//     LATER operand of the same full expression that throws (`Use2(await P(),
//     Thrower())`) now reclaims this task instead of leaking it — and doubles as
//     the conservative backstop if no boundary flush ever reaches the temp: the
//     task is then freed at its own scope end, later than §28.3's boundary but
//     never earlier, so a bounded over-live and never a use-after-free.
//
// (2) The temp is queued for the next full-expression boundary, where
//     flushAwaitedTaskTemps frees it and NULLS the slot — after which the entry
//     from (1) is a null-guarded no-op on every subsequent edge.
void Emitter::registerAwaitedTaskTemp(llvm::AllocaInst* slot) {
    if (!slot) return;
    if (!disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.awaitedTaskTemp = true;
        disposeStack_.back().push_back(d);
    }
    awaitedTaskTemps_.push_back({slot, currentFn_});
}

// fix #3070 / §30.5 + §28.3 (v0.249): the K8 full-expression boundary discharge,
// called from lowerExpr alongside flushFfiExprTemps once the whole full
// expression has been lowered — "after every use of it within that expression
// has completed", which is precisely where §30.5 puts the task's disposal for
// `Process(await Producer())`.
void Emitter::flushAwaitedTaskTemps() {
    llvm::BasicBlock* bb = builder_.GetInsertBlock();
    bool live = currentFn_ && bb && !bb->getTerminator();
    std::vector<AwaitedTaskTemp> keep;
    for (const auto& t : awaitedTaskTemps_) {
        // Never emit against another function's alloca, and never DROP such an
        // entry either — it belongs to the enclosing boundary and merely passed
        // through while a nested function body was being emitted (the identical
        // retain flushFfiExprTemps documents).
        if (t.fn != currentFn_) { keep.push_back(t); continue; }
        // Diverged (the full expression threw): the unwind edge's disposeStack_
        // walk already reclaimed this via entry (1), and emitting after a
        // terminator would be invalid IR. Drop without emitting.
        if (!live) continue;
        llvm::Value* tk = builder_.CreateLoad(ptrTy(), t.slot, "awaittmp.load");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            tk, llvm::ConstantPointerNull::get(ptrTy()));
        auto* frBB = llvm::BasicBlock::Create(llvm_, "awaittmp.free", currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "awaittmp.cont", currentFn_);
        builder_.CreateCondBr(isNull, contBB, frBB);
        builder_.SetInsertPoint(frBB);
        auto* freeTaskFn = declareC("bz_task_free", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy()});
        builder_.CreateCall(freeTaskFn, {tk});
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
        // Discharge: the disposeStack_ entry established at production stays
        // registered but now loads null, so scope end / return / unwind are all
        // no-ops. This is also what makes a LOOP correct — the slot is one
        // entry-block alloca reused by every iteration, and each iteration's
        // flush re-arms it.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), t.slot);
    }
    awaitedTaskTemps_.swap(keep);
}


// §30.2 / §30.13 (#438 + #439-followup): repair SSA dominance in a resume
// function, moving the offending cross-suspend temporaries into the HEAP FRAME
// so they survive a REAL cross-thread suspend.
//
// After the body is lowered, every value computed before a suspend and used in a
// resume block (the dispatch switch / loop back-edge re-enters those blocks)
// must not assume its def dominates the use ("instruction does not dominate all
// uses"). Named locals already round-trip through the frame (smAllocaToField_);
// the remaining offenders are anonymous temporaries — e.g. the left operand of
// `total + await f()` loaded before the await, used after.
//
// We demote every register whose def fails to dominate a use (DemoteRegToStack),
// and every PHI with a mismatched incoming edge (DemotePHIToStack), iterating to
// a fixpoint (a demote can expose a new offender when one demoted value fed
// another). Each demotion creates a function-local stack alloca. A plain stack
// slot is WRONG across a real thread hop: the resume runs on a worker thread
// with a fresh stack, so the slot holding the pre-suspend value is garbage. We
// therefore PROMOTE every demoted slot into the heap state-machine frame: a
// SEPARATELY-calloc'd spill area, pointed to from a frame field
// (asyncSpillPtrField_), reached through the single `state` pointer that
// bz_task_then carries across the thread boundary — exactly how named locals
// (smAllocaToField_) already survive. The demoted alloca is replaced by a GEP
// into that heap spill area (computed in the resume entry, so it dominates every
// use); the store-before-suspend and load-after-resume that DemoteRegToStack
// emitted then read/write the persistent heap slot. This is valid in BOTH the
// synchronous / already-done cut (the await fast path falls through inline) and a
// real cross-thread suspend (the frame is shared across the activation), the
// same property named locals already rely on.
void Emitter::fixupSuspendDominance(llvm::Function* fn) {
    if (!fn || fn->isDeclaration()) return;

    // Iterate to a fixpoint: each pass recomputes the dominator tree, finds the
    // first instruction with a non-dominated use, and demotes it. Collect the
    // alloca each demotion creates so we can promote them into the heap frame.
    // Bounded by the instruction count (each instruction is demoted at most once).
    std::vector<llvm::AllocaInst*> demoted;
    bool changed = true;
    unsigned guard = 0;
    const unsigned cap = 100000;  // safety bound; never expected to be hit
    while (changed && guard++ < cap) {
        changed = false;
        llvm::DominatorTree dt(*fn);

        for (llvm::BasicBlock& bb : *fn) {
            for (llvm::Instruction& inst : bb) {
                // An alloca defines a pointer that is trivially available; skip.
                if (llvm::isa<llvm::AllocaInst>(&inst)) continue;

                bool offends = false;
                for (llvm::Use& use : inst.uses()) {
                    auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
                    if (!user) continue;
                    // A PHI use is checked against the corresponding incoming
                    // block, not the PHI's own block (LLVM dominance rule).
                    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(user)) {
                        llvm::BasicBlock* incoming =
                            phi->getIncomingBlock(use);
                        if (!dt.dominates(inst.getParent(), incoming)) {
                            offends = true;
                            break;
                        }
                    } else if (!dt.dominates(&inst, user)) {
                        offends = true;
                        break;
                    }
                }
                if (!offends) continue;

                llvm::AllocaInst* slot = nullptr;
                if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&inst))
                    slot = llvm::DemotePHIToStack(phi);
                else
                    slot = llvm::DemoteRegToStack(inst);
                if (slot) demoted.push_back(slot);
                changed = true;
                break;  // dominator tree is now stale; recompute next pass
            }
            if (changed) break;
        }
    }

    // §30.9 / §31.2 (#438): FORCE-promote the handler-context allocas registered
    // by lowerTry (the catch-arm binding / caught-exception disposal slots and
    // the finally exit-mode / return-value / pending-exception slots). Unlike the
    // dominance-demoted registers above, these are allocas — they never "fail
    // dominance" (an alloca's pointer is available everywhere) — so the loop never
    // touches them. But they are stored BEFORE an in-handler await and read AFTER
    // it, so on a cross-thread resume a function-local stack slot holds garbage.
    // Appending them to `demoted` heap-spills them through the frame (the store /
    // load then target the persistent heap slot), exactly like a demoted register.
    for (llvm::AllocaInst* a : asyncHandlerSpillSlots_)
        if (a && a->getParent() && a->getParent()->getParent() == fn)
            demoted.push_back(a);
    asyncHandlerSpillSlots_.clear();

    // No cross-suspend temporary → nothing to move into the frame; the spill-ptr
    // frame field stays null (harmless).
    if (demoted.empty()) return;

    // Promote the demoted stack slots into a heap spill area reached through the
    // frame, so they survive a cross-thread resume (§30.13 / #439-followup).
    promoteDemotedSlotsToFrame(fn, demoted);
}


// §30.2 / §30.13 (#439-followup): move the demoted cross-suspend stack slots
// into a heap spill area pointed to from the frame, so a real cross-thread
// resume reads the persisted values rather than garbage on the worker's fresh
// stack. `demoted` are the allocas fixupSuspendDominance created (one per
// cross-suspend temporary). We:
//   1. Lay out a spill struct { temp0, temp1, ... } from the demoted slots.
//   2. In the resume entry block, before the dispatch switch, lazily calloc the
//      spill area on the FIRST entry (when the frame's spill-ptr field is null)
//      and store its pointer back into the frame; on a later resume the existing
//      pointer is reused. The setup runs on every entry, so the per-field GEPs
//      it computes dominate all blocks reachable from the switch.
//   3. Replace each demoted alloca with its GEP into the heap spill area
//      (replaceAllUsesWith), so the store-before-suspend / load-after-resume
//      DemoteRegToStack emitted now target the persistent heap slot.
void Emitter::promoteDemotedSlotsToFrame(
    llvm::Function* fn, const std::vector<llvm::AllocaInst*>& demoted) {
    if (demoted.empty() || !smStateStruct_) return;

    // (1) Spill struct layout: one field per demoted slot, in stable order.
    std::vector<llvm::Type*> spillFields;
    spillFields.reserve(demoted.size());
    for (llvm::AllocaInst* a : demoted)
        spillFields.push_back(a->getAllocatedType());
    auto* spillStructTy = llvm::StructType::create(
        llvm_, spillFields, fn->getName().str() + ".spill");

    // (2) Resume entry setup, inserted before the dispatch switch (the entry
    // block's terminator). The frame pointer is smSelf_ — the resume function's
    // 2nd arg for an async coroutine (getArg(1)), but the sync iterator MoveNext's
    // ONLY arg (getArg(0)); #747 reuses this pass for the sync MoveNext, so read
    // the frame ptr from smSelf_ (set identically by both setups) rather than a
    // hardcoded arg index.
    llvm::Value* self = smSelf_;
    llvm::BasicBlock& entryBB = fn->getEntryBlock();
    llvm::Instruction* term = entryBB.getTerminator();
    // The resume entry always ends in the dispatch switch (emitResumeDispatch);
    // bail defensively if that invariant is ever broken rather than crash.
    if (!term) return;

    llvm::IRBuilder<> b(term);
    // Load the current spill-area pointer from the frame.
    llvm::Value* spillField =
        b.CreateStructGEP(smStateStruct_, self, asyncSpillPtrField_);
    llvm::Value* curPtr = b.CreateLoad(ptrTy(), spillField, "spill.cur");
    llvm::Value* isNull = b.CreateICmpEQ(
        curPtr, llvm::ConstantPointerNull::get(ptrTy()), "spill.isnull");

    // Split off the dispatch (the terminator) into a continuation block, then
    // replace entry's fall-through with a null-check that callocs on first entry.
    llvm::BasicBlock* contBB =
        entryBB.splitBasicBlock(term, entryBB.getName() + ".spillcont");
    llvm::BasicBlock* allocBB =
        llvm::BasicBlock::Create(llvm_, entryBB.getName() + ".spillalloc", fn,
                                 contBB);
    // splitBasicBlock left an unconditional br entry->cont; rewrite it to the
    // null-check branch (the load/icmp above precede this terminator).
    entryBB.getTerminator()->eraseFromParent();
    {
        llvm::IRBuilder<> eb(&entryBB);
        eb.CreateCondBr(isNull, allocBB, contBB);
    }
    // allocBB: calloc(1, sizeof(spillStruct)), store into the frame field, br cont.
    llvm::Value* freshPtr = nullptr;
    {
        llvm::IRBuilder<> ab(allocBB);
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(spillStructTy);
        auto* callocFn = declareC("calloc", ptrTy(), {i64(), i64()});
        freshPtr = ab.CreateCall(
            callocFn, {ab.getInt64(1), ab.getInt64((int64_t)sz)}, "spill.new");
        ab.CreateStore(freshPtr, spillField);
        ab.CreateBr(contBB);
    }
    // contBB head: PHI the spill pointer (existing vs. fresh), then GEP each
    // field. These dominate the dispatch switch and hence every state block.
    llvm::IRBuilder<> cb(&*contBB->getFirstInsertionPt());
    auto* spillPtr = cb.CreatePHI(ptrTy(), 2, "spill.ptr");
    spillPtr->addIncoming(curPtr, &entryBB);
    spillPtr->addIncoming(freshPtr, allocBB);

    // (3) Re-point each demoted alloca at its heap-spill GEP.
    for (unsigned i = 0; i < demoted.size(); ++i) {
        llvm::AllocaInst* a = demoted[i];
        llvm::Value* gep =
            cb.CreateStructGEP(spillStructTy, spillPtr, i,
                               a->getName().str() + ".spill");
        a->replaceAllUsesWith(gep);
        a->eraseFromParent();
    }
}


// §30.2 deferred tail: emit a well-formed but inert entry for an async method
// whose body codegen is deferred (await-in-try). The compile already failed via
// the diagnostic; this only keeps the emitted IR valid. The body is NOT lowered.
void Emitter::emitAsyncStub(sema::MethodSymbol* ms, llvm::Function* fn,
                            const CallableInfo& ci) {
    llvm::Type* resultTy = nullptr;
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0])
            resultTy = lowerTypeRef(retRef->genericArgs[0]);
    }
    currentFn_ = fn;
    currentThis_ = nullptr;
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr; unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);

    auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
    llvm::Value* task = builder_.CreateCall(allocFn, {}, "async.stub.task");
    auto* completeFn = declareC("bz_task_complete", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64()});
    if (resultTy) {
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(resultTy);
        llvm::AllocaInst* slot = entryAlloca(resultTy);
        builder_.CreateStore(llvm::Constant::getNullValue(resultTy), slot);
        builder_.CreateCall(completeFn, {task, slot, builder_.getInt64((int64_t)sz)});
    } else {
        builder_.CreateCall(completeFn,
                            {task, llvm::ConstantPointerNull::get(ptrTy()),
                             builder_.getInt64(0)});
    }
    if (isMain(ms))
        builder_.CreateRet(builder_.getInt32(0));
    else
        builder_.CreateRet(task);
}


// §30.1 (#1521): "`owned` parameters transfer into the frame ... any `owned`
// parameter never moved out is disposed by the frame when the task reaches its
// terminal state — on every completion path (success, fault, and
// cancellation), after the body's `using var` cleanups and `finally` clauses,
// in reverse declaration order (§28.4's ordering)." Mirrors the sync iterator's
// disposeOwnedParams (LlvmBackend_Iterators.cpp) and the async iterator's
// emitFreeAndReturn dispose loop (both keyed off the same ps->iteratorAutoDispose
// flag Ownership.cpp sets — §28.3/§29.8) for the plain (non-iterator) async-
// method frame published in *asyncOwnedParamFrame_. Called from BOTH terminal
// edges: emitAsyncComplete (success — every `return`/fall-off) and the
// async.fault boundary in emitAsyncMethod (fault; the runtime models
// cancellation as an OperationCanceledException fault, so no third edge is
// needed). `self` is the CURRENT frame pointer (smSelf_ at the call site);
// reads/GEPs through the SAME smStateStruct_ that was live when
// asyncOwnedParamFrame_ was published (never a mismatched nested frame — see
// the save/restore-and-null wrappers around nested async-lambda and lazy
// generic-specialization emission). No-op when no frame is published (an async
// iterator's or lambda's own completion, or a synchronous method).
void Emitter::emitAsyncOwnedParamAutoDispose(llvm::Value* self) {
    if (!asyncOwnedParamFrame_ || !self || !smStateStruct_) return;
    const StateMachineFrame& frame = *asyncOwnedParamFrame_;
    for (std::size_t i = frame.captures.size(); i-- > 0;) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(frame.captures[i].first);
        // #973a: `iteratorFrameConditionalAutoDispose` admits a CONDITIONALLY
        // discharged parameter to this same loop. The null-guard below is then
        // the runtime moved-from test — the marked-adoption lowering nulls this
        // slot on the path that adopts, so only the fall-through path disposes.
        // A plain-async frame has no clone, so the frame the body ran on is the
        // one whose completion edges reach here.
        if (!ps || (!ps->iteratorAutoDispose &&
                    !ps->iteratorFrameConditionalAutoDispose))
            continue;
        unsigned fieldIdx = frame.captureStart + (unsigned)i;
        llvm::Value* fptr =
            builder_.CreateStructGEP(smStateStruct_, self, fieldIdx);
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), fptr, "aop.obj");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()), "aop.isnull");
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "aop.dispose", currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "aop.cont", currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // fix #3125 (§30.1 / §30.5, v0.254): route by the parameter's declared
        // type. This was an unconditional emitDisposeAndReclaim — a class-vtable
        // dispatch — so an `owned t: Task<T>` parameter the body never moved out
        // read its "vtable pointer" out of `bz_task_t` bytes and jumped through
        // it. emitOwnedParamCaptureDispose is the ONE routing decision, shared
        // with the two iterator frames' identical loops.
        emitOwnedParamCaptureDispose(frame.captures[i].first, obj);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), fptr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
}

// §30.1 / §30.9: complete the outer task. `value` is the lowered return value
// (null for a `Task` with no value, or a fall-off completion). The result is
// copied by the runtime (bz_task_complete malloc+memcpy's `result_size` bytes),
// so a stack slot is a fine source.
void Emitter::emitAsyncComplete(llvm::Value* value) {
    llvm::Value* self = smSelf_;
    if (!self || !smStateStruct_) return;
    llvm::Value* otField =
        builder_.CreateStructGEP(smStateStruct_, self, asyncOuterTaskField_);
    llvm::Value* outerTask = builder_.CreateLoad(ptrTy(), otField);

    auto* completeFn = declareC(
        "bz_task_complete", llvm::Type::getVoidTy(llvm_),
        {ptrTy(), ptrTy(), i64()});

    if (value && asyncResultTy_) {
        // §30.1 / §7.2 (#493): when T is a nullable value type (int?, double?,
        // enum?) the task result buffer must hold the full {hasValue=1, value}
        // struct (sized by asyncResultTy_). A plain returned literal/primitive
        // (e.g. `return 42`) comes out of lowerExpr as the non-nullable inner
        // type (e.g. i32). Without wrapping, CreateStore would write i32 into a
        // {i8, i32}-typed alloca at offset 0, leaving the hasValue field (byte 0)
        // equal to the low byte of the integer and the value field (offset 4)
        // zero — producing hasValue=non-zero but value=0 for int?, or hasValue=0
        // (first byte of the double) → treated-null for double?. Fix: detect the
        // mismatch and wrap the value into the nullable struct first.
        if (asyncResultSemaType_ && isNullableValueTy(asyncResultSemaType_) &&
            value->getType() != asyncResultTy_) {
            auto* nst = llvm::dyn_cast<llvm::StructType>(asyncResultTy_);
            if (nst && nst->getNumElements() == 2 &&
                value->getType() == nst->getElementType(1)) {
                // Plain value → {hasValue=1, value} (per §7.2).
                value = wrapNullableValue(asyncResultSemaType_, value);
            }
        }
        // Spill the value into a stack slot and hand its address + size to the
        // runtime (§30.1: `return expression;` completes the task with value).
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(asyncResultTy_);
        llvm::AllocaInst* slot = entryAlloca(asyncResultTy_);
        builder_.CreateStore(value, slot);
        builder_.CreateCall(completeFn,
                            {outerTask, slot, builder_.getInt64((int64_t)sz)});
    } else {
        // `Task` (no value), or fall-off / `return;`: complete with null/0.
        builder_.CreateCall(completeFn,
                            {outerTask, llvm::ConstantPointerNull::get(ptrTy()),
                             builder_.getInt64(0)});
    }
    // §30.1 (#1521): the SUCCESS completion edge — dispose any never-discharged
    // `owned` parameter of the current plain async method now, after the task
    // is completed (so a param the return VALUE itself derives from — already
    // read into `value`/the result buffer above — is unaffected either way; a
    // param the body actually returned is already discharged and not flagged
    // here) and before the frame is freed below. No-op outside emitAsyncMethod
    // (asyncOwnedParamFrame_ is null for a fall-off/return inside an async
    // ITERATOR's own emission, which owns a separate dispose path, and for a
    // nested async-lambda emission, suppressed across the call per the
    // save/restore in LlvmBackend_Exprs2.cpp).
    emitAsyncOwnedParamAutoDispose(self);
    // §30.5 (minimal): the frame is dead once the task is completed on a terminal
    // path. In the synchronous / already-done cut the resume runs inline to
    // completion with no pending suspension, so freeing the frame here is safe and
    // keeps every async method (not just Main) leak-clean. (A real suspended
    // resume must keep the frame alive until the LAST resume; that terminal-only
    // free still holds — this runs only on the completing resume. Owned suspend-
    // locals would be disposed before this; the value-typed minimal cut has none.)
    auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    // §30.13 (#439-followup): free the cross-suspend spill area (if any) before
    // the frame. It is a side allocation pointed to from the frame's spill-ptr
    // field; the field is null when the body had no cross-suspend temporary, and
    // free(null) is a no-op, so this is unconditional and safe in every shape.
    emitFreeSpillArea(self);
    builder_.CreateCall(freeFn2, {self});
}


// §30.13 (#439-followup): free the heap spill area pointed to from the frame's
// spill-ptr field, if present. Emitted on each terminal path that frees the
// frame (emitAsyncComplete + the async.fault boundary). The field is null when
// fixupSuspendDominance allocated no spill area, and free(null) is a no-op, so
// this is safe regardless of body shape. Must run BEFORE free(self) (it reads
// the field through the frame pointer).
void Emitter::emitFreeSpillArea(llvm::Value* self) {
    if (!self || !smStateStruct_) return;
    llvm::Value* spillField =
        builder_.CreateStructGEP(smStateStruct_, self, asyncSpillPtrField_);
    llvm::Value* spillPtr = builder_.CreateLoad(ptrTy(), spillField, "spill.free");
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    builder_.CreateCall(freeFn, {spillPtr});
}


// §28.12 / §30 (`await using` normal-exit path): call DisposeAsync() on `obj`
// via `disposeAsyncFn` (the direct LLVM function pointer resolved at
// registration time), await the returned Task (a proper suspend point), then
// run §28.12 step 2's PHYSICAL RECLAMATION on it — the class's
// `bz$<C>$DisposeReclaim` (owned-field cascade + base chain, run-once guarded,
// destructor body skipped per step 1) followed by emitReclaimBlock's block
// release. `cls` is the class `disposeAsyncFn` was resolved from; it selects
// that reclaim function, and a head with none degrades to the full synchronous
// emitDisposeAndReclaim (see the tail). Before fix #3470 this was a bare
// `free(obj)` and the owned subtree was stranded.
//
// When `disposeAsyncFn` is null (concrete class not statically known at
// registration time — interface-typed local or class not yet emitted) OR the
// async state machine is not active (smKind_ not Await/AsyncYield), we fall
// back to synchronous emitDisposeAndReclaim (Dispose() + free). This is sound
// because every IAsyncDisposable implementor should have a destructor for the
// synchronous fallback (§28.12). The sync fallback is also used on the UNWIND
// path (emitDisposals passes null when isUnwind is true), because awaiting
// across an already-active unwind is a deferred increment.
//
// `obj` is assumed NON-NULL here (the caller null-guards before branching).
// DisposeAsync's function type is: ptr(ptr) — takes `this`, returns Task*.
void Emitter::emitAsyncDisposeAwaitAndFree(llvm::Value* obj,
                                           llvm::Function* disposeAsyncFn,
                                           const sema::TypeSymbol* cls) {
    bool inAsyncSM = (smKind_ == SuspendKind::Await ||
                      smKind_ == SuspendKind::AsyncYield) &&
                     smStateStruct_ && smSelf_ && asyncResumeFn_;
    if (!disposeAsyncFn || !inAsyncSM) {
        // Sync fallback: synchronous Dispose() + free.
        // per §28.12: the class should have a destructor for the synchronous
        // fallback path; Dispose() runs it idempotently. Physical reclamation
        // (free) follows. Reported in the commit as the sync-dispose fallback
        // for: (a) unwind path (disposeAsyncFn is null on the unwind path per
        // the emitDisposals call above), (b) interface-typed local where the
        // concrete class is not statically known at registration, (c) non-async
        // context.
        // NOTE: we do NOT advance smNextSuspendState_ here. The orphaned state
        // block (pre-allocated by countAwaits for the implicit dispose-await) is
        // terminated as a post-body step in emitAsyncMethod — after lowerBody
        // completes, any unreached state blocks in smStateBBs_ are terminated
        // with `br smDoneBB_` (a well-formed but dead block).
        emitDisposeAndReclaim(obj);
        return;
    }

    // Call DisposeAsync() directly through the resolved function pointer.
    // DisposeAsync is an async method entry: returns a Task* immediately (the
    // async body runs synchronously until the first genuine suspension); the
    // result Task is completed either inline (already-done path) or via a
    // continuation when the async body's own awaits complete.
    // Function type: ptr DisposeAsync(ptr this).
    auto* dispAsyncTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    llvm::Value* task = builder_.CreateCall(
        dispAsyncTy, disposeAsyncFn, {obj}, "dispasync.task");

    // Await the Task returned by DisposeAsync() — a proper suspend point
    // inside the enclosing async method's Resume function. emitAwaitSuspend
    // spills suspend-locals, registers the resume continuation on the task,
    // and returns; the resume block reads the result and re-raises any fault.
    // freeTaskAfter=true: the task is anonymous/single-use (no other owner).
    // Result type is null (DisposeAsync returns Task, not Task<T>).
    emitAwaitSuspend(task, /*resultTy=*/nullptr, /*freeTaskAfter=*/true);

    // fix #3470 (§28.12 step 2, v0.283): PHYSICAL RECLAMATION. Step 2 reads,
    // verbatim: "owned fields are disposed in reverse declaration order, the
    // base class's reclamation chains, and the heap block is released. This
    // phase is the same regardless of which logical-disposal path ran. For
    // `await using`, the runtime invokes physical reclamation as the second
    // step of the implicit `Dispose()` it issues after `DisposeAsync` returns;
    // the destructor body that would normally run in the synchronous path is
    // skipped."
    //
    // This was a bare `free(obj)`. Its comment named the confusion exactly —
    // §28.12 says `DisposeAsync` supersedes the DESTRUCTOR BODY, and the code
    // read that as "skip the generated `Dispose()` entirely", dropping the
    // other two thirds of step 2 (the owned-field cascade and the base chain)
    // along with it, plus the three block-release steps emitReclaimBlock owns.
    // Measured on the issue's repro: 14400 B leaked in 400 allocations over 200
    // executions, against a byte-identical program whose only difference is the
    // `await` keyword and which is LeakSanitizer-clean.
    //
    // `disposerReclaimFns_[cls]` is emitDisposerBody minus the destructor body:
    // the same run-once flag guard around the same emitPhysicalReclamationSteps
    // the synchronous core runs, so the two paths cannot drift. `cls` is the
    // SAME resolution `disposeAsyncFn` came from (registerUsingDisposal's
    // `cls`), never a second classification of what is in the slot.
    llvm::Function* reclaimFn = nullptr;
    if (cls) {
        auto rit = disposerReclaimFns_.find(cls);
        if (rit != disposerReclaimFns_.end()) reclaimFn = rit->second;
    }
    if (reclaimFn) {
        builder_.CreateCall(reclaimFn, {obj});
        emitReclaimBlock(obj);
    } else {
        // DEGRADE, in the recoverable direction. No reclaim function exists for
        // this head: the class is IMPORTED (its `bz$<C>$DisposeReclaim` belongs
        // to the defining module and is not part of the exported ABI) or
        // GENERIC (its physical reclamation is per-instantiation). Fall back to
        // the FULL synchronous reclamation, which is the same arm the
        // `!disposeAsyncFn` sync fallback above already takes: the object's
        // owned subtree and base chain are reclaimed correctly through the
        // vtable's most-derived `Dispose`, at the cost of also running the
        // destructor body §28.12 step 1 would have skipped. That is a defined
        // synchronous-fallback shape the section already sanctions elsewhere,
        // and it is strictly better than the alternative this replaces — which
        // ran no cleanup at all and stranded the whole subtree.
        emitDisposeAndReclaim(obj);
    }
}


// stdlib §6.10 / §28.14 (v0.175, #808): TaskScope disposal. Normal exit of an
// active async state machine → bz_task_scope_dispose_async (cancel + drain +
// swallow; IOE iff `await Join()` was never reached) and AWAIT the returned
// task, so §6.10 quiescence holds: no child callback runs after this disposal
// completes. Unwind edge (or non-async context) → the NON-BLOCKING
// bz_task_scope_dispose_unwind zombie drain: a blocking wait inside a domain
// continuation is a deterministic deadlock (bz_task_await_sync aborts on it),
// and awaiting across an active unwind is the §28.12 deferred increment; the
// runtime frees the scope (and swallows faults) at the last child completion,
// so there is no leak and no UAF of the scope itself. The runtime frees the
// scope struct on both paths — codegen emits no free().
void Emitter::emitTaskScopeDispose(llvm::Value* sc, bool isUnwind) {
    bool inAsyncSM = (smKind_ == SuspendKind::Await ||
                      smKind_ == SuspendKind::AsyncYield) &&
                     smStateStruct_ && smSelf_ && asyncResumeFn_;
    if (isUnwind || !inAsyncSM) {
        auto* fn = declareC("bz_task_scope_dispose_unwind",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {sc});
        return;
    }
    llvm::Value* ioeVt = llvm::ConstantPointerNull::get(ptrTy());
    {   auto vit = builtinVtable_.find("InvalidOperationException");
        if (vit != builtinVtable_.end()) ioeVt = vit->second;
    }
    auto* fn = declareC("bz_task_scope_dispose_async", ptrTy(),
                        {ptrTy(), llvm::Type::getInt32Ty(llvm_), ptrTy()});
    llvm::Value* task = builder_.CreateCall(
        fn, {sc, builder_.getInt32(1), ioeVt}, "tscope.dispose.task");
    // Await the drain — a real suspend point; a missing-Join IOE fault
    // re-raises in the resume block. freeTaskAfter=true: single-use task.
    emitAwaitSuspend(task, /*resultTy=*/nullptr, /*freeTaskAfter=*/true);
}


}  // namespace bezel::codegen
