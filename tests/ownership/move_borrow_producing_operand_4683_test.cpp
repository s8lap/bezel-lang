// `move` of a borrow-returning call / property / indexer / chain is admitted
// and double-frees. No `case` construct involved (issue #4683; spec §28.8.1,
// §28.3 K3.1/K3.6, v0.307). Follows #4664, whose fix closed the identical
// silent-accept for a `case`-arm pattern binding — this is the SAME root
// shape (`move` accepts an operand the checker never validates) reached by a
// completely different route.
//
// THE RULE. §28.8.1 states `move`'s admitted operand shapes as a CLOSED set:
// "The identifier-operand form `move x` — `x` a named local binding, an
// `owned` parameter, or the bare in-scope spelling of an `owned` field of the
// enclosing instance... `move x.f` is the one owned read of an owned field...
// `move coll[i]` is the analogous form for an owned-element collection
// slot... Its admitted positions... are the consume table's rows; elsewhere
// it is rejected." A method CALL, a property getter, and a user-defined
// indexer getter are none of those three shapes — a property/indexer getter
// is additionally STRUCTURALLY incapable of being owned (§23's "accessor
// read faces are lend-only; transfer-out is a method", rationale axiom A4 —
// PropertyDecl's grammar admits no `owned` on its declared Type, unlike a
// method's return type).
//
// THE BUG (#4683). Sema had a check for a non-`owned` FIELD move
// (`moveOnNonOwnedFieldName`, ExprTypecheck_Exprs.cpp, issue #4284) but none
// for a CALL, a property, or an indexer — and the ownership pass's
// `recordMovesIn` (Ownership_Checks.cpp) recognizes only a bare identifier
// and `this.field`; anything else fell through to a read-only `checkRead`
// call with nothing validating or rejecting the shape. MEASURED on HEAD
// ec0b6e55 (before this fix), all four repros below compiled with ZERO
// diagnostics and heap-use-after-freed under `--sanitize=address`:
// `producerState`'s own `move`-handling arm (Ownership.cpp) classifies ANY
// `move <class-or-interface-typed-expr>` Owned by STATIC TYPE ALONE — the
// "plain-class fallback" — regardless of whether the expression is actually
// a fresh owned producer or a borrow. `using var live := move
// other.Self();` (`Self(): Widget return this; end Self;`, a plain,
// non-`owned` return) left `live` and `other` both disposing the same
// `Widget`.
//
// THE FIX. `recordMovesIn`'s fallthrough (the same site #4063 already
// widened once) now rejects: a CALL whose `producerState` is not already
// Owned (a call CAN legitimately be a fresh owned producer — `return move
// C.Fresh();` stays admitted, the K3.13 redundant-marker case pinned by
// move_borrow_owned_return_1995_test.cpp — so `producerState` is consulted
// rather than a call being unconditionally banned); and a MemberAccessExpr
// or IndexExpr that resolves through a `sema::PropertySymbol` rather than a
// `FieldSymbol` — unconditionally, since a property/indexer getter can never
// be a legitimate owned source (axiom A4).
//
// SINCE narrow #7926. The ownership pass's own rule above is UNCHANGED — a
// `move` of a fresh owned producer is still not rejected BY IT (`producerState`
// is still consulted, and `return move C.Fresh();` is still admitted by it).
// What changed is a SEPARATE, SEMANTIC rule in SEMA: §28.8.1 / A.7's closed
// operand set ("a fresh producer ... needs no marker anywhere ... so `move
// (new Cell(1))` is a compile error at EVERY position", v0.350) rejects the
// marker on a fresh producer at a BINDING POSITION. Row (5) below is
// therefore flipped by that sema rule, not by this file's — and it now also
// pins that the marker-free `owned`-return position still compiles.
//
// OBSERVABILITY. The pre-fix behaviour is a clean COMPILE, so the oracle is
// the SPECIFIC diagnostic text, never `failedAtCompileTimeWithRealLocation`
// (which would answer true for any unrelated rejection) and never `!r.ran`
// (a runtime crash also synthesizes an Error diagnostic — tests/README.md).
//
// per §28.8.1, §28.3 K3.1/K3.6, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The four diagnostics this fix introduces — asserting the specific text is
// load-bearing: the pre-fix compiler ACCEPTS every one of these programs, so
// any weaker oracle would not distinguish the fix from an unrelated error.
const char* kCallNeedle = "`move` is not admitted on this call";
const char* kPropertyNeedle = "`move` is not admitted on a property";
const char* kIndexerNeedle = "`move` is not admitted on a user-defined indexer";

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    return hasErrorContaining(r, needle);
}

