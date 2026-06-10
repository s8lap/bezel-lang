// Codegen — yield / await inside try / catch / finally in an async iterator
// (#554, §30.7 / §29.6 / §29.7 v0.135).
//
// Spec authorities:
//   §29.6: yield in try-with-only-finally: allowed. yield in try-with-catch:
//          forbidden. yield in catch arm body or finally: forbidden.
//   §29.7: on early disposal (break / exception / scope-end), the state machine
//          runs all finally clauses active at the suspension point.
//   §30.7: async iterators obey the same rules; await is additionally permitted
//          in try/catch/finally bodies (§30.9) — just not in a catch GUARD.
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
//   (6) await inside a finally block in an async iterator: runs correctly (§30.9).
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
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_554_" + std::to_string(counter++);
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
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_554_neg_" + std::to_string(counter++);
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

  public async static method Gen(): IAsyncEnumerable<int>
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
// (2) §29.6: yield inside try-with-only-finally with consumer break.
//     The consumer reads only the first yielded value then breaks.
//     The iterator must compile and produce the correct first value.
//     (Early-disposal finally execution is a future increment per §29.7;
//      this test only verifies the produced value, not finallyRan.)
//     Expected exit code: 1.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_try_finally_early_break) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public async static method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
    end;
  end Gen;

  public static async method Main(): Task<int>
    var got := 0;
    await for var x in Gen() do
      got := x;
      break;
    end;
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

  public async static method Gen(): IAsyncEnumerable<int>
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

  public async static method Gen(): IAsyncEnumerable<int>
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

  public async static method Gen(): IAsyncEnumerable<int>
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
// (6) §30.7 / §30.9: await inside a finally block in an async iterator.
//     The finally awaits a completed task that increments a static counter.
//     Expected exit code: 4 when finallyRan == 1.
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, await_in_finally_in_async_iter) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static finallyRan: int := 0;

  public static async method LogAsync(): Task
    C.finallyRan := C.finallyRan + 1;
  end LogAsync;

  public async static method Gen(): IAsyncEnumerable<int>
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
)"), 4);
}

// ===========================================================================
// (7) §29.6: yield inside a try-with-catch is forbidden (compile-time error).
// ===========================================================================
BEZEL_TEST(async_iter_try_finally_554, yield_in_try_with_catch_rejected) {
    BEZEL_EXPECT_EQ(compileFails(R"(
module M;
class C
  public async static method Gen(): IAsyncEnumerable<int>
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
  public async static method Gen(): IAsyncEnumerable<int>
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
  public async static method Gen(): IAsyncEnumerable<int>
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
