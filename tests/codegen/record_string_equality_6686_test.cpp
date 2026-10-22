// fix #6686 (residual: a `string` operand) (§8.4, §25.2, §9.2, v0.340;
// owner ruling #6210): `=` / `<>` between a record or choice value and a
// `string`.
//
//   §8.4 (v0.340): "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper".
//   §25.2 (v0.340): "`=` and `<>` with no applicable overload are §8.4's
//   equality, which governs them."
//   §9.2 (v0.340): "any other argument compares unequal without the body
//   running".
//
// Before the fix the string-equality arm of lowerBinary keyed on the string
// side alone and handed the record struct to emitStringEq: "Both operands to
// ICmp instruction are not of the same type! %str.sameptr = icmp eq ptr
// @.bz.str.59, %record.S %8" (IR verification failed). The record-vs-other-
// type fold #6686's first commit added sits after the string arm and never
// ran for a string operand.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kTypes =
    "record S(N: string) end S;\n"
    "choice Sh\n"
    "  Circle(R: int),\n"
    "  Dot;\n"
    "end Sh;\n"
    "public record Q(X: int)\n"
    "  public static pure method operator =(a: Q, b: Q): bool => a.X = b.X;\n"
    "  public static pure method operator <>(a: Q, b: Q): bool => a.X <> b.X;\n"
    "  public override method Equals(o: Q): bool => this.X = o.X;\n"
    "  public override method GetHashCode(): int => this.X;\n"
    "end Q;\n";

std::string program(const std::string& mainBody) {
    return std::string("module M;\n") + kTypes +
           "public class Program\n"
           "  public static method Main(): int\n" + mainBody +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

std::string runOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::string s = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) s += " | " + d.message;
        return s;
    }
    return r.stdoutText + "exit=" + std::to_string(r.exitCode);
}

}  // namespace

// The reopened residual's probe (a12_strrec_vs_str.bzl), verbatim.
BEZEL_TEST(codegen_record_string_equality_6686, issue_repro_verbatim) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "record S(N: string) end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var v := new S(\"a\");\n"
        "    Console.WriteLine(v = \"a\"); Console.WriteLine(\"a\" <> v);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("False\nTrue\nexit=0"));
}

BEZEL_TEST(codegen_record_string_equality_6686, both_orders_both_operators) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var v := new S(\"a\");\n"
        "    Console.WriteLine(v <> \"a\"); Console.WriteLine(\"a\" = v);\n")),
        std::string("True\nFalse\nexit=0"));
}

BEZEL_TEST(codegen_record_string_equality_6686, string_locals_and_absent_string) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var v := new S(\"a\"); var s := \"a\"; var ns: string? := null;\n"
        "    Console.WriteLine(v = s); Console.WriteLine(s = v); Console.WriteLine(v = ns); Console.WriteLine(ns <> v);\n")),
        std::string("False\nFalse\nFalse\nTrue\nexit=0"));
}

// +1 string temporaries on either side are released (loop: a leak would show
// under the checked oracle's LSan leg).
BEZEL_TEST(codegen_record_string_equality_6686, string_temporaries_in_loop) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var c := 0; var v := new S(\"q\");\n"
        "    for var i in 0 ..< 200 do if v = \"a\" + i.ToString() then c := c + 1; end; end;\n"
        "    for var i in 0 ..< 200 do if \"a\" + i.ToString() <> v then c := c + 1; end; end;\n"
        "    Console.WriteLine(c);\n")),
        std::string("200\nexit=0"));
}

BEZEL_TEST(codegen_record_string_equality_6686, choice_operand) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var c := Sh.Circle(2);\n"
        "    Console.WriteLine(c = \"Circle\"); Console.WriteLine(\"x\" <> c);\n")),
        std::string("False\nTrue\nexit=0"));
}

// A record whose `operator =` is inapplicable to a string falls back to §8.4.
BEZEL_TEST(codegen_record_string_equality_6686, inapplicable_overload_falls_back) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var q := new Q(1);\n"
        "    Console.WriteLine(q = \"a\"); Console.WriteLine(\"a\" <> q); Console.WriteLine(q = new Q(1));\n")),
        std::string("False\nTrue\nTrue\nexit=0"));
}

// A nullable record meets §8.4's null-safe wrapper: both absent is equal.
// Before the fix this shape crashed the compiler (rc 139).
BEZEL_TEST(codegen_record_string_equality_6686, nullable_record_null_safe) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var v: S? := new S(\"a\"); var w: S? := null; var ns: string? := null;\n"
        "    Console.WriteLine(v = \"a\"); Console.WriteLine(w = ns); Console.WriteLine(w <> \"a\"); Console.WriteLine(v = ns);\n")),
        std::string("False\nTrue\nTrue\nFalse\nexit=0"));
}

// Control: record = record and string = string are unchanged.
BEZEL_TEST(codegen_record_string_equality_6686, same_type_control) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var v := new S(\"a\"); var w := new S(\"a\"); var s := \"a\";\n"
        "    Console.WriteLine(v = w); Console.WriteLine(\"a\" = s);\n")),
        std::string("True\nTrue\nexit=0"));
}
