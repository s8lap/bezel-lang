// fix #4800 / §5.2 (v0.307): bz_decimal_to_int64 was sign-unaware — the magnitude
// check rejected long.MinValue (-9223372036854775808) as OVE even though it is exactly
// representable.  The fix makes the bound sign-aware (positive: INT64_MAX, negative:
// INT64_MAX+1) and negates via unsigned subtraction to avoid the signed overflow of
// INT64_MIN.

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// §5.2, #4800: long.MinValue decimal is representable and must not raise.
// BUG PIN — this row fails without the fix (pre-fix: OVE, post-fix: -9223372036854775808).
BEZEL_TEST(decimal_to_long_overflow_4800, decimal_to_long_min_value_accepted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := -9223372036854775808m; "
        "    var x: long := long(d); "
        "    Console.WriteLine($\"{x}\"); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "-9223372036854775808\n");
}

// §5.2, #4800: long.MinValue - 1 is out of range and must still raise OverflowException.
BEZEL_TEST(decimal_to_long_overflow_4800, decimal_to_long_below_min_value_throws) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := -9223372036854775809m; "
        "    try "
        "      var x: long := long(d); "
        "      return 1; "
        "    catch OverflowException => Console.WriteLine(\"OVE\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "OVE\n");
}

// §5.2: positive INT64_MAX is unaffected by the fix.
BEZEL_TEST(decimal_to_long_overflow_4800, decimal_to_long_max_value_accepted) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 9223372036854775807m; "
        "    var x: long := long(d); "
        "    Console.WriteLine($\"{x}\"); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "9223372036854775807\n");
}

// §5.2: INT64_MAX + 1 is out of range and must raise OverflowException.
BEZEL_TEST(decimal_to_long_overflow_4800, decimal_to_long_above_max_value_throws) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 9223372036854775808m; "
        "    try "
        "      var x: long := long(d); "
        "      return 1; "
        "    catch OverflowException => Console.WriteLine(\"OVE\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "OVE\n");
}
