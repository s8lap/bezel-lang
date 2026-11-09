// fix #8347 + fix #8366 (spec §28.8.1 *The `move` expression*, Appendix A.7
// `MoveExpr`, §28.3 K3.13 *Marker economy*, v0.355): the `move` marker
// written on a FRESH PRODUCER is refused at the three positions that were
// still open after narrow #7926 closed the binding position.
//
// THE RULE. §28.8.1 (docs/Bezel_Language_Spec.md:8049), verbatim:
//
//   "No other operand is admitted (A.7): a fresh producer (a `new`
//    expression, a call returning `owned T`, an owned operator result) needs
//    no marker anywhere (K3.13) and takes none, so `move (new Cell(1))` is a
//    compile error at every position, an `owned` argument and an owned-slot
//    store alike; the remedy is to drop the `move`."
//
// K3.13 (spec:6827), verbatim: "Fresh producers need no marker anywhere (no
// live binding is invalidated)."
//
// THE THREE POSITIONS THIS LANE NARROWS (issue #8347 and issue #8366):
//
//   (1) the `return` OPERAND — an `owned` return   (#8347, position 1)
//   (2) an ASSIGNMENT's right-hand side             (#8347, position 2)
//   (3) the DISCARD's right-hand side               (#8366)
//
// The operand rule is POSITION-INDEPENDENT, so it applies at all three, and
// at both operand kinds §28.8.1 names — a `new` expression and a call
// returning `owned T`.
//
// THE DIAGNOSTIC. The wording is the OPERAND-SHAPE wording, BYTE-IDENTICAL
// to narrow #7926's binding-position diagnostic and sharing the needle
// "not written on a fresh producer" with the ownership pass's §22.3 call-site
// twin — so the one rule speaks with one voice at every position where it
// fires.
//
// WHY NOT THE REDUNDANCY WORDING. At the `return` operand and at the discard
// RHS, Appendix A.7's marker-free list also makes the marker "redundant",
// and the ownership pass already rejects a `move` over a NAMED BINDING there
// ("the `move` marker is redundant on a `return` operand …", src/ownership/
// Ownership.cpp). That rule's operand is a named binding; THIS rule's operand
// is a fresh producer — not a binding at all, so there is no live binding to
// invalidate and no announcement to be redundant with. The two rules are
// DISJOINT in operand shape (a fresh producer is never a named binding), so
// they never double-report at one position; rows C14 and C15 below pin that.
//
// ROWS:
//   R1  `return move W.Fresh();`       — the OWNED RETURN, the CALL operand
//   R2  `return move (new Cell(3));`   — the OWNED RETURN, the `new` operand
//   R3  `x := move W.Fresh();`         — the ASSIGNMENT RHS, the CALL operand
//   R4  `x := move (new Cell(3));`     — the ASSIGNMENT RHS, the `new` operand
//   R5  `_ := move W.Fresh();`         — the DISCARD RHS, the CALL operand
//   R6  `_ := move (new Cell(3));`     — the DISCARD RHS, the `new` operand
//   R7  the BARE `new` spelling at all three positions (the same AST node as
//       the parenthesised one — source-level parens leave no `ParenExpr`
//       node, §17.1)
//   R8  the `!` spelling (`move W.Fresh()!`) at all three positions — the
//       `!` is TRANSPARENT to the marker (§17.8 / #5144)
//   R9  the parenthesised operand spelling (`return (move W.Fresh());`)
//   R10 a `return` inside a LAMBDA's block body — the same `return` operand
//       position
//
//   C1  CONTROL: a NAMED LOCAL at a local assignment RHS — Appendix A.7's
//       alternative (c) — still ADMITTED
//   C2  CONTROL: an OWNING SLOT at a local assignment RHS — A.7's
//       alternative (b) — still ADMITTED
//   C3  CONTROL: an OWNED FIELD at a binding position — A.7's alternative
//       (a) — still ADMITTED
//   C4  CONTROL: a `return` with NO marker — unchanged
//   C5  CONTROL: an assignment with NO marker — unchanged
//   C6  CONTROL: a discard with NO marker — unchanged
//   C7  CONTROL: the #7926 LEG — `var a := move (new Cell());` at a binding
//       position is still REJECTED, with the binding-position diagnostic
//   C8  CONTROL: the CALL-SITE LEG — `Take(move (new Cell()));` at an owned
//       argument is still REJECTED, with §22.3's own call-site diagnostic
//   C9  CONTROL: the OWNED-SLOT STORE — `arr.Add(move (new Item(3)));` —
//       issue #8330's position, still ADMITTED (this lane does not touch it)
//   C10 CONTROL: the OWNING-SLOT STORE by ASSIGNMENT — `a[0] := move (new
//       Item(3));` — §28.3's consume-table row K3.1, issue #8330's position,
//       still ADMITTED (this lane does not touch it)
//   C11 CONTROL: a MEMBER's `=>` EXPRESSION BODY — `Give(): owned Cell =>
//       move W.Fresh();` — a different position (not A.7's `return` operand),
//       still ADMITTED
//   C12 CONTROL: a LAMBDA's `=>` EXPRESSION BODY — still ADMITTED
//   C13 CONTROL: the `??=` RIGHT-HAND SIDE — §7.7's own compound form,
//       still ADMITTED
//   C14 CONTROL: `return move y;` — a NAMED OWNED LOCAL at an owned return —
//       still REJECTED, and with the OWNERSHIP PASS's redundancy diagnostic,
//       NOT this lane's: the two rules are disjoint in operand shape
//   C15 CONTROL: `_ := move y;` — the same, at the discard RHS

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The stable substring of the fresh-producer diagnostic — §22.3's own
// wording, shared by narrow #7926's binding-position diagnostic, the
// ownership pass's §22.3 call-site twin, and this lane's three positions.
constexpr const char* kFreshProducer = "not written on a fresh producer";

