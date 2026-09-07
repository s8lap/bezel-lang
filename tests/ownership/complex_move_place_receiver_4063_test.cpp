// Ownership — found in adversarial review of #4023: `checkRead`'s
// `isTopLevelConsumingForm` skip (introduced by #3971, extended to the bare
// `CallExpr` argument loop by #4023) declines to read-check the ENTIRE
// subtree of a top-level `move`/`await` argument — not just the moved VALUE
// itself. For a `move` operand that is a COMPLEX PLACE (`move recv.field`,
// `move recv!.field` where `recv` is not `this`), `recordMovesIn` never
// validates the RECEIVER's read-state either: its `UnaryExpr` "move" arm
// only recognizes a bare `IdentifierExpr` operand (-> `markMovedByName`) or
// `this.field` (-> `recordFieldMove`); any other move-operand shape falls
// through to a plain `recordMovesIn(*un->operand)` recursion, and
// `recordMovesIn` has no `IdentifierExpr` arm outside the move-operand
// context — so a disposed/moved-from receiver went silently unvalidated.
//
// Owned fields are disposed by the generated `Dispose()` cascade (§28.4/K9),
// so reading `recv.field` after `recv.Dispose()` is a genuine
// use-after-dispose the §28.2 warning must still catch.
//
// TWO DISTINCT SEVERITIES (per the review that found this):
//
//   (1) A REGRESSION at the exact position #4023 touches. Before #4023,
//       `checkRead`'s `CallExpr` argument loop ran an UNCONDITIONAL
//       `checkRead(*a.value, false)` — the one position that had not yet
//       been "upgraded" to the `isTopLevelConsumingForm` skip pattern, so it
//       still caught a disposed receiver via its ordinary blanket recursion
//       (`UnaryExpr` -> `MemberAccessExpr` -> `IdentifierExpr` arm -> §28.2
//       Warning). #4023's patch removed that protection with no
//       replacement.
//   (2) The IDENTICAL gap also exists, pre-existing (not a regression
//       there), at every other `isTopLevelConsumingForm`/`readOperand`
//       site: the `NewExpr` ctor-argument site (fix #3971), the marker-free
//       `if`/`case` expression-arm site (fix #3990), and this review's own
//       new `CollectionLiteralExpr` arm (fix #4024, itself under review).
//       They never had the protection to lose.
//
// MEASURED (this worktree, r15-c4-l6-4023, base carrying #4003/#4028):
//
//   position                                before (b6e91e6)   after #4023
//                                            (pre-#4023)        only (8fd0324)
//   -------------------------------------  -----------------  ---------------
//   bare CallExpr arg (the regression)      WARNS (§28.2)      SILENT, SEGV
//   NewExpr ctor arg          (pre-existing) SILENT, SEGV       SILENT, SEGV
//   if/case marker-free arm   (pre-existing) SILENT, SEGV       SILENT, SEGV
//   CollectionLiteralExpr elem (pre-existing/new w/ #4024) SILENT, SEGV  same
//
// THE FIX: `recordMovesIn`'s `UnaryExpr` "move" arm's fallthrough (past the
// bare-identifier and `this.field` returns) now runs
// `checkRead(*un->operand, /*selfTarget=*/false)` before the existing
// generic `recordMovesIn(*un->operand)` recursion — `un->operand` here is
// the PLACE being moved (`recv.field`/…), a plain read position for
// everything except the final field itself, so `checkRead`'s own
// `MemberAccessExpr`/`PostfixBangExpr` arms correctly recurse to the
// receiver identifier and report/poison exactly as the pre-#4023 unguarded
// walk did. This does NOT reintroduce #4023's masking bug: the
// bare-identifier and `this.field` shapes always `return` BEFORE reaching
// this line, so `markMovedByName`/`recordFieldMove` still see the
// unpoisoned original state for those two shapes — only a shape
// `recordMovesIn` was never going to mark anyway reaches the new
// `checkRead` call, and it is the ONLY place that ever will. Because EVERY
// one of the four call sites above ultimately calls `recordMovesIn` on the
// same `move`-operand `UnaryExpr` node, fixing this ONE fallthrough closes
// all four simultaneously — confirmed below, not merely claimed.
//
// SCOPE NOTE: `move recv[i]` (an INDEXER place) is NOT closed by this fix —
// `checkRead`'s own `IndexExpr` arm only read-checks its target for a
// move-only inline array (#1925), a narrower, separate, pre-existing gap
// (unrelated to `move` — a plain `Reader.Peek(arr[0])` off a disposed `arr`
// is equally unwarned) filed as #4065 and NOT addressed here.
//
// per §28.2, §28.3, §28.4 K9, v0.294, #3971, #3990, #4023, #4024, #4063

#include <string>
#include <string_view>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

constexpr char kUseAfterDisposeFragment[] =
    "use of `b` after a dominating explicit Dispose() call";

