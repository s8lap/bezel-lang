// Codegen + runtime — Math constants/rounding and Convert.TryToInt32, filling
// gaps against the standard library spec:
//
//   stdlib §2.2 Math:
//     public static const Pi:  double := 3.14159265358979323846;
//     public static const Tau: double := 6.28318530717958647692;
//     public static method Round(x: double): double;            // banker's rounding
//     public static method Round(x: double, digits: int): double;
//
//   stdlib §2.3 Convert:
//     public static method TryToInt32(value: string, out result: int): bool;
//
// Before this milestone the compiler implemented `Math.PI` (the wrong spelling —
// the spec constant is `Pi`, §2.2 line "public static const Pi") and had no
// `Tau`, no two-argument `Round`, and round-half-AWAY-from-zero for one-argument
// `Round` (the spec mandates banker's rounding, i.e. round-half-to-EVEN). It
// also lacked `Convert.TryToInt32`. These tests pin the spec-stated behavior.
//
// Three layers (this milestone):
//   - Codegen: Math.Pi / Math.Tau constants and Round(x)/Round(x,digits) banker's
//     rounding in LlvmBackend_Calls.cpp + the constant member-access path in
//     LlvmBackend_Exprs.cpp; Convert.TryToInt32 out-param marshalling in
//     LlvmBackend_Calls.cpp.
//   - Sema: typing of Math.Pi/Math.Tau (double) and Convert.TryToInt32 (bool)
//     in ExprTypecheck_Calls.cpp.
//   - Runtime: none needed (all map to LLVM intrinsics / libc strtol).
//
// Value tests run through the JIT harness (exit code = Main()'s return). Since
// the JIT reads the raw int return (not the 8-bit-masked process exit status),
// tests can return values above 255 directly.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

bool hasSemaError(const std::string& src) {
    auto r = runJit(src);
    return !r.ran && r.diagnostics.hasErrors();
}

std::string mainBody(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// #1037 — Math and Convert are distinct classes; cross-class and unknown
// statics must be REJECTED (§3.4), not silently miscompiled.
//
// The sema accept logic used to type the UNION of both classes' methods on
// either class (one `cls == "Convert" || cls == "Math"` block), and the
// builtin-class path (null decl) had no fallback "no such member" reject. So
// `Math.ToInt32(5)` (→45), `Convert.Sqrt(16.0)` (→0) and even `Math.Bogus(5)`
// (→garbage) compiled and miscompiled. They must now be compile errors.
// ===========================================================================

// Convert-only method on Math → reject.
BEZEL_TEST(math_convert_1037, math_toint32_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: int := Math.ToInt32(5); return x;")));
}

// Math-only method on Convert → reject.
BEZEL_TEST(math_convert_1037, convert_sqrt_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: double := Convert.Sqrt(16.0); return int(x);")));
}

// Fully-unknown static on a builtin class → reject (was an unrejected hole).
BEZEL_TEST(math_convert_1037, math_unknown_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: int := Math.Bogus(5); return x;")));
}
BEZEL_TEST(math_convert_1037, convert_unknown_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: int := Convert.Bogus(5); return x;")));
}

// Positive controls — real methods on the right class must STILL work.
BEZEL_TEST(math_convert_1037, math_sqrt_still_works) {
    BEZEL_EXPECT_EQ(run(mainBody("var x: double := Math.Sqrt(16.0); return int(x);")), 4);
}
BEZEL_TEST(math_convert_1037, math_min_still_works) {
    BEZEL_EXPECT_EQ(run(mainBody("return Math.Min(3, 8);")), 3);
}
BEZEL_TEST(math_convert_1037, math_clamp_still_works) {
    BEZEL_EXPECT_EQ(run(mainBody("return Math.Clamp(5, 1, 4);")), 4);
}
BEZEL_TEST(math_convert_1037, convert_toint32_still_works) {
    BEZEL_EXPECT_EQ(run(mainBody("var x: int := Convert.ToInt32(7); return x;")), 7);
}

// --- Math.Pi (stdlib §2.2, "public static const Pi: double := 3.14159...") ---
// The spec spelling is `Pi`. Returns 1 if Pi is within (3.14, 3.15).
BEZEL_TEST(math_convert, math_pi_constant_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p := Math.Pi; "
        "  return (1 if p > 3.14 and p < 3.15 else 0); "
        "end Main; end C;"), 1);
}

