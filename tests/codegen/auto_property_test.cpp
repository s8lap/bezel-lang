// Codegen slice CG33 — auto-properties as backing fields (§A.4). An
// auto-property (`Name: T;` — no expression/accessor body) is backed by a
// compiler-generated instance field: it gets a struct slot like a field,
// and `obj.Name` reads / `obj.Name := v` writes it. Observable via the int
// exit code.
//
// (Expression-body and accessor-block properties, and indexers, are
// deferred; only auto-properties lower as backing fields here.)

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
        "  public property Value: int; "
        "  public static method Main(): int "
        "    var c := new C(); c.Value := 42; return c.Value; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, get_via_method_implicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property Value: int; "
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
        "  public property A: int; "
        "  public property B: int; "
        "  public static method Main(): int "
        "    var c := new C(); c.A := 30; c.B := 12; return c.A + c.B; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, property_alongside_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 10; "
        "  public property P: int; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 32; return c._x + c.P; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, property_compound_assignment) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property N: int; "
        "  public static method Main(): int "
        "    var c := new C(); c.N := 40; c.N += 2; return c.N; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33, inherited_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public property V: int; end Base; "
        "class Derived : Base "
        "  public static method Main(): int "
        "    var d := new Derived(); d.V := 99; return d.V; "
        "  end Main; "
        "end Derived;"), 99);
}

BEZEL_TEST(codegen_cg33, auto_property_with_initializer) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property Count: int := 5; "
        "  public static method Main(): int "
        "    var c := new C(); return c.Count; "
        "  end Main; "
        "end C;"), 5);
}

// CG33b: a computed (expression-body) property evaluates its getter with
// `this` = the receiver.
BEZEL_TEST(codegen_cg33b, expression_body_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 30; public _y: int := 12; "
        "  public property Sum: int => _x + _y; "
        "  public static method Main(): int "
        "    var c := new C(); return c.Sum; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg33b, expression_body_property_implicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _n: int := 7; "
        "  public property Doubled: int => _n * 2; "
        "  public method ViaThis(): int => Doubled; "
        "  public static method Main(): int "
        "    var c := new C(); return c.ViaThis(); "
        "  end Main; "
        "end C;"), 14);
}
