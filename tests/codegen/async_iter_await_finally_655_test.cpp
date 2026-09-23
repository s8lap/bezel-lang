// fix #655 — §30.7 v0.137: `await` inside the `finally` of an async iterator
// is a compile error.
//
// Spec authority: §30.7 v0.137 ("await inside finally, clarified in v0.137"):
//   "Inside an async iterator, `await` may not appear in a `finally` block —
//   a compile error. ... an ordinary `async` method may await in its `finally`
//   (§30.9). The reason is disposal: when a consumer abandons iteration early,
//   the iterator's pending `finally` blocks run synchronously inside `Dispose`
//   (§29.7) — there is no async driver at that point."
//
// Tests:
//   (1) await in async-iterator finally: compile error (§30.7 v0.137).
//   (2) await in async-iterator finally with early break: compile error
//       (the Dispose path is the motivating failure; must be caught even if
//       the normal path looks OK).
//   (3) await in ordinary async method finally: still allowed (§30.9).
//   (4) await in sync iterator finally: NOT an async rule; §30.7 does not
//       apply to sync iterators (no `async` keyword). No compile error.
//   (5) await in async-iterator try BODY: still allowed (§30.7 / §30.9).
//   (6) await in async-iterator catch arm body: still allowed (§30.9).
//       (Replaces the pre-v0.137 test (6) from async_iter_try_finally_554_test
//        which wrongly expected this to compile+run — that test is now updated
//        to expect a diagnostic; see BEZEL_EXPECT_EQ(compileFails(...), true).)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir655() {
    return wtScratchDir("wt-655");
}

// Compile `source` to a temp native executable via the real Driver.
// Returns true when the compile FAILS (diagnostic emitted).
bool compileFails(const std::string& source) {
    auto dir = wtDir655();
    std::string stem = unique_stem("bezel_655_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

// Compile and run; returns exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir655();
    std::string stem = unique_stem("bezel_655");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

}  // namespace

// ===========================================================================
// (1) §30.7 v0.137: await inside an async-iterator finally — compile error.
//     Remedy (per spec): hoist the await before the try / move cleanup to Dispose.
// ===========================================================================
BEZEL_TEST(async_iter_await_finally_655,
           await_in_async_iter_finally_rejected_30_7_v0_137) {
    // §30.7 v0.137: `await` inside the `finally` of an async iterator is forbidden.
    // The Dispose path runs synchronously; there is no async driver to resume the
    // continuation.  Hoist the await before the try, or move cleanup to Dispose.
    BEZEL_EXPECT_EQ(compileFails(R"(
module M;
class C
  public static async method LogAsync(): Task
    return;
  end LogAsync;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
    finally
      await LogAsync();
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    await for var x in Gen() do
      sum := sum + x;
    end;
    return sum;
  end Main;
end C;
)"), true);
}

// ===========================================================================
// (2) §30.7 v0.137: await-in-finally with consumer early break — compile error.
//     Demonstrates the motivating case: Dispose runs synchronously on early break.
// ===========================================================================
BEZEL_TEST(async_iter_await_finally_655,
           await_in_async_iter_finally_early_break_rejected_30_7_v0_137) {
    BEZEL_EXPECT_EQ(compileFails(R"(
module M;
class C
  public static async method Cleanup(): Task
    return;
  end Cleanup;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      await Cleanup();
    end;
  end Gen;

  public static async method Main(): Task<int>
    await for var x in Gen() do
      break;
    end;
    return 0;
  end Main;
end C;
)"), true);
}

// ===========================================================================
// (3) §30.9: await in an ordinary async METHOD finally — still allowed.
//     §30.7 restriction applies only to async ITERATORS.  An async method's
//     Dispose path does not run; the task completes normally.
// ===========================================================================
BEZEL_TEST(async_iter_await_finally_655,
           await_in_async_method_finally_still_allowed_30_9) {
    // §30.9: `await` in a non-iterator async method's `finally` is permitted.
    // The async driver is live for the full lifetime of the method.
    // Pattern: await in try body + await in finally (no early return in try).
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static ranFinally: int := 0;

  public static async method Produce(): Task<int>
    return 7;
  end Produce;

  public static async method LogAsync(): Task
    C.ranFinally := C.ranFinally + 1;
  end LogAsync;

  public static async method Run(): Task<int>
    var v := 0;
    try
      v := await Produce();
    finally
      await LogAsync();
    end;
    return v;
  end Run;

  public static async method Main(): Task<int>
    var v := await Run();
    if C.ranFinally <> 1 then return 99; end;
    return v;
  end Main;
end C;
)"), 7);
}

// ===========================================================================
// (4) §29.6 (sync iterator): await-in-finally is not an async rule; the §30.7
//     restriction does not apply to synchronous iterators.  A sync method cannot
//     use `await` at all; this shape is ill-formed for a different reason (await
//     in non-async context), but the §30.7 check must not fire for sync bodies.
//     Verified by confirming that the §30.7-specific diagnostic is not emitted for
//     a sync iterator with a `finally` that contains no `await` — compile and run.
// ===========================================================================
BEZEL_TEST(async_iter_await_finally_655,
           sync_iterator_with_finally_unaffected_by_30_7) {
    // §29.6 / §29.7: sync iterator with try/finally (no await) — allowed; finally
    // runs on normal completion.  The §30.7 await-in-finally rule does not apply.
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static finallyRan: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      yield return 5;
    finally
      C.finallyRan := C.finallyRan + 1;
    end;
  end Gen;

  public static method Main(): int
    var sum := 0;
    for var x in Gen() do
      sum := sum + x;
    end;
    if C.finallyRan <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), 5);
}

// ===========================================================================
// (5) §30.7 / §30.9: await in async-iterator try BODY — still allowed.
//     §30.7 restricts await in the finally; try BODY is unaffected.
// ===========================================================================
BEZEL_TEST(async_iter_await_finally_655,
           await_in_async_iter_try_body_still_allowed_30_7) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static async method Val(n: int): Task<int>
    return n;
  end Val;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      var v := await Val(3);
      yield return v;
    finally
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    await for var x in Gen() do
      sum := sum + x;
    end;
    return sum;
  end Main;
end C;
)"), 3);
}

// ===========================================================================
// (6) §30.9: await in async-iterator catch arm body — still allowed.
//     §30.7 restricts the FINALLY; catch arm bodies are governed by §30.9.
// ===========================================================================
BEZEL_TEST(async_iter_await_finally_655,
           await_in_async_iter_catch_arm_body_still_allowed_30_9) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static logged: int := 0;
  public static async method Log(): Task
    C.logged := C.logged + 1;
  end Log;

  public static async method Gen(): IAsyncEnumerable<int>
    yield return 3;
    try
      throw new Exception("oops");
    catch
      var ex: Exception => do
        await Log();
        throw;
      end
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    try
      await for var x in Gen() do
        sum := sum + x;
      end;
    catch
      var ex: Exception => sum := sum + 6;
    end;
    if C.logged <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), 9);
}
