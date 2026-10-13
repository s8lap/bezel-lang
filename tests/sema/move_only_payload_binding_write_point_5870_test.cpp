// fix #5870 — §19.3 *Move-only payload bindings across a write point*,
// v0.334. The rule was required in THREE places and enforced in NONE:
// measured before this fix, `grep -rniE "writePoint|write point|
// provablyUnwritten|provably unwritten" src/` returns ZERO hits.
//
// QUOTED (§19.3, :4039, v0.334) — the rule:
//
//   "Such a binding may therefore not be **live at a write point** of the
//    construct, **unless the scrutinee's source is provably unwritten during
//    the construct**."
//
// QUOTED (§15.3 *Pattern binding*, :2891, v0.334) — the restatement:
//
//   "A borrowing-match binding of a move-only payload may not be live at a
//    write point of the construct unless the scrutinee's source is provably
//    unwritten."
//
// QUOTED (§9.4 *Move-only receivers*, :1407, v0.334) — the router, which
// names BOTH shapes this fix reaches:
//
//   "What such a store must never do is leave a borrow addressing bytes it
//    has rewritten. Two borrows could. One is a payload binding of a
//    borrowing match over `this`. The other is a `this` that addresses
//    payload bytes inside another value, because the receiver expression
//    was such a binding. §19.3's rule *Move-only payload bindings across a
//    write point* rejects both: `this`'s source is the caller's, which no
//    member body sees, so it is never provably unwritten, and a binding used
//    as a receiver is live across the call."
//
// QUOTED (§19.3, :4046, v0.334) — the whitelist:
//
//   "The scrutinee's source is **provably unwritten** in exactly these cases:
//    - a **stable local** of the body over the construct, other than a binding
//      a borrowing match introduces …
//    - storage inside such a local's value, reached from it without leaving the
//      value …
//    - a `readonly` field outside its construction window …
//    - a borrowing-match binding whose own scrutinee's source is provably
//      unwritten."
//
//   and immediately, :4046: "`this` in a value-kind member body is never such a
//   source; neither is any other field, an element, a static, or a property or
//   indexer read."
//
// ── WHY A COMPILE-TIME REJECTION (the differential this file pins) ──────────
//
// A borrowing-match binding of a move-only payload "borrows the payload **in
// place**. It addresses storage inside the scrutinee's source, and a store to
// that source while the binding is live would leave it addressing another
// case's bytes, a confusion no disposed flag backstacks (§K4.1)" (§19.3).
//
// MEASURED on the pre-fix base `d2a871ca` with the R1 program below:
//
//   stdout:  1
//           758840747
//   exit:    0
//
// The program COMPILES and RUNS, and the second line is a garbage pointer read
// — neither the snapshot value (1) nor the field's current value (9). The
// binding is a SNAPSHOT COPY: `lowerCaseStmt` does `CreateStore(scrut, slot)`
// (src/codegen/LlvmBackend_Stmts.cpp:11203), so after the store the copy's
// bytes are whatever the field's storage was overwritten with.
//
// THIS IS THE DIFFERENTIAL THE FILE ASSERTS, and it deliberately reads a SLOT
// VALUE, never `.Length`: the restored array is also length 2, so snapshot and
// view print the same number and a `.Length` assertion pins nothing. A6 pins
// the value channel (`1` / `3` through the binding, `1` through the local).
//
// Commit `591fe2243` ("recompute field addr for owning-inline-array case
// binding") targeted this same repro by rerouting the binding through
// `fieldAddr`. It is DECLINED: its premise is wrong (a snapshot copy cannot be
// rerouted into a view) and landing it would convert a REQUIRED REJECTION into
// a silently-wrong runtime answer. This file pins the rejection the spec asks
// for, so a future reroute cannot silently un-reject R1: R1 asserts the
// diagnostic TEXT, which the reroute does not produce.
//
// ── ROWS ────────────────────────────────────────────────────────────────────
//
// REJECT direction (each fails WITHOUT the fix, passes WITH it):
//   R1  class-field source, a store between two uses  — the repro
//   R2  `this`-rooted source inside the member body    — §9.4's first borrow
//   R3  a stable local that IS written in the construct — the whitelist's
//       first case, excluded
//   R4  the binding used as a RECEIVER, with a write inside the call
//                                                      — §9.4's second borrow
//   R5  the write point is in a NESTED `case` inside the arm ("in the arm")
//   R6  determinism: the same program compiled 50× rejects every time, and the
//       diagnostic is emitted EXACTLY ONCE per compile
//   R7  the EXPRESSION-position spelling of `case` (§A.7's parenthesized
//       atom) — the same rule, so the two spellings cannot drift
//
// ADMIT direction — a check that rejects everything is a silent denial of
// valid programs, not a fix. Each row names what fails if the rule were
// over-applied:
//   A1  a stable local NOT written in the construct; the construct writes an
//       unrelated local. Fails if the whitelist is not applied, or if the
//       region is taken per-arm rather than per-construct (an unrelated write
//       in the same arm must not disqualify).
//   A2  the binding is never USED. "Live" is §K5.5's sense — some later use —
//       and no use means nothing can be live at a write point. Fails if
//       liveness is not required.
//   A3  a COPYABLE inline array (`[3]int`, no `owned`) over a local that IS
//       written: no obligation, admitted. Fails if "inline array" is used as
//       the move-only test instead of "owning inline array".
//   A4  a `readonly` field outside its construction window. Fails if the
//       `readonly` whitelist case is missing.
//   A5  THE REMEDY: read what the arm needs BEFORE the write point. Fails if
//       the rule rejects on "a write point exists" rather than "a use is live
//       at a write point".
//   A6  the value differential: through the binding, a copyable and a move-only
//       array both read their slots (1, 3) and the local reads 1.
//   A7  §4046's arm scoping: a write point in a DECLINED arm's guard is not a
//       write point for a later arm's bindings, so the program is admitted.
//       (Its runtime VALUE is deliberately not asserted — see the row.)
//   A8  a PARAMETER is a stable local too (§7.4: "a local variable or parameter
//       of that body"), and a `case` scrutinee is often a formal.
//
// Oracle: `runAot` (the strict AOT oracle — fork-isolated Driver::compileFile
// + clang link + native exec). Reject rows assert the SPECIFIC diagnostic and
// that it is reported at a real source location; admit rows assert the program
// RAN and its stdout. No row asserts only "compiles" or only "does not crash".

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The diagnostic's stable core. The full wording changes with the remedy text;
// this substring is the rule's own predicate and is what a future reroute
// (591fe2243) would NOT produce.
const char kInPlace[] = "borrows its payload IN PLACE";

