// Codegen slice CG14 — owned-field auto-disposal and base-class Dispose
// chaining (§28.4–28.5). The compiler-generated Dispose, after running
// the destructor body (CG13), disposes the class's `owned` class-typed
// fields in reverse declaration order (null-checking each), then chains
// to the base class's Dispose. A destructor that bumps a static counter
// makes the disposal observable via the int exit code.
//
// Owned fields are initialized in a constructor (the §7.9 definite-
// assignment check recognizes the `this.f := …` form). A class-typed
// *field initializer* (`owned _f: T := new T();`) would also work at
// runtime, but its embedded `new T()` type is not resolved by the
// front-end yet (tracked separately), so the tests use constructors.
//
// (Interface-typed owned fields need vtable dispatch, owning-inline-array
// fields need per-slot disposal, inherited *instance* field layout, and
// Dispose idempotency are all deferred; tests avoid them.)

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

BEZEL_TEST(codegen_cg14, owned_field_disposed_with_owner) {
    // Disposing the Owner (a using var) disposes its owned Child, whose
    // destructor bumps the counter. Owner has no destructor of its own.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Owner "
        "  private owned _child: Child; "
        "  public constructor() this._child := new Child(); end constructor; "
        "end Owner; "
        "class Child "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var o := new Owner(); end; "
        "    return _n; "
        "  end Main; "
        "end Child;"), 1);
}

BEZEL_TEST(codegen_cg14, two_owned_fields_both_disposed) {
    // Owner owns two children; disposing it disposes both.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Owner "
        "  private owned _a: Child; "
        "  private owned _b: Child; "
        "  public constructor() "
        "    this._a := new Child(); this._b := new Child(); "
        "  end constructor; "
        "end Owner; "
        "class Child "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var o := new Owner(); end; "
        "    return _n; "
        "  end Main; "
        "end Child;"), 2);
}

BEZEL_TEST(codegen_cg14, base_dispose_chained) {
    // Disposing a Derived (no destructor of its own) chains to Base's
    // Dispose, running Base's destructor → counter 1.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var x := new Derived(); end; "
        "    return _n; "
        "  end Main; "
        "end Base; "
        "class Derived : Base "
        "end Derived;"), 1);
}

BEZEL_TEST(codegen_cg14, transitive_base_chain) {
    // C : B : A. Disposing a C chains C→B→A; only A has a destructor, so
    // transitive chaining is what makes the counter reach 1.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var c := new C(); end; "
        "    return _n; "
        "  end Main; "
        "end A; "
        "class B : A end B; "
        "class C : B end C;"), 1);
}

BEZEL_TEST(codegen_cg14, no_owned_fields_no_extra_disposal) {
    // A plain class with a destructor and no owned fields disposes once.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var c := new C(); end; "
        "    return _n; "
        "  end Main; "
        "end C;"), 1);
}
