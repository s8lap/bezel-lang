// fix #800 (partial) / §30.6: the INFERRED function type of a BLOCK-bodied async
// lambda now carries the `Task<T>` wrapping the spec requires ("The function type
// of an async lambda has a Task, Task<T>, or OwnedTask<T> return type"). Before,
// `var f := async () => do return 1; end` inferred `() -> int`, so `await f()`
// was rejected ("await requires a Task operand"); only an explicit `f: () ->
// Task<T>` annotation supplied the wrapping.
//
// The expression-bodied inferred form (`var f := async () => 1`) remains a clean
// compile error here (a separate inferred-type codegen crash is tracked in #800's
// full fix); it is NOT exercised in this file. Async tests use runJit.

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
