// Codegen — issue #1259 / stdlib §4.10: pin OwningSet<T> set-algebra
// (UnionWith / IntersectWith / ExceptWith) for a CLASS-typed T.
//
// #1259 reported that OwningSet's three set-algebra methods were silent
// NO-OPS and that UnionWith additionally LEAKED the moved source set's
// elements (the issue's repro constructs s1 with 2 elements, s2 with 3,
// and calls s1.UnionWith(move s2); only s1's original 2 were disposed —
// rc == 2 — while the 3 drained from s2 leaked; IntersectWith/ExceptWith
// left Count unchanged). The issue's prose/identifier used the pre-v0.168
// name `OwnedSet` (renamed `OwningSet` in #1372); the stdlib type (§4.10)
// is `OwningSet<T>`, which requires `where T: class`, so every valid
// instantiation is ARRAY-backed (genericBackingType returns null for class
// elements) and the dict-backed tryLowerSetAlgebra guard is dead for it.
//
// The bug was fixed by the array-backed handlers in LlvmBackend_Calls.cpp:
//   * UnionWith  (fix #1474) — drains `other` element-by-element through the
//     same dedup-insert helper Add uses, then frees `other`'s emptied backing
//     with a NULL disposer (every element already resolved).
//   * ExceptWith (fix #1478) — materializes the borrowed `other` once into a
//     throwaway snapshot, scans self removing+disposing each matching element.
//   * IntersectWith (fix #1477) — same shape as ExceptWith with the match
//     outcome inverted (keeps matching, disposes non-matching).
// Before those handlers, tryLowerCollectionInstanceCall returned null for all
// three and the call was silently emitted as a zero (unit) result with no
// side effects — a no-op — and the `move s2` argument to UnionWith was
// leaked (its elements were already moved out of the caller's variable, so
// the caller no longer owned them and never disposed them).
//
// This file pins #1259's exact repro so a regression of ANY of the three
// handlers is caught. Each assertion takes the value #1259 recorded as the
// BUG (rc==2 / Count unchanged) and proves the fix produces the spec-correct
// value instead (stdlib §4.10):
//   * UnionWith  — all drained elements disposed at scope end (no leak);
//   * IntersectWith — self ∩ other keeps only matching elements;
//   * ExceptWith    — self \ other removes+disposes matching elements.
// Disposal is exactly-once (§28/§29.2): no leak (fewer than expected) and no
// double-free (more than expected).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

// Destructor-counting class with NO Equals override (§8.3 falls back to
// reference identity, so every `new Track()` is distinct). Mirrors the `R`
// class in the #1259 UnionWith repro: `disposed` counts destructor calls.
const char* kTrack =
    "module M; class Track "
    "  public static disposed: int := 0; "
    "  public constructor() end constructor; "
    "  destructor disposed := disposed + 1; end destructor; "
    "end Track; class C ";

// A class with a user Equals/GetHashCode override keyed on field X (§8.3) —
// lets `other` hold DISTINCT-but-equal instances of a set's elements, so
// IntersectWith/ExceptWith matching is proven to honour the equality contract
// (not just reference identity). `n` is the LIVE count (ctor +1, dtor -1).
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

// #1259 / stdlib §4.10: UnionWith(owned other) drains `other` into self — the
// issue's exact repro. s1 holds 2 distinct Track instances, s2 holds 3; after
// UnionWith(move s2) all 5 are owned by s1 and disposed exactly once at s1's
// scope end. Pre-fix (#1259): the call no-opped, s2's 3 elements leaked, and
// only s1's original 2 were disposed → rc == 2.
BEZEL_TEST(owned_set_algebra_1259, unionwith_drains_other_no_leak_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kTrack) +
        "public static method Main(): int "
        "  do "
        "    using var s1 := new OwningSet<Track>(); "
        "    s1.Add(new Track()); "
        "    s1.Add(new Track()); "
        "    var s2 := new OwningSet<Track>(); "
        "    s2.Add(new Track()); "
        "    s2.Add(new Track()); "
        "    s2.Add(new Track()); "
        "    s1.UnionWith(move s2); "
        "  end; "
        // 5 Track instances constructed; after s1's scope-end disposal all 5
        // are destructed exactly once (the 3 drained from s2 did not leak).
        "  return Track.disposed; "  // 5 (was 2 pre-fix — the leak)
        "end Main; end C;"), 5);
}

// #1259 / stdlib §4.10: UnionWith drains `other` into self — the §8.3 dedup
// path. s1 {1,2}, s2 {1,3,4}: Box(1) in s2 is value-equal to s1's Box(1)
// (honouring the Equals override) and is disposed immediately rather than
// appended; Box(3), Box(4) transfer into s1. Result Count is the distinct
// union {1,2,3,4} = 4, and the duplicate's disposal drops n from 5 built to
// 4 live. Pre-fix (#1259): no-op → Count stayed 2 and the 3 moved s2
// elements leaked (n stayed 5).
BEZEL_TEST(owned_set_algebra_1259, unionwith_duplicate_disposed_not_appended_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s1 := new OwningSet<Box>(); "
        "  s1.Add(move (new Box(1))); "
        "  s1.Add(move (new Box(2))); "
        "  var s2 := new OwningSet<Box>(); "
        "  s2.Add(move (new Box(1))); "  // value-equal to s1's Box(1) → dup
        "  s2.Add(move (new Box(3))); "
        "  s2.Add(move (new Box(4))); "
        "    s1.UnionWith(move s2); "
        // union {1,2} ∪ {1,3,4} = {1,2,3,4} → Count 4; 5 built, 1 dup disposed
        // → Box.n 4. (Pre-fix no-op: Count 2, n 5 → 502.)
        "  return Box.n * 100 + s1.Count; "  // 4*100 + 4 = 404
        "end Main; end C;"), 404);
}

// #1259 / stdlib §4.10: IntersectWith(other) "disposes elements not in other"
// — keeps only self ∩ other. self {1,2}, other {1,3}: Box(1) matches and is
// kept, Box(2) has no match and is removed+disposed. Pre-fix (#1259): the call
// no-opped and Count stayed at 2.
BEZEL_TEST(owned_set_algebra_1259, intersectwith_keeps_only_intersection_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(1))); "  // value-equal to s's Box(1)
        "  other.Add(move (new Box(3))); "  // not in s
        "  s.IntersectWith(other); "
        // s = {1} → Count 1; 4 Boxes built, 1 (Box(2)) disposed → Box.n 3.
        "  return Box.n * 100 + s.Count; "  // 3*100 + 1 = 301 (was 402 pre-fix)
        "end Main; end C;"), 301);
}

// #1259 / stdlib §4.10: ExceptWith(other) "disposes elements in other" —
// removes self ∩ other. self {1,2,3}, other {2}: Box(2) is matched, removed,
// and disposed; {1,3} are kept. Pre-fix (#1259): the call no-opped and Count
// stayed at 3.
BEZEL_TEST(owned_set_algebra_1259, exceptwith_removes_matched_elements_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var s := new OwningSet<Box>(); "
        "  s.Add(move (new Box(1))); "
        "  s.Add(move (new Box(2))); "
        "  s.Add(move (new Box(3))); "
        "  using var other := new OwningArray<Box>(); "
        "  other.Add(move (new Box(2))); "  // value-equal to s's Box(2)
        "  s.ExceptWith(other); "
        // s = {1,3} → Count 2; 4 Boxes built, 1 (Box(2)) disposed → Box.n 3.
        "  return Box.n * 100 + s.Count; "  // 3*100 + 2 = 302 (was 403 pre-fix)
        "end Main; end C;"), 302);
}
