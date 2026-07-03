// fix #1426 / stdlib §4.17 / §4.18: SortedDictionary<K,V> and SortedSet<T>
// sorting and key-access codegen.
//
// Root causes fixed:
//   SortedDictionary<K,V>:
//     1. For-in entry iteration now produces entries in ascending key order
//        (bz_sort_kv_gen / bz_sort_kv_str sort the snapshot arrays before the loop).
//     2. FirstKey / LastKey (stdlib §4.17) — new runtime + codegen: O(n) scan via
//        bz_dict_min_key_gen / bz_dict_max_key_gen / bz_dict_min_key_str /
//        bz_dict_max_key_str.  Both MemberAccessExpr (no parens) and CallExpr (with
//        parens) forms are supported via LlvmBackend_Exprs.cpp and
//        LlvmBackend_Calls.cpp respectively.
//   SortedSet<T>:
//     1. Was completely missing from the heap-pointer list (LlvmBackend_Layout.cpp):
//        variables had type i32 instead of ptr (bz_dict_t*).
//     2. lowerNew (LlvmBackend_Exprs.cpp) was absent → nullptr, slot uninitialized.
//     3. isCollectionTypeName() was absent → isArr=false → methods bypassed.
//     4. SortedSet.First / Last (stdlib §4.18) — new runtime + codegen: O(n) scan
//        via bz_dict_min/max_key_gen with a per-element-type comparator.
//     5. For-in iteration now in ascending order (bz_sort_keys_gen before loop).
//   Sema: SortedSet First/Last return type was wrong (Min/Max → First/Last per §4.18).
//
// Note: JIT (runJit) returns the full int value of Main; no modulo-256 needed.
//
// Spec refs: stdlib §4.17 (SortedDictionary), stdlib §4.18 (SortedSet), v0.171.

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

// ===========================================================================
// SortedDictionary<K,V> — ascending for-in iteration  (stdlib §4.17)
// ===========================================================================

// #1426 / stdlib §4.17: out-of-order inserts → ascending int key order in for-in.
BEZEL_TEST(sorted_1426, sorted_dict_forin_int_key_ascending_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  var order := 0; "
        "  for var e in d do "
        "    order := order * 100 + e.Key; "
        "  end; "
        "  return order; "
    )), 102030);
}

// #1426 / stdlib §4.17: ascending string key order in for-in.
BEZEL_TEST(sorted_1426, sorted_dict_forin_string_key_ascending_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"c\"] := 3; d[\"a\"] := 1; d[\"b\"] := 2; "
        "  var sum := 0; "
        "  for var e in d do "
        "    sum := sum * 10 + e.Value; "
        "  end; "
        "  return sum; "
    )), 123);
}

// #1426 / stdlib §4.17: after Remove, remaining entries still in ascending order.
BEZEL_TEST(sorted_1426, sorted_dict_forin_after_remove_ascending_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  var ignored := d.Remove(20); "
        "  var order := 0; "
        "  for var e in d do "
        "    order := order * 100 + e.Key; "
        "  end; "
        "  return order; "
    )), 1030);
}

// ===========================================================================
// SortedDictionary<K,V> — FirstKey / LastKey  (stdlib §4.17)
// ===========================================================================

// #1426 / stdlib §4.17: FirstKey returns the smallest key (int).
BEZEL_TEST(sorted_1426, sorted_dict_firstkey_int_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  return d.FirstKey; "
    )), 10);
}

// #1426 / stdlib §4.17: LastKey returns the largest key (int).
BEZEL_TEST(sorted_1426, sorted_dict_lastkey_int_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  return d.LastKey; "
    )), 30);
}

// #1426 / stdlib §4.17: FirstKey + LastKey combined.
BEZEL_TEST(sorted_1426, sorted_dict_firstkey_lastkey_combined_int_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  return d.FirstKey * 100 + d.LastKey; "
    )), 1030);
}

// #1426 / stdlib §4.17: FirstKey on an empty dict → InvalidOperationException.
BEZEL_TEST(sorted_1426, sorted_dict_firstkey_empty_throws_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  try "
        "    var k := d.FirstKey; "
        "    return 0; "
        "  catch InvalidOperationException => return 1; "
        "  end; "
        "  return 0; "
    )), 1);
}

// ===========================================================================
// SortedSet<T> — Count / Add / Contains  (stdlib §4.18)
// ===========================================================================

