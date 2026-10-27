// Regression tests for #7242 — an UNSIGNED operand at a `Math` built-in's
// `double` parameter was SIGN-EXTENDED, so the VALUE was destroyed.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §5.1 "Implicit conversions" (v0.344) lists the widening table:
//
//   | `byte`   | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//   | `ushort` | `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//   | `uint`   | `long`, `ulong`, `float`, `double`, `decimal` |
//   | `ulong`  | `float`, `double`, `decimal` |
//   | `char`   | `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//
// So an UNSIGNED operand IS admitted at a `double` parameter. §5.1 is a
// conversion table of VALUES: an integer conversion that fits the target
// carries the source's numeric value across. `uint` 4000000000 is a
// POSITIVE number; a conversion that yields -294967296 has not converted the
// value, it has converted the 32-bit BIT PATTERN.
//
// stdlib §2.2 "Math" (v0.344) declares the `double` forms
// `Sqrt(x: double): double`, `Pow(x: double, y: double): double`,
// `Floor`/`Ceiling`/`Truncate(x: double): double`,
// `Round(x: double, digits: int := 0): double` and
// `Fmod`/`IEEERemainder(x: double, y: double): double`. §22.6's
// applicability stage (v0.344) reads the parameter types "under the implicit
// conversions (§5.1)", so an unsigned argument reaches a `double` parameter
// and converts there.
//
// WHAT THE DEFECT WAS.
//
// The `double` lowerings converted an integer operand with `sitofp`, which
// SIGN-EXTENDS. The `uint` 4000000000 (bit pattern 0xEE6B2800) was read as
// the `int32` -294967296, so:
//
//   Math.Sqrt(4000000000u)     printed -nan        (sqrt of a negative)
//   Math.Pow(4000000000u, 2.0) printed 87005705709551620
//
// while the `long` spelling of the same number printed 63245.553203367585 and
// 1.6e+19. The `uint` and the `long` are the SAME number and must produce the
// SAME result.
//
// The two converters in the same file DISAGREED about unsignedness: the
// explicit-type-list converter (`mathConvertToT`) already emitted `uitofp`,
// while the `double`-parameter converter emitted `sitofp`. Both now select
// the instruction from the SAME predicate (`isUnsignedTargetExpr`).
//
// WHAT THE FIX DOES.
//
// The integer-to-floating conversion is SIGNEDNESS-AWARE: an unsigned source
// uses `uitofp` (which preserves the VALUE), a signed source keeps the
// `sitofp` it always had (so a negative signed operand keeps its sign and
// every signed result is bit-for-bit what it was).
//
// THE MATRIX.
//
// The int/long/uint/ulong matrix below pins BOTH directions at once: the
// unsigned rows are the fix, and the signed rows in the SAME matrix are
// green on both arms — a signed operand must still convert exactly as it
// did before.
//
// #7289 (stdlib §2.2, v0.344; §22.6, v0.344) — A LATER RULE CHANGED THE
// SPELLING OF THE `Floor`/`Ceiling`/`Truncate` CALLS IN THIS FILE, NOT THEIR
// EXPECTED VALUES. `Floor`/`Ceiling`/`Truncate` each declare a `double` form
// AND a `decimal` form, so an INTEGER argument converts to both and matches
// neither exactly: §22.6's equal-ranking rule makes the call ambiguous, and
// the v0.228 changelog entry records exactly that ("Newly rejected: a
// `Math.Floor`/`Ceiling`/`Truncate` call whose argument is an integer type
// or `char` ... the remedy is an explicit conversion (`Math.Floor(double(n))`)").
// The four affected rows below therefore spell those calls with the explicit
// conversion, and their EXPECTED VALUES ARE UNCHANGED — the same numbers are
// produced. `Round`, `Sqrt`, `Pow`, `Fmod` and `IEEERemainder` declare a
// SINGLE parameter type, so they keep the implicit spelling.
//
// THE PROOF HIERARCHY.
//
// Every accepting row below FAILS pre-fix: the program either printed a
// wrong value or raised, so it never produced the value §5.1's widening
// implies. Each row therefore pins the VALUE, not merely "it compiled". The
// control rows are green on both arms and show this change is a widening of
// the unsigned path and not a change to the signed one.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The accepting shape: the program RAN, it produced exactly this text, and
// it did not fail the compiler.
void expectValue(const std::string& body, const std::string& expected) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    // The value channel: the program's stdout with ONE trailing line-ending
    // stripped (the same rule `runAotOut` applies), so the expectation is
    // platform-independent.
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, expected);
}

