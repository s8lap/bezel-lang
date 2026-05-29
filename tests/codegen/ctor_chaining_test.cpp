// Codegen slice CG16 — constructor chaining `base(...)` / `this(...)`
// (§12.8). A leading `base(args)` runs the base ctor before the body;
// `this(args)` delegates to a sibling ctor; with neither written, an
// implicit `base()` runs the base class's parameterless ctor. Field
// initializers (inherited + own) run in `new` before any ctor body
// (CG15), so ctor bodies observe initialized fields. Observable via the
// int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg16, explicit_base_call_runs_base_ctor) {
    // The base ctor sets the inherited field; the derived ctor (after
    // chaining) reads and adds to it.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public _x: int := 0; "
        "  public constructor(v: int) this._x := v; end constructor; "
        "end Base; "
        "class Derived : Base "
        "  public _y: int := 0; "
        "  public constructor() base(40); this._y := this._x + 2; "
        "  end constructor; "
        "  public method Sum(): int => _x + _y; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.Sum(); "
        "  end Main; "
        "end Derived;"), 82);  // _x=40, _y=42 → 82
}

BEZEL_TEST(codegen_cg16, implicit_base_runs_base_ctor) {
    // No explicit chain in Derived's ctor → implicit base() runs Base's
    // parameterless ctor, which sets the inherited field.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public _x: int := 0; "
        "  public constructor() this._x := 7; end constructor; "
        "end Base; "
        "class Derived : Base "
        "  public _y: int := 0; "
        "  public constructor() this._y := 5; end constructor; "
        "  public method Sum(): int => _x + _y; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.Sum(); "
        "  end Main; "
        "end Derived;"), 12);
}

BEZEL_TEST(codegen_cg16, implicit_base_when_derived_has_no_ctor) {
    // Derived declares no ctor; `new Derived()` still runs Base's ctor.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public _x: int := 0; "
        "  public constructor() this._x := 9; end constructor; "
        "end Base; "
        "class Derived : Base "
        "  public method GetX(): int => _x; "
        "  public static method Main(): int "
        "    var d := new Derived(); return d.GetX(); "
        "  end Main; "
        "end Derived;"), 9);
}

BEZEL_TEST(codegen_cg16, this_delegation) {
    // `this(seed)` delegates to the one-arg ctor.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public _v: int := 0; "
        "  public constructor(seed: int) this._v := seed * 10; end constructor; "
        "  public constructor() this(4); end constructor; "
        "  public method Get(): int => _v; "
        "  public static method Main(): int "
        "    var c := new C(); return c.Get(); "
        "  end Main; "
        "end C;"), 40);
}

BEZEL_TEST(codegen_cg16, base_with_args_passes_arguments) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public _sum: int := 0; "
        "  public constructor(a: int, b: int) this._sum := a + b; "
        "  end constructor; "
        "end Base; "
        "class Derived : Base "
        "  public constructor(n: int) base(n, n); end constructor; "
        "  public method Get(): int => _sum; "
        "  public static method Main(): int "
        "    var d := new Derived(21); return d.Get(); "
        "  end Main; "
        "end Derived;"), 42);
}

BEZEL_TEST(codegen_cg16, three_level_chain) {
    // C : B : A. C's ctor chains base() → B's ctor chains base() → A's
    // ctor. Each sets its own inherited field.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A "
        "  public _a: int := 0; "
        "  public constructor() this._a := 100; end constructor; "
        "end A; "
        "class B : A "
        "  public _b: int := 0; "
        "  public constructor() base(); this._b := 20; end constructor; "
        "end B; "
        "class C2 : B "
        "  public _c: int := 0; "
        "  public constructor() base(); this._c := 3; end constructor; "
        "  public method Sum(): int => _a + _b + _c; "
        "  public static method Main(): int "
        "    var c := new C2(); return c.Sum(); "
        "  end Main; "
        "end C2;"), 123);
}
