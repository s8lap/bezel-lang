// fix #859 / §5.4: an integer-to-enum cast `E(n)` was not range-checked against
// the enum's underlying type. A runtime `E(n)` with `n` outside the underlying
// range silently wrapped (e.g. byte-backed `Color(300)` -> 44, `Color(-1)` ->
// 255) instead of raising OverflowException. lowerEnumCast now range-checks the
// value on a narrowing using the underlying type's signedness, mirroring the
// int->int narrowing guard. (The compile-time-constant case is rejected in sema
// — see tests/sema/enum_cast_const_range_859_test.cpp.)
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kColor =
    "module M; enum Color: byte Red := 0, Green := 1, Blue := 2; end Color; ";
}  // namespace

// Runtime byte-backed enum cast above range -> OverflowException (was 44).
BEZEL_TEST(enum_cast_range_859, runtime_byte_above_range_throws) {
    BEZEL_EXPECT_EQ(run(std::string(kColor) +
        "class C public static method Main(): int "
        "  try var n: int := 300; var c: Color := Color(n); return int(c); "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// Runtime byte-backed enum cast below range (negative) -> OverflowException
// (was 255).
BEZEL_TEST(enum_cast_range_859, runtime_byte_negative_throws) {
    BEZEL_EXPECT_EQ(run(std::string(kColor) +
        "class C public static method Main(): int "
        "  try var n: int := -1; var c: Color := Color(n); return int(c); "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// Runtime short-backed enum cast above range -> OverflowException (was wrapped).
BEZEL_TEST(enum_cast_range_859, runtime_short_above_range_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum HttpStatus: short Ok := 200; end HttpStatus; "
        "class C public static method Main(): int "
        "  try var n: int := 40000; var s: HttpStatus := HttpStatus(n); return 1; "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// Regression: an in-range runtime cast is unaffected.
BEZEL_TEST(enum_cast_range_859, runtime_in_range_ok) {
    BEZEL_EXPECT_EQ(run(std::string(kColor) +
        "class C public static method Main(): int "
        "  var n: int := 200; var c: Color := Color(n); "
        "  var m: int := 2; var c2: Color := Color(m); "
        "  return int(c) + int(c2); "          // 202
        "end Main; end C;"), 202);
}

// Regression: a default int-backed enum still accepts a normal int.
BEZEL_TEST(enum_cast_range_859, default_int_backed_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum E A := 0, B := 1, C := 2; end E; "
        "class C public static method Main(): int "
        "  var n: int := 100000; var e: E := E(n); return int(e) - 99958; "  // 42
        "end Main; end C;"), 42);
}