// A disposable cell carrying the payload value the differential reads.
const char kCell[] =
    "module M; "
    "class Cell "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Cell; ";

// R1/R4/R5 — a class field, so `h.Slots` leaves the value at a
// class-typed hop (§9.4's key) and the source is never provably unwritten.
const char kHolder[] =
    "class Holder "
    "  public Slots: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
    "end Holder; ";

const char kMainClass[] = "class C public static method Main(): int ";
const char kEndMain[] = " end Main; end C;";

std::string prog(const std::string& decls, const std::string& body) {
    return std::string(kCell) + decls + kMainClass + body + kEndMain;
}

}  // namespace

// ── R1 — the repro. A class-field source, a store between two uses.
// MEASURED pre-fix (base d2a871ca): exit 0, stdout "1\n758840747\n" — the
// second read is a garbage pointer, neither the snapshot (1) nor the field's
// current value (9). §19.3 requires a compile error at the second use.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           r1_class_field_source_store_between_uses_rejected) {
    auto r = runAot(prog(kHolder,
        "using var h := new Holder(); "
        "case h.Slots of "
        "  var a => do "
        "    Console.WriteLine($\"{a[0].V}\"); "
        "    h.Slots := [new Cell(9), new Cell(9)]; "
        "    Console.WriteLine($\"{a[0].V}\"); "
        "  end "
        "end; "
        "return 0; "));
    // The diagnostic, not merely "it did not run" — a crash, a timeout or an
    // unrelated reject would all satisfy the weaker predicate.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInPlace));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // Reported at the USE, exactly once — a per-use flood is its own defect.
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kInPlace), 1);
}

