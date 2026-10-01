// Spec-derived regression tests for issue #2005 — `C.Sink(move C.GetBox().f!)` /
// `new T(move C.GetBox().f!)` where the receiver of an owned-field move is a
// TEMPORARY (a call result / fresh value, not a stable IdentifierExpr) leaked
// 24 bytes under LSan (the temporary Box itself was never disposed after the
// field was extracted).
//
// §28.8.1 (the move expression / field-restoration rule, v0.197): `move x.f`
// transfers ownership and leaves `x.f` moved-from; the field-restoration rule
// requires every path to the method's exit to REASSIGN `x.f` before exit. For a
// TEMPORARY receiver (`getBox().f`) restoration is structurally impossible —
// there is no stable place to reassign into (a second `getBox()` call would
// mint a fresh Box, not the moved-from one). Combined with the runtime null-
// after-move + `fieldMovedFrom_` restoration tracking being `this`-only (and
// #2002's non-`this` identifier-receiver slot null requires a stable receiver
// slot), there is no sound way to consume the move; and codegen does not emit
// cleanup for the temporary after extracting its field, so the temporary itself
// leaks — the 24-byte LSan hit on base (bz$Box$GetBox allocation, issue #2005).
//
// The fix: REJECTION, mirroring the #1999 stable-identifier-receiver rejection.
// `handleDischarge`'s three `move` arms (CallExpr `!`, CallExpr non-`!`, NewExpr)
// now call `rejectNonThisOwnedFieldMoveDischarge` not only for an IdentifierExpr
// receiver but also when the receiver is an arbitrary expression (a temporary).
// The diagnostic is tailored for the temporary case (no `recvName`, names the
// leak + the take-method remedy).
//
// Fail-without (base fc6f800): every rejection case below COMPILES (silent
// accept) and the program runs with a 24-byte LSan leak (runAotChecked →
// ran=false). Pass-with: rejected at compile time (no runtime, no leak). The
// positive transfer-method control disposes exactly once (Cell.live == 0) both
// before and after — the fix does not over-reject the sound path.

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
// a leak leaves live > 0; a double-dispose drives it negative or aborts under
// ASan (heap-use-after-free) — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// #2005 — a TEMPORARY-receiver owned-field move in a bare-statement discharge
// is rejected. Fail-without: accepted → 24-byte LSan leak (the temporary Box
// never disposed). Pass-with: rejected at compile time.
// ===========================================================================

// §28.8.1 / §28.8 (#2005): `C.Sink(move Box.GetBox().f!)` — the canonical repro
// from the issue. A static factory returns `owned Box`; the bare-statement
// discharge moves Box's owned field `f` into the owned-sink parameter. Without
// the fix: silent accept + 24-byte LSan leak. With the fix: rejected.
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           call_result_force_unwrap_field_move_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public static method GetBox(): owned Box return new Box(); end GetBox; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    C.Sink(move Box.GetBox().f!); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of a temporary receiver"));
    BEZEL_EXPECT_TRUE(errorSays(r, "the temporary leaks"));
}

// §28.8.1: an INSTANCE-method producer (`move box.GetBox().f!`) — same hazard.
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           instance_method_temporary_receiver_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method GetBox(): owned Box return new Box(); end GetBox; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    C.Sink(move box.GetBox().f!); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of a temporary receiver"));
}

// §28.8.1: the non-force-unwrap form `move Box.GetBox().f` (a non-nullable
// owned field) — same hazard, same rejection (CallExpr non-`!` `move` arm).
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           non_bang_temporary_receiver_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public static method GetBox(): owned Box return new Box(); end GetBox; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    C.Sink(move Box.GetBox().f); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of a temporary receiver"));
}

// §28.8 / §28.8.1: the CONSTRUCTOR-argument sink form `new Holder(move
// Box.GetBox().f!)` — reached through handleDischarge's NewExpr `move` arm —
// identical hazard, identical rejection.
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           new_ctor_arg_temporary_receiver_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public static method GetBox(): owned Box return new Box(); end GetBox; "
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
        "    C.Consume(new Holder(move Box.GetBox().f!)); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of a temporary receiver"));
}

