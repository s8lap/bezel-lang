#include "flow/CFG.hpp"

#include "ast/Node.hpp"
#include "common/AwaitScan.hpp"  // fix #3587: the shared await-site walk

namespace bezel::flow {

CFG::CFG() {
    entry_ = makeBlock(CFGBlock::Kind::Entry);
    exit_  = makeBlock(CFGBlock::Kind::Exit);
}

CFGBlock* CFG::makeBlock(CFGBlock::Kind k) {
    auto owned = std::make_unique<CFGBlock>();
    owned->kind = k;
    owned->id = nextId_++;
    CFGBlock* raw = owned.get();
    blocks_.push_back(std::move(owned));
    return raw;
}

void CFG::addEdge(CFGBlock* from, CFGBlock* to) {
    from->succs.push_back(to);
    to->preds.push_back(from);
}

void CFG::addExceptionEdge(CFGBlock* from, CFGBlock* to) {
    // F2 / O11-real: an exception edge participates in the normal succ/pred
    // lists (it is still a control transfer) but is additionally tagged so
    // a dataflow analysis can apply the §28.3 exception-path discharge rule.
    from->succs.push_back(to);
    to->preds.push_back(from);
    from->exceptionSuccs.insert(to);
}

namespace {

// Async-readiness 3a: record every explicit `await` UnaryExpr (§30.2)
// reachable from `e` into `block->awaitSites`, via `cfg.recordAwaitSite`.
// Purely additive: only `awaitSites` is touched.
//
// fix #3587 (CLAUDE.md §6 dedup) / #2251: thin wrapper over the ONE
// both-sources await-site walk (src/common/AwaitScan.hpp). This used to be
// its own hand-written dynamic_cast chain — the one of the four original
// copies that already had the `BindExpr` arm (see AwaitScan.hpp's header for
// the full rationale, including the LIVE `CaseExpr` gap this closes: MEASURED
// hangs, not latent — the OLD comment below this file carried, asserting
// "CaseExpr: no `await`-bearing child reachable ... in the enclosing method's
// CFG," was wrong). The walk still mirrors the sub-expression recursion shape
// used by the definite-assignment analyzer (sema/DefiniteAssignment.cpp) and
// still deliberately does NOT descend into a `LambdaExpr` body — a
// `LambdaExpr` is its own async scope (§30.6), so an `await` inside it is a
// suspend point of the lambda's CFG, not of the enclosing method's.
void recordAwaitsInExpr(CFG& cfg, CFGBlock* block, const ast::Expr* e) {
    bezel::common::forEachAwaitSite(e, [&](const ast::UnaryExpr* ue) {
        // §30.2: this UnaryExpr is the suspend site.
        cfg.recordAwaitSite(block, ue);
    });
}

// Loop context: where `break` and `continue` should branch to. The CFG
// builder maintains a stack of these as it enters / exits loop bodies.
struct LoopContext {
    CFGBlock* header;   // continue target
    CFGBlock* afterEnd; // break target
};

// Try-statement handler context. Updated as the builder enters / exits
// a `try` block. Throws inside a try statement route to the catch
// dispatcher (F2). Normal exits route through the finally (if any) on
// the way to the after-try block.
struct TryHandlerContext {
    CFGBlock* catchDispatch = nullptr; // first catch's entry (Branch)
    CFGBlock* finallyEntry  = nullptr; // null when no finally
    CFGBlock* afterTry      = nullptr; // post-finally continuation
};

class Builder {
public:
    Builder(CFG& cfg, bool treatTryAsOpaque = false)
        : cfg_(cfg), treatTryAsOpaque_(treatTryAsOpaque) {}

