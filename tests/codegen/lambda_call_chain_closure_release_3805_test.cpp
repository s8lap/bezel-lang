// Codegen — closure-release admission for a CallExpr-to-lambda callee, and
// the transparent-forwarding-lambda cell-content disposal it unblocks
// (§27.5 / §15.3 / §28.9, #3805).
//
// Spec §27.5: a closure's envdtor is the last release of its capture cell.
// §15.1.1 line 1957: "When an owning inline array's lifetime ends, the
// array's auto-dispose pass disposes each non-null slot." §15.3 line 2021:
// "a move-only local's owned slots are auto-disposed at scope end by the
// compiler."
//
// #3805 (issue's stated hypothesis): `collectClosureReleaseEligible` does
// not admit a CallExpr-to-lambda callee (`var inner := outer();` where
// `outer` is itself a lambda-typed local), so `inner`'s closure release
// never fires and #3766's envdtor-owner route is dead code. MEASURED ON
// HEAD (this fix's own investigation): that admission already exists —
// `isLambdaClosureReturnCall` (fix #2714) recognizes exactly this CallExpr
// shape, and `inner`'s own release DOES fire (confirmed: the pre-existing
// 96 B/iter leak from issue #3766's original report had already dropped to
// 48 B/iter on HEAD, from inner's own 40 B closure + 8 B env being
// correctly released). The REMAINING 48 B/iter (a's boxed cell + both Cell
// instances, Cell.n stuck nonzero) has a DIFFERENT root cause than the
// issue named: `collectCellFreeEligible`'s #1135 widening walker (and
// #3766's own `var h :=`-position reclassification) has no case for a
// nested lambda reached at an outer lambda's UNCONDITIONAL TAIL POSITION —
// `var outer := lambda() => lambda() => …;`, where the inner lambda IS the
// entire expression body, not a named `var h :=` binding. This file's fix
// adds that case: a TRANSPARENT FORWARDING LAMBDA (`var L := lambda() => H;`,
// `H` itself a bare LambdaExpr) called through exactly one `var M := L();`
// whose own binding is already proven non-escaping is safe to treat `H`'s
// free variables as captured non-escaping, closing the remaining leak.
//
// SOUNDNESS: the exactly-one-call proof is load-bearing (row 3). Two calls
// to the same forwarding lambda build two independent inner closures
// sharing the SAME boxed cell; if one of those closures ESCAPES while the
// other does not, freeing the cell at the non-escaping call's scope end
// would use-after-free the escaped one's later read. `ClosureEscapeWalker`'s
// new `calleeCallCount` (a genuine occurrence count, not a set — a set
// cannot distinguish "called once" from "called twice") is what rules this
// out; `!escaped` alone would not, since both calls are individually safe
// reads (a direct call is never itself an "escape" of the callee).
//
// LEAK AMPLIFICATION (§lsan-one-shot-leak-tests-pass-vacuously): leak rows
// iterate Run() many times to amplify a single-allocation leak from
// LSan-undetectable to LSan's resolution floor.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// run() — plain AOT exit-code round-trip.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// runChecked() — AOT under AddressSanitizer/LSan. Used for the primary leak
// row: runAot alone cannot see a leak at all (plain allocator).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Cell: disposable class with static live-instance counter (ctor +1, dtor
// -1). After all owners dispose, counter == 0; leak leaves it positive;
// double-free would drive it negative.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

std::string progAmplified(const std::string& runBody,
                          const std::string& mainBody) {
    return std::string(kCell) +
        "class C "
        "  public static method Run(): int " + runBody +
        "  end Run; "
        "  public static method Main(): int " + mainBody +
        "  end Main; "
        "end C;";
}

}  // namespace

