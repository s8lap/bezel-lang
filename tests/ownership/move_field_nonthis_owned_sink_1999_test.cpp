// Spec-derived tests for issue #1999 — `C.Sink(move recv.f!)` / `new
// T(move recv.f!)` where `recv` is a NON-`this` receiver and `f` is an owned
// field, in the bare-statement DISCHARGE walk (handleDischarge), double-freed at
// runtime (heap-use-after-free).
//
// §28.8.1 (§28.8 producers table, v0.197): `move obj.f` where `f` is `owned`
// produces an Owned value and "the field is left in moved-from state and must be
// reassigned before the surrounding method exits." In argument position the move
// runs during argument evaluation, so restoration can only follow the call — and
// a method call is throwable, so the follow-the-call restoration is the §28.8.1
// "WrongOrder" compile error (the field is moved on the exception edge but never
// reassigned). The compiler enforced this ONLY for a `this` receiver: `move
// this.f!` records the field-move (fieldMovedFrom_, keyed by field name) and the
// method-exit field-restoration check rejects the unrestored bare form. For a
// NON-`this` receiver neither the field-move tracking nor the runtime field-slot
// null-after-move (both `this`-only) fired, so the sink adopted and disposed the
// value AND `recv`'s own disposal (scope-end for a `using var`/owned local, or
// the true owner for a borrow) disposed it again — a real pre-existing double-
// free confirmed on base 741b013 (probe: ASan heap-use-after-free in
// bz$Box$DisposeCore). See issue #1999.
//
// The fix (handleDischarge only): for a stable NON-`this` identifier receiver
// whose member is an owned field, reject the argument-position field move — the
// same net rejection the `this` arm produces, reached for a non-`this` receiver
// via a direct §28.8.1/§28.8 diagnostic (the restoration machinery is `this`-
// only). The spec-sanctioned way to hand a non-`this` object's owned field to a
// sink is a take-method that transfers the field out through `move this.f`
// (§28.6 / §23 "an explicit method that returns the owned value") — pinned below
// as an ASan-clean, disposes-exactly-once positive control.
//
// Fail-without (base 741b013): every rejection case below COMPILES (silent
// accept) and the owned/borrow receiver forms heap-use-after-free at runtime.
// Pass-with: rejected at compile time (no runtime, no UAF). The positive
// transfer-method control disposes exactly once (Cell.live == 0) both before and
// after — the fix does not over-reject the sound path.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a double-free/double-dispose drives it negative or aborts under ASan
// (heap-use-after-free) — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// #1999 — a NON-`this` owned-field move in a bare-statement discharge is rejected.
// ===========================================================================

// §28.8.1 / §28.8: `C.Sink(move box.f!)` where `box` is an owned `using var`
// receiver and `f` an owned field. Fail-without: accepted → box.f disposed by
// the sink AND by box's scope-end disposal (ASan heap-use-after-free in
// bz$Box$DisposeCore on base). Pass-with: rejected at compile time.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           using_var_receiver_field_move_into_owned_param_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    C.Sink(move box.f!); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of `box`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "in this argument position"));
}

// §28.8.1: the same hazard with a plain owned `var` receiver (not a `using var`).
// The field is left moved-from and box's eventual disposal double-frees it.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           plain_owned_var_receiver_field_move_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    var box: Box := new Box(); "
        "    C.Sink(move box.f!); "
        "    using var keep := box; "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of `box`"));
}

// §28.8.1: an `owned` PARAMETER receiver — `b` is owned by the callee and
// auto-dropped at method exit, so moving `b.f` out and passing it to a sink
// double-frees at the auto-drop. Rejected.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           owned_param_receiver_field_move_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Take(owned b: Box) C.Sink(move b.f!); end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of `b`"));
}

// §28.8 / §28.6: a BORROW receiver — `b` is a non-`owned` (borrow) parameter. A
// borrower does not own the object and cannot transfer its owned fields out; the
// field's real owner disposes it, so the sink disposes it a second time. Also a
// pre-existing silent double-free on base (sibling of #1999) — now rejected with
// the borrow-specific diagnostic.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           borrow_receiver_field_move_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Take(b: Box) C.Sink(move b.f!); end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "`b` is a borrow"));
}

// §28.8: the CONSTRUCTOR-argument sink form `new Holder(move box.f!)` — reached
// through handleDischarge's NewExpr `move` arm — has the identical hazard and is
// rejected the same way.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           new_ctor_arg_field_move_from_owned_receiver_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class Holder "
        "  public owned c: Cell; "
        "  public constructor(owned c: Cell) this.c := c; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Consume(owned h: Holder) using var k := h; end Consume; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    C.Consume(new Holder(move box.f!)); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of `box`"));
}

// §28.8.1: the non-force-unwrap form `move box.f` on a NON-nullable owned field
// (CallExpr non-`!` `move` arm) is the same hazard and is likewise rejected.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           non_bang_field_move_from_owned_receiver_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    C.Sink(move box.f); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of `box`"));
}

// ---------------------------------------------------------------------------
// Not-a-regression: the `this` arm is unchanged, and the sound spec pattern
// still compiles and disposes exactly once.
// ---------------------------------------------------------------------------

// §28.8.1: `C.Sink(move this.f!)` in the same argument position was ALREADY
// rejected — by the field-restoration check (the field is left moved-from at
// method exit). Pin it so the #1999 fix, which reproduces that rejection for a
// non-`this` receiver, does not disturb the `this` path. // per §28.8.1
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           this_field_move_still_rejected_by_restoration_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Give() C.Sink(move this.f!); end Give; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Sink(owned c: Cell) using var k := c; end Sink; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "left in moved-from state at method exit"));
}

// §28.6 / §23: the spec-sanctioned transfer — a take-method that moves the field
// out through `move this.f` (which the compiler nulls) and returns it — compiles
// and runs ASan-clean, disposing the Cell exactly once (Cell.live == 0). Proves
// the #1999 fix does not over-reject the sound path (the `box.Take()!`
// producer argument is a call, not a bare `recv.f` place, so it is not flagged).
// Spelled WITHOUT `move` (narrow #5158, Appendix A.7 / §28.8.1, v0.310): a call
// result is already an Owned value and is none of A.7's three admitted MoveExpr
// operand shapes (an owned field, an owning-collection slot, or a bare local/
// owned-parameter identifier), so `move` here is not itself an admitted form —
// the take-method's return is passed on unmarked.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           transfer_method_pattern_disposes_once_spec_28_6) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Take(): owned Cell? "
        "    var t: Cell? := move this.f!; "
        "    this.f := null; "
        "    return t!; "
        "  end Take; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    C.Sink(box.Take()!); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Not-a-regression: an ordinary owned-LOCAL move into a sink — `move x!` where
// `x` is a `Cell?` local, NOT a field of another object — is untouched by the
// #1999 fix (the move source is a bare identifier, not a `recv.f` member access)
// and disposes exactly once. Guards the fix's scoping to member-access sources.
BEZEL_TEST(move_field_nonthis_owned_sink_1999,
           owned_local_move_into_sink_unaffected_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() var x: Cell? := new Cell(); C.Sink(move x!); end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
