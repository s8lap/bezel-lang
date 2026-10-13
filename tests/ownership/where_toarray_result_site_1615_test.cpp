// Spec-derived tests for #1615 / §28.3 (ownership result-site checking):
// `MethodOwnership::checkResultSitesExpr`'s `MemberAccessExpr` branch
// recursed into `*ma->target` unconditionally. Every other node kind in this
// walk guards its optional child (e.g. `if (ue->operand) checkResultSitesExpr(
// *ue->operand, false);`), but the `MemberAccessExpr` case fell out of the
// `.Result`-detection `if` and called `checkResultSitesExpr(*ma->target,
// false)` with no null check at all.
//
// `ma->target` is non-null on any AST produced from the grammar's own
// well-formed postfix chain (Appendix A.5 `expr "." Identifier`), but
// `Driver::compileFile` runs `ownership::checkOwnership` UNCONDITIONALLY
// after parsing — even when the parser already queued error diagnostics —
// and only prints the accumulated diagnostics at the very end of the phase
// pipeline (§2 / §33). So a partially error-recovered AST can still reach
// this pass. An unparenthesized single-parameter lambda argument, e.g.
// `xs.Where(v => v > 2)`, is invalid per Appendix A.7's `LambdaExpr ::=
// "async"? "(" LambdaParams? ")" "=>" LambdaBody` — the parameter list's
// parens are mandatory even for a single parameter (every lambda example in
// the spec, and every `.Where(...)` call in the existing test suite, e.g.
// #1226, writes them). Parsing `v` as a bare-identifier argument, the parser
// records "expected ')' to close call argument list" and recovers with a
// `.ToArray` `MemberAccessExpr` whose `target` was never populated.
// `checkResultSitesExpr` then dereferenced that null `unique_ptr<Expr>`,
// asserting `get() != pointer()` and aborting the compiler before the
// already-queued parse diagnostic was ever printed.
//
// Verified below:
//   1. The malformed input (the issue's literal repro) no longer crashes —
//      it now surfaces the parser's own diagnostic like any other syntax
//      error, instead of aborting the process.
//   2. The CORRECT, spec-conformant form of the same idiom — a bound-then-
//      materialized LINQ query with a properly parenthesized lambda,
//      `xs.Where((v) => v > 2).ToArray()` — is unaffected: it still
//      typechecks, passes ownership checking with no diagnostics, and still
//      runs to the correct result.

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// Run via runAot; return the int return value, or -999 on compile failure.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---- #1615 crash fix --------------------------------------------------

// The issue's exact repro shape: an unparenthesized single-param lambda
// argument to `.Where(...)` is invalid per Appendix A.7 (parens around the
// lambda parameter list are mandatory), and before the fix the parser's
// error-recovery AST for the trailing `.ToArray()` left a null
// `MemberAccessExpr::target` that crashed `checkResultSitesExpr`. Must now
// merely report the parser's own diagnostic (a normal compile error) rather
// than aborting the test process.
BEZEL_TEST(where_toarray_result_site_1615,
           unparenthesized_lambda_arg_reports_diagnostic_not_crash) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var xs := new Array<int>(); "
        "    var ys := xs.Where(v => v > 2).ToArray(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // The mere fact that this returns — rather than aborting the test
    // process on the pre-fix `unique_ptr::operator*()` assertion — is the
    // primary regression signal. The malformed input is still expected to be
    // a genuine parse error (§A.7 requires parens around lambda params), not
    // a silently-accepted program.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "retired pre-v0.217 lambda spelling"));
}

// ---- Correct, spec-conformant form is unaffected -----------------------

// §28.3 / §29.11: a bound-then-materialized LINQ query with a properly
// PARENTHESIZED lambda — the form every LINQ example in the spec and the
// existing test suite (e.g. #1226) uses — must still typecheck and pass
// ownership checking with no diagnostics.
BEZEL_TEST(where_toarray_result_site_1615,
           parenthesized_lambda_bound_query_compiles_clean_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Run(xs: Array<int>) "
        "    var ys := xs.Where(lambda(v) => v > 2).ToArray(); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §29.11: the same bound-then-materialized query, run end-to-end,
// confirms the query still executes correctly (not merely typechecks).
// xs = {1,2,3,4}; Where(v > 2) -> {3,4}; sum = 7.
BEZEL_TEST(where_toarray_result_site_1615,
           parenthesized_lambda_bound_query_runs_correctly_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); "
        "  using var ys: Array<int> := xs.Where(lambda(v) => v > 2).ToArray(); "
        "  var s := 0; "
        "  for var y in ys do s := s + y; end; "
        "  return s; "
        "end Main; end C;"),
        7);
}
