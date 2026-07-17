// Spec-derived tests for issues #1985 and #1986 — the two null-forgiving (`E!`)
// owned-sink soundness residuals #1983 surfaced.
//
// #1985 (§31.1 / §28.3 / §17.4): a BUILTIN exception (a null-declaration
// reference type — `Exception` and its hierarchy) force-unwrapped as a PLACE and
// flowed, un-`move`d, into an owned sink is a BORROW and must be rejected. #1983
// closed this for USER-declared types; the builtin case slipped through because
// `isBorrowableType` returned false for EVERY null-declaration Named type —
// lumping builtin CLASSES (exceptions, ordinary owned heap instances per §31.1)
// with bare GENERIC PARAMETERS (which erase to a by-value boundary) and value
// builtins. So `throw x!` (x: Exception?) double-freed at runtime and an
// owned-`Exception?` field / `owned Exception` parameter sink slipped through
// with only an auto-drop warning. The fix recognizes the builtin exception
// hierarchy positively; a generic parameter stays non-borrowable (NOT wrongly
// rejected), and a legit BORROW of a builtin exception still compiles.
//
// #1986 (§28.8.1 / §17.4): `C.Sink(move x!)` / `new Holder(move x!)` compiled
// (only an auto-drop warning) and DOUBLE-FREED — `move x!` failed to CONSUME the
// source for the owned PARAMETER / CTOR-ARG sinks reached through the bare-
// statement discharge walk (handleDischarge), because `move x!`'s operand is a
// `PostfixBangExpr`, not a bare identifier, so it fell to the fresh-producer tail
// and never marked x moved-from. (It consumed correctly for the FIELD sink, whose
// LocalDecl/assignment path runs recordMovesIn, which already looks through the
// force-unwrap.) The #1983 remedy tells users to write `move` — steering an
// un-`move`d `E!` param sink onto THIS double-free — so the two are fixed
// together. The fix looks through the `!` to the underlying place and consumes it
// for the param / ctor sinks, matching the field-sink behavior.
//
// §17.4 (v0.184): a null-aware operator "neither transfers ownership out of `x`
// nor disposes `x`'s value"; its result is borrowed even from an owned operand —
// "Code that needs the result to be owned must move from `x` explicitly." §28.8.1
// makes `move E!` the only owned read of a place.

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
// a leak leaves it >0; a double-free/double-dispose drives it negative or aborts
// under ASan (heap-use-after-free) — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// #1985 — a BUILTIN-exception place `E!` into an owned sink is a borrow.
// Fail-without: accepted → the value is disposed twice (a use-after-free at the
// runtime's / sink's disposal). Pass-with: rejected at compile time.
// ===========================================================================

// §31.1 / §17.12: `throw x!` where `x: Exception?` (a BUILTIN, null-declaration
// exception). `x!` borrows x's value; the throw hands it to the runtime while x
// still auto-drops it — a double-dispose (ASan heap-use-after-free at runtime
// without the fix). Must be rejected (write `throw move x!`).
BEZEL_TEST(null_forgiving_owned_sink_1985,
           builtin_exc_local_bang_into_throw_rejected_spec_31_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Boom() "
        "    var x: Exception? := new InvalidOperationException(\"boom\"); "
        "    throw x!; "
        "  end Boom; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "must be an OWNED exception"));
}

// §28.3 / §28.5 / §17.4: `this.f := x!` — a builtin-exception place `E!` stored
// into an owned `Exception?` field. A double-dispose (the field's owner and x's
// real owner both dispose). Must be rejected.
BEZEL_TEST(null_forgiving_owned_sink_1985,
           builtin_exc_local_bang_into_owned_field_rejected_spec_17_4) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public owned f: Exception? := null; "
        "  public method Set() "
        "    var x: Exception? := new InvalidOperationException(\"boom\"); "
        "    this.f := x!; "
        "  end Set; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow cannot be stored into an"));
}

// §28.8 / §17.4: `C.Sink(x!)` — a builtin-exception place `E!` passed to an
// `owned Exception` parameter. Must be rejected (write `C.Sink(move x!)`).
BEZEL_TEST(null_forgiving_owned_sink_1985,
           builtin_exc_local_bang_into_owned_param_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Sink(owned e: Exception) using var k := e; end Sink; "
        "  public static method Use() "
        "    var x: Exception? := new InvalidOperationException(\"boom\"); "
        "    C.Sink(x!); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        errorSays(r, "borrow cannot be passed where an"));
}

// ---------------------------------------------------------------------------
// #1985 ACCEPT / not-over-rejected: the fix flips ONLY the builtin exception
// hierarchy to borrowable — a GENERIC PARAMETER stays non-borrowable, and a
// legit BORROW of a builtin exception still compiles.
// ---------------------------------------------------------------------------

// A bare GENERIC PARAMETER is also a null-declaration Named type; it erases to a
// by-value boundary and is NOT borrowable, so a `T?` place `E!` into an owned
// sink must NOT be flagged as a borrow (the fix must distinguish a builtin class
// from a type parameter). Guards against lumping the two the other way. // §28.8
BEZEL_TEST(null_forgiving_owned_sink_1985,
           generic_param_bang_into_owned_sink_not_over_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Sink<T> "
        "  public method Take(owned t: T) end Take; "
        "  public method Use(x: T?) this.Take(x!); end Use; "
        "end Sink;");
    // The generic `x!` is NOT the owned-sink borrow the exception/user cases are.
    BEZEL_EXPECT_TRUE(!errorSays(r, "borrow cannot be passed where an"));
}

