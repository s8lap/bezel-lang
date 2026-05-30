// Codegen slice CG33 — auto-properties (§). An auto-property
// `Name: T { get; set; }` (no explicit accessor bodies, no arrow getter)
// is backed by a compiler-generated instance field: it gets a struct slot
// like a field, and `obj.Name` reads / `obj.Name := v` writes it (both
// via explicit `this`/`obj` and bare implicit-`this`). Observable via the
// int exit code.
//
// (Explicit-accessor and arrow-getter properties, and indexers, are
// deferred — only auto-properties lower as backing fields here.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg33, set_and_get_via_obj) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property Value: int { get; set; } "
        "  public static method Main(): int "
        "    var c := new C(); c.Value := 42; return c.Value; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, get_via_method_implicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property Value: int { get; set; } "
        "  public method Init() Value := 7; end Init; "
        "  public method Get(): int => Value; "
        "  public static method Main(): int "
        "    var c := new C(); c.Init(); return c.Get(); "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg33, two_properties_independent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property A: int { get; set; } "
        "  public property B: int { get; set; } "
        "  public static method Main(): int "
        "    var c := new C(); c.A := 30; c.B := 12; return c.A + c.B; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, property_alongside_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 10; "
        "  public property P: int { get; set; } "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 32; return c._x + c.P; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, property_compound_assignment) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property N: int { get; set; } "
        "  public static method Main(): int "
        "    var c := new C(); c.N := 40; c.N += 2; return c.N; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, inherited_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public property V: int { get; set; } end Base; "
        "class Derived : Base "
        "  public static method Main(): int "
        "    var d := new Derived(); d.V := 99; return d.V; "
        "  end Main; "
        "end Derived;"), 99);
}
