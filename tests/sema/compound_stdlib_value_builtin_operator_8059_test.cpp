// fix #8059 (stdlib §2.5, spec §6.6/§25.2, v0.353): the COMPOUND form of a
// stdlib value builtin's operator escaped the §6.6/§25.2 check on an
// ANNOTATED target — `a += 2` on a TimeSpan computed a bare tick addition,
// and `a *= 2.0` aborted the compile with malformed IR.
//
// ── THE TWO SHAPES OF A STDLIB VALUE BUILTIN ─────────────────────────────────
//
// A stdlib value builtin reaches sema in TWO shapes for the SAME type:
//
//   * a value-producing expression stamps the result
//     `declaration = builtinClassSym("<name>")` (`TimeSpan.FromTicks(...)`);
//   * Slice S24 (spec §2.3, TypeResolution_Names.cpp) resolves the NAME in an
//     ANNOTATION — `var a: TimeSpan := ...`, an `a: TimeSpan` parameter, a
//     `TimeSpan` field, `new DateTime(...)` — to a Named TypeRef with
//     `declaration == nullptr` and the AST spelling as its name.
//
// Every rule keyed on `lt->declaration` saw only the FIRST shape.
//
// ── WHAT STDLIB §2.5 DECLARES (v0.353, verbatim) ───────────────────────────
//
//   public static method operator -(a: DateTime, b: DateTime): TimeSpan;
//   public static method operator -(d: DateTime, t: TimeSpan): DateTime;
//   public static method operator +(d: DateTime, t: TimeSpan): DateTime;
//   public static method operator +(t: TimeSpan, d: DateTime): DateTime;
//   ...
//   public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
//   public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
//   public static method operator *(a: TimeSpan, factor: double): TimeSpan;
//                                      // computes over Ticks in double; OverflowException on a
//                                      //   product outside the tick range or a NaN operand
//   public static method operator /(a: TimeSpan, divisor: double): TimeSpan;
//                                      // same computation model and OverflowException rule
//                                      //   as *; DivideByZeroException on divisor = 0.0
//   public static method operator -(t: TimeSpan): TimeSpan;
//                                      // unary negation; OverflowException on MinValue
//
// ⇒ `+(TimeSpan, TimeSpan)`, `-(TimeSpan, TimeSpan)`, `*(TimeSpan, double)`,
//   `/(TimeSpan, double)`, `+(DateTime, TimeSpan)`, `-(DateTime, TimeSpan)` are
//   DECLARED; `+(TimeSpan, int)` and `+(DateTime, int)` are NOT (and spec
//   §5.1's implicit-conversion table lists the numeric widening rows only —
//   there is NO implicit conversion to or from a TimeSpan/DateTime).
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
//
// The compound-assignment operator block in `src/sema/ExprTypecheck.cpp` (the
// named-target arm) is guarded on `tTy->declaration`. On the ANNOTATION shape
// that is `nullptr`, so the whole §6.6/§25.2 check was SKIPPED: no operator
// lookup, no diagnostic.
//
// MEASURED on the parent of this fix (bezelc at 8f66dea6):
//
//   `var a: TimeSpan := TimeSpan.FromTicks(5L); a += 2;`
//     -> COMPILES, prints `a=7` — a bare tick addition (5 ticks + 2 = 7
//        ticks, a span that grew by 200 ns) of an operator pair stdlib §2.5
//        does not declare.
//
//   `var a := TimeSpan.FromTicks(5L); a += 2;`
//     -> "compound assignment `+=` has no valid result on class type
//        `TimeSpan` ..." — the INFERRED shape refused what the ANNOTATED shape
//        accepted: the two shapes of the SAME type disagreed.
//
//   `var d: DateTime := new DateTime(2024, 1, 1); d += 1;`
//     -> COMPILES, prints `d=638396640000000001` (the tick count + 1).
//
//   `var a: TimeSpan := TimeSpan.FromTicks(5L); a *= 2.0;`
//     -> "bezelc: codegen error: IR verification failed (malformed IR emitted)"
//        — the compound lowered through the built-in numeric tail and fed
//        `llvm.smul.with.overflow.i64(i64, double)` to the IR verifier.
//
// ── THE FIX ─────────────────────────────────────────────────────────────────
//
// Spec §6.6 (v0.353) is explicit about the compound form's typing: "For
// every operator above except `??=`, the statement `x op= e` is equivalent to
// `x := x op e` with the target evaluated once", and "Let T be the left-hand
// side's type and R the type of `x op e` under the rules of this chapter
// (§6.2 promotion, §6.5 shifts and bitwise, §25.2 overloads)". So the
// compound form's verdict IS the binary form's verdict — and the binary path
// already answers it correctly for a stdlib value builtin in BOTH shapes,
// since #7547 taught it to name the stdlib value builtins by NAME: it FINDS
// stdlib §2.5's declared operators, and its §25 reject NAMES the operand's
// type instead of printing the placeholder `?`.
//
// The compound form of a stdlib value builtin's operator is therefore
// rewritten to its §6.6 desugaring `x := x op e` — the same rewrite #4711
// already performs for the narrowed-Object compound form, and for the same
// reason: the rewrite is EXACT when the target is a bare identifier (the
// "evaluated once" clause has nothing to evaluate).
//
// ── WHAT EACH ROW IS FOR ───────────────────────────────────────────────────
//
//   B1  the issue's own shape: `a += 2` over an ANNOTATED TimeSpan is a
//       compile error, and the diagnostic NAMES `TimeSpan` (not `?`).
//   B2  the two shapes of the SAME type now AGREE: the INFERRED shape's
//       `a += 2` produces the SAME diagnostic.
//   B3  the DECLARED pairs are FOUND: `a += b` (TimeSpan + TimeSpan) computes
//       12 and `a -= b` computes 5 — on BOTH shapes. Before the fix the
//       INFERRED shape refused the declared pair outright.
//   B4  the DECLARED `*` overload is FOUND for the compound form: `a *= 2.0`
//       computes 10 ticks over the ticks in double — on BOTH shapes. Before
//       the fix the ANNOTATED shape ABORTED the compile with malformed IR and
//       the INFERRED shape refused it.
//   B5  the same for DateTime: `d += 1` is a compile error naming
//       `DateTime` — on BOTH shapes (`new DateTime(...)` is itself an
//       ANNOTATION shape, so the "inferred" spelling was silently accepted).
//   B6  the DECLARED DateTime + TimeSpan pair still computes.
//   B7  CONTROL: the binary form is unchanged — `ts + ts` computes 10.
//   B8  CONTROL: the type-parameter wording is untouched (#3930): an
//       unconstrained `T` still gets the "not available on type parameter
//       `T`" wording and never "type `?`" — the fix names the STDLIB value
//       builtins only.
//   B9  CONTROL: a USER type named `TimeSpan` is not the stdlib builtin: it
//       keeps the §25 rule and may declare its own operators.
//  B10  CONTROL: `ts < ts` is still refused, and the refusal still names
//       `TimeSpan` — stdlib §2.5 declares ordering as `CompareTo` only.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── B1 ─ the issue's own shape: `a += 2` over an ANNOTATED TimeSpan is a
// compile error, and the diagnostic NAMES the operand's type.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           annotated_timespan_plus_int_is_rejected_stdlib_2_5_spec_25) {
    auto r = runAot(
        "module M8059A; "
        "class C public static method Main(): int "
        "  var a: TimeSpan := TimeSpan.FromTicks(5L); "
        "  a += 2; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `TimeSpan`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── B2 ─ the two shapes of the SAME type AGREE: the INFERRED shape's
// `a += 2` produces the SAME diagnostic as the annotated one (B1). Before the
// fix the inferred shape refused it with a DIFFERENT message ("compound
// assignment `+=` has no valid result on class type `TimeSpan`") and the
// annotated shape accepted it silently.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           inferred_timespan_plus_int_agrees_with_the_annotated_shape) {
    auto r = runAot(
        "module M8059B; "
        "class C public static method Main(): int "
        "  var a := TimeSpan.FromTicks(5L); "
        "  a += 2; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `TimeSpan`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── B3 ─ the DECLARED pair `+(a: TimeSpan, b: TimeSpan)` (stdlib §2.5) is
// FOUND for the compound form: `a += b` computes 5 + 7 = 12 ticks and
// `a -= b` computes 5 ticks — on the ANNOTATED shape ...
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           annotated_timespan_plus_timespan_computes_stdlib_2_5) {
    auto r = runAot(
        "module M8059C; "
        "class C public static method Main(): int "
        "  var a: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var b: TimeSpan := TimeSpan.FromTicks(7L); "
        "  a += b; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  a -= b; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=12\na=5\n"));
}

