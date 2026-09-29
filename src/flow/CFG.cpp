#include "flow/CFG.hpp"

#include <functional>
#include <unordered_map>

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

// fix #4180 (§21.7/§21.5/§31.2, v0.298): per-try state shared by every copy of
// its TryHandlerContext frame (the frame is threaded by VALUE through every
// nested Builder as tryHandlers_ propagates downward, so this must be the
// SAME object across all of them — hence shared_ptr, not a plain member).
// `makeClone(target)` builds ONE clone of this try's finally body whose own
// end (if it falls through) connects to `target`, and returns the clone's
// entry block; it captures (by value, at the point the try was entered) the
// finally AST + the enclosing loop/try context, mirroring the ORIGINAL
// (fall-through-triggered) finally build in visitTry so a clone sees
// identical nesting. `byTarget` memoizes one clone per distinct ultimate
// target so N early exits that must hop through the SAME finally to the SAME
// target reuse one clone instead of rebuilding the finally body N times.
struct FinallyCloneState {
    std::function<CFGBlock*(CFGBlock* target)> makeClone;
    std::unordered_map<CFGBlock*, CFGBlock*> byTarget;
};

// Try-statement handler context. Updated as the builder enters / exits
// a `try` block. Throws inside a try statement route to the catch
// dispatcher (F2). Normal exits route through the finally (if any) on
// the way to the after-try block.
struct TryHandlerContext {
    CFGBlock* catchDispatch = nullptr; // first catch's entry (Branch)
    CFGBlock* finallyEntry  = nullptr; // null when no finally
    CFGBlock* afterTry      = nullptr; // post-finally continuation

    // fix #4180: loops_.size() measured when this frame was pushed (i.e. the
    // loop nesting depth the try/arm body was ENTERED at). A break/continue
    // targeting a loop already on the stack at that depth is exiting THIS
    // try (the loop is outside it); a break/continue targeting a loop pushed
    // AFTER (nested inside the try/arm's own body) never leaves it. See
    // Builder::routeExitThroughFinally.
    std::size_t loopDepthAtEntry = 0;

    // fix #4180: null when finallyEntry is null (nothing to hop through).
    std::shared_ptr<FinallyCloneState> finallyClones;

    // fix #4180: true only for the synthetic frame pushed while building a
    // CATCH ARM's body (see visitTry). Such a frame is never itself the
    // landing point for a throw escaping the arm (a throw inside a catch
    // handler is not caught by its OWN try — the pre-existing comment on the
    // arm-building loop below) — `catchDispatch` here is the OUTER target a
    // throw should reach, and `exceptionTarget()` must hop through
    // `finallyEntry` before landing there, unlike a normal (non-arm) frame
    // whose `catchDispatch` IS the landing point (finally already runs AFTER
    // catch dispatch via the pre-existing armEnd/catches.empty() edges, so no
    // hop is added there).
    bool isArmContext = false;
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
    // innermost, so the CFG never indexes out of bounds. fix #4180: split out
    // of loopAtOffset so routeExitThroughFinally can get the target loop's
    // 0-based stack INDEX (for the loopDepthAtEntry comparison) without a
    // second, possibly-inconsistent clamp.
    std::size_t loopIndexAtOffset(int offset) const {
        if (offset < 0 || static_cast<std::size_t>(offset) >= loops_.size())
            offset = 0;
        return loops_.size() - 1 - static_cast<std::size_t>(offset);
    }
    const LoopContext& loopAtOffset(int offset) const {
        return loops_[loopIndexAtOffset(offset)];
    }

    // Where should an exception (from a throwable call or an explicit
    // throw) route to? Nearest enclosing catch dispatcher, or the
    // function-level exit if none. Used by ThrowStmt visit.
    //
    // fix #4180 (§31.2, v0.298): when the innermost frame is a catch-arm
    // context (`isArmContext`), a throw here does not land in that try's OWN
    // catch (it already didn't, pre-fix — see the arm-building loop's
    // comment) — but it DOES still need to run that try's `finally` before
    // reaching `catchDispatch` (the OUTER target), which pre-fix it skipped
    // entirely. A normal (non-arm) frame's `catchDispatch` IS the landing
    // point, so no hop is added there — its finally already runs AFTER catch
    // dispatch via the pre-existing edges (see visitTry).
    CFGBlock* exceptionTarget() {
        if (tryHandlers_.empty()) return cfg_.exit();
        TryHandlerContext& top = tryHandlers_.back();
        if (top.isArmContext && top.finallyEntry) {
            return getOrMakeFinallyClone(top, top.catchDispatch);
        }
        return top.catchDispatch;
    }

