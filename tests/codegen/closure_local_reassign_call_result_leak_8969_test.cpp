// Codegen — issue #8969: a closure-returning CALL's result ASSIGNED to an
// EXISTING function-typed local (`f := make();`, with `f` declared earlier)
// leaked the closure cell the call minted — 64 bytes per call, forever. The
// DECLARATION twin (`var f := make();`) is clean.
//
// THE RULE. §28.9 (v0.359): a function-typed value is runtime-managed — "the
// type's lifetime is managed by the runtime without user involvement" — and
// runtime management governs "the heap-allocated closure object that backs
// it", so that object is reclaimed exactly once. §27.5 (v0.359): "Captured
// variables are heap-allocated in a closure object" — the closure object is a
// heap object, and a lambda's body mints it whoever the caller is.
//
// ROOT CAUSE. `Emitter::lowerClosureLocalReassign` (src/codegen/
// LlvmBackend_Setup.cpp, fix #4174) decides retain-vs-transfer for the slot's
// new value with
//
//     isPlusOneClosureProducer(v) || isUserMethodClosureReturnCall(v)
//
// and MISSED `isLambdaClosureReturnCall(v)` — the predicate (fix #2714,
// widened by #8870 / #8893) that recognises a closure-returning call whose
// callee is a lambda-typed LOCAL / PARAMETER or an immediately-invoked lambda
// literal — and `isAwaitedClosureReturnCall(v)`, the awaited sibling. Such a
// call mints exactly one +1 and the CALLER owns it, so the assignment path
// RETAINED it a second time (rc 1 -> 2) while the slot's single release
// dropped only one reference: 64 B stranded per call.
//
// The DECLARATION path has admitted those producers since #2714 — the
// producer walk in `collectClosureReleaseEligible` reads
// `isPlusOneClosureProducer || isUserMethodClosureReturnCall ||
// isLambdaClosureReturnCall || isAwaitedClosureReturnCall` — which is why
// only the REASSIGNMENT spelling leaked.
//
// MEASURED on the base (r30-c1 @ f2433724b, `bezelc --sanitize=address`):
//   var G := lambda(): () -> int => lambda() => 3;
//   var f := lambda() => 1;
//   f := G();
//     -> "SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200
//        allocation(s)."        (64 B per call)
//   var f := G();                 <- the DECLARATION twin: CLEAN.
//
// Every program runs its body 200 times under runAotChecked (generated-code
// AddressSanitizer + LeakSanitizer).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string runCleanOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// A class with the `Run` method the body under test lives in.
//
//   runSig  — `Run`'s PARAMETER list, without the parentheses: "" or
//             "p: () -> () -> int".
//   callArg — `Main`'s argument list for the `C.Run` call, without the
//             parentheses: "" or "lambda(): () -> int => lambda() => 3".
//   runBody — the body of `Run`.
std::string program(const std::string& runSig, const std::string& callArg,
                    const std::string& runBody) {
    return R"BZL(
module M;
public class C
  public static method Run()BZL" + runSig + R"BZL(): int
)BZL" + runBody + R"BZL(
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run()BZL" + callArg + R"BZL(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// The issue's repro, as filed (#8969): the call's callee is a lambda-typed
// LOCAL. 3 per call.
BEZEL_TEST(closure_local_reassign_call_result_leak_8969,
           named_lambda_callee_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "", "",
        "    var G := lambda(): () -> int => lambda() => 3;\n"
        "    var f := lambda() => 1;\n"
        "    f := G();\n"
        "    return f();\n")),
        "600\n");
}

// The IMMEDIATELY-INVOKED twin (#8870 / #8893's spelling): the same call with
// a lambda LITERAL for a callee. 3 per call.
BEZEL_TEST(closure_local_reassign_call_result_leak_8969,
           iife_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "", "",
        "    var f := lambda() => 1;\n"
        "    f := (lambda(): () -> int => lambda() => 3)();\n"
        "    return f();\n")),
        "600\n");
}

// The PARAMETER-callee twin: the callee is a function-typed PARAMETER, the
// third shape `isLambdaClosureReturnCall` admits. 3 per call.
BEZEL_TEST(closure_local_reassign_call_result_leak_8969,
           parameter_callee_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "p: () -> () -> int", "lambda(): () -> int => lambda() => 3",
        "    var f := lambda() => 1;\n"
        "    f := p();\n"
        "    return f();\n")),
        "600\n");
}

// The AWAITED sibling (#3927's predicate, the fourth arm the declaration
// path's producer walk admits): the awaited task completes with a +1
// reference to the closure cell, and the assignment must transfer it. 3 per
// call.
BEZEL_TEST(closure_local_reassign_call_result_leak_8969,
           awaited_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(R"BZL(
module M;
public class C
  public static async method MakeAsync(): Task<() -> int>
    return lambda() => 3;
  end MakeAsync;
  public static async method Run(): Task<int>
    var f := lambda() => 1;
    f := await C.MakeAsync();
    return f();
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0..<200 do total := total + await C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
        "600\n");
}

// The REASSIGNMENT-IN-A-LOOP shape: the slot is overwritten 200 times, so the
// leak is one closure cell per REASSIGNMENT (64 B), not one per call.
BEZEL_TEST(closure_local_reassign_call_result_leak_8969,
           reassign_in_loop_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "", "",
        "    var G := lambda(): () -> int => lambda() => 3;\n"
        "    var f := lambda() => 1;\n"
        "    for var k in 0..<200 do f := G(); end;\n"
        "    return f();\n")),
        "600\n");
}

// ── the two BASE-GREEN boundary controls (owner, 2026-11-12: at most two per
// suite, each named in the commit body with the wrong fix it catches) ──
//
// CONTROL 1 — the BORROW twin. The assigned value is another LOCAL's slot, so
// it carries NO +1 of its own: the receiving slot must RETAIN it, or the
// source's scope-end release frees the cell the target still holds (a double
// free). Catches the plausible wrong fix "always transfer the +1, never
// retain" (dropping `emitClosureRetain` for the borrow case).
BEZEL_TEST(closure_local_reassign_call_result_leak_8969,
           borrow_from_other_local_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "", "",
        "    var g := lambda() => 3;\n"
        "    var f := lambda() => 1;\n"
        "    f := g;\n"
        "    return f();\n")),
        "600\n");
}

// CONTROL 2 — the LAMBDA-LITERAL twin. The assigned value is a fresh +1
// producer, so the slot must TRANSFER it (no retain) or the second reference
// is stranded (64 B per call). Catches the plausible wrong fix "always
// retain the new value" (the pre-#4422 always-retain discipline).
BEZEL_TEST(closure_local_reassign_call_result_leak_8969,
           lambda_literal_producer_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "", "",
        "    var f := lambda() => 1;\n"
        "    f := lambda() => 2;\n"
        "    return f();\n")),
        "400\n");
}
