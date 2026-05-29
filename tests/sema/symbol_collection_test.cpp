// Spec-derived tests for the Sema slice 1 — Symbol Collection pass.
//
// Slice 1's contract: walk a parsed SourceFile and produce a flat symbol
// list (queryable via SemaContext::symbols()) plus a ModuleSymbol with
// children pointing to top-level type/alias/extension/extern entries.
// Each entry's members are populated (one level deep — nested types
// recurse). Generic params and method/ctor params are also collected.
//
// Detailed type-resolution and member-lookup happen in later slices. The
// tests here assert on the *shape* of the symbol table: number of
// symbols, names, parent links, and a one-line `symbolSummary` rendering.

#include <algorithm>
#include <string>
#include <vector>

#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::Symbol;
using bezel::test::parseAndCollect;
using bezel::test::symbolSummary;

// (Helpers from earlier drafts were removed; the tests inline the small
// amount of traversal they need.)

BEZEL_TEST(sema_symbols, empty_module_only_has_module_symbol) {
    auto r = parseAndCollect("module M;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ctx->module() != nullptr);
    if (r.ctx->module()) {
        BEZEL_EXPECT_EQ(r.ctx->module()->name, std::string_view("M"));
        BEZEL_EXPECT_TRUE(r.ctx->module()->children.empty());
    }
    BEZEL_EXPECT_EQ(r.ctx->symbols().size(), std::size_t(1));
}

BEZEL_TEST(sema_symbols, single_empty_class) {
    auto r = parseAndCollect("module M; public class Foo end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.ctx->module()->children.size(), std::size_t(1));
    if (r.ctx->module()->children.size() == 1) {
        const Symbol* foo = r.ctx->module()->children[0];
        BEZEL_EXPECT_EQ(symbolSummary(*foo), std::string("public class Foo"));
    }
}

BEZEL_TEST(sema_symbols, four_top_level_decl_kinds) {
    auto r = parseAndCollect(
        "module M; "
        "public class C end C; "
        "public interface I end I; "
        "public record R(x: int); "
        "public enum E A, B; end E; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.ctx->module()->children.size(), std::size_t(4));
    std::vector<std::string> got;
    for (auto* c : r.ctx->module()->children) got.push_back(symbolSummary(*c));
    std::vector<std::string> want = {
        "public class C", "public interface I",
        "public record R", "public enum E"};
    BEZEL_EXPECT_EQ(got, want);
}

BEZEL_TEST(sema_symbols, type_alias_is_registered) {
    auto r = parseAndCollect("module M; type Map<K, V> = IDictionary<K, V>;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.ctx->module()->children.size(), std::size_t(1));
    if (r.ctx->module()->children.size() == 1) {
        BEZEL_EXPECT_EQ(
            symbolSummary(*r.ctx->module()->children[0]),
            std::string("type Map"));
    }
}

BEZEL_TEST(sema_symbols, class_member_collection) {
    auto r = parseAndCollect(
        "module M; class Foo "
        "  public x: int; "
        "  public method M(): int => 42; "
        "  public constructor() end constructor; "
        "  public const C := 1; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* foo =
        dynamic_cast<const bezel::sema::TypeSymbol*>(
            r.ctx->module()->children[0]);
    BEZEL_EXPECT_TRUE(foo != nullptr);
    if (foo) {
        std::vector<std::string> memberSums;
        for (auto* m : foo->members) memberSums.push_back(symbolSummary(*m));
        std::vector<std::string> want = {
            "public field Foo.x",
            "public method Foo.M",
            "public constructor Foo.",
            "public const Foo.C"};
        BEZEL_EXPECT_EQ(memberSums, want);
    }
}

BEZEL_TEST(sema_symbols, enum_constants_collected_as_members) {
    auto r = parseAndCollect(
        "module M; public enum Color "
        "  Red, Green, Blue; "
        "end Color;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* color = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    if (color) {
        std::vector<std::string> sums;
        for (auto* m : color->members) sums.push_back(symbolSummary(*m));
        std::vector<std::string> want = {
            "enum-constant Color.Red",
            "enum-constant Color.Green",
            "enum-constant Color.Blue"};
        BEZEL_EXPECT_EQ(sums, want);
    }
}

BEZEL_TEST(sema_symbols, property_indexer_and_event) {
    auto r = parseAndCollect(
        "module M; class C "
        "  public property Name: string => \"x\"; "
        "  public property[i: int]: int => 0; "
        "  public event Click; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* c = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    if (c) {
        BEZEL_EXPECT_EQ(c->members.size(), std::size_t(3));
        BEZEL_EXPECT_EQ(
            symbolSummary(*c->members[0]),
            std::string("public property C.Name"));
        BEZEL_EXPECT_EQ(
            symbolSummary(*c->members[1]),
            std::string("public indexer C..this"));
        BEZEL_EXPECT_EQ(
            symbolSummary(*c->members[2]),
            std::string("public event C.Click"));
    }
}

BEZEL_TEST(sema_symbols, generic_params_collected) {
    auto r = parseAndCollect(
        "module M; public class Box<in T, out U> end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* box = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    if (box) {
        BEZEL_EXPECT_EQ(box->genericParams.size(), std::size_t(2));
        if (box->genericParams.size() == 2) {
            BEZEL_EXPECT_EQ(
                symbolSummary(*box->genericParams[0]),
                std::string("generic-param T"));
            BEZEL_EXPECT_EQ(
                symbolSummary(*box->genericParams[1]),
                std::string("generic-param U"));
            auto* g0 = dynamic_cast<const bezel::sema::GenericParamSymbol*>(
                box->genericParams[0]);
            auto* g1 = dynamic_cast<const bezel::sema::GenericParamSymbol*>(
                box->genericParams[1]);
            if (g0) BEZEL_EXPECT_EQ(g0->variance, std::string_view("in"));
            if (g1) BEZEL_EXPECT_EQ(g1->variance, std::string_view("out"));
        }
    }
}

BEZEL_TEST(sema_symbols, method_params_collected) {
    auto r = parseAndCollect(
        "module M; class C "
        "  public method Add(ref a: int, b: int, owned r: Resource) => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* c = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    if (c) {
        const auto* m = dynamic_cast<const bezel::sema::MethodSymbol*>(
            c->members[0]);
        BEZEL_EXPECT_EQ(m->params.size(), std::size_t(3));
        if (m->params.size() == 3) {
            auto* p0 = dynamic_cast<const bezel::sema::ParameterSymbol*>(
                m->params[0]);
            auto* p2 = dynamic_cast<const bezel::sema::ParameterSymbol*>(
                m->params[2]);
            if (p0) {
                BEZEL_EXPECT_EQ(p0->name, std::string_view("a"));
                BEZEL_EXPECT_EQ(p0->modifier, std::string_view("ref"));
            }
            if (p2) {
                BEZEL_EXPECT_EQ(p2->name, std::string_view("r"));
                BEZEL_EXPECT_EQ(p2->modifier, std::string_view("owned"));
            }
        }
    }
}

BEZEL_TEST(sema_symbols, operator_overload_marked_as_operator) {
    auto r = parseAndCollect(
        "module M; class C "
        "  public static method operator =(a: C, b: C): bool => true; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* c = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    if (c && !c->members.empty()) {
        const auto* m = dynamic_cast<const bezel::sema::MethodSymbol*>(
            c->members[0]);
        if (m) {
            BEZEL_EXPECT_TRUE(m->isOperator);
            BEZEL_EXPECT_EQ(m->operatorToken, std::string_view("="));
        }
    }
}

BEZEL_TEST(sema_symbols, nested_type_recursion) {
    auto r = parseAndCollect(
        "module M; class Outer "
        "  public class Inner "
        "    public x: int; "
        "  end Inner; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* outer = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    if (outer && !outer->members.empty()) {
        const auto* nt =
            dynamic_cast<const bezel::sema::NestedTypeSymbol*>(
                outer->members[0]);
        BEZEL_EXPECT_TRUE(nt != nullptr);
        if (nt && nt->type) {
            BEZEL_EXPECT_EQ(
                std::string(nt->type->name), std::string("Inner"));
            BEZEL_EXPECT_EQ(nt->type->members.size(), std::size_t(1));
        }
    }
}

BEZEL_TEST(sema_symbols, extension_is_registered) {
    auto r = parseAndCollect(
        "module M; public extension string "
        "  public method Greet(): string => \"hi\"; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.ctx->module()->children.size(), std::size_t(1));
    BEZEL_EXPECT_EQ(
        symbolSummary(*r.ctx->module()->children[0]),
        std::string("extension"));
}

BEZEL_TEST(sema_symbols, extern_block_with_decls) {
    auto r = parseAndCollect(
        "module M; extern \"libnet\" convention \"cdecl\" "
        "  method open(p: string): int; "
        "  method close(fd: int): bool; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* xb = dynamic_cast<const bezel::sema::ExternBlockSymbol*>(
        r.ctx->module()->children[0]);
    BEZEL_EXPECT_TRUE(xb != nullptr);
    if (xb) {
        BEZEL_EXPECT_EQ(xb->decls.size(), std::size_t(2));
        BEZEL_EXPECT_EQ(
            symbolSummary(*xb->decls[0]),
            std::string("extern-decl open"));
        BEZEL_EXPECT_EQ(
            symbolSummary(*xb->decls[1]),
            std::string("extern-decl close"));
    }
}

BEZEL_TEST(sema_symbols, scope_ids_are_nested_in_dfs_order) {
    // The contract: a nested-deeper scope always has a larger ScopeId
    // than its enclosing scope. We don't assert exact IDs (that would
    // overspecify allocation order) — only the strict-nesting property.
    auto r = parseAndCollect(
        "module M; class Outer "
        "  public method M() end M; "
        "  public class Inner "
        "    public method N() end N; "
        "  end Inner; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* outer = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    if (outer && outer->members.size() == 2) {
        const auto* outerMethod =
            dynamic_cast<const bezel::sema::MethodSymbol*>(
                outer->members[0]);
        const auto* nt =
            dynamic_cast<const bezel::sema::NestedTypeSymbol*>(
                outer->members[1]);
        if (outerMethod && nt && nt->type && !nt->type->members.empty()) {
            const auto* innerMethod =
                dynamic_cast<const bezel::sema::MethodSymbol*>(
                    nt->type->members[0]);
            // The inner class's scope > outer class's scope > module.
            BEZEL_EXPECT_TRUE(nt->type->scope > bezel::sema::kModuleScope);
            // Both methods have their own scopes (params/locals).
            if (innerMethod) {
                BEZEL_EXPECT_TRUE(
                    innerMethod->scope > outerMethod->scope);
            }
        }
    }
}

BEZEL_TEST(sema_symbols, missing_module_decl_is_diagnosed) {
    auto r = parseAndCollect("class C end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_symbols, kitchen_sink_has_no_diagnostics) {
    // Smoke test: a multi-feature source produces no sema-collection
    // diagnostics. Detailed assertions live in the per-feature tests
    // above; this one just guards against regression.
    const std::string src = R"BEZEL(
module Sink;

import System.Collections.Generic;

type Map<K, V> = IDictionary<K, V>;

public enum Color
  Red, Green, Blue;
  public method ToHex(): string => "#";
end Color;

public interface IClient
  method Connect(): bool;
end IClient;

public record Point(x: int, y: int);

public class Service<T>
  private _id: int;
  public property Name: string => "x";
  public constructor(id: int) end constructor;
  public method Process(): bool => true;
  public static method operator =(a: Service<T>, b: Service<T>): bool => true;
end Service;

public extension string
  public method IsEmpty(): bool => true;
end;
)BEZEL";
    auto r = parseAndCollect(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Module + 6 top-level decls + lots of members + ... at least 10 syms.
    BEZEL_EXPECT_TRUE(r.ctx->symbols().size() > std::size_t(10));
}
