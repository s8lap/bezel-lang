// fix #799 / §30.1: an expression-bodied async method (`async method M():
// Task<T> => expr`) must flow through the async state-machine lowering — the
// result is wrapped in a Task, exactly like the block-bodied `return expr;` form.
// Previously the async detection in emitFunctionBody required a Block body, so an
// expression body fell through to the regular method path and emitted a raw
// `ret <value>` (the unwrapped result) instead of a Task<T>, failing IR
// verification ("return type does not match operand type of return inst").

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.1: the issue's repro — an expression-bodied async method returning a
// constant, awaited from Main. Previously an IR_FAIL; now returns 1.
BEZEL_TEST(expr_bodied_async_799, constant_value) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method One(): Task<int> => 1; "
        "  public static async method Main(): Task<int> "
        "    return await One(); "
        "  end Main; end C;"), 1);
}

// §30.1: the value-producing expression is wrapped correctly and observed across
// two awaits of the same expression-bodied async method.
BEZEL_TEST(expr_bodied_async_799, two_awaits) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method One(): Task<int> => 1; "
        "  public static async method Main(): Task<int> "
        "    var a := await One(); var b := await One(); "
        "    return a + b + 40; "    // 42
        "  end Main; end C;"), 42);
}

// §30.1: an expression body that itself CONTAINS an await flows through the state
// machine (the synthesized `return await Two()` suspends correctly). Chained: 5.
BEZEL_TEST(expr_bodied_async_799, expression_body_contains_await) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method One(): Task<int> => 5; "
        "  public static async method Two(): Task<int> => await One(); "
        "  public static async method Main(): Task<int> => await Two(); "
        "end C;"), 5);
}

// §30.1: an expression body with arithmetic over awaited values.
BEZEL_TEST(expr_bodied_async_799, expression_body_arithmetic) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method N(v: int): Task<int> => v; "
        "  public static async method Sum(): Task<int> => await N(20) + await N(22); "
        "  public static async method Main(): Task<int> => await Sum(); "
        "end C;"), 42);
}

// §30.1 regression: the block-bodied form still compiles and runs.
BEZEL_TEST(expr_bodied_async_799, block_bodied_regression) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method One(): Task<int> return 1; end One; "
        "  public static async method Main(): Task<int> "
        "    return await One() + 41; "  // 42
        "  end Main; end C;"), 42);
}
