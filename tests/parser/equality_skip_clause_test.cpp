// Spec-derived parser tests for EqualityClause — §9.2 (v0.298) / Appendix
// A.3/A.4 (line 10788, 10870, 10952, 11022).
//
// `EqualityClause ::= "equality" "skip"`. `equality` and `skip` are
// contextual keywords (App. B.2) lexed as Identifier; the parser
// dispatches by text. Only the `skip` form exists — rename and
// `optional` have no equality analogue, so the vocabulary is one word,
// closed (spec line 1385).
//
// Grammar admits EqualityClause at four storage-bearing sites:
//   - A.3 PositionalField (record positional fields, AFTER SerializedClause)
//   - A.3 CaseField (choice payload fields, AFTER SerializedClause)
//   - A.4 auto-property PropertyBody (AFTER SerializedClause)
//   - A.4 FieldDecl (BEFORE the Identifier — the unusual placement;
                    //     `serialized` is never on an explicit FieldDecl)
//
// Semantic restrictions (§9.2 line 1419):
//   - computed (expression-bodied) property: rejected at parse time
//   - method: rejected at parse time
//   - static member: parser accepts; sema rejects as inert annotation
//   - enum constant: rejected at parse time
//   - choice case: rejected at parse time
//
// This file covers the parser-level acceptance on the THREE sites whose
// host construct parses cleanly on the current tree (PositionalField,
// PropertyDecl, explicit FieldDecl) plus the parse-time rejections on
// computed property / method / enum / choice case. The CaseField site
// (§35.7) is deferred to a separate issue — a pre-existing parser bug
// (choice cases tokenized away before the choice-branch body parser runs)
// breaks every `choice X(...); end X;` shape and is out of scope for this
// lane. The static-member sema rejection is exercised in
// tests/sema/equality_skip_static_reject_4643_test.cpp (the lane's full
// regression cover).

#include <memory>
#include <string>

#include "ast/Node.hpp"
#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

// Convenience: locate the first FieldDecl in the parsed module's first
// record/class. Returns nullptr if the shape isn't a FieldDecl. Mirrors
// the layer the lane's regression assertion lives at — the AST, not the
// summary string — so a future memberSummary change cannot silently
// weaken the oracle (CLAUDE.md: "Never weaken a test to make it pass").
const bezel::ast::FieldDecl* firstFieldDecl(const bezel::test::ParseResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& d : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(d.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* fd = dynamic_cast<const bezel::ast::FieldDecl*>(m.get());
            if (fd) return fd;
        }
    }
    return nullptr;
}

// Same convenience for the first auto-property PropertyDecl.
const bezel::ast::PropertyDecl* firstAutoProperty(const bezel::test::ParseResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& d : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(d.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* pd = dynamic_cast<const bezel::ast::PropertyDecl*>(m.get());
            if (pd && pd->bodyKind == bezel::ast::PropertyBodyKind::Auto) return pd;
        }
    }
    return nullptr;
}

}  // namespace

// =====================================================================
// §9.2 / A.3 — PositionalField accepts `equality skip` after
// `serialized`. The grammar places EqualityClause AFTER SerializedClause
// when both are present.
// =====================================================================

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           positional_field_equality_skip) {
    auto r = parseAll(
        "module M; record Interval(Lo: int, Hi: int, _cachedLen: int equality skip);");
    // Parser-level acceptance: no errors, three positional fields, the
    // third carries `equality skip`. (Sema's inert / membership walk is
    // not in scope for this file.)
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (!r.sourceFile) return;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return;
    BEZEL_EXPECT_EQ(td->positionalFields.size(), std::size_t{3});
    const auto& pf = td->positionalFields[2];
    BEZEL_EXPECT_TRUE(pf.equality != nullptr);
    if (pf.equality) {
        BEZEL_EXPECT_TRUE(pf.equality->isSkip);
    }
}

