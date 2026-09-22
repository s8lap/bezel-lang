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
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// AOT-compile+run a program and return its `Main` exit code (or -999 if it
// did not run: compile/link reject or program crash/timeout).
int run(const std::string& src) {
    auto r = runAot(src);
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
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  var order := 0; "
        "  for var e in d do "
        "    order := order * 100 + e.Key; "
        "  end; "
        "  Console.WriteLine(order); return 0; "
    )), "102030");
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
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  var ignored := d.Remove(20); "
        "  var order := 0; "
        "  for var e in d do "
        "    order := order * 100 + e.Key; "
        "  end; "
        "  Console.WriteLine(order); return 0; "
    )), "1030");
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
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  Console.WriteLine(d.FirstKey * 100 + d.LastKey); return 0; "
    )), "1030");
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
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var order := 0; "
        "  for var x in s do "
        "    order := order * 100 + x; "
        "  end; "
        "  Console.WriteLine(order); return 0; "
    )), "102030");
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
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var ignored := s.Remove(20); "
        "  var order := 0; "
        "  for var x in s do "
        "    order := order * 100 + x; "
        "  end; "
        "  Console.WriteLine(order); return 0; "
    )), "1030");
}

// fix #2553 (stdlib §4.18, v0.236): the for-in path's stdlib-synthesised
// GetEnumerator iterated the SortedSet<string> dict's internal order[] array,
// yielding insertion order on non-trivial inserts — diverging from the
// explicit `.GetEnumerator()` surface (#2550) which already promised
// ascending order. Route the for-in through wrapSortedSetAsIEnumerable (the
// SAME wrap the explicit `.GetEnumerator()` call site uses, LlvmBackend_Calls2
// .cpp:5070) so the two §4.18 surfaces agree on the same receiver. The
// element-type gate is the `buildSortedKeyComparator` non-support for string
// (it ranks only integer/float scalars); that gate previously caused
// lowerDictBackedSetFor to silently drop the sort and fall through to the
// stdlib-synthesised GetEnumerator, which iterates insertion order. The fix
// defers the non-sortable SortedSet element case to lowerIEnumerableFor's
// SortedSet-aware helper, which routes through the same sorted-snapshot wrap
// as the explicit-GetEnumerator surface.
BEZEL_TEST(sorted_1426, sorted_set_forin_string_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"gamma\"); s.Add(\"alpha\"); s.Add(\"beta\"); "
        "  for var v in s do Console.WriteLine(v); end; "
        "  return 0; "
    )), "alpha\nbeta\ngamma");
}

// fix #2553 follow-up (sibling of the explicit-GetEnumerator #2550 test):
// after Remove, the remaining elements still iterate in ascending order via
// the for-in path (the snapshot is taken before the loop body, so the
// mid-loop Remove doesn't affect iteration order).
BEZEL_TEST(sorted_1426, sorted_set_forin_string_after_remove_spec_4_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"delta\"); s.Add(\"charlie\"); s.Add(\"alpha\"); "
        "  s.Add(\"bravo\"); "
        "  var ignored := s.Remove(\"charlie\"); "
        "  for var v in s do Console.WriteLine(v); end; "
        "  return 0; "
    )), "alpha\nbravo\ndelta");
}

