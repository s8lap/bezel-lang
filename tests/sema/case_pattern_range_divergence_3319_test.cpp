// Spec-derived regression tests for #3319. Two independent §19.6 / §16.3
// divergences the spec catches but the compiler did not:
//
//   (Part 1) A bare `..` range-pattern arm proved exhaustiveness via
//   `ExprTypecheck.cpp:caseArmsExhaustive`. §19.6's list of what establishes
//   exhaustiveness is closed — "irrefutable arm: some arm is irrefutable — an
//   unguarded `_`, or an unguarded untyped `var x` pattern" — and §19.3
//   (v0.273) makes the exclusion explicit: "`_` is the idiomatic spelling of
//   an arm that matches everything (§19.6 counts `_` and an untyped `var x`,
//   not `..`, toward a static exhaustiveness proof)." Without the fix a
//   valid `case x: int of .. => 7` (expression position, no `_`) compiled
//   without the §19.6 warning that should fire.
//
//   (Part 2) Range patterns are implemented over `double`/`float`/`decimal`
//   scrutinees. §16.3 restricts `Range<T>`'s T to the integer types
//   (`byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`) and
//   `char`: "Other substitutions are rejected. This restriction is enforced as
//   a compiler-intrinsic rule." A range pattern `lo..hi` / `lo..<hi` / `..`
//   is a RangePattern whose element type is the scrutinee's, so a range
//   pattern over `double`/`float`/`decimal` is `Range<double>` etc. — which
//   §16.3 forbids. Without the fix the compiler silently routed these through
//   codegen (FCmp / bz_decimal_cmp) and emitted code for a non-existent
//   `Range<T>`. The fix rejects them at sema with a §16.3 diagnostic.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some diagnostic at Warning severity mentions non-exhaustiveness.
bool hasNonExhaustiveWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos)
            return true;
    }
    return false;
}

// True iff some Error-severity diagnostic carries the §16.3 rejection text
// the fix installed. Two needles: the lead phrase and the §16.3 citation.
bool hasRangeOverFloatRejected(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("range pattern is not admitted") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""16.3") != std::string::npos)
            return true;
    }
    return false;
}

// True iff a §16.3 rejection diagnostic names the given scrutinee type
// (`float` / `double` / `decimal`).
bool hasRangeOverFloatRejectedScrutinee(const bezel::test::SemaResult& r,
                                        std::string_view scrutName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("range pattern is not admitted") !=
                std::string::npos &&
            d.message.find(scrutName) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ============================================================================
// Part 1 — a bare `..` arm does NOT prove exhaustiveness (§19.6 closed list,
// §19.3 v0.273 explicit "not `..`"). Expression-position cases therefore
// draw the §19.6 non-exhaustiveness warning without a `_` / untyped `var x`
// arm.
// ============================================================================

// §19.6 / §19.3 / #3319: an int scrutinee with a sole bare-`..` arm draws
// the §19.6 non-exhaustiveness warning. Before the fix, this case was
// treated as exhaustive (no warning) — contradicting §19.6's closed list.
BEZEL_TEST(case_pattern_range_divergence_3319,
           bare_dotdot_alone_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: int): int "
        "  return (case x of .. => 7); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3 / §16.6 (v0.286, #3298) — SUPERSEDED VERDICT. This row used to assert
// that a bare `..` over an `int?` warns (§19.6) and produces NO error. The
// §19.6 half is unchanged and still asserted below; the "no error" half is
// now false: §19.3 rejects a range pattern over a nullable scrutinee, "keyed
// on the KIND of pattern, not on which bounds are written, so it reaches the
// open-ended forms and the fully open `..` with the closed ones". The
// exhaustiveness warning and the §19.3 error are two independent verdicts on
// the same program and BOTH are asserted here, so a regression that silences
// either one fails.
BEZEL_TEST(case_pattern_range_divergence_3319,
           bare_dotdot_over_nullable_warns_and_is_rejected_19_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: int?): int "
        "  return (case x of .. => 7); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    bool sawRangeOverNullable = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("range pattern requires a non-nullable scrutinee") !=
                std::string::npos &&
            d.message.find("19.3") != std::string::npos)
            sawRangeOverNullable = true;
    BEZEL_EXPECT_TRUE(sawRangeOverNullable);
}

// §19.6 / #3319: a bare `..` followed by an unguarded `_` is exhaustive
// because the `_` arm is the irrefutable catch-all the spec lists — the
// bare `..` itself adds nothing. Before the fix this case had two
// independently-exhaustive arms; after the fix the `_` carries it. Same
// verdict either way, but this guards against any regression that would
// make `..` and `_` mutually exclusive (the existing 1472 test covers the
// run-time match; this one covers the static verdict).
BEZEL_TEST(case_pattern_range_divergence_3319,
           bare_dotdot_then_discard_still_exhaustive_spec_19_6) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: int): int "
        "  return (case x of .. => 3, _ => 4); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.6 / #3319 (negative control for the nullable case): a bare `..` and
// a `null` arm over a nullable int scrutinee does NOT make the case
// exhaustive — §19.6's closed list does not let `..` discharge the
// non-null coverage, and `int?` is not in the enum/bool/choice/closed/
// hierarchy set §19.6 statically proves. The runtime behavior IS total
// (`..` matches every present value, `null` matches the absent slot), but
// the compiler cannot statically prove it without an irrefutable arm
// (`_` / untyped `var x`) — so the §19.6 warning fires. This is the
// spec-aligned outcome of the fix; a regression that restored the
// implementation's old "bare `..` proves exhaustiveness" reading would
// stop the warning.
BEZEL_TEST(case_pattern_range_divergence_3319,
           bare_dotdot_over_nullable_with_null_still_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: int?): int "
        "  return (case x of .. => 7, null => 9); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    // §19.3 (v0.286, #3298): the `..` arm is ALSO a compile error here — the
    // §19.6 verdict this row exists for is unaffected by it, but asserting
    // "no errors" would now be false. Pin the coexistence instead.
    bool sawRangeOverNullable = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("range pattern requires a non-nullable scrutinee") !=
                std::string::npos)
            sawRangeOverNullable = true;
    BEZEL_EXPECT_TRUE(sawRangeOverNullable);
}

