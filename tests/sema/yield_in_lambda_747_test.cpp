// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT → runtime CRASH) —
// `yield` inside a lambda.
//
// Authority: §29.9 — "`yield` is a method-level construct only." A `yield
// return` or `yield break` inside a lambda body is forbidden.
//
// Bug: the compiler silently accepted `yield` inside a block-body lambda (no
// diagnostic, exit 0 at compile time); codegen then built a malformed iterator
// state machine for the lambda and the program SIGSEGV'd at runtime. The fix
// rejects any `yield` reached while visiting a lambda body (§29.9). A `yield` in
// a genuine iterator method is unaffected.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasYieldInLambdaError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("not allowed in a lambda") != std::string::npos &&
            d.message.find("29.9") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §29.9: `yield return` inside a block-body lambda is rejected.
BEZEL_TEST(yield_in_lambda_747, yield_return_in_lambda_rejected) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Main(): int "
        "    var g: () -> IEnumerable<int> := () => do yield return 1; end; "
        "    return 0; "
        "  end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasYieldInLambdaError(r));
}

// §29.9: `yield break` inside a lambda is equally rejected.
BEZEL_TEST(yield_in_lambda_747, yield_break_in_lambda_rejected) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Main(): int "
        "    var g: () -> IEnumerable<int> := () => do yield break; end; "
        "    return 0; "
        "  end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasYieldInLambdaError(r));
}

// §29.9: `yield` in a genuine ITERATOR METHOD is fine — no §29.9 error.
BEZEL_TEST(yield_in_lambda_747, yield_in_iterator_method_ok) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Nums(): IEnumerable<int> "
        "    yield return 1; "
        "    yield break; "
        "  end Nums; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasYieldInLambdaError(r));
}

// §29.9: an iterator method that also contains a yield-FREE lambda is fine.
BEZEL_TEST(yield_in_lambda_747, iterator_with_plain_lambda_ok) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Nums(): IEnumerable<int> "
        "    var f: (int) -> int := (x) => x + 1; "
        "    yield return f(1); "
        "    yield break; "
        "  end Nums; "
        "  public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasYieldInLambdaError(r));
}
