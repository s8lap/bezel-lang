// Codegen — fix #1478 / stdlib §4.10: OwningSet<T>.ExceptWith(other:
// IEnumerable<T>) for a CLASS-typed T.
//
// OwningSet<T> requires `where T: class` (stdlib §4.10), so its element is
// never eligible for the generic-key dict backing (genericBackingType,
// LlvmBackend_Calls3.cpp, returns null for every class element) — every valid
// OwningSet<T> instantiation is therefore ARRAY-backed, exactly like
// OwningArray<T>. Before this fix, ExceptWith's only implementation lived
// inside the (for OwningSet, always-dead) dict-backed `genericBackingType`
// guard in tryLowerSetAlgebra, so the array-backed path had no ExceptWith
// handler at all: `tryLowerCollectionInstanceCall` returned null and the call
// was silently emitted as a zero (unit) result with no side effects — a silent
// no-op (issue #1478: Count and the destructor counter both unchanged).
//
// This file exercises the array-backed .ExceptWith handler added alongside the
// existing class-element .Add/.TryAdd/.UnionWith handlers in
// LlvmBackend_Calls.cpp. Per §4.10 ExceptWith "disposes elements in other":
// `other` is a BORROW (not `owned`), so its elements stay owned by `other`;
// only self's matching elements are removed AND disposed (§28/§29.2 disposal
// rules — exactly-once). Match is by §8.3 equality, honouring a user Equals
// override (emitRemoveElementEquality), the same contract Add/UnionWith use.
//   * a self element equal to some element of `other` is removed and disposed
//     exactly once (not zero — the no-op bug; not more than once — double free);
//   * a self element NOT in `other` is kept;
//   * an element of `other` NOT in self removes nothing;
//   * `other`'s own elements are never disposed by ExceptWith (they remain
//     owned by `other`, disposed at `other`'s own scope end — no double free).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
// AOT MIGRATION (JIT->AOT): runs under the AOT driver (runAot) — the stricter
// oracle (full ownership + definite-assignment pipeline). Tests whose packed
// return value exceeds 255 (the 8-bit exit-code ceiling) print it via
// Console.WriteLine and assert on stdout through runAotOut.
#include "support/aot_support.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A class with a user Equals/GetHashCode override keyed on field X — mirrors
// owned_set_union_1474_test.cpp's `Box` so ExceptWith's match is proven to
// honour the §8.3 equality contract (a user Equals override), letting `other`
// hold DISTINCT-but-equal instances of the set's elements. `disposeCount`
// counts every destructor call (for exactly-once accounting); `n` is the LIVE
// count (ctor +1, dtor -1).
const char* kBox =
    "module M; class Box "
    "  public static n: int := 0; "
    "  public static disposeCount: int := 0; "
    "  public X: int; "
    "  public constructor(x: int) X := x; n := n + 1; end constructor; "
    "  destructor n := n - 1; disposeCount := disposeCount + 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Box; class C ";
}  // namespace

// stdlib §4.10: ExceptWith removes (and disposes) every self element that
// appears in `other` — the #1478 repro. self {1,2} \ other {1,3} = {2}: the
// matching Box(1) is removed, so Count == 1, and it is disposed (Box.n drops
// from 4 constructed to 3 live). Proves the call is NOT a no-op.
BEZEL_TEST(owned_set_exceptwith_1478, removes_and_disposes_matching_element_spec_4_10) {
    // 301 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(1))); "  // value-equal to s's Box(1)
        "  other.Add(move (new Box(3))); "  // not in s
        "  s.ExceptWith(other); "
        // s = {2} → Count 1; 4 Boxes built, 1 disposed by ExceptWith → Box.n 3.
        "  Console.WriteLine(Box.n * 100 + s.Count); return 0; "  // 3*100 + 1 = 301
        "end Main; end C;"), "301");
}