// ── R2 — §9.4's FIRST borrow: "`this`'s source is the caller's, which no
// member body sees, so it is never provably unwritten". `this.Slots` inside
// the member body that also writes it.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           r2_this_rooted_source_rejected_spec_9_4) {
    auto r = runAot(prog(
        "class Holder "
        "  public Slots: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "  public method Bump(): int "
        "    case this.Slots of "
        "      var a => do "
        "        this.Slots := [new Cell(9), new Cell(9)]; "
        "        Console.WriteLine($\"{a[0].V}\"); "
        "      end "
        "    end; "
        "    return 0; "
        "  end Bump; "
        "end Holder; ",
        "using var h := new Holder(); return h.Bump(); "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInPlace));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── R3 — the whitelist's FIRST case, excluded: "a stable local of the body
// over the construct". A local IS stable only while nothing writes it, so a
// construct that writes it takes the binding out of the whitelist.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           r3_local_written_in_construct_rejected) {
    auto r = runAot(prog("",
        "var arr: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "case arr of "
        "  var a => do "
        "    arr := [new Cell(9), new Cell(9)]; "
        "    Console.WriteLine($\"{a[0].V}\"); "
        "  end "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInPlace));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── R4 — §9.4's SECOND borrow: "a binding used as a receiver is live across
