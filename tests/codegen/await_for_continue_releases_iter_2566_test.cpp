// Codegen — fix #2566 (spec §29.2 / §29.7 / §30.7, v0.236): `continue` in
// the body of an `await for var v in <IAsyncEnumerable<T>>` must NOT free the
// iterator frame mid-loop. Before the fix, `lowerAwaitFor` pushed onto
// `loopStack_` with `scopeDepth = outerDepth` (the depth BEFORE the
// enumerator's own disposal scope was pushed), so on `continue` the disposal
// walk reached the enumerator's slot, `iterEnumDispose` FREED the iterator
// frame, and the very next condBB re-loaded the frame pointer and read freed
// memory → heap-use-after-free (`READ` in `bz$async$Main$Resume`, `free` in
// `bz$asynciter$Gen$Dispose`).
//
// Parallel to #2556 (the sync IEnumerable<T> + `continue` SEGV fixed in the
// same wave). The sync bug surfaced as a NULL-deref at address 0 because the
// sync path nulls the enumSlot on iterEnumDispose; the async path uses
// `iterEnumDispose = true` to free the FRAME itself, so the same defect
// surfaces one level later as a heap-use-after-free inside the resume
// function — symptom report in the issue matches exactly.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Stmts.cpp, lowerAwaitFor): the
// `loopStack_.push_back({condBB, endBB, outerDepth})` at line ~7361 with
// `outerDepth` recorded BEFORE the enumerator's own
// `disposeStack_.emplace_back()`. On `continue`,
// `emitLoopDisposals(loopStack_.back().scopeDepth)` walked
// `disposeStack_[scopeDepth .. back]` — but `scopeDepth` was the depth ABOVE
// the enumerator's scope, so the walk INCLUDED the enumerator's slot,
// `iterEnumDispose` freed the frame, and the next `condBB` re-loaded the
// frame pointer → heap-use-after-free.
//
// FIX: change to push `disposeStack_.size()` (the depth AFTER the enumerator
// scope was just pushed). The wrapper/enumerator slots are loop-scope
// resources that must outlive every `continue`; `break` and normal
// exhaustion still dispose them via the endBB path
// (`emitDisposals(disposeStack_.back())`). Mirrors the existing
// `lowerIEnumerableForSortedSet` (#2553) and the post-#2556 form in
// `lowerIEnumerableFor` / `lowerIEnumerableBuiltinFor`.
//
// Fail-without / pass-with: every test below is observed as an
// AddressSanitizer `heap-use-after-free` (READ in bz$async$Main$Resume,
// free in bz$asynciter$Gen$Dispose) on the unfixed compiler under
// `BEZEL_AOT_FORCE_SANITIZE=address` AOT. On the fixed compiler every
// program returns the asserted value with no ASan error.
//
// Spec citations (§29.7 v0.236, the general "Disposal of partially-consumed
// iterators" rule, which governs both the sync and async for-in): "Runs the
// pending cleanup actions active at the point of suspension — `using var`
// disposals ... in reverse order of establishment, exactly as if the body
// had exited at the suspension point." The `outerDepth` form ran the
// enumerator's disposal on `continue`, which is wrong (the enumerator is a
// LOOP resource, not a body resource — it must outlive the body). The fix
// aligns `lowerAwaitFor` with `lowerIEnumerableForSortedSet`'s
// `disposeStack_.size()` (post-push) form.

#include "support/aot_support.hpp"   // runAot — AOT + ASan oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A SEGV or ASan-detected UAF under AOT shows as a non-zero exit and
// r.ran==true (the kernel propagated the signal / sanitizer into a real
// exit code we can assert on, distinct from any legal value the program
// would return). ASan reports a heap-use-after-free with READ in
// bz$async$Main$Resume; the program never reaches its `return`.
int run(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return -999;
    return r.exitCode;
}

