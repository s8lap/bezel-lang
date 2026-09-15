// Structural CFG tests for issue #4211 — restoring #4180's discriminating
// power.
//
// fix #4180 (§21.5/§21.7/§31.2, v0.298) made `Builder::routeExitThroughFinally`
// (src/flow/CFG.cpp) route an early `return`/`break`/`continue` through every
// active `finally` it actually exits before reaching its real target, via a
// per-(frame, target) MEMOIZED CLONE of the finally body
// (`getOrMakeFinallyClone`). `cfg_try_finally_early_exit_4180_test.cpp`'s Rows
// 1/2 (`return`/`break`) pin this via an OWNERSHIP DIAGNOSTIC (a double
// `move x`, once in the try body and once in the finally, detected only if
// the finally is analyzed reachable from the early exit).
//
// Issue #4211: that diagnostic-based signal stopped discriminating #4180's
// OWN clone-routing mechanism once fix #4194 landed. #4194 added
// `Builder::addApproximateTryBodyExceptionEdge`, which UNCONDITIONALLY adds a
// coarse "a throwable call earlier in this try body could have thrown"
// exception edge from EVERY return/break/continue statement's own block to
// `exceptionTarget()` (the innermost active frame's `catchDispatch`) —
// regardless of whether the body actually contains a throwable call. For a
// SINGLE, unnested try/finally with no catch arms (Rows 1/2/3's exact shape),
// `catchDispatch`'s own unconditional `s.catches.empty()` pass-through then
// ALWAYS wires into the REAL (non-clone) `finallyEntry` too — so the real
// finally becomes reachable via #4194's edge alone, carrying the SAME
// already-moved state the clone would, independent of whether
// `routeExitThroughFinally`'s OWN edge exists at all.
//
// MEASURED (this issue's own 3-variant methodology, `/var/tmp/bezel-scratch/
// r18/iso-4211-neutered`, git-untracked scratch build): reverting ONLY the
// three call sites (`ReturnStmt`/`BreakStmt`/`ContinueStmt` in
// `Builder::visitStmt`) to their pre-#4180 direct-edge form — bypassing
// `routeExitThroughFinally` entirely — while KEEPING #4194's
// `addApproximateTryBodyExceptionEdge` calls intact, leaves ALL 9 rows in
// `cfg_try_finally_early_exit_4180_test.cpp` passing unchanged, INCLUDING Row
// 1 (`return_through_active_finally_rejects_double_move_21_7`) and Row 2
// (`break_through_active_finally_rejects_double_move_21_5`) — confirming the
// issue's own "reasoned suspicion, not measurement" about Row 1 is correct,
// and that this is structural, not incidental: for this shape (no catch
// arms), #4194's coarse edge ALWAYS parallels `routeExitThroughFinally`'s
// edge into a representation of the SAME finally body, carrying the SAME
// dataflow state (both edges originate from the SAME block), so no
// ownership-diagnostic-based row in that shape can discriminate the two
// mechanisms. This is documented as an explicit non-discrimination note on
// Rows 1/2 in that file (this same commit) — option 2 of the issue's two
// suggested directions.
//
// THIS FILE takes option 1's alternative: a DIRECT STRUCTURAL test of the
// CFG graph itself, bypassing the ownership-diagnostic layer (and #4194's
// contamination of it) entirely. `addApproximateTryBodyExceptionEdge`'s own
// edge always targets `catchDispatch` — a `CFGBlock::Kind::Branch` block
// with NO `stmts` of its own (`tryForDispatch` is set instead) — so it can
// NEVER be confused, by pointer identity, with a `finally` clone's entry
// block, whose `stmts` begin with the SAME `bezel::ast::Stmt*` as the source
// `TryStmt::finallyBody`'s first element (`FinallyCloneState::makeClone`
// visits the identical AST vector into a fresh block). Asserting "the
// terminator's own block has a DIRECT successor edge into a block whose
// first statement is (by pointer) the finally body's first statement" is
// therefore immune to #4194's edge (whose target block has zero stmts) and
// exercises `routeExitThroughFinally`/`getOrMakeFinallyClone` specifically.
//
// MEASURED (same 3-variant build): every BEZEL_TEST in this file FAILS under
// the neutered (routeExitThroughFinally-bypassed) build and PASSES on the
// real tree — see this commit's body for the exact counts.

#include "support/cfg_support.hpp"
#include "test_harness.hpp"

#include <functional>

using bezel::flow::CFG;
using bezel::flow::CFGBlock;
using bezel::test::parseFirstMethodCFG;

namespace {

// Recursively find the first bezel::ast::TryStmt in a statement list (descending
// into while/if/do bodies as needed by this file's shapes — a small,
// file-local walker; the CFG test support header has no AST-search helper
// and none of this file's shapes need more than while/try nesting).
const bezel::ast::TryStmt* findFirstTry(
    const std::vector<std::unique_ptr<bezel::ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* t = dynamic_cast<const bezel::ast::TryStmt*>(s.get())) return t;
        if (auto* w = dynamic_cast<const bezel::ast::WhileStmt*>(s.get())) {
            if (auto* found = findFirstTry(w->body)) return found;
        }
        if (auto* d = dynamic_cast<const bezel::ast::DoEndBlock*>(s.get())) {
            if (auto* found = findFirstTry(d->body)) return found;
        }
    }
    return nullptr;
}

