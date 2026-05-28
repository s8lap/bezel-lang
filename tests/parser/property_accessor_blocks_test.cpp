// Spec-derived parser tests for the PropertyBody accessor-block form —
// §A.4:
//
//   PropertyBody    ::= ... | Accessor* "end" Identifier? ";"
//   Accessor        ::= Visibility? "get" AccessorBody
//                     | Visibility? "set" SetterParamSpec?
//                       GenericConstraints? AccessorBody
//   SetterParamSpec ::= "(" ParamMod? Identifier? ")"
//   AccessorBody    ::= "=>" (Expression | "do" Statement* "end") ";"
//                     | ";"  /* abstract / interface body */
//
// Per-accessor visibility, abstract accessors, expression-body / block-body
// accessors, and the SetterParamSpec variants are covered here.
// AutoAccessorVis (`AutoAccessorVis? (":=" Expr)?` on the auto-property
// form), the SerializedClause on auto-properties, and GenericConstraints on
// `set` are still deferred.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_property_accessor, getter_only_expression_body) {
    auto r = parseAll(
        "module M; class C "
        "public property X: int get => 0; end; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: int { get => 0; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, get_and_set_expression_bodies) {
    // AccessorBody's `=> EXPR ;` form takes an Expression — for a setter,
    // the typical pattern is a side-effecting call. The statement-form
    // `storage := value;` requires the `=> do ... end ;` block form
    // (exercised in `block_body_accessor` below).
    auto r = parseAll(
        "module M; class C "
        "private storage: int; "
        "public property X: int "
        "  get => storage; "
        "  set => Update(value); "
        "end X; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 2)
        BEZEL_EXPECT_EQ(
            ms[1],
            std::string(
                "public property X: int { get => storage; "
                "set => Update(value); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, per_accessor_visibility_private_set) {
    auto r = parseAll(
        "module M; class C "
        "public property X: int "
        "  get => 0; "
        "  private set => Update(value); "
        "end X; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: int { get => 0; "
                "private set => Update(value); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, abstract_accessors) {
    // `;` body — abstract / interface form. Still valid on a class
    // PropertyDecl when the class is abstract; sema enforces consistency.
    auto r = parseAll(
        "module M; class C "
        "public property X: int get; set; end X; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: int { get; set; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, block_body_accessor) {
    // `get => do ... end ;` AccessorBody alternative.
    auto r = parseAll(
        "module M; class C "
        "public property X: int "
        "  get => do return 0; end; "
        "end X; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: int { get => do { return 0; } end; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, setter_with_param_spec_owned) {
    // SetterParamSpec carrying the owned modifier — typical for owned-element
    // collection-indexer setters per the §A.4 note. Tested here on a plain
    // property; indexers come below.
    auto r = parseAll(
        "module M; class C "
        "public property X: Resource "
        "  set(owned newVal) => Take(newVal); "
        "end X; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: Resource "
                "{ set(owned newVal) => Take(newVal); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, setter_with_modifier_only) {
    // `set(owned)` — modifier with implicit `value` name per the §A.4 note.
    auto r = parseAll(
        "module M; class C "
        "public property X: Resource "
        "  set(owned) => Take(value); "
        "end X; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: Resource "
                "{ set(owned) => Take(value); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, indexer_accessor_block) {
    // PropertyBody reuse on indexer per A.4 IndexerDecl. The optional
    // `Identifier` after `end` is not valid for indexers per the spec note;
    // we only assert plain `end;`.
    auto r = parseAll(
        "module M; class C "
        "public property[i: int]: int "
        "  get => 0; "
        "  set => Update(i, value); "
        "end; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property [i: int]: int "
                "{ get => 0; set => Update(i, value); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, end_label_matches_property_name) {
    // §1.6 keyword-labelled construct — accessor block accepts `end NAME ;`.
    // Cross-name match check is sema's job; the parser only records what
    // was written. Here we exercise the label and check that parsing
    // succeeds.
    auto r = parseAll(
        "module M; class C "
        "public property X: int get => 0; end X; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, auto_property_still_works) {
    // Regression: with the new dispatch, the plain auto-property form
    // (`;` immediately after type or `:= EXPR ;`) must still parse.
    auto r = parseAll(
        "module M; class C "
        "public property X: int := 42; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("public property X: int := 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_property_accessor, expression_body_still_works) {
    // Regression: `=> EXPR ;` continues to take the expression-body path,
    // not the accessor-block path.
    auto r = parseAll(
        "module M; class C "
        "public property X: int => 0; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("public property X: int => 0"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
