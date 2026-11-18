// Regression test for issue #8788 — the `string` result of a FORWARDING
// closure call (the callee is the result of ANOTHER call) was never released
// when the result was bound to a local.
//
// THE DEFECT. `var r := f2()();` — where `f2` is a lambda whose body is
// another lambda — leaked the string the INNER lambda produced (32 B direct +
// 4 B indirect, allocated inside `lam$1`), once per call. MEASURED on
// r30-c1 @ 8c1e2ceb2 with `bezelc --sanitize=address`:
//
//   Direct leak of 32 byte(s) in 1 object(s) allocated from:
//       #1 0x00000036bbcb in lam$1 bezel
//   Indirect leak of 4 byte(s) in 1 object(s) allocated from:
//       #1 0x00000036bb18 in lam$1 bezel
//   SUMMARY: AddressSanitizer: 36 byte(s) leaked in 2 allocation(s).
//
// The single-level twins (`var f := lambda() => s + "!"; var r := f();`, a
// function-typed PARAMETER, a method reference) are CLEAN on the base.
//
// THE ROOT CAUSE. `isPlusOneStringProducer`
// (src/codegen/LlvmBackend_Setup.cpp) classifies an expression as a "+1 string
// producer" — one that hands the consumer exactly ONE reference, which the
// consumer must discharge once. Its closure CALL-RETURN arm recognised a call
// whose callee resolves to a LocalSymbol or a ParameterSymbol, so the
// FORWARDING shape (whose callee is a CALL expression and names no slot) fell
// through to the BORROW path: the bound local retained the result ON TOP of
// the callee's +1 and released only its own retain at scope end, so the
// callee's reference was never discharged.
//
// THE FIX. The same arm now also recognises a callee that is itself a CALL —
// the closure such a call produces is a lambda like any other, so its body
// mints +1 for its `string` return (lowerLambda's expression-body path, or
// the block-body ReturnStmt path) exactly as the local/parameter twin does.
// The consumer then TRANSFERS that single reference instead of retaining on
// top of it.
//
// THE SPEC. §28.9 (v0.359): "`string` — pervasive; created by literals,
// interpolation, concatenation, and library methods" is a RUNTIME-MANAGED
// type: "the type's lifetime is managed by the runtime without user
// involvement", and "The compiler does not warn about undisposed
// runtime-managed values and does not require them to appear in `using var` or
// `owned` declarations" — the runtime's own reference is still the runtime's
// to reclaim, exactly once. §27.5 (v0.359): "Captured variables are
// heap-allocated in a closure object"; a forwarding lambda's body is a
// closure like any other, and its `string` result is the fresh production
// §28.9 puts under the runtime's lifetime.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the leak cannot hide under the sanitizer's noise.

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

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// THE REGRESSION ROW: the issue's own repro, verbatim in shape, amplified
// 200x (the issue's own measurement). The captured `s` is read by the inner
// lambda, which produces `s + "!"` — 3 characters, one fresh heap string per
// call.
//
// RED on the base (7200 B / 400 allocations — 200 x (32 B + 4 B)): the
// forwarding call's string result is never discharged. GREEN with the fix.
BEZEL_TEST(closure_forwarding_call_string_result_leak_8788,
           issue_repro_200_iterations_reclaims_the_string_result) {
    const std::string src =
        "module P; public class Program "
        "  public static method Run(): int "
        "    var s := \"hi\"; "
        "    var f2 := lambda() => lambda() => s + \"!\"; "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var r := f2()(); "
        "      t := t + r.Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n");
}

// The same shape, but the callee of the outer call is the result of a METHOD
// call rather than of a lambda literal — the rule is about the SHAPE (the
// callee is the result of another call), not about the spelling.
//
// RED on the base (36 B / 2 allocations per call), GREEN with the fix.
BEZEL_TEST(closure_forwarding_call_string_result_leak_8788,
           callee_is_a_method_call_result_200_iterations_reclaims_it) {
    const std::string src =
        "module P; public class Program "
        "  public static method Make(): () -> string "
        "    var s := \"hi\"; "
        "    return lambda() => s + \"!\"; "
        "  end Make; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var r := Program.Make()(); "
        "      t := t + r.Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n");
}

// The DISCARDED-result sibling of the same root cause: the forwarding call's
// string result is read for its length and never bound to a local, so the
// temporary has no scope-end release of its own. The producer's +1 must still
// be discharged by the consumer. (The DISCARED-result family is tracked
// separately as #8206; this row pins the shape this fix also discharges.)
//
// RED on the base (36 B / 2 allocations per call), GREEN with the fix.
BEZEL_TEST(closure_forwarding_call_string_result_leak_8788,
           discarded_forwarding_result_200_iterations_reclaims_it) {
    const std::string src =
        "module P; public class Program "
        "  public static method Run(): int "
        "    var s := \"hi\"; "
        "    var f2 := lambda() => lambda() => s + \"!\"; "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + f2()().Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n");
}

}  // namespace