// The stable substring that names the CALL operand shape.
constexpr const char* kFreshProducerCall =
    "fresh producer (a call returning `owned T`)";

// The stable substring that names the `new` operand shape. It distinguishes
// this lane's diagnostic ("... a fresh producer (a `new` expression) — ...")
// from §22.3's CALL-SITE one ("... a fresh producer (a `new` expression or a
// call returning an owned result) — ..."), which the closing parenthesis
// makes non-matching.
constexpr const char* kFreshProducerNew = "fresh producer (a `new` expression)";

// The stable substring of the ownership pass's own CALL-SITE fresh-producer
// diagnostic (§22.3) — the position this lane does NOT touch.
constexpr const char* kCallSiteFreshProducer =
    "the `move` call-site marker is not written on a fresh producer";

// The stable substring of the ownership pass's REDUNDANCY diagnostic at a
// marker-free position — the other rule, whose operand is a NAMED BINDING.
constexpr const char* kRedundantMarker = "the `move` marker is redundant";

// A `Cell` with a destructor — a fresh producer of a class type, so the
// §28.3 must-use auto-drop warning fires on a plain `var` binding.
const char* kCell =
    "class Cell "
    "  public constructor(id: int) end constructor; "
    "  destructor end destructor; "
    "end Cell; ";

// The fresh-producer CALL: a static method declared to return `owned Cell`.
const char* kFreshCall =
    "class W "
    "  public static method Fresh(): owned Cell return new Cell(3); end Fresh; "
    "end W; ";

}  // namespace

// R1 — the OWNED RETURN, the CALL operand. Issue #8347's own repro.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           owned_return_of_a_fresh_producer_call_is_refused) {
    auto r = runAot(
        "module M8347R1; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Give(): owned Cell return move W.Fresh(); end Give; "
        "  public static method Main(): int "
        "    using var g := Program.Give(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerCall));
}

// R2 — the OWNED RETURN, the `new` operand.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           owned_return_of_a_fresh_producer_new_is_refused) {
    auto r = runAot(
        "module M8347R2; " + std::string(kCell) +
        "class Program "
        "  public static method Give(): owned Cell return move (new Cell(3)); end Give; "
        "  public static method Main(): int "
        "    using var g := Program.Give(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerNew));
}

// R3 — the ASSIGNMENT RHS, the CALL operand. Issue #8347's own repro.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           assignment_rhs_of_a_fresh_producer_call_is_refused) {
    auto r = runAot(
        "module M8347R3; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Main(): int "
        "    var x: Cell := new Cell(1); "
        "    x := move W.Fresh(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerCall));
}

// R4 — the ASSIGNMENT RHS, the `new` operand.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           assignment_rhs_of_a_fresh_producer_new_is_refused) {
    auto r = runAot(
        "module M8347R4; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    var x: Cell := new Cell(1); "
        "    x := move (new Cell(3)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerNew));
}

// R5 — the DISCARD RHS, the CALL operand. Issue #8366's own repro.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           discard_rhs_of_a_fresh_producer_call_is_refused) {
    auto r = runAot(
        "module M8366R5; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Main(): int "
        "    _ := move W.Fresh(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerCall));
}

// R6 — the DISCARD RHS, the `new` operand.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           discard_rhs_of_a_fresh_producer_new_is_refused) {
    auto r = runAot(
        "module M8366R6; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    _ := move (new Cell(3)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerNew));
}

