// Spec-derived sema tests for §35.5 / §35.6 (v0.334) — the `|` alternative
// list over a CHOICE scrutinee.
//
// §35.5 *Case patterns*, last bullet, verbatim:
//
//   "Payload-free case names are admissible members of `|` **alternative
//   lists** (and count toward exhaustiveness, §19.6); payload-carrying case
//   patterns are not, even in the all-discard form — they are
//   destructurings, not constants (§19.3)."
//
// §35.6 verbatim: "coverage is counted **per case name, one level deep** —
// a payload-free case by a constant-style pattern naming it (**directly or in
// a `|` list**), a payload case by a destructuring whose every slot is
// irrefutable."
//
// The enum counterpart is §10.6 / §19.6, where `collectEnumPatternNames`
// (src/sema/ExprTypecheck.cpp) already recurses into an `AltPattern`'s
// members. The CHOICE exhaustiveness branch handles only a bare
// `NamedPattern` and a `DestructuringPattern`, so an `AltPattern` arm
// contributes nothing and a fully-covering `Red | Green` is reported as
// "missing case(s): Red Green". Rows below pin that.
//
// The programs are single-line on purpose: each is built by concatenating C++
// string literals with NO in-program `//` comment (a `//` would swallow the
// rest of the concatenated line and every row would return -999). The prose
// citations live in the C++ comments OUTSIDE the literals.

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool isExhaustiveWarn(const bezel::Diagnostic& d) {
    return d.severity == bezel::Severity::Warning &&
           d.message.find("non-exhaustive") != std::string::npos;
}

