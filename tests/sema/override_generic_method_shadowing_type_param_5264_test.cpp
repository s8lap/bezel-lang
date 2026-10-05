// Spec-derived regression test for issue #5264 (spec §14.1, v0.314).
//
// THE DEFECT. When an override method's type parameter shadows the enclosing
// class's type parameter (`class SH<X>: GA { public override method VB<X>() }`),
// sema accepted the program (the override's VB<X> was treated as unrelated to
// GA.VB<T>), and codegen then aborted with "cannot specialize" at runtime.
//
// THE FIX. #5393 added checks in resolveMember's method arm:
// - duplicate within the method's own type-parameter list
// - shadowing the enclosing type's generic params (currentOuterType_)
// - shadowing enclosing scope params
// The override case is caught by the currentOuterType_ check: when resolving
// SH's members, currentOuterType_ == SH, so VB<X> conflicts with SH's X.
//
// THE SHAPE. A generic virtual method on a base class, overridden by a subclass
// whose type parameter has the SAME name as the subclass's own class parameter.
// The override body reads the class's field (proving the shadowing is live).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// -----------------------------------------------------------------------------
// The #5264 repro: override method VB<X> shadows class SH<X>, and VB<X>'s body
// reads the class field Item (typed X) — the shadowing is live in the body.
// -----------------------------------------------------------------------------
BEZEL_TEST(override_generic_method_shadowing_5264,
           override_shadows_class_tparam_rejected_spec_14_1) {
    auto r = runAot(
        "module P; "
        "public class Box<U> "
        "  private _u: U; "
        "  public constructor(u: U) _u := u; end constructor; "
        "  public method Get(): U return _u; end Get; "
        "end Box; "
        "public class GA "
        "  public constructor() end constructor; "
        "  public virtual method VB<T>(b: Box<T>): int return 1; end VB; "
        "  public static method RunS(g: GA): int "
        "    var bx := new Box<string>(\"tee\"); "
        "    return g.VB(bx); "
        "  end RunS; "
        "end GA; "
        "class SH<X>: GA "
        "  public Item: X; "
        "  public constructor(x: X) Item := x; end constructor; "
        "  public override method VB<X>(b: Box<X>): int "
        "    Console.WriteLine($\"SH item={Item} t={b.Get()}\"); "
        "    return 41; "
        "  end VB; "
        "end SH; "
        "class Main "
        "  public static method Main(): int "
        "    GA.RunS(new SH<string>(\"sh\")); "
        "    return 0; "
        "  end Main; "
        "end Main;");
    // After fix #5393, rejected at the type-parameter list (not an abort).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type parameter `X` reuses the name of a "
                                   "type parameter of enclosing type `SH`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§14.1"));
}

// -----------------------------------------------------------------------------
// Control: the same shape but with DIFFERENT names (class SH<X>, override
// VB<T>) — must still compile cleanly. This is the #5256 control shape.
// -----------------------------------------------------------------------------
BEZEL_TEST(override_generic_method_shadowing_5264,
           control_different_names_compile_cleanly) {
    auto r = runAot(
        "module P; "
        "public class Box<U> "
        "  private _u: U; "
        "  public constructor(u: U) _u := u; end constructor; "
        "  public method Get(): U return _u; end Get; "
        "end Box; "
        "public class GA "
        "  public constructor() end constructor; "
        "  public virtual method VB<T>(b: Box<T>): int return 1; end VB; "
        "  public static method RunS(g: GA): int "
        "    var bx := new Box<string>(\"tee\"); "
        "    return g.VB(bx); "
        "  end RunS; "
        "end GA; "
        "class SH<X>: GA "
        "  public Item: X; "
        "  public constructor(x: X) Item := x; end constructor; "
        "  public override method VB<T>(b: Box<T>): int "
        "    Console.WriteLine($\"SH item={Item} t={b.Get()}\"); "
        "    return 41; "
        "  end VB; "
        "end SH; "
        "class Main "
        "  public static method Main(): int "
        "    GA.RunS(new SH<string>(\"sh\")); "
        "    return 0; "
        "  end Main; "
        "end Main;");
    // No sema errors.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Runs and produces correct output.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
