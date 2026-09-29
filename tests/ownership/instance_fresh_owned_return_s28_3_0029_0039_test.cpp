// Spec-derived tests for the rule-corpus rows s28_3_0029 / s28_3_0039
// (tests/conformance/rules/cases/) — §28.3's announcement table, row 4
// (docs v0.307):
//
//   "Instance/extension: return only a receiver-(parameter-)sourced borrow;
//   a fresh owned value is rejected (§28.6) — declare `owned`."
//
// `checkInstanceBorrowReturn` (src/ownership/Ownership.cpp) already enforced
// the "wrong-source borrow" half of row 4 (issue #1885: a borrow reached
// through a plain parameter, rejected) but its own guard —
// `if (!ownedArgSourceIsBorrow(e)) return;` — early-returned for EVERY shape
// `ownedArgSourceIsBorrow` does not affirmatively classify as a borrow,
// which includes a genuine fresh owned production: a direct `new` expression
// (s28_3_0039) and a bare read of a tracked local currently in the OWNED
// state (s28_3_0029). Both compiled clean with ZERO ownership diagnostics
// before this fix — measured on the corpus cases directly (FAIL-WITHOUT: both
// rows read UNBANKED/pass-when-banked-reject on the pre-fix binary; green
// on the fix).
//
// This is issue #1538's other still-open half ("reject owned-LHS := borrow")
// sibling defect at the RETURN site rather than the reassignment site — the
// reassignment half is still open; s28_3_0006 is pinned to #4842/#4846.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.3 announcement table row 4's fresh-owned-return diagnostic.
bool rejectsFreshOwnedInstanceReturn(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("receiver-sourced borrow") == std::string::npos)
            continue;
        if (d.message.find("fresh owned value is rejected") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — a fresh owned value returned through a plain (non-`owned`)
// instance class-typed return is rejected.
// =============================================================================

// s28_3_0039's exact shape: a direct `new` expression returned straight
// through a non-`owned` instance method return.
BEZEL_TEST(instance_fresh_owned_return_0029_0039, direct_new_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Factory "
        "  public method Make(): Widget "
        "    return new Widget(); "
        "  end Make; "
        "end Factory;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsFreshOwnedInstanceReturn(r));
}

// s28_3_0029's exact shape: a fresh value bound to a local first, then
// returned unmarked (a bare read never consumes, so this is still a fresh
// owned production reaching the return, not a `move`).
BEZEL_TEST(instance_fresh_owned_return_0029_0039, local_bound_new_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public method Peek(): Widget "
        "    var w := new Widget(); "
        "    return w; "
        "  end Peek; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsFreshOwnedInstanceReturn(r));
}

// The `=>` expression-body form is checked by the same rule
// (runExprBodyOwningReturnCheck), so `=> new Widget();` is rejected
// identically to the block-body `return new Widget();`.
BEZEL_TEST(instance_fresh_owned_return_0029_0039, expression_body_new_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Factory "
        "  public method Make(): Widget => new Widget(); "
        "end Factory;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsFreshOwnedInstanceReturn(r));
}

// =============================================================================
// POSITIVE — no over-rejection.
// =============================================================================

// Declaring the return type `owned` is exactly the remedy the diagnostic
// names — the same fresh production is admitted once the transfer is
// announced.
BEZEL_TEST(instance_fresh_owned_return_0029_0039, owned_return_type_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Factory "
        "  public method Make(): owned Widget "
        "    return new Widget(); "
        "  end Make; "
        "end Factory;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A receiver-sourced borrow (a bare owned-field read) is unaffected — the
// admitted half of row 4 stays admitted.
BEZEL_TEST(instance_fresh_owned_return_0029_0039, receiver_owned_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Holder "
        "  public owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public method Get(): Widget => _w; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `move`d local IS an announced transfer via the caller's own binding —
// but the receiving return type here is still not `owned`, so this remains a
// transfer with nowhere to go. This positive control instead checks the
// deliberately UNTOUCHED CallExpr-relay shape (#4009, cited in this fix's
// comment as out of scope): a call to an owned-returning static factory,
// relayed through the instance method's non-`owned` return, is NOT flagged
// by this fix (it is a pre-existing, separately-tracked gap, not something
// this narrowly-scoped fix should newly reject without its own spec-derived
// test). Guards against silently widening the fix's blast radius beyond the
// two corpus rows it targets.
BEZEL_TEST(instance_fresh_owned_return_0029_0039,
          callexpr_relay_not_newly_rejected_4009_scope_guard) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Factory "
        "  public static method Build(): owned Widget return new Widget(); "
        "  end Build; "
        "  public method Relay(): Widget "
        "    return Factory.Build(); "
        "  end Relay; "
        "end Factory;");
    BEZEL_EXPECT_FALSE(rejectsFreshOwnedInstanceReturn(r));
}
