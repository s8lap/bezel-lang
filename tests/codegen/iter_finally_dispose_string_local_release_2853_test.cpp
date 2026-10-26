// Codegen / refcounting — issue #2853: a `finally` body REPLAYED by an iterator's
// Dispose is lowered in a function NO per-body analysis names, so its own
// `var` string locals leak.
//
// Spec: §29.7 "Iterator disposal and `finally`" (final clause ran on every
// exit from the try — including the abandonment-mid-iteration path the
// consumer's `break` triggers, on which Dispose replays the active finally
// chain); §28.10 "Cleanup ordering"; §4.6 "`string`"; §28.9 "Runtime-managed
// types". Revision v0.240.
//
// THE DEFECT. emitIteratorMethod lowers MoveNext with `stringReleaseEligibleFn_`
// pinned to the MOVENEXT fn (#2817, §4.6/§28.9); the iterator's Dispose fn is
// emitted later in the SAME emitIteratorMethod call, with `currentFn_` set to
// the Dispose fn but `stringReleaseEligibleFn_` STILL the MoveNext fn. When
// Dispose hits an `IterCleanup::Kind::Finally`, it lowers the SAME finally body
// AST (`*ac.finallyBody`, /home/stepan/w/code/bezel-lang/src/codegen/LlvmBackend_Iterators.cpp
// — the file under the #2853 fix) inside the Dispose fn — and EVERY consumer
// of the three string-eligibility sets is gated on
// `currentFn_ == stringReleaseEligibleFn_`. The gate is false, so EVERY
// `var fin := "..."` declared inside that body leaked its bz_string header +
// its data buffer (measured 380 B / 20 alloc for a 10x-abandoned
// Sync-iterator reproducer — the value is correct, no UAF / double-free /
// SEGV, only a BOUNDED leak).
//
// THE FIX mirrors #2817's MOVE-NEXT pin (#1897's unwind pin is set just above
// in the same fn for the body, and #2148/#2142/#2143 the
// borrowing-collection pins) and #2852's destructor pin: save the token, run
// `collectStringReleaseEligible` over `*ac.finallyBody`, pin the token to the
// Dispose fn for the duration of the body lowering, restore — then emit the
// frame's `Disposal{stringRelease}` entries (the same `lowerBody` (vs
// `lowerScopedBlock`) call shape the replay already uses means the per-scope
// emit-on-pop doesn't fire automatically; the filter on `d.stringRelease`
// stays in #2853's SCOPE — the other per-body tokens aren't re-armed for the
// Dispose fn, so a plain owned / closure / borrowing-collection local in a
// replayed `finally` is a separate, broader defect filed in parallel).
//
// THE ACTIVATION MODEL. A MoveNext resume runs ONCE PER ADVANCE, so the bind
// and the scope-end release of a local declared inside the yielding loop land
// in DIFFERENT advances (advance k binds it, suspends at the `yield`; advance
// k+1 resumes just past the yield, runs the loop body's scope-end disposals).
// The `finally` body is lowered TWICE from TWO different functions (once
// inline in MoveNext for the normal/exception edge, once in Dispose for the
// abandonment edge), over the SAME `LocalDecl` AST nodes but different
// allocas. The three eligibility sets are keyed by node, so the two lowerings
// agree by construction — but each registers its release into its OWN
// function's disposeStack_. The abandonment and normal edges are mutually
// exclusive: Dispose stamps the DISPOSED sentinel (-2, §28.2 / §29.7) so a
// later MoveNext cannot resume past the yield; a run-to-exhaustion iterator
// parks at -1, whose switch default skips cleanup. So each release fires
// exactly once — once per local, once per (MoveNext scope or Dispose fn
// replay) — across either exit.
//
// MEASURED, pre-fix vs post-fix. The reproducer in the issue is the (1) test
// below — measured 380 B / 20 pre-fix on `9644baa` (#2817 landed), clean on
// this commit's HEAD. (2) and (3) are the SYNC and ASYNC "full iteration
// then discard" oracle: the finally runs once on the run-to-exhaustion path
// (state -1, switch default skips replay — so the fix does NOT introduce
// a double-free / heap-use-after-free on the NORMAL path). (4) pins the
// SYNC-iterator "bail out of MoveNext before the yield" edge: the iterator
// is never abandoned (MoveNext returns false), the finally runs in its
// inline-lowering place, NOT in Dispose.
//
// ⚠ Every string here is built at RUNTIME ($"it-{i}"). A string
// constant-folded `+"ab"` reduces to an rc sentinel on which
// bz_string_release no-ops — the trap that has made probes in this family
// vacuous repeatedly. Each caller is loop-amplified so the leak shows up
// repeatedly enough for LSan to report.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// Pin the DIRECTION of the defect: a leak is invisible without a sanitizer,
// so this case is the binary-is-FIXED oracle. A passed run also reports no
// heap-use-after-free / double-free / SEGV (the fix turns an existing
// bounded leak into exactly-once releases — the inverse regression a
// broken pair of releases would show).
bool hasMemoryError(const std::string& t) {
    return t.find("heap-use-after-free") != std::string::npos ||
           t.find("double-free") != std::string::npos ||
           t.find("SEGV") != std::string::npos;
}

