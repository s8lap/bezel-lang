// Codegen — issue #8811: a `string` local assigned through a NESTED lambda
// (`lambda() => lambda() => do s := s + "c"; ... end`) was freed by the nested
// lambda's envdtor while the frame still owned it, so the frame's later read
// of `s` was a heap-use-after-free.
//
// THE RULE. §27.5 (v0.359): "Captured variables are heap-allocated in a
// closure object ... Modifications to a captured variable inside the lambda
// are visible outside, and vice versa." The frame reads the value the nested
// lambda wrote, after that lambda has died. §28.9 (v0.359): `string` and
// function-typed values are runtime-managed, so the runtime, not the program,
// keeps that value alive while the binding holds it.
//
// ROOT CAUSE. #2706 suppresses BOTH the per-capture retain and the envdtor's
// content release for a cell that is WRITTEN and shared by more than one env
// (`mutatedSharedCaptureCell`): the retain lands on the value present at
// capture, the release on whatever the last write left. The "written" half
// is `assignedCaptureVars_`, which lowerLambda clears for each nested lambda's
// own re-scan. An EXPRESSION-bodied lambda never re-scans, so the set stayed
// empty inside it; a block-bodied one re-scans only its own statements, so a
// write in an ancestor body was invisible. Either way the nested capture of
// the ancestor's cell took the unsuppressed path: a retain on "ab" and an
// envdtor release of the frame's only reference to "abc".
//
// THE FIX. The same snapshot-and-merge #4226 (returnedCaptureVars_) and #4240
// (multiCaptureLocals_) apply: the ancestor's "assigned somewhere in this
// nesting" verdict is merged back after the nested re-scan. It can only add
// keys, so it can only suppress a nested env's retain/release PAIR, which is
// self-balanced; the cell's final value then leaks (#2718, the #2706 trade),
// so these rows use `runAotCheckedLenient` (AddressSanitizer on, LeakSanitizer
// off) and compare the full stdout.
//
// Every program runs its body 200 times under generated-code
// AddressSanitizer. MEASURED on r30-c1 @ e8b35d104: every row below is a
// heap-use-after-free (freed by `bz$lam$N$envdtor`) with no output.

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

// `runBody` is the body of `Run(t: string): int` (called with "a"); Main
// prints the sum over 200 calls.
std::string program(const std::string& runBody) {
    return R"BZL(
module M;
public class C
  public static method Run(t: string): int
)BZL" + runBody + R"BZL(
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// #8811's repro, as filed (looped): 3 + 3 per call.
BEZEL_TEST(closure_captured_cell_nested_assigned_uaf_8811,
           issue_repro_nested_expr_lambda_assigns_string_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var s := \"ab\"; var f2 := lambda() => lambda() => do s := s + \"c\"; return s.Length; end;\n"
        "    var r := f2()(); return r + s.Length;\n")),
        "1200\n");
}

// The local starts as a heap string, so the frame's reference is a real +1.
BEZEL_TEST(closure_captured_cell_nested_assigned_uaf_8811,
           heap_string_initial_value_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var s := t + \"b\"; var f2 := lambda() => lambda() => do s := s + \"c\"; return s.Length; end;\n"
        "    var r := f2()(); return r + s.Length;\n")),
        "1200\n");
}

// Three expression-bodied levels: the cell passes through two intermediate
// envs before the writing one.
BEZEL_TEST(closure_captured_cell_nested_assigned_uaf_8811,
           three_levels_expression_bodied_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var s := t + \"b\"; var f3 := lambda() => lambda() => lambda() => do s := s + \"c\"; return s.Length; end;\n"
        "    var r := f3()()(); return r + s.Length;\n")),
        "1200\n");
}

// Three levels whose middle one is block-bodied: that body re-scans and sees
// the write, and the expression-bodied lambda inside it lost the verdict.
BEZEL_TEST(closure_captured_cell_nested_assigned_uaf_8811,
           three_levels_block_bodied_middle_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var s := t + \"b\"; var f3 := lambda() => do var g := lambda() => lambda() => do s := s + \"c\"; return s.Length; end; return g; end;\n"
        "    var r := f3()()(); return r + s.Length;\n")),
        "1200\n");
}

// The FRAME writes the cell after a block-bodied lambda's nested capture
// took it; that lambda's own re-scan cannot see a write in the frame. `g` is
// reassigned before the frame's read, so the nested envdtor runs first:
// 3 + 3 per call.
BEZEL_TEST(closure_captured_cell_nested_assigned_uaf_8811,
           frame_writes_after_block_lambda_nested_capture_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var s := t + \"b\"; var f2 := lambda() => do return lambda() => s.Length; end;\n"
        "    var g := f2(); s := s + \"c\"; var r := g(); g := lambda() => 0; return r + s.Length;\n")),
        "1200\n");
}

// A FUNCTION-typed local reassigned through the nested lambda (the verdict's
// other runtime-managed kind): 2 + 2 per call.
BEZEL_TEST(closure_captured_cell_nested_assigned_uaf_8811,
           closure_local_reassigned_through_nested_lambda_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var f := lambda() => 1; var f2 := lambda() => lambda() => do f := lambda() => 2; return f(); end;\n"
        "    var r := f2()(); return r + f();\n")),
        "800\n");
}

// The captured variable is a `string` PARAMETER (the parameter capture path
// consults the same verdict): "abc" twice per call.
BEZEL_TEST(closure_captured_cell_nested_assigned_uaf_8811,
           string_parameter_assigned_through_nested_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var f2 := lambda() => lambda() => do t := t + \"bc\"; return t.Length; end;\n"
        "    var r := f2()(); return r + t.Length;\n")),
        "1200\n");
}
