// Spec-derived test for the rule-corpus row s28_3_0006
// (tests/conformance/rules/cases/s28_3_0006.bzl) — §28.3 "State match on
// reassignment" (docs v0.307):
//
//   "Reassignment to an existing `var x` is state-matched: an owned-state
//   LHS requires an owned RHS — a fresh producer, or a named owned binding
//   under the explicit transfer marker (`x := move y;`) ... Assigning a
//   borrow to an owned-state LHS ... is rejected — the mismatch would
//   silently drop or double-hold a disposal obligation."
//
// `MethodOwnership::handleAssignment`'s general (unmarked) `:=` path never
// checked this: `best := next;`, over two owned locals, unconditionally ran
// `st.state = producerState(*as.value);` — reclassifying `best` as Borrowed
// with no diagnostic and no disposal, while `next` stayed a live owner too.
// This is the "double-ownership" hazard issue #1538 recorded and left open
// ("today `b := a` with owned `b`, owned `a` compiles as drop-on-overwrite +
// borrow while `a` stays owned"); the marker forms (`:= owned x` / `:= move
// x`) were already implemented and are the accepted controls below.
//
// See also instance_fresh_owned_return_s28_3_0029_0039_test.cpp for issue
// #1538's other still-open half, at the RETURN site rather than the
// reassignment site.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The state-match diagnostic this fix adds.
bool rejectsStateMatchBorrowIntoOwned(const OwnershipResult& r,
                                      std::string_view targetName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("state-match rule") == std::string::npos)
            continue;
        if (d.message.find("cannot assign a borrow to") == std::string::npos)
            continue;
        if (d.message.find(std::string(targetName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — assigning a borrow into an owned-state local, unmarked.
// =============================================================================

// The corpus case's exact shape: `best := next;` over two plain-`var` owned
// locals. `next` is a bare (unmarked) read — never a consume, §28.3's
// no-silent-consume rule — so it reaches the assignment as a BORROW while
// `best` is currently Owned: state mismatch, rejected.
BEZEL_TEST(instance_state_match_reassign_0006, borrow_into_owned_local_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public method Demo() "
        "    var best := new Widget(); "
        "    var next := new Widget(); "
        "    best := next; "
        "  end Demo; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsStateMatchBorrowIntoOwned(r, "best"));
}

// A borrowed PARAMETER assigned into an owned-state local is the identical
// mismatch through a different RHS source.
BEZEL_TEST(instance_state_match_reassign_0006, borrow_parameter_into_owned_local_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public method Demo(p: Widget) "
        "    var best := new Widget(); "
        "    best := p; "
        "  end Demo; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsStateMatchBorrowIntoOwned(r, "best"));
}

// =============================================================================
// POSITIVE — no over-rejection: the marker forms and non-Owned targets.
// =============================================================================

// The corpus's own sibling accept case (s28_3_0007): `best := move next;`
// is the explicit transfer marker form the rule's remedy names — admitted,
// disposing the displaced value by drop-on-overwrite.
BEZEL_TEST(instance_state_match_reassign_0006, marked_move_into_owned_local_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public method Demo() "
        "    var best := new Widget(); "
        "    var next := new Widget(); "
        "    best := move next; "
        "    best.Dispose(); "
        "  end Demo; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A borrow assigned into a POLYMORPHIC-NULL-state local (not yet Owned) is
// the ordinary accumulator idiom's first iteration — unaffected by this fix,
// which gates strictly on the LHS's CURRENT state being Owned.
BEZEL_TEST(instance_state_match_reassign_0006, borrow_into_null_state_local_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public method Demo(p: Widget) "
        "    var cur: Widget? := null; "
        "    cur := p; "
        "  end Demo; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Reassigning an already-BORROWED-state local to a different borrow is a
// same-state match — unaffected.
BEZEL_TEST(instance_state_match_reassign_0006, borrow_into_borrowed_state_local_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program "
        "  public method Demo(p: Widget, q: Widget) "
        "    var cur := p; "
        "    cur := q; "
        "  end Demo; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
