// Spec-derived tests for §23.2 (v0.214, issue #2088, proposal 0058):
// `readonly` has ONE meaning on a property / indexer. It is admitted only
// where the shape cannot express read-onlyness — auto-properties (§23.1)
// and interface properties — and is a compile error on every body-bearing
// form. The five rejection shapes enumerated in issue #2088 are:
//
//   1. Expression-bodied property (`=>`) on a class/record.
//   2. Expression-bodied indexer (`=>` on a `this[…]` indexer).
//   3. Get-only block-form property — omitting `set` already expresses
//      read-onlyness, so `readonly` is redundant.
//   4. Expression-bodied property of a choice / enum / extension (the
//      "ExpressionPropertyDecl" slot — same AST PropertyDecl, bodyKind
//      Expression).
//   5. Block form declaring a `set` accessor (contradictory — may not be
//      both `readonly` and writeable).
//
// Each test names the spec sub-section it derives from. Control tests
// confirm the two ACCEPTED forms (auto-property on class/record, signature
// auto-property on interface) keep compiling — the rejection must not
// over-fire.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// §23.2 ACCEPT controls — `readonly` is valid exactly where shape cannot
// express read-onlyness (auto-properties, interface properties).
// =========================================================================

// §23.2: a `readonly` AUTO-property on a class is the canonical valid use —
// the shape (no body) cannot express read-onlyness, so the keyword supplies
// the fact. Compiler must accept.
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_auto_property_accepted_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly property X: int := 5; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §23.2: a `readonly` auto-property WITHOUT an initializer is still a
// declared read-only auto-property — accepted (§23.1).
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_auto_property_no_init_accepted_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly property X: int; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §23.2: a `readonly` auto-property on an interface is accepted — the
// abstract accessor set is declared, not implemented, so the keyword is
// the spelling for "expose a getter only".
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_interface_signature_property_accepted_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  readonly property Count: int; "
        "end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §23.2: a `readonly` auto-property as an indexer signature on an interface
// is accepted — same rule, indexer shape (§23.4).
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_interface_indexer_signature_accepted_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  readonly property [i: int]: int; "
        "end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §23.2 REJECTION shape 1: expression-bodied property on a class/record.
// Spec: "`readonly` has one meaning (v0.214): it is written exactly where
// shape cannot express read-onlyness … On every body-bearing form — an
// expression-bodied property or indexer …" (§23.2).
// =========================================================================
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_expression_body_property_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public readonly property V: int => _v; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is redundant on an "
                                "expression-bodied property"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§23.2"));
}

// §23.2: same shape on a RECORD — same diagnostic path, same expected
// error.
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_expression_body_record_property_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  private _v: int := 0; "
        "  public readonly property V: int => _v; "
        "end R;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is redundant on an "
                                "expression-bodied property"));
}

// =========================================================================
// §23.2 REJECTION shape 2: expression-bodied indexer. Spec enumerates
// "expression-bodied property or indexer" as a body-bearing form (§23.2);
// the rejection message names "indexer".
// =========================================================================
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_expression_body_indexer_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Bag "
        "  private _v: int := 0; "
        "  public readonly property this[i: int]: int => _v; "
        "end Bag;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is redundant on an "
                                "expression-bodied indexer"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§23.2"));
}

// =========================================================================
// §23.2 REJECTION shape 3: get-only block-form property. Omitting `set`
// already expresses read-onlyness; `readonly` is redundant (NOT the
// "contradictory" case — there is no setter to contradict).
// =========================================================================
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_get_only_block_property_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public readonly property V: int "
        "    get => _v; "
        "  end V; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is redundant on a get-only "
                                "block-form property"));
}

// =========================================================================
// §23.2 REJECTION shape 4: expression-bodied property of a choice /
// enum / extension — same PropertyDecl AST node, bodyKind Expression.
// =========================================================================

// §23.2: a choice body's expression-bodied property with `readonly` is
// redundant — the choice slot (ExpressionPropertyDecl in §A.3) is one of
// the three body-bearing grammar slots named in issue #2088.
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_choice_expression_property_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "choice Foo "
        "  Yes, No; "
        "  public readonly property Value: int => 0; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is redundant on an "
                                "expression-bodied property"));
}

// §23.2: an enum body's expression-bodied property with `readonly` is
// rejected — the enum slot's `readonly` is one of the §23.2 redundant
// placements.
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_enum_expression_property_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color "
        "  Red, Green, Blue; "
        "  public readonly property Hue: int => 0; "
        "end Color;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is redundant on an "
                                "expression-bodied property"));
}

// §23.2: an extension body's expression-bodied property with `readonly`
// is rejected — the extension slot's `readonly` is redundant.
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_extension_expression_property_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C public property V: int; end C; "
        "extension C "
        "  public readonly property Doubled: int => 0; "
        "end extension;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is redundant on an "
                                "expression-bodied property"));
}

// =========================================================================
// §23.2 REJECTION shape 5: block form declaring a `set` accessor —
// `readonly` is CONTRADICTORY with a setter (the spec calls this out
// specifically: "on a block form declaring a setter it is contradictory
// and likewise an error" — §23.2).
// =========================================================================
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_block_with_set_accessor_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public readonly property V: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end V; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is contradictory on a "
                                "block-form property"));
}

// §23.2: same shape but on an indexer — message names "indexer".
BEZEL_TEST(readonly_property_body_bearing_2088,
           readonly_block_indexer_with_set_rejected_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Bag "
        "  private _v: int := 0; "
        "  public readonly property this[i: int]: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end Bag;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "`readonly` is contradictory on a "
                                "block-form indexer"));
}

// =========================================================================
// Negative control: WITHOUT `readonly`, the body-bearing shapes compile.
// Confirms the rejection is keyed on the modifier, not on the body shape.
// =========================================================================
BEZEL_TEST(readonly_property_body_bearing_2088,
           non_readonly_expression_body_accepted_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public property V: int => _v; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Negative control: a NON-readonly block form with a setter is the
// canonical read-write property — accepted.
BEZEL_TEST(readonly_property_body_bearing_2088,
           non_readonly_block_with_set_accepted_spec_23_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public property V: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end V; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
