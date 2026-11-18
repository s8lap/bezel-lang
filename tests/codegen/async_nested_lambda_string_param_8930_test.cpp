// fix #8930 (§30.1 / §28.9, v0.359): an async method with a `string`
// parameter SEGVs in `bz_string_release` when its body contains an async
// lambda.
//
// THE DEFECT. §30.1 (v0.359) holds a borrowed string parameter "until the
// task SETTLES", so the async method's frame takes its own +1 on the string
// (§28.9) and drops it at the task's terminal edge. The frame publishes that
// hold in `asyncStringPinFrame_` / `asyncStringPinFields_` for the WHOLE of
// its body emission.
//
// A nested async lambda in that body is emitted inline by `lowerLambda` ->
// `emitAsyncLambda`, which swaps `smStateStruct_` / `smSelf_` to the LAMBDA's
// own (differently laid out) frame. The string-pin publication was not
// suppressed across that emission (unlike `asyncOwnedParamFrame_`, which is
// nulled for exactly this reason), so the LAMBDA's completion ran
// `emitAsyncStringParamRelease(self)` with the LAMBDA's frame pointer and the
// ENCLOSING method's field indices: it read a word of the LAMBDA's own frame
// and released it as a string — a pointer to whatever that word happened to
// be — so the runtime dereferenced garbage: an AddressSanitizer SEGV inside
// `bz_string_release`.
//
// This is the STRING twin of #8867's closure-pin defect (the same publication
// mechanism, the same nested-async-lambda venue, the same fix shape: save,
// NULL and restore the publication across the nested emission, exactly as
// `asyncOwnedParamFrame_` is handled).
//
// MEASURED on the base (the #8867 fix-forward commit, which does not touch
// this publication): the repro below prints NOTHING and then
// "ERROR: AddressSanitizer: SEGV ... in bz_string_release".
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. The row asserts
// the program's own output (1290) and exit code as well as the leak count,
// so a silent zero-fill cannot pass. The program LOOPS (200 iterations), so
// neither the crash nor a leak can hide under the sanitizer's noise.
//
// FAIL-WITHOUT / PASS-WITH: RED on the base (the SEGV), GREEN with the fix.

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

// THE REGRESSION ROW: the issue's own repro, verbatim. An async method with
// a `string` parameter whose body declares an async lambda.
//
// The expected output is 1290: 200 × 2 (the awaited lambda's `n + 1` with
// n = 1) + 10 × 3 + 90 × 4 + 100 × 5 (the lengths of "ab0" … "ab199").
//
// RED on the base: the program prints NOTHING and AddressSanitizer reports a
// SEGV in `bz_string_release`. GREEN with the fix: it prints 1290, exits 0,
// and leaks nothing.
BEZEL_TEST(async_nested_lambda_string_param_8930,
           async_string_param_with_nested_async_lambda_does_not_segv_spec_30_1) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Outer(s: string): Task<int> "
        "    var f := async lambda(n: int): Task<int> => do "
        "      await Task.Delay(1); "
        "      return n + 1; "
        "    end; "
        "    var x := await f(1); "
        "    await Task.Delay(1); "
        "    return x + s.Length; "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var a := 0; "
        "    for var k in 0..<200 do "
        "      a := a + await Outer(\"ab\" + k.ToString()); "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(source);
    expectClean(r, 0, "1290\n");
}

}  // namespace
