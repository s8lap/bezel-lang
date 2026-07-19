// Spec-derived parser tests for `choice` declarations — §35.1 (v0.200, #2044)
// and §A.3 `ChoiceDecl` / `ChoiceCases` / `ChoiceCase` / `CasePayload` /
// `CaseField`. Mirrors the role of enum_constants_test.cpp for the new
// kind: parses source text and asserts on the rendered AST summary.
//
// The grammar productions exercised here (spec §A.3, v0.200):
//   ChoiceDecl  ::= Visibility? "choice" Identifier GenericParams?
//                   Inheritance? GenericConstraints* ChoiceCases
//                   ChoiceMember* "end" Identifier? ";"
//   ChoiceCases ::= ChoiceCase ("," ChoiceCase)* ";"
//   ChoiceCase  ::= Identifier CasePayload? SerializedClause?
//   CasePayload ::= "(" CaseField ("," CaseField)* ")"
//   CaseField   ::= "owned"? Identifier ":" Type SerializedClause?
//
// The spec's §35.1 headline example carries one payload-free case and two
// payload cases (the `FetchOutcome` example); the first test below
// transcribes that example's case list verbatim.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

// --- §35.1 declaration shape ------------------------------------------------

BEZEL_TEST(parser_choice_decl, spec_35_1_payload_free_case_only) {
    // §35.1: "a case with no payload list is payload-free." A single payload-
    // free case parses to the bare-name form.
    auto r = parseAll("module M; choice C Empty; end C;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("choice C { Empty }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_choice_decl, spec_35_1_multiple_payload_free_cases) {
    auto r = parseAll("module M; choice C Red, Green, Blue; end C;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("choice C { Red, Green, Blue }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_choice_decl, spec_35_1_fetch_outcome_shape) {
    // §35.1's headline example: one owned-class payload case, one payload-free
    // case, one multi-payload value case. (We use `int` and `string` for the
    // value-payload arm so the parse test stays free of stdlib dependencies.)
    auto r = parseAll(
        "module M; choice FetchOutcome "
        "  Fetched(owned response: int), "
        "  NotModified, "
        "  Failed(status: int, message: string); "
        "end FetchOutcome;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("choice FetchOutcome { "
                                    "Fetched(owned response: int), "
                                    "NotModified, "
                                    "Failed(status: int, message: string) }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_choice_decl, spec_35_1_case_with_single_value_payload) {
    auto r = parseAll(
        "module M; choice Opt Some(value: int), None; end Opt;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("choice Opt { "
                                    "Some(value: int), None }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §A.3 CaseField: the `owned` marker positions ---------------------------

BEZEL_TEST(parser_choice_decl, spec_35_1_owned_marker_on_payload_field) {
    // §A.3 CaseField: the leading `owned` marker is admissible per §28.1 on
    // class/interface types and bare type parameters. Here it is recorded on
    // the payload Param's `modifier` slot and round-trips through the
    // summary.
    auto r = parseAll(
        "module M; choice Tree "
        "  Leaf, "
        "  Node(owned left: int, owned right: int); "
        "end Tree;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("choice Tree { "
                                    "Leaf, "
                                    "Node(owned left: int, "
                                    "owned right: int) }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §A.3 ChoiceDecl: generic params + inheritance --------------------------

BEZEL_TEST(parser_choice_decl, spec_35_11_generic_choice) {
    // §35.11: "A choice may declare type parameters and constraints, as
    // records do." Generic params appear between the name and the inheritance
    // clause (same position as records/classes).
    auto r = parseAll(
        "module M; choice Result<T, E> Ok(value: T), Error(err: E); "
        "end Result;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("choice Result<T, E> { "
                                    "Ok(value: T), Error(err: E) }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Errors -----------------------------------------------------------------

BEZEL_TEST(parser_choice_decl, missing_semicolon_after_cases_is_error) {
    auto r = parseAll("module M; choice C A, B end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_choice_decl, missing_case_name_is_error) {
    // §35.1: "A choice declares at least one case."
    auto r = parseAll("module M; choice C ; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
