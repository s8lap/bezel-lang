// Spec-derived regression tests for fix #747 (R42 WRONG_VALUE) — unary negation
// of an unsigned integer.
//
// Authority: §6.1 — "Operators +, -, *, /, mod, unary -, ++, and -- on integer
// types are checked for overflow. An operation whose mathematical result is not
// representable in the result type raises OverflowException."
//
// For an UNSIGNED type, `-x` is mathematically negative and representable only
// when x == 0; any non-zero value overflows. Bug: codegen only checked the
// signed INT_MIN case, so `-uint(1)` silently wrapped to uint.MaxValue instead
// of raising OverflowException. The fix adds the unsigned `x != 0` overflow
// guard while leaving signed negation (overflow only at INT_MIN) unchanged.
//
// The test programs CATCH the exception and return a sentinel; the sentinel is
// the observed exit code.
//
// Harness: migrated to the AOT path (runAot, support/aot_support.hpp) as the
// demonstrative swap for the JIT->AOT test-harness migration (plan §2, P0).
// runAot compiles + links a native executable via the real bezelc driver and
// runs it as a subprocess, so an unhandled Bezel exception would fail only that
// subprocess (not the whole test process). All expected values here are in
// 0..255, so they round-trip the 8-bit process exit status faithfully; parity
// with the prior runJit run is exact (verified: 6/6 identical exit codes).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Negate `value` of unsigned type `ty`, catching OverflowException → 7.
std::string negCatch(const char* ty, const char* value) {
    return std::string(
        "module M; static class F public static method Main(): int "
        "  var a: ") + ty + " := " + value + "; "
        "  try var b := -a; return int(b); "
        "  catch OverflowException => return 7; end; "
        "end Main; end F;";
}
}  // namespace

// §6.1: -uint(1) overflows (result -1 not representable in uint).
BEZEL_TEST(unsigned_negate_747, uint_nonzero_overflows) {
    BEZEL_EXPECT_EQ(run(negCatch("uint", "1")), 7);
}

// §6.1: -ulong(1) overflows.
BEZEL_TEST(unsigned_negate_747, ulong_nonzero_overflows) {
    BEZEL_EXPECT_EQ(run(negCatch("ulong", "1")), 7);
}

// §6.1: -byte(5) overflows (byte is unsigned 8-bit).
BEZEL_TEST(unsigned_negate_747, byte_nonzero_overflows) {
    BEZEL_EXPECT_EQ(run(negCatch("byte", "5")), 7);
}

// §6.1: -ushort(5) overflows.
BEZEL_TEST(unsigned_negate_747, ushort_nonzero_overflows) {
    BEZEL_EXPECT_EQ(run(negCatch("ushort", "5")), 7);
}

// §6.1: -uint(0) is 0 — the one representable case, no exception.
BEZEL_TEST(unsigned_negate_747, uint_zero_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var a: uint := 0; var b := -a; return int(b); "  // 0
        "end Main; end F;"), 0);
}

// §6.1: signed negation is unaffected — -5 + 12 = 7 (no overflow).
BEZEL_TEST(unsigned_negate_747, signed_negate_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var a: int := 5; var b := -a; return b + 12; "  // 7
        "end Main; end F;"), 7);
}