// R7 — the BARE `new` spelling at all three positions. It is the SAME AST
// node as the parenthesised spelling (source-level parens leave no
// `ParenExpr` node — the parser stamps `parenthesized` on the node inside the
// group, §17.1), so the one arm rejects both spellings; this row pins that
// the rule is about the SHAPE and not about the spelling.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           bare_new_spelling_is_refused_at_all_three_positions) {
    auto r = runAot(
        "module M8347R7; " + std::string(kCell) +
        "class Program "
        "  public static method Give(): owned Cell return move new Cell(3); end Give; "
        "  public static method Main(): int "
        "    var x: Cell := new Cell(1); "
        "    x := move new Cell(3); "
        "    _ := move new Cell(3); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerNew));
}

// R8 — the `!` spelling at all three positions. §17.8 / #5144: the `!` is
// TRANSPARENT to the marker, so `move x!` is the same marker as `move x`.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           force_unwrap_spelling_is_refused_at_all_three_positions) {
    auto r = runAot(
        "module M8347R8; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Give(): owned Cell return move W.Fresh()!; end Give; "
        "  public static method Main(): int "
        "    var x: Cell := new Cell(1); "
        "    x := move W.Fresh()!; "
        "    _ := move W.Fresh()!; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerCall));
}

// R9 — the PARENTHESISED OPERAND spelling at the `return` operand.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           parenthesised_operand_spelling_is_refused) {
    auto r = runAot(
        "module M8347R9; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Give(): owned Cell return (move W.Fresh()); end Give; "
        "  public static method Main(): int "
        "    using var g := Program.Give(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerCall));
}

// R10 — a `return` inside a LAMBDA's BLOCK BODY is the same `return` operand
// position: the operand rule is position-independent.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           lambda_block_body_return_of_a_fresh_producer_is_refused) {
    auto r = runAot(
        "module M8347R10; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Main(): int "
        "    var f: (owned Cell) -> owned Cell := "
        "      lambda(owned p: Cell) => do return move W.Fresh(); end; "
        "    using var g := f(new Cell(1)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerCall));
}

