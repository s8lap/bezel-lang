// Spec-derived regression tests for #3080 — §28.8.1 "Exception paths are not
// exempt" (spec v0.254): the EXCEPTION-EDGE half of the field-restoration rule.
//
// §28.8.1's restoration rule is K9.2's: "every control-flow path from a `move` of
// a field or slot to the enclosing method's exit — normal, `throw`, `yield break`,
// and every exception edge raised by an intervening throwable expression — must
// reassign the slot". The section then spells the exception half out: "a throwable
// expression between the move and the reassignment introduces an exit path on
// which the reassignment has not run, and if the destructor on `this` is reached
// via that path, it would observe the field in moved-from state — the hole a
// destructor must never observe."
//
// WHAT WAS BROKEN (#3080, measured on 88c8f93). The NORMAL-path half was enforced
// (the `Wrong` example below is rejected, and has been), but the exception half was
// entirely absent: the section's own `WrongOrder` example compiled clean, as did
// the same shape with the non-call throwables the inventory names. The accepted
// program was not merely mis-typed — built and run, it SEGV'd inside the holder's
// destructor reading the field the exception edge had left nulled, which is
// verbatim the hole quoted above (ASan: "SEGV on unknown address 0x0c … READ
// memory access … in bz$M.Holder$DisposeCore").
//
// Each test below names the §28.8.1 example it derives from. The rejection tests
// assert the DIAGNOSTIC TEXT, not merely `hasErrors()` — per #2988 a bare
// `hasErrors()` passes on an unrelated error (an unknown member, say) and so does
// not witness the rule it claims to.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §28.8.1 exception-edge diagnostic's stable identifier: the §-reference plus
// the section heading it quotes. Deliberately NOT the whole sentence, so wording
// tweaks do not break these tests (support/ownership_support.hpp's guidance).
constexpr const char* kEdgeDiag = "§28.8.1 \"Exception paths are not exempt\"";

// The PRE-EXISTING at-method-exit diagnostic (the normal-path half). Tests that
// pin "this shape's behaviour did not change" assert this one.
constexpr const char* kExitDiag = "left in moved-from state at method exit";

// `Res` is a disposable class (it declares a destructor, so it is NOT
// trivial-dispose per §28.9 and the owned-field mechanics below are real).
// `members` is spliced into `Holder`, which holds the `owned` field the §28.8.1
// examples move out of.
std::string prog(const std::string& members) {
    return std::string(
        "module M; "
        "class Res "
        "  public Tag: int := 7; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  private owned _r: Res; "
        "  public constructor() _r := new Res(); end constructor; "
        "  private static method Dangerous(): int return 1; end Dangerous; "
        "  private static method MakeRes(): owned Res return new Res(); "
        "  end MakeRes; ") + members +
        " end Holder;";
}

// True when some ERROR-severity diagnostic carries `needle` AND names a real
// source location. A diagnostic reported at 0:0 is unusable to a reader and means
// the code that named the location was not the code that found the problem.
bool hasLocatedError(const bezel::test::OwnershipResult& r,
                     const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) == std::string::npos) continue;
        if (d.location.line == 0 && d.location.column == 0) continue;
        return true;
    }
    return false;
}

} // namespace

// --- The rule: the section's own `WrongOrder` example ---------------------

// §28.8.1 "Exception paths are not exempt", the `WrongOrder` example verbatim in
// shape: move the field, call a throwable factory, then restore. "ERROR: if
// DangerousFactory throws, _resource is moved but never reassigned; the exception
// edge is an unrestored exit." A method call is the inventory's first entry
// ("Method calls … are throwable"). THIS IS THE BUG #3080 PINS.
BEZEL_TEST(own_throwable_edge_3080, wrongorder_method_call_between_move_and_restore_rejected_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method WrongOrder(): owned Res "
        "  var old := move _r; "
        "  var q := Dangerous(); "
        "  _r := new Res(); "
        "  return old; "
        "end WrongOrder;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
    // The diagnostic must name the field whose obligation is unmet...
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "owned field `_r`"));
    // ...and must point at a real location (never 0:0).
    BEZEL_EXPECT_TRUE(hasLocatedError(r, kEdgeDiag));
}

