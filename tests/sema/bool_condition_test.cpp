// Spec-derived tests for Sema slice S18 — control-flow conditions must
// be `bool` (§21.2 if, §21.3 while, §20 if-expression).
//
// The check only fires when the condition has a resolved primitive type
// that is not bool; un-typed conditions are not flagged (no false
// positives while expression typing is still incomplete).

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

std::string wrap(const std::string& body) {
    return std::string("module M; class C public method M(flag: bool, n: int) ")
        + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_cond, if_bool_param_ok) {
    auto r = parseSemaAll(wrap("if flag then n := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap("if n then n := 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_comparison_ok) {
    auto r = parseSemaAll(wrap("if n > 0 then n := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, while_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap("while n do n := n - 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, while_bool_ok) {
    auto r = parseSemaAll(wrap("while flag do n := n - 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, elif_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap(
        "if flag then n := 1; elif n then n := 2; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_expression_int_condition_diagnoses) {
    auto r = parseSemaAll(wrap("n := 1 if n else 2;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, if_expression_bool_ok) {
    auto r = parseSemaAll(wrap("n := 1 if flag else 2;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_cond, string_condition_diagnoses) {
    auto r = parseSemaAll(wrap("if \"x\" then n := 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// R39 / §21.2 + §7.11: a `bool?` is not a valid condition (no three-valued
// logic). Previously it slipped past the bool check and reached codegen, which
// branched on the whole `{i8,i1}` nullable struct and emitted invalid IR. It
// must be rejected at sema with a clean diagnostic in every condition position.
namespace {
bool mentionsNullableBoolCond(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("not `bool?`") != std::string::npos) return true;
    return false;
}
} // namespace

BEZEL_TEST(sema_cond, if_nullable_bool_condition_diagnoses_r39) {
    auto r = parseSemaAll(wrap("var b: bool? := null; if b then n := 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsNullableBoolCond(r));
}

BEZEL_TEST(sema_cond, while_nullable_bool_condition_diagnoses_r39) {
    auto r = parseSemaAll(wrap("var b: bool? := null; while b do n := 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsNullableBoolCond(r));
}

BEZEL_TEST(sema_cond, if_expression_nullable_bool_condition_diagnoses_r39) {
    auto r = parseSemaAll(wrap("var b: bool? := null; n := 1 if b else 2;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsNullableBoolCond(r));
}

// Control: a coalesced `bool?` (`b ?? false`) is a plain `bool` and is fine.
BEZEL_TEST(sema_cond, coalesced_nullable_bool_condition_ok_r39) {
    auto r = parseSemaAll(
        wrap("var b: bool? := null; if (b ?? false) then n := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
