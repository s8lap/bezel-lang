// Spec-derived regression test for issues #7232 and #7418 — a NULLABLE
// argument at a `Math` / `Convert` position.
//
// spec §7 "Nullability" (v0.343) and §7.3 "Assigning null" (v0.343);
// spec §22.6 "Overloading" (v0.343);
// stdlib §2.2 `Math`; stdlib §2.3 `Convert`.
//
// WHAT THE DEFECTS WERE.
//
// (1) #7418 — `Math.Abs(null)` ABORTED THE COMPILER with malformed IR:
//
//     Both operands to ICmp instruction are not of the same type!
//     %abs.ismin = icmp eq ptr null, i0 0
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
//     stdlib §2.2 declares `Abs(x: int): int` (and the `long` / `double` /
//     `decimal` forms) and declares NO nullable overload, so the call matches
//     no declared form.
//
// (2) #7232 — the §7 NULLABILITY REGISTRY carried rows for the `Convert`
//     positions but NOT for `Math`'s, so a nullable argument at a `Math`
//     position was never claimed by the §7 gate and reached codegen, where
//     the whole nullable box was loaded where a scalar was expected:
//
//     Call parameter type does not match function signature!
//       %1 = load { i8, double }, ptr %0, align 8
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// THE RULE.
//
// §7.3 (v0.343): "Assigning `null` to a non-nullable target is a compile
// error." §7.4 (v0.343) is the ONE narrowing that discharges it, and §22.6
// (v0.343) reads the argument's static type AT THE CALL ("a read of a
// binding §7.4 has narrowed non-null is of its non-nullable type").
//
// So an argument that is still `T?` at the call reaches NO non-nullable
// parameter of the declared forms (§5.1's nullable lifting converts a `T` to
// a `T?`, never the other way), and the compiler must SAY SO at the call —
// never abort inside the LLVM verifier.
//
// THE FIX.
//
// The `Math` / `Convert` declared surface lives in the ONE built-in
// parameter table (`common::kBuiltinParams`), and that table now carries
// each declared form's parameter NULLABILITY (§7.3) beside its parameter
// TYPE (§5.1) and its parameter NAME (§22.5) — so the §7 nullability gate
// claims the `Math` positions exactly as it already claimed the `Convert`
// ones, and the refusal is the house §7 diagnostic.
//
// NOTE ON THE NARROWED CASE.
//
// A §7.4-NARROWED read (`var d: double? := 2.5;` — the declaration's
// assignment narrows it, §7.4) is LEGAL at a `double` parameter, so it is
// NOT refused here; it must compile and run. Its lowering is the OTHER half
// of the defect family (the `Math` call lowering loads the whole nullable
// box instead of extracting the payload) and lives in `src/codegen`, which
// this lane does not own — so no row here pins that shape. The rows below
// pin the UN-narrowed shapes, which are the ones the §7 gate owns.

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

// The §7 refusal: the program did not run, the driver rejected it at a REAL
// source location (never a crash / signal / timeout / ASan report), the
// diagnostic carries the house §7 wording, and the pre-fix symptom — the
// compiler ABORTING inside the LLVM verifier — is GONE.
void expectRefusedBySection7(const JitResult& r, const char* member) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the argument to `") + member +
            "` is nullable but the target type is non-nullable (§7)")
                .c_str()));
    // The pre-fix symptom: the compiler aborted inside the LLVM verifier.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
}

}  // namespace

// ===========================================================================
// #7418 — the `null` LITERAL at a `Math` position.
// ===========================================================================

// The issue's own repro.
BEZEL_TEST(math_convert_nullability_7232_7418,
           math_abs_null_literal_is_refused_7_not_22_6) {
    auto r = runAot(prog("    Console.WriteLine(Math.Abs(null));"));
    expectRefusedBySection7(r, "Math.Abs");
}

