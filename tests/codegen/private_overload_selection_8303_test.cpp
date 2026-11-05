// Spec-derived regression test for issue #8303.
//
// SPEC §13.1 (v0.355), *Levels*, verbatim table rows:
//
//   | `public`    | Anyone who can see the enclosing context          |
//   | `internal`  | Code in the same module                           |
//   | `protected` | The declaring class and all classes derived from it |
//   | `private`   | The declaring class only                          |
//
// and §22.6 (v0.355), *Overloading*: overload resolution ranks the candidates
// the call can name; a member the call site cannot see (§13.1) is not one of
// them. A private overload therefore never takes part in a call made from
// another class, and the PUBLIC overload of the same name wins.
//
// THE DEFECT. The overload group of a user method call was built without the
// visibility filter, so `v.Pick(1)` from class `C` selected the inaccessible
// `private Pick(x: int)` (exact) over the visible `public Pick(x: long)`, and
// the 0-arity `v.Len()` compiled and ran the private body. Same group builder
// for the instance, static, `this.`, bare-identifier and method-reference
// forms. Controls: the declaring class still sees its own private overload;
// `protected` is seen from a derived class and not from an unrelated one.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectChecked(const std::string& src, const std::string& out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

BEZEL_TEST(codegen_private_overload_selection_8303, issue_repro_public_overload) {
    // per §13.1 / §22.6 (v0.355): the issue's literal class, public call form
    expectChecked(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  private method Len(): int => 42;\n"
        "  public method Len(x: int): int => x + 100;\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    Console.WriteLine(v.Len(1));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "101\n");
}

BEZEL_TEST(codegen_private_overload_selection_8303, instance_call_public_wins) {
    // the private overload is the EXACT match; the public one is selected
    expectChecked(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  private method Pick(x: int): string => \"private-int\";\n"
        "  public method Pick(x: long): string => \"public-long\";\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    Console.WriteLine(v.Pick(1));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "public-long\n");
}

BEZEL_TEST(codegen_private_overload_selection_8303, static_call_public_wins) {
    expectChecked(
        "module M;\n"
        "class V\n"
        "  private static method Pick(x: int): string => \"private-int\";\n"
        "  public static method Pick(x: long): string => \"public-long\";\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(V.Pick(1));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "public-long\n");
}

BEZEL_TEST(codegen_private_overload_selection_8303,
           derived_bare_and_this_calls_public_wins) {
    // a derived class does not see the base's private overload (§13.1)
    expectChecked(
        "module M;\n"
        "class B\n"
        "  public constructor() end constructor;\n"
        "  private method Pick(x: int): string => \"private-int\";\n"
        "  public method Pick(x: long): string => \"public-long\";\n"
        "end B;\n"
        "class D: B\n"
        "  public constructor() end constructor;\n"
        "  public method Go(): string => Pick(1) + \",\" + this.Pick(2);\n"
        "end D;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Go());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "public-long,public-long\n");
}

BEZEL_TEST(codegen_private_overload_selection_8303,
           method_reference_over_private_and_public_overload) {
    // per §27.6 + §13.1: the reference's overload group has the same filter;
    // the target `(int) -> string` fits the private `Pick(int)` exactly and
    // the public `Pick(long)` by conversion, and only the latter is visible
    expectChecked(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  private method Pick(x: int): string => \"private-int\";\n"
        "  public method Pick(x: long): string => \"public-long\";\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    var f: (int) -> string := v.Pick;\n"
        "    Console.WriteLine(f(1));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "public-long\n");
}

// ── controls ────────────────────────────────────────────────────────────────

BEZEL_TEST(codegen_private_overload_selection_8303,
           control_declaring_class_still_selects_its_private_overload) {
    // §13.1: private is visible to the declaring class
    expectChecked(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  private method Pick(x: int): string => \"private-int\";\n"
        "  public method Pick(x: long): string => \"public-long\";\n"
        "  public method Go(): string => Pick(1) + \",\" + this.Pick(2);\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    Console.WriteLine(v.Go());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "private-int,private-int\n");
}

BEZEL_TEST(codegen_private_overload_selection_8303,
           control_protected_overload_is_selected_from_a_derived_class) {
    // §13.1: protected is visible to derived classes, so the exact one wins
    expectChecked(
        "module M;\n"
        "class B\n"
        "  public constructor() end constructor;\n"
        "  protected method Pick(x: int): string => \"protected-int\";\n"
        "  public method Pick(x: long): string => \"public-long\";\n"
        "end B;\n"
        "class D: B\n"
        "  public constructor() end constructor;\n"
        "  public method Go(): string => Pick(1);\n"
        "end D;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Go());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "protected-int\n");
}

BEZEL_TEST(codegen_private_overload_selection_8303,
           protected_overload_is_not_selected_from_an_unrelated_class) {
    // §13.1: protected is not visible to an unrelated class
    expectChecked(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  protected method Pick(x: int): string => \"protected-int\";\n"
        "  public method Pick(x: long): string => \"public-long\";\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    Console.WriteLine(v.Pick(1));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "public-long\n");
}

BEZEL_TEST(codegen_private_overload_selection_8303,
           control_nested_class_sees_the_enclosing_private_overload) {
    // §11.6 + §13.1: private extends between a class and its nested types
    expectChecked(
        "module M;\n"
        "class Outer\n"
        "  public constructor() end constructor;\n"
        "  private method Pick(x: int): string => \"private-int\";\n"
        "  public method Pick(x: long): string => \"public-long\";\n"
        "  public class Inner\n"
        "    public constructor() end constructor;\n"
        "    public method Go(o: Outer): string => o.Pick(1);\n"
        "  end Inner;\n"
        "end Outer;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var o := new Outer();\n"
        "    var i := new Outer.Inner();\n"
        "    Console.WriteLine(i.Go(o));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "private-int\n");
}
