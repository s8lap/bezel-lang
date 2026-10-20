// Regression tests for #6202 (§10.5 / §9.2, v0.338): an enum declaring only
// `Equals(other: int)` had that member run through `Object.Equals` on the
// enumerators' integers, so two boxes of the same enumerator compared unequal.
// Only the own-typed `Equals(other: E)` fills the slot; with none declared the
// generated integer comparison answers.  Rows assert stdout.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string enumProg(const std::string& members) {
    return "module P;\n"
           "public enum E\n"
           "  A, B;\n" + members +
           "end E;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    var n := 0; var m := 0;\n"
           "    for var j in 0..<100 do\n"
           "      var o: Object := E.A;\n"
           "      var o2: Object := E.A;\n"
           "      var o3: Object := E.B;\n"
           "      if o.Equals(o2) then n := n + 1; end;\n"
           "      if o.Equals(o3) then m := m + 1; end;\n"
           "    end for;\n"
           "    Console.WriteLine(\"same=\" + n.ToString() + \" diff=\" + m.ToString());\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// #6202 / §10.5: only `Equals(other: int)` declared -> generated comparison.
BEZEL_TEST(object_slot_equals_selection_10_5_6202, enum_only_int_overload) {
    JitResult r = runAotChecked(enumProg(
        "  public method Equals(other: int): bool => other = 42;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("same=100 diff=0\n"));
}

// Own-typed beside an int overload, either order: the own-typed one fills the
// slot (it answers true for every pair here).
BEZEL_TEST(object_slot_equals_selection_10_5_6202, enum_int_then_own) {
    JitResult r = runAotChecked(enumProg(
        "  public method Equals(other: int): bool => false;\n"
        "  public method Equals(other: E): bool => true;\n"
        "  public method GetHashCode(): int => 7;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("same=100 diff=100\n"));
}

BEZEL_TEST(object_slot_equals_selection_10_5_6202, enum_own_then_int) {
    JitResult r = runAotChecked(enumProg(
        "  public method Equals(other: E): bool => true;\n"
        "  public method Equals(other: int): bool => false;\n"
        "  public method GetHashCode(): int => 7;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("same=100 diff=100\n"));
}

// Hash-container use: a Set of E holds one A and one B even though the declared
// `Equals(other: int)` would answer false for every pair.
BEZEL_TEST(object_slot_equals_selection_10_5_6202, enum_only_int_overload_set) {
    JitResult r = runAotChecked(
        "module P;\n"
        "public enum E\n"
        "  A, B;\n"
        "  public method Equals(other: int): bool => false;\n"
        "end E;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var s := new Set<E>();\n"
        "    s.Add(E.A); s.Add(E.A); s.Add(E.B);\n"
        "    Console.WriteLine(s.Count.ToString());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}
