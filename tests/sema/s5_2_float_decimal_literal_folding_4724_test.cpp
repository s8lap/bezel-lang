// fix #4724 / §5.2 / §17.14 (v0.307): a float/double/decimal LITERAL operand
// (optionally under one unary `+`/`-`) of an explicit conversion to an integer
// target is decided at compile time.
//
// Per §5.2 (v0.307): "Floating-point to integer truncates toward zero; `NaN`,
// `±Infinity`, and out-of-range values raise `OverflowException`", and "where
// the operand of an explicit conversion is a compile-time constant ... whose
// value would raise `OverflowException` ... the conversion is a compile error
// at the conversion site" (`int(1e30)` is the spec's own example). The fold
// truncates toward zero FIRST and range-checks the truncated value, exactly as
// the runtime conversion does — so `byte(-0.5)` is 0 and admitted.
//
// `decimal` literals are folded exactly through the runtime literal parser
// `bz_decimal_parse_cstr` (96-bit mantissa / 10^scale in 128-bit integers), so
// `long(9223372036854775807m)` is long.MaxValue, not strtod's 2^63. `_` digit
// separators (Appendix A.1 DigitGroup) are stripped exactly as literal
// formation and codegen strip them.
//
// Authority: spec §5.2 / §17.14 / §4.2 / §4.3 / §4.4 / Appendix A.1, v0.307.

#include <string_view>

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndTypecheckGated;
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

// True iff a diagnostic carries the §5.2/§17.14 constant-operand markers:
// "constant" AND "OverflowException" — jointly they pin this fix's (c) arm.
bool hasConstantOverflowDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("constant") != std::string::npos &&
            d.message.find("OverflowException") != std::string::npos)
            return true;
    return false;
}

const char* kHeader =
    "module M; class C public static method Main(): int ";
const char* kFooter = " return 0; end Main; end C;";

const char* kHeaderFloat =
    "module M; class C public static method Main(): float ";

bezel::test::SemaResult check(const std::string& body) {
    return parseSemaAll(std::string(kHeader) + body + kFooter);
}

bezel::test::SemaResult checkFloat(const std::string& body) {
    return parseSemaAll(std::string(kHeaderFloat) + body + kFooter);
}

}  // namespace

// =====================================================================
// DECIMAL literals (`m`/`M` suffix) — REJECT.
// Each literal is parsed by bz_decimal_parse_cstr into a 96-bit mantissa
// (lo32 | mid32<<32 | hi32<<64) with 0..28 scale and a sign bit. The
// truncated integer is `mantissa / 10^scale`; a REJECT here means the
// truncated integer does not fit the target integer type per its min/max
// endpoints, computed exactly via __int128 / __uint128_t.
// =====================================================================

