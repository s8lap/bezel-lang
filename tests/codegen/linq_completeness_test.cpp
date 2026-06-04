// Codegen tests for the LINQ operators completed in this milestone (stdlib §5).
//
// These derive from the §5 `System.Collections.Linq` extension surface. The
// operators added here, with their §5 signatures and lazy/eager classification:
//
//   Aggregate<TAccum>(seed, fold): TAccum                 — eager (§5 Aggregation)
//   Any(): bool                                           — eager (§5 Quantifiers)
//   Average(selector: (T)->int|long|double): double       — eager (§5 Aggregation)
//   Average(): double  (per-type, §5.1)                   — eager
//   Concat(other: IEnumerable<T>): IEnumerable<T>         — LAZY  (§5 Combination)
//   TakeWhile(predicate: (T)->bool): IEnumerable<T>       — LAZY  (§5 Slicing)
//   SkipWhile(predicate: (T)->bool): IEnumerable<T>       — LAZY  (§5 Slicing)
//   SelectMany<U>(selector: (T)->IEnumerable<U>): IEnum<U>— LAZY  (§5 Projection)
//   Zip<U,R>(other, combine: (T,U)->R): IEnumerable<R>    — LAZY  (§5 Combination)
//
// §5: "All are lazy unless documented otherwise." The materializing/aggregating
// operators (Aggregate/Any/Average) are eager. The lazy operators do no work
// until enumerated; these tests consume them with a for-loop or .ToArray().

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- Aggregate (eager, §5 Aggregation: Aggregate<TAccum>(seed, fold)) ---

BEZEL_TEST(linq_complete, aggregate_sum_spec_5) {
    // [1,2,3,4].Aggregate(0, (acc,x) => acc + x) = 10.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return a.Aggregate(0, (acc: int, x: int) => acc + x); "
        "end Main; end C;"), 10);
}

BEZEL_TEST(linq_complete, aggregate_product_seed_spec_5) {
    // Aggregate with a non-zero seed: 1 * 1 * 2 * 3 * 4 = 24.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return a.Aggregate(1, (acc: int, x: int) => acc * x); "
        "end Main; end C;"), 24);
}

BEZEL_TEST(linq_complete, aggregate_empty_returns_seed_spec_5) {
    // Aggregate over an empty source returns the seed unchanged.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  return a.Aggregate(42, (acc: int, x: int) => acc + x); "
        "end Main; end C;"), 42);
}

// --- Any() no-predicate (eager, §5 Quantifiers: Any(): bool) ---

BEZEL_TEST(linq_complete, any_nonempty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); "
        "  return int(a.Any()); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_complete, any_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  return int(a.Any()); "
        "end Main; end C;"), 0);
}

// --- Average (eager, §5 Aggregation / §5.1: returns double) ---

BEZEL_TEST(linq_complete, average_selector_spec_5) {
    // Average(selector: (T)->int): double — (10+20+30)/3 = 20.0.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var avg: double := a.Average((x: int) => x); "
        "  return int(avg); "
        "end Main; end C;"), 20);
}

BEZEL_TEST(linq_complete, average_noselector_spec_5_1) {
    // §5.1 Average(): double on IEnumerable<int> — (2+4+6+8)/4 = 5.0.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(2); a.Add(4); a.Add(6); a.Add(8); "
        "  var avg: double := a.Average(); "
        "  return int(avg); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(linq_complete, average_truncation_spec_5) {
    // (1+2)/2 = 1.5 as a double; multiply by 10 then truncate → 15.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  var avg: double := a.Average(); "
        "  return int(avg * 10.0); "  // 15
        "end Main; end C;"), 15);
}

// --- Concat (lazy, §5 Combination: Concat(other): IEnumerable<T>) ---

BEZEL_TEST(linq_complete, concat_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); "
        "  using var b := new Array<int>(); b.Add(3); b.Add(4); b.Add(5); "
        "  var s := 0; var n := 0; "
        "  for var x in a.Concat(b) do s := s + x; n := n + 1; end; "
        "  return int(s = 15 and n = 5); "  // 1+2+3+4+5, 5 elements
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_complete, concat_order_spec_5) {
    // Concat preserves order: first source's elements precede other's.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); "
        "  using var b := new Array<int>(); b.Add(2); "
        "  using var r: Array<int> := a.Concat(b).ToArray(); "
        "  return int(r.Length = 2 and r.Get(0) = 1 and r.Get(1) = 2); "
        "end Main; end C;"), 1);
}

// --- TakeWhile / SkipWhile (lazy, §5 Slicing) ---

BEZEL_TEST(linq_complete, takewhile_spec_5) {
    // TakeWhile yields a prefix while the predicate holds, then stops — even if
    // later elements would satisfy it. [1,2,3,4,1] TakeWhile(<4) = [1,2,3].
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(1); "
        "  var s := 0; var n := 0; "
        "  for var x in a.TakeWhile((x) => x < 4) do s := s + x; n := n + 1; end; "
        "  return int(s = 6 and n = 3); "  // 1+2+3, stops at 4
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq_complete, skipwhile_spec_5) {
    // SkipWhile drops the leading run satisfying the predicate, then yields the
    // rest verbatim. [1,2,3,4,1] SkipWhile(<4) = [4,1].
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(1); "
        "  var s := 0; var n := 0; "
        "  for var x in a.SkipWhile((x) => x < 4) do s := s + x; n := n + 1; end; "
        "  return int(s = 5 and n = 2); "  // 4+1
        "end Main; end C;"), 1);
}

// --- SelectMany (lazy, §5 Projection: flattens (T)->IEnumerable<U>) ---

BEZEL_TEST(linq_complete, selectmany_spec_5) {
    // Each element maps to a sub-sequence; SelectMany concatenates them.
    // [1,2,3].SelectMany(x => [x, x*10]) = [1,10, 2,20, 3,30] → sum 66, 6 elems.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
        "  var s := 0; var n := 0; "
        "  for var y in a.SelectMany((x: int) => do "
        "      var inner := new Array<int>(); inner.Add(x); inner.Add(x * 10); "
        "      return inner; end) do s := s + y; n := n + 1; end; "
        "  return int(s = 66 and n = 6); "
        "end Main; end C;"), 1);
}

// --- Zip (lazy, §5 Combination: Zip<U,R>(other, combine)) ---

BEZEL_TEST(linq_complete, zip_spec_5) {
    // Zip pairs elements positionally and applies combine. Lengths differ → the
    // result length is the shorter of the two. [1,2,3] zip [10,20] (+) = [11,22].
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
        "  using var b := new Array<int>(); b.Add(10); b.Add(20); "
        "  var s := 0; var n := 0; "
        "  for var z in a.Zip(b, (x: int, y: int) => x + y) do s := s + z; n := n + 1; end; "
        "  return int(s = 33 and n = 2); "  // 11 + 22, length 2 (shorter)
        "end Main; end C;"), 1);
}

// --- Laziness witness: Concat over a Where pipeline does not over-enumerate ---

BEZEL_TEST(linq_complete, concat_chained_lazy_spec_5) {
    // Chain a lazy Where into Concat into Take and consume only a prefix — proves
    // the new operators compose with the existing lazy pipeline (no eager
    // materialization between stages).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  using var b := new Array<int>(); b.Add(5); b.Add(6); "
        "  var s := 0; "
        "  for var x in a.Where((x) => x mod 2 = 0).Concat(b).Take(3) do s := s + x; end; "
        "  return s; "  // evens of a = [2,4], concat [5,6] = [2,4,5,6], take 3 = [2,4,5] = 11
        "end Main; end C;"), 11);
}
