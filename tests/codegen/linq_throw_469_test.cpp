// Codegen tests for #469: LINQ terminal throwing contracts (stdlib §5).
//
// stdlib §5 (line 2515):
//   "First/Last/Single/ElementAt throw InvalidOperationException (or
//    IndexOutOfRangeException for ElementAt) on missing-element; the *OrNull
//    variants return null instead."
//
// stdlib §5 (lines 2528–2529):
//   "Min(): T  — throws if empty"
//   "Max(): T  — throws if empty"
//
// stdlib §5 (lines 2445–2446):
//   "First(): T  — throws if empty"
//   "First(predicate): T"  (throws if no match, per §5 line 2515)
//
// stdlib §5 (line 2451):
//   "Single(): T  — throws if not exactly one"
//
// These tests verify:
//   - empty-source throws are catchable as InvalidOperationException
//   - Single() on a 2-element array throws
//   - First(pred) with no match throws
//   - Min()/Max() on empty throw
//   - ElementAt() out-of-range throws IndexOutOfRangeException (not Invalid)
//   - Positive regressions: non-empty / exactly-one / matching cases still work
//   - *OrNull variants on empty still return null (NOT throw)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── First() on empty → InvalidOperationException ─────────────────────────────

// stdlib §5 line 2445: "First(): T — throws if empty"
// stdlib §5 line 2515: "throw InvalidOperationException … on missing-element"
BEZEL_TEST(linq_throw_469, first_empty_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.First(); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Last() on empty → InvalidOperationException ───────────────────────────────

// stdlib §5 line 2449: "Last(): T"
// stdlib §5 line 2515: "throw InvalidOperationException … on missing-element"
BEZEL_TEST(linq_throw_469, last_empty_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.Last(); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Single() on empty → InvalidOperationException ─────────────────────────────

// stdlib §5 line 2451: "Single(): T — throws if not exactly one"
// stdlib §5 line 2515: "throw InvalidOperationException … on missing-element"
BEZEL_TEST(linq_throw_469, single_empty_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.Single(); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Single() on a 2-element array → InvalidOperationException ─────────────────

// stdlib §5 line 2451: "throws if not exactly one" — ≥2 elements also throws
BEZEL_TEST(linq_throw_469, single_multi_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  try "
        "    var v := a.Single(); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Min() on empty → InvalidOperationException ────────────────────────────────

// stdlib §5.1 line 2528: "Min(): T — throws if empty"
BEZEL_TEST(linq_throw_469, min_empty_throws_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.Min(); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Max() on empty → InvalidOperationException ────────────────────────────────

// stdlib §5.1 line 2529: "Max(): T — throws if empty"
BEZEL_TEST(linq_throw_469, max_empty_throws_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.Max(); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Min(selector) on empty → InvalidOperationException ───────────────────────

// stdlib §5 line 2463: "Min<U>(selector: (T)->U): U where U: IComparable<U>"
// Also covered by §5 line 2515 general principle: throws on missing-element.
BEZEL_TEST(linq_throw_469, min_selector_empty_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.Min((x: int) => x * 2); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Max(selector) on empty → InvalidOperationException ───────────────────────

// stdlib §5 line 2464: "Max<U>(selector: (T)->U): U where U: IComparable<U>"
BEZEL_TEST(linq_throw_469, max_selector_empty_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.Max((x: int) => x * 2); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── First(predicate) with no match → InvalidOperationException ────────────────

// stdlib §5 line 2446: "First(predicate: (T)->bool): T"
// stdlib §5 line 2515: "throw InvalidOperationException … on missing-element"
BEZEL_TEST(linq_throw_469, first_pred_no_match_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(3); a.Add(5); "
        "  try "
        "    var v := a.First((x: int) => x mod 2 = 0); return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── ElementAt(i) out of range → IndexOutOfRangeException ─────────────────────

// stdlib §5 line 2453: "ElementAt(index: int): T"
// stdlib §5 line 2515: "IndexOutOfRangeException for ElementAt"
BEZEL_TEST(linq_throw_469, element_at_oor_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); "
        "  try "
        "    var v := a.ElementAt(5); return v; "
        "  catch IndexOutOfRangeException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── ElementAt(i) negative index → IndexOutOfRangeException ────────────────────

BEZEL_TEST(linq_throw_469, element_at_neg_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); "
        "  try "
        "    var v := a.ElementAt(-1); return v; "
        "  catch IndexOutOfRangeException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// ── Positive regression: First() / Last() / Single() on valid arrays ──────────

// stdlib §5 line 2445: First() returns first element when non-empty.
BEZEL_TEST(linq_throw_469, first_nonempty_still_works_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(42); a.Add(7); "
        "  return a.First(); "
        "end Main; end C;"), 42);
}

// stdlib §5 line 2449: Last() returns last element when non-empty.
BEZEL_TEST(linq_throw_469, last_nonempty_still_works_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(7); "
        "  return a.Last(); "
        "end Main; end C;"), 7);
}

// stdlib §5 line 2451: Single() returns the element when there is exactly one.
BEZEL_TEST(linq_throw_469, single_exactly_one_still_works_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(55); "
        "  return a.Single(); "
        "end Main; end C;"), 55);
}

// stdlib §5.1: Min()/Max() still return the correct result when non-empty.
BEZEL_TEST(linq_throw_469, min_max_nonempty_still_works_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(4); a.Add(1); a.Add(5); "
        "  return int(a.Min() = 1 and a.Max() = 5); "
        "end Main; end C;"), 1);
}

// stdlib §5 line 2446: First(pred) still returns match when one exists.
BEZEL_TEST(linq_throw_469, first_pred_match_still_works_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(4); a.Add(5); "
        "  return a.First((x: int) => x mod 2 = 0); "
        "end Main; end C;"), 4);
}

