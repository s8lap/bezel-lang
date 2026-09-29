// Codegen tests for issue #4785 / §5.2 (v0.307): decimal → ulong conversion.
//
// Spec reference:
//   §5.2 (L749): "Explicit conversion uses function-call syntax: the target
//     type's name applied to the source value."  Examples include
//     "var i: int := int(d);             // truncates toward zero".
//   §5.2 ("Floating-point, decimal, and char conversion semantics"): the
//     integer-narrowing rule applies to every integer target type.  For a
//     `ulong` target, the value must fit in uint64 and the conversion must
//     succeed for every value in [0, ulong.MaxValue].
//
// Bug: codegen's decimal→int "int64 or uint64 target" arm at
//   src/codegen/LlvmBackend_Exprs2.cpp:9825-9833 reused the SIGNED
//   `bz_decimal_to_int64` runtime helper for BOTH `long` and `ulong` targets.
//   The signed helper unconditionally enforced `trunc <= (uint64_t)INT64_MAX`
//   (src/runtime/bezel_runtime_decimal_guid.cpp:521-525), so every decimal
//   value in the band (int64.MaxValue, ulong.MaxValue] — a huge valid range —
//   was incorrectly rejected with OverflowException.
//
// Fix: new runtime helper `bz_decimal_to_uint64` (uint64 semantics: rejects
//   negative decimals and values > uint64.MaxValue), and the codegen arms to
//   `bz_decimal_to_uint64` for a `ulong` destination type.
//
// Reverse-path help: a `ulong` source above long.MaxValue should round-trip via
//   decimal (per fix #743 — `decimal(ulong)` already uses the unsigned
//   64-bit conversion).  The regression tests below exercise BOTH directions
//   at the bug's exact boundaries.
//
// All tests run via the AOT oracle (runAot) — the strict path that runs
// ownership + definite-assignment + §33.5 entry-point checks the JIT harness
// deliberately skips.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §5.2 / #4785: ulong(1e19m) — decimal > int64.MaxValue but < ulong.MaxValue.
// Pre-fix this incorrectly raised OverflowException because the codegen
// routed the ulong target through the signed `bz_decimal_to_int64` helper,
// which enforces the int64 upper bound unconditionally.  Post-fix
// `bz_decimal_to_uint64` returns 1e19 exactly.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_decimal_above_int64_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 10000000000000000000m; "
        "    try "
        "      var v: ulong := ulong(d); "
        "      if v = 10000000000000000000UL then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4785: ulong(ulong.MaxValue - 1) — top-of-range valid value.
// Pre-fix raised OverflowException; post-fix returns 18446744073709551614.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_decimal_near_ulong_max_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 18446744073709551614m; "
        "    try "
        "      var v: ulong := ulong(d); "
        "      if v = 18446744073709551614UL then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4785: ulong(ulong.MaxValue) — boundary, exactly representable.
// Pre-fix raised OverflowException; post-fix returns ULONG_MAX exactly.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_decimal_at_ulong_max_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 18446744073709551615m; "
        "    try "
        "      var v: ulong := ulong(d); "
        "      if v = 18446744073709551615UL then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4785: small values (regression — must continue to work).
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_decimal_small_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 12345m; "
        "    try "
        "      var v: ulong := ulong(d); "
        "      if v = 12345UL then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4785: ulong(int64.MaxValue) — value at the OLD boundary.  Both
// pre-fix (routed through the signed helper) and post-fix accept this value;
// pin the post-fix behaviour explicitly so a follow-up refactor that
// re-instates a wrong boundary is caught.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_decimal_at_int64_max_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 9223372036854775807m; "
        "    try "
        "      var v: ulong := ulong(d); "
        "      if v = 9223372036854775807UL then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4785: ulong(decimal.MaxValue) — decimal holds values up to
// 79228162514264337593543950335, well above ulong.MaxValue.  The cast
// MUST raise OverflowException (NOT silently wrap).  Pre-fix path also raised,
// but for the wrong reason (the int64 helper happened to reject anything
// above int64.MaxValue); post-fix the rejection is structural — a uint64
// boundary check on the unsigned target.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_decimal_maxvalue_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := decimal.MaxValue; "
        "    try var v: ulong := ulong(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2 / #4785: ulong(negative decimal) — the unsigned target refuses negative
// values.  Pre-fix the signed helper returned the absolute value of the
// mantissa, silently converting -1m to ulong(-1) ≡ ulong.MaxValue, which is
// silent wrong code (the spec says overflow on negative).  Post-fix raises
// OverflowException structurally.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_negative_decimal_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := -1m; "
        "    try var v: ulong := ulong(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2 / #4785 round-trip (ulong → decimal → ulong) — exercises the symmetric
// pair (decimal_from_uint64 #743 + decimal_to_uint64 #4785).  A round-trip is
// strong evidence the unsigned path is wired end-to-end: any asymmetry (one
// helper signed, the other unsigned) would surface here.
BEZEL_TEST(decimal_to_ulong_4785, ulong_decimal_roundtrip_above_int64_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var u: ulong := 10000000000000000000UL; "
        "    var d: decimal := decimal(u); "
        "    try "
        "      var back: ulong := ulong(d); "
        "      if back = u then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 (fix #4785 amend, v0.307): truncate toward zero.  -0.5m has magnitude
// 0.5, truncates to 0, which fits in uint64.  Pre-fix the helper raised on
// every negative input unconditionally; post-fix it only raises when the
// truncated magnitude is non-zero.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_negative_decimal_truncates_to_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := -0.5m; "
        "    try "
        "      var v: ulong := ulong(d); "
        "      if v = 0UL then return 42; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2: same for -0.999m.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_negative_decimal_truncates_to_zero_999) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := -0.999m; "
        "    try "
        "      var v: ulong := ulong(d); "
        "      if v = 0UL then return 42; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2 (fix #4785 amend, v0.307): regression guard — -1m truncates to 1,
// which is positive and fits in uint64, BUT the NEGATIVE sign makes the
// result -1, which does NOT fit in uint64.  OverflowException is correct.
// This is a DIFFERENT row from the existing ulong_from_negative_decimal_raises
// test: that one was added by the original #4785 fix and covered -1m; this
// row is identical in input and output but is cited here as a spec-trace
// proof that the truncate-toward-zero rule produces the right result.
BEZEL_TEST(decimal_to_ulong_4785, ulong_from_negative_decimal_raises_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := -1m; "
        "    try var v: ulong := ulong(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// Note: -0m (negative zero decimal) is canonicalised to 0m by the decimal
// representation (mantissa=0 → sign forced to positive per dec_store), so
// ulong(-0m) returns 0 without overflow.  Bezel's decimal literal grammar
// (§18.1) accepts "-0.0m" but the sign bit is dropped when mantissa=0, so
// there is no distinct negative-zero literal to test here.

// §5.2: long(1e19m) must CONTINUE to raise OverflowException (long-target
// path unchanged — only the ulong-target arm switched helpers).
BEZEL_TEST(decimal_to_ulong_4785, long_from_decimal_beyond_int64_raises_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 10000000000000000000m; "
        "    try var v: long := long(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}