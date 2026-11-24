// Regression test for issue #9102 — a CALLER-OWNED closure local passed as
// an INDEX argument leaked its closure (12800 B / 200 allocations per 200
// calls), in EVERY indexer path, the single-module ones included.
//
// THE DEFECT. `var g := lambda() => 7; fl[g];` — the closure is NOT a producer
// written at the index position (that is #9083's shape, already fixed), it is
// a LOCAL the caller owns: the local's slot holds the +1 the lambda literal
// minted, and the local's own scope-end `bz_closure_release` is the one that
// must drop it (§28.9). MEASURED on r30-c1 @ 116823e00 with `bezelc
// --sanitize=address` over the issue's own 200-iteration two-module repro:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #1 ... in main ...
//   SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s).
//
// 64 bytes per call — the closure object alone (the probe's lambda captures
// nothing, so there is no capture cell and no cell payload). The single-module
// twin measures 12736 B / 199 allocations (199 of 200: the last closure is
// still reachable at exit, so LeakSanitizer does not report it).
//
// THE ROOT CAUSE. `ClosureEscapeWalker::expr`'s `IndexExpr` arm
// (src/codegen/LlvmBackend_Setup.cpp) walked EVERY index argument with
// `safeRead=false` — an ESCAPE — with no premise of the kind the METHOD twin
// has. `safeRead` is "true when a bare identifier in the current expression
// position is a non-consuming read; false when it would escape", so a closure
// local used as an index argument was classified as escaping:
// `collectClosureReleaseEligible` did not arm its scope-end release, and the
// local's own +1 was never dropped.
//
// THE SPEC. §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded** … the callee
// receives the callback for the duration of the call only" — so the CALLER
// keeps its own reference and releases it at scope end, exactly as at a
// method's plain function-typed formal. §23.5 (v0.359): "`ref` and `out`
// parameters are not allowed in indexer parameter lists" — so an indexer's
// function-typed parameter is plain, hence call-bounded. §28.9 (v0.359): the
// closure object is runtime-managed, so its own reference must be dropped
// exactly once. Nothing in the spec turns on virtual vs non-virtual dispatch,
// nor on whether the indexer is declared in this module or another one.
//
// THE FIX. The walker's `IndexExpr` arm now asks the index-argument twin of
// the METHOD twin's premise — `indexArgBindsCallBoundedIndexerParam`
// (src/codegen/LlvmBackend_IndexerParams.hpp) — and reads an index argument
// bound to a plain function-typed indexer parameter as a non-consuming read.
// The premise lives in the WALKER (like the METHOD twin's), so EVERY walk over
// the body sees it: the producer walk that decides the scope-end release, the
// KEPT walk that decides whether the value survives the scope, and the alias
// walks in between.
//
// ORACLE. `runAotModulesChecked` / `runAotChecked` — the checked AOT path
// with the AddressSanitizer pass over the GENERATED modules and
// LeakSanitizer at exit. Every row asserts the program's own output AND that
// LeakSanitizer counted nothing, so a silent zero-fill cannot pass. The rows
// LOOP (200 iterations inside the program), so the per-call leak cannot hide
// under the sanitizer's noise.
//
// ROWS.
//   Row 1 — the issue's own repro: an IMPORTED (virtual) indexer, a
//           caller-owned local as the index argument, 200 iterations. RED on
//           the base (12800 B / 200 allocations), GREEN with the fix.
//   Row 2 — the SINGLE-MODULE twin, which the issue measures as leaking
//           identically (12736 B / 199 allocations). RED on the base, GREEN
//           with the fix.
//   Row 3 — GREEN CONTROL: the METHOD twin — the same caller-owned local
//           passed to a plain function-typed METHOD parameter, which the
//           issue measures as CLEAN. It is the DOUBLE-RELEASE guard: a fix
//           that released the local's +1 at the call's end as well as at
//           scope end would turn this row red with a bad free /
//           use-after-free under ASan.
//   Row 4 — GREEN CONTROL: the RETURN sink — an indexer whose ELEMENT type
//           is itself a function type, so the accessor hands the callback
//           BACK OUT of the call (`property [q: () -> int]: () -> int => q;`).
//           The premise must decline it, so the local keeps its +1 and the
//           callback stays alive for the caller that received it: the row
//           asserts the program runs and prints the right value and that NO
//           memory error fired. (The accessor's body is never §28.14-verified
//           — that is #9084's gap — so the conservative ESCAPE is the sound
//           answer for this shape, and the row pins it.)
//
// The `scoped` index-parameter twin (an indexer whose parameter is declared
// `scoped`, which §28.14 makes STORAGE-bounded — the callee MAY keep the
// callback) is declined by the same premise and keeps its residual leak; it
// is the METHOD twin's own shape (`isCallBoundedClosureArgPosition` declines
// every modifier too) and is not pinned here.
//
// per §28.14, §28.9, §23.4, §23.5, §22.3 (issue #9102)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotChecked;
using bezel::test::runAotModulesChecked;

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
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotModulesChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// The program produced its own output and NO memory error fired — the oracle
// for a shape whose residual LEAK is documented (the premise declines it, so
// the local keeps its +1 and the callback stays alive). The exit code is NOT
// asserted here: LeakSanitizer's exit code supersedes the program's own when
// it fires (the #4532 lesson the harness documents), so the row pins the
// program's OUTPUT and the ABSENCE of a memory error, and nothing that the
// sanitizer's exit status would decide.
void expectNoMemoryError(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("SEGV") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The imported module's indexer, spelled by each row so the row's twin is
// visible next to the row it contrasts with. `virtual` is "" for the
// NON-VIRTUAL twin and "virtual " for the VIRTUAL one.
std::string libModule(const std::string& virtualKeyword,
                      const std::string& className) {
    return "module " + className + "; "
           "public class Fl" + className + " "
           "  public " + virtualKeyword + "property [q: () -> int]: int => q(); "
           "  public constructor() end constructor; "
           "end Fl" + className + ";";
}

// The importing module's program: `indexExpr` is the index argument's
// source. Here it is the CALLER-OWNED LOCAL — the shape #9102 is about.
std::string appModule(const std::string& className,
                      const std::string& indexExpr) {
    return "module App" + className + "; "
           "import " + className + "; "
           "public class Program" + className + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      var fl := new " + className + ".Fl" + className + "(); "
           "      var g := lambda() => 7; "
           "      t := t + fl[" + indexExpr + "]; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + className + ";";
}

// ── Row 1 (§28.14 / §28.9 / §23.5): the issue's own repro — an IMPORTED
// indexer whose plain function-typed parameter receives a CALLER-OWNED local.
// 200 iterations, so the per-call leak (64 B in 1 allocation) is unambiguous.
//
// RED on the base (12800 B / 200 allocations): the local's own +1 is never
// dropped, because the escape walk classified the index argument as an
// escape. GREEN with the fix: the local's scope-end release fires.
BEZEL_TEST(indexer_caller_owned_closure_local_9102,
           imported_virtual_indexer_caller_owned_local_200_iterations_reclaims_the_closure) {
    const std::string cls = "Lib9102V";
    auto r = runAotModulesChecked(
        {ModuleSource{cls, libModule("virtual ", cls)},
         ModuleSource{"App9102V", appModule(cls, "g")}},
        "App9102V");
    expectClean(r, 0, "1400\n");
}

// ── Row 2 (§28.14 / §28.9): the SINGLE-MODULE twin — the issue measures it
// as leaking identically (12736 B / 199 allocations), so it is NOT an import
// defect: the index argument is escaped by the SAME walk, in the same
// one-module program.
//
// RED on the base, GREEN with the fix.
BEZEL_TEST(indexer_caller_owned_closure_local_9102,
           single_module_indexer_caller_owned_local_200_iterations_reclaims_the_closure) {
    const std::string src =
        "module Single9102; "
        "public class FlSingle9102 "
        "  public property [q: () -> int]: int => q(); "
        "  public constructor() end constructor; "
        "end FlSingle9102; "
        "public class ProgramSingle9102 "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var fl := new FlSingle9102(); "
        "      var g := lambda() => 7; "
        "      t := t + fl[g]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end ProgramSingle9102;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "1400\n");
}

// ── Row 3 (GREEN CONTROL — the METHOD twin the issue names as already
// correct): the SAME caller-owned local passed to a plain function-typed
// METHOD parameter. The METHOD twin's premise
// (`closureArgBindsMethodOwnTypeParamBorrow`) already reads that position as
// a non-consuming read, so the local's scope-end release fires and the
// program is clean — on the base and with this fix.
//
// It is the DOUBLE-RELEASE guard: a fix that released the local's +1 at the
// call's end as well as at scope end (or that armed the release twice) turns
// this row red with a bad free / use-after-free under ASan.
BEZEL_TEST(indexer_caller_owned_closure_local_9102,
           method_twin_plain_function_typed_parameter_stays_clean) {
    const std::string src =
        "module MethodTwin9102; "
        "public class FlMethodTwin9102 "
        "  public static method Take(q: () -> int): int => q(); "
        "  public constructor() end constructor; "
        "end FlMethodTwin9102; "
        "public class ProgramMethodTwin9102 "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var g := lambda() => 7; "
        "      t := t + FlMethodTwin9102.Take(g); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end ProgramMethodTwin9102;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "1400\n");
}

// ── Row 4 (GREEN CONTROL — the RETURN sink): an indexer whose ELEMENT type
// is itself a function type, so the accessor hands the callback BACK OUT of
// the call (`property [q: () -> int]: () -> int => q;`). The premise declines
// that shape — the element type is a function type, so the call may carry the
// callback out — so the local keeps its +1, the callback stays alive for the
// caller that received it, and the program prints the right value.
//
// The row pins the DECLINE: a premise that admitted the return sink would
// release the local's +1 at scope end while the returned callback is still
// live, and the row would go red with a use-after-free under ASan.
BEZEL_TEST(indexer_caller_owned_closure_local_9102,
           function_typed_element_indexer_keeps_the_callback_alive) {
    const std::string src =
        "module Ret9102; "
        "public class FlRet9102 "
        "  public property [q: () -> int]: () -> int => q; "
        "  public constructor() end constructor; "
        "end FlRet9102; "
        "public class ProgramRet9102 "
        "  public static method Use(fl: FlRet9102): () -> int "
        "    var g := lambda() => 7; "
        "    return fl[g]; "
        "  end Use; "
        "  public static method Main(): int "
        "    var fl := new FlRet9102(); "
        "    var h := Use(fl); "
        "    Console.WriteLine(h()); "
        "    return 0; "
        "  end Main; "
        "end ProgramRet9102;";
    auto r = runAotChecked(src);
    expectNoMemoryError(r, "7\n");
}

}  // namespace
