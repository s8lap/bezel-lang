// Codegen — issue #8834: an IMMEDIATELY-INVOKED capturing lambda
// (`(lambda() => s.Length)()`) freed the captured local in its envdtor while
// the frame still owned it, so the frame's later read of `s` was a
// heap-use-after-free.
//
// THE RULE. §27.5 (v0.359): "Captured variables are heap-allocated in a
// closure object" — the binding stays the frame's after the closure dies, so
// `return r + s.Length;` reads a live `s`. §28.9 (v0.359): `string` and
// function-typed values are runtime-managed, so every reference is reclaimed
// exactly once, and not while a binding still holds it.
//
// ROOT CAUSE. The first capture of a local transfers the local's deferred
// scope-end release into the capturing env's envdtor, which is only sound when
// the env dies at or after the local's scope end. #3752 (a capture in a deeper
// scope) and #3778 (a lambda literal passed as a call ARGUMENT, signalled by
// `inArgumentLambda_`) move that release to the local's owner scope instead.
// An IIFE's closure is the same statement-bounded temporary — lowerCall's IIFE
// arm releases it right after the call — and the pre-scan already admits its
// captures into `cellFreeEligible_` (#2182), but the IIFE arm never armed
// `inArgumentLambda_`, so the CALLEE position fell through both gates and the
// envdtor freed the frame's only reference.
//
// THE FIX arms `inArgumentLambda_` around the IIFE arm's `lowerLambda`, as
// #3778 does for an argument and #3890 for the GetOrAdd factory.
//
// Every program runs its body 200 times under runAotChecked (generated-code
// AddressSanitizer + LeakSanitizer). MEASURED on r30-c1 @ 4fa29b1d9: every row
// below is a heap-use-after-free (freed by `bz$lam$N$envdtor`) with no output.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and neither
// AddressSanitizer nor LeakSanitizer fired; a marker otherwise.
std::string runCleanOut(const std::string& prog) {
    auto r = runAotChecked(prog);
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

// #8834's repro, as filed: the IIFE writes the capture. 3 + 3 per call.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           issue_repro_iife_writes_captured_string_spec_27_5) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda() => do s := s + \"c\"; return s.Length; end)(); return r + s.Length;\n")),
        "1200\n");
}

// The issue's read-only form: no write is needed. 2 + 2 per call.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           read_only_iife_capture_spec_27_5) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda() => s.Length)(); return r + s.Length;\n")),
        "800\n");
}

// A block-bodied IIFE that only reads. 3 + 2 per call.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           block_body_iife_capture_spec_27_5) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda() => do var k := s.Length; return k + 1; end)(); return r + s.Length;\n")),
        "1000\n");
}

// An IIFE nested in an IIFE: the outer one holds the first capture. 2 + 2.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           iife_nested_in_iife_spec_27_5) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda() => (lambda() => s.Length)())(); return r + s.Length;\n")),
        "800\n");
}

// The IIFE's result assigned to an existing local, twice. 2 + 3 + 2.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           iife_result_assigned_to_existing_local_spec_27_5) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := 0; r := (lambda() => s.Length)(); r := r + (lambda() => s.Length + 1)(); return r + s.Length;\n")),
        "1400\n");
}

// The function-typed half of the capture model: a closure local called after
// the IIFE that captured it. 2 + 2 per call.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           iife_captures_closure_local_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var f := lambda(): int => 2; var r := (lambda() => f())(); return r + f();\n")),
        "800\n");
}

// The first capture is an IIFE, a named lambda captures the same local after
// it. 2 + 2 + 2 per call.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           iife_first_capture_then_named_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda() => s.Length)(); var h := lambda() => s.Length; return r + h() + s.Length;\n")),
        "1200\n");
}

// An IIFE with a parameter, passed the captured local as its argument too.
// 2 + 2 + 2 per call.
BEZEL_TEST(iife_capture_envdtor_content_release_uaf_8834,
           iife_with_argument_and_capture_spec_27_5) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda(u: string) => u.Length + s.Length)(s); return r + s.Length;\n")),
        "1200\n");
}
