// Codegen — issue #5247: a virtual GENERIC method (`VB<T>`) called through its
// base type must reach EVERY override, including one declared on a GENERIC
// subclass whose type parameters its base clause does not pin
// (`class EGO<X> : GA`), per §12.1 (virtual dispatch, v0.313) and §14.1.
//
// MECHANISM. `Emitter::emitVirtualGenericDispatch` (LlvmBackend_Calls2.cpp)
// builds one branch per overriding subclass, comparing the receiver's
// descriptor. For a generic candidate it inverts the base chain
// (`deriveDescendantGenericSubst`); `EGO<X> : GA` leaves `X` undetermined (a
// `GA` may hold an `EGO<int>` and an `EGO<string>` at once), so the whole
// dispatch was abandoned and the call fell to the vtable slot's erased
// template, whose return degrades to zero (`internal: ... #2491`). MEASURED on
// r23-c28 68fbe062: an unrelated `EGO` class turned `GA.Run(new Mine())` from
// 37 into 0, for EVERY receiver, with only an internal warning.
//
// THE FIX keeps the dispatch: each concrete instantiation of the candidate the
// program names gets its own descriptor-guarded branch compiled under that
// instantiation's arguments, and an instantiation the scan cannot see aborts
// loudly (never a silent zero).
//
// ORACLE. runAot fills diagnostics only on reject, so the degraded return is
// pinned through the stdout value (it prints 0). The program still carries the
// `#2491` warnings the erased generic-method TEMPLATES produce even with no
// generic subclass (MEASURED on the no-EGO control), so the warning's presence
// is not the discriminator; the value is.
//
// §12.1, v0.313.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kBase =
    "module T5247; "
    "public interface IV5247 method Area(): int; end IV5247; "
    "public class Sq5247: IV5247 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end Sq5247; "
    "public class Box5247<U> "
    "  private _u: U; "
    "  public constructor(u: U) _u := u; end constructor; "
    "  public method Get(): U return _u; end Get; "
    "end Box5247; "
    "public class GA5247 "
    "  public constructor() end constructor; "
    "  public virtual method VB<T>(b: Box5247<T>): int where T: IV5247 "
    "    return b.Get().Area() + 120; "
    "  end VB; "
    "  public static method Run(g: GA5247): int "
    "    using var s := new Sq5247(); "
    "    var bx := new Box5247<Sq5247>(s); "
    "    return g.VB(bx); "
    "  end Run; "
    "end GA5247; "
    "class Mine5247: GA5247 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: Box5247<T>): int where T: IV5247 "
    "    return b.Get().Area() + 30; "
    "  end VB; "
    "end Mine5247; "
    "class Plain5247: GA5247 "
    "  public constructor() end constructor; "
    "end Plain5247; ";

const char* kEgo =
    "class EGO5247<X>: GA5247 "
    "  private _x: X; "
    "  public constructor(x: X) _x := x; end constructor; "
    "  public override method VB<T>(b: Box5247<T>): int where T: IV5247 "
    "    return b.Get().Area() + 40; "
    "  end VB; "
    "end EGO5247; ";

std::string out(const std::string& mainBody, bool withEgo) {
    std::string src = kBase;
    if (withEgo) src += kEgo;
    src += "class Main5247 public static method Main(): int " + mainBody +
           " return 0; end Main; end Main5247;";
    auto r = runAot(src);
    return r.ran && r.exitCode == 0 ? r.stdoutText : "FAILED";
}

}  // namespace

// R1: the issue's repro — an unrelated generic subclass must not disturb the
// Mine receiver (37 = Area 7 + 30, not the degraded 0).
BEZEL_TEST(virtual_generic_generic_subclass_override_5247, r1_unrelated_generic_subclass_keeps_mine_spec_12_1) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine($\"v={GA5247.Run(new Mine5247())}\");", true),
                    std::string("v=37\n"));
}

// R2: the generic subclass's own receiver reaches ITS override (7 + 40 = 47).
BEZEL_TEST(virtual_generic_generic_subclass_override_5247, r2_generic_subclass_receiver_reaches_its_override_spec_12_1) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine($\"v={GA5247.Run(new EGO5247<int>(1))}\");", true),
                    std::string("v=47\n"));
}

// R3: two instantiations, plus the other receivers, in one program.
BEZEL_TEST(virtual_generic_generic_subclass_override_5247, r3_two_instantiations_and_others_spec_12_1) {
    BEZEL_EXPECT_EQ(
        out("var str: string := \"s\"; "
            "Console.WriteLine($\"a={GA5247.Run(new EGO5247<int>(1))}\"); "
            "Console.WriteLine($\"b={GA5247.Run(new EGO5247<string>(str))}\"); "
            "Console.WriteLine($\"c={GA5247.Run(new Mine5247())}\"); "
            "Console.WriteLine($\"d={GA5247.Run(new Plain5247())}\"); "
            "Console.WriteLine($\"e={GA5247.Run(new GA5247())}\");", true),
        std::string("a=47\nb=47\nc=37\nd=127\ne=127\n"));
}

// R4: control — without the generic subclass the same program prints 37.
BEZEL_TEST(virtual_generic_generic_subclass_override_5247, r4_control_without_generic_subclass_spec_12_1) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine($\"v={GA5247.Run(new Mine5247())}\");", false),
                    std::string("v=37\n"));
}

// R5: the default (base) receiver is unaffected by the generic subclass.
BEZEL_TEST(virtual_generic_generic_subclass_override_5247, r5_base_receiver_unaffected_spec_12_1) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine($\"v={GA5247.Run(new GA5247())}\");", true),
                    std::string("v=127\n"));
}

// R6: an instantiation the call site cannot see (`EGO<int>` is only ever built
// inside the generic factory `Make<Y>`, never named concretely) must fail LOUD
// (abort, no output), never print a silent zero.
BEZEL_TEST(virtual_generic_generic_subclass_override_5247, r6_unseen_instantiation_aborts_loudly_spec_12_1) {
    auto prog = [](const std::string& call) {
        return std::string(kBase) + kEgo +
            "class Fac5247 public static method Make<Y>(y: Y): owned GA5247 "
            "  return new EGO5247<Y>(y); end Make; end Fac5247; "
            "class Main5247 public static method Main(): int "
            "  using var g := Fac5247.Make<int>(5); " + call +
            " return 0; end Main; end Main5247;";
    };
    // Control: the same program, VB never called, compiles and runs (so the
    // abort below is not a compile failure in disguise).
    auto ok = runAot(prog("Console.WriteLine(\"ok\");"));
    BEZEL_EXPECT_EQ(ok.ran && ok.exitCode == 0, true);
    BEZEL_EXPECT_EQ(ok.stdoutText, std::string("ok\n"));
    auto r = runAot(prog("Console.WriteLine($\"v={GA5247.Run(g)}\");"));
    BEZEL_EXPECT_EQ(r.ran && r.exitCode == 0, false);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}
