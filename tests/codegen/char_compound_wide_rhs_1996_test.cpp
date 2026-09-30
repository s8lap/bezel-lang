// Codegen — #1996 (§6.6 / §5.2 / §6.2): a `char` compound assignment whose RHS
// is a `long` exceeding i32 range must run the §5.2 code-point check on the
// FULL-WIDTH result, matching the explicit-cast form — not silently truncate
// the RHS to i32 first.
//
// Per §6.6 (spec v0.214) a compound assignment whose LHS type is `char` and
// whose `c op e` result is an integer is `c := char(c op e)`; §6.2 promotes the
// char to the wider operand type (`char + long` runs in `long`), and §5.2 (L679)
// narrows the result back to `char`, raising OverflowException when it falls
// outside U+0000–U+10FFFF (or in the surrogate range). The explicit
// `char(int(c) + eL)` cast computes the addition at i64 and range-checks the
// true value.
//
// Before the fix, emitCompoundOp's char branch recursed into the generic
// integer path, which coerced the RHS to the char/i32 width (the #1042 operand
// width-match) BEFORE the arithmetic — truncating a wide `long`. So
// `char(0) += 4294967296L` (2^32) truncated 2^32 → 0 and stored char(0) with no
// raise, while `char(int(c) + 4294967296L)` correctly raised: a compound-vs-cast
// parity gap. Worse, `char(0) += 4294967361L` (2^32 + 65) truncated to 65 and
// silently stored 'A' — a wrong value from an out-of-range input. The fix widens
// the char operand to the RHS width so the arithmetic runs at full width and the
// §5.2 check sees the untruncated result. This test pins the parity: the
// compound and the cast agree on every wide-RHS input, while ordinary in-range
// compounds are unaffected.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning `int(c)` (or the named int) as the exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Returns 1 if `body` raises OverflowException, 0 if it runs to completion,
// -999 on a front-end failure.
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// --- The exact repro: `char(0) += 4294967296L` (2^32) must RAISE, not store
// char(0). §6.6: `c += e` is `c := char(c + e)`; §5.2: 2^32 > U+10FFFF. FAILS
// WITHOUT the fix (the i64 RHS truncated to i32 0 → stored char(0), exit 0). ---
BEZEL_TEST(codegen_1996_char_wide_rhs, plus_equals_pow2_32_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0); c += 4294967296L;"), 1);
}

// The sharpest case: `char(0) += 4294967361L` (2^32 + 65). Truncating the RHS to
// i32 yields 65 — a VALID 'A' code point — so the bug produced a WRONG in-range
// value with no raise. The full-width result (2^32 + 65) is out of range → raise.
BEZEL_TEST(codegen_1996_char_wide_rhs, plus_equals_pow2_32_plus_65_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0); c += 4294967361L;"), 1);
}

// A wide NEGATIVE long below U+0000: `char(0) += -4294967296L` → raise.
BEZEL_TEST(codegen_1996_char_wide_rhs, plus_equals_negative_wide_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0); c += -4294967296L;"), 1);
}

// `-=` with a wide long: `char(0) -= 4294967296L` → char(-2^32), below U+0000.
BEZEL_TEST(codegen_1996_char_wide_rhs, minus_equals_wide_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0); c -= 4294967296L;"), 1);
}

// PARITY CONTROL — the explicit-cast form raises on the identical input; the
// compound (above) must agree. `char(int(c) + 4294967296L)`.
BEZEL_TEST(codegen_1996_char_wide_rhs, explicit_cast_pow2_32_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var c: char := char(0); var e: char := char(int(c) + 4294967296L);"),
        1);
}

// In-range `long` RHS still stores its value (no false raise): a `long`-typed
// RHS that lands in the code-point range is not perturbed. char(0) += 65L → 'A'.
BEZEL_TEST(codegen_1996_char_wide_rhs, plus_equals_in_range_long_stores) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(0); c += 65L; return int(c);"), 65);
}

// In-range `long` RHS via `-=`: char(100) -= 35L → char(65) = 'A'.
BEZEL_TEST(codegen_1996_char_wide_rhs, minus_equals_in_range_long_stores) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(100); c -= 35L; return int(c);"), 65);
}

// fix #5150 (§6.6, v0.309): respelled `c *= 13L` as `c := char(int(c) * 13L)` —
// the previous test row relied on the spec-violating `c *=` admission and is a
// behaviour flip: §6.6 (v0.309) says "`c *= 2` and `c /= 2` are compile errors".
// The explicit `char(int(c) * 13L)` form is valid Bezel and produces the same
// result (5 * 13 = 65 = 'A').
BEZEL_TEST(codegen_1996_char_wide_rhs, times_equals_in_range_long_stores) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(5); c := char(int(c) * 13L); return int(c);"), 65);
}

// CONTROL — an ordinary small-RHS (int) char compound is unchanged: 'A' += 1 →
// 'B' (66). The wide-RHS widening is a no-op when the RHS is not wider than i32.
BEZEL_TEST(codegen_1996_char_wide_rhs, plus_equals_int_rhs_unaffected) {
    BEZEL_EXPECT_EQ(run("var c: char := 'A'; c += 1; return int(c);"), 66);
}

// CONTROL — a small-RHS overflow still raises through the normal i32 path:
// char(0x10FFFF) += 1 → above U+10FFFF (the #1961 behavior is undisturbed).
BEZEL_TEST(codegen_1996_char_wide_rhs, plus_equals_int_rhs_overflow_still_raises) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0x10FFFF); c += 1;"), 1);
}