// §19.6 / #3319: a bare `..` followed by an unguarded `_` is exhaustive
// because the `_` arm is the irrefutable catch-all the spec lists — the
// bare `..` itself adds nothing. Before the fix this case had two
// independently-exhaustive arms; after the fix the `_` carries it. Same
// verdict either way, but this guards against any regression that would
// make `..` and `_` mutually exclusive (the existing 1472 test covers the
// run-time match; this one covers the static verdict).
// (Originally also tested the nullable `.., null` shape — removed because
// per §19.6's closed list a bare `..` does NOT discharge exhaustiveness,
// so even with an explicit `null` arm this case draws the §19.6 warning
// for a non-enum/bool/choice/closed/hierarchy scrutinee. The right shape
// for an exhaustive nullable case is `_ => 7, null => 9` (or an untyped
// `var x` binding).)
BEZEL_TEST(case_pattern_range_divergence_3319,
           discard_over_nullable_with_null_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: int?): int "
        "  return (case x of null => 9, _ => 7); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// Part 2 — a range pattern over a `double` / `float` / `decimal` scrutinee
// is rejected at sema with a §16.3 diagnostic. `Range<T>`'s T must be an
// integer type or `char`; the patterns the compiler previously admitted
// silently admitted `Range<double>` / `Range<float>` / `Range<decimal>`,
// none of which exist.
// ============================================================================

// §16.3 / #3319: a closed range pattern over a `double` scrutinee is
// rejected (range pattern is not admitted over a `double` scrutinee).
BEZEL_TEST(case_pattern_range_divergence_3319,
           closed_range_over_double_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: double): int "
        "  return (case x of 1.0..5.0 => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejectedScrutinee(r, "double"));
}

// §16.3 / #3319: a half-open range pattern over a `double` scrutinee is
// rejected (same §16.3 reasoning; the half-open form is not separately
// permitted by §16.3).
BEZEL_TEST(case_pattern_range_divergence_3319,
           half_open_range_over_double_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: double): int "
        "  return (case x of 1.0..<5.0 => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejected(r));
}

// §16.3 / #3319: a range pattern over a `float` scrutinee is rejected.
BEZEL_TEST(case_pattern_range_divergence_3319,
           closed_range_over_float_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: float): int "
        "  return (case x of 1.0..5.0 => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejectedScrutinee(r, "float"));
}

// §16.3 / #3319: a range pattern over a `decimal` scrutinee is rejected.
// (The #1152 fix routed decimal range patterns through bz_decimal_cmp; the
// §16.3 gate is upstream of that path and supersedes it — Range<decimal>
// is not a type the compiler admits.)
BEZEL_TEST(case_pattern_range_divergence_3319,
           closed_range_over_decimal_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: decimal): int "
        "  return (case x of 1m..5m => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejected(r));
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejectedScrutinee(r, "decimal"));
}

// §16.3 / #3319: a BARE `..` (the unbounded RangePattern, grammar A.7) is
// also a `Range<T>` whose T is the scrutinee type — so a bare `..` over a
// `double` scrutinee is `Range<double>` and §16.3 rejects it.
BEZEL_TEST(case_pattern_range_divergence_3319,
           bare_dotdot_over_double_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: double): int "
        "  return (case x of .. => 1); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejected(r));
}

// §16.3 / #3319: a range-pattern member of a `|` alternative list over a
// `decimal` scrutinee is rejected. §19.3 v0.183 / #1584 governs alt-list
// member types and inherits the same §16.3 restriction on T.
BEZEL_TEST(case_pattern_range_divergence_3319,
           alt_list_with_range_over_decimal_rejected_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: decimal): int "
        "  return (case x of 1m..5m | 10m => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRangeOverFloatRejected(r));
}

// §16.3 / #3319 (positive control): a range pattern over an `int`
// scrutinee is admitted — `Range<int>` is what §16.3 specifically allows.
// (Guards against an over-broad reject that would also break the integer
// scrutinee case the existing range-pattern tests rely on.)
BEZEL_TEST(case_pattern_range_divergence_3319,
           closed_range_over_int_ok_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: int): int "
        "  return (case x of 1..5 => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 / #3319 (positive control): a range pattern over a `char`
// scrutinee is admitted — `Range<char>` is the §16.3-allowed char
// substitution.
BEZEL_TEST(case_pattern_range_divergence_3319,
           closed_range_over_char_ok_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: char): int "
        "  return (case x of 'a'..'z' => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.3 / #3319 (positive control): a range pattern over a `long`
// scrutinee is admitted — `Range<long>` is one of the §16.3 integer
// substitutions. (The integer scrutinee range check at caseIntScrutineeRange
// is no-op for Long/ULong; the §16.3 gate is independent of that.)
BEZEL_TEST(case_pattern_range_divergence_3319,
           closed_range_over_long_ok_spec_16_3) {
    auto r = parseSemaAll(
        "module M; class C public static method M(x: long): int "
        "  return (case x of 1L..5L => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}