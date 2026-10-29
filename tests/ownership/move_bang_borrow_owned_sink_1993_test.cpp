// Spec-derived tests for issue #1993 — `move b!` where `b` is a BORROW into an
// owned sink must be REJECTED, exactly as plain `move b` is.
//
// §28.8 / §28.8.1 / §17.4: `move E!` reads the underlying PLACE `E` as an owned
// value ONLY when that place is itself owned (§28.8.1: `move` is the owned read
// of a place). When `E` is a BORROW — a non-`owned` parameter, a catch-bound
// exception (§31.1), a Borrowed-state local, a non-`owned` field — `move E!` is
// no more an owned value than `move E` is: the borrow's real owner still disposes
// the value, so handing it to an owned sink (an `owned` parameter/ctor argument
// §28.8, an `owned` field store §28.5, a `throw` operand §31.1) makes the sink
// dispose it too — a double-free / use-after-free.
//
// The bug (#1993): plain `move b` was correctly rejected by the §28.8 borrow-
// source guard (`checkOwnedArgSource` / `ownedArgSourceIsBorrow`), but the
// null-forgiving `move b!` form slipped past it and was silently consumed:
//   * `ownedArgSourceIsBorrow` unwrapped the leading `move`, then SKIPPED its
//     `PostfixBangExpr` place-borrow branch whenever a `move` had been unwrapped
//     (it assumed `move E!` is ALWAYS the legal owned transfer). So the field-
//     store (§28.5) and throw-operand (§31.1) sinks accepted a `move <borrow>!`.
//   * handleDischarge's bare-statement CallExpr / NewExpr `move` arms (added by
//     #1986 to CONSUME `move x!`) peeled the `!` and marked the source moved-from
//     WITHOUT running the borrow-source guard, so the `owned` parameter / bare-
//     statement ctor-argument sinks accepted a `move <borrow>!`.
// In every case the value was disposed twice (ASan heap-use-after-free at runtime).
//
// Fix: classify the peeled place under `move E!` exactly as `move E` is — an
// owned local/field is a legal owned source (accepted), a BORROW place is
// rejected. The legit #1985/#1986 owned cases (`move x!` of an owned local, an
// owned-field read `move obj.f!`, `throw move x!` of an owned exception) are NOT
// over-rejected.
//
// Fail-without / pass-with: each REJECT case below compiled and double-freed
// before the fix (verified under the AOT+ASan oracle) and is a compile error
// after it; each ACCEPT case disposes the value exactly once (Cell.live == 0).

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
// REJECT — `move b!` of a BORROW into each owned sink.
// Fail-without: accepted -> the value is disposed twice (ASan heap-use-after-
// free at runtime). Pass-with: rejected at compile time (as plain `move b` is).
// ===========================================================================

// §28.8: `C.Sink(move b!)` — a bare-statement CALL into an `owned` parameter,
// `b` a non-`owned` (borrow) parameter. This is the issue's exact repro. The
// `!` form must be rejected exactly as plain `move b` (which already is).
BEZEL_TEST(move_bang_borrow_owned_sink_1993,
           call_owned_param_borrow_bang_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Take(b: Cell?) C.Sink(move b!); end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow cannot be passed where an"));
}

// §28.8: `C.Consume(new Holder(move b!))` — the owned CONSTRUCTOR argument reached
// through the bare-statement DISCHARGE walk (handleDischarge's NewExpr `move`
// arm, the #1986-added consume). `b` a borrow -> must be rejected.
BEZEL_TEST(move_bang_borrow_owned_sink_1993,
           barestmt_ctor_arg_borrow_bang_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Holder "
        "  public owned c: Cell; "
        "  public constructor(owned c: Cell) this.c := c; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Consume(owned h: Holder) using var k := move h; end Consume; "
        "  public static method Take(b: Cell?) C.Consume(new Holder(move b!)); end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow cannot be passed where an"));
}

// §28.5 / §28.3: `this.f := move b!` — a borrow stored into an `owned` field. The
// field's owner AND `b`'s real owner both dispose the value. Must be rejected.
BEZEL_TEST(move_bang_borrow_owned_sink_1993,
           owned_field_store_borrow_bang_rejected_spec_28_5) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public method Set(b: Cell?) this.f := move b!; end Set; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow cannot be stored into an"));
}

// §31.1: `throw move b!` — a borrowed exception thrown as an owned value. The
// runtime disposes it after the arm while `b`'s owner also disposes it. Must be
// rejected (write a fresh `throw new ...`).
BEZEL_TEST(move_bang_borrow_owned_sink_1993,
           throw_borrow_bang_rejected_spec_31_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Boom(b: Exception?) throw move b!; end Boom; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "must be an OWNED exception"));
}

// §31.1 / §28.8: a CATCH-bound exception is a borrow whose lifetime is the arm
// (the runtime disposes it after the arm). `C.Sink(move ex!)` into an `owned
// Exception` sink is the same double-dispose — the `!` form must be rejected just
// as `move ex` (a bare catch borrow) is. Covers the "user class AND exception"
// generality: the fix is not exception-specific.
BEZEL_TEST(move_bang_borrow_owned_sink_1993,
           catch_binding_borrow_bang_rejected_spec_31_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Sink(owned e: Exception) using var k := move e; end Sink; "
        "  public static method Boom() throw new InvalidOperationException(\"x\"); end Boom; "
        "  public static method Use() "
        "    try C.Boom(); catch var ex: Exception => C.Sink(move ex!); end; "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow cannot be passed where an"));
}

// ===========================================================================
// ACCEPT — a genuinely OWNED `move x!` is NOT over-rejected (the #1986 case).
// Disposes the value EXACTLY once (Cell.live == 0 under the ASan oracle).
// ===========================================================================

// §28.8.1: `C.Sink(move x!)` where `x` is an OWNED local — the legal owned
// transfer. The callee disposes it exactly once; `x` does not auto-drop. Guards
// against the fix over-rejecting the legit owned-local case.
BEZEL_TEST(move_bang_borrow_owned_sink_1993,
           owned_local_move_bang_into_param_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() var x: Cell? := new Cell(); C.Sink(move x!); end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `C.Sink(move this.f!)` where `f` is an OWNED field — `move obj.f!` is
// the legal owned READ of an owned field (§28.8.1), NOT a borrow. The field is
// left moved-from and reassigned (`this.f := null`) before exit; the sink
// disposes the value exactly once -> Cell.live == 0. Guards against the fix
// mis-classifying an owned-field read as a borrow. // per §28.8.1
BEZEL_TEST(move_bang_borrow_owned_sink_1993,
           owned_field_read_move_bang_into_param_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Give() C.Sink(move this.f!); this.f := null; end Give; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Main(): int "
        "    do using var b := new Box(); b.Give(); end; "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
