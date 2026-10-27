// Regression tests for #7249 (spec §5.1, v0.345): an UNSIGNED operand at
// `TimeSpan.FromTicks(ticks: long)`'s `long` parameter is SIGN-EXTENDED, so
// the value does not survive the widening.
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
// => §5.1 converts VALUES, not bit patterns. The `uint` 4000000000 IS
// 4000000000; it is not the `int32` -294967296 that its low 32 bits read as.
// The LLVM instruction that widens an unsigned value to a wider signed
// integer is `zext` (zero-extending), not `sext` (sign-extending).
//
// Standard library §2.5 (docs/Bezel_Standard_Library.md, v0.345) declares:
//
//   public static method FromTicks(ticks: long): TimeSpan
//
// => `ticks` is declared `long`, so an unsigned argument is CONVERTED at the
// call, under §5.1's table.
//
// THE DEFECT. The `TimeSpan.FromTicks` lowering widened its argument with an
// UNCONDITIONAL `sext` — a sign-extending widening — so
// `TimeSpan.FromTicks(4000000000u).Ticks` printed -294967296.
//
// THE FIX. The widening is now SIGNEDNESS-AWARE: an unsigned source
// zero-extends, a signed source keeps the `sext` it always had, so a
// negative `ticks` keeps its sign. The signedness is read with
// `isUnsignedTargetExpr` — the house predicate the #7242 `double`-parameter
// converter and the #7253 `Step` widening use — and NOT from the bit width
// (`int` and `uint` are both 32 bits, `long` and `ulong` both 64).
//
// THE ORACLE. `runAot` — the real AOT path. A wrong rendering's oracle is
// the program's exact stdout, never the exit code; and a compiler abort's
// oracle is that the program compiles and runs. Every row pins the printed
// text, and every row also asserts that the compiler did not abort.
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
// `TimeSpan.FromTicks(4000000000u).Ticks` is 4000000000.
BEZEL_TEST(timespan_fromticks_unsigned_7249,
           a_uint_argument_keeps_its_value_5_1) {
    expectValue(
        prog("    Console.WriteLine(TimeSpan.FromTicks(4000000000u).Ticks);\n"),
        "4000000000");
}

// THE ISSUE'S OWN CLAIM — the two spellings of the same number must AGREE.
// Both calls run in ONE program, so the row pins the disagreement itself, not
// just one spelling's value.
BEZEL_TEST(timespan_fromticks_unsigned_7249, two_spellings_agree_5_1) {
    expectValue(
        prog("    Console.WriteLine(TimeSpan.FromTicks(4000000000u).Ticks);\n"
             "    Console.WriteLine(TimeSpan.FromTicks(4000000000L).Ticks);\n"),
        "4000000000\n4000000000");
}

// The same with a NON-CONSTANT source — a `uint` VARIABLE, so the widening
// is a real instruction and not a folded constant.
BEZEL_TEST(timespan_fromticks_unsigned_7249,
           a_uint_variable_keeps_its_value_5_1) {
    expectValue(
        prog("    var u: uint := 4000000000;\n"
             "    Console.WriteLine(TimeSpan.FromTicks(u).Ticks);\n"
             "    Console.WriteLine(TimeSpan.FromTicks(u).Ticks);\n"),
        "4000000000\n4000000000");
}

// THE SIGNED CONTROL — a widening that zero-extended INDISCRIMINATELY would
// break this row: a negative `ticks` must keep its sign.
BEZEL_TEST(timespan_fromticks_unsigned_7249,
           a_negative_signed_argument_keeps_its_sign_5_1) {
    expectValue(
        prog("    Console.WriteLine(TimeSpan.FromTicks(-5L).Ticks);\n"),
        "-5");
}

// The full width of an unsigned operand: the `uint` 4000000000 is four
// billion TICK-CARRIING units, and the `long` parameter holds it exactly.
BEZEL_TEST(timespan_fromticks_unsigned_7249,
           the_uint_value_survives_the_widening_5_1) {
    expectValue(
        prog("    var u: uint := 4000000000;\n"
             "    var s: TimeSpan := TimeSpan.FromTicks(u);\n"
             "    Console.WriteLine(s.Ticks);\n"),
        "4000000000");
}
