// Codegen slice CG46 — named arguments (§22.4). An argument written
// `name := expr` is placed at the parameter named `name`, so arguments may
// be reordered or interleaved with positionals; omitted parameters still
// take their defaults. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg46, named_args_reordered) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Sub(a: int, b: int): int => a - b; "
        "  public static method Main(): int => Sub(b := 3, a := 10); "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg46, named_args_in_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Sub(a: int, b: int): int => a - b; "
        "  public static method Main(): int => Sub(a := 10, b := 3); "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg46, positional_then_named) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int, c: int): int "
        "    => a * 100 + b * 10 + c; "
        "  public static method Main(): int => F(1, c := 3, b := 2); "
        "end C;"), 123);
}

BEZEL_TEST(codegen_cg46, named_arg_with_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method G(a: int, b: int := 5, c: int := 9): int "
        "    => a * 100 + b * 10 + c; "
        "  public static method Main(): int => G(1, c := 7); "
        "end C;"), 157);
}