    // Build the CFG. Returns the last "live" block on exit so the caller
    // can wire it to the function-level exit — null when control reaches
    // the end of the body via a terminator (return / throw / unbounded
    // break) and there is no live block to connect.
    CFGBlock* build(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        CFGBlock* current = cfg_.entry();
        for (const auto& s : body) {
            if (!s) continue;
            CFGBlock* after = visitStmt(*s, current);
            if (!after) {
                // Unreachable continuation; subsequent stmts would be
                // dead code (sema diagnoses; we just track CFG shape).
                current = cfg_.makeBlock(CFGBlock::Kind::Linear);
                // Don't connect from the previous block — the terminator
                // already routed past us. The fresh block has no preds
                // and exists only to absorb the dead stmts so we don't
                // crash the visitor.
            } else {
                current = after;
            }
        }
        // If the last block has no successor edge, it's the live exit
        // path; return it. If it does (e.g. a dead-code fresh block has
        // nothing wired in OR a fresh block that absorbed dead stmts has
        // no preds), check explicitly.
        if (current->preds.empty() && current->stmts.empty() &&
            current != cfg_.entry()) {
            return nullptr;
        }
        return current;
    }

private:
    CFG& cfg_;
    bool treatTryAsOpaque_ = false;
    std::vector<LoopContext> loops_;
    std::vector<TryHandlerContext> tryHandlers_;

    // §21.5 (v0.201, #2051): the loop `offset` levels out from the innermost
    // (0 = innermost). Precondition: loops_ non-empty. An out-of-range offset
    // (only reachable when sema already reported an unresolved label and left
    // targetLoopOffset at its 0 default, or a malformed offset) clamps to the
    // innermost, so the CFG never indexes out of bounds.
    const LoopContext& loopAtOffset(int offset) const {
        if (offset < 0 || static_cast<std::size_t>(offset) >= loops_.size())
            offset = 0;
        return loops_[loops_.size() - 1 - static_cast<std::size_t>(offset)];
    }

    // Where should an exception (from a throwable call or an explicit
    // throw) route to? Nearest enclosing catch dispatcher, or the
    // function-level exit if none. Used by ThrowStmt visit.
    CFGBlock* exceptionTarget() const {
        if (!tryHandlers_.empty()) {
            return tryHandlers_.back().catchDispatch;
        }
        return cfg_.exit();
    }

    // Async-readiness 3a: record into `block` every `await` suspend site
    // carried by the *direct* expressions of the linear / terminator
    // statement `s` (the kinds that `visitStmt` appends to a block). Nested
    // statements (if/while/for/try bodies) are NOT handled here — the builder
    // already recurses into them and records their awaits at their own visit
    // sites. Control-flow conditions (if/while/for) are recorded by the
    // respective visit methods into the branch/header block. Purely additive.
    void recordStmtAwaits(CFGBlock* block, const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            // §30.2 explicit awaits in the initializer execute in this block.
            recordAwaitsInExpr(cfg_, block, ld->initializer.get());
            // §28.12: `await using var` carries an implicit scope-end
            // `DisposeAsync()` await. No UnaryExpr exists for it, so record
            // the LocalDecl node itself as the await-bearing construct.
            if (ld->isAwaitUsing) cfg_.recordAwaitSite(block, ld);
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            recordAwaitsInExpr(cfg_, block, lc->value.get());
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            recordAwaitsInExpr(cfg_, block, as->target.get());
            recordAwaitsInExpr(cfg_, block, as->value.get());
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            recordAwaitsInExpr(cfg_, block, es->expr.get());
            return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            recordAwaitsInExpr(cfg_, block, ds->value.get());
            return;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            recordAwaitsInExpr(cfg_, block, rs->value.get());
            return;
        }
        if (auto* tr = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            recordAwaitsInExpr(cfg_, block, tr->value.get());
            return;
        }
        if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
            recordAwaitsInExpr(cfg_, block, is->target.get());
            return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
            // §30.7: an async iterator's `yield return await ...;` carries an
            // explicit await in the yielded expression.
            recordAwaitsInExpr(cfg_, block, ys->value.get());
            return;
        }
        // Break/Continue/DoEnd/If/While/For/Try carry no own direct value
        // expression to scan here (conditions/bodies handled at their sites).
    }

