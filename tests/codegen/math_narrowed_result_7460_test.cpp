// Spec-derived regression test for issues #7461, #7460 and #7459 (this file: #7460) — the
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
// #7460 — the RESULT type of Abs over a NARROWED nullable.
// ===========================================================================

// The issue's initializer repro: the result is a `double`, not a `double?`.
BEZEL_TEST(math_narrowed_result_7460,
           narrowed_double_abs_result_is_non_nullable_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := -2.5; "
                        "    if d <> null then "
                        "      var r: double := Math.Abs(d); "
                        "      Console.WriteLine(r); "
                        "    end;")),
                    "2.5");
}

// The issue's first example: the call as a `Console.WriteLine` argument (a
// non-nullable parameter) over a declaration-narrowed local.
BEZEL_TEST(math_narrowed_result_7460,
           declaration_narrowed_double_abs_as_argument_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Abs(d));")),
                    "2.5");
}

// An inferred `var` takes the non-nullable type.
BEZEL_TEST(math_narrowed_result_7460,
           narrowed_double_abs_inferred_var_is_non_nullable_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := -2.5; "
                        "    var a := Math.Abs(d); "
                        "    var s: double := a; "
                        "    Console.WriteLine(s);")),
                    "2.5");
}

// The other polymorphic `Abs` operands: float and long.
BEZEL_TEST(math_narrowed_result_7460,
           narrowed_float_long_abs_results_are_non_nullable_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var f: float? := -4.5f; "
                        "    var l: long? := -3L; "
                        "    var a: float := Math.Abs(f); "
                        "    var b: long := Math.Abs(l); "
                        "    Console.WriteLine(a); "
                        "    Console.WriteLine(b);")),
                    "4.5\n3");
}

// A narrowed `decimal?` through `Abs` keeps its decimal result.
BEZEL_TEST(math_narrowed_result_7460,
           narrowed_decimal_abs_result_is_decimal_22_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var m: decimal? := -2.5m; "
                        "    var r: decimal := Math.Abs(m); "
                        "    Console.WriteLine(r);")),
                    "2.5");
}

// CONTROL: an UN-narrowed nullable PARAMETER is still refused (§7).
BEZEL_TEST(math_narrowed_result_7460,
           nullable_parameter_double_abs_stays_refused_7) {
    auto r = runAot(paramProg(
        "double?", "    var r: double := Math.Abs(x); "));
    expectRefusedCleanly(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Math.Abs` is nullable but the target type is "
           "non-nullable (§7)"));
}

// CONTROL: a narrowed local reassigned from a nullable is nullable again, and
// so is its Abs result (the argument is refused first).
BEZEL_TEST(math_narrowed_result_7460,
           invalidated_narrowing_double_abs_stays_refused_7_4) {
    auto r = runAot(prog(
        "    var d: double? := 2.5; "
        "    d := null; "
        "    var r: double := Math.Abs(d);"));
    expectRefusedCleanly(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Math.Abs` is nullable but the target type is "
           "non-nullable (§7)"));
}

// CONTROL: a mutable field never narrows.
BEZEL_TEST(math_narrowed_result_7460,
           mutable_field_double_abs_stays_refused_7_4) {
    expectRefusedCleanly(runAot(
        "module M; "
        "class C "
        "  public f: double?; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var r: double := Math.Abs(c.f); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// CONTROL: `d!` stays the accepted present-value read.
BEZEL_TEST(math_narrowed_result_7460,
           forced_double_abs_result_is_non_nullable_7_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d: double? := -2.5; "
                        "    var r: double := Math.Abs(d!); "
                        "    Console.WriteLine(r);")),
                    "2.5");
}

