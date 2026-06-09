// §7.11 Lifted operators — sema tests (issue #486).
//
// Tests:
//  1. Arithmetic on nullable primitives → result is T? (nullable).
//  2. Relational on nullable primitives → result is bool (NOT bool?).
//  3. bool? to `and` / `or` / `not` → compile error (§7.11: no three-valued
//     logic).
//  4. Unary `-` / `~` on T? → result is T? (nullable).
//  5. Mixed T? + T → result is T? (nullable).
//  6. Assigning T? result to non-nullable var → compile error (§7.4 narrowing).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

// Parse, collect, resolve, and typecheck. Mirror of the helper used in
// expr_operator_types_test.cpp.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Return the rhs of the FIRST DiscardStmt (`_ := expr`) in the first method of
// the first top-level type. Mirrors the pattern used in expr_operator_types_test.
const bezel::ast::Expr* discardRhs(const bezel::test::SemaResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md || md->stmts.empty()) continue;
        for (const auto& s : md->stmts) {
            if (const auto* ds =
                    dynamic_cast<const bezel::ast::DiscardStmt*>(s.get()))
                return ds->value.get();
        }
    }
    return nullptr;
}

// Type of the discard-rhs expression.
const TypeRef* typeOf(const bezel::test::SemaResult& r) {
    const auto* e = discardRhs(r);
    return e ? r.ctx->exprType(*e) : nullptr;
}

// Type of the inner binary expr inside a discard (the discard rhs IS the
// binary expr when written `_ := a OP b`).
const TypeRef* typeOfBinaryDiscard(const bezel::test::SemaResult& r) {
    return typeOf(r);   // the discard rhs is the binary expr directly
}

// Type of a unary expr: the discard rhs is the unary expr directly.
const TypeRef* typeOfUnaryDiscard(const bezel::test::SemaResult& r) {
    return typeOf(r);
}

} // anonymous namespace


// =========================================================================
// §7.11 — Arithmetic on int? → result is int? (nullable)
// =========================================================================

// §7.11: int? + int? → result is int? (nullable = true, primitive = Int).
BEZEL_TEST(sema_lifted_ops_486, int_add_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a + b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Primitive);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    BEZEL_EXPECT_TRUE(t && t->nullable);
}

// §7.11: int? - int? → result is int?.
BEZEL_TEST(sema_lifted_ops_486, int_sub_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a - b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

// §7.11: int? * int? → result is int?.
BEZEL_TEST(sema_lifted_ops_486, int_mul_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a * b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

// §7.11: int? / int? → result is int?.
BEZEL_TEST(sema_lifted_ops_486, int_div_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a / b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
}

// §7.11: int? mod int? → result is int?.
BEZEL_TEST(sema_lifted_ops_486, int_mod_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a mod b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
}

// §7.11: int? & int? → result is int? (bitwise lift).
BEZEL_TEST(sema_lifted_ops_486, int_bitand_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a & b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
}

// §7.11: int? | int? → result is int? (bitwise lift).
BEZEL_TEST(sema_lifted_ops_486, int_bitor_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a | b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
}


// =========================================================================
// §7.11 — Relational on int? → result is bool (NOT bool?)
// =========================================================================

// §7.11: int? < int? → result type is bool (non-nullable).
BEZEL_TEST(sema_lifted_ops_486, int_lt_result_is_bool_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a < b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Primitive);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
    BEZEL_EXPECT_TRUE(t && !t->nullable);
}

// §7.11: int? <= int? → result is bool (not nullable).
BEZEL_TEST(sema_lifted_ops_486, int_le_result_is_bool_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a <= b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && !t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

// §7.11: int? > int? → result is bool.
BEZEL_TEST(sema_lifted_ops_486, int_gt_result_is_bool_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a > b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && !t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

// §7.11: int? >= int? → result is bool.
BEZEL_TEST(sema_lifted_ops_486, int_ge_result_is_bool_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) _ := a >= b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && !t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}


// =========================================================================
// §7.11 — bool? to `and` / `or` / `not` → compile error
// =========================================================================

// §7.11 "No three-valued logic": bool? as left operand to `and` is a compile
// error.
BEZEL_TEST(sema_lifted_ops_486, bool_nullable_and_is_error_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool?, b: bool?) _ := a and b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited711 = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("7.11") != std::string::npos ||
            d.message.find("bool?") != std::string::npos ||
            d.message.find("nullable") != std::string::npos)
            cited711 = true;
    BEZEL_EXPECT_TRUE(cited711);
}

// §7.11: bool? `or` bool? is a compile error.
BEZEL_TEST(sema_lifted_ops_486, bool_nullable_or_is_error_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool?, b: bool?) _ := a or b; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.11: `not bool?` is a compile error.
BEZEL_TEST(sema_lifted_ops_486, bool_nullable_not_is_error_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: bool?) _ := not a; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}


// =========================================================================
// §7.11 — Unary `-` / `~` on int? → result is int? (nullable)
// =========================================================================

// §7.11: unary `-` on int? → result is int? (nullable).
BEZEL_TEST(sema_lifted_ops_486, int_unary_neg_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?) _ := -a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfUnaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

// §7.11: unary `~` on int? → result is int? (nullable).
BEZEL_TEST(sema_lifted_ops_486, int_unary_bitnot_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?) _ := ~a; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfUnaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}


// =========================================================================
// §7.11 — Mixed T? + T → result is T? (nullable)
// =========================================================================

// §7.11: int? + int (non-nullable) → result is int?.
BEZEL_TEST(sema_lifted_ops_486, int_mixed_nullable_plus_nonnull_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int) _ := a + b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

// §7.11: int + int? → result is int?.
BEZEL_TEST(sema_lifted_ops_486, int_mixed_nonnull_plus_nullable_result_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int, b: int?) _ := a + b; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const TypeRef* t = typeOfBinaryDiscard(r);
    BEZEL_EXPECT_TRUE(t && t->nullable);
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}


// =========================================================================
// §7.11 — Narrowing required: assigning T? result to non-nullable is a
// type error (§7.4)
// =========================================================================

// §7.11: the lifted result is int?; assigning it to a plain `int` local (with
// explicit non-nullable type annotation) must be a compile error.
BEZEL_TEST(sema_lifted_ops_486, int_add_result_needs_narrowing_spec_7_11) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method M(a: int?, b: int?) "
        "  var x: int := a + b; "   // int? not assignable to int
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