// ── B3 ─ ... and on the INFERRED shape, which before the fix REFUSED the
// declared pair outright ("compound assignment `+=` has no valid result on
// class type `TimeSpan`" — the operator §2.5 declares was never found).
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           inferred_timespan_plus_timespan_computes_stdlib_2_5) {
    auto r = runAot(
        "module M8059D; "
        "class C public static method Main(): int "
        "  var a := TimeSpan.FromTicks(5L); "
        "  var b := TimeSpan.FromTicks(7L); "
        "  a += b; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  a -= b; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=12\na=5\n"));
}

// ── B4 ─ the DECLARED `*(a: TimeSpan, factor: double)` overload (stdlib
// §2.5) is FOUND for the compound form: `a *= 2.0` computes 5 ticks * 2.0 =
// 10 ticks. Before the fix the ANNOTATED shape ABORTED the compile with
// "IR verification failed (malformed IR emitted)" (the compound lowered
// through the built-in numeric tail and fed `llvm.smul.with.overflow.i64`),
// and the INFERRED shape refused it.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           annotated_timespan_mul_double_computes_stdlib_2_5) {
    auto r = runAot(
        "module M8059E; "
        "class C public static method Main(): int "
        "  var a: TimeSpan := TimeSpan.FromTicks(5L); "
        "  a *= 2.0; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=10\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// ── B4 ─ ... and on the INFERRED shape, which before the fix refused the
// declared `*` overload.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           inferred_timespan_mul_double_computes_stdlib_2_5) {
    auto r = runAot(
        "module M8059F; "
        "class C public static method Main(): int "
        "  var a := TimeSpan.FromTicks(5L); "
        "  a *= 2.0; "
        "  Console.WriteLine($\"a={a.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=10\n"));
}

