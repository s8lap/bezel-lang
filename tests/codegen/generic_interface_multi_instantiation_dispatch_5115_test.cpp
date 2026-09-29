// Regression tests for #5115: interface dispatch on a class that lists ONE
// generic interface at TWO instantiations, each witnessed by a different
// same-name, same-arity overload.
//
// Spec §11.1 (v0.313): "A generic interface may be listed at two or more
// instantiations (`class D: IComparable<int>, IComparable<string>`) ... The
// class still declares one member set, and §11.2's matching rule is applied
// once per obligation, each under its own instantiation's substitution".
// So a call through `IEquatable<int>` must reach `Equals(other: int)` and a
// call through `IEquatable<Point>` must reach `Equals(other: Point)`.
//
// Before the fix codegen built ONE itab per (class, interface declaration)
// and wired the first same-arity overload into it for every instantiation:
// the `IEquatable<int>` call ran `Equals(other: Point)` on an int and the
// program died with SIGSEGV. The fix emits one itab per instantiation, keyed
// by the per-instantiation descriptor, and a dispatch through `I<args>`
// selects it (bz_itable_lookup_inst).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
const char* POINT =
    "module M;\n"
    "class Point: IEquatable<Point>, IEquatable<int>\n"
    "  public X: int;\n"
    "  public constructor(x: int)\n"
    "    X := x;\n"
    "  end constructor;\n"
    "  public method Equals(other: Point): bool\n"
    "    Console.WriteLine(\"P\");\n"
    "    return X = other.X;\n"
    "  end Equals;\n"
    "  public method Equals(other: int): bool\n"
    "    Console.WriteLine(\"I\");\n"
    "    return X = other;\n"
    "  end Equals;\n"
    "  public override method GetHashCode(): int => X;\n"
    "end Point;\n";
}  // namespace

// The issue's literal repro (constructor arity corrected to two ints as
// the call site passes). Point(1, 2).X = 1 ≠ 5, so Main returns 4; pre-fix
// it crashed in bz$M.Point$Equals.
BEZEL_TEST(generic_interface_multi_instantiation_dispatch_5115,
           issue_repro_spec_11_1_returns_4) {
    auto r = runAot(
        "module M;\n"
        "class Point: IEquatable<Point>, IEquatable<int>\n"
        "  public X: int;\n"
        "  public constructor(x: int, y: int)\n"
        "    X := x;\n"
        "  end constructor;\n"
        "  public method Equals(other: Point): bool => X = other.X;\n"
        "  public method Equals(other: int): bool => X = other;\n"
        "  public override method GetHashCode(): int => X;\n"
        "end Point;\n"
        "class App\n"
        "  public static method Chk(a: IEquatable<int>, b: int): bool => a.Equals(b);\n"
        "  public static method Main(): int\n"
        "    if Chk(new Point(1, 2), 5) then return 3; end if;\n"
        "    return 4;\n"
        "  end Main;\n"
        "end App;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// Dual-overload shape: each instantiation dispatches to its OWN overload
// (distinct stdout tags), true and false results both observed.
BEZEL_TEST(generic_interface_multi_instantiation_dispatch_5115,
           dual_overload_each_instantiation_reaches_its_witness_spec_11_1) {
    auto r = runAot(std::string(POINT) +
        "class App\n"
        "  public static method ChkI(a: IEquatable<int>, b: int): bool => a.Equals(b);\n"
        "  public static method ChkP(a: IEquatable<Point>, b: Point): bool => a.Equals(b);\n"
        "  public static method Main(): int\n"
        "    using var p := new Point(5);\n"
        "    using var q := new Point(5);\n"
        "    using var w := new Point(9);\n"
        "    Console.WriteLine(ChkI(p, 5).ToString());\n"
        "    Console.WriteLine(ChkP(p, q).ToString());\n"
        "    Console.WriteLine(ChkI(p, 6).ToString());\n"
        "    Console.WriteLine(ChkP(p, w).ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end App;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("I\nTrue\nP\nTrue\nI\nFalse\nP\nFalse\n"));
}

// The same dispatch reached from a monomorphized generic body: the receiver
// type `IEquatable<T>` is resolved through the active substitution.
BEZEL_TEST(generic_interface_multi_instantiation_dispatch_5115,
           dual_overload_via_generic_method_spec_11_1) {
    auto r = runAot(std::string(POINT) +
        "class App\n"
        "  public static method Chk<T>(a: IEquatable<T>, b: T): bool => a.Equals(b);\n"
        "  public static method Main(): int\n"
        "    using var p := new Point(5);\n"
        "    using var q := new Point(5);\n"
        "    Console.WriteLine(Chk<int>(p, 5).ToString());\n"
        "    Console.WriteLine(Chk<Point>(p, q).ToString());\n"
        "    Console.WriteLine(Chk<int>(p, 6).ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end App;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("I\nTrue\nP\nTrue\nI\nFalse\n"));
}

// A user-declared generic interface at two instantiations (resolved
// parameter types rather than the builtin's synthesized ones).
BEZEL_TEST(generic_interface_multi_instantiation_dispatch_5115,
           user_generic_interface_two_instantiations_spec_11_1) {
    auto r = runAot(
        "module M;\n"
        "interface ISink<T>\n"
        "  method Put(x: T): string;\n"
        "end ISink;\n"
        "class Both: ISink<int>, ISink<string>\n"
        "  public method Put(x: string): string => \"s:\" + x;\n"
        "  public method Put(x: int): string => \"i:\" + x.ToString();\n"
        "end Both;\n"
        "class App\n"
        "  public static method PI(s: ISink<int>): string => s.Put(7);\n"
        "  public static method PS(s: ISink<string>): string => s.Put(\"q\");\n"
        "  public static method Main(): int\n"
        "    using var b := new Both();\n"
        "    Console.WriteLine(PI(b));\n"
        "    Console.WriteLine(PS(b));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end App;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("i:7\ns:q\n"));
}

// Control: one instantiation, overloads that differ in ARITY (the #742
// arity pick). Unchanged by the fix; guards the shared-itab fallback.
BEZEL_TEST(generic_interface_multi_instantiation_dispatch_5115,
           control_single_instantiation_arity_overloads) {
    auto r = runAot(
        "module M;\n"
        "interface IRun<T>\n"
        "  method Run(x: T): string;\n"
        "end IRun;\n"
        "class R: IRun<int>\n"
        "  public method Run(): string => \"zero\";\n"
        "  public method Run(x: int): string => \"one:\" + x.ToString();\n"
        "end R;\n"
        "class App\n"
        "  public static method Go(r: IRun<int>): string => r.Run(4);\n"
        "  public static method Main(): int\n"
        "    using var r := new R();\n"
        "    Console.WriteLine(Go(r));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end App;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("one:4\n"));
}
