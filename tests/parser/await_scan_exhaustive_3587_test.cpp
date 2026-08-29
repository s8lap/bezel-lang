// Structural tests for the shared await-site walk (src/common/AwaitScan.hpp,
// fix #3587 — CLAUDE.md §6 dedup: "four hand-written copies of 'does this
// contain an await?'"; issue #2251 is the CaseExpr half).
//
// These pin `ast::BindExpr` at the PARSER level rather than as a compile-
// and-run behavioral test, because the gap there is LATENT: sema's
// `inBindAtom_` gate (ExprTypecheck_Exprs.cpp, §17.15/§30.5, #2098) rejects
// `await` inside a `(var x := E; body)` binding atom in an async body
// outright, so no program with an `await` there ever reaches codegen or
// ownership — a behavioral test proving "the compiled program's VALUE is
// correct" cannot be written; the program never compiles (by a DIFFERENT,
// deliberate, and still-in-force rejection, #2122). What CAN be pinned, and
// is pinned here, is the STRUCTURE: `bezel::common::forEachAwaitSite`, run
// directly on a hand-parsed AST (bypassing sema entirely, so `inBindAtom_`
// never engages), finds every `await` inside a BindExpr's clause
// initializers and body. This is exactly the arm three of the four original
// hand-written copies lacked (only `flow/CFG.cpp`'s `recordAwaitsInExpr`
// had it) — see AwaitScan.hpp's header for the full rationale.
//
// The companion `ast::CaseExpr` gap (all FOUR original copies, including
// CFG.cpp) is NOT latent — codegen_2251_test.cpp
// (case_expr_await_uncounted_2251_test.cpp) pins it behaviorally (a real
// cross-thread hang, fixed). The RelOpPattern case here re-checks the same
// shape structurally, cheaply, with no `Task.Delay`/30s-timeout exposure.
//
// `await` parses as a plain keyword-led UnaryExpr regardless of surrounding
// context (src/parser/Parser_Exprs.cpp:308) — only sema's `inAsyncContext_`
// / `inBindAtom_` checks are context-sensitive — so a non-`async`,
// non-typechecked parse still produces the exact same AST shape codegen and
// ownership would see if sema admitted it.

#include <string>

#include "ast/Node.hpp"
#include "common/AwaitScan.hpp"
#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::ParseResult;

namespace {

// Parses `module M; class C public method M() _ := EXPR; end M; end C;` and
// returns the DiscardStmt's value expression (the parsed `EXPR`), or null on
// any parse-shape mismatch. Mirrors bind_atom_parse_2098_test.cpp /
// marked_transfer_parse_2080_test.cpp's `exprFromStmt`, returning the AST
// node itself instead of `exprToString`'s rendering.
const bezel::ast::Expr* discardExprOf(const ParseResult& r) {
    if (r.diagnostics.hasErrors()) return nullptr;
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    const auto* ds =
        dynamic_cast<const bezel::ast::DiscardStmt*>(md->stmts[0].get());
    if (!ds) return nullptr;
    return ds->value.get();
}

unsigned countAwaitSites(const bezel::ast::Expr* e) {
    unsigned n = 0;
    bezel::common::forEachAwaitSite(
        e, [&](const bezel::ast::UnaryExpr*) { ++n; });
    return n;
}

}  // namespace

// §17.15 / A.7 BindExpr, §30.2 `await` (v0.286): a single-clause binding
// atom whose CLAUSE INITIALIZER contains an `await`. The pre-fix
// `Emitter::exprContainsAwait` / `countAwaitsInExpr`
// (LlvmBackend_Async.cpp) and `MethodOwnership::exprContainsAwait`
// (Ownership_Checks.cpp) had no `BindExpr` arm at all, so this would have
// been silently undercounted (0) had #2122 lifted the `inBindAtom_`
// rejection without also fixing those three copies — exactly the coupling
// #3587 documents. Only `flow/CFG.cpp` had this arm before.
BEZEL_TEST(await_scan_bindexpr_3587, clause_initializer_await_found) {
    auto r = parseAll(
        "module M; class C public method M() "
        "  _ := (var x := await Foo(); x); "
        "end M; end C;");
    const auto* e = discardExprOf(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    BEZEL_EXPECT_TRUE(dynamic_cast<const bezel::ast::BindExpr*>(e) != nullptr);
    BEZEL_EXPECT_EQ(countAwaitSites(e), 1u);
}

// Same node kind, `await` in the ATOM'S BODY instead of a clause
// initializer — the walk's other BindExpr sub-position
// (`bx->body`, AwaitScan.cpp).
BEZEL_TEST(await_scan_bindexpr_3587, body_await_found) {
    auto r = parseAll(
        "module M; class C public method M() "
        "  _ := (var x := 1; await Foo(x)); "
        "end M; end C;");
    const auto* e = discardExprOf(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    BEZEL_EXPECT_EQ(countAwaitSites(e), 1u);
}

// Two clauses AND the body each carry their own `await` — proves the walk
// sums across every clause, not just the first (`for (const auto& cl :
// bx->clauses)` in AwaitScan.cpp), plus the body: 3 sites total.
BEZEL_TEST(await_scan_bindexpr_3587, multi_clause_and_body_awaits_summed) {
    auto r = parseAll(
        "module M; class C public method M() "
        "  _ := (var a := await F(); var b := await G(a); await H(b)); "
        "end M; end C;");
    const auto* e = discardExprOf(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    BEZEL_EXPECT_EQ(countAwaitSites(e), 3u);
}

// A binding atom with NO await anywhere counts zero — the walk must not
// over-report either.
BEZEL_TEST(await_scan_bindexpr_3587, no_await_counts_zero) {
    auto r = parseAll(
        "module M; class C public method M() "
        "  _ := (var x := F(); x); "
        "end M; end C;");
    const auto* e = discardExprOf(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    BEZEL_EXPECT_EQ(countAwaitSites(e), 0u);
}

// §19.3 "Patterns" (v0.286): a RelOpPattern bound (`> EXPR`) inside a
// CaseExpr arm carries an `await`. Structural re-check of
// case_expr_await_uncounted_2251_test.cpp's
// `await_in_relop_pattern_bound` behavioral repro — reached only via
// `forEachAwaitSiteInCaseArm` -> `forEachAwaitSiteInPattern`, a layer NONE
// of the four original copies visited at all (none looked at
// `ast::CaseArm`).
BEZEL_TEST(await_scan_bindexpr_3587, case_relop_pattern_await_found) {
    auto r = parseAll(
        "module M; class C public method M() "
        "  _ := (case 10 of > (await Slow()) => 100, _ => 200); "
        "end M; end C;");
    const auto* e = discardExprOf(r);
    BEZEL_EXPECT_TRUE(e != nullptr);
    BEZEL_EXPECT_TRUE(dynamic_cast<const bezel::ast::CaseExpr*>(e) != nullptr);
    BEZEL_EXPECT_EQ(countAwaitSites(e), 1u);
}
