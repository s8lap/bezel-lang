// fix #1406 / stdlib §4.17: SortedDictionary<K,V> codegen — receiver lowered as
// i32 instead of ptr (bz_dict_t*) → malformed IR for every bz_dict_* call.
//
// Root causes (both fixed together):
//   1. lowerTypeRef (LlvmBackend_Layout.cpp): "SortedDictionary" was absent from
//      the heap-pointer name list → returned i32() for a SortedDictionary
//      variable, so the slot was `alloca i32` and a load produced an i32 receiver.
//   2. lowerNew (LlvmBackend_Exprs.cpp): "SortedDictionary" was absent from the
//      dict-alloc branch → new SortedDictionary<K,V>() returned nullptr, leaving
//      the slot uninitialized (and i32-typed by lowerTypeRef).
//
// Once both fixes are applied, isDictTypeName() covers SortedDictionary, so the
// full existing dict-method lowering (Count, ContainsKey, TryGetValue, Remove,
// Clear, indexer get/set, for-in entry iteration) all route correctly through the
// isDict section and receive a ptr-typed bz_dict_t* receiver.
//
// This test file probes every SortedDictionary operation in stdlib §4.17 that
// has existing codegen support.  FirstKey / LastKey require new codegen (no
// runtime function exists); they are deferred and tracked as a separate issue.
//
// Spec refs: stdlib §4.17 (SortedDictionary), §4.8 (TryGetValue, ContainsKey,
//   Remove, Clear, indexer, for-in), v0.171 (#1336, #1406).

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

// JIT a program and return its `Main` exit code (or -999 if it did not run).
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Wrap a Main body in a minimal module/class shell.
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// TryGetValue — string-keyed SortedDictionary<string,int>
// stdlib §4.17 / §4.8 (#1406)
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: the original repro from the issue.
// SortedDictionary.TryGetValue hit: returns true, out var carries the value.
BEZEL_TEST(sorted_dict_1406, trygetvalue_hit_string_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 12; "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  if d.TryGetValue(\"a\", out v) then return v!; end; "
        "  return 0; "
    )), 12);
}

// #1406 / stdlib §4.17: TryGetValue miss — returns false, out var stays null.
BEZEL_TEST(sorted_dict_1406, trygetvalue_miss_string_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"x\"] := 7; "
        "  var v: int? := null; "
        "  var found := d.TryGetValue(\"missing\", out v); "
        "  return int(not found and v = null); "
    )), 1);
}

// #1406 / stdlib §4.17: TryGetValue with generic (non-string) key.
BEZEL_TEST(sorted_dict_1406, trygetvalue_hit_int_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[3] := 99; "
        "  var v: int? := null; "
        "  if d.TryGetValue(3, out v) then return v!; end; "
        "  return 0; "
    )), 99);
}

// ---------------------------------------------------------------------------
// ContainsKey — stdlib §4.17
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: ContainsKey returns true for a present key,
// false for an absent one.
BEZEL_TEST(sorted_dict_1406, containskey_present_and_absent_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"b\"] := 5; "
        "  return int(d.ContainsKey(\"b\") and not d.ContainsKey(\"z\")); "
    )), 1);
}

// Generic (int) key ContainsKey.
BEZEL_TEST(sorted_dict_1406, containskey_int_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[10] := 1; d[20] := 2; "
        "  return int(d.ContainsKey(10) and d.ContainsKey(20) "
        "             and not d.ContainsKey(99)); "
    )), 1);
}

// ---------------------------------------------------------------------------
// Count — stdlib §4.17
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: Count reflects the number of inserted entries.
BEZEL_TEST(sorted_dict_1406, count_after_inserts_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 1; d[\"b\"] := 2; d[\"c\"] := 3; "
        "  return d.Count; "
    )), 3);
}

// Count of an empty SortedDictionary is 0.
BEZEL_TEST(sorted_dict_1406, count_empty_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  return d.Count; "
    )), 0);
}

// ---------------------------------------------------------------------------
// Indexer get/set — stdlib §4.17
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: indexer set (`d[k] := v`) adds or updates;
// indexer get (`d[k]`) retrieves the stored value.
BEZEL_TEST(sorted_dict_1406, indexer_set_and_get_string_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"x\"] := 10; "
        "  d[\"y\"] := 20; "
        "  d[\"x\"] := 99; "  // overwrite
        "  return d[\"x\"] + d[\"y\"]; "
    )), 119);
}

BEZEL_TEST(sorted_dict_1406, indexer_set_and_get_int_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[1] := 100; d[2] := 200; "
        "  return d[1] + d[2] + d.Count; "
    )), 302);
}

// ---------------------------------------------------------------------------
// Remove — stdlib §4.17
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: Remove deletes an entry and returns true.
// Remove of an absent key returns false.
BEZEL_TEST(sorted_dict_1406, remove_present_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 1; d[\"b\"] := 2; "
        "  var removed := d.Remove(\"a\"); "
        "  return int(removed and d.Count = 1 and not d.ContainsKey(\"a\") "
        "             and d.ContainsKey(\"b\")); "
    )), 1);
}

BEZEL_TEST(sorted_dict_1406, remove_absent_key_returns_false_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"x\"] := 5; "
        "  return int(not d.Remove(\"missing\") and d.Count = 1); "
    )), 1);
}

// ---------------------------------------------------------------------------
// Clear — stdlib §4.17
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: Clear empties the table; Count is 0 afterward.
BEZEL_TEST(sorted_dict_1406, clear_empties_table_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 1; d[\"b\"] := 2; d[\"c\"] := 3; "
        "  d.Clear(); "
        "  return d.Count; "
    )), 0);
}

// Clear then re-add: the dict is reusable after Clear.
BEZEL_TEST(sorted_dict_1406, clear_then_readd_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 1; "
        "  d.Clear(); "
        "  d[\"a\"] := 42; "
        "  return d[\"a\"]; "
    )), 42);
}

// ---------------------------------------------------------------------------
// For-in entry iteration — stdlib §4.17 / §4.8
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: `for var e in d` iterates (Key, Value) DictionaryEntry
// pairs.  Sum of all values == sum of all inserted values.
BEZEL_TEST(sorted_dict_1406, forin_sum_values_string_key_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"one\"] := 1; d[\"two\"] := 2; d[\"three\"] := 3; "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; "
    )), 6);
}

BEZEL_TEST(sorted_dict_1406, forin_count_iterations_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 10; d[\"b\"] := 20; "
        "  var n := 0; "
        "  for var e in d do n := n + 1; end; "
        "  return n; "
    )), 2);
}

BEZEL_TEST(sorted_dict_1406, forin_empty_dict_zero_iterations_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  var n := 0; "
        "  for var e in d do n := n + 1; end; "
        "  return n; "
    )), 0);
}

// #1406 / stdlib §4.17: int-keyed SortedDictionary for-in iteration.
BEZEL_TEST(sorted_dict_1406, forin_int_key_sum_values_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[5] := 50; d[10] := 100; d[15] := 150; "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; "
    )), 300);
}

// ---------------------------------------------------------------------------
// Disposal — SortedDictionary disposes correctly (bz_dict_free, not leak)
// ---------------------------------------------------------------------------

// #1406 / stdlib §4.17: a using-var SortedDictionary is disposed at scope-end
// without crash (checks the bz_dict_free path via isDictTypeName).
// The exit code is the sum of inserted values (proves the dict worked too).
BEZEL_TEST(sorted_dict_1406, using_var_disposed_at_scope_end_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"p\"] := 7; d[\"q\"] := 13; "
        "  return d[\"p\"] + d[\"q\"]; "
    )), 20);
}
