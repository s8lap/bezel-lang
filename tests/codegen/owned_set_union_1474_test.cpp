// Codegen — fix #1474 / stdlib §4.10: OwningSet<T>.UnionWith(owned other:
// OwningSet<T>) for a CLASS-typed T.
//
// OwningSet<T> requires `where T: class` (stdlib §4.10), so its element is
// never eligible for the generic-key dict backing (genericBackingType,
// LlvmBackend_Calls3.cpp, returns null for every class element) — every valid
// OwningSet<T> instantiation is therefore ARRAY-backed, exactly like
// OwningArray<T>. Before this fix, UnionWith's only implementation lived
// inside the (for OwningSet, always-dead) dict-backed `genericBackingType`
// guard, so the array-backed path had no UnionWith handler at all:
// `tryLowerCollectionInstanceCall` returned null, the call silently no-opped,
// and `other`'s elements (already moved out of the caller's variable, so the
// caller no longer owns them and never disposes them) were leaked for the
// remainder of the process — set_dispose_leak_1012 tracked the general "Set
// leaks its dict struct" family, but this array-backed-class-T slice
// (UnionWith specifically) was untouched by that fix.
//
// This file exercises the array-backed .UnionWith handler added alongside the
// existing class-element .Add/.TryAdd handlers in LlvmBackend_Calls.cpp:
//   * every element of `other` ends up in `self` (transferred, not leaked);
//   * a duplicate (per §8.3 equality — a user Equals override, else reference
//     identity) is disposed exactly once, not appended;
//   * `other` is left with Count == 0 and its own backing is reclaimed (no
//     leak of the header/backing array itself);
//   * every element - whether kept in self or disposed as a duplicate - is
//     destructed EXACTLY once (no leak, no double-free).

// AOT MIGRATION (JIT->AOT): runs under the AOT driver (runAot) — the stricter
// oracle that runs the full ownership + definite-assignment pipeline. Tests
// whose packed return value exceeds 255 (the 8-bit exit-code ceiling) print it
// via Console.WriteLine and assert on stdout through runAotOut.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Destructor-counting class, no Equals override (§8.3 reference-identity
// default dedup).
const char* kCell =
    "module M; class Cell "
    "  public static n: int := 0; "
    "  public static disposeCount: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor "
    "    Cell.n := Cell.n - 1; "
    "    Cell.disposeCount := Cell.disposeCount + 1; "
    "  end destructor; "
    "end Cell; class C ";

// A class with a user Equals/GetHashCode override keyed on field X — mirrors
// owned_set_equality_837_test.cpp's `Box` so UnionWith's dedup is proven to
// honour the same §8.3 equality contract as Add/TryAdd, not reference
// identity.
const char* kBox =
    "module M; class Box "
    "  public static n: int := 0; "
    "  public X: int; "
    "  public constructor(x: int) X := x; n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Box; class C ";
}  // namespace

// stdlib §4.10: UnionWith drains `other` into self — the #1474 repro. Two
// disjoint single-element sets: after UnionWith, self holds both (Count == 2)
// and every element is still alive (Cell.n == 2, none leaked or freed).
BEZEL_TEST(owned_set_union_1474, disjoint_union_merges_both_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kCell) +
        "public static method Main(): int "
        "  using var s1 := new OwningSet<Cell>(); "
        "  s1.Add(new Cell()); "
        "  var s2 := new OwningSet<Cell>(); "
        "  s2.Add(new Cell()); "
        "  s1.UnionWith(move s2); "
        "  return Cell.n * 100 + s1.Count; "  // 2*100 + 2 = 202
        "end Main; end C;"), 202);
}

