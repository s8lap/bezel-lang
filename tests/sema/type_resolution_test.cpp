// Spec-derived tests for the Sema slice 2 — Type Resolution pass.
//
// Slice 2's contract: walk every ast::Type node and attach a resolved
// TypeRef via SemaContext::resolvedType. Primitives + Named + Unit +
// InlineArray + Function are all admitted. Unresolved names and generic-
// arity mismatches are diagnosed.
//
// Detailed inter-type semantics (variance, conversions, etc.) belong to
// later sema slices.

#include <string>
#include <vector>

#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::ast::Type;
using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseAndResolve(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

const bezel::ast::Type* firstFieldType(const bezel::test::SemaResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* fd = dynamic_cast<const bezel::ast::FieldDecl*>(
        td->members[0].get());
    return fd ? fd->type.get() : nullptr;
}

// The type named in a `catch T` arm must be resolved (else codegen can't
// match the arm). Finds the first try statement's first catch arm type.
const bezel::ast::Type* firstCatchArmType(const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md) continue;
            for (const auto& s : md->stmts) {
                auto* ts = dynamic_cast<const bezel::ast::TryStmt*>(s.get());
                if (ts && !ts->catches.empty() &&
                    !ts->catches[0].types.empty())
                    return ts->catches[0].types[0].get();
            }
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_types, catch_arm_type_resolved) {
    auto r = parseAndResolve(
        "module M; class E: Exception end E; "
        "class C public static method Main(): int "
        "  try return 0; catch E => return 1; end; return 2; "
        "end Main; end C;");
    const auto* ty = firstCatchArmType(r);
    BEZEL_EXPECT_TRUE(ty != nullptr);
    if (ty) {
        auto* rt = r.ctx->resolvedType(*ty);
        BEZEL_EXPECT_TRUE(rt && rt->kind == TypeRef::Kind::Named &&
                          rt->declaration != nullptr);
    }
}

BEZEL_TEST(sema_types, primitive_int) {
    auto r = parseAndResolve("module M; class C public x: int; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* t = firstFieldType(r);
    auto* ref = t ? r.ctx->resolvedType(*t) : nullptr;
    BEZEL_EXPECT_TRUE(ref != nullptr);
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(ref->primitive == PrimitiveType::Int);
        BEZEL_EXPECT_FALSE(ref->nullable);
    }
}

BEZEL_TEST(sema_types, primitive_nullable_string) {
    auto r = parseAndResolve("module M; class C public x: string?; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(ref->primitive == PrimitiveType::String);
        BEZEL_EXPECT_TRUE(ref->nullable);
    }
}

BEZEL_TEST(sema_types, every_primitive_recognised) {
    const std::vector<std::pair<const char*, PrimitiveType>> cases = {
        {"bool", PrimitiveType::Bool},
        {"char", PrimitiveType::Char},
        {"byte", PrimitiveType::Byte},
        {"sbyte", PrimitiveType::SByte},
        {"short", PrimitiveType::Short},
        {"ushort", PrimitiveType::UShort},
        {"int", PrimitiveType::Int},
        {"uint", PrimitiveType::UInt},
        {"long", PrimitiveType::Long},
        {"ulong", PrimitiveType::ULong},
        {"float", PrimitiveType::Float},
        {"double", PrimitiveType::Double},
        {"decimal", PrimitiveType::Decimal},
    };
    for (const auto& [name, prim] : cases) {
        const std::string src =
            std::string("module M; class C public x: ") + name + "; end C;";
        auto r = parseAndResolve(src);
        BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
        auto* ref = r.ctx->resolvedType(*firstFieldType(r));
        if (ref) {
            BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Primitive);
            BEZEL_EXPECT_TRUE(ref->primitive == prim);
        }
    }
}

BEZEL_TEST(sema_types, named_type_resolves_to_local_type_symbol) {
    auto r = parseAndResolve(
        "module M; class C public x: D; end C; class D end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Named);
        BEZEL_EXPECT_TRUE(ref->declaration != nullptr);
        if (ref->declaration) {
            BEZEL_EXPECT_EQ(
                std::string(ref->declaration->name), std::string("D"));
        }
    }
}