    // fix #4180 (§21.7 return / §21.5 break,continue / §31.2 try-finally,
    // v0.298): route an early exit's edge to `ultimateTarget` through every
    // enclosing try frame's ACTIVE finally that the exit actually leaves,
    // innermost first — mirroring what ThrowStmt already got from
    // exceptionTarget() (now itself extended above), which ReturnStmt /
    // BreakStmt / ContinueStmt bypassed entirely before this fix (issue
    // #4180: `tryBodyEnd`/`armEnd` comes back null for a body ending in one
    // of these, so visitTry's own `if (tryBodyEnd) addEdge(..., finallyEntry
    // ...)` never ran — finallyEntry could end up with NO predecessor
    // reachable from cfg_.entry() at all, so the ownership dataflow never
    // visited it).
    //
    // `exitsAllFrames = true` (return; also the "no active loop" defensive
    // fallback for a malformed break/continue) routes through EVERY
    // enclosing frame's finally unconditionally — a return leaves the whole
    // method, so every active try is exited. `exitsAllFrames = false`
    // (break/continue) instead compares each frame's `loopDepthAtEntry`
    // against `targetLoopIdx` (the target loop's 0-based index in the
    // CURRENT loops_, from loopIndexAtOffset): a frame entered while the
    // target loop was ALREADY on the stack (loopDepthAtEntry > targetLoopIdx)
    // is exited by this break/continue and gets a hop; a frame entered
    // AFTER the target loop (the loop is nested INSIDE the try/arm) is not
    // exited and gets none. This is monotonic scanning outward — loops_ only
    // grows walking inward, so an outer frame's loopDepthAtEntry is never
    // greater than an inner one's — so the scan can stop at the first
    // ineligible frame.
    CFGBlock* routeExitThroughFinally(CFGBlock* ultimateTarget,
                                       bool exitsAllFrames,
                                       std::size_t targetLoopIdx) {
        if (tryHandlers_.empty()) return ultimateTarget;
        std::vector<TryHandlerContext*> eligible; // innermost first
        for (auto it = tryHandlers_.rbegin(); it != tryHandlers_.rend();
             ++it) {
            if (!exitsAllFrames && it->loopDepthAtEntry <= targetLoopIdx)
                break;
            eligible.push_back(&*it);
        }
        // Wire hops from the OUTERMOST eligible frame inward, so the
        // terminator's own edge (below) lands on the INNERMOST clone —
        // finally runs innermost-first, matching real execution order.
        CFGBlock* target = ultimateTarget;
        for (auto it = eligible.rbegin(); it != eligible.rend(); ++it) {
            TryHandlerContext* frame = *it;
            if (!frame->finallyEntry) continue; // try…catch, no finally
            target = getOrMakeFinallyClone(*frame, target);
        }
        return target;
    }

    // fix #4180: return the memoized clone of `frame`'s finally body that
    // routes to `target`, building one on first request. See
    // FinallyCloneState's comment for what the closure captures.
    CFGBlock* getOrMakeFinallyClone(TryHandlerContext& frame,
                                     CFGBlock* target) {
        auto& state = *frame.finallyClones;
        auto it = state.byTarget.find(target);
        if (it != state.byTarget.end()) return it->second;
        CFGBlock* entry = state.makeClone(target);
        state.byTarget[target] = entry;
        return entry;
    }

