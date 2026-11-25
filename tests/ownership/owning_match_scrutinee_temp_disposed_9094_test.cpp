// fix #9094 (§35.5 *The owning match*, §19.3 *Patterns*, §28.3.11 *Unbound
// owned temporaries*, v0.359): an OWNING-CHOICE scrutinee temporary in a
// `case` whose arm binds the transferred payload NEVER has that payload
// disposed — the destructor never runs and the heap block leaks.
//
// THE DEFECT. §35.5's owning match "transfers the selected case's `owned` and
// move-only value payloads into the arm's bindings and consumes the scrutinee
// value", and such a binding "is an owned local of the arm body's scope, and
// §28.3's standard rules apply to it on every arm-exit edge — falling off
// the arm still owning the value auto-drops it".
//
// Codegen implemented the TRANSFER half and not the DISCHARGE half. The
// transfer mark (`dischargeOwningMatchTransfers`) takes the transferred
// payload slot out of the construct-completion reclaim — §35.5's "transfers …
// into its bindings", so that reclaim disposes only the UN-TRANSFERRED
// remainder — which makes the ARM BINDING the payload's only owner. Every
// discharge §35.5 names reaches the payload THROUGH that binding's own slot
// (`move r`, `r.Dispose()`, `_ := r;`), but an arm that discharges NOTHING
// had no owner at all: the payload's destructor never ran and the block
// leaked.
//
// MEASURED on the base (r30-c1 @ 74b77d29d), ASan/LSan, over the issue's own
// program: stdout `N=0 Made=100 S=700` (the counting destructor's total is
// ZERO) and "Direct leak of 1600 byte(s) in 100 object(s)", allocated in
// `bz$M.H$Make`.
//
// WHAT IS NOW TRUE. The owning match's transferred payload binding is an owned
// local of the arm body's scope, so the payload is disposed at the arm's exit
// edge — on the normal path AND on the arm's abnormal exits (a `throw`, a
// `return`, a `break`/`continue` out of the arm) — exactly once, and exactly
// once with the three discharges §35.5 names (`move r`, `r.Dispose()`,
// `_ := r;`), which all reclaim through the same slot and null it.
//
// OBSERVABILITY. The leak is the ONLY oracle: the counting destructor's total
// is 0 on the base and 200 with the fix, and LeakSanitizer reports the block
// on the base and nothing with the fix. 200 iterations because a one-shot leak
// row can keep its block reachable from a live stack slot and read clean on
// BOTH sides.
//
// The rows are `runAotChecked` (the ASan/LSan-instrumented oracle), so the
// base's leak is caught by the SANITIZER as well as by the count — the two
// error directions a discharge can introduce (a leak below the count, a
// double free above it) are separated at once.
//
// per §35.5, §19.3, §28.3.11, §28.3, v0.359

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — the AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// §35.5's own owning-choice shape: an `owned` CLASS payload in a case, the
// choice returned by an owning-choice-returning call (the "unbound owned
// temporary" announcing form), and a counting destructor so the disposal is
// observable as a NUMBER rather than as a sanitizer report alone.
const char* kDecls =
    "public class Res "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "  public method Id(): int => 7; "
    "end Res; "
    "public choice OChoice Text(owned r: Res), Empty; "
    "end OChoice; "
    "public class H "
    "  public constructor() end constructor; "
    "  public method Make(): OChoice return OChoice.Text(new Res()); end Make; "
    "  public method Take(owned x: Res): int "
    "    using var u := owned x; "
    "    return 7; "
    "  end Take; "
    "  public method Kaboom(): int throw new Exception(\"boom\"); end Kaboom; "
    "end H; ";

// The issue's own program, amplified to 200 iterations and with the
// counting destructor's total printed.
const char* kStmtBody =
    "var i := 0; "
    "var s := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  case h.Make() of "
    "    Text(r) => s := s + r.Id(); "
    "  end case; "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "return 0;";

// The EXPRESSION-position twin: the same owning match as a `case` EXPRESSION's
// operand. The transfer and the leak are the same (§35.5's rule is the
// construct's, not the position's), so the fix must cover both.
const char* kExprBody =
    "var i := 0; "
    "var s := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  var v := (case h.Make() of Text(r) => r.Id(), Empty => 0); "
    "  s := s + v; "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "return 0;";

std::string prog(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end Program;";
}

// The arm that DISCHARGES the binding by MOVING the payload out (§35.5's
// `using var u := move r;` adoption). The payload is disposed by `u`, at the
// arm's own scope end — so the binding's scope-end disposal must NOT fire a
// second time (the move nulls the binding's slot, which is what makes the
// two exactly-once).
const char* kMoveBody =
    "var i := 0; "
    "var s := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  case h.Make() of "
    "    Text(r) => do "
    "      using var u := move r; "
    "      s := s + u.Id(); "
    "    end "
    "  end case; "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "return 0;";

