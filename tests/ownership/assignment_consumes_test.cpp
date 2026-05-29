// Spec-derived tests for Ownership slice O15 — assignment RHS records
// move side-effects (§28.3 / §28.8.1).
//
// `target := move source;` should mark source as MovedFrom regardless
// of what target is. This applies to both local-target and
// field-target assignments. The existing producer-table behavior on
// the target is unchanged.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string("module M; class T end T; ") + body;
}

} // namespace

BEZEL_TEST(own_assign_move, local_to_local_move_consumes_source) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M() "
        "    var src := new T(); "
        "    var dst: T; "
        "    dst := move src; "
        "    using var hold := move dst; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_assign_move, read_after_move_assign_diagnoses) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M() "
        "    var src := new T(); "
        "    var dst: T; "
        "    dst := move src; "
        "    _ := src; "
        "    using var hold := move dst; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_assign_move, field_target_move_consumes_source) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _t: T := new T(); "
        "  public method Replace() "
        "    var fresh := new T(); "
        "    this._t := move fresh; "
        "  end Replace; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_assign_move, field_self_swap_via_move) {
    // Take this._t out into a local, then assign a fresh T back.
    // The move on this._t marks the field moved-from; the subsequent
    // assignment clears it — clean at scope exit.
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _t: T := new T(); "
        "  public method Swap() "
        "    using var old := move this._t; "
        "    this._t := new T(); "
        "  end Swap; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