// fix #2553 follow-up (sibling of #2550): break/continue inside a `for var x in
// sortedSet` over a SortedSet<string> disposes the loop's wrapper/enumerator
// scope correctly — the loop's vtable-protocol path used scopeDepth == my-scope-
// index (not outerDepth), so emitLoopDisposals on `continue` is a no-op (the
// wrapper/enumerator stay live for the next MoveNext read); `break` and normal
// exhaustion both land at endBB, which calls emitDisposals(disposeStack_.back())
// for the wrapper/enumerator scope exactly once. Each variant uses a
// shuffled input so the assertion discriminates ascending order from
// insertion order (the #2553 bug would otherwise mask itself by yielding
// insertion-order output that "looks sorted" for trivially-sorted inputs).
BEZEL_TEST(sorted_1426, sorted_set_forin_string_with_break_continue_spec_4_18) {
    // break: SHUFFLED input — break on "beta", assert ascending prefix.
    // Without the fix, the for-in yields insertion order ("gamma,alpha")
    // which disagrees with the expected ascending prefix ("alpha,").
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"gamma\"); s.Add(\"alpha\"); s.Add(\"beta\"); "
        "  var prefix := \"\"; "
        "  for var v in s do "
        "    if v = \"beta\" then break; end; "
        "    prefix := prefix + v + \",\"; "
        "  end; "
        "  Console.WriteLine(prefix); return 0; "
    )), "alpha,");
    // continue: SHUFFLED input — skip "alpha" mid-list, assert ascending suffix.
    // Without the fix, iteration would yield "gamma,beta" insertion order,
    // not the ascending "beta,gamma," suffix.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"gamma\"); s.Add(\"alpha\"); s.Add(\"beta\"); "
        "  var suffix := \"\"; "
        "  for var v in s do "
        "    if v = \"alpha\" then continue; end; "
        "    suffix := suffix + v + \",\"; "
        "  end; "
        "  Console.WriteLine(suffix); return 0; "
    )), "beta,gamma,");
    // empty body: SHUFFLED input, no break/continue — must be fully ascending.
    // Without the fix, this would yield "gamma,alpha,beta".
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"gamma\"); s.Add(\"alpha\"); s.Add(\"beta\"); "
        "  var all := \"\"; "
        "  for var v in s do "
        "    if v = \"z\" then continue; end; "
        "    all := all + v; "
        "  end; "
        "  Console.WriteLine(all); return 0; "
    )), "alphabetagamma");
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
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(30); s.Add(10); s.Add(20); "
        "  var result := 0; "
        "  result := result + s.First; "
        "  result := result + s.Last * 100; "
        "  Console.WriteLine(result); return 0; "
    )), "3010");
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

// ===========================================================================
// SortedSet<T> — explicit .GetEnumerator()  (stdlib §4.18; spec §29.1, v0.236)
// ===========================================================================
//
// #2546: explicit `.GetEnumerator()` on a SortedSet<T> was non-functional:
// the sema gate (ExprTypecheck_Calls.cpp, #2471's #1002 block) did not
// include SortedSet, so the call's result type was never set to the
// IEnumerator<T> sentinel, lowerLocalDecl's initializer degraded to a
// zero-initialized value (#2434), and a subsequent `e.MoveNext()` either
// crashed via the zero-def or yielded nothing. The codegen wrap gate
// (LlvmBackend_Calls2.cpp, the Set/LinkedList/Dictionary block) was also
// missing SortedSet — once the sema gate was added, the codegen needed a
// dedicated sorted-snapshot wrap (wrapSortedSetAsIEnumerable) to sort the
// keys via bz_dict_keys_gen + bz_sort_keys_gen before materialising the
// entries into an owning bz_array_t. The bug was confirmed live on
// origin/main @ 451c4fe (before this fix): the reordering [3, 1, 2] came
// back as the hash order, not ascending, and the local `e` was zero-
// initialised under the gate gap. The fix lands BOTH halves: the sema
// typing gate (§29.1 contract) and the codegen snapshot+sort wrap
// (§4.18 ascending-order promise).

// #2546 / stdlib §4.18: explicit .GetEnumerator() iterates in ascending
// order. The 3-1-2 insert order is the discriminator — without the sort
// step the enumerator would yield them in hash order (or zero out).
BEZEL_TEST(sorted_1426, sorted_set_getenumerator_returns_ascending_spec_4_18) {
    auto r = runAotOut(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(3); s.Add(1); s.Add(2); "
        "  using var e := s.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    Console.WriteLine(e.Current.ToString()); "
        "  end; "
        "  return 0; "
    ));
    // runAotOut strips a single trailing newline (platform-independent
    // helper per tests/support/aot_support.hpp), so the expected bare
    // payload is "1\n2\n3", not "1\n2\n3\n".
    BEZEL_EXPECT_EQ(r, "1\n2\n3");
}

// #2546: SortedSet<int> ascending order matches the for-loop path
// directly (#1426's "two paths must not disagree" cross-check).
BEZEL_TEST(sorted_1426, sorted_set_getenumerator_order_matches_forloop_order_directly) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(2); s.Add(5); s.Add(8); s.Add(1); "
        "  var geSum := 0; var flSum := 0; "
        "  using var e := s.GetEnumerator(); "
        "  while e.MoveNext() do geSum := geSum * 10 + e.Current; end; "
        "  for var x in s do flSum := flSum * 10 + x; end; "
        "  return flSum - geSum; "
    )), 0);
}