// C1 — CONTROL: a NAMED LOCAL at a local assignment RHS is Appendix A.7's
// alternative (c) — the marker is REQUIRED there (§28.3's consume table,
// row K3.4), so it must stay ADMITTED.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_named_local_at_an_assignment_rhs_is_still_admitted) {
    auto r = runAot(
        "module M8347C1; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    var x: Cell := new Cell(1); "
        "    var y := new Cell(2); "
        "    x := move y; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C2 — CONTROL: an OWNING SLOT at a local assignment RHS is A.7's
// alternative (b) — must stay ADMITTED.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_owning_slot_at_an_assignment_rhs_is_still_admitted) {
    auto r = runAot(
        "module M8347C2; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    using var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell(1)); "
        "    var x: Cell := new Cell(2); "
        "    x := move a[0]; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C3 — CONTROL: an OWNED FIELD is A.7's alternative (a) — must stay
// ADMITTED.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_owned_field_is_still_admitted) {
    auto r = runAot(
        "module M8347C3; " + std::string(kCell) +
        "class H "
        "  public owned _f: Cell; "
        "  public constructor() _f := new Cell(1); end constructor; "
        "  destructor end destructor; "
        "  public method Give(): owned Cell "
        "    var r := move _f; "
        "    _f := new Cell(2); "
        "    return r; "
        "  end Give; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    using var c := h.Give(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C4 — CONTROL: a `return` with NO marker — unchanged.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_owned_return_without_a_marker_is_unchanged) {
    auto r = runAot(
        "module M8347C4; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Give(): owned Cell return W.Fresh(); end Give; "
        "  public static method Main(): int "
        "    using var g := Program.Give(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C5 — CONTROL: an assignment with NO marker — unchanged.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_assignment_without_a_marker_is_unchanged) {
    auto r = runAot(
        "module M8347C5; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Main(): int "
        "    var x: Cell := new Cell(1); "
        "    x := W.Fresh(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C6 — CONTROL: a discard with NO marker — unchanged.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_discard_without_a_marker_is_unchanged) {
    auto r = runAot(
        "module M8347C6; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Main(): int "
        "    _ := W.Fresh(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C7 — CONTROL: the #7926 LEG — the binding position is still REJECTED, and
// still with the binding-position diagnostic.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_binding_position_keeps_its_own_diagnostic) {
    auto r = runAot(
        "module M8347C7; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    var c := move (new Cell(1)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerNew));
}

// C8 — CONTROL: the CALL-SITE LEG — an owned ARGUMENT is still REJECTED, and
// still by the OWNERSHIP PASS's own §22.3 call-site diagnostic. This lane's
// rule must not take the call-site position over.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_owned_argument_keeps_the_call_site_diagnostic) {
    auto r = runAot(
        "module M8347C8; " + std::string(kCell) +
        "class Program "
        "  public static method Take(owned p: Cell) "
        "    using var h := move p; "
        "  end Take; "
        "  public static method Main(): int "
        "    Take(move (new Cell(1))); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCallSiteFreshProducer));
}

// C9 — CONTROL: the OWNED-SLOT STORE — `arr.Add(move (new Item(3)));` — is
// issue #8330's position. This lane does not touch it.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_owned_slot_store_is_still_admitted) {
    auto r = runAot(
        "module M8347C9; "
        "class Item "
        "  public constructor(v: int) end constructor; "
        "  destructor end destructor; "
        "end Item; "
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<Item>(); "
        "    arr.Add(move (new Item(3))); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C10 — CONTROL: the OWNING-SLOT STORE by ASSIGNMENT — `a[0] := move (new
// Item(3));` — is §28.3's consume-table row K3.1 ("store into an `owned`
// field or owning-collection slot", whose Spelling column is "none — the
// declared slot announces (marker-free)"). That is issue #8330's position,
// not issue #8347's K3.4 assignment RHS; this lane does not touch it.
//
// The slot must EXIST for the program to run, so the row adds one element
// first; the row is about the COMPILE verdict (the marker is still admitted
// at this position), and the runtime exit code is the supplementary check
// that the program behaves as before the change.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_owning_slot_store_by_assignment_is_still_admitted) {
    auto r = runAot(
        "module M8347C10; "
        "class Item "
        "  public constructor(v: int) end constructor; "
        "  destructor end destructor; "
        "end Item; "
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<Item>(); "
        "    arr.Add(new Item(1)); "
        "    arr[0] := move (new Item(3)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C11 — CONTROL: a MEMBER's `=>` EXPRESSION BODY is a different position
// (it is not A.7's "`return` operand"), and it is pinned ACCEPTED by
// existing tests; this lane does not touch it.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_member_expression_body_is_still_admitted) {
    auto r = runAot(
        "module M8347C11; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Give(): owned Cell => move W.Fresh(); "
        "  public static method Main(): int "
        "    using var g := Program.Give(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C12 — CONTROL: a LAMBDA's `=>` EXPRESSION BODY is the same shape of
// position and is likewise out of this lane's scope.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_lambda_expression_body_is_still_admitted) {
    auto r = runAot(
        "module M8347C12; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Main(): int "
        "    var f: (owned Cell) -> owned Cell := lambda(owned p: Cell) => move W.Fresh(); "
        "    using var g := f(new Cell(1)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C13 — CONTROL: the `??=` RIGHT-HAND SIDE is §7.7's own compound form (not
// the plain `:=` assignment RHS of §28.3's consume-table row K3.4), and it
// stays ADMITTED.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_coalescing_assignment_rhs_is_still_admitted) {
    auto r = runAot(
        "module M8347C13; " + std::string(kCell) + kFreshCall +
        "class Program "
        "  public static method Main(): int "
        "    var x: Cell := new Cell(1); "
        "    x ??= move W.Fresh(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C14 — CONTROL: `return move y;` — a NAMED OWNED LOCAL at an owned return —
// is still REJECTED, and with the OWNERSHIP PASS's REDUNDANCY diagnostic,
// NOT this lane's operand-shape diagnostic. The two rules are DISJOINT in
// operand shape (a fresh producer is never a named binding), so this lane's
// rule did not take the marker-free position over.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_named_local_at_an_owned_return_keeps_the_redundancy_rule) {
    auto r = runAot(
        "module M8347C14; " + std::string(kCell) +
        "class Program "
        "  public static method Give(): owned Cell "
        "    var y := new Cell(2); "
        "    return move y; "
        "  end Give; "
        "  public static method Main(): int "
        "    using var g := Program.Give(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRedundantMarker));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kFreshProducer));
}

// C15 — CONTROL: `_ := move y;` — the same, at the discard RHS.
BEZEL_TEST(sema_move_fresh_producer_positions_8347_8366,
           control_named_local_at_a_discard_rhs_keeps_the_redundancy_rule) {
    auto r = runAot(
        "module M8347C15; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    var y := new Cell(2); "
        "    _ := move y; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRedundantMarker));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kFreshProducer));
}