// `Self()` returns a plain (borrowing) reference to the receiver — the
// #4683 repro's own shape. `Holder` exposes the SAME borrow through a
// property getter and a user-defined indexer getter, so all three non-place
// operand shapes share one prelude.
const char* kPrelude =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "  public method Self(): Widget return this; end Self; "
    "end Widget; "
    "public class Holder "
    "  private _held: Widget; "
    "  public constructor(w: Widget) _held := w; end constructor; "
    "  public property Current: Widget => _held; "
    "  public property [i: int]: Widget => _held; "
    "end Holder; "
    "public class Program ";

std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "  public static method Main(): int " + mainBody +
           "  end Main; "
           "end Program;";
}

// --- REJECT rows: the four non-place `move` operand shapes -----------------

// (1) THE ISSUE'S REPRO, verbatim: `move` of a borrow-returning METHOD CALL.
BEZEL_TEST(move_borrow_producing_operand_4683,
           method_call_rejected_spec_28_8_1) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"x\"); "
        "    using var live := move other.Self(); "
        "    Console.WriteLine(live.Ping().ToString()); "
        "    return 0; "), kCallNeedle));
}

// (2) `move` of a borrow-returning PROPERTY getter.
BEZEL_TEST(move_borrow_producing_operand_4683,
           property_getter_rejected_spec_23) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var w := new Widget(\"a\"); "
        "    using var h := new Holder(w); "
        "    using var live := move h.Current; "
        "    Console.WriteLine(live.Ping().ToString()); "
        "    return 0; "), kPropertyNeedle));
}

// (3) `move` of a borrow-returning user-defined INDEXER getter.
BEZEL_TEST(move_borrow_producing_operand_4683,
           indexer_getter_rejected_spec_23) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var w := new Widget(\"a\"); "
        "    using var h := new Holder(w); "
        "    using var live := move h[0]; "
        "    Console.WriteLine(live.Ping().ToString()); "
        "    return 0; "), kIndexerNeedle));
}

// (4) A CHAIN whose last link borrows (`other.Self().Self()`) — the outermost
// node is a CallExpr, so it reaches the same call check as row (1); this pins
// that the rejection isn't defeated by one extra level of chaining.
BEZEL_TEST(move_borrow_producing_operand_4683,
           borrow_ending_chain_rejected_spec_28_8_1) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"x\"); "
        "    using var live := move other.Self().Self(); "
        "    Console.WriteLine(live.Ping().ToString()); "
        "    return 0; "), kCallNeedle));
}

