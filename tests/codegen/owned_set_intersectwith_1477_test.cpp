// Codegen — fix #1477 / stdlib §4.10: OwningSet<T>.IntersectWith(other:
// IEnumerable<T>) for a CLASS-typed T.
//
// OwningSet<T> requires `where T: class` (stdlib §4.10), so its element is
// never eligible for the generic-key dict backing (genericBackingType,
// LlvmBackend_Calls3.cpp, returns null for every class element) — every valid
// OwningSet<T> instantiation is therefore ARRAY-backed, exactly like
// OwningArray<T>. Before this fix, IntersectWith's only implementation lived
// inside the (for OwningSet, always-dead) dict-backed `genericBackingType`
// guard in tryLowerSetAlgebra, so the array-backed path had no IntersectWith
// handler at all: `tryLowerCollectionInstanceCall` returned null and the call
// was silently emitted as a zero (unit) result with no side effects — a silent
// no-op (issue #1477: Count and the destructor counter both unchanged).
//
// This file exercises the array-backed .IntersectWith handler added right
// after .ExceptWith (fix #1478) in LlvmBackend_Calls.cpp — the same
// materialize-`other`-once-then-scan-self shape, with the match outcome
// inverted. Per §4.10 IntersectWith "disposes elements not in other": `other`
// is a BORROW (not `owned`), so its elements stay owned by `other`; self keeps
// only the elements that ALSO appear in `other`, and every self element that
// does NOT appear in `other` is removed AND disposed (§28/§29.2 disposal
// rules — exactly-once). Match is by §8.3 equality, honouring a user Equals
// override (emitRemoveElementEquality), the same contract Add/UnionWith/
// ExceptWith use.
//   * a self element equal to some element of `other` is KEPT, never disposed;
//   * a self element NOT matched by any element of `other` is removed and
//     disposed exactly once (not zero — the no-op bug; not more than once —
//     double free);
//   * an element of `other` NOT in self removes nothing;
//   * `other`'s own elements are never disposed by IntersectWith (they remain
//     owned by `other`, disposed at `other`'s own scope end — no double free).

// AOT MIGRATION (JIT->AOT): runs under the AOT driver (runAot) — the stricter
// oracle (full ownership + definite-assignment pipeline). Tests whose packed
// return value exceeds 255 (the 8-bit exit-code ceiling) print it via
// Console.WriteLine and assert on stdout through runAotOut.
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

// A class with a user Equals/GetHashCode override keyed on field X — mirrors
// owned_set_exceptwith_1478_test.cpp's `Box` so IntersectWith's match is
// proven to honour the §8.3 equality contract (a user Equals override),
// letting `other` hold DISTINCT-but-equal instances of the set's elements.
// `disposeCount` counts every destructor call (for exactly-once accounting);
// `n` is the LIVE count (ctor +1, dtor -1).
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

// stdlib §4.10: IntersectWith keeps every self element that ALSO appears in
// `other`, and removes+disposes every self element that does not — the
// #1477 repro. self {1,2} ∩ other {1,3}: Box(1) matches and is kept, Box(2)
// has no match and is removed+disposed, so Count == 1, and Box.n drops from
// 4 constructed to 3 live. Proves the call is NOT a no-op.
BEZEL_TEST(owned_set_intersectwith_1477, keeps_matching_removes_nonmatching_spec_4_10) {
    // 301 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(1))); "  // value-equal to s's Box(1)
        "  other.Add(move (new Box(3))); "  // not in s
        "  s.IntersectWith(other); "
        // s = {1} → Count 1; 4 Boxes built, 1 (Box(2)) disposed → Box.n 3.
        "  Console.WriteLine(Box.n * 100 + s.Count); return 0; "  // 3*100 + 1 = 301
        "end Main; end C;"), "301");
}

