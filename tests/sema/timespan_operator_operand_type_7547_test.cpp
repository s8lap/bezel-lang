// fix #7547 (stdlib §2.5, v0.350; language spec §25, §25.2, §5.1):
// the stdlib-declared `TimeSpan` operators were not found on an ANNOTATED
// TimeSpan, and the operand's TYPE was lost when they were.
//
// ── WHAT §2.5 DECLARES (v0.350, verbatim) ─────────────────────────────────
//
//   public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
//   public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
//   public static method operator *(a: TimeSpan, factor: double): TimeSpan;
//                                      // computes over Ticks in double; OverflowException on a
//                                      //   product outside the tick range or a NaN operand
//                                      //   (pinned)
//   public static method operator /(a: TimeSpan, divisor: double): TimeSpan;
//                                      // same computation model and OverflowException rule
//                                      //   as *; DivideByZeroException on
//                                      //   divisor = 0.0
//   public static method operator -(t: TimeSpan): TimeSpan;
//                                      // unary negation; OverflowException on MinValue
//
// and, on the `TimeSpan` record's own header:
//
//   // (#1357): CompareTo = tick order. Ordering is CompareTo only — no
//   //   <,>,<=,>= overloads are declared (spec §9.2, §25.2).
//
// ⇒ `+`/`-` (TimeSpan, TimeSpan) and `*`/`/` (TimeSpan, double) ARE
//   declared; NO ordering operator is; and NO `Add` member is.
//
// ── THE DEFECT ────────────────────────────────────────────────────────────
//
// A stdlib value builtin reaches sema in TWO shapes for the SAME type:
//
//   * a value-producing expression stamps the result
//     `declaration = builtinClassSym("<name>")` (`TimeSpan.FromTicks(...)`);
//   * Slice S24 (§2.3, TypeResolution_Names.cpp) resolves the NAME in an
//     ANNOTATION — `var ts: TimeSpan`, a `ts: TimeSpan` parameter, a
//     `TimeSpan` field — to a Named TypeRef with `declaration == nullptr`
//     and the AST spelling as its name.
//
// Every rule that keyed on `lt->declaration` saw only the FIRST shape, so
// on the SECOND shape the operand's TYPE was LOST: the §25 operator reject
// named it `?` (the placeholder it uses for a type it cannot name), and the
// stdlib-declared `TimeSpan` operators were not found at all.
//
// MEASURED on the parent of this fix (bezelc at 06bf4c77):
//
//   `var ts: TimeSpan := TimeSpan.FromTicks(5L); var r := ts + ts;`
//     -> "operator `+` is not defined for type `?` (§25); declare `static
//         method operator +(a: ?, b: ?)` inside `?` to overload it"
//   `method F(ts: TimeSpan): TimeSpan => ts + ts`
//     -> the same `?` message
//   `var ts: TimeSpan := TimeSpan.FromTicks(5L); var r := ts * 2.0;`
//     -> the same `?` message
//
// while the byte-identical program over the INFERRED shape
// (`TimeSpan.FromTicks(5L) + TimeSpan.FromTicks(5L)`) compiles and computes
// 10. #7604 is this same hole for the `*`/`/` overloads and the compound
// forms; #7717 is its nullable (`ts!`) variant.
//
// The fix names the stdlib value builtins by NAME in the operator path (a
// USER type of the same name keeps its own §25 rule), so the declared
// operators are found on every shape of the type and the diagnostic names
// the operand instead of `?`.
//
// ── WHAT EACH ROW IS FOR ──────────────────────────────────────────────────
//
//   A1  the issue's own shape (`ts + ts` over an ANNOTATED local) —
//       compiles AND computes the right answer, not merely "not rejected".
//   A2  the same over an ANNOTATED PARAMETER (the shape #7604 filed).
//   A3  the `*`/`/` overloads over an annotated TimeSpan (§2.5's `double`
//       factor) — the other two members of the declared set.
//   A4  the `?` is GONE from the diagnostic: an annotated TimeSpan with no
//       applicable overload names `TimeSpan` (and not "type `?`").
//   A5  the same for the other §2.5 builtins: a `DateTime` operand names
//       `DateTime`.
//   A6  the over-acceptance the fix must NOT widen: `TimeSpan + int` is not
//       a declared operator (§2.5) and no implicit conversion to a TimeSpan
//       exists (§5.1), so it is REJECTED — on both shapes. Before the fix
//       the inferred shape computed it as a bare tick addition (5 ticks + 2
//       = 7 ticks: a silently wrong span).
//   A7  CONTROL: the inferred shape still computes `+` exactly as before.
//   A8  CONTROL: `<` is still refused, and the refusal still names
//       `TimeSpan` — §2.5 declares ordering as `CompareTo` only, so
//       refusing `ts < ts` is the specified behaviour (the issue's third
//       bullet), NOT a defect to "fix" by inventing an operator.
//   A9  CONTROL: the type-parameter wording is untouched (#3930): an
//       unconstrained `T` still gets the "not available on type parameter
//       `T`" wording and never "type `?`".
//  A10  CONTROL: the declared `+` still raises `OverflowException` past the
//       tick range (§2.5's OverflowException rule) — the fix must not have
//       disturbed the overflow arithmetic.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── A1 ─ the issue's shape: `ts + ts` over an ANNOTATED local compiles and
// computes (10 ticks), where the parent of this fix rejected it with
// "operator `+` is not defined for type `?`".
BEZEL_TEST(timespan_operator_operand_type_7547,
           plus_over_annotated_timespan_local_computes_stdlib_2_5) {
    auto r = runAot(
        "module M7547A; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var r := ts + ts; "
        "  Console.WriteLine($\"{r.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── A2 ─ the same over an ANNOTATED PARAMETER — the shape #7604 filed, and
// the one a method signature uses.
BEZEL_TEST(timespan_operator_operand_type_7547,
           plus_over_annotated_timespan_parameter_computes_stdlib_2_5) {
    auto r = runAot(
        "module M7547B; "
        "class C "
        "  public static method Add(a: TimeSpan, b: TimeSpan): TimeSpan "
        "    return a + b; "
        "  end Add; "
        "  public static method Main(): int "
        "    var r := Add(TimeSpan.FromTicks(5L), TimeSpan.FromTicks(5L)); "
        "    Console.WriteLine($\"{r.Ticks}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A3 ─ the `*`/`/` overloads over an annotated TimeSpan (§2.5's `double`
// factor): 5 ticks * 2.0 = 10 ticks, 10 ticks / 2.0 = 5 ticks.
BEZEL_TEST(timespan_operator_operand_type_7547,
           mul_and_div_over_annotated_timespan_compute_stdlib_2_5) {
    auto r = runAot(
        "module M7547C; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2.0; "
        "  var d := ts * 2.0 / 2.0; "
        "  Console.WriteLine($\"{m.Ticks} {d.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10 5\n"));
}

// ── A4 ─ the `?` is GONE: an annotated TimeSpan with no applicable overload
// names `TimeSpan` — the operand's type is no longer lost.
//
// FLIPPED by fix #7605 (spec §5.1, v0.355; §25.2, v0.350; stdlib §2.5,
// v0.350): this row used `* 2L` as its "no applicable overload" shape,
// because the arm demanded an EXACT `double` factor. §5.1 widens `long` to
// `double` IMPLICITLY, so `ts * 2L` now binds the operand to the declared
// `factor: double` parameter by the ordinary argument rules (§25.2) and
// COMPILES — 5 ticks * 2 = 10 ticks. The row's INTENT (the diagnostic names
// `TimeSpan`, never the `?` placeholder) is kept by A4b below, which uses a
// shape that still has no applicable overload.
BEZEL_TEST(timespan_operator_operand_type_7547,
           reject_names_the_timespan_operand_not_the_placeholder_spec_25) {
    auto r = runAot(
        "module M7547D; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * 2L; "
        "  Console.WriteLine($\"{m.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "a: ?, b: ?"));
}

// ── A4b ─ the row A4 kept: an annotated TimeSpan with NO applicable
// overload still names `TimeSpan` (never the `?` placeholder). `ts * ts`
// is that shape — stdlib §2.5 declares no `*(TimeSpan, TimeSpan)`, and
// §5.1 converts no type to `TimeSpan` (fix #7605 widened the FACTOR, not
// the operand pair).
BEZEL_TEST(timespan_operator_operand_type_7547,
           reject_names_the_timespan_operand_not_the_placeholder_spec_25_pair) {
    auto r = runAot(
        "module M7547D2; "
        "class C public static method Main(): int "
        "  var ts: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var m := ts * ts; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined for type `TimeSpan`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "a: ?, b: ?"));
}

// ── A5 ─ the same for the other §2.5 builtins: a `DateTime` operand names
// `DateTime`.
BEZEL_TEST(timespan_operator_operand_type_7547,
           reject_names_the_datetime_operand_not_the_placeholder_spec_25) {
    auto r = runAot(
        "module M7547E; "
        "class C public static method Main(): int "
        "  var d := new DateTime(2024, 1, 1); "
        "  var x := d + 2; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `DateTime`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── A6 ─ the over-acceptance the fix must not widen: `TimeSpan + int` is
// NOT a declared operator (§2.5 declares `+(a: TimeSpan, b: TimeSpan)`) and
// no implicit conversion to a TimeSpan exists (§5.1), so it is rejected —
// on the inferred shape too, which before the fix computed it as a bare tick
// addition (5 ticks + 2 = 7 ticks, a silently wrong span).
BEZEL_TEST(timespan_operator_operand_type_7547,
           timespan_plus_int_is_rejected_stdlib_2_5_spec_25_2) {
    auto r = runAot(
        "module M7547F; "
        "class C public static method Main(): int "
        "  var ts := TimeSpan.FromTicks(5L); "
        "  var r := ts + 2; "
        "  Console.WriteLine($\"{r.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `TimeSpan`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── A7 ─ CONTROL: the inferred shape still computes `+` exactly as before.
BEZEL_TEST(timespan_operator_operand_type_7547,
           plus_over_inferred_timespan_still_computes_stdlib_2_5) {
    auto r = runAot(
        "module M7547G; "
        "class C public static method Main(): int "
        "  var r := TimeSpan.FromTicks(5L) + TimeSpan.FromTicks(5L); "
        "  Console.WriteLine($\"{r.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// ── A8 ─ CONTROL: `<` is still refused and the refusal still names
// `TimeSpan`. stdlib §2.5 declares ordering as `CompareTo` only ("no
// <,>,<=,>= overloads are declared"), so refusing `ts < ts` is the
// specified behaviour — the issue's third bullet is NOT a defect, and no
// ordering operator may be invented for it.
BEZEL_TEST(timespan_operator_operand_type_7547,
           ordering_operator_still_refused_stdlib_2_5) {
    auto r = runAot(
        "module M7547H; "
        "class C public static method Main(): int "
        "  var ts := TimeSpan.FromTicks(5L); "
        "  var b := ts < ts; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `<` is not defined for type `TimeSpan`"));
}

// ── A9 ─ CONTROL: the type-parameter wording is untouched (#3930): an
// unconstrained `T` still gets the "not available on type parameter `T`"
// wording, and never "type `?`" — the fix names the STDLIB value builtins
// only.
BEZEL_TEST(timespan_operator_operand_type_7547,
           type_parameter_wording_unchanged_spec_25_2_11_8) {
    auto r = runAot(
        "module M7547I; "
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

// ── A10 ─ CONTROL: the declared `+` still raises `OverflowException` past
// the tick range (§2.5's OverflowException rule) — the fix must not have
// disturbed the overflow arithmetic.
BEZEL_TEST(timespan_operator_operand_type_7547,
           annotated_timespan_plus_past_max_raises_overflow_stdlib_2_5) {
    auto r = runAot(
        "module M7547J; "
        "class C public static method Main(): int "
        "  var m: TimeSpan := TimeSpan.FromTicks(long.MaxValue); "
        "  var b: TimeSpan := TimeSpan.FromTicks(7L); "
        "  try "
        "    var c := m + b; "
        "    Console.WriteLine($\"no raise {c}\"); "
        "  catch OverflowException => Console.WriteLine(\"OVF\"); "
        "  catch Exception => Console.WriteLine(\"OTHER\"); "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("OVF\n"));
}
