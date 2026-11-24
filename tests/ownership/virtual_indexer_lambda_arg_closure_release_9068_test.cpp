// Regression test for issue #9068 — a lambda passed as the ARGUMENT of a
// VIRTUAL indexer leaked its closure (16000 B / 600 allocations per 200
// calls), while the non-virtual indexer twin and the virtual METHOD twin were
// clean.
//
// THE DEFECT. `public virtual property [q: () -> int]: int => q();` — a
// class-declared VIRTUAL indexer whose parameter is a plain function-typed
// parameter — read through a `new Fl()` receiver. The index argument is a
// lambda literal `lambda() => r.V`, a +1 closure producer (§28.3). MEASURED on
// r30-c1 @ 346f3191b with `bezelc --sanitize=address` over the issue's own
// 200-iteration repro:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #1 0x000000375bf0 in bz$M.Program$Mk ...
//   Indirect leak of 1600 byte(s) in 200 object(s) ...
//   Indirect leak of 1600 byte(s) in 200 object(s) ...
//   SUMMARY: AddressSanitizer: 16000 byte(s) leaked in 600 allocation(s).
//
// Three allocations per call — the 64-byte closure object, its 8-byte capture
// cell and the cell's 8-byte payload — all attributed to the CALLER.
//
// THE ROOT CAUSE. `Emitter::lowerIndexerGet`
// (src/codegen/LlvmBackend_Assign.cpp) has three getter call paths. #4078
// wired `releaseIndexArgClosureProducers` — the caller's drop of the
// producer's +1 — into the block-form getter's standalone call and into the
// expression-bodied getter inlined at the call site, but NOT into the third
// path: the CLASS-VTABLE dispatch (#1080 / §23.4), the path a VIRTUAL indexer
// takes on a class receiver. So the vtable-dispatched getter (a standalone
// function built by `ensureIndexerGetterFn`) kept the callback for the call's
// duration, as §28.14 requires, and the CALLER never dropped its +1.
//
// THE SPEC. §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded** … the callee
// receives the callback for the duration of the call only". §23.5 (v0.359),
// *Restrictions*: "`ref` and `out` parameters are not allowed in indexer
// parameter lists" — so an indexer's function-typed parameter is plain, hence
// call-bounded, and the producer's +1 is the caller's to drop on the
// normal-return edge. §28.9 (v0.359): the closure object is runtime-managed,
// so its own reference must be dropped exactly once. Nothing in the spec
// turns on virtual vs non-virtual dispatch — the twins the issue names are
// clean only because their paths happen to carry the release.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the per-call leak cannot hide under the sanitizer's noise.
//
// ROWS.
//   Row 1 — the issue's own repro: a VIRTUAL indexer, a lambda argument
//           capturing a class instance, 200 iterations. RED on the base
//           (16000 B / 600 allocations), GREEN with the fix.
//   Row 2 — the same shape dispatched through the vtable slot to an OVERRIDE
//           in a derived class (the receiver's static type is the BASE, so the
//           call really goes through the vtable). RED on the base, GREEN with
//           the fix.
//   Row 3 — GREEN CONTROL: the NON-VIRTUAL indexer twin the issue names
//           (`public property [q: () -> int]: int => q();`). Clean before and
//           after; a fix that over-releases (a second release of the same
//           closure, e.g. one that also releases in the callee) turns this row
//           red with a double-free / use-after-free.
//   Row 4 — GREEN CONTROL: the virtual METHOD twin the issue names
//           (`public virtual method M(q: () -> int): int => q();`). Clean
//           before and after; it fails if the fix touches the method-call path
//           (`prepareCallClosureArgRelease`), which already releases.
//
// per §28.14, §27.5, §28.9, §23.4, §23.5 (issue #9068)

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

