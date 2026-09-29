// issue #5134: a §35.5 owning-match ARM PAYLOAD BINDING (`w` in
// `case move o of W(var w, _) => ...`) could be `move`d a SECOND time, or
// read AFTER a `move`, and both compiled clean — the dataflow kept no
// `states_` entry for a pattern binding at all (fix #4944's own doc comment:
// "the dataflow keeps no `states_` entry for a pattern binding"), so
// `isOwningMatchTransferredArmBinding(sym)` admitted every occurrence of a
// `move`/`owned` of the binding, with zero notion of how many times it had
// already fired. MEASURED (non-author review of #4944's 087b86a0):
// `do var r1 := move w; var r2 := move w; end` compiled clean and SEGVs
// under ASan (null read in `Widget.Name` via the already-moved binding);
// same for reading `w` after `move w`.
//
// THE RULE. Per §28.3 K3.4 / §35.5 (v0.307/v0.308) a moved-from owned
// binding is no longer owned: a second transfer, or any later use, of `w`
// on that path must be rejected with the existing use-after-move / K3.4
// diagnostic — exactly as it would be for an ORDINARY owned local. §35.5
// makes the payload binding "an owned local of the arm body's scope", so it
// is bound by the SAME rule, not a permissive carve-out.
//
// THE FIX. `seedOwningMatchArmBindings` (Ownership.cpp) gives the binding a
// real `states_` entry (Owned), seeded fresh both at the arm's CFG entry
// block (`CFGBlock::caseArmNode`, for a block-form arm reached via ordinary
// statement-walking) and — since a `CaseExpr` used as a VALUE is explored
// EAGERLY from wherever it is discharged, before CFG traversal ever reaches
// the arm's own block — inside `processMarkedArmConstruct`'s own per-arm
// loop (for an expression-form arm). `processMarkedArmConstruct` then routes
// EVERY `move`/`owned` of the binding through the ordinary
// `handleMarkedArmTransfer` / `handleMarkedArmMoveTransfer` ->
// `consumeMarkedArmTransfer` call — no parallel state machine — so the
// EXISTING K3.4 double-consume check and K7.4 maybe-consumed-join / use-
// after-move read check, already correct for an ordinary owned local, cover
// the binding too. The symmetric endState exemption (§28.3's method-exit
// "undischarged owned value" / must-use-warning sweep must never flag this
// binding, whatever its tracked state) preserves every already-accepted
// #4633/#4664/#4734/#4944 shape where `w` is legitimately left unmoved —
// its physical reclaim is codegen's own per-slot mark (`#4617`), entirely
// independent of this dataflow's obligation tracking.
//
// FAIL-WITHOUT: on 087b86a0 (pre-fix, the tip #4944 landed), every REJECT
// row below compiles with ZERO diagnostics instead of the asserted error.
//
// per §35.5, §28.3 K3.4/K7.4, v0.307/v0.308

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int errorCount(const JitResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

// Shared prelude: a single-field `Widget` so each disposal prints one
// deterministic line, and a two-payload owning choice `Outer` (mirrors
// #4944's own prelude and the issue's own probe files).
const char* kPrelude =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Name(): string => _n; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public class Program "
    "  public static method Mk(): Outer => "
    "      Outer.W(new Widget(\"a\"), new Widget(\"b\")); ";

std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "  public static method Main(): int " + mainBody +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// (REJECT 1 — p1b) two UNCONDITIONAL, sequential `move w` of the same arm
// binding in a block-form arm. The FIRST transfers `w` out (Owned ->
// MovedFrom); the second must draw the standard K3.4 double-consume
// diagnostic ("owned value in `w` was already moved out at this point
// (§28.3)") — the one `consumeMarkedArmTransfer` already emits for an
// ordinary owned local's second move.
BEZEL_TEST(owning_match_arm_binding_double_move_5134,
           double_move_same_path_rejected_28_3_k3_4) {
    JitResult r = runAot(prog(
        "    var o := Mk(); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var r1 := move w; "
        "        Console.WriteLine($\"r1={r1.Name()}\"); "
        "        var r2 := move w; "
        "        Console.WriteLine($\"r2={r2.Name()}\"); "
        "      end, "
        "      X => () "
        "    end case; "));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "was already moved out at this point"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xc2\xa7" "28.3"));
}

// (REJECT 2 — p1) the same double-consume, but the FIRST `move w` sits on
// only ONE branch of a nested if-expression (the #4944-admitted shape) and
// the SECOND `move w` is an unconditional statement following it. After the
// nested if's own K7.4 join, `w` is Owned + maybe-consumed; the second
// (unconditional) move must draw the EXISTING maybe-consumed-join
// diagnostic, exactly as it would for an ordinary owned local moved on one
// `if` branch and moved again afterward.
BEZEL_TEST(owning_match_arm_binding_double_move_5134,
           double_move_after_nested_if_branch_rejected_28_3_k7_4) {
    JitResult r = runAot(prog(
        "    var p := true; "
        "    var o := Mk(); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var r1 := (if p then move w else new Widget(\"z\")); "
        "        Console.WriteLine($\"r1={r1.Name()}\"); "
        "        var r2 := move w; "
        "        Console.WriteLine($\"r2={r2.Name()}\"); "
        "      end, "
        "      X => () "
        "    end case; "));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "it may have already been consumed on another branch"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "maybe-consumed join, \xc2\xa7" "28.3"));
}

