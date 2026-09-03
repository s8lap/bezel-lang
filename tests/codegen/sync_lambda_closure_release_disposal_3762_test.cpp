// Codegen / leak — issue #3762 (spec §27.5 "Closures", §28.9 "Runtime-
// managed types", v0.286): a lambda-typed LOCAL declared and invoked inside
// a SYNC (non-async) lambda's own body leaks its 40-byte closure cell
// allocation — the closure cell for the inner lambda-local is never
// released. A SINGLE level of such nesting is enough to leak; no capture at
// all is required (unlike #3757's sibling family, this fires even for a
// non-capturing lambda-local).
//
// THIRD member of the same Fn-keyed-token family as #3738
// (`moveOnlyArrayTransferredFn_`) and #3757 (`cellFreeEligibleFn_` /
// `boxCaptureLocalsFn_`): `closureReleaseEligibleFn_`, never re-pinned for
// the sync-lambda venue either. `emitAsyncLambda`
// (`LlvmBackend_Async.cpp:1961-1969`) DOES call
// `collectClosureReleaseEligible(lam.stmts); closureReleaseEligibleFn_ =
// resumeFn;` for an async lambda's own body — which is why a lambda-local
// declared directly inside an ASYNC lambda is clean — but `lowerLambda`'s
// SYNC branch had no equivalent call, so `closureReleaseEligibleFn_` stayed
// whatever the ENCLOSING function last pinned while a sync lambda body was
// mid-emission: a mismatch for any lambda-local declared in that body, so
// its closure release silently never registered (fail-safe direction: a
// leak, never a double-free).
//
// FIXED BY THE SAME COMMIT AS #3757: reusing `CaptureReclaimSetScope`
// (LlvmBackend_impl.hpp, added by fix #3500 — no new predicate) around
// `lowerLambda`'s sync branch and re-running `collectClosureReleaseEligible`
// over the lambda's own `stmts`, pinning `closureReleaseEligibleFn_` to the
// lambda's own `fn`, exactly mirrors what #3757 does for the sibling
// capture-cell tokens IN THE SAME BLOCK — one fix closes both issues. See
// `tests/codegen/sync_lambda_capture_cell_disposal_3757_test.cpp` for the
// capture-cell family's own grid.
//
// Every row below is `runAotChecked` (generated-code ASan with
// LeakSanitizer on) and uses a NON-CAPTURING lambda-local — mirroring
// `tests/codegen/async_iterator_lambda_closure_release_3553_test.cpp`'s own
// convention exactly, because a lambda that ALSO captures a local still
// leaks its (separate) capture cell via the #3757 family, which is a
// DIFFERENT leak this file is not testing — so these rows' LSan oracle must
// be able to reach a clean zero. The printed `total` is pure arithmetic
// (unaffected by whether the closure leaks), so it is checked as a
// same-both-sides sanity value; LSan is the oracle that actually
// distinguishes pre-fix from post-fix. 200 activations, full drain.
//
// Measured on r12-cycle-1 + #3699/#3705/#3728/#3738/#3757 (this lane's own
// cherry-pick + fix, BEFORE this commit's closure-release half — same
// scratch-copy swap-and-rebuild method as the #3757 file's header
// documents), `bezelc --sanitize=address`,
// `ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=99`:
//
//     2 levels, SYNC/SYNC (the issue's own repro)   8000 B / 200 allocations
//     3 levels, SYNC/SYNC/SYNC                     16000 B / 400 allocations
//     early return before the inner call             8000 B / 200 allocations
//     enclosing method's own lambda-local after call  8000 B / 200 allocations
//
// AFTER this commit's fix: all four rows are clean (0 leaked, exit 0), and
// the printed totals are unchanged.
// per §27.5, §28.9 (issue #3762)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

const char* kHdr = "module M; import System; ";

const char* kTail =
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

// ── Row 1, the issue's own minimal repro: a SYNC lambda `outer`'s body
// declares and calls a lambda-local `inner` (2 levels; SYNC/SYNC), neither
// capturing anything. inner()=5, outer() returns 5+3=8. 200 * 8 = 1600.
// per §27.5, §28.9 (issue #3762)
BEZEL_TEST(sync_lambda_closure_release_disposal_3762,
           lambda_local_two_levels_deep_releases_its_closure_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var outer := lambda() => do "
        "      var inner := lambda() => 5; "
        "      var r := inner(); "
        "      return r + 3; "
        "    end; "
        "    return outer(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=1600\n");
}

// ── Row 2, 3 LEVELS (SYNC/SYNC/SYNC): `A` declares+calls `B`; `B`'s OWN
// body (also a sync lambda, going through the SAME sync-branch pin)
// declares+calls `C`. Proves the fix's `CaptureReclaimSetScope` save/
// restore composes across TWO nested `lowerLambda` recursions — A's own
// pin, saved/restored around B's own pin (which is itself saved/restored
// around C's own pin) — releasing BOTH B's closure (in A's scope) and C's
// closure (in B's scope), not just the outermost one. Cc()=5, B()=5+1=6,
// A()=6+2=8, Run()=8+3=11. 200 * 11 = 2200. per §27.5 (issue #3762)
BEZEL_TEST(sync_lambda_closure_release_disposal_3762,
           lambda_local_three_levels_deep_releases_both_closures_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var A := lambda() => do "
        "      var B := lambda() => do "
        "        var Cc := lambda() => 5; "
        "        return Cc() + 1; "
        "      end; "
        "      return B() + 2; "
        "    end; "
        "    return A() + 3; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=2200\n");
}

// ── Row 3, EARLY RETURN: the `return` that exits the outer sync lambda's
// body sits textually BEFORE the fall-through point, proving the
// return-handler's disposal walk sees the closure-release registration
// this fix adds, not only the fall-through path. inner()=5, early return
// yields 5+7=12. 200 * 12 = 2400. per §27.5, §28.3 (issue #3762)
BEZEL_TEST(sync_lambda_closure_release_disposal_3762,
           early_return_releases_the_inner_closure_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var outer := lambda() => do "
        "      var inner := lambda() => 5; "
        "      if true then return inner() + 7; end; "
        "      return 0; "
        "    end; "
        "    return outer(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=2400\n");
}

// ── Row 4, the SAVE/RESTORE half (LANE_RULES §8): the ENCLOSING method
// declares its OWN lambda-local `later` AFTER calling a sync lambda
// (`outer`) that also declares a lambda-local. Without
// `CaptureReclaimSetScope` restoring `closureReleaseEligibleFn_` to Run's
// own value on return from `outer()`, `later`'s own registration would be
// tested against `outer`'s stale token (currentFn_ == RunFn !=
// closureReleaseEligibleFn_ == outerFn) and leak. r = inner()+3 = 5+3 = 8;
// later() = 9; Run() returns 8+9=17. 200 * 17 = 3400.
// per §27.5, §28.3 (issue #3762)
BEZEL_TEST(sync_lambda_closure_release_disposal_3762,
           enclosing_method_restores_its_own_closure_release_token_after_the_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Run(): int "
        "    var outer := lambda() => do "
        "      var inner := lambda() => 5; "
        "      return inner() + 3; "
        "    end; "
        "    var r := outer(); "
        "    var later := lambda() => 9; "
        "    return r + later(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; " +
        kTail),
        "total=3400\n");
}