    // Visit one statement starting from `from`. Returns the block that
    // continues the linear thread after this statement (which may be
    // `from` for straight-line stmts, or a fresh merge block for
    // branches, or null for non-returning stmts like `return`/`throw`).
    CFGBlock* visitStmt(const ast::Stmt& s, CFGBlock* from) {
        if (auto* ifStmt = dynamic_cast<const ast::IfStmt*>(&s)) {
            return visitIf(*ifStmt, from);
        }
        if (auto* whileStmt = dynamic_cast<const ast::WhileStmt*>(&s)) {
            return visitWhile(*whileStmt, from);
        }
        if (auto* forStmt = dynamic_cast<const ast::ForStmt*>(&s)) {
            return visitFor(*forStmt, from);
        }
        if (auto* doBlock = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            return visitDoEnd(*doBlock, from);
        }
        if (dynamic_cast<const ast::ReturnStmt*>(&s)) {
            from->stmts.push_back(&s);
            recordStmtAwaits(from, s); // async-readiness 3a (additive)
            cfg_.addEdge(from, cfg_.exit());
            return nullptr;
        }
        if (dynamic_cast<const ast::ThrowStmt*>(&s)) {
            // F2: throws route to the nearest enclosing catch dispatcher,
            // falling through to the function exit when no try frame is
            // active. The transfer is an exception (unwinding) edge — tag
            // it so the ownership pass applies §28.3 exception-path
            // discharge to owned locals live at the throw.
            from->stmts.push_back(&s);
            recordStmtAwaits(from, s); // async-readiness 3a (additive)
            cfg_.addExceptionEdge(from, exceptionTarget());
            return nullptr;
        }
        if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
            // Slice O3a: when the consumer requested opaque-try handling
            // (the ownership pass), emit the TryStmt as a single linear
            // statement instead of the F2 exception-edge expansion. The
            // analysis then walks the try/catch/finally bodies itself,
            // preserving the O11-simple behavior.
            if (treatTryAsOpaque_) {
                from->stmts.push_back(&s);
                return from;
            }
            return visitTry(*trys, from);
        }
        if (auto* brk = dynamic_cast<const ast::BreakStmt*>(&s)) {
            from->stmts.push_back(&s);
            if (!loops_.empty()) {
                // §21.5 (v0.201, #2051): a labeled `break` exits the target loop
                // `targetLoopOffset` levels out (0 = innermost), which sema
                // resolved. The loops_ nesting matches, so index from the back.
                cfg_.addEdge(from, loopAtOffset(brk->targetLoopOffset).afterEnd);
            } else {
                cfg_.addEdge(from, cfg_.exit());
            }
            return nullptr;
        }
        if (auto* cnt = dynamic_cast<const ast::ContinueStmt*>(&s)) {
            from->stmts.push_back(&s);
            if (!loops_.empty()) {
                // §21.5 (v0.201): a labeled `continue` advances the target loop.
                cfg_.addEdge(from, loopAtOffset(cnt->targetLoopOffset).header);
            } else {
                cfg_.addEdge(from, cfg_.exit());
            }
            return nullptr;
        }
        // All other statements (LocalDecl, AssignmentStmt, ExpressionStmt,
        // LocalConstDecl, DiscardStmt, IncrementStmt, YieldStmt, ...) are
        // linear: append to the current block.
        from->stmts.push_back(&s);
        recordStmtAwaits(from, s); // async-readiness 3a (additive)
        return from;
    }

    CFGBlock* visitIf(const ast::IfStmt& s, CFGBlock* from) {
        // Branch block carries the `if` AST node so diagnostics can point
        // at the condition. Successors: [then-entry, else-or-elif-entry,
        // (...)] — chained elifs produce nested branch blocks.
        CFGBlock* merge = cfg_.makeBlock(CFGBlock::Kind::Merge);
        // Record the originating `if` on the merge so a forward analysis
        // can locate the §28.3 branch-join diagnostic at the if's span.
        merge->sourceStmt = &s;
        visitIfBranches(s, from, merge);
        return merge;
    }

    void visitIfBranches(const ast::IfStmt& s, CFGBlock* from,
                         CFGBlock* merge) {
        CFGBlock* current = from;
        // If-branch.
        CFGBlock* ifEnd = visitBranch(s.ifBranch, current, merge);
        (void)ifEnd;
        // Else / elif chain.
        for (const auto& elif : s.elifBranches) {
            CFGBlock* elifBranchHead =
                cfg_.makeBlock(CFGBlock::Kind::Branch);
            cfg_.addEdge(current, elifBranchHead);
            CFGBlock* elifEnd =
                visitBranch(elif, elifBranchHead, merge);
            (void)elifEnd;
            current = elifBranchHead;
        }
        if (s.hasElse) {
            CFGBlock* elseEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
            cfg_.addEdge(current, elseEntry);
            Builder inner(cfg_, treatTryAsOpaque_);
            inner.loops_ = loops_;
            // Propagate the enclosing try frames so a `throw` (or throwable
            // operation) nested in this branch routes to the right catch
            // dispatcher / finally, not straight to the function exit. F2
            // built nested control flow without this; O11-real needs the
            // exception edges to be complete for §28.3 discharge.
            inner.tryHandlers_ = tryHandlers_;
            CFGBlock* elseExit = inner.runInto(s.elseBody, elseEntry);
            if (elseExit) cfg_.addEdge(elseExit, merge);
        } else {
            // No `else`: the branch's false edge falls through to merge.
            cfg_.addEdge(current, merge);
        }
    }

    CFGBlock* visitBranch(const ast::IfStmt::Branch& b, CFGBlock* from,
                          CFGBlock* merge) {
        // Convert `from` into a branch block by leaving its kind alone
        // but adding successor edges. To keep the contract clean, we
        // promote `from` to Branch kind.
        from->kind = CFGBlock::Kind::Branch;
        // #764: record the (el)if condition so the ownership pass can
        // null-discriminate the branch successors (succs[0]=true, succs[1]=false).
        from->condition = b.condition.get();
        // Async-readiness 3a: the (el)if condition is evaluated in this
        // branch block; record any `await` in it as a suspend site here.
        recordAwaitsInExpr(cfg_, from, b.condition.get());
        CFGBlock* bodyEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
        cfg_.addEdge(from, bodyEntry);
        // Walk the branch body in a sub-Builder so loops_ are preserved.
        Builder inner(cfg_, treatTryAsOpaque_);
        inner.loops_ = loops_;
        // Propagate enclosing try frames (see else-branch note above) so a
        // `throw` in this branch routes to the enclosing catch / finally.
        inner.tryHandlers_ = tryHandlers_;
        CFGBlock* end = inner.runInto(b.body, bodyEntry);
        if (end) cfg_.addEdge(end, merge);
        return end;
    }

    CFGBlock* visitWhile(const ast::WhileStmt& s, CFGBlock* from) {
        CFGBlock* header = cfg_.makeBlock(CFGBlock::Kind::Branch);
        header->isLoopHeader = true;
        // §28.3 / #892: store the while condition on the header block so
        // processBlock can process it through handleDischarge (out-owned
        // marking + move recording). Mirrors what visitBranch does for
        // if/elif conditions. The nullEdgeName discriminator in computeEntry
        // already skips loop-header blocks (`isLoopHeader` guard), so this
        // additive store does not affect null-edge discrimination.
        header->condition = s.condition.get();
        // Async-readiness 3a: the while-condition is re-evaluated each pass
        // in the header; record any `await` in it as a suspend site there.
        recordAwaitsInExpr(cfg_, header, s.condition.get());
        CFGBlock* bodyEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
        CFGBlock* after = cfg_.makeBlock(CFGBlock::Kind::Linear);
        cfg_.addEdge(from, header);
        cfg_.addEdge(header, bodyEntry); // true edge
        cfg_.addEdge(header, after);     // false edge
        Builder inner(cfg_, treatTryAsOpaque_);
        inner.loops_ = loops_;
        inner.loops_.push_back({header, after});
        // Propagate enclosing try frames so a `throw` in the loop body
        // routes to the enclosing catch / finally (O11-real exception
        // edges); see the else-branch note in visitIfBranches.
        inner.tryHandlers_ = tryHandlers_;
        CFGBlock* bodyEnd = inner.runInto(s.body, bodyEntry);
        if (bodyEnd) cfg_.addEdge(bodyEnd, header); // back-edge
        return after;
    }

    CFGBlock* visitFor(const ast::ForStmt& s, CFGBlock* from) {
        // For-loop CFG mirrors while-loop: header (the iteration check),
        // body, after.
        // Async-readiness 3a: the iteration source (and optional `with`
        // cancellation-token expression, §30.7) are evaluated once before
        // the loop, in `from`; record any explicit `await` in them there.
        recordAwaitsInExpr(cfg_, from, s.iterable.get());
        recordAwaitsInExpr(cfg_, from, s.withExpr.get());
        // #2109 (§30.7 / §28.3): tag `from` as the block that originates this
        // for-loop's iterable evaluation, so the ownership pass can locate the
        // iterable expression and run its owned-sink argument moves through
        // `recordMovesIn`. Additive — the iterable has never been part of any
        // block's `stmts`, so this is the only way an analysis can find it.
        from->forOrigins.push_back(&s);
        CFGBlock* header = cfg_.makeBlock(CFGBlock::Kind::Branch);
        header->isLoopHeader = true;
        // §30.7: `await for` desugars to a `MoveNextAsync()` `await` per
        // iteration at the loop header. No UnaryExpr exists for this implicit
        // await, so record the ForStmt node itself as the suspend site on the
        // header (the block the desugaring re-evaluates each pass).
        if (s.isAwait) cfg_.recordAwaitSite(header, &s);
        CFGBlock* bodyEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
        // §29.2 (#834): tag the body-entry block with its originating ForStmt
        // so the ownership pass can seed an owned iteration variable there when
        // the iterable is an owning enumerable ("Iterator yield ownership").
        bodyEntry->forBodyEntry = &s;
        CFGBlock* after = cfg_.makeBlock(CFGBlock::Kind::Linear);
        cfg_.addEdge(from, header);
        cfg_.addEdge(header, bodyEntry);
        cfg_.addEdge(header, after);
        Builder inner(cfg_, treatTryAsOpaque_);
        inner.loops_ = loops_;
        inner.loops_.push_back({header, after});
        // Propagate enclosing try frames so a `throw` in the loop body
        // routes to the enclosing catch / finally (O11-real exception
        // edges); see the else-branch note in visitIfBranches.
        inner.tryHandlers_ = tryHandlers_;
        CFGBlock* bodyEnd = inner.runInto(s.body, bodyEntry);
        if (bodyEnd) cfg_.addEdge(bodyEnd, header);
        return after;
    }

    CFGBlock* visitTry(const ast::TryStmt& s, CFGBlock* from) {
        // CFG shape (F2):
        //
        //   from → tryEntry → (linear body) → ┬── normal-exit ──→ finally?
        //                                     └── exception-edge ─→ catchDispatch
        //
        //   catchDispatch → catch1Body → finally?
        //                 → catch2Body → finally?
        //                 ...
        //   finally → afterTry
        //
        // Finally is omitted when the try has no FinallyClause; in that
        // case the normal/catch exits go directly to afterTry.
        CFGBlock* tryEntry  = cfg_.makeBlock(CFGBlock::Kind::Linear);
        CFGBlock* afterTry  = cfg_.makeBlock(CFGBlock::Kind::Linear);
        CFGBlock* finallyEntry =
            s.hasFinally ? cfg_.makeBlock(CFGBlock::Kind::Linear) : nullptr;
        // Catch dispatch: a Branch block fanning out to each catch arm.
        // Even when there are zero catches (try…finally form), we still
        // need a dispatch placeholder so throws have somewhere to land —
        // it just unconditionally routes to finally (or beyond).
        CFGBlock* catchDispatch =
            cfg_.makeBlock(CFGBlock::Kind::Branch);
        // O11-real: record the originating try so the ownership pass can
        // scope §28.3 exception-path discharge to this try body's locals.
        catchDispatch->tryForDispatch = &s;

        cfg_.addEdge(from, tryEntry);

        // Build try body with this handler context active.
        Builder inner(cfg_, treatTryAsOpaque_);
        inner.loops_ = loops_;
        inner.tryHandlers_ = tryHandlers_;
        inner.tryHandlers_.push_back({catchDispatch, finallyEntry, afterTry});
        CFGBlock* tryBodyEnd = inner.runInto(s.body, tryEntry);
        if (tryBodyEnd) {
            // Normal exit from the try body: route through finally (if any)
            // then to afterTry.
            cfg_.addEdge(tryBodyEnd,
                         finallyEntry ? finallyEntry : afterTry);
            // Also model the "throwable call inside the try body" as a
            // possible exception edge from the body's end. A more precise
            // F2 would emit one edge per throwable call site; for now we
            // over-approximate with a single edge — sound for ownership
            // (it sees the throw-path as reachable from the body end). Tag
            // it as an exception edge so §28.3 exception-path discharge
            // applies to owned locals live at that point.
            cfg_.addExceptionEdge(tryBodyEnd, catchDispatch);
        }

        // Build each catch arm.
        for (const auto& arm : s.catches) {
            CFGBlock* armEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
            cfg_.addEdge(catchDispatch, armEntry);
            // #1591 (§31.1 / §31.2.1): record the arm's `when` guard (if any)
            // on its entry block so the ownership pass's processBlock walks it
            // through handleDischarge exactly like an if/while condition —
            // see CFGBlock::catchGuard.
            if (arm.guard) armEntry->catchGuard = arm.guard.get();
            // The arm's body is a single statement (DoEndBlock or any
            // other Stmt). Visit it with the outer (one-frame-popped)
            // try handler stack — a throw in a catch handler shouldn't
            // route back to this catch.
            Builder armInner(cfg_, treatTryAsOpaque_);
            armInner.loops_ = loops_;
            armInner.tryHandlers_ = tryHandlers_;
            CFGBlock* armEnd = armEntry;
            if (arm.body) {
                armEnd = armInner.visitStmt(*arm.body, armEntry);
            }
            if (armEnd) {
                cfg_.addEdge(armEnd,
                             finallyEntry ? finallyEntry : afterTry);
            }
        }

        // If there are no catches, the dispatch acts as a pass-through
        // (any incoming exception edge propagates through finally to
        // outside).
        if (s.catches.empty()) {
            cfg_.addEdge(catchDispatch,
                         finallyEntry ? finallyEntry : afterTry);
        }

        // Build the finally block (if present).
        if (finallyEntry) {
            Builder finallyInner(cfg_, treatTryAsOpaque_);
            finallyInner.loops_ = loops_;
            finallyInner.tryHandlers_ = tryHandlers_;
            CFGBlock* finallyEnd =
                finallyInner.runInto(s.finallyBody, finallyEntry);
            if (finallyEnd) {
                cfg_.addEdge(finallyEnd, afterTry);
            }
        }

        return afterTry;
    }

    CFGBlock* visitDoEnd(const ast::DoEndBlock& s, CFGBlock* from) {
        // do/end is pure sequencing — same as inlining the body.
        Builder inner(cfg_, treatTryAsOpaque_);
        inner.loops_ = loops_;
        // Propagate enclosing try frames so a `throw` in the do-block
        // routes to the enclosing catch / finally (O11-real exception
        // edges); see the else-branch note in visitIfBranches.
        inner.tryHandlers_ = tryHandlers_;
        CFGBlock* end = inner.runInto(s.body, from);
        return end;
    }

    CFGBlock* runInto(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        CFGBlock* start) {
        CFGBlock* current = start;
        for (const auto& s : body) {
            if (!s) continue;
            current = visitStmt(*s, current);
            if (!current) {
                current = cfg_.makeBlock(CFGBlock::Kind::Linear);
            }
        }
        // Mirror build() (#747 R42): when the body ends in a terminator
        // (return / throw / unbounded break), the loop above left `current`
        // pointing at a fresh dead-code block — one created to absorb any
        // statements after the terminator, with no predecessors and (usually)
        // no statements. It is NOT a live exit of the body. Returning it would
        // let a caller wire a spurious orphan→merge / orphan→header edge
        // (visitBranch / visitIfBranches' else / the loop visitors all do
        // `if (end) addEdge(end, target)`). That orphan, carrying no locals,
        // then dilutes the §28.3 ownership meet at the merge: an owned local
        // present only on the real fall-through edge is silently dropped by the
        // non-union if-merge join, masking a leak (a `var c := new T()` whose
        // `if` branch does `c.Dispose(); return;` left the fall-through leak
        // undiagnosed). Returning null here — exactly as build() does — keeps
        // the returning branch from contributing a fall-through edge.
        if (current->preds.empty() && current->stmts.empty() &&
            current != cfg_.entry()) {
            return nullptr;
        }
        return current;
    }
};

} // namespace

std::unique_ptr<CFG> buildCFG(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    bool treatTryAsOpaque) {
    auto cfg = std::make_unique<CFG>();
    Builder builder(*cfg, treatTryAsOpaque);
    CFGBlock* end = builder.build(body);
    if (end) {
        cfg->addEdge(end, cfg->exit());
    }
    return cfg;
}

} // namespace bezel::flow
