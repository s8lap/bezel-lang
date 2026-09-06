// Spec-derived sema tests for §35.4 (v0.200, #2045): the choice
// classification — the three-way derived type property (value-like /
// borrow-bearing / owning) computed over ALL of a choice's cases' payload
// fields. Per §35.4:
//
//   1. OWNING — any case has a payload field marked `owned` (a class,
//      interface, or — at a class/interface instantiation — bare-type-
//      parameter payload), OR a payload field of a move-only value type
//      (§15.3 — an owning inline array or another owning choice,
//      transitively).
//   2. BORROW-BEARING — otherwise, any case has a plain (unmarked) borrow-
//      carrying payload field under §9.7's definition (a class/interface
//      reference, nullable or not; an inline array of reference-typed
//      elements; a borrow-bearing record; or transitively a borrow-bearing
//      choice).
//   3. VALUE-LIKE — otherwise: every payload is a value, `string`, a
//      runtime-managed type, or (transitively) a value-like record or
//      choice.
//
// This file exercises `bezel::common::classifyChoiceType` directly: it
// parses + collects + resolves a choice declaration and asserts on the
// classifier's verdict. The classifier is the foundation that downstream
// §35.5/§35.7/§35.8/§35.12 admissibility rules will consult; landing it in
// isolation lets those use-site diagnostics key off one shared predicate.

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "common/OwnershipFacts.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::common::ChoiceClassification;
using bezel::common::classifyChoiceType;
using bezel::sema::Symbol;
using bezel::sema::TypeSymbol;
using bezel::test::parseAndCollect;

namespace {

// Parse + collect + resolve (mirrors choice_sema_classification_35_test.cpp).
// Resolution is enough: classifyChoiceType reads `ctx.resolvedType(*field.type)`
// on each case payload, which is what TypeResolution populates.
//
// fix #3985: delegates to the shared gated helper (sema_support.hpp) instead
// of duplicating the parse-error-gate logic locally — the file's own copy
// used to run resolveTypes even when parsing failed.
bezel::test::SemaResult resolveAll(std::string_view src) {
    return bezel::test::parseAndResolveGated(src);
}

const TypeSymbol* findType(const bezel::test::SemaResult& r,
                           std::string_view name) {
    if (!r.ctx || !r.ctx->module()) return nullptr;
    for (Symbol* child : r.ctx->module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            if (t->name == name) return t;
        }
    }
    return nullptr;
}

// Build a Named TypeRef for the choice and run the classifier.
std::optional<ChoiceClassification> classify(
    const bezel::test::SemaResult& r, std::string_view name) {
    auto* ts = findType(r, name);
    if (!ts || !r.ctx) return std::nullopt;
    bezel::sema::TypeRef rt;
    rt.kind = bezel::sema::TypeRef::Kind::Named;
    rt.declaration = const_cast<TypeSymbol*>(ts);
    return classifyChoiceType(&rt, *r.ctx);
}

}  // namespace

// --- §35.4.3: VALUE-LIKE ----------------------------------------------------

BEZEL_TEST(choice_classification_35_4, spec_35_4_value_like_payload_free) {
    // §35.4.3: a choice with only payload-free cases is value-like.
    auto r = resolveAll("module M; choice Color Red, Green, Blue; end Color;");
    auto c = classify(r, "Color");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::ValueLike);
}

BEZEL_TEST(choice_classification_35_4, spec_35_4_value_like_value_payloads) {
    // §35.4.3 headline: value, `string`, and runtime-managed payloads are
    // classification-neutral. `Opt.Some(value: int)` with a None case is the
    // spec's small value-like sum.
    auto r = resolveAll(
        "module M; choice Opt Some(value: int), None; end Opt;");
    auto c = classify(r, "Opt");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::ValueLike);
}

BEZEL_TEST(choice_classification_35_4, spec_35_4_value_like_string_payload) {
    // §35.4 final paragraph: "Payloads of runtime-managed reference types
    // (`string`, function types, boxed values) are classification-neutral."
    auto r = resolveAll(
        "module M; choice Log Msg(text: string), Empty; end Log;");
    auto c = classify(r, "Log");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::ValueLike);
}

// --- §35.4.2: BORROW-BEARING ------------------------------------------------

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_borrow_bearing_plain_class_payload) {
    // §35.4.2: "a plain (unmarked) field of class or interface type (nullable
    // or not)" makes the choice borrow-bearing. Plain payload = borrow (no
    // `owned` marker).
    auto r = resolveAll(
        "module M; class C end C; "
        "choice Box Some(value: C), None; end Box;");
    auto c = classify(r, "Box");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::BorrowBearing);
}

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_borrow_bearing_nullable_class_payload) {
    // §35.4.2 "(nullable or not)" — a nullable class payload is still a
    // borrow-carrying field.
    auto r = resolveAll(
        "module M; class C end C; "
        "choice Lookup Found(value: C?), Missing; end Lookup;");
    auto c = classify(r, "Lookup");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::BorrowBearing);
}

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_borrow_bearing_via_record_payload) {
    // §9.7 transitivity: a payload of a borrow-bearing record makes the
    // choice borrow-bearing.
    auto r = resolveAll(
        "module M; class C end C; "
        "record Ref(obj: C); "
        "choice Hold Some(r: Ref), None; end Hold;");
    auto c = classify(r, "Hold");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::BorrowBearing);
}

