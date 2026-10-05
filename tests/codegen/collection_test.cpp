// Codegen slice §3 stdlib — Array<T>, Dictionary<K,V>, Stack<T>, Queue<T>,
// LINQ basics (Any/All/Where/Count/ForEach/Sum/Min/Max/First/Select).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- Array<T> ---

BEZEL_TEST(collection_array, add_get_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  return int(a.Count = 3 and a.Get(0) = 10 and a.Get(2) = 30); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(collection_array, set_and_remove) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  a[1] := 99; "
        "  a.RemoveAt(0); "
        "  return int(a.Count = 2 and a.Get(0) = 99 and a.Get(1) = 3); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(collection_array, string_elements) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"world\"); "
        "  return int(a.Count = 2 and a.Get(0) = \"hello\"); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(collection_array, for_in) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var s := 0; "
        "  for var x in a do s := s + x; end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// §A.6: `continue` inside `for var x in <runtime Array<T>>`. The loop's
// continue-target is the per-iteration increment block; the body must branch
// there on `continue` and the increment must live in that block. Regression:
// the increment was previously emitted in the body block, leaving the
// continue-target block without a terminator (malformed IR) — a `continue`
// branched to an unterminated block. Sums only the even elements (skips odds).
BEZEL_TEST(collection_array, for_in_with_continue) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var s := 0; "
        "  for var x in a do "
        "    if x mod 2 <> 0 then continue; end; "
        "    s := s + x; "
        "  end; "
        "  return s; "          // 2 + 4 = 6 (odds 1,3 skipped)
        "end Main; end C;"), 6);
}

// --- Stack<T> and Queue<T> ---

BEZEL_TEST(collection_stack, push_peek_pop) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var top := s.Peek(); "
        "  var popped := s.Pop(); "
        "  return int(top = 3 and popped = 3 and s.Count = 2); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(collection_queue, enqueue_dequeue) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(10); q.Enqueue(20); q.Enqueue(30); "
        "  var front := q.Dequeue(); "
        "  return int(front = 10 and q.Count = 2 and q.Get(0) = 20); "
        "end Main; end C;"), 1);
}

// --- Dictionary<K,V> ---

BEZEL_TEST(collection_dict, add_get_contains) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 1); d.Add(\"b\", 2); "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(\"a\", out v); "
        "  return int(d.Count = 2 and v = 1 and d.ContainsKey(\"b\") and not d.ContainsKey(\"c\")); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(collection_dict, set_and_remove) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"x\", 10); "
        "  d[\"x\"] := 99; "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(\"x\", out v); "
        "  d.Remove(\"x\"); "
        "  return int(v = 99 and d.Count = 0); "
        "end Main; end C;"), 1);
}

// §4.8 / #1201: Dictionary.Add on a duplicate key must throw ArgumentException.
// The indexer setter `d[k] := v` is the add-or-update (overwrite) path and must
// not throw.
BEZEL_TEST(collection_dict, add_duplicate_string_key_throws_arg_exc_spec_4_8_1201) {
    // String key: duplicate Add caught as ArgumentException → sentinel 42.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 10); "
        "  try d.Add(\"a\", 20); return 0; "
        "  catch ArgumentException => return 42; end; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(collection_dict, add_duplicate_int_key_throws_arg_exc_spec_4_8_1201) {
    // Generic (non-string) key: duplicate Add must also throw ArgumentException.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Add(1, 10); "
        "  try d.Add(1, 20); return 0; "
        "  catch ArgumentException => return 42; end; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(collection_dict, add_unique_keys_succeeds_spec_4_8_1201) {
    // Non-duplicate Add must still work normally (no spurious throw).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Add(1, 10); d.Add(2, 20); "
        "  return d[1] + d[2] + d.Count; "
        "end Main; end C;"), 32);  // 10+20+2=32
}

BEZEL_TEST(collection_dict, indexer_set_overwrites_no_throw_spec_4_8_1201) {
    // Indexer setter d[k]:=v is the add-or-update path and must NOT throw on dup.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Add(1, 10); "
        "  d[1] := 99; "
        "  return d[1] + d.Count; "
        "end Main; end C;"), 100);  // 99+1=100
}

// --- LINQ on Array<int> ---

BEZEL_TEST(linq_int, any) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return int(a.Any(lambda(x) => x > 2) and not a.Any(lambda(x) => x > 10)); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_int, all) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return int(a.All(lambda(x) => x > 0) and not a.All(lambda(x) => x > 2)); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_int, count) {
    // fix #3342 (§26.5 rule 1 / stdlib §4.4, v0.277): `Array<T>` declares
    // `Count` as a 0-arg property, so the direct `arr.Count(pred)` shape is
    // a compile error — the §5 LINQ operator is unreachable by name. The
    // composed spelling `arr.Where(pred).Count()` (Where is shadowed nowhere
    // in this document) reaches the operator pipeline.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return a.Where(lambda(x) => x mod 2 = 0).Count(); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(linq_int, where) {
    // stdlib §5: Where is LAZY — it returns a deferred IEnumerable<int>.
    // Materialize with ToArray() to observe the filtered elements.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var evens: Array<int> := a.Where(lambda(x) => x mod 2 = 0).ToArray(); "
        "  return int(evens.Count = 2 and evens.Get(0) = 2 and evens.Get(1) = 4); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_int, where_lazy_foreach) {
    // stdlib §5: a lazy Where pipeline is enumerable directly in a for-loop
    // without materializing — the deferred contract end-to-end.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  var s := 0; var n := 0; "
        "  for var x in a.Where(lambda(x) => x mod 2 = 0) do s := s + x; n := n + 1; end; "
        "  return int(s = 6 and n = 2); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_int, sum_min_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(4); a.Add(1); a.Add(5); "
        "  return int(a.Sum() = 14 and a.Min() = 1 and a.Max() = 5); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_int, first) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var f := a.First(lambda(x) => x > 2); "
        "  return f; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(linq_int, foreach_and_select) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Sum2(ref acc: int, x: int) acc := acc + x; end Sum2; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        // stdlib §5: Select is LAZY — materialize with ToArray() to observe.
        "    using var doubled: Array<int> := a.Select(lambda(x: int) => x * 2).ToArray(); "
        "    return int(doubled.Count = 3 and doubled.Get(0) = 2 and doubled.Get(2) = 6); "
        "  end Main; end C;"), 1);
}

// --- LINQ on Array<string> ---

BEZEL_TEST(linq_string, any_all_count) {
    // fix #3342 (§26.5 rule 1 / stdlib §4.4, v0.277): `Array<T>` declares
    // `Count` as a 0-arg property, so the direct `arr.Count(pred)` shape
    // is a compile error — the §5 LINQ operator is unreachable by name on
    // a receiver that declares `Count`. The composed spelling
    // `arr.Where(pred).Count()` (Where is shadowed nowhere) reaches the
    // operator pipeline.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"apple\"); a.Add(\"banana\"); a.Add(\"avocado\"); "
        "  var hasA := a.Any(lambda(s) => s[0] = 'a'); "
        "  var allNE := a.All(lambda(s) => s.Length > 0); "
        "  var aCount := a.Where(lambda(s) => s[0] = 'a').Count(); "
        "  return int(hasA and allNE and aCount = 2); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_string, where) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hi\"); a.Add(\"hello\"); a.Add(\"hey\"); "
        // stdlib §5: Where is LAZY — materialize with ToArray() to observe.
        "  using var long2: Array<string> := a.Where(lambda(s) => s.Length > 2).ToArray(); "
        "  return int(long2.Count = 2 and long2.Get(0) = \"hello\"); "
        "end Main; end C;"), 1);
}
