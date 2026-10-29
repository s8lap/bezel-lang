// Regression tests for #7219 -- a purely POSITIONAL `Math.Fmod` /
// `Math.IEEERemainder` call evaluated each argument TWICE.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §22.5 (v0.350): "A call's supplied argument expressions are evaluated
// in call-site textual order -- left to right as written, for positional and
// named arguments alike; naming an argument changes which parameter it binds
// to, never when it runs." Each supplied argument expression therefore runs
// exactly once, in textual order.  stdlib §2.2 declares
// `Fmod(x: double, y: double): double` and
// `IEEERemainder(x: double, y: double): double`.
//
// WHAT THE DEFECT WAS.
//
// `src/codegen/LlvmBackend_Calls.cpp`'s shared 2-argument `Math` block lowers
// both operands and then dispatches on the member name; it has arms for
// Min / Max / Pow / Round only, so a `Fmod` / `IEEERemainder` call fell out of
// the block and the member's OWN arm lowered `x` / `y` a second time.  Any
// side effect in an operand fired twice (MEASURED before the fix:
// `Math.Fmod(c.D("a"), c.D("b"))` traces `a b a b`).
//
// THE SWEEP.  The issue says "a Math/Convert member"; every `Math` and
// `Convert` member of stdlib §2.2 / §2.3 was traced with a counting argument,
// positionally and (where it has more than one parameter or a default) with a
// named spelling.  Only `Fmod` / `IEEERemainder` double-evaluated; every
// other member row below is a CONTROL that is green on both arms.
//
// THE VALUE, NOT THE EXIT CODE.  Every row asserts STDOUT: a trace of the
// operand tags in the order they ran, then the result.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A tracing operand source: each call prints its tag then returns its value.
const char* kCtr =
    "class Ctr "
    "  public n: int; "
    "  public method I(tag: string, v: int): int "
    "    this.n := this.n + 1; Console.Write(tag + \" \"); return v; "
    "  end I; "
    "  public method D(tag: string, v: double): double "
    "    this.n := this.n + 1; Console.Write(tag + \" \"); return v; "
    "  end D; "
    "  public method L(tag: string, v: long): long "
    "    this.n := this.n + 1; Console.Write(tag + \" \"); return v; "
    "  end L; "
    "  public method S(tag: string, v: string): string "
    "    this.n := this.n + 1; Console.Write(tag + \" \"); return v; "
    "  end S; "
    "end Ctr; ";

std::string stdoutOf(const std::string& body) {
    auto r = runAot(std::string("module M; import System; ") + kCtr +
                    "class C public static method Main(): int "
                    "var c: Ctr := new Ctr(); " + body +
                    " return 0; end Main; end C;");
    if (!r.ran) return std::string("<did-not-run>");
    std::string out;
    for (char ch : r.stdoutText)
        if (ch != '\r') out += ch;
    return out;
}

}  // namespace

// ---- the regression rows: fail before the fix ("a b a b 3") ----------------

