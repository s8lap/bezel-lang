// Tests for issue #135 — the parser populates AST node spans
// (previously every node had a default {0,0,0} span, so all sema/type
// diagnostics reported location 0:0). Statements, top-level
// expressions, and leaf primary expressions should carry a 1-based
// span begin line.

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

const bezel::ast::MethodDecl* firstMethod(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    for (const auto& m : td->members) {
        if (auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get()))
            return md;
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(node_spans, statement_span_populated) {
    auto r = parseAll(
        "module M; class C public method F() var x := 1; end F; end C;");
    auto* md = firstMethod(r);
    BEZEL_EXPECT_TRUE(md != nullptr);
    if (md && !md->stmts.empty() && md->stmts[0]) {
        BEZEL_EXPECT_TRUE(md->stmts[0]->span.begin.line > 0);
    }
}

BEZEL_TEST(node_spans, expression_span_populated) {
    auto r = parseAll(
        "module M; class C public method F() _ := a + b; end F; end C;");
    auto* md = firstMethod(r);
    if (md && !md->stmts.empty()) {
        const auto* ds =
            dynamic_cast<const bezel::ast::DiscardStmt*>(md->stmts[0].get());
        BEZEL_EXPECT_TRUE(ds != nullptr);
        if (ds && ds->value) {
            BEZEL_EXPECT_TRUE(ds->value->span.begin.line > 0);
        }
    }
}

BEZEL_TEST(node_spans, leaf_identifier_span_populated) {
    auto r = parseAll(
        "module M; class C public method F() _ := x; end F; end C;");
    auto* md = firstMethod(r);
    if (md && !md->stmts.empty()) {
        const auto* ds =
            dynamic_cast<const bezel::ast::DiscardStmt*>(md->stmts[0].get());
        if (ds && ds->value) {
            const auto* id = dynamic_cast<const bezel::ast::IdentifierExpr*>(
                ds->value.get());
            BEZEL_EXPECT_TRUE(id != nullptr);
            if (id) BEZEL_EXPECT_TRUE(id->span.begin.line > 0);
        }
    }
}

BEZEL_TEST(node_spans, span_column_reflects_position) {
    // The second statement starts further right / lower than the first;
    // its span should differ from the first's.
    auto r = parseAll(
        "module M; class C public method F() "
        "var a := 1; var b := 2; end F; end C;");
    auto* md = firstMethod(r);
    if (md && md->stmts.size() >= 2 && md->stmts[0] && md->stmts[1]) {
        bool differ =
            md->stmts[0]->span.begin.offset != md->stmts[1]->span.begin.offset;
        BEZEL_EXPECT_TRUE(differ);
    }
}