// ============================================================================
// Row 1 (LEAK-AMPLIFIED x200): #3805's own repro — a two-level lambda chain
// `var outer := lambda() => lambda() => …;` called through `var inner :=
// outer();`, both proven non-escaping. Pre-fix (measured on HEAD, no code
// change): Cell.n stuck at 400 (2 slots x 200 iterations never disposed) —
// the 9600 B / 600-allocation leak this fix's own investigation measured.
// Post-fix: the terminal lambda's free variables join capturedByNonEscaping
// (not capturedByEscaping), so `a` is cellFreeEligible_ and #782's element-
// dispose-through-cell fires at Run's scope end every iteration. Cell.n
// must be 0; a regression restoring the leak surfaces as Cell.n == 400.
// ============================================================================
BEZEL_TEST(codegen_lambda_call_chain_3805,
           two_level_forwarding_chain_leak_amplified_200x) {
    std::string src = progAmplified(
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var outer := lambda() => lambda() => a[0]!.V + a[1]!.V; "
        "  var inner := outer(); "
        "  return inner(); ",
        "  var total: int := 0; "
        "  for var j in 0..<200 do total := total + C.Run(); end; "
        "  return Cell.n; ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);  // both slots disposed every iter
}

// ============================================================================
// Row 2 (CONTROL, single-owned twin): the brief's own `[1 owned]Cell`
// (non-nullable, single-slot) shape at higher amplification, matching the
// issue's exact repro form. Regression sentinel distinct from row 1's
// `[2 owned]Cell?` shape (nullable slots).
// ============================================================================
BEZEL_TEST(codegen_lambda_call_chain_3805,
           single_owned_non_nullable_leak_amplified_500x) {
    std::string src = progAmplified(
        "  var a: [1 owned]Cell := [new Cell(7)]; "
        "  var outer := lambda() => lambda() => a[0]!.V; "
        "  var inner := outer(); "
        "  return inner(); ",
        "  var total: int := 0; "
        "  for var j in 0..<500 do total := total + C.Run(); end; "
        "  return Cell.n; ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
}

// ============================================================================
// Row 3 (SOUNDNESS): `outer` is called TWICE, producing two independent
// inner closures that share the SAME boxed cell for `a`. `inner2` ESCAPES
// by being RETURNED from `Run` (a function-typed return — a genuine escape
// per ClosureEscapeWalker's own rules); `inner1` does not (called locally,
// only its int result used). Critically, the read through the escaped
// closure happens in `Main`, STRICTLY AFTER `Run` has returned — i.e. after
// the point where `Run`'s OWN scope-end disposal would run, if `a` were
// (wrongly) admitted into cellFreeEligible_. This is what makes the row
// able to actually FAIL: a row that reads the escaped closure from WITHIN
// the same scope as `a`'s disposal can never observe a wrongly-freed cell,
// because the read always precedes that scope's own epilogue (measured:
// an earlier draft of this row read `escaped()` from inside the same
// method as `a`, and PASSED even with the count guard forced true — see
// the "guard-disabled" experiment recorded below).
//
// GUARD-DISABLED EXPERIMENT (performed once, by hand, to justify this row):
// with `collectCellFreeEligible`'s `cew3805.calleeCallCount == 1` conjunct
// temporarily forced to `true` (i.e. accepting on `!escaped` alone), this
// exact program aborts under ASan — heap-use-after-free reading through
// `escaped()`'s env after `Run`'s frame (and `a`'s wrongly-freed cell) is
// gone. With the guard restored, the program runs cleanly: `a` stays in
// the pre-existing conservative-leak classification, Cell.n == 2 (never
// disposed), and reading through the still-live (leaked) cell via
// `escaped()` is safe. This row is therefore guard-LOAD-BEARING, not
// guard-invariant.
//
// Asserts: the program runs cleanly under ASan (no UAF) AND the arithmetic
// values match reading the (leaked, not disposed) cells: n == 3
// (Cell(1).V + Cell(2).V), Cell.n == 2. ⚠ AOT exit status truncates to 8
// bits (LANE_RULES) — the packed encoding stays well under 256.
// ============================================================================
BEZEL_TEST(codegen_lambda_call_chain_3805,
           forwarding_lambda_called_twice_one_escapes_no_uaf) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Run(): () -> int "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var outer := lambda() => lambda() => a[0]!.V + a[1]!.V; "
        "    var inner1 := outer(); "
        "    var inner2 := outer(); "
        "    var r: int := inner1(); "
        "    return inner2; "
        "  end Run; "
        "  public static method Main(): int "
        "    var escaped := C.Run(); "
        "    var n: int := escaped(); "
        "    return n * 50 + Cell.n; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);  // ASan would have aborted on a UAF
    BEZEL_EXPECT_EQ(r.exitCode, 3 * 50 + 2);  // n=3, leak=2 -> 152
}

// ============================================================================
// Row 5 (UNDERCOUNT REGRESSION): `outer` is called twice, but the SECOND
// call's callee is not a bare identifier — it is a `(if … then … else …)`
// atom (grammar A.7 `IfExpr`, no `end` — §20: "the matching `)` closes the
// atom; there is no `end`"; `ClosureEscapeWalker`'s `ConditionalExpr` arm
// forwards `safeRead` unchanged into both branches, so `(if true then
// outer else outer)()` is a safe, non-escaping read of `outer` by the
// walker's own existing rules — exactly like a bare `outer()`). A counter
// that only recognizes `isTargetIdent(call->callee)` never increments for
// this call, so it would report `calleeCallCount == 1` (only the row's
// first, bare-identifier call counted) even though `outer` is reachable
// through TWO distinct call sites. Same escaping shape as row 3 (`inner2`
// returned from `Run`, read from `Main` after `Run` has returned).
//
// Same GUARD-DISABLED-style justification as row 3, this time targeting
// the UNDERCOUNT specifically rather than the count conjunct itself: with
// `scanCalleePosition`'s if-expression recursion removed (reverting to a
// bare `isTargetIdent(c->callee.get())` check, i.e. the pre-fix counter),
// this program's `calleeCallCount` reads 1 (the conditional call is never
// seen), the guard wrongly admits, and the run aborts under ASan on the
// same heap-use-after-free as row 3. With the fix (recursing into both
// branches of the if-expression atom), `calleeCallCount` correctly reads 3
// for this program (1 from `inner1`'s bare call, 2 from BOTH branches of
// `inner2`'s if-expression naming `outer` — conservative over-counting in
// the safe direction, never under), the guard declines, and the run is
// clean with the same conservative leak as row 3 (Cell.n == 2, n == 3).
// ============================================================================
BEZEL_TEST(codegen_lambda_call_chain_3805,
           forwarding_lambda_called_via_if_expr_callee_no_uaf) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Run(): () -> int "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var outer := lambda() => lambda() => a[0]!.V + a[1]!.V; "
        "    var inner1 := outer(); "
        "    var inner2 := (if true then outer else outer)(); "
        "    var r: int := inner1(); "
        "    return inner2; "
        "  end Run; "
        "  public static method Main(): int "
        "    var escaped := C.Run(); "
        "    var n: int := escaped(); "
        "    return n * 50 + Cell.n; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);  // ASan would have aborted on a UAF
    BEZEL_EXPECT_EQ(r.exitCode, 3 * 50 + 2);  // n=3, leak=2 -> 152
}

