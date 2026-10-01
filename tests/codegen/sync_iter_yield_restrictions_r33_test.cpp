// Codegen — §29.6 yield-placement restrictions for a SYNCHRONOUS iterator
// (returning IEnumerable<T>). R33 discovery found that these were enforced only
// for async iterators (emitAsyncIteratorMethod); a synchronous iterator with a
// `yield return` inside a `finally` reached codegen and emitted malformed IR
// ("Terminator found in the middle of a basic block"), and a `yield` inside a
// try-with-catch was accepted unsoundly. emitIteratorMethod now runs the same
// suspendInTryPosition check and rejects the forbidden positions with a clean
// §29.6 diagnostic.
//
// Spec §29.6 (v0.141):
//   - `yield` in a `try` body whose surrounding statement has only a `finally`:
//     ALLOWED.
//   - `yield` in a `try` body whose surrounding statement has a `catch`:
//     forbidden.
//   - `yield` in a catch arm body or a `finally` block: forbidden.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_synciter_r33");
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
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_synciter_r33_neg");
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

// §29.6 ALLOWED: yield inside a try whose surrounding statement has only a
// finally. The body yields 5 and 6; the consumer sums them → 11.  Must compile
// and run (the fix must not over-reject this case).
BEZEL_TEST(sync_iter_yield_restrictions_r33, yield_in_try_only_finally_allowed) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public static method Gen(): IEnumerable<int>
    try
      yield return 5;
      yield return 6;
    finally
      var cleanup := 1;
    end;
  end Gen;
  public static method Main(): int
    var sum := 0;
    for var v in C.Gen() do sum := sum + v; end;
    return sum;
  end Main;
end C;
)"), 11);
}

// §29.6 forbidden (R33 Bug 5): yield inside a `finally` block. Previously
// reached codegen → malformed IR. Now a clean compile-time diagnostic.
BEZEL_TEST(sync_iter_yield_restrictions_r33, yield_in_finally_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(R"(
module M;
class C
  public static method Gen(): IEnumerable<int>
    try
      var x := 1;
    finally
      yield return 99;
    end;
    yield return 2;
  end Gen;
  public static method Main(): int return 0; end Main;
end C;
)"));
}

// §29.6 forbidden (R33 Bug 4): yield inside a `try` body with a catch arm.
// Previously accepted unsoundly. Now a clean compile-time diagnostic.
BEZEL_TEST(sync_iter_yield_restrictions_r33, yield_in_try_with_catch_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(R"(
module M;
class C
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
    catch Exception => var y := 0;
    end;
  end Gen;
  public static method Main(): int return 0; end Main;
end C;
)"));
}

// §29.6 forbidden: yield inside a catch arm body.
BEZEL_TEST(sync_iter_yield_restrictions_r33, yield_in_catch_body_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(R"(
module M;
class C
  public static method Gen(): IEnumerable<int>
    try
      var x := 1;
    catch Exception => yield return 7;
    end;
  end Gen;
  public static method Main(): int return 0; end Main;
end C;
)"));
}
