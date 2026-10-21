// fix #6258 (§22.6 *Lambda-literal arguments*, §26.5, v0.339):
//
// §26.5 selects the extension block (most specific receiver); the same-name
// members of that block are then an ordinary §22.6 overload set, and §22.6
// *Lambda-literal arguments* makes a candidate whose return slot is a value
// inapplicable for a procedure-bodied lambda ("a unit-typed body or a `: ()`
// ascription is exact" at a `-> ()` slot; §27.3: "a procedure-bodied lambda
// does not convert to a value-returning function type").
//
// Pre-fix `resolveMethodOverload` returned at once for a method with no parent
// type, so an extension call ran the FIRST same-name, same-arity member of the
// block, never ranked: `"a".Over(lambda() => do ... end)` against
// `Over(f: () -> int)` declared before `Over(f: () -> ())` ran the `int` one.
// The same early return is #5987's: `Ext(x: int)` beside `Ext(x: string)`
// called with a `string` was rejected against the first member (narrowed
// here; #5987's §11.3 duplicate-signature half is not touched).
//
// The interface-receiver half of #6258's title is already correct on the base
// (MEASURED: the #5859 tie-break picks the `-> ()` member); its row here is a
// guard.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string overPair6258(bool unitFirst) {
    const std::string i = "  public method Over(f: () -> int): string => \"int\";\n";
    const std::string u = "  public method Over(f: () -> ()): string => \"unit\";\n";
    return unitFirst ? u + i : i + u;
}

}  // namespace

// The issue's repro: a closed (`string`) block, both declaration orders.
BEZEL_TEST(extension_overload_procedure_lambda_6258,
           closed_block_procedure_lambda_selects_unit_member_both_orders_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        auto r = runAot(
            "module T;\n"
            "extension string\n" +
            overPair6258(unitFirst != 0) +
            "end;\n"
            "public static class Program\n"
            "  public static method Main(args: Array<string>): int\n"
            "    Console.WriteLine(\"a\".Over(lambda() => do Console.WriteLine(\"x\"); end));\n"
            "    Console.WriteLine(\"a\".Over(lambda(): () => do Console.WriteLine(\"y\"); end));\n"
            "    return 0;\n"
            "  end;\n"
            "end;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("unit\nunit\n"));
    }
}

// A generic block (`extension Array<T>`): its members are filtered by the same
// certain-inapplicability evidence (not ranked by type), both orders.
BEZEL_TEST(extension_overload_procedure_lambda_6258,
           generic_block_procedure_lambda_selects_unit_member_both_orders_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        auto r = runAot(
            "module T;\n"
            "extension Array<T>\n" +
            overPair6258(unitFirst != 0) +
            "end;\n"
            "public static class Program\n"
            "  public static method Main(args: Array<string>): int\n"
            "    var a: Array<int> := [1];\n"
            "    Console.WriteLine(a.Over(lambda() => do Console.WriteLine(\"x\"); end));\n"
            "    return 0;\n"
            "  end;\n"
            "end;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("unit\n"));
    }
}

// The selected member CALLS the lambda. Pre-fix the `() -> int` member ran and
// printed whatever the procedure lambda left in the return register; a value
// lambda still selects the `() -> int` member. Looped, checked (ASan/LSan).
BEZEL_TEST(extension_overload_procedure_lambda_6258,
           selected_member_invokes_lambda_and_value_lambda_keeps_int_checked_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "extension string\n"
        "  public method Over(f: () -> int): int => f();\n"
        "  public method Over(f: () -> ()): int\n"
        "    f();\n"
        "    return 7;\n"
        "  end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var t := 0;\n"
        "    for var i in 0..<50 do\n"
        "      t += \"a\".Over(lambda() => do Console.Write(\"w\"); end);\n"
        "      t += \"a\".Over(lambda() => 3);\n"
        "    end;\n"
        "    Console.WriteLine(\"\");\n"
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    std::string want;
    for (int i = 0; i < 50; ++i) want += "w";
    want += "\n500\n";
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// narrow #5987: the block's members are ranked by argument type too, so the
// `string` argument reaches `Ext(x: string)` declared second.
BEZEL_TEST(extension_overload_procedure_lambda_6258,
           closed_block_members_ranked_by_argument_type_5987_spec_22_6) {
    auto r = runAot(
        "module T;\n"
        "extension string\n"
        "  public method Ext(x: int): string => \"int\";\n"
        "  public method Ext(x: string): string => \"str\";\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Console.WriteLine(\"a\".Ext(\"s\"));\n"
        "    Console.WriteLine(\"a\".Ext(1));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("str\nint\n"));
}

// narrow #5987, the issue's own repro: `Pick(e: Bar)` is the exact match for
// a `Bar` argument and beats `Pick(e: Foo)` declared first (§22.6
// *Implicit conversions and tie-breaking*).
BEZEL_TEST(extension_overload_procedure_lambda_6258,
           closed_block_exact_match_beats_first_declared_5987_spec_22_6) {
    auto r = runAot(
        "module M;\n"
        "class Foo end Foo;\n"
        "class Bar: Foo end Bar;\n"
        "class K end K;\n"
        "extension K\n"
        "  public method Pick(e: Foo): int => 11;\n"
        "  public method Pick(e: Bar): int => 22;\n"
        "end;\n"
        "class C public static method Main(): int\n"
        "  using var b := new Bar(); using var k := new K();\n"
        "  Console.WriteLine(k.Pick(b));\n"
        "  return 0;\n"
        "end Main; end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("22\n"));
}

// Guard (passes on the base too): an INTERFACE-declared pair called through the
// interface, both orders, picks the `-> ()` member.
BEZEL_TEST(extension_overload_procedure_lambda_6258,
           interface_pair_procedure_lambda_selects_unit_member_guard_spec_22_6) {
    for (int unitFirst = 0; unitFirst < 2; ++unitFirst) {
        std::string a = "  method Over(f: () -> int): string;\n";
        std::string b = "  method Over(f: () -> ()): string;\n";
        auto r = runAot(
            "module T;\npublic interface I\n" + (unitFirst ? b + a : a + b) + "end;\n"
            "public class C : I\n"
            "  public method Over(f: () -> int): string => \"int\";\n"
            "  public method Over(f: () -> ()): string => \"unit\";\n"
            "end;\n"
            "public static class Program\n"
            "  public static method Main(args: Array<string>): int\n"
            "    var i: I := new C();\n"
            "    Console.WriteLine(i.Over(lambda() => do Console.WriteLine(\"x\"); end));\n"
            "    return 0;\n"
            "  end;\n"
            "end;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("unit\n"));
    }
}
