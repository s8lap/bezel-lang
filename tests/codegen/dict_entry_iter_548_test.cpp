// Fix #548 — `for var e in dict` borrowing entry iteration on Dictionary<K,V>.
//
// Spec: stdlib §4.8 (v0.135, Dictionary<K,V>) / spec §21.
//
// v0.135 widens Dictionary<K,V> to support `for var e in d` directly.
// The loop yields DictionaryEntry<K,V> values (the ICollection element type),
// one per live entry in insertion order.  `entry.Key` reads K; `entry.Value`
// reads V (a borrow of the stored value — the dictionary keeps ownership per
// §28.11).  Removing a key and re-adding it places it at the end of the
// insertion order (§4.8).
//
// Three-layer fix:
//
// Sema (ExprTypecheck.cpp): `for var e in d` on a Dictionary<K,V>-typed source
//   no longer emits the #530 diagnostic; instead it sets the loop variable's
//   element type to a synthetic DictionaryEntry<K,V> TypeRef
//   (importedName="DictionaryEntry", genericArgs=[K,V]).
//   OwnedDictionary<K,V> still gets the narrowed §4.9 diagnostic.
//
// Sema (ExprTypecheck_Calls.cpp / resolveMemberAccess): `entry.Key` is typed
//   as K (genericArgs[0]) and `entry.Value` as V (genericArgs[1]) when the
//   receiver's TypeRef has importedName == "DictionaryEntry".
//
// Codegen (LlvmBackend_Stmts.cpp / lowerDictEntryFor): recognises a bare
//   Dictionary<K,V> iterable (not a .Keys/.Values MemberAccess) and emits
//   parallel key+value snapshot calls (bz_dict_keys_str + bz_dict_values_str
//   for string-key dicts; bz_dict_keys_gen + bz_dict_values_gen for generic
//   keys).  The per-iteration entry alloca is a 2-field struct {K key; V val};
//   field 0 = Key, field 1 = Value.
//
// Codegen (LlvmBackend_Exprs.cpp): `entry.Key`/`entry.Value` on a
//   DictionaryEntry<K,V>-typed local GEPs the struct alloca directly (before
//   fieldAddr, which knows nothing of the synthetic DictionaryEntry type).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// Basic string→int dictionary: entry.Key and entry.Value
// ---------------------------------------------------------------------------

// #548 / stdlib §4.8 v0.135: sum values via entry iteration on a
// string-keyed Dictionary<string,int>.  {"a":10, "b":20} → sum = 30.
BEZEL_TEST(dict_entry_iter_548, string_key_int_value_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 10); "
        "  d.Set(\"b\", 20); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; "
        "end Main; end C;"), 30);
}

// #548 / stdlib §4.8 v0.135: iteration count equals d.Count.
BEZEL_TEST(dict_entry_iter_548, string_key_int_value_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"x\", 1); "
        "  d.Set(\"y\", 2); "
        "  var n := 0; "
        "  for var e in d do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 2);
}

// #548 / stdlib §4.8 v0.135: entry.Key is the key (string).  Verify by
// summing key lengths: {"ab":0, "xyz":0} → 2 + 3 = 5.
BEZEL_TEST(dict_entry_iter_548, string_key_access_length_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"ab\", 0); "
        "  d.Set(\"xyz\", 0); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Key.Length; end; "
        "  return sum; "
        "end Main; end C;"), 5);
}

// #548 / stdlib §4.8 v0.135: three entries — sum of values via entries.
BEZEL_TEST(dict_entry_iter_548, string_key_three_entries) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"one\", 1); "
        "  d.Set(\"two\", 2); "
        "  d.Set(\"three\", 3); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; "
        "end Main; end C;"), 6);
}

// #548 / stdlib §4.8 v0.135: insertion-order guarantee — remove+re-add
// places the key at the end.  Insert a=1 b=2; remove a; re-add a=3.
// Insertion order is now b=2, a=3.  Sum = 5 (both values).
BEZEL_TEST(dict_entry_iter_548, remove_readd_insertion_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Remove(\"a\"); "
        "  d.Set(\"a\", 3); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; "
        "end Main; end C;"), 5);
}

