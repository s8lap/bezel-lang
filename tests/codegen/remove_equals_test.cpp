// Codegen — Array<T>/OwningArray<T>.Remove(item) matches via the element type's
// EQUALITY, not just reference identity (issue #400).
//
// stdlib §4.4/§4.5: `Remove(item): bool` finds and removes the first matching
// element. §25.2: "collections … use `Equals` rather than `=`." §8.3: a class
// defaults to reference equality but may OVERRIDE `Equals`; a positional record
// defaults to field-by-field value equality; a primitive uses value comparison.
// §4.6: strings compare by content. §8.4: for classes, `=` always invokes
// `Equals`.
//
// The Remove query argument is a PURE BORROW (§4.5: "borrow-taking query"): the
// caller still owns it; Remove only compares. A matched OwningArray element is
// DISPOSED exactly once on removal; a borrowing Array's Remove disposes nothing.
//
// Disposal is made observable through a static destructor counter returned in
// the exit code. Reference-identity matching (the pre-fix behavior) fails the
// user-Equals cases (returns the not-found sentinel), so those are load-bearing.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §25.2/§8.3/§8.4: an OwningArray<Box> element matches an equal-but-DISTINCT Box
// through the user `Equals` override (not reference identity). The query Box(2)
// is a different instance from the stored Box(2); reference identity would not
// match, but value equality (X = X) does. Remove returns true and Count drops to
// 2. This is the issue #400 reproducer — pre-fix it returns 99.
BEZEL_TEST(remove_equals_4_5, owned_array_remove_via_user_equals_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is Box then var b := other as Box; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Box>(); "
        "  a.Add(move (new Box(1))); "
        "  a.Add(move (new Box(2))); "
        "  a.Add(move (new Box(3))); "
        "  using var query := new Box(2); "
        "  var removed := a.Remove(query); "
        "  if removed then return a.Count; end; "
        "  return 99; "
        "end Main; end C;"), 2);
}

// §25.2/§8.3/§8.4: same dispatch on a BORROWING Array<Box>. An equal-but-distinct
// Box matches via Equals; Remove returns true and Count drops to 1. Pre-fix this
// returns 99 (reference identity misses).
BEZEL_TEST(remove_equals_4_4, borrowing_array_remove_via_user_equals_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is Box then var b := other as Box; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b1 := new Box(1); using var b2 := new Box(2); "
        "  var a := new Array<Box>(); a.Add(b1); a.Add(b2); "
        "  using var query := new Box(2); "
        "  var removed := a.Remove(query); "
        "  var n := a.Count; a.Dispose(); "
        "  if removed then return n; end; "
        "  return 99; "
        "end Main; end C;"), 1);
}

// §8.3: a class with NO `Equals` override keeps the default REFERENCE-identity
// match. The SAME instance matches (Remove → true, Count 1).
BEZEL_TEST(remove_equals_8_3, no_override_reference_identity_same_instance) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b1 := new Box(1); using var b2 := new Box(2); "
        "  var a := new Array<Box>(); a.Add(b1); a.Add(b2); "
        "  var removed := a.Remove(b2); "  // same instance → identity match
        "  var n := a.Count; a.Dispose(); "
        "  if removed then return n; end; "
        "  return 99; "
        "end Main; end C;"), 1);
}

// §8.3: a class with NO override does NOT match an equal-but-distinct instance.
// Remove returns false (no element removed), Count stays 2 → sentinel path 88.
BEZEL_TEST(remove_equals_8_3, no_override_reference_identity_distinct_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b1 := new Box(1); using var b2 := new Box(2); "
        "  using var query := new Box(2); "  // distinct from b2
        "  var a := new Array<Box>(); a.Add(b1); a.Add(b2); "
        "  var removed := a.Remove(query); "
        "  var n := a.Count; a.Dispose(); "
        "  if removed then return n; end; "
        "  return 88; "  // not removed: count still 2, removed is false
        "end Main; end C;"), 88);
}

// stdlib §4.5: an OwningArray's MATCHED element is DISPOSED exactly once on
// removal; the query argument is a PURE BORROW and is NOT disposed by Remove.
// The destructor counter R._n is returned. Three elements (1,2,3); Remove(query)
// matches the stored R(2) via Equals and disposes it (counter 1). The query is
// disposed by its own `using var` scope-end (counter 2). The remaining two
// elements are disposed by the array's `using var` scope-end (counter 4). Total
// disposals = 4, each exactly once (no double-free of the matched element or the
// query). Encoded: <counter right after Remove>*10 + <final counter>.
// After Remove: the matched element only → 1. Final: 1 matched + 1 query + 2
// remaining = 4. Expect 14.
BEZEL_TEST(remove_equals_4_5, owned_remove_disposes_matched_once_query_borrowed) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is R then var b := other as R; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "  destructor R._n := R._n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterRemove := -1; "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R(1))); "
        "    a.Add(move (new R(2))); "
        "    a.Add(move (new R(3))); "
        "    using var query := new R(2); "
        "    var removed := a.Remove(query); "
        "    afterRemove := R._n; "  // matched element disposed: 1
        "  end; "  // query (+1) and the 2 remaining elements (+2) disposed
        "  return afterRemove * 10 + R._n; "
        "end Main; end C;"), 14);
}

