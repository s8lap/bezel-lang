// Regression tests for #8575 — the RADIX spellings of the 64-bit widths'
// `Parse` family SATURATED on an out-of-range value instead of raising the
// range failure.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §9.1 (Integer types, v0.357), "The radix alphabet":
//       "`Parse(s, radix)` and `TryParse(s, radix, …)` accept **either
//        case** of the letter digits … under the input grammar the string
//        forms carry throughout: an optional leading `+` or `-`, then one or
//        more digits of that radix, and nothing else; no whitespace, no
//        `0x`/`0b` prefix, no digit separators, and any other input raises
//        `FormatException`, a value outside the type's range raising
//        `ParseRangeException`"
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
//   exception set has no `ParseRangeException` type (MEASURED in #8540:
//   `catch ParseRangeException` is "unresolved type name"), so the parse
//   family raises `OverflowException` — the same class the narrower widths
//   already raise for the same condition (pinned by
//   tests/codegen/int_parse_radix_2024_test.cpp: `int.Parse("80000000", 16)`
//   raises OverflowException).  #8417 retires the class in v0.358.
//
//   stdlib §9.1 declares `TryParse` as shape (c) of the Try convention: it returns `false` in
//   every case where `Parse` would raise, and the `out` parameter receives
//   the default value.
//
// THE DEFECT (issue #8575), MEASURED ON THE BRANCH TIP (r30-c1 @ f803d470d,
// before this fix): the radix arms of `long`/`ulong` SATURATED on an
// out-of-range value exactly like the 1-argument forms did (which #8540
// fixed — 8fce41165, in the base).  strtoll/strtoull saturate at the 64-bit
// boundary and report the failure through errno (ERANGE); the radix arms
// read neither.
//
//   The radix `Parse` arm carried the comment "long/ulong are the full
//   strtoll/strtoull width — no range check" and returned the converted
//   value unchecked; the 3-argument radix `TryParse` arm had the same gap.
//
// THE FIX (`src/codegen/LlvmBackend_Calls2.cpp`,
// `Emitter::tryLowerPrimitiveStaticCall`): the radix arms reuse #8540's
// `emitI64TowerParse` helper — it clears `errno` BEFORE the strtoll/strtoull
// conversion and reads it IMMEDIATELY after it, and reports the RANGE
// FAILURE (ERANGE) as a boolean.  `Parse` raises `OverflowException`;
// `TryParse` returns false with the default in the `out` parameter.
//
// THE ORACLE.  Every row runs a real program through the AOT path and reads
// the program's EXIT STATUS.  A row that only asserted "the program compiles"
// would be green on the pre-fix compiler for the wrong reason.
//
//   * The FIX rows are RED on the pre-fix compiler (the program returns the
//     "no raise" code).
//   * TWO NAMED BOUNDARY CONTROLS (section B) are green on both arms; each
//     catches a named wrong fix.  The other admitting twins are in the
//     conformance-corpus candidates (owner rule 2026-11-12: no row that is
//     green on base).
//
//   A NOTE ON THE SPELLINGS: Bezel's "not equal" is `<>`, and `x!` is the
//   NON-NULL ASSERTION (the unwrap of a nullable) — so `v <> 0L` is "v is not
//   zero" and `v! = 0L` is "the unwrapped v equals zero".  The rows below
//   compare a non-nullable local, so they use `<>`.

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
// A. THE ISSUE'S OWN REPRO — the radix spellings raise on an out-of-range
//    value (stdlib §9.1's range failure, v0.357).
// ===========================================================================

// long.Parse("9223372036854775808", 10) — one past Int64.MaxValue.
//
// Pre-fix: the radix arm SATURATES (9223372036854775807) and raises nothing
// → the program returns 0.  Post-fix: the range failure raises
// `ParseRangeException` (an `OverflowException`) → the arm is taken → 1.
BEZEL_TEST(parse_radix_range_8575, long_parse_radix_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(\"9223372036854775808\", 10); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// ulong.Parse("18446744073709551616", 10) — one past UInt64.MaxValue.
//
// Pre-fix: the radix arm SATURATES and raises nothing → 0.
BEZEL_TEST(parse_radix_range_8575, ulong_parse_radix_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := ulong.Parse(\"18446744073709551616\", 10); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// long.TryParse("9223372036854775808", 10, out v) — false, and v = 0.
//
// stdlib §1 shape (c): `TryParse` returns false in every case where `Parse`
// would raise; the out parameter receives the default value (0).
//
// Pre-fix: ok = true (and v = the saturated 9223372036854775807) → 0.
BEZEL_TEST(parse_radix_range_8575, long_try_parse_radix_out_of_range_is_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v: long; "
        "var ok := long.TryParse(\"9223372036854775808\", 10, out v); "
        "if ok then return 0; end; "
        "if v <> 0L then return 2; end; "
        "return 1; "
        )), 1);
}

// ulong.TryParse("18446744073709551616", 10, out v) — false, and v = 0.
//
// Pre-fix: ok = true (and v = the saturated 18446744073709551615) → 0.
BEZEL_TEST(parse_radix_range_8575, ulong_try_parse_radix_out_of_range_is_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v: ulong; "
        "var ok := ulong.TryParse(\"18446744073709551616\", 10, out v); "
        "if ok then return 0; end; "
        "if v <> 0UL then return 2; end; "
        "return 1; "
        )), 1);
}

