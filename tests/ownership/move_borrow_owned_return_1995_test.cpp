// Spec-derived tests for issue #1995 (part A) — `return move b` / `return move
// b!` (and the `=>` expression-body form) where `b` is a BORROW returned through
// an `owned` return type must be REJECTED. Before the fix it was accepted and
// double-freed at runtime.
//
// §28.6 / §28.8 / §28.8.1: a borrow (a borrowed local/parameter, a catch-bound
// exception §31.1, a Borrowed-state local, a non-`owned` field) is owned
// ELSEWHERE. `move`-wrapping it does NOT make it owned — §28.8.1 makes `move` the
// owned read of a place ONLY when the place is itself owned. Returning a `move`d
// borrow through an `owned` return type hands the caller a value it disposes
// while the borrow's real owner disposes it too — a double-free / use-after-free.
//
// The bug (#1995): the ARGUMENT-sink positions (a CALL `owned` parameter, a ctor
// argument, an `owned`-field store, a `throw` operand) run the §28.8 borrow-source
// guard (`checkOwnedArgSource` / `ownedArgSourceIsBorrow`, #1993), but the RETURN
// position never did — only the borrow-of-scope-local lifetime checks lived there.
// So `return move b;` / `return move b!;` of a borrow slipped through for BOTH the
// `!` and the plain forms and was returned as if owned.
//
// Fix: run the SAME owned-source classifier at the return position (a no-op unless
// the return type is `owned` and the returned expression is `move <place>`). A
// `move`d owned-FIELD read (`move this.f` / `move _f`) is the legal owned-field
// transfer (§28.8.1) and is NOT flagged — its field-restoration obligation is
// enforced separately.
//
// Fail-without / pass-with: each REJECT case below compiled and double-freed
// before the fix (verified under the AOT+ASan oracle — heap-use-after-free) and is
// a compile error after it; each ACCEPT case disposes the value exactly once
// (Cell.live == 0).

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

// The #1995 return-position diagnostic's stable substring.
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

}  // namespace

// ===========================================================================
// REJECT — `return move <borrow>` / `return move <borrow>!` through an `owned`
// return type. Fail-without: accepted -> disposed twice (ASan heap-use-after-
// free at runtime). Pass-with: rejected at compile time.
// ===========================================================================

