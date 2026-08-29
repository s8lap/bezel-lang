#pragma once

// fix #3587 (CLAUDE.md §6 dedup) / #2251 — the ONE both-sources walk that
// finds every `await` UnaryExpr syntactically reachable from an expression
// (or a CaseArm's pattern) along a SYNCHRONOUS sub-expression edge — i.e. NOT
// descending into a nested LambdaExpr body, because an `async` lambda is its
// own async scope (§30.6): an `await` inside it suspends the LAMBDA's state
// machine, not the enclosing one.
//
// WHY THIS FILE EXISTS. Four independent hand-written copies of this walk
// existed — `Emitter::exprContainsAwait` / `countAwaitsInExpr`
// (src/codegen/LlvmBackend_Async.cpp), `MethodOwnership::exprContainsAwait`
// (src/ownership/Ownership_Checks.cpp), `recordAwaitsInExpr`
// (src/flow/CFG.cpp) — each independently enumerating its OWN subset of the
// 32 `ast::*Expr` struct kinds declared in Node.hpp (16 kinds in three of the
// four copies; 17 in the CFG.cpp copy, the one that additionally handled
// `ast::BindExpr`). MEASURED on HEAD 907ce0f (re-derived; the filing issue's
// line ranges/counts were off by one throughout — the functions are 55-125 /
// 169-235 / 11386-11456 / 48-144, and the per-copy counts are 16/16/16/17,
// not the issue's stated 17/17/17/18).
//
// TWO gaps, of different severity:
//
//   * `ast::BindExpr` — dropped by three of the four (everything except
//     CFG.cpp). LATENT today: sema's `inBindAtom_` gate
//     (ExprTypecheck_Exprs.cpp, §17.15/§30.5, #2098) rejects an `await`
//     anywhere inside a `(var x := E; body)` binding atom in an async body
//     outright, so no such program reaches codegen or ownership. Protected
//     only by #2122 (lifting that rejection) staying unlanded.
//
//   * `ast::CaseExpr` — dropped by ALL FOUR. Nothing in sema restricts
//     `await` inside a case-expression's scrutinee, an arm's `when` guard, an
//     arm's expression body, or a pattern's embedded constant expression
//     (RelOpPattern / RangePattern / ConstantPattern) — so this is NOT
//     latent (#2251). MEASURED on HEAD 907ce0f, three independent repros,
//     each hangs (`timeout 10 ./a.out` -> exit 124, stable across repeats):
//     `await` in the scrutinee, in an arm's `when` guard (via a RelOpPattern
//     bound), and in an arm's expression body. The undercounted await leaves
//     the async resume switch without a state block for it — the same
//     #441/#2226 hang shape (a pool worker fires the continuation with an
//     unhandled state, the switch falls to its `default: return`, and
//     `bz_task_complete` is never called).
//
// EXHAUSTIVENESS. `ast::Expr`/`ast::Pattern` carry NO discriminant kind enum
// (Node.hpp is RTTI-only: dynamic_cast chains, no `switch`), and
// `src/ast/Node.hpp` is held by another lane this run, so adding one is not
// an option here. The next-best available shape — and what this file does —
// is to enumerate EVERY `*Expr`/`*Pattern` struct explicitly in ONE
// dynamic_cast chain each (leaves included, doing nothing but returning), and
// call an UNCONDITIONAL `std::abort()` (see AwaitScan.cpp) instead of falling
// through to a silent "no await here" on any kind not recognised. This is
// deliberately NOT a plain `assert()`: the gate's own RelWithDebInfo build
// defines NDEBUG (CMakeLists.txt), which would silently compile out a plain
// assert and hand the exact silent-fallthrough failure this file exists to
// eliminate right back. A *new* `*Expr`/`*Pattern` struct added to Node.hpp
// without a matching arm here crashes loudly the first time an
// `await`-scanning caller exercises it, instead of silently undercounting.

#include <functional>

namespace bezel::ast {
struct Expr;
struct UnaryExpr;
struct Pattern;
struct CaseArm;
}  // namespace bezel::ast

namespace bezel::common {

using AwaitSiteFn = const std::function<void(const ast::UnaryExpr*)>&;

// Calls `onAwait(ue)` once for every explicit `await` UnaryExpr reachable
// from `e` (including `e` itself, when it is one) along a synchronous
// sub-expression edge. `e` may be null (no-op). See the file header for the
// LambdaExpr exclusion and the exhaustiveness contract.
void forEachAwaitSite(const ast::Expr* e, AwaitSiteFn onAwait);

// Same walk, rooted at a Pattern's embedded constant-expression slots:
// ConstantPattern::constants, RangePattern::rangeExpr, RelOpPattern::expr,
// and (recursively) AltPattern::members and
// DestructuringPattern::subPatterns. `p` may be null. MEASURED live: a
// RelOpPattern bound (`case … of > (await Slow()) => …, …`) hangs exactly
// like the scrutinee/guard/body positions above — see the file header.
void forEachAwaitSiteInPattern(const ast::Pattern* p, AwaitSiteFn onAwait);

// One CaseArm's await sites, in source order: pattern, guard, then the
// Expression-form body (`arm.exprBody`). A Block-form body's `stmts` are
// Stmt-level and out of this Expr-level walk's scope — see the
// statement-level `Emitter::containsAwait` / `countAwaits` CaseStmt handling
// (LlvmBackend_Async.cpp) instead.
void forEachAwaitSiteInCaseArm(const ast::CaseArm& arm, AwaitSiteFn onAwait);

}  // namespace bezel::common
