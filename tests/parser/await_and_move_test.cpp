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
