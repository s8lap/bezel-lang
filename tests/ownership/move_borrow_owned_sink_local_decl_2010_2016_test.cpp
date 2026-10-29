// Spec-derived regression tests for issues #2010 and #2016 — two manifestations
// of ONE root cause: the LOCAL-DECL-initializer / assignment-RHS arg-move path
// (`recordMovesIn` / `recordMovesInArgs`) never ran the §28.8 owned-source borrow
// classifier that the discharge (#1993/#1999) and return (#1995/#2011) positions
// use. So a `move` of a BORROW into an owned ctor/call sink, written as a binding
// initializer (`var h := new Holder(move b)`), slipped through and the value was
// disposed twice.
//
// §28.8 / §28.8.1 / §28.6 (the move expression, v0.197): a `move` produces an
// Owned value ONLY from an owned place. `move b` of a BORROW (a non-`owned`
// parameter, a catch-bound exception §31.1, a Borrowed-state local) is NOT an
// owned value (§28.8) — the borrow's real owner still disposes it. `move recv.f`
// reads an owned field as owned ONLY when the RECEIVER `recv` is an owned place
// (§28.8.1); off a BORROW receiver the field's real owner disposes the value
// (§28.6 — "a borrow does not own the object"). Handing either to an `owned` sink,
// which also disposes it, is a double-free.
//
// The two manifestations:
//   * #2010: `var h := new Holder(move b)` — `b` a borrow, moved into a
//     CONSTRUCTOR's owned formal in a local-decl initializer. The ctor arg-move
//     path (`recordMovesIn`'s NewExpr handler) ran NO borrow check on a `move`
//     argument — it fell straight to `recordMovesIn`, which marked `b` moved-from
//     silently. The Holder adopts and disposes the value AND `b`'s real owner
//     disposes it — a double-free.
//   * #2016: `var r := C.Sink(move recv.f!)` — `recv` a BORROW receiver, `f` an
//     owned field, moved into a CALL's owned formal in a local-decl initializer.
//     `recordMovesInArgs`' `move` borrow check fired ONLY for a BARE-identifier
//     operand, so the `!`-wrapped field read `move recv.f!` slipped past it; the
//     owned field was moved out of a borrow and disposed twice.
//
// The fix (Ownership_Checks.cpp, sema only): a new `checkOwnedArgMoveSource`
// classifies a `move <place>` argument to an owned sink exactly as the discharge
// and return positions already do — a BORROW source or an owned field of a BORROW
// receiver is REJECTED; an owned local, an owned producer, or a `move` of an owned
// field off an owned-place receiver (`move this.f` / `move _f` / `move
// ownedBox.f!`) is accepted. It is now called from BOTH the ctor arg-move path
// (`recordMovesIn`'s NewExpr handler) and the call arg-move path
// (`recordMovesInArgs`) for the place forms the bare-identifier check misses.
//
// A destructor-counter Cell (ctor ++, dtor --) is the exact-once oracle: Main
// returns Cell.live, so exitCode 0 == disposed exactly once. Fail-without (base
// 19b6566): the REJECT programs COMPILE (the classifier never ran) and the runtime
// forms ABORT under the harness's generated-code AddressSanitizer (heap-use-after-
// free — verified by probe: #2010 `new Holder(move b)` and #2016 nested-receiver
// `move o.inner.f!` both double-freed on base). Pass-with: each REJECT is a compile
// error; each ACCEPT disposes the value exactly once (exitCode 0).

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

// The §28.8 borrow-source diagnostic's stable substring — shared by the plain
// borrow-source rejection (#2010) and the borrow-receiver owned-field rejection
// (#2016; its message is a field-tailored variant of the same opener).
constexpr std::string_view kBorrowSink = "borrow cannot be passed where an";
// The #2016 borrow-RECEIVER field-move rejection's distinguishing phrase.
constexpr std::string_view kBorrowRecv = "moved out of a BORROW receiver";

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a double-free drives it negative or aborts under ASan — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

// A Holder whose constructor takes an `owned` Cell (an owned CTOR sink).
const char* kHolder =
    "class Holder "
    "  public owned c: Cell? := null; "
    "  public constructor(owned x: Cell) this.c := x; end constructor; "
    "  destructor end destructor; ";

// A Box that owns a nullable Cell field `f`.
const char* kBox =
    "class Box "
    "  public owned f: Cell? := null; "
    "  public constructor() this.f := new Cell(); end constructor; "
    "  destructor end destructor; ";

}  // namespace

// ===========================================================================
// #2010 REJECT — a BORROW moved into an owned CONSTRUCTOR argument in a binding.
// Fail-without: compiles (ctor arg-move path ran no borrow check) and double-frees
// at runtime. Pass-with: compile error (kBorrowSink).
// ===========================================================================

// §28.8: `var h := new Holder(move b)` where `b` is a borrow (non-`owned`)
// parameter — the issue's exact repro. On base the Holder adopts and disposes the
// Cell AND `b`'s real owner disposes it (probe: ASan heap-use-after-free).
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           ctor_arg_borrow_param_local_decl_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kHolder + "end Holder; " +
        "class C "
        "  public static method Borrow(b: Cell) using var h := new Holder(move b); end Borrow; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowSink));
}

// §28.8: the ASSIGNMENT-RHS twin — `x := new Holder(move b)` routes through the
// same `recordMovesIn` NewExpr handler (handleAssignment), so the borrow source is
// rejected there too.
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           ctor_arg_borrow_param_assignment_rhs_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kHolder + "end Holder; " +
        "class C "
        "  public static method Borrow(b: Cell) "
        "    var x: Holder? := null; "
        "    x := new Holder(move b); "
        "    using var keep := x!; "
        "  end Borrow; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowSink));
}