// See checkread_call_arg_move_order_4023_test.cpp's identical helper and its
// header comment: `hasErrorMessage` (ownership_support.hpp) deliberately
// scans ERROR-severity diagnostics only; the §28.2 diagnostic this file
// asserts on is a WARNING, so scan Warning-severity directly.
bool hasWarningMessage(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Shared shapes: `Res` (plain disposable), `Box` (owns a NON-nullable `Res`
// field `f`, the complex move-place receiver under test), `Holder` (consumes
// an `owned r: Res`), `Factory.Make` (forwards a moved owned argument
// through a bare, unwrapped call — reaches `checkRead`'s `CallExpr` arg loop
// exactly as #4023's own regression test does).
constexpr char kShapes[] =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Box "
    "  public owned f: Res; "
    "  public constructor(owned r: Res) this.f := r; end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class Holder "
    "  public v: int; "
    "  public constructor(owned r: Res) this.v := r.tag; r.Dispose(); "
    "end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class Factory "
    "  public static method Make(owned r: Res): owned Holder? "
    "    return new Holder(move r); "
    "  end Make; "
    "end Factory; ";

std::string program(const std::string& classesAndMain) {
    return kShapes + classesAndMain;
}

}  // namespace

// =============================================================================
// NEGATIVE (1) — the exact regression: bare CallExpr argument, complex move
// place `move b.f`, receiver `b` disposed.
// FAIL-WITHOUT (measured on 8fd0324, #4023 landed / #4063 not yet):
//   r.diagnostics.hasErrors() false, hasWarningMessage(...) false — SILENT.
// PASS-WITH (this fix): hasWarningMessage(...) true, matching the pre-#4023
// baseline (b6e91e6) byte-for-byte.
// =============================================================================
BEZEL_TEST(complex_move_place_receiver_4063,
           bare_call_move_complex_place_disposed_receiver_warns) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    b.Dispose(); "
        "    var y := Factory.Make(move b.f); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

// =============================================================================
// NEGATIVE (2a) — pre-existing sibling gap, NewExpr ctor argument.
// FAIL-WITHOUT verified on 0ffd8ba (before #4023/#4024/#4063 existed at
// all): SILENT there too — never a regression, always broken.
// =============================================================================
BEZEL_TEST(complex_move_place_receiver_4063,
           newexpr_ctor_arg_move_complex_place_disposed_receiver_warns) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    b.Dispose(); "
        "    using var h := new Holder(move b.f); "
        "    return h.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

// =============================================================================
// NEGATIVE (2b) — pre-existing sibling gap, marker-free if-expression arm.
// =============================================================================
BEZEL_TEST(complex_move_place_receiver_4063,
           ifexpr_marker_free_arm_move_complex_place_disposed_receiver_warns) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    b.Dispose(); "
        "    var flag := true; "
        "    using var h := (if flag then new Holder(move b.f) "
        "                    else new Holder(new Res(0))); "
        "    return h.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

// =============================================================================
// NEGATIVE (2c) — pre-existing sibling gap, this review's own new
// `CollectionLiteralExpr` arm (#4024).
// =============================================================================
BEZEL_TEST(complex_move_place_receiver_4063,
           collection_literal_move_complex_place_disposed_receiver_warns) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    b.Dispose(); "
        "    var arr: OwningArray<Res> := [move b.f]; "
        "    return arr[0].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

// =============================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15) — a LIVE (non-disposed) receiver must
// NOT spuriously draw the disposed-state warning, for the four shapes above.
// =============================================================================

BEZEL_TEST(complex_move_place_receiver_4063,
           accept_bare_call_move_complex_place_live_receiver_no_warning) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    using var y := Factory.Make(move b.f); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

BEZEL_TEST(complex_move_place_receiver_4063,
           accept_newexpr_ctor_arg_move_complex_place_live_receiver_no_warning) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    using var h := new Holder(move b.f); "
        "    return h.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

BEZEL_TEST(complex_move_place_receiver_4063,
           accept_collection_literal_move_complex_place_live_receiver_no_warning) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    var arr: OwningArray<Res> := [move b.f]; "
        "    return arr[0].tag; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

// =============================================================================
// CONTROL — a PLAIN (non-`move`) read of a complex place off a disposed
// receiver must still warn, exactly as before this fix: proves the new
// `checkRead(*un->operand, false)` call did not change or duplicate the
// PLAIN-read path — it is the SAME diagnostic, reached through the SAME
// `checkRead` machinery, just also now reached from the `move`-operand
// fallthrough for the complex-place case.
// =============================================================================
BEZEL_TEST(complex_move_place_receiver_4063,
           control_plain_read_complex_place_disposed_receiver_still_warns) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Box "
        "  public owned f: Res; "
        "  public constructor(owned r: Res) this.f := r; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class Reader "
        "  public static method Peek(r: Res): int return r.tag; end Peek; "
        "end Reader; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(new Res(1)); "
        "    b.Dispose(); "
        "    var y := Reader.Peek(b.f); "
        "    return y; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWarningMessage(r, kUseAfterDisposeFragment));
}

// =============================================================================
// CONTROL — the ORIGINAL #4023 fix's core mechanism (bare-identifier /
// `this.field` move of a DISPOSED source) must stay a precise ERROR, not a
// masked Warning: proves this fix's new fallthrough call did not
// reintroduce #4023's own masking bug for the shapes that `return` before
// reaching it.
// =============================================================================
BEZEL_TEST(complex_move_place_receiver_4063,
           control_bare_identifier_move_of_disposed_local_still_precise_error) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := Factory.Make(move x); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "cannot move from disposed local `x` (§28.3)"));
}
