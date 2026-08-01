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
    auto r = parseAndCollect("module M; type Map<K, V> := IDictionary<K, V>;");
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

type Map<K, V> := IDictionary<K, V>;

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

// fix #2418: `Symbol::declLoc` was populated in exactly one place across all
// of src/sema/ — the TypeSymbol branch of collectTypeDecl
// (SymbolCollection.cpp, "sym->declLoc = td.span.begin") — so every OTHER
// member kind (FieldSymbol, MethodSymbol, ConstructorSymbol,
// DestructorSymbol, PropertySymbol, EventSymbol, ConstMemberSymbol,
// NestedTypeSymbol) read the SourceLocation default {line:0, column:0,
// offset:0} (support/SourceLocation.hpp) no matter where in the source the
// member was actually declared. The fix has two parts: (1) collectMember
// (SymbolCollection.cpp) now stamps `sym->declLoc` from the member's own
// ast::Member-derived decl node's `span.begin`; (2) several ast::Member
// subtypes (FieldDecl/PropertyDecl/EventDecl/ConstDecl/OperatorDecl/
// NestedTypeMember, plus every interface-member kind) never had their
// `span` populated by the parser at all — Parser_Decls.cpp now stamps it,
// mirroring the ConstructorDecl/DestructorDecl/(class-body)MethodDecl
// convention that already existed (#662c/#783). Without EITHER half, this
// test fails: without (1), every assertion below sees line 0; without (2)
// alone, (1) would just copy a still-unpopulated span (another 0).
//
// One member of each kind, each on its own source line, so declLoc.line is
// a precise, unambiguous pin (1-based per SourceLocation's contract).
BEZEL_TEST(sema_symbols, member_decl_locations_are_populated_2418) {
    const std::string src =
        "module M;\n"                                              // 1
        "class C\n"                                                 // 2
        "  public x: int;\n"                                        // 3  FieldDecl
        "  public method Meth(): int => 42;\n"                      // 4  MethodDecl
        "  public constructor() end constructor;\n"                 // 5  ConstructorDecl
        "  destructor end destructor;\n"                            // 6  DestructorDecl
        "  public property Prop: int => 1;\n"                       // 7  PropertyDecl
        "  public event Click;\n"                                   // 8  EventDecl
        "  public const K := 1;\n"                                  // 9  ConstDecl
        "  public static method operator +(a: C, b: C): C => a;\n"  // 10 OperatorDecl
        "  public class Nested end Nested;\n"                       // 11 NestedTypeMember
        "end C;\n";
    auto r = parseAndCollect(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* c = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    BEZEL_EXPECT_TRUE(c != nullptr);
    if (!c) return;
    BEZEL_EXPECT_EQ(c->members.size(), std::size_t(9));
    if (c->members.size() != 9) return;

    auto* field = dynamic_cast<const bezel::sema::FieldSymbol*>(c->members[0]);
    BEZEL_EXPECT_TRUE(field != nullptr);
    if (field) {
        BEZEL_EXPECT_EQ(field->declLoc.line, std::uint32_t(3));
        // Column pins the span begins at the FIRST member token ("public"),
        // not e.g. the field name — 2 leading spaces -> column 3.
        BEZEL_EXPECT_EQ(field->declLoc.column, std::uint32_t(3));
    }
    auto* meth = dynamic_cast<const bezel::sema::MethodSymbol*>(c->members[1]);
    BEZEL_EXPECT_TRUE(meth != nullptr);
    if (meth) BEZEL_EXPECT_EQ(meth->declLoc.line, std::uint32_t(4));

    auto* ctor =
        dynamic_cast<const bezel::sema::ConstructorSymbol*>(c->members[2]);
    BEZEL_EXPECT_TRUE(ctor != nullptr);
    if (ctor) BEZEL_EXPECT_EQ(ctor->declLoc.line, std::uint32_t(5));

    auto* dtor =
        dynamic_cast<const bezel::sema::DestructorSymbol*>(c->members[3]);
    BEZEL_EXPECT_TRUE(dtor != nullptr);
    if (dtor) BEZEL_EXPECT_EQ(dtor->declLoc.line, std::uint32_t(6));

    auto* prop = dynamic_cast<const bezel::sema::PropertySymbol*>(c->members[4]);
    BEZEL_EXPECT_TRUE(prop != nullptr);
    if (prop) BEZEL_EXPECT_EQ(prop->declLoc.line, std::uint32_t(7));

    auto* evt = dynamic_cast<const bezel::sema::EventSymbol*>(c->members[5]);
    BEZEL_EXPECT_TRUE(evt != nullptr);
    if (evt) BEZEL_EXPECT_EQ(evt->declLoc.line, std::uint32_t(8));

    auto* konst =
        dynamic_cast<const bezel::sema::ConstMemberSymbol*>(c->members[6]);
    BEZEL_EXPECT_TRUE(konst != nullptr);
    if (konst) BEZEL_EXPECT_EQ(konst->declLoc.line, std::uint32_t(9));

    // Operators reuse MethodSymbol (isOperator=true) — see collectMember.
    auto* op = dynamic_cast<const bezel::sema::MethodSymbol*>(c->members[7]);
    BEZEL_EXPECT_TRUE(op != nullptr);
    if (op) {
        BEZEL_EXPECT_TRUE(op->isOperator);
        BEZEL_EXPECT_EQ(op->declLoc.line, std::uint32_t(10));
    }

    auto* nested =
        dynamic_cast<const bezel::sema::NestedTypeSymbol*>(c->members[8]);
    BEZEL_EXPECT_TRUE(nested != nullptr);
    if (nested) {
        // The wrapper Member node's own declLoc (NestedTypeMember::span,
        // previously never stamped at all)...
        BEZEL_EXPECT_EQ(nested->declLoc.line, std::uint32_t(11));
        // ...and the wrapped TypeSymbol's declLoc (already worked pre-#2418
        // via the #1761 TypeSymbol precedent — pinned here so a future
        // regression in either half is caught).
        if (nested->type) {
            BEZEL_EXPECT_EQ(nested->type->declLoc.line, std::uint32_t(11));
        }
    }
}

// fix #2418: interface members (IfaceProperty/IfaceEvent/IfaceMethod/
// IfaceOperator, parsed by Parser::parseInterfaceMember) are a completely
// separate parser code path from class-body members and never had ANY span
// stamped — this pins that the companion parser fix covers them too, not
// just the class-body forms exercised above.
BEZEL_TEST(sema_symbols, interface_member_decl_locations_are_populated_2418) {
    const std::string src =
        "module M;\n"                                                  // 1
        "interface I\n"                                                 // 2
        "  property Prop: int;\n"                                       // 3  IfaceProperty
        "  event Click;\n"                                              // 4  IfaceEvent
        "  method Meth(): int;\n"                                       // 5  IfaceMethod
        "  static operator +(a: I, b: I): I;\n"                         // 6  IfaceOperator
        "end I;\n";
    auto r = parseAndCollect(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* iface = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    BEZEL_EXPECT_TRUE(iface != nullptr);
    if (!iface) return;
    BEZEL_EXPECT_EQ(iface->members.size(), std::size_t(4));
    if (iface->members.size() != 4) return;

    auto* prop =
        dynamic_cast<const bezel::sema::PropertySymbol*>(iface->members[0]);
    BEZEL_EXPECT_TRUE(prop != nullptr);
    if (prop) BEZEL_EXPECT_EQ(prop->declLoc.line, std::uint32_t(3));

    auto* evt = dynamic_cast<const bezel::sema::EventSymbol*>(iface->members[1]);
    BEZEL_EXPECT_TRUE(evt != nullptr);
    if (evt) BEZEL_EXPECT_EQ(evt->declLoc.line, std::uint32_t(4));

    auto* meth = dynamic_cast<const bezel::sema::MethodSymbol*>(iface->members[2]);
    BEZEL_EXPECT_TRUE(meth != nullptr);
    if (meth) BEZEL_EXPECT_EQ(meth->declLoc.line, std::uint32_t(5));

    auto* op = dynamic_cast<const bezel::sema::MethodSymbol*>(iface->members[3]);
    BEZEL_EXPECT_TRUE(op != nullptr);
    if (op) BEZEL_EXPECT_EQ(op->declLoc.line, std::uint32_t(6));
}

// fix #2418: the sibling audit — top-level symbol kinds constructed outside
// collectMember (TypeAliasSymbol/ExtensionSymbol/ExternBlockSymbol in
// collectTopDecl) had the identical gap and are fixed the same way, sourced
// from their own ast::TopDecl-derived node's span.
BEZEL_TEST(sema_symbols, toplevel_decl_locations_are_populated_2418) {
    const std::string src =
        "module M;\n"                                    // 1
        "type Alias := int;\n"                            // 2
        "public extension string\n"                       // 3
        "  public method Greet(): string => \"hi\";\n"     // 4
        "end;\n"                                           // 5
        "extern \"libc\"\n"                                // 6
        "  method open(p: string): int;\n"                 // 7
        "end;\n";
    auto r = parseAndCollect(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.ctx->module()->children.size(), std::size_t(3));
    if (r.ctx->module()->children.size() != 3) return;

    auto* alias = dynamic_cast<const bezel::sema::TypeAliasSymbol*>(
        r.ctx->module()->children[0]);
    BEZEL_EXPECT_TRUE(alias != nullptr);
    if (alias) BEZEL_EXPECT_EQ(alias->declLoc.line, std::uint32_t(2));

    auto* ext = dynamic_cast<const bezel::sema::ExtensionSymbol*>(
        r.ctx->module()->children[1]);
    BEZEL_EXPECT_TRUE(ext != nullptr);
    if (ext) BEZEL_EXPECT_EQ(ext->declLoc.line, std::uint32_t(3));

    auto* xb = dynamic_cast<const bezel::sema::ExternBlockSymbol*>(
        r.ctx->module()->children[2]);
    BEZEL_EXPECT_TRUE(xb != nullptr);
    if (xb) {
        BEZEL_EXPECT_EQ(xb->declLoc.line, std::uint32_t(6));
        // fix #2482: ExternDecl now carries its own source location (stamped
        // at the decl's first token) — points at the method's own line (7),
        // not the enclosing block's (6). Pre-#2482 this fell back to the
        // block's location because ast::ExternDecl is not an ast::Node.
        BEZEL_EXPECT_EQ(xb->decls.size(), std::size_t(1));
        if (xb->decls.size() == 1) {
            BEZEL_EXPECT_EQ(xb->decls[0]->declLoc.line, std::uint32_t(7));
        }
    }
}

// fix #2482 (follow-up to #2418): positional record fields (§9.1), enum
// constants (§10), choice cases (§35.1), and extern decls (§32) are the four
// member kinds whose AST node is NOT an ast::Node (ast::Param /
// ast::EnumConstant / ast::ChoiceCase / ast::ExternDecl carry no `span`), so
// #2418 had their member declLoc FALL BACK to the enclosing type/block's
// location — a diagnostic on one of these members cited the enclosing TYPE's
// line, not the member's own. #2482 adds a per-member SourceLocation to each
// of those structs (stamped by the parser at the member's first token) and
// reads it in SymbolCollection, so each member's declLoc is its OWN line.
//
// Each member is deliberately placed on a DIFFERENT source line than its
// enclosing type so the assertion is a real fail-without/pass-with proof:
// without the fix every member below resolves to its type's line; with it,
// to the member's own line. (Diagnostic source-location quality, §8.)
BEZEL_TEST(sema_symbols, member_kind_decl_locations_point_at_own_line_2482) {
    const std::string src =
        "module M;\n"                                  // 1
        "public record R(\n"                           // 2  TypeDecl
        "  x: int);\n"                                 // 3  positional field
        "public enum E\n"                              // 4  TypeDecl
        "  Red,\n"                                     // 5  EnumConstant
        "  Green;\n"                                   // 6  EnumConstant
        "end E;\n"                                     // 7
        "public choice Op\n"                           // 8  TypeDecl
        "  Add,\n"                                     // 9  ChoiceCase
        "  Sub;\n"                                     // 10 ChoiceCase
        "end Op;\n"                                    // 11
        "extern \"libc\"\n"                            // 12 ExternBlock
        "  method fopen(p: int): int;\n"               // 13 ExternDecl
        "end;\n";                                      // 14
    auto r = parseAndCollect(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.ctx->module()->children.size(), std::size_t(4));
    if (r.ctx->module()->children.size() != 4) return;

    // Helper: find a member of `t` by name (order-independent assertions).
    auto findMember = [](const bezel::sema::TypeSymbol* t,
                         std::string_view name) -> const bezel::sema::Symbol* {
        for (auto* m : t->members)
            if (m->name == name) return m;
        return nullptr;
    };

    // record R (line 2); positional field x on its OWN line (3).
    auto* rec = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[0]);
    BEZEL_EXPECT_TRUE(rec != nullptr);
    if (rec) {
        BEZEL_EXPECT_EQ(rec->declLoc.line, std::uint32_t(2));
        BEZEL_EXPECT_EQ(rec->members.size(), std::size_t(1));
        auto* x = dynamic_cast<const bezel::sema::FieldSymbol*>(findMember(rec, "x"));
        BEZEL_EXPECT_TRUE(x != nullptr);
        if (x) BEZEL_EXPECT_EQ(x->declLoc.line, std::uint32_t(3));
    }

    // enum E (line 4); constants Red/Green on their OWN lines (5/6).
    auto* en = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[1]);
    BEZEL_EXPECT_TRUE(en != nullptr);
    if (en) {
        BEZEL_EXPECT_EQ(en->declLoc.line, std::uint32_t(4));
        BEZEL_EXPECT_EQ(en->members.size(), std::size_t(2));
        auto* red = dynamic_cast<const bezel::sema::EnumConstantSymbol*>(
            findMember(en, "Red"));
        if (red) BEZEL_EXPECT_EQ(red->declLoc.line, std::uint32_t(5));
        auto* green = dynamic_cast<const bezel::sema::EnumConstantSymbol*>(
            findMember(en, "Green"));
        if (green) BEZEL_EXPECT_EQ(green->declLoc.line, std::uint32_t(6));
    }

    // choice Op (line 8); cases Add/Sub on their OWN lines (9/10).
    auto* op = dynamic_cast<const bezel::sema::TypeSymbol*>(
        r.ctx->module()->children[2]);
    BEZEL_EXPECT_TRUE(op != nullptr);
    if (op) {
        BEZEL_EXPECT_EQ(op->declLoc.line, std::uint32_t(8));
        BEZEL_EXPECT_EQ(op->members.size(), std::size_t(2));
        auto* add = dynamic_cast<const bezel::sema::ChoiceCaseSymbol*>(
            findMember(op, "Add"));
        if (add) BEZEL_EXPECT_EQ(add->declLoc.line, std::uint32_t(9));
        auto* sub = dynamic_cast<const bezel::sema::ChoiceCaseSymbol*>(
            findMember(op, "Sub"));
        if (sub) BEZEL_EXPECT_EQ(sub->declLoc.line, std::uint32_t(10));
    }

    // extern block (line 12); the per-method ExternDecl on its OWN line (13).
    auto* xb = dynamic_cast<const bezel::sema::ExternBlockSymbol*>(
        r.ctx->module()->children[3]);
    BEZEL_EXPECT_TRUE(xb != nullptr);
    if (xb) {
        BEZEL_EXPECT_EQ(xb->declLoc.line, std::uint32_t(12));
        BEZEL_EXPECT_EQ(xb->decls.size(), std::size_t(1));
        if (xb->decls.size() == 1) {
            BEZEL_EXPECT_EQ(xb->decls[0]->declLoc.line, std::uint32_t(13));
        }
    }
}
