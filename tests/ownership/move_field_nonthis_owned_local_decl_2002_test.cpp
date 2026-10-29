// Spec-derived regression tests for issue #2002 — moving an owned field out of a
// NON-`this` receiver in a LOCAL-DECL move (`var t := move box.f!`) did not null
// the source field slot in codegen, so the otherwise-sound "build then take then
// restore" form double-freed at runtime (ASan heap-use-after-free).
//
// §28.8/§28.8.1 (the move expression, v0.197): `move recv.f` where `f` is `owned`
// produces an Owned value and leaves the field moved-from. §28.7 (reassigning
// owned fields): assigning a new value to an owned field disposes the DISPLACED
// old value by drop-on-overwrite. Codegen nulls the source field slot after a
// `move` so a subsequent read / `:= null` / owner disposal sees null and skips —
// otherwise the slot keeps the moved-out pointer and the value is freed twice.
//
// The bug (#2002): that null-after-move fired ONLY for a `this` receiver (`move
// this.f`). For a NON-`this` receiver reached through the LOCAL-DECL path
// (`recordMovesIn`, NOT the sink-discharge walk #1999 rejected), the field slot
// kept the moved-out pointer, so:
//     var t := move box.f!;   // box.f NOT nulled on base
//     box.f := null;          // §28.7 disposes the OLD box.f — already moved into t
//     C.Sink(move t!);        // t (the same value) disposed again -> double-free
// #1999 only added a SEMA rejection of the argument-position sink-discharge form
// (`C.Sink(move box.f!)`); the local-decl form above routes elsewhere, reaches
// codegen, and double-freed. Confirmed on base 09ccb4e (probe: ASan
// heap-use-after-free in bz$C$Sink / bz$Box$DisposeCore, block allocated in
// bz$Box$ctor).
//
// The fix (codegen only, LlvmBackend_Exprs2.cpp lowerUnary): null the source
// field slot for ANY owned receiver this scope owns whose slot is stable — an
// identifier resolving to a local / using-var / `owned` param — not just `this`.
// (A borrowed receiver's owned field cannot be moved out at all — rejected before
// codegen — so any move reaching here is on an owned receiver. A receiver with no
// stable slot, e.g. `getBox().f`, is a separate deferred case.)
//
// A destructor-counter Cell (ctor ++, dtor --) is the exact-once oracle: Main
// returns Cell.live, so exitCode 0 == disposed exactly once (a leak would return
// 1; a double-dispose drives it negative and aborts under the harness's
// generated-code AddressSanitizer first). Fail-without (base 09ccb4e): the
// disposes-once cases below ABORT under ASan (ran=false / nonzero). Pass-with:
// each runs clean and returns 0.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAotChecked — AOT + generated-code ASan oracle
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
// a double-dispose drives it negative or aborts under ASan (heap-use-after-free).
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
    "  destructor end destructor; "
    "end Box; ";

}  // namespace

// ===========================================================================
// #2002 — the LOCAL-DECL move out of a NON-`this` owned receiver nulls the slot.
// ===========================================================================

// §28.7 / §28.8.1: the canonical "build then take then restore" repro. On base
// `box.f` keeps the moved-out pointer, so `box.f := null` (§28.7 drop-on-
// overwrite) disposes the value `t` already owns, and `C.Sink(move t!)` disposes
// it again — ASan heap-use-after-free. With the fix `box.f` is nulled by the
// move, the §28.7 drop sees null and skips, and the Cell is disposed exactly once.
BEZEL_TEST(move_field_nonthis_owned_local_decl_2002,
           build_take_restore_owned_local_disposes_once_spec_28_7) {
    std::string src =
        std::string(kCell) + kBox +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    var t: Cell? := move box.f!; "
        "    box.f := null; "
        "    C.Sink(move t!); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // Cell.live == 0: disposed exactly once
}

