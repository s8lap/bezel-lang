// Codegen — issue #5256 (residual of #5247): a virtual GENERIC method (`VB<T>`)
// called through its base type `GA` must reach the override a NON-generic
// subclass INHERITS from a generic subclass `class EGO<X>: GA`
// (`class Sub: EGO<int>`), per §12.1 (virtual dispatch, v0.313) and §14.1.
//
// MECHANISM. `Emitter::emitVirtualGenericDispatch` (LlvmBackend_Calls2.cpp)
// gave `Sub` no branch (the base-chain inversion fails at the unpinned `X` of
// `EGO<X>`), so a `Sub` receiver reached #5247's loud abort. Now a non-generic
// candidate is branched on its own descriptor, with the declarer's parameters
// derived by walking its chain upward (`computeAncestorGenericSubst`).
//
// The factory-only instantiation half of #5256 is NOT fixed here (see R6 of
// virtual_generic_generic_subclass_override_5247_test.cpp, still pinned).
//
// ORACLE: runAot stdout values.
//
// §12.1, v0.313.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kProg =
    "module T5256; "
    "public interface IV5256 method Area(): int; end IV5256; "
    "public class Sq5256: IV5256 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end Sq5256; "
    "public class Box5256<U> "
    "  private _u: U; "
    "  public constructor(u: U) _u := u; end constructor; "
    "  public method Get(): U return _u; end Get; "
    "end Box5256; "
    "public class GA5256 "
    "  public constructor() end constructor; "
    "  public virtual method VB<T>(b: Box5256<T>): int where T: IV5256 "
    "    return b.Get().Area() + 120; "
    "  end VB; "
    "  public static method Run(g: GA5256): int "
    "    using var s := new Sq5256(); "
    "    var bx := new Box5256<Sq5256>(s); "
    "    return g.VB(bx); "
    "  end Run; "
    "end GA5256; "
    "class Mine5256: GA5256 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: Box5256<T>): int where T: IV5256 "
    "    return b.Get().Area() + 30; "
    "  end VB; "
    "end Mine5256; "
    "class EGO5256<X>: GA5256 "
    "  private _x: X; "
    "  public constructor(x: X) _x := x; end constructor; "
    "  public override method VB<T>(b: Box5256<T>): int where T: IV5256 "
    "    return b.Get().Area() + 40; "
    "  end VB; "
    "end EGO5256; "
    "class Sub5256: EGO5256<int> "
    "  public constructor() base(1); end constructor; "
    "end Sub5256; "
    "class Sub2_5256: Sub5256 "
    "  public constructor() end constructor; "
    "end Sub2_5256; "
    "class SubOv5256: EGO5256<int> "
    "  public constructor() base(1); end constructor; "
    "  public override method VB<T>(b: Box5256<T>): int where T: IV5256 "
    "    return b.Get().Area() + 50; "
    "  end VB; "
    "end SubOv5256; ";

std::string out(const std::string& mainBody) {
    std::string src = kProg;
    src += "class Main5256 public static method Main(): int " + mainBody +
           " return 0; end Main; end Main5256;";
    auto r = runAot(src);
    return r.ran && r.exitCode == 0 ? r.stdoutText : "FAILED";
}

}  // namespace

// R1: the issue's repro P1 — a `Sub: EGO<int>` receiver and an `EGO<int>`.
BEZEL_TEST(virtual_generic_inherited_override_5256, r1_sub_of_concrete_generic_subclass_spec_12_1) {
    BEZEL_EXPECT_EQ(
        out("Console.WriteLine($\"a={GA5256.Run(new Sub5256())}\"); "
            "Console.WriteLine($\"b={GA5256.Run(new EGO5256<int>(2))}\");"),
        std::string("a=47\nb=47\n"));
}

// R2: two levels below the generic subclass (`Sub2: Sub: EGO<int>`).
BEZEL_TEST(virtual_generic_inherited_override_5256, r2_two_level_inherited_override_spec_12_1) {
    BEZEL_EXPECT_EQ(
        out("Console.WriteLine($\"v={GA5256.Run(new Sub2_5256())}\");"),
        std::string("v=47\n"));
}

// R3: a non-generic subclass of EGO<int> that re-overrides wins over EGO's.
BEZEL_TEST(virtual_generic_inherited_override_5256, r3_non_generic_subclass_re_override_spec_12_1) {
    BEZEL_EXPECT_EQ(
        out("Console.WriteLine($\"v={GA5256.Run(new SubOv5256())}\");"),
        std::string("v=57\n"));
}

// R4: every receiver kind in one program.
BEZEL_TEST(virtual_generic_inherited_override_5256, r4_all_receivers_spec_12_1) {
    BEZEL_EXPECT_EQ(
        out("Console.WriteLine($\"a={GA5256.Run(new Sub5256())}\"); "
            "Console.WriteLine($\"b={GA5256.Run(new Sub2_5256())}\"); "
            "Console.WriteLine($\"c={GA5256.Run(new SubOv5256())}\"); "
            "Console.WriteLine($\"d={GA5256.Run(new Mine5256())}\"); "
            "Console.WriteLine($\"e={GA5256.Run(new GA5256())}\"); "
            "Console.WriteLine($\"f={GA5256.Run(new EGO5256<int>(1))}\");"),
        std::string("a=47\nb=47\nc=57\nd=37\ne=127\nf=47\n"));
}

// R5: control — the receivers that never involved the inherited override.
BEZEL_TEST(virtual_generic_inherited_override_5256, r5_control_mine_and_base_spec_12_1) {
    BEZEL_EXPECT_EQ(
        out("Console.WriteLine($\"d={GA5256.Run(new Mine5256())}\"); "
            "Console.WriteLine($\"e={GA5256.Run(new GA5256())}\");"),
        std::string("d=37\ne=127\n"));
}