// the call". The USE precedes the write point in evaluation order; the write
// is inside the call's ARGUMENT evaluation, so only the "live across" clause
// catches it — the plain ordering rule alone would admit this program.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           r4_binding_as_receiver_live_across_call_rejected_spec_9_4) {
    auto r = runAot(prog(
        "class Holder "
        "  public Slots: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "  public method Fresh(): Cell? "
        "    this.Slots := [new Cell(9), new Cell(9)]; return null; "
        "  end Fresh; "
        "end Holder; ",
        "using var h := new Holder(); "
        "case h.Slots of "
        "  var a => do "
        "    var found := a.Contains(h.Fresh()); "
        "    Console.WriteLine($\"{found}\"); "
        "  end "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "used as a RECEIVER"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── R5 — a write point "in the arm" may sit in a NESTED construct inside
// that arm; the nested `case` is part of the enclosing construct's write
// points, and the outer binding outlives it.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           r5_write_point_in_nested_case_in_arm_rejected) {
    auto r = runAot(prog(kHolder,
        "using var h := new Holder(); "
        "case h.Slots of "
        "  var a => do "
        "    case h.Slots of "
        "      var b => do h.Slots := [new Cell(9), new Cell(9)]; end "
        "    end; "
        "    Console.WriteLine($\"{a[0].V}\"); "
        "  end "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInPlace));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── R6 — DETERMINISM AND EXACT-ONCE. A one-shot compile is the vacuous shape
// this repo warns about: it cannot distinguish a stable rule from a
// state-dependent one (a cached verdict, a symbol-table ordering, a hash
// iteration order). The SAME program is compiled 50 times and every compile
// must reject with the diagnostic; the count is the oracle, not "it did not
// run". R1 already pins exactly-one-per-compile; this pins it 50×.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           r6_rejection_is_deterministic_over_fifty_compiles) {
    const std::string src = prog(kHolder,
        "using var h := new Holder(); "
        "case h.Slots of "
        "  var a => do "
        "    Console.WriteLine($\"{a[0].V}\"); "
        "    h.Slots := [new Cell(9), new Cell(9)]; "
        "    Console.WriteLine($\"{a[0].V}\"); "
        "  end "
        "end; "
        "return 0; ");
    int rejectsWithDiagnostic = 0;
    for (int i = 0; i < 50; ++i) {
        auto r = runAot(src);
        if (!r.ran && diagnosticMessageContains(r, kInPlace) &&
            diagnosticMessageCount(r, kInPlace) == 1)
            ++rejectsWithDiagnostic;
    }
    BEZEL_EXPECT_EQ(rejectsWithDiagnostic, 50);
}

// ── R7 — the OTHER SPELLING OF `case`. §A.7 has two productions for the same
// construct: the statement form (A.6, `case … end;`) used by R1–R6, and the
// parenthesized expression ATOM (A.7, `case … of … end`) used by
// `var n := (case … of …, _ => …)`. The fix installs ONE checker and calls it
// from both handlers, so the two spellings cannot drift; this row is what
// makes that claim falsifiable. An expression-position arm's body is an
// EXPRESSION, so the write point is a §17.15 bind-atom CLAUSE (evaluated
// first) and the use is the atom's body — the same evaluation order, reached
// by different syntax.
// Fails if only the CaseStmt handler calls the checker.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           r7_expression_position_case_rejected_same_rule) {
    auto r = runAot(prog(
        "class Holder "
        "  public Slots: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "  public method Bump(): int "
        "    this.Slots := [new Cell(9), new Cell(9)]; return 0; "
        "  end Bump; "
        "end Holder; ",
        "using var h := new Holder(); "
        "var n := (case h.Slots of "
        "  var a => (var t := h.Bump(); t + a[0].V), _ => 0 "
        "); "
        "Console.WriteLine($\"{n}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kInPlace));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── A7 — §19.3 (:4046, v0.334), QUOTED:
//
//     "Each arm's case test reads the source as it stands when that arm is
//      tried (§19.4), so a write point in a declined arm's guard is not a
//      write point for a later arm's bindings."
//
//   Arm 1 is guarded, declines, and its GUARD writes the field. Arm 2 then
//   matches, and its binding is made AFTER that write already ran — so it
//   reads the post-write value and no stale-borrow hazard exists. Admitted.
//   The write point is real and it precedes the use in textual order; only
//   the ARM scoping of §4046 admits the program. Fails if the event log is
//   replayed across arms, which is a superset of the rule and therefore a
//   silent denial of this valid program.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a7_write_in_a_declined_arms_guard_does_not_reach_a_later_arm) {
    // Arm 1 is GUARDED and its guard writes the field and returns false, so
    // arm 1 is DECLINED — and the write really happened. Arm 2 then matches
    // and its binding is made after it, so by §4046 no write point stands
    // between the two, and the program is ADMITTED. Fails if the event log is
    // replayed across arms: that is a superset of the rule, and a superset
    // here is a silent denial of this valid program.
    auto r = runAot(prog(
        "class Holder "
        "  public Slots: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "  public method Decline(): bool "
        "    this.Slots := [new Cell(9), new Cell(9)]; return false; "
        "  end Decline; "
        "end Holder; ",
        "using var h := new Holder(); "
        "var n := 0; "
        "case h.Slots of "
        "  var a when h.Decline() => do n := n + 1; end, "  // declines
        "  var b => do Console.WriteLine($\"{b[0].V}\"); end "
        "end; "
        "Console.WriteLine($\"{n}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // §19.3 :4046 (v0.336) is a rule about the VALUE, not only about
    // admission: "Each arm's case test reads the source as it stands when
    // that arm is tried (§19.4)". Arm 2's case test runs after arm 1's guard
    // has already assigned `h.Slots`, so the source it reads is the NEW
    // `[new Cell(9), new Cell(9)]` and `b[0].V` is 9.
    //
    // This assertion is STRENGTHENED, not added: the row previously asserted
    // only `ran` and `exitCode`, with a NOTE recording that the printed value
    // was a garbage pointer because `lowerCaseStmt` stored the hoisted
    // scrutinee into the binding slot — a read of storage §28.3 had already
    // disposed inside `Holder$Decline`. The plain oracle passed that row; the
    // ASan oracle is the one that saw it (heap-use-after-free, READ of size 4
    // in `main`, freed by `bz$M.Holder$Decline`, allocated by `calloc` in
    // `main`). `lowerCaseStmt` now re-reads the source at each arm's test
    // point (`caseScrutRereadable` + the per-arm `lowerExpr`), which is the
    // spec's own sentence implemented in codegen.
    //
    // The two lines are the whole differential:
    //   "9" — arm 2's binding observes the POST-write source (§19.3/§19.4).
    //   "0" — arm 1 was DECLINED, so its body never ran; the guard's write
    //         point is real and n is untouched.
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n0\n");
}

// ── A8 — §7.4's stable local covers "a local variable **or parameter** of
// that body", and a `case` scrutinee is very often a formal. Admitted, with
// an unrelated write in the same arm. Fails if the stable-source predicate
// only accepts a `LocalSymbol`: every parameter-sourced construct then takes
// the reject-by-default branch, which is a silent denial of valid programs.
// MEASURED on an intermediate tree of this fix: exactly this program was
// rejected before the predicate was widened.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a8_parameter_source_is_a_stable_local_spec_7_4) {
    auto r = runAot(prog(
        "class C2 "
        "  public static method F(p: [2 owned]Cell): int "
        "    var n := 0; "
        "    case p of "
        "      var a => do "
        "        n := n + 1; "
        "        Console.WriteLine($\"{a[0].V}\"); "
        "      end "
        "    end; "
        "    Console.WriteLine($\"{n}\"); "
        "    return 0; "
        "  end F; "
        "end C2; ",
        "return C2.F([new Cell(1), new Cell(3)]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// ADMIT direction — the whitelist must still be honoured. A check that
// rejects everything is a silent denial of valid programs.
// ═══════════════════════════════════════════════════════════════════════════

// ── A1 — a stable local NOT written anywhere in the construct, while the
// construct DOES write an unrelated local. Two things must hold or this row
// goes red: the whitelist must be applied (else R1's mechanism over-fires),
// and §7.4's region must be the WHOLE CONSTRUCT — an unrelated write in the
// same arm is a write point, but it does not write `arr`, so `arr` stays
// stable. Fails if the implementation keys "stable" on "no write point
// anywhere" instead of "no write to THIS source".
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a1_stable_local_not_written_in_construct_admitted) {
    auto r = runAot(prog("",
        "var arr: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "var n := 0; "
        "case arr of "
        "  var a => do "
        "    n := n + 1; "          // a write point — but not to `arr`
        "    Console.WriteLine($\"{a[0].V}\"); "
        "  end "
        "end; "
        "Console.WriteLine($\"{n}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The value channel, not just "it ran": the binding read its own slot and
    // the unrelated store ran.
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\n");
}

// ── A2 — LIVENESS IS K5.5's. "Live" means "some later use"; a binding with
// no use cannot be live at a write point, so a construct that writes the
// field and never reads the binding is admitted. Fails if the rule rejects on
// "a write point exists" rather than "a use is live at a write point".
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a2_unused_binding_admitted_k5_5_liveness) {
    auto r = runAot(prog(kHolder,
        "using var h := new Holder(); "
        "case h.Slots of "
        "  var a => do h.Slots := [new Cell(9), new Cell(9)]; end "  // `a` unused
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── A3 — COPYABLE vs MOVE-ONLY. `[3]int` has no `owned` modifier, so §15.3
// makes it copyable and §19.3's rule has nothing to bite on — even though the
// local IS written in the construct. Fails if the implementation tests "inline
// array" where the spec says "payload of MOVE-ONLY value type", which is the
// §15.3 classification and not the syntactic kind.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a3_copyable_inline_array_binding_admitted_spec_15_3) {
    auto r = runAot(prog("",
        "var x: [3]int := [1, 2, 3]; "
        "var s: [3]int := [9, 9, 9]; "
        "case x of "
        "  var a => do s := a; Console.WriteLine($\"{s[0]}\"); end "  // `x` written
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ── A4 — the `readonly` whitelist case: "a `readonly` field outside its
// construction window", in §7.4's tracked shape. The method body writes an
// unrelated local and reads the binding. Fails if that case is missing from
// the whitelist — a field-rooted scrutinee would then be rejected outright,
// which is a denial of the shape the spec admits.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a4_readonly_field_source_admitted) {
    auto r = runAot(prog(
        "class Holder "
        "  public readonly Slots: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "  public method Show(): int "
        "    var n := 0; "
        "    case this.Slots of "
        "      var a => do "
        "        n := n + 1; "
        "        Console.WriteLine($\"{a[0].V}\"); "
        "      end "
        "    end; "
        "    Console.WriteLine($\"{n}\"); "
        "    return 0; "
        "  end Show; "
        "end Holder; ",
        "using var h := new Holder(); return h.Show(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\n");
}

// ── A5 — THE REMEDY THE DIAGNOSTIC NAMES. §19.3: "read what the arm needs
// **before** the point". The same program as R1 with the second read moved
// ahead of the store compiles and prints the snapshot value. This row is the
// admit-direction twin of R1: if the fix rejected "a write point exists in the
// arm" rather than "a use is live AT a write point", R1 would go green by
// over-rejection and this row would go red.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a5_read_before_the_write_point_admitted_remedy) {
    auto r = runAot(prog(kHolder,
        "using var h := new Holder(); "
        "case h.Slots of "
        "  var a => do "
        "    Console.WriteLine($\"{a[0].V}\"); "  // the read the remedy names
        "    h.Slots := [new Cell(9), new Cell(9)]; "
        "  end "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ── A6 — THE VALUE DIFFERENTIAL, on the admitted side. Asserts a SLOT VALUE
// through a move-only binding (1, 3) and through the local (1). `.Length`
// would pin nothing here: the restored array is also length 2, so a snapshot
// and a view print the same number — that is why the reject-direction
// measurement in this file's header reads `a[0].V` and not `.Length`.
BEZEL_TEST(sema_move_only_payload_binding_write_point_5870,
           a6_move_only_binding_reads_slot_values) {
    auto r = runAot(prog("",
        "var arr: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "case arr of "
        "  var a => do "
        "    Console.WriteLine($\"{a[0].V}\"); "
        "    Console.WriteLine($\"{a[1].V}\"); "
        "    Console.WriteLine($\"{arr[0].V}\"); "
        "  end "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n3\n1\n");
}
// ═══════════════════════════════════════════════════════════════════════════
// fix #5941 — §19.3 (v0.338) re-read for a PARAMETER-rooted and a
// `this`-rooted case scrutinee.
//
// QUOTED (§19.3, v0.338): "Each arm's case test reads the source as it stands
// when that arm is tried (§19.4), so a write point in a declined arm's guard is
// not a write point for a later arm's bindings." §7.4 counts "a local variable
// or parameter" as a stable local. `ParameterSymbol` is a sibling of
// `LocalSymbol`, so `caseScrutRereadable` declined a parameter root (and a
// `this` / bare-field root) and `lowerCaseStmt` kept the value hoisted before
// the arms: arm 2's binding aliased the array arm 1's guard had disposed
// (heap-use-after-free under ASan). Oracle: `runAotChecked` (always ASan), and
// every row asserts the post-write VALUE through stdout ("9" = arm 2 read the
// NEW source, "0" = arm 1 was declined).
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Holder whose Decline() overwrites Slots (disposing the old array) and
// declines the guard. `extra` adds the member under test.
std::string holder5941(const std::string& extra) {
    return std::string(
        "class Holder "
        "  public Slots: [2 owned]Cell := [new Cell(1), new Cell(3)]; "
        "  public method Decline(): bool "
        "    this.Slots := [new Cell(9), new Cell(9)]; return false; "
        "  end Decline; ") + extra + " end Holder; ";
}

}  // namespace

BEZEL_TEST(codegen_case_scrutinee_reread_5941,
           by_value_parameter_root_rereads_after_declined_guard) {
    auto r = runAotChecked(prog(
        holder5941("") +
        "class R "
        "  public static method Run(h: Holder): int "
        "    var n := 0; "
        "    case h.Slots of "
        "      var a when h.Decline() => do n := n + 1; end, "
        "      var b => do Console.WriteLine($\"{b[0].V}\"); end "
        "    end; "
        "    Console.WriteLine($\"{n}\"); "
        "    return 0; "
        "  end Run; "
        "end R; ",
        "using var h := new Holder(); return R.Run(h); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n0\n");
}

BEZEL_TEST(codegen_case_scrutinee_reread_5941,
           ref_parameter_root_rereads_after_declined_guard) {
    auto r = runAotChecked(prog(
        holder5941("") +
        "class R "
        "  public static method Run(ref h: Holder): int "
        "    var n := 0; "
        "    case h.Slots of "
        "      var a when h.Decline() => do n := n + 1; end, "
        "      var b => do Console.WriteLine($\"{b[0].V}\"); end "
        "    end; "
        "    Console.WriteLine($\"{n}\"); "
        "    return 0; "
        "  end Run; "
        "end R; ",
        "using var o := new Holder(); var h: Holder := o; return R.Run(ref h); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n0\n");
}

BEZEL_TEST(codegen_case_scrutinee_reread_5941,
           this_rooted_scrutinee_rereads_after_declined_guard) {
    auto r = runAotChecked(prog(
        holder5941(
            "public method Run(): int "
            "  var n := 0; "
            "  case this.Slots of "
            "    var a when Decline() => do n := n + 1; end, "
            "    var b => do Console.WriteLine($\"{b[0].V}\"); end "
            "  end; "
            "  Console.WriteLine($\"{n}\"); "
            "  return 0; "
            "end Run; "),
        "using var h := new Holder(); return h.Run(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n0\n");
}

BEZEL_TEST(codegen_case_scrutinee_reread_5941,
           bare_field_name_scrutinee_rereads_after_declined_guard) {
    auto r = runAotChecked(prog(
        holder5941(
            "public method Run(): int "
            "  case Slots of "
            "    var a when Decline() => do end, "
            "    var b => do Console.WriteLine($\"{b[0].V}\"); end "
            "  end; "
            "  return 0; "
            "end Run; "),
        "using var h := new Holder(); return h.Run(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// Loop the parameter scenario 50x: also a leak check (every iteration's
// replaced array and the final one must be disposed exactly once).
BEZEL_TEST(codegen_case_scrutinee_reread_5941,
           parameter_root_looped_50x_reads_new_value_and_does_not_leak) {
    auto r = runAotChecked(prog(
        holder5941("") +
        "class R "
        "  public static method Run(h: Holder): int "
        "    case h.Slots of "
        "      var a when h.Decline() => do return -1; end, "
        "      var b => do return b[0].V; end "
        "    end; "
        "    return -2; "
        "  end Run; "
        "end R; ",
        "var sum := 0; var i := 0; "
        "while i < 50 do "
        "  using var h := new Holder(); "
        "  sum := sum + R.Run(h); "
        "  i := i + 1; "
        "end; "
        "Console.WriteLine($\"{sum}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "450\n");
}

// Control: the already-fixed LOCAL-rooted shape must stay green.
BEZEL_TEST(codegen_case_scrutinee_reread_5941,
           local_rooted_control_still_rereads) {
    auto r = runAotChecked(prog(
        holder5941(""),
        "using var h := new Holder(); "
        "var n := 0; "
        "case h.Slots of "
        "  var a when h.Decline() => do n := n + 1; end, "
        "  var b => do Console.WriteLine($\"{b[0].V}\"); end "
        "end; "
        "Console.WriteLine($\"{n}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n0\n");
}