// #2550: SortedSet<string> ascending order via the explicit
// .GetEnumerator() path. The #2546 wrapSortedSetAsIEnumerable string
// decode was a leftover from the SortedDictionary<string,V> family —
// it called `bz_dict_keys_str` / `bz_sort_keys_str` on a SortedSet's
// GENERIC-key backing (`bz_dict_set_gen` stores each element as a
// malloc(8) buffer holding the bz_string_struct* pointer value, NOT
// the raw chars SortedDictionary uses), so the runtime read those 8
// pointer bytes as char data and produced garbage bytes for the
// bz_string_t readback (#2546's reproducer `alpha\\nbeta\\ngamma\\n`
// came back as `<F\\n0<F\\nP<F\\n`). The fix switches the string
// element path to `bz_dict_keys_gen` + a `bz_sort_kv_gen` driven by a
// codegen-side string-content comparator (`bz$sscmp$string`: deref
// each keys[i] into its bz_string_struct*, then forward BOTH to the
// runtime's ordinal string comparison). ⚠ That forward used to be
// `bz_string_compare` with (data, len); fix #4487 deleted that helper
// — it walked min(len) BYTES over a CODE-POINT length (#535 class) —
// and this comparator now calls `bz_str_compare_cp`, which decodes
// each operand at its own storage width (stdlib §9.6 / spec §4.6,
// v0.306). ⚠ `SortedDictionary` is NO LONGER the same comparator
// contract: it still runs on its own strcmp-driven bz_sort_kv_str
// path, which #4487 did not move (see #4493). For the ASCII inputs
// below the two agree; for non-ASCII they do not. Decoding stores
// the live `bz_string_struct*` directly in the result array; the
// wrapper shares the SortedSet's +1 (single owner at scope end via
// `bz_set_string_keys_release` → `bz_string_release`).
//
// Discriminator: a 3-string input [gamma, alpha, beta] must iterate
// as alpha, beta, gamma (ascending order, real chars per line).
BEZEL_TEST(sorted_1426, sorted_set_getenumerator_string_ascending_spec_4_18) {
    auto r = runAotOut(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"gamma\"); s.Add(\"alpha\"); s.Add(\"beta\"); "
        "  using var e := s.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    Console.WriteLine(e.Current); "
        "  end; "
        "  return 0; "
    ));
    BEZEL_EXPECT_EQ(r, "alpha\nbeta\ngamma");
}

// #2550: ascending order holds across more elements (reverse-insert
// fan-out) and after a Remove — both the read path (sort comparator)
// and the underlying bz_dict_t mutators are exercised.
BEZEL_TEST(sorted_1426, sorted_set_getenumerator_string_after_remove_spec_4_18) {
    auto r = runAotOut(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"delta\"); s.Add(\"charlie\"); s.Add(\"alpha\"); "
        "  s.Add(\"bravo\"); "
        "  var ignored := s.Remove(\"charlie\"); "
        "  using var e := s.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    Console.WriteLine(e.Current); "
        "  end; "
        "  return 0; "
    ));
    BEZEL_EXPECT_EQ(r, "alpha\nbravo\ndelta");
}

// #2550 follow-on (NOT in scope for this fix; filed separately):
// `for var x in s` over a SortedSet<string> currently iterates in
// insertion order (the for-in path's lowerIEnumerableFor dispatches the
// interface-inherited GetEnumerator() differently from the explicit
// `using var e := s.GetEnumerator()` call — the former hits the
// stdlib-synthesised IEnumerator without the sorted-snapshot wrap).
// Cross-checking `for-in == GetEnumerator` here would mask the real
// sibling bug; the two paths must agree on ascending order before this
// cross-check can run. Today's two tests prove the explicit-GetEnumerator
// path — the for-in path is a separate regression.

// #2546 (sibling of the original repro in the issue): after Remove, the
// remaining elements still iterate in ascending order via the explicit
// .GetEnumerator() path (the source-collection-mutation guard is
// lowerDictEntryFor's §29.12 epoch check, not the GetEnumerator path's
// concern — but the upward-order guarantee holds).
BEZEL_TEST(sorted_1426, sorted_set_getenumerator_after_remove_ascending_spec_4_18) {
    auto r = runAotOut(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(3); s.Add(1); s.Add(2); "
        "  var ignored := s.Remove(2); "
        "  using var e := s.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    Console.WriteLine(e.Current.ToString()); "
        "  end; "
        "  return 0; "
    ));
    BEZEL_EXPECT_EQ(r, "1\n3");
}

