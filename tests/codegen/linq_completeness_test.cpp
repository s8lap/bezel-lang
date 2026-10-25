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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
// #1845: same as run() but the generated code is ASan/LSan-instrumented — a
// heap-buffer-overflow / UAF / leak flips r.ran to false (→ -999), so a
// memory-unsafe program fails the exit-code assertion.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
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
        "  return a.Aggregate(0, lambda(acc: int, x: int) => acc + x); "
        "end Main; end C;"), 10);
}

BEZEL_TEST(linq_complete, aggregate_product_seed_spec_5) {
    // Aggregate with a non-zero seed: 1 * 1 * 2 * 3 * 4 = 24.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return a.Aggregate(1, lambda(acc: int, x: int) => acc * x); "
        "end Main; end C;"), 24);
}

BEZEL_TEST(linq_complete, aggregate_empty_returns_seed_spec_5) {
    // Aggregate over an empty source returns the seed unchanged.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  return a.Aggregate(42, lambda(acc: int, x: int) => acc + x); "
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
        "  var avg: double := a.Average(lambda(x: int) => x); "
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
        "  return int(r.Count = 2 and r.Get(0) = 1 and r.Get(1) = 2); "
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
        "  for var x in a.TakeWhile(lambda(x) => x < 4) do s := s + x; n := n + 1; end; "
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
        "  for var x in a.SkipWhile(lambda(x) => x < 4) do s := s + x; n := n + 1; end; "
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
        "  for var y in a.SelectMany(lambda(x: int) => do "
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
        "  for var z in a.Zip(b, lambda(x: int, y: int) => x + y) do s := s + z; n := n + 1; end; "
        "  return int(s = 33 and n = 2); "  // 11 + 22, length 2 (shorter)
        "end Main; end C;"), 1);
}

// --- Zip with a `string` element type (#1845): the combiner is `(T,U)->R` and
// its parameters were left UN-annotated, so before the fix sema never seeded
// them → the combiner's return type R never resolved → the Zip result was typed
// IEnumerable<T> (the receiver element) instead of IEnumerable<R>, and codegen
// sized the yielded element / built the combine trampoline+closure return by the
// wrong type. Two distinct faults, both requiring `string` (a runtime-managed
// reference) as one of T/U/R. The existing `zip_spec_5` above cannot catch these
// — its params are annotated `(x: int, y: int)`, so R was always resolved.

// MEMORY-SAFETY regression (stdlib §5 Zip<U,R> / spec §29.4 lazy evaluation,
// v0.196): a `string`-RECEIVER Zip with an `int` result. Before the fix the
// result was typed IEnumerable<string>, so the for-loop consumer read the 4-byte
// int `cur` at 8-byte (string) width → deterministic heap-buffer-overflow READ
// of size 8 (SIGSEGV). runAotChecked's ASan catches it: fails-without (r.ran
// false → -999), passes-with (3, ASan-clean).
BEZEL_TEST(linq_complete, zip_string_receiver_int_result_no_overflow_spec_5) {
    // "aa".Length + "x".Length = 2 + 1 = 3. Un-annotated (p, q) infer to string.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var a: Array<string> := [\"aa\"]; "
        "  using var b: Array<string> := [\"x\"]; "
        "  var s := 0; "
        "  for var v in a.Zip(b, lambda(p, q) => p.Length + q.Length) do s := s + v; end; "
        "  return s; "
        "end Main; end C;"), 3);
}

// MALFORMED-IR regression (stdlib §5 Zip<U,R> / spec §29.4, v0.196): a
// `string`-RESULT combiner `(p, q) => p + q`. Before the fix R stayed unresolved,
// so the combine closure returned `ptr` (string) against an `i32`-typed function
// → "Function return type does not match operand type of return inst" → the
// compile ABORTED. Fails-without (compile abort → r.ran false → -999),
// passes-with (compiles; "aa"+"x" = "aax", .Length = 3).
//
// BEHAVIOUR FLIP (#7201): this row used to assert that the MINTING string-result
// combiner COMPILES and yields 3. It no longer compiles: a `Zip` combiner that
// MINTS a fresh value is the §28.3 laundering class (stdlib §5 classifies the
// combined element as a receiver-sourced borrow, so the lazy pipeline takes no
// disposal obligation on it and every minted combination leaks — 72 B in 4
// allocations for this shape), and #7201 closes it by rejecting the launder at
// the combiner, exactly as the `Select` (#1917) and `Min`/`Max` (#1915) gates
// reject their minting selectors. The row now asserts the §28.3 REJECT.
//
// The MALFORMED-IR guard this row carried is preserved by its sibling below —
// a BORROWING string-result combiner (`(p, q) => p`), which reaches codegen
// with R = string and so exercises the same combine-trampoline return type,
// and is admitted because it mints nothing.
BEZEL_TEST(linq_complete, zip_string_result_combiner_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var a: Array<string> := [\"aa\"]; "
        "  using var b: Array<string> := [\"x\"]; "
        "  var n := 0; "
        "  for var v in a.Zip(b, lambda(p, q) => p + q) do n := n + v.Length; end; "
        "  return n; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawLaunder = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Zip` combiner that mints") != std::string::npos)
            sawLaunder = true;
    }
    BEZEL_EXPECT_TRUE(sawLaunder);
}

// MALFORMED-IR regression (stdlib §5 Zip<U,R> / spec §29.4, v0.196), BORROWING
// spelling: a `string`-RESULT combiner that MINTS NOTHING — `(p, q) => p`
// returns the LEFT element's own string. R = string, so the combine closure
// returns `ptr` (string) against a wrongly-`i32`-typed function would abort
// the compile; the row pins that it COMPILES and yields the left element's
// length (2). This is the malformed-IR guard the flipped row above (a MINTING
// string-result combiner, now refused by #7201) can no longer carry — the
// borrowing spelling still reaches codegen with R = string.
//
// Uses runAotChecked: the borrowed element is owned by the source array, so the
// pipeline is leak-free and LSan stays silent.
BEZEL_TEST(linq_complete, zip_string_result_borrow_combiner_compiles_spec_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var a: Array<string> := [\"aa\"]; "
        "  using var b: Array<string> := [\"x\"]; "
        "  var n := 0; "
        "  for var v in a.Zip(b, lambda(p, q) => p) do n := n + v.Length; end; "
        "  return n; "
        "end Main; end C;"), 2);
}

// Value-type control (stdlib §5 Zip<U,R> / spec §29.4, v0.196): un-annotated
// (p, q) over int arrays — the common value case. Confirms the new combiner
// param-seeding does not regress inferred value-typed Zips and stays ASan-clean.
BEZEL_TEST(linq_complete, zip_value_result_inferred_params_no_regression_spec_5) {
    // [1,2,3] zip [10,20] with (p,q)=>p+q = [11,22]; sum = 33 (length = shorter).
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  using var b: Array<int> := [10, 20]; "
        "  var s := 0; "
        "  for var v in a.Zip(b, lambda(p, q) => p + q) do s := s + v; end; "
        "  return s; "
        "end Main; end C;"), 33);
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
        "  for var x in a.Where(lambda(x) => x mod 2 = 0).Concat(b).Take(3) do s := s + x; end; "
        "  return s; "  // evens of a = [2,4], concat [5,6] = [2,4,5,6], take 3 = [2,4,5] = 11
        "end Main; end C;"), 11);
}