// The refusing shape: the program did not run, the driver rejected it at a
// REAL source location (never a crash / signal / timeout).
void expectRefused(const std::string& body, const char* overload,
                   const char* expectedType) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, overload));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, expectedType));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — the value §5.1's widening implies.
// ===========================================================================

// `Sqrt(x: double): double` (stdlib §2.2). The `uint` 4000000000 converts to
// the `double` 4000000000.0, whose square root is 63245.553203367585 — the
// very value the `long` spelling produces.
BEZEL_TEST(math_uint_double_7242, math_sqrt_uint_is_value_preserving_5_1) {
    expectValue("    Console.WriteLine(Math.Sqrt(4000000000u));",
                "63245.553203367585");
}

// THE CONTROL: the `long` spelling of the same number. §5.1 admits `long` at
// `double` too, and its conversion is signed — the value is the same, and the
// fix must not disturb it.
BEZEL_TEST(math_uint_double_7242, math_sqrt_long_is_unchanged_5_1) {
    expectValue("    Console.WriteLine(Math.Sqrt(4000000000L));",
                "63245.553203367585");
}

// `Pow(x: double, y: double): double` (stdlib §2.2). 4000000000^2 is
// 1.6e+19; the pre-fix `sitofp` computed (-294967296)^2 = 87005705709551620.
BEZEL_TEST(math_uint_double_7242, math_pow_uint_is_value_preserving_5_1) {
    expectValue("    Console.WriteLine(Math.Pow(4000000000u, 2.0));", "1.6e+19");
}

// The same `Pow` with a `long` base — the control.
BEZEL_TEST(math_uint_double_7242, math_pow_long_is_unchanged_5_1) {
    expectValue("    Console.WriteLine(Math.Pow(4000000000L, 2.0));", "1.6e+19");
}

// ===========================================================================
// THE int / long / uint / ulong MATRIX.
//
// The unsigned rows are the fix; the signed rows in the SAME matrix are the
// control that the signed path is unchanged.
// ===========================================================================

// The matrix at `Sqrt`. §5.1 admits `int`, `long`, `uint` and `ulong` at a
// `double` parameter; all four must carry their VALUE across. The `uint` and
// `ulong` rows are the ones the pre-fix `sitofp` destroyed.
BEZEL_TEST(math_uint_double_7242, math_sqrt_int_long_uint_ulong_matrix_5_1) {
    expectValue("    var i: int := 16; "
                "    var l: long := 16L; "
                "    var u: uint := 4000000000u; "
                "    var ul: ulong := 4000000000UL; "
                "    Console.WriteLine(Math.Sqrt(i)); "
                "    Console.WriteLine(Math.Sqrt(l)); "
                "    Console.WriteLine(Math.Sqrt(u)); "
                "    Console.WriteLine(Math.Sqrt(ul));",
                "4\n4\n63245.553203367585\n63245.553203367585");
}

// The matrix at `Pow`. The `uint`/`ulong` bases must reach 1.6e+19, and the
// `int`/`long` bases must be unchanged.
BEZEL_TEST(math_uint_double_7242, math_pow_int_long_uint_ulong_matrix_5_1) {
    expectValue("    var i: int := 2; "
                "    var l: long := 2L; "
                "    var u: uint := 4000000000u; "
                "    var ul: ulong := 4000000000UL; "
                "    Console.WriteLine(Math.Pow(i, 10)); "
                "    Console.WriteLine(Math.Pow(l, 10)); "
                "    Console.WriteLine(Math.Pow(u, 2.0)); "
                "    Console.WriteLine(Math.Pow(ul, 2.0));",
                "1024\n1024\n1.6e+19\n1.6e+19");
}

