// Codegen — §22.12 rule 2 two-form mutable-static admission (v0.181, #1556):
// runtime behavior of an admitted mutable static.
//
// Authority: language spec §22.12 rule 2 (v0.181) + §22.13 pattern (b) +
//   §28.8 (spec "Static class-typed fields without owned").  Two store
//   semantics have observable runtime consequences these tests exercise:
//
//   - Ownership discharge: assigning a fresh value into an admitted mutable
//     static DISCHARGES the producer (the §28.8 ownership-discharge exemption
//     for non-`owned` statics extends to admitted mutable statics), so the
//     §22.13 pattern (b) lazy task cache `_cfg := Make();` compiles and runs.
//   - Atomicity: the store into the static reference field is emitted as a
//     single aligned atomic (monotonic) pointer write — verified at the IR
//     level; here we confirm the admitted forms produce correct runtime values.
//
// The lazy task cache is exactly §22.13 pattern (b): a `null`-initialized
// `Task<T>?` static, set once on first use and awaited at every use site
// (Pattern A multi-await, §30.5).  The second use reuses the SAME cached task,
// so both awaits observe the same result.
//
// Async codegen accumulates emitter state across in-process JIT runs, so — like
// async_nullable_return_493_test.cpp and async_test.cpp — these tests use the
// AOT/driver native-executable harness (compile to a temp exe, run, read the
// exit code) rather than the in-process JIT.

#include <cstdio>
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
    std::string stem = unique_stem("bezel_static1556");
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

// §22.12 rule 2 form (ii) + §22.13 pattern (b): a mutable static `Task<int>?`
// lazy cache is admitted, the `_v := Make();` store discharges its producer, and
// the cached task is reused across two `Compute()` calls (Pattern A multi-await).
// Both awaits observe 42, so the program exits 84.  Exercises admission,
// discharge, the atomic store, and multi-await reuse of the cached task.
BEZEL_TEST(static_mutable_admission_1556, task_cache_pattern_b_reused_returns_84) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class App "
        "  private static _v: Task<int>? := null; "
        "  public static async method Make(): Task<int> "
        "    return 42; "
        "  end Make; "
        "  public static async method Compute(): Task<int> "
        "    if _v = null then _v := Make(); end; "
        "    return await _v!; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var a := await Compute(); "
        "    var b := await Compute(); "
        "    return a + b; "
        "  end Main; "
        "end App;"), 84);
}

// §22.12 rule 2 form (ii): the cache is genuinely lazy — the task is created
// exactly once (on the first `Compute()`), so a mutable counter incremented by
// Make() reaches 1, not 2, after two `Compute()` calls.  Confirms the cached
// task is reused rather than recreated on the second use.
BEZEL_TEST(static_mutable_admission_1556, task_cache_created_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class App "
        "  private static _v: Task<int>? := null; "
        "  private static _calls: int := 0; "
        "  public static async method Make(): Task<int> "
        "    _calls := _calls + 1; "
        "    return 7; "
        "  end Make; "
        "  public static async method Compute(): Task<int> "
        "    if _v = null then _v := Make(); end; "
        "    return await _v!; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var a := await Compute(); "
        "    var b := await Compute(); "
        "    return _calls; "
        "  end Main; "
        "end App;"), 1);
}
