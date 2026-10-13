// #5817 (spec §14.2 *Interface conformance* rule 3, v0.338; §11.8 *Static
// interface members*, v0.338).
//
// Rule 3 keys on the BOUND interface I: an interface T conforms to I only
// through its extension chain, and an interface that declares or inherits
// static members (§11.8: static methods and operators; static constants and
// properties are deferred) is conformed to by no interface. So:
//   - a static-free interface argument that extends a static-bearing bound is
//     rejected (direct, two-hop, and a bound that inherits its statics);
//   - an interface argument extending a static-free bound is admitted even if
//     the argument itself inherits statics (the bound carries none);
//   - a class conformer with statics is governed by rule 1, not rule 3.
// The reflexive case (the argument is the static-bearing bound itself) is
// pinned to the literal reading of rule 3 (rejected); the spec's worked
// examples do not cover it.
//
// Rows:
//   R1  direct: Gen.Make<IThing>() with IThing: IZero<IThing>        -> reject
//   R2  two-hop: Gen.Make<IDeep>() with IDeep: IThing                -> reject
//   R3  bound inherits statics: Gen.Use<IArgOfChild>() with
//       IArgOfChild: IChildOfZero, IChildOfZero: IZero<IChildOfZero>  -> reject
//   R4  static operator (§11.8 operators): Gen.Use<IOpThing>() with
//       IOpThing: IOp<IOpThing>                                       -> reject
//   R5  reflexive: Gen.Use<IStat>() with the bound IStat itself       -> reject
//   A1  mixed: Gen.Use<IThing>() with IThing: IPlain, IZero<IThing>, bound
//       IPlain (static-free)                                          -> accept, exit 1
//   A2  class conformer with statics: Gen.Make<Thing>()              -> accept, exit 1

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kDecls =
    "module M; "
    "public interface IZero<T> "
    "  static method Zero(): owned T; "
    "end IZero; "
    "public interface IThing: IZero<IThing> "
    "end IThing; "
    "public interface IDeep: IThing "
    "end IDeep; "
    "public class Thing: IZero<Thing> "
    "  public constructor() end constructor; "
    "  public static method Zero(): owned Thing => new Thing(); "
    "end Thing; "
    "public class Gen "
    "  public static method Make<T>(): owned T where T: IZero<T> => T.Zero(); "
    "end Gen; "
    "public interface IPlain "
    "  method P(): int; "
    "end IPlain; "
    "public interface IMixed: IPlain, IZero<IMixed> "
    "end IMixed; "
    "public interface IChildOfZero: IZero<IChildOfZero> "
    "end IChildOfZero; "
    "public interface IArgOfChild: IChildOfZero "
    "end IArgOfChild; "
    "public interface IOp<T> "
    "  static operator +(a: T, b: T): owned T; "
    "end IOp; "
    "public interface IOpThing: IOp<IOpThing> "
    "end IOpThing; "
    "public interface IStat "
    "  static method Z(): int; "
    "end IStat; "
    "public class Gen2 "
    "  public static method Use<T>(): int where T: IPlain => 1; "
    "  public static method UseChild<T>(): int where T: IChildOfZero => 1; "
    "  public static method UseOp<T>(): int where T: IOp<T> => 1; "
    "  public static method UseStat<T>(): int where T: IStat => 1; "
    "end Gen2; ";

// Rejected at compile time, with the §14.2 constraint diagnostic naming the
// bound. Asserts the specific diagnostic, not merely that the compile failed.
bool rejectedNamingBound(const std::string& src, const std::string& bound) {
    auto r = runAot(src);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    const std::string needle =
        "does not satisfy the constraint `where T: " + bound + "` (§14.2)";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// R1 (§14.2 rule 3): the issue's direct case.
BEZEL_TEST(sema_static_interface_no_conformer_5817,
           interface_inheriting_static_member_rejected_spec_14_2_rule3) {
    BEZEL_EXPECT_TRUE(rejectedNamingBound(
        std::string(kDecls) +
        "public class User "
        "  public static method Run(): int "
        "    using var t := Gen.Make<IThing>(); "
        "    return 1; "
        "  end Run; "
        "end User; ",
        "IZero"));
}

// R2 (§14.2 rule 3): the chain IDeep -> IThing -> IZero reaches the statics.
BEZEL_TEST(sema_static_interface_no_conformer_5817,
           interface_inheriting_static_member_transitively_rejected_spec_14_2_rule3) {
    BEZEL_EXPECT_TRUE(rejectedNamingBound(
        std::string(kDecls) +
        "public class User "
        "  public static method Run(): int "
        "    using var t := Gen.Make<IDeep>(); "
        "    return 1; "
        "  end Run; "
        "end User; ",
        "IZero"));
}

// R3 (§14.2 rule 3, keyed on the bound): the bound IChildOfZero inherits its
// statics from IZero, so an interface argument extending it is rejected.
BEZEL_TEST(sema_static_interface_no_conformer_5817,
           bound_inheriting_static_member_rejected_spec_14_2_rule3) {
    BEZEL_EXPECT_TRUE(rejectedNamingBound(
        std::string(kDecls) +
        "public class User "
        "  public static method Run(): int "
        "    return Gen2.UseChild<IArgOfChild>(); "
        "  end Run; "
        "end User; ",
        "IChildOfZero"));
}

// R4 (§11.8 operators; §14.2 rule 3): a static operator is a static member,
// so an interface argument conforming to IOp<T> through its chain is rejected.
BEZEL_TEST(sema_static_interface_no_conformer_5817,
           static_operator_interface_rejected_spec_11_8_and_14_2_rule3) {
    BEZEL_EXPECT_TRUE(rejectedNamingBound(
        std::string(kDecls) +
        "public class User "
        "  public static method Run(): int "
        "    return Gen2.UseOp<IOpThing>(); "
        "  end Run; "
        "end User; ",
        "IOp"));
}

// R5 (reflexive, literal reading of §14.2 rule 3): the argument IS the
// static-bearing bound. Rejected under the literal rule; the spec's worked
// examples do not cover this case (possible spec-question).
BEZEL_TEST(sema_static_interface_no_conformer_5817,
           reflexive_static_bound_rejected_literal_spec_14_2_rule3) {
    BEZEL_EXPECT_TRUE(rejectedNamingBound(
        std::string(kDecls) +
        "public class User "
        "  public static method Run(): int "
        "    return Gen2.UseStat<IStat>(); "
        "  end Run; "
        "end User; ",
        "IStat"));
}

// A1 (polarity, §14.2 rule 3 keyed on the bound): an interface argument that
// inherits statics still conforms to a STATIC-FREE bound it extends. The
// program compiles and runs, exit code 1.
BEZEL_TEST(sema_static_interface_no_conformer_5817,
           static_bearing_argument_static_free_bound_accepted_spec_14_2_rule3) {
    auto r = runAot(std::string(kDecls) +
                    "public class C public static method Main(): int "
                    "  return Gen2.Use<IMixed>(); "
                    "end Main; "
                    "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

// A2 (polarity, §14.2 rule 1): a class conformer with statics still satisfies
// the static-bearing bound; the program compiles and runs, exit code 1.
BEZEL_TEST(sema_static_interface_no_conformer_5817,
           class_conformer_with_statics_accepted_spec_14_2_rule1) {
    auto r = runAot(std::string(kDecls) +
                    "public class C public static method Main(): int "
                    "  using var t := Gen.Make<Thing>(); "
                    "  return 1; "
                    "end Main; "
                    "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}
