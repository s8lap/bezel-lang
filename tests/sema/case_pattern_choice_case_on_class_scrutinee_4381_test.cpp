// fix #4381 (spec §19.3, v0.298):
// a case-expression pattern whose head names a choice case (`Fetched(_)`)
// is valid only when the scrutinee is that choice type. When the scrutinee
// is a class, the sema phase must reject the pattern with a §19.3 error,
// rather than letting the invalid code reach codegen and produce malformed
// IR ("Basic Block ... does not have terminator", LLVM IR verifier failure).
//
// Before the fix: `case this of Fetched(_) => true, _ => false` inside a
// class C (where this is a C instance, not an Outcome choice) compiled
// without a sema error but produced IR verification failure at codegen:
//   Basic Block in function 'bz$M.C$IsOk' does not have terminator!
//   label %case.end
//   internal: return-value expression failed to lower to a value
//
// After the fix: the sema phase detects that Fetched is a choice case name
// but this is not a choice scrutinee, and emits a §19.3 error.

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

// True iff an Error-severity diagnostic mentions that the pattern head
// names a choice case but the scrutinee is not a choice.
bool hasChoiceCaseOnNonChoiceError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("choice case") != std::string::npos &&
            d.message.find("scrutinee") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// §19.3 / #4381: case expression with a choice-case pattern (Fetched)
// on a CLASS scrutinee (this) — the sema phase must reject this, not
// let it through to codegen which would produce malformed IR.
BEZEL_TEST(case_pattern_choice_case_on_class_scrutinee_4381,
           case_expr_choice_case_on_class_this_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "choice Outcome Fetched(body: int), NotModified; end Outcome; "
        "class C "
        "  public method IsOk(): bool => "
        "    (case this of Outcome.Fetched(_) => true, _ => false); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasChoiceCaseOnNonChoiceError(r));
}

// §19.3 / #4381: same bug but with short-form pattern name (no choice prefix).
// Both forms must be rejected.
BEZEL_TEST(case_pattern_choice_case_on_class_scrutinee_4381,
           case_expr_choice_case_short_form_on_class_this_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "choice Outcome Fetched(body: int), NotModified; end Outcome; "
        "class C "
        "  public method IsOk(): bool => "
        "    (case this of Fetched(_) => true, _ => false); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasChoiceCaseOnNonChoiceError(r));
}

// §19.3 / #4381: same bug in statement form (CaseStmt, not CaseExpr).
BEZEL_TEST(case_pattern_choice_case_on_class_scrutinee_4381,
           case_stmt_choice_case_on_class_this_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "choice Outcome Fetched(body: int), NotModified; end Outcome; "
        "class C "
        "  public method IsOk(): bool "
        "    case this of "
        "      Outcome.Fetched(_) => return true, "
        "      _ => return false "
        "    end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasChoiceCaseOnNonChoiceError(r));
}

// §19.3 / #4381 (positive control): same pattern on the CORRECT choice
// scrutinee is valid and must compile without error.
BEZEL_TEST(case_pattern_choice_case_on_class_scrutinee_4381,
           case_expr_choice_case_on_choice_scrutinee_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "choice Outcome Fetched(body: int), NotModified; end Outcome; "
        "class Helper "
        "  public static method Check(o: Outcome): bool => "
        "    (case o of Outcome.Fetched(_) => true, _ => false); "
        "end Helper;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
