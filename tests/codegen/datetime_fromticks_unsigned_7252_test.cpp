// Regression tests for #7252 (spec §5.1, v0.345): an UNSIGNED operand at
// `DateTime.FromTicks(ticks: long)`'s `long` parameter arrives as the
// SIGN-EXTENDED `int32` -294967296 — and, in the state the issue measured,
// as an i32 where an i64 was expected, which ABORTED the compiler with
// "IR verification failed (malformed IR emitted)".
//
// THE GOVERNING TEXT.
//
// Language spec §5.1 (docs/Bezel_Language_Spec.md:632, v0.345) — the
// implicit-conversion table:
//
//   | `uint` | `long`, `ulong`, `float`, `double`, `decimal` |
//
// and the same section's "never implicit" list (docs/Bezel_Language_Spec.md:
// 642):
//
//   Between signed and unsigned types of the same or narrower width (e.g.
//   `int → uint`, `uint → int`, `short → ushort`). Widening from unsigned to a
//   strictly larger signed type (e.g. `byte → short`, `ushort → int`) is
//   permitted because no value is lost.
//
// => §5.1 converts VALUES, not bit patterns: the `uint` 4000000000 IS
// 4000000000, not the `int32` -294967296 its low 32 bits read as, and the
// widening that preserves a value is `zext` (zero-extending), not `sext`
// (sign-extending).
//
// Standard library §2.5 (docs/Bezel_Standard_Library.md, v0.345) declares:
//
//   public static method FromTicks(ticks: long): DateTime
//   public static method FromUnixTimeSeconds(seconds: long): DateTime
//
// => both parameters are declared `long`, so an unsigned argument is
// CONVERTED at the call, under §5.1's table.
//
// THE DEFECT. The `DateTime.FromTicks` lowering returned the argument's own
// value AT THE ARGUMENT'S OWN WIDTH — so a `uint` reached a `long` consumer
// as an i32, and the verifier rejected the IR.
//
// THE FIX. The widening is now SIGNEDNESS-AWARE: an unsigned source
// zero-extends, a signed source keeps the `sext` it always had, so a
// negative `ticks` keeps its sign. The signedness is read with
// `isUnsignedTargetExpr` — the house predicate the #7242 `double`-parameter
// converter and the #7253 `Step` widening use — and NOT from the bit width.
//
// THE THIRD SITE. `DateTime.FromUnixTimeSeconds(seconds: long)` is the third
// tick-carrying conversion of this shape in the same family, and it had the
// same unconditional `sext`. It is fixed with the same change; it carries no
// issue of its own.
//
// THE ORACLE. `runAot` — the real AOT path. A wrong value's oracle is the
// program's exact stdout, never the exit code; and a compiler abort's oracle
// is that the program compiles and runs. Every row pins the printed text, and
// every row also asserts that the compiler did not abort.
//
// NOT `runAotChecked`: the ASan oracle moves `check_toolchain_skip_surface`'s
// `checked_calls` denominator, which is a coordinator-owned ratchet.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

using bezel::test::diagnosticMessageContains;

// A program that RUNS (the compiler did not abort) and prints exactly
// `expected` (one trailing line-ending stripped, so the expectation is
// platform-independent).
void expectValue(const std::string& program, const std::string& expected) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, expected);
}

// A program whose `Main` body is `body`.
std::string prog(const std::string& body) {
    return "module M;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// THE PIN ROW — the `uint` spelling of the number.
// ===========================================================================

// §5.1: the `uint` 4000000000 converts to `long` by VALUE, so
// `DateTime.FromTicks(4000000000u).Ticks` is 4000000000 — and the program
// COMPILES AND RUNS (before the fix the compiler aborted on it).
BEZEL_TEST(datetime_fromticks_unsigned_7252,
           a_uint_argument_keeps_its_value_5_1) {
    expectValue(
        prog("    Console.WriteLine(DateTime.FromTicks(4000000000u).Ticks);\n"),
        "4000000000");
}

// THE ISSUE'S OWN CLAIM — the two spellings of the same number must AGREE.
BEZEL_TEST(datetime_fromticks_unsigned_7252, two_spellings_agree_5_1) {
    expectValue(
        prog("    Console.WriteLine(DateTime.FromTicks(4000000000u).Ticks);\n"
             "    Console.WriteLine(DateTime.FromTicks(4000000000L).Ticks);\n"),
        "4000000000\n4000000000");
}

// The same with a NON-CONSTANT source — a `uint` VARIABLE.
BEZEL_TEST(datetime_fromticks_unsigned_7252,
           a_uint_variable_keeps_its_value_5_1) {
    expectValue(
        prog("    var u: uint := 4000000000;\n"
             "    Console.WriteLine(DateTime.FromTicks(u).Ticks);\n"),
        "4000000000");
}

// THE SIGNED CONTROL — a widening that zero-extended INDISCRIMINATELY would
// break this row: a negative `ticks` must keep its sign. Since #7716 a DateTime
// stores `Kind` in the top two bits of its scalar, and stdlib §2.5 ("Parse and
// range contracts", v0.350) says `FromTicks` raises OverflowException outside
// MinValue..MaxValue, so a negative count is no longer returned as-is: it
// RAISES (sign-extended -5 is out of range). A zero-extending widening would
// turn the `int` -5 into 4294967291, an IN-range tick count that does NOT
// raise, so the row still tells the two apart.
BEZEL_TEST(datetime_fromticks_unsigned_7252,
           a_negative_signed_argument_keeps_its_sign_5_1) {
    expectValue(
        prog("    var i: int := -5;\n"
             "    try\n"
             "      var d := DateTime.FromTicks(i);\n"
             "      Console.WriteLine($\"no raise {d.Ticks}\");\n"
             "    catch OverflowException =>\n"
             "      Console.WriteLine(\"OVF\");\n"
             "    end;\n"),
        "OVF");
}

// ===========================================================================
// THE THIRD TICK-CARRYING CONVERSION OF THE SHAPE — the seconds.
// ===========================================================================

// `DateTime.FromUnixTimeSeconds(seconds: long)` — the same family, the same
// unconditional `sext`, the same fix. 4000000000 seconds after the Unix
// epoch is 661355968000000000 ticks.
BEZEL_TEST(datetime_fromticks_unsigned_7252,
           the_unix_seconds_conversion_keeps_its_value_5_1) {
    expectValue(
        prog("    Console.WriteLine(DateTime.FromUnixTimeSeconds(4000000000u).Ticks);\n"),
        "661355968000000000");
}

// The control: the `long` spelling of the same number is unchanged by the
// fix (the fix changes only the UNSIGNED path).
BEZEL_TEST(datetime_fromticks_unsigned_7252,
           the_unix_seconds_control_is_unchanged_5_1) {
    expectValue(
        prog("    Console.WriteLine(DateTime.FromUnixTimeSeconds(4000000000L).Ticks);\n"
             "    Console.WriteLine(DateTime.FromUnixTimeSeconds(4000000000u).Ticks);\n"),
        "661355968000000000\n661355968000000000");
}

// The signed control for the seconds conversion: a negative count of seconds
// must keep its sign.
BEZEL_TEST(datetime_fromticks_unsigned_7252,
           a_negative_unix_seconds_keeps_its_sign_5_1) {
    expectValue(
        prog("    Console.WriteLine(DateTime.FromUnixTimeSeconds(-5L).Ticks);\n"),
        "621355967950000000");
}
