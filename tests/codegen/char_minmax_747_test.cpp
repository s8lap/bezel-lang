// Spec-derived regression tests for fix #747 (R42 WRONG_VALUE) —
// `char.MinValue` / `char.MaxValue`.
//
// Authority: Bezel_Standard_Library.md §char —
//   public static const MinValue: char;  // U+0000
//   public static const MaxValue: char;  // U+10FFFF (1114111)
//
// Bug: the primitive-constant lowering in LlvmBackend_Exprs.cpp handled
// int/long/short/byte/float/double MinValue/MaxValue but had no `P::Char` case,
// so `char.MinValue` / `char.MaxValue` fell through and the variable read an
// uninitialized alloca (emitting no store). Char is a 32-bit code point (i32);
// MinValue = 0, MaxValue = 1114111.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §char: MinValue is U+0000 = 0.
BEZEL_TEST(char_minmax_747, min_value_is_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return int(char.MinValue); "  // 0
        "end Main; end C;"), 0);
}

// §char: MaxValue is U+10FFFF = 1114111 (compared, since it exceeds an 8-bit
// exit code).
BEZEL_TEST(char_minmax_747, max_value_is_10ffff) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if int(char.MaxValue) = 1114111 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §char: both constants materialize independently — range width is
// MaxValue - MinValue = 1114111.
BEZEL_TEST(char_minmax_747, range_width) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return int(char.MaxValue) - int(char.MinValue) - 1114110; "  // 1
        "end Main; end C;"), 1);
}
