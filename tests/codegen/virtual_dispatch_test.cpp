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

// §12.1 (v0.181, #1560/#1592): an override may widen a parameter `T` to
// `T?`. For a VALUE type (int, enum, record) this is an ABI change — `T?`
// lowers to a tagged `{i8 hasValue, T}` struct, a different runtime shape
// than the bare `T` the un-widened virtual declaration's ABI uses — so
// `declareVtables` (LlvmBackend_Setup.cpp) installs a per-slot adapter thunk
// wherever needed, and `emitMethodCall` (LlvmBackend_Calls2.cpp) builds every
// call to a widened slot against one uniform, family-wide ABI regardless of
// which ancestor's static type resolved the call (issue #1592).

BEZEL_TEST(codegen_cg17,
          override_valuetype_widen_dispatched_via_base_typed_reference) {
    // THE key regression test for #1592: Base declares `F(x: int)`; Derived
    // widens to `F(x: int?)`. Dispatching through a BASE-typed reference to a
    // Derived instance builds the call from Base's own (narrow, un-widened)
    // declared signature — before the #1592 fix this reached the
    // wide-ABI-compiled Derived::F with a raw (unboxed) argument and
    // corrupted the value (the issue's own pre-fix repro printed 1 instead
    // of 7). With the fix, both static types agree on one uniform ABI and
    // the value passes through uncorrupted.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method F(x: int): int => x; end Base; "
        "class Derived : Base "
        "  public override method F(x: int?): int => x ?? -1; "
        "end Derived; "
        "class C "
        "  public static method Dispatch(b: Base): int => b.F(7); "
        "  public static method Main(): int "
        "    var d := new Derived(); return Dispatch(d); "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg17,
          override_valuetype_widen_via_derived_typed_reference_null) {
    // The companion path: dispatching through the DERIVED-typed reference
    // itself, with a genuine `null` argument, must still reach Derived::F
    // with the real boxed (hasValue=false) value — the uniform-ABI dispatch
    // must not corrupt the already-wide call.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method F(x: int): int => x; end Base; "
        "class Derived : Base "
        "  public override method F(x: int?): int => x ?? -1; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.F(null); "
        "  end Main; "
        "end Derived;"), -1);
}

BEZEL_TEST(codegen_cg17, override_valuetype_widen_sibling_branch_unaffected) {
    // Sibling (a second, non-overriding subclass of Base sharing the SAME
    // vtable slot as Derived's widened override) must still dispatch to
    // Base's own un-widened body, uncorrupted, when reached via the same
    // Base-typed call site — the family-wide uniform-ABI thunking must not
    // leak the widening into a branch that never opted into it.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual method F(x: int): int => x * 2; end Base; "
        "class Derived : Base "
        "  public override method F(x: int?): int => x ?? -1; "
        "end Derived; "
        "class Sibling : Base end Sibling; "
        "class C "
        "  public static method Dispatch(b: Base): int => b.F(5); "
        "  public static method Main(): int "
        "    var s := new Sibling(); return Dispatch(s); "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg17,
          override_reftype_widen_dispatched_via_base_typed_reference) {
    // Reference-typed widening (#1560) dispatched via a BASE-typed reference
    // must keep working unchanged — reference types are ABI-identical
    // nullable-or-not (a plain pointer either way), so no thunk is needed
    // and this path predates #1592, but it had no dedicated codegen
    // regression test until now.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal public method Sound(): int => 5; end Animal; "
        "class Base public virtual method F(a: Animal): int => a.Sound(); "
        "end Base; "
        "class Derived : Base "
        "  public override method F(a: Animal?): int "
        "    if a = null then return -1; end; "
        "    return a.Sound() + 100; "
        "  end F; "
        "end Derived; "
        "class C "
        "  public static method Dispatch(b: Base, a: Animal): int => b.F(a); "
        "  public static method Main(): int "
        "    var d := new Derived(); var an := new Animal(); "
        "    return Dispatch(d, an); "
        "  end Main; "
        "end C;"), 105);
}
