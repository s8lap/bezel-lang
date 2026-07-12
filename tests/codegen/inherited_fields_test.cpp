// Codegen slice CG15 — inherited instance field layout (§11.1, §12).
// A derived class's instance lays out its base-class fields first
// (prefix layout), then its own fields. Inherited fields are addressable
// from derived-class methods, their initializers run on `new Derived()`,
// and base + derived fields coexist without aliasing. Observable via the
// int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg15, inherited_field_read_via_this) {
    // A derived method reads an inherited field (initialized in the base).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public _x: int := 7; end Base; "
        "class Derived : Base "
        "  public method GetX(): int => this._x; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.GetX(); "
        "  end Main; "
        "end Derived;"), 7);
}

BEZEL_TEST(codegen_cg15, inherited_field_implicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public _x: int := 11; end Base; "
        "class Derived : Base "
        "  public method GetX(): int => _x; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.GetX(); "
        "  end Main; "
        "end Derived;"), 11);
}

BEZEL_TEST(codegen_cg15, base_and_derived_fields_coexist) {
    // Base field _x and derived field _y are at distinct slots; both
    // initializers run on `new Derived()`.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public _x: int := 30; end Base; "
        "class Derived : Base "
        "  public _y: int := 12; "
        "  public method Sum(): int => _x + _y; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.Sum(); "
        "  end Main; "
        "end Derived;"), 42);
}

BEZEL_TEST(codegen_cg15, inherited_field_write) {
    // Writing an inherited field updates the right slot.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public _x: int := 0; end Base; "
        "class Derived : Base "
        "  public method SetX(v: int) _x := v; end SetX; "
        "  public method GetX(): int => _x; "
        "  public static method Main(): int "
        "    var d := new Derived(); d.SetX(99); return d.GetX(); "
        "  end Main; "
        "end Derived;"), 99);
}

BEZEL_TEST(codegen_cg15, field_read_through_base_typed_access) {
    // External read of an inherited public field via the derived instance.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public _x: int := 5; end Base; "
        "class Derived : Base public _y: int := 8; end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(); return d._x + d._y; "
        "end Main; end C;"), 13);
}

BEZEL_TEST(codegen_cg15, two_level_inheritance) {
    // C : B : A — A's and B's fields both inherited into C, base-first.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A public _a: int := 100; end A; "
        "class B : A public _b: int := 20; end B; "
        "class C2 : B "
        "  public _c: int := 3; "
        "  public method Sum(): int => _a + _b + _c; "
        "  public static method Main(): int "
        "    var c := new C2(); return c.Sum(); "
        "  end Main; "
        "end C2;"), 123);
}
