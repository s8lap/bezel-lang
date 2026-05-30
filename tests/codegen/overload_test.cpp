// Codegen + sema for arity-based method overload resolution (§22.6). A call
// resolves to the same-named method whose parameter count matches the
// argument count. (Type-based tie-breaking between equal-arity overloads is
// a later refinement.) Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_overload, static_overload_by_arity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(x: int): int => x; "
        "  public static method F(x: int, y: int): int => x + y; "
        "  public static method Main(): int => F(2) + F(3, 4); "
        "end C;"), 9);
}

BEZEL_TEST(codegen_overload, three_overloads) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method G(): int => 1; "
        "  public static method G(a: int): int => a * 10; "
        "  public static method G(a: int, b: int): int => a + b; "
        "  public static method Main(): int => G() + G(5) + G(2, 3); "
        "end C;"), 56);
}

BEZEL_TEST(codegen_overload, instance_overload_by_arity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public method M(x: int): int => x; "
        "  public method M(x: int, y: int): int => x * y; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); return n.M(3) + n.M(4, 5); "
        "end Main; end C;"), 23);
}

BEZEL_TEST(codegen_overload, overloads_with_distinct_return_paths) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method H(a: int): int => a; "
        "  public static method H(a: int, b: int, c: int): int => a + b + c; "
        "  public static method Main(): int => H(7) + H(1, 2, 3); "
        "end C;"), 13);
}