// §31.1: a legit BORROW of a builtin exception is unaffected — passing a caught
// `ex` to a borrow (non-`owned`) parameter, reading `ex.Message`, and a fresh
// `throw new` producer all compile and run leak-clean. Guards against the fix
// over-rejecting ordinary exception use. // per §31.1
BEZEL_TEST(null_forgiving_owned_sink_1985,
           builtin_exc_legit_borrow_accepted_spec_31_1) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Log(e: Exception): int return e.Message.Length; end Log; "
        "  public static method Boom() throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    try "
        "      C.Boom(); "
        "    catch var e: Exception => do "
        "      n := C.Log(e); "
        "      var msg: string := e.Message; "
        "    end; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8.1: `throw move x!` — the EXPLICIT owned transfer of a builtin exception.
// x is left moved-from (no auto-drop), the runtime owns and disposes the value
// exactly once → no double-free (ASan-clean). The `!`-free form the #1985
// rejections steer to. // per §28.8.1, §31.1
BEZEL_TEST(null_forgiving_owned_sink_1985,
           builtin_exc_move_bang_into_throw_disposes_once_28_8_1) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Boom() "
        "    var x: Exception? := new InvalidOperationException(\"boom\"); "
        "    throw move x!; "
        "  end Boom; "
        "  public static method Main(): int "
        "    try C.Boom(); catch var e: Exception => return 0; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1986 — `move x!` must CONSUME the source for owned PARAMETER / CTOR-ARG sinks.
// Fail-without: x is never marked moved-from → x auto-drops AND the sink disposes
// → double-free (ASan abort). Pass-with: x is consumed → disposed exactly once.
// ===========================================================================

// §28.8.1: `C.Sink(move x!)` — a bare-statement call into an `owned` parameter.
// `move x!` moves x out; the callee's `using var` disposes it exactly once, x
// does NOT auto-drop → Cell.live == 0. (Fail-without: the double-free ASan-aborts;
// the runAotChecked ASan oracle catches it.)
BEZEL_TEST(null_forgiving_owned_sink_1986,
           move_bang_into_owned_param_disposes_once_28_8_1) {
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

// §28.8: `new Holder(move x!)` — `move x!` into an owned CONSTRUCTOR argument.
// The Holder owns and disposes the value exactly once → Cell.live == 0.
BEZEL_TEST(null_forgiving_owned_sink_1986,
           move_bang_into_owned_ctor_arg_disposes_once_28_8) {
    std::string src =
        std::string(kCell) +
        "class Holder "
        "  public owned c: Cell; "
        "  public constructor(owned c: Cell) this.c := c; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Use() "
        "    var x: Cell? := new Cell(); do using var h := new Holder(move x!); end; "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8: `C.Consume(new Holder(move x!))` — a `new` owned-ctor-arg reached
// through the bare-statement DISCHARGE walk (the outer owned-arg is a `new`, so
// the ctor's `move x!` is classified by handleDischarge's NewExpr arm, NOT the
// recordMovesIn local-decl path the plain `using var h := new Holder(move x!)`
// takes). Pins the handleDischarge NewExpr force-unwrap consume: x is moved out,
// Holder owns and disposes the Cell exactly once → Cell.live == 0. Fail-without:
// x is never consumed → auto-drop + Holder's disposal double-free (ASan abort).
BEZEL_TEST(null_forgiving_owned_sink_1986,
           move_bang_into_nested_new_owned_arg_disposes_once_28_8) {
    std::string src =
        std::string(kCell) +
        "class Holder "
        "  public owned c: Cell; "
        "  public constructor(owned c: Cell) this.c := c; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Consume(owned h: Holder) using var k := h; end Consume; "
        "  public static method Use() "
        "    var x: Cell? := new Cell(); C.Consume(new Holder(move x!)); "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3: after `C.Sink(move x!)` consumes x, a SECOND `move x!` is a use of a
// moved-from local and must be rejected — direct proof the source is marked
// moved-from (the consume that fixes the double-free), not merely that the first
// call happened not to crash. // per §28.3, §28.8.1
BEZEL_TEST(null_forgiving_owned_sink_1986,
           move_bang_leaves_source_moved_from_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() "
        "    var x: Cell? := new Cell(); "
        "    C.Sink(move x!); "
        "    C.Sink(move x!); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "already moved out"));
}

// §28.8.1: the FIELD sink `this.f := move x!` was ALREADY correct (its LocalDecl/
// assignment path runs recordMovesIn) — pin it so the handleDischarge fix does
// not disturb it. Disposes exactly once → Cell.live == 0.
BEZEL_TEST(null_forgiving_owned_sink_1986,
           move_bang_into_owned_field_still_disposes_once_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public method Set() var x: Cell? := new Cell(); this.f := move x!; end Set; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Set(); end; "
        "  return Cell.live; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