// The matrix at `Floor` — the rounding family (`Floor`/`Ceiling`/`Round`/
// `Truncate`, stdlib §2.2's `double` forms). The unsigned rows must report
// the number itself; the signed rows must be unchanged.
//
// #7289 (stdlib §2.2, v0.344; §22.6, v0.344): `Floor` declares a `double`
// form AND a `decimal` form, so an INTEGER argument converts to both and
// matches neither exactly — the §22.6 ambiguity rule, which the v0.228
// changelog entry records as "Newly rejected: a `Math.Floor`/`Ceiling`/
// `Truncate` call whose argument is an integer type or `char`". The
// argument is therefore spelled with the EXPLICIT CONVERSION that record
// names as the remedy ("`Math.Floor(double(n))`"), which is the same value
// the implicit call produced: the EXPECTED VALUES below are unchanged.
BEZEL_TEST(math_uint_double_7242, math_floor_int_long_uint_ulong_matrix_5_1) {
    expectValue("    var i: int := 16; "
                "    var l: long := 16L; "
                "    var u: uint := 4000000000u; "
                "    var ul: ulong := 4000000000UL; "
                "    Console.WriteLine(Math.Floor(double(i))); "
                "    Console.WriteLine(Math.Floor(double(l))); "
                "    Console.WriteLine(Math.Floor(double(u))); "
                "    Console.WriteLine(Math.Floor(double(ul)));",
                "16\n16\n4e+09\n4e+09");
}

// The matrix at `Fmod` (stdlib §2.2's `Fmod(x: double, y: double)`). Both
// operands convert at the call site; fmod(4000000000, 3) is 1.
BEZEL_TEST(math_uint_double_7242, math_fmod_int_long_uint_ulong_matrix_5_1) {
    expectValue("    var i: int := 10; "
                "    var l: long := 10L; "
                "    var u: uint := 4000000000u; "
                "    var ul: ulong := 4000000000UL; "
                "    Console.WriteLine(Math.Fmod(i, 3)); "
                "    Console.WriteLine(Math.Fmod(l, 3)); "
                "    Console.WriteLine(Math.Fmod(u, 3u)); "
                "    Console.WriteLine(Math.Fmod(ul, 3UL));",
                "1\n1\n1\n1");
}

// ===========================================================================
// THE REST OF THE `double`-PARAMETER FAMILY, at an unsigned argument.
// ===========================================================================

// The rounding family at a `uint` argument (stdlib §2.2's `double` forms).
//
// #7289 (stdlib §2.2, v0.344; §22.6, v0.344): `Floor`/`Ceiling`/`Truncate`
// each declare a `double` form AND a `decimal` form, so an integer argument
// ties them (§22.6's equal-ranking rule) and is refused as ambiguous — the
// v0.228 changelog entry's class: "an integer type or `char`". The three
// affected calls are spelled with the EXPLICIT CONVERSION that record names
// as the remedy; the EXPECTED VALUES below are unchanged. `Round` declares a
// SINGLE form (stdlib §2.2: "Round(decimal) is the instance Decimal.Round"),
// so it is NOT in the class and its two calls keep the implicit spelling.
BEZEL_TEST(math_uint_double_7242, math_rounding_family_uint_widens_5_1) {
    expectValue("    Console.WriteLine(Math.Floor(double(4000000000u))); "
                "    Console.WriteLine(Math.Ceiling(double(4000000000u))); "
                "    Console.WriteLine(Math.Truncate(double(4000000000u))); "
                "    Console.WriteLine(Math.Round(4000000000u)); "
                "    Console.WriteLine(Math.Round(4000000000u, 2));",
                "4e+09\n4e+09\n4e+09\n4e+09\n4e+09");
}

// `IEEERemainder(x: double, y: double): double` (stdlib §2.2) — the sibling
// of `Fmod`.
BEZEL_TEST(math_uint_double_7242, math_ieeeremainder_uint_widens_5_1) {
    expectValue("    Console.WriteLine(Math.IEEERemainder(4000000000u, 3u));", "1");
}

