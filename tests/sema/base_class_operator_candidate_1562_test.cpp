// Spec-derived tests for §25.2 / §12.2 (v0.181, #1562): the operator
// candidate set for a binary or unary expression includes operators
// *inherited from base classes*, not only those declared on the operand's
// own type. §25.2 (v0.181): "the candidate set is the union of the
// `operator <op>` overloads declared on the static types of both operands,
// including declarations inherited from their base classes" and "A unary
// operator consults the operand's type and its base classes, likewise." A
// derived-typed operand satisfies the base-typed parameter by the ordinary
// argument rules.
//
// The sibling change #1498 covered the *constraint*-sourced half of #1562
// (type-parameter operands drawing candidates from static-interface
// constraints); these tests exercise the distinct base-class half.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::MethodSymbol;
using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The RHS of the first `_ := <expr>;` discard found in the method named
// `methodName` on the class named `className`.
const bezel::ast::Expr* discardIn(const bezel::test::SemaResult& r,
                                  std::string_view className,
                                  std::string_view methodName) {
    if (!r.sourceFile) return nullptr;
    for (const auto& top : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(top.get());
        if (!td || td->name != className) continue;
        for (const auto& m : td->members) {
            const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md || md->name != methodName) continue;
            for (const auto& s : md->stmts) {
                if (const auto* ds =
                        dynamic_cast<const bezel::ast::DiscardStmt*>(s.get()))
                    return ds->value.get();
            }
        }
    }
    return nullptr;
}

// The MethodSymbol sema resolved for an expression (the operator), or null.
const MethodSymbol* operatorOf(const bezel::test::SemaResult& r,
                               const bezel::ast::Expr* e) {
    if (!e || !r.ctx) return nullptr;
    return dynamic_cast<const MethodSymbol*>(r.ctx->exprSymbol(*e));
}

} // namespace

// §25.2: a binary `operator +` declared on base class `V` is a candidate for
// a derived-typed (`W`) operand — the operator resolves to V's declaration.
BEZEL_TEST(base_class_operator_1562, binary_plus_inherited_from_base) {
    auto r = parseSema(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator +(a: V, b: V): int => a.X + b.X; "
        "end V; "
        "class W : V end W; "
        "class C public method M(a: W, b: W) _ := a + b; end M; end C;");
    const auto* e = discardIn(r, "C", "M");
    const MethodSymbol* opm = operatorOf(r, e);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(opm != nullptr);
    BEZEL_EXPECT_TRUE(opm && opm->isOperator && opm->operatorToken == "+");
    // The resolved operator is the one declared on the BASE class V.
    BEZEL_EXPECT_TRUE(opm && opm->parent && opm->parent->name == "V");
    // Result type flows through: `operator +(V,V): int` → int.
    const TypeRef* t = e ? r.ctx->exprType(*e) : nullptr;
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
}

// §25.2: a comparison operator inherited from a base class resolves for a
// derived-typed operand and yields `bool` (complete group declared on base).
BEZEL_TEST(base_class_operator_1562, comparison_inherited_from_base) {
    auto r = parseSema(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator <(a: V, b: V): bool => a.X < b.X; "
        "  public static method operator >(a: V, b: V): bool => a.X > b.X; "
        "  public static method operator <=(a: V, b: V): bool => a.X <= b.X; "
        "  public static method operator >=(a: V, b: V): bool => a.X >= b.X; "
        "end V; "
        "class W : V end W; "
        "class C public method M(a: W, b: W) _ := a < b; end M; end C;");
    const auto* e = discardIn(r, "C", "M");
    const MethodSymbol* opm = operatorOf(r, e);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(opm && opm->isOperator && opm->operatorToken == "<");
    BEZEL_EXPECT_TRUE(opm && opm->parent && opm->parent->name == "V");
    const TypeRef* t = e ? r.ctx->exprType(*e) : nullptr;
    BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Bool);
}

// §25.2: the inheritance walk is transitive — an operator declared two levels
// up (`U`, base of `V`, base of `W`) is still a candidate for a `W` operand.
BEZEL_TEST(base_class_operator_1562, binary_inherited_two_levels) {
    auto r = parseSema(
        "module M; class U "
        "  public X: int := 0; "
        "  public static method operator *(a: U, b: U): int => a.X * b.X; "
        "end U; "
        "class V : U end V; "
        "class W : V end W; "
        "class C public method M(a: W, b: W) _ := a * b; end M; end C;");
    const auto* e = discardIn(r, "C", "M");
    const MethodSymbol* opm = operatorOf(r, e);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(opm && opm->isOperator && opm->operatorToken == "*");
    BEZEL_EXPECT_TRUE(opm && opm->parent && opm->parent->name == "U");
}

// §25.2: "A unary operator consults the operand's type and its base classes,
// likewise." A unary `operator -` declared on base `V` resolves for a `W`
// operand.
BEZEL_TEST(base_class_operator_1562, unary_minus_inherited_from_base) {
    auto r = parseSema(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator -(a: V): int => 0 - a.X; "
        "end V; "
        "class W : V end W; "
        "class C public method M(a: W) _ := -a; end M; end C;");
    const auto* e = discardIn(r, "C", "M");
    const MethodSymbol* opm = operatorOf(r, e);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(opm && opm->isOperator && opm->operatorToken == "-");
    BEZEL_EXPECT_TRUE(opm && opm->parent && opm->parent->name == "V");
}

// §25.2 order rule is preserved through inheritance: an operator inherited on
// one operand still requires the *other* operand to match its parameter in
// order — a mismatched primitive right operand does not resolve the operator
// (no commutation), so no base-class operator is selected.
BEZEL_TEST(base_class_operator_1562, order_still_enforced_through_inheritance) {
    auto r = parseSema(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator +(a: V, b: V): int => a.X + b.X; "
        "end V; "
        "class W : V end W; "
        "class C public method M(a: W) _ := a + 1; end M; end C;");
    const auto* e = discardIn(r, "C", "M");
    // `a + 1`: right operand is a primitive, not a V/W — the inherited
    // `operator +(V, V)` does not apply, so no user operator is selected.
    const MethodSymbol* opm = operatorOf(r, e);
    BEZEL_EXPECT_TRUE(opm == nullptr);
}
