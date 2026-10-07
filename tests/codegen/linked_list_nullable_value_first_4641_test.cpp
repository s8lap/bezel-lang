// Codegen — issue #4641: `LinkedList<T?>.First`/`.Last` emit malformed IR for a
// nullable VALUE element (Guid?, TimeOnly?, …), failing IR verification.
//
// THE DEFECT. In `LlvmBackend_Exprs.cpp`'s LinkedList First/Last property
// lowering, for a nullable value element type (T?), the node's value slot
// already stores the full `{i8, payload}` nullable struct (the element type
// IS the nullable). However, the code loaded with `elemLLTy` (which is the
// LLVM type for the element type — the nullable struct) and then called
// `wrapNullableValue` AGAIN, producing:
//   %69 = insertvalue { i8, %bz.guid } { i8 1, %bz.guid undef }, { i8, %bz.guid } %ll.val, 1
// i.e., wrapping an already-tagged optional — IR verification rejects this.
//
// THE FIX. When the element type IS a nullable value type, the node slot
// already holds the complete nullable struct; return it directly. Only wrap
// when the element type is non-nullable (the bare inner type) and needs
// tagging.
//
// SPEC: §7.2 (nullable value types), stdlib §4.6 (LinkedList<T>.First/Last
// return T?), v0.307.
//
// FAIL-WITHOUT: reverse-applying the fix and rebuilding reproduces:
//   Invalid InsertValueInst operands!
//   codegen error: IR verification failed

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// per §4.6: LinkedList<T>.First and .Last return T?.
// The primary symptom was an IR verification failure at compile time. This test
// verifies the fix by checking compilation succeeds and runtime values are correct.
BEZEL_TEST(linked_list_nullable_value_first_4641, first_last_guid_nullable) {
    // The issue's own repro: LinkedList<Guid?>.First/.Last compile and produce
    // correct values. Pre-fix: IR verification failure at compile time.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Guid?>(); "
        "  ll.AddLast(Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff00\")); "
        "  ll.AddLast(Guid.Parse(\"aabbccdd-eeff-0011-2233-445566778899\")); "
        "  var f: Guid? := ll.First; "
        "  var l: Guid? := ll.Last; "
        "  if f = null then return 1; end; "
        "  if l = null then return 2; end; "
        "  if f!.ToString() <> \"11223344-5566-7788-99aa-bbccddeeff00\" then return 3; end; "
        "  if l!.ToString() <> \"aabbccdd-eeff-0011-2233-445566778899\" then return 4; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// int? — primitive nullable.
BEZEL_TEST(linked_list_nullable_value_first_4641, first_last_int_nullable) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(42); "
        "  ll.AddLast(99); "
        "  var f: int? := ll.First; "
        "  var l: int? := ll.Last; "
        "  if f = null then return 1; end; "
        "  if l = null then return 2; end; "
        "  if f! <> 42 then return 3; end; "
        "  if l! <> 99 then return 4; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// LinkedList<int?>.First/.Last on an EMPTY list returns null.
BEZEL_TEST(linked_list_nullable_value_first_4641, first_last_empty_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  var f: int? := ll.First; "
        "  var l: int? := ll.Last; "
        "  if f <> null then return 1; end; "
        "  if l <> null then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Single-element list: First == Last.
BEZEL_TEST(linked_list_nullable_value_first_4641, single_element_first_equals_last) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(77); "
        "  var f: int? := ll.First; "
        "  var l: int? := ll.Last; "
        "  if f <> l then return 1; end; "
        "  if f! <> 77 then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}
