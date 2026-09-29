// fix #790 / §30.8: `await for … with <token>` now HONORS the CancellationToken
// (the deferred remainder of #765/#757, which made the form compile + iterate but
// evaluated and discarded the token). At each iteration's suspend point the
// loop observes the token (bz_ctoken_is_cancelled); a cancelled token disposes
// the iterator frame and raises a catchable OperationCanceledException (§30.8 —
// cooperative cancellation, the consumer observes it at the await point).
// CancellationToken.None (a null state cell) never cancels.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
const char* gen =
    "module M; import System.Threading; "
    "class C "
    "  public static async method Gen(): IAsyncEnumerable<int> "
    "    yield return 1; yield return 2; yield return 3; "
    "  end Gen; ";
}  // namespace

// §30.8: a token cancelled before iteration makes the await-for raise
// OperationCanceledException — caught → 42 (the loop body never runs).
BEZEL_TEST(await_for_cancellation_790, cancelled_before_loop_throws) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(gen) +
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); cts.Cancel(); "
        "    var sum := 0; "
        "    try "
        "      await for var x in Gen() with cts.Token do sum := sum + x; end; "
        "    catch var ex: OperationCanceledException => do return 42; end "
        "    end; "
        "    return sum; "
        "  end Main; end C;"), 42);
}

// §30.8: CancellationToken.None never cancels — the loop iterates fully (sum 6).
BEZEL_TEST(await_for_cancellation_790, none_iterates_normally) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(gen) +
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() with CancellationToken.None do sum := sum + x; end; "
        "    return sum; "  // 6
        "  end Main; end C;"), 6);
}

// §30.8: an un-cancelled CancellationTokenSource token iterates fully (sum 6).
BEZEL_TEST(await_for_cancellation_790, uncancelled_cts_iterates) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(gen) +
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    var sum := 0; "
        "    await for var x in Gen() with cts.Token do sum := sum + x; end; "
        "    return sum; "  // 6
        "  end Main; end C;"), 6);
}

// §30.8: cancelling MID-loop stops it at the next iteration's suspend point —
// the body runs for x=1 (sum 1), cancels, and the next advance raises OCE.
BEZEL_TEST(await_for_cancellation_790, cancelled_mid_loop_stops) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(gen) +
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    var sum := 0; "
        "    try "
        "      await for var x in Gen() with cts.Token do "
        "        sum := sum + x; "
        "        if x = 1 then cts.Cancel(); end; "
        "      end; "
        "    catch var ex: OperationCanceledException => do return sum; end "  // 1
        "    end; "
        "    return 99; "
        "  end Main; end C;"), 1);
}