// §5.1's table admits `byte` and `ushort` at `double` too. A `byte` 200
// (0xC8) sign-extends to -56, and a `ushort` 60000 (0xEA60) to -5536, so
// the pre-fix `sitofp` produced sqrt of a NEGATIVE number for both.
//
// #7289 (stdlib §2.2, v0.344; §22.6, v0.344): the `Floor` call is spelled
// with the EXPLICIT CONVERSION the v0.228 changelog entry names as the
// remedy for the `double`/`decimal` tie; the EXPECTED VALUE below is
// unchanged.
BEZEL_TEST(math_uint_double_7242, math_byte_and_ushort_widen_5_1) {
    expectValue("    var b: byte := 200; "
                "    var us: ushort := 60000; "
                "    Console.WriteLine(Math.Sqrt(b)); "
                "    Console.WriteLine(Math.Sqrt(us)); "
                "    Console.WriteLine(Math.Floor(double(b)));",
                "14.142135623730951\n244.94897427831782\n200");
}

// A NON-CONSTANT unsigned operand — the conversion happens at run time, not
// at a constant fold.
BEZEL_TEST(math_uint_double_7242, math_uint_variable_operand_widens_5_1) {
    expectValue("    var u: uint := 4000000000u; "
                "    Console.WriteLine(Math.Sqrt(u));",
                "63245.553203367585");
}

// A `ulong` whose value does NOT fit in an `int64`. This is where the
// sign-extension destroyed the value outright: the `ulong`
// 18446744073709551615 has the `int64` bit pattern -1, so the pre-fix
// `sitofp` read it as -1.0 — the square root was of a NEGATIVE number and
// the power was of -1.
BEZEL_TEST(math_uint_double_7242, math_ulong_beyond_int64_widens_5_1) {
    // #7289 (stdlib §2.2, v0.344; §22.6, v0.344): the `Floor` call is
    // spelled with the EXPLICIT CONVERSION the v0.228 changelog entry names
    // as the remedy for the `double`/`decimal` tie; the EXPECTED VALUE below
    // is unchanged — the `ulong` still reaches the `double` with its VALUE
    // intact, which is what this row is about.
    expectValue("    var big: ulong := 18446744073709551615UL; "
                "    Console.WriteLine(Math.Sqrt(big)); "
                "    Console.WriteLine(Math.Floor(double(big))); "
                "    Console.WriteLine(Math.Pow(big, 2.0));",
                "4294967296\n18446744073709552000\n3.402823669209385e+38");
}

// ===========================================================================
// THE SAME-FILE SIBLINGS — a `double` parameter in stdlib §2.5's
// `TimeSpan`/`DateTime`, reached from an unsigned operand by the same
// `sitofp`.
// ===========================================================================

// stdlib §2.5 declares `TimeSpan.FromSeconds(seconds: double)` (and the
// `FromDays`/`FromHours`/`FromMinutes`/`FromMilliseconds` siblings). §5.1
// admits an unsigned operand at a `double` parameter, so the same
// sign-extending conversion applied here: `FromSeconds(4000000000u)` built a
// NEGATIVE span.
BEZEL_TEST(math_uint_double_7242, timespan_from_seconds_uint_widens_2_5) {
    expectValue("    Console.WriteLine(TimeSpan.FromSeconds(4000000000u).TotalSeconds); "
                "    Console.WriteLine(TimeSpan.FromSeconds(4000000000L).TotalSeconds); "
                "    Console.WriteLine(TimeSpan.FromMinutes(4000000000u).TotalMinutes);",
                "4e+09\n4e+09\n4e+09");
}

// stdlib §2.5 declares `DateTime.AddSeconds(seconds: double)` (and the
// `AddDays`/`AddHours`/`AddMinutes`/`AddMilliseconds` siblings). The
// `uint` and the `long` spelling of the same number must move the clock by
// the same amount.
BEZEL_TEST(math_uint_double_7242, datetime_add_seconds_uint_widens_2_5) {
    expectValue("    var d: DateTime := DateTime.FromTicks(0L); "
                "    Console.WriteLine(d.AddSeconds(4000000000u).ToUnixTimeSeconds()); "
                "    Console.WriteLine(d.AddSeconds(4000000000L).ToUnixTimeSeconds());",
                "-58135596800\n-58135596800");
}

