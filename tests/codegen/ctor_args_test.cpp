// Codegen — constructor calls honor default parameter values (§22.7) and
// named arguments (§22.4), via the same parameter-ordering used for method
// calls. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_ctor_args, constructor_default_used) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public X: int := 0; "
        "  public constructor(x: int := 9) X := x; end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  var p := new P(); return p.X; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_ctor_args, constructor_default_overridden) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public X: int := 0; "
        "  public constructor(x: int := 9) X := x; end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  var p := new P(3); return p.X; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(codegen_ctor_args, constructor_named_args_reordered) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public X: int := 0; public Y: int := 0; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  var p := new P(y := 3, x := 10); return p.X * 10 + p.Y; "
        "end Main; end C;"), 103);
}

BEZEL_TEST(codegen_ctor_args, constructor_overload_by_arity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public X: int := 0; "
        "  public constructor() X := 100; end constructor; "
        "  public constructor(x: int) X := x; end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  var a := new P(); var b := new P(7); return a.X + b.X; "
        "end Main; end C;"), 107);
}
