// §35.5 transfer OUT of an owning-match arm, in EXPRESSION position
// (issue #4633; spec §35.5 *The owning match*, §28.3 K3.4/K8.3, §19.2, v0.307).
//
// THE RULE. §35.5 makes an `owned`-payload binding of an owning match "an
// **owned local of the arm body's scope**", to which "§28.3's standard rules
// apply … on every arm-exit edge". §28.3's K3.4 consume table names, among its
// marked-transfer positions, "the marked arm transfer: `move x` as an
// if-expression branch or an expression-`case` arm result" (§17.13 states the
// same from the expression side: "In an expression-position `case`, an arm
// result may be the **marked transfer** `move x`"). §28.3 applies the K8.3
// boundary "in statement position exactly as in expression position". Composing
// them, `W(var w, _) => move w` is a §35.5 payload transfer written in a K3.4
// position, and it must be admitted.
//
// THE BUG (#4633). It was rejected — every spelling. MEASURED on this lane's
// base (f644098f), over `choice Outer W(owned a: Widget, owned b: Widget), X;`:
//
//   using var kept := (case move o of W(var w, _) => move w,
//                                     X => new Widget("x"));
//     → the arm transfer marker `move w` requires a named owned binding —
//       `w` is not an owned binding at this point (§28.3 K3.4, v0.209)
//
//   … => owned w  → the same diagnostic
//
// while the STATEMENT-position spelling of the same transfer
// (`W(var w, _) => do using var live := move w; end`) has been admitted since
// #4617. The cause: the ownership dataflow keeps no `states_` entry for a
// pattern binding at all, so `consumeMarkedArmTransfer`'s "is this a named
// OWNED binding?" gate answered no, and its LOCALDECL-ONLY operand bound
// excluded one besides. Code the spec admits could not be written — and with
// #4635 now rejecting the UNMARKED spelling as an escaping borrow, an author
// had neither the safe spelling nor a diagnostic for the unsafe one.
//
// OBSERVABILITY. The pre-fix behaviour is a REJECT, so the accept rows fail
// without the fix at `r.ran == false` with the quoted diagnostic present. The
// disposal ORDER is the second half of the oracle: `dispose b` must land at
// construct completion (§35.5's un-transferred remainder) and `dispose a` at
// the receiving `using var`'s scope end — a single "it compiled" assertion
// would not distinguish a correct transfer from a leak or a double free. The
// amplified row runs 500 iterations with a counting destructor and asserts the
// EXACT total, because that is the oracle that separates "disposed exactly
// once" from both error directions at once — below the total is a leak, above
// it a double free.
//
// per §35.5, §28.3 K3.4/K8.3, §19.2, §17.13, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §35.5's own owning-choice shape: two `owned` class payloads and a
// payload-free case.
const char* kDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

// The same choice with a COUNTING destructor, for the amplified row.
const char* kCountingDecls =
    "public class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