// The same shape at the other declared `Math` positions: every one of them
// declares a NON-nullable parameter, so the `null` literal reaches none.
BEZEL_TEST(math_convert_nullability_7232_7418,
           math_sqrt_null_literal_is_refused_7) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sqrt(null));"));
    expectRefusedBySection7(r, "Math.Sqrt");
}

BEZEL_TEST(math_convert_nullability_7232_7418,
           math_floor_null_literal_is_refused_7) {
    auto r = runAot(prog("    Console.WriteLine(Math.Floor(null));"));
    expectRefusedBySection7(r, "Math.Floor");
}

BEZEL_TEST(math_convert_nullability_7232_7418,
           math_pow_null_literal_is_refused_7) {
    auto r = runAot(prog("    Console.WriteLine(Math.Pow(null, 2.0));"));
    expectRefusedBySection7(r, "Math.Pow");
}

// ===========================================================================
// #7232 — a STILL-NULLABLE argument at a `Math` position.
// ===========================================================================

// A local declared `double?` and initialized with `null`: §7.4's "the
// declaration's assignment" narrows only a NON-nullable initializer, so this
// read is still `double?` at the call.
BEZEL_TEST(math_convert_nullability_7232_7418,
           math_floor_unnarrowed_nullable_is_refused_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Floor(d));"));
    expectRefusedBySection7(r, "Math.Floor");
}

// A nullable PARAMETER is never narrowed (§7.4: "Narrowing applies to
// locals — and `readonly` fields — only"), so the read is `double?` at the
// call whatever the caller passed.
BEZEL_TEST(math_convert_nullability_7232_7418,
           math_abs_nullable_parameter_is_refused_7) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(F(2.5)); "
        "    return 0; "
        "  end Main; "
        "  public static method F(d: double?): double "
        "    return Math.Abs(d); "
        "  end F; "
        "end C;");
    expectRefusedBySection7(r, "Math.Abs");
}

// The DECIMAL rounding family declares a `decimal` form (stdlib §2.2), and
// a nullable `decimal` reaches neither it nor the `double` one.
BEZEL_TEST(math_convert_nullability_7232_7418,
           math_abs_unnarrowed_nullable_decimal_is_refused_7) {
    auto r = runAot(prog(
        "    var n: decimal? := null; "
        "    Console.WriteLine(Math.Abs(n));"));
    expectRefusedBySection7(r, "Math.Abs");
}

// ===========================================================================
// The `Convert` positions — the §7 gate already claimed them; they must keep
// their own diagnostic and not acquire a second one.
// ===========================================================================

// The control the #7218 test file already pins, kept here so the `Math`
// rows above and the `Convert` rows below are read as ONE rule.
BEZEL_TEST(math_convert_nullability_7232_7418,
           convert_toint32_null_literal_is_refused_7) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToInt32(null));"));
    expectRefusedBySection7(r, "Convert.ToInt32");
}

// `Convert.ToString`'s VALUE position is declared non-nullable (stdlib §2.3
// declares `ToString(value: int): string` and its siblings), so a nullable
// value reaches no declared form there.
BEZEL_TEST(math_convert_nullability_7232_7418,
           convert_tostring_unnarrowed_nullable_is_refused_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Convert.ToString(d));"));
    expectRefusedBySection7(r, "Convert.ToString");
}

// ===========================================================================
// The CONTROLS — the conforming spellings are unaffected.
// ===========================================================================

BEZEL_TEST(math_convert_nullability_7232_7418,
           math_abs_double_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Abs(-2.5));")),
                    "2.5");
}

BEZEL_TEST(math_convert_nullability_7232_7418,
           math_floor_double_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Floor(2.5));")),
                    "2");
}

BEZEL_TEST(math_convert_nullability_7232_7418,
           math_pow_double_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Pow(2.0, 10.0));")),
                    "1024");
}

BEZEL_TEST(math_convert_nullability_7232_7418,
           convert_tostring_int_argument_still_works_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Convert.ToString(255));")),
                    "255");
}