// ===========================================================================
// THE CONTROLS — the conforming spellings are unaffected (green on both arms).
// ===========================================================================

// The `double` spellings themselves.
BEZEL_TEST(math_uint_double_7242, math_double_arguments_still_work_2_2) {
    expectValue("    Console.WriteLine(Math.Pow(2.0, 10.0)); "
                "    Console.WriteLine(Math.Sqrt(16.0)); "
                "    Console.WriteLine(Math.Round(3.14159, 2));",
                "1024\n4\n3.14");
}

// A NEGATIVE signed operand keeps its sign: the signed path is untouched, so
// the `sitofp` it always had still applies.
BEZEL_TEST(math_uint_double_7242, math_signed_negative_operand_unchanged_5_1) {
    expectValue("    Console.WriteLine(Math.Floor(-2.5)); "
                "    Console.WriteLine(Math.Ceiling(-2.5)); "
                "    Console.WriteLine(Math.Truncate(-2.5)); "
                "    Console.WriteLine(Math.Fmod(-10, 3));",
                "-3\n-2\n-2\n-1");
}

// The EXPLICIT type list already widened an unsigned operand correctly (the
// `mathConvertToT` converter emitted `uitofp`); the fix must not disturb it.
BEZEL_TEST(math_uint_double_7242, math_explicit_type_list_uint_unchanged_2_2) {
    expectValue("    Console.WriteLine(Math.Max<double>(4000000000u, 1.0)); "
                "    Console.WriteLine(Math.Min<double>(4000000000u, 1.0)); "
                "    Console.WriteLine(Math.Clamp<double>(4000000000u, 1.5, 2.5));",
                "4e+09\n1\n2.5");
}

// The GENERIC family at an unsigned operand is untouched: `Min`/`Max`/`Clamp`
// declare a type parameter, so an unsigned operand is its own `T` and the
// comparison runs at the operand's own integer type.
BEZEL_TEST(math_uint_double_7242, math_generic_minmax_uint_unchanged_2_2) {
    expectValue("    Console.WriteLine(Math.Max(4000000000u, 3u)); "
                "    Console.WriteLine(Math.Min(4000000000u, 3u)); "
                "    Console.WriteLine(Math.Clamp(4000000000u, 3u, 4000000001u));",
                "4000000000\n3\n4000000000");
}

// The `uint` → `decimal` conversion (§5.1's table admits it) is untouched:
// the decimal converter takes the source's own primitive, so the value
// already survived there.
BEZEL_TEST(math_uint_double_7242, math_uint_to_decimal_unchanged_5_1) {
    expectValue("    Console.WriteLine(Math.Max<decimal>(4000000000u, 3m));",
                "4000000000");
}

// The EXPLICIT cast `double(x)` is untouched: it already converted an
// unsigned operand with the value-preserving instruction.
BEZEL_TEST(math_uint_double_7242, double_cast_of_uint_unchanged_5_2) {
    expectValue("    Console.WriteLine(double(4000000000u));", "4e+09");
}

// ===========================================================================
// THE CONTROL IN THE OTHER DIRECTION — the `decimal` spellings stay REFUSED.
// ===========================================================================

// §5.1: "Between `decimal` and binary floats (`float`, `double`)" is never
// implicit, and `Math.Pow` declares `x: double` (stdlib §2.2). The fix
// widened the UNSIGNED path; it must not have widened a `decimal` into a
// `double` parameter.
BEZEL_TEST(math_uint_double_7242, math_pow_decimal_stays_refused_5_1) {
    expectRefused("    Console.WriteLine(Math.Pow(1.0m, 2.0));",
                  "no matching overload of `Math.Pow`",
                  "the `x` argument must be a `double`");
}

BEZEL_TEST(math_uint_double_7242, math_sqrt_decimal_stays_refused_5_1) {
    expectRefused("    Console.WriteLine(Math.Sqrt(2.0m));",
                  "no matching overload of `Math.Sqrt`",
                  "the `x` argument must be a `double`");
}
