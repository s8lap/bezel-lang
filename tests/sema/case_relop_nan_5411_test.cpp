// Fix verification tests for bug #5411 (§19.3/§19.6): a NaN scrutinee must not
// match a relational pattern, and no relational arm counts toward exhaustiveness.
//
// §19.6: over float/double, no relational arm counts toward the exhaustiveness
// proof. NaN is unmatched by any relational comparison (§4.3.1: "NaN is unordered
// — every comparison is false"), and no finite set of relational arms covers the
// full value space. A `case` over float/double with only relational arms is
// NON-EXHAUSTIVE; the compiler must warn for expression-position case.
//
// The matching half (§4.3.1): the codegen lowering uses ordered floating-point
// comparisons (CreateFCmpOLT/OGT/OLE/OGE), which correctly return false when
// either operand is NaN. These codegen tests verify the runtime behavior.
//
// The exhaustiveness half (§19.6): verified here in the sema suite using the
// SemaResult harness, which preserves all diagnostics including warnings on
// successful compiles (unlike runAot which discards stderr on CompileOutcome::Ok).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// True iff some diagnostic at Warning severity mentions the float/double
// relational-pattern non-exhaustiveness rule.
bool hasFloatDoubleRelationalNonExhaustiveWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find("float") != std::string::npos &&
            d.message.find("double") != std::string::npos)
            return true;
    }
    return false;
}

bezel::test::SemaResult semaAll(const std::string& src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

}  // namespace

// ── §19.6: double with only relational arm — expression-position warns ───────────
//
// BEFORE FIX: caseArmsExhaustive had no float/double handling, so a double with
// only relational arms fell through to the generic "cannot prove exhaustive"
// path — but the comment at that path said "scrutinee type cannot be proven
// exhaustive" which is a DIFFERENT message from the float/double-specific one.
// After the fix: the specific diagnostic "relational pattern over float/double
// cannot cover NaN" is emitted.

BEZEL_TEST(case_nan_5411, double_only_relational_arm_non_exhaustive_lt) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: double := double.NaN; "
        "  var r := (case v of < 1.0 => 1); "
        "  return r; "
        "end Main; end C;");
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find("relational pattern") != std::string::npos &&
            d.message.find("float/double") != std::string::npos &&
            d.message.find("NaN") != std::string::npos)
            found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}

BEZEL_TEST(case_nan_5411, double_only_relational_arm_non_exhaustive_ge) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: double := double.NaN; "
        "  var r := (case v of >= 1.0 => 1); "
        "  return r; "
        "end Main; end C;");
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find("relational pattern") != std::string::npos &&
            d.message.find("float/double") != std::string::npos)
            found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}

BEZEL_TEST(case_nan_5411, double_two_relational_arms_still_non_exhaustive) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: double := double.NaN; "
        "  var r := (case v of < 0 => 1, >= 10 => 2); "
        "  return r; "
        "end Main; end C;");
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find("relational pattern") != std::string::npos)
            found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}

BEZEL_TEST(case_nan_5411, float_only_relational_arm_non_exhaustive) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: float := float.NaN; "
        "  var r := (case v of < 1.0f => 1); "
        "  return r; "
        "end Main; end C;");
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find("relational pattern") != std::string::npos &&
            d.message.find("float") != std::string::npos)
            found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}

// ── §19.6: double with relational arm + `_` catchall — exhaustive ───────────────
//
// AFTER FIX: a `_` catchall makes the case exhaustive regardless of what other
// arms are present. No warning should be emitted.

BEZEL_TEST(case_nan_5411, double_relational_arm_with_catchall_exhaustive) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var v: double := double.NaN; "
        "  var r := (case v of < 1.0 => 1, _ => 2); "
        "  return r; "
        "end Main; end C;");
    bool hasNonExhaustive = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos)
            hasNonExhaustive = true;
    }
    BEZEL_EXPECT_FALSE(hasNonExhaustive);
}
