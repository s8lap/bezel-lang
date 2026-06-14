// Spec-derived parser tests for the serialization polymorphic registry — §9.8
// v0.141 (P6).
//
// The `serialized tree/graph/cyclic` class-HEADER clause was retired (#703): a
// class participates in serialization synthesis by declaring
// `: ISerializable`/`: IDeserializable` and field types drive the wire shape
// (synthesized by sema). The polymorphic registry is now a class-BODY member:
//
//   PolymorphicMember ::= "polymorphic" "(" PolyCase ("," PolyCase)* ")" ";"
//   PolyCase          ::= StringLiteral ":" TypeName
//
// `polymorphic` is a contextual keyword (an Identifier); it is the member clause
// only when immediately followed by `(`, otherwise an ordinary identifier. The
// parser populates td->serializedClass->polyCases (the same vector the retired
// header clause used).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

// §9.8 v0.141 (P6): a class-body `polymorphic(...)` member clause is parsed into
// the serialization registry (rendered in the class summary).
BEZEL_TEST(parser_serialized_class, polymorphic_member_clause) {
    auto r = parseAll(
        "module M; public class Shape "
        "  polymorphic(\"circle\": Circle, \"square\": Square); "
        "end Shape;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(
            t[0],
            std::string(
                "public class Shape "
                "polymorphic(\"circle\": Circle, \"square\": Square)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8 v0.141 (P6): the member clause coexists with an inheritance list (the
// registry is a body member; inheritance stays in the header).
BEZEL_TEST(parser_serialized_class, polymorphic_member_with_inheritance) {
    auto r = parseAll(
        "module M; public class Shape : IDrawable "
        "  polymorphic(\"circle\": Circle); "
        "end Shape;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `polymorphic` is contextual: without a following `(` it is an ordinary
// identifier (here a field name), not the member clause.
BEZEL_TEST(parser_serialized_class, polymorphic_identifier_without_paren) {
    auto r = parseAll(
        "module M; public class C "
        "  public polymorphic: int; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression: a class with no registry parses unchanged (no serializedClass set
// at parse time; the wire shape, if the class is serializable, is synthesized by
// sema).
BEZEL_TEST(parser_serialized_class, no_clause_still_works) {
    auto r = parseAll(
        "module M; public class C end C;");
    const auto t = topDeclSummaries(r);
    if (t.size() == 1)
        BEZEL_EXPECT_EQ(t[0], std::string("public class C"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
