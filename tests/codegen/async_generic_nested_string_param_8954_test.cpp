// fix #8954 (§30.1 / §28.9, v0.359): an async method with a `string`
// parameter SEGVs in `bz_string_release` when its body calls a GENERIC async
// method (monomorphized inline).
//
// THE DEFECT. §30.1 (v0.359) holds a borrowed string parameter "until the
// task SETTLES", so the async method's frame takes its own +1 on the string
// (§28.9) and drops it at the task's terminal edge. The frame publishes that
// hold in `asyncStringPinFrame_` / `asyncStringPinFields_` for the WHOLE of
// its body emission.
//
// `emitAsyncMethod` is RE-ENTERED by a nested emission: a generic async
// method monomorphized inside this body (`buildSpecializedMethodFn`). The
// CLOSURE twin of this publication already had the save/restore that scopes
// it to the frame that took the hold (#8867); the STRING twin did not — it
// was CLEARED at the end of `emitAsyncMethod`. So in the nested method:
//
//   - `asyncStringPinFields_` still holds the ENCLOSING method's field
//     indices (the nested method only APPENDS its own), and
//   - `asyncStringPinFrame_` is then set to the NESTED method's frame
//     (`asyncStringPinFrame_ = asyncStringPinFields_.empty() ? nullptr :
//     &frame;`).
//
// The nested method's own completion then calls
// `emitAsyncStringParamRelease(self)`, which GEPs the ENCLOSING method's
// field index through `smStateStruct_` — the NESTED method's struct type —
// and releases that word of the nested frame as a `string`: a wild pointer,
// dereferenced by the runtime.
//
// MEASURED on the base (`cabfcbbfc`, the staging tip): the repro below
// prints NOTHING and then
//   "ERROR: AddressSanitizer: SEGV ... in bz_string_release
//    (build/../src/runtime/bezel_runtime_refcounting.cpp:148:9)"
// with the crashing frame `bz$async$M.C$Inner$Resume.2` — the NESTED
// method's resume, exactly as the mechanism above predicts.
//
// The same root cause has a SECOND manifestation, which this file's second
// row pins: when the monomorphized callee is itself instantiated at
// `T = string` it has a string parameter of its own, so it PUBLISHES a
// second hold; the nested method's terminal edge then releases the
// ENCLOSING frame's index against the NESTED frame's layout AND the
// enclosing frame's own terminal edge drops NOTHING (the plain clear at the
// end of `emitAsyncMethod` took the enclosing publication away) — so the
// borrowed `string`'s +1 LEAKS. MEASURED on the base: the program prints the
// right number and then LeakSanitizer reports "7490 byte(s) leaked in 400
// allocation(s)".
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. The rows assert
// the program's own output and exit code as well as the leak count, so a
// silent zero-fill cannot pass. The program LOOPS (200 iterations), so
// neither the crash nor a leak can hide under the sanitizer's noise.
//
// FAIL-WITHOUT / PASS-WITH: RED on the base (row 1: the SEGV; row 2: the
// leak), GREEN with the fix.
//
// NOTE ON THE EXPECTED OUTPUT. The issue's text says the repro prints
// "1290". MEASURED, it prints 1090: the awaited generic method returns 1, so
// each iteration contributes 1 + len("ab" + k), and the sum of the lengths of
// "ab0" … "ab199" is 30 + 360 + 500 = 890, giving 200 + 890 = 1090. The
// "1290" is the value of #8930's repro, whose awaited lambda returns
// n + 1 = 2 (400 + 890 = 1290). The NON-generic twin of this repro — the
// same program with a plain (non-generic) async callee, which is CLEAN on
// the base — prints 1090 too, which is the independent confirmation.

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
// a `string` parameter whose body calls a GENERIC async method.
//
// The expected output is 1090: 200 × 1 (the awaited generic method's
// `return 1`) + the lengths of "ab0" … "ab199" (30 + 360 + 500 = 890).
//
// RED on the base: the program prints NOTHING and AddressSanitizer reports a
// SEGV in `bz_string_release` (the crashing frame is the nested method's
// `Inner$Resume`). GREEN with the fix: it prints 1090, exits 0, and leaks
// nothing.
BEZEL_TEST(async_generic_nested_string_param_8954,
           async_string_param_with_nested_generic_async_method_does_not_segv_spec_30_1) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Inner<T>(x: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Inner; "
        "  public static async method Outer(s: string): Task<int> "
        "    var y := await Inner<int>(3); "
        "    await Task.Delay(1); "
        "    return y + s.Length; "
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
    expectClean(r, 0, "1090\n");
}

// THE SAME ROOT CAUSE, SECOND MANIFESTATION: the monomorphized callee is
// itself instantiated at `T = string`, so it has a string parameter of its
// own and publishes a SECOND hold in the same two members.
//
// RED on the base: the program prints the right number and then
// LeakSanitizer reports "7490 byte(s) leaked in 400 allocation(s)" — the
// enclosing frame's borrowed `string` hold is never dropped, because the
// plain clear at the end of the nested `emitAsyncMethod` took the enclosing
// publication away before the enclosing frame's terminal edge ran. GREEN
// with the fix: it prints 1090, exits 0, and leaks nothing.
BEZEL_TEST(async_generic_nested_string_param_8954,
           async_generic_callee_at_t_string_does_not_leak_spec_30_1) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Inner<T>(x: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Inner; "
        "  public static async method Outer(s: string): Task<int> "
        "    var y := await Inner<string>(\"q\"); "
        "    await Task.Delay(1); "
        "    return y + s.Length; "
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
    expectClean(r, 0, "1090\n");
}

}  // namespace