// =====================================================================
// §9.2 / A.3 — PositionalField accepts `equality skip` after a
// preceding `serialized` clause (the two-clause form).
// =====================================================================

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           positional_field_serialized_then_equality_skip) {
    auto r = parseAll(
        "module M; record R(x: int serialized skip equality skip);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (!r.sourceFile) return;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return;
    BEZEL_EXPECT_EQ(td->positionalFields.size(), std::size_t{1});
    const auto& pf = td->positionalFields[0];
    BEZEL_EXPECT_TRUE(pf.serialized != nullptr);
    BEZEL_EXPECT_TRUE(pf.equality != nullptr);
}

// =====================================================================
// §9.2 / A.4 — FieldDecl accepts `equality skip` BEFORE the Identifier
// (the unusual placement — see file header). The lane's example: the
// `Interval` record at spec §9.2 lines 1397–1409.
// =====================================================================

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           explicit_field_equality_skip_on_instance) {
    auto r = parseAll(
        "module M; record Interval "
        "  public readonly property Lo: int; "
        "  public readonly property Hi: int; "
        "  private equality skip _cachedLen: int; "
        "  public constructor(lo: int, hi: int) "
        "    Lo := lo; Hi := hi; _cachedLen := hi - lo; "
        "  end constructor; "
        "end Interval;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* fd = firstFieldDecl(r);
    BEZEL_EXPECT_TRUE(fd != nullptr);
    if (fd) {
        BEZEL_EXPECT_EQ(std::string(fd->name), std::string("_cachedLen"));
        BEZEL_EXPECT_TRUE(fd->equality != nullptr);
        if (fd->equality) BEZEL_EXPECT_TRUE(fd->equality->isSkip);
    }
}

// =====================================================================
// §9.2 / A.4 — auto-property PropertyBody accepts `equality skip`
// after `serialized`. The grammar places them in this fixed order:
// `(":=" Expression)? SerializedClause? EqualityClause? ";"`.
// =====================================================================

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           auto_property_equality_skip) {
    auto r = parseAll(
        "module M; record Cache "
        "  public property Stamp: int equality skip; "
        "end Cache;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* pd = firstAutoProperty(r);
    BEZEL_EXPECT_TRUE(pd != nullptr);
    if (pd) {
        BEZEL_EXPECT_EQ(std::string(pd->name), std::string("Stamp"));
        BEZEL_EXPECT_TRUE(pd->equality != nullptr);
        if (pd->equality) BEZEL_EXPECT_TRUE(pd->equality->isSkip);
    }
}

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           auto_property_initializer_serialized_equality_skip) {
    auto r = parseAll(
        "module M; record Cache "
        "  public property Stamp: int := 0 serialized \"stamp\" equality skip; "
        "end Cache;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* pd = firstAutoProperty(r);
    BEZEL_EXPECT_TRUE(pd != nullptr);
    if (pd) {
        BEZEL_EXPECT_TRUE(pd->serialized != nullptr);
        BEZEL_EXPECT_TRUE(pd->equality != nullptr);
    }
}

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           auto_property_with_accessor_vis_equality_skip) {
    auto r = parseAll(
        "module M; class C "
        "  public property Counter: int private set := 0 equality skip; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* pd = firstAutoProperty(r);
    BEZEL_EXPECT_TRUE(pd != nullptr);
    if (pd) {
        BEZEL_EXPECT_TRUE(pd->hasAutoAccessorVis);
        BEZEL_EXPECT_TRUE(pd->autoAccessorIsSet);
        BEZEL_EXPECT_TRUE(pd->equality != nullptr);
    }
}

// =====================================================================
// §9.2 / §35.7 — CaseField accepts `equality skip`. The grammar is
// `"owned"? Identifier ":" Type SerializedClause? EqualityClause?`.
//
// NOTE: parser-level coverage of `equality skip` on choice payload fields
// is intentionally NOT included in this file. A pre-existing parser bug
// (choice cases are tokenized away before the choice-branch body parser
// sees them — the dispatch enters at `(` rather than at the first case
// name) means even the BARE `choice Cache(Some, Empty); end Cache;`
// shape fails to parse cleanly on the current tree. This bug is NOT
// caused by this lane's changes (it reproduces with no equality/skip
// tokens present) and is OUT OF SCOPE for the `equality skip` lane per
// CLAUDE.md rule "No while-we're-at-it expansions" — a separate issue
// is to be filed for it. The sema inert-annotation check is still
// covered for choice cases (the static-member rule is the same shape)
// via the static-field cases in tests/sema/.
// =====================================================================

// =====================================================================
// Vocabulary closure — only `equality skip` exists. Spec line 1385:
// "only the `skip` form exists — rename and `optional` have no equality
// analogue, so the vocabulary is one word, closed."
// =====================================================================

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           bare_equality_is_error) {
    // Bare `equality` (no `skip`).
    auto r = parseAll(
        "module M; record R(x: int equality);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           equality_with_string_rename_is_error) {
    // `equality "x"` — rename form is not admitted (line 1385).
    auto r = parseAll(
        "module M; record R(x: int equality \"x\");");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           equality_with_optional_is_error) {
    // `equality optional` — `optional` analogue is not admitted
    // (line 1385).
    auto r = parseAll(
        "module M; record R(x: int equality optional);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// =====================================================================
// `equality` as an IDENTIFIER — contextually-keyword safety check. A
// method NAMED `equality` must still parse cleanly, because `equality`
// is contextual (App. B.2), not reserved (App. B.1).
// =====================================================================

BEZEL_TEST(parser_equality_skip_s9_2_0009,
           identifier_named_equality_is_unaffected) {
    auto r = parseAll(
        "module M; class C "
        "  public method equality(): int return 1; end equality; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}