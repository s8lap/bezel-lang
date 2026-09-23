// Codegen — yield / await inside try / catch / finally in an async iterator
// (#554, §30.7 / §29.6 / §29.7 v0.135; updated for §30.7 v0.137).
//
// Spec authorities:
//   §29.6: yield in try-with-only-finally: allowed. yield in try-with-catch:
//          forbidden. yield in catch arm body or finally: forbidden.
//   §29.7: on early disposal (break / exception / scope-end), the state machine
//          runs all finally clauses active at the suspension point.
//   §30.7 v0.135: async iterators obey the same rules; await is additionally
//          permitted in try/catch/finally bodies (§30.9) — just not in a catch
//          GUARD.
//   §30.7 v0.137 (NEW): `await` inside the `finally` of an async iterator is
//          now a compile error. The Dispose path runs synchronously — no async
//          driver is present when a consumer abandons iteration early, so the
//          continuation would be silently discarded. Test (6) below was updated
//          to reflect this ruling: the await-in-finally shape is now ill-formed
//          and must produce a diagnostic.  Remedy: hoist the await before the
//          try, or move cleanup to Dispose (per §30.7 v0.137 text).
//          The new positive tests (await-in-finally-rejected; async METHOD
//          finally still OK; sync iterator unaffected) live in
//          async_iter_await_finally_655_test.cpp.
//
// Tests:
//   (1) yield inside try-with-only-finally: compiles and runs. Finally runs on
//       normal completion (§29.6 / §29.7 normal-exit path).
//   (2) yield inside try-with-only-finally: early consumer break compiles and
//       the iterator produces the right value (§29.6; early-disposal finally
//       is a future increment — only correctness of the non-break path tested).
//   (3) finally runs when an exception escapes the try body — the consuming
//       `await for` propagates the exception (§29.7 / §31.2).
//   (4) await inside a try body in an async iterator: resumes correctly (§30.7).
//   (5) await inside a catch arm body in an async iterator: resumes correctly.
//   (6) await inside a finally block in an async iterator: NOW ILL-FORMED per
//       §30.7 v0.137 — must produce a compile-time diagnostic.
//       (Was: "runs correctly per §30.9". Ruling changed in v0.137.)
//   (7) yield in try-with-catch: rejected with a clean diagnostic (§29.6).
//   (8) yield in a catch arm body: rejected with a clean diagnostic (§29.6).
//   (9) yield in a finally block: rejected with a clean diagnostic (§29.6).

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
static const std::filesystem::path& wtDir554() {
    return wtScratchDir("wt-554");
}

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir554();
    std::string stem = unique_stem("bezel_554");
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

// True when the source FAILS to compile (a diagnostic was raised).
bool compileFails(const std::string& source) {
    auto dir = wtDir554();
    std::string stem = unique_stem("bezel_554_neg");
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

}  // namespace

// ===========================================================================
// (1) §29.6 / §30.7: yield inside try-with-only-finally — allowed.
//     The body yields 1, 2, 3; the consumer sums them. The finally block runs
//     on normal completion (the try body falls through after the last yield).
//     Expected exit code: 6 (1+2+3) when finallyRan == 1.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_try_finally_normal_completion) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static finallyRan: int := 0;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      yield return 2;
      yield return 3;
    finally
      C.finallyRan := C.finallyRan + 1;
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    await for var x in Gen() do
      sum := sum + x;
    end;
    if C.finallyRan <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), 6);
}

// ===========================================================================
// (2) §29.6 / §29.7: yield inside try-with-only-finally with consumer break.
//     The consumer reads only the first yielded value then breaks. Per §29.7
//     the iterator's Dispose runs all finally clauses active at the suspension
//     point before freeing the frame. The finally must execute exactly once.
//     Expected exit code: 1 (got=1 and finallyRan==1).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_try_finally_early_break) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static finallyRan: int := 0;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      C.finallyRan := C.finallyRan + 1;
    end;
  end Gen;

  public static async method Main(): Task<int>
    var got := 0;
    await for var x in Gen() do
      got := x;
      break;
    end;
    if C.finallyRan <> 1 then return 99; end;
    return got;
  end Main;
