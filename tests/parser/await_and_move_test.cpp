// Spec-derived parser tests for `await` (§17.1 L2 unary prefix) and `move`
// (§A.7 / §28.6 primary-level prefix on owned fields).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseInitText(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public x: int := ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

// --- await (§17.1 L2) ------------------------------------------------------

BEZEL_TEST(parser_await_move, await_call) {
    BEZEL_EXPECT_EQ(parseInitText("await f()"),
                    std::string("(await f())"));
}

// §17.2 example: `await x.y` parses as `await (x.y)` — primary binds tighter
// than await.
BEZEL_TEST(parser_await_move, await_binds_looser_than_primary) {
    BEZEL_EXPECT_EQ(parseInitText("await x.y"),
                    std::string("(await x.y)"));
}

// §17.2: `await x + 1` parses as `(await x) + 1` — await is unary-level,
// additive `+` is looser.
BEZEL_TEST(parser_await_move, await_tighter_than_additive) {
    BEZEL_EXPECT_EQ(parseInitText("await x + 1"),
                    std::string("((await x) + 1)"));
}

// §17.2: `await -x` parses as `await (-x)` — both at level 2,
// right-recursive.
BEZEL_TEST(parser_await_move, await_chains_with_other_unary) {
    BEZEL_EXPECT_EQ(parseInitText("await -x"),
                    std::string("(await (-x))"));
}

// §17.2: `await await x` is permitted (awaiting Task<Task<T>>).
BEZEL_TEST(parser_await_move, double_await) {
    BEZEL_EXPECT_EQ(parseInitText("await await x"),
                    std::string("(await (await x))"));
}

// --- move (§A.7 / §28.6) ---------------------------------------------------

BEZEL_TEST(parser_await_move, move_identifier) {
    BEZEL_EXPECT_EQ(parseInitText("move conn"),
                    std::string("(move conn)"));
}

BEZEL_TEST(parser_await_move, move_field) {
    BEZEL_EXPECT_EQ(parseInitText("move this.conn"),
                    std::string("(move this.conn)"));
}

// --- the replacing `move` (§28.8.1 *The replacing `move`*, A.7 MoveExpr, v0.350;
//     issue #7567) ----------------------------------------------------------
//
// A.7: `MoveExpr ::= "move" PrimaryExpr ("with" Expression)?`. Before the fix the
// `with` after the operand was read as the record-update suffix and failed with
// the retired-spelling diagnostic, so none of these parsed.

// §28.8.1 spec example `Replace`: the implicit-`this` field spelling.
BEZEL_TEST(parser_await_move, replacing_move_bare_field) {
    BEZEL_EXPECT_EQ(parseInitText("move _resource with new FileStream(_path)"),
                    std::string("(move _resource with new FileStream(_path))"));
}

// §28.8.1: the qualified field spelling, `move obj.f with e`.
BEZEL_TEST(parser_await_move, replacing_move_qualified_field) {
    BEZEL_EXPECT_EQ(parseInitText("move h.F with null"),
                    std::string("(move h.F with null)"));
}

// §28.8.1: the owning-collection slot spelling, `move coll[i] with e`.
BEZEL_TEST(parser_await_move, replacing_move_slot) {
    BEZEL_EXPECT_EQ(parseInitText("move arr[i] with fresh"),
                    std::string("(move arr[i] with fresh)"));
}

// A.7 MoveExpr: a trailing `!` on the operand is transparent, and the
// replacement is a full Expression (here a binary expression).
BEZEL_TEST(parser_await_move, replacing_move_bang_operand_and_full_expression) {
    BEZEL_EXPECT_EQ(parseInitText("move x.f! with a + b"),
                    std::string("(move x.f! with (a + b))"));
}

// A bare `move` still takes no clause (no regression).
BEZEL_TEST(parser_await_move, bare_move_has_no_replacement) {
    BEZEL_EXPECT_EQ(parseInitText("move h.F"), std::string("(move h.F)"));
}

// §9.5's tie-break: `with` followed by `(` and `Identifier :=` is the
// record-update suffix on the operand - `move r with(X := 1)` is
// `move (r with(X := 1))` - while `with (e)` with any other parenthesized
// content opens the replacement clause.
BEZEL_TEST(parser_await_move, replacing_move_tie_break_record_update) {
    BEZEL_EXPECT_EQ(parseInitText("move r with(X := 1)"),
                    std::string("(move r with(X := 1))"));
    BEZEL_EXPECT_EQ(parseInitText("move h.F with (OC.Empty)"),
                    std::string("(move h.F with OC.Empty)"));
}

// The clause is in the `using var` initializer position too (issue #7689's
// repro shape).
BEZEL_TEST(parser_await_move, replacing_move_in_using_var_initializer) {
    auto r = parseAll(
        "module M; class C public method M() "
        "using var x := move h.F with OC.Empty; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.5 / §30.7 / A.7 MoveExpr: in an `await for` header a `with` after a move
// source is the cancellation clause (the replacing form is parenthesized).
BEZEL_TEST(parser_await_move, await_for_move_source_with_is_cancellation) {
    auto r = parseAll(
        "module M; class C public method M() "
        "await for var x in move h.F with token do _ := x; end; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), static_cast<std::size_t>(1));
    BEZEL_EXPECT_TRUE(ms[0].find("in (move h.F) with token do") !=
                      std::string::npos);
}

// ... and the parenthesized replacing `move` keeps its clause inside the header.
BEZEL_TEST(parser_await_move, await_for_parenthesized_replacing_move_source) {
    auto r = parseAll(
        "module M; class C public method M() "
        "await for var x in (move h.F with fresh) with token do _ := x; end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), static_cast<std::size_t>(1));
    BEZEL_EXPECT_TRUE(ms[0].find("in (move h.F with fresh) with token do") !=
                      std::string::npos);
}
