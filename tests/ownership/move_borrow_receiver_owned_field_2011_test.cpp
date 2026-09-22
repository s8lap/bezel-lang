// Spec-derived tests for issue #2011 — moving an owned field OUT OF A BORROW
// RECEIVER into an owned sink (or as an owned return) must be REJECTED. Before
// the fix the owned-source classifier accepted it, and the value was disposed
// twice (a double-free / heap-use-after-free).
//
// §28.6 / §28.8 / §28.8.1: `move recv.f` reads an owned field `f` as an owned
// value ONLY when the RECEIVER `recv` is an owned place — the class instance
// itself (`this`, which owns its fields, §28.8.1) or an owned local / using-var /
// `owned` parameter this scope owns. A BORROW receiver (a non-`owned` parameter,
// a catch-bound exception §31.1, a Borrowed-state local, or a nested field read —
// itself a borrow, §28.3) does NOT own the object, so it cannot transfer the
// object's owned fields out: the field's real owner disposes the value and the
// owned sink would dispose it a SECOND time — a double-free (§28.6 "a borrow does
// not own the object").
//
// The bug (#2011): the owned-source classifier `ownedArgSourceIsBorrow`, and the
// return-position twin `checkOwnedReturnBorrowSource` (#1995 part A), EXCLUDED
// every owned-field read (`exprIsOwnedFieldRead`) from the borrow classification
// WITHOUT checking the field's receiver — so `move b.f!` off a BORROW receiver `b`
// was treated as the legal owned-field transfer (as if it were `move this.f`) and
// slipped through. Both the `return move b.f!` position and the classifier itself
// (consulted by the argument-sink / `throw`-operand paths) accepted it.
//
// The fix (Ownership_Checks.cpp, sema only): `ownedFieldMoveReceiverIsBorrow`
// classifies the receiver of an owned-field read. Both exclusion sites now accept
// the owned-field read ONLY when the receiver is an owned place (`this`, an owned
// local / `owned` param); a provably-borrow receiver is REJECTED. Positive-
// rejection only, so the legal `move this.f` / `move _f` / `move ownedBox.f!` are
// NOT over-rejected.
//
// Fail-without / pass-with: each REJECT case below compiled before the fix (the
// classifier accepted it; a non-identifier borrow receiver double-freed at
// runtime under the AOT+ASan oracle — see moveBorrowReceiverRuntimeUAF) and is a
// compile error after it. Each ACCEPT case disposes the value exactly once
// (Cell.live == 0 under the AOT+ASan oracle) — the owned-receiver forms are not
// newly rejected.

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

// The #1995 return-position diagnostic's stable substring (the return form of the
// #2011 fix routes through it).
constexpr std::string_view kBorrowReturn = "cannot return a borrow as an `owned`";
// The #1999 argument-sink discharge diagnostic's stable substring.
constexpr std::string_view kBorrowField = "cannot move owned field";

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a double-free drives it negative or aborts under ASan — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

// A Box that owns a Cell field `f`.
const char* kBox =
    "class Box "
    "  public owned f: Cell? := null; "
    "  public constructor() this.f := new Cell(); end constructor; "
    "  destructor end destructor; ";

}  // namespace

// ===========================================================================
// REJECT — `return move b.f!` off a BORROW receiver through an `owned` return.
// Fail-without: accepted (classifier excluded the owned-field read regardless of
// receiver). Pass-with: rejected at compile time (the borrow-return diagnostic).
// ===========================================================================

// §28.6 / §28.8.1: `return move b.f!` where `b` is a BORROW (non-`owned`)
// parameter and `f` an owned field — the issue's exact return repro.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           return_move_borrow_param_field_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Steal(b: Box): owned Cell return move b.f!; end Steal; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §28.6 / §28.8.1: the `=>` expression-body form of the return field-move off a
// borrow receiver — the body IS the returned value, same classification.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           exprbody_move_borrow_param_field_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Steal(b: Box): owned Cell => move b.f!; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §28.6 / §28.8.1: a Borrowed-state LOCAL receiver. `bl` is bound to a borrow
// parameter (a borrow of a borrow is still a borrow), so `move bl.f!` moves an
// owned field out of a place this scope does not own — reject.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           return_move_borrow_local_field_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Steal(b: Box): owned Cell "
        "    var bl := b; return move bl.f!; "
        "  end Steal; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §31.1 / §28.6: a CATCH-bound exception is a borrow whose lifetime is the arm.