// §28.8.1: the moved-from field is NOT reassigned; the owned receiver's scope-end
// disposal (`using var box`) is the second free on base (UAF in Box.DisposeCore).
// With the fix the field is nulled, so the owner's null-guarded owned-field
// disposal skips it and the Cell is disposed exactly once (by the sink).
BEZEL_TEST(move_field_nonthis_owned_local_decl_2002,
           moved_out_field_not_reassigned_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kBox +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    var t: Cell? := move box.f!; "
        "    C.Sink(move t!); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8/§28.3: an `owned` PARAMETER receiver — `b` is owned by the callee. Moving
// `b.f` out must null the field slot so that discharging `b` (here by rebinding
// into a `using var`) disposes the Box shell without re-freeing the moved-out
// Cell. Disposed exactly once.
BEZEL_TEST(move_field_nonthis_owned_local_decl_2002,
           owned_param_receiver_field_move_disposes_once_spec_28_8) {
    std::string src =
        std::string(kCell) + kBox +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Take(owned b: Box) "
        "    var t: Cell? := move b.f!; "
        "    using var keep := move b; "
        "    C.Sink(move t!); "
        "  end Take; "
        "  public static method Make(): owned Box return new Box(); end Make; "
        "  public static method Use() C.Take(C.Make()); end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §14.1 / §28.8.1: a GENERIC receiver `Box<long>` with a value field (`first: T`)
// laid out BEFORE the owned field. The fix locates the field slot via `fieldAddr`
// (which installs the receiver's generic substitution from its exprType), so the
// null-store hits the correct specialized-layout offset — a raw StructGEP under
// the enclosing (non-generic) method's substitution could compute a wrong byte
// offset and corrupt memory. Disposed exactly once.
BEZEL_TEST(move_field_nonthis_owned_local_decl_2002,
           generic_receiver_value_field_before_owned_field_disposes_once_spec_14_1) {
    std::string src =
        std::string(kCell) +
        "class Box<T> "
        "  public first: T; "
        "  public owned f: Cell? := null; "
        "  public constructor(v: T) this.first := v; this.f := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box<long>(1234567890123L); "
        "    var t: Cell? := move box.f!; "
        "    box.f := null; "
        "    C.Sink(move t!); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// Not-a-regression guards.
// ---------------------------------------------------------------------------

// §28.8.1: the `this`-receiver build-take-restore form was ALREADY handled (the
// `this._field` null-after-move). Pin it so the #2002 generalization to non-`this`
// receivers does not disturb the `this` path — disposed exactly once.
BEZEL_TEST(move_field_nonthis_owned_local_decl_2002,
           this_field_move_build_take_restore_still_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Give() "
        "    var t: Cell? := move this.f!; "
        "    this.f := null; "
        "    C.Sink(move t!); "
        "  end Give; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() using var box := new Box(); box.Give(); end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8: a NON-owned (value) field move is untouched by the fix — the null-after-
// move is gated on an OWNED reference field, so a `move box.n` of an `int` field
// leaves the slot intact and reads the value. Guards the fix's owned-field
// scoping. Returns the field value (42), proving the value survived.
BEZEL_TEST(move_field_nonthis_owned_local_decl_2002,
           value_field_move_from_owned_receiver_unaffected_spec_28_8) {
    std::string src =
        "module M; "
        "class Box "
        "  public n: int := 42; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Use(): int "
        "    using var box := new Box(); "
        "    var q: int := move box.n; "
        "    return q; "
        "  end Use; "
        "  public static method Main(): int return C.Use(); end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §28.8.1 (#1999): the fix is codegen-only for the forms that reach codegen; it
// does NOT re-enable the argument-position sink-discharge form `C.Sink(move
// box.f!)`, which #1999 rejects at compile time. Pin that rejection so the #2002
// codegen change stays scoped to the local-decl form.
BEZEL_TEST(move_field_nonthis_owned_local_decl_2002,
           nonthis_sink_discharge_still_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    C.Sink(move box.f!); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot move owned field `f` out of `box`"));
}