// The shared preamble: a class instance whose field the lambda captures (the
// closure therefore owns a capture cell — the second of the three leaked
// allocations). The indexer under test is spelled by each row, so that the
// row's twin is visible next to the row it contrasts with.
constexpr const char* kRes =
    "module M; "
    "public class Res "
    "  public V: int; "
    "  public constructor() V := 42; end constructor; "
    "end Res; ";

// ── Row 1 (the issue's own repro / §28.14): a VIRTUAL indexer on a CLASS
// receiver, invoked with an inline lambda that captures a class instance.
// 200 iterations, so the per-call leak (80 B in 3 allocations) is
// unambiguous.
//
// RED on the base (16000 B / 600 allocations — 80 B per call): the closure,
// its capture cell and the cell's payload are all stranded in the CALLER.
// GREEN with the fix: `releaseIndexArgClosureProducers` drops the producer's
// +1 on the normal-return edge of the vtable-dispatched getter call.
BEZEL_TEST(virtual_indexer_lambda_arg_closure_release_9068,
           issue_repro_200_iterations_reclaims_the_closure) {
    const std::string src = std::string(kRes) +
        "public class Fl "
        "  public virtual property [q: () -> int]: int => q(); "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var fl := new Fl(); "
        "      t := t + fl[lambda() => r.V]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "8400\n");
}

// ── Row 2 (§23.4 / §12.1): the same shape, but the receiver's STATIC type is
// the BASE class while the runtime object is a derived class with an
// OVERRIDE indexer — so the call really goes through the vtable slot and the
// OVERRIDE's accessor runs (the override returns `q() + 1`, so the sum is
// 200 x 43 = 8600, not the base's 8400 — the printed value is an independent
// oracle that the dispatched accessor really ran).
//
// RED on the base (16000 B / 600 allocations), GREEN with the fix.
BEZEL_TEST(virtual_indexer_lambda_arg_closure_release_9068,
           vtable_dispatch_to_an_override_reclaims_the_closure) {
    const std::string src = std::string(kRes) +
        "public class Fl "
        "  public virtual property [q: () -> int]: int => q(); "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Fl2 : Fl "
        "  public override property [q: () -> int]: int => q() + 1; "
        "  public constructor() end constructor; "
        "end Fl2; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var fl: Fl := new Fl2(); "
        "      t := t + fl[lambda() => r.V]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "8600\n");
}

// ── Row 3 (GREEN CONTROL — the issue's non-virtual twin): the same program
// with a NON-VIRTUAL indexer. Its getter body is INLINED at the call site,
// and that path already carried `releaseIndexArgClosureProducers` before the
// fix, so the row is green on the base and green with the fix.
//
// It is the OVER-RELEASE guard: a fix that releases the closure twice (for
// example one that also releases inside the callee, or one that widens
// `releaseIndexArgClosureProducers` to a caller-owned local) turns this row
// red with a double-free / use-after-free under ASan.
BEZEL_TEST(virtual_indexer_lambda_arg_closure_release_9068,
           non_virtual_indexer_twin_stays_clean) {
    const std::string src = std::string(kRes) +
        "public class Fl "
        "  public property [q: () -> int]: int => q(); "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var fl := new Fl(); "
        "      t := t + fl[lambda() => r.V]; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "8400\n");
}

// ── Row 4 (GREEN CONTROL — the issue's virtual METHOD twin): the callback
// passed to a virtual METHOD's plain function-typed parameter. The method-call
// path (`prepareCallClosureArgRelease`) already released the producer's +1, so
// the row is green on the base and green with the fix.
//
// It is the SCOPE guard: a fix that touched the method-call path as well would
// release the closure twice here, and the row would fail with a double-free.
BEZEL_TEST(virtual_indexer_lambda_arg_closure_release_9068,
           virtual_method_twin_stays_clean) {
    const std::string src = std::string(kRes) +
        "public class Fl "
        "  public virtual method M(q: () -> int): int => q(); "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var fl := new Fl(); "
        "      t := t + fl.M(lambda() => r.V); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "8400\n");
}

}  // namespace
