// Spec-derived regression test for issues #7461, #7460 and #7459 (this file: #7461) — the
// operand ADMISSION and RESULT TYPE of a `Math` / `Convert` call over a
// §7.4-NARROWED nullable value (campaign 2, builtin-call-admission, lane
// C2-01).
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// spec §22.6 "Overloading": "**The argument's static type** is its type at the
//   call: a read of a binding §7.4 has narrowed non-null is of its
//   non-nullable type, as §7.11 reads it". So a narrowed `d: double?` is a
//   `double` at the call, and the call's RESULT is not nullable.
//
// spec §7.4 "How narrowing is established": a declaration `var x: T? := expr;`
//   with a non-nullable `expr` (the declaration's assignment), a null test
//   (`x <> null`), an early exit. Narrowing applies to LOCALS (and `readonly`
//   fields) and is invalidated by an assignment of a nullable value; a
//   parameter or a mutable field never narrows.
//
// spec §14.2: "nullable types do not themselves implement the interfaces of
//   their non-nullable counterparts" — so an UN-narrowed `double?` cannot bind
//   `T` of `Min<T>(a: T, b: T) where T: IComparable<T>` (stdlib §2.2).
//
// spec §5.1 "Nullable conversions": the lifting `T -> T?` is ONE-WAY; a `T?`
//   reaches no non-nullable parameter. stdlib §2.2 declares `Sqrt(x: double)`,
//   `Floor`, `Pow`, ... and no `Math` overload over `decimal` (except
//   `Abs`/`Floor`/`Ceiling`/`Truncate`/`Min`/`Max`/`Clamp`), `string` or
//   `bool`; stdlib §2.3 declares `ToInt32(value: string)`.
//
// ─── THE DEFECTS ────────────────────────────────────────────────────────────
//
// #7461: `if d <> null then Math.Max(d, 1.0)` was REFUSED by codegen's own
//   fail-closed guard ("`Math.Max` could not lower a comparison for this
//   argument type ... (#2786)"): the generic `Min`/`Max`/`Clamp` forms take
//   `T` from inference, so the §7 gate has no declared position to claim, and
//   the narrowed read's `{i8, double}` carrier reached the comparison.
// #7460: `var r: double := Math.Abs(d)` over a narrowed `d: double?` was
//   refused as "the initializer for `r` is nullable" — sema's polymorphic
//   result refinement copied the operand's DECLARED nullable type to the call.
// #7459: `Math.Sqrt(m)` over a narrowed `m: decimal?` and
//   `Convert.ToInt32(d)` over a narrowed `d: double?` are ILLEGAL (§5.1) but
//   the admission check skipped every nullable argument, so they reached
//   codegen and aborted the compiler in the LLVM verifier.
//
// The oracle is the program's OUTPUT (accept rows) or a located diagnostic
// that is NOT an IR-verifier abort (refuse rows).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A program with a nullable PARAMETER `x` (never narrowed, §7.4).
std::string paramProg(const std::string& type, const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    return F(null); "
           "  end Main; "
           "  public static method F(x: " + type + "): int " +
           body +
           "    return 0; "
           "  end F; "
           "end C;";
}

// A refusal: the driver rejected the program at a REAL location and the
// compiler did not crash (no verifier abort, no signal).
void expectRefusedCleanly(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
}

}  // namespace

// ===========================================================================
// #7461 — the generic Min / Max / Clamp / DivRem over a NARROWED nullable.
// ===========================================================================

// The issue's own repro (a null-test narrowing).
BEZEL_TEST(math_narrowed_generic_7461,
           null_test_narrowed_double_max_7_4_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    if d <> null then "
                        "      Console.WriteLine(Math.Max(d, 1.0)); "
                        "    end;")),
                    "2.5");
}

// The declaration's assignment narrows too (§7.4).
BEZEL_TEST(math_narrowed_generic_7461,
           declaration_narrowed_double_min_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Min(d, 1.0));")),
                    "1");
}

// The narrowed operand in the SECOND position and in BOTH positions.
BEZEL_TEST(math_narrowed_generic_7461,
           narrowed_operand_in_second_and_both_positions_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    var e: double? := 7.5; "
                        "    Console.WriteLine(Math.Max(1.0, d)); "
                        "    Console.WriteLine(Math.Max(d, e));")),
                    "2.5\n7.5");
}

