// Ownership — a `return` OPERAND that performs TWO moves of the same owned
// local was admitted; the byte-identical expression as a local INITIALIZER is
// rejected.  Issue #8938.
//
// SPEC: §28.3 "Local variables, ownership, and borrows", K1 "Value
// classification", item 3 (v0.359):
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
// A second `move x` of an already moved-from binding is a second transfer of
// an obligation that has already left the binding — §28.3 K1.3's
// "moved-from" state is exactly what forbids it, and the initializer path
// enforces it.
//
// ROOT CAUSE: the local-INITIALIZER path (`MethodOwnership::handleLocalDecl`)
// records the moves its initializer performs
// (`recordMovesIn(*ld.initializer)`), so the first `move x` leaves the
// binding moved-from and a SECOND `move x` — at a different site — is
// reported ("owned value in `x` was already moved out at this point
// (§28.3)").  The assignment-RHS path records them the same way
// (`handleAssignment`'s `recordMovesIn(*as.value)`).  The RETURN-operand path
// did not: `handleReturn`'s `handleDischarge(e)` walks a TOP-LEVEL call /
// new operand for its own moves, but a COMPOSITE operand (`a + b`) falls
// through to that function's `checkRead` fallback, which READS the operand
// without RECORDING its moves.  `return Sink.Take(move x) + Sink.Take(move
// x);` therefore never marked `x` moved-from at all.
//
// MEASURED PRE-FIX (base r30-c1 @ 9356d0a6a): the program below compiled
// rc=0 with no diagnostic, and the compiled program SEGV'd at run time —
// the second `Sink.Take` received the value the first one had already
// transferred away (a null read inside `Sink.Take`, measured under
// AddressSanitizer by the reporting review lane).  The byte-identical
// expression as a `var` initializer is rejected with the §28.3 moved-out
// error.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

const char* kMovedOut =
    "owned value in `x` was already moved out at this point (§28.3)";

// How many diagnostics carry `needle` — one, never two: the return-operand
// move recording must report the double move ONCE, not also report a
// use-after-move read of the same binding (the read check runs first, the
// move recording second — the initializer path's order).
int diagCount(const OwnershipResult& r, const std::string& needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) ++n;
    return n;
}

bool hasMovedOutError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kMovedOut) != std::string::npos)
            return true;
    return false;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method Peek(r: Res): int return r.tag; end Peek; "
    "end Sink; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method F(): int "
           "    var x := new Res(1); " +
           body +
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(C.F()); return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's own repro, verbatim: two moves of the same owned local inside
// ONE return operand.  Rejected with the §28.3 K1.3 moved-out error, once.
BEZEL_TEST(return_operand_double_move_8938,
           return_operand_double_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap("return Sink.Take(move x) + Sink.Take(move x); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// Three moves of the same binding in one return operand: the SECOND one is
// the double move, so the error is reported exactly once.
BEZEL_TEST(return_operand_double_move_8938,
           return_operand_three_moves_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap("return Sink.Take(move x) + Sink.Take(move x) + "
             "Sink.Take(move x); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The AOT leg: the program used to COMPILE and then SEGV at run time (the
// second `Sink.Take` received the already-transferred value).  It must now be
// a compile error with a real source location.
BEZEL_TEST(return_operand_double_move_8938,
           aot_return_operand_double_move_is_a_compile_error_28_3) {
    JitResult jr =
        runAot(wrap("return Sink.Take(move x) + Sink.Take(move x); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    // The rejection is THE §28.3 moved-out error, not some other failure.
    bool movedOut = false;
    for (const auto& d : jr.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kMovedOut) != std::string::npos)
            movedOut = true;
    BEZEL_EXPECT_TRUE(movedOut);
}