// True iff a non-exhaustiveness WARNING fired naming the choice and, when
// `missingNeedle` is non-empty, exactly that missing-case list.
bool hasExhaustiveWarn(const bezel::test::SemaResult& r,
                       std::string_view choiceName,
                       std::string_view missingNeedle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (!isExhaustiveWarn(d)) continue;
        if (d.message.find(std::string("choice `") + std::string(choiceName) +
                           "`") == std::string::npos)
            continue;
        if (!missingNeedle.empty() &&
            d.message.find(std::string("missing case(s):") +
                           std::string(missingNeedle)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

bool lacksExhaustiveWarn(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (isExhaustiveWarn(d)) return false;
    return true;
}

bool hasErrWith(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// === THE PINNED DEFECT =====================================================
//
// §35.6: a payload-free case is covered by a constant-style pattern naming it
// "directly or in a `|` list". `Red | Green` covers Red and Green; `Blue` is
// the only other case and it is covered by its own arm. The match is
// exhaustive, so the non-exhaustiveness warning MUST NOT fire.
//
// Measured on the base tree (r23-c28): this row FAILS — the compiler reports
// "missing case(s): Red Green", because the choice exhaustiveness branch
// matches only NamedPattern / DestructuringPattern and never descends into an
// AltPattern's members.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_alt_list_of_payload_free_cases_is_exhaustive) {
    // §35.6: "a payload-free case by a constant-style pattern naming it
    // (directly or in a `|` list)". Red and Green are named by ONE
    // alternative-list arm; Blue by the next. Every case is covered.
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Green => 1, "
        "    Blue        => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// A two-case choice exhaustively covered by a SINGLE alternative-list arm.
// The minimal shape of the same defect: with no second arm to be blamed, the
// warning (if any) can only come from the `|` list being ignored.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_single_alt_list_arm_covers_every_case) {
    auto r = semaAll(
        "module M; choice Color Red, Green; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Green => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// A three-way alternative list, plus a payload case covered by a
// destructuring. Exercises the AltPattern arm alongside the (already-working)
// DestructuringPattern arm in the same `case`.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_alt_list_beside_payload_case_is_exhaustive) {
    auto r = semaAll(
        "module M; "
        "choice Shape Empty, Point(x: int), Line(a: int, b: int); end Shape; "
        "class C public static method Main(): int "
        "  var s: Shape := Shape.Empty; "
        "  return (case s of "
        "    Point(var p)       => p, "
        "    Line(var a, var b) => a + b, "
        "    Empty              => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// The `|` list must ALSO count when the alternative list is the only thing
// covering the case names while the remaining arms cover the payload case.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_alt_list_covers_names_while_destructuring_covers_payload) {
    auto r = semaAll(
        "module M; "
        "choice Sig Off, On(flag: bool), Broken(code: int); end Sig; "
        "class C public static method Main(): int "
        "  var s: Sig := Sig.Off; "
        "  return (case s of "
        "    On(var f)       => (if f then 1 else 0), "
        "    Broken(var code) => code, "
        "    Off             => -1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// === DIFFERENTIAL CONTROLS ================================================
//
// A fix that made EVERY alternative-list arm a catch-all would pass the rows
// above while silently defeating the exhaustiveness check. These rows pin the
// old behaviour that must survive.

// A `|` list naming only SOME of the payload-free cases leaves the others
// uncovered: `Red | Green` does NOT cover Blue.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_alt_list_missing_third_case_still_warns) {
    // §35.6: coverage is per case NAME. Naming Red and Green in one arm
    // leaves Blue uncovered, so the diagnostic must still name Blue — and
    // must NOT name Red or Green.
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Green => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Color", " Blue"));
    BEZEL_EXPECT_FALSE(hasExhaustiveWarn(r, "Color", " Red"));
    BEZEL_EXPECT_FALSE(hasExhaustiveWarn(r, "Color", " Green"));
}

// An alternative list whose arm carries a GUARD contributes nothing, exactly
// as a guarded plain arm does (§35.6: "Guarded arms ... contribute nothing to
// the static proof").

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_guarded_alt_list_arm_does_not_contribute) {
    // The guard is on the ARM, so the whole alternative list is untrusted:
    // both Red and Green stay uncovered.
    auto r = semaAll(
        "module M; choice Color Red, Green; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Green when true => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Color", " Red Green"));
}

// CONTROL (the sibling kind): the same `|` alternative list over an ENUM
// scrutinee is exhaustive and has always been. §10.6 / §19.6 route it through
// `collectEnumPatternNames`, which recurses into the AltPattern. This row
// passes before and after the choice fix and proves the choice branch is the
// only one that was missing the recursion.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_alt_list_over_enum_scrutinee_stays_exhaustive) {
    auto r = semaAll(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Green => 1, "
        "    Blue        => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// === §35.5: which members a `|` list admits ================================

// §35.5: "Payload-free case names are admissible members of `|` alternative
// lists". The members resolve as case patterns, never as type tests — so
// neither `Red` nor `Green` may be reported as failing to name a type.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_5_alt_list_members_are_case_patterns_not_type_tests) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  case x of "
        "    Red | Green => do return 1; end, "
        "    Blue        => do return 2; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrWith(r, "does not name a type"));
    BEZEL_EXPECT_FALSE(hasErrWith(r, "unresolved identifier `Red`"));
    BEZEL_EXPECT_FALSE(hasErrWith(r, "unresolved identifier `Green`"));
}

// §35.5: "payload-carrying case patterns are not [admissible in a `|` list],
// even in the all-discard form — they are destructurings, not constants".
// The diagnostic is §19.3's alternative-member rule.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_5_payload_case_rejected_in_alt_list_even_all_discard) {
    auto r = semaAll(
        "module M; choice Color Red, Green(v: int); end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Green(_) => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasErrWith(r, "pattern alternative member must be a constant"));
}

// The same rule for a choice whose payload case is NOT all-discard: the
// destructuring shape is rejected in a `|` list for the same reason.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_5_binding_payload_case_rejected_in_alt_list) {
    auto r = semaAll(
        "module M; choice Color Red, Green(v: int); end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Green(var v) => 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasErrWith(r, "pattern alternative member must be a constant"));
}

// === §35.5 / §35.6 neighbouring rules, pinned as controls ================

// §35.5: a payload case may not be matched bare — the diagnostic names the
// arity and the two admitted spellings. (Pinned here as the neighbour of the
// alt-list rows: accepting payload-FREE names in a `|` list is not a blanket
// loosening of the pattern checker — a payload case in the very same `case`
// is still rejected for the arity.)

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_5_payload_case_bare_still_rejected_beside_alt_lists) {
    auto r = semaAll(
        "module M; choice Color Red, Green(v: int), Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | Blue => 1, "
        "    Green      => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasErrWith(r,
                   "Payload case `Green` requires a payload pattern"));
    BEZEL_EXPECT_TRUE(hasErrWith(r, "Green(_)"));
}

// §35.6: "Nested refutable patterns (`Ok(0)`) are admitted syntactically and
// contribute nothing statically". A refutable slot does not cover its case.

BEZEL_TEST(choice_alt_pattern_exhaustiveness_35_6,
           spec_35_6_refutable_slot_beside_alt_list_still_warns) {
    auto r = semaAll(
        "module M; choice Color Red, Green(v: int); end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red        => 1, "
        "    Green(0)   => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Color", " Green"));
}