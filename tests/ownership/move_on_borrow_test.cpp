// Spec-derived tests for the §28.3 / §28.8.1 move-on-borrow rejection:
//
//// `move` is not admitted on a borrow — the marker asserts a transfer, and
//// a plain parameter carries no obligation to transfer. Sibling to the K3.13
//// marker-economy fix (redundant_move_marker_economy_28_3_test.cpp) but a
//// SEPARATE rule: K3.13 is about marker REDUNDANCY at marker-free positions
//// (the target/keyword already announces); the move-on-borrow rule is about
//// marker ADMISSION on a source that carries no obligation to transfer. The
//// two ship as separate commits in the same lane.
////
//// Before this fix the pre-fix behavior was a wrong-shape error further
//// downstream: `var b := move w;` on a borrow parameter `w` left `b` as a
//// fresh owned value with no backing obligation, drawing the "owned value
//// in `b` is implicitly disposed" lint — wrong shape (it's a marker
//// admission problem, not a scope-disposal problem), wrong site (the bug is
//// at `move`, not at the scope end), wrong wording (no ownership was ever
//// transferred in the first place).
////
//// Per the case source (`tests/conformance/rules/cases/s28_3_0020.bzl`):
//// "`move` is also an error on a borrow: the marker asserts a transfer, and
//// a plain parameter carries no obligation to transfer."

#include <string>
#include <string_view>

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

// §28.3 / §28.8.1: `var b := move w;` on a PLAIN parameter (no `owned`
// modifier) is rejected — `w` is a borrow, no obligation to transfer.
BEZEL_TEST(move_on_borrow, plain_parameter_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C public method Peek(w: T) "
        "  var b := move w; "
        "end Peek; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "move"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow"));
}

// §28.3 / §28.8.1 NEGATIVE: `var b := w;` (no marker) on a plain parameter
// compiles clean — a plain borrow is admitted.
BEZEL_TEST(move_on_borrow, plain_parameter_no_marker_admitted_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C public method Peek(w: T) "
        "  var b := w; "
        "end Peek; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.8.1 NEGATIVE: `var b := move w;` on an `owned` parameter IS
// admitted (the marker transfers ownership out of `w`, which is the
// marker-required consuming position). The move-on-borrow check is bounded to
// plain parameters (modifier != "owned").
BEZEL_TEST(move_on_borrow, owned_parameter_marker_admitted_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T public constructor() end constructor; end T; "
        "class C public method Take(owned w: T) "
        "  var b := move w; "
        "  b.Dispose(); "
        "end Take; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}