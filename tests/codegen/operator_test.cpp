// Codegen slice CG48 — user-defined operator overloading (§25). A binary
// expression on a class/record type that declares a matching `static method
// operator <op>` resolves to that operator method and calls it. Operators
// are emitted via the shared callable-shape path (they carry an
// `OperatorDecl` rather than a `MethodDecl`). Observable via exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg48, operator_plus_value_result) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator +(a: V, b: V): int => a.X + b.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 3; var b := new V(); b.X := 4; "
        "  return a + b; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg48, operator_times) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator *(a: V, b: V): int => a.X * b.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 6; var b := new V(); b.X := 7; "
        "  return a * b; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg48, operator_comparison) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator <(a: V, b: V): bool => a.X < b.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 3; var b := new V(); b.X := 5; "
        "  return (1 if a < b else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg48, operator_block_body_returns_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator +(a: V, b: V): V "
        "    var r := new V(); r.X := a.X + b.X; return r; "
        "  end; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 30; var b := new V(); b.X := 12; "
        "  var c := a + b; return c.X; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg48, primitive_operators_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return 3 + 4 * 2 - 1; "
        "end Main; end C;"), 10);
}

// §25.1: unary operator overloads (`-`, `~`) on a user type.
BEZEL_TEST(codegen_cg48, unary_minus_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator -(a: V): int => 0 - a.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var v := new V(); v.X := 5; return 0 - (-v); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg48, unary_complement_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator ~(a: V): int => ~a.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var v := new V(); v.X := 0; return ~v; "
        "end Main; end C;"), -1);
}

BEZEL_TEST(codegen_cg48, primitive_unary_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 5; return -x; "
        "end Main; end C;"), -5);
}
