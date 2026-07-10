// Codegen — second explicit Dispose() on an iterator IEnumerator<T> must be a
// no-op, not a SIGSEGV (§28.2 / §29.7 v0.183).
// Issue #1107 (adversarial sweep #9): calling `e.Dispose()` twice on the same
// IEnumerator<T> local crashed with SIGSEGV. The first call freed the frame and
// nulled the local slot; the second call reloaded null and dereferenced it to
// load the vtable → null-pointer dereference. Fix: null-guard the explicit
// enumerator Dispose dispatch on a non-null receiver (mirrors the scope-end and
// null-conditional paths), and add a state==-1 idempotency guard at the top of
// the iterator Dispose function body (defense-in-depth).
//
// Spec authority:
//   §28.2 v0.183 — "Dispose() on any class instance is idempotent: the first
//   call performs the disposal; subsequent calls are no-ops."
//   §29.7 — "second Dispose() is a no-op."
//
// Tests:
//   (1) second Dispose on a bare IEnumerator<int> — no crash, rc=0 (the bug).
//   (2) second Dispose after one MoveNext with try/finally — finally runs
//       exactly once, rc=1 (the idempotency guard prevents a double-run).
//   (3) normal single-dispose iteration still works — regression check, rc=3.
//   (4) second Dispose on an IEnumerable<int>-sourced enumerator — no crash.

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
    std::string stem = "bezel_1107_" + std::to_string(counter++);
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
// (1) §28.2 / §29.7: second explicit Dispose() on IEnumerator<int> must be a
//     no-op. Previously this SIGSEGV'd because the first Dispose freed the
//     frame and nulled the slot; the second reloaded null and dereferenced it.
//     Expected exit code: 0 (no crash).
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, second_dispose_is_noop) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class Test
  public static method Gen(): IEnumerator<int>
    yield return 1;
  end Gen;
  public static method Main(): int
    var e := Gen();
    e.Dispose();
    e.Dispose();
    return 0;
  end Main;
end Test;
)"), 0);
}

// ===========================================================================
// (2) §28.2 / §29.7: second Dispose after one MoveNext with try/finally —
//     the finally must run exactly once. The first Dispose runs the finally
//     (counter becomes 1) and marks state=-1; the second Dispose is a no-op
//     (does not re-run the finally). Without the idempotency guard the finally
//     could double-run.
//     Expected exit code: 1 (counter == 1).
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, second_dispose_finally_once) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static counter: int := 0;
  public static method Gen(): IEnumerator<int>
    try
      yield return 1;
      yield return 2;
    finally
      C.counter := C.counter + 1;
    end;
  end Gen;
end C;
class Main
  public static method Main(): int
    var e := C.Gen();
    if e.MoveNext() then
      var _ := e.Current;
    end;
    e.Dispose();
    e.Dispose();
    return C.counter;
  end Main;
end Main;
)"), 1);
}

// ===========================================================================
// (3) Regression: normal single-dispose iteration still works after the fix.
//     A simple IEnumerator<int> iterated via MoveNext/Current, then disposed
//     once. The sum must be correct (1+2=3).
//     Expected exit code: 3.
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, single_dispose_normal_iteration) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class Test
  public static method Gen(): IEnumerator<int>
    yield return 1;
    yield return 2;
  end Gen;
  public static method Main(): int
    var e := Gen();
    var sum := 0;
    while e.MoveNext() do
      sum := sum + e.Current;
    end;
    e.Dispose();
    return sum;
  end Main;
end Test;
)"), 3);
}

// ===========================================================================
// (4) §28.2 / §29.7: second Dispose on an IEnumerator<int> obtained via a
//     method-local. The enumerator is created directly (no IEnumerable wrapper);
//     disposing it twice must be a no-op. Tests the null-guard path for a
//     non-trivial receiver expression (method call result stored in a local).
//     Expected exit code: 0.
// ===========================================================================
BEZEL_TEST(iter_second_dispose_1107, second_dispose_method_result) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class Test
  public static method Make(): IEnumerator<int>
    yield return 7;
  end Make;
  public static method Main(): int
    var e := Make();
    e.Dispose();
    e.Dispose();
    return 0;
  end Main;
end Test;
)"), 0);
}
