// fix #1496 (§16.3 v0.162 / §17.5): open-ended Range<T> membership.
//
// §16.3: "`Contains(v)` is `true` iff the range is nonempty under iteration
// ... and `min(Start, End) <= v <= max(Start, End)` ... a `null` endpoint
// (open-ended form) imposes no bound on its side and never makes the range
// empty." §16.3 also states `IsEmpty` is `false` whenever `Start` or `End`
// is `null`. §17.5: "`x in collection` evaluates to `true` if
// `collection.Contains(x)` is true" — `in` and `.Contains` must never
// disagree on the same range.
//
// Bug: the Range struct stores a fake 0 sentinel in the `start`/`end` i64
// fields on an open side (there is nothing else to store for a missing
// endpoint). `Contains`, the `in` operator, and the `IsEmpty` property all
// compared that sentinel as if it were a real bound, corrupting the
// direction/extent computation for every open-ended range:
//   (10..).IsEmpty        -> was True,  must be False
//   50 in (10..)          -> was False, must be True
//   (10..).Contains(5)    -> was True,  must be False (5 < lower bound 10)
//   (..<0).Contains(-3)   -> was False, must be True
// `IsEmpty` additionally had no open-ended short-circuit at all.

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

// ── Variable-bound open-ended ranges (the #1496 probe shape) ────────────────

// §16.3 v0.182: an open-ended range is never empty. `(10..).IsEmpty` -> false.
BEZEL_TEST(range_openended_membership_1496, isempty_open_upper_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 10..; "
        "  return (0 if r.IsEmpty else 1); "
        "end Main; end C;"), 1);
}

// §16.3 v0.182: open-ended lower side is also never empty. `(..<0).IsEmpty`.
BEZEL_TEST(range_openended_membership_1496, isempty_open_lower_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var h := ..<0; "
        "  return (0 if h.IsEmpty else 1); "
        "end Main; end C;"), 1);
}

// §16.3/§17.5: `50 in (10..)` -> true (no upper bound to violate).
BEZEL_TEST(range_openended_membership_1496, in_open_upper_unbounded_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 10..; "
        "  return (1 if 50 in r else 0); "
        "end Main; end C;"), 1);
}

// §16.3: `(10..).Contains(5)` -> false (5 is below the lower bound 10; the
// open UPPER side must not be misread as leaving no bound at all).
BEZEL_TEST(range_openended_membership_1496, contains_open_upper_below_lower_bound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 10..; "
        "  return (1 if r.Contains(5) else 0); "
        "end Main; end C;"), 0);
}

// §16.3: regression guard for the sentinel value itself — the fake 0 used
// internally for a missing endpoint must not leak into comparisons. 0 is
// still below the lower bound 10.
BEZEL_TEST(range_openended_membership_1496, contains_open_upper_sentinel_value_excluded) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 10..; "
        "  return (1 if r.Contains(0) else 0); "
        "end Main; end C;"), 0);
}

// §16.3: `(..<0).Contains(-3)` -> true (no lower bound; -3 < 0 half-open end).
BEZEL_TEST(range_openended_membership_1496, contains_open_lower_within_upper_bound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var h := ..<0; "
        "  return (1 if h.Contains(-3) else 0); "
        "end Main; end C;"), 1);
}

// §16.3: an open lower bound truly has no floor — a very negative value
// must still be contained.
BEZEL_TEST(range_openended_membership_1496, contains_open_lower_unbounded_far_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var h := ..<0; "
        "  return (1 if h.Contains(-1000000) else 0); "
        "end Main; end C;"), 1);
}

// §16.3: half-open upper bound is still exclusive even when the lower side
// is open — `(..<0).Contains(0)` -> false.
BEZEL_TEST(range_openended_membership_1496, contains_open_lower_excludes_halfopen_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var h := ..<0; "
        "  return (1 if h.Contains(0) else 0); "
        "end Main; end C;"), 0);
}

// §16.3: the inclusive form of the same upper bound DOES include it —
// `(..0).Contains(0)` -> true.
BEZEL_TEST(range_openended_membership_1496, contains_open_lower_includes_inclusive_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var h := ..0; "
        "  return (1 if h.Contains(0) else 0); "
        "end Main; end C;"), 1);
}

// §17.5: `x in r` and `r.Contains(x)` must never disagree, for a value well
// within the open-ended extent.
BEZEL_TEST(range_openended_membership_1496, in_and_contains_agree_open_upper) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 10..; "
        "  if (50 in r) = r.Contains(50) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5: agreement also holds for a value OUTSIDE the extent.
BEZEL_TEST(range_openended_membership_1496, in_and_contains_agree_open_upper_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 10..; "
        "  if (5 in r) = r.Contains(5) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Inline range-literal forms (fix #1496 also taught `in` to accept an
//    inline open-ended RangeExpr literal on the right; previously this
//    combination hit "not yet implemented: codegen for this `in` expression
//    form" because neither of the two existing `in` lowering paths matched
//    a literal with an absent endpoint) ─────────────────────────────────────

BEZEL_TEST(range_openended_membership_1496, inline_literal_isempty_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (0 if (10..).IsEmpty else 1); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(range_openended_membership_1496, inline_literal_in_open_upper_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if 50 in (10..) else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(range_openended_membership_1496, inline_literal_in_open_upper_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if 5 in (10..) else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(range_openended_membership_1496, inline_literal_contains_open_upper_below_lower) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if (10..).Contains(5) else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(range_openended_membership_1496, inline_literal_contains_open_lower_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if (..<0).Contains(-3) else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(range_openended_membership_1496, inline_literal_in_and_contains_agree) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if (50 in (10..)) = (10..).Contains(50) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Closed-range regression guard — the open-side rewrite must not disturb
//    the already-correct closed-range direction/extent logic (§16.3 v0.162,
//    fix #1340). ────────────────────────────────────────────────────────────

BEZEL_TEST(range_openended_membership_1496, closed_range_descending_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 10..0 step -1; "
        "  var a := 1 if 5 in r else 0; "
        "  var b := 1 if r.Contains(11) else 0; "
        "  return a * 2 + b; "
        "end Main; end C;"), 2);
}
