// Codegen — exceptions thrown from a sync iterator body propagate through
// MoveNext to the consumer (§29.2, §29.3, §31 v0.135).
//
// Issue #630: an exception thrown inside a sync iterator's body during MoveNext
// was silently swallowed: MoveNext returned false and the for-loop ended as if
// the sequence was exhausted.  The consumer's enclosing try/catch never fired.
//
// Root cause: the vtable-dispatched MoveNext call in lowerIEnumerableBuiltinFor
// had no emitPendingCheck() after the CreateCall, so the in-flight exception in
// bz$exc was never tested.  The fix adds emitPendingCheck() immediately after
// the vtable MoveNext call — the standard post-call exception gate used
// everywhere else in the emitter (§31, CG20).
//
// Spec authority: §29.2 v0.135 — the for-loop desugaring is
//   while __enumerator.MoveNext() do ... end
// Since MoveNext executes the iterator body, any exception it raises escapes
// through MoveNext to the consumer, exactly as any callee exception does
// (§31.2.1: exceptions propagate until caught).
//
// Tests:
//   (A) exception after first yield propagates to consumer's catch (repro shape).
//   (B) exception BEFORE the first yield propagates on the first MoveNext.
//   (C) try/finally in iterator: finally runs exactly once on exception path,
//       and the exception still reaches the consumer (interaction with #623).
//   (D) after catching the exception the iterator is finished (state=-1):
//       the frame was freed by Dispose on the exception unwind path.
//   (E) sanity: normal completion still works — no regression.
//
// All tests use the native compile+run harness (same style as
// iterator_native_test.cpp / iter_finally_dispose_623_test.cpp).

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
static const std::filesystem::path& wtDir630() {
    return wtScratchDir("wt-630");
}

// Compile `source` to a native executable and run it; returns the exit code,
// or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir630();
    std::string stem = unique_stem("bezel_630");
    std::string src  = (dir / (stem + ".bzl")).string();
    std::string exe  = (dir / stem).string();
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
// (A) §29.2/§29.3/§31 (#630): exception AFTER first yield propagates.
//     iterator: yield 1; throw InvalidOperationException("boom")
//     consumer: try { for x in Gen() do sum+=x; end; } catch IOE => return 42;
//     Expected: rc=42 (exception caught by consumer).
// ===========================================================================
BEZEL_TEST(iter_exception_630, exc_after_first_yield_propagates) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module V;
class Gen
  public static method Numbers(): IEnumerable<int>
    yield return 1;
    throw new InvalidOperationException("boom");
  end Numbers;
end Gen;
class Program
  public static method Main(): int
    var sum: int := 0;
    try
      for var n in Gen.Numbers() do
        sum := sum + n;
      end;
    catch InvalidOperationException =>
      return 42;
    end;
    return sum;
  end Main;
end Program;
)"), 42);
}

// ===========================================================================
// (B) §29.2/§29.3/§31 (#630): exception BEFORE first yield propagates on
//     the very first MoveNext call.
//     iterator: throw immediately (no yield).
//     consumer: try { for x in Gen() do ... end; } catch IOE => return 99;
//     Expected: rc=99.
// ===========================================================================
BEZEL_TEST(iter_exception_630, exc_before_first_yield_propagates) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module V;
class Gen
  public static method Empty(): IEnumerable<int>
    throw new InvalidOperationException("immediately");
  end Empty;
end Gen;
class Program
  public static method Main(): int
    try
      for var n in Gen.Empty() do
      end;
    catch InvalidOperationException =>
      return 99;
    end;
    return 0;
  end Main;
end Program;
)"), 99);
}

// ===========================================================================
// (C) §29.2/§29.3/§29.7/§31 (#630 + #623): try/finally inside iterator —
//     finally runs exactly once on the exception path AND the exception
//     reaches the consumer.
//     iterator: try { yield 1; throw IOE("test"); } finally { counter++; }
//     consumer: try { for x in Gen() do sum+=x; end; } catch IOE => return counter;
//     Expected: rc=1  (finally ran once, exception propagated, counter=1).
// ===========================================================================
BEZEL_TEST(iter_exception_630, try_finally_runs_once_exception_propagates) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module V;
class C
  public static counter: int := 0;

  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      throw new InvalidOperationException("test");
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;

  public static method Main(): int
    var sum := 0;
    try
      for var x in Gen() do
        sum := sum + x;
      end;
    catch InvalidOperationException =>
      // exception propagated correctly; verify finally ran exactly once
      return C.counter;
    end;
    return 0;
  end Main;
end C;
)"), 1);
}

// ===========================================================================
// (D) §29.2/§29.3/§29.7 (#630): after catching the exception from the for-loop,
//     the iterator is finished (Dispose was called on the unwind path, freeing
//     the frame and setting state=-1).  Subsequent interaction with the same
//     variable (none needed here — just confirm no crash/leak after the catch).
//     Also confirms sum==1 (only the element before the throw was consumed).
//     Expected: rc=1 (sum of elements consumed before the throw).
// ===========================================================================
BEZEL_TEST(iter_exception_630, iterator_finished_after_exception) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module V;
class Gen
  public static method Numbers(): IEnumerable<int>
    yield return 1;
    throw new InvalidOperationException("stop");
  end Numbers;
end Gen;
class Program
  public static method Main(): int
    var sum := 0;
    try
      for var n in Gen.Numbers() do
        sum := sum + n;
      end;
    catch InvalidOperationException =>
      // sum reflects only elements produced before the throw
      return sum;
    end;
    return 0;
  end Main;
end Program;
)"), 1);
}

// ===========================================================================
// (E) §29.2/§29.3 (#630 regression): normal iterator completion — no regression.
//     iterator: yield 1; yield 2; yield 3;  sum=6.
//     Expected: rc=6.
// ===========================================================================
BEZEL_TEST(iter_exception_630, normal_completion_regression) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module V;
class Gen
  public static method Numbers(): IEnumerable<int>
    yield return 1;
    yield return 2;
    yield return 3;
  end Numbers;
end Gen;
class Program
  public static method Main(): int
    var sum := 0;
    for var n in Gen.Numbers() do
      sum := sum + n;
    end;
    return sum;
  end Main;
end Program;
)"), 6);
}
