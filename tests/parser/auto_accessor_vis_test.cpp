// Spec-derived parser tests for AutoAccessorVis on auto-properties —
// §A.4:
//
//   PropertyBody ::= ... | AutoAccessorVis? (":=" Expression)?
//                          SerializedClause? ";"
//   AutoAccessorVis ::= Visibility ("get" | "set")
//
// The auto-property form takes an optional per-accessor visibility
// override — `property X: int private set := 0;` is publicly-gettable,
// privately-settable. Discriminator vs the accessor-block form: an
// AutoAccessorVis run is the visibility keyword + `get`/`set` followed
// IMMEDIATELY by `:=` or `;`. Anything else (`=>`, `(`, `{`) is an
// accessor block.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_auto_acc_vis, private_set_with_init) {
    auto r = parseAll(
        "module M; class C "
        "public property X: int private set := 0; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: int private set := 0"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_auto_acc_vis, public_get_with_init) {
    auto r = parseAll(
        "module M; class C "
        "public property X: int public get := 42; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: int public get := 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_auto_acc_vis, private_set_no_init) {
    auto r = parseAll(
        "module M; class C "
        "public property X: int private set; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public property X: int private set"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_auto_acc_vis, accessor_block_path_unaffected) {
    // Regression: `public get =>` should still take the accessor-block
    // path, NOT the AutoAccessorVis path. The disambiguator is the
    // `=>` follower.
    auto r = parseAll(
        "module M; class C "
        "public property X: int public get => 0; end; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property X: int { public get => 0; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_auto_acc_vis, auto_no_accessor_vis_still_works) {
    // Regression: the plain auto-property form (no AutoAccessorVis) must
    // continue to parse unchanged.
    auto r = parseAll(
        "module M; class C "
        "public property X: int := 0; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("public property X: int := 0"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_auto_acc_vis, with_serialized_clause) {
    auto r = parseAll(
        "module M; class C "
        "public property Port: int private set := 80 "
        "serialized \"port\" optional; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property Port: int private set := 80 "
                "serialized \"port\" optional"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
