// fix #4498 (§17.7, v0.306): a `var f: () -> Widget := lambda() => new();`
// lambda has NO ascription of its own — its return type arrives entirely from
// the outer target type. §17.7 places this shape OUTSIDE the written-declared-
// type family; the governing clause is the section's closing catch-all,
// "nor in general expression contexts where the type would have to be inferred
// from broader context", NOT the ascribed-return carve-out.
//
// Before this fix the diagnostic cited the ascribed-return exclusion:
//   "and excludes this one: \"It is not permitted at ... a lambda's ascribed
//   return\""
// which sends the reader to the wrong §17.7 sentence. The rejection is correct
// (the same program SIGSEGV'd on the pre-fix binary, per the issue); only the
// cited clause was wrong.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// The §17.7 diagnostic for the new() shorthand at a lambda return.
// The diagnostic message is: "the `new()` shorthand is not permitted..."
bool hasNewShorthandLambdaReturnError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not permitted at a lambda's return") != std::string::npos)
            return true;
    return false;
}

// Post-fix: the diagnostic must NOT cite the ascribed-return clause for a
// target-inference-only lambda.
bool citesAscribedReturnClause(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("ascribed return") != std::string::npos)
            return true;
    return false;
}

// Post-fix: the diagnostic must cite the general-expression-context clause
// or equivalent target-inference wording.
bool citesGeneralExpressionContextClause(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not permitted at a lambda's return") != std::string::npos &&
            (d.message.find("general expression") != std::string::npos ||
             d.message.find("target inference") != std::string::npos ||
             d.message.find("inferred from broader context") != std::string::npos ||
             d.message.find("outside the written-declared-type family") !=
                 std::string::npos))
            return true;
    return false;
}

bezel::test::OwnershipResult check(const std::string& src) {
    return parseAndCheckOwnership(src);
}

}  // namespace

// §17.7 / #4498: expression-bodied lambda with target-inference-only return
// type. The lambda `lambda() => new()` has no `: T` ascription; its return
// type flows in from the outer `() -> Widget` target type. The diagnostic
// must cite the general-expression-context clause, not the ascribed-return
// carve-out.
BEZEL_TEST(lambda_target_inference_only_citation_4498,
           expr_body_target_inference_rejects_correct_clause) {
    auto r = check(
        "module M; "
        "class WidgetLambdaExpr4498 "
        "  public property Tag: int := 77; "
        "end WidgetLambdaExpr4498; "
        "class ProgramLambdaExpr4498 "
        "  public method Main() "
        "    var f: () -> WidgetLambdaExpr4498 := lambda() => new(); "
        "  end Main "
        "end ProgramLambdaExpr4498;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNewShorthandLambdaReturnError(r));
    // Pre-fix: the diagnostic cited the ascribed-return clause, which is
    // incorrect for a target-inference-only lambda. Post-fix: it cites the
    // general-expression-context clause or equivalent wording.
    BEZEL_EXPECT_TRUE(citesGeneralExpressionContextClause(r));
    BEZEL_EXPECT_FALSE(citesAscribedReturnClause(r));
}

// §17.7 / #4498: block-bodied lambda with target-inference-only return type.
// The same principle applies: when the lambda has no `: T` ascription, the
// rejection must not cite the ascribed-return clause.
BEZEL_TEST(lambda_target_inference_only_citation_4498,
           block_body_target_inference_rejects_correct_clause) {
    auto r = check(
        "module M; "
        "class WidgetLambdaExpr4498 "
        "  public property Tag: int := 77; "
        "end WidgetLambdaExpr4498; "
        "class ProgramLambdaExpr4498 "
        "  public method Main() "
        "    var f: () -> WidgetLambdaExpr4498 := lambda() do return new(); end; "
        "  end Main "
        "end ProgramLambdaExpr4498;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNewShorthandLambdaReturnError(r));
    BEZEL_EXPECT_TRUE(citesGeneralExpressionContextClause(r));
    BEZEL_EXPECT_FALSE(citesAscribedReturnClause(r));
}

// §17.7 / #4498: a lambda WITH an explicit `: T` ascription should still
// cite the ascribed-return clause (the fix is scoped to target-inference-only).
BEZEL_TEST(lambda_target_inference_only_citation_4498,
           ascribed_return_still_cites_ascribed_clause) {
    auto r = check(
        "module M; "
        "class WidgetLambdaExpr4498 "
        "  public property Tag: int := 77; "
        "end WidgetLambdaExpr4498; "
        "class ProgramLambdaExpr4498 "
        "  public method Main() "
        "    var f := lambda(): WidgetLambdaExpr4498 => new(); "
        "  end Main "
        "end ProgramLambdaExpr4498;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNewShorthandLambdaReturnError(r));
    // A lambda WITH a `: Widget` ascription still cites the ascribed-return
    // clause correctly.
    BEZEL_EXPECT_TRUE(citesAscribedReturnClause(r));
}
