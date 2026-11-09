// fix #7605 — `ts * 2` / `ts * 2L` / `ts * 2.0f` are REJECTED although §5.1
// converts int/long/float to the declared `factor: double`.
//
// THE DEFECT. stdlib §2.5 (v0.350) declares
//
//   public static method operator *(a: TimeSpan, factor: double): TimeSpan;
//   public static method operator /(a: TimeSpan, divisor: double): TimeSpan;
//
// §25.2 (v0.350) matches the operands to the parameters IN ORDER, by the
// ordinary argument rules, and §5.1's table widens `int`/`long`/`float`
// (and the other numeric rows) to `double` IMPLICITLY — so `ts * 2` binds
// the `int` operand to the `factor: double` parameter exactly as `ts * 2.0`
// does. The stdlib value builtin's operator arm (ExprTypecheck_Exprs.cpp)
// nevertheless demanded an EXACT `double` right operand, so all three shapes
// fell through to the §25 reject:
//
//   "operator `*` is not defined for type `TimeSpan` (§25); declare
//    `static method operator *(a: TimeSpan, b: TimeSpan)` inside `TimeSpan`
//    to overload it"
//
// THE FIX. The arm now admits a second operand that §5.1 widens to `double`
// and REALIZES the widening (the operand becomes a `double` value), so
// codegen's stdlib §2.5 arm lowers the product over the TICK COUNT in
// `double` — the same lowering `ts * 2.0` uses.
//
// WHAT THE FIX MUST NOT DO (the rows below pin it):
//
//   * `ts * 2.0m` (a `decimal` factor) stays REJECTED — §5.1's "never
//     implicit" list forbids a conversion between `decimal` and the binary
//     floats;
//   * `2 * ts` (the factor on the LEFT) stays REJECTED — §25.2 matches the
//     operands in order, with no automatic commutation (the mirrored `2.0 *
//     ts` case is #7606);
//   * `ts * ts` stays REJECTED — stdlib §2.5 declares no `*(TimeSpan,
//     TimeSpan)`;
//   * `ts + 2` stays REJECTED — §5.1 converts no type to `TimeSpan`;
//   * a USER type's own `operator *` keeps its own rules (the §25.2
//     "a derived-typed operand satisfies a base-typed parameter by the
//     ordinary argument rules" path is untouched).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── A1 ─ the issue's own shape: `ts * 2` over an ANNOTATED TimeSpan
// compiles and computes 5 ticks * 2 = 10 ticks (the `double` factor's
// spelling is not what §25.2 asks for — the CONVERSION is).
BEZEL_TEST(timespan_factor_widening_7605,
           int_factor_widens_to_double_stdlib_2_5_spec_5_1) {
    auto r = runAot(
        "module M7605A; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A2 ─ the `long` shape (`ts * 2L`).
BEZEL_TEST(timespan_factor_widening_7605,
           long_factor_widens_to_double_stdlib_2_5_spec_5_1) {
    auto r = runAot(
        "module M7605B; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2L; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A3 ─ the `float` shape (`ts * 2.0f`): §5.1 widens `float` to `double`.
BEZEL_TEST(timespan_factor_widening_7605,
           float_factor_widens_to_double_stdlib_2_5_spec_5_1) {
    auto r = runAot(
        "module M7605C; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2.0f; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A4 ─ the INFERRED shape agrees with the annotated one (the same two
// shapes of the same type, #7547's rule).
BEZEL_TEST(timespan_factor_widening_7605,
           inferred_shape_agrees_with_the_annotated_shape) {
    auto r = runAot(
        "module M7605D; "
        "class C public static method Main(): int "
        "  var ts := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A5 ─ a NON-CONSTANT factor: the conversion is realized for a value, not
// only for a literal.
BEZEL_TEST(timespan_factor_widening_7605,
           int_variable_factor_widens_to_double) {
    auto r = runAot(
        "module M7605E; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var n := 2; "
        "  var m := ts * n; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A6 ─ an UNSIGNED factor converts UNSIGNED: 4 000 000 000u read as a
// signed `int` is -294967296, so a signed conversion would compute
// 5 * -294967296 = -1474836480 ticks. §5.1 converts VALUES, so the
// unsigned row must give 20000000000 ticks.
BEZEL_TEST(timespan_factor_widening_7605,
           uint_factor_converts_unsigned) {
    auto r = runAot(
        "module M7605F; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 4000000000U; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("20000000000\n"));
}

// ── A7 ─ the `ulong` row, with the same unsigned reading.
BEZEL_TEST(timespan_factor_widening_7605,
           ulong_factor_converts_unsigned) {
    auto r = runAot(
        "module M7605G; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 4000000000UL; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("20000000000\n"));
}

// ── A8 ─ the `/` overload widens its divisor the same way: 10 ticks / 2 = 5.
BEZEL_TEST(timespan_factor_widening_7605,
           int_divisor_widens_to_double_stdlib_2_5_spec_5_1) {
    auto r = runAot(
        "module M7605H; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(10L); "
        "  var d := ts / 2; "
        "  Console.WriteLine($\"{d.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// ── A9 ─ the ANNOTATED PARAMETER shape (a method signature's operand).
BEZEL_TEST(timespan_factor_widening_7605,
           annotated_parameter_factor_widens_to_double) {
    auto r = runAot(
        "module M7605I; "
        "class C "
        "  public static method Scale(ts: TimeSpan, factor: int): TimeSpan "
        "    return ts * factor; "
        "  end Scale; "
        "  public static method Main(): int "
        "    var r := Scale(TimeSpan.FromTicks(5L), 2); "
        "    Console.WriteLine($\"{r.Ticks}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A10 ─ CONTROL: the declared `double` factor still computes (the fix
// must not disturb the shape that already worked).
BEZEL_TEST(timespan_factor_widening_7605,
           double_factor_still_computes_stdlib_2_5) {
    auto r = runAot(
        "module M7605J; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2.0; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A11 ─ CONTROL: a `decimal` factor stays REJECTED — §5.1's "never
// implicit" list forbids a conversion between `decimal` and the binary
// floats (so no `double` value reaches the `factor: double` parameter).
BEZEL_TEST(timespan_factor_widening_7605,
           decimal_factor_stays_rejected_spec_5_1) {
    auto r = runAot(
        "module M7605K; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2.0m; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined for type `TimeSpan`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── A12 ─ CONTROL: the MIRRORED order stays REJECTED — §25.2 matches the
// operands to the parameters in order, with no automatic commutation (the
// mirrored `2.0 * ts` case is #7606).
BEZEL_TEST(timespan_factor_widening_7605,
           mirrored_order_stays_rejected_spec_25_2) {
    auto r = runAot(
        "module M7605L; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := 2 * ts; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is not defined with a primitive left operand"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── A13 ─ CONTROL: `ts * ts` stays REJECTED — stdlib §2.5 declares no
// `*(TimeSpan, TimeSpan)`, and §5.1 converts no type to `TimeSpan`.
BEZEL_TEST(timespan_factor_widening_7605,
           timespan_timespan_stays_rejected_stdlib_2_5) {
    auto r = runAot(
        "module M7605M; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * ts; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined for type `TimeSpan`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── A14 ─ CONTROL: `ts + 2` stays REJECTED — §5.1 converts no type to
// `TimeSpan`, so the `+(TimeSpan, TimeSpan)` overload does not apply.
BEZEL_TEST(timespan_factor_widening_7605,
           timespan_plus_int_stays_rejected_spec_5_1) {
    auto r = runAot(
        "module M7605N; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var s := ts + 2; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `TimeSpan`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── A15 ─ CONTROL: a USER type's own `operator *` keeps its own rules —
// §25.2's "a derived-typed operand satisfies a base-typed parameter by the
// ordinary argument rules" reaches a `double` parameter from an `int`
// operand, and the fix must not disturb that path.
BEZEL_TEST(timespan_factor_widening_7605,
           user_operator_with_double_parameter_still_applies) {
    auto r = runAot(
        "module M7605O; "
        "record Vec(X: double) "
        "  public static method operator *(v: Vec, factor: double): Vec => "
        "    new Vec(v.X * factor); "
        "end Vec; "
        "class C public static method Main(): int "
        "  var v := new Vec(3.0); "
        "  var r := v * 2; "
        "  Console.WriteLine($\"{r.X}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}
