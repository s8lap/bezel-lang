// Codegen tests for issue #1579 (v0.182) — §8.5 `ToString`:
//
//   "For inline arrays (all forms, including move-only `[N owned]…` arrays,
//    whose receiver is borrowed for the call) and for the unit type: the
//    type's name — `"[3]int"`-style for an inline array, `"()"` for unit
//    (v0.182 — previously unstated; element-listing output is deliberately
//    not synthesized, since an inline array's elements may be class-typed
//    borrows whose formatting is not the container's business)."
//
// Before this fix, neither case had a typed result at all in sema — a
// consuming `Console.WriteLine(arr.ToString())` / `Console.WriteLine(u.
// ToString())` silently no-op'd (the call typed to nothing, so codegen never
// reached it). Both results are compile-time-constant strings: N and the
// element type are static for an inline array, and unit has exactly one
// value, so no runtime array/receiver content is ever read.
//
// runJit executes and returns the program's exit code; these tests route
// the rendered string back through the exit code as its LENGTH (or a
// content comparison folded to 0/1) since JIT tests here compare integers,
// mirroring the project's existing string-content-via-comparison pattern.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §8.5: a fixed-size `[3]int` inline array's default `ToString` is
// "[3]int" — verified via `=` content comparison against the literal.
BEZEL_TEST(default_tostring_inline_array_unit_1579, inline_array_int_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]int := [1, 2, 3]; "
        "  return (1 if arr.ToString() = \"[3]int\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: the element-type name is included, and a NULLABLE element type is
// spelled with the trailing `?` — "[2]string?".
BEZEL_TEST(default_tostring_inline_array_unit_1579, inline_array_nullable_element_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [2]string? := [null, null]; "
        "  return (1 if arr.ToString() = \"[2]string?\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: rendering does NOT list elements — even non-default element values
// still produce the same type-name-only string.
BEZEL_TEST(default_tostring_inline_array_unit_1579, inline_array_tostring_not_element_listing) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]int := [9, 8, 7]; "
        "  return (1 if arr.ToString() = \"[3]int\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: the unit type's default `ToString` is the constant `"()"`.
BEZEL_TEST(default_tostring_inline_array_unit_1579, unit_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var u := (); "
        "  return (1 if u.ToString() = \"()\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: an inline array of a DIFFERENT element size still spells the correct
// N and element type — "[5]bool".
BEZEL_TEST(default_tostring_inline_array_unit_1579, inline_array_bool_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [5]bool := [true, false, true, false, true]; "
        "  return (1 if arr.ToString() = \"[5]bool\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5 / §18.5: the inline-array ToString composes with string
// concatenation (a `string` result usable like any other).
BEZEL_TEST(default_tostring_inline_array_unit_1579, inline_array_tostring_concatenates) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]int := [1, 2, 3]; "
        "  var s := \"n=\" + arr.ToString(); "
        "  return (1 if s = \"n=[3]int\" else 0); "
        "end Main; end C;"), 1);
}
