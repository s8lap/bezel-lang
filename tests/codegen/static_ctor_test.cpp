// Codegen slice CG47 — static constructors (§22.7). A `static constructor`
// runs once, before the program's body, to initialize a type's static
// state (it takes no parameters and has no `this`). Observable via the int
// exit code. (Run order is declaration order, at the start of Main — the
// single program root.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg47, static_ctor_sets_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public static V: int := 0; "
        "  public static constructor() V := 99; end constructor; "
        "end N; "
        "class C public static method Main(): int return N.V; end Main; "
        "end C;"), 99);
}

BEZEL_TEST(codegen_cg47, static_ctor_computes_from_other_static) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public static A: int := 0; public static B: int := 0; "
        "  public static constructor() A := 10; B := A * 5; end constructor; "
        "end N; "
        "class C public static method Main(): int => N.A + N.B; "
        "end C;"), 60);
}

BEZEL_TEST(codegen_cg47, static_ctor_with_control_flow) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public static Sum: int := 0; "
        "  public static constructor() "
        "    var i := 1; while i <= 5 do Sum := Sum + i; i := i + 1; end; "
        "  end constructor; "
        "end N; "
        "class C public static method Main(): int return N.Sum; end Main; "
        "end C;"), 15);
}

BEZEL_TEST(codegen_cg47, no_static_ctor_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public static V: int := 7; end N; "
        "class C public static method Main(): int return N.V; end Main; "
        "end C;"), 7);
}