// #2546: §29.11 "each GetEnumerator() call produces an independent
// enumerator" — two separate .GetEnumerator() calls on the SAME SortedSet
// each replay the full ascending-element sequence from the start. The
// sum-discriminator (1+2+3 + 1+2+3) catches the gate-gap regression where
// the second call would either return the same exhausted cursor or zero.
BEZEL_TEST(sorted_1426, sorted_set_getenumerator_two_calls_each_independent_and_replay) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(1); s.Add(2); s.Add(3); "
        "  var n := 0; "
        "  using var e1 := s.GetEnumerator(); "
        "  while e1.MoveNext() do n := n + e1.Current; end; "
        "  using var e2 := s.GetEnumerator(); "
        "  while e2.MoveNext() do n := n + e2.Current; end; "
        "  return n; "
    )), 12);
}

// ===========================================================================
// SortedDictionary<K,V> — explicit .GetEnumerator()  (stdlib §4.17; spec §29.1,
// v0.236)
// ===========================================================================
//
// #2546: explicit `.GetEnumerator()` on a SortedDictionary<K,V> was also
// non-functional — same root cause as SortedSet (gate gap in both sema +
// codegen). The fix routes SortedDictionary through a dedicated
// wrapSortedDictionaryAsIEnumerable that sorts keys + values together via
// bz_sort_kv_gen / bz_sort_kv_str, then materialises {K, V} entries into
// a fresh bz_array_t (the K field is a rebuilt bz_string for string keys,
// ref-counted by the #2522 Dispose-walker).

// #2546 / stdlib §4.17: SortedDictionary<int,int> ascending key order via
// the explicit .GetEnumerator() path. The 30-10-20 insert order
// discriminator — without the sort, the entries would yield in insertion
// order (10, 20, 30) which would otherwise be ambiguous with the
// Dictionary test in 2471; the SortedSet-style hash-order fallback is
// strictly less than ascending for this input.
BEZEL_TEST(sorted_1426, sorted_dict_getenumerator_int_key_ascending_spec_4_17) {
    auto r = runAotOut(prog(
        "  using var d := new SortedDictionary<int,int>(); "
        "  d[30] := 300; d[10] := 100; d[20] := 200; "
        "  using var e := d.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    Console.WriteLine(e.Current.Key.ToString()); "
        "  end; "
        "  return 0; "
    ));
    BEZEL_EXPECT_EQ(r, "10\n20\n30");
}

// #2546: SortedDictionary<string,int> ascending key order via the string
// fast path (bz_dict_keys_str + bz_sort_kv_str). Discriminator: c < a < b
// insertion order → ascending iteration order is a, b, c.
BEZEL_TEST(sorted_1426, sorted_dict_getenumerator_string_key_ascending_spec_4_17) {
    auto r = runAotOut(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"c\"] := 3; d[\"a\"] := 1; d[\"b\"] := 2; "
        "  using var e := d.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    Console.WriteLine(e.Current.Key + \"=\" + e.Current.Value.ToString()); "
        "  end; "
        "  return 0; "
    ));
    BEZEL_EXPECT_EQ(r, "a=1\nb=2\nc=3");
}

// #2546: SortedDictionary<K,V> ascending order matches the for-loop path
// directly. The two paths must iterate the same sequence under #1426's
// contract; this test catches any future regression where the codegen
// path and the desugaring path drift apart.
BEZEL_TEST(sorted_1426, sorted_dict_getenumerator_order_matches_forloop_order_directly) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"c\"] := 3; d[\"a\"] := 1; d[\"b\"] := 2; "
        "  var geSum := \"\"; var flSum := \"\"; "
        "  using var e := d.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    geSum := geSum + e.Current.Key; "
        "  end; "
        "  for var x in d do "
        "    flSum := flSum + x.Key; "
        "  end; "
        "  return (if geSum = flSum then 42 else 1); "
    )), 42);
}

// #2546: SortedDictionary<string,int> ascending .GetEnumerator() with
// .Key + .Value field access — the DictionaryEntry<K,V> sentinel-typed
// `.Current.Key` / `.Current.Value` field reads must work after the fix
// (the sema gate now types the call as IEnumerator<DictionaryEntry<K,V>>;
// the codegen dispatches through the entry-materializing path).
BEZEL_TEST(sorted_1426, sorted_dict_getenumerator_key_value_combined_spec_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"c\"] := 3; d[\"a\"] := 1; d[\"b\"] := 2; "
        "  var sum := 0; "
        "  using var e := d.GetEnumerator(); "
        "  while e.MoveNext() do "
        "    sum := sum + e.Current.Value; "
        "  end; "
        "  return sum; "
    )), 6);  // 1 + 2 + 3
}