// ── B5 ─ the same for DateTime: `d += 1` is a compile error naming
// `DateTime` — stdlib §2.5 declares `+(d: DateTime, t: TimeSpan)`, not
// `+(DateTime, int)`, and §5.1 admits no implicit conversion to a DateTime.
// Before the fix BOTH spellings compiled silently and printed the tick count
// + 1 (`new DateTime(...)` is itself an ANNOTATION shape, so the "inferred"
// spelling was accepted the same way).
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           annotated_datetime_plus_int_is_rejected_stdlib_2_5_spec_25) {
    auto r = runAot(
        "module M8059G; "
        "class C public static method Main(): int "
        "  var d: DateTime := new DateTime(2024, 1, 1); "
        "  d += 1; "
        "  Console.WriteLine($\"d={d.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `DateTime`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── B5 ─ ... and the INFERRED spelling of the same program.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           inferred_datetime_plus_int_is_rejected_stdlib_2_5_spec_25) {
    auto r = runAot(
        "module M8059H; "
        "class C public static method Main(): int "
        "  var d := new DateTime(2024, 1, 1); "
        "  d += 1; "
        "  Console.WriteLine($\"d={d.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `DateTime`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── B6 ─ the DECLARED `+(d: DateTime, t: TimeSpan)` pair (stdlib §2.5)
// still computes through the compound form.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           datetime_plus_timespan_computes_stdlib_2_5) {
    auto r = runAot(
        "module M8059I; "
        "class C public static method Main(): int "
        "  var d: DateTime := new DateTime(2024, 1, 1); "
        "  var sp: TimeSpan := TimeSpan.FromTicks(1L); "
        "  d += sp; "
        "  Console.WriteLine($\"d={d.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("d=638396640000000001\n"));
}

// ── B7 ─ CONTROL: the binary form is unchanged — `ts + ts` computes 10.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           binary_timespan_plus_timespan_still_computes_stdlib_2_5) {
    auto r = runAot(
        "module M8059J; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var r2 := ts + ts; "
        "  Console.WriteLine($\"{r2.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── B8 ─ CONTROL: the type-parameter wording is untouched (#3930): an
// unconstrained `T` still gets the "not available on type parameter `T`"
// wording and never "type `?`" — the fix names the STDLIB value builtins
// only.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           type_parameter_wording_unchanged_spec_25_2_11_8) {
    auto r = runAot(
        "module M8059K; "
        "class Box<T> "
        "  public static method Add(a: T, b: T): T "
        "    return a + b; "
        "  end Add; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not available on type parameter `T`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── B9 ─ CONTROL: a USER type named `TimeSpan` is NOT the stdlib builtin:
// it keeps the §25 rule and may declare its own operators (so its compound
// assignment lowers through ITS declared `operator +`).
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           user_type_named_timespan_keeps_its_own_operators) {
    auto r = runAot(
        "module M8059L; "
        "record TimeSpan(v: int) "
        "  public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan "
        "    => new TimeSpan(a.v + b.v); "
        "end TimeSpan; "
        "class C public static method Main(): int "
        "  var a := new TimeSpan(5); "
        "  var b := new TimeSpan(7); "
        "  a += b; "
        "  Console.WriteLine($\"{a.v}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12\n"));
}

// ── B10 ─ CONTROL: `ts < ts` is still refused, and the refusal still names
// `TimeSpan` — stdlib §2.5 declares ordering as `CompareTo` only ("no
// <,>,<=,>= overloads are declared"), so refusing the ordering operator is
// the specified behaviour.
BEZEL_TEST(compound_stdlib_value_builtin_operator_8059,
           ordering_operator_still_refused_stdlib_2_5) {
    auto r = runAot(
        "module M8059M; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var b := ts < ts; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `<` is not defined for type `TimeSpan`"));
}
