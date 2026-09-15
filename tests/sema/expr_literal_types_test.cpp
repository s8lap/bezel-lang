// Spec-derived tests for Sema slice 3 (literal-type subset). Each
// literal form should receive its canonical primitive TypeRef via
// SemaContext::exprType.

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

// Pull the first method-body expression (its first statement's RHS or
// the method's expression body) and return its annotated TypeRef.
const TypeRef* firstMethodFirstExprType(const bezel::test::SemaResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md) continue;
        if (md->bodyKind == bezel::ast::MethodBodyKind::Expression &&
            md->exprBody) {
            return r.ctx->exprType(*md->exprBody);
        }
        if (md->bodyKind == bezel::ast::MethodBodyKind::Block &&
            !md->stmts.empty()) {
            const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
                md->stmts[0].get());
            if (ds && ds->value) return r.ctx->exprType(*ds->value);
            const auto* rs = dynamic_cast<const bezel::ast::ReturnStmt*>(
                md->stmts[0].get());
            if (rs && rs->value) return r.ctx->exprType(*rs->value);
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_expr_lit, int_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 42; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Int);
}

BEZEL_TEST(sema_expr_lit, float_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 3.14; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Double);
}

BEZEL_TEST(sema_expr_lit, char_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 'x'; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Char);
}

BEZEL_TEST(sema_expr_lit, string_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := \"hi\"; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::String);
}

BEZEL_TEST(sema_expr_lit, verbatim_string_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := @\"hi\"; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::String);
}

BEZEL_TEST(sema_expr_lit, interp_string_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := $\"hi {name}\"; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::String);
}

BEZEL_TEST(sema_expr_lit, bool_literal) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := true; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Bool);
}

BEZEL_TEST(sema_expr_lit, null_literal_is_nullable_object) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := null; end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) {
        BEZEL_EXPECT_TRUE(t->primitive == PrimitiveType::Object);
        BEZEL_EXPECT_TRUE(t->nullable);
    }
}

BEZEL_TEST(sema_expr_lit, unit_value) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := (); end M; end C;");
    auto* t = firstMethodFirstExprType(r);
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) BEZEL_EXPECT_TRUE(t->kind == TypeRef::Kind::Unit);
}

BEZEL_TEST(sema_expr_lit, primitive_typeref_cache) {
    // Two int literals should share the same TypeRef* (the primitive cache).
    auto r = parseSemaAll(
        "module M; class C "
        "  public method M() _ := 1; end M; "
        "  public method N() _ := 2; end N; "
        "end C;");
    const TypeRef* a = nullptr;
    const TypeRef* b = nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    for (const auto& m : td->members) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
        if (!md || md->stmts.empty()) continue;
        const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
            md->stmts[0].get());
        if (!ds || !ds->value) continue;
        if (!a) a = r.ctx->exprType(*ds->value);
        else if (!b) b = r.ctx->exprType(*ds->value);
    }
    BEZEL_EXPECT_TRUE(a != nullptr && a == b);
}

BEZEL_TEST(sema_expr_lit, recursion_into_binary_subexprs) {
    // Slice 3 doesn't compute a BinaryExpr's own type, but it should still
    // attach types to the leaf literal subexpressions.
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 1 + 2; end M; end C;");
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    const auto* be = dynamic_cast<const bezel::ast::BinaryExpr*>(ds->value.get());
    BEZEL_EXPECT_TRUE(be != nullptr);
    if (be && be->left) {
        auto* lt = r.ctx->exprType(*be->left);
        BEZEL_EXPECT_TRUE(lt && lt->primitive == PrimitiveType::Int);
    }
    if (be && be->right) {
        auto* rt = r.ctx->exprType(*be->right);
        BEZEL_EXPECT_TRUE(rt && rt->primitive == PrimitiveType::Int);
    }
    // Slice 3c typed the BinaryExpr itself — `int + int` is int.
    auto* whole = r.ctx->exprType(*ds->value);
    BEZEL_EXPECT_TRUE(whole && whole->primitive == PrimitiveType::Int);
}

