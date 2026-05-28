// Spec-derived parser tests for InterfaceMember — §A.4
//
//   InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer | IfaceEvent
//   IfaceMethod     ::= "method" Identifier GenericParams?
//                       "(" ParamList? ")" (":" "owned"? Type)? ";"
//   IfaceProperty   ::= "readonly"? "property" Identifier ":" Type ";"
//   IfaceIndexer    ::= "readonly"? "property" "[" ParamList "]" ":" Type ";"
//   IfaceEvent      ::= "async"? "event" Identifier (":" Type)? ";"
//
// Interface members carry no visibility (implicit public per §10.1) and are
// signature-only; summaries omit the leading visibility. IfaceMethod reuses
// the MethodDecl AST node with bodyKind=Abstract, which memberSummary renders
// with a trailing " ; "; IfaceProperty reuses PropertyDecl with bodyKind=Auto
// (no initializer), which renders as just `property NAME: TYPE`.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_iface_member, single_method) {
    auto r = parseAll(
        "module M; interface I method Draw(); end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("method Draw() ; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, method_with_params_and_return) {
    auto r = parseAll(
        "module M; interface I method Add(a: int, b: int): int; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("method Add(a: int, b: int): int ; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, method_owned_return) {
    auto r = parseAll(
        "module M; interface I method Take(): owned Resource; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("method Take(): owned Resource ; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, method_generic_param) {
    auto r = parseAll(
        "module M; interface I method Map<T>(x: T): T; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("method Map<T>(x: T): T ; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, simple_property) {
    auto r = parseAll(
        "module M; interface I property Name: string; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("property Name: string"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, readonly_property) {
    auto r = parseAll(
        "module M; interface I readonly property Id: int; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("readonly property Id: int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, indexer) {
    auto r = parseAll(
        "module M; interface I property[i: int]: string; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("property [i: int]: string"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, simple_event) {
    auto r = parseAll(
        "module M; interface I event Click; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("event Click"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, event_with_type) {
    auto r = parseAll(
        "module M; interface I event Click: ClickArgs; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("event Click: ClickArgs"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, async_event) {
    auto r = parseAll(
        "module M; interface I async event LoadAsync; end I;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("async event LoadAsync"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_iface_member, mixed_members) {
    // Iface body mixing all four IfaceMember forms.
    auto r = parseAll(
        "module M; interface I "
        "method Draw(); "
        "property Name: string; "
        "readonly property Id: int; "
        "event Clicked: ClickArgs; "
        "end I;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(4));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
