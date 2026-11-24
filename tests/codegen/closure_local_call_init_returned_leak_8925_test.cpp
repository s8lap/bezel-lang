// Codegen — issue #8925: a closure local initialised from a CLOSURE-RETURNING
// CALL leaked its cell when the method RETURNED that local. The
// PRODUCER-initialised twin (`var f: () -> int := lambda() => 3; return f;`)
// was clean.
//
// THE RULE. §28.9 (v0.359): a function-typed value is runtime-managed — "the
// type's lifetime is managed by the runtime without user involvement" — and
// runtime management governs "the heap-allocated closure object that backs
// it", so that object is reclaimed exactly once. §27.5 (v0.359): "Captured
// variables are heap-allocated in a closure object" — the closure object is a
// heap object, and a lambda's body mints it whoever the caller is.
//
// ROOT CAUSE. `collectClosureReleaseEligible` treats `return f;` as an ESCAPE,
// so no scope-end release is armed for `f` — the RETURN transfers the slot's
// single reference to the caller. `returnedClosureLocalOwnsPlusOne`
// (src/codegen/LlvmBackend_Setup.cpp ~L38574) is the predicate that decides
// "the slot's +1 is transferred by the return". Its PRODUCER arm read
//
//     if (dv.init && !dv.isUsing && isPlusOneClosureProducer(dv.init))
//
// and `isPlusOneClosureProducer` DELIBERATELY declines every `CallExpr` — so
// for a call-initialised local the predicate answered "does not own", and the
// return site MINTED a second +1 (the retain at src/codegen/
// LlvmBackend_Stmts.cpp ~L4794) while the slot's own +1 was never dropped:
// the caller dropped the minted one and the slot's leaked.
//
// The candidate walk that DECIDES the local is a producer-initialised closure
// local has read, since fix #2714,
//
//     isPlusOneClosureProducer || isUserMethodClosureReturnCall ||
//     isLambdaClosureReturnCall || isAwaitedClosureReturnCall
//
// so the slot demonstrably holds the CALL's single +1 (the callee's own body
// minted it and transferred it — #2714's reasoning). The two disagreed: the
// walk admitted the producer the predicate did not recognise.
//
// MEASURED on the base (r43-f45-toolchain @ 86ced0bec,
// `bezelc --sanitize=address`):
//   var f: () -> int := g();  return f;
//     -> "SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s)."
//   var f: () -> int := lambda() => 3;  return f;
//     -> the PRODUCER-initialised twin: CLEAN.
//
// Every program runs its body 200 times under runAotChecked (generated-code
// AddressSanitizer + LeakSanitizer).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// The class under test. `members` is the body of the class (its extra
// methods); `mainCall` is the expression `Main` calls once per iteration.
std::string program(const std::string& members, const std::string& mainCall) {
    return R"BZL(
module M;
public class P
)BZL" + members + R"BZL(
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + )BZL" + mainCall + R"BZL(; end;
    Console.WriteLine(total);
    return 0;
  end Main;
end P;
)BZL";
}

}  // namespace

// THE REGRESSION ROW: the issue's repro, verbatim, in a 200× loop. `Mk` binds
// the call's result to a closure local and RETURNS that local, so the return
// must transfer the slot's single +1 instead of minting a second one.
//
// RED on the base: 64 B leaked per call (200 calls ⇒ 12800 B in 200
// allocations). GREEN with the fix: prints 600, exits 0, leaks nothing.
BEZEL_TEST(closure_local_call_init_returned_leak_8925,
           lambda_callee_call_init_returned_spec_28_9) {
    expectClean(runAotChecked(program(
        "  public static method Mk(): () -> int\n"
        "    var g: () -> () -> int := lambda(): () -> int => lambda() => 3;\n"
        "    var f: () -> int := g();\n"
        "    return f;\n"
        "  end Mk;\n",
        "P.Mk()()")),
        0, "600\n");
}

// The USER-METHOD-callee twin: the closure-returning call's callee is a
// user-defined method (`isUserMethodClosureReturnCall`) rather than a
// lambda-typed local (`isLambdaClosureReturnCall`). Both are producers the
// candidate walk admits, so both must be recognised as owners.
//
// RED on the base for the same reason. GREEN with the fix.
BEZEL_TEST(closure_local_call_init_returned_leak_8925,
           user_method_callee_call_init_returned_spec_28_9) {
    expectClean(runAotChecked(program(
        "  public static method Mk2(): () -> int\n"
        "    var h: () -> int := lambda() => 3;\n"
        "    return h;\n"
        "  end Mk2;\n"
        "  public static method Run(): () -> int\n"
        "    var f: () -> int := P.Mk2();\n"
        "    return f;\n"
        "  end Run;\n",
        "P.Run()()")),
        0, "600\n");
}

// ── the two BASE-GREEN boundary controls (owner, 2026-11-12: at most two per
// suite, each named in the commit body with the wrong fix it catches) ──
//
// CONTROL 1 — the PRODUCER-initialised twin. The local's initialiser is a
// lambda LITERAL, the producer the producer arm already recognised. It was
// clean on the base. It catches the plausible wrong fix "REPLACE the
// producer arm's `isPlusOneClosureProducer` test with the call predicates"
// (the literal's own +1 would then not be recognised as owned, the return
// would mint a second one, and the literal's cell would leak).
BEZEL_TEST(closure_local_call_init_returned_leak_8925,
           producer_init_returned_spec_28_9) {
    expectClean(runAotChecked(program(
        "  public static method Run(): () -> int\n"
        "    var f: () -> int := lambda() => 3;\n"
        "    return f;\n"
        "  end Run;\n",
        "P.Run()()")),
        0, "600\n");
}

// CONTROL 2 — the NOT-RETURNED spelling. The same call-initialised local, but
// the method CALLS it instead of returning it, so the local does not escape
// and its scope-end release still fires. It was clean on the base. It catches
// the plausible wrong fix "the producer arm's widening turned the
// call-initialised local into a TRANSFER owner and dropped its scope-end
// release" (the call's +1 would then never be dropped at all).
BEZEL_TEST(closure_local_call_init_returned_leak_8925,
           call_init_not_returned_spec_28_9) {
    expectClean(runAotChecked(program(
        "  public static method Run(): int\n"
        "    var g: () -> () -> int := lambda(): () -> int => lambda() => 3;\n"
        "    var f: () -> int := g();\n"
        "    return f();\n"
        "  end Run;\n",
        "P.Run()")),
        0, "600\n");
}
