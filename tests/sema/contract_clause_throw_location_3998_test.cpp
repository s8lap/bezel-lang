// fix #3998 (§17.12 / §22.15 / §22.16, v0.295): the §22.15 pure-body
// "`throw` is not admitted in a pure body" diagnostic, when it fires for a
// `throw` reached inside a `requires(...)`/`ensures(...)` contract clause
// (PureChecker.cpp's `clauseMode_` arm), was reported at `0:0` — no real
// source position — instead of the `throw` token's actual line/column.
//
// Root cause (src/parser/Parser_Exprs.cpp, `parseBinaryExpression`): a
// `throw` reached ONLY as the right operand of a binary operator (§17.12's
// worked idiom `x ?? throw new E(...)`, and this issue's `... or (b ??
// throw new E())`) is parsed by a special-cased branch that called
// `parseExpressionImpl()` directly. Every OTHER `throw` in the grammar goes
// through `parseExpression()`, whose wrapper stamps `span = [start, end)`
// on the returned node (`stampSpan`, Parser_Exprs.cpp ~101-114) — the
// binary-RHS branch skipped that wrapper, so the `ThrowExpr` node kept its
// default-constructed span (line 0, col 0), and `PureBodyWalker::
// reportPureViolation` (PureChecker.cpp:699, called with `te->span.begin`
// at the clause-mode arm ~1305) faithfully reported that zero location.
//
// The fix (same file) calls `parseExpression()` instead of
// `parseExpressionImpl()` at that one call site, so the `ThrowExpr` gets a
// real span exactly like every other expression node — with no change to
// what is accepted or rejected (see `..._still_accepted_outside_clause`
// below, which pins the accept side is unaffected).
//
// Spec citations: §17.12 "The `throw` expression" (the grammar position
// this fix's parser change concerns — a `throw` as a binary right operand);
// §22.15 "Pure members" (the pure-body admission rule that reports this
// diagnostic, `reportPureViolation`); §22.16 "Contract clauses" (the
// clause-specific `throw`-rejection amendment whose diagnostic text names
// it, PureChecker.cpp ~1301-1312).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
constexpr const char* kThrowMsg = "`throw` is not admitted in a pure body";
}  // namespace

// Issue #3998's own measured repro, verbatim: `throw` reached via the `??`
// operand of an `or` clause inside `requires(...)`. Pre-fix: the driver
// prints "<path>:0:0: `throw` is not admitted...". Post-fix: a real
// `<path>:<line>:<col>: ` location (the `throw` token itself, confirmed by
// hand against this exact source to be line 4, column 29 — 4 leading
// indent spaces + "requires(x > 0 or (b ?? " is 28 characters).
BEZEL_TEST(contract_clause_throw_location_3998,
           throw_via_null_coalesce_in_clause_reports_real_location) {
    auto r = runAot(
        "module M;\n"
        "class C\n"
        "  public method M(x: int, b: bool?)\n"
        "    requires(x > 0 or (b ?? throw new ArgumentException()))\n"
        "    => 0;\n"
        "end C;\n");
    // The correct-verdict half: this program IS rejected (accept/reject was
    // already right pre-fix per #3998; this is a location-only bug).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kThrowMsg));
    // The specific defect #3998 reports: no diagnostic names line/col 0:0.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, ":0:0: "));
    // Pin the exact location the parser fix produces, not just "some real
    // location" — the throw token starts at line 4, column 29 in the
    // source above.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":4:29: "));
}

// A second grammar site from the same clause-mode arm (PureChecker.cpp
// ~1301-1312): a `throw` as the `then`/`else` value of a conditional atom
// inside `ensures(...)` (already covered for accept/reject by
// contract_clauses_typing_2053_test.cpp's
// `throw_expr_in_clause_rejected_spec_22_16`, but not for location — that
// row reaches `ThrowExpr` via `ConditionalExpr::thenValue`, a plain
// recursive-descent field assignment that already went through
// `parseExpression()`, so it was never broken by this parser bug; included
// here as a control showing this shape already reported a real location
// both before and after the fix).
BEZEL_TEST(contract_clause_throw_location_3998,
           throw_in_conditional_atom_in_clause_reports_real_location) {
    auto r = runAot(
        "module M;\n"
        "class C\n"
        "  public method M(x: int): int\n"
        "    ensures((if x < 0 then (throw new ArgumentException()) else x > 0))\n"
        "    => x;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kThrowMsg));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, ":0:0: "));
}

// Accept-side control (§15 discipline): the SAME `x ?? throw new E()`
// shape, OUTSIDE a contract clause (an ordinary local-variable
// initializer), is admitted per §17.12 — `throw` has the bottom type and is
// valid in any expression position, including a binary right operand — and
// must keep compiling and running correctly. This pins that the parser fix
// (switching to `parseExpression()`) changes ONLY the node's span, not
// what parses or what the throw carve-out accepts.
BEZEL_TEST(contract_clause_throw_location_3998,
           throw_via_null_coalesce_still_accepted_outside_clause) {
    auto r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var b: bool? := true;\n"
        "    var v: bool := b ?? throw new ArgumentException();\n"
        "    return (if v then 1 else 0);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