// §4.10 / §28 / §29.2: the removed element is disposed EXACTLY once — 4 Boxes
// are constructed, ExceptWith disposes exactly 1 (the match) immediately, and
// the remaining 3 (s's Box(2), other's Box(1) and Box(3)) are disposed at
// their owners' scope end — 4 destructor calls total, never more (double free)
// and never fewer (leak). Box.n (live count) must settle to 0.
BEZEL_TEST(owned_set_exceptwith_1478, matching_element_disposed_exactly_once_spec_4_10) {
    // Packed value 10400 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  var midDisp := 0; "
        "  do "
        "    using var s := new OwningSet<Box>(); "
        "    s.Add(move (new Box(1))); "
        "    s.Add(move (new Box(2))); "
        "    using var other := new OwningArray<Box>(); "
        "    other.Add(move (new Box(1))); "
        "    other.Add(move (new Box(3))); "
        "    s.ExceptWith(other); "
        "    midDisp := Box.disposeCount; "  // exactly 1 disposed by ExceptWith
        "  end; "
        // midDisp 1, total disposeCount 4, live Box.n 0.
        "  Console.WriteLine(midDisp * 10000 + Box.disposeCount * 100 + Box.n); return 0; "
        // 1*10000 + 4*100 + 0 = 10400
        "end Main; end C;"), "10400");
}

// §4.10: ExceptWith with an `other` sharing NO element with self removes
// nothing — Count unchanged, nothing disposed (Box.n stays at all 4 live).
BEZEL_TEST(owned_set_exceptwith_1478, disjoint_other_removes_nothing_spec_4_10) {
    // 402 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(3))); "
        "  other.Add(move (new Box(4))); "
        "  s.ExceptWith(other); "
        // s unchanged {1,2} → Count 2; nothing disposed → Box.n 4.
        "  Console.WriteLine(Box.n * 100 + s.Count); return 0; "  // 4*100 + 2 = 402
        "end Main; end C;"), "402");
}

// §4.10: ExceptWith with an EMPTY other is a no-op on self's contents (Count
// unchanged) and disposes nothing.
BEZEL_TEST(owned_set_exceptwith_1478, except_with_empty_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  using var other := new OwningArray<Box>(); "
        "  s.ExceptWith(other); "
        "  return Box.n * 100 + s.Count; "  // 1*100 + 1 = 101
        "end Main; end C;"), 101);
}

// §4.10: an `other` covering EVERY self element empties self — all 3 self
// elements are removed and disposed (Box.n drops from 6 constructed to 3),
// Count == 0. The three `other` elements remain owned by `other`.
BEZEL_TEST(owned_set_exceptwith_1478, other_covers_all_empties_self_spec_4_10) {
    // 300 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  s.Add(move (new Box(3))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(1))); "
        "  other.Add(move (new Box(2))); "
        "  other.Add(move (new Box(3))); "
        "  s.ExceptWith(other); "
        // s emptied → Count 0; 3 of 6 disposed by ExceptWith → Box.n 3.
        "  Console.WriteLine(Box.n * 100 + s.Count); return 0; "  // 3*100 + 0 = 300
        "end Main; end C;"), "300");
}

// §4.10: self is fully usable after ExceptWith — a subsequent Add grows Count,
// and the array backing (compacted by the removals) still deduplicates.
BEZEL_TEST(owned_set_exceptwith_1478, self_reusable_after_except_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(1))); "  // removes s's Box(1) → s = {2}
        "  s.ExceptWith(other); "
        "  s.Add(move (new Box(5))); "      // s = {2,5}
        "  return s.Count; "  // 2
        "end Main; end C;"), 2);
}

// §4.10 / §5: `other` may be any IEnumerable<T>, not only a concrete
// collection — a borrowing Array<Box> source works too (materialized via the
// same lazy-enumerator path). Box(1) matches and is removed+disposed; the
// Array only borrows its elements (they are owned by the `using var` boxes),
// so ExceptWith never disposes them.
BEZEL_TEST(owned_set_exceptwith_1478, other_is_borrowing_array_spec_4_10) {
    // 301 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  using var q1 := new Box(1); "  // value-equal to s's Box(1)
        "  using var q3 := new Box(3); "
        "  using var other := new Array<Box>(); "
        "  other.Add(q1); "
        "  other.Add(q3); "
        "  s.ExceptWith(other); "
        // s = {2} → Count 1; s's Box(1) disposed; q1/q3 still live → Box.n 3.
        "  Console.WriteLine(Box.n * 100 + s.Count); return 0; "  // 3*100 + 1 = 301
        "end Main; end C;"), "301");
}
