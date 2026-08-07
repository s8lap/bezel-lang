// Sema — issue #2984, the ACCEPT side. §12.2's no-hiding rule is now enforced
// against a BUILT-IN exception base; this file pins the boundary of that new
// rejection, i.e. everything it must still let through, with values.
//
// The reject side (the diagnostic text, its locations, and the built-in it
// names) is pinned in tests/lit/builtin_exception_member_hiding_2984.bzl.
//
// Spec: §12.2 "Hiding is not allowed", verbatim —
//
//     Declaring a member in a derived class with the same name as **any**
//     inherited member **visible at the derived class's declaration site** is a
//     compile error, unless the declaration is an `override` of a virtual
//     member with a matching signature per §12.1 ... To add a new member,
//     choose a different name.
//
// — with the blocked-name set: "`public` and `protected` base members always
// block". §31.4 declares, verbatim:
//
//     public class Exception
//       public readonly property Message: string;
//       public readonly property StackTrace: string;
//       public readonly property CauseChain: Array<ExceptionInfo>;
//     end Exception;
//     public class ArgumentException: Exception
//       public readonly property ParamName: string?;
//     end ArgumentException;
//
// None is `virtual`, so no `override` escape exists. v0.240.
//
// WHY THE ACCEPT SIDE NEEDS ITS OWN FILE. #2984 ADDS a rejection, and an added
// rejection's cost is measured in what it wrongly forbids. The rule is keyed on
// the §31.4 DECLARATION WALK (`sema::builtinExceptionSurfaceMemberOf`), not on
// the four names, and these cells are what makes that observable:
//
//   * `ParamName` is declared on `ArgumentException`. A subclass of PLAIN
//     `Exception` does not inherit it, so declaring it there is LEGAL — and the
//     member must then be the class's own, readable with its own value. A fix
//     keyed on the name set alone would reject this.
//   * A class with no built-in exception ancestor may declare all four.
//   * Every other name on an exception subclass is unaffected.
//   * The §8.1 universal names (`ToString`, `Equals`, …) are NOT part of §31.4's
//     surface and are not blocked by this rule — `override ToString` on an
//     exception subclass is ordinary and must keep compiling.
//
// MEASURED FALSE-REJECT COST across the whole corpus, before this landed:
// exactly ONE existing test declared a §31.4 name on a class deriving from a
// built-in exception — `own_message_member_wins_on_exception_subclass_spec_11_3`
// in tests/codegen/this_qualified_exception_message_2972_test.cpp, whose premise
// this fix EXPIRES. It is rewritten in place to pin the reject, with the reason
// recorded there. Nothing else in tests/ or docs/ declares one.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

}  // namespace

// §31.4 declares `ParamName` on `ArgumentException`, NOT on `Exception`. A
// subclass of plain `Exception` therefore inherits no `ParamName`, so declaring
// one is a NEW member, not a hidden one — legal per §12.2 ("To add a new
// member, choose a different name" — this name IS different from every
// inherited one). The value read must be the class's own.
BEZEL_TEST(sema_builtin_exception_hiding_2984,
           paramname_is_legal_on_a_plain_exception_subclass_spec_12_2) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2984a; "
        "class ExOk2984: Exception "
        "  public constructor() base(\"m\"); end constructor; "
        "  public property ParamName: string => \"own-pn\"; "
        "  public property Other: string => \"own-other\"; "
        "end ExOk2984; "
        "class C public static method Main(): int "
        "  using var e := new ExOk2984(); "
        "  Console.WriteLine(\"pn=\" + e.ParamName); "
        "  Console.WriteLine(\"o=\" + e.Other); "
        "  Console.WriteLine(\"m=\" + e.Message); "
        "  return 0; "
        "end Main; end C;",
        "pn=own-pn\no=own-other\nm=m"));
}

// A class with NO built-in exception ancestor may use every one of the four
// names — the rule is about what a class INHERITS, and this one inherits none
// of them.
BEZEL_TEST(sema_builtin_exception_hiding_2984,
           all_four_names_are_legal_on_a_non_exception_class_spec_12_2) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2984b; "
        "class NotEx2984 "
        "  public property Message: string => \"a\"; "
        "  public property StackTrace: string => \"b\"; "
        "  public property CauseChain: string => \"c\"; "
        "  public property ParamName: string => \"d\"; "
        "end NotEx2984; "
        "class C public static method Main(): int "
        "  using var n := new NotEx2984(); "
        "  Console.WriteLine(n.Message + n.StackTrace + n.CauseChain "
        "                    + n.ParamName); "
        "  return 0; "
        "end Main; end C;",
        "abcd"));
}

// Every OTHER member name on an exception subclass is untouched, including
// names that merely resemble the surface. The rule must not spread by
// similarity.
BEZEL_TEST(sema_builtin_exception_hiding_2984,
           other_members_on_an_exception_subclass_are_unaffected_spec_12_2) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2984c; "
        "class ExMisc2984: Exception "
        "  public constructor() base(\"m\"); end constructor; "
        "  public property Messages: string => \"plural\"; "
        "  public property Trace: string => \"trace\"; "
        "  public property Cause: string => \"cause\"; "
        "  public method Describe(): string return Messages + Trace + Cause; "
        "  end Describe; "
        "end ExMisc2984; "
        "class C public static method Main(): int "
        "  using var e := new ExMisc2984(); "
        "  Console.WriteLine(e.Describe()); "
        "  Console.WriteLine(\"m=\" + e.Message); "
        "  return 0; "
        "end Main; end C;",
        "pluraltracecause\nm=m"));
}

// The §8.1 universal `Object` members are not part of §31.4's surface and are
// not collected by this rule. `override ToString` on an exception subclass is
// the ordinary way to give an exception a rendering, and a rule that blocked it
// would break a common shape for no §12.2 reason.
BEZEL_TEST(sema_builtin_exception_hiding_2984,
           override_tostring_on_an_exception_subclass_still_legal_spec_8_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2984d; "
        "class ExTs2984: Exception "
        "  public constructor() base(\"mm\"); end constructor; "
        "  public override method ToString(): string "
        "    return \"ExTs2984(\" + Message + \")\"; "
        "  end ToString; "
        "end ExTs2984; "
        "class C public static method Main(): int "
        "  using var e := new ExTs2984(); "
        "  Console.WriteLine(e.ToString()); "
        "  return 0; "
        "end Main; end C;",
        "ExTs2984(mm)"));
}

// A PRIVATE field whose name merely LOOKS like the surface, plus the surface
// read alongside it: the two coexist because the field's name differs. This is
// the shape a subclass has to use now to carry its own text, so it is worth
// pinning that it works and that both values are readable.
BEZEL_TEST(sema_builtin_exception_hiding_2984,
           the_recommended_rename_shape_compiles_and_reads_both_spec_12_2) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2984e; "
        "class ExRen2984: Exception "
        "  private _detail: string := \"detail\"; "
        "  public property Detail: string => _detail; "
        "  public constructor() base(\"header\"); end constructor; "
        "  public method Both(): string return Detail + \"/\" + this.Message; "
        "  end Both; "
        "end ExRen2984; "
        "class C public static method Main(): int "
        "  using var e := new ExRen2984(); "
        "  Console.WriteLine(e.Both()); "
        "  Console.WriteLine(e.Detail + \"/\" + e.Message); "
        "  return 0; "
        "end Main; end C;",
        "detail/header\ndetail/header"));
}