// Moving an owned field out of it and returning it as `owned` double-disposes —
// reject. Uses a user exception type carrying an owned field.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           return_move_catch_binding_field_rejected_spec_31_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Boom(msg: string): Exception(msg) "
        "  public owned f: Cell? := null; "
        "  destructor end destructor; "
        "end Boom; "
        "class C "
        "  public static method Bang() throw new Boom(\"x\"); end Bang; "
        "  public static method Grab(): owned Cell "
        "    try C.Bang(); catch var ex: Boom => return move ex.f!; end; "
        "    return new Cell(); "
        "  end Grab; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// ===========================================================================
// REJECT — the ARGUMENT-sink (bare-statement discharge) form `C.Sink(move b.f!)`
// off a borrow receiver. Already rejected pre-#2011 by the #1999 discharge walk;
// pinned here as a regression guard (the classifier fix keeps it rejected).
// ===========================================================================

// §28.8 / §28.6: `C.Sink(move b.f!)` into an `owned` parameter, `b` a borrow.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           argsink_barestmt_borrow_param_field_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Take(b: Box) C.Sink(move b.f!); end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowField));
}

// ===========================================================================
// ACCEPT — an owned-RECEIVER field-move is NOT over-rejected, and disposes the
// value EXACTLY once (Cell.live == 0 under the AOT+ASan oracle).
// ===========================================================================

// §28.8.1: `return move box.f!` where `box` is an OWNED using-var local — the
// legal owned-field transfer out of an owned place. #2002 nulls the slot; the
// caller disposes the returned value once. Guards against over-rejection.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           return_move_owned_local_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Make(): owned Cell "
        "    using var box := new Box(); return move box.f!; "
        "  end Make; "
        "  public static method Main(): int "
        "    do using var c := C.Make(); end; return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `return move this.f!` — the take-method pattern (this-receiver owned
// field), the field reassigned to null before exit. The owner disposes nothing
// (field nulled); the caller disposes the returned value once. Guards against the
// fix mis-classifying a `this`-receiver owned-field move as a borrow. The
// trailing `return r;` is marker-free per §28.3 K3.13 / K3.2 (v0.307); the
// `move this.f!` owned-field read it pins keeps its marker.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           return_move_this_field_take_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Take(): owned Cell var r := move this.f!; this.f := null; return r; end Take; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    do using var box := new Box(); do using var c := box.Take(); end; end; "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `C.Sink(move this.f!)` — the this-receiver owned-field move into an
// owned parameter, the field reassigned before exit. Guards against the fix
// mis-classifying the this-receiver arg-sink field move as a borrow.
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           argsink_move_this_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Give() C.Sink(move this.f!); this.f := null; end Give; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Main(): int "
        "    do using var b := new Box(); b.Give(); end; return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// NON-REGRESSION — the #1995 owned-field-return controls must still pass: a
// `this`-receiver owned-field return (`move this.f!` / `move _f!`) must NOT trip
// the borrow-return diagnostic the #2011 fix now applies to BORROW receivers.
// ===========================================================================
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           this_receiver_owned_field_return_not_flagged_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method TakeBare(): owned Cell return move f!; end TakeBare; "
        "  public method TakeThis(): owned Cell return move this.f!; end TakeThis; "
        "  destructor end destructor; "
        "end Box;");
    // The this-receiver owned-field move is not the #2011 borrow-receiver hazard.
    BEZEL_EXPECT_FALSE(errorSays(r, kBorrowReturn));
}

// ===========================================================================
// RUNTIME UAF — a NON-identifier BORROW receiver (`o.inner.f`, whose receiver
// `o.inner` is itself a borrow field read) defeats the #2002 slot-nulling
// (nulling fires only for a stable identifier receiver), so on base the value is
// disposed twice: an ASan heap-use-after-free. The #2011 fix rejects it at
// compile time, so with the fix this program never runs — assert the compile
// error rather than a runtime exit code. Fail-without: base compiled and aborted
// under ASan. Pass-with: compile error (kBorrowReturn).
// ===========================================================================
BEZEL_TEST(move_borrow_receiver_owned_field_2011,
           nested_borrow_receiver_field_rejected_spec_28_6) {
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
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Steal(o: Outer): owned Cell return move o.inner.f!; end Steal; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}
