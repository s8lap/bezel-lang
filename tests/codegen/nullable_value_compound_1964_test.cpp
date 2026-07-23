// Codegen — #1964 (§7.11 / §6.6 / §5.2): a compound assignment whose LHS is a
// nullable VALUE type (`char?`, `int?`, `long?`, `double?`, …) must lift per
// §7.11, not crash codegen with malformed IR.
//
// The target slot is the tagged `{i8 hasValue; T}` struct. Before the fix the
// compound-assignment path loaded that struct and fed it straight to the
// built-in emitCompoundOp, which passed the WHOLE struct to
// `llvm.*add.with.overflow` — emitting `llvm.uadd.with.overflow.sl_i8i32s`
// (an i32-payload/i8-tag struct where an integer was required) and aborting the
// IR verifier ("Intrinsic has incorrect return type"). The exact repro from the
// issue — `var c: char? := char('A'); c += 1;` — crashed the compiler.
//
// §7.11 (spec v0.214, L1095): the arithmetic/bitwise/shift operators lift to
// `T?` — the result is `null` if EITHER operand is `null`, and the underlying
// operation and its runtime checks (`OverflowException` on integer overflow,
// `DivideByZeroException`) run ONLY when both operands are present. §6.6 (L864):
// the desugared `n := n OP e` applies the checked narrow-integer write-back;
// for a `char` target the §5.2 code-point range check (L679:
// U+0000–U+10FFFF, surrogate range excluded → OverflowException) applies. This
// test pins that a nullable-value compound assignment lowers correctly for the
// whole family: the value is computed when present, `null` propagates when the
// operand is null, and the §6.6/§5.2 checked write-back still raises on an
// out-of-range result.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning an int exit code (result fits 0..255).
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

// --- The exact issue repro: `char? := char('A'); c += 1;` no longer crashes
// codegen and stores 'B' (66). §7.11 lift over §6.6 char write-back. ---
BEZEL_TEST(codegen_1964_nullable_compound, char_plus_equals_present_stores_value) {
    BEZEL_EXPECT_EQ(
        run("var c: char? := char('A'); c += 1; return int(c ?? char(0));"), 66);
}

// §7.11: a `null` char? operand propagates null — no arithmetic runs, so the
// coalesced sentinel (99) is observed, not a value.
BEZEL_TEST(codegen_1964_nullable_compound, char_plus_equals_null_stays_null) {
    BEZEL_EXPECT_EQ(
        run("var c: char? := null; c += 1; return int(c ?? char(99));"), 99);
}

// §6.6/§5.2: the char code-point check still fires on the present branch —
// `char(0x10FFFF) += 1` desugars to `char(0x110000)`, out of range → Overflow.
BEZEL_TEST(codegen_1964_nullable_compound, char_plus_equals_above_max_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char? := char(0x10FFFF); c += 1;"), 1);
}

// §6.6/§5.2: a `null` char? never runs the op, so no §5.2 check fires even at
// the max code point (the check is present-branch only, per §7.11).
BEZEL_TEST(codegen_1964_nullable_compound, char_null_at_max_no_throw) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char? := null; c += 1;"), 0);
}

// §7.11: `int?` compound — the family member from the issue's "value-builtin /
// nullable-value-struct" note; `int? 5 += 10` → 15.
BEZEL_TEST(codegen_1964_nullable_compound, int_plus_equals_present_stores_value) {
    BEZEL_EXPECT_EQ(run("var i: int? := 5; i += 10; return (i ?? -1);"), 15);
}

// §7.11: a `null` int? operand propagates null (coalesced sentinel observed).
BEZEL_TEST(codegen_1964_nullable_compound, int_plus_equals_null_stays_null) {
    BEZEL_EXPECT_EQ(run("var i: int? := null; i += 10; return (i ?? 42);"), 42);
}

// §7.11: `int?` other operators lower too — `*=` and `-=`.
BEZEL_TEST(codegen_1964_nullable_compound, int_times_and_minus_present) {
    BEZEL_EXPECT_EQ(run("var i: int? := 6; i *= 7; return (i ?? -1);"), 42);
    BEZEL_EXPECT_EQ(run("var i: int? := 50; i -= 8; return (i ?? -1);"), 42);
}

// §7.11 / §6.1: division-by-zero still raises on the present branch.
BEZEL_TEST(codegen_1964_nullable_compound, int_div_by_zero_throws) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var i: int? := 10; "
        "  try i /= 0; return 0; catch DivideByZeroException => return 1; end; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// §5.1 width promotion in the lifted op: `long? += int` promotes the i32 RHS to
// i64 before the checked add — `9000000000 + 1` (exceeds i32) is exact.
BEZEL_TEST(codegen_1964_nullable_compound, long_plus_equals_width_promotion) {
    BEZEL_EXPECT_EQ(run(
        "var l: long? := 9000000000L; l += 1; "
        "if (l ?? 0L) = 9000000001L then return 1; end; return 0;"), 1);
}

// §6.6/§5.2 sub-i32 checked narrow lifts too: `byte? 200 += 100` = 300, out of
// byte range → OverflowException (not a silent truncation).
BEZEL_TEST(codegen_1964_nullable_compound, byte_plus_equals_overflow_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte? := 200; b += 100;"), 1);
}

// Control: an in-range byte? compound still stores its value (150).
BEZEL_TEST(codegen_1964_nullable_compound, byte_plus_equals_in_range) {
    BEZEL_EXPECT_EQ(run("var b: byte? := 100; b += 50; return (b ?? 0);"), 150);
}

// §7.11: an unsigned nullable must select UNSIGNED arithmetic — `uint? += 1`
// near uint's upper range must NOT spuriously overflow (the operation is
// unsigned; a signed intrinsic would report a false overflow).
BEZEL_TEST(codegen_1964_nullable_compound, uint_plus_equals_no_false_overflow) {
    BEZEL_EXPECT_EQ(throwsOverflow("var u: uint? := 4000000000U; u += 1;"), 0);
}

// §7.11: `double?` compound (FP payload) — `10.0 += 1` promotes the int RHS to
// double and yields 11.0 (no overflow check on FP, §6.3).
BEZEL_TEST(codegen_1964_nullable_compound, double_plus_equals_present) {
    BEZEL_EXPECT_EQ(run("var d: double? := 10.0; d += 1; return int(d ?? 0.0);"),
                    11);
}

// §7.11: a `null` double? propagates null through the FP branch.
BEZEL_TEST(codegen_1964_nullable_compound, double_plus_equals_null_stays_null) {
    BEZEL_EXPECT_EQ(run("var d: double? := null; d += 1; return int(d ?? 7.0);"),
                    7);
}
