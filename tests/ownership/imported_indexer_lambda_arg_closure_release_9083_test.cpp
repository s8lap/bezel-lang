// Regression test for issue #9083 — a lambda passed as the ARGUMENT of an
// IMPORTED indexer leaked its closure (12800 B / 200 allocations per 200
// calls), for the imported VIRTUAL indexer and, identically, for the imported
// NON-VIRTUAL one.
//
// THE DEFECT. `public virtual property [q: () -> int]: int => q();` — an
// indexer declared in ANOTHER module, whose parameter is a plain function-typed
// parameter — read through an imported receiver. The index argument is a
// lambda literal `lambda() => 7`, a +1 closure producer (§28.3). MEASURED on
// r30-c1 @ d3e4526e4 with `bezelc --sanitize=address` over the issue's own
// 200-iteration two-module repro:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #1 ... in main ...
//   SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s).
//
// 64 bytes per call — the closure object alone (the probe's lambda captures
// nothing, so there is no capture cell and no cell payload).
//
// THE ROOT CAUSE. `Emitter::lowerIndexerGet`
// (src/codegen/LlvmBackend_Assign.cpp) has several getter call paths. #4078
// (commit 504abbd49) wired `releaseIndexArgClosureProducers` — the caller's
// drop of the producer's +1 — into the block-form getter's standalone call and
// into the expression-bodied getter inlined at the call site; #9068
// (commit af52ef536) wired it into the class-VTABLE dispatch. The IMPORTED
// path — the `if (!pd && recvDecl->kind != ast::TypeDeclKind::Interface)` arm,
// whose call is the exported stub `bz$<Class>$idxget$<arity>` — was never
// wired, so the CALLER never dropped its +1 and the closure object was
// stranded for the life of the program.
//
// THE SPEC. §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded** … the callee
// receives the callback for the duration of the call only". §23.5 (v0.359),
// *Restrictions*: "`ref` and `out` parameters are not allowed in indexer
// parameter lists" — so an indexer's function-typed parameter is plain, hence
// call-bounded, and the producer's +1 is the caller's to drop on the
// normal-return edge. §28.9 (v0.359): the closure object is runtime-managed,
// so its own reference must be dropped exactly once. Nothing in the spec
// turns on virtual vs non-virtual dispatch, nor on whether the indexer is
// declared in this module or another one.
//
// ORACLE. `runAotModulesChecked` — the checked two-module AOT path with the
// AddressSanitizer pass over the GENERATED modules and LeakSanitizer at exit.
// Every row asserts the program's own output AND that LeakSanitizer counted
// nothing, so a silent zero-fill cannot pass. The rows LOOP (200 iterations
// inside the program), so the per-call leak cannot hide under the sanitizer's
// noise.
//
// ROWS.
//   Row 1 — the issue's own repro: an IMPORTED VIRTUAL indexer, a lambda
//           argument, 200 iterations. RED on the base (12800 B / 200
//           allocations), GREEN with the fix.
//   Row 2 — the imported NON-VIRTUAL indexer twin, which the issue measures
//           as leaking IDENTICALLY (12800 B / 200 allocations). RED on the
//           base, GREEN with the fix.
//   Row 3 — GREEN CONTROL: the SINGLE-MODULE non-virtual indexer twin — the
//           path #4078 already wired (`releaseIndexArgClosureProducers` at
//           the expression-bodied getter inlined at the call site). Clean
//           before and after; it is the DOUBLE-RELEASE guard: a fix that
//           released the producer's +1 twice (or that widened the release
//           into a path that already releases it) turns this row red with a
//           bad free / use-after-free under ASan.
//
// NOT A ROW — the caller-owned local. `var g := lambda() => 7; fl[g];` —
// a closure local passed as the index argument — ALSO leaks (12736 B / 199
// allocations single-module, 12800 B / 200 imported), on the base AND with
// this fix, because the closure ESCAPE walk treats every index argument as
// an escape (ClosureEscapeWalker::expr's IndexExpr arm walks each index
// argument with `safeRead=false`), so the local's own +1 is never released.
// That is a DIFFERENT defect with a DIFFERENT root cause (it affects every
// indexer path, single-module ones included, and the METHOD twin — a plain
// function-typed method parameter — is clean), so it is FILED separately
// rather than pinned here.
//
// per §28.14, §27.5, §28.9, §23.4, §23.5 (issue #9083)

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
// source (a producer written AT the index position).
std::string appModule(const std::string& className,
                      const std::string& indexExpr) {
    return "module App" + className + "; "
           "import " + className + "; "
           "public class Program" + className + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      var fl := new " + className + ".Fl" + className + "(); "
           "      t := t + fl[" + indexExpr + "]; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + className + ";";
}

// ── Row 1 (§28.14 / §28.9): the issue's own repro — an IMPORTED VIRTUAL
// indexer whose plain function-typed parameter receives a lambda literal.
// 200 iterations, so the per-call leak (64 B in 1 allocation) is unambiguous.
//
// RED on the base (12800 B / 200 allocations): the closure object is stranded
// in the CALLER, which never drops the producer's +1 on the imported getter's
// normal-return edge. GREEN with the fix: `releaseIndexArgClosureProducers`
// drops it there.
BEZEL_TEST(imported_indexer_lambda_arg_closure_release_9083,
           imported_virtual_indexer_200_iterations_reclaims_the_closure) {
    const std::string cls = "Lib9083V";
    auto r = runAotModulesChecked(
        {ModuleSource{cls, libModule("virtual ", cls)},
         ModuleSource{"App9083V", appModule(cls, "lambda() => 7")}},
        "App9083V");
    expectClean(r, 0, "1400\n");
}

// ── Row 2 (§28.14 / §28.9): the imported NON-VIRTUAL indexer twin — the
// issue measures it as leaking IDENTICALLY (12800 B / 200 allocations), which
// is why the defect is NOT part of #9068 (whose shape is "the virtual indexer
// leaks and the non-virtual twin is clean").
//
// RED on the base, GREEN with the fix.
BEZEL_TEST(imported_indexer_lambda_arg_closure_release_9083,
           imported_non_virtual_indexer_200_iterations_reclaims_the_closure) {
    const std::string cls = "Lib9083NV";
    auto r = runAotModulesChecked(
        {ModuleSource{cls, libModule("", cls)},
         ModuleSource{"App9083NV", appModule(cls, "lambda() => 7")}},
        "App9083NV");
    expectClean(r, 0, "1400\n");
}

// ── Row 3 (GREEN CONTROL — the single-module twin the issue names as already
// correct): the SAME shape in ONE module — a NON-VIRTUAL indexer whose getter
// body is INLINED at the call site. That path already carried
// `releaseIndexArgClosureProducers` before this fix (#4078), so the row is
// green on the base and green with the fix.
//
// It is the DOUBLE-RELEASE guard: a fix that releases the producer's +1 twice
// (for example one that also releases inside the callee, or one that widens
// the release into the single-module path as well) turns this row red with a
// bad free / use-after-free under ASan.
BEZEL_TEST(imported_indexer_lambda_arg_closure_release_9083,
           single_module_non_virtual_indexer_twin_stays_clean) {
    const std::string src =
        "module Single9083; "
        "public class FlSingle9083 "
        "  public property [q: () -> int]: int => q(); "
        "  public constructor() end constructor; "
        "end FlSingle9083; "
        "public class ProgramSingle9083 "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var fl := new FlSingle9083(); "
        "      t := t + fl[lambda() => 7]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end ProgramSingle9083;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "1400\n");
}

}  // namespace