// §4.10: after UnionWith, `other` is fully consumed — using its now-moved-from
// slot again is a compile-time error (checked statically), so what matters at
// runtime is that `other`'s own backing never leaks and its elements are
// never double-freed. Drain self back out via a second UnionWith-independent
// check: dispose self at scope end and confirm every element that was merged
// in is destructed exactly once (not zero — leaked; not more than once —
// double free).
BEZEL_TEST(owned_set_union_1474, merged_elements_disposed_exactly_once_spec_4_10) {
    // Packed value 20220 exceeds the 8-bit exit-code ceiling → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kCell) +
        "public static method Main(): int "
        "  var midCount := 0; "
        "  var midN := 0; "
        "  do "
        "    using var s1 := new OwningSet<Cell>(); "
        "    s1.Add(new Cell()); "
        "    var s2 := new OwningSet<Cell>(); "
        "    s2.Add(new Cell()); "
        "    s1.UnionWith(move s2); "
        "    midCount := s1.Count; "
        "    midN := Cell.n; "
        "  end; "
        // after s1's using-var disposal: both cells destructed exactly once.
        "  Console.WriteLine(midCount * 10000 + midN * 100 + Cell.disposeCount * 10 + Cell.n); return 0; "
        // 2*10000 + 2*100 + 2*10 + 0 = 20220
        "end Main; end C;"), "20220");
}

// §4.10 / §8.3: UnionWith merging a set that contains a DUPLICATE (per the
// receiver's equality contract) disposes the duplicate rather than appending
// it — Count reflects only the distinct union, and the duplicate is
// destructed immediately (not leaked to process exit, not kept as a second
// live copy).
BEZEL_TEST(owned_set_union_1474, duplicate_element_disposed_not_appended_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s1 := new OwningSet<Box>(); "
        "  s1.Add(move (new Box(1))); "
        "  s1.Add(move (new Box(2))); "
        "  var s2 := new OwningSet<Box>(); "
        "  s2.Add(move (new Box(1))); "  // content-equal to s1's Box(1)
        "  s2.Add(move (new Box(3))); "
        "  s1.UnionWith(move s2); "
        // union {1,2} ∪ {1,3} = {1,2,3} → Count 3; all 4 constructed Boxes
        // disposed exactly once (the duplicate immediately, the rest at
        // scope end) → Box.n (live count) settles to 3 here, 0 after scope.
        "  return s1.Count; "  // 3
        "end Main; end C;"), 3);
}

// §4.10 / §8.3: the duplicate's disposal count is exact — 4 Boxes are
// constructed (2 in s1, 2 in s2, one of which duplicates s1's), UnionWith
// disposes exactly 1 (the duplicate) immediately, and the remaining 3 are
// disposed when s1 goes out of scope — 4 destructor calls total, never more
// (double-free) and never fewer (leak).
BEZEL_TEST(owned_set_union_1474, duplicate_and_kept_elements_total_disposals_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do "
        "    using var s1 := new OwningSet<Box>(); "
        "    s1.Add(move (new Box(1))); "
        "    s1.Add(move (new Box(2))); "
        "    var s2 := new OwningSet<Box>(); "
        "    s2.Add(move (new Box(1))); "
        "    s2.Add(move (new Box(3))); "
        "    s1.UnionWith(move s2); "
        "  end; "
        // Box.n is the LIVE count (ctor increments, dtor decrements) — after
        // all 4 constructions and all 4 disposals it must be back to 0.
        "  return Box.n; "
        "end Main; end C;"), 0);
}

// §4.10: after UnionWith, `other`'s backing is fully reclaimed — Count on the
// (fully consumed) receiver side is correct and self remains usable
// afterward (Add still works, growing Count further).
BEZEL_TEST(owned_set_union_1474, self_reusable_after_union_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kCell) +
        "public static method Main(): int "
        "  using var s1 := new OwningSet<Cell>(); "
        "  s1.Add(new Cell()); "
        "  var s2 := new OwningSet<Cell>(); "
        "  s2.Add(new Cell()); "
        "  s1.UnionWith(move s2); "
        "  s1.Add(new Cell()); "  // Add after UnionWith
        "  return s1.Count; "  // 3
        "end Main; end C;"), 3);
}

// §4.10: unioning with an EMPTY other is a no-op on self's contents (Count
// unchanged) and disposes nothing (other had nothing to drain).
BEZEL_TEST(owned_set_union_1474, union_with_empty_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kCell) +
        "public static method Main(): int "
        "  using var s1 := new OwningSet<Cell>(); "
        "  s1.Add(new Cell()); "
        "  var s2 := new OwningSet<Cell>(); "
        "  s1.UnionWith(move s2); "
        "  return Cell.n * 100 + s1.Count; "  // 1*100 + 1 = 101
        "end Main; end C;"), 101);
}
