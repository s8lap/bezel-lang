// Spec-derived regression test for issue #8036.
//
// spec §22.3 (v0.350) "the `out` modifier" and §22.5 (v0.350) "Optional
// parameters and named arguments";
// stdlib §2.2 (v0.350) `Math.DivRem`, §2.3 (v0.350) `Convert`.
//
// THE DEFECT. The `Math` / `Convert` head had NO arity gate at all:
// `checkBuiltinMathConvertCallArgs` (the §5.1 gate) stands down for a member
// the parameter table declares no form at that EXACT arity, and the shared
// resolver `checkBuiltinCallAdmission` reaches the VALUE built-ins (`int`,
// `float`, `Guid`, `DateTime`, …), not `Math` / `Convert`. So an
// UNDER-SUPPLIED call compiled clean and DEGRADED to a zero-initialized
// placeholder:
//
//   var q := Math.DivRem(17, 5);
//     -> "warning: internal: lowerLocalDecl's initializer expression failed
//         to lower to a value for this local's slot (degraded to a
//         zero-initialized value) — #2434"
//     -> the program prints 0.  THE CORRECT ANSWER IS 3 (17 = 3*5 + 2).
//
// stdlib §2.2 declares `DivRem(a: int, b: int, out remainder: int)` — THREE
// parameters. §22.3 says an `out` slot IS an argument and §22.5 says "`out`
// can never carry a default", so every one of the three must be bound and the
// two-argument spelling must be REFUSED.
//
// THE ORACLE is the refusal (a compile error at a real source location),
// never the absence of a diagnostic. The three-argument form is the CONTROL
// that must stay green: a fix that refuses it is a FALSE REJECTION.
//
// (The relation to #8034: that issue corrects the parameter TABLE's
// `required` column for `Math.DivRem` (2 -> 3). The table is the registry
// lane's file; this test does not depend on it — the gate reads the `out`
// slot out of the table's own declared shape, so it answers 3 before and
// after that correction.)
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A refusal row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run clean under the strict AOT
// oracle, and print exactly `out`.
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAotChecked(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ── the `Math` head: an UNDER-SUPPLIED call must be REFUSED ────────────────

// stdlib §2.2 declares `DivRem(a: int, b: int, out remainder: int)`. The
// two-argument spelling omits the `out` slot, and the base compiled it clean
// and printed 0 (the degraded value) where the answer is 3.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_divrem_without_the_out_slot_is_refused) {
    expectRejected(prog("var q := Math.DivRem(17, 5);"),
                   "no matching overload of `Math.DivRem`");
}

// The same member at a different arity — over-supplied is refused too.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_divrem_with_a_surplus_argument_is_refused) {
    expectRejected(prog("var r: int := 0; var q := Math.DivRem(17, 5, out r, 9);"),
                   "no matching overload of `Math.DivRem`");
}

// stdlib §2.2 declares `Pow(x: double, y: double): double` — two
// parameters. The one-argument spelling was admitted and degraded to 0.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_pow_with_one_argument_is_refused) {
    expectRejected(prog("var q := Math.Pow(2.0);"),
                   "no matching overload of `Math.Pow`");
}

// stdlib §2.2 declares `Abs(x: int): int` — ONE parameter. The
// zero-argument spelling was admitted and degraded to 0.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_abs_without_its_argument_is_refused) {
    expectRejected(prog("var q := Math.Abs();"),
                   "no matching overload of `Math.Abs`");
}

// stdlib §2.2 declares `Clamp(value: T, low: T, high: T): T` — three
// parameters.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_clamp_with_two_arguments_is_refused) {
    expectRejected(prog("var q := Math.Clamp(5, 10);"),
                   "no matching overload of `Math.Clamp`");
}

// stdlib §2.3 declares `TryToInt32(value: string, out result: int): bool` —
// two parameters, the `out` slot included (§22.3: an `out` slot IS an
// argument). The one-argument spelling was admitted and degraded.
BEZEL_TEST(builtin_math_convert_arity_8036,
           convert_trytoint32_without_the_out_slot_is_refused) {
    expectRejected(prog("var ok := Convert.TryToInt32(\"42\");"),
                   "no matching overload of `Convert.TryToInt32`");
}

// ── CONTROLS: every LEGAL arity must still be admitted ────────────────────

// THE CONTROL THAT MATTERS: the three-argument form is ADMITTED, and `r`
// receives the remainder 2 (17 = 3*5 + 2).
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_divrem_with_the_out_slot_is_admitted) {
    expectStdout(prog("var r: int := 0; var q := Math.DivRem(17, 5, out r); "
                      "Console.WriteLine(q); Console.WriteLine(r);"),
                 "3\n2\n");
}

// The `long` overload behaves the same.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_divrem_long_with_the_out_slot_is_admitted) {
    expectStdout(prog("var r: long := 0L; var q := Math.DivRem(17L, 5L, out r); "
                      "Console.WriteLine(q); Console.WriteLine(r);"),
                 "3\n2\n");
}

// The legal arities of the other `Math` members are untouched.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_members_at_their_declared_arities_are_admitted) {
    expectStdout(prog("Console.WriteLine(Math.Pow(2.0, 8.0)); "
                      "Console.WriteLine(Math.Abs(-42)); "
                      "Console.WriteLine(Math.Min(3, 7)); "
                      "Console.WriteLine(Math.Clamp(15, 0, 10)); "
                      "Console.WriteLine(Math.Round(3.14159, 2));"),
                 "256\n42\n3\n10\n3.14\n");
}

// `Math.Round`'s `digits` parameter DECLARES A DEFAULT, so the one-argument
// form is legal (§22.5) — the gate must not refuse it.
BEZEL_TEST(builtin_math_convert_arity_8036,
           math_round_with_the_defaulted_parameter_omitted_is_admitted) {
    expectStdout(prog("Console.WriteLine(Math.Round(3.14159));"), "3\n");
}

// The legal `Convert` arities are untouched, including the `out` slot.
BEZEL_TEST(builtin_math_convert_arity_8036,
           convert_members_at_their_declared_arities_are_admitted) {
    expectStdout(prog("var ok: bool := false; var v: int := 0; "
                      "Console.WriteLine(Convert.ToInt32(\"42\")); "
                      "Console.WriteLine(Convert.ToString(255, 16)); "
                      "ok := Convert.TryToInt32(\"42\", out v); "
                      "Console.WriteLine(ok); Console.WriteLine(v);"),
                 "42\nff\nTrue\n42\n");
}