// --- Math.Tau (stdlib §2.2, "public static const Tau: double := 6.28318...") ---
// Tau = 2*Pi. Returns 1 if Tau is within (6.28, 6.29) and ~2*Pi.
BEZEL_TEST(math_convert, math_tau_constant_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t := Math.Tau; "
        "  var d := t - 2.0 * Math.Pi; "
        "  return (1 if t > 6.28 and t < 6.29 and Math.Abs(d) < 0.0001 else 0); "
        "end Main; end C;"), 1);
}

// --- Math.Round(x): banker's rounding (round half to even) — stdlib §2.2 ---
// Round(2.5) = 2, Round(3.5) = 4, Round(0.5) = 0, Round(1.5) = 2.
// Encodes the four results as a single integer: r25*1000 + r35*100 + r05*10 + r15.
// Expected: 2*1000 + 4*100 + 0*10 + 2 = 2402.
BEZEL_TEST(math_convert, math_round_bankers_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r25 := int(Math.Round(2.5)); "
        "  var r35 := int(Math.Round(3.5)); "
        "  var r05 := int(Math.Round(0.5)); "
        "  var r15 := int(Math.Round(1.5)); "
        "  return r25 * 1000 + r35 * 100 + r05 * 10 + r15; "
        "end Main; end C;"), 2402);
}

// --- Math.Round(x): ordinary (non-half) cases round to nearest — stdlib §2.2 ---
// Round(2.4) = 2, Round(2.6) = 3, Round(-2.6) = -3.
// Encode: (r24 + 10) * 100 + (r26 + 10) * 10 + (r_neg + 10)
//       = 12*100 + 13*10 + 7 = 1337.
BEZEL_TEST(math_convert, math_round_nearest_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r24 := int(Math.Round(2.4)); "
        "  var r26 := int(Math.Round(2.6)); "
        "  var rn  := int(Math.Round(0.0 - 2.6)); "
        "  return (r24 + 10) * 100 + (r26 + 10) * 10 + (rn + 10); "
        "end Main; end C;"), 1337);
}

// --- Math.Round(x, digits): round to N fractional digits — stdlib §2.2 ---
// Round(3.14159, 2) = 3.14, Round(3.14159, 4) = 3.1416.
// Returns 1 if Round(3.14159, 2) is within (3.139, 3.141) and
//                Round(3.14159, 4) is within (3.1415, 3.1417).
BEZEL_TEST(math_convert, math_round_digits_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := Math.Round(3.14159, 2); "
        "  var b := Math.Round(3.14159, 4); "
        "  return (1 if a > 3.139 and a < 3.141 and b > 3.1415 and b < 3.1417 else 0); "
        "end Main; end C;"), 1);
}

// --- Math.Round(x, digits): banker's rounding at the digit boundary — §2.2 ---
// Round(2.125, 2): 2.125 * 100 = 212.5, round-half-to-even -> 212, / 100 = 2.12.
// Assert the result lands in (2.119, 2.121).
BEZEL_TEST(math_convert, math_round_digits_bankers_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := Math.Round(2.125, 2); "
        "  return (1 if a > 2.119 and a < 2.121 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: success — stdlib §2.3 ---
// TryToInt32("42", out n) returns true and sets n = 42.
// Return 1 if ok and n == 42, else 0.
BEZEL_TEST(math_convert, convert_tryto_int32_ok_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  var ok := Convert.TryToInt32(\"42\", out n); "
        "  return (1 if ok and n = 42 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: failure — stdlib §2.3 ---
// TryToInt32("abc", out n) returns false (no exception). Return 1 if not ok.
BEZEL_TEST(math_convert, convert_tryto_int32_fail_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  var ok := Convert.TryToInt32(\"abc\", out n); "
        "  return (1 if not ok else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: trailing junk is a failure — stdlib §2.3 ---
// "12x" is not a fully-valid integer, so TryToInt32 returns false.
BEZEL_TEST(math_convert, convert_tryto_int32_trailing_junk_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  var ok := Convert.TryToInt32(\"12x\", out n); "
        "  return (1 if not ok else 0); "
        "end Main; end C;"), 1);
}
