// Regression tests for #8576 — `Convert.ToInt64` SATURATED on an out-of-range
// value instead of raising the range failure.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §2.3 (`Convert`, v0.357):
//       "The `To*` methods raise `FormatException` when the string is not a
//        well-formed representation of the target type and
//        `ParseRangeException` (§9.1; an `OverflowException`) when the value
//        does not fit — the same contract as the §9 per-type `Parse` family
//        these conversions mirror; `Try` forms return `false` in exactly the
//        cases the throwing forms would raise; the `out` parameter receives
//        the default value."
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
//   `catch ParseRangeException` is "unresolved type name"), so the family
//   raises `OverflowException` — the same class `Convert.ToInt32` already
//   raises for the same condition.  #8417 retires the class in v0.358.
//
// THE DEFECT (issue #8576), MEASURED ON THE BRANCH TIP (r30-c1 @ f803d470d,
// before this fix):
//
//   Convert.ToInt64("9223372036854775808") -> 9223372036854775807
//                                               (no exception)
//
// `src/codegen/LlvmBackend_Calls.cpp`, `Emitter::tryLowerBuiltinClassCall`,
// the `ToInt64` arm: strtoll is called with an end-pointer check for the
// input grammar, but nothing reads `errno`, so the saturated value comes
// back as if it had been parsed.  `Convert.ToInt32` is already correct: its
// arm range-checks the converted value against the 32-bit bounds.
//
// §2.3 declares exactly two 64-bit-width members of the `Convert` family —
// `ToInt64` and `TryToInt64` (`Convert.ToUInt64` does NOT exist) — and §2.3
// binds them together: "Try forms return `false` in exactly the cases the
// throwing forms would raise".  Both are in scope here.
//
// THE FIX (`src/codegen/LlvmBackend_Calls.cpp`,
// `Emitter::tryLowerBuiltinClassCall`): the `ToInt64` and `TryToInt64` arms
// clear `errno` BEFORE the strtoll conversion and read it IMMEDIATELY after
// it, and report the RANGE FAILURE (ERANGE) — `ToInt64` raises
// `OverflowException`, `TryToInt64` returns false with the default in the
// `out` parameter.
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
// A. THE ISSUE'S OWN REPRO — `Convert.ToInt64` raises on an out-of-range
//    value (stdlib §2.3's range failure, v0.357).
// ===========================================================================

// Convert.ToInt64("9223372036854775808") — one past Int64.MaxValue.
//
// Pre-fix: the arm SATURATES (9223372036854775807) and raises nothing →
// the program returns 0.  Post-fix: the range failure raises
// `ParseRangeException` (an `OverflowException`) → the arm is taken → 1.
BEZEL_TEST(convert_to_int64_8576, to_int64_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := Convert.ToInt64(\"9223372036854775808\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// Convert.TryToInt64("9223372036854775808", out v) — false, and v = 0.
//
// stdlib §2.3: "`Try` forms return `false` in exactly the cases the
// throwing forms would raise; the `out` parameter receives the default
// value."
//
// Pre-fix: ok = true (and v = the saturated 9223372036854775807) → 0.
BEZEL_TEST(convert_to_int64_8576, try_to_int64_out_of_range_is_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v: long; "
        "var ok := Convert.TryToInt64(\"9223372036854775808\", out v); "
        "if ok then return 0; end; "
        "if v <> 0L then return 2; end; "
        "return 1; "
        )), 1);
}

// The NEGATIVE side of the range: -9223372036854775809 is one past
// Int64.MinValue.
//
// Pre-fix: SATURATES to -9223372036854775808 and raises nothing → 0.
BEZEL_TEST(convert_to_int64_8576, to_int64_below_min_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := Convert.ToInt64(\"-9223372036854775809\"); return 0; "
        "catch OverflowException => return 1; "
        "end; "
        )), 1);
}

// A magnitude far past the width (30 digits) — the same range failure.
//
// Pre-fix: SATURATES to 9223372036854775807, no raise → 0.
BEZEL_TEST(convert_to_int64_8576, to_int64_far_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  _ := Convert.ToInt64(\"999999999999999999999999999999\"); return 0; "
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
// making Convert.ToInt64("9223372036854775807") raise.  Int64.MaxValue is a
// LEGAL value of the width, which must convert to itself.
BEZEL_TEST(convert_to_int64_8576, to_int64_at_max_value_returns_the_value) {
    expectOut(prog(
        "Console.WriteLine(Convert.ToInt64(\"9223372036854775807\").ToString()); "
        "return 0; "
        ), "9223372036854775807\n");
}

// CONTROL.  Wrong fix caught: a one-sided range check that rejects the exact
// MINIMUM (a `<` for a `<=`, or a check that only looks at the upper bound),
// making Convert.ToInt64("-9223372036854775808") raise.  Int64.MinValue is a
// LEGAL value of the width.
BEZEL_TEST(convert_to_int64_8576, to_int64_at_min_value_returns_the_value) {
    expectOut(prog(
        "Console.WriteLine(Convert.ToInt64(\"-9223372036854775808\").ToString()); "
        "return 0; "
        ), "-9223372036854775808\n");
}
