// fix #6939 (§5.1, §7.11, v0.340): the `null` literal as an operand of a
// user-defined operator whose formal is a nullable value type.
//
//   §5.1 (v0.340), nullable conversions: "For any type T, T implicitly
//   converts to T?", and the `null` literal converts to any `T?`.
//   §7.11 (v0.340), User-defined operators: a user-declared operator "is
//   likewise not defined on a nullable operand" -- the rejection applies where
//   the operator's formal is NON-nullable; a `T?` formal takes a nullable
//   operand (and the null literal) as an ordinary argument.
//
// Before the fix `p + null` against `operator +(a: Pt?, b: Pt?)` was rejected
// with the §7.11 nullable-operand error although `p + q` (two `Pt?` locals)
// was accepted: the candidate search classed the `null` literal (context-free
// type `Object?`) as inapplicable to a `Pt?` formal.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPt =
    "public record Pt(X: int, Y: int)\n"
    "  public static pure method operator +(a: Pt?, b: Pt?): int => 200 + (a?.X ?? 50) + (b?.X ?? 50);\n"
    "  public static pure method operator *(a: Pt, b: Pt?): int => 1000 + a.X + (b?.X ?? 50);\n"
    "  public static pure method operator -(a: Pt?): int => 500 + (a?.X ?? 50);\n"
    "end Pt;\n";

std::string program(const std::string& mainBody) {
    return std::string("module M;\n") + kPt +
           "public class P\n"
           "  public static method Pick(b: bool): Pt? if b then return new Pt(1, 1); end; return null; end Pick;\n"
           "  public static method Main(): int\n" + mainBody +
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
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

bool rejectedWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// The issue's repro (n_null_lit.bzl): 200 + 1 + 50 on both sides.
BEZEL_TEST(codegen_user_operator_null_literal_6939, issue_repro_null_right_and_left) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var p: Pt? := P.Pick(true);\n"
        "    Console.WriteLine(p + null); Console.WriteLine(null + p);\n")),
        std::string("251\n251\nexit=0"));
}

// An absent nullable operand beside the null literal: 200 + 50 + 50.
BEZEL_TEST(codegen_user_operator_null_literal_6939, absent_operand_and_null) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var q: Pt? := P.Pick(false);\n"
        "    Console.WriteLine(q + null); Console.WriteLine(null + q);\n")),
        std::string("300\n300\nexit=0"));
}

// A bare (non-nullable) operand beside the null literal, and a mixed-formal
// operator `(Pt, Pt?)` taking null at its nullable second formal: 1000+7+50.
BEZEL_TEST(codegen_user_operator_null_literal_6939, bare_operand_and_mixed_formals) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var a := new Pt(7, 7);\n"
        "    Console.WriteLine(a + null); Console.WriteLine(a * null);\n")),
        std::string("257\n1057\nexit=0"));
}

// Loop: the null-literal carrier is built per iteration without leaking.
BEZEL_TEST(codegen_user_operator_null_literal_6939, null_literal_in_loop) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var s := 0;\n"
        "    for var i in 0 ..< 300 do s := s + (new Pt(i, i) + null) - 250; end;\n"
        "    Console.WriteLine(s);\n")),
        std::string("44850\nexit=0"));
}

// Control (unchanged): two nullable locals at the same operator.
BEZEL_TEST(codegen_user_operator_null_literal_6939, nullable_locals_control) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var p: Pt? := P.Pick(true); var q: Pt? := P.Pick(true);\n"
        "    Console.WriteLine(p + q);\n")),
        std::string("202\nexit=0"));
}

// Unary control with a nullable formal.
BEZEL_TEST(codegen_user_operator_null_literal_6939, unary_nullable_formal_control) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var p: Pt? := P.Pick(true); var q: Pt? := P.Pick(false);\n"
        "    Console.WriteLine(-p); Console.WriteLine(-q);\n")),
        std::string("501\n550\nexit=0"));
}

// Still rejected: a nullable operand beside `null` at a NON-nullable formal
// is the §7.11 error (the rejection turns on the formal, not the operand).
BEZEL_TEST(codegen_user_operator_null_literal_6939, nonnullable_formal_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "  public static pure method operator +(a: Pt, b: Pt): int => a.X + b.X;\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Pick(b: bool): Pt? if b then return new Pt(1, 1); end; return null; end Pick;\n"
        "  public static method Main(): int\n"
        "    var p: Pt? := P.Pick(true);\n"
        "    Console.WriteLine(p + null);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        "operator `+` is not defined on a nullable value type"));
}

// Still rejected: null at a NON-nullable first formal of `(Pt, Pt?)`.
BEZEL_TEST(codegen_user_operator_null_literal_6939, null_at_nonnullable_formal_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(program(
        "    var a := new Pt(7, 7);\n"
        "    Console.WriteLine(null * a);\n"),
        "operator `*`"));
}

// `null + null` names no operand type to draw a candidate set from (§25.2).
BEZEL_TEST(codegen_user_operator_null_literal_6939, null_plus_null_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(program(
        "    Console.WriteLine(null + null);\n"),
        "operator `+`"));
}
