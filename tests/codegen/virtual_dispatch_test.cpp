// Codegen slice CG17 — virtual / override dispatch via vtables (§12.1).
// A `virtual` method is dispatched through the receiver's vtable, so an
// `override` in a derived class is selected at runtime even when the
// static type is the base. `base.M()` bypasses dispatch (§12.3) and
// non-virtual methods bind statically. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg17, override_dispatched_on_derived_instance) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method V(): int => 1; end Base; "
        "class Derived : Base "
        "  public override method V(): int => 2; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.V(); "
        "  end Main; "
        "end Derived;"), 2);
}

BEZEL_TEST(codegen_cg17, virtual_dispatched_from_base_method) {
    // Base.Describe (non-virtual) calls the virtual V via implicit this;
    // on a Derived instance, that resolves to Derived's override.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method V(): int => 1; "
        "  public method Describe(): int => V(); "
        "end Base; "
        "class Derived : Base "
        "  public override method V(): int => 2; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.Describe(); "
        "  end Main; "
        "end Derived;"), 2);
}

BEZEL_TEST(codegen_cg17, polymorphism_through_base_typed_parameter) {
    // The key test: a Base-typed parameter holding a Derived dispatches
    // to the derived override.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method V(): int => 10; end Base; "
        "class Derived : Base public override method V(): int => 20; "
        "end Derived; "
        "class C "
        "  public static method Dispatch(b: Base): int => b.V(); "
        "  public static method Main(): int "
        "    var d := new Derived(); return Dispatch(d); "
        "  end Main; "
        "end C;"), 20);
}

BEZEL_TEST(codegen_cg17, non_overriding_derived_uses_base_impl) {
    // Derived does not override V → the base implementation runs.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method V(): int => 7; end Base; "
        "class Derived : Base "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.V(); "
        "  end Main; "
        "end Derived;"), 7);
}

BEZEL_TEST(codegen_cg17, base_call_bypasses_dispatch) {
    // `base.V()` calls the base implementation directly even though V is
    // overridden (§12.3) — base.V()=1, d.V()=2 → 12.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method V(): int => 1; end Base; "
        "class Derived : Base "
        "  public override method V(): int => 2; "
        "  public method CallBase(): int => base.V(); "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.CallBase() * 10 + d.V(); "
        "  end Main; "
        "end Derived;"), 12);
}

BEZEL_TEST(codegen_cg17, three_level_override) {
    // A : virtual; B : override; C : override. A C instance runs C's.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A public virtual method V(): int => 1; end A; "
        "class B : A public override method V(): int => 2; end B; "
        "class C3 : B "
        "  public override method V(): int => 3; "
        "  public static method Main(): int "
        "    var c := new C3(); return c.V(); "
        "  end Main; "
        "end C3;"), 3);
}

BEZEL_TEST(codegen_cg17, non_virtual_method_binds_statically) {
    // A non-virtual method still works (direct call), unaffected by the
    // vtable machinery.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public method Plain(): int => 5; "
        "  public static method Main(): int "
        "    var b := new Base(); return b.Plain(); "
        "  end Main; "
        "end Base;"), 5);
}
