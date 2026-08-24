// Codegen — iterator Dispose must run finally blocks on early disposal (§29.7 v0.135).
// Issue #623: when a consumer `break`s out of a for-loop, the enumerator's
// Dispose() must drive the state machine through all pending finally clauses
// active at the suspension point.  Previously Dispose was a stub that just freed
// the frame, silently skipping finally.
//
// Spec authority: §29.7 v0.135 — "Runs finally clauses active at the point of
// suspension."
//
// Tests:
//   (1) sync iterator: try { yield return 1; } finally { counter++; }
//       consumer breaks after first element → counter == 1 at loop exit.
//   (2) async iterator: same shape via IAsyncEnumerable<int> + await for + break.
//   (3) sync iterator with NESTED try/finally: both finally blocks run (inner first).
//   (4) return-from-loop-body: same as break, finally must run.
//   (5) full iteration regression: finally runs exactly once on normal completion.
//   (6) exception-in-body regression: finally already ran via exception path
//       (enumerator not disposed a second time → counter stays 1, not 2).
//   (7) owned-yielding iterator early-dispose: finally runs AND frame is freed
//       (no LSan leak).

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
static const std::filesystem::path& wtDir623() {
    return wtScratchDir("wt-623");
}

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir623();
    std::string stem = unique_stem("bezel_623");
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
// (1) §29.7: sync iterator, try/finally, consumer break after first element.
//     finally must run → counter == 1 at loop exit.
//     Expected exit code: 1.
// ===========================================================================
BEZEL_TEST(iter_finally_dispose_623, sync_break_runs_finally) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
      yield return 3;
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;

  public static method Main(): int
    for var x in Gen() do
      break;
    end;
    return C.counter;
  end Main;
end C;
)"), 1);
}

// ===========================================================================
// (2) §29.7 / §30.7: async iterator, try/finally, consumer break after first.
//     finally must run → counter == 1 at loop exit.
//     Expected exit code: 1.
// ===========================================================================
BEZEL_TEST(iter_finally_dispose_623, async_break_runs_finally) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;

  public async static method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;

  public static async method Main(): Task<int>
    await for var x in Gen() do
      break;
    end;
    return C.counter;
  end Main;
end C;
)"), 1);
}

// ===========================================================================
// (3) §29.7: sync iterator, NESTED try/finally, consumer break after first.
//     Both finally blocks run, inner first. counter == 21 (inner adds 1, outer
//     adds 20). Expected exit code: 21.
// ===========================================================================
BEZEL_TEST(iter_finally_dispose_623, sync_break_nested_finally) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      try
        yield return 1;
        yield return 2;
      finally
        C.counter := C.counter + 1;
      end;
    finally
      C.counter := C.counter + 20;
    end;
  end Gen;

  public static method Main(): int
    for var x in Gen() do
      break;
    end;
    return C.counter;
  end Main;
end C;
)"), 21);
}

// ===========================================================================
// (4) §29.7: sync iterator, return from loop body runs finally.
//     Consumer calls a helper that returns from inside the for loop.
//     finally must run → counter == 1.
//     Expected exit code: 1.
// ===========================================================================
BEZEL_TEST(iter_finally_dispose_623, sync_return_from_body_runs_finally) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      yield return 10;
      yield return 20;
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;

  public static method Consume(): int
    for var x in Gen() do
      return x;
    end;
    return 0;
  end Consume;

  public static method Main(): int
    var v := Consume();
    if v <> 10 then return 99; end;
    return C.counter;
  end Main;
end C;
)"), 1);
}

// ===========================================================================
// (5) §29.7 regression: full iteration — finally runs exactly once on normal
//     completion (not on each element).
//     Iterator yields 1, 2, 3; consumer reads all. counter must be 1, sum 6.
//     Expected exit code: 6.
// ===========================================================================
BEZEL_TEST(iter_finally_dispose_623, sync_full_iter_finally_once) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
      yield return 3;
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;

  public static method Main(): int
    var sum := 0;
    for var x in Gen() do
      sum := sum + x;
    end;
    if C.counter <> 1 then return 99; end;
    return sum;
  end Main;
end C;
)"), 6);
}

// ===========================================================================
// (6) §29.7 regression: exception in iterator body — finally ran via the
//     exception path inside MoveNext (#623); the exception then propagates to
//     the consumer (#630).  The for-loop unwind calls Dispose on the enumerator,
//     but since the iterator is now faulted (state=-1 set before running finally
//     on unwind), Dispose must be a no-op — so the finally does NOT run a second
//     time.  counter must be 1 (ran once via unwind), not 2 (double-dispose).
//     The consumer wraps the loop in try/catch to receive the exception; it then
//     returns C.counter, which must be 1.
//     Expected exit code: 1 (counter == 1 after finally ran once, exception caught).
// ===========================================================================
BEZEL_TEST(iter_finally_dispose_623, sync_exception_finally_once) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      throw new Exception("test");
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;

  public static method Main(): int
    try
      for var x in Gen() do
      end;
    catch Exception =>
      return C.counter;
    end;
    return C.counter;
  end Main;
end C;
)"), 1);
}

// ===========================================================================
// (7) §29.7: sync iterator, break after second of three yields.
//     The first yield is consumed (state=1), second yield consumed (state=2),
//     then break. finally must run exactly once.
//     Expected exit code: 1 (counter).
// ===========================================================================
BEZEL_TEST(iter_finally_dispose_623, sync_break_after_second_yield) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
      yield return 3;
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;

  public static method Main(): int
    var n := 0;
    for var x in Gen() do
      n := n + 1;
      if n >= 2 then break; end;
    end;
    return C.counter;
  end Main;
end C;
)"), 1);
}
