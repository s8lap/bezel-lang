// Codegen — equality on whole inline-array values (`a = b` / `a <> b` where a
// and b are `[N]T` values).
//
// §8.3 (equality table): "Inline array `[N]T`, `[N]T?` … Element-by-element
// value equality (§15.1.2)". §8.4: "`a = b` invokes `a.Equals(b)`" and "`<>` is
// the negation". §15.1.2 (line 1948): "Two inline-array values of the same type
// are equal iff their elements are pairwise equal under the element type's
// equality. Inline arrays follow record-equality discipline: structural
// equality grounded in element equality, with no special cases."
//
// The element type's equality is recursive: §15.1.2 string elements compare by
// content (§4.6 / §8.3), record elements compare field-by-field (§9.1),
// primitive elements compare by value.
//
// Regression: previously `a = b` on two inline arrays aborted codegen with an
// LLVM `ICmpInst::AssertOK` assertion — the structural-equality dispatch was
// gated on LLVM StructType (records only); an inline array lowers to an LLVM
// ArrayType and fell through to the scalar integer ICmp path, which rejects an
// aggregate operand. These tests are load-bearing: each crashed before the fix.

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

// §15.1.2: two equal [2]int values compare equal → 1.
BEZEL_TEST(codegen_inline_array_eq, int2_equal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]int := [1, 2]; "
        "  var b: [2]int := [1, 2]; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2: differing in one slot → not equal → 0.
BEZEL_TEST(codegen_inline_array_eq, int2_unequal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]int := [1, 2]; "
        "  var b: [2]int := [1, 3]; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §15.1.2: a difference in the *first* slot is also detected (every slot
// participates, not just the last).
BEZEL_TEST(codegen_inline_array_eq, int2_unequal_first_slot_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]int := [9, 2]; "
        "  var b: [2]int := [1, 2]; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §15.1.2: arity-3 array, all slots equal → 1.
BEZEL_TEST(codegen_inline_array_eq, int3_equal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  var b: [3]int := [1, 2, 3]; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2: arity-3 array, middle slot differs → 0.
BEZEL_TEST(codegen_inline_array_eq, int3_unequal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  var b: [3]int := [1, 9, 3]; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §8.4: `<>` is the negation of `=`. Equal arrays → `<>` is false → 0.
BEZEL_TEST(codegen_inline_array_eq, int2_neq_equal_arrays_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]int := [4, 5]; "
        "  var b: [2]int := [4, 5]; "
        "  return (if a <> b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §8.4: `<>` on differing arrays → true → 1.
BEZEL_TEST(codegen_inline_array_eq, int2_neq_unequal_arrays_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]int := [4, 5]; "
        "  var b: [2]int := [4, 6]; "
        "  return (if a <> b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2 + §9.1: record elements compare field-by-field. Two [2]Point arrays
// with field-equal elements compare equal → 1.
BEZEL_TEST(codegen_inline_array_eq, record2_equal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a: [2]Point := [new Point(1, 2), new Point(3, 4)]; "
        "  var b: [2]Point := [new Point(1, 2), new Point(3, 4)]; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2 + §9.1: a differing record field in one slot → not equal → 0.
BEZEL_TEST(codegen_inline_array_eq, record2_unequal_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a: [2]Point := [new Point(1, 2), new Point(3, 4)]; "
        "  var b: [2]Point := [new Point(1, 2), new Point(3, 9)]; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §15.1.2: string elements compare by content (§4.6), not pointer identity.
// Two [2]string arrays whose slots hold content-equal strings compare equal.
// The strings are built at runtime (concatenation) so the pointers differ,
// ensuring the comparison is by content and not by interned-literal identity.
// `[2]string` carries an explicit initializer: `string` is not defaultable
// (§17.11, v0.307), so the uninitialised form is rejected (§15.1 / §7.9,
// v0.307; #4419 — pinned in definite_assignment_default_init_4419_test.cpp).
BEZEL_TEST(codegen_inline_array_eq, string2_equal_by_content_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]string := [\"\", \"\"]; a[0] := \"foo\"; a[1] := \"ba\" + \"r\"; "
        "  var b: [2]string := [\"\", \"\"]; b[0] := \"f\" + \"oo\"; b[1] := \"bar\"; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §15.1.2: string elements differing in content → not equal → 0.
// `[2]string` carries an explicit initializer: `string` is not defaultable
// (§17.11, v0.307), so the uninitialised form is rejected (§15.1 / §7.9,
// v0.307; #4419 — pinned in definite_assignment_default_init_4419_test.cpp).
BEZEL_TEST(codegen_inline_array_eq, string2_unequal_by_content_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [2]string := [\"\", \"\"]; a[0] := \"foo\"; a[1] := \"bar\"; "
        "  var b: [2]string := [\"\", \"\"]; b[0] := \"foo\"; b[1] := \"baz\"; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}
