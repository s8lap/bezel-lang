// Spec-derived tests for fix #746 (R41 clusters H and I) — implicit numeric
// conversions in a value-flow position.
//
// Authority: §5.1 — byte/sbyte/short/ushort implicitly widen to int (and wider
// integers); int (and the other integer types) implicitly convert to decimal.
//
// Pre-fix: (H) a narrow integer value (a `byte` indexer result or local)
// returned from a wider-int method was `ret`'d at its own width → IR-verify
// failure (`ret i8` in an i32 function). (I) an implicit int→decimal
// (`var m: decimal := i;`) stored the raw integer bits into the 16-byte decimal
// struct instead of calling the runtime conversion → a misrepresented value
// that failed equality with a decimal literal.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §5.1 (H): a byte indexer result returned from an int method widens.
BEZEL_TEST(implicit_numeric_746, byte_indexer_widens_in_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var arr: Array<byte> := [42]; "
        "  return arr[0]; "  // byte → int widening in return position
        "end Main; end C;"), 42);
}

// §5.1 (H): a byte local (value ≥ 128) widens unsigned on return.
BEZEL_TEST(implicit_numeric_746, byte_local_widens_unsigned_in_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method ByteLocal(): int var b: byte := 200; return b; end ByteLocal; "
        "  public static method Main(): int return ByteLocal(); "  // 200, not -56
        "end Main; end C;"), 200);
}

// §5.1 (H): an sbyte negative local sign-extends on return.
BEZEL_TEST(implicit_numeric_746, sbyte_local_sign_extends_in_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(): int var s: sbyte := -7; return s; end F; "
        "  public static method Main(): int return (1 if F() = -7 else 0); "
        "end Main; end C;"), 1);
}

// §5.1 (I): an implicit int→decimal equals the decimal literal.
BEZEL_TEST(implicit_numeric_746, implicit_int_to_decimal) {
    BEZEL_EXPECT_EQ(run(
        "module P; class M public static method Main(): int "
        "  var i: int := 42; "
        "  var m: decimal := i; "
        "  return (1 if m = 42.0m else 0); "
        "end Main; end M;"), 1);
}

// §5.1 (I): an implicit byte→decimal also converts correctly.
BEZEL_TEST(implicit_numeric_746, implicit_byte_to_decimal) {
    BEZEL_EXPECT_EQ(run(
        "module P; class M public static method Main(): int "
        "  var b: byte := 5; "
        "  var d: decimal := b; "
        "  return (1 if d = 5.0m else 0); "
        "end Main; end M;"), 1);
}

// §5.1 (I): an implicit long→decimal (a value above the int range) converts.
BEZEL_TEST(implicit_numeric_746, implicit_long_to_decimal) {
    BEZEL_EXPECT_EQ(run(
        "module P; class M public static method Main(): int "
        "  var n: long := 9000000000L; "
        "  var d: decimal := n; "
        "  return (1 if d = 9000000000.0m else 0); "
        "end Main; end M;"), 1);
}