// §28.8.1 (#2005 nested-receiver variant): a CHAINED member access
// (`move this.outer.f!` where `outer` is itself a field) is structurally a
// temporary-receiver move too (the receiver `this.outer` is a MemberAccessExpr,
// not an IdentifierExpr) and has the same no-stable-slot hazard. Rejected.
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           chained_member_access_temporary_receiver_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Inner "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Inner; "
        "class Outer "
        "  public owned inner: Inner; "
        "  public constructor() this.inner := new Inner(); end constructor; "
        "  public method Leak() C.Sink(move this.inner.f!); end Leak; "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of a temporary receiver"));
}

// ---------------------------------------------------------------------------
// Fail-without (LSan) confirmation: the canonical repro COMPILES on base and
// runs with a 24-byte LSan leak (so runAotChecked sets ran=false under the
// generated-code ASan/LSan oracle). Pass-with: rejected at compile time, so the
// program never runs (ran=false from the compile failure, NOT from a leak).
// This case asserts the COMPILE-TIME rejection directly (no runtime path).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Not-a-regression: the #1999 stable-identifier path, the #2002 local-decl
// path, the `this`-field path, and the sound take-method transfer still work.
// ---------------------------------------------------------------------------

// §28.8.1 (#1999 not-a-regression): a STABLE identifier receiver's owned-field
// move is still rejected with the #1999 diagnostic (NOT the new temporary-
// receiver diagnostic) — the fix does not collapse the two paths.
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           stable_identifier_receiver_keeps_1999_diagnostic_spec_28_8_1) {
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
    // The stable-identifier diagnostic — NOT the temporary-receiver one.
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of `box`"));
    BEZEL_EXPECT_TRUE(!errorSays(r, "out of a temporary receiver"));
}

// §28.6 / §23: the spec-sanctioned transfer — bind the producer to a stable
// place, take the field out through a `move this.f` take-method, pass the
// result. Compiles and runs ASan-clean, disposing the Cell exactly once
// (Cell.live == 0). Proves the #2005 fix does not over-reject the sound path.
// Spelled WITHOUT `move` at the sink argument (narrow #5158, Appendix A.7 /
// §28.8.1, v0.310): `box.Take()` is a call result, already Owned, and is none
// of A.7's three admitted MoveExpr operand shapes (an owned field, an owning-
// collection slot, or a bare local/owned-parameter identifier) — so it is
// passed on unmarked.
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           take_method_pattern_disposes_once_spec_28_6) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public static method GetBox(): owned Box return new Box(); end GetBox; "
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
        "    using var box := Box.GetBox(); "
        "    C.Sink(box.Take()!); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1 (#2002 not-a-regression): a LOCAL-DECL `var t := move Box.GetBox().f!`
// (NOT a sink discharge) — the local-decl path routes through recordMovesIn and
// is not in this fix's scope (it has its own deferred-TODO for temporary
// receivers). Pinning the unchanged behavior: it currently compiles (whether it
// leaks is a separate, deferred concern under #2002's local-decl path; the
// bare-statement discharge form — the actual #2005 repro — is closed here).
BEZEL_TEST(move_field_temporary_receiver_sink_2005,
           local_decl_temporary_receiver_unaffected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public static method GetBox(): owned Box return new Box(); end GetBox; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Use() "
        "    var t: Cell? := move Box.GetBox().f!; "
        "    using var k := t; "
        "  end Use; "
        "end C;");
    // This is OUT OF SCOPE for #2005 (which targets the discharge walk); we
    // assert only that the #2005 temporary-receiver diagnostic does NOT fire
    // here (the local-decl path is a separate fix's territory).
    BEZEL_EXPECT_TRUE(!errorSays(r, "out of a temporary receiver"));
}