// §28.8: `return move b;` where `b` is a borrow (non-`owned`) parameter returned
// as `owned Cell` — the issue's exact repro (block body, plain `move`).
BEZEL_TEST(move_borrow_owned_return_1995,
           return_move_borrow_param_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Take(b: Cell): owned Cell return move b; end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §28.8 / §17.4 / §28.8.1: the `!` force-unwrap form `return move b!;` — must be
// rejected exactly as the plain `move b` (peel the `!` and classify the place).
BEZEL_TEST(move_borrow_owned_return_1995,
           return_move_borrow_param_bang_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Take(b: Cell?): owned Cell return move b!; end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §28.8: the `=>` expression-body form `Take(b): owned Cell => move b;` — the
// body IS the returned value, so it takes the same owned-source classification.
BEZEL_TEST(move_borrow_owned_return_1995,
           exprbody_move_borrow_param_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Take(b: Cell): owned Cell => move b; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §31.1 / §28.8: a CATCH-bound exception is a borrow whose lifetime is the arm
// (the runtime disposes it after the arm). `return move ex;` as an `owned
// Exception` is the same double-dispose — must be rejected.
BEZEL_TEST(move_borrow_owned_return_1995,
           return_move_catch_binding_rejected_spec_31_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Boom() throw new InvalidOperationException(\"x\"); end Boom; "
        "  public static method Grab(): owned Exception "
        "    try C.Boom(); catch var ex: Exception => return move ex; end; "
        "    return new InvalidOperationException(\"y\"); "
        "  end Grab; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// §31.1 / §28.8.1: the `!` form of the catch-binding return `return move ex!;`.
BEZEL_TEST(move_borrow_owned_return_1995,
           return_move_catch_binding_bang_rejected_spec_31_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Boom() throw new InvalidOperationException(\"x\"); end Boom; "
        "  public static method Grab(): owned Exception "
        "    try C.Boom(); catch var ex: Exception => return move ex!; end; "
        "    return new InvalidOperationException(\"y\"); "
        "  end Grab; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowReturn));
}

// ===========================================================================
// ACCEPT — a genuinely OWNED source is NOT over-rejected, and disposes the value
// EXACTLY once (Cell.live == 0 under the AOT+ASan oracle).
// ===========================================================================

// §28.8.1: `return move x!;` where `x` is an OWNED local — the legal owned
// transfer. The caller disposes it exactly once; `x` does not auto-drop.
BEZEL_TEST(move_borrow_owned_return_1995,
           return_move_owned_local_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Take(): owned Cell "
        "    var x: Cell? := new Cell(); return move x!; "
        "  end Take; "
        "  public static method Main(): int "
        "    do using var got := C.Take(); end; return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8: `return b;` where `b` is an OWNED parameter — the legal transfer
// of a transferred-in owned value. Guards against the fix over-rejecting an
// `owned` (as opposed to borrow) parameter. Spelled marker-free per §28.3
// K3.13 / K3.2 (v0.307): `return` under an `owned` return type is a
// marker-free consuming position, so `return move b;` is now rejected as a
// redundant marker; the classification this row pins is unchanged.
BEZEL_TEST(move_borrow_owned_return_1995,
           return_move_owned_param_disposes_once_spec_28_8) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Pass(owned b: Cell): owned Cell return b; end Pass; "
        "  public static method Main(): int "
        "    do using var got := C.Pass(new Cell()); end; return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `return move C.Fresh();` / `return move C.Fresh()!;` — a `move` of a
// fresh owned producer (a call returning `owned T`) is the legal transfer, NOT a
// borrow. Guards against the fix flagging an owned temporary.
BEZEL_TEST(move_borrow_owned_return_1995,
           return_move_owned_producer_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Fresh(): owned Cell return new Cell(); end Fresh; "
        "  public static method Take1(): owned Cell return move C.Fresh(); end Take1; "
        "  public static method Take2(): owned Cell return move C.Fresh()!; end Take2; "
        "  public static method Main(): int "
        "    do using var a := C.Take1(); end; "
        "    do using var b := C.Take2(); end; "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `move this.f!` / `move _f!` of an OWNED field is the legal owned READ
// of an owned field — moved into a local, the field reassigned, then the local
// returned. Guards against the fix mis-classifying an owned-field read as a
// borrow. The value is disposed exactly once (Cell.live == 0). // per §28.8.1
// The trailing `return r;` is marker-free per §28.3 K3.13 / K3.2 (v0.307); the
// `move f!` owned-field read it pins keeps its marker (K3.13 leaves field moves
// marked, for §28.8.1 K9.2 restoration tracking).
BEZEL_TEST(move_borrow_owned_return_1995,
           owned_field_move_via_local_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Give(): owned Cell "
        "    var r := move f!; this.f := null; return r; "
        "  end Give; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    do using var b := new Box(); do using var got := b.Give(); end; end; "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// NON-REGRESSION — a bare/qualified owned-FIELD move returned as `owned` must
// NOT trip the #1995 borrow-return diagnostic. `move this.f!` / `move _f!` is the
// legal owned-field transfer (§28.8.1); it independently trips the field-moved-
// from restoration error (§28.8.1, an orthogonal rule), but never the borrow-
// return error the fix adds. // per §28.8.1
// ===========================================================================
BEZEL_TEST(move_borrow_owned_return_1995,
           owned_field_return_not_flagged_as_borrow_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method TakeBare(): owned Cell return move f!; end TakeBare; "
        "  public method TakeThis(): owned Cell return move this.f!; end TakeThis; "
        "  destructor end destructor; "
        "end Box;");
    // The owned-field move is not the #1995 borrow-return hazard.
    BEZEL_EXPECT_FALSE(errorSays(r, kBorrowReturn));
}
