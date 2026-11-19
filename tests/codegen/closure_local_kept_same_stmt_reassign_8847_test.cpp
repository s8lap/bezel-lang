// Codegen — issue #8847: a closure local KEPT (stored into a collection) and
// then REASSIGNED IN THE SAME STATEMENT
// (`b := (if d.TryAdd(0, b) then Mk(k) else g);`) had its stored value
// released on the unwind edge when the reassignment's own right-hand side
// threw after the keep, so the caller's collection held a freed closure.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): for a function-typed
// value "the type's lifetime is managed by the runtime without user
// involvement", so a function value a collection still holds stays callable
// whichever way the frame that stored it was left. §27.5 (v0.359): "Captured
// variables are heap-allocated in a closure object" and a closure's captures
// live as long as the closure does.
//
// ROOT CAUSE. #8753's reaching-definition analysis (the donor pass at the
// end of `collectClosureReleaseEligible`) samples the method's state for an
// exception edge (`unwindSt`) and for a `try`'s catch entry (`tryAccs`) only
// at STATEMENT BOUNDARIES. Here the keep (`d.TryAdd(0, b)`) and the throw
// (`Mk(k)`) are in the SAME statement — an assignment to `b` itself — so the
// boundary before it has `b`'s initial definition not yet kept, and the
// boundary after it has only the new definition. The kept initial value was
// therefore released on the unwind path while `d` still held it.
//
// THE FIX. When a statement is a kept site, the kept state is joined into
// the unwind state and into every `try` accumulator before the statement's
// own effect (the assignment's kill) — the same shape as `narrow #8824`
// (ba64bb9c7), which joined a catch guard's kept store into `unwindSt`.
//
// Every program loops 200 times; the odd iterations throw.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1, on
// r30-c1 @ fbe542d4d): every row below prints the k=0 line and then reports
// heap-use-after-free at k=1; with the fix, 201 correct lines and the #5866
// leak of the stored closure (so LeakSanitizer is off: `runAotCheckedLenient`).
//
// THE ORDER-AWARE RULE (fix-forward #8847). §22.5 (v0.359) "Evaluation
// order": "A call's supplied argument expressions are evaluated in call-site
// textual order — left to right as written"; §17.1 (v0.359) gives the
// operators left-to-right associativity. The kept state may therefore reach
// an unwind edge only for a throw that happens AFTER the keep: in
// `b := (if Thr(k) > 0 and d.TryAdd(0, b) then g else g);` the `Thr(k)` call
// is evaluated BEFORE `d.TryAdd(0, b)` stores anything, so the value is NOT
// kept when that exception leaves the frame and the scope-end release must
// still run there. The throw-before-the-keep row below is that shape: it is
// GREEN on the base (a control) and RED under a join that ignores the order.
//
// THE PATH-SENSITIVE RULE (this fix-forward). EVERY node the spec says can
// raise is a raise point — not only the calls: §6.1 (v0.359) checked `+ - *
// / mod` and unary `-`, §7.8 / §17.8 (v0.359) `x!`, §17.12 (v0.359) a
// `throw` expression, §18.5 (v0.359) an interpolation hole's `ToString()`,
// §25 (v0.359) a user-defined operator, §23.3 (v0.359) a property read
// (including a bare implicit-`this` one). And the keep must be reached on
// EVERY path: a short-circuit (`and` / `or` / `??`) or an unselected branch
// that skips it is a decline too. The last row below is the MUST-NOT-LEAK row
// for that rule — it leaks 15200 B under a join that ignores it, so
// LeakSanitizer is ON and the leak IS the failure.
//
// THE TWO BASE-GREEN CONTROLS (owner, 2026-11-12). A gtest row must be RED
// on the base: a row that is green on the base pins nothing, so a suite may
// keep at most TWO boundary controls — the ones a corpus case cannot express.
// This suite keeps exactly those two, and they cover the two independent
// halves of the fix's predicate (`!eff.raiseUnkept && eff.ownMust`):
//   * `throw_evaluated_before_the_keep_in_the_same_statement_spec_28_9` — the
//     ORDER half: a raise evaluated BEFORE the keep leaves the value NOT
//     kept, so the scope-end release must still run there. Red under a join
//     that ignores the evaluation order (the statement-granular join of
//     cbf9aed39).
//   * `short_circuit_skips_the_keep_before_the_raise_spec_17_2` — the REACH
//     half: a path that SKIPS the keep leaves the value NOT kept, so the
//     scope-end release must still run there. Red under a join that treats
//     the statement as kept without checking that the keep was reached on
//     that path (e41f31956's `keepIsFirstInvocation`).
// The two base-green shapes this suite used to carry as well — a division by
// zero before the keep (§6.1) and a null-forgiving `!` before the keep
// (§17.8) — are GREEN on the base, so they pin nothing as gtest rows: they
// moved to the conformance corpus with their deciding spec sentences, in
// /var/tmp/bezel-scratch/r42-m120-8847/corpus-candidates/. The corpus runner
// has no ASan/LSan leg, so the leak oracle of those two shapes is the reason
// the two kept controls stay in gtest.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Iteration i calls the stored closure `x - y + i` twice:
// (1 - 2 + i) + (4 - 1 + i).
std::string expected200() {
    std::string e;
    for (int i = 0; i < 200; ++i) e += std::to_string(2 + 2 * i) + "\n";
    e += "DONE\n";
    return e;
}

