// fix #1588 / §3.5: unary `-` (negation), grouped with unary `+` under
// "Arithmetic" and defined only on the numeric primitives, is invalid on
// `bool`, `char`, and `string`.
//
// Before this fix, ExprTypecheck_Exprs.cpp's `typeUnary` never inspected
// `-`'s operand kind at all (only `~` had a bad-kind check, and #1574 added
// one for `+`), so `-boolExpr` / `-charExpr` silently type-checked as an
// identity pass-through (the generic fall-through), and `-stringExpr` reached
// codegen — which emitted an integer `sub` on a string's ptr representation
// and crashed on LLVM IR verification instead of raising a clean diagnostic.
// Mirrors tests/sema/bool_ordering_1574_test.cpp's unary-`+` coverage.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Type of the rhs of the FIRST DiscardStmt (`_ := expr`) in the first
// method of the first top-level type (mirrors bool_ordering_1574's helper).
const TypeRef* discardExprType(const bezel::test::SemaResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md) continue;
        for (const auto& s : md->stmts) {
            if (const auto* ds =
                    dynamic_cast<const bezel::ast::DiscardStmt*>(s.get()))
                return ds->value ? r.ctx->exprType(*ds->value) : nullptr;
        }
    }
    return nullptr;
}

bool citesSpec35(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("3.5") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =========================================================================
// §3.5: unary `-` remains valid identity-negation on numeric primitives.
// =========================================================================

BEZEL_TEST(sema_unary_minus_1588, unary_minus_int_admitted_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int) _ := -a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Primitive);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    BEZEL_EXPECT_TRUE(t && !t->nullable);
}

BEZEL_TEST(sema_unary_minus_1588, unary_minus_double_admitted_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: double) _ := -a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Double);
}

BEZEL_TEST(sema_unary_minus_1588, unary_minus_decimal_admitted_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: decimal) _ := -a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Decimal);
}

// §7.11: unary `-` lifts over T? to yield T? (nullable).
BEZEL_TEST(sema_unary_minus_1588, unary_minus_nullable_int_lifts_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?) _ := -a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

// =========================================================================
// §3.5: unary `-` is invalid on `bool`, `char`, and `string`.
// =========================================================================

BEZEL_TEST(sema_unary_minus_1588, unary_minus_bool_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool) _ := -a; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

BEZEL_TEST(sema_unary_minus_1588, unary_minus_char_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: char) _ := -a; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

// The reported bug's worst variant: `-stringExpr` used to reach codegen and
// crash on LLVM IR verification (an integer `sub` on a string's ptr
// representation) instead of being rejected here in sema.
BEZEL_TEST(sema_unary_minus_1588, unary_minus_string_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: string) _ := -a; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

// Literal form of the bool case, mirroring #1574's literal-form coverage.
BEZEL_TEST(sema_unary_minus_1588, unary_minus_bool_literal_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M() _ := -true; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}