BEZEL_TEST(math_fmod_positional_once_7219, fmod_positional_each_operand_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Fmod(c.D(\"a\", 7.0), c.D(\"b\", 4.0)));"),
        std::string("a b 3\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, ieee_remainder_positional_each_operand_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.IEEERemainder(c.D(\"a\", 7.0), c.D(\"b\", 4.0)));"),
        std::string("a b -1\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, fmod_positional_counter_is_two) {
    // A second, independent witness: the call counter itself.
    BEZEL_EXPECT_EQ(stdoutOf(
        "var r: double := Math.Fmod(c.D(\"a\", 7.0), c.D(\"b\", 4.0)); "
        "Console.WriteLine(c.n); Console.WriteLine(r);"),
        std::string("a b 2\n3\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, fmod_int_operands_converted_once) {
    // §5.1 int -> double at the parameters; the operands still run once.
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Fmod(c.I(\"a\", 9), c.I(\"b\", 4)));"),
        std::string("a b 1\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, fmod_nested_each_operand_once) {
    // fmod(fmod(11, 7), 3) = fmod(4, 3) = 1; three operands, three runs.
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Fmod(Math.Fmod(c.D(\"a\", 11.0), c.D(\"b\", 7.0)), "
        "c.D(\"c\", 3.0)));"),
        std::string("a b c 1\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, fmod_in_interpolation_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var s: string := $\"{Math.Fmod(c.D(\"a\", 7.0), c.D(\"b\", 4.0))}\"; "
        "Console.WriteLine(s);"),
        std::string("a b 3\n"));
}

// ---- controls: the NAMED spellings were already once-each, textual order ----

BEZEL_TEST(math_fmod_positional_once_7219, fmod_named_reordered_once_textual_order) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Fmod(y := c.D(\"y\", 4.0), x := c.D(\"x\", 7.0)));"),
        std::string("y x 3\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, ieee_remainder_named_reordered_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.IEEERemainder(y := c.D(\"y\", 4.0), x := c.D(\"x\", 7.0)));"),
        std::string("y x -1\n"));
}

// ---- the sweep: every other two-or-more-operand Math member, once each ------

BEZEL_TEST(math_fmod_positional_once_7219, sweep_pow_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Pow(c.D(\"a\", 2.0), c.D(\"b\", 3.0)));"),
        std::string("a b 8\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, sweep_round_digits_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Round(c.D(\"a\", 2.555), c.I(\"b\", 2)));"),
        std::string("a b 2.56\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, sweep_min_max_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Min(c.I(\"a\", 3), c.I(\"b\", 4))); "
        "Console.WriteLine(Math.Max(c.D(\"a\", 3.0), c.D(\"b\", 4.0))); "
        "Console.WriteLine(Math.Max(c.S(\"a\", \"x\"), c.S(\"b\", \"y\")));"),
        std::string("a b 3\na b 4\na b y\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, sweep_clamp_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Clamp(c.I(\"a\", 3), c.I(\"b\", 4), c.I(\"c\", 9)));"),
        std::string("a b c 4\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, sweep_divrem_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var r: int := 0; "
        "Console.WriteLine(Math.DivRem(c.I(\"a\", 7), c.I(\"b\", 2), out r)); "
        "Console.WriteLine(r);"),
        std::string("a b 3\n1\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, sweep_one_operand_members_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Abs(c.I(\"a\", -3))); "
        "Console.WriteLine(Math.Sqrt(c.D(\"a\", 4.0))); "
        "Console.WriteLine(Math.Floor(c.D(\"a\", 2.5))); "
        "Console.WriteLine(Math.Ceiling(c.D(\"a\", 2.5))); "
        "Console.WriteLine(Math.Truncate(c.D(\"a\", 2.5))); "
        "Console.WriteLine(Math.Round(c.D(\"a\", 2.5))); "
        "Console.WriteLine(Math.Sign(c.I(\"a\", -4)));"),
        std::string("a 3\na 2\na 2\na 3\na 2\na 2\na -1\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, sweep_convert_members_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var ri: int := 0; "
        "Console.WriteLine(Convert.ToInt32(c.S(\"a\", \"12\"))); "
        "Console.WriteLine(Convert.ToDouble(c.S(\"a\", \"1.5\"))); "
        "Console.WriteLine(Convert.TryToInt32(c.S(\"a\", \"12\"), out ri)); "
        "Console.WriteLine(Convert.ToString(c.I(\"a\", 5))); "
        "Console.WriteLine(Convert.ToString(c.I(\"a\", 255), c.I(\"b\", 16))); "
        "Console.WriteLine(Convert.ToString(c.L(\"a\", 255L), c.I(\"b\", 16)));"),
        std::string("a 12\na 1.5\na True\na 5\na b ff\na b ff\n"));
}
BEZEL_TEST(math_fmod_positional_once_7219, sweep_named_reordered_members_once) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "Console.WriteLine(Math.Pow(y := c.D(\"y\", 3.0), x := c.D(\"x\", 2.0))); "
        "Console.WriteLine(Math.Round(digits := c.I(\"d\", 2), x := c.D(\"x\", 2.555))); "
        "Console.WriteLine(Math.Clamp(high := c.I(\"H\", 9), low := c.I(\"L\", 4), "
        "value := c.I(\"V\", 3))); "
        "Console.WriteLine(Convert.ToString(toBase := c.I(\"b\", 16), value := c.I(\"v\", 255)));"),
        std::string("y x 8\nd x 2.56\nH L V 4\nb v ff\n"));
}
