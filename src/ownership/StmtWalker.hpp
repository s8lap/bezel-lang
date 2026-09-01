#pragma once

// StmtWalker — fix #3656's SINGLE source of truth for the statement grammar
// (§28.3 / §28.8.1 / §30.5 / §35.5, v0.286).
//
// Why this header exists. `Ownership_Checks.cpp` had FIVE hand-written
// statement walkers — `collectLocalDepthsStmt`, `collectStmtsInsideTryStmt`,
// `collectCaseArmBorrowSourcesStmt`, `collectIdentifiersInStmt`, and
// `collectOwnedConsumingAwaitsStmt` — each spelling its own `dynamic_cast<>`
// chain over the same statement grammar. The chains drifted independently:
// `CaseStmt` was missed by three of them (#3521 in v0.283, #3552 in v0.286,
// #3586 in this run's cycle 3), each gap a tier-1 soundness hole. The next
// statement kind added to `ast::Stmt` would have required auditing all five
// chains.
//
// `forEachNestedStmt` owns the grammar in ONE place. Every caller supplies only
// its per-statement action; the next statement kind becomes a one-line change
// to the dispatcher below instead of a five-site audit.
//
// Coverage. The walker visits every `ast::Stmt` reachable from the input
// through the container kinds: `IfStmt` / `WhileStmt` / `ForStmt` /
// `DoEndBlock` / `TryStmt` (body + catch arms + finally body) / `CaseStmt`
// (Block-form arm bodies). Leaf statement kinds (`LocalDecl`, `LocalConstDecl`,
// `ExpressionStmt`, `AssignmentStmt`, `ReturnStmt`, `YieldStmt`, `ThrowStmt`,
// `IncrementStmt`, `DiscardStmt`, `BreakStmt`, `ContinueStmt`) are visited but
// have no nested statements to recurse into — the caller's `fn` decides what
// to do with them.
//
// Per §6b (unification lane exclusion diff): the new walker admits exactly
// the union of the five old grammars (every kind all five walked, no kind any
// walked but the new one does not). The diff is documented in the commit
// body of #3656: for `collectLocalDepthsStmt` / `collectStmtsInsideTryStmt` /
// `collectOwnedConsumingAwaitsStmt` / `collectCaseArmBorrowSourcesStmt` /
// `collectIdentifiersInStmt` the old copy EXCLUDED no kind the new one
// ADMITTED, and the new one ADMITS no kind the old one EXCLUDED, in any
// caller.
//
// Two forms. The stateless form `forEachNestedStmt(s, fn)` is for callers
// whose action does not need state to propagate between parent and child.
// The stateful form `forEachNestedStmt(s, fn, state)` threads a `VisitState`
// through recursion: `depth` auto-increments for every scope-introducer kind
// (if / while / for / do-end / try body+catch+finally / case arm body);
// `insideTry` is sticky once set and turns true at every `try`'s body /
// catch / finally; `enclosingTry` is replaced with the IMMEDIATE enclosing
// `TryStmt*` on descent into a try's children. The two forms share the
// same dispatcher so adding a new statement kind touches one site.

#include "ast/Node.hpp"

namespace bezel::ownership {

// The state threaded through the stateful walker form. Caller-`fn` receives
// the state as its third parameter; `depth` is the Block-nesting depth (the
// method body is depth 0; each scope-introducer adds one — per §28.3 / §31.1
// "the binding lives at the arm body's depth"); `insideTry` is true once a
// `try` statement has been lexically entered on this recursion path and
// stays true for every nested statement below it; `enclosingTry` is the
// IMMEDIATE `TryStmt*` whose body / catch / finally the visited statement
// sits inside (per #3241 R2 / §28.8.1).
struct StmtVisitState {
    int depth = 0;
    bool insideTry = false;
    const ast::TryStmt* enclosingTry = nullptr;
};

// Stateless form. `fn(s)` is called for every statement reachable from `s`
// (including `s` itself) in pre-order source order. The walker handles all
// recursion into nested statement lists; the caller's `fn` does its per-
// statement action. State threading is the caller's responsibility (use
// mutable captures).
template <typename F>
void forEachNestedStmt(const ast::Stmt& s, F&& fn) {
    fn(s);
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) forEachNestedStmt(*bs, fn);
        for (const auto& el : ifs->elifBranches)
            for (const auto& bs : el.body)
                if (bs) forEachNestedStmt(*bs, fn);
        for (const auto& bs : ifs->elseBody)
            if (bs) forEachNestedStmt(*bs, fn);
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        for (const auto& bs : ws->body)
            if (bs) forEachNestedStmt(*bs, fn);
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& bs : fs->body)
            if (bs) forEachNestedStmt(*bs, fn);
    } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body)
            if (bs) forEachNestedStmt(*bs, fn);
    } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : ts->body)
            if (bs) forEachNestedStmt(*bs, fn);
        for (const auto& arm : ts->catches)
            if (arm.body) forEachNestedStmt(*arm.body, fn);
        for (const auto& bs : ts->finallyBody)
            if (bs) forEachNestedStmt(*bs, fn);
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        for (const auto& arm : cs->arms)
            for (const auto& bs : arm.stmts)
                if (bs) forEachNestedStmt(*bs, fn);
    }
}

// Stateful form. Threads `VisitState` through recursion: the walker
// auto-increments `depth` for every scope-introducer kind, sets `insideTry`
// to true and `enclosingTry` to the IMMEDIATE `TryStmt*` on descent into a
// try's body / catch / finally. Used by `collectLocalDepthsStmt` and
// `collectStmtsInsideTryStmt`; the two stateful callers.
template <typename F>
void forEachNestedStmt(const ast::Stmt& s, F&& fn, StmtVisitState state) {
    fn(s, state);
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        const int childDepth = state.depth + 1;
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, state.insideTry, state.enclosingTry});
        for (const auto& el : ifs->elifBranches)
            for (const auto& bs : el.body)
                if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, state.insideTry, state.enclosingTry});
        for (const auto& bs : ifs->elseBody)
            if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, state.insideTry, state.enclosingTry});
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        const int childDepth = state.depth + 1;
        for (const auto& bs : ws->body)
            if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, state.insideTry, state.enclosingTry});
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        const int childDepth = state.depth + 1;
        for (const auto& bs : fs->body)
            if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, state.insideTry, state.enclosingTry});
    } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        const int childDepth = state.depth + 1;
        for (const auto& bs : dob->body)
            if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, state.insideTry, state.enclosingTry});
    } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        // The try's body, catch arms, and finally body are `insideTry` from
        // here down, with THIS try as the IMMEDIATE encloser (per #3241 R2).
        const int childDepth = state.depth + 1;
        for (const auto& bs : ts->body)
            if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, /*insideTry=*/true, ts});
        for (const auto& arm : ts->catches)
            if (arm.body) forEachNestedStmt(*arm.body, fn, StmtVisitState{childDepth, /*insideTry=*/true, ts});
        for (const auto& bs : ts->finallyBody)
            if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, /*insideTry=*/true, ts});
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        // fix #3552 / #3586 / #3074: Block-form arm bodies are scope-
        // introducing blocks exactly like `do`-`end`. depth++ for them.
        const int childDepth = state.depth + 1;
        for (const auto& arm : cs->arms)
            for (const auto& bs : arm.stmts)
                if (bs) forEachNestedStmt(*bs, fn, StmtVisitState{childDepth, state.insideTry, state.enclosingTry});
    }
}

} // namespace bezel::ownership