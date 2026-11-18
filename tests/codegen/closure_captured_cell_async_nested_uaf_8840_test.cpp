// Codegen — issue #8840: an ASYNC lambda whose body nests a SYNC lambda that
// captures a `string` local which is also WRITTEN (in the async body or in the
// nested lambda) freed the frame's reference in the nested lambda's envdtor,
// so the async method's later read of the local was a heap-use-after-free.
// This is #8811's twin in the async-lambda venue.
//
// THE RULE. §27.5 (v0.359): "Captured variables are heap-allocated in a
// closure object ... Modifications to a captured variable inside the lambda
// are visible outside, and vice versa." `Run` reads the value the lambdas
// wrote after both have died. §28.9 (v0.359): `string` values are
// runtime-managed, so the runtime, not the program, keeps that value alive
// while the binding holds it.
//
// ROOT CAUSE. #2706 suppresses BOTH the per-capture retain and the envdtor's
// content release for a cell that is WRITTEN and shared by more than one env
// (`mutatedSharedCaptureCell`). emitAsyncLambda clears the capture sets and
// re-scans only the async lambda's own statements, without the ancestor
// merges sync lowerLambda has (#4240 multiCaptureLocals_, #8811
// assignedCaptureVars_, #4226 returnedCaptureVars_). Inside the async body
// `s` is captured by one lambda node (`g`), so it looked single-captured
// although the async lambda's own env shares the cell. The nested capture
// retained "ab" and its envdtor released "abc", the frame's only reference.
//
// THE FIX. emitAsyncLambda snapshots the three ancestor sets before its
// clears and merges them back after the re-scan, as lowerLambda does. It only
// adds keys, so it can only suppress a nested env's retain/release PAIR; the
// cell's final value then leaks (#2718, the #2706 trade, identical to the
// sync twin), so these rows use `runAotCheckedLenient` (AddressSanitizer on,
// LeakSanitizer off) and compare the full stdout.
//
// Every program runs its body 200 times under generated-code
// AddressSanitizer. MEASURED on r30-c1 @ d8e779e5c: every row below is a
// heap-use-after-free in `bz$async$M.C$Run$Resume` with no output.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotCheckedLenient;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off); a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// `asyncLambda` is the initializer of `f2` inside
// `async Run(t: string): Task<int>` (called with "a"), after `var s := t + "b";`
// and the optional `pre` statements; `ret` is Run's final statement. Main
// prints the sum over 200 calls. `members` adds class members.
std::string program(const std::string& asyncLambda,
                    const std::string& ret = "return r + s.Length;",
                    const std::string& pre = "",
                    const std::string& members = "") {
    return R"BZL(
module M;
public class C
)BZL" + members + R"BZL(
  public static async method Run(t: string): Task<int>
    var s := t + "b";
)BZL" + pre + R"BZL(
    var f2 := )BZL" + asyncLambda + R"BZL(;
    var r := await f2();
    )BZL" + ret + R"BZL(
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0..<200 do total := total + await C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// #8840's repro, as filed: the async body writes `s`, the nested sync lambda
// reads it. 3 + 3 per call.
BEZEL_TEST(closure_captured_cell_async_nested_uaf_8840,
           issue_repro_async_body_writes_nested_sync_reads_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "async lambda() => do await Task.Delay(0); var g := lambda() => s.Length; "
        "s := s + \"c\"; return g(); end")),
        "1200\n");
}

// The issue's writer-in-nested form: the nested sync lambda writes `s`.
BEZEL_TEST(closure_captured_cell_async_nested_uaf_8840,
           nested_sync_lambda_writes_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "async lambda() => do await Task.Delay(0); var g := lambda() => do "
        "s := s + \"c\"; return s.Length; end; return g(); end")),
        "1200\n");
}

// A captured `string` PARAMETER (keyed by its symbol, not a decl node) written
// in the async body and read by the nested lambda.
BEZEL_TEST(closure_captured_cell_async_nested_uaf_8840,
           string_parameter_written_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "async lambda() => do await Task.Delay(0); var g := lambda() => t.Length; "
        "t := t + \"bc\"; return g(); end",
        "return r + t.Length + s.Length - 2;")),
        "1200\n");
}

// An EXPRESSION-bodied async lambda: emitAsyncLambda never re-scans it, so
// its capture sets are empty after the clears unless the ancestor's merge
// refills them.
BEZEL_TEST(closure_captured_cell_async_nested_uaf_8840,
           expression_bodied_async_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "async lambda() => C.Apply(lambda() => do s := s + \"c\"; return s.Length; end)",
        "return r + s.Length;", "",
        "  public static method Apply(f: () -> int): int\n"
        "    return f();\n"
        "  end Apply;\n")),
        "1200\n");
}

// The nested lambda is itself ASYNC: the inner async lambda's re-scan must
// keep the outer async lambda's (and Run's) facts too.
BEZEL_TEST(closure_captured_cell_async_nested_uaf_8840,
           async_lambda_nested_in_async_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "async lambda() => do var g := async lambda() => do await Task.Delay(0); "
        "s := s + \"c\"; return s.Length; end; return await g(); end")),
        "1200\n");
}

// A FUNCTION-typed local (§28.9's other runtime-managed capture kind the
// envdtor releases), reassigned in the async body after the nested capture.
BEZEL_TEST(closure_captured_cell_async_nested_uaf_8840,
           function_typed_local_written_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "async lambda() => do await Task.Delay(0); var g := lambda() => fn(); "
        "fn := lambda() => 2; return g() + 1; end",
        "return r + fn() + 1;",
        "    var fn: () -> int := lambda() => 1;\n")),
        "1200\n");
}
