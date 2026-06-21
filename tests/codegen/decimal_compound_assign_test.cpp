// Codegen — discovery sweep #6: compound assignment and increment on a `decimal`
// local miscompiled. `decimal` is a Kind::Primitive (an {i32,i32,i32,i32}
// struct), so the user-defined-operator guards on the compound-assign and
// increment paths (which require Kind::Named) missed it, and the value fell
// through to the integer emitCompoundOp path — feeding the struct to an integer
// overflow intrinsic:
//   d += / -= / *=  → "Intrinsic has incorrect return type" (IR_FAIL)
//   d /=            → bezelc SIGABRT (LLVM ICmpInst type-mismatch assertion)
//   d++ / d--       → "Call parameter type does not match" (IR_FAIL)
//
// THE FIX: a new emitDecimalArith helper (mirrors the decimal binary-op lowering:
// div/mod zero-guard + bz_decimal_<op> with the OverflowException vtable) is
// invoked from the compound-assign and increment paths when the target type is
// the decimal struct. §6.4 decimal arithmetic.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// d += (was IR_FAIL).
BEZEL_TEST(decimal_compound_assign, plus_equals) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 1.0m; d += 2.0m; "
        "  return (0 if d.CompareTo(3.0m) = 0 else 1); "
        "end Main; end C;"), 0);
}

// d /= (was a bezelc SIGABRT) + d -= and d *= chained.
BEZEL_TEST(decimal_compound_assign, div_sub_mul_equals) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 12.0m; d /= 4.0m; "  // 3
        "  d -= 1.0m; "                            // 2
        "  d *= 5.0m; "                            // 10
        "  return (0 if d.CompareTo(10.0m) = 0 else 1); "
        "end Main; end C;"), 0);
}

// d++ and d-- (were IR_FAIL).
BEZEL_TEST(decimal_compound_assign, increment_decrement) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 1.0m; d++; d++; d--; "  // 2
        "  return (0 if d.CompareTo(2.0m) = 0 else 1); "
        "end Main; end C;"), 0);
}

// d /= 0 must still raise DivideByZeroException (the helper keeps the §6.4 guard).
BEZEL_TEST(decimal_compound_assign, div_by_zero_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 10.0m; var z: decimal := 0.0m; "
        "  try d /= z; return 1; catch DivideByZeroException => return 0; end; "
        "end Main; end C;"), 0);
}
