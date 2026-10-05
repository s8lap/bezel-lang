// §15.1.2 / §17.5 / #1342: inline-array ToArray() and Contains() intrinsics.
//
// §15.1.2 specifies two member intrinsics on `[N]T`:
//   ToArray(): owned Array<T>   — copies N slots into a fresh heap Array<T>.
//     · value types: copied by value.
//     · string elements: +1 retain per element (Array<string> co-ownership).
//     · class/interface elements: pointer-copy (borrow, no disposal).
//   Contains(item: T): bool     — linear scan with element-type §9.2 equality:
//     · value types: `=` by value.
//     · strings: content equality (§4.6, bz_str_eq).
//     · class/interface: reference identity (§8.3), or Equals override.
//     · records: structural equality (§9.1).
//
// §17.5 also allows `item in inline_arr` as sugar for Contains.
//
// Note: §15.1 requires inline-array size > 0; `[0]T` is a sema error.
//
// runJit returns the full int from Main(); values <= 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- ToArray: int elements ---------------------------------------------------

// §15.1.2 / #1342: ToArray() on [5]int copies 5 elements; Length = 5.
BEZEL_TEST(inline_array_intrinsics_1342, to_array_int_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [10, 20, 30, 40, 50]; "
        "  using var h: Array<int> := arr.ToArray(); "
        "  return h.Count; "
        "end Main; end C;"), 5);
}

// §15.1.2 / #1342: element values are preserved in order.
BEZEL_TEST(inline_array_intrinsics_1342, to_array_int_element_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [4]int := [7, 14, 21, 28]; "
        "  using var h: Array<int> := arr.ToArray(); "
        "  return h.Get(0) + h.Get(2); "   // 7 + 21 = 28
        "end Main; end C;"), 28);
}

// §15.1.2 / #1342: mutating the source after ToArray does not affect the copy.
BEZEL_TEST(inline_array_intrinsics_1342, to_array_int_independent_copy) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]int := [1, 2, 3]; "
        "  using var h: Array<int> := arr.ToArray(); "
        "  arr[0] := 99; "
        "  return h.Get(0); "   // still 1, not 99
        "end Main; end C;"), 1);
}

// --- ToArray: string elements ------------------------------------------------

// §15.1.2 / #1342: ToArray() on [3]string copies 3 refs; Length = 3.
BEZEL_TEST(inline_array_intrinsics_1342, to_array_string_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]string := [\"a\", \"b\", \"c\"]; "
        "  using var h: Array<string> := arr.ToArray(); "
        "  return h.Count; "
        "end Main; end C;"), 3);
}

// §15.1.2 / #1342: string element values are correct (content equality).
BEZEL_TEST(inline_array_intrinsics_1342, to_array_string_element_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]string := [\"hello\", \"world\", \"bezel\"]; "
        "  using var h: Array<string> := arr.ToArray(); "
        "  if h.Get(0) = \"hello\" and h.Get(2) = \"bezel\" then return 1; "
        "  else return 0; end; "
        "end Main; end C;"), 1);
}

// §15.1.2 / #1342: using-var disposal of Array<string> releases the +1 string
// retains without crashing (strings are in the literal pool with sentinel
// refcounts, so release is a no-op for them; the test verifies no crash).
BEZEL_TEST(inline_array_intrinsics_1342, to_array_string_using_var_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [2]string := [\"x\", \"y\"]; "
        "  using var h: Array<string> := arr.ToArray(); "
        "  return 42; "
        "end Main; end C;"), 42);
}

// --- Contains: int elements --------------------------------------------------

// §15.1.2 / #1342: Contains finds a present element (linear scan, value eq).
BEZEL_TEST(inline_array_intrinsics_1342, contains_int_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [10, 20, 30, 40, 50]; "
        "  if arr.Contains(30) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §15.1.2 / #1342: Contains returns false for an absent element.
BEZEL_TEST(inline_array_intrinsics_1342, contains_int_not_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [10, 20, 30, 40, 50]; "
        "  if arr.Contains(99) then return 1; else return 0; end; "
        "end Main; end C;"), 0);
}

// §15.1.2 / #1342: Contains(first element) -- short-circuit on first.
BEZEL_TEST(inline_array_intrinsics_1342, contains_int_first_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [4]int := [7, 8, 9, 10]; "
        "  if arr.Contains(7) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §15.1.2 / #1342: Contains on [1]int with a non-matching value → false.
// (§15.1 requires size > 0; this is the smallest valid array.)
BEZEL_TEST(inline_array_intrinsics_1342, contains_single_not_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [1]int := [42]; "
        "  if arr.Contains(0) then return 1; else return 0; end; "
        "end Main; end C;"), 0);
}

// --- Contains: string elements (content equality, §4.6 / §15.1.2) -----------

// §15.1.2 + §4.6 / #1342: Contains on [N]string uses string content equality.
BEZEL_TEST(inline_array_intrinsics_1342, contains_string_content_eq_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]string := [\"apple\", \"banana\", \"cherry\"]; "
        "  if arr.Contains(\"banana\") then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §15.1.2 + §4.6 / #1342: absent string -> false.
BEZEL_TEST(inline_array_intrinsics_1342, contains_string_not_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]string := [\"apple\", \"banana\", \"cherry\"]; "
        "  if arr.Contains(\"mango\") then return 1; else return 0; end; "
        "end Main; end C;"), 0);
}

// --- `in` operator (§17.5 sugar for Contains) --------------------------------

// §17.5 / §15.1.2 / #1342: `x in arr` on an inline int array.
BEZEL_TEST(inline_array_intrinsics_1342, in_op_int_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [10, 20, 30, 40, 50]; "
        "  if 30 in arr then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §17.5 / §15.1.2 / #1342: `x in arr` -- element not found.
BEZEL_TEST(inline_array_intrinsics_1342, in_op_int_not_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]int := [10, 20, 30, 40, 50]; "
        "  if 99 in arr then return 1; else return 0; end; "
        "end Main; end C;"), 0);
}

// §17.5 / §15.1.2 / #1342: `x in arr` on inline string array (content eq).
BEZEL_TEST(inline_array_intrinsics_1342, in_op_string_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]string := [\"a\", \"b\", \"c\"]; "
        "  if \"b\" in arr then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §17.5 / §15.1.2 / #1342: `x in arr` -- string not found.
BEZEL_TEST(inline_array_intrinsics_1342, in_op_string_not_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]string := [\"a\", \"b\", \"c\"]; "
        "  if \"z\" in arr then return 1; else return 0; end; "
        "end Main; end C;"), 0);
}

// --- Combined usage (ToArray + Array.Contains chain) ------------------------

// §15.1.2 / #1342: ToArray then use the result's own Contains method.
BEZEL_TEST(inline_array_intrinsics_1342, to_array_then_contains) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [4]int := [3, 6, 9, 12]; "
        "  using var h: Array<int> := arr.ToArray(); "
        "  if h.Contains(9) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}