BEZEL_TEST(sema_types, generic_type_param_resolves_in_class_scope) {
    auto r = parseAndResolve(
        "module M; class Box<T> public x: T; end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Named);
        // Generic params resolve to "Named with no declaration" (a sentinel
        // the typecheck pass will replace with a GenericParamSymbol pointer
        // when it gets fancier).
        BEZEL_EXPECT_TRUE(ref->declaration == nullptr);
    }
}

BEZEL_TEST(sema_types, unresolved_name_is_diagnosed) {
    auto r = parseAndResolve(
        "module M; class C public x: UnknownType; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Error);
    }
}

BEZEL_TEST(sema_types, generic_arity_mismatch_is_diagnosed) {
    auto r = parseAndResolve(
        "module M; class Map<K, V> end Map; class C public x: Map<int>; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_types, generic_arity_match_is_clean) {
    auto r = parseAndResolve(
        "module M; class Map<K, V> end Map; class C public x: Map<int, string>; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Named);
        BEZEL_EXPECT_EQ(ref->genericArgs.size(), std::size_t(2));
    }
}

BEZEL_TEST(sema_types, unit_type) {
    auto r = parseAndResolve("module M; class C public x: (); end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Unit);
}

BEZEL_TEST(sema_types, inline_array) {
    auto r = parseAndResolve("module M; class C public x: [10]int; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::InlineArray);
        BEZEL_EXPECT_TRUE(ref->arrayElement != nullptr);
        if (ref->arrayElement) {
            BEZEL_EXPECT_TRUE(
                ref->arrayElement->kind == TypeRef::Kind::Primitive);
            BEZEL_EXPECT_TRUE(
                ref->arrayElement->primitive == PrimitiveType::Int);
        }
    }
}

BEZEL_TEST(sema_types, inline_array_owned_slots) {
    auto r = parseAndResolve(
        "module M; class Resource end Resource; "
        "class C public x: [16 owned]Resource; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::InlineArray);
        BEZEL_EXPECT_TRUE(ref->arrayOwned);
    }
}

BEZEL_TEST(sema_types, function_type_two_param) {
    auto r = parseAndResolve(
        "module M; class C public x: (int, string) -> bool; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Function);
        BEZEL_EXPECT_EQ(ref->funcParams.size(), std::size_t(2));
        BEZEL_EXPECT_TRUE(ref->funcReturn != nullptr);
        if (ref->funcReturn) {
            BEZEL_EXPECT_TRUE(
                ref->funcReturn->primitive == PrimitiveType::Bool);
        }
    }
}

BEZEL_TEST(sema_types, function_type_owned_return) {
    auto r = parseAndResolve(
        "module M; class Resource end Resource; "
        "class C public x: () -> owned Resource; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) {
        BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Function);
        BEZEL_EXPECT_TRUE(ref->funcReturnOwned);
    }
}

BEZEL_TEST(sema_types, primitive_with_generic_args_is_diagnosed) {
    auto r = parseAndResolve(
        "module M; class C public x: int<bool>; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_types, method_return_type_resolves) {
    auto r = parseAndResolve(
        "module M; class C "
        "  public method M(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (td && !td->members.empty()) {
        const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
            td->members[0].get());
        if (md && md->returnType) {
            auto* ref = r.ctx->resolvedType(*md->returnType);
            BEZEL_EXPECT_TRUE(ref && ref->kind == TypeRef::Kind::Primitive);
        }
    }
}

BEZEL_TEST(sema_types, base_type_resolves) {
    auto r = parseAndResolve(
        "module M; "
        "public interface IFoo end IFoo; "
        "public class C: IFoo end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[1].get());
    if (td && !td->baseTypes.empty()) {
        auto* ref = r.ctx->resolvedType(*td->baseTypes[0]);
        BEZEL_EXPECT_TRUE(ref && ref->kind == TypeRef::Kind::Named);
        if (ref && ref->declaration) {
            BEZEL_EXPECT_EQ(
                std::string(ref->declaration->name), std::string("IFoo"));
        }
    }
}

BEZEL_TEST(sema_types, kitchen_sink_resolves_cleanly) {
    const std::string src = R"BEZEL(
module Sink;
public class Service<T>
  private _id: int;
  private _items: [10]T;
  private _factory: (int) -> owned Service<T>;
  public property Name: string => "x";
  public method Process(): bool => true;
end Service;
)BEZEL";
    auto r = parseAndResolve(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