// §28.8 / §28.8.1 (#1993 sibling): `var r := C.Sink(move b!)` — the null-forgiving
// `move b!` of a BORROW local into an owned CALL formal, in a local-decl
// initializer. #1993 fixed this at the bare-statement discharge position; this is
// the binding-initializer twin the arg-move path missed.
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           call_arg_borrow_bang_local_decl_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell): int using var k := move c; return 0; end Sink; "
        "  public static method Borrow(b: Cell?) var r := C.Sink(move b!); end Borrow; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowSink));
}

// ===========================================================================
// #2016 REJECT — an owned field moved out of a BORROW RECEIVER into an owned sink
// in a binding. Fail-without: compiled (the `!`-wrapped field read slipped past
// the bare-identifier `move` check). Pass-with: compile error (kBorrowRecv).
// ===========================================================================

// §28.6 / §28.8.1: `var r := C.Sink(move box.f!)` where `box` is a BORROW
// parameter and `f` an owned field — the issue's exact repro (a CALL sink).
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           call_arg_borrow_receiver_field_local_decl_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Sink(owned c: Cell): int using var k := move c; return 0; end Sink; "
        "  public static method Borrow(box: Box) var r := C.Sink(move box.f!); end Borrow; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowSink));
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowRecv));
}

// §28.6 / §28.8.1: the CONSTRUCTOR-sink variant — `var h := new Holder(move
// box.f!)`, `box` a borrow — routes through `recordMovesIn`'s NewExpr handler.
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           ctor_arg_borrow_receiver_field_local_decl_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " + kHolder + "end Holder; " +
        "class C "
        "  public static method Borrow(box: Box) using var h := new Holder(move box.f!); end Borrow; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowRecv));
}

// §28.6 / §28.8.1 — RUNTIME UAF: a NESTED borrow receiver (`o.inner.f`, whose
// receiver `o.inner` is itself an owned-field read — a borrow) defeats the #2002
// slot-nulling (which fires only for a stable identifier receiver), so on base the
// Cell is disposed twice: an ASan heap-use-after-free in `bz$Inner$DisposeCore`
// (verified by probe on base 19b6566). The fix rejects it at compile time, so with
// the fix the program never runs — assert the compile error. Fail-without: base
// compiled and aborted under ASan. Pass-with: compile error (kBorrowRecv).
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           call_arg_nested_borrow_receiver_field_local_decl_rejected_spec_28_6) {
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
        "  public static method Sink(owned c: Cell): int using var k := move c; return 0; end Sink; "
        "  public static method Steal(o: Outer) var r := C.Sink(move o.inner.f!); end Steal; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowRecv));
}

// ===========================================================================
// ACCEPT — the legal owned sources are NOT over-rejected and dispose the value
// EXACTLY once (Cell.live == 0 under the AOT+ASan oracle).
// ===========================================================================

// §28.8: `var h := new Holder(move x)` where `x` is an OWNED local — the legal
// owned transfer into an owned ctor formal. The Holder disposes the value once.
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           ctor_arg_owned_local_disposes_once_spec_28_8) {
    std::string src =
        std::string(kCell) + kHolder + "end Holder; " +
        "class C "
        "  public static method Use() var x: Cell? := new Cell(); using var h := new Holder(move x!); end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `new Holder(move box.f!)` where `box` is an OWNED using-var local — the
// legal owned-field transfer out of an owned-place receiver. #2002 nulls the slot;
// the Holder disposes the value once. Guards against the receiver classifier
// over-rejecting an owned-place receiver.
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           ctor_arg_owned_local_receiver_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kBox + "end Box; " + kHolder + "end Holder; " +
        "class C "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    using var h := new Holder(move box.f!); "
        "    box.f := new Cell(); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `new Holder(move this.f!)` — a `this`-receiver owned-field move into an
// owned ctor formal in a binding, the field reassigned before exit. Guards against
// the fix mis-classifying a `this`-receiver owned-field move as a borrow (the
// owned-field-of-owned-receiver exclusion). Disposed exactly once.
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           ctor_arg_this_receiver_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kHolder + "end Holder; " +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Give() using var h := new Holder(move this.f!); this.f := null; end Give; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Use() using var b := new Box(); b.Give(); end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// NON-REGRESSION — the `this`-receiver owned-field move into an owned CALL formal
// (`var r := C.Sink(move _f!)`) must NOT trip the borrow diagnostic the fix now
// applies to BORROW receivers. This is the accept side of the #2016 exclusion at
// the call arg-move position (the field is restored before exit). Assert no borrow
// error (a compile check — this is the exclusion, not a runtime hazard).
// ===========================================================================
BEZEL_TEST(move_borrow_owned_sink_local_decl_2010_2016,
           call_arg_this_receiver_field_not_flagged_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell): int using var k := move c; return 0; end Sink; "
        "end C; "
        "class Box "
        "  public owned f: Cell? := null; "
        "  public constructor() this.f := new Cell(); end constructor; "
        "  public method Give(): int var r := C.Sink(move f!); this.f := new Cell(); return r; end Give; "
        "  destructor end destructor; "
        "end Box;");
    // The this-receiver owned-field move is not the #2016 borrow-receiver hazard.
    BEZEL_EXPECT_FALSE(errorSays(r, kBorrowSink));
}