// §5: an unsuffixed integer literal must fit in `int`; a larger value
// requires a suffix (it is not silently widened or wrapped).
BEZEL_TEST(sema_literal, unsuffixed_int_in_range_ok) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 2147483647; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_literal, unsuffixed_int_too_large_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 2147483648; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_literal, large_literal_with_long_suffix_ok) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 5000000000L; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_literal, large_hex_literal_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C public method M() _ := 0xFFFFFFFF; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// #1051 / §5.1: an unsuffixed integer literal > int.MaxValue passed as an
// argument to a `uint`/`long`/`ulong` parameter (method, explicit constructor,
// or positional-record field) is target-typed to the parameter and widened,
// rather than rejected with "does not fit in int" (mirrors the variable-init
// hint, #184). A too-large literal for an `int` parameter still rejects.
namespace {
bool semaHasErrors(std::string_view src) {
    auto r = parseSemaAll(src);
    return r.diagnostics.hasErrors();
}
}  // namespace
BEZEL_TEST(sema_expr_lit, literal_widens_to_uint_method_arg_1051) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Id(x: uint): uint return x; end Id; "
        "public static method Main(): int var u: uint := Id(4000000000); "
        "return (if (u > 1000) then 1 else 0); end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_widens_to_long_method_arg_1051) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Id(x: long): long return x; end Id; "
        "public static method Main(): int var l: long := Id(5000000000); "
        "return (if (l > 1000) then 1 else 0); end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_widens_to_positional_record_uint_field_1051) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; record P(V: uint); class C public static method Main(): int "
        "var p := new P(4000000000); return (if (p.V > 1000) then 1 else 0); end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_too_large_for_int_param_still_rejected_1051) {
    BEZEL_EXPECT_TRUE(semaHasErrors(
        "module M; class C public static method Id(x: int): int return x; end Id; "
        "public static method Main(): int return Id(5000000000); end Main; end C;"));
}

// #1052 / §5.1: a generic positional-record field declared as a bare type
// parameter (`Box<T>(V: T)`) target-types a large integer literal to the
// CONCRETE generic argument of the instantiation — `new Box<uint>(4000000000)`
// widens to uint rather than rejecting. A `Box<int>` instantiation still
// rejects a literal > int.MaxValue.
BEZEL_TEST(sema_expr_lit, literal_widens_to_generic_record_uint_arg_1052) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; record Box<T>(V: T); class C public static method Main(): int "
        "var b := new Box<uint>(4000000000); return (if (b.V > 1000) then 1 else 0); end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_widens_to_generic_record_long_arg_1052) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; record Box<T>(V: T); class C public static method Main(): int "
        "var b := new Box<long>(5000000000); return (if (b.V > 1000) then 1 else 0); end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_too_large_for_generic_int_arg_rejected_1052) {
    BEZEL_EXPECT_TRUE(semaHasErrors(
        "module M; record Box<T>(V: T); class C public static method Main(): int "
        "var b := new Box<int>(5000000000); return int(b.V); end Main; end C;"));
}

// issue #1576 / §4.2 (v0.182): "Typed-context widening" lists the admitting
// contexts as a typed variable declaration, a typed field initializer, a
// declared-integer-type parameter, an assignment target whose declared type
// is an integer type, or a `return` in a method with a declared integer
// return type. v0.182 corrected "decimal literal" to "integer literal" (hex/
// binary spellings widen identically to decimal) and completed the context
// list with the assignment-target and `return` cases. Each admitting context
// below widens an unsuffixed literal exceeding `int`'s range to the target's
// declared wider integer type (long/ulong/uint); an `int`-typed target in the
// same position still rejects the literal (no accidental width).

// A typed field initializer (already-admitting per v0.159's "codifying
// implemented behavior," but the compiler had never actually threaded the
// declared field type as a widening hint before this fix).
BEZEL_TEST(sema_expr_lit, literal_widens_to_long_field_initializer_1576) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public F: long := 3000000000; "
        "public static method Main(): int return 0; end Main; end C;"));
}