// ============================================================================
// Row 6 (UNDERCOUNT REGRESSION, delta re-review finding): `outer` is called
// twice, the second call's callee is a `(var x := outer; x)` binding atom
// (§17.15, grammar A.7 `BindExpr`) — the EXACT shape the delta re-review
// found had NO dispatch arm anywhere in `ClosureEscapeWalker::expr()`, so a
// `target` reference inside one (as a clause's source, or in the atom's
// body) was invisible to BOTH `escaped`-tracking and `calleeCallCount`:
// silently read as "not present" rather than declined. The reviewer
// reproduced the same `heap-use-after-free ... freed by ... bz$M.C$Run`
// signature on the then-committed code by direct ASan repro. Same escaping
// shape as rows 3 and 5 (`inner2` returned from `Run`, read from `Main`
// only after `Run` has returned).
//
// Fixed two ways, deliberately redundant (defense in depth, not belt-and-
// suspenders for its own sake): (1) `ClosureEscapeWalker::expr()` gained a
// BindExpr arm (walks each clause's initializer as an ordinary binding-
// into-another-local escape, and the atom's body inheriting the caller's
// `safeRead`) — the shared authority's own gap, fixed for every consumer.
// (2) `containsTargetAnywhere` (this fix's own helper, feeding
// `scanCalleePosition`'s `uncountableCalleeSeen` decision) was rewritten
// as an INDEPENDENT walk enumerated by hand against the complete 32-struct
// `ast::Expr` hierarchy, defaulting to "not proven absent" for anything
// outside that enumeration — so a THIRD, still-undiscovered gap in
// `expr()`'s dispatch would no longer silently read as "target absent"
// for THIS fix's own admission gate, even if it did for some other,
// pre-existing consumer of the shared walker.
// ============================================================================
BEZEL_TEST(codegen_lambda_call_chain_3805,
           forwarding_lambda_called_via_bind_atom_callee_no_uaf) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Run(): () -> int "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var outer := lambda() => lambda() => a[0]!.V + a[1]!.V; "
        "    var inner1 := outer(); "
        "    var inner2 := (var x := outer; x)(); "
        "    var r: int := inner1(); "
        "    return inner2; "
        "  end Run; "
        "  public static method Main(): int "
        "    var escaped := C.Run(); "
        "    var n: int := escaped(); "
        "    return n * 50 + Cell.n; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);  // ASan would have aborted on a UAF
    BEZEL_EXPECT_EQ(r.exitCode, 3 * 50 + 2);  // n=3, leak=2 -> 152
}

// ============================================================================
// Row 4 (CONTROL, plain venue): the exact single-iteration shape at the
// venue the issue's b94 investigation used (static Run() called once from
// Main, no loop). Cell.n MUST be 0.
// ============================================================================
BEZEL_TEST(codegen_lambda_call_chain_3805,
           plain_venue_single_iteration) {
    BEZEL_EXPECT_EQ(run(progAmplified(
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var outer := lambda() => lambda() => a[0]!.V + a[1]!.V; "
        "  var inner := outer(); "
        "  return inner(); ",
        "  var r: int := C.Run(); "
        "  return Cell.n; ")),
        0);
}
