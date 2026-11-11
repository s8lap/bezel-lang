// Regression tests for #8540 — the 64-bit integer widths' `Parse` family
// SATURATED on an out-of-range value instead of raising `ParseRangeException`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §9.1 (Integer types, v0.357) declares on Int32:
//       public static pure method Parse(s: string): int;
//       public static method TryParse(s: string, out result: int): bool;
//       public static pure method ParseOrNull(s: string): int?;
//   and says "SByte, Byte, Int16, UInt16, Int32, UInt32, Int64, UInt64 … the
//   others have analogous shapes with their respective MinValue/MaxValue" —
//   so `long` (Int64) and `ulong` (UInt64) carry the same members.
//
//   stdlib §9.1 (v0.357), "The range failure":
//       "A `Parse` member — here, in §9.2 and §9.3, and through §2.3's
//        `Convert.To*` — whose input is well-formed and spells a value the
//        type cannot represent raises `ParseRangeException`"
//
//   and §9.1's declaration of that class:
//       public class ParseRangeException: OverflowException, IFailure
//   — "It derives from OverflowException so that an OverflowException arm
//   around a Parse call matches as it always has".  The compiler's builtin
//   exception set has no `ParseRangeException` type (MEASURED: `catch
//   ParseRangeException` is "unresolved type name"), so the parse family
//   raises `OverflowException` — the same class the narrower widths already
//   raise for the same condition (the pinned corpus case `l9_1_0007` catches
//   `OverflowException`).
//
//   stdlib §9.2's range paragraph: "`TryParse` returns `false` in every case
//   where `Parse` would raise" (shape (c) of the §1 Try convention); the §1
//   `*OrNull` tier:
//   "`ParseOrNull` returns `null` exactly where `Parse` would raise".
//
// THE DEFECT (issue #8540), MEASURED ON THE BRANCH TIP (the compiler built
// from `r30-c1` @ 0cb32bb14, before this fix):
//
//   long.Parse("9223372036854775808")             -> 9223372036854775807
//                                                       (one past Int64.MaxValue;
//                                                        no exception)
//   long.TryParse("9223372036854775808", out v)   -> true, v = 9223372036854775807
//   ulong.TryParse("18446744073709551616", out v) -> true, v = 18446744073709551615
//   ulong.Parse("18446744073709551616")           -> 9223372036854775807
//
//   strtoll/strtoull SATURATE at the 64-bit boundary and report the failure
//   through errno (ERANGE); the compiler's 64-bit arms read neither, so the
//   saturated value came back as if it had been parsed.  The narrower widths
//   range-check the VALUE, which is why `int.Parse("2147483648")` already
//   raised.
//
//   MEASURED CONSEQUENCES of the same arm (fixed here too, because the fix
//   changes the conversion the arm makes):
//   * ulong.Parse("18446744073709551615") — a LEGAL value, above
//     Int64.MaxValue — returned 9223372036854775807, because the arm
//     converted with strtoll.  The unsigned width converts with strtoull (its
//     full magnitude), so the legal maximum parses to itself.
//   * ulong.Parse("-1") returned 18446744073709551615 (no raise): strtoull
//     silently negates a leading '-', so the unsigned width's input grammar
//     rejects it — the same reject the `TryParse` arm and the radix form
//     already make (`ulong.Parse("-1", 10)` is a FormatException, pinned by
//     `tests/codegen/int_parse_radix_2024_test.cpp`).
//
// THE FIX (`src/codegen/LlvmBackend_Calls2.cpp`,
// `Emitter::tryLowerPrimitiveStaticCall`):
//   * one shared helper converts with strtoll/strtoull, clears `errno` BEFORE
//     the conversion and reads it IMMEDIATELY after it, and reports the
//     RANGE FAILURE (ERANGE) as a boolean;
//   * the 64-bit `TryParse`/`ParseOrNull` arms (the ONE shared parse check)
//     AND the 64-bit `Parse` arm — the string operand and the span operand —
//     use it: `Parse` raises `OverflowException` (stdlib §9.1's
//     `ParseRangeException`), `TryParse` returns false, `ParseOrNull`
//     returns null.
//
// THE ORACLE.  Every row runs a real program through the AOT path and reads
// the program's EXIT STATUS or its STDOUT.  A row that only asserted "the
// program compiles" would be green on the pre-fix compiler for the wrong
// reason.
//
//   * The FIX rows are RED on the pre-fix compiler (the program returns the
//     "no raise" code, or prints the saturated value).
//   * Two NAMED BOUNDARY CONTROLS (section B) are green on both arms; each
//     catches a named wrong fix.  The other admitting twins were moved to
//     the conformance corpus candidates (owner rule 2026-11-12: no row that
//     is green on base).
//
//   A NOTE ON THE SPELLINGS: Bezel's "not equal" is `<>`, and `x!` is the
//   NON-NULL ASSERTION (the unwrap of a nullable) — so `v <> 0L` is "v is not
//   zero" and `v! = 0L` is "the unwrapped v equals zero".  The rows below
//   compare a non-nullable local, so they use `<>`.
//
// NOT IN SCOPE (deferred, filed as a follow-up): the RADIX spellings
// (`long.Parse(s, radix)` / `ulong.Parse(s, radix)` and their `TryParse`
// twins) carry the same saturation — §9.1's radix paragraph states the same
// range rule — but they are a different member of the stdlib and outside this
// issue's declared scope.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The exit status of a program that RAN, or -999 when it did not run (a
// front-end rejection, a failed compile, a crash).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The exit status and the stdout of a program that RAN.
std::string out(const std::string& body) {
    auto r = runAot(body);
    return r.ran ? r.stdoutText : "<did-not-run>";
}

