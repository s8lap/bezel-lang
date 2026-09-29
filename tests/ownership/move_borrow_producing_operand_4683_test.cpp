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

// --- ACCEPT controls: shapes the fix must NOT touch -------------------------

// (5) `move` of a genuinely OWNED-producing call — the K3.13 "redundant
// marker on a fresh producer" case. Guards against the call-side check
// becoming a blanket ban on every CallExpr operand rather than consulting
// `producerState`. Mirrors move_borrow_owned_return_1995_test.cpp's own
// pinned ACCEPT row for this exact shape (`return move C.Fresh();`).
BEZEL_TEST(move_borrow_producing_operand_4683,
           move_of_owned_returning_call_still_accepted_spec_28_8_1) {
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
    auto r = runAot(kCellPrelude);
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kCallNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
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