// Find the CFGBlock whose `.stmts` contains a statement satisfying `pred`
// (by dynamic_cast success) — the block a terminator statement's own
// visitStmt call appended it to.
CFGBlock* findBlockContaining(
    const CFG& cfg, const std::function<bool(const bezel::ast::Stmt*)>& pred) {
    for (const auto& b : cfg.blocks()) {
        for (const auto* s : b->stmts) {
            if (pred(s)) return b.get();
        }
    }
    return nullptr;
}

// True when `from` has a DIRECT successor block whose `.stmts` begins with
// exactly the pointer `target` — i.e. a clone (or the real block) built by
// visiting `target`'s owning statement list. #4194's coarse edge always
// targets a stmt-less Branch block, so this can never accidentally match
// that edge.
bool hasDirectSuccessorStartingWith(CFGBlock* from, const bezel::ast::Stmt* target) {
    for (auto* succ : from->succs) {
        if (!succ->stmts.empty() && succ->stmts.front() == target) return true;
    }
    return false;
}

}  // namespace

// =====================================================================
// Row 1 analogue: `return` directly exits an active try's own finally —
// same shape as cfg_try_finally_early_exit_4180_test.cpp Row 1, structural
// assertion instead of an ownership diagnostic.
// =====================================================================
BEZEL_TEST(cfg_finally_clone_routing_4211,
           return_gets_direct_edge_to_finally_clone_spec_21_7) {
    auto r = parseFirstMethodCFG(
        "module M; class C public static method M(): int "
        "  try "
        "    cleanup1(); "
        "    return 1; "
        "  finally "
        "    cleanup2(); "
        "  end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const bezel::ast::TryStmt* trys = findFirstTry(
        dynamic_cast<const bezel::ast::MethodDecl*>(
            dynamic_cast<const bezel::ast::TypeDecl*>(
                r.sourceFile->topDecls[0].get())
                ->members[0].get())
            ->stmts);
    BEZEL_EXPECT_TRUE(trys != nullptr && trys->hasFinally &&
                       !trys->finallyBody.empty());
    CFGBlock* returnBlock = findBlockContaining(*r.cfg, [](const bezel::ast::Stmt* s) {
        return dynamic_cast<const bezel::ast::ReturnStmt*>(s) != nullptr;
    });
    BEZEL_EXPECT_TRUE(returnBlock != nullptr);
    BEZEL_EXPECT_TRUE(hasDirectSuccessorStartingWith(
        returnBlock, trys->finallyBody.front().get()));
}

// =====================================================================
// Row 2 analogue: `break` out of a `while true` loop, exiting an active
// try's own finally — the EXACT shape of the contaminated ownership-based
// row this issue is about.
// =====================================================================
BEZEL_TEST(cfg_finally_clone_routing_4211,
           break_gets_direct_edge_to_finally_clone_spec_21_5) {
    auto r = parseFirstMethodCFG(
        "module M; class C public static method M(): int "
        "  while true do "
        "    try "
        "      cleanup1(); "
        "      break; "
        "    finally "
        "      cleanup2(); "
        "    end; "
        "  end; "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const bezel::ast::TryStmt* trys = findFirstTry(
        dynamic_cast<const bezel::ast::MethodDecl*>(
            dynamic_cast<const bezel::ast::TypeDecl*>(
                r.sourceFile->topDecls[0].get())
                ->members[0].get())
            ->stmts);
    BEZEL_EXPECT_TRUE(trys != nullptr && trys->hasFinally &&
                       !trys->finallyBody.empty());
    CFGBlock* breakBlock = findBlockContaining(*r.cfg, [](const bezel::ast::Stmt* s) {
        return dynamic_cast<const bezel::ast::BreakStmt*>(s) != nullptr;
    });
    BEZEL_EXPECT_TRUE(breakBlock != nullptr);
    BEZEL_EXPECT_TRUE(hasDirectSuccessorStartingWith(
        breakBlock, trys->finallyBody.front().get()));
}

// =====================================================================
// Row 3 analogue: `continue`, same shape as break above but targeting the
// loop header instead of its after-block.
// =====================================================================
BEZEL_TEST(cfg_finally_clone_routing_4211,
           continue_gets_direct_edge_to_finally_clone_spec_21_5) {
    auto r = parseFirstMethodCFG(
        "module M; class C public static method M(): int "
        "  while true do "
        "    try "
        "      cleanup1(); "
        "      continue; "
        "    finally "
        "      cleanup2(); "
        "    end; "
        "  end; "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const bezel::ast::TryStmt* trys = findFirstTry(
        dynamic_cast<const bezel::ast::MethodDecl*>(
            dynamic_cast<const bezel::ast::TypeDecl*>(
                r.sourceFile->topDecls[0].get())
                ->members[0].get())
            ->stmts);
    BEZEL_EXPECT_TRUE(trys != nullptr && trys->hasFinally &&
                       !trys->finallyBody.empty());
    CFGBlock* continueBlock =
        findBlockContaining(*r.cfg, [](const bezel::ast::Stmt* s) {
            return dynamic_cast<const bezel::ast::ContinueStmt*>(s) != nullptr;
        });
    BEZEL_EXPECT_TRUE(continueBlock != nullptr);
    BEZEL_EXPECT_TRUE(hasDirectSuccessorStartingWith(
        continueBlock, trys->finallyBody.front().get()));
}