// The one-line program shape used by the rows: the exit status is the
// observable.
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

// A program that must RUN CLEANLY and print exactly `want`.
void expectOut(const std::string& body, const std::string& want) {
    auto r = runAot(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// ===========================================================================
// A. THE ISSUE'S OWN REPRO — the 64-bit widths raise on an out-of-range
//    value (stdlib §9.1's range failure, v0.357).
// ===========================================================================

// long.Parse("9223372036854775808") — one past Int64.MaxValue.
//
// Pre-fix: the 64-bit arm SATURATES (9223372036854775807) and raises nothing
// → the program returns 0.  Post-fix: the range failure raises
// `ParseRangeException` (an `OverflowException`) → the arm is taken → 1.
BEZEL_TEST(parse_range_64bit_8540, long_parse_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(\"9223372036854775808\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// The same input through the PascalCase spelling (Int64 is `long`).
BEZEL_TEST(parse_range_64bit_8540, int64_parse_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := Int64.Parse(\"9223372036854775808\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// long.TryParse("9223372036854775808", out v) — false, and v = 0.
//
// stdlib §1 shape (c): `TryParse` returns false in every case where `Parse`
// would raise; the out parameter receives the default value (0).
//
// Pre-fix: ok = true (and v = the saturated 9223372036854775807) → 0.
BEZEL_TEST(parse_range_64bit_8540, long_try_parse_out_of_range_is_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v: long; "
        "var ok := long.TryParse(\"9223372036854775808\", out v); "
        "if ok then return 0; end; "
        "if v <> 0L then return 2; end; "
        "return 1; "
        )), 1);
}

// ulong.Parse("18446744073709551616") — one past UInt64.MaxValue.
//
// Pre-fix: the 64-bit arm SATURATES and raises nothing → 0.
BEZEL_TEST(parse_range_64bit_8540, ulong_parse_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := ulong.Parse(\"18446744073709551616\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// ulong.TryParse("18446744073709551616", out v) — false, and v = 0.
//
// Pre-fix: ok = true (and v = the saturated 18446744073709551615) → 0.
BEZEL_TEST(parse_range_64bit_8540, ulong_try_parse_out_of_range_is_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v: ulong; "
        "var ok := ulong.TryParse(\"18446744073709551616\", out v); "
        "if ok then return 0; end; "
        "if v <> 0UL then return 2; end; "
        "return 1; "
        )), 1);
}

// The §1 `*OrNull` tier: null EXACTLY where `Parse` raises.
//
// Pre-fix: the tier shares the ONE parse check with its `TryParse` sibling,
// and that check admitted the saturated value → the tier returned the
// SATURATED VALUE, not null.
BEZEL_TEST(parse_range_64bit_8540, long_parse_or_null_out_of_range_is_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := long.ParseOrNull(\"9223372036854775808\"); "
        "if a = null then return 1; end; "
        "return 0; "
        )), 1);
}