// §28.8.1's inventory also names "division and modulo (which can throw
// `DivideByZeroException`)". The same shape with `/` between the move and the
// restore is the same unrestored exception edge.
BEZEL_TEST(own_throwable_edge_3080, wrongorder_division_between_move_and_restore_rejected_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method TakeWithDiv(a: int, b: int): owned Res "
        "  var old := move _r; "
        "  var q := a / b; "
        "  _r := new Res(); "
        "  return old; "
        "end TakeWithDiv;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}

// ...and modulo, the other half of that same inventory entry. Bezel spells it
// `mod`, not `%` (§17.1's precedence table, level 3: `*` `/` `mod`) — an earlier
// draft of the fix's classifier tested for `%`, which never matches, and this test
// is what caught it.
BEZEL_TEST(own_throwable_edge_3080, wrongorder_modulo_between_move_and_restore_rejected_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method TakeWithMod(a: int, b: int): owned Res "
        "  var old := move _r; "
        "  var q := a mod b; "
        "  _r := new Res(); "
        "  return old; "
        "end TakeWithMod;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}

// A throwable call in the RESTORING assignment's own RHS is the same violation:
// the call is evaluated BEFORE the store, so its exception edge leaves the field
// moved-from. This is precisely why §28.8.1's `RightLoop` fix hoists
// `MakeStream()` above the move instead of restoring with it directly.
BEZEL_TEST(own_throwable_edge_3080, throwable_call_in_restore_rhs_rejected_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method TakeRestoreFromCall(): owned Res "
        "  var old := move _r; "
        "  _r := MakeRes(); "
        "  return old; "
        "end TakeRestoreFromCall;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}

// --- The accepted forms: the section's own fixes must stay legal -----------

// §28.8.1 `RightOrder`: "The fix is to perform any throwable construction
// *before* the move, leaving only non-throwable assignments and the return
// between the move and the exits." Must compile clean — this is the
// over-rejection guard for the whole fix.
BEZEL_TEST(own_throwable_edge_3080, rightorder_build_before_take_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method RightOrder(): owned Res "
        "  var fresh := MakeRes(); "
        "  var old := move _r; "
        "  _r := fresh; "
        "  return old; "
        "end RightOrder;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 `Right`: a `new` in the restore position, with the replacement built
// before the move. A `new` expression is NOT in §28.8.1's throwable inventory, so
// the shape stays legal.
BEZEL_TEST(own_throwable_edge_3080, right_new_replacement_built_before_move_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method Right(): owned Res "
        "  var fresh := new Res(); "
        "  var old := move _r; "
        "  _r := fresh; "
        "  return old; "
        "end Right;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 `TransferToHelperSafe`: "the fix is to construct the replacement and
// reassign before the call" — restore first, THEN make the throwable call. The
// field is not moved-from when the call runs, so no edge is unrestored.
BEZEL_TEST(own_throwable_edge_3080, transfer_to_helper_safe_restore_before_call_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method TransferSafe(): owned Res "
        "  var fresh := new Res(); "
        "  var taken := move _r; "
        "  _r := fresh; "
        "  var q := Dangerous(); "
        "  return taken; "
        "end TransferSafe;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 "The `as` operator is **not** throwable … Code that uses `as` between a
// move and the field-restoration point does not introduce an unrestored exception
// edge." The section's explicit carve-out, asserted directly.
BEZEL_TEST(own_throwable_edge_3080, as_operator_between_move_and_restore_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method WithAs(o: Object): owned Res "
        "  var old := move _r; "
        "  var probe := o as Res; "
        "  _r := new Res(); "
        "  return old; "
        "end WithAs;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
}

// --- No churn on the shapes the NORMAL-path half already governed ---------

// §28.8.1 `Wrong`: the field is restored only on the true branch. This was, and
// must remain, an at-method-exit rejection — the exception-edge check must NOT
// also fire (the restore uses `new`, not a throwable), so the diagnostic a reader
// sees for this classic shape is unchanged.
BEZEL_TEST(own_throwable_edge_3080, wrong_conditional_restore_keeps_exit_diagnostic_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method Wrong(replace: bool): owned Res "
        "  var old := move _r; "
        "  if replace then _r := new Res(); end; "
        "  return old; "
        "end Wrong;"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kExitDiag));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
}

// K9.3 / §28.8.1 "Constructors and destructors": "the field-restoration rule does
// *not* require reassignment inside a destructor (K9.3 — teardown, no future
// read)". The exception-edge half takes the SAME exemption, for the same reason:
// there is no future read to protect.
BEZEL_TEST(own_throwable_edge_3080, destructor_exempt_from_exception_edge_rule_k9_3) {
    auto r = parseAndCheckOwnership(prog(
        "destructor "
        "  using var t := move _r; "
        "  var q := Dangerous(); "
        "end destructor;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
}

// A method that never moves the field is untouched by the rule however many
// throwable expressions it contains — the check is gated on the field being
// moved-from, not on throwability alone.
BEZEL_TEST(own_throwable_edge_3080, no_move_means_no_obligation_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method NoMove(a: int, b: int): int "
        "  var q := Dangerous(); "
        "  var w := a / b; "
        "  return q + w; "
        "end NoMove;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
}

// --- The two granularity skips, pinned -----------------------------------

// A `case` arm that restores the field and only THEN evaluates a throwable is
// legal under §28.8.1: on that arm's path nothing is unrestored when the throwable
// runs. The CFG appends a CaseStmt as a single OPAQUE node (flow/CFG.cpp visitStmt
// has no case handler), so the exception-edge check receives the whole `case` and
// would otherwise judge every arm's expressions against the PRE-case entry state —
// a FALSE REJECT. Measured: this shape compiled clean at 88c8f93 and was rejected
// at bcbc553 before the CaseStmt skip landed. This test pins the skip; deleting it
// makes this test fail.
BEZEL_TEST(own_throwable_edge_3080, case_arm_restore_then_throwable_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method CaseRestoreThenThrowable(tag: int): owned Res "
        "  var old := move _r; "
        "  case tag of "
        "    1 => do _r := new Res(); var q := Dangerous(); end, "
        "    _ => do _r := new Res(); end, "
        "  end; "
        "  return old; "
        "end CaseRestoreThenThrowable;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A move inside a `try` whose catch arm restores the field: the throwable call's
// exception edge does NOT leave the method — it lands in the catch, which restores —
// so §28.8.1's "path to the method's exit" is satisfied and the shape is legal.
// This is what `stmtsInsideTry_` exists for; without that set the call would be
// reported. Deleting the `stmtsInsideTry_` skip makes this test fail.
BEZEL_TEST(own_throwable_edge_3080, in_try_move_with_catch_restore_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(prog(
        "public method InTryCatchRestores(): owned Res "
        "  var old := move _r; "
        "  try "
        "    var q := Dangerous(); "
        "    _r := new Res(); "
        "  catch var ex: Exception => "
        "    _r := new Res(); "
        "  end; "
        "  return old; "
        "end InTryCatchRestores;"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kEdgeDiag));
}