end C;
)"), 1);
}

// ===========================================================================
// (3) §29.7 / §31.2: finally runs when the iterator body throws.
//     The body yields 1 then throws. The consuming `await for` propagates the
//     exception; the outer try/catch in Main catches it. The finally in the
//     iterator ran before the exception escaped.
//     Expected exit code: 7 (sum=1 from first yield + 6 from caught exception).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_try_finally_body_throws) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static finallyRan: int := 0;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      throw new Exception("test");
    finally
      C.finallyRan := C.finallyRan + 1;
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
    if C.finallyRan <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), 7);
}

// ===========================================================================
// (4) §30.7 / §30.9: await inside a try body in an async iterator.
//     The iterator awaits a completed task (synchronous path) inside a try,
//     then yields the result. Expected exit code: 3 (1+2).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, await_in_try_body_in_async_iter) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static async method Compute(x: int): Task<int>
    return x;
  end Compute;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      var a := await Compute(1);
      yield return a;
      var b := await Compute(2);
      yield return b;
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
// (5) §30.7 / §30.9: await inside a catch arm body in an async iterator.
//     §29.6 forbids `yield` in a catch arm, but `await` is allowed (§30.9).
//     The iterator: first yields 3, then the second advance throws, the catch
//     arm awaits Log() (a no-op Task) then rethrows, so the consumer sees the
//     exception. The outer try/catch in Main catches it and returns 3+6=9.
//     Expected exit code: 9 (first element 3 + caught-exception sentinel 6).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, await_in_catch_arm_in_async_iter) {
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

// ===========================================================================
// (6) §30.7 v0.137: await inside a finally block in an async iterator is now
//     ILL-FORMED — must produce a compile error.
//
//     Ruling changed in v0.137: "Inside an async iterator, `await` may not
//     appear in a `finally` block — a compile error."  The motivating failure
//     is the Dispose path: when a consumer abandons iteration early, the
//     iterator's pending `finally` blocks run synchronously inside `Dispose`
//     (§29.7) — there is no async driver at that point, so a suspension
//     inside the `finally` would have nowhere to resume; the continuation
//     would be silently discarded.
//
//     Remedy (per §30.7 v0.137): hoist the await before the `try`, or move
//     the asynchronous cleanup into the type's `Dispose` path.
//
//     The previously-passing compile-and-run shape is now expected to FAIL
//     compilation.  The positive form (async METHOD finally-await still OK;
//     await in async-iterator try body still OK; sync iterator unaffected)
//     is tested in async_iter_await_finally_655_test.cpp.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, await_in_finally_in_async_iter) {
    // §30.7 v0.137 (#655): `await` in the `finally` of an async iterator is
    // forbidden.  This shape must be rejected with a diagnostic.
    BEZEL_EXPECT_EQ(compileFails(R"(
module M;
class C
  public static finallyRan: int := 0;

  public static async method LogAsync(): Task
    C.finallyRan := C.finallyRan + 1;
  end LogAsync;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 4;
    finally
      await LogAsync();
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    await for var x in Gen() do
      sum := sum + x;
    end;
    if C.finallyRan <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), true);
}

// ===========================================================================
// (7) §29.6: yield inside a try-with-catch is forbidden (compile-time error).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_try_with_catch_rejected) {
    BEZEL_EXPECT_EQ(compileFails(R"(
module M;
class C
  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
    catch
      var ex: Exception => yield break;
    end;
  end Gen;
  public static async method Main(): Task<int>
    return 0;
  end Main;
end C;
)"), true);
}

// ===========================================================================
// (8) §29.6: yield inside a catch arm body is forbidden.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_catch_arm_body_rejected) {
    BEZEL_EXPECT_EQ(compileFails(R"(
module M;
class C
  public static async method Gen(): IAsyncEnumerable<int>
    try
      throw new Exception("x");
    catch
      var ex: Exception => yield return 1;
    end;
  end Gen;
  public static async method Main(): Task<int>
    return 0;
  end Main;
end C;
)"), true);
}