// The assignment-target context, added to the admitting list in v0.182.
BEZEL_TEST(sema_expr_lit, literal_widens_to_long_assignment_target_1576) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Main(): int "
        "var x: long := 0; x := 3000000000; return 0; end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_widens_to_uint_assignment_target_1576) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Main(): int "
        "var x: uint := 0; x := 4000000000; return 0; end Main; end C;"));
}
// A compound assignment (`+=`) target is an assignment target too; its RHS
// literal widens by the same rule.
BEZEL_TEST(sema_expr_lit, literal_widens_to_long_compound_assignment_target_1576) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Main(): int "
        "var x: long := 1; x += 3000000000; return 0; end Main; end C;"));
}
// An `int`-typed assignment target still rejects — the widening only applies
// when the declared target type is itself wider than `int`.
BEZEL_TEST(sema_expr_lit, literal_too_large_for_int_assignment_target_rejected_1576) {
    BEZEL_EXPECT_TRUE(semaHasErrors(
        "module M; class C public static method Main(): int "
        "var x: int := 0; x := 3000000000; return 0; end Main; end C;"));
}

// The `return` context, added to the admitting list in v0.182.
BEZEL_TEST(sema_expr_lit, literal_widens_to_long_return_1576) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Big(): long "
        "return 3000000000; end Big; "
        "public static method Main(): int return 0; end Main; end C;"));
}
// An `int`-returning method still rejects a too-large literal `return`.
BEZEL_TEST(sema_expr_lit, literal_too_large_for_int_return_rejected_1576) {
    BEZEL_EXPECT_TRUE(semaHasErrors(
        "module M; class C public static method Big(): int "
        "return 3000000000; end Big; "
        "public static method Main(): int return 0; end Main; end C;"));
}

// v0.182's "decimal literal" -> "integer literal" correction: a hex or
// binary spelling widens exactly like the decimal spelling in each admitting
// context (the widening reads the literal's magnitude, not its base).
BEZEL_TEST(sema_expr_lit, hex_literal_widens_to_long_assignment_target_1576) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Main(): int "
        "var x: long := 0; x := 0x100000000; return 0; end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, binary_literal_widens_to_ulong_return_1576) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public static method Big(): ulong "
        "return 0b100000000000000000000000000000000; end Big; "
        "public static method Main(): int return 0; end Main; end C;"));
}

// issue #1590 / §4.2 (v0.159/v0.182): an auto-property default initializer
// (`property P: T := …;`, §23.1) initializes the compiler-generated backing
// field at the same construction-time step §22.8 governs for an explicit
// field initializer -- §11.1 and the design rationale's cyclic-serialization
// rule both speak of "field and property initializers" together as one
// construction-time concept, so the property-initializer form is the same
// admitting typed context §4.2 names for a field initializer, even though
// it is a distinct AST shape (ast::PropertyDecl::autoInitializer rather than
// ast::FieldDecl::initializer). #1576 fixed the field-initializer,
// assignment-target, and return-target gaps but explicitly left this one
// unfixed pending this ruling.
BEZEL_TEST(sema_expr_lit, literal_widens_to_long_property_initializer_1590) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public property P: long := 3000000000; "
        "public static method Main(): int return 0; end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_widens_to_uint_property_initializer_1590) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public property P: uint := 4000000000; "
        "public static method Main(): int return 0; end Main; end C;"));
}
BEZEL_TEST(sema_expr_lit, literal_widens_to_ulong_property_initializer_1590) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public property P: ulong := 10000000000; "
        "public static method Main(): int return 0; end Main; end C;"));
}
// Hex/binary spellings widen identically to decimal in this context too,
// mirroring the #1576 field-initializer/assignment/return coverage.
BEZEL_TEST(sema_expr_lit, hex_literal_widens_to_long_property_initializer_1590) {
    BEZEL_EXPECT_FALSE(semaHasErrors(
        "module M; class C public property P: long := 0x100000000; "
        "public static method Main(): int return 0; end Main; end C;"));
}
// An `int`-typed auto-property still rejects -- the widening only applies
// when the declared property type is itself wider than `int` (no accidental
// width).
BEZEL_TEST(sema_expr_lit,
           literal_too_large_for_int_property_initializer_rejected_1590) {
    BEZEL_EXPECT_TRUE(semaHasErrors(
        "module M; class C public property P: int := 3000000000; "
        "public static method Main(): int return 0; end Main; end C;"));
}
// A magnitude exceeding even the wider declared target's range still
// rejects -- widening admits the value only when it actually fits.
BEZEL_TEST(sema_expr_lit,
           literal_too_large_for_long_property_initializer_rejected_1590) {
    BEZEL_EXPECT_TRUE(semaHasErrors(
        "module M; class C public property P: long := "
        "99999999999999999999999999; "
        "public static method Main(): int return 0; end Main; end C;"));
}
