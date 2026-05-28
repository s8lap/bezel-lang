// Spec-derived parser tests for SerializedClassClause — §A.3
//
//   ClassDecl ::= Visibility? Modifier* "class" Identifier GenericParams?
//                 PrimaryCtorParams? SerializedClassClause?
//                 Inheritance? GenericConstraints* ClassBody
//   SerializedClassClause ::= "serialized" ("tree"|"graph"|"cyclic")
//                             PolymorphicClause?
//   PolymorphicClause ::= "polymorphic" "(" PolyCase ("," PolyCase)* ")"
//   PolyCase ::= StringLiteral ":" TypeName
//
// All four word-like tokens here (`serialized`, `tree`/`graph`/`cyclic`,
// `polymorphic`) are contextual keywords lexed as Identifier; the parser
// dispatches by text. Slice 42 admits the clause on Record as well as
// Class — the grammar only puts it on ClassDecl, but the syntactic
// position is the same; sema enforces kind-specific restrictions.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

BEZEL_TEST(parser_serialized_class, tree_shape_on_class) {
    auto r = parseAll(
        "module M; public class C serialized tree end C;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(t[0], std::string("public class C serialized tree"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized_class, graph_shape_on_record) {
    auto r = parseAll(
        "module M; public record R(x: int) serialized graph;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(
            t[0],
            std::string("public record R(x: int) serialized graph"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized_class, cyclic_shape_on_class) {
    auto r = parseAll(
        "module M; public class Graph serialized cyclic end Graph;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(
            t[0],
            std::string("public class Graph serialized cyclic"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized_class, with_inheritance) {
    auto r = parseAll(
        "module M; public class C serialized tree: IFoo end C;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(
            t[0],
            std::string("public class C serialized tree: IFoo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized_class, polymorphic_clause) {
    auto r = parseAll(
        "module M; public class Shape serialized graph "
        "  polymorphic(\"circle\": Circle, \"square\": Square) "
        "end Shape;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(
            t[0],
            std::string(
                "public class Shape serialized graph "
                "polymorphic(\"circle\": Circle, \"square\": Square)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized_class, polymorphic_with_inheritance) {
    auto r = parseAll(
        "module M; public class Shape serialized graph "
        "  polymorphic(\"circle\": Circle): IDrawable "
        "end Shape;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized_class, invalid_shape_word_diagnosed) {
    // `serialized weird` is invalid — `weird` is none of tree/graph/cyclic.
    // But the parser must still not blow up; it should report and recover.
    // (We rely on the disambiguation: only tree/graph/cyclic engages this
    // path. Anything else after `serialized` falls through to the field-
    // level SerializedClause path or to subsequent parsing.)
    auto r = parseAll(
        "module M; public class C serialized weird end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized_class, no_clause_still_works) {
    // Regression: declarations without the clause must continue to parse
    // unchanged (no extra modifier picked up accidentally).
    auto r = parseAll(
        "module M; public class C end C;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(t[0], std::string("public class C"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
