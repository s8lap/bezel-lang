// fix #1574 / §3.5 (v0.182): ordering operators (`<`, `<=`, `>`, `>=`) on
// `bool` operands are a compile error; `=`/`<>` remain the only comparisons
// defined on `bool`. Also covers unary `+` (identity, §3.5 v0.182): defined
// on the numeric primitives only, invalid on `bool`, `char`, `string`.
//
// Before this fix, ExprTypecheck_Exprs.cpp's relational-operator result-type
// rule typed `<`/`<=`/`>`/`>=` as `bool` for ANY primitive operand pair with
// no bool-specific guard, so `true < false` compiled and evaluated to `true`
// (matching no defined semantics — §4.1's `Boolean` wrapper deliberately
// declares no `IComparable`). Separately, unary `+` had no operand-kind
// check at all (unlike unary `~`'s existing bad-kind check), so
// `+trueExpr` / `+charExpr` / `+stringExpr` silently type-checked as an
// identity pass-through.

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
// method of the first top-level type (mirrors lifted_operators_486's
// helper).
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
        if (d.message.find("3.5") != std::string::npos ||
            d.message.find("bool") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =========================================================================
// §3.5 (v0.182): ordering operators on `bool` operands are a compile error.
// =========================================================================

BEZEL_TEST(sema_bool_ordering_1574, lt_on_bool_vars_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool, b: bool) _ := a < b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

BEZEL_TEST(sema_bool_ordering_1574, le_on_bool_vars_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool, b: bool) _ := a <= b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

BEZEL_TEST(sema_bool_ordering_1574, gt_on_bool_vars_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool, b: bool) _ := a > b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

BEZEL_TEST(sema_bool_ordering_1574, ge_on_bool_vars_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool, b: bool) _ := a >= b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

// The reported bug: `true < false` compiled and evaluated `true`. Confirm
// the literal form is rejected too (not just typed-parameter operands).
BEZEL_TEST(sema_bool_ordering_1574, lt_on_bool_literals_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M() _ := true < false; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

// One bool operand, one non-bool: still rejected (the guard fires on EITHER
// operand being bool, mirroring the record/class relational guard above it).
BEZEL_TEST(sema_bool_ordering_1574, lt_bool_and_int_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool, b: int) _ := a < b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.11 boundary note (v0.182): `bool?` does not lift for ordering either,
// since bool itself has no ordering to lift.
BEZEL_TEST(sema_bool_ordering_1574, lt_on_nullable_bool_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool?, b: bool?) _ := a < b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// =========================================================================
// §3.5 / §8.4: `=` and `<>` remain valid on `bool` (unaffected by the
// ordering rejection above).
// =========================================================================

BEZEL_TEST(sema_bool_ordering_1574, eq_on_bool_still_admitted_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool, b: bool) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Primitive);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

BEZEL_TEST(sema_bool_ordering_1574, neq_on_bool_still_admitted_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool, b: bool) _ := a <> b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: ordering on a non-bool primitive is unaffected by the new guard.
BEZEL_TEST(sema_bool_ordering_1574, lt_on_int_still_admitted_control) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int, b: int) _ := a < b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_bool_ordering_1574, lt_on_char_still_admitted_control) {
    // §4.5: "Character values compare by code-point value: 'A' < 'B'."
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: char, b: char) _ := a < b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §3.5 (v0.182): unary `+` is the identity on numeric primitives.
// =========================================================================

BEZEL_TEST(sema_bool_ordering_1574, unary_plus_int_is_identity_type_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int) _ := +a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Primitive);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    BEZEL_EXPECT_TRUE(t && !t->nullable);
}

BEZEL_TEST(sema_bool_ordering_1574, unary_plus_double_is_identity_type_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: double) _ := +a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Double);
}

BEZEL_TEST(sema_bool_ordering_1574, unary_plus_decimal_is_identity_type_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: decimal) _ := +a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Decimal);
}

BEZEL_TEST(sema_bool_ordering_1574, unary_plus_byte_admitted_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: byte) _ := +a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.11: unary `+` lifts over T? to yield T? (nullable), same as unary `-`.
BEZEL_TEST(sema_bool_ordering_1574, unary_plus_nullable_int_lifts_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?) _ := +a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = discardExprType(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

// =========================================================================
// §3.5 (v0.182): unary `+` is invalid on `bool`, `char`, and `string`.
// =========================================================================

BEZEL_TEST(sema_bool_ordering_1574, unary_plus_bool_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool) _ := +a; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(citesSpec35(r));
}

BEZEL_TEST(sema_bool_ordering_1574, unary_plus_char_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: char) _ := +a; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_bool_ordering_1574, unary_plus_string_rejected_spec_3_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: string) _ := +a; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
