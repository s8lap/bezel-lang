// Regression tests for #6263 (§9.2, v0.338): the own-typed `Equals(other: T)` is
// "the one member that fills the kind's `Object` slot". A mode-only overload
// `Equals(ref other: T)` declared AFTER the exact one was selected instead
// (last name+arity match), judged off-shape, and the box answered member-wise
// although the exact member was declared. Hash containers (Set) went through
// the same selection.
//
// Every row asserts stdout. The exact member compares a key mod 10 (so it is
// distinguishable from member-wise equality) and the competing overload
// answers `false`, so choosing it is visible.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kExactRec =
    "  public method Equals(other: R): bool => (X mod 10) = (other.X mod 10);\n";
const char* kModeRec =
    "  public method Equals(ref other: R): bool => false;\n";
// The homogeneous operators sit beside the pair (the pair-only shape is #5934).
const char* kHashRec =
    "  public method GetHashCode(): int => (X mod 10) + 500;\n"
    "  public static method operator =(a: R, b: R): bool =>\n"
    "    (a.X mod 10) = (b.X mod 10);\n"
    "  public static method operator <>(a: R, b: R): bool =>\n"
    "    (a.X mod 10) <> (b.X mod 10);\n";

std::string recordProg(const std::string& first, const std::string& second) {
    return "module M;\n"
           "public record R(X: int, Y: int)\n" + first + second + kHashRec +
           "end R;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    var n := 0; var d := 0; var s := 0;\n"
           "    for var j in 0..<100 do\n"
           "      var a: Object := new R(5, 1);\n"
           "      var b: Object := new R(15, 2);\n"
           "      var c: Object := new R(6, 1);\n"
           "      if a.Equals(b) then n := n + 1; end;\n"
           "      if a.Equals(c) then d := d + 1; end;\n"
           "      var set := new Set<R>();\n"
           "      set.Add(new R(5, 1)); set.Add(new R(15, 2)); set.Add(new R(6, 1));\n"
           "      s := s + set.Count;\n"
           "    end for;\n"
           "    Console.WriteLine(n.ToString() + \" \" + d.ToString() + \" \" +\n"
           "                      s.ToString());\n"
           "  end Main;\n"
           "end Program;\n";
}

std::string choiceProg(const std::string& first, const std::string& second) {
    return "module M;\n"
           "public choice C\n"
           "  P(x: int), Q;\n"
           "  public method Key(): int => (case this of P(v) => v mod 10, Q => -1);\n" +
           first + second +
           "  public method GetHashCode(): int => Key() + 500;\n"
           "end C;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    var n := 0; var d := 0;\n"
           "    for var j in 0..<100 do\n"
           "      var a: Object := C.P(5);\n"
           "      var b: Object := C.P(15);\n"
           "      var c: Object := C.P(6);\n"
           "      if a.Equals(b) then n := n + 1; end;\n"
           "      if a.Equals(c) then d := d + 1; end;\n"
           "    end for;\n"
           "    Console.WriteLine(n.ToString() + \" \" + d.ToString());\n"
           "  end Main;\n"
           "end Program;\n";
}

const char* kExactCho =
    "  public method Equals(other: C): bool => Key() = other.Key();\n";
const char* kModeCho =
    "  public method Equals(ref other: C): bool => false;\n";

}  // namespace

// #6263 / §9.2: exact first, mode-only overload second.
BEZEL_TEST(object_slot_equals_selection_9_2_6263, record_exact_then_mode) {
    JitResult r = runAotChecked(recordProg(kExactRec, kModeRec));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0 200\n"));
}

// Declaration order swapped: the exact member must win either way.
BEZEL_TEST(object_slot_equals_selection_9_2_6263, record_mode_then_exact) {
    JitResult r = runAotChecked(recordProg(kModeRec, kExactRec));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0 200\n"));
}

BEZEL_TEST(object_slot_equals_selection_9_2_6263, choice_exact_then_mode) {
    JitResult r = runAotChecked(choiceProg(kExactCho, kModeCho));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0\n"));
}

BEZEL_TEST(object_slot_equals_selection_9_2_6263, choice_mode_then_exact) {
    JitResult r = runAotChecked(choiceProg(kModeCho, kExactCho));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0\n"));
}

