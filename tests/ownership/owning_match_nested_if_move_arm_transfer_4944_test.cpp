// issue #4944: `move w` of an owning-match payload binding is over-rejected
// when the marked transfer sits inside a NESTED if-expression branch of the
// owning-match arm's own result, instead of being the arm's bare value.
//
// THE RULE. §35.5 *The owning match* transfers an owned payload into the
// arm's bindings, making the binding "an owned local of the arm body's
// scope"; §28.3's consume-table K3.4 row and the `(if c then v1 else v2)` /
// expression-`case` meet row (§20/§19.2) both admit the **marked arm**
// `move x` there: "an owned producer and K3.4 transfer consuming on the path
// where its arm is selected." `w`, bound owned by the owning match, is such
// a binding regardless of how deeply the selecting form nests within the
// arm's own result expression.
//
// THE BUG (#4944). `processMarkedArmConstruct` (Ownership_Checks.cpp)
// special-cases a marked/`move` transfer that is the outer `case` arm's OWN
// value via `isOwningMatchTransferBinding(*armNode, head, sym)` — but that
// call is gated on `armNode`, which is only non-null for the OUTER case
// arm's construct. When the marked transfer instead sits one level down, as
// a branch of a NESTED if-expression inside the arm body
// (`W(var w, _) => (if p then move w else new Widget("z"))`), the recursive
// call into `processMarkedArmConstruct` for that inner `ConditionalExpr`
// always has `armNode == nullptr` (if-expressions carry no CaseArm), so the
// §35.5 payload-transfer admission was skipped and `move w` fell through to
// `handleMarkedArmMoveTransfer` -> `consumeMarkedArmTransfer`, which finds no
// `states_` entry for the pattern binding `w` (dataflow keeps none for arm
// bindings) and rejects it as "not an owned binding at this point" (§28.3
// K3.4, v0.209) — code the spec admits could not be written.
//
// THE FIX. `owningMatchTransferBindings_` is populated for the WHOLE method
// by `registerBorrowOnlyArmBindings`'s prepass over every `case`
// construct — independent of which nested selecting form is being walked —
// so `isOwningMatchTransferredArmBinding(sym)` is checked as a fallback
// alongside the `armNode`-gated form, admitting the transfer regardless of
// nesting depth.
//
// FAIL-WITHOUT: on origin/main (pre-fix), A5 below fails to compile with the
// exact K3.4 "not an owned binding" diagnostic; D2 (statement position) fails
// identically.
//
// per §35.5, §28.3 K3.4, §20/§19.2, v0.307

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

// The issue's own prelude, trimmed to what these rows need: a single-field
// `Widget` so each disposal prints one deterministic line.
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

// (A5) THE ISSUE'S EXPRESSION-POSITION REPRO, verbatim shape: the owning
// match's arm value is a nested if-expression whose selected branch is the
// marked transfer `move w`.
BEZEL_TEST(owning_match_nested_if_move_arm_transfer_4944,
           expr_arm_nested_if_move_w_accepted_28_3_k3_4) {
    JitResult r = runAot(prog(
        "    var p := true; "
        "    var o := Mk(); "
        "    var r := (case move o of "
        "        W(var w, _) => (if p then move w else new Widget(\"z\")), "
        "        X => new Widget(\"x\")); "
        "    Console.WriteLine($\"r={r.Name()}\"); "));
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // `b` is never moved out of the payload — it is disposed with the
    // scrutinee's construct-completion reclaim; `a`/`w` is moved into `r`,
    // printed, then disposed once when `r`'s scope ends.
    BEZEL_EXPECT_EQ(r.stdoutText, "dispose b\nr=a\ndispose a\n");
}

// (D2) THE ISSUE'S STATEMENT-POSITION REPRO: the same nested-if `move w`
// inside a `do … end` arm body, stored into a pre-existing outer local.
BEZEL_TEST(owning_match_nested_if_move_arm_transfer_4944,
           stmt_arm_nested_if_move_w_accepted_28_3_k3_4) {
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
    // MEASURED (post-fix): unlike the expression-position row above, here
    // `r` is an unbound owned local scoped to the arm body's `do … end`
    // block, so it (and the `w`/`a` payload it holds) disposes at THAT
    // block's exit, before the owning match's own construct-completion
    // reclaim of the untransferred payload `b`.
    BEZEL_EXPECT_EQ(r.stdoutText, "r=a\ndispose a\ndispose b\n");
}

// Loop the accepted shape 200x under the plain oracle: a false-negative
// double-free/leak from the admission would show under repetition even
// without ASan instrumentation (allocator corruption / crash), and this row
// is also run again with BEZEL_AOT_FORCE_SANITIZE=address in the lane check.
BEZEL_TEST(owning_match_nested_if_move_arm_transfer_4944,
           expr_arm_nested_if_move_w_looped_no_leak_no_crash) {
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