// The RESULT of the generic selectors over a narrowed operand is the
// non-nullable `T` (§22.6) — assignable to a `double` and an inferred `var`.
BEZEL_TEST(math_narrowed_generic_7461,
           narrowed_double_max_result_is_non_nullable_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    var c: double := Math.Max(d, 1.0); "
                        "    var v := Math.Min(d, 9.0); "
                        "    var w: double := v; "
                        "    Console.WriteLine(c); "
                        "    Console.WriteLine(w);")),
                    "2.5\n2.5");
}

// `Clamp<T>(value, low, high)` over a narrowed value.
BEZEL_TEST(math_narrowed_generic_7461,
           narrowed_double_clamp_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Clamp(d, 0.0, 1.0));")),
                    "1");
}

// The other primitive carriers: int, float, decimal.
BEZEL_TEST(math_narrowed_generic_7461,
           narrowed_int_float_decimal_max_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var n: int? := 3; "
                        "    var f: float? := 3.5f; "
                        "    var m: decimal? := 3.5m; "
                        "    var a: int := Math.Max(n, 1); "
                        "    var b: float := Math.Max(f, 1.0f); "
                        "    var c: decimal := Math.Max(m, 1.0m); "
                        "    Console.WriteLine(a); "
                        "    Console.WriteLine(b); "
                        "    Console.WriteLine(c);")),
                    "3\n3.5\n3.5");
}

// An explicit type argument binds `T`; the narrowed operand converts at its
// conversion site.
BEZEL_TEST(math_narrowed_generic_7461,
           narrowed_double_max_explicit_type_argument_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 4.5; "
                        "    var r: double := Math.Max<double>(d, 1.0); "
                        "    Console.WriteLine(r);")),
                    "4.5");
}

// `Math.DivRem(a, b, out rem)` over narrowed `int?` operands.
BEZEL_TEST(math_narrowed_generic_7461,
           narrowed_int_divrem_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a: int? := 17; "
                        "    var b: int? := 5; "
                        "    var rem: int := 0; "
                        "    var q: int := Math.DivRem(a, b, out rem); "
                        "    Console.WriteLine(q); "
                        "    Console.WriteLine(rem);")),
                    "3\n2");
}

// §7.8: the null-forgiving read is the present value (unchanged).
BEZEL_TEST(math_narrowed_generic_7461,
           forced_double_max_7_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Max(d!, 1.0));")),
                    "2.5");
}

// CONTROL (the over-accept mirror): a nullable PARAMETER never narrows, so
// `T` cannot bind `double?` (§14.2) — still refused.
BEZEL_TEST(math_narrowed_generic_7461,
           nullable_parameter_double_max_stays_refused_14_2) {
    expectRefusedCleanly(runAot(paramProg(
        "double?", "    Console.WriteLine(Math.Max(x, 1.0)); ")));
}

// CONTROL: a narrowed local REASSIGNED from a nullable loses the narrowing.
BEZEL_TEST(math_narrowed_generic_7461,
           invalidated_narrowing_double_max_stays_refused_7_4) {
    expectRefusedCleanly(runAot(prog(
        "    var d: double? := 2.5; "
        "    d := null; "
        "    Console.WriteLine(Math.Max(d, 1.0));")));
}

// CONTROL: a null assignment INSIDE the narrowing branch invalidates it.
BEZEL_TEST(math_narrowed_generic_7461,
           reassigned_inside_null_test_double_max_stays_refused_7_4) {
    expectRefusedCleanly(runAot(prog(
        "    var d: double? := 4.5; "
        "    if d <> null then "
        "      d := null; "
        "      Console.WriteLine(Math.Max(d, 1.0)); "
        "    end;")));
}

// CONTROL: a mutable FIELD never narrows (§7.4).
BEZEL_TEST(math_narrowed_generic_7461,
           mutable_field_double_max_stays_refused_7_4) {
    expectRefusedCleanly(runAot(
        "module M; "
        "class C "
        "  public f: double?; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine(Math.Max(c.f, 1.0)); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