// (5) `move` of a genuinely OWNED-producing call — the K3.13 "redundant
// marker on a fresh producer" case. REJECTED at a binding position since
// narrow #7926 — a SEMANTIC change, not this file's rule: see the note in
// the file header.
//
// (5a) AT A BINDING POSITION it is now REJECTED. §28.8.1 (v0.350) closes
// the operand set and says so in its own words: "No other operand is
// admitted (A.7): a fresh producer (a `new` expression, a call returning
// `owned T`, an owned operator result) needs no marker anywhere (K3.13) and
// takes none, so `move (new Cell(1))` is a compile error at EVERY position".
// A BINDING POSITION is one of those positions, so the marker on
// `W.Fresh()` — a call returning `owned T` — is a compile error there
// (narrow #7926: the §22.3 fresh-producer rule, extended to this position).
// This row was pinned ACCEPTED before that change (the position-scoped
// exemption it encoded is not one the spec grants).
//
// (5b) The guard this row still carries is that the rejection is
// POSITION-SCOPED, not a blanket ban on every CallExpr operand: the SAME
// `move` of the SAME owned-returning call at a MARKER-FREE position (an
// `owned` return) still compiles. That is the shape
// move_borrow_owned_return_1995_test.cpp pins ACCEPTed on its own
// (`return move C.Fresh();`).
BEZEL_TEST(move_borrow_producing_operand_4683,
           move_of_owned_returning_call_rejected_at_a_binding_position_spec_28_8_1) {
    const char* kCellPrelude =
        "module M; "
        "class Cell "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
        "end Cell; "
        "class W "
        "  public static method Fresh(): owned Cell return new Cell(3); end Fresh; "
        "  public static method Main(): int "
        "    using var live := move W.Fresh(); "
        "    Console.WriteLine(live.Id.ToString()); "
        "    return 0; "
        "  end Main; "
        "end W;";
    // (5a) the binding position: REJECTED by the fresh-producer rule. The
    // ORACLE is the diagnostic text (a crash would synthesise an Error
    // diagnostic too — tests/README.md — so `!r.ran` below is only a
    // supplementary assertion, never the oracle).
    auto r = runAot(kCellPrelude);
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "not written on a fresh producer"));
    BEZEL_EXPECT_FALSE(r.ran);

    // (5b) the `owned`-return position (marker-free): still COMPILES.
    const char* kReturnPrelude =
        "module M; "
        "class Cell "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
        "end Cell; "
        "class W "
        "  public static method Fresh(): owned Cell return new Cell(3); end Fresh; "
        "  public static method Take(): owned Cell return move W.Fresh(); end Take; "
        "  public static method Main(): int "
        "    using var live := W.Take(); "
        "    Console.WriteLine(live.Id.ToString()); "
        "    return 0; "
        "  end Main; "
        "end W;";
    auto r2 = runAot(kReturnPrelude);
    BEZEL_EXPECT_FALSE(hasErrorContaining(r2, kCallNeedle));
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 0);
    BEZEL_EXPECT_TRUE(r2.stdoutText.find("3") != std::string::npos);
}

// (6) A plain (non-`move`) READ through the same borrow-returning call,
// property, and indexer must keep compiling and running — the rejection is
// specific to `move`, not to reading a borrow at all.
BEZEL_TEST(move_borrow_producing_operand_4683,
           plain_read_of_borrow_shapes_still_accepted_spec_28_8_1) {
    auto r = runAot(prog(
        "    using var w := new Widget(\"a\"); "
        "    using var h := new Holder(w); "
        "    var a := w.Self(); "
        "    var b := h.Current; "
        "    var c := h[0]; "
        "    Console.WriteLine($\"{a.Ping()},{b.Ping()},{c.Ping()}\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kCallNeedle));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kPropertyNeedle));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kIndexerNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("7,7,7") != std::string::npos);
}

// (7) `move` of a non-`owned` FIELD (an existing, unrelated sema rejection —
// #4284's `moveOnNonOwnedFieldName`) must still fire, unchanged, and must
// NOT also draw one of this fix's new diagnostics — the fix's field-shaped
// MemberAccessExpr arm is scoped to a PropertySymbol only, never a
// FieldSymbol, so it must not double-report #4284's own shape.
BEZEL_TEST(move_borrow_producing_operand_4683,
           existing_nonowned_field_rejection_unchanged_spec_28_3) {
    const char* kFieldProg =
        "module M; "
        "public class Widget "
        "  private _n: string; "
        "  public constructor(n: string) _n := n; end constructor; "
        "  public method Ping(): int => 7; "
        "end Widget; "
        "public class Holder2 "
        "  public Held: Widget; "
        "  public constructor(w: Widget) Held := w; end constructor; "
        "end Holder2; "
        "public class Program2 "
        "  public static method Main(): int "
        "    using var w := new Widget(\"a\"); "
        "    using var h := new Holder2(w); "
        "    using var live := move h.Held; "
        "    Console.WriteLine(live.Ping().ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program2;";
    auto r2 = runAot(kFieldProg);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r2, "is not admitted on a non-`owned` field"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r2, kCallNeedle));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r2, kPropertyNeedle));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r2, kIndexerNeedle));
}

}  // namespace