std::string prog(const char* decls, const std::string& body) {
    return std::string("module M; ") + decls +
           "public class Program public static method Main(): int " + body +
           " end Main; end Program;";
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRun(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The pre-fix diagnostic, asserted ABSENT on every accept row: the rows must
// pass because the transfer is admitted, not because some other error text
// happened to change.
const char* kNotOwnedNeedle = "requires a named owned binding";

// --- THE FIX: both transfer spellings are admitted -------------------------

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           spec_35_5_bare_move_arm_transfers_the_payload_out_28_3_k3_4) {
    // §28.3 K3.4's canonical spelling. `dispose b` is §35.5's un-transferred
    // remainder, reclaimed at construct completion; `a` rides the arm's value
    // into `kept` and is disposed once at `kept`'s scope end, AFTER "after".
    auto r = runAot(prog(kDecls,
        "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "using var kept := (case move o of "
        "  W(var w, _) => move w, "
        "  X           => new Widget(\"x\")); "
        "Console.WriteLine($\"kept={kept.Ping()}\"); "
        "Console.WriteLine(\"after\"); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kNotOwnedNeedle));
    expectRun(r, std::string("dispose b\nkept=7\nafter\ndispose a\n"));
}

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           spec_35_5_owned_marker_arm_transfers_the_payload_out_28_3_k3_4) {
    // §A.7's `MarkedTransferExpr ::= "owned" Identifier` — K3.4's other
    // spelling of the same position, which #4006 routed through one core so
    // the two can never diverge. Same transfer, same trace.
    auto r = runAot(prog(kDecls,
        "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "using var kept := (case move o of "
        "  W(var w, _) => owned w, "
        "  X           => new Widget(\"x\")); "
        "Console.WriteLine($\"kept={kept.Ping()}\"); "
        "Console.WriteLine(\"after\"); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kNotOwnedNeedle));
    expectRun(r, std::string("dispose b\nkept=7\nafter\ndispose a\n"));
}

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           spec_35_5_unbound_temporary_scrutinee_arm_transfer_admitted) {
    // §35.5's FIRST announcing scrutinee form — an unbound owned temporary of
    // owning-choice classification. The transfer is the same; only the form
    // that announces the consume differs.
    auto r = runAot(prog(kDecls,
        "using var kept := "
        "  (case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
        "    W(var w, _) => move w, "
        "    X           => new Widget(\"x\")); "
        "Console.WriteLine($\"kept={kept.Ping()}\"); "
        "Console.WriteLine(\"after\"); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kNotOwnedNeedle));
    expectRun(r, std::string("dispose b\nkept=7\nafter\ndispose a\n"));
}

// --- THE AMPLIFIED ORACLE ---------------------------------------------------

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           spec_35_5_five_hundred_arm_transfers_dispose_exactly_2n) {
    // The strong oracle for the two error directions a transfer can introduce.
    // Per iteration §35.5 requires exactly two disposals: the transferred
    // payload (once, by the receiving `using var`) and the un-transferred
    // sibling (once, at construct completion). 1000 is the EXACT answer; below
    // it is a leak, above it a double free. 500 iterations because a one-shot
    // probe can keep its block reachable from a live stack slot and read the
    // same on BOTH sides of a fix.
    //
    // ⚠ Deliberately `runAot`, not `runAotChecked`: the gate's `--full` leg
    // sets `BEZEL_AOT_FORCE_SANITIZE=address` for EVERY test program, so this
    // row is ASan/LSan-instrumented there without adding a `runAotChecked`
    // call site to `check_toolchain_skip_surface`'s single shared baseline
    // (which every lane in a batch would otherwise collide on). The counting
    // destructor is itself a two-directional oracle; this lane also ran the
    // filter under `BEZEL_AOT_FORCE_SANITIZE=address` by hand.
    auto r = runAot(prog(kCountingDecls,
        "var i := 0; "
        "while i < 500 do "
        "  var o := Outer.W(new Widget(), new Widget()); "
        "  do "
        "    using var kept := (case move o of "
        "      W(var w, _) => move w, "
        "      X           => new Widget()); "
        "    _ := kept; "
        "  end; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(Widget._n); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}

// --- THE CONTROLS -----------------------------------------------------------

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           control_borrowing_match_arm_transfer_still_rejected_35_5) {
    // §35.5 *The borrowing match*: an UNMARKED named owned scrutinee stays a
    // borrowing match, whose bindings are ordinary BORROWS of a value `o`
    // still owns. Transferring one out would double-free it at `o`'s scope
    // end, so the admission must NOT widen to this shape — only to the two
    // announcing owning-match forms.
    auto r = runAot(prog(kDecls,
        "using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "using var kept := (case o of "
        "  W(var w, _) => move w, "
        "  X           => new Widget(\"x\")); "
        "Console.WriteLine($\"kept={kept.Ping()}\"); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNotOwnedNeedle));
}

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           control_scrutinee_self_transfer_still_rejected_k8_3) {
    // §28.3 v0.209 / K8.3: "An arm result marking the *scrutinee's own
    // binding* is always rejected by the consume-site freeze — the scrutinee
    // borrow is live through the selected arm's completion". That rejection is
    // asked FIRST, before the §35.5 payload-transfer admission, so the new
    // admission cannot launder a scrutinee self-transfer.
    auto r = runAot(prog(kDecls,
        "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "var r := (case o of "
        "  W(_, _) => move o, "
        "  X       => Outer.X); "
        "_ := r; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "may not transfer the scrutinee's own binding"));
}

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           control_marked_scrutinee_self_transfer_still_rejected_35_5) {
    // The same self-transfer under §35.5's MARKED scrutinee form. `case move o`
    // leaves `o` moved-from for the construct's duration (§35.5: "the consumed
    // binding is left **moved-from** when the construct completes"), so an arm
    // marking `o` again is rejected by `consumeMarkedArmTransfer`'s
    // not-an-owned-binding gate rather than by the K8.3 self-transfer arm —
    // `identifierRoot` does not see through the scrutinee's own `move`. Either
    // way it stays rejected, and the new §35.5 admission (which requires the
    // marked name to be a PAYLOAD BINDING of that arm's pattern, which `o` is
    // not) does not launder it.
    //
    // fix-forward #3575 (§35.4 / §35.3, v0.334): the REJECT is unchanged but
    // its GROUND moves. §35.3 makes `Outer.W(...)` "an `owned` move-only
    // value", so `o` is now a genuine owned binding — the not-an-owned-binding
    // gate no longer applies to it, and the arm is rejected one clause later,
    // on §28.3's own type restriction: the marker is supported only on a plain
    // `var` owned local of CLASS or INTERFACE type. That is the correct
    // rejection and a stricter one — §15.3 lists a move-only value (an owning
    // choice among them) among the positions where `move` is an error, and the
    // old needle was firing only because the compiler had not yet recognised
    // `o` as owned at all.
    auto r = runAot(prog(kDecls,
        "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "var r := (case move o of "
        "  W(_, _) => move o, "
        "  X       => Outer.X); "
        "_ := r; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "supported only on a plain `var` owned local"));
}

BEZEL_TEST(owning_match_expr_arm_transfer_4633,
           control_unmarked_arm_binding_is_still_the_4635_escape_reject) {
    // The sibling half of the pair. §28.3's arm-meet row makes an UNMARKED
    // named owned binding written as an arm a BORROW arm, and #4635 rejects
    // that borrow for escaping the K8.4 boundary. Admitting the MARKED
    // spellings above must not re-admit the unmarked one — the two rules are
    // complementary, not alternatives.
    auto r = runAot(prog(kDecls,
        "using var other := new Widget(\"other\"); "
        "var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "var got := (case move o of "
        "  W(var w, _) => w, "
        "  X           => other); "
        "Console.WriteLine($\"got={got.Ping()}\"); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "borrows owning-match payload binding"));
}

}  // namespace
