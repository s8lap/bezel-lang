// Regression test for issue #9012 — a METHOD-GROUP selector passed to an
// EAGER stdlib §5 LINQ operator (`Array<int>.Sum(P)`) leaked the 64 B closure
// cell it formed, once per call.
//
// THE DEFECT. §27.6 (v0.359) forms a method reference as a function value, and
// §28.9 (v0.359) puts a function value's backing object under the runtime's
// lifetime: "Function-typed values — values of any function type (T1, ..., Tn)
// -> R, regardless of whether they capture. This includes lambda closures
// (capturing or not), method references (`obj.Method` and `Type.StaticMethod`)"
// — and "Runtime management governs the *function value itself* — the
// heap-allocated closure object that backs it".
//
// A method reference forms a FRESH +1 closure cell at the reference site
// (`makeMethodThunk` -> `buildMethodClosureCell` callocs one rc=1 cell, 64 B;
// the instance spelling adds an 8 B env block). stdlib §5 makes the eager
// operators CALL-BOUNDED: "The **eager** operators (First(pred), …, Sum, Min,
// Max, Average, Aggregate, ToArray, ToDictionary, ToSet) are **call-bounded**
// as their unmarked signatures state — they invoke the callback during the call
// and retain nothing." So the CALL SITE solely owns that +1 and must discharge
// it when the call returns.
//
// The eager lowering did discharge it — but only for an INLINE LAMBDA. Its
// producer test (`argIsInlineLambda` in `tryLowerCollectionInstanceCall`,
// src/codegen/LlvmBackend_Calls.cpp) recognised a `LambdaExpr` and a
// primitive-intrinsic method reference, and nothing else: a METHOD GROUP fell
// through it, so the release never fired and the cell leaked.
//
// MEASURED on the base (r30-c1 @ 044b93855) with `bezelc --sanitize=address`,
// the issue's own repro shape (`Array<int>.Sum(P)`, 200 calls):
//
//   Direct leak of 64 byte(s) in 1 object(s) allocated from:
//       #0 calloc
//   SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s).
//
// (the issue's literal two-call repro leaks 128 B in 2 allocations — 64 B per
// call; the loop is what makes the count decisive).
//
// THE FIX. The eager operator's producer test now asks the SHARED gate
// `isPlusOneClosureProducer` — the predicate the rest of the compiler already
// asks for exactly this question ("does this expression mint a fresh +1
// closure cell the consumer must release"): the general call path asks it for
// a call-bounded function-typed formal (LlvmBackend_Calls2.cpp, `producerArg`),
// and the LAZY path's `argIsFreshClosureCell` (LlvmBackend_Calls3.cpp, #7160)
// mirrors its shape. The drift between the three copies IS the defect.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer pass
// over the GENERATED module and LeakSanitizer at exit. Every row asserts the
// program's own output AND that LeakSanitizer counted nothing, so a silent
// zero-fill cannot pass, and no double free can hide. The rows LOOP (200
// iterations inside the program), so the leak cannot hide under the
// sanitizer's noise.
//
// `runAot` CANNOT be the oracle here: it does not run the AddressSanitizer
// pass over the generated module (only `runAotChecked` sets
// `opts.sanitizeAddress`), so the leak would go unreported and every row would
// be green on the base — pinning nothing. This is the same reason the
// neighbouring ownership leak rows (tests/ownership/
// closure_forwarding_call_string_result_leak_8788_test.cpp) use the checked
// oracle, and lane-contract §5 requires it for disposal/ownership work.

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

}  // namespace

// ROW 1 — the issue's literal repro, amplified 200x. A STATIC method group
// (`Program.P`) passed straight to the eager `Sum` selector. The total is
// 200 x (5 + 6) = 2200.
//
// RED on the base: 12800 B leaked in 200 allocations (one 64 B cell per call).
// GREEN with the fix.
BEZEL_TEST(builtin_selector_method_group_closure_release_9012,
           static_method_group_selector_closure_is_released) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): int => i; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      t := t + a.Sum(P); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2200\n");
}

// ROW 2 — the INSTANCE method group spelling (`s.P`). §27.6: "the instance is
// captured at formation — by reference for class- or interface-typed
// receivers", so the cell also carries an 8 B env block: the base leaks 72 B
// per call (the 64 B cell + the 8 B env).
//
// RED on the base: 14400 B leaked in 400 allocations. GREEN with the fix.
BEZEL_TEST(builtin_selector_method_group_closure_release_9012,
           instance_method_group_selector_closure_is_released) {
    const std::string src =
        "module M; "
        "public class Sel "
        "  method P(i: int): int => i; "
        "end Sel; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := new Sel(); "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      t := t + a.Sum(s.P); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2200\n");
}

// ROW 3 — the same method-group selector on an `IEnumerable<T>` HEAD. The
// stdlib §5 operators live on `extension IEnumerable<T>`, so the head is the
// shape the issue's operators are actually declared on; the eager terminal
// materializes the source and then runs the same array lowering.
//
// RED on the base: 12800 B leaked in 200 allocations. GREEN with the fix.
BEZEL_TEST(builtin_selector_method_group_closure_release_9012,
           enumerable_head_method_group_selector_closure_is_released) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): int => i; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var src := new Array<int>(); "
        "      src.Add(5); src.Add(6); "
        "      var v: IEnumerable<int> := src; "
        "      t := t + v.Sum(P); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2200\n");
}

// CONTROL 1 — the LAMBDA twin of ROW 1, the shape the issue records as
// already correct. It must stay correct: the fix must not turn the lambda's
// single discharge into two.
//
// GREEN on the base and with the fix. RED under a plausible wrong fix — one
// that discharges the callback a second time (e.g. an unconditional release
// beside the producer-gated one): the lambda's cell would be freed twice and
// AddressSanitizer would report a double free.
BEZEL_TEST(builtin_selector_method_group_closure_release_9012,
           lambda_selector_twin_stays_clean) {
    const std::string src =
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      t := t + a.Sum(lambda(x: int) => x); "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2200\n");
}

// CONTROL 2 — the LAZY twin: the SAME method-group selector on a LAZY §5
// operator (`Select`). stdlib §5: "A predicate/selector/key/combiner passed to
// a **lazy** operator … is **retained by the returned query value until
// enumeration**" — so the lazy path's discharge is at the ENUMERATOR's dispose,
// not at the call, and the eager path's discharge must not fire on it.
//
// GREEN on the base (the #7160 arm already owns the lazy method-group cell)
// and with the fix. RED under a plausible wrong fix — one that also releases
// the lazy operator's callback at the call site: the query value would hold a
// freed cell and the enumeration would read freed memory.
BEZEL_TEST(builtin_selector_method_group_closure_release_9012,
           lazy_method_group_selector_stays_owned_by_the_query) {
    const std::string src =
        "module M; "
        "public class Program "
        "  static method P(i: int): int => i; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 ..< 200 do "
        "      var a := new Array<int>(); "
        "      a.Add(5); a.Add(6); "
        "      var q := a.Select(P); "
        "      for var it in q do t := t + it; end for; "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "2200\n");
}
