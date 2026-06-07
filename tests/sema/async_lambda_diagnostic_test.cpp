// Spec-derived tests for async-lambda compile error — §30.6.
//
// §30.6 specifies `async` lambdas as a language feature; their state-machine
// codegen is NOT yet implemented. The compiler must reject any `async` lambda
// with a clean compile error citing §30.6 rather than crashing (SIGILL/SIGABRT
// from malformed IR due to cross-function basic-block references).
//
// Derivation:
//   - §30.6 ("Async lambdas"): `async () => …` is a valid syntax form but
//     requires full async state-machine lowering that is not yet implemented.
//   - Decline-with-reasons: the bounded fix is a §30.6 compile error; a full
//     implementation is a separate async-codegen milestone.
//   - §30.2: `await` inside an async lambda is valid *if* the lambda itself is
//     async — this boundary rule is tested via sema, but the async lambda as a
//     whole is still rejected as unimplemented (the boundary rule is logically
//     correct; only codegen is missing).

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

// Returns true if any diagnostic mentions "async lambda" and "§30.6".
bool hasAsyncLambdaDiagnostic(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool mentionsAsync = d.message.find("async") != std::string::npos &&
                             d.message.find("lambda") != std::string::npos;
        bool mentionsSec = d.message.find("30.6") != std::string::npos;
        if (mentionsAsync && mentionsSec) return true;
    }
    return false;
}

} // namespace

// §30.6 — an `async` lambda that awaits a Task<int> must produce a clean
// compile error, not a crash (the bug repro: "Referring to a basic block in
// another function" IR verifier failure leading to SIGILL exit 132).
BEZEL_TEST(sema_async_lambda_diagnostic_spec_30_6,
           async_lambda_with_await_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<int> := async () => await Producer(); "
        "    return await f(); "
        "  end Main; "
        "end C;");
    // Must produce a compile error (not crash).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The error must mention async lambda and §30.6.
    BEZEL_EXPECT_TRUE(hasAsyncLambdaDiagnostic(r));
}

// §30.6 — an `async` lambda without `await` also triggers the diagnostic
// (the issue is the async modifier on the lambda, not the presence of await).
BEZEL_TEST(sema_async_lambda_diagnostic_spec_30_6,
           async_lambda_without_await_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := async () => 42; "
        "    _ := f; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAsyncLambdaDiagnostic(r));
}

// CONTROL §A.7 / §21.7: a NON-async lambda must be completely unaffected —
// no spurious error from the async-lambda guard.
BEZEL_TEST(sema_async_lambda_diagnostic_spec_30_6,
           non_async_lambda_ok) {
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

// CONTROL §30.1 / §30.3: a normal (non-lambda) async METHOD must still
// compile and typecheck without errors. Only async LAMBDAS are diagnosed.
BEZEL_TEST(sema_async_lambda_diagnostic_spec_30_6,
           async_method_unaffected) {
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