// stdlib §5 line 2453: ElementAt(i) in range still returns the element.
BEZEL_TEST(linq_throw_469, element_at_inrange_still_works_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  return a.ElementAt(1); "
        "end Main; end C;"), 20);
}

// ── *OrNull variants on empty still return null, NOT throw ───────────────────

// stdlib §5 line 2447: "FirstOrNull(): T? — null if empty"
// Must NOT throw even though the source is empty.
BEZEL_TEST(linq_throw_469, first_or_null_empty_returns_null_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var v: int? := a.FirstOrNull(); "
        "  return int(v = null); "
        "end Main; end C;"), 1);
}

// stdlib §5 line 2450: "LastOrNull(): T? — null if empty"
BEZEL_TEST(linq_throw_469, last_or_null_empty_returns_null_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var v: int? := a.LastOrNull(); "
        "  return int(v = null); "
        "end Main; end C;"), 1);
}

// stdlib §5 line 2452: "SingleOrNull(): T? — null if not exactly one"
BEZEL_TEST(linq_throw_469, single_or_null_empty_returns_null_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var v: int? := a.SingleOrNull(); "
        "  return int(v = null); "
        "end Main; end C;"), 1);
}

// SingleOrNull with 2 elements should also return null (not throw).
BEZEL_TEST(linq_throw_469, single_or_null_multi_returns_null_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  var v: int? := a.SingleOrNull(); "
        "  return int(v = null); "
        "end Main; end C;"), 1);
}

// ── Exception is catchable by declared type ───────────────────────────────────

// Confirm the thrown exception is visible to a `catch var e` binding —
// proving the exception OBJECT is properly typed, not just a bare branch.
BEZEL_TEST(linq_throw_469, first_empty_exception_is_typed_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  try "
        "    var v := a.First(); return v; "
        "  catch var e: InvalidOperationException => return 77; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 77);
}

// ── First() throw on lazy pipeline (Where → First with no match) ──────────────

// stdlib §5 line 2446 + 2515: First(pred) also throws on lazy pipelines.
BEZEL_TEST(linq_throw_469, lazy_first_pred_no_match_throws_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(3); a.Add(5); "
        "  try "
        "    var v := a.Where((x: int) => x > 0).First((x: int) => x mod 2 = 0); "
        "    return v; "
        "  catch InvalidOperationException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}
