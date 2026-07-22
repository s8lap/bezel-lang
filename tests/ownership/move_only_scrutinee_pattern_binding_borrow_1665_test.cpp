// Spec-derived tests for move-only scrutinee pattern bindings being borrows
// (issue #1665, spec §19.3 / §15.3, v0.184).
//
// §19.3 (Patterns), "Pattern binding and ownership":
//   "For a scrutinee of move-only value type (§15.3 — an owning inline
//    array), the binding is a borrow of the value: a copy is forbidden and
//    the case construct never moves from the scrutinee, so the borrow form
//    is the only consistent option — the same reasoning as closure capture
//    of a move-only local (§28.9). Slot reads through the binding are
//    admitted; `move s[i]` through the binding is rejected (the binding
//    does not own the array). The narrowing produced by the case arm is
//    purely informational about the type tag; it does not change the
//    ownership state of the scrutinee."
// §15.3 (Move-only value types), "Pattern binding" bullet (v0.184):
//   "a `case`/`when` binding over a move-only scrutinee is a **borrow** of
//    the value; consuming operations through the binding (`move s[i]`) are
//    rejected (§19.3)."
//
// Before this fix, the ownership/typecheck pass did not classify a case
// pattern binding over a move-only owning-inline-array scrutinee as a
// borrow: `move s[i]` through the binding compiled clean. Because codegen
// backs a value-type pattern binding with its own alloca (a bitwise copy of
// the scrutinee's slots, including the raw owned-class pointers), a `move`
// through the binding extracted a pointer the ORIGINAL scrutinee still
// considered live — a double-ownership hazard (the extracted value and the
// scrutinee's own later `move slots[i]` / scope-end dispose can both reach
// the same object). This test pins the compile-time rejection of `move
// s[i]`, and separately confirms the always-legal shape (slot reads; the
// scrutinee itself remains movable/disposable after the match) still
// compiles clean.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// `class Cell` is a disposable-shaped class (mirrors the #1549/#780 sink
// tests); `body` is the Main method body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; class Cell public n: int := 0; end Cell; "
        "class C public static method Main(): int ") + body +
        " end Main; end C;";
}

// True iff some diagnostic is the §19.3 / §15.3 move-through-pattern-binding
// rejection, naming `name` as the borrow-bound identifier.
bool rejectsMoveThroughPatternBinding(const OwnershipResult& r,
                                      const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("§19.3") == std::string::npos) continue;
        if (d.message.find("pattern binding of move-only value type") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — `move s[i]` through a case pattern binding is rejected.
// =============================================================================

// §19.3 / §15.3: the exact repro shape from #1665 — `case slots of var s =>
// move s[0]` over a `[2 owned]Cell?` scrutinee. `s` is a borrow of `slots`;
// `move s[0]` has no owned array to take from. REJECTED, naming `s`.
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           move_through_naked_binding_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var slots: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  case slots of "
        "    var s => do "
        "      var w := move s[0]; "
        "      w!.Dispose(); "
        "    end "
        "  end; "
        "  var w0 := move slots[0]; w0!.Dispose(); "
        "  var w1 := move slots[1]; w1!.Dispose(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughPatternBinding(r, "s"));
}

// §19.3 / §15.3: the same shape in EXPRESSION-position `case` (the arm body
// is `move s[0]` directly, not a `do...end` block) — the same borrow rule
// applies regardless of arm-body shape. REJECTED, naming `s`.
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           move_through_binding_rejected_in_expression_position_case) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell public n: int := 0; end Cell; "
        "class C "
        "  public static method Grab(slots: [2 owned]Cell?): int "
        "    var w := (case slots of "
        "      var s => move s[0]); "
        "    w!.Dispose(); "
        "    return 0; "
        "  end Grab; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughPatternBinding(r, "s"));
}