// (1) The issue's REPRODUCER, verbatim. The `var fin := $"fin-0"` here is the
// LEAF the issue measures: 10 abandoned iterators, each builds the string
// once in the replayed `finally`, the fix releases each once per abandonment.
// Pre-fix on `9644baa` (#2817 landed):
//   "SUMMARY: AddressSanitizer: 380 byte(s) leaked in 20 allocation(s)."
// Post-fix this runs CLEAN (no leak).
// Value: 10 outer × (4+4+4 = 12 each) = 120 + the final Seq(20) full iteration
// sum = 91 (4*9 + 5*11 — like (R)/(S) in
// iterator_lambda_ctor_string_local_release_2817_test.cpp / (k)).
// 120 + 91 = 211.
const char* kIteratorAbandonedWithFinally =
    "module M; class C "
    "  private static method Seq(n: int): IEnumerable<int> "
    "    try "
    "      for var i in 1..n do "
    "        var built := $\"it-{i}\"; "
    "        var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "        if churn.Length < 0 then yield return 99; end; "
    "        yield return built.Length; "
    "      end; "
    "    finally "
    "      var fin := $\"fin-0\"; "
    "      var sink := fin.Length; "
    "      if sink < 0 then sink := 0; end; "
    "    end; "
    "  end Seq; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var r in 1..10 do "
    "      var seen := 0; "
    "      for var v in C.Seq(50) do "
    "        total := total + v; "
    "        seen := seen + 1; "
    "        if seen >= 3 then break; end; "
    "      end; "
    "    end; "
    "    for var v2 in C.Seq(20) do "
    "      total := total + v2; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (2) SYNC iterator: full iteration then discard (consumer read all, then
// abandon). The finally runs once (in MoveNext's inline lowering — not the
// Dispose replay, because state is -1 and Dispose's state switch hits the
// default and skips cleanup). Pin that #2853 introduces no double-free
// on the EXHAUSTION path. `var fin := $"fin-i"` is read once so it cannot be
// optimized out (yield in a finally body is forbidden by §29.6; `return` in
// a finally body is forbidden by §31.2; assignment to a module-static is the
// remaining consume-side option). Sum: 9*4 + 5 (Seq10) + 9*4 + 11*5 (Seq20) = 132.
const char* kSyncExhaustedFinally =
    "module M; class C "
    "  public static finSink: int := 0; "
    "  private static method Seq(n: int): IEnumerable<int> "
    "    try "
    "      for var i in 1..n do "
    "        var built := $\"it-{i}\"; "
    "        yield return built.Length; "
    "      end; "
    "    finally "
    "      var fin := $\"fin-i\"; "
    "      C.finSink := fin.Length; "
    "    end; "
    "  end Seq; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var v in C.Seq(10) do "
    "      total := total + v; "
    "    end; "
    "    for var v2 in C.Seq(20) do "
    "      total := total + v2; "
    "    end; "
    "    if C.finSink <> 5 then return 88; end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (3) ASYNC iterator: abandonment edge, with a `finally` body's own string
// local. The async twin is lowered in `LlvmBackend_Async.cpp`'s
// emitAsyncDisposeBody, with the SAME defect and the SAME shape fix.
const char* kAsyncIteratorAbandonedWithFinally =
    "module M; class C "
    "  private static async method Seq(n: int): IAsyncEnumerable<int> "
    "    try "
    "      for var i in 1..n do "
    "        var built := $\"it-{i}\"; "
    "        var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "        if churn.Length < 0 then yield return 99; end; "
    "        yield return built.Length; "
    "      end; "
    "    finally "
    "      var fin := $\"fin-0\"; "
    "      var sink := fin.Length; "
    "      if sink < 0 then sink := 0; end; "
    "    end; "
    "  end Seq; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var r in 1..10 do "
    "      var seen := 0; "
    "      await for var v in C.Seq(50) do "
    "        total := total + v; "
    "        seen := seen + 1; "
    "        if seen >= 3 then break; end; "
    "      end; "
    "    end; "
    "    await for var v2 in C.Seq(20) do "
    "      total := total + v2; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (4) SYNC iterator, MoveNext returns false on the FIRST call (state 0,
