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
//     OwnedTask<T>) becomes the ENTRY function. It allocates the outer Task,
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
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        if (exprContainsAwait(ce->callee.get())) return true;
        for (const auto& a : ce->args)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return exprContainsAwait(me->target.get());
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        if (exprContainsAwait(ix->target.get())) return true;
        for (const auto& a : ix->indices)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e))
        return exprContainsAwait(cd->condition.get()) ||
               exprContainsAwait(cd->thenValue.get()) ||
               exprContainsAwait(cd->elseValue.get());
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
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        unsigned n = countAwaitsInExpr(ce->callee.get());
        for (const auto& a : ce->args) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return countAwaitsInExpr(me->target.get());
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        unsigned n = countAwaitsInExpr(ix->target.get());
        for (const auto& a : ix->indices) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e))
        return countAwaitsInExpr(cd->condition.get()) +
               countAwaitsInExpr(cd->thenValue.get()) +
               countAwaitsInExpr(cd->elseValue.get());
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
        else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s.get()))
            n += countAwaitsInExpr(as->value.get());
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


void Emitter::emitAsyncMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                              const CallableInfo& ci) {
    if (!ci.stmts) return;
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
    //   OwnedTask<T>   → T (Pattern B; ownership transfer is the awaiter's job)
    llvm::Type* resultTy = nullptr;
    const sema::TypeRef* resultSemaTy = nullptr;
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0]) {
            resultSemaTy = retRef->genericArgs[0];
            resultTy = lowerTypeRef(resultSemaTy);
        }
    }
    asyncResultTy_ = resultTy;
    // §30.1 / §7.2 (#493): keep the sema TypeRef for T so emitAsyncComplete
    // can detect a nullable value type and wrap the returned plain value.
    asyncResultSemaType_ = resultSemaTy;

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
            auto* unhandledFn = declareC("bz_unhandled_exception",
                                         llvm::Type::getVoidTy(llvm_), {});
            unhandledFn->setDoesNotReturn();
            builder_.CreateCall(unhandledFn, {});
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
        // it. (Scoped to closures — the allocation async lambdas introduce. Boxes
        // / strings in async-method locals are a distinct pre-existing leak left
        // to a separate fix to keep this change bounded.)
        collectClosureReleaseEligible(*ci.stmts);
        closureReleaseEligibleFn_ = resumeFn;
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
                              const sema::TypeRef* resultElemSemaTy) {
    // `fn` is the closure entry; the builder is already positioned at its entry
    // block (lowerLambda's prologue), and currentFn_ == fn. We drive the entry
    // through builder_ / currentFn_, so `fn` itself is referenced only for the
    // contract (kept as a parameter for clarity / future use).
    (void)fn;
    smKind_ = SuspendKind::Await;

    // §30.6 result element type T (from the lambda's Task<T>/OwnedTask<T> return
    // type; null for a no-value `Task`). Mirrors emitAsyncMethod's resultTy/
    // resultSemaTy setup (which reads it from the method's declared return type).
    llvm::Type* resultTy =
        resultElemSemaTy ? lowerTypeRef(resultElemSemaTy) : nullptr;
    asyncResultTy_ = resultTy;
    asyncResultSemaType_ = resultElemSemaTy;

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
    if (!lam.exprBody) collectSuspendLocals(lam.stmts, bodyLocals);

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
        if (!lam.exprBody)
            collectClosureReleaseEligible(lam.stmts);
        else
            closureReleaseEligible_.clear();
        closureReleaseEligibleFn_ = resumeFn;
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
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0]) {
            elemSemaTy = retRef->genericArgs[0];
            elemTy = lowerTypeRef(elemSemaTy);
        }
    }
    // The per-advance MoveNextAsync result is Task<bool>; the await-result type
    // the Resume completes is bool (i1). asyncResultTy_ sizes bz_task_complete.
    asyncResultTy_ = i1();

    // Frame: locals + [ T current_val, ptr perAdvanceTask, ptr pendingTask,
    //                   ptr spill ].
    StateMachineFrame frame = buildStateMachineFrame(
        ms, *ci.stmts, {elemTy, ptrTy(), ptrTy(), ptrTy()},
        std::string("asynciter.") + std::string(ms->name));
    auto* stateStructTy = frame.structTy;
    const unsigned VTABLE_FIELD = frame.vtableField;
    const unsigned STATE_FIELD = frame.stateField;
    const unsigned CURRENT_VAL_FIELD = frame.currentValField;       // first trailing
    const unsigned PER_ADVANCE_TASK_FIELD = frame.currentValField + 1;
    const unsigned PENDING_TASK_FIELD = frame.currentValField + 2;
    const unsigned SPILL_PTR_FIELD = frame.currentValField + 3;
    // The Resume completes the per-advance task; reuse asyncOuterTaskField_ for it.
    asyncOuterTaskField_ = PER_ADVANCE_TASK_FIELD;
    asyncPendingTaskField_ = PENDING_TASK_FIELD;
    asyncSpillPtrField_ = SPILL_PTR_FIELD;

    bool isInstance = !isStaticMethod(ms);
    std::string prefix = std::string("bz$asynciter$") + std::string(ms->name) + "$";

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

    // --- Vtable: [0]=self, [1]=Dispose, [2]=MoveNextAsync, [3]=Current ---
    const unsigned VT_SIZE = 4;
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
    vtGV->setInitializer(llvm::ConstantArray::get(vtGVType, vtElems));

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
        builder_.CreateRet(st);
    }

    // --- Current: load current_val from the frame ---
    {
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", currentFn);
        llvm::IRBuilder<> cb(entry);
        llvm::Value* self = &*currentFn->arg_begin();
        llvm::Value* cvField =
            cb.CreateStructGEP(stateStructTy, self, CURRENT_VAL_FIELD);
        cb.CreateRet(cb.CreateLoad(elemTy, cvField));
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
        smIterEnumFrameSlot_.clear();      // §29.7 (#944)
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", resumeFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = resumeFn->getArg(1);
        emitFrameCaptureRestores(frame, self);

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

        // "Done" (exhausted / fall-off / state -1): complete the per-advance task
        // with FALSE and return. The frame is NOT freed here (it persists for the
        // next MoveNextAsync / until Dispose).
        std::vector<llvm::BasicBlock*> stateBBs = emitResumeDispatch(
            frame, self, stateGEP, numSuspends,
            [&]() {
                emitAsyncIterComplete(/*hasValue=*/false);
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

        auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

        auto emitFreeAndReturn = [&]() {
            // §29.8 (#973b): dispose never-discharged `owned` parameters captured
            // in the frame BEFORE freeing it — the async analogue of the sync
            // iterator's disposeOwnedParams (LlvmBackend_Iterators.cpp). Runs on
            // EVERY Dispose path (both call sites funnel here), AFTER any finally
            // chains, reverse capture order. Null-guarded and the field is nulled
            // after disposal so a re-entrant Dispose (or a param already moved out
            // and nulled) never double-frees. Only params flagged
            // iteratorAutoDispose (never discharged on any path, set in
            // Ownership.cpp) are disposed; a conditionally-discharged param stays a
            // reject (#973a).
            for (std::size_t i = frame.captures.size(); i-- > 0;) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    frame.captures[i].first);
                if (!ps || !ps->iteratorAutoDispose) continue;
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
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), fptr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            // Free the spill area first (null-safe: free(null) is a no-op).
            llvm::Value* spillField2 =
                builder_.CreateStructGEP(stateStructTy, self, SPILL_PTR_FIELD);
            llvm::Value* spillPtr2 = builder_.CreateLoad(ptrTy(), spillField2);
            builder_.CreateCall(freeFn2, {spillPtr2});
            builder_.CreateCall(freeFn2, {self});
            builder_.CreateRetVoid();
        };

        // #821 (async): an async iterator Dispose must dispose `using var`
        // resources active at the suspension point too — not only explicit
        // try/finally chains. Mirrors the sync iterator Dispose
        // (LlvmBackend_Iterators.cpp).
        if (smIterFinallyAtState_.empty() && smIterDisposalsAtState_.empty()) {
            // No yields inside try/finally and no active using-var — mark done
            // and free.
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            builder_.CreateStore(builder_.getInt32(-1), stateGEP2);
            emitFreeAndReturn();
        } else {
            auto* doneBB = llvm::BasicBlock::Create(llvm_, "dsp.done", disposeFn);
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP2);
            // Mark done immediately so re-entrant Dispose is a no-op.
            builder_.CreateStore(builder_.getInt32(-1), stateGEP2);
            std::set<unsigned> states;
            for (auto& kv : smIterFinallyAtState_) states.insert(kv.first);
            for (auto& kv : smIterDisposalsAtState_) states.insert(kv.first);
            auto* sw = builder_.CreateSwitch(curState, doneBB,
                                             (unsigned)states.size());

            for (unsigned stateIdx : states) {
                auto* chainBB = llvm::BasicBlock::Create(
                    llvm_, "dsp.s" + std::to_string(stateIdx), disposeFn);
                sw->addCase(builder_.getInt32((int)stateIdx), chainBB);
                builder_.SetInsertPoint(chainBB);
                // #821: dispose the using-var resources active at this yield,
                // innermost-first; rebind each slot to the restored local.
                auto dit = smIterDisposalsAtState_.find(stateIdx);
                if (dit != smIterDisposalsAtState_.end()) {
                    for (auto rit = dit->second.rbegin();
                         rit != dit->second.rend(); ++rit) {
                        auto lit = locals_.find(rit->first);
                        if (lit == locals_.end()) continue;
                        Disposal d = rit->second;
                        d.slot = lit->second;
                        std::vector<Disposal> one{d};
                        emitDisposals(one);
                    }
                }
                // Run finally bodies innermost-first (chain is outermost-first).
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
            emitFreeAndReturn();
        }

        smIterFinallyAtState_.clear();
        smIterDisposalsAtState_.clear();  // #821
        smIterEnumFramesAtState_.clear();  // §29.7 (#944)
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

    // The await result type (§30.3 / §30.5): T for Task<T>/OwnedTask<T>, or
    // unit for `await Task`. sema set this on the UnaryExpr.
    llvm::Type* resultTy = nullptr;
    if (auto* rt = ctx_.exprType(un)) {
        if (rt->kind != sema::TypeRef::Kind::Unit)
            resultTy = lowerTypeRef(rt);
    }

    // §30.5: reclaim the awaited task at the await site when its operand is an
    // anonymous producer result (`await Producer()`) — a single-use task with no
    // other owner. A task bound to a local is left to its binder's disposal.
    bool freeTaskAfter =
        dynamic_cast<const ast::CallExpr*>(un.operand.get()) != nullptr;
    // §28.3 / #1250: for a BOUND OwnedTask<T:class> await, null the result buffer
    // after loading so bz_tcs_dispose_owned_result (called at TCS disposal) sees no
    // result and skips the dispose — preventing a double-free of the caller-owned obj.
    // Only fires for OwnedTask (not plain Task) with a ClassOwned element type.
    bool takeResultAfter = false;
    if (!freeTaskAfter) {
        const sema::TypeRef* taskTy = ctx_.exprType(*un.operand);
        if (taskTy && taskTy->kind == sema::TypeRef::Kind::Named &&
            !taskTy->declaration && taskTy->astNode && taskTy->astNode->name &&
            !taskTy->astNode->name->parts.empty() &&
            taskTy->astNode->name->parts[0] == "OwnedTask" &&
            !taskTy->genericArgs.empty() && taskTy->genericArgs[0]) {
            takeResultAfter =
                (elementPolicyFor(taskTy->genericArgs[0], /*owningFamily=*/true).cat ==
                 ElementPolicy::Category::ClassOwned);
        }
    }
    return emitAwaitSuspend(task, resultTy, freeTaskAfter, takeResultAfter);
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
                                       bool freeTaskAfter, bool takeResultAfter) {
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
        // single-use and freed just below — keep the current handler-owns behavior
        // (no mark), so the lone awaiter reclaims error_obj exactly once.
        if (!freeTaskAfter) {
            auto* markFn = declareC("bz_mark_exc_borrowed",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(markFn, {excObj});
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
        // §28.3 / #1250: for a BOUND OwnedTask<T:class> await, null the result
        // buffer after loading (consumed-tracking). bz_tcs_dispose_owned_result
        // checks task->result; a null here tells it the result was already taken.
        if (takeResultAfter) {
            auto* takeResultFn = declareC("bz_task_take_result",
                                          llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(takeResultFn, {awaitedTask});
        }
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
        auto* freeTaskFn = declareC("bz_task_free", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy()});
        builder_.CreateCall(freeTaskFn, {awaitedTask});
    }
    return result;
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
// free the block (physical reclamation — §28.12 step 2).
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
                                           llvm::Function* disposeAsyncFn) {
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

    // Physical reclamation: free the object block (§28.12 step 2). The
    // logical disposal (DisposeAsync) has already completed above; the
    // destructor body is NOT re-run (DisposeAsync supersedes it per §28.12).
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    builder_.CreateCall(freeFn, {obj});
}


}  // namespace bezel::codegen
