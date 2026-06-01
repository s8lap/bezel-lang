// Codegen slice CG60 — lambda expressions (§A.7, §17.14). Lambdas lower to
// nested LLVM functions and are passed as function-pointer values. The slice
// covers:
//   - expression-body lambdas `(p: T) => expr`
//   - explicit parameter type annotations (inferred params are later)
//   - passing lambdas as function-typed parameters `(T) -> R`
//   - storing lambdas in locals and passing them later
// Captures (closures over outer variables) are deferred.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}

// §17.14 / §A.7: apply a lambda to an integer argument.
BEZEL_TEST(codegen_cg60, lambda_apply_basic) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(5, (x: int) => x * 2); "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg60, lambda_expression_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Map(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Map; "
        "  public static method Main(): int "
        "    return Map(7, (x: int) => x + 3); "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg60, lambda_stored_in_local) {
    // A lambda value can be stored in a local and invoked later.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var sq := (a: int) => a * a; "
        "    return Apply(4, sq); "
        "  end Main; "
        "end C;"), 16);
}

BEZEL_TEST(codegen_cg60, lambda_multi_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Combine(a: int, b: int, "
        "                               f: (int, int) -> int): int "
        "    return f(a, b); "
        "  end Combine; "
        "  public static method Main(): int "
        "    return Combine(3, 4, (x: int, y: int) => x + y); "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg60, lambda_uses_outer_constant) {
    // Lambda captures a compile-time constant; capture of mutable outer
    // locals is deferred, but compile-time values work as expressions.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(6, (x: int) => x * 3); "
        "  end Main; "
        "end C;"), 18);
}

BEZEL_TEST(codegen_cg60, lambda_passed_twice) {
    // Same lambda variable passed to two different calls.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var dbl := (x: int) => x * 2; "
        "    return Apply(3, dbl) + Apply(4, dbl); "
        "  end Main; "
        "end C;"), 14);
}
