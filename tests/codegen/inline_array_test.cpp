// Codegen slice CG-array — inline arrays `[N]T` (§15.1).
//
// Inline arrays are value-typed, fixed-size, stored inline in their
// enclosing context (stack frame). No heap allocation, no Dispose.
// `[N]T` lowers to an LLVM `[N x elemType]` alloca (zero-initialized
// on declaration without an initializer, per §15.1).
//
// Element read `a[i]` → GEP + load.
// Element write `a[i] := v` → GEP + store (via lvalueAddr).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Basic read/write to int array elements.
BEZEL_TEST(codegen_array, int_read_write) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; a[0] := 10; a[1] := 20; "
        "  return a[0] + a[1]; "
        "end Main; end C;"), 30);
}

// §15.1: inline arrays are zero-initialized on declaration.
BEZEL_TEST(codegen_array, zero_initialized) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; return a[2]; "
        "end Main; end C;"), 0);
}

// Fill an array using a for-in loop, then read back.
BEZEL_TEST(codegen_array, loop_fill_and_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; "
        "  for var i in 0..4 do a[i] := i * 2; end; "
        "  return a[3]; "
        "end Main; end C;"), 6);
}

// Sum all elements of an array.
BEZEL_TEST(codegen_array, sum_all_elements) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; "
        "  for var i in 0..4 do a[i] := i + 1; end; "
        "  var s := 0; "
        "  for var i in 0..4 do s := s + a[i]; end; "
        "  return s; "
        "end Main; end C;"), 15);
}

// Byte array element round-trip via explicit cast.
BEZEL_TEST(codegen_array, byte_array) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [4]byte; a[0] := byte(42); return int(a[0]); "
        "end Main; end C;"), 42);
}

// Array element updated inside a while loop.
BEZEL_TEST(codegen_array, while_loop_accumulate) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int; "
        "  var i := 0; while i < 3 do a[i] := i * i; i := i + 1; end; "
        "  return a[0] + a[1] + a[2]; "
        "end Main; end C;"), 5);  // 0 + 1 + 4
}

// Array as accumulator in computing max via a for loop.
BEZEL_TEST(codegen_array, findmax_via_array) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; "
        "  a[0] := 3; a[1] := 7; a[2] := 2; a[3] := 9; a[4] := 4; "
        "  var m := 0; "
        "  for var i in 0..4 do if a[i] > m then m := a[i]; end; end; "
        "  return m; "
        "end Main; end C;"), 9);
}
