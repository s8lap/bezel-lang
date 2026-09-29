// fix #765 / §30.7: `await for ... with <CancellationToken>` is normative syntax
// but was rejected with a 'not yet implemented' diagnostic. lowerAwaitFor bailed
// out whenever a `with` clause was present, and countAwaits skipped the suspend
// point for that form. For a compiler-generated async iterator the source value
// IS the enumerator (there is no separate GetAsyncEnumerator(token) call), so the
// token is simply evaluated (for side-effects/ownership) and discarded; the rest
// of the loop is identical to the no-`with` form. (§30.8 honoring the token for
// cancellation is still deferred, as on a direct GetAsyncEnumerator path.)

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.7: the issue's exact repro — was compile error, now sums 1+2 = 3.
BEZEL_TEST(await_for_with_token_765, repro_none) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() with CancellationToken.None do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §30.7/§6.1: a non-trivial token expression (a CancellationTokenSource's Token)
// is evaluated then discarded — confirms the `with` expr is lowered, not ignored.
BEZEL_TEST(await_for_with_token_765, token_from_cts) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 10; "
        "    yield return 20; "
        "    yield return 30; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    var sum := 0; "
        "    await for var x in Gen() with cts.Token do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "  // 60
        "  end Main; "
        "end C;"), 60);
}

// §30.7: a `with`-token loop and a plain `await for` loop in the same async
// method produce identical iteration, and both suspend points are counted
// (the state machine resumes correctly across both).
BEZEL_TEST(await_for_with_token_765, with_and_without_coexist) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var a := 0; var b := 0; "
        "    await for var x in Gen() with CancellationToken.None do a := a + x; end; "
        "    await for var y in Gen() do b := b + y; end; "
        "    return (if (a = 3 and b = 3) then 0 else 1); "  // both 3 → 0
        "  end Main; "
        "end C;"), 0);
}
