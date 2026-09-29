// fix #792 / §29.7 / §30.7: `await for` disposes the async-iterator enumerator
// frame on EVERY exit edge — normal exhaustion, `break`, `return`, and
// exception-unwind (a throw in the loop body) — not only on normal completion.
// Previously the frame was disposed only at the loop's normal-exit block, so a
// body that threw / broke / returned leaked the iterator frame (its pending
// `finally` blocks never ran and the frame was never freed).
//
// Observed via an async iterator whose body has a `try/finally`: disposing the
// frame runs the `finally` (§29.7 — disposing a partially-consumed iterator runs
// its pending finally blocks). A static flag records whether it ran. runJit
// captures the outcome (caught exceptions and returned values) via the exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// An async iterator whose body sets `T.cleaned := true` in a `finally`, so the
// flag is set exactly when the frame is disposed.
const char* prelude =
    "module M; import System.Threading; "
    "class T public static cleaned: bool := false; end T; "
    "class C "
    "  public static async method Gen(): IAsyncEnumerable<int> "
    "    try "
    "      yield return 1; "
    "      yield return 2; "
    "      yield return 3; "
    "    finally "
    "      T.cleaned := true; "
    "    end; "
    "  end Gen; ";
}  // namespace

// §29.7: a throw in the loop body disposes the frame during unwind — the
// iterator's finally runs, so T.cleaned is true after the (caught) exception.
BEZEL_TEST(await_for_frame_dispose_792, body_throw_disposes_frame) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(prelude) +
        "  public static async method Main(): Task<int> "
        "    var caught := false; "
        "    try "
        "      await for var x in Gen() do throw new Exception(\"boom\"); end; "
        "    catch Exception => do caught := true; end "
        "    end; "
        "    return (if T.cleaned then 1 else 0); "   // 1: frame disposed on the throw
        "  end Main; end C;"), 1);
}

// §29.7: `break` out of the loop disposes the frame (its finally runs).
BEZEL_TEST(await_for_frame_dispose_792, break_disposes_frame) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(prelude) +
        "  public static async method Main(): Task<int> "
        "    await for var x in Gen() do "
        "      if x = 1 then break; end; "
        "    end; "
        "    return (if T.cleaned then 1 else 0); "   // 1: frame disposed on break
        "  end Main; end C;"), 1);
}

// §30.7 control: normal exhaustion also disposes the frame (the finally runs),
// and the loop sums all yielded values (1+2+3 = 6) — pins the rewrite to a
// frame-backed enumerator slot did not change normal iteration.
BEZEL_TEST(await_for_frame_dispose_792, normal_completion_disposes_and_iterates) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(prelude) +
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum + (if T.cleaned then 100 else 0); "  // 6 + 100 = 106
        "  end Main; end C;"), 106);
}
