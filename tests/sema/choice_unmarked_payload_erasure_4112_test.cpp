// Spec-derived sema tests for §35.4 with the §28.1 per-instantiation
// owned-marker erasure oracle applied through an UNMARKED PAYLOAD walk
// (fix #4112, family J §2.6, v0.298).
//
// Background:
//
// §35.4 classifies a choice as OWNING if any case has a payload that
// resolves to a move-only value type (§15.3 — an owning inline array or
// another owning choice, transitively). §28.1 erases the `owned` marker
// at value-type instantiations: an `owned t: int` is just an int.
//
// The unguarded "hand-rolled walk" through `payloadFieldTypeIsMoveOnly`
// used to check `pf.modifier == "owned"` and unconditionally return
// `true`, applying NO erasure oracle. The consequence: a `D<int>` reached
// directly classified ValueLike (owning erases at `int`), but a
// `D<int>` reached through an UNMARKED payload of another choice
// (`Outer { A(x: D<int>) }`) classified Owning — same choice, same
// instantiation, two different answers.
//
// This regression pins the fix: the Choice arm of
// `payloadFieldTypeIsMoveOnly` now delegates to `classifyChoiceCore`,
// which threads the §28.1 erasure oracle through
// `ownedMarkerActiveOnInstantiatedTypeImpl` (consulted via
// `substitutedChoicePayloadType`). Both routes agree.

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

// Parse + collect + resolve (mirrors choice_classification_35_4_test.cpp).
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

// --- §35.4 + §28.1: erasure oracle applied through unmarked payload --------

BEZEL_TEST(choice_unmarked_payload_erasure_4112,
           spec_35_4_4112_value_instantiation_through_unmarked_payload) {
    // #4112 headline shape: `D<int>` reached through an unmarked payload
    // of an enclosing choice must classify the SAME as `D<int>` reached
    // directly. §28.1 erases the `owned` marker for value-type
    // instantiations, so `D<int>` is value-like and `Outer` (whose only
    // case is `D<int>`) is therefore value-like too. Pre-fix the
    // unmarked-payload walk answered Owning (the bug).
    auto r = resolveAll(
        "module M; "
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer A(x: D<int>); end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto c = classify(r, "Outer");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::ValueLike);
}

BEZEL_TEST(choice_unmarked_payload_erasure_4112,
           spec_35_4_4112_class_instantiation_through_unmarked_payload_owns) {
    // #4112 sanity: when `D<Widget>` is reached through an unmarked
    // payload and `Widget` is a class (an owning reference), the
    // delegation to `classifyChoiceCore` answers Owning. This is what the
    // fix has to do — and what the masking direct-reach verdict (Owning
    // via #4112's bug) happened to coincidentally say. Without this
    // sanity row a "fix" that simply answered ValueLike for both would
    // also pass the headline regression above.
    auto r = resolveAll(
        "module M; "
        "class Widget end Widget; "
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer A(x: D<Widget>); end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto c = classify(r, "Outer");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::Owning);
}
