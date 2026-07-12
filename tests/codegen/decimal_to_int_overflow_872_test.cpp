// fix #872 / §5.2: decimal→int casts with a value exceeding int64 range must
// raise OverflowException.  Before the fix, the ≤32-bit-target path called
// bz_decimal_to_int64 with a NULL vtable, so a decimal > int64.MaxValue (e.g.
// 1e19) wrapped to 0, passed the int32 IR range check, and returned 0 instead
// of raising.  The int64-target path (§5.2, fix #664) already used
// getOVEVtable()+emitPendingCheck() correctly; the ≤32-bit path now mirrors it.
//
// Value tests — runAot returns the raw int (kept in 0..255).

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

// §5.2, #872: int(1e19) — decimal exceeds int64 range → OverflowException.
BEZEL_TEST(decimal_to_int_overflow_872, int_from_decimal_beyond_int64_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 10000000000000000000m; "
        "    try var v: int := int(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2, #872: int(decimal.MaxValue) → OverflowException.
BEZEL_TEST(decimal_to_int_overflow_872, int_from_decimal_max_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := decimal.MaxValue; "
        "    try var v: int := int(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2, #872: int(decimal.MinValue) → OverflowException.
BEZEL_TEST(decimal_to_int_overflow_872, int_from_decimal_min_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := decimal.MinValue; "
        "    try var v: int := int(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2: int(3e9) — in int64 range but > int32 → existing IR range-check still
// raises OverflowException (regression: ensure the int32 check is preserved).
BEZEL_TEST(decimal_to_int_overflow_872, int_from_decimal_in_int64_out_of_int32_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 3000000000m; "
        "    try var v: int := int(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2: int(100m) → 100, no exception (happy path).
BEZEL_TEST(decimal_to_int_overflow_872, int_from_decimal_in_range_no_exception) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 100m; "
        "    try "
        "      var v: int := int(d); "
        "      if v = 100 then return 100; end; "
        "      return 201; "
        "    catch OverflowException => return 202; end; "
        "  end Main; "
        "end C;"), 100);
}

// §5.2: int(-100m) → -100 (returned as 156 = 256-100 via unsigned exit code).
BEZEL_TEST(decimal_to_int_overflow_872, int_from_negative_decimal_in_range_no_exception) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := -100m; "
        "    try "
        "      var v: int := int(d); "
        "      if v = -100 then return 156; end; "
        "      return 201; "
        "    catch OverflowException => return 202; end; "
        "  end Main; "
        "end C;"), 156);
}

// §5.2: long(1e19) — int64-target path unchanged, still raises OverflowException.
BEZEL_TEST(decimal_to_int_overflow_872, long_from_decimal_beyond_int64_raises_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 10000000000000000000m; "
        "    try var v: long := long(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2: long(100m) → 100 (in range, no exception).
BEZEL_TEST(decimal_to_int_overflow_872, long_from_decimal_in_range_no_exception) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 100m; "
        "    try "
        "      var v: long := long(d); "
        "      if v = 100L then return 100; end; "
        "      return 201; "
        "    catch OverflowException => return 202; end; "
        "  end Main; "
        "end C;"), 100);
}

// §5.2: byte(300m) — sub-range narrowing check still raises OverflowException.
BEZEL_TEST(decimal_to_int_overflow_872, byte_from_out_of_range_decimal_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 300m; "
        "    try var v: byte := byte(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2 Case C: double(1e19m) — floating-point cast is unaffected (no exception).
BEZEL_TEST(decimal_to_int_overflow_872, double_from_large_decimal_no_exception) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 10000000000000000000m; "
        "    try "
        "      var v: double := double(d); "
        "      if v > 9.0e18 then return 77; end; "
        "      return 201; "
        "    catch OverflowException => return 202; end; "
        "  end Main; "
        "end C;"), 77);
}