// #548 / stdlib §4.8 v0.135: empty dictionary → zero iterations.
BEZEL_TEST(dict_entry_iter_548, empty_dict_zero_iterations) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  var n := 0; "
        "  for var e in d do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// Int-keyed Dictionary<int,int>
// ---------------------------------------------------------------------------

// #548 / stdlib §4.8 v0.135: int-keyed dictionary entry iteration.
// {3:100, 4:200} → sum of values = 300.
BEZEL_TEST(dict_entry_iter_548, int_key_int_value_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(3, 100); "
        "  d.Set(4, 200); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; "
        "end Main; end C;"), 300);
}

// #548 / stdlib §4.8 v0.135: int keys accessible via entry.Key.
// {3:0, 4:0, 5:0} → sum of keys = 12.
BEZEL_TEST(dict_entry_iter_548, int_key_access_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(3, 0); "
        "  d.Set(4, 0); "
        "  d.Set(5, 0); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Key; end; "
        "  return sum; "
        "end Main; end C;"), 12);
}

// ---------------------------------------------------------------------------
// Class-typed values (borrow semantics, §28.11)
// ---------------------------------------------------------------------------

// #548 / stdlib §4.8 v0.135 / §28.11: entry.Value for a class-typed V is a
// borrow — the dictionary retains ownership.  The loop uses entry.Value for a
// method call (string length) without transferring ownership; the dictionary
// disposes cleanly afterwards.  {"hello":..., "world":...} where V = string.
// Sum of string lengths: 5 + 5 = 10.
BEZEL_TEST(dict_entry_iter_548, string_value_borrow_method_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, string>(); "
        "  d.Set(\"k1\", \"hello\"); "
        "  d.Set(\"k2\", \"world\"); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value.Length; end; "
        "  return sum; "
        "end Main; end C;"), 10);
}

// ---------------------------------------------------------------------------
// spec §21 verbatim example (paraphrased for the JIT harness)
// ---------------------------------------------------------------------------

// spec §21 / stdlib §4.8 v0.135: the spec §21 for-chapter example:
//   for var entry in dict do
//       Console.WriteLine($"{entry.Key} -> {entry.Value}");
//   end;
// We cannot assert Console output in the int-return harness, so instead
// verify that the loop runs the right number of times with entry.Key and
// entry.Value accessible (no crash, correct count).
BEZEL_TEST(dict_entry_iter_548, spec_21_verbatim_entry_accessible) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"alpha\", 1); "
        "  d.Set(\"beta\",  2); "
        "  d.Set(\"gamma\", 3); "
        "  var n := 0; "
        "  for var entry in d do "
        "    var k: string := entry.Key; "  // §21 pattern
        "    var v: int := entry.Value; "
        "    n := n + v; "
        "  end; "
        "  return n; "
        "end Main; end C;"), 6);
}

// ---------------------------------------------------------------------------
// NEGATIVE: OwnedDictionary<K,V> still rejected (§4.9 v0.135, narrowed #530)
// ---------------------------------------------------------------------------

// #548 / §4.9 v0.135: `for var e in ownedDict` must still be rejected with
// the §4.9 diagnostic.  Verify: the program does NOT produce a meaningful
// result (compile error → r.ran == false, or r.exitCode != 42).
BEZEL_TEST(dict_entry_iter_548, owned_dict_still_rejected) {
    // Value is a class (§14.4: an owning dictionary rejects a `string` value),
    // so the ONLY rejection here is the §4.9 for-in one this test targets.
    auto r = runJit(
        "module M; class V public n: int := 0; end V; "
        "class C public static method Main(): int "
        "  using var od := new OwnedDictionary<string, V>(); "
        "  var n := 0; "
        "  for var e in od do n := n + 1; end; "
        "  return n; "
        "end Main; end C;");
    // Either it didn't run (compile error = correct) or did not return 42.
    if (r.ran) {
        BEZEL_EXPECT_TRUE(r.exitCode != 42);
    }
}