// The harness's BEZEL_EXPECT_EQ prints only the EXPRESSION, so a failure
// would not say whether the program failed to run, exited non-zero (the
// sanitizer fired) or printed the wrong lines. This prints WHAT happened —
// the sanitizer report the harness captured, and the program's own output —
// so the failure is self-describing.
void expectKeptClosureStillCallable(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    const std::string want = expected200();
    std::string got;
    if (!r.ran) got = "<did-not-run>";
    else if (r.exitCode != 0) got = "<exit " + std::to_string(r.exitCode) + ">";
    else got = r.stdoutText;
    if (got == want) return;
    std::cout << "    expected 201 lines (\"2 + 2k\" per iteration, then "
                 "\"DONE\"); got \""
              << got.substr(0, 120) << "\"\n";
    for (const auto& d : r.diagnostics.diagnostics())
        std::cout << "    diagnostic: " << d.message.substr(0, 600) << "\n";
    BEZEL_EXPECT_TRUE(got == want);
}

// The issue's own repro: the keep and the throwing right-hand side are in
// ONE statement, and the exception leaves `Fill` (caught in `Main`).
std::string fillProgram(const std::string& fillBody) {
    return R"BZL(
module M;
type Fn := (int, int) -> int;
public class C
  public static method Mk(k: int): Fn
    if k mod 2 = 1 then
      throw new InvalidOperationException("x");
    end;
    return lambda(x: int, y: int) => 0;
  end Mk;
  public static method Fill(d: Dictionary<int, Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
)BZL" + fillBody + R"BZL(
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
      try
        Fill(d, i);
      catch
        var e: InvalidOperationException => do var z := 0; end
      end try;
      Console.WriteLine(d[0](1, 2) + d[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
}

// The program for the MUST-NOT-LEAK rows: the operation that raises is
// evaluated BEFORE the keep, in the SAME statement, so the value is NOT kept
// when the exception leaves `Fill` — and the scope-end release must still run
// there. The frame's reference is then the closure's only one, so the
// program must leak NOTHING: LeakSanitizer is ON (`runAotChecked`).
//
// `z` and `n` are the operands the moved corpus candidates use (a division by
// zero and a null-forgiving `!`); the row that stayed in gtest uses `flag`.
// The parameters stay so that the corpus-candidate programs and the gtest row
// are the same program shape.
std::string leakOracleProgram(const std::string& fillBody) {
    return R"BZL(
module M;
type Fn := (int, int) -> int;
public class C
  public static method Mk(k: int): Fn
    throw new InvalidOperationException("x");
  end Mk;
  public static method Fill(d: Dictionary<int, Fn>, k: int, z: int, n: int?, flag: bool)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
)BZL" + fillBody + R"BZL(
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
      try
        Fill(d, i, 0, null, false);
      catch
        var e: Exception => do var z := 0; end
      end try;
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
}

// The LEAK ORACLE: the program must exit 0, print "DONE", and leak nothing.
// LeakSanitizer makes a leak a non-zero exit, so `exitCode == 0` IS the
// leak assertion.
void expectNoLeak(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran || r.exitCode != 0 || r.stdoutText != "DONE\n") {
        std::cout << "    expected exit 0 and \"DONE\" (LeakSanitizer on, so "
                     "a leak is a non-zero exit); ran="
                  << (r.ran ? "yes" : "no")
                  << " exit=" << r.exitCode << " out=\""
                  << r.stdoutText.substr(0, 120) << "\"\n";
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "    diagnostic: " << d.message.substr(0, 600) << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "DONE\n");
}

}  // namespace

// #8847's repro, as filed: the keep (`d.TryAdd(0, b)`) and the throw
// (`Mk(k)`) are in the SAME statement, an assignment to `b` itself.
BEZEL_TEST(closure_local_kept_same_stmt_reassign_8847,
           issue_repro_kept_then_reassigned_in_same_statement_spec_28_9) {
    expectKeptClosureStillCallable(fillProgram(
        "    b := (if d.TryAdd(0, b) then Mk(k) else g);\n"));
}

// The same, with the assignment inside a SAME-FRAME `try … catch … end try;`:
// the catch entry and the normal scope-end release are both reached with
// `b` still holding the kept value.
BEZEL_TEST(closure_local_kept_same_stmt_reassign_8847,
           kept_then_reassigned_in_same_statement_caught_same_frame_spec_28_9) {
    expectKeptClosureStillCallable(fillProgram(
        "    try\n"
        "      b := (if d.TryAdd(0, b) then Mk(k) else g);\n"
        "    catch\n"
        "      var e: InvalidOperationException => do var z := 0; end\n"
        "    end try;\n"));
}

// The same-frame catch REASSIGNS `b` itself: the reassignment in the catch
// arm must not release the kept value either (the collection holds it), so
// the closure stays callable.
BEZEL_TEST(closure_local_kept_same_stmt_reassign_8847,
           kept_then_reassigned_in_same_statement_catch_reassigns_spec_28_9) {
    expectKeptClosureStillCallable(fillProgram(
        "    try\n"
        "      b := (if d.TryAdd(0, b) then Mk(k) else g);\n"
        "    catch\n"
        "      var e: InvalidOperationException => do b := g; end\n"
        "    end try;\n"));
}

// THE ORDER-AWARE CONTROL. The throw is evaluated BEFORE the keep in the
// SAME statement: `Thr(k)` is the left operand of the `and`, so it runs
// before `d.TryAdd(0, b)` stores anything (§22.5, v0.359: a call's supplied
// argument expressions are evaluated in call-site textual order — left to
// right as written; §17.1, v0.359: the operators are left-to-right
// associative). The value is therefore NOT kept when the exception leaves
// `Fill`, so the scope-end release must still run there — the frame's
// reference is the only one the closure has.
//
// This row is GREEN on the base (it is a control, not a fix row: the base
// already releases the value) and RED under a join that ignores the order —
// the statement-granular join of cbf9aed39, which marked the local kept on
// the unwind edge and leaked the closure. MEASURED by this lane against
// cbf9aed39's Setup.cpp: "LeakSanitizer: detected memory leaks" — "Direct leak
// of 12800 byte(s) in 200 object(s)" — and the row's exit code is 99.
// LeakSanitizer is ON here, so the leak is the failure the row catches.
BEZEL_TEST(closure_local_kept_same_stmt_reassign_8847,
           throw_evaluated_before_the_keep_in_the_same_statement_spec_28_9) {
    const std::string prog = R"BZL(
module M;
type Fn := (int, int) -> int;
public class C
  public static method Thr(k: int): int
    throw new InvalidOperationException("t");
  end Thr;
  public static method Fill(d: Dictionary<int, Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    b := (if Thr(k) > 0 and d.TryAdd(0, b) then g else g);
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
      try
        Fill(d, i);
      catch
        var e: InvalidOperationException => do var z := 0; end
      end try;
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
    auto r = runAotChecked(prog);
    if (!r.ran || r.exitCode != 0 || r.stdoutText != "DONE\n") {
        std::cout << "    expected exit 0 and \"DONE\"; ran="
                  << (r.ran ? "yes" : "no")
                  << " exit=" << r.exitCode << " out=\""
                  << r.stdoutText.substr(0, 120) << "\"\n";
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "    diagnostic: " << d.message.substr(0, 600) << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "DONE\n");
}

// ---------------------------------------------------------------------------
// THE MUST-NOT-LEAK CONTROL (fix-forward #8847). A path that SKIPS the keep
// leaves the value NOT kept, so the scope-end release must still run on the
// unwind edge. A join that ignores this marks the local kept there and leaks
// the closure: MEASURED 15200 B in 600 allocations over the 200 iterations.
// LeakSanitizer is ON, so the leak IS the failure.
//
// MOVED TO THE CORPUS (they are GREEN on the base, so they pin nothing as
// gtest rows, and the cap is two base-green controls per suite):
//   * divide_by_zero_before_the_keep_in_the_same_statement_spec_6_1 — a
//     division by zero before the keep (§6.1, v0.359);
//   * null_forgiving_bang_before_the_keep_in_the_same_statement_spec_17_8 —
//     a null-forgiving `!` before the keep (§7.8 / §17.8, v0.359).
// Both programs, with the spec sentence that decides each verdict, are in
// /var/tmp/bezel-scratch/r42-m120-8847/corpus-candidates/.
// ---------------------------------------------------------------------------

// §17.2 (v0.359): "`and` and `or` short-circuit: the right operand is not
// evaluated if the result is determined by the left." `flag` is FALSE, so the
// keep `d.TryAdd(0, b)` is SKIPPED, and the `else` branch's `Mk(1)` raises.
// The value is not kept when the exception leaves `Fill` — the frame's
// reference is the closure's only one, so the scope-end release must run
// there and the program must leak nothing.
//
// THE SECOND OF THE SUITE'S TWO BASE-GREEN CONTROLS. It is GREEN on the base
// (the base already releases the value on the unwind edge) and RED under a
// join that treats the statement as kept without checking that the keep was
// REACHED on the path — e41f31956's `keepIsFirstInvocation`, which walks the
// keep as the first CALL and so joins it even when a short-circuit skips it
// (MEASURED on e41f31956's Setup.cpp: "LeakSanitizer: detected memory leaks"
// — "Direct leak of 12800 byte(s) in 200 object(s)" — and the row's exit code
// is 99). LeakSanitizer is ON here, so the leak is the failure the row
// catches.
BEZEL_TEST(closure_local_kept_same_stmt_reassign_8847,
           short_circuit_skips_the_keep_before_the_raise_spec_17_2) {
    expectNoLeak(leakOracleProgram(
        "    b := (if flag and d.TryAdd(0, b) then g else Mk(1));\n"));
}
