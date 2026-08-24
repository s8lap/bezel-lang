// Codegen — §30.9 fault ownership (v0.179): a faulted task OWNS its fault
// exception and disposes it EXACTLY ONCE, at task disposal — with the in-flight
// deferral refinement (#1528, subsuming crash #1288 / #1197).
//
// Bug (#1288 / #1197): a faulted `Task`/`Task<T>` bound to a local INSIDE a
// `try` and awaited there re-raises its fault, which unwinds to the enclosing
// catch. During that unwind the try-scope disposes the task local; pre-fix the
// task-local scope-end disposal FREED the task's `error_obj` (the exception)
// while it was still in flight — being routed to the catch arm. The catch arm's
// type-test / binding then read the freed object → SIGSEGV (UAF / double-free).
//
// §30.9 (v0.179) states the exception "is disposed exactly once, by the task
// that owns it, when the task is disposed — with two timing refinements: a task
// disposed while its own fault exception is in flight defers that disposal until
// the exception's handling completes (so a `using var`-bound task unwound by its
// own fault never destroys the exception mid-dispatch) ...". The fix implements
// refinement (a): the task local's scope-end disposal, on finding the exception
// still BORROWED (in flight), records it for deferred disposal instead of
// freeing it; the catch arm's normal exit performs the dispose as its terminal
// act. A fault that was already fully handled (not borrowed) is disposed at
// scope-end as before, and the anonymous-producer path (`await Producer()`,
// handler-owned) is unchanged.
//
// Spec derivation: §30.9 ("Fault ownership (v0.179)"), §30.2 (await re-raises a
// faulted task), §31.1 (a caught exception is a borrow of the in-flight
// instance). Each case below is a program whose exit code encodes the result;
// each is run 3x to surface any nondeterminism in the suspend/resume + unwind
// disposal ordering.

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
static const std::filesystem::path& wtDir1528() {
    return wtScratchDir("wt-1528");
}

// Compile `source` to a temp native executable via the real Driver and run it;
// returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir1528();
    std::string stem = unique_stem("bezel_1528");
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

void expectValue1528(const std::string& source, int expected) {
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(source), expected);
}

}  // namespace

// ---------------------------------------------------------------------------
// §30.9 / §30.2 — the #1288 repro: a faulted Task<int> bound to a `var` INSIDE
// the try, awaited there; the re-raise is caught by the surrounding arm. Pre-
// fix: SIGSEGV (the task local's scope-end freed the in-flight exception during
// unwind). Must return 37.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_ownership_1528, var_bound_inside_try_reraises) {
    expectValue1528(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      var f := Boom(); "
        "      return await f; "
        "    catch InvalidOperationException => return 37; "
        "    end; "
        "  end Main; end C;",
        37);
}

// ---------------------------------------------------------------------------
// §30.9 / §30.2 — the #1197 repro: a `using var`-bound faulted task inside the
// try (unwound by its OWN fault). Pre-fix: UAF crash. Must return 7.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_ownership_1528, using_var_inside_try_reraises) {
    expectValue1528(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      using var t := Boom(); "
        "      var r := await t; "
        "      return r; "
        "    catch Exception => return 7; "
        "    end; "
        "  end Main; end C;",
        7);
}

// ---------------------------------------------------------------------------
// §30.9 / §30.4 — regression guard for the multi-awaiter case (#854): a bound
// faulted task awaited TWICE re-raises the SAME task-owned instance to each
// awaiter (a borrow), disposed once at scope-end. Each catch adds; 40+48 = 88.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_ownership_1528, multi_await_reraises_same_instance) {
    expectValue1528(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var f := Boom(); "
        "    var a := 0; "
        "    try a := await f; catch InvalidOperationException => a := 40; end; "
        "    try a := a + await f; catch InvalidOperationException => a := a + 48; end; "
        "    return a; "
        "  end Main; end C;",
        88);
}

// ---------------------------------------------------------------------------
// §30.9 — control: the task bound OUTSIDE the try (disposed at method scope,
// AFTER the catch) took the not-in-flight (owned) scope-end path pre-fix and
// still must, returning 37.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_ownership_1528, var_bound_outside_try_reraises) {
    expectValue1528(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var f := Boom(); "
        "    try "
        "      return await f; "
        "    catch InvalidOperationException => return 37; "
        "    end; "
        "  end Main; end C;",
        37);
}

// ---------------------------------------------------------------------------
// §30.9 — control: the anonymous-producer path (`await Producer()`) is handler-
// owned and unchanged by the fix; awaited-in-try + caught must still return 37.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_ownership_1528, anonymous_producer_reraises) {
    expectValue1528(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      return await Boom(); "
        "    catch InvalidOperationException => return 37; "
        "    end; "
        "  end Main; end C;",
        37);
}