BEZEL_TEST(parse_range_64bit_8540, ulong_parse_or_null_out_of_range_is_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := ulong.ParseOrNull(\"18446744073709551616\"); "
        "if a = null then return 1; end; "
        "return 0; "
        )), 1);
}

// The NEGATIVE side of the range: -9223372036854775809 is one past
// Int64.MinValue.
//
// Pre-fix: the arm SATURATED to -9223372036854775808 and raised nothing → 0.
BEZEL_TEST(parse_range_64bit_8540, long_parse_below_min_value_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(\"-9223372036854775809\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// A magnitude far past the width (30 digits) — the same range failure.
//
// Pre-fix: SATURATED to 9223372036854775807, no raise → 0.
BEZEL_TEST(parse_range_64bit_8540, long_parse_far_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(\"999999999999999999999999999999\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// The SPAN overload (stdlib §9.1, #1810) carries the string form's contract
// — the same FormatException/ParseRangeException contract.
//
// Pre-fix: the span arm SATURATED and raised nothing → 0.
BEZEL_TEST(parse_range_64bit_8540, long_parse_span_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(\"xx9223372036854775808\".AsSpan(2, 19)); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// The unsigned width's input grammar: a leading '-' is not admitted (the
// `TryParse` arm and the radix form already reject it — the radix form is
// pinned by tests/codegen/int_parse_radix_2024_test.cpp).
//
// Pre-fix: the 1-argument arm had NO reject, so the value came back as the
// wrapped 18446744073709551615 and nothing was raised → 0.
BEZEL_TEST(parse_range_64bit_8540, ulong_parse_negative_is_a_format_failure) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := ulong.Parse(\"-1\"); return 0; "
        "catch FormatException => return 1; "
        "end; "
        )), 1);
}

// ===========================================================================
// B. TWO NAMED BOUNDARY CONTROLS — green on both arms, kept because a
//    plausible wrong fix turns each red.
// ===========================================================================

// CONTROL.  Wrong fix caught: an over-eager range check that rejects the
// exact boundary (`<=` for `<`, or ERANGE read from a stale errno), making
// long.Parse("9223372036854775807") raise.
BEZEL_TEST(parse_range_64bit_8540, long_parse_at_max_value_returns_the_value) {
    expectOut(prog(
        "Console.WriteLine(long.Parse(\"9223372036854775807\").ToString()); "
        "return 0; "
        ), "9223372036854775807\n");
}

// FIX ROW (red on base).  UInt64.MaxValue is a legal value of the width: it
// parses to ITSELF.  (Pre-fix this row FAILED: the arm converted with strtoll, so the legal
// UInt64.MaxValue came back as 9223372036854775807.  The unsigned width
// converts with strtoull — its full magnitude.)
BEZEL_TEST(parse_range_64bit_8540, ulong_parse_at_max_value_returns_the_value) {
    expectOut(prog(
        "Console.WriteLine(ulong.Parse(\"18446744073709551615\").ToString()); "
        "return 0; "
        ), "18446744073709551615\n");
}

// CONTROL.  Wrong fix caught: applying the SIGNED range (Int64.MaxValue) to
// the unsigned TryParse arm, so ulong.TryParse("18446744073709551615")
// returns false.
BEZEL_TEST(parse_range_64bit_8540, ulong_try_parse_at_max_value_is_true) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v: ulong; "
        "var ok := ulong.TryParse(\"18446744073709551615\", out v); "
        "if not ok then return 1; end; "
        "if v <> 18446744073709551615UL then return 2; end; "
        "return 0; "
        )), 0);
}