// An async iterator that yields 1..5 — a non-trivial frame with a
// pending-finally free path on Dispose (so iterEnumDispose actually frees
// the frame's allocator handle, exercising the bug). Mirrors the
// await_for_frame_dispose_792_test.cpp prelude.
const char* kGen =
    "module M; "
    "class C "
    "  public static async method Gen(): IAsyncEnumerable<int> "
    "    yield return 1; "
    "    yield return 2; "
    "    yield return 3; "
    "    yield return 4; "
    "    yield return 5; "
    "  end Gen; ";

}  // namespace

// =============================================================================
// §29.2 / §29.7 / fix #2566 (v0.236): the issue's exact shape — `continue` in
// the body of an `await for var v in Gen()`. Pre-fix: condBB re-loads the
// freed iterator frame after Dispose frees it on `continue` → ASan reports
// heap-use-after-free. Post-fix: the frame survives the continue, the next
// MoveNext reads a live frame, the loop terminates cleanly. Sum = 1+2+4+5
// = 12 (3 skipped by continue).
// =============================================================================

BEZEL_TEST(codegen_await_for_continue_releases_iter_2566,
           await_for_continue_does_not_uaf_spec_29_7) {
    std::string src = std::string(kGen) +
        "  public static async method Main(): Task<int> "
        "    var sum: int := 0; "
        "    await for var v in Gen() do "
        "      if v = 3 then continue; end; "
        "      sum := sum + v; "
        "    end; "
        "    return sum; "                  // 12
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 12);
}

// REGRESSION GUARD: a `break` out of the same async-iterator for-in must
// still dispose the iterator frame (its pending finally / Dispose runs).
// Pre-fix did dispose the frame (the wrong-but-sound code path on `break`);
// post-fix must still dispose it (the endBB path runs
// `emitDisposals(disposeStack_.back())` exactly once on `break`). 1+2+3 = 6
// (break at 4).
BEZEL_TEST(codegen_await_for_continue_releases_iter_2566,
           await_for_break_still_works_spec_29_7) {
    std::string src = std::string(kGen) +
        "  public static async method Main(): Task<int> "
        "    var sum: int := 0; "
        "    await for var v in Gen() do "
        "      if v >= 4 then break; end; "
        "      sum := sum + v; "
        "    end; "
        "    return sum; "                  // 6
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// REGRESSION GUARD: normal exhaustion (no break, no continue) must still
// dispose the iterator frame exactly once at endBB. 1+2+3+4+5 = 15.
BEZEL_TEST(codegen_await_for_continue_releases_iter_2566,
           await_for_exhausts_releases_iter_spec_29_7) {
    std::string src = std::string(kGen) +
        "  public static async method Main(): Task<int> "
        "    var sum: int := 0; "
        "    await for var v in Gen() do sum := sum + v; end; "
        "    return sum; "                  // 15
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 15);
}

// NESTED: an `await for var v in Gen()` whose body has a regular inner
// `while`-loop with `continue` must not free the async iterator frame on
// each inner continue (the outer await-for's frame is a loop-scope resource
// that must outlive every inner `continue`). This is the same code path as
// the issue's repro but with an inner synchronous loop, isolating the await-
// for fix from any orthogonal nested-async-iterator concerns. Each outer
// iteration runs the inner while for j=0,1,2 (j=2 skipped by continue), so
// the inner loop adds v twice per outer iteration → sum = 2*(1+2+3+4+5)
// = 30.
BEZEL_TEST(codegen_await_for_continue_releases_iter_2566,
           await_for_nested_continue_spec_29_7) {
    std::string src = std::string(kGen) +
        "  public static async method Main(): Task<int> "
        "    var sum: int := 0; "
        "    await for var v in Gen() do "
        "      var j: int := 0; "
        "      while j < 3 do "
        "        if j = 2 then j := j + 1; continue; end; "
        "        sum := sum + v; "
        "        j := j + 1; "
        "      end; "
        "    end; "
        "    return sum; "                  // 30
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 30);
}