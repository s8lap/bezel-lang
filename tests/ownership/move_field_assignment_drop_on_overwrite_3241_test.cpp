// Spec-derived regression tests for #3241 — §28.8.1 "Exception paths are not
// exempt" (R4b): the drop-on-overwrite clause of the non-throwable assignments
// list.
//
// §28.8.1's non-throwable list (v0.254, verbatim):
//
//   "assignments where neither the LHS nor the RHS is a throwable expression
//    **and** the assignment performs no drop-on-overwrite of a possibly-live
//    value: a store into an `owned` field, a plain-`var` owned local, or an
//    owning-collection slot that may hold a live (non-null, not moved-from)
//    value is **throwable** — the implicit §28.7 disposal runs the displaced
//    value's destructor, which may throw with defined behavior (§28.4),
//    propagating from the drop-on-overwrite site. Stores into provably
//    moved-from or null locations, and stores into borrow-state or value-typed
//    locations, remain non-throwable — which is why this section's
//    restoration idioms are unaffected (`_resource := fresh;` after a `move`
//    targets a moved-from slot)."
//
// WHAT WAS BROKEN (R4b, measured on 1280641). The R1 invasive fix wired
// `findThrowable28_8_1` into `checkThrowableWhileFieldMovedFrom` and the
// post-`try` deferred fire, but the throwable inventory it walks has no
// clause for the assignment AS A WHOLE — only for its RHS/LHS sub-expressions.
// `_other := new Res();` between a move and the restore is therefore neither
// classified by the inventory walk (its RHS is `new Res()`, not in the
// inventory per v0.255 / #3240) nor by any LHS-side check the predecessor
// R1–R3 wiring had. The accepted program is not merely mis-typed — built and
// run, the displaced `_other`'s destructor takes the exception edge before
// the restore of `_r` runs, and the holder's destructor cascade observes
// `_r` in moved-from state on the unwinding edge (the spec's "the hole a
// destructor must never observe"). Same shape as the #3080 WrongOrder
// example, but the throwable is the implicit §28.7 disposal of the
// drop-on-overwrite at `_other`, not a call or division.
//
// Each test below names the §28.8.1 clause it derives from. The rejection
// tests assert the DIAGNOSTIC TEXT, not merely `hasErrors()` — per #2988 a
// bare `hasErrors()` passes on an unrelated error (an unknown member, say)
// and so does not witness the rule it claims.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §28.8.1 exception-edge diagnostic's stable identifier: the §-reference
// plus the section heading it quotes. Mirrors the #3080 test's constant so
// the diagnostic-text contracts of the two fixes sit side-by-side.
constexpr const char* kEdgeDiag = "§28.8.1 \"Exception paths are not exempt\"";

// Two owned fields: `_r` is the one the test moves and restores via the
// field-restoration rule; `_other` is the live owned slot whose store
// (the bug) drops a live value and is therefore throwable. The classic
// #3080 WrongOrder contract is preserved — the constructor seeds both
// fields so the move out of `_r` is well-defined and the store into `_other`
// is over a LIVE owned value, never a moved-from one.
std::string prog(const std::string& members) {
    return std::string(
        "module M; "
        "class Res "
        "  public Tag: int := 7; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  private owned _r: Res; "
        "  private owned _other: Res; "
        "  public constructor() _r := new Res(); _other := new Res(); "
        "  end constructor; "
        "  private static method Dangerous(): int return 1; end Dangerous; ") +
        members + " end Holder;";
}

// True when some ERROR-severity diagnostic carries `needle` AND names a real
// source location. A diagnostic reported at 0:0 is unusable to a reader and
// means the code that named the location was not the code that found the
// problem.
bool hasLocatedError(const bezel::test::OwnershipResult& r,
                     const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) == std::string::npos) continue;
        if (d.location.line == 0 && d.location.column == 0) continue;
        return true;
    }
    return false;
}

} // namespace

// --- The rule: a non-throwable-RHS assignment that displaces a live value ---
//                                                 is throwable (§28.8.1) ---

