// Spec-derived sema tests for async lambdas — §30.6 (issue #488).
//
// §30.6 specifies `async` lambdas as a language feature. As of #488 they are
// IMPLEMENTED (the closure entry drives an async state machine — see
// emitAsyncLambda); the prior "async lambdas are not yet supported (§30.6)"
// diagnostic is removed. These tests pin the SEMA contract: an async lambda
// type-checks WITHOUT error, `await` inside it is valid because the lambda is
// its own async boundary (§30.2), and the control cases (non-async lambda,
// named async method) are unaffected. The runtime/codegen behaviour is covered
// by tests/codegen/async_lambda_488_test.cpp.
//
// Derivation:
//   - §30.6 ("Async lambdas"): `async (url) => await CountBytesAsync(url)` and
//     the block-bodied form are valid syntax that must type-check.
//   - §30.2 / Appendix C.6: `await` is valid inside an `async` lambda (the
//     lambda is its own async boundary, independent of the enclosing method).

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

// True if any diagnostic still mentions the removed "async lambda … §30.6"
// not-supported error (it must not, post-#488).
bool hasAsyncLambdaNotSupportedDiagnostic(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool mentionsAsync = d.message.find("async") != std::string::npos &&
                             d.message.find("lambda") != std::string::npos;
        bool mentionsSec = d.message.find("30.6") != std::string::npos;
        bool notSupported = d.message.find("not yet supported") != std::string::npos;
        if (mentionsAsync && mentionsSec && notSupported) return true;
    }
    return false;
}

} // namespace

// §30.6 — an `async` lambda that awaits a Task<int> now type-checks with no
// error (the not-supported diagnostic is gone post-#488).
BEZEL_TEST(sema_async_lambda_30_6, async_lambda_with_await_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<int> := async () => await Producer(); "
        "    using var t := f(); "
        "    return await t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAsyncLambdaNotSupportedDiagnostic(r));
}

// §30.6 — a block-bodied async lambda type-checks; `await` inside its body is
// valid because the lambda is its own async boundary (§30.2).
BEZEL_TEST(sema_async_lambda_30_6, block_body_async_lambda_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 10; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f: (int) -> Task<int> := async (x) => do "
        "      var d := await Producer(x); "
        "      return d + 1; "
        "    end; "
        "    using var t := f(2); "
        "    return await t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAsyncLambdaNotSupportedDiagnostic(r));
}

// §30.2 — `await` in a NON-async lambda is still rejected (the lambda's own
// async-ness, not the enclosing method's, governs await validity). This pins
// that removing the not-supported diagnostic did not relax the await boundary.
BEZEL_TEST(sema_async_lambda_30_6, await_in_non_async_lambda_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f: () -> int := () => await Producer(); "
        "    _ := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // The `await` inside the non-async lambda must be diagnosed (§30.2).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// CONTROL §A.7 / §21.7: a NON-async lambda must be completely unaffected.
BEZEL_TEST(sema_async_lambda_30_6, non_async_lambda_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Apply(f: (int) -> int): int "
        "    return f(3); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply((x: int) => x + 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL §30.1 / §30.3: a normal (non-lambda) async METHOD still type-checks.
BEZEL_TEST(sema_async_lambda_30_6, async_method_unaffected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public async method Consumer(t: Task<int>): Task<int> "
        "    var n := await t; "
        "    return n + 1; "
        "  end Consumer; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
