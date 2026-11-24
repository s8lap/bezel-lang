// Regression test for issue #8970 — a CLOSURE's own `owned` parameter was
// never discharged when the closure body did not consume it, so the value the
// caller transferred INTO the closure LEAKED (the METHOD twin auto-drops it
// at the end of the method).
//
// THE DEFECT. §28.3.10 *Owned-local discharge requirement* (spec v0.359):
// "Undischarged `owned` parameters' implicit disposals are established at
// entry to the body, in declaration order — so, undischarged, they are
// disposed in reverse declaration order, for a method, a constructor, a
// LAMBDA and an uncaptured primary-constructor parameter alike". The METHOD
// half was implemented (the ownership pass's end-of-method sweep flags an
// `owned` parameter the body never discharges; codegen's
// `registerUnwindOwnedParam` turns that flag into the scope-end free). The
// LAMBDA half was missing: the ownership pass never runs its dataflow over a
// lambda body (the cross-cutting fix #2674 records as deferred), so nothing
// flagged a closure's own `owned` parameter, and the closure ended with the
// value still owned and never disposed.
//
// MEASURED on r43-f45-toolchain with `bezelc --sanitize=address`, the issue's
// literal repro:
//
//   1
//   =================================================================
//   ==1948259==ERROR: LeakSanitizer: detected memory leaks
//   Direct leak of 96 byte(s) in 1 object(s) allocated from:
//       #1 ... in bz_array_alloc build/../src/runtime/bezel_runtime_collections.cpp:493:30
//   Indirect leak of 64 byte(s) in 1 object(s) allocated from:
//       #1 ... in bz_array_alloc build/../src/runtime/bezel_runtime_collections.cpp:498:21
//   SUMMARY: AddressSanitizer: 160 byte(s) leaked in 2 allocation(s).
//
// The METHOD twin of the same program (`method Take(owned ys: Array<int>)`
// whose body does not consume `ys`) is ASan-clean and carries the §28.3
// auto-drop warning "owned parameter `ys` is implicitly disposed at the end
// of the method".
//
// THE FIX. The ownership pass now flags a CLOSURE's own `owned` parameter
// that the closure body never discharges (the same
// `ParameterSymbol::autoDropAtScopeEnd` flag the method path sets), and
// codegen registers the closure's own parameter in the CLOSURE BODY's root
// scope — so the scope-end free fires on every exit path of that body: the
// normal fall-through, an early `return` (which disposes every live scope),
// and the exception edge.
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

// THE REGRESSION ROW: the issue's literal repro, amplified 200x (the issue's
// own measurement). The closure's `owned` parameter is an `Array<int>` the
// closure body never consumes; the value the caller transfers into the
// closure must be reclaimed when the closure body ends.
//
// RED on the base (160 B / 2 allocations per call — 32 000 B over the loop):
// the transferred value is stranded. GREEN with the fix.
BEZEL_TEST(closure_owned_param_undischarged_8970,
           issue_repro_200_iterations_reclaims_the_transferred_value) {
    const std::string src =
        "module Conf; import System; "
        "public class C "
        "  public method Go(): int "
        "    var f := lambda(owned ys: Array<int>): int => do "
        "      return 1; "
        "    end; "
        "    var a := new Array<int>(); "
        "    return f(move a); "
        "  end Go; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      using var c := new C(); "
        "      t := t + c.Go(); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// THE DESTRUCTOR-COUNT row: the same shape over a USER CLASS, so the oracle
// is the program's OWN output — the destructor must run exactly once per
// transferred value (200 times), not zero times. This is the sanitizer-free
// twin of the row above: a leak-free program whose destructor never ran
// would still be wrong, and a program that disposed the value twice would
// trip the sanitizer leg.
//
// RED on the base: the destructor never runs (the program prints "0").
// GREEN with the fix (the program prints "200").
BEZEL_TEST(closure_owned_param_undischarged_8970,
           user_class_twin_disposes_the_transferred_value_once_per_call) {
    const std::string src =
        "module Conf; import System; "
        "public class Res "
        "  public constructor() end constructor; "
        "  destructor "
        "    Console.WriteLine(\"disposed\"); "
        "  end destructor; "
        "end Res; "
        "public class C "
        "  public method Go(): int "
        "    var f := lambda(owned ys: Res): int => do "
        "      return 1; "
        "    end; "
        "    return f(new Res()); "
        "  end Go; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      using var c := new C(); "
        "      t := t + c.Go(); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
    // 200 transfers, 200 disposals, then the total.
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "disposed\n";
    expected += "200\n";
    expectClean(runAotChecked(src), 0, expected);
}

}  // namespace