// #4724: `int(2147483648m)` — decimal value one past int.MaxValue (2^31).
// Exact arithmetic: mantissa=2147483648, scale=0, truncated=2147483648,
// exceeds int.MaxValue=2147483647 → REJECT.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_decimal_one_past_int_max_rejected) {
    auto r = check("return int(2147483648m);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `int(1e19m)` — decimal 10^19, five orders past int.MaxValue.
// Exact arithmetic rejects via the same integer-range gate.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_decimal_out_of_range_rejected) {
    auto r = check("return int(1e19m);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `long(9223372036854775808m)` — 2^63, one past long.MaxValue.
// Exact arithmetic: mantissa=9223372036854775808, scale=0, exceeds
// long.MaxValue = 2^63-1 → REJECT.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           long_of_decimal_one_past_max_rejected) {
    auto r = check("var v: long := long(9223372036854775808m); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `long(-9223372036854775809m)` — -(2^63+1), one past long.MinValue.
// Exact arithmetic: signed truncated value -9223372036854775809 is
// strictly less than long.MinValue = -(2^63). (strtod would round
// -(2^63+1) to -(2^63) and wrongly admit it.)
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           long_of_decimal_one_past_min_rejected) {
    auto r = check("var v: long := long(-9223372036854775809m); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `ulong(-1m)` — negative decimal to unsigned target.
// Exact arithmetic: neg=true after unary-unwrap (none here), rejected.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           ulong_of_decimal_negative_rejected) {
    auto r = check("var v: ulong := ulong(-1m); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `ulong(18446744073709551616m)` — 2^64, one past ulong.MaxValue.
// Exact arithmetic: mantissa=18446744073709551616, scale=0, NOT < 2^64
// → REJECT.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           ulong_of_decimal_one_past_max_rejected) {
    auto r = check("var v: ulong := ulong(18446744073709551616m); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `uint(1e19m)` — decimal 10^19 to unsigned int.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           uint_of_decimal_out_of_range_rejected) {
    auto r = check("return uint(1e19m);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `byte(1e2m)` exists as a 1e2 = 100 fit. But `byte(300m)` rejects
// because mantissa=300 doesn't fit byte's [0,255]. (Pure decimal path.)
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_decimal_out_of_range_rejected) {
    auto r = check("return byte(300m);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// =====================================================================
// DECIMAL literals — ACCEPT. The exact-arithmetic boundary tests:
// each is admitted BECAUSE the truncated mantissa fits the target
// exactly (a strtod-based fold would round 9223372036854775807m up
// to 2^63 and wrongly reject it).
// =====================================================================

// #4724: `int(2147483647m)` — int.MaxValue, fits as 2^31-1.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_decimal_at_int_max_accepted) {
    auto r = check("return int(2147483647m);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4724: `long(9223372036854775807m)` — long.MaxValue, fits as 2^63-1.
// strtod would round 9223372036854775807 to 2^63, which exceeds the long
// range; exact mantissa arithmetic admits the exact long.MaxValue.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           long_of_decimal_at_long_max_accepted) {
    auto r = check("var v: long := long(9223372036854775807m); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4724: `ulong(18446744073709551615m)` — ulong.MaxValue, fits as 2^64-1.
// Same shape as the long boundary, on the unsigned side.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           ulong_of_decimal_at_ulong_max_accepted) {
    auto r = check("var v: ulong := ulong(18446744073709551615m); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4724: `int(2147483648.0f)` — float 2^31, exactly representable, exceeds
// int.MaxValue. The most discriminating f-suffix REJECT row.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_f_suffix_at_2p31_rejected) {
    auto r = check("return int(2147483648.0f);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `byte(256.0f)` — float32 represents 256.0 exactly (it's a power
// of two); truncation gives 256, which exceeds byte.MaxValue=255.
// Per §4.3 the literal's type is `float`, rounded as codegen materialises
// it (`(float)strtod`); truncate-toward-zero
// (per §5.2 line 760) then produces 256 → REJECT.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_f_suffix_rounds_up_rejected) {
    auto r = check("return byte(256.0f);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `sbyte(-129.0f)` — float rounds -129.0 exactly; below sbyte.MinValue.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           sbyte_of_f_suffix_below_min_rejected) {
    auto r = check("return sbyte(-129.0f);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// =====================================================================
// DOUBLE literals — same boundary tests as the (a)/(b) arms extend.
// =====================================================================

// #4724: `int(1.0e30)` — bare double exceeds int.MaxValue.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_double_out_of_range_rejected) {
    auto r = check("return int(1.0e30);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724 amendment A (truncate-toward-zero): `int(2147483647.9)` rounds
// to 2147483647.9 (double), which truncates to 2147483647, fitting int.
// A raw `v <= upper` comparison on the un-truncated value would wrongly
// reject it.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_double_over_max_truncate_accepted) {
    auto r = check("return int(2147483647.9);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// FLOAT literals — ACCEPT.
// =====================================================================

// #4724: `int(2147483520.0f)` — float 2^31-128, exactly representable
// (exponent 30, mantissa 0x7FFFFF), truncates to itself, fits int. Note
// that `int(2147483647.0f)` itself REJECTS (the float32 ULP at 2^31 is
// 256, so 2147483647 rounds up to 2147483648 = 2^31 which doesn't fit
// int). The discriminating value at the float32 representable boundary
// is 2^31-128=2147483520 for the largest exactly-representable below 2^31.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_f_suffix_at_int_max_accepted) {
    auto r = check("return int(2147483520.0f);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #4724: `byte(255.0f)` — at byte.MaxValue.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_f_suffix_at_max_accepted) {
    auto r = check("return byte(255.0f);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// AMENDMENT B — unary `+`/`-` unwrap. Parser_Exprs.cpp emits a
// FloatLiteralExpr only for the bare literal token; `-1.0e30` parses
// as `UnaryExpr{"-", FloatLiteralExpr{"1.0e30"}}`; the fold looks through
// one such shell.
// =====================================================================

// #4724: `int(-1.0e30)` — unary negative double. Without unwrap the arm
// missed the inner FloatLiteralExpr; with unwrap, magnitude exceeds int.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_negative_double_rejected) {
    auto r = check("return int(-1.0e30);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `uint(-1.0)` — unary negative to unsigned target.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           uint_of_negative_double_rejected) {
    auto r = check("return uint(-1.0);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// =====================================================================
// Saturating carve-out: float(1e300) → float is Infinity (the correct
// §5.2 saturation), admitted per §5.2's "bounds" paragraph. The fix
// does not fire for a float/double TARGET (it only checks narrower
// integer targets; saturating float→float is correct runtime behavior).
// =====================================================================

// #4724: saturating `float(1e300)` → Infinity, accepted.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           float_saturating_accepted) {
    auto r = checkFloat("return float(1e300);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// Runtime-value rows (runAot per cycle-25 lesson on value-assertion
// oracles): the compile-time fold accepts and the runtime conversion
// truncates toward zero per §5.2; both paths agree on the boundary.
// =====================================================================

// #4724: `int(127.5f)` — 127.5 is exactly representable as a float
// (127.5 = 255/2, a power-of-2-denominator fraction); truncation
// toward zero per §5.2 yields 127; both compile-fold and runtime agree.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_f_suffix_truncate_to_zero_127_accepted_runtime_127) {
    auto r = runAot(
        std::string(kHeader) + "return int(127.5f);" + kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 127);
}

// #4724: `int(-128.9f)` — float value rounds to nearest float ~-128.9
// (or -128.9 itself, exactly representable in float); truncation toward
// zero yields -128; both compile-fold and runtime agree. Per cycle-25
// rule (`runAot.exitCode` is `WEXITSTATUS`-masked to the lower 8 bits,
// so a return of -128 reads as 128), observe via stdout instead.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_f_suffix_truncate_to_zero_neg128_accepted_runtime_neg128) {
    auto r = runAot(
        std::string(kHeader) +
        "var v := int(-128.9f);"
        "Console.WriteLine(int(v));"
        + kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("-128") != std::string::npos);
}

// =====================================================================
// Truncate-FIRST, then range-check (§5.2 v0.307: "Floating-point to
// integer truncates toward zero"). A negative fraction whose truncation
// is 0 fits every unsigned target; `-0.0m` is decimal zero. VALUE rows:
// compile + run + exact stdout, so a wrong reject (no run) and a wrong
// value both fail.
// =====================================================================

namespace {

// Compile and run `Console.WriteLine(<expr>);`, returning the result.
bezel::test::JitResult runPrint(const std::string& expr) {
    return runAot(std::string(kHeader) + "Console.WriteLine(" + expr +
                  ");" + kFooter);
}

}  // namespace

// #4724: `byte(-0.5)` truncates to 0 → prints 0 (not a compile reject).
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_negative_half_double_truncates_to_zero_runtime_0) {
    auto r = runPrint("byte(-0.5)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// #4724: `byte(-0.5m)` — decimal twin; truncation to 0 fits `byte`.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_negative_half_decimal_truncates_to_zero_runtime_0) {
    auto r = runPrint("byte(-0.5m)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// #4724: `ulong(-0.5m)` — widest unsigned target, same truncation.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           ulong_of_negative_half_decimal_truncates_to_zero_runtime_0) {
    auto r = runPrint("ulong(-0.5m)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// #4724: `uint(-0.9)` — |fraction| near 1 still truncates to 0.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           uint_of_negative_fraction_double_truncates_to_zero_runtime_0) {
    auto r = runPrint("uint(-0.9)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// #4724: `byte(-0.0m)` — negative-zero decimal spelling is zero.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_negative_zero_decimal_runtime_0) {
    auto r = runPrint("byte(-0.0m)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// =====================================================================
// `_` digit separators (Appendix A.1 DigitGroup) are stripped before the
// fold parses the literal, exactly as literal formation and codegen do.
// =====================================================================

// #4724: `int(1_0m)` is decimal 10 → prints 10.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_underscore_decimal_runtime_10) {
    auto r = runPrint("int(1_0m)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// #4724: `long(1_000_000.5m)` truncates to 1000000.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           long_of_underscore_decimal_fraction_runtime_1000000) {
    auto r = runPrint("long(1_000_000.5m)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000000\n"));
}

// #4724: `byte(2_56.0)` is 256.0 → exceeds byte.MaxValue → REJECT.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_underscore_double_256_rejected) {
    auto r = check("return int(byte(2_56.0));");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `byte(1_000.5)` truncates to 1000 → REJECT.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_underscore_double_fraction_rejected) {
    auto r = check("return int(byte(1_000.5));");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `int(3_000_000_000.0)` exceeds int.MaxValue → REJECT.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_underscore_double_3e9_rejected) {
    auto r = check("return int(3_000_000_000.0);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: `int(1e1_0)` — the separator is in the EXPONENT's DigitGroup;
// the value is 1e10 → REJECT (strtod stopping at `_` would read 1e1).
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_underscore_exponent_double_rejected) {
    auto r = check("return int(1e1_0);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// =====================================================================
// Double-domain range check: `long(1.0e19)` is a constant operand beyond
// long.MaxValue — rejected at compile time (the #739 runtime rows in
// codegen/numeric_conversion_test.cpp route the value through a local).
// =====================================================================

BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           long_of_double_1e19_rejected) {
    auto r = check("var c: long := long(1.0e19); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
}

// #4724: a literal already out of range at FORMATION (§4.3: `1e400` is a
// compile error, never ±Infinity) is not re-diagnosed by the conversion
// fold — exactly one error, the §4.3 formation error.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_formation_overflow_literal_not_rediagnosed) {
    auto r = check("return int(1e400);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstantOverflowDiag(r));
}

// #4724: the diagnostic names the operand WITH its sign — `-1.0`, not
// `1.0` — since the value being rejected is the negative one.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           byte_of_negative_one_diag_shows_sign) {
    auto r = check("return int(byte(-1.0));");
    BEZEL_EXPECT_TRUE(hasConstantOverflowDiag(r));
    bool signed_ = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`-1.0`") != std::string::npos) signed_ = true;
    BEZEL_EXPECT_TRUE(signed_);
}

// #4724: the decimal twin — a `decimal` literal beyond decimal.MaxValue is
// a §4.3/§4.4 formation error; the fold's parse faults and it skips, so no
// second (conversion) diagnostic is added.
BEZEL_TEST(s5_2_float_decimal_literal_folding_4724,
           int_of_formation_overflow_decimal_not_rediagnosed) {
    auto r = check("return int(99999999999999999999999999999999m);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstantOverflowDiag(r));
}
