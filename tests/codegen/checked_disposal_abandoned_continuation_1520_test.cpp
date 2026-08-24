// Codegen/runtime — issue #1520 (§30.10 v0.179): report DisposedAccessException
// from an ABANDONED continuation in checked disposal mode.
//
// §30.10's `Task.FireAndForget` paragraph states the two legal "I don't want
// to await this task" patterns:
//   - `Task.FireAndForget(call())` — supervised: exceptions are logged.
//   - `using var t := call();` — unsupervised: "the task is disposed at scope
//     end without blocking, and any exceptions it raises are silently
//     swallowed — except that in checked disposal mode (§28.2) a
//     DisposedAccessException raised in an abandoned continuation is first
//     REPORTED through the same implementation-defined diagnostic channel as
//     FireAndForget's logging, then swallowed (v0.179): the backstop that
//     catches a §30.1 source-outlives-settlement violation must never itself
//     be invisible."
// Stdlib §6.7 (docs/Bezel_Standard_Library.md, FireAndForget entry) restates
// this: "In checked disposal mode, a DisposedAccessException raised in such
// an abandoned continuation is reported through the diagnostic channel before
// being swallowed."
//
// "Abandoned continuation" is the specific case where a `using var`-bound
// Task is STILL RUNNING (suspended past an `await`) at the moment its
// disposing scope exits — the classic §30.1 "borrowed parameters ... held
// until settlement" hazard: the task's frame holds a borrow of a caller-side
// source (here, a plain non-owned class-typed parameter) that the caller may
// go on to dispose while the frame is suspended. When the continuation
// eventually resumes and touches that now-disposed source, checked disposal
// mode's ordinary member-access guard (§28.2) raises DisposedAccessException
// — but because nothing awaits the abandoned task, nothing would otherwise
// ever observe that fault. This suite's positive case (`p6_dangle`, matching
// the issue's own named repro) exercises exactly that: `Worker` borrows a
// `Resource`, suspends on `Task.Delay`, and the caller disposes the Resource
// and lets the `using var t := Worker(r);` scope exit before the delay
// elapses.
//
// A task that is NOT abandoned — either because it is properly `await`ed
// before its `using var` scope ends, or because its abandoned continuation
// completes without faulting, or because it faults with a DIFFERENT
// exception type — must NOT print anything: the negative cases below pin
// that "report" is precisely scoped to the abandoned + DisposedAccessException
// combination, not a general fault-logging change (a general "log every
// FireAndForget/using-var fault" behavior remains the separate, broader
// residual tracked by #1021 and is deliberately NOT implemented by this fix).
//
// Because the report goes to a real OS-level diagnostic channel (stderr) via
// a runtime code path that must survive past the disposing scope's exit, this
// cannot be observed through the in-process JIT harness (which only returns
// `main`'s int result, mirroring runJit/runJitChecked elsewhere in this
// directory) — it needs a real native executable whose stdout+stderr can be
// captured, exactly as tests/codegen/console_write_unicode_592_test.cpp does
// for Console.Write and tests/codegen/uncaught_exit_test.cpp does for the
// §33.3 unhandled-exception boundary.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1520() {
    return wtScratchDir("wt-1520");
}

struct RunResult {
    bool compiled = false;
    int exitCode = -1;
    std::string output;  // combined stdout+stderr
};

// Compile `source` to a temp native executable (checked disposal mode
// controlled by `checkedDisposal`) via the real Driver pipeline, run it, and
// capture its combined stdout+stderr plus its exit code.
RunResult compileAndCapture(const std::string& source, bool checkedDisposal) {
    auto dir = wtDir1520();
    std::string stem = unique_stem("bezel_1520");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }

    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = checkedDisposal;
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe, {}, opts);

    RunResult result;
    result.compiled = r.success;
    if (r.success) {
        // Redirect stderr into the same pipe as stdout so the diagnostic
        // (written to stderr, matching the runtime's other exception-to-text
        // renderers) is captured regardless of which stream is used.
        std::string cmd = exe + " 2>&1";
        if (FILE* pipe = popen(cmd.c_str(), "r")) {
            std::array<char, 512> buf;
            std::size_t n;
            while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0)
                result.output.append(buf.data(), n);
            int status = pclose(pipe);
            if (status != -1 && WIFEXITED(status))
                result.exitCode = WEXITSTATUS(status);
        }
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return result;
}

// A Resource with a plain instance method (an instance-member access), so
// that accessing it after disposal exercises the §28.2 checked-mode guard.
const char* kResource =
    "class Resource "
    "  private _value: int := 42; "
    "  public method GetValue(): int return _value; end GetValue; "
    "end Resource; ";

}  // namespace

// ── §30.10 v0.179: abandoned continuation + DisposedAccessException ──────
// reported, then swallowed, in checked disposal mode.

