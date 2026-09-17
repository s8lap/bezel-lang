// Spec-derived sema tests for §35.4 — sibling instantiation order
// independence for a SHARED generic choice (fix #4125, family J §2.6,
// v0.298).
//
// Background:
//
// #4125: when a choice `D<T>` is reached at TWO distinct instantiations
// (`D<int>` and `D<Widget>`) through sibling case payloads of an
// enclosing choice, the §35.4 classification must not depend on the
// order in which the cases are declared or walked. The pre-fix
// `owningSeen` guard was keyed on `TypeSymbol*` (the BARE declaration
// `D`), so the first sibling erased the second's verdict regardless of
// ordering — §3.6 rule 1's "order-independent" rule violated.
//
// Fix #4125 re-keyed `owningSeen` (and `payloadFieldTypeIsMoveOnly`'s
// sibling `seen`) on `const sema::TypeRef*` (the INSTANTIATION).
// `D<int>` and `D<Widget>` no longer collide in the sibling guard, and
// the per-call memoization cache on `classifyChoiceCore` further
// guarantees each instantiation is classified at most once.
//
// Companion: §28.1's per-instantiation `owned`-marker erasure oracle
// — for a value-type instantiation, the `owned` marker is erased, so
// `D<int>` through `Outer { A(x: D<int>), B(y: D<Widget>) }` ends up
// owning only via case B (Widget is a class, owning at that
// instantiation). The pre-fix #4112 over-conservative walk returned
// Owning via case A's payload regardless of `T`'s instantiation; the
// fix delegates to classifyChoiceCore, which routes through the §28.1
// oracle and answers correctly for both.

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

namespace {

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

// --- §35.4 + #4125: sibling instantiations do not collide --------------------

BEZEL_TEST(choice_sibling_instantiation_order_4125,
           spec_35_4_4125_a_first_two_distinct_instantiations_orders_consistent) {
    // #4125 — `Outer` declared with case A first (`D<int>`), then case B
    // (`D<Widget>`). `D<int>` erases the marker (int is a value builtin,
    // §28.1); `D<Widget>` does not (Widget is a class). The MAX over
    // cases is Owning, via B. Both `Outer` and the inner `D<Widget>`
    // verdict are guaranteed by the §35.4 spec to be the SAME whether
    // A or B is declared first.
    auto r = resolveAll(
        "module M; "
        "class Widget end Widget; "
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer "
        "  A(x: D<int>), "
        "  B(y: D<Widget>); "
        "end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto c = classify(r, "Outer");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::Owning);
}

BEZEL_TEST(choice_sibling_instantiation_order_4125,
           spec_35_4_4125_b_first_two_distinct_instantiations_orders_consistent) {
    // #4125 — the SAME `Outer` declaration with case B FIRST
    // (`D<Widget>`), then case A (`D<int>`). Pre-fix the
    // declaration-keyed sibling guard erased the FIRST sibling's verdict
    // when it encountered the SECOND (or vice-versa depending on the
    // walk order), returning the wrong classification for at least one
    // shape. Post-fix both orderings agree: Owning, via `D<Widget>`,
    // same as the A-first row.
    auto r = resolveAll(
        "module M; "
        "class Widget end Widget; "
        "choice D<T> Some(owned t: T), None; end D; "
        "choice Outer "
        "  B(y: D<Widget>), "
        "  A(x: D<int>); "
        "end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto c = classify(r, "Outer");
    BEZEL_EXPECT_TRUE(c.has_value());
    BEZEL_EXPECT_EQ(*c, ChoiceClassification::Owning);
}

BEZEL_TEST(choice_sibling_instantiation_order_4125,
           spec_35_4_4125_both_classes_neither_erases_still_orders_consistent) {
    // #4125 — sanity: when BOTH instantiations are class parameters
    // (neither erases), both orderings still agree (Owning). This
    // confirms the Instantiation-keying is doing what it should — and
    // that the answer is not accidentally tied to which sibling happens
    // to be classified first.
    auto r = resolveAll(
        "module M; "
        "class A end A; class B end B; "
        "choice D<T> Some(owned t: T), None; end D; "
        // two orderings inlined as separate bodies to share a fixture.
        // Same library code, different Outer orderings.
        "choice O1 "
        "  X(x: D<A>), Y(y: D<B>); "
        "end O1; "
        "choice O2 "
        "  Y(y: D<B>), X(x: D<A>); "
        "end O2;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto c1 = classify(r, "O1");
    auto c2 = classify(r, "O2");
    BEZEL_EXPECT_TRUE(c1.has_value());
    BEZEL_EXPECT_TRUE(c2.has_value());
    BEZEL_EXPECT_EQ(*c1, *c2);
    BEZEL_EXPECT_EQ(*c1, ChoiceClassification::Owning);
}
