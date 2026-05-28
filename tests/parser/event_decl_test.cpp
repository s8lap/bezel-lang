// Spec-derived parser tests for EventDecl — §A.4 `EventDecl ::= Visibility
// "static"? "async"? "event" Identifier (":" Type)? ";"`.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_event, simple_event) {
    auto r = parseAll(
        "module M; class Foo public event Click; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public event Click"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_event, event_with_type) {
    auto r = parseAll(
        "module M; class Foo public event Click: ClickArgs; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public event Click: ClickArgs"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_event, async_event) {
    auto r = parseAll(
        "module M; class Foo public async event LoadAsync; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("public async event LoadAsync"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_event, static_event) {
    auto r = parseAll(
        "module M; class Foo "
        "public static event Counter: int; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public static event Counter: int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
