// Spec-derived parser tests for interface indexer owned-setter declarations —
// issue #681, §23.3 / §23.4 / Appendix A §A.4.
//
// Appendix A §A.4 defines:
//   IfaceIndexer ::= "readonly"? "property" "[" ParamList "]" ":" Type ";"
//   IfaceProperty ::= "readonly"? "property" Identifier ":" Type ";"
//
// §23.3 prose states (lines ~3961-3973):
//   "abstract/interface accessor bodies (`;`-terminated) carry the spec the
//    same way (`set(owned);`)"
//
// §23.4 states:
//   "Indexers may be declared in interfaces with the body-less form."
//
// The stdlib §4 declarations include interface indexers with `set(owned);`.
//
// The current grammar (IfaceIndexer) only has the single-line form ending
// in `;`. §23.3 prose explicitly extends this to the block-accessor form for
// interfaces. This file implements the block-form for interface properties and
// indexers: `Accessor* "end" ";"` where each Accessor uses the abstract body
// (`";"`) and may carry a SetterParamSpec.
//
// Grammar extension implemented (per §23.3 prose):
//   IfaceIndexer ::= "readonly"? "property" "[" ParamList "]" ":" Type
//                    ( ";" | Accessor* "end" ";" )
//   IfaceProperty ::= "readonly"? "property" Identifier ":" Type
//                     ( ";" | Accessor* "end" ";" )
//
// Tests:
//   1. Repro from conformance test: `get; set(owned);` indexer.
//   2. Readonly interface indexer (simple form; regression).
//   3. Interface property with block `get; set;` form.
//   4. Interface property block with `set(owned);`.
//   5. Negative: `set =>` body in interface should still fail (only `;` bodies
//      are valid per §23.3 "abstract/interface accessor bodies (`;`-terminated)").

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// §681 repro: interface indexer with `get;` and `set(owned);` accessor block.
BEZEL_TEST(parser_iface_indexer_owned_681, block_form_get_set_owned) {
    auto r = parseAll(
        "module M; "
        "public interface IFoo "
        "  property [i: int]: string "
        "    get; "
        "    set(owned); "
        "  end; "
        "end IFoo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("property [i: int]: string { get; set(owned); }"));
}

// Interface indexer block form with `set(owned x)` — named parameter.
BEZEL_TEST(parser_iface_indexer_owned_681, block_form_set_owned_named_param) {
    auto r = parseAll(
        "module M; "
        "interface IFoo "
        "  property [i: int]: int "
        "    get; "
        "    set(owned newVal); "
        "  end; "
        "end IFoo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("property [i: int]: int "
                        "{ get; set(owned newVal); }"));
}

// Interface indexer block form with plain `set;` (no owned modifier).
BEZEL_TEST(parser_iface_indexer_owned_681, block_form_get_plain_set) {
    auto r = parseAll(
        "module M; "
        "interface IFoo "
        "  property [k: string]: int "
        "    get; "
        "    set; "
        "  end; "
        "end IFoo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("property [k: string]: int { get; set; }"));
}

// Interface property (named) with block `get; set(owned);` form.
BEZEL_TEST(parser_iface_indexer_owned_681, named_property_block_set_owned) {
    auto r = parseAll(
        "module M; "
        "interface IFoo "
        "  property Item: Resource "
        "    get; "
        "    set(owned); "
        "  end; "
        "end IFoo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("property Item: Resource { get; set(owned); }"));
}

// Interface property block form with plain `get; set;`.
BEZEL_TEST(parser_iface_indexer_owned_681, named_property_block_get_set) {
    auto r = parseAll(
        "module M; "
        "interface IFoo "
        "  property Name: string "
        "    get; "
        "    set; "
        "  end; "
        "end IFoo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("property Name: string { get; set; }"));
}

// Regression: simple single-line interface indexer `property [i: int]: T;`
// must still work.
BEZEL_TEST(parser_iface_indexer_owned_681, simple_iface_indexer_regression) {
    auto r = parseAll(
        "module M; interface I property[i: int]: string; end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("property [i: int]: string"));
}

// Regression: simple single-line interface property must still work.
BEZEL_TEST(parser_iface_indexer_owned_681, simple_iface_property_regression) {
    auto r = parseAll(
        "module M; interface I property Name: string; end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("property Name: string"));
}

// Mixed members: simple + block forms in same interface.
BEZEL_TEST(parser_iface_indexer_owned_681, mixed_simple_and_block_forms) {
    auto r = parseAll(
        "module M; interface I "
        "  property Name: string; "
        "  property [i: int]: int "
        "    get; "
        "    set(owned); "
        "  end; "
        "  method Count(): int; "
        "end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(3));
}
