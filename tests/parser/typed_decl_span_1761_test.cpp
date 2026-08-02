// Regression pin for issue #1761 — the parser populates the AST span on
// top-level `TypeDecl` (class / interface / record / enum / choice) and
// `sema::collectTypeDecl` then carries it onto the resulting
// `sema::TypeSymbol::declLoc`. Pre-fix, the type's `span.begin` was the
// default {0,0,0} (the parser never stamped it), so EVERY diagnostic keyed
// on `t->declLoc` reported the location as `0:0` — making it impossible for
// a user to jump to the offending declaration in an editor. The most visible
// victim was the §12.5 abstract-member-not-implemented error:
//
//     class Impl : IFoo end Impl;   // missing `Bar()`
//       → repro.bzl:0:0: concrete class `Impl` does not implement ...
//
// Post-fix, both the AST `TypeDecl::span` and the symbol's `declLoc` carry
// the source location of the type keyword (`class` / `interface` / etc.), and
// the diagnostic correctly reports `:line:col: concrete class ...`. The same
// machinery covers interface, record, enum, and choice top-level decls (one
// assertion per kind below). Module-scope (`module X;`) and method/field
// spans were already populated by the §1 lexer's #135 stampSpan wrappers;
// this test pins the top-level type-decl half of #1761.

#include "support/parse_support.hpp"
#include "test_harness.hpp"

#include "sema/SymbolCollection.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

using bezel::test::parseAll;

namespace {

const bezel::ast::TypeDecl* firstTypeDecl(const bezel::test::ParseResult& r,
                                          std::size_t idx = 0) {
    if (!r.sourceFile) return nullptr;
    std::size_t seen = 0;
    for (const auto& td : r.sourceFile->topDecls) {
        if (auto* d = dynamic_cast<const bezel::ast::TypeDecl*>(td.get())) {
            if (seen == idx) return d;
            ++seen;
        }
    }
    return nullptr;
}

}  // namespace

// #1761-1: a class top-level decl's span.begin is non-zero (the parser
// stamps it at the `class` keyword location). This is the half of the bug
// that lives in Parser_Decls.cpp's parseTypeOrInterfaceDecl.
BEZEL_TEST(typed_decl_span_1761, class_type_decl_span_populated) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(): int return 0; end M; "
        "end C;");
    auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td) BEZEL_EXPECT_TRUE(td->span.begin.line > 0);
}

// #1761-2: an interface top-level decl's span.begin is non-zero.
BEZEL_TEST(typed_decl_span_1761, interface_type_decl_span_populated) {
    auto r = parseAll(
        "module M; "
        "interface IFoo "
        "  method Bar(); "
        "end IFoo;");
    auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td) {
        // The exact column convention is owned by the lexer (§1 / #135);
        // we just pin "non-zero and on the kind-keyword's line". The
        // pre-fix value was {0,0,0}; post-fix it is {2, c, o} for the
        // `interface` keyword at column 1 of line 2.
        BEZEL_EXPECT_TRUE(td->span.begin.line > 0);
        BEZEL_EXPECT_TRUE(td->span.begin.column > 0);
    }
}

// #1761-3: a record top-level decl's span.begin is non-zero.
BEZEL_TEST(typed_decl_span_1761, record_type_decl_span_populated) {
    auto r = parseAll("module M; record R(int X, int Y) end R;");
    auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td) BEZEL_EXPECT_TRUE(td->span.begin.line > 0);
}

// #1761-4: an enum top-level decl's span.begin is non-zero.
BEZEL_TEST(typed_decl_span_1761, enum_type_decl_span_populated) {
    auto r = parseAll("module M; enum E A, B, C end E;");
    auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td) BEZEL_EXPECT_TRUE(td->span.begin.line > 0);
}

// #1761-5: a choice top-level decl's span.begin is non-zero.
BEZEL_TEST(typed_decl_span_1761, choice_type_decl_span_populated) {
    auto r = parseAll("module M; choice C A, B, C; end C;");
    auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (td) BEZEL_EXPECT_TRUE(td->span.begin.line > 0);
}

// #1761-6: the symbol-side half. After `collectSymbols`, the
// `TypeSymbol::declLoc` reflects the type's `span.begin` (the parser-side
// fix alone is not enough — sema's `collectTypeDecl` must carry the
// location onto the symbol that diagnostics consult). This pin guarantees
// the symbol-side wiring stays in place across future refactors.
BEZEL_TEST(typed_decl_span_1761, type_symbol_decl_loc_populated) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(): int return 0; end M; "
        "end C;");
    auto* td = firstTypeDecl(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (!td) return;

    bezel::sema::SemaContext ctx(r.diagnostics);
    if (!bezel::sema::collectSymbols(*r.sourceFile, ctx)) {
        // Diagnostics may have been raised for other reasons; the symbol-
        // side assertion below doesn't depend on a clean compile.
    }
    bool found = false;
    for (const auto& sym : ctx.symbols()) {
        auto* ts = dynamic_cast<const bezel::sema::TypeSymbol*>(sym.get());
        if (ts && ts->decl == td) {
            // The key pin: the symbol's declLoc must equal the AST's
            // span.begin (NOT the default {0,0,0}). The 0:0 of the
            // pre-fix world is exactly the bug #1761 names.
            BEZEL_EXPECT_EQ(ts->declLoc.line, td->span.begin.line);
            BEZEL_EXPECT_EQ(ts->declLoc.column, td->span.begin.column);
            found = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}
