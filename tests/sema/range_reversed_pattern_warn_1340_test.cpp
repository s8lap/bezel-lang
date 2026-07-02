// fix #1340 (§16.6, v0.162): a stepless range pattern with constant integer
// bounds that are reversed (lo > hi for `lo..hi`; lo >= hi for `lo..<hi`)
// can never match — the compiler emits a Warning at the pattern site.
//
// §16.6: "Because a constant reversed range pattern can never match, the
// compiler emits a warning at the pattern ('this pattern never matches —
// bounds are reversed'), the same constant-fold discipline as §16.4's step
// diagnostics."
//
// The warning fires in ExprTC::validateCaseArms when both bounds fold to
// integer constants and the relationship is reversed.  A non-constant bound,
// a correctly-ordered pattern, an open-ended pattern, or an equal-bound
// inclusive pattern (`5..5`) must NOT draw the warning.

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

// True iff some warning diagnostic carries the "bounds are reversed" message.
bool hasReversedPatternWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("bounds are reversed") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// §16.6 v0.162: a closed reversed range `100..0` in a pattern fires the warning.
// The bounds are constant integers with lo=100 > hi=0, so the pattern can
// never match (equivalent to `50 in 100..0` returning false for any scrutinee).
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           closed_reversed_constant_warns_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 100..0 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReversedPatternWarning(r));
    // It is a WARNING, not an error: the program is still valid.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.6 v0.162: a half-open reversed range `100..<0` fires the warning.
// lo=100 > hi=0 (and >= threshold): never matches.
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           halfopen_reversed_constant_warns_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 100..<0 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReversedPatternWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.6 v0.162: a half-open equal-bound `5..<5` fires the warning.
// lo >= hi for half-open: the interval [5,5) is empty.
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           halfopen_equal_bounds_warns_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 5..<5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReversedPatternWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.6 v0.162: a CORRECT closed pattern `0..100` must NOT warn.
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           correct_closed_pattern_no_warn_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 0..100 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReversedPatternWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.6 v0.162: an equal-bound closed inclusive `5..5` must NOT warn
// (it matches exactly the value 5, so it is not empty).
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           equal_bounds_closed_no_warn_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 5..5 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReversedPatternWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.6 v0.162: a correct half-open `0..<10` must NOT warn.
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           correct_halfopen_no_warn_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 0..<10 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReversedPatternWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.6 v0.162: a negative-bounds reversed pattern `0..-10` fires warning.
// lo=0 > hi=-10 for the default ascending direction.
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           negative_hi_reversed_warns_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 0..-10 => 1, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReversedPatternWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.6 v0.162: a reversed constant pattern fires ONLY a warning — the
// program is well-typed and compiles; the arm just never matches (the
// wildcard `_` arm handles all values instead).
BEZEL_TEST(sema_range_reversed_pattern_warn_1340,
           reversed_pattern_still_compiles_spec_16_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of 50..10 => 99, _ => 0 end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReversedPatternWarning(r));
    // Warning only — no compile error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