// §19.3 / §15.3 (v0.196, #1745, follow-up to #1665): moving the WHOLE binding
// (`var w := move s`, a bare identifier — not an `s[i]` slot) over a move-only
// `[2 owned]Cell?` scrutinee is rejected for the same reason `move s[0]` is:
// `s` is a borrow of `slots`, so laundering the whole borrowed value through
// `move s` would duplicate ownership the original scrutinee still holds (`w`'s
// scope-end auto-dispose and `slots`'s own later `move slots[i]` both reach the
// same Cells — a double-free). Before the #1745 fix the slot check matched only
// the IndexExpr shape, so `move s` compiled clean. REJECTED, naming `s`.
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           move_through_whole_naked_binding_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var slots: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  case slots of "
        "    var s => do "
        "      var w := move s; "
        "      var t0 := move w[0]; t0!.Dispose(); "
        "      var t1 := move w[1]; t1!.Dispose(); "
        "    end "
        "  end; "
        "  var w0 := move slots[0]; w0!.Dispose(); "
        "  var w1 := move slots[1]; w1!.Dispose(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughPatternBinding(r, "s"));
}

// §19.3 / §15.3 (v0.196, #1745): the whole-binding move in EXPRESSION-position
// `case` (the arm body is `move s` directly). Same borrow rule, regardless of
// arm-body shape. REJECTED, naming `s`.
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           move_through_whole_binding_rejected_in_expression_position_case) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell public n: int := 0; end Cell; "
        "class C "
        "  public static method Grab(slots: [2 owned]Cell?): int "
        "    var w := (case slots of "
        "      var s => move s); "
        "    var t0 := move w[0]; t0!.Dispose(); "
        "    var t1 := move w[1]; t1!.Dispose(); "
        "    return 0; "
        "  end Grab; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughPatternBinding(r, "s"));
}

// =============================================================================
// POSITIVE — slot reads through the binding are admitted; the scrutinee
// itself stays usable (movable, disposable) after the match.
// =============================================================================

// §19.3: "Slot reads through the binding are admitted" — a plain (non-`move`)
// indexer read `s[0]` through the pattern binding is a borrow read, legal.
// ACCEPTED.
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           slot_read_through_binding_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var slots: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  case slots of "
        "    var s => do "
        "      if s[0] <> null then "
        "        var x := 1; "
        "      end; "
        "    end "
        "  end; "
        "  var w0 := move slots[0]; w0!.Dispose(); "
        "  var w1 := move slots[1]; w1!.Dispose(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3: "the scrutinee retains whatever ownership state it had" and "the
// case construct itself never moves from the scrutinee" — after a case
// match whose arm only reads through the binding, the ORIGINAL scrutinee
// `slots` is still fully owned and may be drained via `move slots[i]`.
// ACCEPTED (no double-free, no rejected move).
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           scrutinee_still_movable_after_match) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var slots: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  case slots of "
        "    var s => do end "
        "  end; "
        "  var w0 := move slots[0]; w0!.Dispose(); "
        "  var w1 := move slots[1]; w1!.Dispose(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3 / §15.3 (v0.196, #1745): the #1745 whole-binding rejection is gated on
// the operand being a `case`/`when` pattern BINDING (a BindingPattern-declared
// LocalSymbol) — it must NOT over-reject a move of the ORIGINAL scrutinee. The
// whole-value move `move slots` of the plain-`var` move-only local (a legal
// local-to-local move, §15.3) after the match compiles clean. ACCEPTED.
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           whole_move_of_original_scrutinee_after_match_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var slots: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  case slots of "
        "    var s => do end "
        "  end; "
        "  var moved := move slots; "
        "  var w0 := move moved[0]; w0!.Dispose(); "
        "  var w1 := move moved[1]; w1!.Dispose(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3 / §15.3: `move` on a NON-move-only inline-array-typed identifier
// through a case pattern binding is untouched by this rule — the new check
// is gated on `[N owned]…` specifically, so a plain (non-owning) local's
// `move` diagnostics (whatever they may be for other reasons) are not
// spuriously attributed to the pattern-binding rule. Here the scrutinee is a
// plain class (not an inline array at all), so no pattern-binding-borrow
// diagnostic should ever fire.
BEZEL_TEST(move_only_scrutinee_pattern_binding_borrow_1665,
           non_move_only_scrutinee_binding_not_flagged_by_this_rule) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var c: Cell := new Cell(); "
        "  case c of "
        "    var d => do "
        "      var x := d.n; "
        "    end "
        "  end; "
        "end; "
        "return 0;"));
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("pattern binding of move-only "
                                         "value type") == std::string::npos);
    }
}