// never-iterated). The finally was never entered (the try body's loop has
// not run yet), so the replay path doesn't fire; the finally's own body —
// and ITS string local, if any — never runs. Pin a clean `0` total: the
// `break` before the first yield means we skip the iterator entirely.
const char* kSyncNeverIteratedFinally =
    "module M; class C "
    "  private static method Seq(): IEnumerable<int> "
    "    try "
    "      for var i in 1..10 do "
    "        var built := $\"it-{i}\"; "
    "        yield return built.Length; "
    "      end; "
    "    finally "
    "      var fin := $\"fin-i\"; "
    "    end; "
    "  end Seq; "
    "  public static method Main(): int "
    "    var gen := Seq(); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// (1) §29.7 / §4.6 / §28.9 — abandonment. Each `for .. in C.Seq(50) .. break`
//     abandons a suspended iterator; Dispose replays the in-flight `finally`
//     body. Pre-fix: 380 B / 20 leaked string headers + payloads. Post-fix:
//     clean. Plain value oracle: the program returns the SUM of the consumed
//     lengths and exits 0. // per §29.7, §4.6, §28.9.
// ===========================================================================
BEZEL_TEST(codegen_iter_finally_dispose_string_local_release_2853,
           abandoned_sync_iterator_finally_body_string_local_releases_spec_29_7) {
    JitResult r = runAotChecked(kIteratorAbandonedWithFinally);
    BEZEL_EXPECT_TRUE(r.ran);              // 380 B / 20 pre-fix
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_EQ(t.find("detected memory leaks"), std::string::npos);
    BEZEL_EXPECT_TRUE(!hasMemoryError(t)); // the fix is exactly-once — no UAF
    BEZEL_EXPECT_EQ(r.exitCode, 211);      // 10*12 + 91
}

// (1b) the plain (unsanitized) value-oracle — a freed-then-reused block would
// change the value observed by the second MoveNext, so the plain path, while
// weaker than the ASan path, pins the value the ASan path is checking.
BEZEL_TEST(codegen_iter_finally_dispose_string_local_release_2853,
           abandoned_sync_iterator_finally_body_value_invariant_spec_29_7) {
    JitResult r = runAot(kIteratorAbandonedWithFinally);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 211);      // 10*12 + 91
}

// ===========================================================================
// (2) §29.7 / §4.6 / §28.9 — full-iteration exhaustion. Dispose's state
//     switch hits the `-1` default and skips the finally chains, so the fix's
//     new replay emit must not fire on the EXHAUSTION path (no double-free,
//     no use-after-free) and must not change the value. // per §29.7.
// ===========================================================================
BEZEL_TEST(codegen_iter_finally_dispose_string_local_release_2853,
           exhausted_sync_iterator_finally_body_releases_once_spec_29_7) {
    JitResult r = runAotChecked(kSyncExhaustedFinally);
    BEZEL_EXPECT_TRUE(r.ran);
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_EQ(t.find("detected memory leaks"), std::string::npos);
    BEZEL_EXPECT_TRUE(!hasMemoryError(t));
    BEZEL_EXPECT_EQ(r.exitCode, 132);      // 41 (Seq10) + 91 (Seq20)
}

// ===========================================================================
// (3) §30.7 / §29.7 / §4.6 / §28.9 — the ASYNC twin. emitAsyncDisposeBody
//     in `LlvmBackend_Async.cpp` plays the same shape; the SAME defect and
//     the SAME fix apply. // per §30.7, §29.7.
// ===========================================================================
BEZEL_TEST(codegen_iter_finally_dispose_string_local_release_2853,
           abandoned_async_iterator_finally_body_string_local_releases_spec_30_7) {
    JitResult r = runAotChecked(kAsyncIteratorAbandonedWithFinally);
    BEZEL_EXPECT_TRUE(r.ran);
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_EQ(t.find("detected memory leaks"), std::string::npos);
    BEZEL_EXPECT_TRUE(!hasMemoryError(t));
    BEZEL_EXPECT_EQ(r.exitCode, 211);      // 10*12 + 91
}

// ===========================================================================
// (4) §29.7 / §4.6 / §28.9 — `never-iterated` edge. The iterator's MoveNext
//     is never called (the consumer constructs the source and discards it
//     without iterating). Dispose's state switch hits `0` (never-iterated,
//     not suspended at a yield), and no finally chain is active. The fix
//     must NOT touch this path. // per §29.7.
// ===========================================================================
BEZEL_TEST(codegen_iter_finally_dispose_string_local_release_2853,
           never_iterated_iterator_dispose_is_clean_spec_29_7) {
    JitResult r = runAot(kSyncNeverIteratedFinally);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
