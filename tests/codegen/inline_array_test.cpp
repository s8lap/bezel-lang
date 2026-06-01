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

// §15.1 + §9: array of records — element exprType is set via IndexExpr
// typing in sema so that member access on array elements (a[i].field)
// resolves correctly.
BEZEL_TEST(codegen_array, array_of_records_element_access) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var ps: [3]P; "
        "  ps[0] := new P(1, 2); ps[1] := new P(3, 4); ps[2] := new P(5, 6); "
        "  return ps[0].x + ps[1].y + ps[2].x; "
        "end Main; end C;"), 10);
}

BEZEL_TEST(codegen_array, array_of_records_sum_via_loop) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var ps: [3]P; "
        "  ps[0] := new P(1, 2); ps[1] := new P(3, 4); ps[2] := new P(5, 6); "
        "  var s := 0; "
        "  for var i in 0..2 do s := s + ps[i].x + ps[i].y; end; "
        "  return s; "
        "end Main; end C;"), 21);  // (1+2)+(3+4)+(5+6) = 21
}

BEZEL_TEST(codegen_array, array_of_records_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var ps: [2]P; "
        "  ps[0] := new P(1, 2); ps[1] := new P(1, 2); "
        "  return (1 if ps[0] = ps[1] else 0); "
        "end Main; end C;"), 1);
}

// Array passed by ref — element modifications visible to caller.
BEZEL_TEST(codegen_array, fill_via_ref_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Fill(ref a: [5]int, v: int) "
        "    for var i in 0..4 do a[i] := v; end; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var b: [5]int; Fill(ref b, 7); return b[2]; "
        "  end Main; "
        "end C;"), 7);
}

// Inline array as a field of a class instance.
BEZEL_TEST(codegen_array, array_as_class_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Container public data: [3]int; "
        "  public static method Main(): int "
        "    var c := new Container(); "
        "    c.data[0] := 10; c.data[1] := 20; c.data[2] := 30; "
        "    return c.data[0] + c.data[1] + c.data[2]; "
        "  end Main; "
        "end Container;"), 60);
}

// Binary search over a sorted inline array.
BEZEL_TEST(codegen_array, binary_search) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [8]int; "
        "  a[0]:=1; a[1]:=3; a[2]:=5; a[3]:=7; "
        "  a[4]:=9; a[5]:=11; a[6]:=13; a[7]:=15; "
        "  var target := 9; var lo := 0; var hi := 7; var found := -1; "
        "  while lo <= hi do "
        "    var mid := (lo + hi) / 2; "
        "    if a[mid] = target then found := mid; lo := hi + 1; "
        "    elif a[mid] < target then lo := mid + 1; "
        "    else hi := mid - 1; end; "
        "  end; "
        "  return found; "
        "end Main; end C;"), 4);
}

// §15.1: `for var x in a do … end` — iterating over inline array elements.
// The compiler emits a direct counted loop (0..N-1) loading a[i] into the
// loop variable at the start of each iteration. No IEnumerable protocol.

BEZEL_TEST(codegen_array, for_in_array_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; a[0]:=1; a[1]:=2; a[2]:=3; a[3]:=4; a[4]:=5; "
        "  var s := 0; for var x in a do s := s + x; end; return s; "
        "end Main; end C;"), 15);
}

BEZEL_TEST(codegen_array, for_in_array_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; a[0]:=3; a[1]:=7; a[2]:=1; a[3]:=9; a[4]:=4; "
        "  var m := 0; for var x in a do if x > m then m := x; end; end; "
        "  return m; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_array, for_in_array_count_in_range) {
    // Count elements of the array that fall in a range.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [6]int; a[0]:=1; a[1]:=5; a[2]:=3; a[3]:=8; a[4]:=2; a[5]:=7; "
        "  var cnt := 0; for var x in a do if x in 3..7 then cnt := cnt + 1; end; end; "
        "  return cnt; "
        "end Main; end C;"), 3);  // 5, 3, 7 are in 3..7
}

BEZEL_TEST(codegen_array, for_in_array_with_break) {
    // Find the first element above a threshold and exit.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; a[0]:=2; a[1]:=4; a[2]:=7; a[3]:=1; a[4]:=9; "
        "  var found := -1; "
        "  for var x in a do if x > 5 then found := x; break; end; end; "
        "  return found; "
        "end Main; end C;"), 7);
}
