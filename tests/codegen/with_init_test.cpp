// Codegen slice CG34 — object initializer `new C() with f := v, …`
// (§A.7). The source is constructed, then each named field/auto-property
// is assigned. The result has the source's type. Observable via the int
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

BEZEL_TEST(codegen_cg34, with_sets_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; public _y: int := 0; "
        "  public static method Main(): int "
        "    var c := new C() with _x := 40, _y := 2 end; "
        "    return c._x + c._y; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg34, with_result_keeps_type_for_method_call) {
    // The `with` result is typed as C, so a method call on it resolves.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; public _y: int := 0; "
        "  public method Sum(): int => _x + _y; "
        "  public static method Main(): int "
        "    var c := new C() with _x := 30, _y := 12 end; "
        "    return c.Sum(); "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg34, with_overrides_initializer) {
    // A field initialized to 5 is overridden to 7 by the `with`.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _v: int := 5; "
        "  public static method Main(): int "
        "    var c := new C() with _v := 7 end; return c._v; "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg34, with_sets_auto_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property P: int; "
        "  public static method Main(): int "
        "    var c := new C() with P := 42 end; return c.P; "
        "  end Main; "
        "end C;"), 42);
}
