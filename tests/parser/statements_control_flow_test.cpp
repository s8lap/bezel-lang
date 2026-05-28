// Spec-derived parser tests for control-flow statements — §A.6 IfStmt,
// WhileStmt, DoEndBlock, plus the trivial transfer statements
// BreakStmt and ContinueStmt.
//
// For (§A.6 ForStmt — with `step` and range/collection iteration) and the
// async loop forms (`await for`) come later, as does TryStmt.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseBodyText(const std::string& body) {
    auto r = parseAll(std::string("module M; class Foo public method run() ") +
                      body + std::string(" end run; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public method run() ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

// --- IfStmt -----------------------------------------------------------------

BEZEL_TEST(parser_control_flow, if_then_end) {
    BEZEL_EXPECT_EQ(parseBodyText("if cond then x := 1; end;"),
                    std::string("{ if cond then { x := 1; } end; }"));
}

BEZEL_TEST(parser_control_flow, if_then_else_end) {
    BEZEL_EXPECT_EQ(
        parseBodyText("if cond then x := 1; else x := 2; end;"),
        std::string(
            "{ if cond then { x := 1; } else { x := 2; } end; }"));
}

BEZEL_TEST(parser_control_flow, if_then_elif_else_end) {
    BEZEL_EXPECT_EQ(
        parseBodyText(
            "if a then x := 1; elif b then x := 2; else x := 3; end;"),
        std::string(
            "{ if a then { x := 1; } elif b then { x := 2; } else "
            "{ x := 3; } end; }"));
}

BEZEL_TEST(parser_control_flow, if_multiple_elifs) {
    BEZEL_EXPECT_EQ(
        parseBodyText(
            "if a then x := 1; elif b then x := 2; elif c then x := 3; "
            "end;"),
        std::string(
            "{ if a then { x := 1; } elif b then { x := 2; } elif c "
            "then { x := 3; } end; }"));
}

BEZEL_TEST(parser_control_flow, if_with_empty_branches_is_permitted) {
    // Grammar `Statement*` admits zero — empty branches.
    BEZEL_EXPECT_EQ(parseBodyText("if cond then end;"),
                    std::string("{ if cond then {  } end; }"));
}

BEZEL_TEST(parser_control_flow, nested_if) {
    BEZEL_EXPECT_EQ(
        parseBodyText("if a then if b then x := 1; end; end;"),
        std::string(
            "{ if a then { if b then { x := 1; } end; } end; }"));
}

// --- WhileStmt --------------------------------------------------------------

BEZEL_TEST(parser_control_flow, while_simple) {
    BEZEL_EXPECT_EQ(parseBodyText("while cond do x := x + 1; end;"),
                    std::string(
                        "{ while cond do { x := (x + 1); } end; }"));
}

BEZEL_TEST(parser_control_flow, while_with_complex_condition) {
    BEZEL_EXPECT_EQ(parseBodyText("while a and b do x := 0; end;"),
                    std::string(
                        "{ while (a and b) do { x := 0; } end; }"));
}

// --- DoEndBlock -------------------------------------------------------------

BEZEL_TEST(parser_control_flow, do_end_block) {
    BEZEL_EXPECT_EQ(parseBodyText("do x := 1; x := x + 1; end;"),
                    std::string(
                        "{ do { x := 1; x := (x + 1); } end; }"));
}

// --- Break / Continue -------------------------------------------------------

BEZEL_TEST(parser_control_flow, break_inside_while) {
    BEZEL_EXPECT_EQ(
        parseBodyText("while cond do if done then break; end; end;"),
        std::string(
            "{ while cond do { if done then { break; } end; } end; }"));
}

BEZEL_TEST(parser_control_flow, continue_inside_while) {
    BEZEL_EXPECT_EQ(
        parseBodyText("while cond do if skip then continue; end; end;"),
        std::string(
            "{ while cond do { if skip then { continue; } end; } end; }"));
}

// --- Error cases ------------------------------------------------------------

BEZEL_TEST(parser_control_flow, if_missing_then_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "if cond x := 1; end; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_control_flow, while_missing_end_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "while cond do x := 1; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_control_flow, while_missing_do_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "while cond x := 1; end; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_control_flow, break_outside_of_anything_still_parses) {
    // The parser accepts `break;` anywhere syntactically; sema enforces it
    // only inside loop/match/etc.
    BEZEL_EXPECT_EQ(parseBodyText("break;"), std::string("{ break; }"));
}