    // fix #4194 (§21.5/§21.7/§28.3/§31.2, v0.298): the SAME coarse
    // "throwable call somewhere in this try body might have thrown"
    // over-approximation visitTry adds from `tryBodyEnd` (the fall-through
    // tail) — `cfg_.addExceptionEdge(tryBodyEnd, catchDispatch)` — but from
    // an early-exit statement's own block instead. Without this, a try body
    // whose reachable tail is a return/break/continue rather than a
    // fall-through gets NO exception-edge modeling of its throwable calls at
    // all: `inner.runInto(s.body, tryEntry)` returns null for any body
    // ending in one of these terminators (see runInto's dead-block comment),
    // so visitTry's own `if (tryBodyEnd) { ... addExceptionEdge(...) }`
    // never runs — a call earlier in the body (e.g. `this.Risky()` before a
    // `return`) has no CFG edge showing it might have thrown before that
    // return's own (correctly #4180-routed) normal-exit edge took effect.
    //
    // fix #4201 (same §§, v0.298): fires for a CATCH ARM's own body too, not
    // just a try body. #4194 originally excluded arm frames here via
    // `!isArmContext`, because nothing modeled the arm body's FALL-THROUGH
    // tail either — landing here only for the terminator case (return /
    // break / continue), with nothing for the fall-through case, would have
    // been an inconsistent partial model, so both ship together: see the
    // mirrored `cfg_.addExceptionEdge(armEnd, armInner.exceptionTarget())`
    // added right after `armInner.visitStmt` in the arm-building loop below.
    // `exceptionTarget()` already answers correctly for both frame kinds
    // without any change here: a normal (non-arm) frame's `catchDispatch`
    // IS the landing point, while an arm frame (`isArmContext`) hops through
    // THIS try's finally (if any) before reaching the OUTER target instead
    // (#4180's routing, reused unchanged) — matching the invariant that a
    // throw inside a catch arm is never caught by that arm's own try.
    //
    // This fires while still lexically inside a try's own body, or a catch
    // arm's own body — directly, or nested through if/while/nested-try (each
    // of which threads `tryHandlers_` down via its own sub-Builder, so
    // `tryHandlers_.back()` always names the innermost SUCH frame) — at
    // exactly the same nesting scope the fall-through edge already covers.
    // `exceptionTarget()` (not `routeExitThroughFinally`) is deliberately
    // the target: an exception from within a try's own body, or a catch
    // arm's own body, always lands at that frame's own computed target
    // directly, with no separate hop needed here — there is nothing being
    // "exited" yet, unlike a break/continue/return that must hop through
    // finally chains to reach an OUTER destination.
    void addApproximateTryBodyExceptionEdge(CFGBlock* from) {
        if (tryHandlers_.empty()) return;
        cfg_.addExceptionEdge(from, exceptionTarget());
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
        if (auto* caseStmt = dynamic_cast<const ast::CaseStmt*>(&s)) {
            return visitCase(*caseStmt, from);
        }
        if (dynamic_cast<const ast::ReturnStmt*>(&s)) {
            from->stmts.push_back(&s);
            recordStmtAwaits(from, s); // async-readiness 3a (additive)
            // fix #4194: a throwable call earlier in this try body could
            // have thrown before this return runs — see
            // addApproximateTryBodyExceptionEdge.
            addApproximateTryBodyExceptionEdge(from);
            // fix #4180 (§21.7, v0.298): a return leaves every enclosing
            // try, so route through each one's active finally first — see
            // routeExitThroughFinally.
            cfg_.addEdge(from, routeExitThroughFinally(
                                    cfg_.exit(), /*exitsAllFrames=*/true, 0));
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
            // fix #4194: a throwable call earlier in this try body could
            // have thrown before this break runs — see
            // addApproximateTryBodyExceptionEdge.
            addApproximateTryBodyExceptionEdge(from);
            // fix #4180 (§21.5, v0.298): route through the finally of every
            // enclosing try this break actually EXITS (the target loop was
            // already active when that try was entered) before reaching the
            // loop's `afterEnd` — see routeExitThroughFinally. `loops_`
            // empty is the pre-existing defensive fallback for a malformed
            // break outside any loop (sema-rejected in practice); treat it
            // like a return (exits every active frame) rather than leaving
            // it unrouted.
            if (!loops_.empty()) {
                // §21.5 (v0.201, #2051): a labeled `break` exits the target loop
                // `targetLoopOffset` levels out (0 = innermost), which sema
                // resolved. The loops_ nesting matches, so index from the back.
                std::size_t idx = loopIndexAtOffset(brk->targetLoopOffset);
                cfg_.addEdge(from, routeExitThroughFinally(
                                        loops_[idx].afterEnd,
                                        /*exitsAllFrames=*/false, idx));
            } else {
                cfg_.addEdge(from, routeExitThroughFinally(
                                        cfg_.exit(),
                                        /*exitsAllFrames=*/true, 0));
            }
            return nullptr;
        }
        if (auto* cnt = dynamic_cast<const ast::ContinueStmt*>(&s)) {
            from->stmts.push_back(&s);
            // fix #4194: a throwable call earlier in this try body could
            // have thrown before this continue runs — see
            // addApproximateTryBodyExceptionEdge.
            addApproximateTryBodyExceptionEdge(from);
            // fix #4180: same routing as BreakStmt above, targeting the
            // loop's `header` instead of `afterEnd`.
            if (!loops_.empty()) {
                // §21.5 (v0.201): a labeled `continue` advances the target loop.
                std::size_t idx = loopIndexAtOffset(cnt->targetLoopOffset);
                cfg_.addEdge(from, routeExitThroughFinally(
                                        loops_[idx].header,
                                        /*exitsAllFrames=*/false, idx));
            } else {
                cfg_.addEdge(from, routeExitThroughFinally(
                                        cfg_.exit(),
                                        /*exitsAllFrames=*/true, 0));
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
        // fix #4339 (§28.3, v0.298): the CFG builder's standard reuse
        // pattern promotes `from` to a Branch block (visitBranch below).
        // That is sound for a Linear predecessor — its kind carries no
        // dataflow semantics — but is UNSOUND for a Merge predecessor:
        // when a sibling (non-nested) second `if` statement follows a
        // first whose branches clash, the first `if`'s Merge block IS the
        // second `if`'s condition's predecessor (visitIf's `from` is the
        // previous block; visitBranch then mutates that block in place).
        // The in-place kind promotion overwrote `Merge` with `Branch`, so
        // `computeEntry`'s `b->kind == flow::CFGBlock::Kind::Merge`
        // gate (`isIfMerge`) was false on the final emit pass, the
        // §28.3 branch-join clash report was suppressed, and the
        // poisoned merge state silently leaked the rejected branch's
        // value into codegen — measured as one Res per loop iteration
        // under LeakSanitizer at 200x amplification, and zero diagnostics
        // on a fresh build of the parent commit (issue 4339). Insert a
        // fresh Linear block between Merge and the if's Branch when
        // `from` is itself a Merge block; the fresh block then gets the
        // usual Branch promotion in visitBranch, while Merge's kind
        // survives intact for the dataflow's `isIfMerge` decision.
        if (from->kind == CFGBlock::Kind::Merge) {
            CFGBlock* bridge = cfg_.makeBlock(CFGBlock::Kind::Linear);
            cfg_.addEdge(from, bridge);
            from = bridge;
        }
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

    // fix #4146 (§19, §28.2, §28.3 K1.4, v0.298): a `case` STATEMENT. Each
    // arm becomes its own block sequence (guard → body), all joining at a
    // fresh Merge block. The scrutinee is recorded as a back-pointer on the
    // case's entry block (`caseScrutinee`) so the ownership pass can locate
    // it for the K8 full-expression boundary mark (`disposeOwnedScrutinee`)
    // without re-walking the AST. The no-match fallthrough (§19.6) is a
    // real edge from the entry block to the merge block, gated by the
    // AST's cached exhaustiveness verdict (`noMatchEdgeStaticallyDead`).
    //
    // This retires the long hand-rolled armEnds/joinInto walker the
    // ownership pass's `MethodOwnership::visitStmt` and
    // `applyUsingVarBorrowPinInLambdaStmt` had to keep in lockstep with the
    // real per-method CFG's if/while/for/try branches — the duplicated
    // arm-join predicate that produced the entire
    // #4116/#4117/#4138/#4141/#4142 family of false-rejects. The CFG-driven
    // dataflow (`computeEntry`'s `joinInto` at the merge block) now
    // computes the arm join for free, and the existing real CFG edges
    // (`addEdge` from each arm's terminator to the function exit / loop
    // afterEnd / header) handle return/throw/break/continue routing — no
    // separate `armNeverFallsThrough` predicate needed at this layer.
    CFGBlock* visitCase(const ast::CaseStmt& s, CFGBlock* from) {
        CFGBlock* merge = cfg_.makeBlock(CFGBlock::Kind::Merge);
        // Record the originating `case` on the merge so a forward analysis
        // can locate the §28.3 branch-join diagnostic at the case's span
        // (mirrors visitIf's merge->sourceStmt).
        merge->sourceStmt = &s;
        // fix #4146: also record the originating `case` on the entry
        // block so the ownership pass's processBlock can fire the K8
        // full-expression boundary mark (`disposeOwnedScrutinee`, #1795)
        // without walking successors to find the merge — the entry block
        // is where the case construct's `case EXPR of` keyword lives, and
        // its K8 mark needs the CaseStmt AST node to set the flag on.
        // The merge's own `sourceStmt` already serves the §28.3
        // branch-join diagnostic lookup; this entry-side copy is additive.
        from->sourceStmt = &s;
        // Record the scrutinee Expr on the entry block so the ownership
        // pass's processBlock can fire the K8 / checkRead side-band actions
        // on it (mirrors visitIfBranches' `from->condition` field for
        // if/elif conditions, except here it is the scrutinee, not the
        // condition — a `case` has both, in different places).
        from->caseScrutinee = s.scrutinee.get();
        // The scrutinee's `await` sites (§30.2) execute in this entry
        // block; record them so a later async-ownership pass finds them
        // (mirrors visitIfBranches' `recordAwaitsInExpr(cfg_, from, ...)`
        // for the if condition, but here for the scrutinee). The pattern
        // match itself is non-`await`-bearing (Pattern is not an Expr and
        // owns no UnaryExpr); only the scrutinee expression can carry
        // awaits at this statement position.
        if (s.scrutinee) recordAwaitsInExpr(cfg_, from, s.scrutinee.get());
        // For each arm: emit a Branch block (the pattern-test decision
        // point), a Linear block for the body, and connect the body's
        // fall-through exit to the merge block. The arm guard (`when EXPR`)
        // runs in the same Branch block as a follow-on guard (mirrors
        // catch-arm `catchGuard` on the arm entry block — see #1591's
        // rationale, which transfers verbatim here).
        for (const auto& arm : s.arms) {
            CFGBlock* armEntry = cfg_.makeBlock(CFGBlock::Kind::Branch);
            cfg_.addEdge(from, armEntry);
            // Record the arm's `when` guard (if any) on the arm entry
            // block so the ownership pass's processBlock walks it through
            // handleDischarge exactly like an if/while condition. The
            // pattern itself is a Pattern (not an Expr), so it cannot carry
            // an `await` and is not recorded via recordAwaitsInExpr — only
            // the guard's expression form can. Mirrors visitTry's
            // catch-arm `arm.guard` handling at the corresponding code path.
            if (arm.guard) {
                armEntry->catchGuard = arm.guard.get();
                recordAwaitsInExpr(cfg_, armEntry, arm.guard.get());
            }
            // Walk the arm body in a sub-Builder so loops_/tryHandlers_ are
            // preserved. For Block-form arms, run the body via runInto;
            // for Expression-form arms, the arm body is an Expr (not a
            // Stmt list) and is recorded on the arm entry block as a
            // back-pointer (mirrors the scrutinee pattern above) so
            // codegen / the ownership pass can locate it.
            CFGBlock* armExit;
            if (arm.bodyKind == ast::CaseArmBodyKind::Expression) {
                if (arm.exprBody) {
                    armEntry->caseArmExprBody = arm.exprBody.get();
                    recordAwaitsInExpr(cfg_, armEntry, arm.exprBody.get());
                }
                armExit = armEntry; // no body stmts → no fall-through edge
                cfg_.addEdge(armExit, merge);
            } else {
                Builder inner(cfg_, treatTryAsOpaque_);
                inner.loops_ = loops_;
                inner.tryHandlers_ = tryHandlers_;
                armExit = inner.runInto(arm.stmts, armEntry);
                if (armExit) cfg_.addEdge(armExit, merge);
            }
        }
        // No-match fallthrough (§19.6): an unmatched value in statement
        // position "falls through with no action" — a real edge to the
        // merge block. The AST's cached exhaustiveness verdict
        // (`noMatchEdgeStaticallyDead`, set by ExprTC::caseArmsExhaustive
        // — see #4139) gates whether this edge is present: when set, the
        // no-match edge is provably dead and codegen MUST lower it as a
        // defined trap (`totalityGranted`, raising MatchFailureException or
        // aborting) rather than as a fall-through to end-of-callable.
        if (!s.noMatchEdgeStaticallyDead) {
            cfg_.addEdge(from, merge);
        }
        return merge;
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

        // fix #4180 (§31.2, v0.298): build the per-target finally-clone state
        // used to route an early return/break/continue/throw out of the try
        // body OR a catch arm through this finally before reaching its real
        // target (routeExitThroughFinally / exceptionTarget). Captures the
        // finally AST + the OUTER loop/try context by value — the same
        // context the ORIGINAL fall-through finally build below uses
        // (`finallyInner.loops_ = loops_; finallyInner.tryHandlers_ =
        // tryHandlers_;`) — so a clone sees identical enclosing nesting. Null
        // when there is no finally (nothing to hop through).
        std::shared_ptr<FinallyCloneState> finallyClones;
        if (finallyEntry) {
            finallyClones = std::make_shared<FinallyCloneState>();
            bool opaque = treatTryAsOpaque_;
            std::vector<LoopContext> outerLoops = loops_;
            std::vector<TryHandlerContext> outerTryHandlers = tryHandlers_;
            const std::vector<std::unique_ptr<ast::Stmt>>* finallyBodyPtr =
                &s.finallyBody;
            CFG* cfgPtr = &cfg_;
            finallyClones->makeClone =
                [cfgPtr, opaque, outerLoops, outerTryHandlers,
                 finallyBodyPtr](CFGBlock* target) -> CFGBlock* {
                CFGBlock* cloneEntry = cfgPtr->makeBlock(CFGBlock::Kind::Linear);
                Builder cloneBuilder(*cfgPtr, opaque);
                cloneBuilder.loops_ = outerLoops;
                cloneBuilder.tryHandlers_ = outerTryHandlers;
                CFGBlock* cloneEnd =
                    cloneBuilder.runInto(*finallyBodyPtr, cloneEntry);
                if (cloneEnd) cfgPtr->addEdge(cloneEnd, target);
                return cloneEntry;
            };
        }

        // Build try body with this handler context active.
        Builder inner(cfg_, treatTryAsOpaque_);
        inner.loops_ = loops_;
        inner.tryHandlers_ = tryHandlers_;
        inner.tryHandlers_.push_back(
            {catchDispatch, finallyEntry, afterTry, loops_.size(),
             finallyClones, /*isArmContext=*/false});
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
            // fix #4180: give the arm body a frame for THIS try's OWN
            // finally, so an early return/break/continue/throw inside the
            // arm still runs it before leaving — pre-fix, an arm body had NO
            // path to this try's finally at all except the "falls through"
            // edge below. `catchDispatch` here is deliberately the OUTER
            // exceptionTarget() (evaluated on `this`, i.e. BEFORE `inner`'s
            // push above — the outer, one-frame-popped stack), not this
            // try's own dispatch — a throw in a catch handler is still not
            // caught by its own try (unchanged from before this fix).
            // `isArmContext=true` tells exceptionTarget() to hop through
            // `finallyEntry` before landing at that outer target instead of
            // landing here directly, matching what routeExitThroughFinally
            // now does for return/break/continue via loopDepthAtEntry.
            //
            // narrow #4194 (§21.5/§21.7/§28.3/§31.2, v0.298): pushed
            // UNCONDITIONALLY now, not just `if (finallyEntry)` — a
            // no-finally arm's Builder used to inherit `tryHandlers_`
            // UNCHANGED, so `armInner.tryHandlers_.back()` named the OUTER
            // frame, not "am I directly inside this arm's own body". That
            // silently broke `addApproximateTryBodyExceptionEdge`'s
            // `!isArmContext` guard (added by this same fix) for any try
            // with catch arms and no finally: the guard read the wrong
            // frame and could scope a coarse try-body exception edge as if
            // it were still inside an OUTER try's body while actually
            // inside a sibling catch arm — measured false reject on two
            // mutually exclusive catch arms that each `move x` and
            // `return` (see this fix's own test row). `finallyEntry`/
            // `finallyClones` stay null when there is no finally — both
            // `exceptionTarget()` (`top.isArmContext && top.finallyEntry`)
            // and `routeExitThroughFinally` (`if (!frame->finallyEntry)
            // continue;`) already treat a null-finallyEntry frame as
            // "nothing to hop through" and never dereference
            // `finallyClones` in that case, so this is transparent to both
            // — the ONLY thing it changes is that `.back().isArmContext`
            // now always answers correctly for arm-body statements.
            armInner.tryHandlers_.push_back(
                {/*catchDispatch=*/exceptionTarget(), finallyEntry,
                 afterTry, loops_.size(), finallyClones,
                 /*isArmContext=*/true});
            CFGBlock* armEnd = armEntry;
            if (arm.body) {
                armEnd = armInner.visitStmt(*arm.body, armEntry);
            }
            if (armEnd) {
                cfg_.addEdge(armEnd,
                             finallyEntry ? finallyEntry : afterTry);
                // fix #4201 (§21.5/§21.7/§28.3/§31.2, v0.298): mirror
                // `tryBodyEnd`'s own coarse "a throwable call somewhere in
                // this body might have thrown" exception edge (above, at
                // `cfg_.addExceptionEdge(tryBodyEnd, catchDispatch)`) for the
                // ARM's own body falling through. Before this fix, a
                // throwable call inside a catch arm's body that was not an
                // explicit `throw` (e.g. `this.Risky(); x.Dispose();`) had
                // NO CFG representation of "this call might have thrown
                // before `x.Dispose()` ran" at all — an explicit `throw` was
                // already correctly modeled via #4180's `exceptionTarget()`
                // arm hop, but nothing approximated any OTHER throwable call.
                // `armInner.exceptionTarget()` reads
                // `armInner.tryHandlers_.back()` — still the arm frame
                // pushed above, since visiting the body never pops it — so
                // it resolves exactly like an explicit `throw` written right
                // here would: NOT this try's own `catchDispatch` (a throw in
                // a catch arm is not caught by its own try), but the OUTER
                // target, hopping through this try's finally first when one
                // is active (#4180's isArmContext routing, reused
                // unchanged). See addApproximateTryBodyExceptionEdge's
                // comment for the sibling terminator-case (return / break /
                // continue inside the arm body) half of this same fix.
                cfg_.addExceptionEdge(armEnd, armInner.exceptionTarget());
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

// prep #4146 Stage 2-A (§28.3, v0.298): build a CFG for a lambda body.
// Block-bodied lambdas: walks `lam->stmts` with the Builder, same as buildCFG.
// Expression-bodied lambdas: creates a single Linear block that holds the
// `exprBody` and connects it to the exit — the expression's value is the
// lambda's implicit return value, and recording it in the block is sufficient
// for ownership analysis to reach every sub-expression position.
std::unique_ptr<CFG> CFG::buildLambdaBodyCFG(
    const ast::LambdaExpr* lam,
    const CFGBuildOptions& opts) {
    auto cfg = std::make_unique<CFG>();
    Builder builder(*cfg, opts.treatTryAsOpaque);
    CFGBlock* end = nullptr;
    if (lam->isBlockBody) {
        end = builder.build(lam->stmts);
    } else {
        // Expression-bodied lambda: create a synthetic block that records the
        // expression's await sites (if any) and links to exit. The expression
        // is the implicit return value; ownership analysis reaches it through
        // the block's stmt list exactly as it reaches every other expression
        // position in a Linear block.
        CFGBlock* block = cfg->makeBlock(CFGBlock::Kind::Linear);
        cfg->addEdge(cfg->entry(), block);
        recordAwaitsInExpr(*cfg, block, lam->exprBody.get());
        end = block;
    }
    if (end) {
        cfg->addEdge(end, cfg->exit());
    }
    return cfg;
}

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
