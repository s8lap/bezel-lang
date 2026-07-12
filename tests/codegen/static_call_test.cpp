// Codegen slice CG6 — static-method definition and calls (§22). All
// static methods in the program are emitted as LLVM functions; a call
// whose callee resolves to one becomes a direct call with lowered
// positional arguments. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& members) {
    auto r = runAot(std::string("module M; class C ") + members + " end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg6, call_static_with_args) {
    BEZEL_EXPECT_EQ(run(
        "public static method Add(a: int, b: int): int => a + b; "
        "public static method Main(): int => Add(40, 2);"), 42);
}

BEZEL_TEST(codegen_cg6, no_arg_call) {
    BEZEL_EXPECT_EQ(run(
        "public static method Answer(): int => 42; "
        "public static method Main(): int => Answer();"), 42);
}

BEZEL_TEST(codegen_cg6, call_uses_parameters) {
    BEZEL_EXPECT_EQ(run(
        "public static method Square(n: int): int => n * n; "
        "public static method Main(): int => Square(9);"), 81);
}

BEZEL_TEST(codegen_cg6, nested_calls) {
    BEZEL_EXPECT_EQ(run(
        "public static method Inc(n: int): int => n + 1; "
        "public static method Main(): int => Inc(Inc(Inc(0)));"), 3);
}

BEZEL_TEST(codegen_cg6, callee_defined_after_caller) {
    // Two-pass declaration lets Main call Later, declared below it.
    BEZEL_EXPECT_EQ(run(
        "public static method Main(): int => Later(5); "
        "public static method Later(n: int): int => n * 2;"), 10);
}

BEZEL_TEST(codegen_cg6, parameter_in_block_body) {
    BEZEL_EXPECT_EQ(run(
        "public static method Clamp(n: int): int "
        "  if n > 100 then return 100; end; "
        "  return n; "
        "end Clamp; "
        "public static method Main(): int => Clamp(250);"), 100);
}

BEZEL_TEST(codegen_cg6, parameter_reassigned) {
    BEZEL_EXPECT_EQ(run(
        "public static method F(n: int): int "
        "  n := n + 1; return n * 10; "
        "end F; "
        "public static method Main(): int => F(3);"), 40);
}

BEZEL_TEST(codegen_cg6, recursive_factorial) {
    BEZEL_EXPECT_EQ(run(
        "public static method Fact(n: int): int "
        "  if n <= 1 then return 1; end; "
        "  return n * Fact(n - 1); "
        "end Fact; "
        "public static method Main(): int => Fact(5);"), 120);
}
