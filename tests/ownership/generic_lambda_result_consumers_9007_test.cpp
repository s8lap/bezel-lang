// Regression test for issue #9007 — after fix #8946 a generic method body that
// CALLS a `T`-returning lambda it just made leaked the +1 result the lambda now
// mints, at T = string (one string per call). The non-generic twin was clean.
// (The T = function-type half of the report is not pinned here: the measured
// residual is the caller's own closure local passed to a bare-`T` parameter,
// which leaks identically on the pre-#8946 base, #5129 family.)
//
// THE DEFECT. #8946 made `lowerLambda` mint +1 for a lambda whose result
// substitutes to `string` / a function type (so the caller's release of an
// escaping result balances). A generic body's OWN consumers of that result
// (`var r := f();`, `f();`, `Sink<T>(f())`, `var q := r;`) were still
// classified from sema's bare `T`, so they took the minted reference for a
// borrow and never released it.
//
// THE ROOT CAUSES (§14.1: a generic instantiation owes what its hand-written
// non-generic body owes):
//  1. isPlusOneStringProducer's closure-call arm read the call's type, the
//     bare `T`, so `f()` was not a string producer at T = string;
//  2. isLambdaClosureReturnCall read the callee's `funcReturn`, the bare `T`,
//     so `f()` was not a closure producer at T = a function type (the same
//     substitution, no row: see above);
//  3. collectStringReleaseEligible's borrow arm typed `var q := r;` by the
//     bare `T`, so the COPY of a tracked producer local was unclassified, the
//     escape walk read it as an escape of `r`, and `r`'s +1 was never released.
//
// THE SPEC. §27.5 / §28.9 (v0.359): a closure and a `string` are released
// exactly once per reference; §14.1: monomorphized body, same obligations.
//
// ORACLE. `runAotChecked` (ASan over the generated module + LeakSanitizer);
// every row loops 200 times inside the program. Rows that are clean on the
// base too (the #8946 rows, in generic_lambda_captures_t_param_8946_test.cpp)
// are not repeated here.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing. LSan prints a SUMMARY line only when it finds
// a leak, so "no summary" is 0 leaked allocations.
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

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// `Use<T>(p: T): int` with the given body, and a `Run` that executes `call`
// 200 times.
std::string program(const std::string& useBody, const std::string& call) {
    return "module P; public class Program "
           "  public static method Sink<U>(u: U): int return 1; end Sink; "
           "  public static method Use<T>(p: T): int " + useBody +
           "  end Use; "
           "  public static method Run(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do " + call + " end; "
           "    return t; "
           "  end Run; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.Run()); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

const char* const kStringCall =
    "var s := \"hi\" + \"x\"; t := t + Program.Use<string>(s); ";

// Issue repro shape: the lambda's result is BOUND to a local (`var r := f();`).
// RED on r30-c1: 7200 B / 400 allocations (200 strings + 200 string headers).
BEZEL_TEST(generic_lambda_result_consumers_9007,
           string_result_bound_to_a_local) {
    const std::string src = program(
        "var f := lambda() => p; var r := f(); return 1; ", kStringCall);
    expectClean(runAotChecked(src), 0, "200\n");
}

// The result is DISCARDED (`f();`). RED on r30-c1.
BEZEL_TEST(generic_lambda_result_consumers_9007,
           string_result_discarded_by_an_expression_statement) {
    const std::string src = program(
        "var f := lambda() => p; f(); return 1; ", kStringCall);
    expectClean(runAotChecked(src), 0, "200\n");
}

// The result is COPIED to a second local (`var q := r;`): the copy must not read
// as an escape of the producer local. RED on r30-c1.
BEZEL_TEST(generic_lambda_result_consumers_9007,
           string_result_copied_to_a_second_local) {
    const std::string src = program(
        "var f := lambda() => p; var r := f(); var q := r; return 1; ",
        kStringCall);
    expectClean(runAotChecked(src), 0, "200\n");
}

// The result is passed as an ARGUMENT to a generic callee (`Sink<T>(f())`).
// RED on r30-c1.
BEZEL_TEST(generic_lambda_result_consumers_9007,
           string_result_passed_as_an_argument) {
    const std::string src = program(
        "var f := lambda() => p; Program.Sink<T>(f()); return 1; ", kStringCall);
    expectClean(runAotChecked(src), 0, "200\n");
}

}  // namespace