BEZEL_TEST(checked_disposal_abandoned_continuation_1520,
          reports_disposed_access_from_abandoned_continuation) {
    // Matches the issue's own named repro shape (p6_dangle): `Worker` borrows
    // `r` (a plain, non-owned parameter — a frame-held borrow per §30.1) and
    // suspends on `Task.Delay(30)` as its first statement, so it is
    // GUARANTEED still running when the `do using var t := C.Worker(r); end;`
    // block exits immediately afterward (no intervening suspension in Main).
    // `t` is thus abandoned while genuinely in-flight. `r.Dispose()` then
    // invalidates the borrow the suspended frame holds; when the continuation
    // resumes (~30ms later) and calls `r.GetValue()`, the checked-mode guard
    // raises DisposedAccessException with no awaiter to observe it. Main's
    // own `await Task.Delay(150)` — a comfortable margin over the 30ms delay
    // — gives the abandoned continuation time to resume and fault before the
    // process exits, mirroring the timing idiom of
    // async_task_result_disposal_1503_test.cpp's slow-path tests.
    const std::string prog =
        "module M; import System.Threading; " + std::string(kResource) +
        "class C "
        "  public static async method Worker(r: Resource): Task "
        "    await Task.Delay(30); "
        "    _ := r.GetValue(); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    var r := new Resource(); "
        "    do using var t := C.Worker(r); end; "
        "    r.Dispose(); "
        "    await Task.Delay(150); "
        "    return 0; "
        "  end Main; "
        "end C;";
    RunResult r = compileAndCapture(prog, /*checkedDisposal=*/true);
    BEZEL_EXPECT_EQ(r.compiled, true);
    // "then swallowed": the process completes normally, not abnormally (the
    // exception must never propagate to the §33.3 unhandled-exception
    // boundary or crash the process) — spec §30.10.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // "first reported through the ... diagnostic channel": the fault's type
    // must be observable in the process's output.
    BEZEL_EXPECT_EQ(
        r.output.find("DisposedAccessException") != std::string::npos, true);
}

// ── Negative: a NORMAL (properly awaited) continuation is unaffected ─────

BEZEL_TEST(checked_disposal_abandoned_continuation_1520,
          no_report_when_continuation_is_awaited_not_abandoned) {
    // Identical shape, except `t` is `await`ed INSIDE the `do` block before
    // its `using var` scope ends — never abandoned. By the time the scope-end
    // drop runs, the task is already done (the fast path of
    // bz_task_fire_and_forget_checked), which per the fix must NEVER inspect
    // `error_obj` (codegen's scope-end error_obj disposal already handled it,
    // silently, before this call) — so nothing should print, and disposing
    // `r` afterward is an ordinary, unobserved disposal (Worker never runs
    // again).
    const std::string prog =
        "module M; import System.Threading; " + std::string(kResource) +
        "class C "
        "  public static async method Worker(r: Resource): Task "
        "    await Task.Delay(10); "
        "    _ := r.GetValue(); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    var r := new Resource(); "
        "    do using var t := C.Worker(r); await t; end; "
        "    r.Dispose(); "
        "    await Task.Delay(60); "
        "    return 0; "
        "  end Main; "
        "end C;";
    RunResult r = compileAndCapture(prog, /*checkedDisposal=*/true);
    BEZEL_EXPECT_EQ(r.compiled, true);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.output.empty(), true);
}

// ── Negative: an abandoned continuation that completes SUCCESSFULLY ──────
// (no fault at all) still runs its side effect, unaffected and unreported.

BEZEL_TEST(checked_disposal_abandoned_continuation_1520,
          no_report_for_successful_abandoned_continuation) {
    // Stdlib §6.7: "The underlying operation continues to run in the
    // background" for the `using var` abandonment idiom — a non-faulting
    // abandoned continuation must still complete its work (the counter
    // increment below is observable via Main's return value), and printing
    // nothing along the way.
    const std::string prog =
        "module M; import System.Threading; "
        "class C "
        "  public static counter: int := 0; "
        "  public static async method Worker(): Task "
        "    await Task.Delay(20); "
        "    C.counter := C.counter + 1; "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.Worker(); end; "
        "    await Task.Delay(80); "
        "    return C.counter; "
        "  end Main; "
        "end C;";
    RunResult r = compileAndCapture(prog, /*checkedDisposal=*/true);
    BEZEL_EXPECT_EQ(r.compiled, true);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // the background increment ran
    BEZEL_EXPECT_EQ(r.output.empty(), true);
}

// ── Negative: an abandoned continuation faulting with a DIFFERENT ────────
// exception type remains silently swallowed (unchanged baseline; the "report"
// carve-out is scoped to DisposedAccessException specifically, per §30.10).

BEZEL_TEST(checked_disposal_abandoned_continuation_1520,
          no_report_for_non_disposed_access_fault) {
    const std::string prog =
        "module M; import System.Threading; "
        "class C "
        "  public static async method Worker(): Task "
        "    await Task.Delay(20); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.Worker(); end; "
        "    await Task.Delay(80); "
        "    return 0; "
        "  end Main; "
        "end C;";
    RunResult r = compileAndCapture(prog, /*checkedDisposal=*/true);
    BEZEL_EXPECT_EQ(r.compiled, true);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.output.empty(), true);
}
