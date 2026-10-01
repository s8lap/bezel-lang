// Codegen — abnormal process exit for an exception that escapes Main with no
// enclosing handler (§33.3 + §22.13, line 3845).
//
// §33.3: "When Main returns int ... the returned value becomes the process exit
// code. Otherwise, NORMAL completion produces an exit code of 0."  §22.13
// (line 3845) explicitly distinguishes "Main returning normally" from "Main
// returning by an uncaught exception that the runtime intercepts" — an uncaught
// exception is abnormal, not normal, completion, so it must NOT report the
// normal-completion exit code (0, which is also the legitimate `return 0`).
//
// Before this fix, Emitter::propagateBB() reached the outermost frame (main)
// with no handler and emitted `ret i32 0` (Constant::getNullValue) — identical
// to normal success — so divide-by-zero, an out-of-range index, and an explicit
// `throw` all silently produced exit code 0, defeating every exit-code-based
// failure detector (FINDING [2]).
//
// The exact non-zero code is not pinned by the spec; the implementation chooses
// 70 (EX_SOFTWARE, "internal software error" from BSD sysexits.h) — a clean,
// deterministic exit (not a signal) that the runtime helper
// bz_unhandled_exception() returns after writing "Unhandled exception" to
// stderr. These tests assert (a) the abnormal paths exit with exactly that
// non-zero code, and (b) the normal paths (return 0, return 42, and a CAUGHT
// exception whose finally/disposal still runs) are unaffected.
//
// These use the native compile+run harness (compileFile + std::system), NOT the
// in-process JIT: the abnormal path calls exit(70), which would terminate the
// test process under the JIT (the JIT invokes `main` as a function pointer in
// the test's own address space).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {

namespace fs = std::filesystem;

// The implementation's chosen abnormal-termination exit code (EX_SOFTWARE).
constexpr int kUnhandledExceptionExit = 70;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the process exit code, or -1 if compilation/link/run failed.
// Mirrors exception_disposal_test.cpp::compileAndRun.
int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_uncaught");
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

// ── Abnormal paths: an uncaught exception out of Main → non-zero exit ────────

// §22.13 / FINDING [2]: an explicit `throw new Boom();` with no enclosing handler
// escapes Main. Before the fix this returned 0 (normal-completion code); it must
// now terminate abnormally with the chosen non-zero code.
BEZEL_TEST(codegen_uncaught_exit, explicit_throw_out_of_main_nonzero) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class Boom: Exception end Boom; "
        "class C public static method Main(): int "
        "  throw new Boom(); "
        "end Main; end C;"),
        kUnhandledExceptionExit);
}

// §17.12 / §22 / FINDING [3]: an expression-bodied method whose ENTIRE body is a
// bare `throw` (`=> throw E`), called with no enclosing handler, escapes Main.
// This is the exact crash repro: FINDING [3] segfaulted bezelc in the OPTIMIZER
// chewing on malformed IR (two terminators in the throwing method's block) —
// which only the NATIVE compile path (this harness, addPassesToEmitFile) runs,
// not the in-process JIT. So this test both (a) proves the compiler no longer
// crashes lowering+optimizing the throwing expr-body method and (b) confirms the
// throw propagates out of Main as an uncaught exception (non-zero exit).
BEZEL_TEST(codegen_uncaught_exit, expr_body_method_throw_out_of_main_nonzero) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class Boom: Exception end Boom; "
        "class C "
        "  public static method Bad(): int => throw new Boom(); "
        "  public static method Main(): int => Bad(); "
        "end C;"),
        kUnhandledExceptionExit);
}

// §22.13 / FINDING [2]: a DivideByZeroException raised by `a / b` (b == 0) with
// no handler escapes Main → non-zero exit (was 0).
BEZEL_TEST(codegen_uncaught_exit, divide_by_zero_out_of_main_nonzero) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a := 10; var b := 0; return a / b; "
        "end Main; end C;"),
        kUnhandledExceptionExit);
}

// §22.13 / FINDING [2]: an IndexOutOfRangeException from an out-of-range array
// read with no handler escapes Main → non-zero exit (was 0).
BEZEL_TEST(codegen_uncaught_exit, index_out_of_range_out_of_main_nonzero) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; return a[10]; "
        "end Main; end C;"),
        kUnhandledExceptionExit);
}

// ── Controls: normal completion is unaffected (must NOT regress) ─────────────

// §33.3: a legitimate `return 0` is normal completion and must still exit 0 —
// the abnormal-exit code must not be confused with a real zero return.
BEZEL_TEST(codegen_uncaught_exit, normal_return_zero_exits_zero) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  return 0; "
        "end Main; end C;"),
        0);
}

// §33.3: the returned value becomes the exit code — `return 42` exits 42.
BEZEL_TEST(codegen_uncaught_exit, normal_return_value_becomes_exit_code) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  return 42; "
        "end Main; end C;"),
        42);
}

// §33.3 / §28.3: a CAUGHT exception is normal completion. The throw is caught,
// the `finally` runs and the owned `using var` is disposed (R's destructor bumps
// the static counter to 1 on the unwind path), then Main returns that count (1)
// normally — exit 1, NOT the abnormal code. This proves the fix is scoped to the
// no-handler path: the catch/finally/disposal machinery is untouched.
BEZEL_TEST(codegen_uncaught_exit, caught_exception_finally_disposal_then_normal_exit) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class Boom: Exception end Boom; "
        "class R "
        "  public static count: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R.count := R.count + 1; end destructor; "
        "  public static method Main(): int "
        "    var ran := 0; "
        "    try "
        "      do using var r := new R(); throw new Boom(); end; "
        "    catch Boom => ran := 1; "
        "    finally ran := ran + 10; end; "
        "    return R.count + ran; "  // 1 (disposed) + 1 (caught) + 10 (finally)
        "  end Main; "
        "end R;"),
        12);
}

}  // namespace