// The issue's verbatim shape: move `_r`, then `_other := new Res();` (the
// drop-on-overwrite that drops a LIVE `_other`), then restore `_r` and
// return. The implicit §28.7 disposal of the displaced `_other` may throw,
// so the slot is throwable per §28.8.1's drop-on-overwrite clause, and the
// exception edge is an unrestored exit on `_r`. This is the R4b test it
// requested verbatim.
//                                         // per §28.8.1, #3241 R4b
BEZEL_TEST(own_assign_drop_on_overwrite_3241,
           two_live_owned_fields_drop_on_overwrite_creates_exception_edge_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method Two(): owned Res "
        "  var old := move _r; "
        "  _other := new Res(); "
        "  _r := new Res(); "
        "  return old; "
        "end Two;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "owned field `_r`"));
    BEZEL_EXPECT_TRUE(hasLocatedError(r, kEdgeDiag));
}

// Same shape, but the move-and-displace pair is in the other order: the
// displacement of `_other` runs BEFORE the move of `_r`. The second drop is
// still a drop-on-overwrite of a live owned value (`_other` was assigned
// `new Res()` immediately above, so it is live again at the second
// `_other := new Res();`). The field-restoration rule trips on `_r`'s
// restoration point just as in the first test.
BEZEL_TEST(own_assign_drop_on_overwrite_3241,
           assign_before_move_still_creates_exception_edge_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method Two(): owned Res "
        "  _other := new Res(); "
        "  var old := move _r; "
        "  _other := new Res(); "
        "  _r := new Res(); "
        "  return old; "
        "end Two;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}

// The qualifier spelling `this._other := new Res();` is the same drop-on-
// overwrite of a live owned field — the R4b predicate looks at the field
// NAME, not at the receiver spelling.
BEZEL_TEST(own_assign_drop_on_overwrite_3241,
           qualified_this_other_drop_on_overwrite_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method Two(): owned Res "
        "  var old := move _r; "
        "  this._other := new Res(); "
        "  _r := new Res(); "
        "  return old; "
        "end Two;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}

// --- The accepted forms: the rule's non-throwable subclasses stay accepted -

// §28.8.1 (v0.254): "Stores into provably moved-from or null locations, and
// stores into borrow-state or value-typed locations, remain non-throwable —
// which is why this section's restoration idioms are unaffected (`_resource
// := fresh;` after a `move` targets a moved-from slot)." The standard
// restoration idiom `var old := move _r; _r := new Res(); return old;` is
// precisely the case where the LHS is a moved-from slot between the move
// and the restore, and it must compile clean. Without this guard the
// R4b fix would reject the section's own `Right` example.
BEZEL_TEST(own_assign_drop_on_overwrite_3241,
           right_restore_into_moved_from_slot_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method Right(): owned Res "
        "  var old := move _r; "
        "  _r := new Res(); "
        "  return old; "
        "end Right;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A non-`owned` (borrow) FIELD is a non-throwable subclass on its own
// ("stores into borrow-state or value-typed locations remain non-throwable").
// `_br: Res` (the un-modified branch) is a borrow READ of the receiver's
// field — a store into it cannot drop a live owned value because the field
// does not own the value. A drop-on-overwrite between the move and the
// restore of `_r` is therefore not throwable on its own account. The borrow
// source is `b` (a borrowed parameter), so the borrow store is the legal
// shape rather than a #1020 fresh-leak ("a freshly-produced owned value
// stored into a non-`owned` field has no owner and leaks").
BEZEL_TEST(own_assign_drop_on_overwrite_3241,
           drop_on_overwrite_borrow_field_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(
            "module M; "
            "class Res "
            "  public Tag: int := 7; "
            "  destructor end destructor; "
            "end Res; "
            "class Holder "
            "  private owned _r: Res; "
            "  private _br: Res; "
            "  public constructor(b: Res) "
            "    _r := new Res(); _br := b; "
            "  end constructor; "
            "  public method Two(b: Res): owned Res "
            "    var old := move _r; "
            "    _br := b; "
            "    _r := new Res(); "
            "    return old; "
            "  end Two; "
            "end Holder;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A throwable RHS already trips the rule via the inventory walk — the new
// drop-on-overwrite predicate is the additional, LHS-side clause. The
// existing #3080 fix's `WrongOrder` shape must keep firing. This is the
// over-rejection guard for the reroute: the inventory walk still runs
// AT, and the LHS-side check is additive.
BEZEL_TEST(own_assign_drop_on_overwrite_3241,
           rhs_call_still_throwable_via_inventory_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method WrongOrder(): owned Res "
        "  var old := move _r; "
        "  var q := Dangerous(); "
        "  _r := new Res(); "
        "  return old; "
        "end WrongOrder;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}
