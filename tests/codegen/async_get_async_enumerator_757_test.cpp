// fix #757 / §30.7: a direct `src.GetAsyncEnumerator(token)` on a builtin
// IAsyncEnumerable<T> result. Previously SIGSEGV'd: sema left the call untyped,
// so the `await using` slot was sized i32 (not ptr) and scope-exit disposal
// loaded a garbage pointer and dispatched through it. Now sema types the call as
// IAsyncEnumerator<T> (so the slot is ptr-sized and disposal routes to the
// iterator-format slot-1 Dispose) and codegen returns the source frame — which,
// for a compiler-generated async iterator, IS the enumerator (the same identity
// the `await for` desugaring uses).

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

// §30.7: the issue's exact repro — was runtime SIGSEGV (exit 139), now reaches
// `return 5`.
BEZEL_TEST(async_get_async_enumerator_757, repro_no_crash) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var source := Gen(); "
        "    await using var en := source.GetAsyncEnumerator(CancellationToken.None); "
        "    return 5; "
        "  end Main; "
        "end C;"), 5);
}

// §30.7: the enumerator returned by GetAsyncEnumerator is a real, working
// enumerator — drive it through the manual MoveNextAsync()/Current protocol and
// read back the yielded values.
BEZEL_TEST(async_get_async_enumerator_757, drives_iteration) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 42; "
        "    yield return 99; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var source := Gen(); "
        "    await using var en := source.GetAsyncEnumerator(CancellationToken.None); "
        "    var m1 := await en.MoveNextAsync(); "
        "    if not m1 then return 1; end; "
        "    if en.Current <> 42 then return 2; end; "
        "    var m2 := await en.MoveNextAsync(); "
        "    if not m2 then return 3; end; "
        "    if en.Current <> 99 then return 4; end; "
        "    var m3 := await en.MoveNextAsync(); "
        "    if m3 then return 5; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §30.7: the inlined form (no intermediate `source` local) — Gen() produces the
// frame and GetAsyncEnumerator returns it directly into the await-using owner.
BEZEL_TEST(async_get_async_enumerator_757, inlined_source) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 7; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var en := Gen().GetAsyncEnumerator(CancellationToken.None); "
        "    var ok := await en.MoveNextAsync(); "
        "    if not ok then return 1; end; "
        "    return en.Current; "  // 7
        "  end Main; "
        "end C;"), 7);
}
