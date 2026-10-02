// fix #5150 (§3.5 / §6.6 / §17.6, v0.309, owner-ruled via #5067): `char` is not
// an arithmetic operand. A `char` compound target admits only `+=` / `-=`
// (the code-point advance `c++` / `c--` desugar to) with a right operand typed
// as in `int op e` -- `sbyte`..`int`, `long`, or an integer constant. So:
//   * a `char` RIGHT operand (`c -= d`, `c += d`, `c += 'x'`) is rejected;
//   * `n mod= c` (integer target, `char` operand) is rejected;
//   * a `char` shift count (`x << k`, `x <<= k`) is rejected;
//   * a non-constant `uint`/`ulong` right operand is rejected (#5211 pins it);
//   * `c *= 2`, `c /= 2` are rejected (landed earlier, #5150 narrow).
// Accept controls: `c += 1`, `c -= n` (int), `c -= m` (long), `c += 1L`,
// `c++`, `c--`, `int(c) * 2`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module M5150; class C public static method Main(): int " + body +
           " end Main; end C;";
}

void expectRejected(const std::string& body, const char* needle) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

int run(const std::string& body) {
    auto r = runAot(prog(body));
    return r.ran ? r.exitCode : -999;
}

constexpr const char* kCharOperand = "is not defined for `char` operands";

}  // namespace

// §6.6: "`c -= d` over `d: char` is rejected".
BEZEL_TEST(char_compound_operand_rejected_5150, char_minus_eq_char_rejected_spec_6_6) {
    expectRejected(
        "var c: char := char(50); var d: char := char(5); c -= d; return 0;",
        kCharOperand);
}

BEZEL_TEST(char_compound_operand_rejected_5150, char_plus_eq_char_rejected_spec_6_6) {
    expectRejected(
        "var c: char := char(50); var d: char := char(5); c += d; return 0;",
        kCharOperand);
}

BEZEL_TEST(char_compound_operand_rejected_5150, char_plus_eq_char_literal_rejected_spec_6_6) {
    expectRejected("var c: char := char(50); c += 'x'; return 0;", kCharOperand);
}

// §3.5 / §6.6: `n mod= c` -- a `char` operand of `mod`.
BEZEL_TEST(char_compound_operand_rejected_5150, int_mod_eq_char_rejected_spec_3_5) {
    expectRejected(
        "var n: int := 47; var c: char := char(10); n mod= c; return 0;",
        kCharOperand);
}

// §3.5: "a `char` shift count (`x << k` over `k: char`)".
BEZEL_TEST(char_compound_operand_rejected_5150, shift_count_char_rejected_spec_3_5) {
    expectRejected(
        "var x: int := 1; var k: char := char(2); var y := x << k; return y;",
        "is not valid on `char`");
}

BEZEL_TEST(char_compound_operand_rejected_5150, shift_assign_count_char_rejected_spec_6_6) {
    expectRejected(
        "var x: int := 1; var k: char := char(2); x <<= k; return x;",
        "is not valid on `char`");
}

// §6.6: a non-constant `uint` / `ulong` right operand has no §6.2 promotion.
BEZEL_TEST(char_compound_operand_rejected_5150, char_plus_eq_uint_rejected_spec_6_6) {
    expectRejected(
        "var c: char := char(50); var u: uint := 3u; c += u; return 0;",
        "has no `uint` right operand");
}

BEZEL_TEST(char_compound_operand_rejected_5150, char_plus_eq_ulong_rejected_spec_6_6) {
    expectRejected(
        "var c: char := char(50); var u: ulong := 3ul; c += u; return 0;",
        "has no `ulong` right operand");
}

// §6.6: "`c *= 2`, `c /= 2` ... are compile errors".
BEZEL_TEST(char_compound_operand_rejected_5150, char_times_eq_rejected_spec_6_6) {
    expectRejected("var c: char := char(5); c *= 2; return 0;", kCharOperand);
}

BEZEL_TEST(char_compound_operand_rejected_5150, char_div_eq_rejected_spec_6_6) {
    expectRejected("var c: char := char(50); c /= 2; return 0;", kCharOperand);
}

// ---- Accept controls (§6.6: `c += 1` is `c := char(int(c) + 1)`) ----

BEZEL_TEST(char_compound_operand_rejected_5150, accept_plus_eq_int_literal) {
    BEZEL_EXPECT_EQ(run("var c: char := char(50); c += 1; return int(c);"), 51);
}

BEZEL_TEST(char_compound_operand_rejected_5150, accept_minus_eq_int_variable) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(50); var n: int := 3; c -= n; return int(c);"), 47);
}

BEZEL_TEST(char_compound_operand_rejected_5150, accept_minus_eq_long_variable) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(50); var m: long := 4L; c -= m; return int(c);"), 46);
}

BEZEL_TEST(char_compound_operand_rejected_5150, accept_plus_eq_long_literal) {
    BEZEL_EXPECT_EQ(run("var c: char := char(50); c += 1L; return int(c);"), 51);
}

BEZEL_TEST(char_compound_operand_rejected_5150, accept_increment_decrement) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(50); c++; c++; c--; return int(c);"), 51);
}

BEZEL_TEST(char_compound_operand_rejected_5150, accept_int_cast_times) {
    BEZEL_EXPECT_EQ(run("var c: char := char(50); return int(c) * 2;"), 100);
}

BEZEL_TEST(char_compound_operand_rejected_5150, accept_int_cast_shift_count) {
    BEZEL_EXPECT_EQ(
        run("var x: int := 1; var k: char := char(2); return x << int(k);"), 4);
}
