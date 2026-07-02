// fix #800 / §30.6: the INFERRED function type of an async lambda now carries the
// `Task<T>` (or `OwningTask<T>`) wrapping the spec requires ("The function type of
// an async lambda has a Task, Task<T>, or OwningTask<T> return type"). Before,
// `var f := async () => do return 1; end` (and the expression form `async () =>
// 1`) inferred `() -> int`, so `await f()` was rejected ("await requires a Task
// operand"); only an explicit `f: () -> Task<T>` annotation supplied the wrapping.
//
// Layer 1 wrapped block-bodied async lambdas; Layer 2 extends it to EXPRESSION
// bodies, deriving the closure's async result type from the wrapped funcReturn so
// the body's value is wrapped into the outer Task instead of returned raw (an
// earlier naive attempt left the inferred-expr form a runtime SIGSEGV). Async
// tests use runJit.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.6: an inferred (un-annotated) BLOCK-bodied async lambda is awaitable — its
// type is `() -> Task<int>`, so `await f()` yields the int. 21*2 = 42.
BEZEL_TEST(async_lambda_infer_block_800, inferred_block_awaitable) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var f := async () => do return 21; end; "
        "    var v := await f(); "
        "    return v * 2; "    // 42
        "  end Main; end C;"), 42);
}

// §30.6: an inferred block-bodied async lambda whose body itself awaits.
BEZEL_TEST(async_lambda_infer_block_800, inferred_block_with_inner_await) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Producer(): Task<int> return 20; end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f := async () => do return await Producer() + 1; end; "
        "    return await f() + 21; "    // 21 + 21 = 42
        "  end Main; end C;"), 42);
}

// §30.6 regression: an explicitly-typed async lambda (the form the spec examples
// use) still compiles and runs, with both block and expression bodies.
BEZEL_TEST(async_lambda_infer_block_800, explicit_type_block_and_expr) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var g: () -> Task<int> := async () => do return 2; end; "
        "    var h: () -> Task<int> := async () => 40; "
        "    return await g() + await h(); "  // 2 + 40 = 42
        "  end Main; end C;"), 42);
}

// #800 Layer-2 / §30.6: an inferred (un-annotated) EXPRESSION-bodied async lambda
// is awaitable. This was a runtime SIGSEGV under the naive Layer-1 wrap; now its
// value is wrapped into the outer Task. 21*2 = 42.
BEZEL_TEST(async_lambda_infer_block_800, inferred_expr_awaitable) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var f := async () => 21; "
        "    var v := await f(); "
        "    return v * 2; "    // 42
        "  end Main; end C;"), 42);
}

// #800 Layer-2: an inferred expression-bodied async lambda whose body AWAITS flows
// through the state machine.
BEZEL_TEST(async_lambda_infer_block_800, inferred_expr_with_await) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Producer(): Task<int> => 20; "
        "  public static async method Main(): Task<int> "
        "    var f := async () => await Producer() + 1; "
        "    return await f() + 21; "    // 21 + 21 = 42
        "  end Main; end C;"), 42);
}

// #800 Layer-2 / §30.5: an inferred expression-bodied async lambda returning a
// CLASS uses the OwningTask<T> form — the awaited owned value transfers to the
// awaiter.
BEZEL_TEST(async_lambda_infer_block_800, inferred_expr_class_result) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Res public _v: int; public constructor(v: int) _v := v; end constructor; end Res; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async () => new Res(42); "
        "    using var r := await f(); "
        "    return r._v; "    // 42
        "  end Main; end C;"), 42);
}