// stdlib §4.4: a BORROWING Array<R>.Remove disposes NOTHING — the matched element
// is a borrow owned elsewhere. Encoded: <counter right after Remove>*10 +
// <counter after the borrowed instances are disposed>. After Remove: 0 (Remove
// disposes nothing). b1, b2 and query are disposed by their own `using var`
// scope-ends → final 3. Expect 03 == 3.
BEZEL_TEST(remove_equals_4_4, borrowing_array_remove_disposes_nothing) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is R then var b := other as R; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "  destructor R._n := R._n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterRemove := -1; "
        "  do "
        "    using var b1 := new R(1); using var b2 := new R(2); "
        "    using var query := new R(2); "
        "    var a := new Array<R>(); a.Add(b1); a.Add(b2); "
        "    var removed := a.Remove(query); "
        "    afterRemove := R._n; "  // borrowing Remove disposes nothing → 0
        "    a.Dispose(); "
        "  end; "  // b1, b2, query disposed by scope-end → +3 = 3
        "  return afterRemove * 10 + R._n; "
        "end Main; end C;"), 3);
}

// stdlib §4.5: a Remove MISS on an OwningArray returns false and disposes NOTHING
// (no element matched). Encoded: <counter after the miss>*10 + <final counter>.
// query R(9) matches no stored element (1,2,3); after Remove the counter is 0;
// the array scope-end disposes all 3 stored elements and the query (+1) → 4.
BEZEL_TEST(remove_equals_4_5, owned_remove_miss_disposes_nothing) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is R then var b := other as R; return b!.X = X; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => X; "
        "  destructor R._n := R._n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterMiss := -1; var removed := true; "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R(1))); "
        "    a.Add(move (new R(2))); "
        "    a.Add(move (new R(3))); "
        "    using var query := new R(9); "
        "    removed := a.Remove(query); "
        "    afterMiss := R._n; "  // miss disposes nothing → 0
        "  end; "  // 3 stored + 1 query disposed at scope-end → 4
        "  if removed then return 77; end; "  // must be false
        "  return afterMiss * 10 + R._n; "
        "end Main; end C;"), 4);
}

// §4.6: a string element matches by CONTENT. Two equal-content but distinct-by-
// construction strings match. Remove returns true; Count drops to 1.
BEZEL_TEST(remove_equals_4_6, string_element_remove_by_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"ab\"); a.Add(\"cd\"); "
        "  var removed := a.Remove(\"a\" + \"b\"); "  // distinct value, same content
        "  if removed then return a.Count; end; "
        "  return 99; "
        "end Main; end C;"), 1);
}

// §8.3: a primitive (value-type) element matches by `==`. Remove(20) removes the
// matching int; Count drops to 2.
BEZEL_TEST(remove_equals_8_3, value_type_element_remove_by_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var removed := a.Remove(20); "
        "  if removed then return a.Count; end; "
        "  return 99; "
        "end Main; end C;"), 2);
}

// §8.3: a positional-record element matches by field-by-field value equality
// (the §8.3 default), NOT reference identity (records are value types). An equal-
// but-distinct Pt(3,4) matches the stored one. Pre-fix this CRASHED the compiler
// (ICmp on a struct); the equality dispatch routes it through structural
// equality. Remove returns true; Count drops to 1.
BEZEL_TEST(remove_equals_8_3, record_element_remove_by_structural_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1, 2)); a.Add(new Pt(3, 4)); "
        "  var removed := a.Remove(new Pt(3, 4)); "
        "  if removed then return a.Count; end; "
        "  return 99; "
        "end Main; end C;"), 1);
}

// §8.3: a record-element Remove MISS returns false, Count unchanged.
BEZEL_TEST(remove_equals_8_3, record_element_remove_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1, 2)); a.Add(new Pt(3, 4)); "
        "  var removed := a.Remove(new Pt(5, 6)); "
        "  if removed then return 77; end; "
        "  return a.Count; "  // unchanged → 2
        "end Main; end C;"), 2);
}
