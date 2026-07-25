// Spec-derived sema regression tests for #2210.
// Per §9.7 and §35.4.1 (v0.226), a record cannot contain an owning choice
// because records do not provide owning-field disposal. Value-like, borrow-
// bearing, and generic non-owning choices remain valid record fields.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {
bezel::test::SemaResult check(std::string_view decls) {
    auto r = bezel::test::parseAndCollect(std::string("module M; ") +
                                          std::string(decls) + " end M;");
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}
}  // namespace

BEZEL_TEST(sema_record_choice_field_2210,
           owning_choice_field_rejected_at_record_field) {
    auto r = check(
        "class Res public V: int := 0; destructor end destructor; end Res; "
        "choice Ch Wrap(owned r: Res), None; end Ch; "
        "record Rec(K: Ch); ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record_choice_field_2210, value_like_choice_field_accepted) {
    auto r = check("choice Ch Wrap(n: int), None; end Ch; record Rec(K: Ch); ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record_choice_field_2210, borrow_bearing_choice_field_accepted) {
    auto r = check(
        "choice Ch Wrap(s: ArraySpan<int>), None; end Ch; "
        "record Rec(K: Ch); ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record_choice_field_2210,
           generic_choice_with_non_owning_payload_accepted) {
    auto r = check(
        "choice Ch<T> Wrap(value: T), None; end Ch; "
        "record Rec<T>(K: Ch<T>); ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
