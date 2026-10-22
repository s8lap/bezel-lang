// fix #6869 (§6.1 checked unary `-`, §17.14 constant evaluation, v0.340).
//
// §17.14 (v0.340): a `const` reference and unary `-` over a constant operand
// are a constant expression; it "is evaluated at compile time under the same
// semantics the expression has at run time -- §6.1 checked integer
// arithmetic", and an evaluation fault (integer overflow) "is a compile error
// at the constant expression, never a deferred runtime fault". §6.1: unary
// `-` is checked; `-m` has no `uint`/`ulong`/`byte`/`ushort` result for
// m > 0, and `-int.MinValue` has no `int` result.
//
// Before the fix `const C: uint := 3; var y: uint := -C;` (and the byte /
// ushort / ulong / `-(1U + 2U)` / `-uint.MaxValue` forms) compiled and threw
// OverflowException at run time. #6834 had decided the literal operand only.
//
// The refuted first attempt (beb35afd) also rejected a `-C` that a constant
// short-circuits; §17.14: "an operand that would not be evaluated at run time
// is not evaluated at compile time and contributes no fault". Those rows are
// pinned as controls below.
//
// Probes: /var/tmp/bezel-scratch/r34-6869/p/ (i6869_*, b01-b23).

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string prog(const std::string& members, const std::string& body) {
    return "module M;\nclass P\n" + members +
           "\npublic static method Main(): int\n  " + body +
           "\n  return 0; end Main; end P;\n";
}

bool rejected(const std::string& members, const std::string& body) {
    auto r = runAot(prog(members, body));
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, "has no representable result");
}

std::string prints(const std::string& members, const std::string& body) {
    auto r = runAot(prog(members, body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's repro and the byte / ushort / uint forms from its comment.
BEZEL_TEST(negative_unsigned_constant_unary_6869,
           negated_unsigned_const_reference_rejected_spec_17_14) {
    BEZEL_EXPECT_TRUE(rejected(
        "", "const C: uint := 3; var y: uint := -C; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected("", "const K: byte := 3; Console.WriteLine(-K);"));
    BEZEL_EXPECT_TRUE(
        rejected("", "const K: ushort := 3; Console.WriteLine(-K);"));
    BEZEL_EXPECT_TRUE(rejected("", "const K: uint := 3; Console.WriteLine(-K);"));
    BEZEL_EXPECT_TRUE(rejected(
        "private const C: ulong := 3;",
        "var y: ulong := -C; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected(
        "public const C: ulong := 3;",
        "var y: ulong := -P.C; Console.WriteLine(y);"));
    // A ulong const above LLONG_MAX (the untyped fold's bit pattern).
    BEZEL_EXPECT_TRUE(rejected(
        "private const C: ulong := 18446744073709551615UL;",
        "var y: ulong := -C; Console.WriteLine(y);"));
    // The result type is the operand's, whatever the target.
    BEZEL_EXPECT_TRUE(rejected(
        "private const C: uint := 3;",
        "var y: long := -C; Console.WriteLine(y);"));
}

// Other constant operands: an operation, a chained const, a primitive wrapper
// constant, the signed `-int.MinValue`, a doubled negation.
BEZEL_TEST(negative_unsigned_constant_unary_6869,
           negated_constant_expression_rejected_spec_6_1) {
    BEZEL_EXPECT_TRUE(
        rejected("", "var y: uint := -(1U + 2U); Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected(
        "", "const C: uint := 3; const D: uint := C + 1U; "
            "var y: uint := -D; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(
        rejected("", "var y: uint := -uint.MaxValue; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(
        rejected("", "var y := -int.MinValue; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected(
        "private const C: uint := 3;",
        "var y: uint := -(-C); Console.WriteLine(y);"));
}

// Controls: zero, signed constants, a runtime const and a variable are
// unchanged (the last two stay §6.1's run-time check).
BEZEL_TEST(negative_unsigned_constant_unary_6869,
           zero_signed_and_runtime_operands_unchanged) {
    BEZEL_EXPECT_EQ(prints("", "const Z: uint := 0; var y: uint := -Z; "
                               "Console.WriteLine(y);"),
                    std::string("0\n"));
    BEZEL_EXPECT_EQ(prints("", "const C: int := 3; var y: int := -C; "
                               "Console.WriteLine(y);"),
                    std::string("-3\n"));
    BEZEL_EXPECT_EQ(prints("", "var y := -int.MaxValue; Console.WriteLine(y);"),
                    std::string("-2147483647\n"));
    BEZEL_EXPECT_EQ(prints("", "var a: uint := 3; const R: uint := a; "
                               "try var y: uint := -R; Console.WriteLine(y); "
                               "catch OverflowException => "
                               "Console.WriteLine(\"ovf\"); end try;"),
                    std::string("ovf\n"));
    BEZEL_EXPECT_EQ(prints("", "var x: uint := 3; try var y: uint := -x; "
                               "Console.WriteLine(y); catch OverflowException "
                               "=> Console.WriteLine(\"ovf\"); end try;"),
                    std::string("ovf\n"));
}

// Controls: the rows the refuted beb35afd over-rejected. A `-C` that a
// constant short-circuits contributes no fault (§17.14).
BEZEL_TEST(negative_unsigned_constant_unary_6869,
           short_circuited_operand_contributes_no_fault_spec_17_14) {
    const std::string c = "private const C: uint := 3;";
    BEZEL_EXPECT_EQ(prints(c, "var y: uint := (if false then -C else 1U); "
                              "Console.WriteLine(y);"),
                    std::string("1\n"));
    BEZEL_EXPECT_EQ(prints(c, "const Y: uint := (if false then -C else 1U); "
                              "Console.WriteLine(Y);"),
                    std::string("1\n"));
    BEZEL_EXPECT_EQ(prints(c, "var y := false and (-C > 0U); "
                              "Console.WriteLine(y);"),
                    std::string("False\n"));
    BEZEL_EXPECT_EQ(prints(c, "var y := true or (-C > 0U); "
                              "Console.WriteLine(y);"),
                    std::string("True\n"));
    // ... but an evaluated one still faults.
    BEZEL_EXPECT_TRUE(rejected(c, "var t := true; var y := t and (-C > 0U); "
                                  "Console.WriteLine(y);"));
}
