// Spec-derived tests for §28.3 K3.13 marker economy — the `move` marker is
// rejected as REDUNDANT at every marker-free consuming position (the
// [consume table](#sec-consume-table) rows tagged with "Spelling column = none —
// the declared slot / keyword announces (marker-free)"). The five positions
// in scope here are:
//
//   K3.1  store into an `owned` field / owning-collection slot — declared
//         slot announces
//   K3.2  `return` under an owned return type — declared return type
//         announces
//   K3.7  operand of `yield return` in an OWNING iterator — declared owning
//         interface announces
//   K3.8  operand of `throw` — the keyword announces
//   K3.9  `_ := x;` — the discard statement itself announces
//
// Before this fix the compiler silently accepted every one of these (the
// K3.13 carve-out was unwritten), leaving programs that conflated the
// marker-free consume with the marker-required transfer without any
// diagnostic. Per K3.13 verbatim: "a consume is spelled by the transfer
// marker (`move x`) except at a marker-free position, where the target or
// keyword already announces it" — the marker is admitted at the
// marker-required consuming positions and only at those.
//
// Spec revisions cited inline at each test; the cluster is one logical fix
// (the marker-economy carve-out) and ships as one commit. The move-on-borrow
// rule (s28_3_0020) is a SEPARATE commit in the same lane; covered by
// move_on_borrow_test.cpp.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// §28.3 K3.13 / K3.2: `return move x;` under an owned return type is
// rejected as redundant. The owned return type is the announcement.
BEZEL_TEST(redundant_move_marker_economy, return_under_owned_return_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C public method Acquire(): owned T "
        "  var w := new T(); "
        "  return move w; "
        "end Acquire; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "return"));
    BEZEL_EXPECT_TRUE(errorSays(r, "K3.13"));
    BEZEL_EXPECT_TRUE(errorSays(r, "K3.2"));
}

// §28.3 K3.13 / K3.2 NEGATIVE: `return w;` (no marker) under an owned return
// type compiles clean — the marker is rejected, but the marker-free spelling
// is admitted by the same rule.
BEZEL_TEST(redundant_move_marker_economy, return_without_marker_admitted_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C public method Acquire(): owned T "
        "  var w := new T(); "
        "  return w; "
        "end Acquire; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 K3.13 / K3.2 FIELD-MOVE NEGATIVE: `return move _f;` over an OWNED
// FIELD must NOT fire the redundant-move diagnostic (a field's slot needs
// the marker for §28.8.1 K9.2 restoration tracking; the field-not-restored
// diagnostic owns those cases).
BEZEL_TEST(redundant_move_marker_economy, return_field_move_not_redundant_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C "
        "  private owned _r: T; "
        "  public constructor() _r := new T(); end constructor; "
        "  public method Take(): owned T "
        "    return move _r; "
        "  end Take; "
        "end C;");
    // The field-not-restored diagnostic is the OWNING one here; the redundant
    // marker diagnostic must NOT be present.
    BEZEL_EXPECT_FALSE(errorSays(r, "redundant"));
    BEZEL_EXPECT_FALSE(errorSays(r, "K3.13 marker economy"));
}

// §28.3 K3.13 / K3.8: `throw move e;` is rejected as redundant — the throw
// keyword announces.
BEZEL_TEST(redundant_move_marker_economy, throw_operand_redundant_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class E public constructor(s: string) end constructor; end E; "
        "class C public method Demo() "
        "  var ex := new E(\"boom\"); "
        "  throw move ex; "
        "end Demo; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "throw"));
    BEZEL_EXPECT_TRUE(errorSays(r, "K3.13"));
}

// §28.3 K3.13 / K3.9: `_ := move x;` is rejected as redundant — the discard
// statement itself is the announcement.
BEZEL_TEST(redundant_move_marker_economy, discard_rhs_redundant_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C public method Demo() "
        "  var w := new T(); "
        "  _ := move w; "
        "end Demo; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "discard"));
    BEZEL_EXPECT_TRUE(errorSays(r, "K3.13"));
}

// §28.3 K3.13 / K3.1: store into an `owned` field RHS is rejected as
// redundant — the declared slot announces.
BEZEL_TEST(redundant_move_marker_economy, owned_field_store_rhs_redundant_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C "
        "  private owned _w: T; "
        "  public constructor(owned w: T) "
        "    _w := move w; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "field"));
    BEZEL_EXPECT_TRUE(errorSays(r, "K3.13"));
}

// §28.3 K3.13 / K3.7: operand of `yield return` in an OWNING iterator is
// rejected as redundant — the declared owning interface announces.
BEZEL_TEST(redundant_move_marker_economy, owning_yield_return_redundant_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class T public constructor() end constructor; end T; "
        "class Factory "
        "  public method Produce(): IOwningEnumerable<T> "
        "    var w := new T(); "
        "    yield return move w; "
        "  end Produce; "
        "end Factory;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "yield"));
    BEZEL_EXPECT_TRUE(errorSays(r, "K3.13"));
}