// #1426 / stdlib §4.18: Count reflects inserted elements.
BEZEL_TEST(sorted_1426, sorted_set_count_after_inserts_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  return s.Count; "
    )), 3);
}

// #1426 / stdlib §4.18: Count of empty SortedSet is 0.
BEZEL_TEST(sorted_1426, sorted_set_count_empty_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  return s.Count; "
    )), 0);
}

// #1426 / stdlib §4.18: Contains returns true for present element.
BEZEL_TEST(sorted_1426, sorted_set_contains_present_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(10); s.Add(20); s.Add(30); "
        "  return int(s.Contains(10) and s.Contains(20) and s.Contains(30)); "
    )), 1);
}

// #1426 / stdlib §4.18: Contains returns false for absent element.
BEZEL_TEST(sorted_1426, sorted_set_contains_absent_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(10); "
        "  return int(not s.Contains(99)); "
    )), 1);
}

// #1426 / stdlib §4.18: Add is idempotent (set semantics — no duplicates).
BEZEL_TEST(sorted_1426, sorted_set_add_idempotent_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(5); s.Add(5); s.Add(5); "
        "  return s.Count; "
    )), 1);
}

// ===========================================================================
// SortedSet<T> — ascending for-in iteration  (stdlib §4.18)
// ===========================================================================

// #1426 / stdlib §4.18: out-of-order inserts → ascending order in for-in.
BEZEL_TEST(sorted_1426, sorted_set_forin_int_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var order := 0; "
        "  for var x in s do "
        "    order := order * 100 + x; "
        "  end; "
        "  return order; "
    )), 102030);
}

// #1426 / stdlib §4.18: empty SortedSet → zero iterations.
BEZEL_TEST(sorted_1426, sorted_set_forin_empty_zero_iters_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  var n := 0; "
        "  for var x in s do n := n + 1; end; "
        "  return n; "
    )), 0);
}

// #1426 / stdlib §4.18: after Remove, remaining elements still ascending.
BEZEL_TEST(sorted_1426, sorted_set_forin_after_remove_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var ignored := s.Remove(20); "
        "  var order := 0; "
        "  for var x in s do "
        "    order := order * 100 + x; "
        "  end; "
        "  return order; "
    )), 1030);
}

// ===========================================================================
// SortedSet<T> — First (least) and Last (greatest)  (stdlib §4.18)
// ===========================================================================

// #1426 / stdlib §4.18: First returns the least element.
BEZEL_TEST(sorted_1426, sorted_set_first_returns_least_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  return s.First; "
    )), 10);
}

// #1426 / stdlib §4.18: Last returns the greatest element.
BEZEL_TEST(sorted_1426, sorted_set_last_returns_greatest_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  return s.Last; "
    )), 30);
}

// #1426 / stdlib §4.18: First and Last combined encode min + max.
BEZEL_TEST(sorted_1426, sorted_set_first_last_combined_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var result := 0; "
        "  result := result + s.First; "
        "  result := result + s.Last * 100; "
        "  return result; "
    )), 3010);
}

// #1426 / stdlib §4.18: First/Last after Remove — recalculate from remaining.
BEZEL_TEST(sorted_1426, sorted_set_first_after_remove_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var ignored := s.Remove(10); "
        "  return s.First; "
    )), 20);
}

// #1426 / stdlib §4.18: First on an empty SortedSet → InvalidOperationException.
BEZEL_TEST(sorted_1426, sorted_set_first_empty_throws_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  try "
        "    var x := s.First; "
        "    return 0; "
        "  catch InvalidOperationException => return 1; "
        "  end; "
        "  return 0; "
    )), 1);
}

// #1426 / stdlib §4.18: Last on an empty SortedSet → InvalidOperationException.
BEZEL_TEST(sorted_1426, sorted_set_last_empty_throws_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  try "
        "    var x := s.Last; "
        "    return 0; "
        "  catch InvalidOperationException => return 1; "
        "  end; "
        "  return 0; "
    )), 1);
}

// ===========================================================================
// SortedSet<T> — disposal  (stdlib §4.18)
// ===========================================================================

// #1426 / stdlib §4.18: using-var SortedSet is disposed at scope end (bz_dict_free).
BEZEL_TEST(sorted_1426, sorted_set_using_var_disposed_at_scope_end_spec_4_18) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(5); s.Add(10); s.Add(15); "
        "  return s.Count; "
    )), 3);
}