// The arm that THROWS: §35.5's "§28.3's standard rules apply to it on every
// arm-exit edge" covers the EXCEPTIONAL exit too — the payload is disposed
// while the exception unwinds out of the arm, not at the construct's
// completion (which never happens on that path).
const char* kThrowBody =
    "var i := 0; "
    "var h := new H(); "
    "while i < 200 do "
    "  try "
    "    case h.Make() of "
    "      Text(r) => do h.Kaboom(); end "
    "    end case; "
    "  catch var e: Exception => Console.WriteLine(\"caught\"); "
    "  end; "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made}\"); "
    "return 0;";

}  // namespace

// ---------------------------------------------------------------------------
// THE LEAK — statement position (the issue's own program).
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_owning_match_scrutinee_temp_payload_disposed_at_arm_exit) {
    // 200 is the EXACT answer: one disposal per iteration, at the arm's exit
    // edge. Below it is a leak (the base: 0), above it a double free.
    auto r = runAotChecked(prog(kStmtBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=200 Made=200 S=1400\n"));
}

// ---------------------------------------------------------------------------
// THE LEAK — expression position (the same owning match as an operand).
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_owning_match_expr_scrutinee_temp_payload_disposed_at_arm_exit) {
    auto r = runAotChecked(prog(kExprBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=200 Made=200 S=1400\n"));
}

// ---------------------------------------------------------------------------
// THE EXCEPTIONAL ARM-EXIT EDGE — the payload is disposed while unwinding.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_owning_match_scrutinee_temp_payload_disposed_on_unwind) {
    auto r = runAotChecked(prog(kThrowBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // One catch per iteration, and ONE disposal per iteration: the disposal
    // happens while the exception unwinds OUT OF THE ARM (the construct's
    // completion never happens on that path), so the count is already 200
    // when the last catch runs.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "caught"), 200);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("N=200 Made=200") != std::string::npos);
}

// ---------------------------------------------------------------------------
// THE CONTROL — an arm that DISCHARGES the binding by moving the payload out.
// The payload is disposed by the adopting `using var`, exactly once: the
// binding's scope-end disposal must NOT fire a second time (the move nulls
// the binding's slot, which is what makes the two exactly-once). A double
// free here would be the fix's own defect.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           control_35_5_arm_moving_the_payload_out_disposes_it_exactly_once) {
    auto r = runAotChecked(prog(kMoveBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=200 Made=200 S=1400\n"));
}

// ===========================================================================
// fix-forward of #9094 (§35.5, §28.3, v0.359): EXPRESSION position, an
// owned-binding arm that is NOT the selected one.
//
// The first attempt registered every owned-binding arm's disposal on the
// scope ENCLOSING the `case` expression, at lowering time, so that scope's
// exit disposed the binding slot of an arm that never ran — an uninitialised
// slot. MEASURED on that attempt (commit 9e4fd56a4): each row below crashed,
// SEGV in a plain build (rc 139) as well as under ASan. §35.5: the binding
// "is an owned local of the arm body's scope, and §28.3's standard rules
// apply to it on every arm-exit edge — falling off the arm still owning the
// value auto-drops it". Falling off the arm is a property of the arm that
// RAN; an arm that did not run has no binding to drop.
//
// Every row is a 200-iteration loop on `runAotChecked`, and its oracle is the
// EXACT destructor count against the construction count (`N` vs `Made`):
// below is a leak, above a double free, and a crash prints nothing.
// ===========================================================================
namespace {

const char* kDecls2 =
    "public class Res "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "  public method Id(): int => 7; "
    "end Res; "
    "public choice OChoice Text(owned r: Res), Two(owned a: Res, owned b: Res), "
    "  Empty; "
    "end OChoice; "
    "public class H "
    "  public constructor() end constructor; "
    "  public method Make(): OChoice return OChoice.Text(new Res()); end Make; "
    "  public method MakeTwo(): OChoice "
    "    return OChoice.Two(new Res(), new Res()); "
    "  end MakeTwo; "
    "  public method MakeE(): OChoice return OChoice.Empty; end MakeE; "
    "  public method Kaboom(): int throw new Exception(\"boom\"); end Kaboom; "
    "end H; "
    "public class G "
    "  public static method Mk(i: int): Result<Res, string> "
    "    if i mod 2 = 0 then return Result<Res, string>.Ok(new Res()); end; "
    "    return Result<Res, string>.Error(\"bad\"); "
    "  end Mk; "
    "end G; ";

std::string prog2(const std::string& loopBody) {
    return std::string("module M; ") + kDecls2 +
           "public class Program "
           "  public static method Main(): int "
           "    var h := new H(); "
           "    var s := 0; "
           "    var i := 0; "
           "    while i < 200 do " + loopBody +
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ---------------------------------------------------------------------------
// CONTROL — the `Empty` arm is selected, the two owned-binding arms are not,
// and nothing owned is ever allocated. Correct before #9094 was touched; the
// first attempt SEGV'd here (the unselected arms' slots were disposed at the
// enclosing scope's exit).
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           control_35_5_expr_unselected_owned_binding_arms_dispose_nothing) {
    auto r = runAotChecked(prog2(
        "var v := (case h.MakeE() of Text(r) => r.Id(), Two(a, b) => 2, "
        "          Empty => 3); "
        "s := s + v; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N=0 Made=0 S=600\n"));
}

// ---------------------------------------------------------------------------
// The stdlib `Result<Res, string>` in expression position, `case move`, the
// arms alternating: `Ok(v)` on even iterations, `Error(e)` on odd ones. Each
// `Ok` payload is dropped at its arm's exit (100 of them); no `Ok` slot is
// touched on an `Error` iteration.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_expr_stdlib_result_alternating_arms_disposes_each_ok_once) {
    auto r = runAotChecked(prog2(
        "var k := 0; "
        "var b := G.Mk(i); "
        "k := (case move b of Ok(v) => v.Id(), Error(e) => 2); "
        "s := s + k; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N=100 Made=100 S=900\n"));
}

// ---------------------------------------------------------------------------
// A `when` guard: the guarded `Text(r)` arm is selected on even iterations,
// the unguarded `Text(r)` arm on odd ones, and `Two`/`Empty` never. The guard
// is evaluated BEFORE the transfer, so a declined guard transfers nothing and
// the next arm's binding is the one that owns (and drops) the payload.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_expr_guarded_arm_drops_only_the_selected_arms_binding) {
    auto r = runAotChecked(prog2(
        "var v := (case h.Make() of Text(r) when i mod 2 = 0 => 1, "
        "          Text(r) => 2, Two(a, b) => 3, Empty => 4); "
        "s := s + v; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N=200 Made=200 S=300\n"));
}

// ---------------------------------------------------------------------------
// A NESTED owning match: the inner `case` is the outer `Text(r)` arm's
// expression. Both selected arms' payloads are dropped (two per iteration),
// and neither match's unselected `Two(a, b)` arm disposes anything.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_expr_nested_owning_match_drops_both_selected_bindings) {
    auto r = runAotChecked(prog2(
        "var v := (case h.Make() of "
        "            Text(r) => (case h.Make() of Text(q) => r.Id() + q.Id(), "
        "                        Two(a, b) => 0, Empty => 0), "
        "            Two(a, b) => 2, Empty => 3); "
        "s := s + v; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N=400 Made=400 S=2800\n"));
}

// ---------------------------------------------------------------------------
// The arms ALTERNATE over all three cases (`Two` when i mod 3 = 0, `Empty`
// when 1, `Text` when 2): 67 + 67 `Two` payloads and 66 `Text` payloads are
// made, and each is dropped exactly once, by the arm that ran.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_expr_alternating_arms_drop_only_the_arm_that_ran) {
    auto r = runAotChecked(prog2(
        "var o: OChoice := h.MakeE(); "
        "if i mod 3 = 0 then o := h.MakeTwo(); end; "
        "if i mod 3 = 2 then o := h.Make(); end; "
        "var v := (case move o of Text(r) => r.Id(), Two(a, b) => 2, "
        "          Empty => 3); "
        "s := s + v; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // i mod 3 = 0: 67 iterations (Two, 2 each), = 1: 67 (Empty, 3), = 2: 66
    // (Text, 7): Made = 134 + 66 = 200, S = 134 + 201 + 462 = 797.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N=200 Made=200 S=797\n"));
}

// ---------------------------------------------------------------------------
// The arm's EXCEPTIONAL exit in expression position: the selected `Text(r)`
// arm's expression throws, and the payload is dropped on that edge — while
// the exception unwinds out of the arm, inside the arm's own control flow.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_expr_throwing_arm_drops_its_binding_on_unwind) {
    auto r = runAotChecked(prog2(
        "try "
        "  var v := (case h.Make() of Text(r) => h.Kaboom(), Two(a, b) => 2, "
        "            Empty => 3); "
        "  s := s + v; "
        "catch var e: Exception => s := s + 1; "
        "end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N=200 Made=200 S=200\n"));
}

// ---------------------------------------------------------------------------
// A closure in the selected arm CAPTURES the binding (§27.5): the capture
// boxes `r` into a cell and rebinds the binding's pending disposal to the
// cell holder, so the arm-exit drop must go through that holder. Dropping
// through the pre-boxing slot left the cell naming the freed payload, and a
// stale entry then read it (heap-use-after-free; a plain-build SEGV).
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_match_scrutinee_temp_disposed_9094,
           spec_35_5_expr_arm_capturing_its_binding_drops_it_once) {
    auto r = runAotChecked(prog2(
        "var v := (case h.Make() of Text(r) => (lambda() => r.Id())(), "
        "          Two(a, b) => 2, Empty => 3); "
        "s := s + v; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N=200 Made=200 S=1400\n"));
}
