// Codegen / leak — a LAMBDA LITERAL passed to a `?.` (null-conditional)
// METHOD CALL leaked its closure cell — and the capture env behind it — on
// EVERY call: the `?.` call path never performed the §28.14 call-bounded
// argument discharge the `.` path performs. Issue #8624.
//
// Spec: §28.14 (scoped callback parameters — "The call-bounded default"),
// §28.9 (runtime-managed types — the closure cell the runtime reclaims),
// §27.5 (closures), §7.5 (null-conditional access — "`x?.M` returns null if
// `x` is null; otherwise it performs the access"). Spec revision v0.359.
//
// THE DEFECT. §28.14's call-bounded default: "A function-typed parameter
// declared *without* `scoped` is call-bounded: the callee receives the
// callback for the duration of the call only". A lambda literal handed
// DIRECTLY to such a parameter is a `+1` closure producer the CALL SITE
// solely owns, so the call site must release it once the call is done —
// `MethodOwnership::callBoundedScan` (src/ownership/Ownership_Checks.cpp)
// proves the callee holds no reference past the call, so nothing on the
// callee side ever drops that +1.
//
// The `.` call path (`Emitter::lowerCallDispatch`) has always discharged it
// (codegen #1073, and #3498's capture-cell half). The `?.` call path
// (`Emitter::lowerNullCondCall`) called `emitMethodCall` DIRECTLY and never
// installed that discharge, so the closure cell (64 B) and its capture env
// (8 B) leaked once per call.
//
// MEASURED on this tree's base (r30-c1), the issue's own repro compiled with
// `bezelc --sanitize=address`:
//
//   18                                     <- the program's stdout
//   SUMMARY: AddressSanitizer: 3840 byte(s) leaked in 60 allocation(s).
//
// while the `.` and `!.` spellings of the very same call were clean — the
// `?.` spelling differs from `x.M` in exactly one way it must not (§7.5:
// "`x?.M` returns null if `x` is null; otherwise it performs the access").
//
// THE FIX. The closure-argument discharge — its three release vectors, its
// exception-edge cleanup and its normal-return release — is now ONE shared
// function pair (`Emitter::prepareCallClosureArgRelease` /
// `Emitter::releaseCallClosureArgs`, src/codegen/LlvmBackend_Calls2.cpp),
// called from BOTH call paths, so the two spellings of one access cannot
// drift apart. The `?.` call runs inside `callBB` — the non-null arm — so the
// discharge runs exactly where the call ran: §7.5's "if it is null the access
// does not occur and nothing to its right in the chain is evaluated" still
// holds, and a null receiver evaluates no argument at all.
//
// ORACLE. `runAotChecked` links and runs the generated code under ASan/LSan,
// so a leak, double-free or use-after-free flips `r.ran` to false; the stdout
// assertion independently pins the computed value, so a premature free (a
// double release) would corrupt it too. The 60-iteration loop is the
// issue's own repro and satisfies the "loop it 50–500x" discipline: one
// execution's 64 B cell can hide under the sanitizer's noise, sixty cannot.
//
// FAIL-WITHOUT / PASS-WITH. Every row below leaks on the base (row A: 3840 B
// in 60 allocations; row B: 4324 B in 121 allocations) and is LSan-clean with
// the fix.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue #8624 repro, verbatim, as a program string.
std::string issueRepro() {
    return
        "module P; "
        "public class H "
        "  public method M(xs: Array<int>, key: (int) -> int): int "
        "    var b := 0; "
        "    for var x in xs do "
        "      if key(x) > b then b := key(x); end; "
        "    end; "
        "    return b; "
        "  end M; "
        "end H; "
        "public class Program "
        "  public static method Main() "
        "    var xs: Array<int> := [3, 9, 4]; "
        "    var h: H? := new H(); "
        "    var n := 0; "
        "    var i := 0; "
        "    while i < 60 do "
        "      n := h?.M(xs, lambda(v: int) => v * 2) ?? -1; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;";
}

}  // namespace

// ===========================================================================
// A — THE ISSUE'S OWN REPRO. A lambda literal at a §28.14 call-bounded
// parameter of a `?.`-called method. On base: 3840 B in 60 allocations (the
// 64 B closure cell, once per call); with the fix: LSan-clean.
//
// The value assertion (stdout "18") is what makes the row fail loudly if the
// discharge were ever wrong in the OTHER direction — a premature release
// would let the callee read a freed cell and the program would print
// garbage or crash.
// ===========================================================================
BEZEL_TEST(codegen_8624_null_cond_closure_arg,
           null_cond_call_lambda_literal_arg_released_spec_28_14) {
    auto r = runAotChecked(issueRepro());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "18\n");
}

// ===========================================================================
// B — the CAPTURE-BEARING spelling. §27.5: "Captured variables are
// heap-allocated in a closure object" — the env block behind the cell is a
// SEPARATE allocation, so the `?.` path leaked BOTH: 4324 B in 121
// allocations on base (60 cells + 60 env blocks + 1). This row pins the
// capture-cell half of the discharge (#3498) on the `?.` spelling as well.
//
// The captured local is read by the callee THROUGH the callback, so a
// premature release would read a freed capture cell — the row's value
// assertion ("18") covers that too.
// ===========================================================================
BEZEL_TEST(codegen_8624_null_cond_closure_arg,
           null_cond_call_capturing_lambda_arg_released_spec_27_5) {
    auto r = runAotChecked(
        "module P; "
        "public class H "
        "  public method M(xs: Array<int>, key: (int) -> int): int "
        "    var b := 0; "
        "    for var x in xs do "
        "      if key(x) > b then b := key(x); end; "
        "    end; "
        "    return b; "
        "  end M; "
        "end H; "
        "public class Program "
        "  public static method Main() "
        "    var xs: Array<int> := [3, 9, 4]; "
        "    var h: H? := new H(); "
        "    var cap := 2; "
        "    var n := 0; "
        "    var i := 0; "
        "    while i < 60 do "
        "      n := h?.M(xs, lambda(v: int) => v * cap) ?? -1; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "18\n");
}
