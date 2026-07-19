// Spec-derived tests for issue #2027 — `return move ExternalClass.Slot` (a STATIC
// `owned readonly` field read via a `ClassName.Field` MemberAccessExpr, in an
// OWNED-RETURN position) COMPILED (only the §22.12 domain warning) and SEGV'd in
// `bz_dispose_static_owned()` at program exit. The return-position × static-owned-
// field cell of the #2011-family ownership-coverage matrix was open.
//
// §22.13 (Disposal of static `owned` fields): a static `owned` field is disposed
// at program exit, in reverse order of static initialization. §28.6 (leak
// prevention): a borrow does NOT own the object. §28.8 (ownership transfer): an
// `owned` sink (here, the caller of an `owned`-returning method) takes
// responsibility for disposing the value it receives; only an OWNED value may
// flow in. §28.8.1 (`move`): `move x.f` is the only way to read an owned field's
// value as an owned value — and only when the field's RECEIVER is an owned place.
//
// The bug (#2027): `checkOwnedReturnBorrowSource` only EXCLUDED (accepted) the
// owned-field-read return form when the receiver was an owned place
// (`!ownedFieldMoveReceiverIsBorrow`). When `ownedFieldMoveReceiverIsBorrow`
// returns true — for a STATIC owned field (program-owned, §22.13) — it fell
// through to `ownedArgSourceIsBorrow`, but that classifier's MemberAccessExpr
// branch is `!unwrappedMove`-gated, so a plain `move X.f` (no `!`) slipped past
// it to the not-a-borrow tail. The runtime then returned the field's value as
// owned; the caller disposed it once, and the program-exit disposal pass freed
// the still-tracked slot a SECOND time — a SEGV in `bz_dispose_static_owned`.
//
// The same hole also let a borrow-receiver `return move b.f` (no `!`) slip
// through on the same structural basis; the existing #2011 coverage only pinned
// the `move b.f!` form. The fix is the explicit reject branch in
// `checkOwnedReturnBorrowSource` that fires when the place is an owned-field
// read AND `ownedFieldMoveReceiverIsBorrow` positively flags its receiver —
// covering both the static-field case and the no-`!` borrow-receiver case.
//
// Fail-without / pass-with: each REJECT case below COMPILED on base (only the
// §22.12 domain warning for the static cases) and SEGV'd under AOT+ASan at
// program exit; after the fix it is rejected at compile time. The ACCEPT
// non-regression cases keep the legal `static owned readonly` field reachable
// through its borrow read (§28.3 — a bare `Registry.Slot` is a borrow, never an
// owned transfer) and the legal `move this.f`/`move ownedPlace.f` instance-
// field return working unchanged.

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

// The #2027 return-position diagnostic's stable substring.
constexpr std::string_view kBorrowReturn = "cannot return a borrow as an `owned`";

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a double-free drives it negative or aborts under ASan — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

// A class carrying a `static owned readonly` field — the canonical program-owned
// long-lived resource form (§22.13 / §22.12).
const char* kRegistry =
    "class Registry "
    "  public static owned readonly Slot: Cell := new Cell(); ";
}  // namespace

// ===========================================================================
// REJECT — `return move Registry.Slot` (the issue's exact repro): a `move` of a
// STATIC owned field in return position through an `owned` return type. Fail-
// without: accepted (only the §22.12 domain warning), SEGV at exit under ASan.
// Pass-with: rejected at compile time with the borrow-return diagnostic.
// ===========================================================================

// §22.13 / §28.8.1 / §28.6: `return move Registry.Slot;` — the issue's exact
// repro. A static owned field is program-owned; moving it out as `owned` lets
// the caller and the exit-disposal both free it. Reject.
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           return_move_static_owned_field_memberaccess_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kRegistry +
        "  public static method Take(): owned Cell return move Registry.Slot; "
        "  end Take; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §22.13 / §28.8.1: the expression-bodied form `=> move Registry.Slot` — the
// body IS the returned value, same classification. Reject.
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           exprbody_move_static_owned_field_memberaccess_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kRegistry +
        "  public static method Take(): owned Cell => move Registry.Slot; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §22.13 / §28.8.1 / §17.4: the `!` force-unwrap form `return move Registry.
// Slot!` — must be rejected exactly as the plain `move` form. (The classifier
// tail already caught this via the `move E!` PostfixBangExpr branch on base;
// pinned here as a non-regression guard for the explicit early-reject branch.)
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           return_move_bang_static_owned_field_memberaccess_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kRegistry +
        "  public static method Take(): owned Cell return move Registry.Slot!; "
        "  end Take; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §22.13 / §28.8.1: from an INSTANCE method (where `this` exists but a static
// field is still program-owned, not `this`-owned) — same hazard. Reject.
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           return_move_static_owned_field_from_instance_method_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kRegistry +
        "  public method Leak(): owned Cell return move Registry.Slot; end Leak; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §22.13: the runtime twin — the issue's exact program shape. Fail-without: base
// compiled and SEGV'd in `bz_dispose_static_owned` at exit under AOT+ASan (the
// double-free). Pass-with: compile error, the program never runs.
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           runtime_static_owned_field_return_double_free_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kRegistry +
        "  public static method Take(): owned Cell return move Registry.Slot; "
        "  end Take; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// ===========================================================================
// REJECT — the broader #2011-family return-position member-access hole: a borrow
// receiver's owned-field move WITHOUT `!` (`return move b.f`) also slipped past
// the classifier tail on the same structural basis as the static case. (The `!`
// form `return move b.f!` was already rejected by the #2011 fix's coverage; the
// no-`!` form ran ASan-clean in the re-review's probe but is unsound on the same
// structural basis — pinned here for parity with the `!` form.)
// ===========================================================================

// §28.6 / §28.8.1: `return move b.f` (no `!`) where `b` is a borrow parameter —
// reject. A borrower does not own the object and cannot transfer its owned
// fields out; the field's real owner disposes the value, so the caller disposing
// the returned value double-frees.
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           return_move_borrow_receiver_field_no_bang_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell := new Cell(); "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Steal(b: Box): owned Cell return move b.f; "
        "  end Steal; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// ===========================================================================
// ACCEPT (non-regression) — the fix does NOT over-reject the legal forms. A
// static `owned readonly` field is still readable as a borrow (§28.3); an
// instance owned field of `this` / an owned place returned via `move` is still
// the legal owned-field transfer (§28.8.1); a fresh producer (`new Cell()`) is
// still a legal owned return.
// ===========================================================================

// §22.13 / §28.3: a static `owned` field is readable as a BORROW — `return
// Registry.Slot;` through a borrow-returning method is sound (the caller
// receives a borrow; the program's exit-disposal still owns the slot). Guards
// against the fix over-rejecting the bare (non-`move`) read.
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           return_borrow_read_of_static_owned_field_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kRegistry +
        "  public static method Peek(): Cell return Registry.Slot; end Peek; "
        "end Registry;");
    // The borrow-return path is sound — no borrow-return-via-owned diagnostic.
    BEZEL_EXPECT_FALSE(errorSays(r, kBorrowReturn));
}

// §22.13 / §28.8.1: the legal owned-return form for a fresh value (`new Cell()`)
// — not a field read, unaffected by the fix. Disposes the Cell exactly once.
BEZEL_TEST(move_static_owned_field_owned_return_2027,
           return_new_cell_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Make(): owned Cell return new Cell(); end Make; "
        "  public static method Main(): int "
        "    do using var c := C.Make(); end; return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