// (REJECT 3 — p2) a plain READ of `w` (not a further move) after a
// conditional `move w` on one branch of a nested if-expression. The read
// must be rejected as a maybe-consumed-join use — the binding's storage is
// null on the moved branch, so an unrejected read is the null-deref half of
// #5134's MEASURED SEGV.
BEZEL_TEST(owning_match_arm_binding_double_move_5134,
           use_after_move_on_branch_rejected_28_3_k7_4) {
    JitResult r = runAot(prog(
        "    var p := true; "
        "    var o := Mk(); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var r1 := (if p then move w else new Widget(\"z\")); "
        "        Console.WriteLine($\"r1={r1.Name()}\"); "
        "        Console.WriteLine($\"w after move: {w.Name()}\"); "
        "      end, "
        "      X => () "
        "    end case; "));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "may have already been consumed on another branch and may not "
           "be read here"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "maybe-consumed join, \xc2\xa7" "28.3"));
}

// (REJECT 4) the expression-arm twin of REJECT 1/2: `w` moved as the arm's
// OWN direct transfer value, then read again in a later statement outside
// the `case` construct via an alias — i.e. a plain double-transfer with the
// FIRST occurrence being the arm's bare `move w` result. Confirms the
// expression-position seed (`processMarkedArmConstruct`'s own per-arm loop)
// also catches a SECOND, unrelated move of the same already-transferred
// binding reached from a DIFFERENT statement.
BEZEL_TEST(owning_match_arm_binding_double_move_5134,
           expr_arm_direct_transfer_then_second_move_rejected) {
    JitResult r = runAot(prog(
        "    var o := Mk(); "
        "    var r1 := (case move o of "
        "        W(var w, _) => move w, "
        "        X => new Widget(\"x\")); "
        "    Console.WriteLine($\"r1={r1.Name()}\"); "));
    // `w` is out of scope once the case construct's arm ends, so the ONLY
    // way to attempt a second move of the SAME symbol from the same source
    // text would re-enter the same arm — already covered by REJECT 1/2.
    // This row instead pins that the FIRST direct-transfer shape itself
    // still compiles clean (the #4633 baseline #5134 must not regress),
    // as a differential control for the reject rows above.
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "dispose b\nr1=a\ndispose a\n");
}

// (ACCEPT 1 — p6) `move w` on exactly ONE branch of a nested if-expression,
// with NO later use of `w` — the #4944-admitted shape itself. Must still
// compile clean and dispose exactly once per iteration: #5134's fix must not
// regress the maybe-consumed-but-never-read-again case into a false
// "undischarged owned value" report at the arm's own scope end.
BEZEL_TEST(owning_match_arm_binding_double_move_5134,
           move_on_one_branch_only_no_later_use_accepted) {
    JitResult r = runAot(prog(
        "    var p := true; "
        "    var o := Mk(); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var r := (if p then move w else new Widget(\"z\")); "
        "        Console.WriteLine($\"r={r.Name()}\"); "
        "      end, "
        "      X => () "
        "    end case; "));
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "r=a\ndispose a\ndispose b\n");
}

// (ACCEPT 1, looped 200x under the plain oracle) a one-shot leak/UAF repro
// can pass by accident; loop the accepted shape so an unsound admission
// (double-free / leak from a wrongly-cleared state) shows under repetition
// even without ASan. Also run with BEZEL_AOT_FORCE_SANITIZE=address in the
// lane check (not here — see the commit's own verification notes).
BEZEL_TEST(owning_match_arm_binding_double_move_5134,
           move_on_one_branch_only_looped_no_leak_no_crash) {
    std::string body =
        "    var p := true; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var o := Mk(); "
        "      using var r := (case move o of "
        "          W(var w, _) => (if p then move w else new Widget(\"z\")), "
        "          X => new Widget(\"x\")); "
        "      i := i + 1; "
        "    end while; ";
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (ACCEPT 2, looped 200x) the double-move REJECT-1 shape's accepted
// counterpart: a single `move w`, no second consume, no later use — the
// ordinary #4633 direct-transfer baseline, looped to confirm #5134's
// `states_` seed does not itself introduce a leak or double-free on the
// UNCONDITIONAL single-transfer path (the common case, exercised on every
// iteration rather than only on the branch-taken half of ACCEPT 1's `if`).
BEZEL_TEST(owning_match_arm_binding_double_move_5134,
           single_move_looped_no_leak_no_crash) {
    std::string body =
        "    var i := 0; "
        "    while i < 200 do "
        "      var o := Mk(); "
        "      using var r := (case move o of "
        "          W(var w, _) => move w, "
        "          X => new Widget(\"x\")); "
        "      i := i + 1; "
        "    end while; ";
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
