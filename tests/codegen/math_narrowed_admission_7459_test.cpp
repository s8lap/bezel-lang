// Spec-derived regression test for issues #7461, #7460 and #7459 (this file: #7459) — the
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
// #7459 — an ILLEGAL call over a NARROWED nullable is refused, not aborted.
// ===========================================================================

// stdlib §2.2 declares `Sqrt(x: double)` only; a `decimal` never converts to
// `double` implicitly (§5.1) — the narrowed spelling reached codegen.
BEZEL_TEST(math_narrowed_admission_7459,
           narrowed_decimal_sqrt_is_refused_5_1_22_6) {
    auto r = runAot(prog(
        "    var m: decimal? := 2.5m; "
        "    if m <> null then "
        "      Console.WriteLine(Math.Sqrt(m)); "
        "    end;"));
    expectRefusedCleanly(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Math.Sqrt`"));
}

// The same at `Round`, `Pow` and `Fmod`.
BEZEL_TEST(math_narrowed_admission_7459,
           narrowed_decimal_round_pow_fmod_are_refused_5_1_22_6) {
    auto round = runAot(prog(
        "    var m: decimal? := 2.5m; "
        "    if m <> null then Console.WriteLine(Math.Round(m)); end;"));
    expectRefusedCleanly(round);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        round, "no matching overload of `Math.Round`"));
    auto pow = runAot(prog(
        "    var m: decimal? := 2.5m; "
        "    if m <> null then Console.WriteLine(Math.Pow(m, 2.0)); end;"));
    expectRefusedCleanly(pow);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        pow, "no matching overload of `Math.Pow`"));
    auto fmod = runAot(prog(
        "    var m: decimal? := 2.5m; "
        "    if m <> null then Console.WriteLine(Math.Fmod(m, 2.0)); end;"));
    expectRefusedCleanly(fmod);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        fmod, "no matching overload of `Math.Fmod`"));
}

// stdlib §2.3: `ToInt32(value: string)` — a `double` never reaches the
// `string` parameter. The issue's repro (a declaration-narrowed local).
BEZEL_TEST(math_narrowed_admission_7459,
           narrowed_double_to_int32_is_refused_5_1_22_6) {
    auto r = runAot(prog(
        "    var d: double? := 2.5; "
        "    Console.WriteLine(Convert.ToInt32(d));"));
    expectRefusedCleanly(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToInt32`"));
}

// The NAMED-argument spelling is adjudicated the same way.
BEZEL_TEST(math_narrowed_admission_7459,
           narrowed_double_to_int32_named_argument_is_refused_5_1_22_5) {
    auto r = runAot(prog(
        "    var d: double? := 2.5; "
        "    if d <> null then "
        "      Console.WriteLine(Convert.ToInt32(value := d)); "
        "    end;"));
    expectRefusedCleanly(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToInt32`"));
}

// `string?` and `bool?` narrowed at a numeric `Math` position.
BEZEL_TEST(math_narrowed_admission_7459,
           narrowed_string_and_bool_at_math_are_refused_5_1_22_6) {
    auto s = runAot(prog(
        "    var s: string? := \"x\"; "
        "    if s <> null then Console.WriteLine(Math.Abs(s)); end;"));
    expectRefusedCleanly(s);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        s, "no matching overload of `Math.Abs`"));
    auto b = runAot(prog(
        "    var b: bool? := true; "
        "    if b <> null then Console.WriteLine(Math.Floor(b)); end;"));
    expectRefusedCleanly(b);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        b, "no matching overload of `Math.Floor`"));
}

// CONTROL (the mirror of the new refusal): LEGAL narrowed calls stay
// accepted — a narrowed `int?` widens to `double` (§5.1), a narrowed
// `string?` is a `string`, a narrowed `double?` is a `double`.
BEZEL_TEST(math_narrowed_admission_7459,
           legal_narrowed_calls_stay_accepted_5_1_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var n: int? := 16; "
                        "    var s: string? := \"42\"; "
                        "    var d: double? := 2.5; "
                        "    Console.WriteLine(Math.Sqrt(n)); "
                        "    Console.WriteLine(Convert.ToInt32(s)); "
                        "    Console.WriteLine(Math.Floor(d));")),
                    "4\n42\n2");
}
