// fix #836 / §19.3: a `case` over a non-`int` integer scrutinee
// (long/ulong/short/ushort/sbyte/byte/char) — or a `float` scrutinee — with a
// constant, range, or relational pattern previously aborted the compiler with an
// LLVM "Both operands to ICmp instruction are not of the same type!" assertion:
// the pattern constant has no resolved sema type, so its literal lowered at the
// default i32 / double width and mismatched the scrutinee's width.
//
// The fix reconciles each pattern value to the scrutinee's LLVM type before the
// comparison (integer trunc / (z|s)ext, FP ext/trunc), lowers integer-literal
// constants directly at the scrutinee width (so a wide value is not truncated to
// i32 first), and uses UNSIGNED comparison predicates for an unsigned scrutinee.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
std::string mainOf(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}
}  // namespace

// §19.3: a `long` constant pattern matches (was a compiler abort, rc=134).
BEZEL_TEST(case_wide_int_836, long_constant_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long := 5L; return (case x of 5L => 1, _ => 2);")), 1);
}

// The non-matching arm falls through to `_`.
BEZEL_TEST(case_wide_int_836, long_constant_no_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long := 9L; return (case x of 5L => 1, _ => 2);")), 2);
}

// A `long` constant exceeding i32 must NOT be truncated to i32 before the
// compare (5000000000 & 0xFFFFFFFF != 5000000000).
BEZEL_TEST(case_wide_int_836, long_wide_constant_not_truncated) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long := 5000000000L; "
        "return (case x of 5000000000L => 1, _ => 2);")), 1);
}

// A `long` range whose bounds exceed i32.
BEZEL_TEST(case_wide_int_836, long_wide_range) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long := 5000000000L; "
        "return (case x of 4000000000L..6000000000L => 1, _ => 2);")), 1);
}

// §19.3: a `short` constant pattern (i16 scrutinee, i32-default constant).
BEZEL_TEST(case_wide_int_836, short_constant_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: short := 5; return (case x of 5 => 1, _ => 2);")), 1);
}

// §19.3: an `sbyte` constant pattern (i8 scrutinee).
BEZEL_TEST(case_wide_int_836, sbyte_constant_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: sbyte := 5; return (case x of 5 => 1, _ => 2);")), 1);
}

// §19.3: a `byte` constant pattern (unsigned i8 scrutinee).
BEZEL_TEST(case_wide_int_836, byte_constant_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: byte := 200; return (case x of 200 => 1, _ => 2);")), 1);
}

// §19.3: a `ulong` constant pattern (unsigned i64 scrutinee).
BEZEL_TEST(case_wide_int_836, ulong_constant_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: ulong := 5UL; return (case x of 5UL => 1, _ => 2);")), 1);
}

// A `ulong` value above 2^63 in a range pattern: the comparison must be
// UNSIGNED (a signed compare would treat the high-bit value as negative and the
// range would not match).
BEZEL_TEST(case_wide_int_836, ulong_high_bit_range_unsigned) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: ulong := 10000000000000000000UL; "
        "return (case x of 9000000000000000000UL..11000000000000000000UL => 1, "
        "_ => 2);")), 1);
}

// §19.3: a `ushort` constant pattern.
BEZEL_TEST(case_wide_int_836, ushort_constant_match) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: ushort := 7; return (case x of 7 => 1, _ => 2);")), 1);
}

// §19.3: a relational pattern on a `long` with a wide bound.
BEZEL_TEST(case_wide_int_836, long_relop_wide_bound) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long := 5000000000L; "
        "return (case x of > 4000000000L => 1, _ => 2);")), 1);
}

// §19.3: a relational pattern on an unsigned `byte` scrutinee.
BEZEL_TEST(case_wide_int_836, byte_relop_unsigned) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: byte := 200; return (case x of >= 100 => 1, _ => 2);")), 1);
}

// §19.3: a `long` range pattern (closed) matches a value inside it.
BEZEL_TEST(case_wide_int_836, long_range_inside) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long := 5L; return (case x of 1L..10L => 1, _ => 2);")), 1);
}

// §19.3: a `long` range pattern excludes a value outside it.
BEZEL_TEST(case_wide_int_836, long_range_outside) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: long := 50L; return (case x of 1L..10L => 1, _ => 2);")), 2);
}

// Regression: an ordinary `int` constant pattern still works.
BEZEL_TEST(case_wide_int_836, int_constant_regression) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: int := 5; return (case x of 5 => 1, _ => 2);")), 1);
}

// Regression: a `string` scrutinee still routes through content equality.
BEZEL_TEST(case_wide_int_836, string_constant_regression) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: string := \"hi\"; return (case x of \"hi\" => 1, _ => 2);")), 1);
}
