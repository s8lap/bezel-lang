// Regression tests for issue #8785 — §28.3 closure bullet *Provenance*
// clause 1 and *Set-erasing positions* (v0.359).
//
// THE DEFECT.  `exprIsFunctionValueWithNonEmptyCaptureSet`
// (src/ownership/Ownership_Checks.cpp) decides, for a §28.3 set-erasing
// position, whether the function value reaching it has a NON-EMPTY source
// set.  It counted EVERY capture of a lambda, so a lambda capturing only a
// VALUE-TYPE local (`var n := 5;`) was treated as capture-bearing and
// REJECTED at every set-erasing position: an `Object` argument, a bare type
// parameter substituted with a function type, and — since #7259 — the
// receiver of an extension call whose head is `Object`.
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Provenance*, clause 1:
//
//   "A **lambda literal** contributes the sources of the borrows it captures
//    ... a captured local or parameter of a value type that holds borrows
//    ([§3.6]) the sources of the borrows it carries, ...; **a captured value
//    that carries no borrow — a value type that holds none, a null-source
//    value, a `string` — contributes nothing**, and lives in its heap capture
//    cell)"
//
// and §28.3 (v0.359), *Set-erasing positions*:
//
//   "**At every set-erasing position a function value with a non-empty source
//    set is rejected, and a capture-free one is admitted**"
//
// A value-type capture carries no borrow, so it contributes nothing to the
// source set: the set stays EMPTY, the closure is capture-free, and the
// position must ADMIT it.  The predicate now counts a capture only when it
// contributes a source (the new `captureContributesSource`).
//
// THE ISSUE'S OWN REPRO (issue #8785; the review lane r42-rev-m63's probe
// `/var/tmp/bezel-scratch/r42-rev-m63/probes/b_value_capture.bzl`):
//
//   var n := 5;
//   var f: () -> int := lambda() => n;
//   Console.WriteLine(f.Hold());          // `Hold` is `extension Object`
//
// MEASURED on the base tree (r30-c1 @ 80439bb9d): that program is REJECTED
// at compile time.  With the fix it compiles, prints 1 and leaks nothing.
//
// The rows below are RED on the base tree (the programs do not compile there)
// and GREEN with the fix.  The must-still-reject shapes (a class-typed
// capture, a borrow-bearing record capture, a function-typed capture) are
// base-GREEN controls and therefore live in the conformance corpus, not here
// — see this lane's `corpus-candidates/`.
//
// ORACLE.  `runAot` for the verdict rows; `runAotChecked` (the checked AOT
// path with the AddressSanitizer pass over the GENERATED module and
// LeakSanitizer at exit) for the leak row, which LOOPS 200 iterations so a
// leak cannot hide under the sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes.  LSan
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

// The issue's own repro, verbatim (issue #8785; the review lane r42-rev-m63's
// probe `b_value_capture.bzl`): a lambda capturing only the value-type local
// `n`, used as the RECEIVER of an extension call whose head is `Object`
// (§28.3 *Set-erasing positions*, clause 4).
const char* kIssueRepro =
    "module M; "
    "import System; "
    "class Res "
    "  public constructor() end constructor; "
    "  public method Use(): int return 3; end Use; "
    "  public method Add(): int return 4; end Add; "
    "  public static method Stat(): int return 5; end Stat; "
    "end Res; "
    "extension Object "
    "  public method Hold(): int => 1; "
    "end; "
    "public class Program "
    "  public static method Main(): int "
    "    var n := 5; "
    "    var f: () -> int := lambda() => n; "
    "    Console.WriteLine(f.Hold()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

// ACCEPT: the issue's own repro — a value-type capture contributes nothing to
// the source set (§28.3 *Provenance* clause 1), so the closure is capture-free
// and the extension-Object receiver — a set-erasing position (§28.3 clause 4)
// — admits it.  RED on the base tree: the program was REJECTED there.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_at_extension_object_receiver_accepted) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ACCEPT: the same capture at an `Object` ARGUMENT — the other set-erasing
// position (§28.3 *Set-erasing positions*: "a position declared `Object`").
// RED on the base tree.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_at_object_argument_accepted) {
    auto r = runAot(
        std::string("module M; "
                    "import System; "
                    "public class Program "
                    "  public static method Take(o: Object) end Take; "
                    "  public static method Main(): int "
                    "    var n := 5; "
                    "    var f: () -> int := lambda() => n; "
                    "    Take(f); "
                    "    Console.WriteLine(\"ok\"); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// ACCEPT: the same capture at a BARE TYPE PARAMETER substituted with a
// function type — the third set-erasing position (§28.3 *Set-erasing
// positions*: "a bare type parameter ... at an instantiation substituting a
// function type or `Object`").  RED on the base tree.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_at_bare_type_parameter_accepted) {
    auto r = runAot(
        std::string("module M; "
                    "import System; "
                    "public class Util "
                    "  public static method Take<T>(x: T) "
                    "    Console.WriteLine(\"took\"); "
                    "  end Take; "
                    "end Util; "
                    "public class Program "
                    "  public static method Main(): int "
                    "    var n := 5; "
                    "    var f: () -> int := lambda() => n; "
                    "    Util.Take(f); "
                    "    Console.WriteLine(\"ok\"); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "took\nok\n");
}

// The newly admitted program must not leak: the closure's capture cell holds
// the value-type local, and the cell is reclaimed when the closure dies.  The
// program LOOPS 200 times (the issue's shape, in a loop) so a per-iteration
// leak cannot hide under the sanitizer's noise.  RED on the base tree: the
// program does not compile there.
BEZEL_TEST(set_erasing_value_type_capture_8785,
           value_type_capture_is_leak_free_over_200_iterations) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "public class Program "
                    "  public static method Take(o: Object) end Take; "
                    "  public static method Main(): int "
                    "    var total := 0; "
                    "    for var i in 0..<200 do "
                    "      var n := i; "
                    "      var f: () -> int := lambda() => n; "
                    "      Take(f); "
                    "      total := total + f(); "
                    "    end; "
                    "    Console.WriteLine(total); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "19900\n");
    assertNoLeak(r);
}
