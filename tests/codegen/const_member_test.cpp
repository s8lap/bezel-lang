// Codegen slice CG36 — const members (§A.4). A `const X := …` member
// lowers to its initializer's compile-time constant value, referenced
// bare (`X`), qualified (`C.X`), or cross-class. Observable via the int
// exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg36, bare_const_reference) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const MAX: int := 100; "
        "  public static method Main(): int => MAX; "
        "end C;"), 100);
}

BEZEL_TEST(codegen_cg36, const_in_expression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const A: int := 40; "
        "  public const B: int := 2; "
        "  public static method Main(): int => A + B; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg36, qualified_const_reference) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class K public const V: int := 7; end K; "
        "class C public static method Main(): int "
        "  return K.V; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg36, const_bool_in_condition) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const ON: bool := true; "
        "  public static method Main(): int "
        "    if ON then return 5; end; return 0; "
        "  end Main; "
        "end C;"), 5);
}

BEZEL_TEST(codegen_cg36, const_used_as_loop_bound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const N: int := 5; "
        "  public static method Main(): int "
        "    var i := 0; var s := 0; "
        "    while i < N do s += 10; i += 1; end; return s; "
        "  end Main; "
        "end C;"), 50);
}