// §4.10 / §28 / §29.2: the removed (non-matching) element is disposed EXACTLY
// once — 4 Boxes are constructed, IntersectWith disposes exactly 1 (Box(2),
// the non-match) immediately, and the remaining 3 (s's Box(1), other's Box(1)
// and Box(3)) are disposed at their owners' scope end — 4 destructor calls
// total, never more (double free) and never fewer (leak). Box.n (live count)
// must settle to 0.
BEZEL_TEST(owned_set_intersectwith_1477, nonmatching_element_disposed_exactly_once_spec_4_10) {
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
        "    s.IntersectWith(other); "
        "    midDisp := Box.disposeCount; "  // exactly 1 disposed by IntersectWith
        "  end; "
        // midDisp 1, total disposeCount 4, live Box.n 0.
        "  Console.WriteLine(midDisp * 10000 + Box.disposeCount * 100 + Box.n); return 0; "
        // 1*10000 + 4*100 + 0 = 10400
        "end Main; end C;"), "10400");
}

// §4.10: IntersectWith with an `other` sharing NO element with self removes
// every self element — Count drops to 0, both self elements disposed (Box.n
// keeps only other's 2 elements alive).
BEZEL_TEST(owned_set_intersectwith_1477, disjoint_other_empties_self_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(3))); "
        "  other.Add(move (new Box(4))); "
        "  s.IntersectWith(other); "
        // s emptied → Count 0; both self elements disposed → Box.n 2 (other's).
        "  return Box.n * 100 + s.Count; "  // 2*100 + 0 = 200
        "end Main; end C;"), 200);
}

// §4.10: IntersectWith with an EMPTY other empties self entirely — nothing in
// self can match an empty `other`, so every self element is removed and
// disposed.
BEZEL_TEST(owned_set_intersectwith_1477, empty_other_empties_self_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  using var other := new OwningArray<Box>(); "
        "  s.IntersectWith(other); "
        "  return Box.n * 100 + s.Count; "  // 0*100 + 0 = 0
        "end Main; end C;"), 0);
}

// §4.10: an `other` covering EVERY self element keeps self entirely unchanged
// — Count stays 3, nothing disposed (Box.n holds all 6 constructed: 3 self +
// 3 other, none disposed by IntersectWith).
BEZEL_TEST(owned_set_intersectwith_1477, other_covers_all_keeps_everything_spec_4_10) {
    // 603 > 255 → print + runAotOut.
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
        "  s.IntersectWith(other); "
        // s unchanged → Count 3; nothing disposed → Box.n 6.
        "  Console.WriteLine(Box.n * 100 + s.Count); return 0; "  // 6*100 + 3 = 603
        "end Main; end C;"), "603");
}

// §4.10: self is fully usable after IntersectWith — a subsequent Add grows
// Count, and the array backing (compacted by the removals) still deduplicates.
BEZEL_TEST(owned_set_intersectwith_1477, self_reusable_after_intersect_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "  // no match in other → disposed
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(1))); "  // keeps s's Box(1) → s = {1}
        "  s.IntersectWith(other); "
        "  s.Add(move (new Box(5))); "      // s = {1,5}
        "  return s.Count; "  // 2
        "end Main; end C;"), 2);
}

// §4.10 / §5: `other` may be any IEnumerable<T>, not only a concrete
// collection — a borrowing Array<Box> source works too (materialized via the
// same lazy-enumerator path). Box(1) matches q1 and is kept; Box(2) has no
// match and is removed+disposed; the Array only borrows its elements (they
// are owned by the `using var` boxes), so IntersectWith never disposes them.
BEZEL_TEST(owned_set_intersectwith_1477, other_is_borrowing_array_spec_4_10) {
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
        "  s.IntersectWith(other); "
        // s = {1} → Count 1; s's Box(2) disposed; q1/q3 still live → Box.n 3.
        "  Console.WriteLine(Box.n * 100 + s.Count); return 0; "  // 3*100 + 1 = 301
        "end Main; end C;"), "301");
}
