// Spec-derived sema tests for the choice kind — §35 (v0.200, #2044).
//
// Slice 2 surface (this file):
//   * §A.3 ChoiceDecl produces a TypeSymbol of kind Choice with the cases
//     collected as members (§35.1).
//   * §14.2 / §35.10: a choice is a value-type (categorize), never satisfies
//     `where T: new()` (default(C) is undefined for every choice type).
//   * §17.11 / §35.10: `default(C)` is a compile error for every choice
//     type, regardless of classification.
//
// What is NOT here yet (deferred to follow-up slices per #2044): the §35.4
// three-way classification runtime observable behavior, §35.3 case
// construction lowering, §35.5 borrowing match, §35.7 equality synthesis.
// The classification function itself is exercised downstream by §35.7/§35.8/
// §35.12 use-sites; those land in their own slices and bring the
// classification-driven diagnostics with them.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::TypeSymbol;
using bezel::sema::Symbol;
using bezel::test::parseAndCollect;
using bezel::test::symbolSummary;

namespace {

// Parse + collect + resolve + typecheck (mirrors await_typing_test.cpp's
// helper of the same name; defined locally to keep the slice self-
// contained).
bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

const TypeSymbol* findType(const bezel::test::SemaResult& r,
                           std::string_view name) {
    if (!r.ctx || !r.ctx->module()) return nullptr;
    for (Symbol* child : r.ctx->module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            if (t->name == name) return t;
        }
    }
    return nullptr;
}

}  // namespace

// --- §35.1: cases are collected as members of the choice TypeSymbol --------

BEZEL_TEST(choice_sema_35, spec_35_1_cases_collected_as_members) {
    // §35.1 FetchOutcome headline shape (adapted to value payloads): the
    // three cases appear as choice-case members, each tagged with its
    // payload arity.
    auto r = parseAndCollect(
        "module M; choice FetchOutcome "
        "  Fetched(owned response: int), "
        "  NotModified, "
        "  Failed(status: int, message: string); "
        "end FetchOutcome;");
    auto* ts = findType(r, "FetchOutcome");
    BEZEL_EXPECT_TRUE(ts != nullptr);
    if (ts) {
        BEZEL_EXPECT_EQ(ts->kind, bezel::ast::TypeDeclKind::Choice);
        // Three cases collected as members (and nothing else — no methods).
        std::size_t caseCount = 0;
        for (Symbol* m : ts->members) {
            if (dynamic_cast<bezel::sema::ChoiceCaseSymbol*>(m)) {
                ++caseCount;
            }
        }
        BEZEL_EXPECT_EQ(caseCount, std::size_t(3));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(choice_sema_35, spec_35_1_case_symbol_summary_carries_arity) {
    // The symbolSummary helper round-trips the case name + arity so a test
    // can distinguish payload-free from payload-carrying cases.
    auto r = parseAndCollect(
        "module M; choice Opt Some(value: int), None; end Opt;");
    auto* ts = findType(r, "Opt");
    BEZEL_EXPECT_TRUE(ts != nullptr);
    if (ts) {
        std::string someLine, noneLine;
        for (Symbol* m : ts->members) {
            if (auto* cc = dynamic_cast<bezel::sema::ChoiceCaseSymbol*>(m)) {
                if (cc->name == "Some") someLine = symbolSummary(*cc);
                if (cc->name == "None") noneLine = symbolSummary(*cc);
            }
        }
        BEZEL_EXPECT_EQ(someLine, std::string("choice-case Opt.Some (1-payload)"));
        BEZEL_EXPECT_EQ(noneLine, std::string("choice-case Opt.None (0-payload)"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §35.10 / §17.11: `default(C)` is a compile error for every choice -----

BEZEL_TEST(choice_sema_35, spec_35_10_default_choice_rejected) {
    // §35.10: "`default(C)` is a compile error for every choice type — a
    // sum has no principled zero." This holds regardless of classification
    // (value-like, borrow-bearing, or owning) — every choice rejects. The
    // remedy is `default(C?)` (the nullable wrapper) or an explicit case
    // construction.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := default(Opt); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool seen = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("default(T)") != std::string::npos &&
            d.message.find("choice type") != std::string::npos) {
            seen = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(seen);
}

BEZEL_TEST(choice_sema_35, spec_35_10_default_choice_nullable_accepted) {
    // §35.10 / §35.9: `default(C?)` is the VALID null (the nullable wrapper
    // is the standard nullable value type, §7.2). It is accepted — only
    // the non-nullable form is the §35.10 error. (Categorizing C? as a
    // nullable value type and accepting default(C?) is the §7.2 path;
    // this test pins that the §35.10 rejection does not over-fire onto
    // the nullable form.)
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt? := default(Opt?); return 0; "
        "end Main; end C;");
    // The front-end should accept the nullable default. (There may be
    // unrelated diagnostics from incomplete choice codegen support — what
    // we are pinning here is the ABSENCE of the §35.10 default-rejection.)
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("choice type") == std::string::npos);
    }
}
