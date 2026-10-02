// fix #5194 (§4.3/§5.2, v0.312): a `float` literal is rounded once, to nearest
// with ties to even.  Every `float` literal site was materialising
// `(float)strtod(text)` — double rounding (decimal → nearest double → nearest
// float) — which silently produces a different value for magnitudes in the
// so-called "double-rounding window" near float.MaxValue and near 1.0.
// Fix: use strtof directly at all six sites (sema range check, sema fold,
// codegen constant materialisation × 3, module-loader imported const).
//
// Edge band: 3.4028235677973366e38f = float.MaxValue — today a formation
// error (strtod lands on the tie 2^128 − 2^103 and the cast gives inf).
//
// Misrounding: 1.0000001788139343f = 1.00000012f — today False (strtod lands
// on the midpoint 1 + 3·2^−24 and the cast gives 1 + 2^−22).
//
// Round-trip: float(7.0385306918512e-26); x.ToString() = 7.038531e-26;
// float.Parse(x.ToString()) = x — the one float magnitude whose shortest form
// double-rounds.
//
// Guards: float.MaxValue edge accepts; just-beyond rejects; double.MaxValue
// accepts and rejects.
//
// Authority: spec §4.3 / §5.2, v0.312.

#include <string_view>

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;
using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bezel::test::SemaResult check(std::string_view body) {
    return parseSemaAll(std::string(
        "module M; class C public static method Main(): int ") +
        std::string(body) + " return 0; end Main; end C;");
}

// Compile and run `Console.WriteLine(<boolExpr>);`, returning stdout.
bezel::test::JitResult runBoolPrint(std::string_view boolExpr) {
    return runAot(std::string(
        "module M; class C public static method Main(): int\n"
        "Console.WriteLine(") +
        std::string(boolExpr) +
        std::string(");\n"
                    "return 0;\n"
                    "end Main; end C;"));
}

}  // namespace

// =====================================================================
// §4.3 edge band — the float.MaxValue boundary.
// A `float` literal at exactly the double-rounding window boundary must
// fold to float.MaxValue and be accepted (§4.3 formation: only out-of-range
// when the correctly-rounded value is not finite).
// =====================================================================

// #5194 edge band: 3.4028235677973366e38f (≈ 2^128 − 2^103 − 2^74) — strtod
// lands on the tie 2^128 − 2^103; the cast (float)strtod gives inf, so the
// formation check rejects it.  strtof lands on float.MaxValue directly.
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           edge_band_float_max_value_literal_accepted) {
    auto r = check("return int(3.4028235677973366e38f = float.MaxValue);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #5194: same boundary expressed as the exact 128-bit integer form
// (2^128 − 2^103 − 2^74).  Must also be accepted.
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           edge_band_float_max_value_exact_integer_form_accepted) {
    auto r = check(
        "return int(340282356779733642748073463979561713664f = float.MaxValue);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// §4.3 misrounding — the 1.0 + midpoint double-rounding window.
// 1.0000001788139343 (≈ 1 + 3·2^−24) is exactly at the midpoint between
// two adjacent float values; strtod rounds it to the nearest double, then
// the cast rounds that to the next float UP (ties-to-even applies at each
// step independently), giving 1 + 2^−22 ≈ 1.00000012f.  strtof rounds it
// once to the nearest float, which is 1.00000012f.
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           misrounding_1_plus_3_2_minus_24_equals_1_00000012f) {
    auto r = runBoolPrint(
        "1.0000001788139343f = 1.00000012f");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
}

// =====================================================================
// §4.3 / §5.2 round-trip: the one float magnitude whose shortest
// decimal representation double-rounds.
// 7.0385306918512e-26 is exactly representable as a float; its shortest
// decimal form that round-trips is 7.038531e-26.  float.Parse of that
// string is a runtime concern; the compile-time fold is verified here.
// =====================================================================

// #5194: compile-time literal 7.038531e-26f must fold to the same value as
// the original 7.0385306918512e-26f (same float, different spelling).
// Before the fix: strtod lands on 7.0385306918512e-26 as a double, then
// the cast rounds it to a different float value.  After the fix: strtof
// rounds once to the correct float value.
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           round_trip_7_038531e_minus_26_literal_equals_original) {
    auto r = runBoolPrint(
        "7.038531e-26f = 7.0385306918512e-26f");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
}

// =====================================================================
// §4.3 / §5.2 guards — values that must be accepted or rejected as
// documented in the issue.  These are not changed by the fix; they
// verify the fix did not regress the existing boundaries.
// =====================================================================

// Guard: just below float.MaxValue — accepted.
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           guard_float_max_value_minus_one_ulp_accepted) {
    auto r = check("return int(3.4028235e38f = float.MaxValue);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Guard: just above float.MaxValue — rejected (formation error).
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           guard_just_above_float_max_value_rejected) {
    auto r = check("return int(3.40282357e38f);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Guard: double.MaxValue — accepted.
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           guard_double_max_value_accepted) {
    auto r = check("return int(1.7976931348623158e308 = double.MaxValue);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Guard: just above double.MaxValue — rejected.
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           guard_just_above_double_max_value_rejected) {
    auto r = check("return int(1.7976931348623159e308);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Guard: float(float.MaxValue) — accepted (float.MaxValue fits in float).
BEZEL_TEST(s4_3_float_literal_rounding_5194,
           guard_float_of_float_max_value_literal_accepted) {
    auto r = check("return int(float(3.4028235e38) = float.MaxValue);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
