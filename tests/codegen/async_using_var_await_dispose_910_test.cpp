// Codegen — issue #910 (discovery sweep #7): `using var c := await <OwnedTask<T>>`
// inside an async method did not dispose the owned T at scope end (a leak).
// usingVarOwnsClassObjectDynamically only inspected a CallExpr initializer, so
// the `await` UnaryExpr fell through and no disposal was registered. The
// `await using var` (async-dispose) and explicit `c.Dispose()` forms both worked,
// confirming the object and runtime paths are sound — only the synchronous
// `using var` discharge of an awaited owned result was missing. §30.5 Pattern B /
// §28.10.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// `using var c := await Make()` (Make returns OwnedTask<Cell>) disposes c.
BEZEL_TEST(async_using_var_await_dispose_910, using_var_awaited_owned_task_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Make(): OwnedTask<Cell> return new Cell(); end Make; "
        "  public static async method Run(): Task<int> "
        "    using var c := await C.Make(); "
        "    return 0; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var r: int := await C.Run(); "
        "    return Cell.n; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// Control: the `await using var` (async-dispose) form still disposes.
BEZEL_TEST(async_using_var_await_dispose_910, await_using_var_still_disposes) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Make(): OwnedTask<Cell> return new Cell(); end Make; "
        "  public static async method Run(): Task<int> "
        "    await using var c := await C.Make(); "
        "    return 0; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var r: int := await C.Run(); "
        "    return Cell.n; "  // 0
        "  end Main; "
        "end C;"), 0);
}