// The HEX radix, the other admitted radix: "8000000000000000" base 16 is
// 2^63 — one past Int64.MaxValue.
//
// Pre-fix: SATURATES to 9223372036854775807, no raise → 0.
BEZEL_TEST(parse_radix_range_8575, long_parse_radix_hex_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(\"8000000000000000\", 16); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// The unsigned width's hex form: "10000000000000000" base 16 is 2^64 — one
// past UInt64.MaxValue.
//
// Pre-fix: SATURATES to 18446744073709551615, no raise → 0.
BEZEL_TEST(parse_radix_range_8575, ulong_parse_radix_hex_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := ulong.Parse(\"10000000000000000\", 16); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// The NEGATIVE side of the range: -9223372036854775809 is one past
// Int64.MinValue.
//
// Pre-fix: SATURATES to -9223372036854775808 and raises nothing → 0.
BEZEL_TEST(parse_radix_range_8575, long_parse_radix_below_min_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(\"-9223372036854775809\", 10); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// The PascalCase spelling (Int64 is `long`).
//
// Pre-fix: SATURATES and raises nothing → 0.
BEZEL_TEST(parse_radix_range_8575, int64_parse_radix_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := Int64.Parse(\"9223372036854775808\", 10); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// The hex radix through the Try form.
//
// Pre-fix: ok = true (v = the saturated value) → 0.
BEZEL_TEST(parse_radix_range_8575, long_try_parse_radix_hex_out_of_range_is_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v: long; "
        "var ok := long.TryParse(\"8000000000000000\", 16, out v); "
        "if ok then return 0; end; "
        "if v <> 0L then return 2; end; "
        "return 1; "
        )), 1);
}

// The NAMED-ARGUMENT binding of §22.5 (v0.357): "Named arguments may appear
// in any order after positional arguments" — so `radix` may be bound to the
// FIRST argument and `s` to the second.  The arm reads the parameter's
// SOURCE argument (fix #7602), so the range rule must hold in this order
// too.
//
// Pre-fix: SATURATES and raises nothing → 0.
BEZEL_TEST(parse_radix_range_8575, long_parse_radix_named_args_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := long.Parse(radix := 16, s := \"8000000000000000\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// ===========================================================================
// B. TWO NAMED BOUNDARY CONTROLS — green on both arms, kept because a
//    plausible wrong fix turns each red.
// ===========================================================================

// CONTROL.  Wrong fix caught: an over-eager range check that rejects the
// exact boundary (a `<=` for a `<`, or an ERANGE read from a stale errno),
// making long.Parse("7FFFFFFFFFFFFFFF", 16) raise.  0x7FFFFFFFFFFFFFFF is
// Int64.MaxValue — a LEGAL value of the width, which must parse to itself.
BEZEL_TEST(parse_radix_range_8575, long_parse_radix_hex_at_max_value_returns_the_value) {
    expectOut(prog(
        "Console.WriteLine(long.Parse(\"7FFFFFFFFFFFFFFF\", 16).ToString()); "
        "return 0; "
        ), "9223372036854775807\n");
}

// CONTROL.  Wrong fix caught: applying the SIGNED range (Int64.MaxValue) to
// the unsigned width, so ulong.Parse("FFFFFFFFFFFFFFFF", 16) raises.
// 0xFFFFFFFFFFFFFFFF is UInt64.MaxValue — a LEGAL value of the width.
BEZEL_TEST(parse_radix_range_8575, ulong_parse_radix_hex_at_max_value_returns_the_value) {
    expectOut(prog(
        "Console.WriteLine(ulong.Parse(\"FFFFFFFFFFFFFFFF\", 16).ToString()); "
        "return 0; "
        ), "18446744073709551615\n");
}