// --- §35.4.1: OWNING --------------------------------------------------------

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_owning_owned_marker_on_class_payload) {
    // §35.4.1: "A choice is owning if any case has a payload field marked
    // `owned` (a class, interface, or … payload)." The spec's `FetchOutcome`
    // headline shape — `Fetched(owned response: Response)`.
    auto r = resolveAll(
        "module M; class R end R; "
        "choice FetchOutcome Fetched(owned response: R), NotModified; "
        "end FetchOutcome;");
    auto c = classify(r, "FetchOutcome");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::Owning);
}

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_owning_one_case_decides_whole_choice) {
    // §35.4: "computed from the payload fields of *all* the choice's cases"
    // — a single owning case flips the WHOLE choice to owning, even when
    // sibling cases are value-like or borrow-bearing. The decision is
    // MAX over cases, not per-case.
    auto r = resolveAll(
        "module M; class R end R; "
        "choice Mixed "
        "  Ok(status: int), "                       // value-like case
        "  Borrowed(r: R), "                        // borrow-bearing case
        "  Owned(owned r: R); "                     // owning case
        "end Mixed;");
    auto c = classify(r, "Mixed");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::Owning);
}

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_owning_transitive_owning_choice_payload) {
    // §35.4.1: "a payload field of a move-only value type (§15.3 — an owning
    // inline array or another owning choice, transitively)". An unmarked
    // payload whose TYPE is itself an owning choice is move-only; this makes
    // the OUTER choice owning too, even though the outer payload slot has no
    // `owned` marker of its own.
    auto r = resolveAll(
        "module M; class R end R; "
        "choice Inner Owned(owned r: R), None; end Inner; "
        "choice Outer Wrap(i: Inner), Skip; end Outer;");
    auto c = classify(r, "Outer");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::Owning);
}

// --- §35.4: classification is over cases' payload fields (not methods) ------

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_methods_do_not_affect_classification) {
    // §35.4 computes over CASE PAYLOAD FIELDS only. A method body that
    // returns a class-typed value does NOT change the classification —
    // choices are sealed, methods are statically dispatched, and a method
    // is not payload storage. This choice stays value-like.
    auto r = resolveAll(
        "module M; class R end R; "
        "choice Color Red, Green, Blue; "
        "  public method Make(): R => new R(); "
        "end Color;");
    auto c = classify(r, "Color");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::ValueLike);
}

// --- §35.4 / #2130: cycle through the owned-marker oracle must terminate -----

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_self_recursive_choice_via_owned_marker_no_crash) {
    // #2130: a choice that references ITSELF through an `owned` payload
    // marker (`choice L Nil(), Cons(owned t: L)`) is degenerate input that
    // §35.1 rejects elsewhere (the §35.1 no-self-containment rule,
    // `Resolver::checkChoiceSelfContainment`, emits the diagnostic). The
    // classifier itself must NOT crash on it: the #2047 shared oracle pair
    // `classifyChoiceType <-> ownedMarkerActiveOnInstantiatedType` recursed
    // with a FRESH cycle-guard set per invocation, so the guard never fired
    // across the pair and the stack overflowed. The cycle guard now threads
    // through the pair, so the re-entry edge classifies ValueLike (per the
    // existing #2159 cycle-edge rule) and returns instead of overflowing.
    auto r = resolveAll(
        "module M; choice L Nil(), Cons(owned t: L); end L;");
    auto c = classify(r, "L");
    // The verdict itself is unspecified for degenerate input (§35.1 rejects
    // the program); the assertion is that classification TERMINATES here
    // rather than stack-overflowing.
    BEZEL_EXPECT_TRUE(c.has_value());
}

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_mutually_recursive_choice_pair_via_owned_marker_no_crash) {
    // #2130 (the A<->B form): two choices that reference each other through
    // `owned` payload markers form a cycle through the same oracle pair. The
    // threaded cycle guard terminates the recursion here too. §35.1 rejects
    // the program separately; this test asserts only that the classifier
    // does not stack-overflow on the cyclic value-typed component walk.
    auto r = resolveAll(
        "module M; "
        "choice A X(owned t: B); end A; "
        "choice B Y(owned t: A); end B;");
    auto c = classify(r, "A");
    BEZEL_EXPECT_TRUE(c.has_value());
}

// --- §35.4: kind gate — non-choice types return nullopt ---------------------

BEZEL_TEST(choice_classification_35_4,
           spec_35_4_non_choice_returns_nullopt) {
    // The classifier is gated on the choice kind: a record or class with a
    // similar shape returns nullopt so callers can branch on kind in one
    // test.
    auto r = resolveAll(
        "module M; class C end C; record Rec(x: int);");
    BEZEL_EXPECT_FALSE(classify(r, "C").has_value());
    BEZEL_EXPECT_FALSE(classify(r, "Rec").has_value());
}
