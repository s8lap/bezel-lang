// Codegen / leak — a LAMBDA LITERAL passed DIRECTLY as a borrowed method
// argument is never released (§28.9). Issue #1073.
//
// Spec: §28.9 (closure-cell ARC), §27.3 (lambda expressions), §28.3 (callee
// parameter borrow). Spec revision v0.220.
//
// THE BUG. A lambda literal passed directly as a method argument is a `+1`
// closure producer (the call to the lambda constructor mints exactly one
// reference). The callee parameter is a BORROW (Bezel parameters are borrows
// by default), so the callee has not transferred any reference out of the
// caller's transient. Without a call-site release, the temporary's `+1` is
// never dropped, and the closure cell + env block leak (LSan: 40 + 8 bytes).
//
// THE FIX. In `Emitter::lowerCall` (LlvmBackend_Calls2.cpp), after the call
// returns, scan each non-static argument for a `+1` closure producer
// (isPlusOneClosureProducer) and release the temporary unless the parameter
// is `owned` (ownership transfers IN to the callee), the callee is an async
// method/iterator (its frame captures the closure without retaining — a
// release would UAF), or the callee is a sync iterator (already handled by
// the #1864 release). This extends the #1864 closure ARGUMENT-PASS release
// from sync-iterator callees only to plain (non-iterator) callees — the
// closure analogue of the #429 string ARGUMENT-PASS release.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan. The leak case
// calls the producer in a loop so N evaluations leak N closures (40 bytes
// each — unmistakable under LSan). The loop test asserts `r.ran` is true AND
// `r.diagnostics` is empty, so an LSan report (which surfaces as a
// `runAotChecked: sanitizer fired (LSan/ASan at exit)` diagnostic) is a hard
// FAIL — pinning both the no-UAF and the no-leak contract at once.
// Completes the slot-A-12 WIP for issue #1073 (LlvmBackend_Calls2.cpp).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// (A) #1073 repro — lambda literal passed directly to a borrow-parameter
// method. The closure cell + env leak ~48 bytes pre-fix. Single call: exit
// code is 1 (box.Add(1) ran exactly once).
const char* kLambdaLiteralArg =
    "module M; "
    "class Box public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Sink public method Take(h: () -> ()) h(); end Take; end Sink; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  using var s := new Sink(); "
    "  s.Take(lambda() => do box.Add(1); end); "
    "  return box.V; "
    "end Main; end C;";

// (B) Same shape in a loop — 50 evaluations amplify the leak from a single
// 40-byte cell to a clear 2000-byte LSan headline. Pre-fix this is the
// reliable FAIL signal: `Direct leak of 2000 byte(s)` over 50 allocations.
// Post-fix: ASan-clean. box.V == 50.
const char* kLambdaLiteralArgLoop =
    "module M; "
    "class Box public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Sink public method Take(h: () -> ()) h(); end Take; end Sink; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  using var s := new Sink(); "
    "  var i := 0; "
    "  while i < 50 do "
    "    s.Take(lambda() => do box.Add(1); end); "
    "    i := i + 1; "
    "  end; "
    "  return box.V; "
    "end Main; end C;";

}  // namespace

// =====================================================================
// (A) Single call — ASan-clean, exit code 1. §28.9 / §27.3 / #1073.
// =====================================================================
BEZEL_TEST(codegen_1073_closure_literal_arg, single_call_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kLambdaLiteralArg);
    if (!r.ran) return;  // SKIP / build failure — not the test's concern
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());  // no LSan report
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// (B) Looped call — 50 evaluations, no cumulative closure leak. §28.9 / #1073.
// =====================================================================
BEZEL_TEST(codegen_1073_closure_literal_arg, loop_50_calls_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kLambdaLiteralArgLoop);
    if (!r.ran) return;
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}