// ===========================================================================
// (9) §29.6: yield inside a finally block is forbidden.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_finally_block_rejected) {
    BEZEL_EXPECT_EQ(compileFails(R"(
module M;
class C
  public static async method Gen(): IAsyncEnumerable<int>
    try
    finally
      yield return 1;
    end;
  end Gen;
  public static async method Main(): Task<int>
    return 0;
  end Main;
end C;
)"), true);
}

// ===========================================================================
// (10) §30.7 / §30.9: await in try body, exception from awaited task caught by
//      the iterator's own catch arm (no yield inside the try/catch — §29.6).
//      MayFail(true) throws InvalidOperationException; the catch arm sets x=5
//      and the iterator yields 5 outside the try/catch block.
//      Expected exit code: 5 (sum=5, caught==1).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, await_in_try_with_catch_exception_caught) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static caught: int := 0;

  public static async method MayFail(shouldFail: bool): Task<int>
    if shouldFail then
      throw new InvalidOperationException("fail");
    end;
    return 42;
  end MayFail;

  public static async method Gen(): IAsyncEnumerable<int>
    var x: int := 0;
    try
      x := await MayFail(true);
    catch
      var ex: InvalidOperationException => do
        C.caught := C.caught + 1;
        x := 5;
      end
    end;
    yield return x;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    await for var v in Gen() do
      sum := sum + v;
    end;
    if C.caught <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), 5);
}

// ===========================================================================
// (11) §29.7: throw before first yield — finally runs, exception propagates.
//      The iterator throws before yielding anything; the consuming `await for`
//      sees the exception; the outer catch in Main captures it; finallyRan==1.
//      Expected exit code: 9 (sum=0 + 9 sentinel from exception catch).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, throw_before_first_yield_finally_runs) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static finallyRan: int := 0;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      throw new Exception("before yield");
      yield return 1;
    finally
      C.finallyRan := C.finallyRan + 1;
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    try
      await for var x in Gen() do
        sum := sum + x;
      end;
    catch
      var ex: Exception => sum := sum + 9;
    end;
    if C.finallyRan <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), 9);
}

// ===========================================================================
// (12) §29.6 / §29.7: multiple sequential try/finally blocks in an async
//      iterator — each finally runs exactly once on normal completion.
//      Gen yields 1 (try1) then 2 (try2); cleanups: 1 (try1 finally) + 10
//      (try2 finally) = 11.  Expected exit code: 3 (1+2) with cleanups==11.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, multiple_sequential_try_finally_blocks) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static cleanups: int := 0;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
    finally
      C.cleanups := C.cleanups + 1;
    end;
    try
      yield return 2;
    finally
      C.cleanups := C.cleanups + 10;
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    await for var x in Gen() do
      sum := sum + x;
    end;
    if C.cleanups <> 11 then return 99; end;
    return sum;
  end Main;
end C;
)"), 3);
}

// ===========================================================================
// (13) §29.6 / §29.7: nested try inside try in an async iterator — both
//      finally blocks run in innermost-first order on normal completion.
//      Gen yields 1 (inner try), then 2 (outer try after inner); cleanups:
//      inner(1) + outer(10) = 11.  Expected exit code: 3 (1+2) cleanups==11.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, nested_try_inside_try_both_finally_run) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static cleanups: int := 0;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      try
        yield return 1;
      finally
        C.cleanups := C.cleanups + 1;
      end;
      yield return 2;
    finally
      C.cleanups := C.cleanups + 10;
    end;
  end Gen;

  public static async method Main(): Task<int>
    var sum := 0;
    await for var x in Gen() do
      sum := sum + x;
    end;
    if C.cleanups <> 11 then return 99; end;
    return sum;
  end Main;
end C;
)"), 3);
}

// ===========================================================================
// (14) §29.6 / §29.7: nested try, consumer breaks early — dispose runs
//      innermost-first: inner finally runs before outer finally.
//      After a break at the first yield, order should be inner(1) then
//      outer(2) → order field == 12.  Expected exit code: 12.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, nested_try_early_break_innermost_first) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static order: int := 0;

  public static async method Gen(): IAsyncEnumerable<int>
    try
      try
        yield return 1;
        yield return 2;
      finally
        C.order := C.order * 10 + 1;
      end;
    finally
      C.order := C.order * 10 + 2;
    end;
  end Gen;

  public static async method Main(): Task<int>
    await for var x in Gen() do
      break;
    end;
    if C.order <> 12 then return 99; end;
    return C.order;
  end Main;
end C;
)"), 12);
}

// #721 / §29.5: a plain `return;` inside an ASYNC iterator (a method that uses
// `yield`, returning IAsyncEnumerable<T>) must be a compile error — use
// `yield break`. The async iterator is a VoidMethod return context, so §21.7
// did not catch the bare return (as it does for a sync iterator); the program
// compiled and HUNG at runtime. Now rejected with §29.5.
BEZEL_TEST(async_iter_try_finally_554, async_iterator_plain_return_rejected_721) {
    BEZEL_EXPECT_TRUE(compileFails(R"(
module M;
class C
  public static async method Gen(): IAsyncEnumerable<int>
    yield return 1;
    return;
  end Gen;
  public static method Main(): int return 0; end Main;
end C;
)"));
}

// #721: `yield break` is the correct way to end early — must compile.
BEZEL_TEST(async_iter_try_finally_554, async_iterator_yield_break_ok_721) {
    BEZEL_EXPECT_FALSE(compileFails(R"(
module M;
class C
  public static async method Gen(): IAsyncEnumerable<int>
    yield return 1;
    yield break;
  end Gen;
  public static method Main(): int return 0; end Main;
end C;
)"));
}

// #721 C2 / §29.7 / §30.9: a `yield break` exiting a try-with-finally must run
// the finally on its way out (it was silently skipped before). SYNC iterator.
BEZEL_TEST(async_iter_try_finally_554, sync_yield_break_runs_finally_721) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static ran: int := 0;
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield break;
    finally
      C.ran := 1;
    end;
  end Gen;
  public static method Main(): int
    var sum := 0;
    for var x in C.Gen() do sum := sum + x; end;
    return sum + C.ran;
  end Main;
end C;
)"), 2);  // 1 yielded + finally ran
}

// #721 C2: `yield break` as the FIRST statement of a try-with-finally — finally
// still runs (no prior yield).
BEZEL_TEST(async_iter_try_finally_554, sync_yield_break_first_runs_finally_721) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static ran: int := 0;
  public static method Gen(): IEnumerable<int>
    try
      yield break;
    finally
      C.ran := 1;
    end;
  end Gen;
  public static method Main(): int
    var sum := 0;
    for var x in C.Gen() do sum := sum + x; end;
    return sum + C.ran;
  end Main;
end C;
)"), 1);  // nothing yielded + finally ran
}

// #721 C2: a `yield break` in an inner try runs BOTH the inner and outer
// finally (exit mode 4 propagates through the outer finally frame).
BEZEL_TEST(async_iter_try_finally_554, sync_yield_break_runs_nested_finally_721) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static a: int := 0;
  public static b: int := 0;
  public static method Gen(): IEnumerable<int>
    try
      try
        yield return 1;
        yield break;
      finally
        C.a := 1;
      end;
    finally
      C.b := 1;
    end;
  end Gen;
  public static method Main(): int
    var sum := 0;
    for var x in C.Gen() do sum := sum + x; end;
    return sum + C.a * 10 + C.b * 100;
  end Main;
end C;
)"), 111);  // 1 yielded + both finallys ran
}

// #721 C2: ASYNC iterator `yield break` exiting a try-with-finally runs the
// finally (the async state-machine path uses the same exit-mode-4 dispatch).
BEZEL_TEST(async_iter_try_finally_554, async_yield_break_runs_finally_721) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static ran: int := 0;
  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      yield break;
    finally
      C.ran := 1;
    end;
  end Gen;
  public static async method Main(): Task<int>
    var sum := 0;
    await for var x in Gen() do sum := sum + x; end;
    return sum + C.ran;
  end Main;
end C;
)"), 2);  // 1 yielded + finally ran
}
